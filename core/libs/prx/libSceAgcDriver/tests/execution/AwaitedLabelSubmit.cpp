#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/LabelWake.hpp"
#include <atomic>
#include <chrono>
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>

namespace AgcDriver::DriverDetail {

struct DriverTestAccess {
    static void Device(std::shared_ptr<VulkanDevice> device) { Driver::Get().device = std::move(device); }
    static std::uint64_t Stamp() { return ++Driver::Get().eventSerial; }
    static void History(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue) {
        Driver::Get().noteLabelStore(address, bytes, stamp, queue);
    }
    static void Wait(std::span<const std::uint32_t> packet, std::uint32_t queue, std::uint64_t received) {
        Driver::Get().waitMemory(packet, queue, PacketHistory{}, received, false);
    }
    static void Await(std::uint32_t queue, std::uint64_t address, std::uint64_t bytes) {
        auto& slot = Driver::Get().awaitedSlots[queue == 0 ? 0 : queue - 0x20 + 1];
        slot.shape.store(bytes << 32u, std::memory_order_relaxed);
        slot.address.store(address, std::memory_order_release);
    }
    static bool Label(std::span<const std::uint32_t> packet, std::uint32_t queue) {
        Submission submission{};
        submission.queue = queue;
        QueueState state{};
        bool wrote = false, interrupt = false, deferred = false, draw = false, sample = false;
        const auto opcode = (packet[0] >> 8u) & 0xffu;
        Driver::Get().preparePacketMemory(submission, state, packet, packet[0], opcode, Driver::releaseInterrupt(packet, opcode, false), wrote, interrupt, deferred, draw, sample);
        return wrote;
    }
    static bool Deferred() { return !deferredLabels().labels.empty(); }
    static std::size_t DeferredCount() { return deferredLabels().labels.size(); }
    static void Age(std::chrono::hours age) { deferredLabels().since = std::chrono::steady_clock::now() - age; }
    static void Flush(std::uint32_t header, std::uint32_t queue) { Driver::Get().flushBetweenPackets(queue, header, false); }
    static void PacketRecord(VulkanDevice& device, std::uint32_t queue) { Driver::Get().recordLabelsForPacket(&device, queue); }
    static void Record(VulkanDevice& device, std::uint32_t queue) { Driver::Get().recordDeferredLabels(&device, queue); }
    static std::pair<std::uint64_t, std::uint64_t> FlushCounts() {
        const auto& outcomes = Driver::waitOutcomes();
        return {outcomes.awaitedLabelFlushes, outcomes.awaitedLabelBusy};
    }
};

}

namespace {

using AgcDriver::Graphics::Require;
using Recorder = AgcDriver::Graphics::Recorder;
using Access = AgcDriver::DriverDetail::DriverTestAccess;
constexpr std::size_t BlockBytes = 65536;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        block = std::aligned_alloc(BlockBytes, BlockBytes);
#endif
        Require(block != nullptr, "cannot allocate label memory");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }
    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(block); }
private:
    void* block = nullptr;
};

