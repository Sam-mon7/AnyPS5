#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchGatherShadow.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ImageEpochs.hpp"
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
#include <atomic>
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

constexpr std::size_t Piece = 262144;
constexpr std::size_t Pieces = Shadow::VariantLimit + 4;
constexpr std::size_t BlockBytes = Pieces * Piece;
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

alignas(256) constexpr std::array<std::uint32_t, 12> SampleStoreCode{
    0x7e2802ffu, 0x3f000000u, 0x7e2a02ffu, 0x3f000000u,
    0xf09c0108u, 0x00610a14u, 0xbf8c3f70u,
    0x7e3c0280u, 0x7e3e0280u, 0xf0201108u, 0x00040a1eu, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 21> ItemFreeCode{
    0xf4080100u, 0xfa000000u, 0xf4000200u, 0xfa000020u,
    0xf4000240u, 0xfa000024u, 0xbf8cc07fu, 0xbe8c0391u,
    0xbf0a8408u, 0xbf850001u, 0x930c830cu, 0xbf068009u,
    0xbf850004u, 0xf4000280u, 0xfa000028u, 0xbf8cc07fu,
    0x800c0a0cu, 0x7e02020cu, 0xe0700000u, 0x80010100u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 27> UndecidableResourceCode{
    0xf4080100u, 0xfa000000u, 0xf4080400u, 0xfa000030u,
    0xbf8cc07fu, 0xe0300000u, 0x80040100u, 0xbf8c3f70u,
    0x7e100501u, 0xbf8cc07fu, 0xbf068008u, 0xbf840006u,
    0xf4080300u, 0xfa000010u, 0xbf8cc07fu,
    0xe0300000u, 0x80030100u, 0xbf820005u,
    0xf4080300u, 0xfa000020u, 0xbf8cc07fu,
    0xe0300000u, 0x80030100u, 0xbf8c3f70u,
    0xe0700000u, 0x80010100u, 0xbf810000u,
};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header LoadHeader{};
alignas(8) Header SampleHeader{};
alignas(8) Header ArraySampleHeader{};
alignas(8) Header StoreHeader{};
alignas(8) Header SampleStoreHeader{};
alignas(8) Header ItemFreeHeader{};
alignas(8) Header UndecidableResourceHeader{};

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
        for (std::size_t piece = 0; piece < Pieces; ++piece) GuestAllocations::Mutation().Add(At(piece), Piece, true, true, true);
    }

    ~GuestBlock() {
        for (std::size_t piece = 0; piece < Pieces; ++piece) GuestAllocations::Mutation().Remove(At(piece));
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
    std::cerr << name << " calls=" << after.calls - before.calls << " templates=" << after.templatesBuilt - before.templatesBuilt << " matched=" << after.matched - before.matched << " gathered=" << after.gathered - before.gathered << " materialized=" << after.materializedChecks - before.materializedChecks << " committed=" << after.commitChecks - before.commitChecks << " reused=" << after.committedWithoutRebuild - before.committedWithoutRebuild << " pending=" << after.pending - before.pending << " queued=" << after.queued - before.queued << " queuedWithoutDrain=" << after.queuedWithoutDrain - before.queuedWithoutDrain << " queuedCommitted=" << after.queuedCommittedWithoutRebuild - before.queuedCommittedWithoutRebuild << " variantHits=" << after.variantHits - before.variantHits << " variantsLearned=" << after.variantsLearned - before.variantsLearned << " variantSearches=" << after.variantSearches - before.variantSearches;
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
    Require(structural && after.variantHits == before.variantHits, "dispatch gather images: descriptor mutation matched a prior variant");
    Require(after.variantsLearned == before.variantsLearned + 1u, "dispatch gather images: changed image or sampler did not learn a new exact variant");
    Submit(SampleCode.data(), words);
    Expect(guest.At(2), expected, "repeated changed image or sampler");
    const auto repeated = Shadow::ReadCounters();
    Require(repeated.variantHits == after.variantHits + 1u && repeated.commitChecks == after.commitChecks + 1u && repeated.variantsLearned == after.variantsLearned, "dispatch gather images: changed image or sampler did not reuse its verified variant");
}

void WriterRepeats(const GuestBlock& guest, bool sampled) {
    Fill(guest.At(0), sampled, sampled ? 0x3f000000u : 0u);
    Fill(guest.At(1), false, 0u);
    const auto base = UserData(guest, sampled);
    std::array<std::uint32_t, 24> words{};
    std::copy(base.begin(), base.end(), words.begin());
    const auto destination = TextureDescriptor(guest.At(1), false);
    std::copy(destination.begin(), destination.end(), words.begin() + 16);
    if (!sampled) words[12] = 41u;
    if (sampled) Register(SampleStoreHeader, SampleStoreCode);
    else Register(StoreHeader, StoreCode);
    const auto* code = sampled ? SampleStoreCode.data() : StoreCode.data();
    const auto data = std::span<const std::uint32_t>(words).first(sampled ? 24u : 16u);
    const auto before = Shadow::ReadCounters();
    Submit(code, data);
    Expect(guest.At(sampled ? 1u : 0u), sampled ? 0x3f000000u : 41u, "storage writer warmup");
    const auto warm = Shadow::ReadCounters();
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        if (!sampled) words[12] = 59u + repeat;
        Submit(code, data);
        Expect(guest.At(sampled ? 1u : 0u), sampled ? 0x3f000000u : 59u + repeat, "storage writer repeat");
    }
    const auto after = Shadow::ReadCounters();
    PrintDelta(sampled ? "one writer with sampled reader" : "one writer without readers", before, after);
    Require(after.calls == before.calls + 3 && warm.templatesBuilt > before.templatesBuilt, "dispatch gather images: real entry did not learn the writer template");
    Require(after.matched >= warm.matched + 2 && after.gathered >= warm.gathered + 2, "dispatch gather images: repeated writer descriptors were not gathered");
    Require(after.materializedChecks >= warm.materializedChecks + 2 && after.commitChecks >= warm.commitChecks + 2, "dispatch gather images: repeated writers lacked materialization and actual commit proof");
    const auto& classWarm = warm.populations[static_cast<std::size_t>(Shadow::Population::ImageWrite)];
    const auto& classAfter = after.populations[static_cast<std::size_t>(Shadow::Population::ImageWrite)];
    Require(classAfter[static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectChecks)] >= classWarm[static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectChecks)] + 2, "dispatch gather images: repeated writers lacked actual dirty and pending publication proof");
    Require(classAfter[static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectMismatch)] == 0, "dispatch gather images: writer publication disagreed");
}

