#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "Optimization/SrtWalker.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libc/include/CriticalPathTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetTransfer.hpp"
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
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::DrawMiss;

alignas(256) constexpr std::array<std::uint32_t, 8> VertexCode{0x7e000208u, 0x7e020280u, 0x7e040280u, 0x7e0602f2u, 0xf80008cfu, 0x03020100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> SecondVertexCode = VertexCode;
alignas(256) constexpr std::array<std::uint32_t, 8> RecipeVertexCode = VertexCode;
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0xf4000100u, 0xfa000000u, 0x7e000204u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 16> TriangleVertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u, 0xf80008cfu, 0x04030201u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 16> ForwardedPixelCode{
    0xf4040080u, 0xfa000000u, 0xbf8cc07fu, 0xf4000101u, 0xfa000000u, 0xbf8cc07fu, 0x7e000204u, 0xd5480001u,
    0x02050800u, 0x7e020d01u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};
alignas(16) std::array<float, 4> Other{0.5f, 0.0f, 0.0f, 0.0f};
alignas(16) std::array<float, 4> ForwardedData{};
alignas(32) std::array<std::array<std::uint64_t, 4>, 6> AlignedTables{};
alignas(32) std::array<std::uint64_t, 4> OffsetTable{};
alignas(256) std::array<std::byte, 1024> ColorMemory{};
constexpr std::uint32_t LinearAttrib3 = 0x9000000u;
constexpr std::uint32_t RenderTargetAttrib3 = LinearAttrib3 | (0x1bu << 14u);
constexpr std::size_t TiledTargetBytes = 65536;

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};
alignas(8) Header SecondVertexHeader{};
alignas(8) Header RecipeVertexHeader{};
alignas(8) Header TriangleVertexHeader{};
alignas(8) Header ForwardedPixelHeader{};

const Shader* Registered(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
    return &header.shader;
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

std::vector<std::uint32_t> DrawFrom(std::initializer_list<const void*> sources, const std::uint32_t* vertexCode = VertexCode.data(), const std::uint32_t* pixelCode = PixelCode.data(), const void* target = ColorMemory.data(), std::uint32_t attrib3 = LinearAttrib3) {
    const auto color = reinterpret_cast<std::uintptr_t>(target);
    const auto vertex = reinterpret_cast<std::uintptr_t>(vertexCode);
    const auto pixel = reinterpret_cast<std::uintptr_t>(pixelCode);
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, attrib3}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
    };
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 1u << 1u}, {0x8c, 0},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 2u << 1u},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    for (const auto* source : sources) {
        const auto address = reinterpret_cast<std::uintptr_t>(source);
        Append(commands, 0x76, {0x00c, static_cast<std::uint32_t>(address)});
        Append(commands, 0x76, {0x00d, static_cast<std::uint32_t>(address >> 32u)});
        Append(commands, 0x2d, {3, 2});
    }
    return commands;
}

void MovedDataHitsTheCache(bool libraries) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const auto draw = [&](std::initializer_list<const void*> sources) {
        auto commands = DrawFrom(sources);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    static std::array<std::array<float, 4>, 8> same{};
    for (auto& entry : same) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (std::size_t i = 0; i < 4; ++i) counters = draw({same[i].data()});
    Require(counters.lookups == 4 && counters.hits == 0 && counters.inserts == 2, mode + ": the draws that teach the moved pointer and its contents made " + std::to_string(counters.hits) + " hits and " + std::to_string(counters.inserts) + " inserts, expected 0 and 2");
    for (std::size_t i = 4; i < same.size(); ++i) counters = draw({same[i].data()});
    Require(counters.lookups == 8 && counters.hits == 4 && counters.inserts == 2, mode + ": draws whose data moved again did not reuse the cached draw (" + std::to_string(counters.hits) + " hits in " + std::to_string(counters.lookups) + " lookups)");
    const auto other = draw({Other.data()});
    Require(other.lookups == 9 && other.hits == 4, mode + ": a draw whose moved data holds another value reused the cached draw");
    const auto again = draw({same[0].data()});
    Require(again.lookups == 10 && again.hits == 5, mode + ": a draw at an address seen before did not reuse the cached draw (" + std::to_string(again.hits) + " hits)");
}

void EvictedEntriesServeNoRelocation(bool libraries) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    Registered(SecondVertexHeader, SecondVertexCode.data(), sizeof(SecondVertexCode), 2);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const auto draw = [&](const float* source, const std::uint32_t* vertexCode) {
        auto commands = DrawFrom({source}, vertexCode);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    static std::array<std::array<float, 4>, 6> same{};
    for (auto& entry : same) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    alignas(16) static std::array<std::array<float, 4>, 3> others{};
    for (std::size_t i = 0; i < others.size(); ++i) others[i] = {0.25f + 0.03125f * static_cast<float>(i), 0.0f, 0.0f, 0.0f};
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (std::size_t i = 0; i < 4; ++i) counters = draw(same[i].data(), VertexCode.data());
    Require(counters.lookups == 4 && counters.hits == 0 && counters.inserts == 2, mode + ": the draws that teach the moved pointer and its contents made " + std::to_string(counters.hits) + " hits and " + std::to_string(counters.inserts) + " inserts, expected 0 and 2");
    const auto relocated = draw(same[4].data(), VertexCode.data());
    Require(relocated.hits == 1, mode + ": a draw whose data moved after the teaching draws did not reuse the cached draw (" + std::to_string(relocated.hits) + " hits)");
    auto evicted = relocated;
    for (const auto& entry : others) {
        draw(entry.data(), SecondVertexCode.data());
        evicted = draw(entry.data(), SecondVertexCode.data());
    }
    Require(evicted.evictions - relocated.evictions >= 2 && evicted.inserts - relocated.inserts >= 2, mode + ": draws of another vertex shader did not evict the two taught entries from the draw cache (" + std::to_string(evicted.inserts - relocated.inserts) + " inserts, " + std::to_string(evicted.evictions - relocated.evictions) + " evictions; run with APS5_DRAW_CACHE_ENTRIES=2)");
    const auto after = draw(same[5].data(), VertexCode.data());
    Require(after.lookups == evicted.lookups + 1 && after.hits == evicted.hits, mode + ": a draw whose data moved reused a draw the cache had evicted, through its relocation alias (" + std::to_string(after.hits - evicted.hits) + " hits)");
}

