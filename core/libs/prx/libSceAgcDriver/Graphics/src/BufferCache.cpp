#include "prx/libSceAgcDriver/Graphics/include/BufferCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramOptions.hpp"
#include "prx/libSceAgcDriver/Execution/include/PageTracker.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <chrono>
#include <map>

namespace AgcDriver::Graphics::BufferCache {
namespace {

using PageTracker::PageBytes;
constexpr std::size_t BlockPages = BlockBytes / PageBytes;
struct Block {
    std::shared_ptr<Buffer> buffer;
    std::array<PageTracker::Epoch, BlockPages> pages;
    PageTracker::Bitmap valid{};
    std::uint64_t used = 0;
};
struct Binding {
    PageTracker::Range range;
    std::uint64_t base;
    std::shared_ptr<Block> block;
    VkDeviceSize alignment;
};
struct Device {
    std::map<std::uint64_t, std::shared_ptr<Block>> blocks;
    std::vector<std::weak_ptr<Block>> live;
    std::unique_ptr<Buffer> stream;
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
};
State& Cache() { static State state; return state; }

bool Valid(const Block& block, std::size_t page) { return (block.valid[page / 64] & (1ull << (page % 64))) != 0; }
void Validate(Block& block, std::size_t page) { block.valid[page / 64] |= 1ull << (page % 64); }

void Report() {
    auto& state = Cache();
    const auto now = std::chrono::steady_clock::now();
    if (now - state.reported < std::chrono::seconds(10)) return;
    state.reported = now;
    const auto& c = state.counts;
    AgcDriver::ProfilePrint_nid_no_patch("[buffer-cache] cumulative uploaded=%llu copied-back=%llu hits=%llu misses=%llu uploads=%llu ring-wraps=%llu fallbacks coverage=%llu size=%llu alias=%llu written=%llu addressable=%llu mapping=%llu alignment=%llu budget=%llu allocation=%llu changed=%llu resident=%llu\n",
        static_cast<unsigned long long>(c.uploaded), static_cast<unsigned long long>(c.copiedBack), static_cast<unsigned long long>(c.hits), static_cast<unsigned long long>(c.misses), static_cast<unsigned long long>(c.uploads), static_cast<unsigned long long>(c.wraps),
        static_cast<unsigned long long>(c.fallbacks[0]), static_cast<unsigned long long>(c.fallbacks[1]), static_cast<unsigned long long>(c.fallbacks[2]), static_cast<unsigned long long>(c.fallbacks[3]), static_cast<unsigned long long>(c.fallbacks[4]), static_cast<unsigned long long>(c.fallbacks[5]), static_cast<unsigned long long>(c.fallbacks[6]), static_cast<unsigned long long>(c.fallbacks[7]), static_cast<unsigned long long>(c.fallbacks[8]), static_cast<unsigned long long>(c.fallbacks[9]), static_cast<unsigned long long>(c.fallbacks[10]));
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
            if (it->second.use_count() != 1) continue;
            if (oldest == device.blocks.end() || it->second->used < oldest->second->used) oldest = it;
        }
        if (oldest == device.blocks.end()) return false;
        device.blocks.erase(oldest);
    }
    return true;
}