bool Run(AgcDriver::VulkanDevice& device, std::uint64_t address, std::uint32_t waiter, bool history, bool wide, bool atEntry, bool contended = false) {
    constexpr std::uint32_t writer = 0x20;
    const std::uint64_t value = atEntry ? 0 : wide ? 0x1234567800000001ull : 1;
    const auto bytes = std::as_bytes(std::span(&value, 1)).first(wide ? 8 : 4);
    const auto stamp = Access::Stamp();
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::GuestMemory::TagGpuLockThread(writer);
        AgcDriver::GuestMemory::SetCurrentPacket(0x49, writer);
        const std::array<std::uint32_t, 4> pattern{1, 2, 3, 4};
        if (!device.FillBuffer(address + 4096, 4096, pattern)) return false;
        Require(device.WriteLabelOnGpu(address, bytes, stamp, writer, false) == 0, "label was not recorded on the GPU");
        if (history) Access::History(address, bytes, stamp, writer);
        Require(device.OpenWriteOverlaps(address, bytes.size()), "label batch was submitted before the wait");
        if (std::getenv("APS5_PROFILE_DRAW")) {
            const auto entry = Recorder::ProfileWriter(address, bytes.size());
            Require(entry.writer && entry.writer->queue == writer && entry.writer->opcode == 0x49 && entry.stage == Recorder::WaitStage::Recorded, "recorded writer attribution is wrong");
        }
    }
    const std::array<std::uint32_t, 9> packet{wide ? 0xc0079300u : 0xc0053c00u, 0x13u,
        static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u),
        static_cast<std::uint32_t>(value), wide ? static_cast<std::uint32_t>(value >> 32u) : 0xffffffffu,
        wide ? 0xffffffffu : 0x19u, 0xffffffffu, 0x19u};
    std::promise<void> held;
    std::jthread holder;
    if (contended) {
        auto ready = held.get_future();
        holder = std::jthread([&](std::stop_token stop) {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            held.set_value();
            while (!stop.stop_requested()) std::this_thread::yield();
        });
        ready.wait();
    }
    Access::Wait(std::span(packet).first(wide ? 9 : 7), waiter, stamp - 1);
    if (holder.joinable()) { holder.request_stop(); holder.join(); }
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        const bool submitted = !atEntry && !contended && waiter != writer && !std::getenv("APS5_NO_WAIT_LABEL_SUBMIT");
        Require(device.OpenWriteOverlaps(address, bytes.size()) != submitted, "awaited-label submission did not respect queue ownership or the opt-out");
        if (std::getenv("APS5_PROFILE_DRAW") && submitted) {
            const auto seen = Recorder::ProfileWriter(address, bytes.size());
            Require(seen.stage == Recorder::WaitStage::Submitted || seen.stage == Recorder::WaitStage::GpuComplete, "submitted writer attribution is wrong");
        }
        device.WaitIdle();
        std::uint64_t actual = 0;
        std::memcpy(&actual, reinterpret_cast<const void*>(address), bytes.size());
        Require(actual == value, "the submitted label has the wrong value");
        if (std::getenv("APS5_PROFILE_DRAW")) Require(Recorder::ProfileWriter(address, bytes.size()).stage == Recorder::WaitStage::Retired, "completed writer was lost when its fence was recycled");
    }
    return true;
}

