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
#include <memory>
#include <set>
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
        Require(data != nullptr, "dispatch address reuse: guest allocation failed");
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
    std::size_t outputOffset = OutputOffset;
    std::vector<std::uint32_t> lastKey;
    explicit Scene(AgcDriver::VulkanDevice& device) : device(device) {
        constants.Words()[0] = Constant;
    }
    void Compile(std::size_t offset) {
        outputOffset = offset;
        std::vector<std::uint32_t> program(Code.begin(), Code.end());
        std::memcpy(code.Data(), program.data(), program.size() * sizeof(std::uint32_t));
        const auto out = output.Address(outputOffset);
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
        }), "dispatch address reuse: shader lacks genuine BDA loads");
        Require(!AgcDriver::Graphics::ShaderStoresThroughAddresses(compiled.spirv), "dispatch address reuse: address-store classifier differs from shader");
        snapshots = {{code.Address(), std::as_bytes(std::span(code.Words(), program.size()))}};
        const AgcDriver::Graphics::CompiledShader shader{ShaderRecompiler::ShaderStage::Compute, &compiled, 0};
        auto key = AgcDriver::Graphics::ShaderResources::ContentKey(shader, true, false);
        Require(lastKey.empty() || key != lastKey, "dispatch address reuse: the issued resource key did not change");
        lastKey = std::move(key);
    }
    void Input(std::uint32_t seed) {
        for (std::uint32_t lane = 0; lane < Lanes; ++lane) input.Words()[lane] = seed + 17 * lane;
        std::memset(output.Data(), 0xa5, BlockBytes);
    }
    void Run(bool wait = true) {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        device.Dispatch(compiled, 1, 1, 1, snapshots, code.Address());
        if (wait) {
            device.WaitIdle();
            AgcDriver::Graphics::SharedResourceCache().Clear();
        }
    }
    void Check(std::uint32_t seed, const std::string& label, bool emit = true) {
        AgcDriver::GuestMemory::FlushGpuWrites(output.Address(), BlockBytes);
        std::array<std::uint8_t, BlockBytes> expected;
        expected.fill(0xa5);
        for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
            const auto value = Constant + 1u + seed + 17u * lane;
            std::memcpy(expected.data() + outputOffset + lane * 4, &value, sizeof(value));
        }
        for (std::size_t byte = 0; byte < BlockBytes; ++byte) {
            Require(output.Data()[byte] == expected[byte], "dispatch address reuse: " + label + " output/guard byte " + std::to_string(byte) + " differs");
        }
        if (emit) {
            std::printf("BYTES %s ", label.c_str());
            for (std::size_t byte = 0; byte < BlockBytes; ++byte) std::printf("%02x", output.Data()[byte]);
            std::putchar('\n');
        }
    }
};


using Resources = AgcDriver::Graphics::ShaderResources;
using Tables = AgcDriver::Graphics::BdaResources;

void Counts() {
    const auto c = Resources::AddressReuseCounts();
    const auto t = Tables::AddressTableReuseCounts();
    std::printf("COUNTS builds=%llu eligible=%llu reusedSets=%llu freshSets=%llu retiredSets=%llu descriptorsWritten=%llu verified=%llu failures=%llu tableBuilds=%llu tableFills=%llu logicalBytes=%llu storageBytes=%llu paddedBuilds=%llu tableVerified=%llu tableFailures=%llu tableCacheHits=%llu tableHandleReuses=%llu\n",
        static_cast<unsigned long long>(c.builds), static_cast<unsigned long long>(c.eligible), static_cast<unsigned long long>(c.reusedSets), static_cast<unsigned long long>(c.freshSets), static_cast<unsigned long long>(c.retiredSets), static_cast<unsigned long long>(c.descriptorsWritten), static_cast<unsigned long long>(c.verified), static_cast<unsigned long long>(c.failures), static_cast<unsigned long long>(t.builds), static_cast<unsigned long long>(t.fills), static_cast<unsigned long long>(t.logicalBytesWritten), static_cast<unsigned long long>(t.storageBytesRequested), static_cast<unsigned long long>(t.paddedBuilds), static_cast<unsigned long long>(t.verified), static_cast<unsigned long long>(t.failures), static_cast<unsigned long long>(t.cacheHits), static_cast<unsigned long long>(t.handleReuses));
}

