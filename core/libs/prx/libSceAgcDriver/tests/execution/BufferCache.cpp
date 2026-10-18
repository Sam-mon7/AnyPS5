#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramOptions.hpp"
#include "prx/libSceAgcDriver/Execution/include/PageTracker.hpp"
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

namespace {
using namespace AgcDriver;
using namespace AgcDriver::Graphics;
constexpr auto PageBytes = PageTracker::PageBytes;

struct Memory {
    void* allocation = nullptr;
    std::uint64_t address = 0;
    Memory() {
#ifdef _WIN32
        allocation = VirtualAlloc(nullptr, 3 * BufferCache::BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        allocation = std::aligned_alloc(BufferCache::BlockBytes, 3 * BufferCache::BlockBytes);
#endif
        Require(allocation != nullptr, "buffer cache test allocation failed");
        const auto raw = reinterpret_cast<std::uintptr_t>(allocation);
        address = (raw + BufferCache::BlockBytes - 1) / BufferCache::BlockBytes * BufferCache::BlockBytes;
        std::memset(reinterpret_cast<void*>(address), 0x35, 2 * BufferCache::BlockBytes);
        GuestAllocations::Mutation().Add(allocation, 3 * BufferCache::BlockBytes, true, true, true);
    }
    ~Memory() {
        GuestAllocations::Mutation().Remove(allocation);
#ifdef _WIN32
        VirtualFree(allocation, 0, MEM_RELEASE);
#else
        std::free(allocation);
#endif
    }
    void Track() const {
        if (auto* tracker = PageTracker::Get()) tracker->RegisterRange({address, 2 * BufferCache::BlockBytes}, PageTracker::Kind::Section, PageTracker::Bit(PageTracker::Reason::ProtectedSection));
    }
};

VkDescriptorBufferInfo Upload(GuestBufferMemory& buffers, std::uint64_t address, std::size_t bytes) {
    BufferCache::PermitBindings(&buffers, true);
    buffers.AddReadable(address, bytes);
    buffers.Upload(false);
    std::uint32_t adjustment = 0;
    const auto view = buffers.Descriptor(address, bytes, adjustment);
    Require(adjustment == 0, "aligned cache view was adjusted");
    return view;
}

std::vector<std::byte> ReadGpu(const Context& context, VkDescriptorBufferInfo view, std::size_t bytes) {
    Buffer result(context, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    CommandBatch batch(context, CommandBatch::Mode::Independent);
    RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    CopyBuffer(context, batch.Handle(), view.buffer, view.offset, result.Handle(), 0, bytes);
    RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    batch.SubmitAndWait();
    result.Invalidate();
    const auto data = result.ReadBytes(0, bytes);
    return {data.begin(), data.end()};
}

void ReadOnly(const Context& context, const Memory& memory) {
    memory.Track();
    const auto initial = BufferCache::Counters();
    GuestBufferMemory first(context);
    const auto firstView = Upload(first, memory.address, PageBytes);
    Require(BufferCache::HasBindings(&first) == BufferCache::Enabled(), "cache enablement differs from binding path");
    const auto copied = ReadGpu(context, firstView, PageBytes);
    Require(std::memcmp(copied.data(), reinterpret_cast<void*>(memory.address), PageBytes) == 0, "GPU read differs from CPU upload");
    const auto uploaded = BufferCache::Counters().uploaded;
    GuestBufferMemory second(context);
    const auto secondView = Upload(second, memory.address, PageBytes);
    Require(BufferCache::Counters().uploaded == uploaded, "unchanged pages uploaded twice");
    if (BufferCache::Enabled()) {
        Require(firstView.buffer == secondView.buffer && BufferCache::Counters().hits > initial.hits, "unchanged page missed the merged block");
        Require(uploaded - initial.uploaded == PageBytes, "initial upload copied unrequested pages");
    }
    GuestBufferMemory neighbor(context);
    const auto neighborView = Upload(neighbor, memory.address + PageBytes, PageBytes);
    Require(ReadGpu(context, neighborView, PageBytes) == copied, "neighbor page upload failed");
    GuestBufferMemory retained(context);
    const auto retainedView = Upload(retained, memory.address, PageBytes);
    if (BufferCache::Enabled()) Require(retainedView.buffer == neighborView.buffer && retainedView.offset + PageBytes == neighborView.offset, "adjacent ranges did not merge or clean pages were lost");
    std::memset(reinterpret_cast<void*>(memory.address), 0x79, PageBytes);
    PageTracker::CpuWrite({memory.address, PageBytes});
    GuestBufferMemory changed(context);
    const auto changedView = Upload(changed, memory.address, PageBytes);
    const auto changedBytes = ReadGpu(context, changedView, PageBytes);
    Require(changedBytes.front() == std::byte{0x79} && changedBytes.back() == std::byte{0x79}, "CPU write did not reach GPU read");
    Require(ReadGpu(context, firstView, PageBytes) == copied, "refresh overwrote an earlier in-flight snapshot");
    if (BufferCache::Enabled()) Require(BufferCache::Counters().uploaded == uploaded + 2 * PageBytes, "dirty refresh uploaded unrelated pages");
    GuestBufferMemory unaligned(context);
    BufferCache::PermitBindings(&unaligned, true);
    unaligned.AddReadable(memory.address + 3, PageBytes - 7);
    unaligned.Upload(false);
    std::uint32_t adjustment = 0;
    const auto unalignedView = unaligned.Descriptor(memory.address + 3, PageBytes - 7, adjustment);
    const auto unalignedBytes = ReadGpu(context, unalignedView, unalignedView.range);
    Require(std::memcmp(unalignedBytes.data() + adjustment, reinterpret_cast<void*>(memory.address + 3), PageBytes - 7) == 0, "adjusted descriptor read differs");
}

void Fallbacks(const Context& context, const Memory& memory) {
    memory.Track();
    if (auto* tracker = PageTracker::Get()) tracker->SetCoverage({memory.address, PageBytes}, PageTracker::Bit(PageTracker::Reason::HostAlias));
    const auto before = BufferCache::Counters();
    {
        GuestBufferMemory unknown(context);
        const auto view = Upload(unknown, memory.address, PageBytes);
        Require(!BufferCache::HasBindings(&unknown), "unknown alias coverage entered cache");
        Require(ReadGpu(context, view, PageBytes).front() == std::byte{0x79}, "coverage fallback lost CPU data");
    }
    memory.Track();
    {
        GuestBufferMemory large(context);
        Upload(large, memory.address, BufferCache::BlockBytes + PageBytes);
        Require(!BufferCache::HasBindings(&large), "oversized range entered cache");
    }
    {
        GuestBufferMemory crossing(context);
        Upload(crossing, memory.address + BufferCache::BlockBytes - PageBytes, 2 * PageBytes);
        Require(!BufferCache::HasBindings(&crossing), "cross-block range entered cache");
    }
    {
        GuestBufferMemory written(context);
        BufferCache::PermitBindings(&written, true);
        written.AddWritable(memory.address, PageBytes);
        written.AddReadable(memory.address, PageBytes);
        written.Upload(false);
        Require(!BufferCache::HasBindings(&written), "descriptor alias with GPU writer entered cache");
        std::uint32_t adjustment = 0;
        const auto view = written.Descriptor(memory.address, PageBytes, adjustment);
        CommandBatch batch(context, CommandBatch::Mode::Independent);
        written.BeginGpuWrites(batch);
        context.Resolved(&DeviceFunctions::cmdFillBuffer, "vkCmdFillBuffer")(batch.Handle(), view.buffer, view.offset, PageBytes, 0xababababu);
        written.SealForSubmit(&batch);
        batch.SubmitAndWait();
        written.WriteBack();
        const auto* data = reinterpret_cast<const std::uint8_t*>(memory.address);
        Require(data[0] == 0xab && data[PageBytes - 1] == 0xab, "GPU write was not visible after sync fallback");
    }
    if (BufferCache::Enabled()) {
        const auto after = BufferCache::Counters();
        Require(after.fallbacks[static_cast<std::size_t>(BufferCache::Fallback::Coverage)] > before.fallbacks[static_cast<std::size_t>(BufferCache::Fallback::Coverage)], "coverage fallback was not counted");
        Require(after.fallbacks[static_cast<std::size_t>(BufferCache::Fallback::Size)] >= before.fallbacks[static_cast<std::size_t>(BufferCache::Fallback::Size)] + 2, "size fallbacks were not counted");
        Require(after.fallbacks[static_cast<std::size_t>(BufferCache::Fallback::Written)] > before.fallbacks[static_cast<std::size_t>(BufferCache::Fallback::Written)], "write fallback was not counted");
    }
}

void RingAndBudget(const Context& context, const Memory& memory) {
    BufferCache::Clear(context.device);
    memory.Track();
    {
        GuestBufferMemory first(context), second(context);
        Upload(first, memory.address, BufferCache::BlockBytes);
        const auto wrapped = Upload(second, memory.address + BufferCache::BlockBytes, BufferCache::BlockBytes);
        const auto bytes = ReadGpu(context, wrapped, BufferCache::BlockBytes);
        Require(std::memcmp(bytes.data(), reinterpret_cast<void*>(memory.address + BufferCache::BlockBytes), bytes.size()) == 0, "wrapped staging ring overwrote upload data");
        if (BufferCache::Enabled()) Require(BufferCache::Counters().wraps != 0, "staging ring did not wrap");
    }
    BufferCache::Clear(context.device);
    memory.Track();
    const auto before = BufferCache::Counters();
    {
        std::vector<std::unique_ptr<GuestBufferMemory>> held;
        VkDescriptorBufferInfo first{};
        for (std::size_t index = 0; index <= BufferCache::BudgetBytes / BufferCache::BlockBytes; ++index) {
            std::memset(reinterpret_cast<void*>(memory.address), static_cast<int>(index + 1), PageBytes);
            PageTracker::CpuWrite({memory.address, PageBytes});
            auto buffers = std::make_unique<GuestBufferMemory>(context);
            const auto view = Upload(*buffers, memory.address, PageBytes);
            if (index == 0) first = view;
            held.push_back(std::move(buffers));
        }
        Require(ReadGpu(context, first, PageBytes).front() == std::byte{1}, "budget pressure recycled a live snapshot");
        if (BufferCache::Enabled()) Require(BufferCache::Counters().fallbacks[static_cast<std::size_t>(BufferCache::Fallback::Budget)] > before.fallbacks[static_cast<std::size_t>(BufferCache::Fallback::Budget)], "live block budget did not fall back");
    }
    BufferCache::Clear(context.device);
}
}

int main() {
    try {
        const bool expected = VramOption("APS5_BUFFER_CACHE") && PageTracker::Enabled();
        Require(BufferCache::Enabled() == expected, "cache switch does not require both exact values");
        std::lock_guard gpu(GuestMemory::GpuMutex());
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        auto context = VulkanDeviceTestAccess::ContextOf(*device);
        context.hostImportAlignment = 0;
        {
            Memory memory;
            ReadOnly(context, memory);
            Fallbacks(context, memory);
            RingAndBudget(context, memory);
        }
        BufferCache::Clear(context.device);
        if (!expected) {
            const auto counts = BufferCache::Counters();
            Require(counts.uploaded == 0 && counts.copiedBack == 0 && counts.hits == 0 && counts.misses == 0, "disabled cache changed counters");
        }
        std::puts("buffer cache: ok");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "buffer cache: %s\n", error.what());
        return 1;
    }
}
