#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using Path = AgcDriver::Graphics::IndirectDrawPath;

constexpr std::byte Kept{0x40};
constexpr std::uint32_t BaseVertexLocation = 0x8c;
constexpr std::uint32_t StaleBaseVertex = 0x51;
constexpr std::size_t Draws = 400;
constexpr std::size_t Frames = 12;

alignas(256) constexpr std::array<std::uint32_t, 20> VertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u, 0x7e0a02ffu, 0x44800000u, 0xf80000d1u, 0x00000005u, 0xf80008cfu, 0x04030201u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 11> SrtPixelCode{0xf4080100u, 0xfa000000u, 0xbf8cc07fu, 0xe0300000u, 0x80010000u, 0xbf8c3f70u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 16> NestedSrtPixelCode{0xf4040100u, 0xfa000000u, 0xbf8cc07fu, 0xf4000202u, 0xfa000010u, 0xf4080102u, 0xfa000000u, 0xbf8cc07fu, 0xe0300000u, 0x80010000u, 0xbf8c3f70u, 0x7e020208u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u};
alignas(256) std::array<float, 4> SrtColors{1.0f, 0.5f, 0.0f, 0.0f};
alignas(256) std::array<float, 4> OtherSrtColors{0.5f, 0.0f, 0.0f, 0.0f};
alignas(256) std::array<std::array<float, 4>, 5> NestedSrtColors{};
alignas(256) std::array<std::array<std::uint32_t, 5>, 5> SrtTables{};
alignas(256) std::array<std::array<std::uint32_t, 2>, 5> SrtRoots{};
alignas(256) constexpr auto CpuVertexCode = [] {
    auto code = VertexCode;
    code[10] = 0x7e0a0208u;
    code[11] = 0xbf800000u;
    return code;
}();
alignas(256) std::array<std::byte, 1024> DirectMemory{};
alignas(256) std::array<std::byte, 1024> IndirectMemory{};
constexpr std::size_t RecordBytes = 65536;
std::uint32_t* Records = nullptr;

void AllocateRecords() {
    Records = static_cast<std::uint32_t*>(::operator new(RecordBytes, std::align_val_t{RecordBytes}));
    GuestAllocations::Mutation mutation;
    mutation.Add(Records, RecordBytes, true, true, true);
    const std::array<std::uint32_t, 4> record{3, 1, 0, 0};
    std::copy(record.begin(), record.end(), Records);
}

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};
alignas(8) Header CpuVertexHeader{};
alignas(8) Header SrtPixelHeader{};

void Registered(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

void AppendState(std::vector<std::uint32_t>& commands, std::array<std::byte, 1024>& memory, const std::uint32_t* vertexCode = VertexCode.data(), const std::uint32_t* pixelCode = PixelCode.data()) {
    const auto vertex = reinterpret_cast<std::uintptr_t>(vertexCode);
    const auto pixel = reinterpret_cast<std::uintptr_t>(pixelCode);
    const auto color = reinterpret_cast<std::uintptr_t>(memory.data());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0x01200000}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
    };
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 1u << 1u},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 1u << 1u}, {0x00c, std::bit_cast<std::uint32_t>(1.0f)},
        {BaseVertexLocation, StaleBaseVertex},
    };
    Append(commands, 0x79, {0x242, 4});
    Append(commands, 0x79, {0x24a, 0});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
}

void Submit(std::vector<std::uint32_t>& commands) {
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

void Flush(const std::array<std::byte, 1024>& memory) {
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(memory.data()), memory.size());
}

void IndirectDrawsMatchTheDirectDraw() {
    DirectMemory.fill(Kept);
    IndirectMemory.fill(Kept);
    std::vector<std::uint32_t> direct;
    AppendState(direct, DirectMemory);
    Append(direct, 0x2d, {3, 2});
    Submit(direct);
    std::vector<std::uint32_t> indirect;
    AppendState(indirect, IndirectMemory);
    const auto records = reinterpret_cast<std::uintptr_t>(Records);
    Append(indirect, 0x11, {1, static_cast<std::uint32_t>(records), static_cast<std::uint32_t>(records >> 32u)});
    Append(indirect, 0x24, {0, BaseVertexLocation, 0, 2});
    Submit(indirect);
    Flush(DirectMemory);
    Flush(IndirectMemory);
    const auto covered = std::count_if(DirectMemory.begin(), DirectMemory.end(), [](std::byte value) { return value != Kept; });
    Require(covered != 0 && covered != static_cast<std::ptrdiff_t>(DirectMemory.size()), "the direct draw covered " + std::to_string(covered) + " bytes, expected an edge through the target");
    Require(IndirectMemory == DirectMemory, "the indirect draw whose program ignores its base vertex register drew other pixels than the direct draw");
}

