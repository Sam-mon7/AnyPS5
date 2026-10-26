#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/DwordPatternFill.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace AgcDriver::DriverDetail {

struct DriverTestAccess {
    static void Fill(const std::shared_ptr<VulkanDevice>& device, std::uint64_t address, std::size_t bytes, const std::array<std::uint32_t, 4>& pattern) {
        Driver::Get().storeBufferFill(0x20, address, bytes, pattern, device, false, false);
    }
    static bool Stable(std::span<const ShaderRecompiler::MemoryRegion> captured) { return Driver::Get().captureStable(captured); }
    static void Foreign(std::uint64_t address, std::size_t bytes) { Driver::Get().noteForeignWriter(address, address + bytes, 0x28); }
};

}

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::MatchDwordPatternFill;

alignas(256) constexpr std::array<std::uint32_t, 69> Code{
    0xbfa00003u, 0xd7460002u, 0x04010c0au, 0x7da80408u, 0xbf88003fu, 0x7e000c09u, 0xbf070980u, 0x858a807eu,
    0x7e005700u, 0x100000ffu, 0x4f800000u, 0x7e060f00u, 0xd5766a00u, 0x02020609u, 0x7d8a0280u, 0x4c020080u,
    0x02000101u, 0xd56a0001u, 0x00020700u, 0x4c000303u, 0x4a020303u, 0x02000101u, 0xd56a0000u, 0x00020500u,
    0xd5690001u, 0x00020009u, 0x4c060302u, 0x7d860609u, 0x7d8c02f9u, 0x06068c02u, 0x87ea6a0cu, 0x50000080u,
    0xd5286a00u, 0x003200c1u, 0xd5010000u, 0x002a00c1u, 0xd5690000u, 0x00020009u, 0x4c000102u, 0x7d0a0080u,
    0xbe88246au, 0xbf880015u, 0x7d0a0081u, 0xbe8a246au, 0xbf88000cu, 0x7d0a0082u, 0xbeea246au, 0xbf880003u,
    0x7e000207u, 0xe0102000u, 0x80000002u, 0x8afe7e6au, 0xbf880003u, 0x7e000206u, 0xe0102000u, 0x80000002u,
    0xbefe046au, 0x8afe7e0au, 0xbf880003u, 0x7e000205u, 0xe0102000u, 0x80000002u, 0xbefe040au, 0x8afe7e08u,
    0xbf880003u, 0x7e000204u, 0xe0102000u, 0x80000002u, 0xbf810000u};
constexpr std::size_t ComparesAt = 27;
constexpr std::array<std::uint32_t, 69> WithCompares(std::array<std::uint32_t, 3> compares) {
    auto code = Code;
    for (std::size_t word = 0; word < compares.size(); ++word) code[ComparesAt + word] = compares[word];
    return code;
}
alignas(256) constexpr std::array<std::uint32_t, 69> SwappedCode = WithCompares({0x7d8c02f9u, 0x06068c02u, 0x7d860609u});
constexpr std::array<std::uint32_t, 4> Pattern{0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
constexpr std::uint32_t Untouched = 0xcafebabeu;
alignas(256) std::array<std::uint32_t, 1028> Output{};

std::vector<std::uint32_t> UserData(void* output, std::uint32_t records, std::uint32_t count, std::uint32_t period, std::uint32_t format = 0x00014004u) {
    const auto address = reinterpret_cast<std::uintptr_t>(output);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), records, format, Pattern[0], Pattern[1], Pattern[2], Pattern[3], count, period};
}

