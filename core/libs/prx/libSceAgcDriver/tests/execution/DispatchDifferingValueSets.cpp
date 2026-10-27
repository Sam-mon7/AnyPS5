#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
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
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::Driver;

constexpr std::array<std::uint32_t, 7> Code{0xf4080100u, 0xfa000000u, 0xbf8cc07fu, 0x7e020280u, 0xe0702000u, 0x80010100u, 0xbf810000u};
constexpr std::array<std::uint32_t, 21> ItemFreeCode{
    0xf4080100u, // s_load_dwordx4 s[4:7], s[0:1], 0 (output descriptor)
    0xfa000000u, // SMEM continuation: soffset=null, byte offset=0
    0xf4000200u, // s_load_dword s8, s[0:1], 32 (ALU selector)
    0xfa000020u, // SMEM continuation: soffset=null, byte offset=32
    0xf4000240u, // s_load_dword s9, s[0:1], 36 (flat-read guard)
    0xfa000024u, // SMEM continuation: soffset=null, byte offset=36
    0xbf8cc07fu, // s_waitcnt lgkmcnt(0)
    0xbe8c0391u, // s_mov_b32 s12, 17
    0xbf0a8408u, // s_cmp_lt_u32 s8, 4
    0xbf850001u, // s_cbranch_scc1 +1 dword (join at word 11)
    0x930c830cu, // s_mul_i32 s12, s12, 3
    0xbf068009u, // s_cmp_eq_u32 s9, 0
    0xbf850004u, // s_cbranch_scc1 +4 dwords (join at word 17)
    0xf4000280u, // s_load_dword s10, s[0:1], 40 (guarded flat slot)
    0xfa000028u, // SMEM continuation: soffset=null, byte offset=40
    0xbf8cc07fu, // s_waitcnt lgkmcnt(0)
    0x800c0a0cu, // s_add_u32 s12, s12, s10
    0x7e02020cu, // v_mov_b32 v1, s12
    0xe0700000u, // buffer_store_dword v1, off, s[4:7], 0
    0x80010100u, // MUBUF continuation: soffset=0, srsrc=1, vdata=1, vaddr unused
    0xbf810000u, // s_endpgm
};
constexpr std::array<std::uint32_t, 22> ResourceGuardCode{
    0xf4080100u, // s_load_dwordx4 s[4:7], s[0:1], 0 (output descriptor)
    0xfa000000u, // SMEM continuation: soffset=null, byte offset=0
    0xf4000200u, // s_load_dword s8, s[0:1], 48 (resource selector)
    0xfa000030u, // SMEM continuation: soffset=null, byte offset=48
    0xbf8cc07fu, // s_waitcnt lgkmcnt(0)
    0xbf068008u, // s_cmp_eq_u32 s8, 0
    0xbf840006u, // s_cbranch_scc0 +6 dwords (second resource at word 13)
    0xf4080300u, // s_load_dwordx4 s[12:15], s[0:1], 16 (first input descriptor)
    0xfa000010u, // SMEM continuation: soffset=null, byte offset=16
    0xbf8cc07fu, // s_waitcnt lgkmcnt(0)
    0xe0300000u, // buffer_load_dword v1, off, s[12:15], 0
    0x80030100u, // MUBUF continuation: soffset=0, srsrc=3, vdata=1, vaddr unused
    0xbf820005u, // s_branch +5 dwords (join at word 18)
    0xf4080300u, // s_load_dwordx4 s[12:15], s[0:1], 32 (second input descriptor)
    0xfa000020u, // SMEM continuation: soffset=null, byte offset=32
    0xbf8cc07fu, // s_waitcnt lgkmcnt(0)
    0xe0300000u, // buffer_load_dword v1, off, s[12:15], 0
    0x80030100u, // MUBUF continuation: soffset=0, srsrc=3, vdata=1, vaddr unused
    0xbf8c3f70u, // s_waitcnt vmcnt(0)
    0xe0700000u, // buffer_store_dword v1, off, s[4:7], 0
    0x80010100u, // MUBUF continuation: soffset=0, srsrc=1, vdata=1, vaddr unused
    0xbf810000u, // s_endpgm
};
constexpr std::array<std::uint32_t, 14> SharedAddressCode{
    0xf4000200u, // s_load_dword s8, s[0:1], 32 (ALU selector and descriptor offset)
    0xfa000020u, // SMEM continuation: soffset=null, byte offset=32
    0xbf8cc07fu, // s_waitcnt lgkmcnt(0)
    0xbe8c0391u, // s_mov_b32 s12, 17
    0xbf068008u, // s_cmp_eq_u32 s8, 0
    0xbf850001u, // s_cbranch_scc1 +1 dword (join at word 7)
    0x930c830cu, // s_mul_i32 s12, s12, 3
    0xf4080100u, // s_load_dwordx4 s[4:7], s[0:1], s8 (descriptor after the join)
    0x10000000u, // SMEM continuation: soffset=s8, byte offset=0
    0xbf8cc07fu, // s_waitcnt lgkmcnt(0)
    0x7e02020cu, // v_mov_b32 v1, s12
    0xe0700000u, // buffer_store_dword v1, off, s[4:7], 0
    0x80010100u, // MUBUF continuation: soffset=0, srsrc=1, vdata=1, vaddr unused
    0xbf810000u, // s_endpgm
};
constexpr std::size_t SrtOffset = 4096;
constexpr std::size_t OutputOffset = 8192;
constexpr std::size_t AlternateOutputOffset = 12288;
constexpr std::size_t InputOffset = 16384;
constexpr std::size_t AlternateInputOffset = 20480;
constexpr std::size_t BlockBytes = 65536;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        void* mapped = mmap(nullptr, BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            block = static_cast<std::uint8_t*>(mapped);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "dispatch differing value sets: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint64_t At(std::size_t offset) const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block + offset)); }
    std::uint8_t* Bytes(std::size_t offset) const { return block + offset; }
    std::uint32_t& Word(std::size_t offset) const { return *reinterpret_cast<std::uint32_t*>(block + offset); }

private:
    std::uint8_t* block = nullptr;
};