void WriterModeRefusal(const GuestBlock& guest) {
    Fill(guest.At(0), false, 0u);
    auto words = UserData(guest, false);
    words[12] = 83u;
    Register(StoreHeader, StoreCode);
    const auto before = Shadow::ReadCounters();
    Submit(StoreCode.data(), words);
    Expect(guest.At(0), 83u, "writer mode warmup");
    words[12] = 97u;
    Submit(StoreCode.data(), words);
    Expect(guest.At(0), 97u, "writer mode repeat");
    const auto after = Shadow::ReadCounters();
    PrintDelta("writer mode refusal", before, after);
    Require(after.gathered > before.gathered && after.commitRefusals[12] > before.commitRefusals[12], "dispatch gather images: unsupported writer mode lacked its commit refusal");
    Require(after.commitChecks == before.commitChecks, "dispatch gather images: unsupported writer mode was credited with a verified commit");
    const auto& classBefore = before.populations[static_cast<std::size_t>(Shadow::Population::ImageWrite)];
    const auto& classAfter = after.populations[static_cast<std::size_t>(Shadow::Population::ImageWrite)];
    Require(classAfter[static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectChecks)] == classBefore[static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectChecks)], "dispatch gather images: unsupported writer mode was credited with a verified publication");
}

void WriterDescriptorMutation(const GuestBlock& guest) {
    WriterRepeats(guest, false);
    Fill(guest.At(1), false, 0u);
    auto words = UserData(guest, false);
    const auto descriptor = TextureDescriptor(guest.At(1), false);
    std::copy(descriptor.begin(), descriptor.end(), words.begin() + 4);
    words[12] = 103u;
    const auto before = Shadow::ReadCounters();
    Submit(StoreCode.data(), words);
    Expect(guest.At(1), 103u, "changed written image fallback");
    const auto after = Shadow::ReadCounters();
    PrintDelta("written image structural refusal", before, after);
    const auto changed = static_cast<std::size_t>(AgcDriver::DriverDetail::DispatchGatherDifference::DescriptorWords);
    Require(after.differences[changed] > before.differences[changed] && after.variantHits == before.variantHits, "dispatch gather images: changed written-image descriptor matched a prior variant");
    const auto& classBefore = before.populations[static_cast<std::size_t>(Shadow::Population::ImageWrite)];
    const auto& classAfter = after.populations[static_cast<std::size_t>(Shadow::Population::ImageWrite)];
    Require(classAfter[static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectChecks)] - classBefore[static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectChecks)] == after.commitChecks - before.commitChecks, "dispatch gather images: changed written-image commit lacked its publication proof");
}

