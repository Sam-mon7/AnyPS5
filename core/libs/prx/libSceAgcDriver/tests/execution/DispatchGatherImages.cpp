#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchGatherShadow.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
namespace Shadow = AgcDriver::DriverDetail::DispatchShadow;
using AgcDriver::DriverDetail::DispatchGatherRefusal;
using AgcDriver::ArchB::Feature;
using AgcDriver::ArchB::Switches;

constexpr std::size_t Piece = 65536;
constexpr std::size_t BlockBytes = 4 * Piece;
constexpr std::uint32_t Side = 8;
constexpr std::uint32_t Sentinel = 0xdeadbeefu;
constexpr std::array<std::uint32_t, 4> Sampler{0x00000092u, 0x00fff000u, 0u, 0u};

alignas(256) constexpr std::array<std::uint32_t, 9> LoadCode{
    0x7e3c0280u, 0x7e3e0280u, 0xf0000108u, 0x00010a1eu,
    0xbf8c3f70u, 0x7e060280u, 0xe0700000u, 0x80000a03u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 11> SampleCode{
    0x7e2802ffu, 0x3f000000u, 0x7e2a02ffu, 0x3f000000u,
    0xf09c0108u, 0x00610a14u, 0xbf8c3f70u, 0x7e060280u,
    0xe0700000u, 0x80000a03u, 0xbf810000u,
};

alignas(256) constexpr auto ArraySampleCode = [] {
    auto code = SampleCode;
    code[3] = 0x3f800000u;
    code[4] = 0xf09c0120u;
    return code;
}();

alignas(256) constexpr std::array<std::uint32_t, 6> StoreCode{
    0x7e14020cu, 0x7e3c0280u, 0x7e3e0280u,
    0xf0201108u, 0x00010a1eu, 0xbf810000u,
};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header LoadHeader{};
alignas(8) Header SampleHeader{};
alignas(8) Header ArraySampleHeader{};
alignas(8) Header StoreHeader{};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, Piece));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        void* mapped = mmap(nullptr, BlockBytes + Piece, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + Piece - 1) & ~(static_cast<std::uintptr_t>(Piece) - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + Piece) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + Piece - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "dispatch gather images: cannot allocate guest memory");
        std::fill_n(block, BlockBytes, 0);
        for (std::size_t piece = 0; piece < 4; ++piece) GuestAllocations::Mutation().Add(At(piece), Piece, true, true, true);
    }

    ~GuestBlock() {
        for (std::size_t piece = 0; piece < 4; ++piece) GuestAllocations::Mutation().Remove(At(piece));
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
        munmap(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    std::uint32_t* At(std::size_t piece) const { return reinterpret_cast<std::uint32_t*>(block + piece * Piece); }

private:
    std::uint8_t* block = nullptr;
};

std::uint64_t Address(const void* pointer) { return reinterpret_cast<std::uintptr_t>(pointer); }

std::array<std::uint32_t, 8> TextureDescriptor(const void* pointer, bool sampled, bool array = false) {
    const auto address = Address(pointer);
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | ((sampled ? 22u : 20u) << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | (array ? 0u : ((Side - 1u) << 14u)), 0xfacu | ((array ? 12u : 9u) << 28u), array ? 2u : 0u, 0u, 0u, 0u};
}

void Fill(std::uint32_t* texture, bool sampled, std::uint32_t value, bool array = false) {
    const auto descriptor = TextureDescriptor(texture, sampled, array);
    const auto surface = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(descriptor));
    Require(surface.guestBytes <= Piece && surface.mips.size() == 1, "dispatch gather images: texture geometry exceeds its backing");
    auto* bytes = reinterpret_cast<std::uint8_t*>(texture);
    for (std::uint32_t layer = 0; layer < surface.layers; ++layer) {
        for (std::uint32_t y = 0; y < (array ? 1u : Side); ++y) {
            auto* row = reinterpret_cast<std::uint32_t*>(bytes + surface.GuestLayerOffset(layer) + surface.mips[0].tiledOffset + y * surface.mips[0].pitchBytes);
            std::fill_n(row, Side, value);
        }
    }
}

std::array<std::uint32_t, 16> UserData(const GuestBlock& guest, bool sampled, bool array = false) {
    std::array<std::uint32_t, 16> words{};
    const auto output = Address(guest.At(2));
    words[0] = static_cast<std::uint32_t>(output);
    words[1] = static_cast<std::uint32_t>(output >> 32u) & 0xffffu;
    words[2] = 256;
    words[3] = 0x31016facu;
    const auto image = TextureDescriptor(guest.At(0), sampled, array);
    std::copy(image.begin(), image.end(), words.begin() + 4);
    std::copy(Sampler.begin(), Sampler.end(), words.begin() + 12);
    return words;
}

