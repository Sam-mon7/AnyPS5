#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <thread>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t ReleaseMemHeader = 0xc0064900u;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "unimported label order: cannot allocate the guest block");
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

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    volatile std::uint32_t* Words() { return reinterpret_cast<volatile std::uint32_t*>(block); }

    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    std::uint8_t* block = nullptr;
};

alignas(64) volatile std::uint32_t commandMemory[16] = {};

std::uint64_t AddressOf(volatile const std::uint32_t* word) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(word));
}

int StoreLabel(AgcDriver::VulkanDevice& device, std::uint64_t address, std::uint32_t value, std::uint64_t stamp) {
    const auto bytes = std::as_bytes(std::span(&value, 1));
    const int reason = device.WriteLabelOnGpu(address, bytes, stamp, 0);
    if (reason >= 1 && reason <= 4) {
        if (reason != 1) device.WaitIdle();
        AgcDriver::GuestMemory::Write(address, bytes, 4);
    }
    return reason;
}

void LabelReserve(AgcDriver::VulkanDevice& device, GuestBlock& preceding, GuestBlock& reserved, GuestBlock& exhausted) {
    using namespace AgcDriver::Graphics;
    // The baseline has drained all work. Start with one ordinary block and room for one label block.
    struct RestoreImportWindow {
        ~RestoreImportWindow() { SetHostImportWindow(0); }
    } restoreWindow;
    SetHostImportWindow(BlockBytes);
    ClearHostImports(device.Device());
    SetHostImportBudget(2 * BlockBytes);
    SetHostImportLabelReserve(BlockBytes);
    const std::array<std::uint32_t, 4> pattern{0xa5a5a5a5u, 0xa5a5a5a5u, 0xa5a5a5a5u, 0xa5a5a5a5u};
    Require(device.FillBuffer(preceding.Address() + 8192, 4096, pattern), "label reserve: ordinary capacity cannot import its first block");
    Require(HostImportLiveBytes() == BlockBytes, "label reserve: the ordinary fixture did not occupy exactly one block");
    Require(!device.FillBuffer(reserved.Address() + 4096, 4096, pattern), "label reserve: an ordinary fill consumed unused label reserve");
    Require(HostImportLiveBytes() == BlockBytes, "label reserve: refusing an ordinary fill changed residency");
    Require(StoreLabel(device, reserved.Address(), 3, 3) == 0, "label reserve: a busy recorder could not import and record a reserved label");
    Require(HostImportLiveBytes() == 2 * BlockBytes, "label reserve: the label did not consume exactly the reserved block");
    device.SubmitRecorded(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (reserved.Words()[0] != 3) {
        Require(std::chrono::steady_clock::now() < deadline, "label reserve: the GPU label never landed");
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    for (std::size_t i = 8192 / 4; i < (8192 + 4096) / 4; ++i)
        Require(preceding.Words()[i] == pattern[0], "label reserve: a label became visible before the preceding GPU fill");
    device.WaitIdle();

    // Both blocks remain protected by recent use. Exhaustion must keep StoreLabel's synchronous fallback.
    const std::array<std::uint32_t, 4> later{0x5a5a5a5au, 0x5a5a5a5au, 0x5a5a5a5au, 0x5a5a5a5au};
    Require(device.FillBuffer(preceding.Address() + 12288, 4096, later), "label reserve: the existing ordinary import was not reusable at the total cap");
    Require(StoreLabel(device, exhausted.Address(), 4, 4) == 3, "label reserve: exhaustion did not use the unimported-label fallback");
    Require(HostImportLiveBytes() == 2 * BlockBytes, "label reserve: exhaustion exceeded the total import budget");
    Require(exhausted.Words()[0] == 4, "label reserve: the fallback label did not land synchronously");
    for (std::size_t i = 12288 / 4; i < (12288 + 4096) / 4; ++i)
        Require(preceding.Words()[i] == later[0], "label reserve: fallback returned before the preceding GPU fill completed");
    exhausted.Words()[0] = ReleaseMemHeader;
    Require(device.FillBuffer(preceding.Address() + 16384, 4096, pattern), "label reserve: cannot record work after the exhaustion fallback");
    Require(StoreLabel(device, reserved.Address() + 4, 5, 5) == 0, "label reserve: an existing reserved import was not reused");
    device.WaitIdle();
    Require(reserved.Words()[1] == 5 && exhausted.Words()[0] == ReleaseMemHeader,
            "label reserve: a later GPU label overwrote the guest's reused fallback word");
}

}

int main() {
    try {
        struct RestoreImportLimits {
            ~RestoreImportLimits() {
                AgcDriver::Graphics::SetHostImportLabelReserve(0);
                AgcDriver::Graphics::SetHostImportBudget(0);
            }
        } restore;
        AgcDriver::Graphics::SetHostImportLabelReserve(0);
        GuestBlock labels;
        GuestBlock reserved;
        GuestBlock exhausted;
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        const std::array<std::uint32_t, 4> pattern{0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
        if (!device->FillBuffer(labels.Address() + 4096, 4096, pattern)) {
            std::printf("skipped, the device does not import guest memory\n");
            return VulkanTestSkipped;
        }
        StoreLabel(*device, AddressOf(&commandMemory[8]), 1, 1);
        StoreLabel(*device, labels.Address() + 4, 2, 2);
        device->SubmitRecorded(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (labels.Words()[1] != 2) {
            Require(std::chrono::steady_clock::now() < deadline, "unimported label order: the later label never landed");
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        const std::uint32_t seen = commandMemory[8];
        commandMemory[8] = ReleaseMemHeader;
        device->WaitIdle();
        const std::uint32_t reused = commandMemory[8];
        std::array<char, 256> message{};
        std::snprintf(message.data(), message.size(), "unimported label order: the label held 0x%08x when a later label had landed, and the guest's reused word holds 0x%08x instead of 0x%08x", seen, reused, ReleaseMemHeader);
        Require(seen == 1 && reused == ReleaseMemHeader, message.data());
        LabelReserve(*device, labels, reserved, exhausted);
        std::puts("unimported label order tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
