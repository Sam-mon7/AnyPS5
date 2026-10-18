#include "VulkanTestDevice.hpp"
#include "Recompiler.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/src/Driver/Dispatch/PreparedHeapRefresh.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static std::weak_ptr<Graphics::BufferPool> Pool(const VulkanDevice& device) { return device.graphicsContext().bufferPool; }
};
}

namespace {

using namespace AgcDriver::Graphics;
namespace Memory = AgcDriver::GuestMemory;

#ifdef _WIN32
constexpr std::size_t BlockBytes = 65536;
alignas(256) constexpr std::array<std::uint32_t, 8> ReadCode{
    0x34020082u, 0xdc308000u, 0x04080001u, 0xbf8c3f70u, 0xe0701000u, 0x80010401u, 0xbf800000u, 0xbf810000u
};

struct GuestBlock {
    void* raw = GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes);
    GuestBlock() {
        GuestArena::GuestArenaCommit_nid_postfix(raw, BlockBytes, PAGE_READWRITE, BlockBytes);
        std::memset(raw, 0x11, BlockBytes);
        GuestAllocations::Mutation().Add(raw, BlockBytes, true, true, true);
    }
    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(raw);
        GuestArena::GuestArenaReset_nid_postfix(raw, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(raw, BlockBytes);
    }
};

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::span<std::uint32_t> output) {
    const auto address = reinterpret_cast<std::uintptr_t>(guest.raw);
    const auto destination = reinterpret_cast<std::uintptr_t>(output.data());
    const std::array<std::uint32_t, 10> userData{0, 0, 0, 0,
        static_cast<std::uint32_t>(destination), static_cast<std::uint32_t>(destination >> 32u), static_cast<std::uint32_t>(output.size_bytes()), 0x31016facu,
        static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{32, 1, 1}, 0, {false, false, false}, false, 1};
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(ReadCode.data()), std::as_bytes(std::span(ReadCode))}}};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(ReadCode.data()), ReadCode, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128}
    };
    request.useCache = false;
    auto shader = ShaderRecompiler::Recompile(request);
    Require(!shader.bdaWrites && std::any_of(shader.bindings.begin(), shader.bindings.end(), [](const auto& binding) {
        return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable;
    }), "production heap fixture did not compile a BDA reader");
    return shader;
}