void RelocatingDrawsKeepTheirDonor(bool libraries) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    Registered(SecondVertexHeader, SecondVertexCode.data(), sizeof(SecondVertexCode), 2);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const auto draw = [&](const float* source, const std::uint32_t* vertexCode) {
        auto commands = DrawFrom({source}, vertexCode);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    constexpr std::size_t Rounds = 3;
    alignas(16) static std::array<std::array<float, 4>, Rounds> others{};
    for (std::size_t i = 0; i < others.size(); ++i) others[i] = {0.25f + 0.03125f * static_cast<float>(i), 0.0f, 0.0f, 0.0f};
    static std::array<std::array<float, 4>, 5 + Rounds> same{};
    for (auto& entry : same) entry = {1.0f, 0.0f, 0.0f, 0.0f};
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (std::size_t i = 0; i < 4; ++i) counters = draw(same[i].data(), VertexCode.data());
    Require(counters.lookups == 4 && counters.hits == 0 && counters.inserts == 2, mode + ": the draws that teach the moved pointer and its contents made " + std::to_string(counters.hits) + " hits and " + std::to_string(counters.inserts) + " inserts, expected 0 and 2");
    const auto start = draw(same[4].data(), VertexCode.data());
    Require(start.hits == 1, mode + ": a draw whose data moved after the teaching draws did not reuse the cached draw (" + std::to_string(start.hits) + " hits)");
    auto relocated = start;
    for (std::size_t i = 0; i < others.size(); ++i) {
        draw(others[i].data(), SecondVertexCode.data());
        const auto inserted = draw(others[i].data(), SecondVertexCode.data());
        relocated = draw(same[5 + i].data(), VertexCode.data());
        Require(relocated.hits == inserted.hits + 1, mode + ": a draw whose data moved again lost the cached draw it reused before each of " + std::to_string(i + 1) + " rounds of another vertex shader's draws (" + std::to_string(inserted.inserts - start.inserts) + " inserts, " + std::to_string(inserted.evictions - start.evictions) + " evictions)");
    }
    Require(relocated.evictions - start.evictions >= 2, mode + ": draws of another vertex shader evicted " + std::to_string(relocated.evictions - start.evictions) + " entries, expected at least 2 (run with APS5_DRAW_CACHE_ENTRIES=2)");
}

void SplitRunsNameTheirSharedWords() {
    alignas(16) static std::array<std::uint32_t, 4> data{1u, 2u, 3u, 4u};
    auto table = std::make_shared<ShaderRecompiler::SrtGuestWords>();
    table->words = {5u, 6u, 7u, 8u};
    const auto dataAddress = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data.data()));
    const auto tableAddress = dataAddress + 0x1000u;
    const std::array<ShaderRecompiler::MemoryRegion, 2> regions{{{dataAddress, std::as_bytes(std::span(data))}, {tableAddress, std::as_bytes(std::span(table->words))}}};
    ShaderRecompiler::SrtUserDataUse use;
    use.asBase = 0x3u;
    use.reads = {{dataAddress, 0}, {dataAddress + 8u, 1}};
    const std::array<std::shared_ptr<const ShaderRecompiler::SrtGuestWords>, 1> supplied{table};
    AgcDriver::DriverDetail::DispatchVariant variant;
    AgcDriver::DriverDetail::Driver::tagVariantRuns(variant, regions, use, true, 0, supplied);
    Require(variant.runs.size() > regions.size(), "two user-data bases into one region did not split it into several runs");
    Require(variant.shared.size() == 1u && variant.shared.front().regions == 1u, "the table region's words were not recorded as one shared run");
    const auto& shared = variant.shared.front();
    std::size_t first = 0;
    for (std::size_t run = 0; run < shared.region && run < variant.runs.size(); ++run) first += static_cast<std::size_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t));
    Require(shared.region < variant.runs.size() && variant.runs[shared.region].first == tableAddress && first == shared.first, "the shared words of a variant whose runs were split for relocation name run " + std::to_string(shared.region) + " of " + std::to_string(variant.runs.size()) + ", not the table's run " + std::to_string(variant.runs.size() - 1u));

    const std::array<std::uint32_t, 4> pointers{0x2000u, 0u, 0x3000u, 0u};
    const std::array<std::uint32_t, 4> child{0x4000u, 0u, 1u, 2u};
    const std::array<ShaderRecompiler::MemoryRegion, 4> nestedRegions{{{0x1000u, std::as_bytes(std::span(pointers))}, {0x2010u, std::as_bytes(std::span(child))}, {0x3010u, std::as_bytes(std::span(data))}, {0x4010u, std::as_bytes(std::span(data))}}};
    ShaderRecompiler::SrtUserDataUse nestedUse;
    nestedUse.asBase = 1u;
    for (std::uint64_t offset = 0; offset < 16u; offset += 4u) {
        nestedUse.reads.emplace_back(0x1000u + offset, 0);
        nestedUse.reads.emplace_back(0x2010u + offset, -1);
        nestedUse.reads.emplace_back(0x3010u + offset, -1);
        nestedUse.reads.emplace_back(0x4010u + offset, -1);
        nestedUse.nested.emplace_back(0x2010u + offset, 0x1000u);
        nestedUse.nested.emplace_back(0x3010u + offset, offset < 8u ? 0x1000u : 0x1008u);
        nestedUse.nested.emplace_back(0x4010u + offset, 0x2010u);
    }
    AgcDriver::DriverDetail::DispatchVariant nested;
    AgcDriver::DriverDetail::Driver::tagVariantRuns(nested, nestedRegions, nestedUse, true, 0);
    Require(nested.runParents.size() == nested.runs.size() && nested.runBases.size() == nested.runs.size(), "nested relocation: provenance length differs from the run list");
    Require(nested.runParents[1] == AgcDriver::DriverDetail::DispatchVariant::RunParent{0u, 0u} && nested.runParents[3] == AgcDriver::DriverDetail::DispatchVariant::RunParent{1u, 0u}, "nested relocation: child or grandchild lost its supplying pointer position");
    Require(nested.runParents[2].run == AgcDriver::DriverDetail::DispatchVariant::NoFlatBinding, "nested relocation: a run read through two pointers acquired a parent");

    alignas(16) static std::array<std::array<std::uint32_t, 4>, 2> descriptors{{{0x123400u, 0x000c0002u, 16u, 0x4a3acu}, {0x567800u, 0x000c0003u, 32u, 0x4a3acu}}};
    alignas(16) static std::array<std::uint64_t, 2> roots{reinterpret_cast<std::uintptr_t>(descriptors[0].data()), reinterpret_cast<std::uintptr_t>(descriptors[1].data())};
    const auto root = reinterpret_cast<std::uintptr_t>(&roots[0]);
    const auto leaf = roots[0];
    const std::array<ShaderRecompiler::MemoryRegion, 2> aliasRegions{{{root, std::as_bytes(std::span(&roots[0], 1))}, {leaf, std::as_bytes(std::span(descriptors[0]))}}};
    ShaderRecompiler::SrtUserDataUse aliasUse;
    aliasUse.asBase = 1u;
    aliasUse.reads = {{root, 0}, {root + 4u, 0}};
    for (std::uint64_t offset = 0; offset < 16u; offset += 4u) {
        aliasUse.reads.emplace_back(leaf + offset, -1);
        aliasUse.nested.emplace_back(leaf + offset, root);
    }
    auto aliasVariant = std::make_shared<AgcDriver::DriverDetail::DispatchVariant>();
    AgcDriver::DriverDetail::Driver::tagVariantRuns(*aliasVariant, aliasRegions, aliasUse, true, 0);
    aliasVariant->bufferBaseMasks = {{2u, 0xfffffffcu}, {3u, 0xffffu}};
    aliasVariant->dataPositions = {4u};
    AgcDriver::DriverDetail::RelocatableUserWord word{0u, {}};
    word.nestedVariant = aliasVariant;
    word.nestedBase = static_cast<std::uint32_t>(root) & ~3u;
    bool readable = true;
    const auto originalAlias = AgcDriver::DriverDetail::Driver::nestedContentKey(word, static_cast<std::uint32_t>(root), readable);
    const auto movedAlias = AgcDriver::DriverDetail::Driver::nestedContentKey(word, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&roots[1])), readable);
    Require(readable && originalAlias == movedAlias, "nested census: moving both tables or changing masked descriptor words changed the alias");
    descriptors[1][3] ^= 1u;
    const auto materialAlias = AgcDriver::DriverDetail::Driver::nestedContentKey(word, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&roots[1])), readable);
    Require(readable && originalAlias != materialAlias, "nested census: different second-level material content shared an alias");
}

