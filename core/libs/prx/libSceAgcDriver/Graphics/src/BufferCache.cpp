#include "prx/libSceAgcDriver/Graphics/include/BufferCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramOptions.hpp"
#include "prx/libSceAgcDriver/Execution/include/PageTracker.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <chrono>
#include <map>

namespace AgcDriver::Graphics::BufferCache {
namespace {

using PageTracker::PageBytes;
constexpr std::size_t BlockPages = BlockBytes / PageBytes;
struct Block;
#ifdef _WIN32
struct FaultPage {
    std::weak_ptr<Block> block;
    std::uint64_t address = 0;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    bool armed = false;
};
struct FaultResources {
    Context context;
    Buffer staging;
    const void* data;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    PFN_vkQueueSubmit submit;
    PFN_vkWaitForFences wait;
    PFN_vkResetFences reset;
    std::array<std::shared_ptr<FaultPage>, BlockPages> pages{};
    explicit FaultResources(const Context& context) : context(context), staging(context, PageBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT), data(staging.ReadBytes(0, PageBytes).data()),
        submit(context.Function<PFN_vkQueueSubmit>("vkQueueSubmit")), wait(context.Function<PFN_vkWaitForFences>("vkWaitForFences")), reset(context.Function<PFN_vkResetFences>("vkResetFences")) {
        try {
            VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.queueFamilyIndex = context.queueFamilyIndex;
            Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateCommandPool read faults");
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            Check(context.Function<PFN_vkCreateFence>("vkCreateFence")(context.device, &fenceInfo, nullptr, &fence), "vkCreateFence read faults");
        } catch (...) { Release(); throw; }
    }
    void Release() {
        if (pool != VK_NULL_HANDLE) context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, pool, nullptr);
        if (fence != VK_NULL_HANDLE) context.Function<PFN_vkDestroyFence>("vkDestroyFence")(context.device, fence, nullptr);
    }
    ~FaultResources() { Release(); }
};
#endif
struct Block {
    Context context;
    std::shared_ptr<Buffer> buffer;
    std::array<PageTracker::Epoch, BlockPages> pages;
    PageTracker::Bitmap valid{}, dirty{}, owned{};
    std::vector<std::byte> original;
    std::weak_ptr<const GuestAllocations::Range> mapping;
    std::shared_ptr<const GuestAllocations::Range> writerMapping;
    std::size_t pending = 0, reservations = 0;
    std::uint64_t used = 0;
#ifdef _WIN32
    std::unique_ptr<FaultResources> faults;
#endif
};
struct Binding {
    PageTracker::Range range;
    std::uint64_t base;
    std::shared_ptr<Block> block;
    VkDeviceSize alignment;
    bool written = false;
    bool begun = false;
    std::vector<PageTracker::Epoch> proofs;
};
struct Upload {
    std::uint64_t begin, end;
    std::shared_ptr<bool> completed;
};
struct Device {
    std::map<std::uint64_t, std::shared_ptr<Block>> blocks;
    std::vector<std::weak_ptr<Block>> live;
    std::shared_ptr<Buffer> stream;
    std::vector<Upload> uploads;
    std::uint64_t cursor = 0, uses = 0;
};
struct Session {
    bool permitted = false;
    VkDevice device = VK_NULL_HANDLE;
    std::vector<Binding> bindings;
};
struct State {
    std::map<VkDevice, Device> devices;
    std::map<const void*, Session> sessions;
    Statistics counts;
    std::chrono::steady_clock::time_point reported{};
    bool flushing = false;
};
State& Cache() { static State state; return state; }

bool Valid(const Block& block, std::size_t page) { return (block.valid[page / 64] & (1ull << (page % 64))) != 0; }
void Validate(Block& block, std::size_t page) { block.valid[page / 64] |= 1ull << (page % 64); }
bool AnyDirty(const Block& block) { return std::any_of(block.dirty.begin(), block.dirty.end(), [](auto word) { return word != 0; }); }

#ifdef _WIN32
bool DownloadFault(FaultPage& page) noexcept {
    const auto owner = page.block.lock();
    if (owner == nullptr) return !page.armed;
    auto& block = *owner;
    if (!page.armed) return true;
    auto& resources = *block.faults;
    VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &page.commands;
    if (resources.reset(block.context.device, 1, &resources.fence) != VK_SUCCESS) return false;
    if (resources.submit(block.context.queue, 1, &submission, resources.fence) != VK_SUCCESS) return false;
    if (resources.wait(block.context.device, 1, &resources.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) std::terminate();
    if (!GuestArena::GuestArenaPublishPageRead_nid_no_patch(page.address, resources.data)) return false;
    const auto index = (page.address % BlockBytes) / PageBytes;
    const auto mask = 1ull << (index % 64);
    block.dirty[index / 64] &= ~mask;
    block.valid[index / 64] &= ~mask;
    block.owned[index / 64] &= ~mask;
    page.armed = false;
    Cache().counts.copiedBack += PageBytes;
    if (!AnyDirty(block)) block.writerMapping.reset();
    return true;
}

bool ReadFault(void* context) noexcept {
    return PageTracker::ResolveReadback(context, [](void* context, bool held) noexcept {
        ++Cache().counts.readFaults;
        if (held) ++Cache().counts.lockedFaults;
        return DownloadFault(*static_cast<FaultPage*>(context));
    });
}

void PrepareFaults(const std::shared_ptr<Block>& owner, std::uint64_t base, std::size_t first, std::size_t last) {
    auto& block = *owner;
    if (!ReadFaultsEnabled() || SubmitWorkerEnabled()) return;
    try {
        if (!PageTracker::PrepareReadbacks()) return;
        if (block.faults == nullptr) block.faults = std::make_unique<FaultResources>(block.context);
        auto& resources = *block.faults;
        for (auto page = first; page < last; ++page) {
            auto& fault = resources.pages[page];
            if (fault != nullptr) continue;
            VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocation.commandPool = resources.pool;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocation.commandBufferCount = 1;
            VkCommandBuffer commands = VK_NULL_HANDLE;
            Check(block.context.Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(block.context.device, &allocation, &commands), "vkAllocateCommandBuffers read faults");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            Check(block.context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin), "vkBeginCommandBuffer read faults");
            RecordMemoryBarrier(block.context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            CopyBuffer(block.context, commands, block.buffer->Handle(), page * PageBytes, resources.staging.Handle(), 0, PageBytes);
            RecordMemoryBarrier(block.context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            Check(block.context.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands), "vkEndCommandBuffer read faults");
            fault = std::make_shared<FaultPage>(FaultPage{owner, base + page * PageBytes, commands, false});
        }
    } catch (const std::exception&) { return; }
}

void ArmReads(const std::shared_ptr<Block>& block, std::uint64_t base) {
    if (!ReadFaultsEnabled() || block->pending != 0) return;
    for (std::size_t page = 0; page < BlockPages; ++page) {
        if ((block->dirty[page / 64] & (1ull << (page % 64))) == 0) continue;
        if (block->faults == nullptr || block->faults->pages[page] == nullptr || SubmitWorkerEnabled()) {
            ++Cache().counts.refusedReads;
            if (SubmitWorkerEnabled()) ++Cache().counts.submitWorkerRefusals;
            else ++Cache().counts.setupRefusals;
            continue;
        }
        auto& fault = *block->faults->pages[page];
        if (fault.armed) continue;
        fault.armed = true;
        auto refusal = GuestArena::PageArmRefusal::Protection;
        try {
            refusal = GuestArena::GuestArenaArmPageRead_nid_no_patch(base + page * PageBytes, block->faults->pages[page], block->original.data() + page * PageBytes, ReadFault, &fault);
        } catch (const std::bad_alloc&) { ++Cache().counts.setupRefusals; }
        if (refusal == GuestArena::PageArmRefusal::Count) ++Cache().counts.armedReads;
        else {
            fault.armed = false;
            ++Cache().counts.refusedReads;
            ++Cache().counts.readRefusals[static_cast<std::size_t>(refusal)];
        }
    }
}
#endif

bool Proven(PageTracker::Tracker& tracker, const Block& block, std::size_t page) {
    if (!Valid(block, page) || tracker.Poisoned()) return false;
    const auto& proof = block.pages[page];
    const auto current = tracker.CaptureEpoch(proof.range);
    return (proof.covered || (block.owned[page / 64] & (1ull << (page % 64))) != 0) && current.regions == proof.regions;
}

void Publish(Block& block, PageTracker::Range range, bool complete) {
    auto* tracker = PageTracker::Get();
    if (tracker == nullptr) return;
    std::array<bool, BlockPages> proven{};
    for (std::size_t page = 0; page < BlockPages; ++page) proven[page] = Proven(*tracker, block, page);
    if (complete) tracker->PublishGpuWrites(range);
    else tracker->MarkGpuWritten(range, PageTracker::Source::GpuDeclaration);
    for (std::size_t page = 0; page < BlockPages; ++page) if (proven[page]) {
        block.pages[page] = tracker->CaptureEpoch(block.pages[page].range);
        block.owned[page / 64] |= 1ull << (page % 64);
    }
}

void Retire(Device& device) {
    std::erase_if(device.uploads, [](const Upload& upload) {
        if (!*upload.completed) return false;
        ++Cache().counts.retired;
        return true;
    });
}

void Report() {
    auto& state = Cache();
    const auto now = std::chrono::steady_clock::now();
    if (now - state.reported < std::chrono::seconds(10)) return;
    state.reported = now;
    const auto& c = state.counts;
    AgcDriver::ProfilePrint_nid_no_patch("[buffer-cache] cumulative uploaded=%llu copied-back=%llu hits=%llu misses=%llu uploads=%llu ring-wraps=%llu fallbacks coverage=%llu size=%llu alias=%llu written=%llu addressable=%llu mapping=%llu alignment=%llu budget=%llu allocation=%llu changed=%llu resident=%llu\n",
        static_cast<unsigned long long>(c.uploaded), static_cast<unsigned long long>(c.copiedBack), static_cast<unsigned long long>(c.hits), static_cast<unsigned long long>(c.misses), static_cast<unsigned long long>(c.uploads), static_cast<unsigned long long>(c.wraps),
        static_cast<unsigned long long>(c.fallbacks[0]), static_cast<unsigned long long>(c.fallbacks[1]), static_cast<unsigned long long>(c.fallbacks[2]), static_cast<unsigned long long>(c.fallbacks[3]), static_cast<unsigned long long>(c.fallbacks[4]), static_cast<unsigned long long>(c.fallbacks[5]), static_cast<unsigned long long>(c.fallbacks[6]), static_cast<unsigned long long>(c.fallbacks[7]), static_cast<unsigned long long>(c.fallbacks[8]), static_cast<unsigned long long>(c.fallbacks[9]), static_cast<unsigned long long>(c.fallbacks[10]));
    if (WritesEnabled()) AgcDriver::ProfilePrint_nid_no_patch("[buffer-cache-writes] cumulative gpu-written=%llu completed=%llu retired-uploads=%llu ring-spills=%llu\n", static_cast<unsigned long long>(c.gpuWritten), static_cast<unsigned long long>(c.completedWrites), static_cast<unsigned long long>(c.retired), static_cast<unsigned long long>(c.spills));
    if (ReadFaultsEnabled()) {
        AgcDriver::ProfilePrint_nid_no_patch("[buffer-cache-read-faults] cumulative faults=%llu locked=%llu armed=%llu refused=%llu setup-refusals=%llu submit-worker-refusals=%llu sync-fallback-pages=%llu refusals invalid=%llu mapping=%llu identity=%llu metadata=%llu permission=%llu pinned=%llu host-writer=%llu host-alias=%llu protection=%llu\n",
            static_cast<unsigned long long>(c.readFaults), static_cast<unsigned long long>(c.lockedFaults), static_cast<unsigned long long>(c.armedReads), static_cast<unsigned long long>(c.refusedReads), static_cast<unsigned long long>(c.setupRefusals), static_cast<unsigned long long>(c.submitWorkerRefusals), static_cast<unsigned long long>(c.syncFallbackPages),
            static_cast<unsigned long long>(c.readRefusals[0]), static_cast<unsigned long long>(c.readRefusals[1]), static_cast<unsigned long long>(c.readRefusals[2]), static_cast<unsigned long long>(c.readRefusals[3]), static_cast<unsigned long long>(c.readRefusals[4]), static_cast<unsigned long long>(c.readRefusals[5]), static_cast<unsigned long long>(c.readRefusals[6]), static_cast<unsigned long long>(c.readRefusals[7]), static_cast<unsigned long long>(c.readRefusals[8]));
    }
}

bool Refuse(Fallback reason) {
    ++Cache().counts.fallbacks[static_cast<std::size_t>(reason)];
    Report();
    return false;
}

bool MakeRoom(Device& device) {
    const auto liveBytes = [&] {
        std::erase_if(device.live, [](const auto& block) { return block.expired(); });
        return device.live.size() * BlockBytes;
    };
    while (liveBytes() + BlockBytes > BudgetBytes) {
        auto oldest = device.blocks.end();
        for (auto it = device.blocks.begin(); it != device.blocks.end(); ++it) {
            if (it->second.use_count() != 1 || AnyDirty(*it->second)) continue;
            if (oldest == device.blocks.end() || it->second->used < oldest->second->used) oldest = it;
        }
        if (oldest == device.blocks.end()) return false;
        device.blocks.erase(oldest);
    }
    return true;
}

std::shared_ptr<Block> Acquire(const Context& context, Device& device, PageTracker::Tracker& tracker, PageTracker::Range range, std::uint64_t base, bool written) {
    const auto first = static_cast<std::size_t>((range.address - base) / PageBytes);
    const auto last = static_cast<std::size_t>((range.address - base + range.bytes) / PageBytes);
    const auto found = device.blocks.find(base);
    auto previous = found != device.blocks.end() ? found->second : nullptr;
    std::array<PageTracker::Epoch, BlockPages> proofs;
    std::vector<std::size_t> dirty;
    for (auto page = first; page < last; ++page) {
        const PageTracker::Range part{base + page * PageBytes, PageBytes};
        proofs[page] = tracker.CaptureEpoch(part);
        const bool proven = previous != nullptr && (WritesEnabled() ? Proven(tracker, *previous, page) : Valid(*previous, page) && !tracker.DirtySince(part, previous->pages[page]));
        if (!proofs[page].covered && !proven) { Refuse(Fallback::Coverage); return {}; }
        if (!proven) dirty.push_back(page);
    }
    if (previous != nullptr && (AnyDirty(*previous) || previous->reservations != 0) && (written || !dirty.empty())) { Refuse(Fallback::Changed); return {}; }
    if (dirty.empty() && !written) {
        ++Cache().counts.hits;
        previous->used = ++device.uses;
        return previous;
    }
    ++Cache().counts.misses;
    if (!MakeRoom(device)) { Refuse(Fallback::Budget); return {}; }
    auto next = std::make_shared<Block>();
    next->context = context;
    try {
        const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | (WritesEnabled() && context.bufferDeviceAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        next->buffer = std::make_shared<Buffer>(context, BlockBytes, usage, Buffer::Access::DeviceMirror);
        if (device.stream == nullptr) device.stream = std::make_shared<Buffer>(context, BlockBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Access::UploadOnly);
    } catch (const std::runtime_error&) { Refuse(Fallback::Allocation); return {}; }
    const auto uploadBytes = dirty.size() * PageBytes;
    if (device.cursor + uploadBytes > BlockBytes) { device.cursor = 0; ++Cache().counts.wraps; }
    Retire(device);
    auto stream = device.stream;
    auto start = device.cursor;
    if (WritesEnabled() && uploadBytes != 0 && std::any_of(device.uploads.begin(), device.uploads.end(), [&](const Upload& upload) { return start < upload.end && upload.begin < start + uploadBytes; })) {
        stream = std::make_shared<Buffer>(context, uploadBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Access::UploadOnly);
        start = 0;
        ++Cache().counts.spills;
    }
    std::vector<VkBufferCopy> uploads, retained;
    auto cursor = start;
    GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::BufferUpload);
    for (const auto page : dirty) {
        GuestMemory::Read(base + page * PageBytes, stream->WriteBytes().subspan(cursor, PageBytes));
        uploads.push_back({cursor, page * PageBytes, PageBytes});
        cursor += PageBytes;
    }
    for (auto page = first; page < last; ++page) {
        const bool changed = WritesEnabled() && previous != nullptr && std::find(dirty.begin(), dirty.end(), page) == dirty.end()
            ? !Proven(tracker, *previous, page) : tracker.DirtySince({base + page * PageBytes, PageBytes}, proofs[page]);
        if (changed) { Refuse(Fallback::Changed); return {}; }
    }
    if (previous != nullptr) for (std::size_t page = 0; page < BlockPages; ++page) {
        const PageTracker::Range part{base + page * PageBytes, PageBytes};
        if (!(WritesEnabled() ? Proven(tracker, *previous, page) : Valid(*previous, page) && !tracker.DirtySince(part, previous->pages[page]))) continue;
        retained.push_back({page * PageBytes, page * PageBytes, PageBytes});
        next->pages[page] = previous->pages[page];
        Validate(*next, page);
        next->owned[page / 64] |= previous->owned[page / 64] & (1ull << (page % 64));
    }
    const auto record = [&](VkCommandBuffer commands) {
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        if (!retained.empty()) CopyBuffers(context, commands, previous->buffer->Handle(), next->buffer->Handle(), static_cast<std::uint32_t>(retained.size()), retained.data());
        if (!uploads.empty()) CopyBuffers(context, commands, stream->Handle(), next->buffer->Handle(), static_cast<std::uint32_t>(uploads.size()), uploads.data());
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | (WritesEnabled() ? VK_ACCESS_SHADER_WRITE_BIT : 0));
    };
    if (auto* recorder = Recorder::Active(); WritesEnabled() && recorder != nullptr) {
        recorder->Keep(stream);
        recorder->Keep(next);
        if (previous != nullptr) recorder->Keep(previous);
        if (uploadBytes != 0 && stream == device.stream) {
            auto completed = std::make_shared<bool>(false);
            recorder->OnComplete([completed] { *completed = true; });
            device.uploads.push_back({start, cursor, std::move(completed)});
        }
        record(recorder->Commands());
    } else {
        CommandBatch batch(context, CommandBatch::Mode::Independent);
        record(batch.Handle());
        batch.SubmitAndWait();
    }
    if (stream == device.stream) device.cursor = cursor;
    Cache().counts.uploaded += uploadBytes;
    ++Cache().counts.uploads;
    for (const auto page : dirty) { next->pages[page] = std::move(proofs[page]); Validate(*next, page); }
    next->used = ++device.uses;
    device.live.push_back(next);
    device.blocks[base] = next;
    return next;
}

}