bool RunProducer(AgcDriver::VulkanDevice& device, std::uint64_t address, std::uint32_t writer, std::uint32_t waiter,
                 std::int32_t waitOffset = 0, std::uint64_t waitBytes = 4, bool wide = false, bool contended = false) {
    constexpr std::uint64_t value = 0x1234567800000001ull;
    const std::array<std::uint32_t, 4> pattern{1, 2, 3, 4};
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::GuestMemory::TagGpuLockThread(writer);
        AgcDriver::GuestMemory::SetCurrentPacket(0x37, writer);
        if (!device.FillBuffer(address + 16384, 4096, pattern)) return false;
    }
    const auto before = device.SubmittedEpoch();
    const auto countsBefore = Access::FlushCounts();
    const std::array<std::uint32_t, 5> earlier{0xc0033700u, 0x00100200u,
        static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 2};
    Require(Access::Label(earlier, writer) && Access::Deferred(), "an unawaited store did not remain deferred");
    Access::Await(waiter, waitOffset < 0 ? address - static_cast<std::uint64_t>(-waitOffset) : address + waitOffset, waitBytes);
    struct ClearWait {
        std::uint32_t queue;
        ~ClearWait() { Access::Await(queue, 0, 0); }
    } clearWait{waiter};
    std::promise<void> held;
    std::jthread holder;
    if (contended) {
        auto ready = held.get_future();
        holder = std::jthread([&](std::stop_token stop) {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            held.set_value();
            while (!stop.stop_requested()) std::this_thread::yield();
        });
        ready.wait();
    }
    const std::array<std::uint32_t, 6> packet{wide ? 0xc0043700u : 0xc0033700u, 0x00100200u,
        static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u),
        static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32u)};
    Require(Access::Label(std::span(packet).first(wide ? 6 : 5), writer), "producer did not handle WRITE_DATA");
    if (holder.joinable()) { holder.request_stop(); holder.join(); }
    const bool overlaps = waitBytes != 0 && (waitOffset < 0 ? static_cast<std::uint64_t>(-waitOffset) < waitBytes : waitOffset < (wide ? 8 : 4));
    const bool flushed = overlaps && (waitBytes == 4 || waitBytes == 8) && writer != waiter && !contended && !std::getenv("APS5_NO_AWAITED_LABEL_FLUSH");
    Require(Access::Deferred() != flushed, "producer did not respect the active wait range, queue ownership or opt-out");
    const auto countsAfter = Access::FlushCounts();
    const bool profiled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    Require(countsAfter.first - countsBefore.first == (profiled && flushed ? 1u : 0u), "producer flush profiling count is wrong");
    Require(countsAfter.second - countsBefore.second == (profiled && contended && !std::getenv("APS5_NO_AWAITED_LABEL_FLUSH") ? 1u : 0u), "producer busy profiling count is wrong");
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        Require((device.SubmittedEpoch() > before) == flushed, "producer did not submit exactly when an active cross-queue wait overlapped");
        Access::Record(device, writer);
        device.SubmitRecorded(false);
        device.WaitIdle();
        std::uint64_t actual = 0;
        std::memcpy(&actual, reinterpret_cast<const void*>(address), wide ? 8 : 4);
        Require(actual == (wide ? value : static_cast<std::uint32_t>(value)), "producer label has the wrong value");
        Require(std::memcmp(reinterpret_cast<const void*>(address + 16384), pattern.data(), sizeof(pattern)) == 0, "work before the producer label did not complete");
    }
    return true;
}

// Arch C labels, C_LABEL_AWAITED_EARLY: a WRITE_DATA label queued on a compute queue before any waiter, then awaited by
// another queue, is recorded by the boundary before the next dispatch (a self-locking packet) instead of being left to
// that dispatch's lock; without the switch it stays queued. A busy GPU mutex makes the try fail and leaves the label
// to the dispatch's lock (no wait, no deadlock); a wait by the producing queue itself never records early. Either way
// the label lands with its value, after the work recorded before it.
bool RunAwaitedEarly(AgcDriver::VulkanDevice& device, std::uint64_t address, std::uint32_t writer, std::uint32_t waiter, bool contended) {
    const bool on = AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::CLabelAwaitedEarly);
    const std::uint32_t value = 0x5a000000u | writer;
    const std::array<std::uint32_t, 4> pattern{5, 6, 7, 8};
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::GuestMemory::TagGpuLockThread(writer);
        AgcDriver::GuestMemory::SetCurrentPacket(0x37, writer);
        if (!device.FillBuffer(address + 16384, 4096, pattern)) return false;
    }
    const std::array<std::uint32_t, 5> packet{0xc0033700u, 0x00100200u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), value};
    Require(Access::Label(packet, writer) && Access::Deferred(), "awaited early: an unawaited label did not stay queued");
    Access::Await(waiter, address, 4);
    struct ClearWait {
        std::uint32_t queue;
        ~ClearWait() { Access::Await(queue, 0, 0); }
    } clearWait{waiter};
    std::promise<void> held;
    std::jthread holder;
    if (contended) {
        auto ready = held.get_future();
        holder = std::jthread([&](std::stop_token stop) {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            held.set_value();
            while (!stop.stop_requested()) std::this_thread::yield();
        });
        ready.wait();
    }
    Access::Flush(0xc0031500u, writer);
    if (holder.joinable()) { holder.request_stop(); holder.join(); }
    const bool early = on && waiter != writer && !contended;
    Require(Access::Deferred() != early, "awaited early: the boundary before the dispatch did not record exactly when another queue awaited the label");
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        if (early) {
            Recorder::LabelRefusal refusal{};
            const auto hit = device.PendingLabel(address, 4, 0, &refusal);
            Require(hit.has_value() && hit->value == value && hit->queue == writer, "awaited early: the recorded label is not in the table");
        }
        Access::PacketRecord(device, writer);
        Require(!Access::Deferred(), "awaited early: the dispatch's lock left the label queued");
        device.SubmitRecorded(false);
        device.WaitIdle();
        std::uint32_t actual = 0;
        std::memcpy(&actual, reinterpret_cast<const void*>(address), 4);
        Require(actual == value, "awaited early: the label has the wrong value");
        Require(std::memcmp(reinterpret_cast<const void*>(address + 16384), pattern.data(), sizeof(pattern)) == 0, "awaited early: work before the label did not complete");
    }
    return true;
}