void RepeatedIndirectDrawsStayOnTheGpu() {
    std::vector<std::uint32_t> commands;
    AppendState(commands, IndirectMemory);
    const auto records = reinterpret_cast<std::uintptr_t>(Records);
    Append(commands, 0x11, {1, static_cast<std::uint32_t>(records), static_cast<std::uint32_t>(records >> 32u)});
    for (std::size_t draw = 0; draw < Draws; ++draw) Append(commands, 0x24, {0, BaseVertexLocation, 0, 2});
    const auto pathsBefore = AgcDriver::Graphics::IndirectDrawCounts();
    const auto cacheBefore = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    std::vector<double> frameMs;
    for (std::size_t frame = 0; frame < Frames; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        Submit(commands);
        frameMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    const auto paths = AgcDriver::Graphics::IndirectDrawCounts();
    const auto cache = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    std::uint64_t cpuSide = 0;
    for (std::size_t path = 1; path < paths.size(); ++path) {
        cpuSide += paths[path] - pathsBefore[path];
        if (paths[path] != pathsBefore[path]) std::printf("CPU path, %s: %llu\n", AgcDriver::Graphics::IndirectDrawPathName(static_cast<Path>(path)), static_cast<unsigned long long>(paths[path] - pathsBefore[path]));
    }
    const auto gpuSide = paths[static_cast<std::size_t>(Path::Gpu)] - pathsBefore[static_cast<std::size_t>(Path::Gpu)];
    std::vector<double> warm(frameMs.begin() + 2, frameMs.end());
    std::sort(warm.begin(), warm.end());
    const auto median = warm[warm.size() / 2];
    std::printf("%zu frames of %zu indirect draws: median %.2f ms per frame (%.1f us per draw) after two warm-up frames; %llu on the GPU, %llu on the CPU; draw cache: %llu lookups, %llu without an entry, %llu inserts, %llu hits\n", Frames, Draws, median,
                median * 1000.0 / static_cast<double>(Draws), static_cast<unsigned long long>(gpuSide), static_cast<unsigned long long>(cpuSide), static_cast<unsigned long long>(cache.lookups - cacheBefore.lookups),
                static_cast<unsigned long long>(cache.absent - cacheBefore.absent), static_cast<unsigned long long>(cache.inserts - cacheBefore.inserts), static_cast<unsigned long long>(cache.hits - cacheBefore.hits));
    Require(cpuSide == 0 && gpuSide == Draws * Frames, "the indirect draws whose program ignores its base vertex register took the CPU path " + std::to_string(cpuSide) + " times");
    Require(cache.hits - cacheBefore.hits >= (Frames - 1) * Draws, "the repeated indirect draws hit the draw cache " + std::to_string(cache.hits - cacheBefore.hits) + " times");
}

void CpuIndirectCacheRefreshesPushConstants(bool builtinTest = false, bool unknownLimit = false, bool runtimeWords = false, bool relocated = false, bool nested = false) {
    const bool builtins = builtinTest && std::getenv("APS5_NO_INDIRECT_BUILTIN_OFFSETS") == nullptr;
    const bool enabled = std::getenv("APS5_NO_CPU_INDIRECT_DRAW_CACHE") == nullptr;
    alignas(256) std::array<std::uint16_t, 6> indices{0, 1, 2, 0, 0, 0};
    if (builtinTest || runtimeWords) indices = {0, 1, 2, 2, 0, 1};
    const auto indexAddress = reinterpret_cast<std::uintptr_t>(indices.data());
    const auto before = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    const auto pathsBefore = AgcDriver::Graphics::IndirectDrawCounts();
    const auto draws = builtinTest ? 8u : 5u;
    const auto pixel = [&](std::vector<std::uint32_t>& commands, std::size_t table) {
        if (!relocated) return;
        const auto pointer = reinterpret_cast<std::uintptr_t>(nested ? SrtRoots[table].data() : SrtTables[table].data());
        Append(commands, 0x76, {0x00b, 2u << 1u});
        Append(commands, 0x76, {0x00c, static_cast<std::uint32_t>(pointer), static_cast<std::uint32_t>(pointer >> 32u)});
    };
    for (std::size_t table = 0; table < SrtTables.size(); ++table) {
        NestedSrtColors[table][0] = 0.125f * static_cast<float>(table + 1u);
        const auto data = reinterpret_cast<std::uintptr_t>(nested ? NestedSrtColors[table].data() : table == 3u ? OtherSrtColors.data() : SrtColors.data());
        SrtTables[table] = {static_cast<std::uint32_t>(data), static_cast<std::uint32_t>(data >> 32u) & 0xffffu, table == 3u ? 12u : 16u, 0x4bfacu, std::bit_cast<std::uint32_t>(0.125f * static_cast<float>(table + 1u))};
        const auto pointer = reinterpret_cast<std::uintptr_t>(SrtTables[table].data());
        SrtRoots[table] = {static_cast<std::uint32_t>(pointer), static_cast<std::uint32_t>(pointer >> 32u)};
    }
    for (std::uint32_t draw = 0; draw < draws; ++draw) {
        const auto color = std::bit_cast<std::uint32_t>(0.125f * static_cast<float>(draw + 1u));
        const auto location = BaseVertexLocation + (builtinTest && draw % 2u != 0u ? 1u : 0u);
        const auto stale = std::bit_cast<std::uint32_t>(0.75f);
        DirectMemory.fill(Kept);
        IndirectMemory.fill(Kept);
        const auto firstIndex = draw % 2u * 3u;
        const auto indexCount = draw == 4 ? 1u : 3u;
        std::vector<std::uint32_t> direct;
        AppendState(direct, DirectMemory, CpuVertexCode.data(), nested ? NestedSrtPixelCode.data() : relocated ? SrtPixelCode.data() : PixelCode.data());
        pixel(direct, nested ? draw : draw == 3u ? 3u : 0u);
        if (builtinTest) Append(direct, 0x76, {0x8b, 2u << 1u});
        Append(direct, 0x76, {BaseVertexLocation, location == BaseVertexLocation ? color : stale});
        Append(direct, 0x2a, {0});
        const auto directIndices = indexAddress + firstIndex * sizeof(std::uint16_t);
        Append(direct, 0x27, {indexCount, static_cast<std::uint32_t>(directIndices), static_cast<std::uint32_t>(directIndices >> 32u), indexCount, 0});
        Submit(direct);
        const std::array<std::uint32_t, 5> record{indexCount, 1, firstIndex, color, 0};
        std::copy(record.begin(), record.end(), Records);
        std::vector<std::uint32_t> indirect;
        AppendState(indirect, IndirectMemory, CpuVertexCode.data(), nested ? NestedSrtPixelCode.data() : relocated ? SrtPixelCode.data() : PixelCode.data());
        pixel(indirect, draw);
        if (runtimeWords) {
            Append(indirect, 0x76, {BaseVertexLocation, StaleBaseVertex + draw});
            if (draw == 3u && !relocated) Append(indirect, 0x76, {0x00c, std::bit_cast<std::uint32_t>(0.5f)});
        }
        if (builtinTest) {
            Append(indirect, 0x76, {0x8b, 2u << 1u});
            Append(indirect, 0x76, {BaseVertexLocation, stale, 0x51u});
        }
        const auto address = reinterpret_cast<std::uintptr_t>(Records);
        Append(indirect, 0x11, {1, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)});
        Append(indirect, 0x2a, {0});
        Append(indirect, 0x26, {static_cast<std::uint32_t>(indexAddress), static_cast<std::uint32_t>(indexAddress >> 32u)});
        if (!unknownLimit) Append(indirect, 0x13, {static_cast<std::uint32_t>(indices.size())});
        Append(indirect, 0x25, {0, location, builtinTest ? 0x280u : 0u, 0});
        const auto cacheBeforeDraw = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
        Submit(indirect);
        if (runtimeWords && draw == 3u) Require(AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters().indirectCpuHits == cacheBeforeDraw.indirectCpuHits, "changed non-runtime word hit the indirect cache");
        Flush(DirectMemory);
        Flush(IndirectMemory);
        if ((builtinTest || runtimeWords || firstIndex == 0) && indexCount == 3) Require(std::any_of(DirectMemory.begin(), DirectMemory.end(), [](std::byte value) { return value != Kept; }), "CPU indirect reference draw wrote no pixels");
        if (runtimeWords && draw == 3u && !relocated) Require(IndirectMemory != DirectMemory, "changed non-runtime pixel word reused the cached stage");
        else Require(IndirectMemory == DirectMemory, "cached CPU indirect draw reused another record's color or index range");
    }
    const auto after = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    Require(after.verifyMismatches == before.verifyMismatches, "indirect cache hit disagrees with fresh capture");
    const auto paths = AgcDriver::Graphics::IndirectDrawCounts();
    if (builtinTest) {
        const auto expected = unknownLimit ? Path::IndexLimitUnknown : builtins ? std::getenv("APS5_NO_GPU_INDIRECT_DRAW") != nullptr ? Path::Disabled : Path::Gpu : Path::NotFolded;
        const auto expectedCount = !builtins && !unknownLimit ? draws / 2u : draws;
        Require(paths[static_cast<std::size_t>(expected)] - pathsBefore[static_cast<std::size_t>(expected)] == expectedCount, "builtin submission selected the wrong indirect draw path");
        if (builtins && !unknownLimit) {
            Require(after.hits >= before.hits + 2u, "builtin indirect submissions did not reuse warm draw-cache entries");
            Require(after.indirectCpuHits == before.indirectCpuHits, "GPU builtin draws used the CPU indirect cache");
        }
        Require(after.verifyMismatches == before.verifyMismatches, "builtin draw-cache verification disagreed with materialization");
    } else {
        Require(paths[static_cast<std::size_t>(Path::NotFolded)] - pathsBefore[static_cast<std::size_t>(Path::NotFolded)] == 5, "CPU indirect cache regression did not take NotFolded");
        Require(after.indirectCpuHits - before.indirectCpuHits == (enabled ? runtimeWords ? 2u : 3u : 0u), "CPU indirect submission did not exercise the expected cache hits: " + std::to_string(after.indirectCpuHits - before.indirectCpuHits));
    }
}

