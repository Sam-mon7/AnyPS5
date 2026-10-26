#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/BufferCopy.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::DriverDetail::Driver;
using AgcDriver::Graphics::Require;

constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Lanes = 32;
constexpr std::uint32_t CopyRecords = 128;
constexpr std::uint32_t Unwritten = 0xcafebabeu;
alignas(256) constexpr std::array<std::uint32_t, 16> CopyCode{0xd7460008u, 0x04010e08u, 0x8f6a8708u, 0xd76d0009u, 0x0401806au, 0xe00c2000u, 0x80000008u, 0xe00c2000u, 0x80000409u, 0xbf8c3f71u, 0xe01c2000u, 0x80010008u, 0xbf8c3f70u, 0xe01c2000u, 0x80010409u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 11> ReadCode{0xf4080200u, 0xfa000000u, 0xf4000300u, 0xfa000010u, 0xbf8cc07fu, 0x34020082u, 0x7e04020cu, 0x4a040481u, 0xe0701000u, 0x80020201u, 0xbf810000u};

std::uint32_t* NewBlock() {
    std::uint32_t* block = nullptr;
#ifdef _WIN32
    block = static_cast<std::uint32_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes));
    if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
    void* mapped = mmap(nullptr, BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped != MAP_FAILED) {
        block = static_cast<std::uint32_t*>(mapped);
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
    }
#endif
    Require(block != nullptr, "pending data hits: cannot allocate guest memory");
    std::memset(block, 0, BlockBytes);
    GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    return block;
}

void Registers(std::vector<std::uint32_t>& words, std::uint32_t first, std::span<const std::uint32_t> values) {
    words.insert(words.end(), {0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u), first});
    words.insert(words.end(), values.begin(), values.end());
}

void Program(std::vector<std::uint32_t>& words, const void* code, std::span<const std::uint32_t> data, bool copy) {
    const auto address = reinterpret_cast<std::uintptr_t>(code);
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)};
    const std::array<std::uint32_t, 3> shape{copy ? 64u : Lanes, 1u, 1u};
    const std::array<std::uint32_t, 1> resource{(static_cast<std::uint32_t>(data.size()) << 1u) | (copy ? 1u << 7u : 0u)};
    Registers(words, 0x20c, program);
    Registers(words, 0x207, shape);
    Registers(words, 0x213, resource);
    Registers(words, 0x240, data);
}

std::array<std::uint32_t, 4> Descriptor(const void* pointer, std::uint32_t stride, std::uint32_t records, std::uint32_t format) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), records, format};
}

void Table(std::uint32_t* table, std::uint32_t* output, std::uint32_t value) {
    const auto descriptor = Descriptor(output, 0, Lanes * 4u, 0x31016facu);
    std::copy(descriptor.begin(), descriptor.end(), table);
    table[4] = value;
}

void Execute(std::uint32_t* constants, std::uint32_t* source, std::uint32_t* arguments, bool indirect) {
    std::vector<std::uint32_t> words;
    if (source != nullptr) {
        const auto from = Descriptor(source, 16, CopyRecords, 0x0004bfacu);
        const auto to = Descriptor(constants, 16, CopyRecords, 0x0004bfacu);
        std::vector<std::uint32_t> data(from.begin(), from.end());
        data.insert(data.end(), to.begin(), to.end());
        Program(words, CopyCode.data(), data, true);
        words.insert(words.end(), {0xc0031500u, 1u, 1u, 1u, 0x41u});
    }
    const auto address = reinterpret_cast<std::uintptr_t>(constants);
    const std::array<std::uint32_t, 2> data{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)};
    Program(words, ReadCode.data(), data, false);
    if (indirect) {
        const auto argumentAddress = reinterpret_cast<std::uintptr_t>(arguments);
        words.insert(words.end(), {0xc0021600u, static_cast<std::uint32_t>(argumentAddress), static_cast<std::uint32_t>(argumentAddress >> 32u), 0x8041u});
    } else {
        words.insert(words.end(), {0xc0031500u, 1u, 1u, 1u, 0x8041u});
    }
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "pending data hits: submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

void CheckOutput(std::uint32_t* output, std::uint32_t value) {
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(output), Lanes * sizeof(std::uint32_t));
    for (std::size_t lane = 0; lane < Lanes; ++lane) Require(output[lane] == value + 1u, "pending data hits: wrong output at lane " + std::to_string(lane));
}

int Run(bool indirect) {
    auto* constants = NewBlock();
    auto* source = NewBlock();
    auto* output = NewBlock();
    auto* movedOutput = NewBlock();
    auto* arguments = NewBlock();
    std::fill_n(arguments, 3, 1u);
    Table(constants, output, 10u);
    Execute(constants, nullptr, arguments, indirect);
    CheckOutput(output, 10u);
    Require(Driver::Get().DispatchCacheCounters().dataInserts != 0 || std::getenv("APS5_NO_DATA_HITS") != nullptr, "pending data hits: the shader recorded no pure data position");
    const bool enabled = std::getenv("APS5_NO_PENDING_DATA_HITS") == nullptr && std::getenv("APS5_NO_DATA_HITS") == nullptr && std::getenv("APS5_VERIFY_KNOWN_VALUES") == nullptr && std::getenv("APS5_VALIDATE_LEGACY") == nullptr;
    for (std::uint32_t round = 0; round < 4; ++round) {
        auto* target = round < 2 ? output : movedOutput;
        const auto value = 20u + round;
        Table(source, target, value);
        std::fill_n(target, Lanes, Unwritten);
        const auto before = Driver::Get().DispatchCacheCounters();
        const auto copies = AgcDriver::DriverDetail::copyPathCounts[AgcDriver::DriverDetail::CopyGpu].load();
        Execute(constants, source, arguments, indirect);
        CheckOutput(target, value);
        if (AgcDriver::DriverDetail::copyPathCounts[AgcDriver::DriverDetail::CopyGpu].load() == copies || !AgcDriver::GuestMemory::WriteWatched()) {
            std::puts("skipped, known pending copies require host imports and write watching");
            return VulkanTestSkipped;
        }
        const auto after = Driver::Get().DispatchCacheCounters();
        const auto expectedHits = enabled && round != 2 ? 1u : 0u;
        Require(after.dataHits == before.dataHits + expectedHits, "pending data hits: wrong cache outcome on round " + std::to_string(round));
        Require(after.inserts == before.inserts + (expectedHits == 0 ? 1u : 0u), "pending data hits: wrong capture count");
        if (expectedHits != 0 && std::getenv("APS5_VERIFY_DATA_HITS") != nullptr) Require(after.dataVerified == before.dataVerified + 1u, "pending data hits: the hit was not verified");
        if (round == 2) for (std::size_t lane = 0; lane < Lanes; ++lane) Require(output[lane] == 22u, "pending data hits: changed descriptor reused the old output");
    }
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        const int result = Run(argc > 1 && std::string(argv[1]) == "--indirect");
        AgcDriverShutdown_nid_postfix();
        if (result == 0) std::puts("pending data hits: copied constants refresh, changed descriptors recapture");
        return result;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