// Arch C labels, C_LABEL_WAKE: a compute queue's WAIT_REG_MEM on another compute queue's label is left to reach its
// sleep (spin 100 us), then the label is recorded. With the switch the recording thread signals the waiter's slot
// event (one signal) and the wait returns; with it off, or with _VERIFY=1 (every signal dropped), no signal is sent and
// the wait still returns through its timer step. No lock is held by the waiter while it sleeps; the recorder holds the
// GPU mutex while it signals (no deadlock either way). The label lands with its value after the earlier work.
bool RunLabelWake(AgcDriver::VulkanDevice& device, std::uint64_t address, std::uint32_t writer, std::uint32_t waiter) {
    namespace Wake = AgcDriver::ArchB::LabelWake;
    const bool on = Wake::On();
    const char* verifyText = std::getenv("APS5_ARCHB_C_LABEL_WAKE_VERIFY");
    const bool dropped = verifyText != nullptr && std::string_view(verifyText) == "1";
    const std::uint32_t value = 0x77000000u | writer;
    const std::array<std::uint32_t, 4> pattern{9, 10, 11, 12};
    const auto stamp = Access::Stamp();
    const std::array<std::uint32_t, 7> packet{0xc0053c00u, 0x13u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), value, 0xffffffffu, 0x19u};
    const auto signalsBefore = Wake::SignalCount();
    std::atomic<bool> returned{false};
    std::chrono::steady_clock::time_point back{};
    std::exception_ptr failure;
    std::thread wait([&] {
        try {
            AgcDriver::GuestMemory::TagGpuLockThread(waiter);
            Access::Wait(packet, waiter, stamp);
            back = std::chrono::steady_clock::now();
            returned.store(true, std::memory_order_release);
        } catch (...) {
            failure = std::current_exception();
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Require(!returned.load(std::memory_order_acquire), "label wake: the wait returned before its label was recorded");
    std::chrono::steady_clock::time_point recorded{};
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::GuestMemory::TagGpuLockThread(writer);
        AgcDriver::GuestMemory::SetCurrentPacket(0x37, writer);
        if (!device.FillBuffer(address + 16384, 4096, pattern)) {
            Access::Stamp();
            wait.detach();
            return false;
        }
        Require(device.WriteLabelOnGpu(address, std::as_bytes(std::span(&value, 1)), Access::Stamp(), writer, false) == 0, "label wake: the label was not recorded on the GPU");
        recorded = std::chrono::steady_clock::now();
    }
    wait.join();
    if (failure) std::rethrow_exception(failure);
    const auto signals = Wake::SignalCount() - signalsBefore;
    Require(signals == (on && !dropped ? 1u : 0u), "label wake: " + std::to_string(signals) + " signals for one recorded label");
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        device.SubmitRecorded(false);
        device.WaitIdle();
        std::uint32_t actual = 0;
        std::memcpy(&actual, reinterpret_cast<const void*>(address), 4);
        Require(actual == value, "label wake: the label has the wrong value");
        Require(std::memcmp(reinterpret_cast<const void*>(address + 16384), pattern.data(), sizeof(pattern)) == 0, "label wake: work before the label did not complete");
    }
    std::printf("label wake 0x%x <- 0x%x: %s, returned %.1f us after the record\n", waiter, writer, on ? (dropped ? "on, signal dropped" : "on") : "off", std::chrono::duration<double, std::micro>(back - recorded).count());
    return true;
}