void WriterVariants(const GuestBlock& guest, bool full, bool batched) {
    Register(StoreHeader, StoreCode);
    const auto count = full ? Shadow::VariantLimit : 3u;
    const auto wordsFor = [&](std::size_t index, std::uint32_t value) {
        auto words = UserData(guest, false);
        const auto descriptor = TextureDescriptor(guest.At(index), false);
        std::copy(descriptor.begin(), descriptor.end(), words.begin() + 4);
        words[12] = value;
        return words;
    };
    const auto initial = Shadow::ReadCounters();
    for (std::size_t index = 0; index < count; ++index) {
        Fill(guest.At(index), false, 0u);
        Submit(StoreCode.data(), wordsFor(index, 151u + static_cast<std::uint32_t>(index)));
        Expect(guest.At(index), 151u + static_cast<std::uint32_t>(index), "variant learning");
    }
    const auto learned = Shadow::ReadCounters();
    PrintDelta("bounded writer variant learning", initial, learned);
    Require(learned.variantsLearned == initial.variantsLearned + count, "dispatch gather images: distinct writer descriptors did not learn exactly one variant each");
    Require(learned.variantHits == initial.variantHits, "dispatch gather images: new writer descriptors matched old variants");
    for (std::size_t index = 0; index < count; ++index) Require(learned.variantLearnedSlots[index] == initial.variantLearnedSlots[index] + 1u, "dispatch gather images: variant learning slot census differs");
    if (full) {
        Fill(guest.At(count), false, 0u);
        Submit(StoreCode.data(), wordsFor(count, 179u));
        Expect(guest.At(count), 179u, "full variant bucket fallback");
        const auto overflow = Shadow::ReadCounters();
        Require(overflow.variantFull == learned.variantFull + 1u && overflow.variantsLearned == learned.variantsLearned && overflow.variantHits == learned.variantHits && overflow.commitChecks == learned.commitChecks, "dispatch gather images: full bucket admitted or replaced an unmatched variant");
    }
    const auto before = Shadow::ReadCounters();
    const auto calls = count * 2u;
    std::vector<std::uint32_t> commands;
    for (std::size_t call = 0; call < calls; ++call) {
        const auto index = (call + 1u) % count;
        const auto words = wordsFor(index, 191u + static_cast<std::uint32_t>(call));
        if (batched) Dispatch(commands, StoreCode.data(), words);
        else {
            Submit(StoreCode.data(), words);
            Expect(guest.At(index), 191u + static_cast<std::uint32_t>(call), "revisited exact writer variant");
        }
    }
    if (batched) Submit(std::move(commands));
    const auto after = Shadow::ReadCounters();
    for (std::size_t call = count; call < calls; ++call) Expect(guest.At((call + 1u) % count), 191u + static_cast<std::uint32_t>(call), "final alternating writer value");
    PrintDelta(batched ? "same-batch writer variants" : full ? "full bucket retained writer variants" : "alternating writer variants", before, after);
    Require(after.variantHits == before.variantHits + calls && after.variantsLearned == before.variantsLearned, "dispatch gather images: returning exact writer variants were relearned or missed");
    Require(after.gathered == before.gathered + calls && after.materializedChecks == before.materializedChecks + calls && after.commitChecks == before.commitChecks + calls, "dispatch gather images: alternating variants lacked complete gather, materialization or actual commit proof");
    for (std::size_t index = 0; index < count; ++index) Require(after.variantSelected[index] == before.variantSelected[index] + 2u, "dispatch gather images: exact variant selection census differs");
    const auto writer = static_cast<std::size_t>(Shadow::Population::ImageWrite);
    const auto effect = static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectChecks);
    Require(after.populations[writer][effect] == before.populations[writer][effect] + calls, "dispatch gather images: alternating written owners lacked publication proof");
}

