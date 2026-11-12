#include "prx/libSceAgcDriver/Graphics/include/GpuAuthorityCensus.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MemoryAccounting.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ImageEpochs.hpp"
#include "prx/libSceAgcDriver/Graphics/include/SlimMutex.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include "prx/libSceAgcDriver/Graphics/include/LazyCensus.hpp"
#include "prx/libSceAgcDriver/Graphics/include/LazyPublish.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"

namespace AgcDriver::Graphics {

namespace {

constexpr std::uint64_t UnitBytes = 65536;
constexpr std::size_t ReasonCount = static_cast<std::size_t>(PublishReason::Count);
constexpr const char* ReasonNames[ReasonCount] = {"hook", "region", "indirect", "copy-src", "copy-dst", "fill", "fill-clear", "label", "keys", "scanout", "validate", "upload", "tail", "evict", "retire", "teardown", "address", "address-write", "sampled", "label-read", "other"};

std::uint64_t BudgetBytes(const Context& context) {
    static const char* text = std::getenv("APS5_UNIT_SHADOW_MIB");
    if (text != nullptr) return std::strtoull(text, nullptr, 10) << 20u;
    static std::once_flag once;
    static std::uint64_t budget = 0;
    std::call_once(once, [&] {
        if (context.memoryProperties2 == nullptr || context.physical == VK_NULL_HANDLE) {
            budget = UnitShadowBudget(context.memory, nullptr);
            return;
        }
        VkPhysicalDeviceMemoryBudgetPropertiesEXT reported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        properties.pNext = &reported;
        context.memoryProperties2(context.physical, &properties);
        budget = UnitShadowBudget(context.memory, &reported);
    });
    return budget;
}

std::uint32_t SlabUnits() {
    static const std::uint32_t units = [] {
        const char* text = std::getenv("APS5_UNIT_SHADOW_SLAB_MIB");
        const auto mib = text != nullptr ? std::strtoull(text, nullptr, 10) : 8ull;
        return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(mib, 1, 1024) * 16);
    }();
    return units;
}

// APS5_TRACE_SHADOW=<n>: the first n publishes, the first 64 seeds, every evict and retire.
std::atomic<std::int64_t> tracePublishesLeft{[] {
    const char* text = std::getenv("APS5_TRACE_SHADOW");
    return text != nullptr ? static_cast<std::int64_t>(std::strtoll(text, nullptr, 10)) : std::int64_t{0};
}()};
std::atomic<std::int64_t> traceSeedsLeft{64};

bool TraceEnabled() {
    static const bool enabled = std::getenv("APS5_TRACE_SHADOW") != nullptr;
    return enabled;
}

struct Statistics {
    std::atomic<std::uint64_t> retiled{0}, retiledBytes{0}, refused{0}, refusedBytes{0}, seeds{0}, seedBytes{0};
    std::atomic<std::uint64_t> detiledShadow{0}, detiledShadowBytes{0}, detiledImport{0}, detiledImportBytes{0}, tailPublishes{0};
    std::array<std::atomic<std::uint64_t>, ReasonCount> published{}, publishedBytes{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(GuestMemory::ReadSite::Count)> hookPublished{}, hookPublishedBytes{};
    std::atomic<std::uint64_t> addressHandoffs{0}, addressReadBytes{0}, addressPublishUnitsAvoided{0}, addressBytesSaved{0};
    std::atomic<std::uint64_t> staleDroppedCpu{0}, staleDroppedDriver{0}, skippedInCompletion{0}, evictions{0}, lostOnRetire{0}, droppedOnRetire{0}, slabLost{0}, verified{0}, mismatches{0};
};

Statistics& Stats() {
    static Statistics statistics;
    return statistics;
}

struct ShadowAddressCollectStatistics {
    std::atomic<std::uint64_t> calls{0}, spanPages{0}, walkedPages{0}, runs{0}, verified{0}, mismatches{0};
};

ShadowAddressCollectStatistics& AddressCollectStats() {
    static ShadowAddressCollectStatistics statistics;
    return statistics;
}

struct UnitShadow {
    Context context;
    // The import's range, buffer and lead (taken at creation: a publish under the import table's
    // mutex must not look them up), and the unit base (the import's base rounded down to a unit).
    std::uint64_t importBase = 0;
    std::uint64_t importBytes = 0;
    VkBuffer importBuffer = VK_NULL_HANDLE;
    std::uint64_t importLead = 0;
    std::uint64_t base = 0;
    // Per unit: 0 = never shadowed, else the tracker generation the slab bytes are current at;
    // whether a slab -> import copy was recorded at that generation.
    std::vector<std::uint64_t> generation;
    std::vector<std::uint8_t> published;
    std::vector<std::uint8_t> addressed;
    std::vector<std::uint64_t> lazyTicket;
    std::vector<std::uint8_t> lazySettled;
    // Slab k covers units [k * SlabUnits(), (k + 1) * SlabUnits()), made on demand; a failed
    // allocation is not retried before the noted present.
    std::vector<std::shared_ptr<ShadowSlab>> slabs;
    std::vector<std::uint64_t> slabFailedUntil;
    std::uint32_t liveUnits = 0;

    std::uint64_t Units() const { return generation.size(); }
    std::uint64_t UnitBegin(std::uint64_t unit) const { return base + unit * UnitBytes; }
    std::uint64_t UnitOf(std::uint64_t address) const { return (address - base) / UnitBytes; }
    std::uint64_t ImportEnd() const { return importBase + importBytes; }
    // The unit's bytes inside the import.
    std::pair<std::uint64_t, std::uint64_t> UnitRange(std::uint64_t unit) const { return {std::max(UnitBegin(unit), importBase), std::min(UnitBegin(unit + 1), ImportEnd())}; }
    bool Live(std::uint64_t unit) const { return generation[unit] != 0 && published[unit] == 0; }
    std::uint32_t CountLive() const {
        std::uint32_t live = 0;
        for (std::uint64_t unit = 0; unit < Units(); ++unit) live += Live(unit) ? 1u : 0u;
        return live;
    }
    void Recount() { liveUnits = CountLive(); }
};

struct Shadows {
    SlimMutex mutex;
    std::map<std::uint64_t, std::shared_ptr<UnitShadow>> byBase;
    std::uint64_t liveBytes = 0;
    std::uint64_t peakBytes = 0;
    std::uint32_t liveSlabs = 0;
    // Counted for the [storage] segment: shadows retired and the slab bytes they returned.
    std::uint64_t retired = 0;
    std::uint64_t lazySerial = 0;
    std::map<std::uint64_t, std::vector<std::pair<std::uint64_t, std::uint64_t>>> publishing;
};

Shadows& Registry() {
    static Shadows shadows;
    return shadows;
}

VkDeviceSize SlabBytes(const ShadowSlab& slab) {
    return static_cast<VkDeviceSize>(slab.units) * UnitBytes;
}

// The shadows overlapping [address, end): imports never overlap, so at most a few.
template<typename TVisit>
bool anyOverlapping(const Shadows& registry, std::uint64_t address, std::uint64_t end, TVisit&& visit) {
    if (end <= address || registry.byBase.empty()) return false;
    auto it = registry.byBase.upper_bound(address);
    if (it != registry.byBase.begin()) --it;
    for (; it != registry.byBase.end() && it->first < end; ++it) {
        const auto& shadow = it->second;
        if (address < shadow->ImportEnd() && shadow->importBase < end && visit(shadow)) return true;
    }
    return false;
}

std::vector<std::shared_ptr<UnitShadow>> overlapping(const Shadows& registry, std::uint64_t address, std::uint64_t end) {
    std::vector<std::shared_ptr<UnitShadow>> result;
    anyOverlapping(registry, address, end, [&](const std::shared_ptr<UnitShadow>& shadow) {
        result.push_back(shadow);
        return false;
    });
    return result;
}

std::shared_ptr<UnitShadow> find(const Shadows& registry, const HostImport& import) {
    const auto found = registry.byBase.find(import.base);
    if (found == registry.byBase.end()) return nullptr;
    const auto& shadow = found->second;
    return shadow->importBuffer == import.buffer && shadow->importBytes == import.bytes ? shadow : nullptr;
}

std::shared_ptr<UnitShadow> findOrCreate(Shadows& registry, const Context& context, const HostImport& import) {
    if (auto shadow = find(registry, import); shadow != nullptr && shadow->context.device == context.device) return shadow;
    // A stale entry (an import replaced without a retire, a replaced device): dropped; its slabs
    // live on wherever a batch kept them.
    if (const auto found = registry.byBase.find(import.base); found != registry.byBase.end()) {
        for (const auto& slab : found->second->slabs) {
            if (slab == nullptr) continue;
            registry.liveBytes -= SlabBytes(*slab);
            --registry.liveSlabs;
        }
        registry.byBase.erase(found);
    }
    auto shadow = std::make_shared<UnitShadow>();
    shadow->context = context;
    shadow->importBase = import.base;
    shadow->importBytes = import.bytes;
    shadow->importBuffer = import.buffer;
    shadow->importLead = import.lead;
    shadow->base = import.base & ~(UnitBytes - 1);
    const auto units = (import.base + import.bytes - shadow->base + UnitBytes - 1) / UnitBytes;
    shadow->generation.assign(static_cast<std::size_t>(units), 0);
    shadow->published.assign(static_cast<std::size_t>(units), 0);
    shadow->lazyTicket.assign(static_cast<std::size_t>(units), 0);
    shadow->lazySettled.assign(static_cast<std::size_t>(units), 0);
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (ShadowAddressEnabled() && profile) shadow->addressed.assign(static_cast<std::size_t>(units), 0);
    const auto slabs = (units + SlabUnits() - 1) / SlabUnits();
    shadow->slabs.assign(static_cast<std::size_t>(slabs), nullptr);
    shadow->slabFailedUntil.assign(static_cast<std::size_t>(slabs), 0);
    registry.byBase.emplace(import.base, shadow);
    return shadow;
}

// One tracker query over units [first, last]: `changed[k]` (k = unit - first) whether a block
// stamp exceeds the unit's generation (a generation of 0 reads as changed), `cpu[k]` whether a
// collect made it (a CPU store).
void changedUnits(const UnitShadow& shadow, std::uint64_t first, std::uint64_t last, std::vector<std::uint8_t>& changed, std::vector<std::uint8_t>& cpu) {
    const auto count = static_cast<std::size_t>(last - first + 1);
    changed.assign(count, 1);
    cpu.assign(count, 1);
    const auto begin = std::max(shadow.UnitBegin(first), shadow.importBase);
    const auto end = std::min(shadow.UnitBegin(last + 1), shadow.ImportEnd());
    if (end <= begin) return;
    GuestMemory::ChangedBlocks(begin, static_cast<std::size_t>(end - begin), std::span(shadow.generation).subspan(static_cast<std::size_t>(first), count), changed, cpu);
}

std::shared_ptr<ShadowSlab> makeSlab(const Context& context, std::uint64_t firstUnit, std::uint32_t units) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = static_cast<VkDeviceSize>(units) * UnitBytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const bool addressable = ShadowAddressEnabled() && context.bufferDeviceAddress;
    if (addressable) info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer) != VK_SUCCESS) return nullptr;
    VkMemoryRequirements requirements{};
    context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    if (addressable) allocation.pNext = &flags;
    allocation.allocationSize = requirements.size;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    try {
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (MemoryAccounting::Allocate(context, allocation, "unit shadow", &memory) != VK_SUCCESS) memory = VK_NULL_HANDLE;
        if (memory != VK_NULL_HANDLE && context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0) != VK_SUCCESS) {
            MemoryAccounting::Free(context, memory);
            memory = VK_NULL_HANDLE;
        }
    } catch (const std::exception&) {
        memory = VK_NULL_HANDLE;
    }
    if (memory == VK_NULL_HANDLE) {
        context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
        return nullptr;
    }
    auto slab = std::make_shared<ShadowSlab>(context, buffer, memory, firstUnit, units);
    if (addressable) {
        const VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, buffer};
        slab->address = context.Function<PFN_vkGetBufferDeviceAddressKHR>("vkGetBufferDeviceAddressKHR")(context.device, &addressInfo);
    }
    return slab;
}