bool RunStateBatching(AgcDriver::VulkanDevice& device, std::uint64_t address, std::uint32_t queue) {
    using namespace AgcDriver::DriverDetail;
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    AgcDriver::GuestMemory::TagGpuLockThread(queue);
    const std::array<std::uint32_t, 4> pattern{1, 2, 3, 4};
    if (!device.FillBuffer(address + 16384, 4096, pattern)) return false;
    const bool batched = !std::getenv("APS5_NO_LABEL_STATE_BATCHING") && QueuedLabelTable() && LabelBatchSubmit();
    const auto write = [&](std::uint32_t value) {
        const std::array<std::uint32_t, 5> packet{0xc0033700u, 0x00100200u,
            static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), value};
        Require(Access::Label(packet, queue) && Access::Deferred(), "state batching: WRITE_DATA did not queue");
        Access::Age(std::chrono::hours(-1));
    };
    const auto groups = labelGroups.load();
    write(1);
    for (const auto header : {0xc0017600u, 0xc0001000u, 0xc0018100u, 0xc0004600u, 0xc0055800u, 0xc0016900u, 0xc0017900u}) {
        Access::Flush(header, queue);
        Require(Access::Deferred() == batched, "state batching: state packet flushed at the wrong boundary");
    }
    const std::array<std::uint32_t, 8> release{0xc0064900u, 0x514u, 2u << 29u,
        static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 2, 0x12345678u, 0};
    Require(Access::Label(release, queue), "state batching: RELEASE_MEM was not handled");
    Access::Age(std::chrono::hours(-1));
    Access::Flush(0xc0017600u, queue);
    Require(Access::DeferredCount() == (batched ? 2u : 0u), "state batching: labels separated by registers did not join");
    write(3);
    Access::Flush(0xc0031500u, queue);
    Require(Access::Deferred(), "state batching: a dispatch did not leave labels to its own lock");
    Access::PacketRecord(device, queue);
    Require(labelGroups.load() - groups == (batched ? 1u : 3u), "state batching: wrong number of recorded groups");
    device.SubmitRecorded(false);
    device.WaitIdle();
    std::uint64_t actual = 0;
    std::memcpy(&actual, reinterpret_cast<const void*>(address), sizeof(actual));
    Require(actual == 0x1234567800000003ull, "state batching: overlapping 32/64-bit labels lost their order");
    Require(std::memcmp(reinterpret_cast<const void*>(address + 16384), pattern.data(), sizeof(pattern)) == 0, "state batching: work before the labels did not complete");
    for (const auto header : {0xc0053c00u, 0xc0032200u, 0xc0044000u, AgcDriver::RenderingWaitPacketHeader}) {
        write(header);
        Access::Flush(header, queue);
        Require(!Access::Deferred(), "state batching: a memory or rendering-wait boundary retained queued labels");
        device.WaitIdle();
        std::memcpy(&actual, reinterpret_cast<const void*>(address), 4);
        Require(static_cast<std::uint32_t>(actual) == header, "state batching: a boundary lost its label");
    }
    write(4);
    Access::Age(std::chrono::hours(1));
    Access::Flush(0xc0017600u, queue);
    Require(!Access::Deferred(), "state batching: state packets suppressed the label deadline");
    device.WaitIdle();
    std::memcpy(&actual, reinterpret_cast<const void*>(address), 4);
    Require(static_cast<std::uint32_t>(actual) == 4, "state batching: overdue label was not published");
    if (!device.FillBuffer(address + 16384, 4096, pattern)) return false;
    write(5);
    auto interrupt = release;
    interrupt[2] |= 2u << 24u;
    interrupt[5] = 6;
    Require(Access::Label(interrupt, queue) && !Access::Deferred(), "state batching: interrupt label overtook queued labels");
    device.WaitIdle();
    std::memcpy(&actual, reinterpret_cast<const void*>(address), sizeof(actual));
    Require(actual == 0x1234567800000006ull, "state batching: interrupt label lost its value");
    return true;
}

}