bool Enabled() {
    static const bool enabled = VramOption("APS5_BUFFER_CACHE") && PageTracker::Enabled();
    return enabled;
}

bool WritesEnabled() {
    static const bool enabled = VramOption("APS5_BUFFER_CACHE_WRITES") && Enabled() && !ResidentHeapMirrors() && !VramMirrorWrites();
    return enabled;
}

bool ReadFaultsEnabled() {
#ifdef _WIN32
    static const bool enabled = VramOption("APS5_BUFFER_CACHE_READ_FAULTS") && WritesEnabled() && PageTracker::ArmEnabled();
    return enabled;
#else
    return false;
#endif
}

void PermitBindings(const void* owner, bool permitted) {
    if (!Enabled()) return;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    Cache().sessions[owner].permitted = permitted;
}

bool BindingsPermitted(const void* owner) {
    if (!Enabled()) return false;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    const auto found = Cache().sessions.find(owner);
    return found != Cache().sessions.end() && found->second.permitted;
}

namespace {
bool Bind(const Context& context, const void* owner, std::uint64_t address, std::size_t bytes, bool written) {
    if (!Enabled()) return false;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    if (!BindingsPermitted(owner)) return false;
    if (written && (!WritesEnabled() || Recorder::Active() == nullptr)) return Refuse(Fallback::Written);
    if (ResidentHeapMirrors()) return Refuse(Fallback::Resident);
    if (!PageTracker::Range{address, bytes}.Valid() || bytes > BlockBytes) return Refuse(Fallback::Size);
    const auto base = address / BlockBytes * BlockBytes;
    const auto first = address / PageBytes * PageBytes;
    if (address + bytes - base > BlockBytes) return Refuse(Fallback::Size);
    const auto end = ((address + bytes - 1) / PageBytes + 1) * PageBytes;
    if (end <= first) return Refuse(Fallback::Size);
    const auto alignment = std::max<VkDeviceSize>(4, context.limits.minStorageBufferOffsetAlignment);
    if (context.limits.minStorageBufferOffsetAlignment == 0 || alignment > PageBytes || BlockBytes % alignment != 0) return Refuse(Fallback::Alignment);
    if (bytes > context.limits.maxStorageBufferRange || alignment > context.limits.maxStorageBufferRange - bytes) return Refuse(Fallback::Size);
    auto lease = GuestAllocations::GuestAllocationsLookup_nid_postfix(first, false);
    if (lease == nullptr || !lease->readable || !lease->gpu || (written && !lease->writable) || first < lease->address || end - lease->address > lease->bytes) return Refuse(Fallback::Mapping);
    const auto existingDevice = Cache().devices.find(context.device);
    const auto existing = existingDevice == Cache().devices.end() ? std::shared_ptr<Block>{} : [&] {
        const auto found = existingDevice->second.blocks.find(base);
        return found == existingDevice->second.blocks.end() ? std::shared_ptr<Block>{} : found->second;
    }();
    if (existing == nullptr || !AnyDirty(*existing)) CompletePendingCpuWrites(context, first, static_cast<std::size_t>(end - first));
    StorageTexture::FlushPending(first, static_cast<std::size_t>(end - first), nullptr, "buffer cache read");
    auto* tracker = PageTracker::Get();
    if (tracker == nullptr) return Refuse(Fallback::Coverage);
    auto& session = Cache().sessions.at(owner);
    if (session.device != VK_NULL_HANDLE && session.device != context.device) return Refuse(Fallback::Mapping);
    auto block = Acquire(context, Cache().devices[context.device], *tracker, {first, end - first}, base, written);
    if (block == nullptr) return false;
    if (written) {
        block->original.resize(BlockBytes);
        GuestMemory::Read(first, std::span(block->original).subspan(first - base, end - first));
        for (auto page = first; page < end; page += PageBytes) if (!Proven(*tracker, *block, static_cast<std::size_t>((page - base) / PageBytes))) return Refuse(Fallback::Changed);
#ifdef _WIN32
        PrepareFaults(block, base, static_cast<std::size_t>((first - base) / PageBytes), static_cast<std::size_t>((end - base) / PageBytes));
#endif
        block->mapping = lease;
        ++block->reservations;
    }
    std::vector<PageTracker::Epoch> proofs;
    for (auto page = first; page < end; page += PageBytes) proofs.push_back(block->pages[static_cast<std::size_t>((page - base) / PageBytes)]);
    session.device = context.device;
    session.bindings.push_back({{address, bytes}, base, std::move(block), alignment, written, false, std::move(proofs)});
    Report();
    return true;
}
}