void Register(Header& header, std::span<const std::uint32_t> code) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(code.size_bytes());
    header.shader.code = code.data();
    header.shader.type = 0;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> body) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(body.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), body);
}

void Dispatch(std::vector<std::uint32_t>& commands, const std::uint32_t* code, std::span<const std::uint32_t> userData) {
    const auto address = Address(code);
    Append(commands, 0x76, {0x20c, static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)});
    Append(commands, 0x76, {0x207, 1, 1, 1});
    Append(commands, 0x76, {0x212, 0, static_cast<std::uint32_t>(userData.size()) << 1u});
    for (std::uint32_t word = 0; word < userData.size(); ++word) Append(commands, 0x76, {0x240 + word, userData[word]});
    Append(commands, 0x15, {1, 1, 1, 0x8041});
}

void Submit(std::vector<std::uint32_t> commands) {
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "dispatch gather images: submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

void Submit(const std::uint32_t* code, std::span<const std::uint32_t> words) {
    std::vector<std::uint32_t> commands;
    Dispatch(commands, code, words);
    Submit(std::move(commands));
}

void Expect(std::uint32_t* output, std::uint32_t expected, std::string_view what) {
    AgcDriver::GuestMemory::FlushGpuWrites(Address(output), sizeof(*output));
    Require(*output == expected, "dispatch gather images: " + std::string(what) + " produced " + std::to_string(*output) + ", expected " + std::to_string(expected));
}

void PrintDelta(std::string_view name, const Shadow::Counters& before, const Shadow::Counters& after) {
    std::cerr << name << " calls=" << after.calls - before.calls << " templates=" << after.templatesBuilt - before.templatesBuilt << " matched=" << after.matched - before.matched << " gathered=" << after.gathered - before.gathered << " materialized=" << after.materializedChecks - before.materializedChecks << " committed=" << after.commitChecks - before.commitChecks << " reused=" << after.committedWithoutRebuild - before.committedWithoutRebuild << " pending=" << after.pending - before.pending << " queued=" << after.queued - before.queued;
    for (std::size_t index = 0; index < after.graphRefusals.size(); ++index) if (after.graphRefusals[index] != before.graphRefusals[index]) std::cerr << " graph[" << index << "]=" << after.graphRefusals[index] - before.graphRefusals[index];
    for (std::size_t index = 0; index < after.differences.size(); ++index) if (after.differences[index] != before.differences[index]) std::cerr << " differ[" << index << "]=" << after.differences[index] - before.differences[index];
    std::cerr << '\n';
}

void Repeats(const GuestBlock& guest, bool sampled, bool off, bool array = false) {
    const auto* code = array ? ArraySampleCode.data() : sampled ? SampleCode.data() : LoadCode.data();
    const auto value = sampled ? 0x3f000000u : 37u;
    Fill(guest.At(0), sampled, value, array);
    auto words = UserData(guest, sampled, array);
    if (array) Register(ArraySampleHeader, ArraySampleCode);
    else if (sampled) Register(SampleHeader, SampleCode);
    else Register(LoadHeader, LoadCode);
    const auto before = Shadow::ReadCounters();
    Submit(code, words);
    Expect(guest.At(2), value, "image warmup");
    const auto warm = Shadow::ReadCounters();
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        *guest.At(2) = Sentinel;
        Submit(code, words);
        Expect(guest.At(2), value, "image repeat");
    }
    const auto after = Shadow::ReadCounters();
    PrintDelta(array ? "registered sampled array and sampler" : sampled ? "sampled image and sampler" : "read-only storage image", before, after);
    if (off) {
        Require(after.calls == before.calls && after.templatesBuilt == before.templatesBuilt && after.gathered == before.gathered && after.commitChecks == before.commitChecks, "dispatch gather images: disabled switch performed shadow work");
        return;
    }
    Require(after.calls == before.calls + 3 && warm.templatesBuilt > before.templatesBuilt, "dispatch gather images: real entry did not learn the image template");
    Require(after.matched >= warm.matched + 2 && after.gathered >= warm.gathered + 2, "dispatch gather images: repeated images were not gathered");
    Require(after.materializedChecks >= warm.materializedChecks + 2 && after.commitChecks >= warm.commitChecks + 2, "dispatch gather images: repeated images lacked materialization and actual commit proof");
    Require(after.committedWithoutRebuild > warm.committedWithoutRebuild, "dispatch gather images: image repeat did not commit without rebuilding");
    const auto& classBefore = before.populations[static_cast<std::size_t>(Shadow::Population::ImageRead)];
    const auto& classWarm = warm.populations[static_cast<std::size_t>(Shadow::Population::ImageRead)];
    const auto& classAfter = after.populations[static_cast<std::size_t>(Shadow::Population::ImageRead)];
    Require(classAfter[static_cast<std::size_t>(Shadow::PopulationStage::Calls)] == classBefore[static_cast<std::size_t>(Shadow::PopulationStage::Calls)] + 3, "dispatch gather images: image population calls were not counted");
    for (const auto stage : {Shadow::PopulationStage::Matched, Shadow::PopulationStage::Gathered, Shadow::PopulationStage::MaterializedChecks, Shadow::PopulationStage::CommitChecks}) {
        Require(classAfter[static_cast<std::size_t>(stage)] >= classWarm[static_cast<std::size_t>(stage)] + 2, "dispatch gather images: image population stage was not counted");
    }
    Require(classAfter[static_cast<std::size_t>(Shadow::PopulationStage::CommittedWithoutRebuild)] > classWarm[static_cast<std::size_t>(Shadow::PopulationStage::CommittedWithoutRebuild)], "dispatch gather images: image population reusable commit was not counted");
}