class TiledTarget {
public:
    TiledTarget() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(TiledTargetBytes, TiledTargetBytes));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, TiledTargetBytes, PAGE_READWRITE, TiledTargetBytes);
#else
        void* mapped = mmap(nullptr, TiledTargetBytes * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + TiledTargetBytes - 1) & ~(static_cast<std::uintptr_t>(TiledTargetBytes) - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned != begin + TiledTargetBytes) munmap(reinterpret_cast<void*>(aligned + TiledTargetBytes), begin + TiledTargetBytes - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, TiledTargetBytes);
        }
#endif
        Require(block != nullptr, "cannot allocate the tiled color target");
        std::memset(block, 0, TiledTargetBytes);
        GuestAllocations::Mutation().Add(block, TiledTargetBytes, true, true, true);
    }

    TiledTarget(const TiledTarget&) = delete;
    TiledTarget& operator=(const TiledTarget&) = delete;

    const void* Data() const { return block; }

private:
    std::uint8_t* block = nullptr;
};

void RelocatedDrawsReuseTheirRecipe(bool libraries) {
    Registered(RecipeVertexHeader, RecipeVertexCode.data(), sizeof(RecipeVertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    static const TiledTarget target;
    const auto draw = [&](const float* source) {
        auto commands = DrawFrom({source}, RecipeVertexCode.data(), PixelCode.data(), target.Data(), RenderTargetAttrib3);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    alignas(16) static std::array<std::array<float, 4>, 10> moved{};
    for (auto& entry : moved) entry = {0.75f, 0.0f, 0.0f, 0.0f};
    for (std::size_t i = 0; i < 4; ++i) draw(moved[i].data());
    const auto start = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    const auto recipesBefore = AgcDriver::VulkanDevice::DrawRecipeHits();
    AgcDriver::DriverDetail::DrawEntryCounters counters = start;
    for (std::size_t i = 4; i < moved.size(); ++i) counters = draw(moved[i].data());
    const auto recipeHits = AgcDriver::VulkanDevice::DrawRecipeHits() - recipesBefore;
    const auto hits = counters.hits - start.hits;
    Require(hits == moved.size() - 4, mode + ": draws whose data moved again made " + std::to_string(hits) + " draw-cache hits, expected " + std::to_string(moved.size() - 4));
    Require(recipeHits == hits, mode + ": " + std::to_string(recipeHits) + " of " + std::to_string(hits) + " draws served by a relocated entry were recorded from the cached draw's recipe, expected all");
}

void UnrelocatedAliasesStayOut(bool libraries) {
    Registered(SecondVertexHeader, SecondVertexCode.data(), sizeof(SecondVertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const auto draw = [&](const float* source) {
        auto commands = DrawFrom({source}, SecondVertexCode.data());
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    alignas(16) static std::array<float, 4> repeated{1.0f, 0.0f, 0.0f, 0.0f};
    alignas(16) static std::array<std::array<std::array<float, 4>, 2>, 12> twice{};
    for (std::size_t i = 0; i < twice.size(); ++i) {
        for (auto& entry : twice[i]) entry = {0.25f + 0.03125f * static_cast<float>(i), 0.0f, 0.0f, 0.0f};
    }
    draw(repeated.data());
    const auto start = draw(repeated.data());
    AgcDriver::DriverDetail::DrawEntryCounters counters = start;
    for (const auto& pair : twice) {
        for (const auto& entry : pair) draw(entry.data());
        counters = draw(repeated.data());
    }
    const auto inserts = counters.inserts - start.inserts;
    Require(inserts <= AgcDriver::DriverDetail::DrawRelocationTrial::ProbationAdmissions, mode + ": draws whose alias repeats but never relocates were cached (" + std::to_string(inserts) + " inserts for " + std::to_string(twice.size()) + " aliases)");
    Require(counters.hits - start.hits == twice.size(), mode + ": the exact key of a repeated draw lost its hits next to draws whose alias never relocates (" + std::to_string(counters.hits - start.hits) + " hits in " + std::to_string(twice.size()) + " draws)");
}

void MovedForwardedWordReachesStageDataHit(bool libraries) {
    Registered(TriangleVertexHeader, TriangleVertexCode.data(), sizeof(TriangleVertexCode), 2);
    Registered(ForwardedPixelHeader, ForwardedPixelCode.data(), sizeof(ForwardedPixelCode), 1);
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    constexpr std::byte Kept{0x40};
    const auto draw = [&](const void* table) {
        ColorMemory.fill(Kept);
        auto commands = DrawFrom({table}, TriangleVertexCode.data(), ForwardedPixelCode.data());
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(ColorMemory.data()), ColorMemory.size());
        return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    };
    const auto pixels = [&](std::array<std::uint8_t, 4> expected) {
        std::size_t drawn = 0;
        for (std::size_t at = 0; at < ColorMemory.size(); at += 4) {
            const std::array<std::uint8_t, 4> pixel{static_cast<std::uint8_t>(ColorMemory[at]), static_cast<std::uint8_t>(ColorMemory[at + 1]), static_cast<std::uint8_t>(ColorMemory[at + 2]), static_cast<std::uint8_t>(ColorMemory[at + 3])};
            if (pixel == std::array<std::uint8_t, 4>{0x40, 0x40, 0x40, 0x40}) continue;
            if (pixel != expected) return "pixel " + std::to_string(at / 4) + " is " + std::to_string(pixel[0]) + "," + std::to_string(pixel[1]) + "," + std::to_string(pixel[2]) + "," + std::to_string(pixel[3]) + ", expected " + std::to_string(expected[0]) + "," + std::to_string(expected[1]) + "," + std::to_string(expected[2]) + "," + std::to_string(expected[3]);
            ++drawn;
        }
        return drawn == 0 ? std::string("the draw wrote no pixel") : std::string();
    };
    const auto data = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ForwardedData.data()));
    for (auto& table : AlignedTables) table[0] = data;
    OffsetTable[2] = data;
    ForwardedData = {1.0f, 0.0f, 0.0f, 0.0f};
    const auto start = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    AgcDriver::DriverDetail::DrawEntryCounters counters{};
    for (const auto& table : AlignedTables) counters = draw(table.data());
    Require(counters.hits != start.hits, mode + ": draws whose pointer table moved to identical contents never reused the cached draw (" + std::to_string(counters.lookups - start.lookups) + " lookups, " + std::to_string(counters.inserts - start.inserts) + " inserts)");
    const auto taught = pixels({255, 0, 0, 255});
    Require(taught.empty(), mode + ": a draw through a pointer table with bit 4 of its address clear: " + taught);
    ForwardedData[0] = 0.25f;
    const auto before = counters;
    counters = draw(&OffsetTable[2]);
    Require(counters.lookups == before.lookups + 1 && counters.hits == before.hits + 1 && counters.stageDataHits == before.stageDataHits + 1, mode + ": a draw whose pointer table moved and whose data changed in place was not a relocated stage data hit (" + std::to_string(counters.hits - before.hits) + " hits, " + std::to_string(counters.stageDataHits - before.stageDataHits) + " stage data hits)");
    const auto moved = pixels({64, 255, 255, 64});
    Require(moved.empty(), mode + ": a relocated stage data hit did not draw with the moved user word: " + moved);
}

void LiveNestedDraws(const std::filesystem::path& control, unsigned directKind = 0, bool requireFallback = false) {
    using R = AgcDriver::ArchB::Recorder;
    using S = AgcDriver::ArchB::Switches;
    using F = AgcDriver::ArchB::Feature;
    alignas(256) static constexpr std::array<std::uint32_t, 16> code{
        0xf4040080u, 0xfa000000u, 0xbf8cc07fu, 0xf4080101u, 0xfa000000u, 0xbf8cc07fu,
        0xe0300000u, 0x80010000u, 0xbf8c3f70u, 0x7e020280u, 0xf800180fu, 0x00010100u,
        0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
    alignas(8) static Header header{};
    alignas(16) static std::array<std::array<std::uint32_t, 4>, 2> descriptors{};
    alignas(16) static std::array<std::uint64_t, 2> root{};
    static const TiledTarget target;
    static const TiledTarget data;
    static const TiledTarget arguments;
    static const TiledTarget indices;
    static const TiledTarget movedIndices;
    static const TiledTarget otherTarget;
    static const TiledTarget firstDepth;
    static const TiledTarget secondDepth;
    Registered(TriangleVertexHeader, TriangleVertexCode.data(), sizeof(TriangleVertexCode), 2);
    Registered(header, code.data(), sizeof(code), 1);
    const auto dataAddress = reinterpret_cast<std::uintptr_t>(data.Data());
    const auto args = reinterpret_cast<std::uintptr_t>(arguments.Data());
    const auto originalIndexAddress = reinterpret_cast<std::uintptr_t>(indices.Data());
    auto indexAddress = originalIndexAddress;
    const std::array<std::uint32_t, 6> indexValues{0, 1, 2, 0, 1, 2};
    AgcDriver::GuestMemory::Write(indexAddress, std::as_bytes(std::span(indexValues)));
    const std::array<std::uint32_t, 4> record{3, 1, 0, 0};
    AgcDriver::GuestMemory::Write(args, std::as_bytes(std::span(record)));
    const auto value = [&](std::uint64_t address, float number) { AgcDriver::GuestMemory::Write(address, std::as_bytes(std::span(&number, 1))); };
    value(dataAddress, 0.25f);
    value(dataAddress + 256, 0.75f);
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        const auto address = dataAddress + i * 256;
        descriptors[i] = {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32), 64, 0x30016facu};
    }
    root[0] = reinterpret_cast<std::uintptr_t>(descriptors[0].data());
    unsigned sequence = 0;
    double fallbackCount = 0;
    double fallbackVerifyCount = 0;
    const auto collect = [&] {
        auto sample = R::Collect(R::Clock::now());
        for (const auto& [name, count] : sample.counters) {
            if (name == "draw_templates_live_commit_fallback_direct") fallbackCount += count;
            if (name == "draw_templates_live_verify_fallback_checks") fallbackVerifyCount += count;
        }
        return sample;
    };
    const auto enable = [&](bool on, bool templates = true, bool replay = true) {
        std::ofstream(control, std::ios::trunc) << ++sequence << " DRAW_TEMPLATES_LIVE=" << on << " DRAW_TEMPLATES=" << templates << " C_DRAW_REPLAY=" << replay << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        S::PollControl();
        Require(S::Enabled(F::DrawTemplatesLive) == on, "the live switch did not apply");
        R::Boundary(true, sequence, R::Clock::now());
    };
    const auto draw = [&](const void* destination = nullptr, std::uint64_t depth = 0, std::uint32_t count = 3, std::uint32_t firstVertex = 0) {
        auto commands = DrawFrom({root.data()}, TriangleVertexCode.data(), code.data(), destination != nullptr ? destination : target.Data(), RenderTargetAttrib3);
        commands.resize(commands.size() - 3);
        Append(commands, 0x69, {0x000, 0});
        Append(commands, 0x69, {0x002, 0x01000000});
        Append(commands, 0x69, {0x007, 63u | (63u << 16u)});
        Append(commands, 0x69, {0x00b, std::bit_cast<std::uint32_t>(1.0f)});
        Append(commands, 0x69, {0x010, depth != 0 ? 3u | (24u << 4u) | (1u << 20u) : 0u});
        Append(commands, 0x69, {0x011, 0});
        Append(commands, 0x69, {0x012, static_cast<std::uint32_t>(depth >> 8u)});
        Append(commands, 0x69, {0x014, static_cast<std::uint32_t>(depth >> 8u)});
        Append(commands, 0x69, {0x01a, static_cast<std::uint32_t>(depth >> 40u)});
        Append(commands, 0x69, {0x01c, static_cast<std::uint32_t>(depth >> 40u)});
        Append(commands, 0x69, {0x200, depth != 0 ? 0x72u : 0u});
        if (directKind == 0) {
            Append(commands, 0x11, {1, static_cast<std::uint32_t>(args), static_cast<std::uint32_t>(args >> 32)});
            Append(commands, 0x24, {0, 0x8c, 0, 2});
        } else {
            Append(commands, 0x79, {0x24a, firstVertex});
            Append(commands, 0x2f, {directKind >= 3 ? 2u : 1u});
            if (directKind == 2 || directKind == 4) {
                Append(commands, 0x2a, {1});
                Append(commands, 0x27, {6, static_cast<std::uint32_t>(indexAddress), static_cast<std::uint32_t>(indexAddress >> 32), count, 0});
            } else Append(commands, 0x2d, {count, 2});
        }
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the live nested draw failed");
        AgcDriverWaitIdle_nid_postfix();
        AgcDriver::GuestMemory::WtFaultFlip();
    };
    const AgcDriver::Graphics::ColorTarget color{reinterpret_cast<std::uintptr_t>(target.Data()), {64, 64}, VK_FORMAT_R8G8B8A8_UNORM, TiledTargetBytes, 0xe4, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4};
    const auto pixels = [&] {
        std::vector<std::byte> image(64 * 64 * 4);
        AgcDriver::Graphics::ReadColorTarget(color, image);
        Require(std::any_of(image.begin(), image.end(), [](auto byte) { return byte != std::byte{0}; }), "the live nested draw wrote no pixels");
        return image;
    };
    const bool directDisabled = directKind != 0 && std::getenv("APS5_ARCHB_DRAW_DIRECT") != nullptr && std::string(std::getenv("APS5_ARCHB_DRAW_DIRECT")) == "0";
    enable(false, directKind == 0, directKind == 0);
    const auto recipesBefore = AgcDriver::VulkanDevice::DrawRecipeHits();
    for (int repeat = 0; repeat < 10; ++repeat) draw();
    if (directKind != 0) Require(AgcDriver::VulkanDevice::DrawRecipeHits() > recipesBefore, "direct reference never warmed its recipe path");
    const auto first = pixels();
    if (directKind != 0) {
        enable(false, false, true);
        for (int repeat = 0; repeat < 12; ++repeat) draw();
        Require(pixels() == first, "direct replay changed exact pixels");
        const auto replay = collect().counters;
        const auto counter = [](const auto& values, const char* name) {
            const auto found = values.find(name);
            return found == values.end() ? 0.0 : found->second;
        };
        Require(directDisabled ? counter(replay, "archc_replay_hits_direct") == 0 : counter(replay, "archc_replay_hits_direct") > 0, "direct replay path count differs from extension switch");
        Require(directDisabled ? counter(replay, "archc_replay_captures_direct") == 0 : counter(replay, "archc_replay_captures_direct") > 0, "direct capture path count differs from extension switch");
        if (!directDisabled && std::getenv("APS5_ARCHB_C_DRAW_REPLAY_VERIFY") != nullptr && std::string(std::getenv("APS5_ARCHB_C_DRAW_REPLAY_VERIFY")) != "0") Require(counter(replay, "archc_replay_verify_checks_direct") > 0 && counter(replay, "archc_replay_direct_inputs_verified") > 0, "direct replay command/binding VERIFY did not run");
        Require(counter(replay, "archc_replay_verify_mismatches_direct") == 0, "direct replay VERIFY mismatch");
        for (const auto& [name, count] : replay) if (name.starts_with("archc_") && count != 0) std::cout << name << " " << count << "\n";
        enable(false, true, true);
        for (int repeat = 0; repeat < 12; ++repeat) {
            root[0] = reinterpret_cast<std::uintptr_t>(descriptors[repeat % 2].data());
            draw();
        }
        const auto templated = pixels();
        Require(templated != first, "direct template retained old moved values");
        const auto templates = collect().counters;
        Require(directDisabled ? counter(templates, "draw_templates_replays_direct") == 0 : requireFallback || counter(templates, "draw_templates_replays_direct") > 0, "direct template path count differs from extension switch");
        if (!directDisabled && !requireFallback && std::getenv("APS5_ARCHB_DRAW_TEMPLATES_VERIFY") != nullptr && std::string(std::getenv("APS5_ARCHB_DRAW_TEMPLATES_VERIFY")) != "0") Require(counter(templates, "draw_templates_verify_checks_direct") > 0 && counter(templates, "draw_templates_direct_inputs_verified") > 0, "direct template command/binding VERIFY did not run");
        for (const auto& [name, count] : templates) Require(name.find("mismatch") == std::string::npos || count == 0, "direct template mismatch counter is nonzero");
        for (const auto& [name, count] : templates) if ((name.starts_with("draw_templates") || name.starts_with("archc_")) && count != 0) std::cout << name << " " << count << "\n";
        enable(false, false, false);
        draw();
        Require(pixels() == templated, "direct template differs from exact old-path pixels");
        root[0] = reinterpret_cast<std::uintptr_t>(descriptors[0].data());
        enable(false);
        draw();
    }
    for (int repeat = 0; repeat < 6; ++repeat) draw();
    collect();
    enable(true);
    root[0] = reinterpret_cast<std::uintptr_t>(descriptors[1].data());
    const auto lookupsBefore = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters().lookups;
    for (int repeat = 0; repeat < 12; ++repeat) draw();
    const auto changed = pixels();
    Require(changed != first, "moving the nested V# and its value left the old pixels");
    const auto counts = collect().counters;
    for (const auto& [name, count] : counts) if (name.starts_with("draw_templates_live") || name.starts_with("archc_replay_refused") || name.starts_with("archc_replay_hits")) std::cout << name << " " << count << "\n";
    const auto taken = counts.find("draw_templates_live_q0_taken");
    const auto committed = counts.find("draw_templates_live_commits");
    if (directKind != 0 && !directDisabled) {
        const auto directTaken = counts.find("draw_templates_live_q0_taken_direct");
        const auto directCommitted = counts.find("draw_templates_live_commits_direct");
        Require(directTaken != counts.end() && directTaken->second > 0 && (requireFallback || (directCommitted != counts.end() && directCommitted->second > 0)), "direct live counters did not prove the path");
        if (const auto* verify = std::getenv("APS5_ARCHB_DRAW_TEMPLATES_LIVE_VERIFY"); !requireFallback && verify != nullptr && std::strtoull(verify, nullptr, 10) != 0) {
            const auto checks = counts.find("draw_templates_live_verify_checks_direct");
            Require(checks != counts.end() && checks->second > 0, "direct live VERIFY did not run");
        }
    }
    if (directDisabled) {
        Require((taken == counts.end() || taken->second == 0) && (committed == counts.end() || committed->second == 0), "disabled direct extension took a live draw");
    } else Require(taken != counts.end() && taken->second > 0 && (requireFallback || (committed != counts.end() && committed->second > 0)), "the nested draw did not take and commit a live plan");
    if (const auto* period = std::getenv("APS5_ARCHB_DRAW_TEMPLATES_LIVE_VERIFY"); !directDisabled && (period == nullptr || std::strtoull(period, nullptr, 10) == 0)) {
        const auto lookups = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters().lookups - lookupsBefore;
        Require(lookups + static_cast<std::uint64_t>(taken->second) == 12, "a live queue-0 draw also ran the old lookup");
    }
    for (const auto& [name, count] : counts) Require(!name.starts_with("draw_templates_live_verify_mismatch_") || count == 0, "the live draw differs from the old committed draw");
    enable(false);
    draw();
    Require(pixels() == changed, "the live nested draw's exact pixels differ from the old path");
    enable(true);
    if (directKind == 2 || directKind == 4) {
        const auto movedIndexAddress = reinterpret_cast<std::uintptr_t>(movedIndices.Data());
        AgcDriver::GuestMemory::Write(movedIndexAddress, std::as_bytes(std::span(indexValues)));
        const std::array<std::uint32_t, 6> staleIndices{};
        AgcDriver::GuestMemory::Write(originalIndexAddress, std::as_bytes(std::span(staleIndices)));
        indexAddress = movedIndexAddress;
        root[0] = reinterpret_cast<std::uintptr_t>(descriptors[0].data());
        collect();
        draw();
        const auto relocated = pixels();
        Require(relocated == first, "direct relocated index address retained stale index bytes");
        const auto relocationCounts = collect().counters;
        double relocatedPaths = 0;
        for (const auto& [name, count] : relocationCounts) {
            if (name == "draw_templates_live_q0_taken_direct" || name.starts_with("draw_templates_live_q0_refused_") || name.starts_with("draw_templates_live_q0_skip")) relocatedPaths += count;
            Require(name.find("mismatch") == std::string::npos || count == 0, "direct index relocation VERIFY mismatch");
        }
        if (!directDisabled) Require(relocatedPaths > 0, "direct index relocation had neither live admission nor explicit refusal");
        enable(false, false, false);
        draw();
        Require(pixels() == relocated, "direct relocated index address differs from exact old-path pixels");
        AgcDriver::GuestMemory::Write(originalIndexAddress, std::as_bytes(std::span(indexValues)));
        indexAddress = originalIndexAddress;
        root[0] = reinterpret_cast<std::uintptr_t>(descriptors[1].data());
        enable(true);
        for (int repeat = 0; repeat < 8; ++repeat) draw();
    }
    if (directKind != 0) {
        collect();
        draw(nullptr, 0, 6);
        const auto changedCount = collect().counters;
        const auto countTaken = changedCount.find("draw_templates_live_q0_taken");
        Require(countTaken == changedCount.end() || countTaken->second == 0, "changed direct count took the previous live packet");
        draw();
        collect();
        draw(nullptr, 0, 3, 1);
        const auto changedFirst = collect().counters;
        const auto firstTaken = changedFirst.find("draw_templates_live_q0_taken");
        Require(firstTaken == changedFirst.end() || firstTaken->second == 0, "changed direct first vertex took the previous live packet");
        for (int repeat = 0; repeat < 8; ++repeat) draw();
    }
    descriptors[1][2] = 32;
    collect();
    draw();
    const auto structural = collect().counters;
    const auto live = structural.find("draw_templates_live_q0_taken");
    Require(live == structural.end() || live->second == 0, "a changed V# record count took the old structural template");
    descriptors[1][2] = 64;
    for (int repeat = 0; repeat < 8; ++repeat) draw();
    collect();
    draw(otherTarget.Data());
    const auto retargeted = collect().counters;
    const auto targetTaken = retargeted.find("draw_templates_live_q0_taken");
    Require(targetTaken == retargeted.end() || targetTaken->second == 0, "a target change took the previous target's live template");
    const auto depth1 = reinterpret_cast<std::uintptr_t>(firstDepth.Data());
    const auto depth2 = reinterpret_cast<std::uintptr_t>(secondDepth.Data());
    for (int repeat = 0; repeat < 8; ++repeat) draw(nullptr, depth1);
    collect();
    draw(nullptr, depth2);
    const auto redepthed = collect().counters;
    const auto depthTaken = redepthed.find("draw_templates_live_q0_taken");
    Require(depthTaken == redepthed.end() || depthTaken->second == 0, "a depth change took the previous depth's live template");
    if (requireFallback) {
        Require(fallbackCount > 0 && fallbackVerifyCount > 0, "direct live fallback VERIFY was not exercised");
        std::cout << "direct live fallback commits " << fallbackCount << " verified " << fallbackVerifyCount << "\n";
    }
}

void NestedSpeculationRefusesProtectionChanges() {
    constexpr std::size_t bytes = 65536;
#ifdef _WIN32
    auto* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, bytes);
    Require(block != nullptr, "nested speculation: cannot allocate guest pages");
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    auto* block = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(block != MAP_FAILED, "nested speculation: cannot map pages");
#endif
    std::array<std::uint32_t, 4> content{1, 2, 3, 4};
    std::memcpy(block, content.data(), sizeof(content));
    const auto address = reinterpret_cast<std::uintptr_t>(block);
    std::uint64_t pointer = address;
    const auto root = reinterpret_cast<std::uintptr_t>(&pointer);
    auto variant = std::make_shared<AgcDriver::DriverDetail::DispatchVariant>();
    variant->runs = {{root, root + 8}, {address, address + sizeof(content)}};
    variant->runBases = {0, -1};
    variant->runParents = {{}, {0, 0}};
    variant->words.resize(6);
    std::memcpy(variant->words.data(), &pointer, sizeof(pointer));
    std::memcpy(variant->words.data() + 2, content.data(), sizeof(content));
    AgcDriver::DriverDetail::RelocatableUserWord word{0, {}};
    word.nestedVariant = variant;
    word.nestedBase = static_cast<std::uint32_t>(root) & ~3u;
    bool readable = true;
    static_cast<void>(AgcDriver::DriverDetail::Driver::nestedContentKey(word, static_cast<std::uint32_t>(root), readable));
    Require(readable, "nested speculation: the mapped child was refused");
    Require(AgcDriver::GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(content))) == AgcDriver::GuestMemory::Compare::Equal, "nested speculation: cannot prime the mapped-page cache");