bool BindReadOnly(const Context& context, const void* owner, std::uint64_t address, std::size_t bytes) { return Bind(context, owner, address, bytes, false); }
bool BindWritable(const Context& context, const void* owner, std::uint64_t address, std::size_t bytes) { return Bind(context, owner, address, bytes, true); }

std::optional<VkDescriptorBufferInfo> Descriptor(const void* owner, std::uint64_t address, std::size_t bytes, std::uint32_t& adjustment) {
    if (!Enabled()) return std::nullopt;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    const auto found = Cache().sessions.find(owner);
    if (found == Cache().sessions.end() || !PageTracker::Range{address, bytes}.Valid()) return std::nullopt;
    for (const auto& binding : found->second.bindings) {
        if (address < binding.range.address || address + bytes > binding.range.address + binding.range.bytes) continue;
        const auto offset = address - binding.base;
        adjustment = static_cast<std::uint32_t>(offset % binding.alignment);
        return VkDescriptorBufferInfo{binding.block->buffer->Handle(), offset - adjustment, GuestBufferMemory::ViewBytes(bytes, adjustment)};
    }
    return std::nullopt;
}

bool HasBindings(const void* owner) {
    if (!Enabled()) return false;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    const auto found = Cache().sessions.find(owner);
    return found != Cache().sessions.end() && !found->second.bindings.empty();
}