int main(int argc, char** argv) {
    try {
        GuestBlock labels;
        std::shared_ptr<AgcDriver::VulkanDevice> device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Access::Device(device);
        struct Reset {
            ~Reset() { Access::Device(nullptr); }
        } reset;
        const auto base = labels.Address();
        if (argc == 2 && std::string_view(argv[1]) == "--state-batching") {
            for (const auto queue : {0u, 0x28u, 0x40u}) {
                if (!RunStateBatching(*device, base, queue)) return VulkanTestSkipped;
            }
            std::puts("label state batching tests passed");
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--label-wake") {
            std::uint64_t address = base;
            for (const auto writer : {0x20u, 0x38u, 0x50u}) {
                if (!RunLabelWake(*device, address, writer, writer == 0x28u ? 0x30u : 0x28u)) return VulkanTestSkipped;
                address += 256;
            }
            std::puts("label wake tests passed");
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--awaited-early") {
            std::uint64_t address = base;
            for (const auto queue : {0x20u, 0x28u, 0x30u, 0x38u, 0x40u, 0x48u, 0x50u}) {
                if (!RunAwaitedEarly(*device, address, queue, 0, false) || !RunAwaitedEarly(*device, address + 256, queue, queue == 0x50u ? 0x20u : queue + 8, false)) return VulkanTestSkipped;
                address += 512;
            }
            if (!RunAwaitedEarly(*device, address, 0x38, 0x38, false) || !RunAwaitedEarly(*device, address + 256, 0x38, 0, true)) return VulkanTestSkipped;
            std::puts("awaited label early record tests passed");
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--producer") {
            std::uint64_t address = base;
            for (const auto queue : {0x20u, 0x28u, 0x30u, 0x38u, 0x40u, 0x48u, 0x50u}) {
                if (!RunProducer(*device, address, queue, 0) || !RunProducer(*device, address + 256, 0, queue, 4, 4, true)) return VulkanTestSkipped;
                address += 512;
            }
            for (const auto offset : {-8, -4, 4, 8}) {
                if (!RunProducer(*device, address, 0x38, 0x28, offset, 8)) return VulkanTestSkipped;
                address += 256;
            }
            if (!RunProducer(*device, address, 0x38, 0x38)
                || !RunProducer(*device, address + 256, 0x38, 0, 0, 0)
                || !RunProducer(*device, address + 512, 0x38, 0, 0, 4, false, true)) return VulkanTestSkipped;
            std::puts("awaited label producer flush tests passed");
            return 0;
        }
        Require(argc == 1, "invalid test arguments");
        if (!Run(*device, base, 0, false, false, false) || !Run(*device, base + 256, 0, true, false, false)
            || !Run(*device, base + 512, 0, false, true, false) || !Run(*device, base + 768, 0x20, false, false, false)
            || !Run(*device, base + 1024, 0, false, false, true) || !Run(*device, base + 1280, 0, true, true, false)
            || !Run(*device, base + 1536, 0, true, false, false, true)) return VulkanTestSkipped;
        Recorder::ReportWaitLabels(true);
        std::puts("awaited label submit tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