std::vector<std::uint32_t> Pm4(std::uint32_t opcode, std::initializer_list<std::uint32_t> body) {
    std::vector<std::uint32_t> words{0xc0000000u | (static_cast<std::uint32_t>(body.size() - 1) << 16u) | (opcode << 8u)};
    words.insert(words.end(), body);
    return words;
}

void WriteSrt(const GuestBlock& block, std::uint32_t records) {
    const auto address = block.At(OutputOffset);
    const std::array<std::uint32_t, 4> words{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), records, 0x00014204u};
    std::memcpy(block.Bytes(SrtOffset), words.data(), sizeof(words));
}

void Dispatch(const GuestBlock& block, std::uint32_t threads = 64) {
    const auto kernel = block.At(0);
    const auto srt = block.At(SrtOffset);
    std::vector<std::uint32_t> words;
    for (const auto& packet : {Pm4(0x76, {0x20c, static_cast<std::uint32_t>(kernel >> 8u)}), Pm4(0x76, {0x20d, static_cast<std::uint32_t>(kernel >> 40u) & 0xffu}), Pm4(0x76, {0x207, threads}), Pm4(0x76, {0x208, 1}), Pm4(0x76, {0x209, 1}), Pm4(0x76, {0x213, 2u << 1u}), Pm4(0x76, {0x240, static_cast<std::uint32_t>(srt)}), Pm4(0x76, {0x241, static_cast<std::uint32_t>(srt >> 32u)}), Pm4(0x15, {1, 1, 1, 0x41})}) {
        words.insert(words.end(), packet.begin(), packet.end());
    }
    Packet dcb{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&dcb) == 0, "dispatch differing value sets: the dispatch submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

struct Totals {
    std::uint64_t classified = 0, matched = 0, dataHits = 0;
    std::uint64_t lastClassified = 0, lastMatched = 0, lastDataHits = 0;

    void Add() {
        const auto counters = Driver::Get().DispatchCacheCounters();
        classified += counters.differingClassified >= lastClassified ? counters.differingClassified - lastClassified : counters.differingClassified;
        matched += counters.differingMatchedPrior >= lastMatched ? counters.differingMatchedPrior - lastMatched : counters.differingMatchedPrior;
        dataHits += counters.dataHits >= lastDataHits ? counters.dataHits - lastDataHits : counters.dataHits;
        lastClassified = counters.differingClassified;
        lastMatched = counters.differingMatchedPrior;
        lastDataHits = counters.dataHits;
    }
};

void AlternatingValueSets(const GuestBlock& block) {
    Totals totals;
    totals.Add();
    for (std::uint32_t dispatch = 0; dispatch < 5; ++dispatch) {
        WriteSrt(block, dispatch % 2u == 0u ? 64u : 65u);
        Dispatch(block);
        totals.Add();
    }
    std::printf("alternating value sets: %llu differing dispatches classified, %llu matched one of the last 3 value sets\n", static_cast<unsigned long long>(totals.classified), static_cast<unsigned long long>(totals.matched));
    Require(totals.classified == 4, "the four dispatches after the first were not all classified as differing (" + std::to_string(totals.classified) + ")");
    Require(totals.matched == 3, "the three dispatches bringing a value set seen before did not all match it (" + std::to_string(totals.matched) + ")");
    Require(totals.dataHits == 0, "a changing buffer descriptor word was refreshed as pure data");
}

void WriteDescriptor(const GuestBlock& block, std::size_t offset, std::size_t storage) {
    const auto address = block.At(storage);
    const std::array<std::uint32_t, 4> words{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), 4u, 0x31016facu};
    std::memcpy(block.Bytes(SrtOffset + offset), words.data(), sizeof(words));
}