void PendingImageWriter(const GuestBlock& guest, bool repeatWriter = false) {
    Repeats(guest, false, false);
    WriterRepeats(guest, false);
    const auto reader = UserData(guest, false);
    auto words = reader;
    words[12] = 79;
    std::vector<std::uint32_t> commands;
    Dispatch(commands, StoreCode.data(), words);
    if (repeatWriter) {
        words[12] = 89;
        Dispatch(commands, StoreCode.data(), words);
    }
    Dispatch(commands, LoadCode.data(), reader);
    *guest.At(2) = Sentinel;
    const auto before = Shadow::ReadCounters();
    Submit(std::move(commands));
    Expect(guest.At(2), repeatWriter ? 89u : 79u, "read after pending storage image writer");
    const auto after = Shadow::ReadCounters();
    PrintDelta("pending image writer and reader", before, after);
    const auto calls = repeatWriter ? 3u : 2u;
    Require(after.gathered == before.gathered + calls && after.materializedChecks == before.materializedChecks + calls && after.commitChecks == before.commitChecks + calls, "dispatch gather images: writer and reader metadata were not checked through materialization and the actual ordered commit");
    const auto& writerBefore = before.populations[static_cast<std::size_t>(Shadow::Population::ImageWrite)];
    const auto& writerAfter = after.populations[static_cast<std::size_t>(Shadow::Population::ImageWrite)];
    Require(writerAfter[static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectChecks)] == writerBefore[static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectChecks)] + calls - 1u, "dispatch gather images: ordered writes lacked publication proofs, including the already-dirty writer");
    const auto& classBefore = before.populations[static_cast<std::size_t>(Shadow::Population::ImageRead)];
    const auto& classAfter = after.populations[static_cast<std::size_t>(Shadow::Population::ImageRead)];
    for (const auto stage : {Shadow::PopulationStage::Gathered, Shadow::PopulationStage::MaterializedChecks, Shadow::PopulationStage::CommitChecks}) {
        Require(classAfter[static_cast<std::size_t>(stage)] == classBefore[static_cast<std::size_t>(stage)] + 1, "dispatch gather images: read-after-write image stage was not counted");
    }
}

