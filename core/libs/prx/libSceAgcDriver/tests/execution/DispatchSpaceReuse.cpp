#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

namespace {
using AgcDriver::Graphics::Require;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Lanes = 32;
constexpr std::size_t OutputOffset = 256;
constexpr std::uint32_t Constant = 0x13570000u;
alignas(256) constexpr std::array<std::uint32_t, 13> Code{0xf4000200u, 0xfa000000u, 0x34020082u, 0xdc308000u, 0x03020001u, 0xbf8cc07fu, 0x7e040208u, 0x4a040481u, 0xbf8c3f70u, 0x4a040503u, 0xe0701000u, 0x80010201u, 0xbf810000u};

class Block {
public:
    Block() {
#ifdef _WIN32
        data = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes));
        if (data) GuestArena::GuestArenaCommit_nid_postfix(data, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        auto* mapped = mmap(nullptr, BlockBytes * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes * 2) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + BlockBytes - aligned);
            data = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, BlockBytes);
        }
#endif
        Require(data != nullptr, "dispatch space reuse: guest allocation failed");
        std::memset(data, 0, BlockBytes);
        GuestAllocations::Mutation().Add(data, BlockBytes, true, true, true);
    }
    ~Block() {
        GuestAllocations::Mutation().Remove(data);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(data, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(data, BlockBytes);
#else
        munmap(data, BlockBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, BlockBytes);
#endif
    }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    std::uint8_t* Data() const { return data; }
    std::uint32_t* Words(std::size_t offset = 0) const { return reinterpret_cast<std::uint32_t*>(data + offset); }
    std::uint64_t Address(std::size_t offset = 0) const { return reinterpret_cast<std::uintptr_t>(data + offset); }
    void Remap() {
        GuestAllocations::Mutation().Remove(data);
        GuestAllocations::Mutation().Add(data, BlockBytes, true, true, true);
    }
private:
    std::uint8_t* data = nullptr;
};

struct Scene {
    Block constants, input, output, code;
    AgcDriver::VulkanDevice& device;
    ShaderRecompiler::RecompileResult compiled;
    std::vector<AgcDriver::Graphics::GuestMemorySnapshot> snapshots;
    explicit Scene(AgcDriver::VulkanDevice& device, bool writable = false) : device(device) {
        constants.Words()[0] = Constant;
        std::vector<std::uint32_t> program(Code.begin(), Code.end());
        if (writable) program.insert(program.begin() + 6, {0xdc708000u, 0x00020301u, 0xbf8c3f70u});
        std::memcpy(code.Data(), program.data(), program.size() * sizeof(std::uint32_t));
        const auto out = output.Address(OutputOffset);
        const std::vector<std::uint32_t> userData{
            static_cast<std::uint32_t>(constants.Address()), static_cast<std::uint32_t>(constants.Address() >> 32),
            static_cast<std::uint32_t>(input.Address()), static_cast<std::uint32_t>(input.Address() >> 32),
            static_cast<std::uint32_t>(out), static_cast<std::uint32_t>((out >> 32) & 0xffffu), Lanes * 4, 0x31016facu};
        const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{
            {code.Address(), std::as_bytes(std::span(program))},
            {constants.Address(), std::as_bytes(std::span(constants.Words(), Lanes))}}};
        const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
        ShaderRecompiler::RecompileRequest request{
            {ShaderRecompiler::ShaderStage::Compute, code.Address(), program, 0, {}},
            {32, 0, userData, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128}};
        request.useCache = false;
        compiled = ShaderRecompiler::Recompile(request);
        Require(std::any_of(compiled.bindings.begin(), compiled.bindings.end(), [](const auto& binding) {
            return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable;
        }), "dispatch space reuse: shader lacks genuine BDA loads");
        Require(AgcDriver::Graphics::ShaderStoresThroughAddresses(compiled.spirv) == writable, "dispatch space reuse: address-store classifier differs from shader");
        snapshots = {{code.Address(), std::as_bytes(std::span(code.Words(), program.size()))}};
    }
    void Input(std::uint32_t seed) {
        for (std::uint32_t lane = 0; lane < Lanes; ++lane) input.Words()[lane] = seed + 17 * lane;
        std::memset(output.Data(), 0xa5, BlockBytes);
    }
    void Run(bool wait = true) {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        device.Dispatch(compiled, 1, 1, 1, snapshots, code.Address());
        if (wait) device.WaitIdle();
    }
    void Check(std::uint32_t seed, const std::string& label, bool emit = true) {
        AgcDriver::GuestMemory::FlushGpuWrites(output.Address(), BlockBytes);
        std::array<std::uint8_t, BlockBytes> expected;
        expected.fill(0xa5);
        for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
            const auto value = Constant + 1u + seed + 17u * lane;
            std::memcpy(expected.data() + OutputOffset + lane * 4, &value, sizeof(value));
        }
        for (std::size_t byte = 0; byte < BlockBytes; ++byte) {
            Require(output.Data()[byte] == expected[byte], "dispatch space reuse: " + label + " output/guard byte " + std::to_string(byte) + " differs");
        }
        if (emit) {
            std::printf("BYTES %s ", label.c_str());
            for (std::size_t byte = 0; byte < BlockBytes; ++byte) std::printf("%02x", output.Data()[byte]);
            std::putchar('\n');
        }
    }
};

