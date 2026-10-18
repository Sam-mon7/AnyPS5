#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramOptions.hpp"
#include "prx/libSceAgcDriver/Execution/include/PageTracker.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
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

void Writable(const Context& context, const Memory& memory, bool cpuRead) {
    BufferCache::Clear(context.device);
    std::memset(reinterpret_cast<void*>(memory.address), 0x35, 5 * PageBytes);
    const auto label = memory.address + 4 * PageBytes;
    *reinterpret_cast<std::uint32_t*>(label) = 0;
    memory.Track();
    Recorder recorder(context);
    recorder.Activate();
    const auto before = BufferCache::Counters();
    const auto range = PageTracker::Range{memory.address + PageBytes, PageBytes};
    const auto proof = PageTracker::Get() != nullptr ? PageTracker::Get()->CaptureEpoch(range) : PageTracker::Epoch{};
    auto writer = std::make_shared<GuestBufferMemory>(context);
    BufferCache::PermitBindings(writer.get(), true);
    writer->AddReadable(memory.address, 3 * PageBytes);
    writer->AddWritable(range.address, PageBytes);
    writer->Upload(false);
    Require(BufferCache::HasBindings(writer.get()) == BufferCache::WritesEnabled(), "writable binding ignored the write switch");
    std::uint32_t adjustment = 0;
    const auto view = writer->Descriptor(range.address, PageBytes, adjustment);
    writer->BeginGpuWrites(recorder);
    if (PageTracker::Get() != nullptr) Require(PageTracker::Get()->DirtySince(range, proof), "recording a GPU write did not invalidate its tracker proof");
    context.Resolved(&DeviceFunctions::cmdFillBuffer, "vkCmdFillBuffer")(recorder.Commands(), view.buffer, view.offset, cpuRead ? 4 : PageBytes, 0xababababu);
    writer->RecordCopyBacks(recorder);
    writer->NotePendingWrites(recorder);
    writer->MarkDirectWrites();
    if (writer->HasCopiedWrites()) recorder.OnComplete([writer] { writer->WriteBack(); });
    recorder.Keep(writer);
    if (BufferCache::WritesEnabled()) {
        std::array<std::byte, 4> clean{};
        GuestMemory::Read(memory.address, clean);
        Require(clean.front() == std::byte{0x35} && recorder.Submissions() == 0, "clean-page CPU read waited for an unrelated cached writer");
    }
    recorder.Submit();
    recorder.Sync();
    const auto completed = BufferCache::Counters();
    if (BufferCache::WritesEnabled()) {
        Require(completed.completedWrites - before.completedWrites == PageBytes, "batch completion did not publish the cached writer");
        Require(completed.copiedBack == before.copiedBack, "batch completion eagerly downloaded GPU writes");
        Require(*reinterpret_cast<const std::uint8_t*>(range.address) == 0x35, "GPU bytes reached guest memory without a sync");
        GuestBufferMemory consumer(context);
        const auto cached = Upload(consumer, memory.address, 3 * PageBytes);
        Require(cached.buffer == view.buffer, "a GPU consumer did not retain the cached writer's block");
        Require(BufferCache::Counters().copiedBack == before.copiedBack, "cached GPU consumption caused a readback");
        if (context.bufferDeviceAddress) {
            const auto ranges = consumer.AddressRanges();
            const VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, cached.buffer};
            const auto address = context.Function<PFN_vkGetBufferDeviceAddressKHR>("vkGetBufferDeviceAddressKHR")(context.device, &info);
            Require(ranges.size() == 1 && ranges.front().begin == memory.address && ranges.front().end == memory.address + 3 * PageBytes, "BDA ranges did not cover the whole cached range");
            Require(address != 0 && ranges.front().deviceAddress == address + cached.offset && ranges.front().deviceAddress + ranges.front().end - ranges.front().begin <= address + BufferCache::BlockBytes, "BDA address escaped the merged cached block");
        }
    }
    writer.reset();
    if (cpuRead) {
        *reinterpret_cast<std::uint8_t*>(range.address + 128) = 0x92;
        PageTracker::CpuWrite({range.address + 128, 1});
        std::array<std::byte, 4> data{};
        GuestMemory::Read(range.address, data);
        Require(data.front() == std::byte{0xab}, "driver CPU read did not download the dirty page");
        Require(*reinterpret_cast<const std::uint8_t*>(range.address + 128) == 0x92, "copyback rolled back an untouched CPU byte");
    } else {
        const std::array<std::uint32_t, 5> signal{0xc0033700u, 0x00100200u, static_cast<std::uint32_t>(label), static_cast<std::uint32_t>(label >> 32u), 0x76543210u};
        Require(DriverDetail::TryParseTimeLabelPacket(signal), "idle label did not use the synchronization hook");
        const std::array<std::uint32_t, 7> wait{0xc0053c00u, 0x13u, static_cast<std::uint32_t>(label), static_cast<std::uint32_t>(label >> 32u), 0x76543210u, 0xffffffffu, 0x19u};
        Require(Pm4::WaitSatisfiedUnchecked(wait), "guest wait did not observe the published label");
        Require(*reinterpret_cast<const std::uint8_t*>(range.address) == 0xab && *reinterpret_cast<const std::uint8_t*>(range.address + PageBytes - 1) == 0xab, "CPU read after guest wait missed the GPU write");
    }
    Require(*reinterpret_cast<const std::uint8_t*>(memory.address) == 0x35 && *reinterpret_cast<const std::uint8_t*>(memory.address + 2 * PageBytes) == 0x35, "copyback changed a clean neighboring page");
    if (BufferCache::WritesEnabled()) Require(BufferCache::Counters().copiedBack - before.copiedBack == PageBytes, "copyback downloaded pages outside the GPU-dirty set");
    BufferCache::Clear(context.device);
}

