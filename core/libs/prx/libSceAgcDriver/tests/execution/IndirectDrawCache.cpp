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

void AppendState(std::vector<std::uint32_t>& commands, std::array<std::byte, 1024>& memory) {
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(PixelCode.data());
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

}

int main() {
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
        IndirectDrawsMatchTheDirectDraw();
        RepeatedIndirectDrawsStayOnTheGpu();
        AgcDriverShutdown_nid_postfix();
        std::puts("indirect draw cache tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
