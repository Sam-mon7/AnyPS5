#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchGatherShadow.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::Driver;
namespace Shadow = AgcDriver::DriverDetail::DispatchShadow;
using AgcDriver::ArchB::Feature;
using AgcDriver::ArchB::Switches;

constexpr std::size_t Piece = 65536;
constexpr std::size_t BlockBytes = 8 * Piece;
constexpr std::uint32_t Sentinel = 0xdeadbeefu;

alignas(256) constexpr std::array<std::uint32_t, 14> DataCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u,
    0xf4000300u, 0xfa000020u, 0xbf8cc07fu,
    0xe0300000u, 0x80010200u, 0xbf8c3f70u,
    0x4a04040cu, 0xe0700000u, 0x80020200u, 0xbf810000u,
};

alignas(256) constexpr auto ShortExtentCode = DataCode;
alignas(256) constexpr auto AheadColdCode = DataCode;

alignas(256) constexpr std::array<std::uint32_t, 17> NestedCode{
    0xf4040080u, 0xfa000000u, 0xbf8cc07fu,
    0xf4080101u, 0xfa000000u, 0xf4080201u, 0xfa000010u,
    0xf4000301u, 0xfa000020u, 0xbf8cc07fu,
    0xe0300000u, 0x80010200u, 0xbf8c3f70u,
    0x4a04040cu, 0xe0700000u, 0x80020200u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 8> BdaAddressCode{
    0x7e000280u, 0xdc308000u, 0x02000000u, 0xbf8c3f70u,
    0x4a040487u, 0xdc708000u, 0x00020200u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 17> ControlCode{
    0xf4200400u, 0xfa000000u, 0xbf8cc07fu, 0xbf068010u, 0xbf85000bu,
    0xf4080106u, 0xfa000000u, 0xf4080206u, 0xfa000010u,
    0xbf8cc07fu, 0xe0300000u, 0x80010200u, 0xbf8c3f70u,
    0x4a040410u, 0xe0700000u, 0x80020200u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 22> GuardedControlCode{
    0xf4200400u, 0xfa000000u, 0xbf8cc07fu, 0xbf068010u, 0xbf850010u,
    0xf4040086u, 0xfa000000u, 0xbf8cc07fu,
    0xf4080101u, 0xfa000000u, 0xf4080201u, 0xfa000010u,
    0xf4000301u, 0xfa000020u, 0xbf8cc07fu,
    0xe0300000u, 0x80010200u, 0xbf8c3f70u,
    0x4a04040cu, 0xe0700000u, 0x80020200u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 16> CallerCode{
    0x7e020280u, 0xbe8e0300u, 0xbe8f0301u, 0xbe8e210eu,
    0xe0700000u, 0x80010100u, 0xbf810000u, 0xbf9f0000u,
    0x30306c73u, 0x00000040u, 0x00000008u,
};
alignas(256) constexpr std::array<std::uint32_t, 2> AddFiveCode{0x4a020285u, 0xbe80200eu};
alignas(256) constexpr std::array<std::uint32_t, 2> AddNineCode{0x4a020289u, 0xbe80200eu};

alignas(256) constexpr std::array<std::uint32_t, 9> GroupStoreCode{
    0xf4080200u, 0xfa000000u, 0xbf8cc07fu, 0x7e000202u,
    0x34020082u, 0x7e040202u, 0xe0701000u, 0x80020201u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 6> PointerWriterCode{
    0x7e000204u, 0x7e020205u, 0x7e040280u,
    0xe0740000u, 0x80000002u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 9> FaultOnlyCode{
    0xf4080100u, 0xfa000000u, 0xbf8cc07fu,
    0x7e060280u, 0x7e5002c1u, 0x7e5202c1u,
    0xe1705000u, 0x80012803u, 0xbf810000u,
};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header DataHeader{};
alignas(8) Header ShortExtentHeader{};
alignas(8) Header NestedHeader{};
alignas(8) Header ControlHeader{};
alignas(8) Header GuardedControlHeader{};
alignas(8) Header PointerWriterHeader{};
alignas(8) Header GroupStoreHeader{};
alignas(8) Header CallerHeader{};
alignas(8) Header FaultOnlyHeader{};
alignas(8) Header BdaAddressHeader{};
alignas(8) Header AheadColdHeader{};
alignas(256) std::array<std::uint32_t, 9> HostTable{};

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
        Require(block != nullptr, "dispatch gather shadow: cannot allocate guest memory");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        munmap(block, BlockBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    std::uint32_t* At(std::size_t piece, std::size_t words = 0) const { return reinterpret_cast<std::uint32_t*>(block + piece * Piece) + words; }

private:
    std::uint8_t* block = nullptr;
};

std::uint64_t Address(const void* pointer) { return reinterpret_cast<std::uintptr_t>(pointer); }
std::uint32_t Low(const void* pointer) { return static_cast<std::uint32_t>(Address(pointer)); }
std::uint32_t High(const void* pointer) { return static_cast<std::uint32_t>(Address(pointer) >> 32u); }

std::array<std::uint32_t, 4> Descriptor(const void* pointer, std::uint32_t bytes = 256) {
    return {Low(pointer), High(pointer) & 0xffffu, bytes, 0x31016facu};
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

void Dispatch(std::vector<std::uint32_t>& commands, const std::uint32_t* code, std::span<const std::uint32_t> userData, const std::uint32_t* indirect = nullptr, bool groupId = false) {
    const auto address = Address(code);
    Append(commands, 0x76, {0x20c, static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)});
    Append(commands, 0x76, {0x207, 1, 1, 1});
    Append(commands, 0x76, {0x212, 0, (static_cast<std::uint32_t>(userData.size()) << 1u) | (groupId ? 0x80u : 0u)});
    for (std::uint32_t word = 0; word < userData.size(); ++word) Append(commands, 0x76, {0x240 + word, userData[word]});
    if (indirect != nullptr) {
        commands.insert(commands.end(), {0xc0021102u, 1, Low(indirect), High(indirect)});
        Append(commands, 0x16, {0, 0x8041});
    } else {
        Append(commands, 0x15, {1, 1, 1, 0x8041});
    }
}

void Submit(std::vector<std::uint32_t> commands) {
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "dispatch gather shadow: submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

void Submit(const std::uint32_t* code, std::span<const std::uint32_t> userData) {
    std::vector<std::uint32_t> commands;
    Dispatch(commands, code, userData);
    Submit(std::move(commands));
}

void SubmitTable(const std::uint32_t* code, const std::uint32_t* table) {
    const std::array<std::uint32_t, 2> words{Low(table), High(table)};
    Submit(code, words);
}

void FillTable(std::uint32_t* table, const std::uint32_t* input, std::uint32_t* output, std::uint32_t add, std::uint32_t bytes = 256) {
    const auto in = Descriptor(input, bytes);
    const auto out = Descriptor(output);
    std::copy(in.begin(), in.end(), table);
    std::copy(out.begin(), out.end(), table + 4);
    table[8] = add;
}

void Expect(std::uint32_t* output, std::uint32_t expected, std::string_view what) {
    AgcDriver::GuestMemory::FlushGpuWrites(Address(output), sizeof(*output));
    Require(*output == expected, "dispatch gather shadow: " + std::string(what) + " produced " + std::to_string(*output) + ", expected " + std::to_string(expected));
}

struct Reads {
    struct Range { std::uint64_t address; std::size_t bytes; bool after; };
    std::array<Range, 512> ranges{};
    std::atomic<std::size_t> count{0};
    std::atomic<bool> raced{false};
    bool race = false;

    static Reads* active;
    static void Hook(std::uint64_t address, std::size_t bytes, bool after) {
        auto& reads = *active;
        const auto index = reads.count.fetch_add(1);
        if (index < reads.ranges.size()) reads.ranges[index] = {address, bytes, after};
        if (after && reads.race && !reads.raced.exchange(true)) AgcDriver::GuestMemory::MarkWritten(address, bytes);
    }
    Reads(bool race = false) : race(race) {
        active = this;
        Shadow::SetReadHook(Hook);
    }
    ~Reads() { Shadow::SetReadHook(nullptr); active = nullptr; }
    bool Saw(const void* pointer, std::size_t bytes, bool after = true) const {
        const auto begin = Address(pointer);
        for (std::size_t index = 0; index < std::min(count.load(), ranges.size()); ++index) {
            const auto& range = ranges[index];
            if (range.after == after && range.address < begin + bytes && begin < range.address + range.bytes) return true;
        }
        return false;
    }
};

Reads* Reads::active = nullptr;

void Repeat(const GuestBlock& guest, bool off) {
    auto* table = guest.At(0);
    auto* moved = guest.At(1);
    auto* input = guest.At(2);
    auto* otherInput = guest.At(3, 3);
    auto* output = guest.At(4);
    auto* otherOutput = guest.At(5, 8);
    *input = 17;
    *otherInput = 53;
    FillTable(table, input, output, 5);
    FillTable(moved, input, output, 5);
    Register(DataHeader, DataCode);
    const auto before = Shadow::ReadCounters();
    SubmitTable(DataCode.data(), table);
    Expect(output, 22, "initial key miss");
    SubmitTable(DataCode.data(), table);
    Expect(output, 22, "ordinary repeat");
    const auto cacheBeforeMove = Driver::Get().DispatchCacheCounters();
    SubmitTable(DataCode.data(), moved);
    Expect(output, 22, "moved table on key miss");
    Require(Driver::Get().DispatchCacheCounters().absent > cacheBeforeMove.absent, "dispatch gather shadow: moved root did not exercise an old-key miss");
    moved[8] = 11;
    SubmitTable(DataCode.data(), moved);
    Expect(output, 28, "changed scalar data");
    FillTable(moved, otherInput, otherOutput, 13, 192);
    *otherOutput = Sentinel;
    SubmitTable(DataCode.data(), moved);
    Expect(otherOutput, 66, "moved bases and shortened extent");
    SubmitTable(DataCode.data(), table);
    Expect(output, 22, "return to first table");
    const auto after = Shadow::ReadCounters();
    if (off) {
        Require(after.calls == before.calls && after.templatesBuilt == before.templatesBuilt && after.guardedReads == before.guardedReads && after.fallbackReads == before.fallbackReads, "dispatch gather shadow: disabled switch performed shadow work");
    } else {
        Require(after.calls == before.calls + 6, "dispatch gather shadow: calls were not counted at the real dispatch entry");
        Require(after.templatesBuilt > before.templatesBuilt && after.learnedOnMiss > before.learnedOnMiss, "dispatch gather shadow: initial key miss did not teach a template");
        Require(after.matched >= before.matched + 3 && after.gathered >= before.gathered + 3, "dispatch gather shadow: repeats did not match and gather");
        Require(after.absentMatched > before.absentMatched, "dispatch gather shadow: moved table was not matched independently of the old key");
#ifdef _WIN32
        Require(after.guardedReads > before.guardedReads, "dispatch gather shadow: arena tables did not use guarded reads");
#else
        Require(after.fallbackReads > before.fallbackReads, "dispatch gather shadow: tables did not use checked mapped reads");
#endif
        Require(after.materializedChecks >= before.materializedChecks + 3 && after.commitChecks >= before.commitChecks + 3, "dispatch gather shadow: repeated values were not checked against materialization and commit");
        Require(after.committedWithoutRebuild > before.committedWithoutRebuild, "dispatch gather shadow: no repeat committed without rebuilding");
        if (Switches::Enabled(Feature::PipeQ0Committer)) Require(after.queuedWithoutDrain > before.queuedWithoutDrain, "dispatch gather shadow: pipelined repeats did not reach the queued stage");
    }
}

void Control(const GuestBlock& guest) {
    auto* table = guest.At(0);
    auto* input = guest.At(2);
    auto* output = guest.At(4);
    auto* control = guest.At(6);
    *input = 100;
    *control = 1;
    FillTable(table, input, output, 0);
    Register(ControlHeader, ControlCode);
    std::array<std::uint32_t, 14> words{};
    const auto descriptor = Descriptor(control, 4);
    std::copy(descriptor.begin(), descriptor.end(), words.begin());
    words[12] = Low(table);
    words[13] = High(table);
    Submit(ControlCode.data(), words);
    Expect(output, 101, "independent scalar-buffer control warmup");
    {
        Reads reads;
        Submit(ControlCode.data(), words);
        Expect(output, 101, "independent scalar-buffer control repeat");
        Require(reads.Saw(control, 4), "dispatch gather shadow: independent scalar-buffer read was omitted");
        for (std::size_t word = 0; word < 8; ++word) Require(reads.Saw(table + word, 4), "dispatch gather shadow: table word was omitted");
    }
    *control = 0;
    *output = Sentinel;
    words[12] = 0x10000;
    words[13] = 0;
    {
        Reads reads;
        Submit(ControlCode.data(), words);
        Expect(output, Sentinel, "inactive resource branch");
        Require(reads.Saw(control, 4), "dispatch gather shadow: branch control was not gathered");
        Require(!reads.Saw(table, 32, false) && !reads.Saw(reinterpret_cast<const void*>(0x10000), 32, false), "dispatch gather shadow: inactive branch read its resource table");
    }
    *control = 9;
    words[12] = Low(table);
    words[13] = High(table);
    Submit(ControlCode.data(), words);
    Expect(output, 109, "reactivated resource branch");
}

void PrintDelta(std::string_view name, const Shadow::Counters& before, const Shadow::Counters& after) {
    std::cerr << name << " calls=" << after.calls - before.calls << " templates=" << after.templatesBuilt - before.templatesBuilt << " matched=" << after.matched - before.matched << " gathered=" << after.gathered - before.gathered << " ahead=" << after.aheadRefused - before.aheadRefused << " indexMiss=" << after.indexMiss - before.indexMiss << " evaluated=" << after.evaluated - before.evaluated << " pending=" << after.pending - before.pending << " queued=" << after.queued - before.queued << " serial=" << after.serialMoved - before.serialMoved << " checks=" << after.commitChecks - before.commitChecks << " refused=" << after.commitRefused - before.commitRefused;
    std::cerr << " aheadCandidates=" << after.aheadCandidates - before.aheadCandidates << " aheadAdopted=" << after.aheadAdopted - before.aheadAdopted << " aheadFallback=" << after.aheadFallback - before.aheadFallback << " faultAbi=" << after.faultAbiCalls - before.faultAbiCalls << " addressTables=" << after.addressTableCalls - before.addressTableCalls;
    for (std::size_t index = 0; index < after.graphRefusals.size(); ++index) if (after.graphRefusals[index] != before.graphRefusals[index]) std::cerr << " graph[" << index << "]=" << after.graphRefusals[index] - before.graphRefusals[index];
    for (std::size_t index = 0; index < after.differences.size(); ++index) if (after.differences[index] != before.differences[index]) std::cerr << " differ[" << index << "]=" << after.differences[index] - before.differences[index];
    for (std::size_t index = 0; index < after.commitRefusals.size(); ++index) if (after.commitRefusals[index] != before.commitRefusals[index]) std::cerr << " commit[" << index << "]=" << after.commitRefusals[index] - before.commitRefusals[index];
    std::cerr << '\n';
}

void GuardedFaultNested(const GuestBlock& guest) {
    auto* parent = guest.At(0);
    auto* child = guest.At(1);
    auto* input = guest.At(2);
    auto* output = guest.At(4);
    *input = 23;
    parent[0] = Low(child);
    parent[1] = High(child);
    FillTable(child, input, output, 7);
    Register(NestedHeader, NestedCode);
    const auto before = Shadow::ReadCounters();
    Reads reads;
    SubmitTable(NestedCode.data(), parent);
    Expect(output, 30, "guarded fault-only pointer-child warmup");
    SubmitTable(NestedCode.data(), parent);
    Expect(output, 30, "guarded fault-only pointer-child repeat");
    const auto after = Shadow::ReadCounters();
    const auto guarded = static_cast<std::size_t>(AgcDriver::DriverDetail::DispatchGatherRefusal::GuardedSlots);
    PrintDelta("guarded fault-only pointer-child admission", before, after);
    Require(after.calls == before.calls + 2 && after.addressTableCalls == before.addressTableCalls && after.faultAbiCalls == before.faultAbiCalls + 2, "dispatch gather shadow: guarded pointer-child fixture did not use fault-only ABI");
    Require(after.graphRefusals[guarded] == before.graphRefusals[guarded], "dispatch gather shadow: zero-poison guarded slots were refused");
    Require(after.templatesBuilt > before.templatesBuilt && after.matched > before.matched && after.gathered > before.gathered, "dispatch gather shadow: zero-poison guarded pointer-child shape was not admitted");
    Require(after.materializedChecks > before.materializedChecks && after.commitChecks > before.commitChecks && after.committedWithoutRebuild > before.committedWithoutRebuild, "dispatch gather shadow: guarded pointer-child repeat lacked materialization and actual reusable commit proof");
    Require(reads.Saw(parent, 8) && reads.Saw(child, 36), "dispatch gather shadow: guarded pointer-child gathering omitted its parent or child");
    const auto& classBefore = before.populations[static_cast<std::size_t>(Shadow::Population::Guarded)];
    const auto& classAfter = after.populations[static_cast<std::size_t>(Shadow::Population::Guarded)];
    Require(classAfter[static_cast<std::size_t>(Shadow::PopulationStage::Calls)] == classBefore[static_cast<std::size_t>(Shadow::PopulationStage::Calls)] + 2, "dispatch gather shadow: guarded population calls were not counted");
    for (const auto stage : {Shadow::PopulationStage::Matched, Shadow::PopulationStage::Gathered, Shadow::PopulationStage::MaterializedChecks, Shadow::PopulationStage::CommitChecks, Shadow::PopulationStage::CommittedWithoutRebuild}) {
        Require(classAfter[static_cast<std::size_t>(stage)] > classBefore[static_cast<std::size_t>(stage)], "dispatch gather shadow: guarded population stage was not counted");
    }
}

void GuardedInactive(const GuestBlock& guest) {
    auto* parent = guest.At(0);
    auto* child = guest.At(1);
    auto* input = guest.At(2);
    auto* output = guest.At(4);
    auto* control = guest.At(6);
    *input = 47;
    *control = 1;
    parent[0] = Low(child);
    parent[1] = High(child);
    FillTable(child, input, output, 7);
    std::array<std::uint32_t, 14> words{};
    const auto descriptor = Descriptor(control, 4);
    std::copy(descriptor.begin(), descriptor.end(), words.begin());
    words[12] = Low(parent);
    words[13] = High(parent);
    Register(GuardedControlHeader, GuardedControlCode);
    Submit(GuardedControlCode.data(), words);
    const auto warm = Shadow::ReadCounters();
    Submit(GuardedControlCode.data(), words);
    Expect(output, 54, "conditional guarded child warm repeat");
    const auto repeated = Shadow::ReadCounters();
    Require(repeated.gathered > warm.gathered && repeated.commitChecks > warm.commitChecks, "dispatch gather shadow: conditional guarded child was not admitted");
    *control = 0;
    parent[0] = 0x10000;
    parent[1] = 0;
    *output = Sentinel;
    {
        Reads reads;
        Submit(GuardedControlCode.data(), words);
        Expect(output, Sentinel, "inactive guarded inaccessible child");
        Require(reads.Saw(control, 4), "dispatch gather shadow: guarded branch control was not gathered");
        Require(!reads.Saw(parent, 8, false) && !reads.Saw(child, 36, false) && !reads.Saw(reinterpret_cast<const void*>(0x10000), 36, false), "dispatch gather shadow: inactive guarded child or its parent was visited");
    }
    *control = 1;
    parent[0] = Low(child);
    parent[1] = High(child);
    Submit(GuardedControlCode.data(), words);
    Expect(output, 54, "reactivated guarded child");
}

void PendingPointerParent(const GuestBlock& guest) {
    auto* parent = guest.At(0);
    auto* child = guest.At(1);
    auto* input = guest.At(2);
    auto* output = guest.At(4);
    *input = 41;
    parent[0] = Low(child);
    parent[1] = High(child);
    FillTable(child, input, output, 5);
    Register(NestedHeader, NestedCode);
    Register(PointerWriterHeader, PointerWriterCode);
    SubmitTable(NestedCode.data(), parent);
    SubmitTable(NestedCode.data(), parent);
    Expect(output, 46, "guarded pointer-parent warm repeat");
    std::array<std::uint32_t, 6> writer{};
    const auto target = Descriptor(parent, 8);
    std::copy(target.begin(), target.end(), writer.begin());
    writer[4] = Low(child);
    writer[5] = High(child);
    Submit(PointerWriterCode.data(), writer);
    const std::array<std::uint32_t, 2> consumer{Low(parent), High(parent)};
    std::vector<std::uint32_t> commands;
    Dispatch(commands, PointerWriterCode.data(), writer);
    Dispatch(commands, NestedCode.data(), consumer);
    const auto before = Shadow::ReadCounters();
    Reads reads;
    Submit(std::move(commands));
    Expect(output, 46, "pending loaded-pointer parent fallback");
    const auto after = Shadow::ReadCounters();
    PrintDelta("pending loaded-pointer parent", before, after);
    Require(reads.Saw(parent, 8, false), "dispatch gather shadow: pending loaded-pointer parent was not examined");
    Require(!reads.Saw(parent, 8), "dispatch gather shadow: pending loaded-pointer parent was read");
    Require(!reads.Saw(child, 36, false), "dispatch gather shadow: loaded-pointer child was visited before its pending parent was resolved");
    Require(after.pending + after.queued > before.pending + before.queued, "dispatch gather shadow: pending loaded-pointer parent refusal was not counted");
}

void GuardedUnmapped(const GuestBlock& guest) {
    auto* parent = guest.At(0);
    auto* child = guest.At(1);
    auto* output = guest.At(4);
    *guest.At(2) = 43;
    parent[0] = Low(child);
    parent[1] = High(child);
    FillTable(child, guest.At(2), output, 6);
    Register(NestedHeader, NestedCode);
    SubmitTable(NestedCode.data(), parent);
    SubmitTable(NestedCode.data(), parent);
    Expect(output, 49, "guarded unmapped-child warmup");
    parent[0] = 0x10000;
    parent[1] = 0;
    *output = Sentinel;
    const auto before = Shadow::ReadCounters();
    Reads reads;
    bool rejected = false;
    try {
        SubmitTable(NestedCode.data(), parent);
    } catch (const std::exception&) {
        rejected = true;
    }
    const auto after = Shadow::ReadCounters();
    PrintDelta("guarded unmapped-child refusal", before, after);
    Require(reads.Saw(parent, 8), "dispatch gather shadow: guarded unmapped-child parent was not gathered");
    Require(reads.Saw(reinterpret_cast<const void*>(0x10000), 36, false) && !reads.Saw(reinterpret_cast<const void*>(0x10000), 36), "dispatch gather shadow: unmapped guarded child was read successfully or was not examined");
    Require(after.unreadable > before.unreadable && after.matched == before.matched && after.gathered == before.gathered && after.commitChecks == before.commitChecks, "dispatch gather shadow: unmapped guarded child fabricated a proof");
    Require(rejected || *output == Sentinel, "dispatch gather shadow: unchanged path wrote through an unmapped guarded child");
}

void BdaAddress(const GuestBlock& guest) {
    auto* input = guest.At(2);
    auto* output = guest.At(4);
    *input = 23;
    Register(BdaAddressHeader, BdaAddressCode);
    const std::array<std::uint32_t, 4> words{Low(input), High(input), Low(output), High(output)};
    const auto before = Shadow::ReadCounters();
    Reads reads;
    Submit(BdaAddressCode.data(), words);
    Expect(output, 30, "address-table global access warmup");
    *input = 41;
    Submit(BdaAddressCode.data(), words);
    Expect(output, 48, "address-table global access repeat");
    const auto after = Shadow::ReadCounters();
    const auto bda = static_cast<std::size_t>(AgcDriver::DriverDetail::DispatchGatherRefusal::Bda);
    PrintDelta("address-table global access refusal", before, after);
    Require(after.calls == before.calls + 2 && after.addressTableCalls == before.addressTableCalls + 2 && after.faultAbiCalls == before.faultAbiCalls, "dispatch gather shadow: global access fixture did not issue two address-table dispatches");
    Require(after.graphRefusals[bda] == before.graphRefusals[bda] + 2, "dispatch gather shadow: address-table global accesses were not refused");
    Require(after.templatesBuilt == before.templatesBuilt && after.matched == before.matched && after.gathered == before.gathered && after.commitChecks == before.commitChecks, "dispatch gather shadow: address-table global access entered the supported population");
    Require(reads.count.load() == 0, "dispatch gather shadow: address-table refusal performed guest gathering");
}

void FaultOnly(const GuestBlock& guest) {
    auto* table = guest.At(0);
    auto* moved = guest.At(1);
    auto* output = guest.At(4);
    auto* otherOutput = guest.At(5);
    const auto first = Descriptor(output, 8);
    const auto other = Descriptor(otherOutput, 8);
    std::copy(first.begin(), first.end(), table);
    std::copy(first.begin(), first.end(), moved);
    output[0] = 0xfffffffeu;
    output[1] = 7;
    otherOutput[0] = 41;
    otherOutput[1] = 3;
    Register(FaultOnlyHeader, FaultOnlyCode);
    const auto before = Shadow::ReadCounters();
    SubmitTable(FaultOnlyCode.data(), table);
    Expect(output, 0xffffffffu, "fault-only ABI warmup low word");
    Expect(output + 1, 7, "fault-only ABI warmup high word");
    const auto warm = Shadow::ReadCounters();
    {
        Reads reads;
        SubmitTable(FaultOnlyCode.data(), table);
        Expect(output, 0, "fault-only ABI repeat low word");
        Expect(output + 1, 8, "fault-only ABI repeat high word");
        Require(reads.Saw(table, 16), "dispatch gather shadow: fault-only ABI did not gather its live descriptor");
    }
    SubmitTable(FaultOnlyCode.data(), moved);
    Expect(output, 1, "fault-only ABI moved root");
    const auto beforeMove = Shadow::ReadCounters();
    std::copy(other.begin(), other.end(), moved);
    SubmitTable(FaultOnlyCode.data(), moved);
    Expect(otherOutput, 42, "fault-only ABI moved output low word");
    Expect(otherOutput + 1, 3, "fault-only ABI moved output high word");
    Expect(output, 1, "fault-only ABI previous output remained unchanged");
    const auto afterMove = Shadow::ReadCounters();
    PrintDelta("fault-only ABI changed non-pure descriptor slot", beforeMove, afterMove);
    const auto flat = static_cast<std::size_t>(AgcDriver::DriverDetail::DispatchGatherDifference::FlatStructure);
    Require(afterMove.differences[flat] == beforeMove.differences[flat] + 1 && afterMove.matched == beforeMove.matched && afterMove.commitChecks == beforeMove.commitChecks, "dispatch gather shadow: changed non-pure fault-only descriptor slot was not conservatively refused");
    std::copy(first.begin(), first.end(), moved);
    SubmitTable(FaultOnlyCode.data(), moved);
    Expect(output, 2, "fault-only ABI recovery after changed non-pure descriptor slot");
    const auto after = Shadow::ReadCounters();
    PrintDelta("fault-only ABI", before, after);
    const auto bda = static_cast<std::size_t>(AgcDriver::DriverDetail::DispatchGatherRefusal::Bda);
    Require(after.calls == before.calls + 5 && after.faultAbiCalls == before.faultAbiCalls + 5 && after.addressTableCalls == before.addressTableCalls, "dispatch gather shadow: fault-only fixture did not issue five fault-ABI dispatches without address tables");
    Require(after.graphRefusals[bda] == before.graphRefusals[bda], "dispatch gather shadow: fault-only ABI was refused as address-table BDA");
    Require(warm.templatesBuilt > before.templatesBuilt && warm.learnedOnMiss > before.learnedOnMiss, "dispatch gather shadow: fault-only ABI warmup did not teach a template");
    Require(after.matched >= warm.matched + 3 && after.gathered >= warm.gathered + 3 && after.absentMatched > warm.absentMatched, "dispatch gather shadow: fault-only ABI repeats and moved root did not gather");
    Require(after.materializedChecks >= warm.materializedChecks + 3 && after.commitChecks >= warm.commitChecks + 3, "dispatch gather shadow: fault-only ABI was not verified through materialization and actual commit");
    Require(after.committedWithoutRebuild > warm.committedWithoutRebuild, "dispatch gather shadow: fault-only ABI did not commit a reusable template");
}

struct AheadWindow {
    static AheadWindow* active;
    std::uint32_t* table = nullptr;
    std::uint32_t* output = nullptr;
    bool structural = false;
    std::uint64_t hooks = 0;
    std::size_t readsAtHook = 0;
    Shadow::Counters atHook{};

    AheadWindow() {
        active = this;
        AgcDriverSetAheadAdoptHook_nid_postfix(Hook);
    }
    ~AheadWindow() {
        AgcDriverSetAheadAdoptHook_nid_postfix(nullptr);
        active = nullptr;
    }
    static void Hook() {
        auto& window = *active;
        ++window.hooks;
        window.atHook = Shadow::ReadCounters();
        window.readsAtHook = Reads::active == nullptr ? 0 : Reads::active->count.load();
        if (window.table != nullptr) window.table[8] = 11;
        if (window.output != nullptr) {
            const auto descriptor = Descriptor(window.output);
            std::copy(descriptor.begin(), descriptor.end(), window.table + 4);
        }
        if (window.structural) {
            window.table[1] |= 4u << 16u;
            window.table[2] = 64;
        }
    }
};

AheadWindow* AheadWindow::active = nullptr;

void Ahead(const GuestBlock& guest) {
    auto* table = guest.At(0);
    auto* input = guest.At(2);
    auto* output = guest.At(4);
    auto* otherOutput = guest.At(5);
    auto* gate = guest.At(6);
    auto* done = guest.At(6, 16);
    *input = 23;
    *gate = 1;
    FillTable(table, input, output, 7);
    Register(DataHeader, DataCode);
    const std::array<std::uint32_t, 2> userData{Low(table), High(table)};
    const auto run = [&](bool gated) {
        *done = 0;
        std::vector<std::uint32_t> commands;
        if (gated) Append(commands, 0x3c, {0x13u, Low(gate), High(gate), 1u, 0xffffffffu, 0x19u});
        Dispatch(commands, DataCode.data(), userData);
        Append(commands, 0x37, {0x00100200u, Low(done), High(done), 1u});
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitAcb(0x20u, &packet) == 0, "dispatch gather shadow: ahead compute submission failed");
        AgcDriverWaitIdle_nid_postfix();
        Require(*done == 1, "dispatch gather shadow: ahead compute submission did not finish");
    };
    const auto initial = Shadow::ReadCounters();
    run(false);
    Expect(output, 30, "ahead ordinary warmup");
    const auto warm = Shadow::ReadCounters();
    Require(warm.calls == initial.calls + 1 && warm.templatesBuilt > initial.templatesBuilt, "dispatch gather shadow: ahead ordinary warmup did not teach exactly one issued call");
    AheadWindow window;
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        const auto before = Shadow::ReadCounters();
        const auto adopted = AgcDriverDispatchesAdoptedAhead_nid_postfix();
        const auto hooks = window.hooks;
        Reads reads;
        run(true);
        Expect(output, 30, "accepted ahead repeat");
        const auto after = Shadow::ReadCounters();
        PrintDelta("accepted ahead repeat", before, after);
        Require(AgcDriverDispatchesAdoptedAhead_nid_postfix() == adopted + 1 && window.hooks == hooks + 1, "dispatch gather shadow: gated repeat did not actually adopt the ahead result");
        Require(after.calls == before.calls + 1 && after.aheadCandidates == before.aheadCandidates + 1 && after.aheadAdopted == before.aheadAdopted + 1 && after.aheadFallback == before.aheadFallback, "dispatch gather shadow: accepted ahead dispatch was duplicated or misclassified");
        Require(window.readsAtHook == 0 && window.atHook.evaluated == before.evaluated, "dispatch gather shadow: ahead preparation or pre-adoption work gathered before the unchanged adoption check");
        Require(reads.Saw(table, 36) && after.matched == before.matched + 1 && after.gathered == before.gathered + 1, "dispatch gather shadow: accepted ahead dispatch did not gather current sources");
        Require(after.materializedChecks == before.materializedChecks + 1 && after.commitChecks == before.commitChecks + 1, "dispatch gather shadow: accepted ahead dispatch was not proved through actual commit");
        Require(after.aheadRefused == before.aheadRefused, "dispatch gather shadow: accepted representable ahead call was refused");
    }
    window.table = table;
    for (const bool structural : {false, true}) {
        *output = Sentinel;
        *otherOutput = Sentinel;
        FillTable(table, input, output, 7);
        window.output = structural ? otherOutput : nullptr;
        window.structural = structural;
        const auto before = Shadow::ReadCounters();
        const auto adopted = AgcDriverDispatchesAdoptedAhead_nid_postfix();
        const auto rejected = AgcDriverDispatchesRejectedAhead_nid_postfix();
        const auto hooks = window.hooks;
        Reads reads;
        run(true);
        Expect(output, structural ? Sentinel : 34, "rejected ahead result used current scalar data and output");
        Expect(otherOutput, structural ? 34 : Sentinel, "rejected ahead result used the current output descriptor");
        const auto after = Shadow::ReadCounters();
        PrintDelta(structural ? "structurally stale ahead fallback" : "data-stale ahead fallback", before, after);
        Require(AgcDriverDispatchesAdoptedAhead_nid_postfix() == adopted && AgcDriverDispatchesRejectedAhead_nid_postfix() == rejected + 1 && window.hooks == hooks + 1, "dispatch gather shadow: changed capture was not rejected by the unchanged adoption check");
        Require(after.calls == before.calls + 1 && after.aheadCandidates == before.aheadCandidates + 1 && after.aheadAdopted == before.aheadAdopted && after.aheadFallback == before.aheadFallback + 1, "dispatch gather shadow: rejected ahead dispatch was duplicated or misclassified");
        Require(window.readsAtHook == 0 && window.atHook.evaluated == before.evaluated, "dispatch gather shadow: stale ahead capture was gathered before the adoption hook");
        Require(reads.Saw(table, 36) && after.falseMatch == before.falseMatch && after.materializedMismatch == before.materializedMismatch && after.commitMismatch == before.commitMismatch, "dispatch gather shadow: rejected ahead capture fabricated a matching proof");
        if (structural) {
            const auto descriptor = static_cast<std::size_t>(AgcDriver::DriverDetail::DispatchGatherDifference::DescriptorWords);
            Require(after.matched == before.matched, "dispatch gather shadow: structurally stale ahead result matched the old graph");
            Require(after.differences[descriptor] == before.differences[descriptor] + 1, "dispatch gather shadow: structurally stale ahead descriptor was not specifically classified");
            Require(after.aheadRefused == before.aheadRefused + 1, "dispatch gather shadow: structurally stale ahead refusal was not counted");
        } else {
            Require(after.matched == before.matched + 1 && after.materializedChecks == before.materializedChecks + 1 && after.commitChecks == before.commitChecks + 1, "dispatch gather shadow: data-stale ahead fallback did not prove its fresh gathered values");
            Require(after.aheadRefused == before.aheadRefused, "dispatch gather shadow: representable ahead fallback was refused");
        }
    }
    window.output = nullptr;
    window.table = nullptr;
    window.structural = false;
    FillTable(table, input, output, 7);
    const auto before = Shadow::ReadCounters();
    const auto adopted = AgcDriverDispatchesAdoptedAhead_nid_postfix();
    run(true);
    Expect(output, 30, "ahead recovery after rejected captures");
    const auto after = Shadow::ReadCounters();
    Require(AgcDriverDispatchesAdoptedAhead_nid_postfix() == adopted + 1 && after.calls == before.calls + 1 && after.aheadAdopted == before.aheadAdopted + 1 && after.matched == before.matched + 1 && after.commitChecks == before.commitChecks + 1, "dispatch gather shadow: ahead adoption did not recover after stale captures");
    Require(after.aheadRefused == before.aheadRefused, "dispatch gather shadow: recovered representable ahead call was refused");
}

void PendingParent(const GuestBlock& guest) {
    auto* parent = guest.At(6);
    auto* child = guest.At(0);
    auto* input = guest.At(2);
    auto* output = guest.At(4);
    *input = 23;
    *parent = 7;
    FillTable(child, input, output, 0);
    Register(ControlHeader, ControlCode);
    Register(PointerWriterHeader, PointerWriterCode);
    std::array<std::uint32_t, 14> consumer{};
    const auto source = Descriptor(parent, 4);
    std::copy(source.begin(), source.end(), consumer.begin());
    consumer[12] = Low(child);
    consumer[13] = High(child);
    const auto initial = Shadow::ReadCounters();
    Submit(ControlCode.data(), consumer);
    Expect(output, 30, "control-dependent child warmup");
    const auto warm = Shadow::ReadCounters();
    {
        Reads reads;
        Submit(ControlCode.data(), consumer);
        Expect(output, 30, "control-dependent child warm repeat");
        const auto repeated = Shadow::ReadCounters();
        PrintDelta("control-dependent child admission", initial, repeated);
        Require(repeated.templatesBuilt > initial.templatesBuilt && repeated.matched > warm.matched && repeated.gathered > warm.gathered, "dispatch gather shadow: control-dependent child warm repeat did not admit a reusable template");
        Require(reads.Saw(parent, 4) && reads.Saw(child, 32), "dispatch gather shadow: control-dependent child warm repeat did not read its parent and child");
    }
    std::array<std::uint32_t, 6> writer{};
    const auto target = Descriptor(parent, 8);
    std::copy(target.begin(), target.end(), writer.begin());
    writer[4] = 7;
    writer[5] = 0;
    Submit(PointerWriterCode.data(), writer);
    std::vector<std::uint32_t> commands;
    Dispatch(commands, PointerWriterCode.data(), writer);
    Dispatch(commands, ControlCode.data(), consumer);
    const auto before = Shadow::ReadCounters();
    Reads reads;
    Submit(std::move(commands));
    Expect(output, 30, "pending control parent fallback");
    const auto after = Shadow::ReadCounters();
    PrintDelta("pending control-dependent child", before, after);
    Require(reads.Saw(parent, 4, false), "dispatch gather shadow: pending control parent was not examined");
    Require(!reads.Saw(parent, 4), "dispatch gather shadow: pending control parent was read");
    Require(!reads.Saw(child, 32, false), "dispatch gather shadow: control-dependent child was visited before its pending parent was resolved");
    Require(after.pending + after.queued > before.pending + before.queued, "dispatch gather shadow: pending control parent refusal was not counted");
}

void Fallback(const GuestBlock& guest) {
    *guest.At(2) = 31;
    FillTable(HostTable.data(), guest.At(2), guest.At(4), 6);
    Register(DataHeader, DataCode);
    SubmitTable(DataCode.data(), HostTable.data());
    const auto before = Shadow::ReadCounters();
    Reads reads;
    SubmitTable(DataCode.data(), HostTable.data());
    Expect(guest.At(4), 37, "checked mapped fallback");
    Require(reads.Saw(HostTable.data(), sizeof(HostTable)), "dispatch gather shadow: host table was not gathered");
    const auto after = Shadow::ReadCounters();
    Require(after.fallbackReads > before.fallbackReads && after.gathered > before.gathered, "dispatch gather shadow: checked fallback did not gather the mapped host table");
}

void SerialRace(const GuestBlock& guest) {
    *guest.At(2) = 41;
    FillTable(guest.At(0), guest.At(2), guest.At(4), 3);
    Register(DataHeader, DataCode);
    SubmitTable(DataCode.data(), guest.At(0));
    const auto before = Shadow::ReadCounters();
    {
        Reads reads(true);
        SubmitTable(DataCode.data(), guest.At(0));
        Require(reads.raced.load(), "dispatch gather shadow: serial mutation hook did not run");
    }
    Expect(guest.At(4), 44, "serial-race fallback");
    const auto after = Shadow::ReadCounters();
    Require(after.serialMoved > before.serialMoved, "dispatch gather shadow: serial race was not refused");
    SubmitTable(DataCode.data(), guest.At(0));
    Expect(guest.At(4), 44, "recovery after serial race");
}

void Callee(const GuestBlock& guest) {
    auto* output = guest.At(4);
    std::array<std::uint32_t, 8> words{};
    const auto target = Descriptor(output, 4);
    std::copy(target.begin(), target.end(), words.begin() + 4);
    Register(CallerHeader, CallerCode);
    const auto before = Shadow::ReadCounters();
    Reads reads;
    for (const auto& [callee, expected] : std::array<std::pair<const std::uint32_t*, std::uint32_t>, 2>{{{AddFiveCode.data(), 5}, {AddNineCode.data(), 9}}}) {
        words[0] = Low(callee);
        words[1] = High(callee);
        *output = Sentinel;
        Submit(CallerCode.data(), words);
        Expect(output, expected, "changed external callee");
    }
    const auto after = Shadow::ReadCounters();
    Require(after.callsRefused == before.callsRefused + 2, "dispatch gather shadow: external calls were not refused before gathering");
    Require(after.templatesBuilt == before.templatesBuilt && after.matched == before.matched && after.gathered == before.gathered, "dispatch gather shadow: a caller was admitted without proving its callee identity");
    Require(reads.count.load() == 0, "dispatch gather shadow: rejected caller performed guest gathering");
}
void Structural(const GuestBlock& guest) {
    auto* table = guest.At(0);
    *guest.At(2) = 47;
    FillTable(table, guest.At(2), guest.At(4), 5);
    Register(DataHeader, DataCode);
    SubmitTable(DataCode.data(), table);
    SubmitTable(DataCode.data(), table);
    const auto before = Shadow::ReadCounters();
    table[1] |= 4u << 16u;
    table[2] = 64;
    SubmitTable(DataCode.data(), table);
    Expect(guest.At(4), 52, "descriptor stride change");
    const auto after = Shadow::ReadCounters();
    std::uint64_t beforeDifferences = 0;
    std::uint64_t afterDifferences = 0;
    for (const auto count : before.differences) beforeDifferences += count;
    for (const auto count : after.differences) afterDifferences += count;
    Require(afterDifferences > beforeDifferences, "dispatch gather shadow: structural mismatch was not classified");
    Require(after.matched == before.matched, "dispatch gather shadow: changed descriptor stride matched the old structure");
    FillTable(table, guest.At(2), guest.At(4), 5);
    SubmitTable(DataCode.data(), table);
    Expect(guest.At(4), 52, "return after structural mismatch");
    const auto disjoint = Shadow::ReadCounters();
    auto* overlappingOutput = guest.At(2, 32);
    *overlappingOutput = Sentinel;
    FillTable(table, guest.At(2), overlappingOutput, 5);
    SubmitTable(DataCode.data(), table);
    Expect(overlappingOutput, 52, "overlapping full descriptors with disjoint instruction footprints");
    const auto overlapping = Shadow::ReadCounters();
    Require(overlapping.matched == disjoint.matched, "dispatch gather shadow: changed full-descriptor alias topology matched the old structure");
    const auto alias = static_cast<std::size_t>(AgcDriver::DriverDetail::DispatchGatherDifference::Alias);
    Require(overlapping.differences[alias] > disjoint.differences[alias], "dispatch gather shadow: descriptor alias mismatch was not specifically classified");
}

void ClampedExtent(const GuestBlock& guest, bool off) {
    auto* table = guest.At(0);
    auto* input = guest.At(8) - 6;
    auto* output = guest.At(4);
    *input = 71;
    FillTable(table, input, output, 4);
    table[1] |= 16u << 16u;
    table[2] = 2;
    table[3] &= ~0x30000000u;
    Register(DataHeader, DataCode);
    SubmitTable(DataCode.data(), table);
    const auto before = Shadow::ReadCounters();
    SubmitTable(DataCode.data(), table);
    Expect(output, 75, "strided extent clamped at allocation end");
    const auto after = Shadow::ReadCounters();
    if (!off) Require(after.commitChecks > before.commitChecks, "dispatch gather shadow: clamped binding was not verified against commit");
    input = guest.At(8) - 2;
    *input = 83;
    FillTable(table, input, output, 6, 7);
    Register(ShortExtentHeader, ShortExtentCode);
    SubmitTable(ShortExtentCode.data(), table);
    const auto shortBefore = Shadow::ReadCounters();
    SubmitTable(ShortExtentCode.data(), table);
    Expect(output, 89, "non-DWORD extent at allocation end");
    if (!off) Require(Shadow::ReadCounters().commitChecks > shortBefore.commitChecks, "dispatch gather shadow: partial final DWORD binding was not verified");
}

void Indirect(const GuestBlock& guest) {
    constexpr std::uint32_t OutputWords = 8;
    auto* table = guest.At(0);
    auto* output = guest.At(4);
    auto* arguments = guest.At(6);
    auto* moved = guest.At(7);
    const auto target = Descriptor(output, OutputWords * sizeof(*output));
    std::copy(target.begin(), target.end(), table);
    Register(GroupStoreHeader, GroupStoreCode);
    const std::array<std::uint32_t, 2> userData{Low(table), High(table)};
    const auto run = [&](std::uint32_t* record, std::uint32_t groups) {
        record[0] = groups;
        record[1] = record[2] = 1;
        std::fill_n(output, OutputWords, Sentinel);
        std::vector<std::uint32_t> commands;
        Dispatch(commands, GroupStoreCode.data(), userData, record, true);
        Submit(std::move(commands));
        for (std::uint32_t word = 0; word < OutputWords; ++word) Expect(output + word, word < groups ? word : Sentinel, "CPU indirect group-indexed output");
    };
    const auto initial = Shadow::ReadCounters();
    run(arguments, 1);
    const auto before = Shadow::ReadCounters();
    PrintDelta("CPU indirect initial admission", initial, before);
    run(arguments, 3);
    run(moved, 5);
    const auto after = Shadow::ReadCounters();
    PrintDelta("CPU indirect payload", before, after);
    Require(after.matched >= before.matched + 2 && after.commitChecks >= before.commitChecks + 2, "dispatch gather shadow: CPU indirect payload was not checked at commit");
}
void SetEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

struct ControlFile {
    std::filesystem::path path;
    explicit ControlFile(bool enabled) {
        if (!enabled) return;
        path = std::filesystem::temp_directory_path() / ("agc-dispatch-gather-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
        SetEnvironment("APS5_ARCHB_GATE_RUN", "1");
        SetEnvironment("APS5_ARCHB_CONTROL_FILE", path.string());
    }
    ~ControlFile() { if (!path.empty()) std::filesystem::remove(path); }
    void Set(unsigned sequence, bool enabled) const {
        std::ofstream(path, std::ios::trunc) << sequence << " DISPATCH_GATHER_SHADOW=" << enabled << '\n';
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        Switches::PollControl();
        Require(Switches::Enabled(Feature::DispatchGatherShadow) == enabled, "dispatch gather shadow: runtime switch did not change");
    }
};

void AheadLearning(const GuestBlock& guest, const ControlFile& control, bool cached, bool faultOnly) {
    auto* table = guest.At(0);
    auto* input = guest.At(2);
    auto* output = guest.At(4);
    auto* gate = guest.At(6);
    auto* done = guest.At(6, 16);
    *input = 59;
    *gate = 1;
    FillTable(table, input, output, 7);
    Register(DataHeader, DataCode);
    SubmitTable(DataCode.data(), table);
    Expect(output, 66, "ahead learning device warmup");
    const auto* code = faultOnly ? FaultOnlyCode.data() : AheadColdCode.data();
    std::uint32_t faultExpected = 41;
    if (faultOnly) {
        const auto descriptor = Descriptor(output, 8);
        std::copy(descriptor.begin(), descriptor.end(), table);
        output[0] = faultExpected;
        output[1] = 3;
        Register(FaultOnlyHeader, FaultOnlyCode);
    } else {
        Register(AheadColdHeader, AheadColdCode);
    }
    const std::array<std::uint32_t, 2> userData{Low(table), High(table)};
    const auto run = [&](bool gated) {
        *done = 0;
        std::vector<std::uint32_t> commands;
        if (gated) Append(commands, 0x3c, {0x13u, Low(gate), High(gate), 1u, 0xffffffffu, 0x19u});
        Dispatch(commands, code, userData);
        Append(commands, 0x37, {0x00100200u, Low(done), High(done), 1u});
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitAcb(0x20u, &packet) == 0, "dispatch gather shadow: ahead learning submission failed");
        AgcDriverWaitIdle_nid_postfix();
        Require(*done == 1, "dispatch gather shadow: ahead learning submission did not finish");
        Expect(output, faultOnly ? ++faultExpected : 66, "ahead learning actual output");
        if (faultOnly) Expect(output + 1, 3, "fault-only ahead learning high output word");
    };
    if (cached) {
        control.Set(1, false);
        const auto before = Shadow::ReadCounters();
        run(false);
        const auto after = Shadow::ReadCounters();
        Require(after.calls == before.calls && after.templatesBuilt == before.templatesBuilt && after.guardedReads == before.guardedReads && after.fallbackReads == before.fallbackReads, "dispatch gather shadow: disabled ahead cache warmup performed shadow work");
        control.Set(2, true);
    }
    AheadWindow window;
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        const auto before = Shadow::ReadCounters();
        const auto adopted = AgcDriverDispatchesAdoptedAhead_nid_postfix();
        const auto hooks = window.hooks;
        Reads reads;
        run(true);
        const auto after = Shadow::ReadCounters();
        PrintDelta(cached ? "cached ahead without learned graph" : faultOnly ? "fault-only cold ahead learning" : "cold ahead learning", before, after);
        Require(AgcDriverDispatchesAdoptedAhead_nid_postfix() == adopted + 1 && window.hooks == hooks + 1, "dispatch gather shadow: ahead learning boundary did not actually adopt");
        Require(after.calls == before.calls + 1 && after.aheadCandidates == before.aheadCandidates + 1 && after.aheadAdopted == before.aheadAdopted + 1 && after.aheadFallback == before.aheadFallback, "dispatch gather shadow: ahead learning counted preparation as an issued dispatch");
        Require(window.readsAtHook == 0 && window.atHook.evaluated == before.evaluated, "dispatch gather shadow: ahead learning gathered before adoption");
        if (faultOnly) Require(after.faultAbiCalls == before.faultAbiCalls + 1 && after.addressTableCalls == before.addressTableCalls, "dispatch gather shadow: combined ahead fixture was not a fault-only ABI dispatch");
        if (cached) {
            const auto missing = static_cast<std::size_t>(AgcDriver::DriverDetail::DispatchGatherRefusal::MissingPlan);
            Require(after.graphRefusals[missing] == before.graphRefusals[missing] + 1 && after.aheadRefused == before.aheadRefused + 1, "dispatch gather shadow: cached ahead result without a learned graph was not refused for its missing plan");
            Require(after.aheadGraphRefusals[missing] == before.aheadGraphRefusals[missing] + 1 && after.aheadRefusedOther == before.aheadRefusedOther, "dispatch gather shadow: cached ahead missing-plan refusal was not cross-tabulated");
            Require(reads.count.load() == 0 && after.templatesBuilt == before.templatesBuilt && after.learnedOnMiss == before.learnedOnMiss && after.matched == before.matched && after.gathered == before.gathered && after.materializedChecks == before.materializedChecks && after.commitChecks == before.commitChecks, "dispatch gather shadow: cached ahead result without a plan fabricated a proof");
        } else {
            const auto learned = repeat == 0 ? 1u : 0u;
            Require(after.templatesBuilt == before.templatesBuilt + learned && after.learnedOnMiss == before.learnedOnMiss + learned, "dispatch gather shadow: cold adopted capture did not teach exactly one graph");
            Require(reads.Saw(table, 36) && after.matched == before.matched + 1 && after.gathered == before.gathered + 1 && after.materializedChecks == before.materializedChecks + 1 && after.commitChecks == before.commitChecks + 1 && after.aheadRefused == before.aheadRefused, "dispatch gather shadow: cold adopted capture or its repeat lacked gather and actual commit proof");
        }
    }
}

void Toggle(const GuestBlock& guest, const ControlFile& control) {
    auto* table = guest.At(0);
    *guest.At(2) = 97;
    FillTable(table, guest.At(2), guest.At(4), 3);
    Register(DataHeader, DataCode);
    SubmitTable(DataCode.data(), table);
    SubmitTable(DataCode.data(), table);
    Expect(guest.At(4), 100, "runtime switch warmup");
    const auto on = Shadow::ReadCounters();
    control.Set(1, false);
    table[8] = 7;
    SubmitTable(DataCode.data(), table);
    Expect(guest.At(4), 104, "runtime switch disabled");
    const auto off = Shadow::ReadCounters();
    Require(off.calls == on.calls && off.guardedReads == on.guardedReads && off.fallbackReads == on.fallbackReads, "dispatch gather shadow: disabled runtime switch continued shadow gathering");
    control.Set(2, true);
    SubmitTable(DataCode.data(), table);
    Expect(guest.At(4), 104, "runtime switch re-enabled with unobserved upload");
    const auto unobserved = Shadow::ReadCounters();
    Require(unobserved.calls == off.calls + 1 && unobserved.gathered > off.gathered, "dispatch gather shadow: runtime reactivation did not resume gathering");
    Require(unobserved.commitRefused > off.commitRefused && unobserved.commitChecks == off.commitChecks, "dispatch gather shadow: unchanged unobserved upload fabricated a commit proof");
    table[8] = 11;
    SubmitTable(DataCode.data(), table);
    Expect(guest.At(4), 108, "runtime switch re-enabled with new upload");
    const auto restored = Shadow::ReadCounters();
    Require(restored.calls == unobserved.calls + 1 && restored.gathered > unobserved.gathered && restored.commitChecks > unobserved.commitChecks, "dispatch gather shadow: new upload after runtime reactivation did not resume proof");
}
void Unmapped(const GuestBlock& guest) {
    *guest.At(2) = 61;
    FillTable(guest.At(0), guest.At(2), guest.At(4), 2);
    Register(DataHeader, DataCode);
    SubmitTable(DataCode.data(), guest.At(0));
    const auto before = Shadow::ReadCounters();
    bool rejected = false;
    try {
        SubmitTable(DataCode.data(), reinterpret_cast<const std::uint32_t*>(0x10000));
    } catch (const std::exception&) {
        rejected = true;
    }
    const auto after = Shadow::ReadCounters();
    Require(after.unreadable > before.unreadable, "dispatch gather shadow: unmapped gather was not safely refused");
    Require(after.guardedReads == before.guardedReads && after.fallbackReads == before.fallbackReads, "dispatch gather shadow: failed copy was counted as a successful read");
    Require(after.matched == before.matched && after.gathered == before.gathered, "dispatch gather shadow: unreadable root was accepted");
    Require(rejected, "dispatch gather shadow: unchanged path unexpectedly accepted an unmapped descriptor table");
}
}

int main(int argc, char** argv) {
    try {
        Require(argc <= 2, "dispatch gather shadow: too many arguments");
        const std::string_view mode = argc == 2 ? argv[1] : "--repeat";
        const bool off = mode == "--off" || mode == "--clamp-off";
        ControlFile control(mode == "--toggle" || mode == "--ahead-cached");
        const bool enabled = Switches::Enabled(Feature::DispatchGatherShadow);
        Require(enabled != off, "dispatch gather shadow: required activation state is absent");
        if (!off) Require(std::getenv("APS5_DISPATCH_GATHER_SHADOW_STRICT") != nullptr && std::string_view(std::getenv("APS5_DISPATCH_GATHER_SHADOW_STRICT")) == "1", "dispatch gather shadow: strict test mode was not enabled");
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            if ((mode == "--fault-only" || mode == "--ahead-fault-only") && !TargetHasCapability(device->Target(), spv::CapabilityInt64Atomics)) {
                std::cout << "skipped, the device has no shaderBufferInt64Atomics\n";
                return VulkanTestSkipped;
            }
        }
        {
            GuestBlock guest;
            try {
                if (mode == "--repeat" || mode == "--off") Repeat(guest, off);
                else if (mode == "--control") Control(guest);
                else if (mode == "--pending-parent") PendingParent(guest);
                else if (mode == "--pending-pointer-parent") PendingPointerParent(guest);
                else if (mode == "--guarded-fault-nested") GuardedFaultNested(guest);
                else if (mode == "--guarded-inactive") GuardedInactive(guest);
                else if (mode == "--guarded-unmapped") GuardedUnmapped(guest);
                else if (mode == "--bda-address") BdaAddress(guest);
                else if (mode == "--fault-only") FaultOnly(guest);
                else if (mode == "--ahead") Ahead(guest);
                else if (mode == "--ahead-cold" || mode == "--ahead-cached" || mode == "--ahead-fault-only") AheadLearning(guest, control, mode == "--ahead-cached", mode == "--ahead-fault-only");
                else if (mode == "--fallback") Fallback(guest);
                else if (mode == "--serial-race") SerialRace(guest);
                else if (mode == "--structural") Structural(guest);
                else if (mode == "--callee") Callee(guest);
                else if (mode == "--unmapped") Unmapped(guest);
                else if (mode == "--clamp" || mode == "--clamp-off") ClampedExtent(guest, off);
                else if (mode == "--indirect") Indirect(guest);
                else if (mode == "--toggle") Toggle(guest, control);
                else Require(false, "dispatch gather shadow: unknown argument");
            } catch (...) {
                Shadow::SetReadHook(nullptr);
                try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
                throw;
            }
            try { AgcDriverShutdown_nid_postfix(); } catch (...) { if (mode != "--unmapped" && mode != "--guarded-unmapped") throw; }
        }
        const auto counts = Shadow::ReadCounters();
        Require(counts.falseMatch == 0 && counts.materializedMismatch == 0 && counts.commitMismatch == 0, "dispatch gather shadow: strict verification recorded a false match or value mismatch");
        std::cout << "dispatch gather shadow calls=" << counts.calls << " templates=" << counts.templatesBuilt << " matched=" << counts.matched << " gathered=" << counts.gathered << " queued=" << counts.queuedWithoutDrain << " committed=" << counts.committedWithoutRebuild << " guarded=" << counts.guardedReads << " fallback=" << counts.fallbackReads << "\n";
        std::cout << "dispatch gather shadow " << mode << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        Shadow::SetReadHook(nullptr);
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