void WriterSubresources(const GuestBlock& guest, bool array, std::uint32_t baseArray) {
    auto descriptor = TextureDescriptor(guest.At(0), false);
    if (array) {
        descriptor[3] = (descriptor[3] & 0x0fffffffu) | (13u << 28u);
        descriptor[4] = 2u | (baseArray << 16u);
    } else {
        descriptor[3] |= (1u << 12u) | (2u << 16u);
        descriptor[5] |= 2u << 4u;
    }
    const auto texture = AgcDriver::Graphics::DecodeTextureResource(descriptor);
    const auto surface = AgcDriver::Graphics::DescribeSurface(texture);
    Require(surface.guestBytes <= Piece && !surface.thick && surface.mips.size() == (array ? 1u : 3u) && surface.layers == (array ? 3u : 1u), "dispatch gather images: subresource fixture geometry differs");
    Register(StoreHeader, StoreCode);
    Register(LoadHeader, LoadCode);
    auto words = UserData(guest, false);
    std::copy(descriptor.begin(), descriptor.end(), words.begin() + 4);
    const auto run = [&](std::uint32_t value) {
        auto reader = words;
        reader[12] = 0u;
        words[12] = value;
        std::vector<std::uint32_t> commands;
        Dispatch(commands, StoreCode.data(), words);
        Dispatch(commands, LoadCode.data(), reader);
        Submit(std::move(commands));
        Expect(guest.At(2), value, "subresource ordered reader");
    };
    run(211u);
    const auto before = Shadow::ReadCounters();
    run(223u);
    const auto after = Shadow::ReadCounters();
    Require(after.variantHits == before.variantHits + 2u && after.gathered == before.gathered + 2u && after.materializedChecks == before.materializedChecks + 2u && after.commitChecks == before.commitChecks + 2u, "dispatch gather images: repeated subresources lack exact gather and commit proof");
    const auto writer = static_cast<std::size_t>(Shadow::Population::ImageWrite);
    const auto effect = static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectChecks);
    Require(after.populations[writer][effect] == before.populations[writer][effect] + 1u && after.commitRefusals[13] == before.commitRefusals[13], "dispatch gather images: subresource writer lacks publication proof");
    if (!array) {
        words[7] = (words[7] & ~(15u << 12u)) | (2u << 12u);
        run(227u);
        const auto learned = Shadow::ReadCounters();
        run(229u);
        const auto repeated = Shadow::ReadCounters();
        Require(repeated.variantHits == learned.variantHits + 2u && repeated.commitChecks == learned.commitChecks + 2u && repeated.populations[writer][effect] == learned.populations[writer][effect] + 1u, "dispatch gather images: second selected mip lacks exact variant and publication proof");
    }
    AgcDriver::GuestMemory::FlushGpuWrites(Address(guest.At(0)), surface.guestBytes);
    for (std::uint32_t layer = 0; layer < surface.layers; ++layer) {
        for (std::size_t mip = 0; mip < surface.mips.size(); ++mip) {
            const auto offset = surface.GuestOffset(layer, surface.mips[mip]);
            Require(offset + sizeof(std::uint32_t) <= surface.guestBytes, "dispatch gather images: subresource origin exceeds backing");
            const auto* origin = reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(guest.At(0)) + offset);
            const auto expected = array ? (layer == baseArray ? 223u : 0u) : (mip == 1u ? 223u : mip == 2u ? 229u : 0u);
            Require(*origin == expected, "dispatch gather images: selected or untouched subresource origin differs");
        }
    }
    PrintDelta(array ? "first-layer writer and reader" : "selected mip writer and reader", before, Shadow::ReadCounters());
}
void WriterMultipleUnits(const GuestBlock& guest) {
    constexpr std::uint32_t largeSide = 256u;
    auto descriptor = TextureDescriptor(guest.At(0), false);
    descriptor[1] = (descriptor[1] & 0x3fffffffu) | (((largeSide - 1u) & 3u) << 30u);
    descriptor[2] = ((largeSide - 1u) >> 2u) | ((largeSide - 1u) << 14u);
    descriptor[3] |= 9u << 20u;
    const auto texture = AgcDriver::Graphics::DecodeTextureResource(descriptor);
    const auto surface = AgcDriver::Graphics::DescribeSurface(texture);
    Require(texture.tileMode == AgcDriver::Graphics::TextureTileMode::kStandard64KB && surface.guestBytes > 65536u && surface.guestBytes <= Piece && surface.layers == 1u && surface.mips.size() == 1u && !surface.thick, "dispatch gather images: multiunit fixture lacks its bounded tiled single-layer surface");
    Require(Address(guest.At(0)) % 65536u == 0u && std::getenv("APS5_NO_LAYER_CLEAR") == nullptr && std::getenv("APS5_NO_BLOCK_TRACKING") == nullptr, "dispatch gather images: multiunit fixture cannot enable tracked block units");
    auto words = UserData(guest, false);
    std::copy(descriptor.begin(), descriptor.end(), words.begin() + 4);
    auto reader = words;
    Register(StoreHeader, StoreCode);
    Register(LoadHeader, LoadCode);
    words[12] = 113u;
    std::vector<std::uint32_t> warmup;
    Dispatch(warmup, StoreCode.data(), words);
    Dispatch(warmup, LoadCode.data(), reader);
    Submit(std::move(warmup));
    Expect(guest.At(2), 113u, "multiunit warmup reader");
    AgcDriver::GuestMemory::FlushGpuWrites(Address(guest.At(0)), surface.guestBytes);
    words[12] = 127u;
    std::vector<std::uint32_t> commands;
    Dispatch(commands, StoreCode.data(), words);
    words[12] = 131u;
    Dispatch(commands, StoreCode.data(), words);
    Dispatch(commands, LoadCode.data(), reader);
    *guest.At(2) = Sentinel;
    const auto before = Shadow::ReadCounters();
    Submit(std::move(commands));
    const auto after = Shadow::ReadCounters();
    Expect(guest.At(2), 131u, "multiunit ordered GPU reader");
    AgcDriver::GuestMemory::FlushGpuWrites(Address(guest.At(0)), surface.guestBytes);
    const auto offset = surface.GuestOffset(0u, surface.mips[0]);
    Require(offset + sizeof(std::uint32_t) <= surface.guestBytes, "dispatch gather images: multiunit origin exceeds its surface");
    auto* origin = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(guest.At(0)) + offset);
    Require(*origin == 131u, "dispatch gather images: tiled multiunit guest origin differs from the GPU reader");
    PrintDelta("multiunit writer writer reader", before, after);
    Require(after.writerMultiUnitChecks == before.writerMultiUnitChecks + 2u, "dispatch gather images: multiunit fixture did not observe both writers marking multiple tracked units");
    Require(after.gathered == before.gathered + 3u && after.materializedChecks == before.materializedChecks + 3u && after.commitChecks == before.commitChecks + 3u, "dispatch gather images: multiunit ordered uses lack three commit proofs");
    const auto& classBefore = before.populations[static_cast<std::size_t>(Shadow::Population::ImageWrite)];
    const auto& classAfter = after.populations[static_cast<std::size_t>(Shadow::Population::ImageWrite)];
    Require(classAfter[static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectChecks)] == classBefore[static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectChecks)] + 2u, "dispatch gather images: multiunit writers lack two publication proofs");
}