void Warm(Scene& scene, bool requireHit) {
    for (unsigned round = 0; round < 3; ++round) {
        scene.Input(0x100u);
        scene.Run();
        scene.Check(0x100u, "repeat-" + std::to_string(round));
    }
    const auto counts = AgcDriver::VulkanDevice::DispatchSpaceReuseCounts();
    Require(!requireHit || (counts.enabled && counts.hits >= 2), "dispatch space reuse: required two actual feature hits, got " + std::to_string(counts.hits));
}

void PrintCounts() {
    const auto c = AgcDriver::VulkanDevice::DispatchSpaceReuseCounts();
    std::printf("COUNTS enabled=%u built=%llu hits=%llu inFlight=%llu spaceChanged=%llu guardChanged=%llu snapshotRefused=%llu revalidateFailed=%llu verified=%llu mismatches=%llu\n", c.enabled,
        static_cast<unsigned long long>(c.built), static_cast<unsigned long long>(c.hits), static_cast<unsigned long long>(c.inFlight),
        static_cast<unsigned long long>(c.spaceChanged), static_cast<unsigned long long>(c.guardChanged), static_cast<unsigned long long>(c.snapshotRefused),
        static_cast<unsigned long long>(c.revalidateFailed), static_cast<unsigned long long>(c.verified), static_cast<unsigned long long>(c.mismatches));
}
}

int main(int argc, char** argv) {
    try {
        Require(argc <= 2, "usage: --old | --require-hit | --verify | --in-flight | --remap | --imports | --snapshot | --writable");
        const std::string mode = argc == 2 ? argv[1] : AgcDriver::VulkanDevice::DispatchSpaceReuseCounts().enabled ? "--require-hit" : "--old";
        Require(mode == "--old" || mode == "--require-hit" || mode == "--verify" || mode == "--in-flight" || mode == "--remap" || mode == "--imports" || mode == "--snapshot" || mode == "--writable", "unknown dispatch space reuse mode");
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        {
            Scene scene(*device, mode == "--writable");
            Warm(scene, mode != "--old" && mode != "--writable");
            if (mode == "--old" || mode == "--require-hit" || mode == "--verify") {
                scene.Input(0x2345u);
                scene.Run();
                scene.Check(0x2345u, "changed-input");
                const auto c = AgcDriver::VulkanDevice::DispatchSpaceReuseCounts();
                Require(mode != "--old" || (!c.enabled && c.hits == 0), "dispatch space reuse: OLD unexpectedly enabled reuse");
                Require(mode != "--verify" || (c.verified > 0 && c.mismatches == 0), "dispatch space reuse: VERIFY did not verify cleanly");
            } else if (mode == "--writable") {
                Require(AgcDriver::VulkanDevice::DispatchSpaceReuseCounts().hits == 0, "dispatch space reuse: writable-address shader was reused");
            } else if (mode == "--in-flight") {
                scene.Input(0x765u);
                scene.Run(false);
                const auto before = AgcDriver::VulkanDevice::DispatchSpaceReuseCounts();
                scene.Run();
                const auto after = AgcDriver::VulkanDevice::DispatchSpaceReuseCounts();
                Require(after.inFlight > before.inFlight && after.hits == before.hits, "dispatch space reuse: unfinished use was not refused");
                scene.Check(0x765u, "in-flight");
            } else {
                scene.Input(0x987u);
                const auto before = AgcDriver::VulkanDevice::DispatchSpaceReuseCounts();
                if (mode == "--remap") scene.input.Remap();
                if (mode == "--imports") {
                    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
                    AgcDriver::Graphics::ClearHostImports(AgcDriver::VulkanDeviceTestAccess::ContextOf(*device).device);
                }
                std::array<std::byte, 4> outside{};
                if (mode == "--snapshot") scene.snapshots.push_back({reinterpret_cast<std::uintptr_t>(outside.data()), outside});
                scene.Run();
                const auto after = AgcDriver::VulkanDevice::DispatchSpaceReuseCounts();
                Require(after.hits == before.hits, "dispatch space reuse: changed proof incorrectly hit");
                Require(mode != "--snapshot" || after.snapshotRefused > before.snapshotRefused, "dispatch space reuse: outside snapshot not refused");
                Require(mode == "--snapshot" || after.spaceChanged + after.guardChanged > before.spaceChanged + before.guardChanged, "dispatch space reuse: changed mapping/import proof not refused");
                scene.Check(0x987u, mode.substr(2));
            }
            PrintCounts();
            device->WaitIdle();
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        PrintCounts();
        return 1;
    }
}