void Run(std::shared_ptr<AgcDriver::VulkanDevice>& device, GuestBlock& guest) {
    const bool enabled = DispatchLockDietEnabled();
    const auto initial = DispatchHeapRefreshCounters();
    alignas(256) std::array<std::uint32_t, 32> output{};
    const auto shader = Compile(*device, guest, output);
    const auto dispatch = [&](std::shared_ptr<AgcDriver::PreparedDispatch> prepared = {}, bool wait = true) {
        std::lock_guard gpu(Memory::GpuMutex());
        device->Dispatch(shader, 1, 1, 1, {}, 0, std::move(prepared));
        if (wait) device->WaitIdle();
    };
    const auto check = [&](std::uint32_t expected) {
        Require(output[0] == expected, "production PrepareDispatch consumed stale heap bytes");
        for (std::size_t lane = 1; lane < output.size(); ++lane) Require(output[lane] == 0x11111111u, "production heap fixture changed an untouched lane");
    };
    dispatch(device->PrepareDispatch(shader, {}, 32));
    check(0x11111111u);
    auto* const words = static_cast<volatile std::uint32_t*>(guest.raw);
    words[0] = 0x22334455u;
    const auto before = DispatchHeapRefreshCounters();
    dispatch(device->PrepareDispatch(shader, {}, 32));
    check(words[0]);
    Require(DispatchHeapRefreshCounters().applied == before.applied + (enabled ? 1 : 0), "production stage B did not apply its prepared heap");
    for (const bool explicitSync : {false, true}) {
        dispatch({}, false);
        const std::uint32_t label = explicitSync ? 0x33445566u : 0x44556677u;
        {
            std::lock_guard gpu(Memory::GpuMutex());
            Require(device->WriteLabelOnGpu(reinterpret_cast<std::uintptr_t>(guest.raw), std::as_bytes(std::span(&label, 1)), 1001, 0, false) == 5,
                "production heap fixture failed to queue a completion label");
            if (explicitSync) device->WaitIdle();
        }
        const auto beforeSync = DispatchHeapRefreshCounters();
        auto prepared = device->PrepareDispatch(shader, {}, 32);
        const bool presynced = enabled && !explicitSync && device->CanWaitUnlocked() && std::getenv("APS5_NO_PRESYNC") == nullptr;
        Require(DispatchHeapRefreshCounters().presyncs == beforeSync.presyncs + (presynced ? 1 : 0), "production pending-writer pre-sync count differs");
        dispatch(std::move(prepared));
        check(label);
    }
    dispatch({}, false);
    auto prepared = device->PrepareDispatch(shader, {}, 32);
    const std::uint32_t late = 0x55667788u;
    {
        std::lock_guard gpu(Memory::GpuMutex());
        Require(device->WriteLabelOnGpu(reinterpret_cast<std::uintptr_t>(guest.raw), std::as_bytes(std::span(&late, 1)), 1002, 0, false) == 5,
            "production heap fixture failed to queue a late writer");
    }
    const auto beforeLate = DispatchHeapRefreshCounters();
    dispatch(std::move(prepared));
    check(late);
    Require(DispatchHeapRefreshCounters().fallback == beforeLate.fallback + (enabled ? 1 : 0), "production stage B did not reject a late heap writer");
    std::atomic<bool> ready{false}, release{false};
    std::exception_ptr error;
    const std::weak_ptr<AgcDriver::VulkanDevice> alive = device;
    const auto pool = AgcDriver::VulkanDeviceTestAccess::Pool(*device);
    Require(!pool.expired(), "production heap fixture has no retained buffer pool");
    {
        std::jthread worker([owner = device, &shader, &ready, &release, &error]() mutable {
            try {
                auto abandoned = owner->PrepareDispatch(shader, {}, 32);
                ready.store(true, std::memory_order_release);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!release.load(std::memory_order_acquire)) {
                    Require(std::chrono::steady_clock::now() < deadline, "device teardown fixture timed out");
                    std::this_thread::yield();
                }
                std::lock_guard gpu(Memory::GpuMutex());
                abandoned.reset();
                owner.reset();
            } catch (...) { error = std::current_exception(); ready.store(true, std::memory_order_release); }
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        const bool preparedBeforeTeardown = ready.load(std::memory_order_acquire);
        device.reset();
        const bool held = !alive.expired();
        release.store(true, std::memory_order_release);
        worker.join();
        if (error) std::rethrow_exception(error);
        Require(preparedBeforeTeardown && held && alive.expired(), "prepared dispatch device ownership did not span teardown");
        Require(pool.expired(), "production dispatch heap buffers outlived their Vulkan device");
    }
    device = std::make_shared<AgcDriver::VulkanDevice>();
    words[0] = 0x66778899u;
    const auto recreated = Compile(*device, guest, output);
    auto rebuilt = device->PrepareDispatch(recreated, {}, 32);
    {
        std::lock_guard gpu(Memory::GpuMutex());
        device->Dispatch(recreated, 1, 1, 1, {}, 0, std::move(rebuilt));
        device->WaitIdle();
    }
    check(words[0]);
    const auto after = DispatchHeapRefreshCounters();
    if (!enabled) Require(after.captured == initial.captured && after.unavailable == initial.unavailable &&
        after.stagedBytes == initial.stagedBytes && after.stageUs == initial.stageUs && after.applied == initial.applied &&
        after.fallback == initial.fallback && after.presyncs == initial.presyncs && after.presyncFallbacks == initial.presyncFallbacks,
        "disabled production heap preparation changed counters");
}
#endif

}

int main() {
#ifndef _WIN32
    return VulkanTestSkipped;
#else
    try {
        if (!GuestArena::GuestArenaAvailable_nid_postfix() || !Memory::WriteWatched()) return VulkanTestSkipped;
        GuestBlock guest;
        auto opened = OpenVulkanTestDevice();
        if (!opened) return VulkanTestSkipped;
        std::shared_ptr<AgcDriver::VulkanDevice> device(std::move(opened));
        Run(device, guest);
        {
            std::lock_guard gpu(Memory::GpuMutex());
            device.reset();
        }
        std::cout << "Production dispatch heap preparation and device teardown tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#endif
}