void SwappedComparesTests() {
    const ShaderRecompiler::ShaderComputeStageInfo compute{{64, 1, 1}, 0, {true, false, false}, false, 1};
    const std::array<std::uint32_t, 5> packet{0xc0031500u, 4, 1, 1, 0x41u};
    for (const auto period : {1u, 2u, 4u}) {
        const auto data = UserData(Output.data(), 1024, 256, period);
        const auto listed = MatchDwordPatternFill(packet, Code, data, compute);
        const auto swapped = MatchDwordPatternFill(packet, SwappedCode, data, compute);
        Require(listed && swapped && swapped->base == listed->base && swapped->bytes == listed->bytes && swapped->pattern == listed->pattern, "the kernel with its two compares in the other order was not recognized as the same fill");
    }
    const auto data = UserData(Output.data(), 1024, 256, 4);
    for (std::size_t word = 0; word < SwappedCode.size(); ++word) {
        auto altered = SwappedCode;
        altered[word] ^= 1u;
        Require(!MatchDwordPatternFill(packet, altered, data, compute), "a different shader matched the dword pattern-fill kernel with swapped compares");
    }
    Require(!MatchDwordPatternFill(packet, std::span(SwappedCode).first(68), data, compute), "a truncated shader with swapped compares matched");
    constexpr std::array<std::array<std::uint32_t, 3>, 5> otherOrders{{
        {0x7d8c02f9u, 0x7d860609u, 0x06068c02u},
        {0x06068c02u, 0x7d8c02f9u, 0x7d860609u},
        {0x7d860609u, 0x06068c02u, 0x7d8c02f9u},
        {0x7d860609u, 0x7d860609u, 0x7d8c02f9u},
        {0x7d8c02f9u, 0x06068c02u, 0x7d8c02f9u},
    }};
    for (const auto& order : otherOrders) {
        const auto code = WithCompares(order);
        Require(!MatchDwordPatternFill(packet, code, data, compute), "a kernel whose compare words are not one of the two orders matched");
    }
}

void MatchTests() {
    const ShaderRecompiler::ShaderComputeStageInfo compute{{64, 1, 1}, 0, {true, false, false}, false, 1};
    const std::array<std::uint32_t, 5> packet{0xc0031500u, 4, 1, 1, 0x41u};
    const auto data = UserData(Output.data(), 1024, 256, 4);
    const auto fill = MatchDwordPatternFill(packet, Code, data, compute);
    Require(fill && fill->base == reinterpret_cast<std::uintptr_t>(Output.data()) && fill->bytes == 1024 && fill->pattern == Pattern, "the dword pattern-fill kernel was not recognized as a 16-byte pattern fill");
    for (const auto period : {1u, 2u}) {
        const auto narrow = MatchDwordPatternFill(packet, Code, UserData(Output.data(), 1024, 256, period), compute);
        Require(narrow && narrow->pattern[1] == Pattern[1 % period] && narrow->pattern[3] == Pattern[3 % period], "a shorter pattern period was not repeated over 16 bytes");
    }
    for (std::size_t word = 0; word < Code.size(); ++word) {
        auto altered = Code;
        altered[word] ^= 1u;
        Require(!MatchDwordPatternFill(packet, altered, data, compute), "a different shader matched the dword pattern-fill kernel");
    }
    Require(!MatchDwordPatternFill(packet, std::span(Code).first(68), data, compute), "a truncated shader matched");
    Require(!MatchDwordPatternFill(packet, Code, UserData(Output.data(), 1024, 256, 3), compute), "a three-dword period matched a 16-byte pattern");
    Require(!MatchDwordPatternFill(packet, Code, UserData(Output.data(), 1024, 250, 4), compute), "a count of whole dwords not filling 16-byte units matched");
    Require(!MatchDwordPatternFill(packet, Code, UserData(Output.data() + 1, 1024, 256, 4), compute), "an unaligned destination matched");
    Require(!MatchDwordPatternFill(packet, Code, UserData(Output.data(), 1024, 256, 4, 0x00814004u), compute), "an add-tid descriptor matched");
    for (const std::uint32_t format : {0x00015004u, 0x00016004u}) {
        Require(MatchDwordPatternFill(packet, Code, UserData(Output.data(), 1024, 256, 4, format), compute).has_value(), "a 32-bit output format was refused");
    }
    for (const std::uint32_t format : {0x00005004u, 0x0000b004u, 0x0000d004u, 0x0001b004u, 0x00027facu, 0x0004b004u}) {
        Require(!MatchDwordPatternFill(packet, Code, UserData(Output.data(), 1024, 256, 4, format), compute), "an output format whose elements are not one 32-bit value matched");
    }
    auto extra = data;
    extra.push_back(0);
    Require(!MatchDwordPatternFill(packet, Code, extra, compute), "a different user SGPR count matched (the group id moves)");
    auto partial = packet;
    partial[4] |= 0x20u;
    Require(!MatchDwordPatternFill(partial, Code, data, compute), "a partial dispatch matched");
    auto noGroup = compute;
    noGroup.groupIdEnable[0] = false;
    Require(!MatchDwordPatternFill(packet, Code, data, noGroup), "a dispatch without the group id matched");
    const auto clamped = MatchDwordPatternFill(packet, Code, UserData(Output.data(), 64, 256, 4), compute);
    Require(clamped && clamped->bytes == 256, "the fill exceeded the descriptor's records");
}