void EmptyIndirectPacketsSkipCapture() {
    IndirectDrawsMatchTheDirectDraw();
    const auto empty = AgcDriver::Graphics::EmptyIndirectDrawCount();
    const auto paths = AgcDriver::Graphics::IndirectDrawCounts();
    for (const auto record : {std::array<std::uint32_t, 4>{0, 1, 0, 0}, {3, 0, 0, 0}, {0, 0, 0, 0}}) {
        std::copy(record.begin(), record.end(), Records);
        IndirectMemory.fill(Kept);
        std::vector<std::uint32_t> commands;
        AppendState(commands, IndirectMemory);
        const auto address = reinterpret_cast<std::uintptr_t>(Records);
        Append(commands, 0x11, {1, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)});
        Append(commands, 0x24, {0, BaseVertexLocation, 0, 2});
        Submit(commands);
        Flush(IndirectMemory);
        Require(std::all_of(IndirectMemory.begin(), IndirectMemory.end(), [](std::byte value) { return value == Kept; }), "empty indirect draw wrote pixels");
    }
    Require(AgcDriver::Graphics::EmptyIndirectDrawCount() == empty + 3u, "empty indirect packets were not skipped");
    Require(AgcDriver::Graphics::IndirectDrawCounts() == paths, "empty indirect packets reached shader classification");
}

}