void DescriptorMutation(const GuestBlock& guest, bool sampler) {
    Repeats(guest, true, false);
    auto words = UserData(guest, true);
    const auto expected = sampler ? 0x3f000000u : 0x3f800000u;
    if (sampler) {
        words[14] |= 1u << 20u;
    } else {
        Fill(guest.At(1), true, expected);
        const auto descriptor = TextureDescriptor(guest.At(1), true);
        std::copy(descriptor.begin(), descriptor.end(), words.begin() + 4);
    }
    *guest.At(2) = Sentinel;
    const auto before = Shadow::ReadCounters();
    Submit(SampleCode.data(), words);
    Expect(guest.At(2), expected, sampler ? "changed sampler fallback" : "changed image fallback");
    const auto after = Shadow::ReadCounters();
    PrintDelta(sampler ? "sampler structural refusal" : "image structural refusal", before, after);
    const auto descriptorWords = static_cast<std::size_t>(AgcDriver::DriverDetail::DispatchGatherDifference::DescriptorWords);
    const bool structural = after.differences[descriptorWords] > before.differences[descriptorWords];
    Require(structural && after.matched == before.matched && after.gathered == before.gathered && after.commitChecks == before.commitChecks, "dispatch gather images: descriptor mutation was not a structural refusal");
}

void PendingImageWriter(const GuestBlock& guest) {
    Repeats(guest, false, false);
    const auto reader = UserData(guest, false);
    auto words = reader;
    words[12] = 41;
    Register(StoreHeader, StoreCode);
    const auto warm = Shadow::ReadCounters();
    Submit(StoreCode.data(), words);
    Expect(guest.At(0), 41, "storage image writer warmup");
    const auto written = Shadow::ReadCounters();
    const auto writes = static_cast<std::size_t>(DispatchGatherRefusal::ImageWrites);
    Require(written.graphRefusals[writes] > warm.graphRefusals[writes] && written.gathered == warm.gathered && written.commitChecks == warm.commitChecks, "dispatch gather images: storage image writer was admitted");
    words[12] = 59;
    std::vector<std::uint32_t> commands;
    Dispatch(commands, StoreCode.data(), words);
    Dispatch(commands, LoadCode.data(), reader);
    *guest.At(2) = Sentinel;
    const auto before = Shadow::ReadCounters();
    Submit(std::move(commands));
    Expect(guest.At(2), 59, "read after pending storage image writer");
    const auto after = Shadow::ReadCounters();
    PrintDelta("pending image writer and reader", before, after);
    Require(after.graphRefusals[writes] > before.graphRefusals[writes], "dispatch gather images: pending writer lacked an image-write refusal");
    Require(after.gathered == before.gathered + 1 && after.materializedChecks == before.materializedChecks + 1 && after.commitChecks == before.commitChecks + 1, "dispatch gather images: image reader metadata was not checked through materialization and the actual ordered commit");
    const auto& classBefore = before.populations[static_cast<std::size_t>(Shadow::Population::ImageRead)];
    const auto& classAfter = after.populations[static_cast<std::size_t>(Shadow::Population::ImageRead)];
    for (const auto stage : {Shadow::PopulationStage::Gathered, Shadow::PopulationStage::MaterializedChecks, Shadow::PopulationStage::CommitChecks}) {
        Require(classAfter[static_cast<std::size_t>(stage)] == classBefore[static_cast<std::size_t>(stage)] + 1, "dispatch gather images: read-after-write image stage was not counted");
    }
    const auto reused = after.committedWithoutRebuild - before.committedWithoutRebuild;
    Require(reused <= 1 && classAfter[static_cast<std::size_t>(Shadow::PopulationStage::CommittedWithoutRebuild)] - classBefore[static_cast<std::size_t>(Shadow::PopulationStage::CommittedWithoutRebuild)] == reused, "dispatch gather images: ordered image reader counted a writer or disagreed about resource reuse");
}