bool Contains(const void* owner, std::uint64_t address, std::size_t bytes) {
    std::uint32_t adjustment = 0;
    return Descriptor(owner, address, bytes, adjustment).has_value();
}

bool Reusable(const void* owner) {
    if (!WritesEnabled()) return !HasBindings(owner);
    std::lock_guard gpu(GuestMemory::GpuMutex());
    const auto found = Cache().sessions.find(owner);
    if (found == Cache().sessions.end()) return true;
    auto* tracker = PageTracker::Get();
    if (tracker == nullptr) return false;
    for (const auto& binding : found->second.bindings) {
        if (binding.written) return false;
        for (const auto& proof : binding.proofs) if (tracker->DirtySince(proof.range, proof)) return false;
    }
    return true;
}

void BeginWrites(const void* owner, Recorder& recorder, std::span<const std::pair<std::uint64_t, std::uint64_t>> writes) {
    if (!WritesEnabled()) return;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    const auto found = Cache().sessions.find(owner);
    if (found == Cache().sessions.end()) return;
    for (auto& binding : found->second.bindings) {
        if (!binding.written) continue;
        Require(!binding.begun, "a cached buffer writer cannot be recorded twice");
        binding.begun = true;
        for (const auto& [first, last] : writes) {
            const auto begin = std::max(first, binding.range.address);
            const auto end = std::min(last, binding.range.address + binding.range.bytes);
            if (begin >= end) continue;
            const auto block = binding.block;
            auto mapping = LeaseRegisteredRange(begin, static_cast<std::size_t>(end - begin));
            Require(mapping != nullptr && mapping == block->mapping.lock(), "a cached writer's mapping changed before recording");
            block->writerMapping = std::move(mapping);
            const PageTracker::Range range{begin, end - begin};
            Publish(*block, range, false);
            for (auto page = (begin - binding.base) / PageBytes; page < (end - binding.base + PageBytes - 1) / PageBytes; ++page) block->dirty[page / 64] |= 1ull << (page % 64);
            ++block->pending;
            Cache().counts.gpuWritten += range.bytes;
            recorder.Keep(block);
            recorder.NotePendingWrite(begin, static_cast<std::size_t>(range.bytes));
            recorder.OnComplete([block, range] {
                Publish(*block, range, true);
                --block->pending;
                Cache().counts.completedWrites += range.bytes;
#ifdef _WIN32
                ArmReads(block, range.address / BlockBytes * BlockBytes);
#endif
            });
        }
    }
}