void Repeated(Scene& scene, const std::string& mode) {
    std::set<std::uint64_t> logicalSizes;
    std::vector<std::unique_ptr<Block>> extra;
    const std::array<unsigned, 7> counts{0, 1, 5, 17, 33, 1, 0};
    const unsigned rounds = mode == "--ranges" ? counts.size() : 4u;
    for (unsigned round = 0; round < rounds; ++round) {
        if (mode == "--ranges") {
            while (extra.size() > counts[round]) extra.pop_back();
            while (extra.size() < counts[round]) extra.push_back(std::make_unique<Block>());
        }
        if (round == 2 && mode == "--remap") scene.input.Remap();
        if (round == 2 && mode == "--imports") {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            AgcDriver::Graphics::ClearHostImports(AgcDriver::VulkanDeviceTestAccess::ContextOf(scene.device).device);
        }
        scene.Compile(OutputOffset + round * 256u);
        const auto seed = 0x100u + round * 0x321u;
        scene.Input(seed);
        const auto before = Tables::AddressTableReuseCounts();
        scene.Run();
        const auto after = Tables::AddressTableReuseCounts();
        if (after.fills > before.fills) logicalSizes.insert(after.logicalBytesWritten - before.logicalBytesWritten);
        scene.Check(seed, "changed-key-" + std::to_string(round));
    }
    const auto c = Resources::AddressReuseCounts();
    const auto t = Tables::AddressTableReuseCounts();
    Require(c.failures == 0 && t.failures == 0, "dispatch address reuse: a verification failure was counted");
    if (mode == "--old") {
        Require(c.eligible == 0 && c.reusedSets == 0 && t.builds == 0, "dispatch address reuse: default-off path reused parts");
    } else if (mode == "--no-pool") {
        Require(c.eligible == 0 && c.reusedSets == 0 && c.verified == 0, "dispatch address reuse: descriptor-cache-disabled set unexpectedly recycled");
        Require(t.builds >= rounds && t.verified >= rounds && t.fills > 0, "dispatch address reuse: table VERIFY was skipped without set recycling");
    } else {
        Require(c.eligible >= rounds && c.reusedSets >= 2 && c.retiredSets >= 2, "dispatch address reuse: required retired-set reuse on changing keys");
        Require(c.descriptorsWritten > 0 && t.fills > 0 && t.logicalBytesWritten > 0, "dispatch address reuse: no descriptor/table work was observed");
        if (mode == "--verify") Require(c.verified >= rounds && t.verified >= rounds, "dispatch address reuse: VERIFY did not compare descriptors and table bytes");
        if (mode == "--ranges") Require(logicalSizes.size() >= 3 && t.paddedBuilds > 0 && t.storageBytesRequested >= t.logicalBytesWritten && t.handleReuses > 0, "dispatch address reuse: distinct logical extents, size-class padding and actual table handle reuse were not exercised");
    }
}

void InFlight(AgcDriver::VulkanDevice& device, bool requireTableHit) {
    Scene first(device), second(device);
    first.Compile(OutputOffset);
    second.Compile(OutputOffset + 256);
    first.Input(0x321u);
    second.Input(0x987u);
    first.Run(false);
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::Graphics::SharedResourceCache().Clear();
    }
    const auto pending = Resources::AddressReuseCounts();
    Require(pending.eligible > 0 && pending.freshSets > 0 && pending.retiredSets == 0, "dispatch address reuse: unfinished first set was retired");
    const auto beforeTable = Tables::AddressTableReuseCounts();
    second.Run(false);
    const auto afterTable = Tables::AddressTableReuseCounts();
    if (requireTableHit) Require(afterTable.cacheHits > beforeTable.cacheHits && afterTable.paddedBuilds > 0 && afterTable.verified >= 2, "dispatch address reuse: a held padded table was not shared and verified");
    const auto held = Resources::AddressReuseCounts();
    Require(held.freshSets > pending.freshSets && held.reusedSets == 0 && held.retiredSets == 0, "dispatch address reuse: an in-flight set was recycled");
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        device.WaitIdle();
        AgcDriver::Graphics::SharedResourceCache().Clear();
    }
    first.Check(0x321u, "in-flight-first");
    second.Check(0x987u, "in-flight-second");
    first.Compile(OutputOffset + 512);
    first.Input(0x456u);
    first.Run();
    first.Check(0x456u, "after-retirement");
    const auto retired = Resources::AddressReuseCounts();
    Require(retired.reusedSets > held.reusedSets && retired.retiredSets >= 2, "dispatch address reuse: completed sets did not become reusable");
}
}

int main(int argc, char** argv) {
    try {
        Require(argc <= 2, "usage: --old | --on | --verify | --in-flight | --remap | --imports | --ranges | --no-pool | --cache");
        const std::string mode = argc == 2 ? argv[1] : "--old";
        Require(mode == "--old" || mode == "--on" || mode == "--verify" || mode == "--in-flight" || mode == "--remap" || mode == "--imports" || mode == "--ranges" || mode == "--no-pool" || mode == "--cache", "unknown dispatch address reuse mode");
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (mode == "--in-flight" || mode == "--cache") InFlight(*device, mode == "--cache");
        else {
            Scene scene(*device);
            Repeated(scene, mode);
            device->WaitIdle();
        }
        Counts();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        Counts();
        return 1;
    }
}