void WriterEpochPublication(const GuestBlock& guest, bool suppressed) {
    namespace Epochs = AgcDriver::Graphics::ArchC;
    Require(Epochs::ImageEpochs::Active(), "dispatch gather images: epoch fixture did not enable the existing mechanism");
    WriterRepeats(guest, false);
    struct Witnesses {
        std::atomic<std::uint64_t> pending{1};
        std::atomic<std::uint64_t> inserted{1};
        std::uint64_t address;
        explicit Witnesses(std::uint64_t begin) : address(begin) {
            const std::array<Epochs::EpochRange, 1> ranges{{{begin, begin + Piece}}};
            Epochs::ImageEpochs::Get().RegisterOwner(&pending, pending, ranges, Epochs::SourceBit(Epochs::EpochSource::Pending));
            Epochs::ImageEpochs::Get().RegisterOwner(&inserted, inserted, ranges, Epochs::SourceBit(Epochs::EpochSource::PendingInsert));
        }
        ~Witnesses() {
            Epochs::ImageEpochs::SetSuppressed(Epochs::EpochSource::Pending, false);
            try {
                AgcDriverWaitIdle_nid_postfix();
                AgcDriver::GuestMemory::FlushGpuWrites(address, Piece);
            } catch (...) {}
            Epochs::ImageEpochs::Get().UnregisterOwner(&pending);
            Epochs::ImageEpochs::Get().UnregisterOwner(&inserted);
        }
    } witnesses(Address(guest.At(0)));
    auto words = UserData(guest, false);
    words[12] = 107u;
    std::vector<std::uint32_t> commands;
    Dispatch(commands, StoreCode.data(), words);
    if (!suppressed) {
        words[12] = 109u;
        Dispatch(commands, StoreCode.data(), words);
    }
    const auto pendingBefore = witnesses.pending.load(std::memory_order_acquire);
    const auto insertedBefore = witnesses.inserted.load(std::memory_order_acquire);
    const auto before = Shadow::ReadCounters();
    Epochs::ImageEpochs::SetSuppressed(Epochs::EpochSource::Pending, suppressed);
    Submit(std::move(commands));
    Epochs::ImageEpochs::SetSuppressed(Epochs::EpochSource::Pending, false);
    const auto pendingAfter = witnesses.pending.load(std::memory_order_acquire);
    const auto insertedAfter = witnesses.inserted.load(std::memory_order_acquire);
    const auto after = Shadow::ReadCounters();
    Expect(guest.At(0), suppressed ? 107u : 109u, "writer epoch publication");
    PrintDelta(suppressed ? "suppressed pending publication" : "fresh and already-dirty epoch publication", before, after);
    Require(insertedAfter == insertedBefore + 1u, "dispatch gather images: fresh writer did not insert exactly once into the pending registry");
    Require(pendingAfter == pendingBefore + (suppressed ? 0u : 1u), "dispatch gather images: pending epoch notification disagreed with fresh/already-dirty publication policy");
    const auto& classBefore = before.populations[static_cast<std::size_t>(Shadow::Population::ImageWrite)];
    const auto& classAfter = after.populations[static_cast<std::size_t>(Shadow::Population::ImageWrite)];
    const auto effectsBefore = classBefore[static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectChecks)];
    const auto effectsAfter = classAfter[static_cast<std::size_t>(Shadow::PopulationStage::WriterEffectChecks)];
    if (suppressed) {
        Require(after.commitRefused > before.commitRefused && after.commitRefusals[17] > before.commitRefusals[17] && after.commitChecks == before.commitChecks && effectsAfter == effectsBefore, "dispatch gather images: suppressed publication retained verified writer credit or lacked its epoch suppression refusal");
    } else {
        Require(after.commitChecks == before.commitChecks + 2u && effectsAfter == effectsBefore + 2u, "dispatch gather images: fresh and already-dirty writes lacked actual publication proofs");
    }
}