std::shared_ptr<Block> Acquire(const Context& context, Device& device, PageTracker::Tracker& tracker, PageTracker::Range range, std::uint64_t base) {
    const auto first = static_cast<std::size_t>((range.address - base) / PageBytes);
    const auto last = static_cast<std::size_t>((range.address - base + range.bytes) / PageBytes);
    const auto found = device.blocks.find(base);
    auto previous = found != device.blocks.end() ? found->second : nullptr;
    std::array<PageTracker::Epoch, BlockPages> proofs;
    std::vector<std::size_t> dirty;
    for (auto page = first; page < last; ++page) {
        const PageTracker::Range part{base + page * PageBytes, PageBytes};
        proofs[page] = tracker.CaptureEpoch(part);
        if (!proofs[page].covered) { Refuse(Fallback::Coverage); return {}; }
        if (previous == nullptr || !Valid(*previous, page) || tracker.DirtySince(part, previous->pages[page])) dirty.push_back(page);
    }
    if (dirty.empty()) {
        ++Cache().counts.hits;
        previous->used = ++device.uses;
        return previous;
    }
    ++Cache().counts.misses;
    if (!MakeRoom(device)) { Refuse(Fallback::Budget); return {}; }
    auto next = std::make_shared<Block>();
    try {
        next->buffer = std::make_shared<Buffer>(context, BlockBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Access::DeviceMirror);
        if (device.stream == nullptr) device.stream = std::make_unique<Buffer>(context, BlockBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Access::UploadOnly);
    } catch (const std::runtime_error&) { Refuse(Fallback::Allocation); return {}; }
    const auto uploadBytes = dirty.size() * PageBytes;
    if (device.cursor + uploadBytes > BlockBytes) { device.cursor = 0; ++Cache().counts.wraps; }
    std::vector<VkBufferCopy> uploads, retained;
    auto cursor = device.cursor;
    GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::BufferUpload);
    for (const auto page : dirty) {
        GuestMemory::Read(base + page * PageBytes, device.stream->WriteBytes().subspan(cursor, PageBytes));
        uploads.push_back({cursor, page * PageBytes, PageBytes});
        cursor += PageBytes;
    }
    for (auto page = first; page < last; ++page) {
        if (tracker.DirtySince({base + page * PageBytes, PageBytes}, proofs[page])) { Refuse(Fallback::Changed); return {}; }
    }
    if (previous != nullptr) for (std::size_t page = 0; page < BlockPages; ++page) {
        const PageTracker::Range part{base + page * PageBytes, PageBytes};
        if (!Valid(*previous, page) || tracker.DirtySince(part, previous->pages[page])) continue;
        retained.push_back({page * PageBytes, page * PageBytes, PageBytes});
        next->pages[page] = previous->pages[page];
        Validate(*next, page);
    }
    CommandBatch batch(context, CommandBatch::Mode::Independent);
    RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    if (!retained.empty()) CopyBuffers(context, batch.Handle(), previous->buffer->Handle(), next->buffer->Handle(), static_cast<std::uint32_t>(retained.size()), retained.data());
    CopyBuffers(context, batch.Handle(), device.stream->Handle(), next->buffer->Handle(), static_cast<std::uint32_t>(uploads.size()), uploads.data());
    RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    batch.SubmitAndWait();
    device.cursor = cursor;
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

bool BindReadOnly(const Context& context, const void* owner, std::uint64_t address, std::size_t bytes) {
    if (!Enabled()) return false;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    if (!BindingsPermitted(owner)) return false;
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
    if (lease == nullptr || !lease->readable || !lease->gpu || first < lease->address || end - lease->address > lease->bytes) return Refuse(Fallback::Mapping);
    CompletePendingCpuWrites(context, first, static_cast<std::size_t>(end - first));
    StorageTexture::FlushPending(first, static_cast<std::size_t>(end - first), nullptr, "buffer cache read");
    auto* tracker = PageTracker::Get();
    if (tracker == nullptr) return Refuse(Fallback::Coverage);
    auto& session = Cache().sessions.at(owner);
    if (session.device != VK_NULL_HANDLE && session.device != context.device) return Refuse(Fallback::Mapping);
    auto block = Acquire(context, Cache().devices[context.device], *tracker, {first, end - first}, base);
    if (block == nullptr) return false;
    session.device = context.device;
    session.bindings.push_back({{address, bytes}, base, std::move(block), alignment});
    Report();
    return true;
}

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

void Release(const void* owner) {
    if (!Enabled()) return;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    Cache().sessions.erase(owner);
}

void Clear(VkDevice device) {
    if (!Enabled()) return;
    std::lock_guard gpu(GuestMemory::GpuMutex());
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
    return Cache().counts;
}

}