void RegisteredIdentity(const GuestBlock& guest, bool array) {
    Repeats(guest, true, false, array);
    auto words = UserData(guest, true, array);
    const auto oldSerial = [&] {
        const auto range = GuestAllocations::GuestAllocationsLookup_nid_postfix(Address(guest.At(0)), false);
        Require(range != nullptr, "dispatch gather images: image range was not registered");
        return range->backingSerial;
    }();
    GuestAllocations::Mutation().Remove(guest.At(0));
    GuestAllocations::Mutation().Add(guest.At(0), Piece, true, true, true);
    const auto newRange = GuestAllocations::GuestAllocationsLookup_nid_postfix(Address(guest.At(0)), false);
    Require(newRange != nullptr && newRange->backingSerial != oldSerial, "dispatch gather images: image re-registration did not change its identity");
    *guest.At(2) = Sentinel;
    const auto before = Shadow::ReadCounters();
    Submit(array ? ArraySampleCode.data() : SampleCode.data(), words);
    Expect(guest.At(2), 0x3f000000u, array ? "stale registered-view fallback" : "replaced image backing rebuild");
    const auto after = Shadow::ReadCounters();
    PrintDelta(array ? "registered image backing replacement" : "ordinary image backing replacement", before, after);
    const auto stale = static_cast<std::size_t>(DispatchGatherRefusal::RegisteredViews);
    if (array) {
        Require(after.graphRefusals[stale] > before.graphRefusals[stale] && after.matched == before.matched && after.gathered == before.gathered && after.commitChecks == before.commitChecks, "dispatch gather images: stale registered-view identity was accepted");
        const auto image = static_cast<std::size_t>(Shadow::Population::ImageRead);
        Require(after.populationRefusals[image][stale] > before.populationRefusals[image][stale], "dispatch gather images: stale registered-view refusal was not counted in the image population");
    } else {
        Require(after.graphRefusals[stale] == before.graphRefusals[stale] && after.matched == before.matched + 1 && after.gathered == before.gathered + 1 && after.materializedChecks == before.materializedChecks + 1 && after.commitChecks == before.commitChecks + 1, "dispatch gather images: ordinary image replacement did not verify its rebuilt commit");
        Require(after.committedWithoutRebuild == before.committedWithoutRebuild, "dispatch gather images: replaced image backing was credited as an unreconstructed resource");
    }
}

}

int main(int argc, char** argv) {
    try {
        Require(argc <= 2, "dispatch gather images: too many arguments");
        const std::string_view mode = argc == 2 ? argv[1] : "--sampled";
        const bool off = mode == "--off";
        Require(Switches::Enabled(Feature::DispatchGatherShadow) != off, "dispatch gather images: required activation state is absent");
        if (!off) Require(std::getenv("APS5_DISPATCH_GATHER_SHADOW_STRICT") != nullptr && std::string_view(std::getenv("APS5_DISPATCH_GATHER_SHADOW_STRICT")) == "1", "dispatch gather images: strict test mode was not enabled");
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        {
            GuestBlock guest;
            try {
                if (mode == "--sampled" || mode == "--off") Repeats(guest, true, off);
                else if (mode == "--storage-read") Repeats(guest, false, false);
                else if (mode == "--image-mutation") DescriptorMutation(guest, false);
                else if (mode == "--sampler-mutation") DescriptorMutation(guest, true);
                else if (mode == "--pending-writer") PendingImageWriter(guest);
                else if (mode == "--registered-identity" || mode == "--backing-rebuild") RegisteredIdentity(guest, mode == "--registered-identity");
                else Require(false, "dispatch gather images: unknown argument");
            } catch (...) {
                try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
                throw;
            }
            AgcDriverShutdown_nid_postfix();
        }
        const auto counts = Shadow::ReadCounters();
        Require(counts.falseMatch == 0 && counts.materializedMismatch == 0 && counts.commitMismatch == 0, "dispatch gather images: strict verification recorded a false match or mismatch");
        std::cout << "dispatch gather images " << mode << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