namespace {
void FlushImpl(std::uint64_t address, std::size_t bytes, bool syncPoint) {
    if (!WritesEnabled() || bytes == 0 || Recorder::InCompletion()) return;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    auto& state = Cache();
    if (state.flushing) return;
    state.flushing = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{state.flushing};
    const auto end = address + std::min<std::uint64_t>(bytes, UINT64_MAX - address);
    std::vector<std::pair<std::uint64_t, std::shared_ptr<Block>>> blocks;
    for (const auto& [device, cache] : state.devices) for (const auto& [base, block] : cache.blocks)
        if (address < base + BlockBytes && base < end && AnyDirty(*block)) blocks.emplace_back(base, block);
    for (const auto& [base, block] : blocks) {
        const auto first = std::max(address, base) / PageBytes;
        const auto last = (std::min(end, base + BlockBytes) - 1) / PageBytes + 1;
        std::vector<std::size_t> pages;
        for (auto index = first; index < last; ++index) {
            const auto page = static_cast<std::size_t>(index - base / PageBytes);
            if ((block->dirty[page / 64] & (1ull << (page % 64))) != 0) pages.push_back(page);
        }
        if (pages.empty()) continue;
        if (block->pending != 0) {
            auto* recorder = Recorder::Active();
            Require(recorder != nullptr, "cached buffer copyback has no recorder");
            recorder->SyncThrough(base + pages.front() * PageBytes, (pages.back() - pages.front() + 1) * PageBytes);
            Require(block->pending == 0, "cached buffer copyback precedes its writer completion");
        }
#ifdef _WIN32
        if (ReadFaultsEnabled()) {
            std::erase_if(pages, [&](auto page) {
                if (block->faults == nullptr || block->faults->pages[page] == nullptr || !block->faults->pages[page]->armed) return false;
                if (!syncPoint) Require(GuestArena::GuestArenaResolvePageRead_nid_no_patch(base + page * PageBytes), "cached read fault flush failed");
                return true;
            });
            if (syncPoint) state.counts.syncFallbackPages += pages.size();
            if (pages.empty()) continue;
        }
#endif
        Require(block->writerMapping != nullptr, "cached buffer copyback lost its mapping lease");
        const auto& context = block->context;
        Buffer download(context, pages.size() * PageBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        CommandBatch batch(context, CommandBatch::Mode::Independent);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        for (std::size_t index = 0; index < pages.size(); ++index) CopyBuffer(context, batch.Handle(), block->buffer->Handle(), pages[index] * PageBytes, download.Handle(), index * PageBytes, PageBytes);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        download.Invalidate();
        std::vector<bool> proven;
        for (const auto page : pages) proven.push_back(Proven(*PageTracker::Get(), *block, page));
        for (std::size_t index = 0; index < pages.size(); ++index) {
            const auto page = pages[index];
            const auto guest = base + page * PageBytes;
            const auto result = download.ReadBytes(index * PageBytes, PageBytes);
            GuestMemory::WriteChanged(guest, result, std::span<const std::byte>(block->original).subspan(page * PageBytes, PageBytes));
            Recorder::NoteWrittenBack(guest, PageBytes);
            if (ReadFaultsEnabled()) PageTracker::Get()->ReadbackGpuWrites({guest, PageBytes});
            block->dirty[page / 64] &= ~(1ull << (page % 64));
            state.counts.copiedBack += PageBytes;
        }
        for (std::size_t index = 0; index < pages.size(); ++index) if (proven[index]) {
            const auto page = pages[index];
            block->pages[page] = PageTracker::Get()->CaptureEpoch({base + page * PageBytes, PageBytes});
            block->owned[page / 64] |= 1ull << (page % 64);
        }
        if (!AnyDirty(*block)) block->writerMapping.reset();
    }
}

}

void Flush(std::uint64_t address, std::size_t bytes) { FlushImpl(address, bytes, false); }
void FlushAtSync() { FlushImpl(0, SIZE_MAX, ReadFaultsEnabled()); }

void AddressRanges(const void* owner, std::vector<ShaderRecompiler::BdaAbi::Range>& ranges) {
    if (!WritesEnabled()) return;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    const auto found = Cache().sessions.find(owner);
    if (found == Cache().sessions.end()) return;
    for (const auto& binding : found->second.bindings) {
        const auto address = binding.block->buffer->DeviceAddress();
        if (address != 0) ranges.push_back({binding.range.address, binding.range.address + binding.range.bytes, address + binding.range.address - binding.base, ShaderRecompiler::BdaAbi::Read, 0});
    }
}

bool FlushForMapping(std::uint64_t address, std::size_t bytes) {
    if (!WritesEnabled() || bytes == 0 || Recorder::InCompletion()) return false;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    const auto end = address + std::min<std::uint64_t>(bytes, UINT64_MAX - address);
    std::vector<std::uint64_t> blocks;
    for (const auto& [device, cache] : Cache().devices) for (const auto& [base, block] : cache.blocks) {
        const auto& mapping = block->writerMapping;
        if (mapping != nullptr && mapping->address < end && address < mapping->address + mapping->bytes) blocks.push_back(base);
    }
    for (const auto base : blocks) Flush(base, BlockBytes);
    return !blocks.empty();
}

void Release(const void* owner) {
    if (!Enabled()) return;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    const auto found = Cache().sessions.find(owner);
    if (found == Cache().sessions.end()) return;
    for (const auto& binding : found->second.bindings) if (binding.written) --binding.block->reservations;
    Cache().sessions.erase(found);
}

void Clear(VkDevice device) {
    if (!Enabled()) return;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    if (WritesEnabled()) {
        Flush();
        if (auto* recorder = Recorder::Active()) recorder->Sync();
    }
    std::erase_if(Cache().sessions, [&](const auto& entry) { return entry.second.device == device; });
    Cache().devices.erase(device);
}

void CountFallback(Fallback reason) {
    if (!Enabled()) return;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    Refuse(reason);
}

Statistics Counters() {
    if (!Enabled()) return {};
    std::lock_guard gpu(GuestMemory::GpuMutex());
    auto result = Cache().counts;
    for (auto& [handle, device] : Cache().devices) {
        Retire(device);
        result.pendingUploads += device.uploads.size();
    }
    result.retired = Cache().counts.retired;
    return result;
}

}