std::vector<std::uint32_t> Commands(std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& data, std::uint32_t groups, bool partial, std::uint32_t threads) {
    std::vector<std::uint32_t> words;
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{64, 1, 1};
    registers(0x207, shape);
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)};
    registers(0x20c, program);
    const std::array<std::uint32_t, 1> resource{(static_cast<std::uint32_t>(data.size()) << 1u) | (1u << 7u)};
    registers(0x213, resource);
    registers(0x240, data);
    words.insert(words.end(), {0xc0031500u, partial ? threads : groups, 1, 1, 0x41u | (partial ? 0x20u : 0u)});
    return words;
}

void ExecuteTests() {
    struct Case {
        std::uint32_t groups, records, count, period;
        bool partial;
        std::uint32_t threads;
    };
    constexpr std::array<Case, 10> cases{{
        {4, 1024, 256, 4, false, 0},
        {4, 1024, 256, 2, false, 0},
        {4, 1024, 256, 1, false, 0},
        {16, 1024, 1024, 4, false, 0},
        {4, 1024, 100, 4, false, 0},
        {4, 1024, 250, 4, false, 0},
        {4, 64, 256, 4, false, 0},
        {4, 1024, 256, 3, false, 0},
        {4, 1024, 256, 5, false, 0},
        {0, 1024, 256, 4, true, 70},
    }};
    for (const std::span<const std::uint32_t> code : {std::span<const std::uint32_t>(Code), std::span<const std::uint32_t>(SwappedCode)}) {
        for (const auto& item : cases) {
            Output.fill(Untouched);
            const auto words = Commands(code, UserData(Output.data(), item.records, item.count, item.period), item.groups, item.partial, item.threads);
            Packet packet{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
            AgcDriver::Submit(&packet, 0);
            AgcDriverWaitIdle_nid_postfix();
            AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Output.data()), sizeof(Output));
            const auto launched = item.partial ? item.threads : item.groups * 64u;
            const auto filled = std::min({launched, item.records, item.count});
            for (std::uint32_t i = 0; i < Output.size(); ++i) {
                const auto expected = i < filled ? Pattern[std::min(i % item.period, 3u)] : Untouched;
                Require(Output[i] == expected, "dword pattern fill mismatch at " + std::to_string(i) + " (" + (code.data() == SwappedCode.data() ? "swapped" : "listed") + " compares, period " + std::to_string(item.period) + ", count " + std::to_string(item.count) + ", records " + std::to_string(item.records) + "): got " + std::to_string(Output[i]) + ", expected " + std::to_string(expected));
            }
        }
    }
}