void AsyncRing(const Context& context, const Memory& memory) {
    BufferCache::Clear(context.device);
    memory.Track();
    Recorder recorder(context);
    recorder.Activate();
    const auto before = BufferCache::Counters();
    {
        GuestBufferMemory first(context), second(context);
        const auto firstView = Upload(first, memory.address, BufferCache::BlockBytes);
        const auto secondView = Upload(second, memory.address + BufferCache::BlockBytes, BufferCache::BlockBytes);
        const auto queued = BufferCache::Counters();
        if (BufferCache::WritesEnabled()) {
            Require(recorder.Submissions() == 0 && queued.pendingUploads == 1, "upload ring synchronously retired an unsubmitted copy");
            Require(queued.spills == before.spills + 1, "busy ring range was overwritten instead of spilled");
        }
        recorder.Sync();
        const auto retired = BufferCache::Counters();
        if (BufferCache::WritesEnabled()) Require(retired.pendingUploads == 0 && retired.retired > before.retired, "completed upload range was not retired");
        Require(ReadGpu(context, firstView, PageBytes).front() == std::byte{0x35}, "ring wrap overwrote an earlier upload");
        Require(ReadGpu(context, secondView, PageBytes).front() == std::byte{0x35}, "ring spill lost upload bytes");
        std::memset(reinterpret_cast<void*>(memory.address), 0x74, PageBytes);
        PageTracker::CpuWrite({memory.address, PageBytes});
        GuestBufferMemory changed(context);
        const auto changedView = Upload(changed, memory.address, PageBytes);
        recorder.Sync();
        if (BufferCache::WritesEnabled()) Require(BufferCache::Counters().spills == retired.spills, "retired ring bytes were not reusable");
        Require(ReadGpu(context, changedView, PageBytes).front() == std::byte{0x74}, "retired ring did not upload the new bytes");
    }
    BufferCache::Clear(context.device);
}

void Reuse(const Context& context, const Memory& memory) {
    BufferCache::Clear(context.device);
    memory.Track();
    GuestBufferMemory buffers(context);
    Upload(buffers, memory.address, PageBytes);
    const bool cached = BufferCache::HasBindings(&buffers);
    Require(BufferCache::Reusable(&buffers) == (BufferCache::WritesEnabled() || !cached), "covered descriptor reuse ignored its switch");
    PageTracker::CpuWrite({memory.address, PageBytes});
    if (cached) Require(!BufferCache::Reusable(&buffers), "CPU epoch change accepted an old cached descriptor");
    BufferCache::Clear(context.device);
}

void MappingRetirement(const Context& context, const Memory& memory) {
    if (!BufferCache::WritesEnabled()) return;
    BufferCache::Clear(context.device);
    memory.Track();
    Recorder recorder(context);
    recorder.Activate();
    const auto before = BufferCache::Counters().copiedBack;
    auto writer = std::make_shared<GuestBufferMemory>(context);
    BufferCache::PermitBindings(writer.get(), true);
    writer->AddWritable(memory.address, PageBytes);
    writer->Upload(false);
    Require(BufferCache::HasBindings(writer.get()), "mapping retirement fixture missed the writable cache");
    std::uint32_t adjustment = 0;
    const auto view = writer->Descriptor(memory.address, PageBytes, adjustment);
    writer->BeginGpuWrites(recorder);
    context.Resolved(&DeviceFunctions::cmdFillBuffer, "vkCmdFillBuffer")(recorder.Commands(), view.buffer, view.offset, PageBytes, 0x63636363u);
    recorder.Keep(writer);
    writer.reset();
    GuestAllocations::Mutation().RequireUnpinned(reinterpret_cast<void*>(memory.address + 2 * PageBytes), PageBytes);
    Require(BufferCache::Counters().copiedBack == before + PageBytes && recorder.Idle(), "mapping waiter did not release the cached writer's allocation lease");
    Require(*reinterpret_cast<const std::uint8_t*>(memory.address) == 0x63, "mapping waiter lost the cached GPU bytes");
    BufferCache::Clear(context.device);
}
}

int main(int argc, char** argv) {
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
            if (argc > 1) {
                const bool expectedWrites = VramOption("APS5_BUFFER_CACHE_WRITES") && expected && !ResidentHeapMirrors() && !VramMirrorWrites();
                Require(BufferCache::WritesEnabled() == expectedWrites, "write switch ignored a prerequisite or a conflicting mirror mode");
                Writable(context, memory, std::strcmp(argv[1], "cpu-read") == 0);
                AsyncRing(context, memory);
                Reuse(context, memory);
                MappingRetirement(context, memory);
            } else {
                ReadOnly(context, memory);
                Fallbacks(context, memory);
                RingAndBudget(context, memory);
            }
        }
        BufferCache::Clear(context.device);
        if (argc > 1 && !BufferCache::WritesEnabled()) {
            const auto counts = BufferCache::Counters();
            Require(counts.retired == 0 && counts.spills == 0 && counts.pendingUploads == 0 && counts.gpuWritten == 0 && counts.completedWrites == 0, "disabled write extension changed counters");
        }
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