void MergedControl(const GuestBlock& guest, bool undecidable) {
    auto* table = guest.At(3);
    const auto descriptor = [](std::uint32_t* target) {
        const auto address = Address(target);
        return std::array<std::uint32_t, 4>{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), 256u, 0x31016facu};
    };
    const auto output = descriptor(guest.At(2));
    std::copy(output.begin(), output.end(), table);
    if (undecidable) {
        const auto first = descriptor(guest.At(0));
        const auto second = descriptor(guest.At(1));
        const auto selector = descriptor(guest.At(0) + 1);
        std::copy(first.begin(), first.end(), table + 4);
        std::copy(second.begin(), second.end(), table + 8);
        std::copy(selector.begin(), selector.end(), table + 12);
        guest.At(0)[0] = 37u;
        guest.At(0)[1] = 0u;
        guest.At(1)[0] = 59u;
        Register(UndecidableResourceHeader, UndecidableResourceCode);
    } else {
        table[8] = 2u;
        table[9] = 1u;
        table[10] = 5u;
        Register(ItemFreeHeader, ItemFreeCode);
    }
    const auto address = Address(table);
    const std::array<std::uint32_t, 2> words{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)};
    const auto* code = undecidable ? UndecidableResourceCode.data() : ItemFreeCode.data();
    const auto before = Shadow::ReadCounters();
    Submit(code, words);
    Expect(guest.At(2), undecidable ? 37u : 22u, "merged control warmup");
    const auto warm = Shadow::ReadCounters();
    Submit(code, words);
    Expect(guest.At(2), undecidable ? 37u : 22u, "merged control repeat");
    if (undecidable) guest.At(0)[1] = 1u;
    else table[8] = 6u;
    Submit(code, words);
    Expect(guest.At(2), undecidable ? 59u : 56u, "merged control changed branch");
    const auto after = Shadow::ReadCounters();
    PrintDelta(undecidable ? "resource-affecting undecidable control" : "item-free merged control", before, after);
    if (undecidable) {
        const auto layout = static_cast<std::size_t>(DispatchGatherRefusal::UnsupportedGraph);
        Require(after.graphRefusals[layout] == before.graphRefusals[layout] + 3 && after.matched == before.matched && after.gathered == before.gathered && after.commitChecks == before.commitChecks, "dispatch gather images: resource-affecting removed condition was admitted");
    } else {
        Require(warm.templatesBuilt > before.templatesBuilt && after.gathered >= warm.gathered + 1 && after.materializedChecks >= warm.materializedChecks + 1 && after.commitChecks >= warm.commitChecks + 1, "dispatch gather images: item-free merged control did not pass the real entry proofs");
        const auto& counts = after.populations[static_cast<std::size_t>(Shadow::Population::MergedControl)];
        const auto& prior = before.populations[static_cast<std::size_t>(Shadow::Population::MergedControl)];
        Require(counts[static_cast<std::size_t>(Shadow::PopulationStage::Gathered)] > prior[static_cast<std::size_t>(Shadow::PopulationStage::Gathered)], "dispatch gather images: merged-control gathering was not counted");
    }
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
                else if (mode == "--merged-control" || mode == "--resource-control") MergedControl(guest, mode == "--resource-control");
                else if (mode == "--writer-mips" || mode == "--writer-array" || mode == "--writer-array-base") WriterSubresources(guest, mode != "--writer-mips", mode == "--writer-array-base" ? 1u : 0u);
                else if (mode == "--writer-multiunit") WriterMultipleUnits(guest);
                else if (mode == "--writer-epochs" || mode == "--writer-epochs-suppressed") WriterEpochPublication(guest, mode == "--writer-epochs-suppressed");
                else if (mode == "--writer-mode") WriterModeRefusal(guest);
                else if (mode == "--writer-mutation") WriterDescriptorMutation(guest);
                else if (mode == "--writer-variants" || mode == "--writer-variants-full" || mode == "--writer-variants-batch") WriterVariants(guest, mode == "--writer-variants-full", mode == "--writer-variants-batch");
                else if (mode == "--writer" || mode == "--writer-sampled") WriterRepeats(guest, mode == "--writer-sampled");
                else if (mode == "--pending-writer" || mode == "--pending-writer-twice") PendingImageWriter(guest, mode == "--pending-writer-twice");
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