void KnownCaptureTests(const std::shared_ptr<AgcDriver::VulkanDevice>& device) {
    using Access = AgcDriver::DriverDetail::DriverTestAccess;
    using Memory = AgcDriver::ShaderMemory;
    namespace Guest = AgcDriver::GuestMemory;
    constexpr std::size_t HeapBytes = 1u << 20u;
    auto* heap = static_cast<std::uint32_t*>(GuestArena::GuestArenaAllocate_nid_postfix(HeapBytes, HeapBytes));
    Require(heap != nullptr, "known fill: cannot allocate guest memory");
    GuestArena::GuestArenaCommit_nid_postfix(heap, HeapBytes, 0x04u, HeapBytes);
#ifndef _WIN32
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(heap, HeapBytes);
#endif
    std::fill_n(heap, HeapBytes / 4, Untouched);
    GuestAllocations::Mutation().Add(heap, HeapBytes, true, true, true);
    const auto base = reinterpret_cast<std::uint64_t>(heap);
    const bool enabled = std::getenv("APS5_NO_FILL_KNOWN_VALUES") == nullptr;
    const bool verify = std::getenv("APS5_VERIFY_KNOWN_VALUES") != nullptr;
    const auto expectedPolicy = !enabled ? Memory::PendingWrite::Sync : verify ? Memory::PendingWrite::VerifyKnownValue : Memory::PendingWrite::KnownValue;
    alignas(256) constexpr std::array<std::uint32_t, 13> reader{0xf4000200u, 0xfa000000u, 0x34020082u, 0xdc308000u, 0x03020001u, 0xbf8cc07fu, 0x7e040208u, 0x4a040481u, 0xbf8c3f70u, 0x4a040503u, 0xe0701000u, 0x80010201u, 0xbf810000u};
    const auto capture = [&](std::uint64_t address, std::uint32_t expected) {
        const auto output = base + 8192;
        const std::array<std::uint32_t, 8> data{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), static_cast<std::uint32_t>(base + 4096), static_cast<std::uint32_t>((base + 4096) >> 32u), static_cast<std::uint32_t>(output), static_cast<std::uint32_t>((output >> 32u) & 0xffffu), 128, 0x31016facu};
        ShaderRecompiler::RecompileRequest request{};
        request.shader = {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uint64_t>(reader.data()), reader, 0, {}};
        request.context.waveSize = 64;
        request.context.userData = data;
        request.context.compute = ShaderRecompiler::ShaderComputeStageInfo{{32, 1, 1}, 0, {false, false, false}, false, 1};
        request.target = device->ComputeTarget(64);
        request.layout.pushConstantSizeBytes = 128;
        request.useCache = false;
        auto memory = std::make_shared<Memory>(std::span<const ShaderRecompiler::MemoryRegion>{}, &AgcDriver::DriverDetail::Driver::QueryPendingSpan);
        static_cast<void>(memory->Capture(request));
        const auto regions = memory->Regions();
        const auto found = std::find_if(regions.begin(), regions.end(), [&](const auto& region) { return region.guestAddress <= address && address + 4 <= region.guestAddress + region.bytes.size(); });
        Require(found != regions.end(), "known fill: the scalar input was not captured");
        std::uint32_t actual = 0;
        std::memcpy(&actual, found->bytes.data() + (address - found->guestAddress), sizeof(actual));
        Require(actual == expected, "known fill: captured a stale scalar input");
        return memory;
    };
    std::unique_lock lock(Guest::GpuMutex());
    Require(Guest::CollectWritesUncached(base, HeapBytes) != 0, "known fill: guest memory is not write-watched");
    Access::Fill(device, base, 64, Pattern);
    Require(AgcDriver::Graphics::Recorder::SnapshotWriteOverlaps(base, 64), "known fill: no pending producer");
    Require(heap[0] == Untouched, "known fill: the producer ran before capture");
    Require(Guest::Watched(base, 64), "known fill: the GPU import disabled write tracking");
    Require(AgcDriver::DriverDetail::Driver::QueryPendingSpan(base + 4, 4, {}) == expectedPolicy, "known fill: wrong subrange read policy");
    Require(AgcDriver::DriverDetail::Driver::QueryPendingSpan(base + 60, 8, {}) == Memory::PendingWrite::Sync, "known fill: a crossing read was served from a partial value");
    const auto knownBefore = Memory::KnownValues();
    auto first = capture(base, Pattern[0]);
    Require((Memory::KnownValues().served > knownBefore.served) == enabled, "known fill: capture did not follow the switch");
    if (enabled && !verify) Require(heap[0] == Untouched && AgcDriver::Graphics::Recorder::SnapshotWriteOverlaps(base, 64), "known fill: capture submitted or waited for its producer");
    else Require(heap[0] == Pattern[0] && !AgcDriver::Graphics::Recorder::SnapshotWriteOverlaps(base, 64), "known fill: capture did not synchronize its producer");
    static_cast<void>(capture(base + 4, Pattern[1]));
    static_cast<void>(capture(base + 64, Untouched));
    device->WaitIdle();

    const std::array<std::uint32_t, 4> later{0x55667788u, 0x10203040u, 0x99aabbccu, 0xddeeff00u};
    Access::Fill(device, base, 64, later);
    Require(!Access::Stable(first->Regions()), "known fill: stale capture validated after an overlapping fill");
    static_cast<void>(capture(base, later[0]));
    device->WaitIdle();

    Access::Fill(device, base, 64, Pattern);
    Require(device->FillBuffer(base, 16, later), "known fill: cannot record a later GPU writer");
    Require(AgcDriver::DriverDetail::Driver::QueryPendingSpan(base, 4, {}) == Memory::PendingWrite::Sync, "known fill: a later GPU write did not invalidate the known value");
    static_cast<void>(capture(base, later[0]));
    device->WaitIdle();

    if (device->CanWaitUnlocked()) {
        Access::Fill(device, base, 64, Pattern);
        const auto serial = device->SubmitAndEpoch();
        lock.unlock();
        device->WaitRecorded(serial);
        lock.lock();
        Require(AgcDriver::Graphics::Recorder::SnapshotWriteOverlaps(base, 64), "known fill: the submitted producer was reaped");
        heap[0] = later[0];
        Require(AgcDriver::DriverDetail::Driver::QueryPendingSpan(base, 4, {}) == Memory::PendingWrite::Sync, "known fill: a later CPU write did not invalidate the known value");
        static_cast<void>(capture(base, later[0]));
        device->WaitIdle();
    } else {
        std::puts("known fill: CPU mutation subcase requires timeline semaphores");
    }

    Access::Fill(device, base, 64, Pattern);
    Access::Foreign(base, 4);
    Require(AgcDriver::DriverDetail::Driver::QueryPendingSpan(base, 4, {}) == Memory::PendingWrite::Sync, "known fill: a foreign writer did not invalidate the known value");
    static_cast<void>(capture(base, Pattern[0]));
    device->WaitIdle();

    Access::Fill(device, base, 8192, Pattern);
    Require(AgcDriver::DriverDetail::Driver::QueryPendingSpan(base, 4, {}) == Memory::PendingWrite::Sync, "known fill: an oversized producer retained a known value");
    device->WaitIdle();
    Require(Memory::KnownValues().mismatches == knownBefore.mismatches, "known fill: verification found a stale known value");
    GuestAllocations::Mutation().Remove(heap);
    std::puts("known fill capture tests passed");
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--known-capture") {
            auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            KnownCaptureTests(std::shared_ptr<AgcDriver::VulkanDevice>(std::move(device)));
            AgcDriverShutdown_nid_postfix();
            return 0;
        }
        if (std::getenv("APS5_NO_FILL_HLE") == nullptr) {
            MatchTests();
            SwappedComparesTests();
        }
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        ExecuteTests();
        AgcDriverShutdown_nid_postfix();
        std::puts("dword pattern fill tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