void BranchPureDispatches(const GuestBlock& block, std::string_view mode) {
    const bool itemFree = mode == "--branch-pure-alu";
    const bool resourceGuard = mode == "--branch-pure-resource";
    Require(itemFree || resourceGuard || mode == "--branch-pure-address", "unknown dispatch differing value sets argument");
    const std::span<const std::uint32_t> code = itemFree ? std::span<const std::uint32_t>(ItemFreeCode) : resourceGuard ? std::span<const std::uint32_t>(ResourceGuardCode) : std::span<const std::uint32_t>(SharedAddressCode);
    std::memcpy(block.Bytes(0), code.data(), code.size_bytes());
    WriteDescriptor(block, 0, OutputOffset);
    WriteDescriptor(block, 16, resourceGuard ? InputOffset : AlternateOutputOffset);
    if (resourceGuard) WriteDescriptor(block, 32, AlternateInputOffset);
    else {
        block.Word(SrtOffset + 36) = 1;
        block.Word(SrtOffset + 40) = 7;
    }
    block.Word(InputOffset) = 0x13579bdfu;
    block.Word(AlternateInputOffset) = 0x2468ace0u;
    const char* setting = std::getenv("APS5_SRT_BRANCH_PURE_SLOTS");
    const bool enabled = setting != nullptr && std::string_view(setting) == "1";
    const bool verify = std::getenv("APS5_VERIFY_DATA_HITS") != nullptr;
    const std::array<std::uint32_t, 6> selectors = itemFree ? std::array<std::uint32_t, 6>{0, 5, 1, 6, 2, 7} : resourceGuard ? std::array<std::uint32_t, 6>{0, 1, 0, 2, 0, 1} : std::array<std::uint32_t, 6>{0, 16, 0, 16, 0, 16};
    constexpr std::uint32_t unwritten = 0xcafebabeu;
    for (std::size_t round = 0; round < selectors.size(); ++round) {
        const auto selector = selectors[round];
        block.Word(SrtOffset + (resourceGuard ? 48 : 32)) = selector;
        block.Word(OutputOffset) = unwritten;
        block.Word(AlternateOutputOffset) = unwritten;
        const auto before = Driver::Get().DispatchCacheCounters();
        Dispatch(block, 1);
        AgcDriver::GuestMemory::FlushGpuWrites(block.At(OutputOffset), sizeof(std::uint32_t));
        AgcDriver::GuestMemory::FlushGpuWrites(block.At(AlternateOutputOffset), sizeof(std::uint32_t));
        const auto after = Driver::Get().DispatchCacheCounters();
        const auto what = std::string(mode) + " switch " + (enabled ? "on" : "off") + " round " + std::to_string(round);
        const auto selectedOutput = !itemFree && !resourceGuard && selector != 0 ? AlternateOutputOffset : OutputOffset;
        const auto expected = resourceGuard ? block.Word(selector == 0 ? InputOffset : AlternateInputOffset) : (selector < (itemFree ? 4u : 1u) ? 17u : 51u) + (itemFree ? 7u : 0u);
        Require(block.Word(selectedOutput) == expected, what + ": output is " + std::to_string(block.Word(selectedOutput)) + ", expected " + std::to_string(expected));
        const auto otherOutput = selectedOutput == OutputOffset ? AlternateOutputOffset : OutputOffset;
        Require(block.Word(otherOutput) == unwritten, what + ": the unselected output was written");
        Require(after.lookups == before.lookups + 1, what + ": did not look up the dispatch cache exactly once");
        const bool dataHit = round != 0 && itemFree && enabled;
        Require(after.dataHits == before.dataHits + (dataHit ? 1u : 0u), what + ": unexpected data-hit count " + std::to_string(after.dataHits));
        Require(after.dataVerified == before.dataVerified + (dataHit && verify ? 1u : 0u), what + ": unexpected data-hit verification count");
        Require(after.inserts == before.inserts + (dataHit ? 0u : 1u), what + ": unexpected cache insert count");
        Require(after.equal == before.equal + (dataHit ? 1u : 0u), what + ": unexpected cache hit count");
        Require(after.differing == before.differing + (round != 0 && !dataHit ? 1u : 0u), what + ": a changed resource word did not miss");
        Require(after.dataWordsRefreshed == before.dataWordsRefreshed + (dataHit ? 1u : 0u), what + ": unexpected refreshed word count");
    }
    std::printf("%.*s switch %s: six outputs checked, five %s\n", static_cast<int>(mode.size()), mode.data(), enabled ? "on" : "off", itemFree && enabled ? "data hits" : "resource revalidations");
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 1 || argc == 2, "unexpected dispatch differing value sets arguments");
#ifdef _WIN32
        _putenv_s("APS5_PROFILE_DRAW", argc == 1 ? "1" : "");
        _putenv_s("APS5_DISPATCH_VARIANTS", "1");
#else
        if (argc == 1) setenv("APS5_PROFILE_DRAW", "1", 1);
        else unsetenv("APS5_PROFILE_DRAW");
        setenv("APS5_DISPATCH_VARIANTS", "1", 1);
#endif
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        GuestBlock block;
        if (argc == 2) BranchPureDispatches(block, argv[1]);
        else {
            std::memcpy(block.Bytes(0), Code.data(), sizeof(Code));
            AlternatingValueSets(block);
        }
        AgcDriverShutdown_nid_postfix();
        std::puts("dispatch differing value sets tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