struct SlabCopies {
    std::shared_ptr<ShadowSlab> slab;
    std::vector<VkBufferCopy> regions;
};

struct PublishOnCpuStorage {};

bool& publishOnCpu() {
    return HostThreadLocal<bool, PublishOnCpuStorage>();
}

struct LazyCounters {
    std::atomic<std::uint64_t> lazyStores{0}, eagerStores{0}, leftBytes{0}, enrolled{0}, enrolledBytes{0}, unwatchable{0}, unwatchableBytes{0}, verifyChecks{0}, verifyMismatches{0}, verifySkipped{0}, staleReads{0}, staleChecks{0}, publishedUnits{0};
    std::atomic<std::uint64_t> avoided{0}, avoidedBytes{0};
    LazyCounters() {
        const auto add = [](const char* name, const std::atomic<std::uint64_t>& counter) { AgcDriver::ArchB::Recorder::RegisterCounter(name, counter); };
        add("n5a_lazy_stores", lazyStores);
        add("n5a_eager_stores", eagerStores);
        add("n5a_left_bytes", leftBytes);
        add("n5a_enrolled", enrolled);
        add("n5a_enrolled_bytes", enrolledBytes);
        add("n5a_publications_avoided", avoided);
        add("n5a_publication_bytes_avoided", avoidedBytes);
        add("n5a_unwatchable", unwatchable);
        add("n5a_unwatchable_bytes", unwatchableBytes);
        add("n5a_verify_checks", verifyChecks);
        add("n5a_verify_content_mismatches", verifyMismatches);
        add("n5a_verify_skipped", verifySkipped);
        add("n5a_verify_stale_reads", staleReads);
        add("n5a_verify_read_checks", staleChecks);
        add("n5a_published_units", publishedUnits);
        add("n5a_lost_on_retire", Stats().lostOnRetire);
        add("n5a_slab_lost", Stats().slabLost);
        add("n5a_retired_discarded_units", Stats().droppedOnRetire);
    }
};

LazyCounters& lazyCounters() {
    static LazyCounters counters;
    return counters;
}

struct PublishSiteStorage {};

const char*& publishSite() {
    return HostThreadLocal<const char*, PublishSiteStorage>();
}

struct SiteCounter {
    std::string countName;
    std::string bytesName;
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> bytes{0};
};

struct SiteCounters {
    std::mutex mutex;
    std::map<std::string, std::unique_ptr<SiteCounter>> bySite;
};

SiteCounters& siteCounters() {
    static SiteCounters counters;
    return counters;
}

void countSite(const std::string& prefix, const char* site, std::uint64_t bytes) {
    std::string key = prefix;
    for (const char* at = site; *at != '\0'; ++at) key += std::isalnum(static_cast<unsigned char>(*at)) ? *at : '_';
    auto& counters = siteCounters();
    SiteCounter* counter = nullptr;
    {
        std::lock_guard lock(counters.mutex);
        auto& slot = counters.bySite[key];
        if (slot == nullptr) {
            slot = std::make_unique<SiteCounter>();
            slot->countName = key + "_n";
            slot->bytesName = key + "_bytes";
            AgcDriver::ArchB::Recorder::RegisterCounter(slot->countName.c_str(), slot->count);
            AgcDriver::ArchB::Recorder::RegisterCounter(slot->bytesName.c_str(), slot->bytes);
        }
        counter = slot.get();
    }
    counter->count.fetch_add(1, std::memory_order_relaxed);
    counter->bytes.fetch_add(bytes, std::memory_order_relaxed);
}

std::string siteOf(PublishReason reason) {
    std::string site = publishSite() != nullptr ? publishSite() : ReasonNames[static_cast<std::size_t>(reason)];
    if (reason == PublishReason::Hook || reason == PublishReason::Scanout) site += std::string("@") + GuestMemory::ReadSiteName(GuestMemory::CurrentReadSite());
    return site;
}

std::atomic<void (*)(std::uint64_t, std::uint64_t)> publishSelectedHook{nullptr};

bool recordCopiesOnCpu(const UnitShadow& shadow, const std::vector<SlabCopies>& copies, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, std::uint64_t bytes, std::uint64_t checked) {
    auto* recorder = Recorder::Active();
    if (recorder == nullptr) return false;
    const auto& context = shadow.context;
    auto host = std::make_shared<Buffer>(context, static_cast<std::size_t>(bytes), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    struct Piece {
        std::uint64_t address;
        std::uint64_t offset;
        std::uint64_t size;
    };
    std::vector<Piece> pieces;
    const auto commands = recorder->Commands();
    const auto timing = recorder->BeginGpuTiming(Recorder::CommandClass::ShadowPublish);
    constexpr VkAccessFlags transferAccess = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, transferAccess);
    Recorder::CountBarriers(Recorder::CommandClass::ShadowPublish);
    const auto copyBuffer = context.Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer");
    std::uint64_t offset = 0;
    for (const auto& entry : copies) {
        std::vector<VkBufferCopy> regions;
        for (const auto& region : entry.regions) {
            regions.push_back({region.srcOffset, offset, region.size});
            pieces.push_back({shadow.importBase + region.dstOffset - shadow.importLead, offset, region.size});
            offset += region.size;
        }
        copyBuffer(commands, entry.slab->buffer, host->Handle(), static_cast<std::uint32_t>(regions.size()), regions.data());
        recorder->Keep(entry.slab);
    }
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    Recorder::CountBarriers(Recorder::CommandClass::ShadowPublish);
    recorder->EndGpuTiming(timing, bytes);
    if (GpuAuthorityCensus::Enabled()) for (const auto& [begin, end] : ranges) GpuAuthorityCensus::Published(begin, end, GpuAuthorityCensus::Kind::ShadowPublish, recorder, recorder->OpenSerial(), timing);
    recorder->NotePendingWrites(ranges);
    recorder->OnComplete([host, pieces = std::move(pieces), checked] {
        host->Invalidate();
        for (const auto& piece : pieces) {
            for (std::uint64_t at = piece.address; at < piece.address + piece.size;) {
                const auto next = std::min(piece.address + piece.size, (at & ~(UnitBytes - 1)) + UnitBytes);
                const auto bytes = static_cast<std::size_t>(next - at);
                if (GuestMemory::UnchangedSinceCollected(at, bytes, checked)) GuestMemory::StoreOwnBytes(at, host->Bytes().subspan(static_cast<std::size_t>(piece.offset + (at - piece.address)), bytes));
                else Stats().staleDroppedCpu.fetch_add(1, std::memory_order_relaxed);
                at = next;
            }
        }
    });
    return true;
}

// Records the copies as one command group into the active recorder's open batch (a waited batch
// without one). `ranges` are the guest ranges copied, for the note and the hazard tracker.
void recordCopies(const UnitShadow& shadow, const std::vector<SlabCopies>& copies, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, std::uint64_t bytes) {
    const auto& context = shadow.context;
    using CommandClass = Recorder::CommandClass;
    auto* recorder = Recorder::Active();
    std::unique_ptr<CommandBatch> batch;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkAccessFlags covered = 0;
    auto timing = Recorder::NoTiming;
    if (recorder != nullptr) {
        commands = recorder->Commands(&covered);
        timing = recorder->BeginGpuTiming(CommandClass::ShadowPublish);
        if (Recorder::BarrierValidate()) recorder->NoteAccess(CommandClass::ShadowPublish, Recorder::Access{{}, ranges, {}, VK_PIPELINE_STAGE_TRANSFER_BIT});
    } else {
        batch = std::make_unique<CommandBatch>(context);
        commands = batch->Handle();
    }
    // The retile that filled the slab (its trailing barrier covers transfer reads and writes) and
    // every earlier import writer precede the copies; the CopyBuffer merge rule.
    constexpr VkAccessFlags transferAccess = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    if (recorder != nullptr && (covered & transferAccess) == transferAccess && Recorder::MergeBarriers()) {
        Recorder::CountMerged(CommandClass::ShadowPublish);
    } else {
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, transferAccess);
        Recorder::CountBarriers(CommandClass::ShadowPublish);
    }
    const auto copyBuffer = context.Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer");
    for (const auto& entry : copies) copyBuffer(commands, entry.slab->buffer, shadow.importBuffer, static_cast<std::uint32_t>(entry.regions.size()), entry.regions.data());
    // The published bytes are visible to everything recorded after and to the host, as after a fill.
    constexpr VkAccessFlags publishedAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, publishedAccess);
    Recorder::CountBarriers(CommandClass::ShadowPublish);
    if (batch) {
        batch->SubmitAndWait();
        if (GpuAuthorityCensus::Enabled()) for (const auto& [begin, end] : ranges) GpuAuthorityCensus::Published(begin, end, GpuAuthorityCensus::Kind::ShadowPublish);
        return;
    }
    recorder->EndGpuTiming(timing, bytes);
    if (GpuAuthorityCensus::Enabled()) for (const auto& [begin, end] : ranges) GpuAuthorityCensus::Published(begin, end, GpuAuthorityCensus::Kind::ShadowPublish, recorder, recorder->OpenSerial(), timing);
    recorder->MarkCovered(publishedAccess);
    for (const auto& entry : copies) recorder->Keep(entry.slab);
    // CPU readers reach the import's bytes only after the hook's SyncThrough waits by this note.
    recorder->NotePendingWrites(ranges);
}