int main(int argc, char** argv) {
#ifdef _WIN32
    _putenv_s("APS5_PROFILE_DRAW", "1");
#else
    setenv("APS5_PROFILE_DRAW", "1", 1);
#endif
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
        Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
        AllocateRecords();
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "--builtin-offsets" || mode == "--builtin-offsets-fallback") {
            const auto device = OpenVulkanTestDevice();
            if (!device || !device->DrawIndirectSupport().drawParameters || !device->DrawIndirectSupport().firstInstance) return VulkanTestSkipped;
            Registered(CpuVertexHeader, CpuVertexCode.data(), sizeof(CpuVertexCode), 2);
            CpuIndirectCacheRefreshesPushConstants(true, mode == "--builtin-offsets-fallback");
        } else if (mode == "--empty-indirect") {
            EmptyIndirectPacketsSkipCapture();
        } else if (mode == "--cpu-indirect-cache" || mode == "--runtime-words" || mode == "--relocated-runtime-words" || mode == "--nested-runtime-words") {
            Registered(CpuVertexHeader, CpuVertexCode.data(), sizeof(CpuVertexCode), 2);
            if (mode == "--relocated-runtime-words") Registered(SrtPixelHeader, SrtPixelCode.data(), sizeof(SrtPixelCode), 1);
            if (mode == "--nested-runtime-words") Registered(SrtPixelHeader, NestedSrtPixelCode.data(), sizeof(NestedSrtPixelCode), 1);
            CpuIndirectCacheRefreshesPushConstants(false, false, mode != "--cpu-indirect-cache", mode == "--relocated-runtime-words" || mode == "--nested-runtime-words", mode == "--nested-runtime-words");
        } else {
            IndirectDrawsMatchTheDirectDraw();
            RepeatedIndirectDrawsStayOnTheGpu();
        }
        AgcDriverShutdown_nid_postfix();
        std::puts("indirect draw cache tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