#ifdef _WIN32
    DWORD previous = 0;
    Require(VirtualProtect(block, bytes, PAGE_NOACCESS, &previous) != 0, "nested speculation: cannot protect pages outside guest invalidation");
    const bool cachedReadable = AgcDriver::GuestMemory::Accessible(block, sizeof(content));
#else
    Require(mprotect(block, bytes, PROT_NONE) == 0, "nested speculation: cannot protect pages");
    const bool cachedReadable = true;
#endif
    const auto copied = AgcDriver::GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(content)), true);
    readable = true;
    static_cast<void>(AgcDriver::DriverDetail::Driver::nestedContentKey(word, static_cast<std::uint32_t>(root), readable));
#ifdef _WIN32
    DWORD ignored = 0;
    const bool restored = VirtualProtect(block, bytes, previous, &ignored) != 0;
    GuestArena::GuestArenaReset_nid_postfix(block, bytes);
    GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
#else
    const bool restored = munmap(block, bytes) == 0;
#endif
    Require(restored && cachedReadable, "nested speculation: the stale cached-permission case was not exercised or restored");
    Require(copied == AgcDriver::GuestMemory::Compare::Unmapped && !readable, "nested speculation: a protected child was read through stale cached page permissions");
}

void NestedRelocationPath(const std::filesystem::path& control) {
    using D = AgcDriver::DriverDetail::Driver;
    using S = AgcDriver::ArchB::Switches;
    using F = AgcDriver::ArchB::Feature;
    using R = AgcDriver::ArchB::Recorder;
    alignas(256) static constexpr std::array<std::uint32_t, 16> code{
        0xf4040080u, 0xfa000000u, 0xbf8cc07fu, 0xf4080101u, 0xfa000000u, 0xbf8cc07fu,
        0xe0300000u, 0x80010000u, 0xbf8c3f70u, 0x7e020280u, 0xf800180fu, 0x00010100u,
        0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
    alignas(8) static Header header{};
    alignas(256) static std::array<std::array<float, 64>, 12> data{};
    alignas(16) static std::array<std::array<std::uint32_t, 4>, 12> descriptors{};
    alignas(16) static std::array<std::array<std::uint64_t, 2>, 12> roots{};
    Registered(TriangleVertexHeader, TriangleVertexCode.data(), sizeof(TriangleVertexCode), 2);
    Registered(header, code.data(), sizeof(code), 1);
    for (std::size_t index = 0; index < roots.size(); ++index) {
        const auto address = reinterpret_cast<std::uintptr_t>(data[index].data());
        const bool second = index == 8 || index == 9 || index == 11;
        data[index][0] = second ? 1.0f : 0.25f;
        descriptors[index] = {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), second ? 32u : 64u, 0x30016facu};
        roots[index][0] = reinterpret_cast<std::uintptr_t>(descriptors[index].data());
    }
    unsigned sequence = 0;
    const auto enable = [&](bool enabled) {
        std::ofstream(control, std::ios::trunc) << ++sequence << " RECORDER=1 FIX_DRAW_RELOC_NESTED=" << enabled << " FIX_BUFFER_BASE_REFRESH=1\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        S::PollControl();
        Require(S::Enabled(F::FixDrawRelocNested) == enabled, "nested relocation: runtime switch did not apply");
        R::Boundary(true, sequence, R::Clock::now());
    };
    const auto draw = [&](std::size_t index) {
        ColorMemory.fill(std::byte{0x40});
        auto commands = DrawFrom({roots[index].data()}, TriangleVertexCode.data(), code.data());
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "nested relocation: submission failed");
        AgcDriverWaitIdle_nid_postfix();
        AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(ColorMemory.data()), ColorMemory.size());
        return D::Get().DrawCacheCounters();
    };
    const auto requirePixels = [&](std::uint8_t red) {
        std::size_t written = 0;
        for (std::size_t offset = 0; offset < ColorMemory.size(); offset += 4) {
            if (ColorMemory[offset + 1] == std::byte{0x40}) continue;
            Require(ColorMemory[offset] == static_cast<std::byte>(red) && ColorMemory[offset + 1] == std::byte{0} && ColorMemory[offset + 2] == std::byte{0} && ColorMemory[offset + 3] == static_cast<std::byte>(red), "nested relocation: the rendered buffer value was stale");
            ++written;
        }
        Require(written != 0, "nested relocation: the draw wrote no pixels");
    };
    enable(true);
    const auto before = D::Get().DrawCacheCounters();
    for (std::size_t index = 0; index < 6; ++index) draw(index);
    for (std::size_t index = 0; index < 6; ++index) data[index][0] = 0.0f;
    const auto taught = D::Get().DrawCacheCounters();
    const auto relocated = draw(6);
    Require(relocated.hits == taught.hits + 1 && relocated.hits > before.hits, "nested relocation: moving both tables did not hit the draw cache");
    requirePixels(64);
    const auto image = ColorMemory;
    const auto counts = R::Collect(R::Clock::now()).counters;
    const auto hits = counts.find("fix_draw_reloc_nested_hits");
    Require(hits != counts.end() && hits->second > 0, "nested relocation: no nested-relocated hit was exercised");
    if (std::getenv("APS5_ARCHB_FIX_DRAW_RELOC_NESTED_VERIFY") != nullptr) {
        const auto checked = counts.find("fix_draw_reloc_nested_verified");
        const auto mismatches = counts.find("fix_draw_reloc_nested_verify_mismatches");
        Require(checked != counts.end() && checked->second == hits->second && (mismatches == counts.end() || mismatches->second == 0), "nested relocation: full capture disagreed with the relocated hit");
    }
    enable(false);
    const auto fresh = draw(7);
    Require(fresh.hits == relocated.hits, "nested relocation: the disabled switch accepted a nested relocation");
    requirePixels(64);
    Require(image == ColorMemory, "nested relocation: the relocated image differs from the full-capture image with the switch off");
    enable(true);
    const auto changed = draw(8);
    Require(changed.hits == fresh.hits, "nested relocation: an unmasked child descriptor word change hit the old material");
    draw(8);
    draw(8);
    const auto materialBefore = D::Get().DrawCacheCounters();
    const auto second = draw(9);
    Require(second.hits == materialBefore.hits + 1, "nested relocation: the second material did not retain its own alias");
    requirePixels(255);
    const auto first = draw(10);
    Require(first.hits == second.hits + 1, "nested relocation: the first material lost its alias to the second material");
    requirePixels(64);
    Require(image == ColorMemory, "nested relocation: materials shared a cross hit");
    const auto again = draw(11);
    Require(again.hits == first.hits + 1, "nested relocation: alternating materials did not retain two aliases");
    requirePixels(255);
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--nested-copy") {
            NestedSpeculationRefusesProtectionChanges();
            return 0;
        }
        if (argc == 2 && (std::string(argv[1]) == "--nested" || std::string(argv[1]) == "--nested-pixels" || std::string(argv[1]) == "--live" || std::string(argv[1]).starts_with("--direct-"))) {
            struct Control {
                std::filesystem::path path = std::filesystem::temp_directory_path() / ("agc-nested-relocation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
                ~Control() { std::filesystem::remove(path); }
            } control;
#ifdef _WIN32
            _putenv_s("APS5_ARCHB_GATE_RUN", "1");
            _putenv_s("APS5_ARCHB_CONTROL_FILE", control.path.string().c_str());
#else
            setenv("APS5_ARCHB_GATE_RUN", "1", 1);
            setenv("APS5_ARCHB_CONTROL_FILE", control.path.string().c_str(), 1);
#endif
            const auto probe = OpenVulkanTestDevice();
            if (!probe) return VulkanTestSkipped;
            const bool traceLive = std::string(argv[1]) == "--live" && CriticalPath::Enabled();
            if (traceLive) {
                CriticalPath::Name("j11 live draw fixture");
                CriticalPath::Present(0);
            }
            if (std::string(argv[1]).starts_with("--direct-")) {
                std::string kind = argv[1];
                const bool requireFallback = kind.ends_with("-fallback");
                if (requireFallback) kind.resize(kind.size() - std::string_view("-fallback").size());
                const unsigned directKind = kind == "--direct-auto" ? 1u : kind == "--direct-indexed" ? 2u : kind == "--direct-instanced" ? 3u : kind == "--direct-indexed-instanced" ? 4u : 0u;
                Require(directKind != 0, "unknown direct draw case");
                LiveNestedDraws(control.path, directKind, requireFallback);
            } else if (std::string(argv[1]) == "--live") LiveNestedDraws(control.path);
            else NestedRelocationPath(control.path);
            AgcDriverShutdown_nid_postfix();
            if (traceLive) {
                CriticalPath::Present(1);
                CriticalPath::Shutdown();
            }
            std::cout << "Nested table relocation preserves material aliases, buffer bindings and rendered pixels\n";
            return 0;
        }
        const bool evicted = argc == 2 && std::string(argv[1]) == "--evicted";
        const bool kept = argc == 2 && std::string(argv[1]) == "--kept";
        if (!evicted && !kept) SplitRunsNameTheirSharedWords();
        bool libraries = false;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            libraries = device->GraphicsPipelineLibraries();
        }
        if (!libraries && std::getenv("APS5_NO_GPL") == nullptr) return VulkanTestSkipped;
        if (evicted) {
            EvictedEntriesServeNoRelocation(libraries);
            AgcDriverShutdown_nid_postfix();
            std::cout << "An evicted draw served no relocation " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
            return 0;
        }
        if (kept) {
            RelocatingDrawsKeepTheirDonor(libraries);
            AgcDriverShutdown_nid_postfix();
            std::cout << "A draw relocated from a cached draw kept it cached " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
            return 0;
        }
        MovedDataHitsTheCache(libraries);
        RelocatedDrawsReuseTheirRecipe(libraries);
        UnrelocatedAliasesStayOut(libraries);
        MovedForwardedWordReachesStageDataHit(libraries);
        if (std::getenv("APS5_VERIFY_DRAW_RECIPE") != nullptr) {
            const auto counters = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
            Require(counters.verifyDecodeMismatches == 0, "the effective relocated draw decode differed from its full decode");
        }
        AgcDriverShutdown_nid_postfix();
        std::cout << "A draw whose data moved reused the cached draw " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