// APS5_SHADOW_VERIFY=1: the published bytes read back from the slabs equal the guest bytes.
void verifyPublished(const UnitShadow& shadow, const std::vector<SlabCopies>& copies) {
    const auto& context = shadow.context;
    if (auto* recorder = Recorder::Active()) {
        Recorder::CountSync(4);
        recorder->Sync();
    }
    for (const auto& entry : copies) {
        for (const auto& region : entry.regions) {
            Buffer host(context, static_cast<std::size_t>(region.size), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            CommandBatch batch(context);
            CopyBuffer(context, batch.Handle(), entry.slab->buffer, region.srcOffset, host.Handle(), 0, region.size);
            RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            batch.SubmitAndWait();
            host.Invalidate();
            const auto address = shadow.importBase + region.dstOffset - shadow.importLead;
            Stats().verified.fetch_add(1, std::memory_order_relaxed);
            if (!GuestMemory::Accessible(reinterpret_cast<const void*>(address), static_cast<std::size_t>(region.size)) || std::memcmp(reinterpret_cast<const void*>(address), host.Bytes().data(), static_cast<std::size_t>(region.size)) != 0) {
                Stats().mismatches.fetch_add(1, std::memory_order_relaxed);
                static std::atomic<int> reports{0};
                if (reports.fetch_add(1) < 8) std::fprintf(stderr, "[shadow] verify: published bytes differ from the slab at 0x%llx+0x%llx\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(region.size));
            }
        }
    }
}

// The publish of one shadow: `selected[k]` (k = unit - first) names the units the scope chose.
std::size_t publishUnits(const std::shared_ptr<UnitShadow>& shadow, std::uint64_t first, std::uint64_t last, const std::vector<std::uint8_t>& selected, PublishReason reason) {
    auto& registry = Registry();
    std::vector<SlabCopies> copies;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::uint64_t bytes = 0;
    std::uint64_t forcedBytes = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> forced;
    std::size_t units = 0;
    const auto checked = GuestMemory::TrackerGeneration();
    {
        // The stamps judged below see every CPU store into the live selected units up to now: a
        // store no collect walked yet would read as fresh and the copy would land over it. Every
        // caller holds the GpuMutex, so the live set is stable across the walk.
        std::uint64_t walkFirst = last + 1, walkLast = first;
        {
            std::lock_guard lock(registry.mutex);
            for (auto unit = first; unit <= last; ++unit) {
                if (!selected[static_cast<std::size_t>(unit - first)] || !shadow->Live(unit)) continue;
                walkFirst = std::min(walkFirst, unit);
                walkLast = std::max(walkLast, unit);
            }
        }
        if (walkFirst > walkLast) return 0;
        const auto walkBegin = std::max(shadow->UnitBegin(walkFirst), shadow->importBase);
        const auto walkEnd = std::min(shadow->UnitBegin(walkLast + 1), shadow->ImportEnd());
        if (walkEnd > walkBegin) GuestMemory::CollectWritesUncached(walkBegin, static_cast<std::size_t>(walkEnd - walkBegin));
    }
    {
        std::lock_guard lock(registry.mutex);
        std::vector<std::uint8_t> changed, cpu;
        changedUnits(*shadow, first, last, changed, cpu);
        std::uint32_t ended = 0;
        for (auto unit = first; unit <= last; ++unit) {
            const auto k = static_cast<std::size_t>(unit - first);
            if (!selected[k] || !shadow->Live(unit)) continue;
            if (changed[k] == GuestMemory::BlockWritten) {
                // Newer bytes reached the import since the retile: the slab's are dead.
                shadow->generation[unit] = 0;
                shadow->lazyTicket[unit] = 0;
                ++ended;
                (cpu[k] != 0 ? Stats().staleDroppedCpu : Stats().staleDroppedDriver).fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const auto slabIndex = static_cast<std::size_t>(unit / SlabUnits());
            const auto& slab = shadow->slabs[slabIndex];
            if (slab == nullptr) {
                shadow->generation[unit] = 0;
                shadow->lazyTicket[unit] = 0;
                ++ended;
                continue;
            }
            const auto [begin, end] = shadow->UnitRange(unit);
            if (end <= begin) continue;
            const VkBufferCopy region{(unit - slab->firstUnit) * UnitBytes + (begin - shadow->UnitBegin(unit)), begin - shadow->importBase + shadow->importLead, end - begin};
            if (!copies.empty() && copies.back().slab == slab && copies.back().regions.back().dstOffset + copies.back().regions.back().size == region.dstOffset) {
                copies.back().regions.back().size += region.size;
                ranges.back().second = end;
            } else {
                if (copies.empty() || copies.back().slab != slab) copies.push_back({slab, {}});
                copies.back().regions.push_back(region);
                ranges.emplace_back(begin, end);
            }
            shadow->published[unit] = 1;
            if (shadow->lazyTicket[unit] != 0) {
                forcedBytes += end - begin;
                forced.emplace_back(unit, shadow->lazyTicket[unit]);
            }
            ++ended;
            slab->lastUse = Recorder::Presents();
            bytes += end - begin;
            ++units;
        }
        shadow->liveUnits -= ended;
        if (ShadowVerify() && shadow->liveUnits != shadow->CountLive()) throw std::runtime_error("unit-shadow live count differs after publish");
    }
    if (units == 0) return 0;
    countSite("n5a_pub_", siteOf(reason).c_str(), bytes);
    if (forcedBytes != 0) countSite("n5a_force_", siteOf(reason).c_str(), forcedBytes);
    lazyCounters().publishedUnits.fetch_add(units, std::memory_order_relaxed);
    Stats().published[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    Stats().publishedBytes[static_cast<std::size_t>(reason)].fetch_add(bytes, std::memory_order_relaxed);
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (profile && reason == PublishReason::Hook) {
        const auto site = static_cast<std::size_t>(GuestMemory::CurrentReadSite());
        Stats().hookPublished[site].fetch_add(1, std::memory_order_relaxed);
        Stats().hookPublishedBytes[site].fetch_add(bytes, std::memory_order_relaxed);
    }
    if (tracePublishesLeft.load(std::memory_order_relaxed) > 0 && tracePublishesLeft.fetch_sub(1, std::memory_order_relaxed) > 0) {
        const auto packet = GuestMemory::CurrentPacket();
        std::fprintf(stderr, "[shadow] publish %s: 0x%llx+0x%llx, %zu units, %.2f MiB, %zu slabs (packet 0x%x queue 0x%x)\n", ReasonNames[static_cast<std::size_t>(reason)], static_cast<unsigned long long>(ranges.front().first), static_cast<unsigned long long>(ranges.back().second - ranges.front().first), units, bytes / 1048576.0, copies.size(), packet.opcode, packet.queue);
    }
    if (const auto hook = publishSelectedHook.load(std::memory_order_acquire)) hook(ranges.front().first, ranges.front().second);
    {
        const ArchC::LazyCensus::UseScope censusScope;
        for (const auto& [begin, end] : ranges) ArchC::LazyCensus::Published(begin, end, ArchC::LazyCensus::Kind::ShadowPublish, static_cast<std::uint8_t>(reason));
        if (!publishOnCpu() || !recordCopiesOnCpu(*shadow, copies, ranges, bytes, checked)) recordCopies(*shadow, copies, ranges, bytes);
    }
    if (!forced.empty()) {
        std::uint64_t publication = 0;
        {
            std::lock_guard lock(Registry().mutex);
            publication = ++Registry().lazySerial;
            Registry().publishing.emplace(publication, ranges);
        }
        auto completed = [shadow, forced = std::move(forced), publication] {
            std::lock_guard lock(Registry().mutex);
            Registry().publishing.erase(publication);
            for (const auto& [unit, ticket] : forced) {
                if (shadow->lazyTicket[unit] == ticket) shadow->lazyTicket[unit] = 0;
            }
        };
        if (auto* recorder = Recorder::Active()) recorder->OnComplete(std::move(completed));
        else completed();
    }
    if (ShadowVerify()) verifyPublished(*shadow, copies);
    return units;
}

std::size_t publishRange(const std::shared_ptr<UnitShadow>& shadow, std::uint64_t address, std::uint64_t end, PublishScope scope, PublishReason reason) {
    const auto begin = std::max(address, shadow->importBase);
    const auto stop = std::min(end, shadow->ImportEnd());
    if (stop <= begin || shadow->liveUnits == 0) return 0;
    const auto first = shadow->UnitOf(begin);
    const auto last = shadow->UnitOf(stop - 1);
    std::vector<std::uint8_t> selected(static_cast<std::size_t>(last - first + 1), 0);
    bool any = false;
    for (auto unit = first; unit <= last; ++unit) {
        const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
        const bool whole = address <= unitBegin && end >= unitEnd;
        if (scope == PublishScope::PartialUnits && whole) continue;
        selected[static_cast<std::size_t>(unit - first)] = 1;
        any = true;
    }
    return any ? publishUnits(shadow, first, last, selected, reason) : 0;
}

// Frees the least recently used unpinned slab of any shadow after publishing its fresh units; false
// when nothing can be freed (a pinned slab is a destination of the write-back in progress: its
// copies are recorded into it after this decision).
bool evictOne(Shadows& registry) {
    std::shared_ptr<UnitShadow> victim;
    std::size_t victimSlab = 0;
    {
        std::lock_guard lock(registry.mutex);
        std::uint64_t oldest = ~0ull;
        for (const auto& [base, shadow] : registry.byBase) {
            for (std::size_t k = 0; k < shadow->slabs.size(); ++k) {
                const auto& slab = shadow->slabs[k];
                if (slab != nullptr && slab->pins.load(std::memory_order_relaxed) == 0 && slab->lastUse < oldest) {
                    oldest = slab->lastUse;
                    victim = shadow;
                    victimSlab = k;
                }
            }
        }
    }
    if (victim == nullptr) return false;
    const auto firstUnit = static_cast<std::uint64_t>(victimSlab) * SlabUnits();
    const auto lastUnit = std::min(firstUnit + SlabUnits(), victim->Units()) - 1;
    const auto published = publishRange(victim, victim->UnitBegin(firstUnit), victim->UnitBegin(lastUnit + 1), PublishScope::Whole, PublishReason::Evict);
    std::lock_guard lock(registry.mutex);
    auto& slab = victim->slabs[victimSlab];
    if (slab == nullptr) return true;
    if (TraceEnabled()) std::fprintf(stderr, "[shadow] evict slab %zu of import 0x%llx (%.1f MiB, %zu units published)\n", victimSlab, static_cast<unsigned long long>(victim->importBase), SlabBytes(*slab) / 1048576.0, published);
    // Its units read from the import from now on (the publish put their bytes there).
    for (auto unit = firstUnit; unit <= lastUnit; ++unit) {
        victim->generation[unit] = 0;
        victim->published[unit] = 0;
    }
    victim->Recount();
    registry.liveBytes -= SlabBytes(*slab);
    --registry.liveSlabs;
    slab.reset();
    Stats().evictions.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}

std::uint64_t UnitShadowBudget(const VkPhysicalDeviceMemoryProperties& memory, const VkPhysicalDeviceMemoryBudgetPropertiesEXT* reported) {
    constexpr std::uint64_t Ceiling = std::uint64_t{1024} << 20u;
    constexpr std::uint64_t Floor = std::uint64_t{128} << 20u;
    const auto heap = LargestDeviceLocalHeap(memory);
    if (reported == nullptr || !heap || reported->heapBudget[*heap] == 0) return Ceiling;
    return std::clamp<std::uint64_t>(reported->heapBudget[*heap] / 8u, Floor, Ceiling);
}

bool UnitShadowEnabled() {
    static const bool enabled = std::getenv("APS5_NO_UNIT_SHADOW") == nullptr && GuestMemory::WriteWatched();
    return enabled;
}

bool ShadowAddressEnabled() {
    static const bool enabled = std::getenv("APS5_NO_SHADOW_BDA") == nullptr;
    return enabled && UnitShadowEnabled();
}

bool ShadowVerify() {
    static const bool verify = std::getenv("APS5_SHADOW_VERIFY") != nullptr;
    return verify;
}

void NoteShadowSeed(std::uint64_t begin, std::uint64_t end) {
    Stats().seeds.fetch_add(1, std::memory_order_relaxed);
    Stats().seedBytes.fetch_add(end - begin, std::memory_order_relaxed);
    if (traceSeedsLeft.load(std::memory_order_relaxed) > 0 && traceSeedsLeft.fetch_sub(1, std::memory_order_relaxed) > 0) std::fprintf(stderr, "[shadow] seed 0x%llx+0x%llx from the import\n", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin));
}

PublishReason PublishReasonFor(const char* flushReason) {
    const auto is = [&](const char* name) { return flushReason != nullptr && std::strcmp(flushReason, name) == 0; };
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (profile) {
        if (is("sampled texture")) return PublishReason::Sampled;
        if (is("written buffer range") || is("in-place vertex input")) return PublishReason::Region;
        if (is("label")) return PublishReason::Label;
        if (is("readback before label")) return PublishReason::LabelRead;
        if (is("flip")) return PublishReason::Scanout;
        if (is("writable address-based heaps")) return PublishReason::AddressWrite;
    }
    if (is("memory access")) return GuestMemory::CurrentReadSite() == GuestMemory::ReadSite::Scanout ? PublishReason::Scanout : PublishReason::Hook;
    if (is("imported buffer region") || is("copied buffer region") || is("sampled texture")) return PublishReason::Region;
    if (is("indirect dispatch arguments") || is("indirect draw arguments")) return PublishReason::Indirect;
    if (is("buffer copy source")) return PublishReason::CopySource;
    if (is("buffer copy destination")) return PublishReason::CopyDestination;
    if (is("buffer fill")) return PublishReason::Fill;
    if (is("packet store")) return PublishReason::Label;
    if (is("buffer copy count") || is("dispatch-cache entry")) return PublishReason::Validate;
    if (is("storage image creation") || is("storage refresh")) return PublishReason::Upload;
    if (is("address-based heaps")) return PublishReason::Address;
    return PublishReason::Other;
}

ShadowSlab::ShadowSlab(const Context& context, VkBuffer buffer, VkDeviceMemory memory, std::uint64_t firstUnit, std::uint32_t units) : context(context), buffer(buffer), memory(memory), firstUnit(firstUnit), units(units) {}

ShadowSlab::~ShadowSlab() {
    if (buffer != VK_NULL_HANDLE) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE) MemoryAccounting::Free(context, memory);
}

ShadowSlabPin::ShadowSlabPin(std::shared_ptr<ShadowSlab> slab) : slab(std::move(slab)) {
    this->slab->pins.fetch_add(1, std::memory_order_relaxed);
}

ShadowSlabPin::~ShadowSlabPin() {
    slab->pins.fetch_sub(1, std::memory_order_relaxed);
}

void SetPublishSelectedHook(void (*hook)(std::uint64_t begin, std::uint64_t end)) {
    publishSelectedHook.store(hook, std::memory_order_release);
}

PublishOnCpuScope::PublishOnCpuScope() : previous(std::exchange(publishOnCpu(), true)) {}

PublishOnCpuScope::~PublishOnCpuScope() {
    publishOnCpu() = previous;
}

PublishSiteScope::PublishSiteScope(const char* site) : previous(publishSite()) {
    if (previous == nullptr) publishSite() = site;
}

PublishSiteScope::~PublishSiteScope() {
    publishSite() = previous;
}

std::size_t PublishShadow(std::uint64_t address, std::size_t bytes, PublishScope scope, PublishReason reason, bool waited) {
    if (!UnitShadowEnabled() || scope == PublishScope::None || bytes == 0) return 0;
    // Write generations: a unit-shadow publish changes the range's representation (meta).
    if (GuestMemory::Gen::Active()) static_cast<void>(GuestMemory::Gen::Note(GuestMemory::Gen::Event::ShadowPublish, address, address + bytes, GuestMemory::Gen::Meta, 0, false));
    if (!waited && Recorder::InCompletion() && GuestMemory::CurrentReadSite() == GuestMemory::ReadSite::Store) {
        // The store's own stamp makes the unit stale; a publish recorded now would land over the
        // store when the batch runs (the completion is not waited for).
        Stats().skippedInCompletion.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const auto end = bytes > std::numeric_limits<std::uint64_t>::max() - address ? std::numeric_limits<std::uint64_t>::max() : address + bytes;
    std::vector<std::shared_ptr<UnitShadow>> shadows;
    {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        shadows = overlapping(registry, address, end);
    }
    std::size_t units = 0;
    for (const auto& shadow : shadows) units += publishRange(shadow, address, end, scope, reason);
    return units;
}

namespace {

bool anyShadowedLocked(const Shadows& registry, std::uint64_t address, std::size_t bytes) {
    if (bytes == 0) return false;
    const auto end = bytes > std::numeric_limits<std::uint64_t>::max() - address ? std::numeric_limits<std::uint64_t>::max() : address + bytes;
    return anyOverlapping(registry, address, end, [&](const std::shared_ptr<UnitShadow>& shadow) {
        if (shadow->liveUnits == 0) return false;
        const auto begin = std::max(address, shadow->importBase);
        const auto stop = std::min(end, shadow->ImportEnd());
        for (auto unit = shadow->UnitOf(begin); unit <= shadow->UnitOf(stop - 1); ++unit) {
            if (shadow->Live(unit)) return true;
        }
        return false;
    });
}

}

bool AnyShadowedOverlaps(std::uint64_t address, std::size_t bytes) {
    if (!UnitShadowEnabled() || bytes == 0) return false;
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    return anyShadowedLocked(registry, address, bytes);
}

bool AnyShadowedOverlaps(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
    if (!UnitShadowEnabled() || ranges.empty()) return false;
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    for (const auto& [begin, end] : ranges) {
        if (end > begin && anyShadowedLocked(registry, begin, static_cast<std::size_t>(end - begin))) return true;
    }
    return false;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> LiveShadowedRanges(std::uint64_t address, std::size_t bytes, PublishScope scope) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    if (!UnitShadowEnabled() || scope == PublishScope::None || bytes == 0) return ranges;
    const auto end = bytes > std::numeric_limits<std::uint64_t>::max() - address ? std::numeric_limits<std::uint64_t>::max() : address + bytes;
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    for (const auto& shadow : overlapping(registry, address, end)) {
        if (shadow->liveUnits == 0) continue;
        const auto begin = std::max(address, shadow->importBase);
        const auto stop = std::min(end, shadow->ImportEnd());
        if (stop <= begin) continue;
        for (auto unit = shadow->UnitOf(begin); unit <= shadow->UnitOf(stop - 1); ++unit) {
            if (!shadow->Live(unit)) continue;
            const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
            if (unitEnd <= unitBegin || (scope == PublishScope::PartialUnits && address <= unitBegin && end >= unitEnd)) continue;
            if (!ranges.empty() && ranges.back().second == unitBegin) ranges.back().second = unitEnd;
            else ranges.emplace_back(unitBegin, unitEnd);
        }
    }
    if (ranges.size() > 8) ranges.assign(1, {ranges.front().first, ranges.back().second});
    return ranges;
}

std::uint64_t SlabBoundary(const HostImport& import, std::uint64_t address) {
    const auto base = import.base & ~(UnitBytes - 1);
    const auto unit = (address - base) / UnitBytes;
    return base + (unit / SlabUnits() + 1) * SlabUnits() * UnitBytes;
}

VkDeviceSize SlabOffset(const HostImport& import, const ShadowSlab& slab, std::uint64_t address) {
    const auto base = import.base & ~(UnitBytes - 1);
    return (address - base) - slab.firstUnit * UnitBytes;
}

std::optional<ShadowDestination> ShadowDestinationFor(const Context& context, const HostImport& import, std::uint64_t begin, std::uint64_t end) {
    if (!UnitShadowEnabled() || end <= begin || begin < import.base || end > import.base + import.bytes) return std::nullopt;
    if (!GuestMemory::Watched(begin, static_cast<std::size_t>(end - begin))) return std::nullopt;
    auto& registry = Registry();
    std::shared_ptr<UnitShadow> shadow;
    std::size_t slabIndex = 0;
    std::uint64_t first = 0, last = 0;
    bool missing = false;
    {
        std::lock_guard lock(registry.mutex);
        shadow = findOrCreate(registry, context, import);
        first = shadow->UnitOf(begin);
        last = shadow->UnitOf(end - 1);
        slabIndex = static_cast<std::size_t>(first / SlabUnits());
        if (last / SlabUnits() != slabIndex) return std::nullopt;
        missing = shadow->slabs[slabIndex] == nullptr;
        if (missing && shadow->slabFailedUntil[slabIndex] > Recorder::Presents()) return std::nullopt;
    }
    if (missing) {
        const auto firstUnit = static_cast<std::uint64_t>(slabIndex) * SlabUnits();
        const auto units = static_cast<std::uint32_t>(std::min<std::uint64_t>(SlabUnits(), shadow->Units() - firstUnit));
        const auto needed = static_cast<std::uint64_t>(units) * UnitBytes;
        for (;;) {
            {
                std::lock_guard lock(registry.mutex);
                if (registry.liveBytes + needed <= BudgetBytes(context)) break;
            }
            if (!evictOne(registry)) {
                Stats().refused.fetch_add(1, std::memory_order_relaxed);
                Stats().refusedBytes.fetch_add(end - begin, std::memory_order_relaxed);
                return std::nullopt;
            }
        }
        auto slab = makeSlab(context, firstUnit, units);
        std::lock_guard lock(registry.mutex);
        if (slab == nullptr) {
            shadow->slabFailedUntil[slabIndex] = Recorder::Presents() + 1;
            Stats().refused.fetch_add(1, std::memory_order_relaxed);
            Stats().refusedBytes.fetch_add(end - begin, std::memory_order_relaxed);
            return std::nullopt;
        }
        if (shadow->slabs[slabIndex] == nullptr) {
            shadow->slabs[slabIndex] = slab;
            registry.liveBytes += SlabBytes(*slab);
            registry.peakBytes = std::max(registry.peakBytes, registry.liveBytes);
            ++registry.liveSlabs;
        }
    }
    // A unit the copy covers partly keeps its other bytes from the slab when fresh, else from a
    // seed of the import: the stamps must see the CPU's stores into it first (the write-back's own
    // walk covers the stored span only, not the unit's remainder outside the surface).
    for (auto unit = first; unit <= last; unit = last) {
        const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
        if (!(begin <= unitBegin && end >= unitEnd) && unitEnd > unitBegin) GuestMemory::CollectWritesUncached(unitBegin, static_cast<std::size_t>(unitEnd - unitBegin));
        if (unit == last) break;
    }
    std::lock_guard lock(registry.mutex);
    const auto& slab = shadow->slabs[slabIndex];
    if (slab == nullptr) return std::nullopt;
    slab->lastUse = Recorder::Presents();
    ShadowDestination destination{slab->buffer, SlabOffset(import, *slab, begin), slab, {}, std::make_shared<ShadowSlabPin>(slab)};
    std::vector<std::uint8_t> changed, cpu;
    bool queried = false;
    for (auto unit = first; unit <= last; ++unit) {
        const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
        if (begin <= unitBegin && end >= unitEnd) continue;
        if (!queried) {
            changedUnits(*shadow, first, last, changed, cpu);
            queried = true;
        }
        const auto k = static_cast<std::size_t>(unit - first);
        if (shadow->generation[unit] != 0 && changed[k] == 0) continue;
        destination.seedUnits.emplace_back(unitBegin, unitEnd);
    }
    return destination;
}

std::vector<ShadowRun> ShadowSources(const Context& context, const HostImport& import, std::uint64_t surfaceBase, std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::pair<std::uint64_t, std::uint64_t>> tailBlocks, bool countReads) {
    std::vector<ShadowRun> result;
    if (runs.empty()) return result;
    const auto importPiece = [&](std::uint64_t begin, std::uint64_t end) {
        result.push_back({begin, end, import.buffer, HostImportOffset(import, surfaceBase + begin), false, nullptr});
        if (countReads) Stats().detiledImportBytes.fetch_add(end - begin, std::memory_order_relaxed);
    };
    std::shared_ptr<UnitShadow> shadow;
    if (UnitShadowEnabled()) {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        shadow = find(registry, import);
        if (shadow != nullptr && (shadow->context.device != context.device || (shadow->liveUnits == 0 && std::none_of(shadow->generation.begin(), shadow->generation.end(), [](std::uint64_t generation) { return generation != 0; })))) shadow = nullptr;
    }
    if (shadow == nullptr) {
        for (const auto& [begin, end] : runs) importPiece(begin, end);
        if (countReads) Stats().detiledImport.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    const auto first = shadow->UnitOf(surfaceBase + runs.front().first);
    const auto last = shadow->UnitOf(surfaceBase + runs.back().second - 1);
    std::vector<std::uint8_t> fresh(static_cast<std::size_t>(last - first + 1), 0);
    std::vector<std::shared_ptr<ShadowSlab>> slabs(shadow->slabs.size());
    {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        std::vector<std::uint8_t> changed, cpu;
        changedUnits(*shadow, first, last, changed, cpu);
        for (auto unit = first; unit <= last; ++unit) {
            const auto k = static_cast<std::size_t>(unit - first);
            const auto& slab = shadow->slabs[static_cast<std::size_t>(unit / SlabUnits())];
            fresh[k] = shadow->generation[unit] != 0 && changed[k] == 0 && slab != nullptr ? 1 : 0;
        }
        slabs = shadow->slabs;
    }
    // A tail block is moved whole by one window: its units take one source.
    for (const auto& [tailBegin, tailEnd] : tailBlocks) {
        if (tailEnd <= tailBegin) continue;
        const auto tailFirst = shadow->UnitOf(surfaceBase + tailBegin);
        const auto tailLast = shadow->UnitOf(surfaceBase + tailEnd - 1);
        if (tailFirst < first || tailLast > last) continue;
        bool anyFresh = false, anyImport = false;
        for (auto unit = tailFirst; unit <= tailLast; ++unit) (fresh[static_cast<std::size_t>(unit - first)] != 0 ? anyFresh : anyImport) = true;
        if (!anyFresh || !anyImport) continue;
        PublishShadow(surfaceBase + tailBegin, static_cast<std::size_t>(tailEnd - tailBegin), PublishScope::Whole, PublishReason::TailMip);
        Stats().tailPublishes.fetch_add(1, std::memory_order_relaxed);
        for (auto unit = tailFirst; unit <= tailLast; ++unit) fresh[static_cast<std::size_t>(unit - first)] = 0;
    }
    std::uint64_t shadowBytes = 0, importBytes = 0;
    const auto present = Recorder::Presents();
    for (const auto& [runBegin, runEnd] : runs) {
        if (runEnd <= runBegin) continue;
        const auto guestBegin = surfaceBase + runBegin;
        const auto guestEnd = surfaceBase + runEnd;
        for (auto unit = shadow->UnitOf(guestBegin); unit <= shadow->UnitOf(guestEnd - 1); ++unit) {
            const auto pieceBegin = std::max(guestBegin, shadow->UnitBegin(unit)) - surfaceBase;
            const auto pieceEnd = std::min(guestEnd, shadow->UnitBegin(unit + 1)) - surfaceBase;
            if (pieceEnd <= pieceBegin) continue;
            const bool isFresh = fresh[static_cast<std::size_t>(unit - first)] != 0;
            const auto& slab = isFresh ? slabs[static_cast<std::size_t>(unit / SlabUnits())] : nullptr;
            if (!result.empty() && result.back().end == pieceBegin && result.back().shadow == isFresh && result.back().slab == slab) {
                result.back().end = pieceEnd;
            } else if (isFresh) {
                slab->lastUse = present;
                result.push_back({pieceBegin, pieceEnd, slab->buffer, SlabOffset(import, *slab, surfaceBase + pieceBegin), true, slab});
            } else {
                result.push_back({pieceBegin, pieceEnd, import.buffer, HostImportOffset(import, surfaceBase + pieceBegin), false, nullptr});
            }
            (isFresh ? shadowBytes : importBytes) += pieceEnd - pieceBegin;
        }
    }
    if (!countReads) shadowBytes = importBytes = 0;
    if (shadowBytes != 0) {
        Stats().detiledShadow.fetch_add(1, std::memory_order_relaxed);
        Stats().detiledShadowBytes.fetch_add(shadowBytes, std::memory_order_relaxed);
    }
    if (importBytes != 0) {
        Stats().detiledImport.fetch_add(1, std::memory_order_relaxed);
        Stats().detiledImportBytes.fetch_add(importBytes, std::memory_order_relaxed);
    }
    if (ShadowVerify()) {
        std::vector<std::uint8_t> changed, cpu;
        std::lock_guard lock(Registry().mutex);
        changedUnits(*shadow, first, last, changed, cpu);
        for (const auto& run : result) {
            if (!run.shadow) continue;
            for (auto unit = shadow->UnitOf(surfaceBase + run.begin); unit <= shadow->UnitOf(surfaceBase + run.end - 1); ++unit) {
                Stats().verified.fetch_add(1, std::memory_order_relaxed);
                if (changed[static_cast<std::size_t>(unit - first)] != 0) Stats().mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return result;
}

ShadowAddressCollectCounts ShadowAddressCollectTotals() {
    auto& stats = AddressCollectStats();
    return {stats.calls.load(), stats.spanPages.load(), stats.walkedPages.load(), stats.runs.load(), stats.verified.load(), stats.mismatches.load()};
}

std::vector<ShadowAddressRun> ShadowAddressSources(const Context& context, const HostImport& import, std::uint64_t begin, std::uint64_t end, bool uploadReuse) {
    std::vector<ShadowAddressRun> result;
    if (!ShadowAddressEnabled() || !context.bufferDeviceAddress || end <= begin) return result;
    Require(GuestMemory::GpuMutex().HeldByThisThread(), "shadow address sources require the GPU lock");
    auto& registry = Registry();
    std::shared_ptr<UnitShadow> shadow;
    std::uint64_t first = 0, last = 0;
    {
        std::lock_guard lock(registry.mutex);
        shadow = find(registry, import);
        if (shadow == nullptr || shadow->context.device != context.device || shadow->liveUnits == 0) return result;
        begin = std::max(begin, shadow->importBase);
        end = std::min(end, shadow->ImportEnd());
        if (end <= begin) return result;
        first = shadow->UnitOf(begin);
        last = shadow->UnitOf(end - 1);
        while (first <= last && !shadow->Live(first)) ++first;
        while (first <= last && !shadow->Live(last)) --last;
        if (first > last) return result;
    }
    const auto walkBegin = shadow->UnitRange(first).first;
    const auto walkEnd = shadow->UnitRange(last).second;
    std::vector<std::uint8_t> boundedChanged, boundedCpu;
    static const bool verify = [] {
        const auto* value = std::getenv("APS5_VERIFY_DISPATCH_UPLOAD_REUSE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    if (uploadReuse) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> walks;
        {
            std::lock_guard lock(registry.mutex);
            for (auto unit = first; unit <= last; ++unit) {
                if (!shadow->Live(unit)) continue;
                const auto range = shadow->UnitRange(unit);
                if (!walks.empty() && walks.back().second == range.first) walks.back().second = range.second;
                else walks.push_back(range);
            }
        }
        if (verify) {
            std::lock_guard lock(registry.mutex);
            std::size_t run = 0;
            for (auto unit = first; unit <= last; ++unit) {
                if (!shadow->Live(unit)) continue;
                const auto range = shadow->UnitRange(unit);
                while (run < walks.size() && walks[run].second <= range.first) ++run;
                if (run == walks.size() || walks[run].first > range.first || walks[run].second < range.second) {
                    AddressCollectStats().mismatches.fetch_add(1, std::memory_order_relaxed);
                    ArchB::Recorder::Count("dispatch_upload_shadow_verify_mismatch");
                    throw std::runtime_error("dispatch upload bounded collection omits a live unit");
                }
            }
        }
        const auto pages = [](std::uint64_t from, std::uint64_t to) { return (to + 4095u) / 4096u - from / 4096u; };
        std::uint64_t walked = 0;
        for (const auto& [from, to] : walks) {
            GuestMemory::CollectWritesUncached(from, static_cast<std::size_t>(to - from));
            walked += pages(from, to);
        }
        auto& stats = AddressCollectStats();
        stats.calls.fetch_add(1, std::memory_order_relaxed);
        stats.spanPages.fetch_add(pages(walkBegin, walkEnd), std::memory_order_relaxed);
        stats.walkedPages.fetch_add(walked, std::memory_order_relaxed);
        stats.runs.fetch_add(walks.size(), std::memory_order_relaxed);
        ArchB::Recorder::Count("dispatch_upload_shadow_collect_calls");
        ArchB::Recorder::Count("dispatch_upload_shadow_span_pages", static_cast<double>(pages(walkBegin, walkEnd)));
        ArchB::Recorder::Count("dispatch_upload_shadow_walked_pages", static_cast<double>(walked));
        ArchB::Recorder::Count("dispatch_upload_shadow_live_runs", static_cast<double>(walks.size()));
        if (verify) {
            {
                std::lock_guard lock(registry.mutex);
                changedUnits(*shadow, first, last, boundedChanged, boundedCpu);
            }
            GuestMemory::CollectWritesUncached(walkBegin, static_cast<std::size_t>(walkEnd - walkBegin));
        }
    } else {
        GuestMemory::CollectWritesUncached(walkBegin, static_cast<std::size_t>(walkEnd - walkBegin));
    }
    struct ReferenceSource {
        std::uint64_t begin, end;
        VkDeviceAddress address;
        const ShadowSlab* slab;
    };
    std::vector<ReferenceSource> referenceSources;
    std::vector<std::uint8_t> fallback(static_cast<std::size_t>(last - first + 1), 0);
    bool publish = false;
    std::uint64_t bytes = 0, saved = 0, savedUnits = 0;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    {
        std::lock_guard lock(registry.mutex);
        std::vector<std::uint8_t> changed, cpu;
        changedUnits(*shadow, first, last, changed, cpu);
        if (uploadReuse && verify) {
            for (auto unit = first; unit <= last; ++unit) {
                if (!shadow->Live(unit)) continue;
                const auto k = static_cast<std::size_t>(unit - first);
                if (boundedChanged[k] != changed[k] || boundedCpu[k] != cpu[k]) {
                    AddressCollectStats().mismatches.fetch_add(1, std::memory_order_relaxed);
                    ArchB::Recorder::Count("dispatch_upload_shadow_verify_mismatch");
                    throw std::runtime_error("dispatch upload shadow bounded collect disagrees with old span; intervening CPU write possible but unproven");
                }
            }
            for (auto unit = first; unit <= last; ++unit) {
                const auto k = static_cast<std::size_t>(unit - first);
                if (!shadow->Live(unit) || boundedChanged[k] != 0) continue;
                const auto& slab = shadow->slabs[static_cast<std::size_t>(unit / SlabUnits())];
                if (slab == nullptr || slab->address == 0) continue;
                const auto range = shadow->UnitRange(unit);
                const auto from = std::max(begin, range.first);
                const auto to = std::min(end, range.second);
                if (!referenceSources.empty() && referenceSources.back().end == from && referenceSources.back().slab == slab.get()) referenceSources.back().end = to;
                else referenceSources.push_back({from, to, slab->address + SlabOffset(import, *slab, from), slab.get()});
            }
        }
        for (auto unit = first; unit <= last; ++unit) {
            if (!shadow->Live(unit)) continue;
            const auto k = static_cast<std::size_t>(unit - first);
            const auto& slab = shadow->slabs[static_cast<std::size_t>(unit / SlabUnits())];
            if (changed[k] != 0 || slab == nullptr || slab->address == 0) {
                fallback[k] = 1;
                publish = true;
                continue;
            }
            const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
            const auto from = std::max(begin, unitBegin);
            const auto to = std::min(end, unitEnd);
            const auto address = slab->address + SlabOffset(import, *slab, from);
            if (!result.empty() && result.back().end == from && result.back().pin->slab == slab) {
                result.back().end = to;
            } else {
                result.push_back({from, to, address, std::make_shared<ShadowSlabPin>(slab)});
            }
            slab->lastUse = Recorder::Presents();
            if (profile) {
                bytes += to - from;
                if (shadow->addressed[unit] == 0) {
                    saved += unitEnd - unitBegin;
                    ++savedUnits;
                    shadow->addressed[unit] = 1;
                }
            }
        }
    }
    if (uploadReuse && verify) {
        bool same = referenceSources.size() == result.size();
        for (std::size_t i = 0; same && i < result.size(); ++i) {
            const auto& expected = referenceSources[i];
            const auto& actual = result[i];
            same = expected.begin == actual.begin && expected.end == actual.end && expected.address == actual.address && expected.slab == actual.pin->slab.get();
        }
        if (!same) {
            AddressCollectStats().mismatches.fetch_add(1, std::memory_order_relaxed);
            ArchB::Recorder::Count("dispatch_upload_shadow_verify_mismatch");
            throw std::runtime_error("dispatch upload shadow sources disagree with old span");
        }
        AddressCollectStats().verified.fetch_add(1, std::memory_order_relaxed);
        ArchB::Recorder::Count("dispatch_upload_shadow_verified");
    }
    if (publish) publishUnits(shadow, first, last, fallback, PublishReason::Address);
    if (profile && bytes != 0) {
        Stats().addressHandoffs.fetch_add(1, std::memory_order_relaxed);
        Stats().addressReadBytes.fetch_add(bytes, std::memory_order_relaxed);
        if (saved != 0) {
            Stats().addressPublishUnitsAvoided.fetch_add(savedUnits, std::memory_order_relaxed);
            Stats().addressBytesSaved.fetch_add(saved, std::memory_order_relaxed);
        }
    }
    return result;
}

void MarkShadowed(const HostImport& import, std::span<const ShadowedRange> ranges, std::uint64_t generation) {
    if (!UnitShadowEnabled() || ranges.empty() || generation == 0) return;
    auto& registry = Registry();
    std::uint64_t bytes = 0;
    std::uint64_t lost = 0;
    // Arch C P3b: each range's whole units (what AnyShadowedOverlaps tests), noted as PendingInsert events below.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> fresh;
    {
        std::lock_guard lock(registry.mutex);
        const auto shadow = find(registry, import);
        if (shadow == nullptr) return;
        const auto present = Recorder::Presents();
        for (const auto& [begin, end, slab] : ranges) {
            if (end <= begin) continue;
            bytes += end - begin;
            fresh.emplace_back(shadow->UnitRange(shadow->UnitOf(begin)).first, shadow->UnitRange(shadow->UnitOf(end - 1)).second);
            for (auto unit = shadow->UnitOf(begin); unit <= shadow->UnitOf(end - 1); ++unit) {
                const auto& current = shadow->slabs[static_cast<std::size_t>(unit / SlabUnits())];
                if (current == nullptr || current != slab) {
                    // The copies went into a slab the shadow no longer holds: the import keeps
                    // whatever it had, as after a refused piece.
                    ++lost;
                    continue;
                }
                if (!shadow->Live(unit)) ++shadow->liveUnits;
                shadow->generation[unit] = generation;
                shadow->published[unit] = 0;
                shadow->lazyTicket[unit] = shadow->lazyTicket[unit] != 0 ? ++registry.lazySerial : 0;
                shadow->lazySettled[unit] = 0;
                if (!shadow->addressed.empty()) shadow->addressed[unit] = 0;
                current->lastUse = present;
            }
        }
        if (ShadowVerify() && shadow->liveUnits != shadow->CountLive()) throw std::runtime_error("unit-shadow live count differs after mark");
    }
    if (lost != 0) {
        Stats().slabLost.fetch_add(lost, std::memory_order_relaxed);
        static std::atomic<int> reports{0};
        if (reports.fetch_add(1) < 4) std::fprintf(stderr, "[shadow] %llu retiled units of import 0x%llx lost their slab before they were marked\n", static_cast<unsigned long long>(lost), static_cast<unsigned long long>(import.base));
    }
    Stats().retiled.fetch_add(1, std::memory_order_relaxed);
    Stats().retiledBytes.fetch_add(bytes, std::memory_order_relaxed);
    // The resource cache's pending memos (fastRevalidate) must see the change of the surface's
    // source, as they see a write-back; Arch C P3b's pending-scan tokens see it as a pending insert over the units.
    StorageTexture::BumpPendingSerial();
    for (const auto& [begin, end] : fresh) ArchC::ImageEpochs::Note(begin, end, ArchC::EpochSource::PendingInsert, nullptr);
}

void RetireShadow(VkDevice device, const HostImport& import, const std::function<bool(std::uint64_t, std::uint64_t)>& registered) {
    if (!UnitShadowEnabled()) return;
    auto& registry = Registry();
    std::shared_ptr<UnitShadow> shadow;
    std::vector<std::uint8_t> selected;
    bool any = false;
    std::uint64_t dropped = 0;
    {
        std::lock_guard lock(registry.mutex);
        shadow = find(registry, import);
        if (shadow == nullptr) return;
        // The memory the title took back holds its bytes now: only a unit still inside a readable
        // registered range receives its results.
        selected.assign(static_cast<std::size_t>(shadow->Units()), 0);
        for (std::uint64_t unit = 0; unit < shadow->Units(); ++unit) {
            if (!shadow->Live(unit)) continue;
            const auto [begin, end] = shadow->UnitRange(unit);
            if (end > begin && registered(begin, end)) {
                selected[static_cast<std::size_t>(unit)] = 1;
                any = true;
            } else {
                shadow->generation[unit] = 0;
                ++dropped;
            }
        }
        shadow->Recount();
    }
    if (dropped != 0) Stats().droppedOnRetire.fetch_add(dropped, std::memory_order_relaxed);
    std::size_t published = 0;
    if (any) {
        if (GuestMemory::GpuMutex().HeldByThisThread() && shadow->context.device == device) {
            published = publishUnits(shadow, 0, shadow->Units() - 1, selected, PublishReason::Retire);
        } else {
            Stats().lostOnRetire.fetch_add(shadow->liveUnits, std::memory_order_relaxed);
            static std::atomic<int> reports{0};
            if (reports.fetch_add(1) < 4) std::fprintf(stderr, "[shadow] import 0x%llx+0x%llx retired outside the device lock: %u shadowed units lost\n", static_cast<unsigned long long>(import.base), static_cast<unsigned long long>(import.bytes), shadow->liveUnits);
        }
    }
    std::lock_guard lock(registry.mutex);
    const auto found = registry.byBase.find(import.base);
    if (found == registry.byBase.end() || found->second != shadow) return;
    for (const auto& slab : shadow->slabs) {
        if (slab == nullptr) continue;
        registry.liveBytes -= SlabBytes(*slab);
        --registry.liveSlabs;
    }
    ++registry.retired;
    if (TraceEnabled()) std::fprintf(stderr, "[shadow] retire import 0x%llx+0x%llx: %zu units published, %llu dropped (memory no longer registered)\n", static_cast<unsigned long long>(import.base), static_cast<unsigned long long>(import.bytes), published, static_cast<unsigned long long>(dropped));
    registry.byBase.erase(found);
}

void PublishAllShadows(const Context& context, PublishReason reason) {
    if (!UnitShadowEnabled()) return;
    std::vector<std::shared_ptr<UnitShadow>> shadows;
    {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        for (const auto& [base, shadow] : registry.byBase) {
            if (shadow->context.device == context.device) shadows.push_back(shadow);
        }
    }
    for (const auto& shadow : shadows) publishRange(shadow, shadow->importBase, shadow->ImportEnd(), PublishScope::Whole, reason);
}

void DestroyShadows(VkDevice device) {
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    for (auto it = registry.byBase.begin(); it != registry.byBase.end();) {
        if (it->second->context.device != device) {
            ++it;
            continue;
        }
        for (const auto& slab : it->second->slabs) {
            if (slab == nullptr) continue;
            registry.liveBytes -= SlabBytes(*slab);
            --registry.liveSlabs;
        }
        it = registry.byBase.erase(it);
    }
}

namespace {

struct ConsumerStorage {};

ShadowConsumerScope::State& consumerState() {
    return HostThreadLocal<ShadowConsumerScope::State, ConsumerStorage>();
}

std::uint64_t lazyVerifyEvery() {
    static const std::uint64_t every = [] {
        const char* text = std::getenv("APS5_ARCHB_LAZY_SHADOW_PUBLISH_VERIFY");
        return text == nullptr ? std::uint64_t{0} : static_cast<std::uint64_t>(std::strtoull(text, nullptr, 10));
    }();
    return every;
}

bool consumerPublishes(const ShadowConsumerScope::State& consumer, std::uint64_t begin, std::uint64_t end) {
    if (!consumer.set || consumer.scope == PublishScope::None || consumer.bytes == 0) return false;
    const auto consumerEnd = consumer.bytes > std::numeric_limits<std::uint64_t>::max() - consumer.address ? std::numeric_limits<std::uint64_t>::max() : consumer.address + consumer.bytes;
    if (!(begin < consumerEnd && consumer.address < end)) return false;
    return consumer.scope == PublishScope::Whole || !(consumer.address <= begin && consumerEnd >= end);
}

struct LeftUnits {
    std::shared_ptr<UnitShadow> shadow;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::vector<std::uint8_t> selected;
};

void verifyLeft(const LeftUnits& left) {
    auto& counters = lazyCounters();
    const auto& shadow = left.shadow;
    const auto& context = shadow->context;
    auto* recorder = Recorder::Active();
    if (recorder != nullptr) {
        Recorder::CountSync(4);
        recorder->Sync();
    }
    const auto walkBegin = std::max(shadow->UnitBegin(left.first), shadow->importBase);
    const auto walkEnd = std::min(shadow->UnitBegin(left.last + 1), shadow->ImportEnd());
    if (walkEnd > walkBegin) GuestMemory::CollectWritesUncached(walkBegin, static_cast<std::size_t>(walkEnd - walkBegin));
    struct Region {
        std::shared_ptr<ShadowSlab> slab;
        VkDeviceSize offset;
        std::uint64_t address;
        std::uint64_t size;
    };
    std::vector<Region> regions;
    {
        std::lock_guard lock(Registry().mutex);
        std::vector<std::uint8_t> changed, cpu;
        changedUnits(*shadow, left.first, left.last, changed, cpu);
        for (auto unit = left.first; unit <= left.last; ++unit) {
            const auto k = static_cast<std::size_t>(unit - left.first);
            if (!left.selected[k] || !shadow->Live(unit) || changed[k] != 0) continue;
            const auto& slab = shadow->slabs[static_cast<std::size_t>(unit / SlabUnits())];
            if (slab == nullptr) continue;
            const auto [begin, end] = shadow->UnitRange(unit);
            if (end <= begin) continue;
            regions.push_back({slab, (unit - slab->firstUnit) * UnitBytes + (begin - shadow->UnitBegin(unit)), begin, end - begin});
        }
    }
    std::vector<std::unique_ptr<Buffer>> scratch;
    for (const auto& region : regions) {
        scratch.push_back(std::make_unique<Buffer>(context, static_cast<std::size_t>(region.size), VK_BUFFER_USAGE_TRANSFER_DST_BIT));
        CommandBatch batch(context);
        CopyBuffer(context, batch.Handle(), region.slab->buffer, region.offset, scratch.back()->Handle(), 0, region.size);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        scratch.back()->Invalidate();
    }
    const PublishSiteScope site("verify");
    GuestMemory::FlushGpuWrites(walkBegin, static_cast<std::size_t>(walkEnd - walkBegin));
    if (recorder != nullptr) {
        Recorder::CountSync(4);
        recorder->Sync();
    }
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const auto& region = regions[i];
        GuestArena::GuestWtFaultUnwatch_nid_no_patch(static_cast<std::uintptr_t>(region.address), static_cast<std::size_t>(region.size));
        counters.verifyChecks.fetch_add(1, std::memory_order_relaxed);
        if (GuestArena::GuestWtFaultWatchedPages_nid_no_patch(static_cast<std::uintptr_t>(region.address), static_cast<std::size_t>(region.size)) != 0 || !GuestMemory::Accessible(reinterpret_cast<const void*>(region.address), static_cast<std::size_t>(region.size))) {
            counters.verifySkipped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (std::memcmp(reinterpret_cast<const void*>(region.address), scratch[i]->Bytes().data(), static_cast<std::size_t>(region.size)) == 0) continue;
        counters.verifyMismatches.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> reports{0};
        if (reports.fetch_add(1) < 8) std::fprintf(stderr, "[lazy-shadow] VERIFY mismatch: the forced publication of 0x%llx+0x%llx differs from the eager copy\n", static_cast<unsigned long long>(region.address), static_cast<unsigned long long>(region.size));
        throw std::runtime_error("lazy shadow forced publication differs from eager bytes");
    }
}

}

namespace {

bool needsLazyPreservation(std::uintptr_t address, std::size_t bytes) {
    if (bytes == 0) return false;
    const auto end = bytes > std::numeric_limits<std::uint64_t>::max() - address ? std::numeric_limits<std::uint64_t>::max() : address + bytes;
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    if (anyOverlapping(registry, address, end, [&](const auto& shadow) {
        const auto begin = std::max<std::uint64_t>(address, shadow->importBase);
        const auto stop = std::min(end, shadow->ImportEnd());
        for (auto unit = shadow->UnitOf(begin); unit <= shadow->UnitOf(stop - 1); ++unit) {
            if (shadow->lazyTicket[unit] != 0) return true;
        }
        return false;
    })) return true;
    for (const auto& [ticket, ranges] : registry.publishing) {
        for (const auto& [begin, stop] : ranges) {
            if (begin < end && address < stop) return true;
        }
    }
    return false;
}

}

bool LazyShadowVerifyEnabled() {
    return lazyVerifyEvery() != 0;
}

bool LazyShadowPublishEnabled() {
    if (!UnitShadowEnabled() || !AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::LazyShadowPublish) || !ArchC::LazyPublish::Active()) return false;
    static const bool installed = [] {
        GuestArena::GuestWtFaultSetLazyPreservation_nid_no_patch(&needsLazyPreservation);
        return true;
    }();
    return installed;
}

ShadowConsumerScope::ShadowConsumerScope(const char* reason, std::uint64_t address, std::size_t bytes, PublishScope scope) : previous(consumerState()) {
    consumerState() = {reason, address, bytes, scope, true};
}

ShadowConsumerScope::~ShadowConsumerScope() {
    consumerState() = previous;
}

PublishScope PlaneStoreScope(const char* store) {
    if (!LazyShadowPublishEnabled()) return PublishScope::Whole;
    const auto& consumer = consumerState();
    lazyCounters().lazyStores.fetch_add(1, std::memory_order_relaxed);
    countSite("n5a_store_", consumer.reason != nullptr ? consumer.reason : store, 0);
    return PublishScope::None;
}

void SettlePlaneStore(const char* store, std::uint64_t begin, std::uint64_t end, PublishScope used) {
    if (used != PublishScope::None || end <= begin || !UnitShadowEnabled()) return;
    auto& counters = lazyCounters();
    const auto consumer = consumerState();
    std::lock_guard gpu(GuestMemory::GpuMutex());
    const auto every = lazyVerifyEvery();
    const bool verify = every != 0 && Recorder::Presents() % every == 0;
    std::vector<LeftUnits> lefts;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::uint64_t leftBytes = 0;
    {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        for (const auto& shadow : overlapping(registry, begin, end)) {
            if (shadow->liveUnits == 0) continue;
            const auto from = std::max(begin, shadow->importBase);
            const auto to = std::min(end, shadow->ImportEnd());
            if (to <= from) continue;
            LeftUnits left{shadow, shadow->UnitOf(from), shadow->UnitOf(to - 1), {}};
            left.selected.assign(static_cast<std::size_t>(left.last - left.first + 1), 0);
            bool any = false;
            for (auto unit = left.first; unit <= left.last; ++unit) {
                if (!shadow->Live(unit) || shadow->lazySettled[unit] != 0) continue;
                const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
                if (unitEnd <= unitBegin || consumerPublishes(consumer, unitBegin, unitEnd)) continue;
                if (shadow->lazyTicket[unit] == 0) shadow->lazyTicket[unit] = ++registry.lazySerial;
                shadow->lazySettled[unit] = 1;
                left.selected[static_cast<std::size_t>(unit - left.first)] = 1;
                any = true;
                leftBytes += unitEnd - unitBegin;
                ranges.emplace_back(unitBegin, unitEnd);
            }
            if (any) lefts.push_back(std::move(left));
        }
    }
    counters.leftBytes.fetch_add(leftBytes, std::memory_order_relaxed);
    if (lefts.empty()) return;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> fallback;
    for (const auto& [rangeBegin, rangeEnd] : ranges) {
        const auto bytes = static_cast<std::size_t>(rangeEnd - rangeBegin);
        if (ArchC::LazyPublish::DeferShadowRange(rangeBegin, bytes)) {
            counters.enrolled.fetch_add(1, std::memory_order_relaxed);
            counters.enrolledBytes.fetch_add(bytes, std::memory_order_relaxed);
            if (store != nullptr && (std::strcmp(store, "depth plane store") == 0 || std::strcmp(store, "stencil plane store") == 0 || std::strcmp(store, "address-based heaps") == 0)) {
                counters.avoided.fetch_add(1, std::memory_order_relaxed);
                counters.avoidedBytes.fetch_add(bytes, std::memory_order_relaxed);
            }
            continue;
        }
        counters.unwatchable.fetch_add(1, std::memory_order_relaxed);
        counters.unwatchableBytes.fetch_add(bytes, std::memory_order_relaxed);
        counters.eagerStores.fetch_add(1, std::memory_order_relaxed);
        if (!fallback.empty() && fallback.back().second == rangeBegin) fallback.back().second = rangeEnd;
        else fallback.emplace_back(rangeBegin, rangeEnd);
    }
    for (const auto& [from, to] : fallback) static_cast<void>(StorageTexture::PublishShadowsOnly(from, static_cast<std::size_t>(to - from), PublishScope::Whole, store));
    if (verify) for (const auto& left : lefts) verifyLeft(left);
}

std::size_t LazyLeftUnpublished(std::uint64_t address, std::size_t bytes) {
    if (!UnitShadowEnabled() || bytes == 0) return 0;
    const auto end = bytes > std::numeric_limits<std::uint64_t>::max() - address ? std::numeric_limits<std::uint64_t>::max() : address + bytes;
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    std::size_t units = 0;
    for (const auto& shadow : overlapping(registry, address, end)) {
        if (shadow->liveUnits == 0) continue;
        const auto from = std::max(address, shadow->importBase);
        const auto to = std::min(end, shadow->ImportEnd());
        if (to <= from) continue;
        for (auto unit = shadow->UnitOf(from); unit <= shadow->UnitOf(to - 1); ++unit) {
            if (shadow->Live(unit)) ++units;
        }
    }
    return units;
}

void NoteLazyShadowRead(std::uint64_t address, std::size_t bytes, PublishScope scope) {
    if (lazyVerifyEvery() == 0 || scope != PublishScope::Whole || bytes == 0) return;
    auto& counters = lazyCounters();
    counters.staleChecks.fetch_add(1, std::memory_order_relaxed);
    const auto units = LazyLeftUnpublished(address, bytes);
    if (units == 0) return;
    counters.staleReads.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<int> reports{0};
    if (reports.fetch_add(1) < 8) std::fprintf(stderr, "[lazy-shadow] VERIFY: a CPU read of 0x%llx+0x%zx found %zu unpublished units a lazy plane store left (read site %s)\n", static_cast<unsigned long long>(address), bytes, units, GuestMemory::ReadSiteName(GuestMemory::CurrentReadSite()));
    throw std::runtime_error("lazy shadow CPU reader found unpublished bytes");
}

LazyShadowCounts LazyShadowTotals() {
    auto& counters = lazyCounters();
    LazyShadowCounts counts;
    counts.lazyStores = counters.lazyStores.load();
    counts.eagerStores = counters.eagerStores.load();
    counts.leftBytes = counters.leftBytes.load();
    counts.enrolledBytes = counters.enrolledBytes.load();
    counts.unwatchableBytes = counters.unwatchableBytes.load();
    counts.verifyChecks = counters.verifyChecks.load();
    counts.verifyMismatches = counters.verifyMismatches.load();
    counts.staleReads = counters.staleReads.load();
    counts.publishedUnits = counters.publishedUnits.load();
    return counts;
}

std::string ShadowReport() {
    auto& statistics = Stats();
    const auto take = [](std::atomic<std::uint64_t>& counter) { return static_cast<unsigned long long>(counter.exchange(0, std::memory_order_relaxed)); };
    const auto mib = [](unsigned long long bytes) { return bytes / 1048576.0; };
    char text[256];
    std::string line;
    if (!UnitShadowEnabled()) return "; shadow: off";
    std::snprintf(text, sizeof(text), "; shadow: retiled %llu/%.1f (refused %llu/%.1f to import), seeds %llu/%.1f, detiled from shadow %llu/%.1f, from import %llu/%.1f, tail publishes %llu, published", take(statistics.retiled), mib(take(statistics.retiledBytes)), take(statistics.refused), mib(take(statistics.refusedBytes)), take(statistics.seeds), mib(take(statistics.seedBytes)), take(statistics.detiledShadow), mib(take(statistics.detiledShadowBytes)), take(statistics.detiledImport), mib(take(statistics.detiledImportBytes)), take(statistics.tailPublishes));
    line += text;
    unsigned long long publishedTotal = 0, publishedBytesTotal = 0;
    std::string reasons;
    for (std::size_t i = 0; i < ReasonCount; ++i) {
        const auto count = take(statistics.published[i]);
        const auto bytes = take(statistics.publishedBytes[i]);
        publishedTotal += count;
        publishedBytesTotal += bytes;
        if (count == 0) continue;
        std::snprintf(text, sizeof(text), " %s %llu/%.1f", ReasonNames[i], count, mib(bytes));
        reasons += text;
    }
    const auto cpuDropped = take(statistics.staleDroppedCpu), driverDropped = take(statistics.staleDroppedDriver);
    unsigned long long liveSlabs = 0, liveBytes = 0, peakBytes = 0, retired = 0;
    {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        liveSlabs = registry.liveSlabs;
        liveBytes = registry.liveBytes;
        peakBytes = registry.peakBytes;
        retired = registry.retired;
    }
    std::snprintf(text, sizeof(text), " %llu/%.1f by reason {", publishedTotal, mib(publishedBytesTotal));
    line += text;
    line += reasons;
    std::snprintf(text, sizeof(text), " }, stale-dropped %llu (cpu %llu, driver %llu), skipped-in-completion %llu, slabs %llu live %.1f MiB / peak %.1f MiB, evictions %llu, slab lost %llu, retired %llu, dropped on retire %llu, lost on retire %llu", cpuDropped + driverDropped, cpuDropped, driverDropped, take(statistics.skippedInCompletion), liveSlabs, mib(liveBytes), mib(peakBytes), take(statistics.evictions), take(statistics.slabLost), retired, take(statistics.droppedOnRetire), take(statistics.lostOnRetire));
    line += text;
    std::snprintf(text, sizeof(text), "; BDA shadow handoffs %llu/%.1f MiB, publish units avoided %llu/%.1f MiB (first handoff per retile); hook consumers {", take(statistics.addressHandoffs), mib(take(statistics.addressReadBytes)), take(statistics.addressPublishUnitsAvoided), mib(take(statistics.addressBytesSaved)));
    line += text;
    for (std::size_t i = 0; i < statistics.hookPublished.size(); ++i) {
        const auto count = take(statistics.hookPublished[i]);
        const auto bytes = take(statistics.hookPublishedBytes[i]);
        if (count == 0) continue;
        std::snprintf(text, sizeof(text), " %s %llu/%.1f", GuestMemory::ReadSiteName(static_cast<GuestMemory::ReadSite>(i)), count, mib(bytes));
        line += text;
    }
    line += " }";
    if (ShadowVerify()) {
        std::snprintf(text, sizeof(text), ", verified %llu, mismatches %llu", take(statistics.verified), take(statistics.mismatches));
        line += text;
    }
    return line;
}

}
