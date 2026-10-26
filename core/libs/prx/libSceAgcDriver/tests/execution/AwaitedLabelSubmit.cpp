#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
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

}

int main() {
    try {
        GuestBlock labels;
        std::shared_ptr<AgcDriver::VulkanDevice> device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Access::Device(device);
        struct Reset {
            ~Reset() { Access::Device(nullptr); }
        } reset;
        const auto base = labels.Address();
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
