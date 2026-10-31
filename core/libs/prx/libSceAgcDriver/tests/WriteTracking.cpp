#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

namespace {

using namespace AgcDriver::GuestMemory;

constexpr std::size_t Block = 65536;
#ifdef _WIN32
constexpr wchar_t HostHoleMode[] = L"--host-hole";
constexpr wchar_t ResidentRemapMode[] = L"--resident-direct-remap";
constexpr wchar_t RewatchImportPagesMode[] = L"--rewatch-import-pages";
constexpr wchar_t RewatchedSharedMode[] = L"--rewatched-direct-shared";
constexpr wchar_t ImportAliasSharedMode[] = L"--import-alias-shared";
constexpr wchar_t SharedGenerationsMode[] = L"--compared-shared-generations";
#endif

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

#ifdef _WIN32
bool CheckMetadataCommit() {
    std::uintptr_t arena = 0;
    std::size_t bytes = 0;
    GuestArena::GuestArenaRange_nid_postfix(&arena, &bytes);
    if (bytes == 0 || !GuestArena::GuestArenaWriteWatched_nid_postfix()) return false;
    PROCESS_MEMORY_COUNTERS_EX before{};
    PROCESS_MEMORY_COUNTERS_EX after{};
    Require(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before)), "cannot measure private commit before tracking");
    Require(WriteWatched(), "the arena is not write watched");
    Require(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof(after)), "cannot measure private commit after tracking");
    const auto committed = after.PrivateUsage > before.PrivateUsage ? after.PrivateUsage - before.PrivateUsage : 0;
    std::cout << "write-tracker initialization: " << committed / 1024 << " KiB private commit for " << (bytes >> 30u) << " GiB guest arena\n";
    Require(committed < (64u << 20u), "write tracking committed dense generation tables for untouched guest ranges");
    const auto generation = TrackerGeneration();
    for (std::uint64_t offset = 0; offset + Block <= bytes; offset += std::uint64_t{1} << 32u) {
        Require(NewestStamp(arena + offset, Block) == 0 && UnchangedSince(arena + offset, Block, generation), "an untouched leaf has a write stamp");
    }
    PROCESS_MEMORY_COUNTERS_EX queried{};
    Require(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&queried), sizeof(queried)), "cannot measure private commit after empty-range queries");
    Require(queried.PrivateUsage < after.PrivateUsage + (4u << 20u), "queries allocated untouched tracking leaves");
    return true;
}

void CheckSparseLeafBoundaries() {
    constexpr std::uintptr_t LeafBytes = std::uintptr_t{1} << 32;
    std::uintptr_t arena = 0;
    std::size_t arenaBytes = 0;
    GuestArena::GuestArenaRange_nid_postfix(&arena, &arenaBytes);
    const auto boundary = arena + (arenaBytes / (2 * LeafBytes)) * LeafBytes;
    constexpr std::size_t bytes = 4 * Block;
    void* memory = GuestArena::GuestArenaAllocateAtOrAbove_nid_postfix(boundary - Block, bytes, Block);
    struct Release {
        void* memory;
        ~Release() {
            GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
        }
    } release{memory};
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    Require(base == boundary - Block, "the sparse tracking fixture did not straddle its 4 GiB leaf boundary");
    GuestArena::GuestArenaCommit_nid_postfix(memory, bytes, PAGE_READWRITE, bytes);
    std::memset(memory, 0x11, bytes);
    BumpCollectEpoch();
    const auto collected = CollectWrites(base, bytes);
    Require(collected != 0, "the cross-leaf range was not collected");
    MarkWritten(boundary, 4);
    Require(UnchangedSince(base, Block, collected) && !UnchangedSince(base, bytes, collected), "a driver store across a leaf boundary did not stamp exactly its block");
    Require(UnchangedSinceCollected(boundary, Block, collected), "the cross-leaf driver store became a CPU store");
    static_cast<volatile std::uint8_t*>(memory)[0] = 0x22;
    CollectWritesUncached(base, bytes);
    Require(!UnchangedSinceCollected(base, Block, collected), "a CPU store before the leaf boundary was missed");
    Require(UnchangedSinceCollected(boundary, 3 * Block, collected), "the CPU store stamped the other leaf");
    Unwatch(base, bytes);
    const auto hint = UntrackedHint(base, bytes);
    Require(hint != 0, "the cross-leaf untracked range has no hint");
    static_cast<volatile std::uint8_t*>(memory)[0] = 0x33;
    static_cast<volatile std::uint8_t*>(memory)[3 * Block] = 0x44;
    BumpCollectEpoch();
    const auto changed = UntrackedHint(base, bytes);
    Require(changed > hint, "cross-leaf CPU stores did not change their untracked hint");
    Require(UntrackedHint(base, Block) == changed && UntrackedHint(base + 3 * Block, Block) == changed, "a cross-leaf hint store was missed");
    Require(UntrackedHint(boundary, 2 * Block) == hint, "hint stores changed untouched blocks in the other leaf");
}
#endif

void* AllocateWatched(std::size_t bytes, bool crossLeaf = false) {
#ifdef _WIN32
    static_cast<void>(crossLeaf);
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block);
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    const std::size_t alignment = crossLeaf ? std::size_t{1} << 32 : Block;
    const auto prefix = crossLeaf ? Block : 0;
    void* raw = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) throw std::runtime_error("cannot map the watched block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = ((begin + prefix + alignment - 1) & ~static_cast<std::uintptr_t>(alignment - 1)) - prefix;
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + alignment) munmap(reinterpret_cast<void*>(aligned + bytes), begin + alignment - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    Require(Watched(reinterpret_cast<std::uint64_t>(block), bytes), "the test block is not watched");
    return block;
}

void CheckSharedBlock() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto boundary = base + 2 * Block - 0xb800;
    const auto firstBegin = base;
    const auto secondEnd = base + 3 * Block;
    const auto sharedFirst = boundary - (base + Block);
    const auto sharedSecond = base + 2 * Block - boundary;
    std::memset(memory, 0x11, 3 * Block);

    const auto synced = CollectWrites(base, 3 * Block);
    Require(synced != 0, "the test block is not collected");

    Require(MarkWritten(firstBegin, static_cast<std::size_t>(boundary - firstBegin)) != 0, "a driver store into watched memory is not stamped");
    std::array<std::uint64_t, 2> generations{synced, synced};
    std::array<std::uint8_t, 2> changed{};
    Require(ChangedBlocks(base + Block, 2 * Block, generations, changed) && changed[0] == BlockWritten && changed[1] == BlockUnchanged, "the shared block does not read as written");
    Require(!StoredOver(boundary, static_cast<std::size_t>(sharedSecond), synced), "a store of the neighbour's bytes counts for the second surface's part of the shared block");
    Require(StoredOver(base + Block, static_cast<std::size_t>(sharedFirst), synced), "the first surface's own store is not seen over its part of the shared block");
    Require(!StoredOver(boundary, static_cast<std::size_t>(secondEnd - boundary), synced), "the second surface reads as stored over");

    const auto touched = MarkWritten(boundary + 0x100, 4);
    Require(StoredOver(boundary, static_cast<std::size_t>(sharedSecond), synced), "a store over the second surface's bytes is not seen");
    Require(!StoredOver(boundary, static_cast<std::size_t>(sharedSecond), touched), "a store older than the generation is seen");

    const auto whole = TrackerGeneration();
    MarkWritten(base + Block, Block);
    Require(StoredOver(boundary, static_cast<std::size_t>(sharedSecond), whole), "a store over the whole block is not seen");
    const auto afterWhole = TrackerGeneration();
    MarkWritten(base + Block, static_cast<std::size_t>(sharedFirst));
    Require(!StoredOver(boundary, static_cast<std::size_t>(sharedSecond), afterWhole), "a partial store after a whole-block store is taken for the whole block");

    const auto many = TrackerGeneration();
    for (int store = 0; store < 6; ++store) MarkWritten(base + Block + static_cast<std::uint64_t>(store) * 64, 4);
    Require(StoredOver(boundary, static_cast<std::size_t>(sharedSecond), many), "forgotten driver stores are taken as missing the range");

    const auto beforeCpu = CollectWrites(base, 3 * Block);
    static_cast<volatile std::uint8_t*>(memory)[Block + 8] = 0x22;
    CollectWritesUncached(base, 3 * Block);
    Require(!StoredOver(boundary, static_cast<std::size_t>(sharedSecond), beforeCpu), "a CPU store outside the shared range counts for it");

    std::array<std::uint8_t, 64> unwatched{};
    const auto outside = reinterpret_cast<std::uint64_t>(unwatched.data());
    if (!Watched(outside, unwatched.size())) Require(StoredOver(outside, unwatched.size(), TrackerGeneration()), "an unwatched range reads as not stored over");
}

void CheckStoredOverUsesPageEvidence() {
    constexpr std::size_t Page = 4096;
    auto* memory = static_cast<std::uint8_t*>(AllocateWatched(2 * Block));
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x11, 2 * Block);
    const auto before = CollectWritesUncached(base, 2 * Block);
    Require(before != 0, "the stored-over page fixture was not collected");

    static_cast<volatile std::uint8_t*>(memory)[0] = 0x22;
    const auto after = CollectWritesUncached(base, Block);
    Require(after > before, "the page-local CPU write was not collected");
    Require(StoredOver(base, Page, before), "a collected CPU write on a page was not reported as stored over");
    Require(!StoredOver(base + Page, Page, before), "a page-local CPU write made an untouched page in the same block count as stored over");
}

void CheckStoredAllSince() {
    constexpr std::size_t bytes = 4 * Block;
    auto* memory = static_cast<std::uint8_t*>(AllocateWatched(bytes));
    struct Cleanup {
        void* memory;
        ~Cleanup() {
#ifdef _WIN32
            GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
#else
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(memory, bytes);
            munmap(memory, bytes);
#endif
        }
    } cleanup{memory};
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x11, bytes);
    auto before = CollectWritesUncached(base, bytes);
    Require(before != 0, "the complete-store block is not collected");
    Require(!StoredAllSince_nid_no_patch(base, 64, before), "a range with no driver store has complete coverage");

    MarkWritten(base + Block, Block);
    Require(StoredAllSince_nid_no_patch(base + Block, Block, before), "the first whole-block driver store has no complete coverage");
    Require(StoredAllSince_nid_no_patch(base + Block + 1, Block - 2, before), "a whole-block store lost its interior coverage");
    const auto whole = TrackerGeneration();
    Require(!StoredAllSince_nid_no_patch(base + Block, Block, whole), "a whole-block store at the baseline is taken as newer");

    before = TrackerGeneration();
    static_cast<volatile std::uint8_t*>(memory)[2 * Block + 7] = 0x22;
    CollectWritesUncached(base + 2 * Block, Block);
    Require(!StoredAllSince_nid_no_patch(base + 2 * Block, Block, before), "CPU-only stores prove complete driver coverage");

    before = TrackerGeneration();
    const std::array<std::byte, 4> dword{};
    Write(base + 16, dword);
    Require(StoredAllSince_nid_no_patch(base + 16, dword.size(), before), "a tracked dword store is absent from its exact range");
    Require(!StoredAllSince_nid_no_patch(base, 64, before), "one dword store proves complete surface coverage");
    Require(!StoredAllSince_nid_no_patch(base + 15, 5, before) && !StoredAllSince_nid_no_patch(base + 16, 5, before), "a dword store covers an unstored boundary byte");
    const auto dwordStored = TrackerGeneration();
    Require(!StoredAllSince_nid_no_patch(base + 16, dword.size(), dwordStored), "a dword store at the baseline is taken as newer");
    MarkWritten(base, 64);
    Require(StoredAllSince_nid_no_patch(base, 64, before), "a complete non-block-aligned range store is absent");

    before = TrackerGeneration();
    MarkPossiblyWritten(base + 2048, 16);
    Require(StoredOver(base + 2048, 16, before), "a possible driver write did not invalidate its exact range");
    Require(!StoredOver(base + 2064, 16, before), "a possible driver write invalidated an adjacent range");
    Require(!UnchangedSince(base + 2048, 16, before), "a possible driver write left its exact range unchanged");
    Require(UnchangedSince(base + 2064, 16, before), "a possible driver write changed an adjacent range");
    Require(!StoredAllSince_nid_no_patch(base + 2048, 16, before), "a possible driver write proved complete byte coverage");
    for (unsigned repeat = 0; repeat < 6; ++repeat) MarkPossiblyWritten(base + 2048, 16);
    Require(StoredOver(base + 2048, 16, before), "repeated possible writes lost their overlap evidence");
    Require(!StoredOver(base + 2064, 16, before), "repeated possible writes invalidated an adjacent range");
    Require(!StoredAllSince_nid_no_patch(base + 2048, 16, before), "repeated possible writes proved complete byte coverage");

    before = TrackerGeneration();
    constexpr unsigned PossibleIntervals = 8;
    auto beforeNewest = before;
    for (unsigned interval = 0; interval < PossibleIntervals; ++interval) {
        beforeNewest = TrackerGeneration();
        MarkPossiblyWritten(base + 4096 + static_cast<std::uint64_t>(interval) * 32, 16);
    }
    Require(StoredOver(base + 4096, 16, before), "an evicted possible-write interval was forgotten instead of marking its block");
    Require(StoredOver(base + 4096 + 16, 16, before), "an evicted possible write did not mark the rest of its block");
    Require(StoredOver(base + 4096 + (PossibleIntervals - 1) * 32, 16, beforeNewest), "the newest possible-write interval was evicted");
    const auto beforeRecord = TrackerGeneration();
    MarkPossiblyWritten(base + 4096, 16);
    Require(StoredOver(base + 4096, 16, beforeRecord), "a possible-write interval was not retained after being re-recorded");
    Require(!StoredAllSince_nid_no_patch(base + 4096, 16, before), "possible-write eviction or re-recording proved complete byte coverage");

    before = TrackerGeneration();
    MarkWritten(base + 2560, 16);
    MarkPossiblyWritten(base, Block);
    Require(StoredAllSince_nid_no_patch(base + 2560, 16, before), "a possible whole-block range erased an exact partial-store proof");
    Require(!StoredAllSince_nid_no_patch(base, Block, before), "a possible whole-block range proved complete block coverage");

    before = TrackerGeneration();
    MarkWritten(base + 512, 16);
    MarkWritten(base + 544, 16);
    Require(!StoredAllSince_nid_no_patch(base + 512, 48, before), "split driver stores prove coverage across a hole");
    MarkWritten(base + 528, 16);
    Require(StoredAllSince_nid_no_patch(base + 512, 48, before), "out-of-order split driver stores did not form complete coverage");
    Require(!StoredAllSince_nid_no_patch(base + 512, 49, before), "split coverage includes an unstored final byte");

    before = TrackerGeneration();
    MarkWritten(base + 1024 + 8, 24);
    MarkWritten(base + 1024, 16);
    Require(StoredAllSince_nid_no_patch(base + 1024, 32, before), "overlapping driver intervals did not form complete coverage");

    before = TrackerGeneration();
    MarkWritten(base + Block - 4, 8);
    Require(StoredAllSince_nid_no_patch(base + Block - 4, 8, before), "a tracked store across a block boundary lost coverage");
    Require(!StoredAllSince_nid_no_patch(base + Block - 5, 9, before) && !StoredAllSince_nid_no_patch(base + Block - 4, 9, before), "a boundary store covers an unstored adjacent byte");

    before = TrackerGeneration();
    for (unsigned piece = 0; piece < 6; ++piece) MarkWritten(base + static_cast<std::uint64_t>(piece) * 64, 4);
    Require(!StoredAllSince_nid_no_patch(base, 4, before), "dropped driver evidence proves complete coverage");
    MarkWritten(base, Block);
    Require(StoredAllSince_nid_no_patch(base, Block, before), "a new whole-block store did not replace dropped evidence");

    before = TrackerGeneration();
    MarkWritten(base, 3 * Block);
    Require(StoredAllSince_nid_no_patch(base, 3 * Block, before), "complete multi-block stores lost coverage");
    Require(StoredAllSince_nid_no_patch(base + 1, 3 * Block - 2, before) && StoredAllSince_nid_no_patch(base + 3 * Block - 1, 1, before), "complete multi-block stores lost edge bytes");
    const auto complete = TrackerGeneration();
    Require(!StoredAllSince_nid_no_patch(base, 3 * Block, complete), "stores older than the baseline prove complete coverage");

    before = TrackerGeneration();
    MarkWritten(base + 2 * Block, Block);
    const auto driverStored = TrackerGeneration();
    static_cast<volatile std::uint8_t*>(memory)[2 * Block + 7] = 0x33;
    CollectWritesUncached(base + 2 * Block, Block);
    Require(StoredAllSince_nid_no_patch(base + 2 * Block, Block, before), "a later CPU store erased the proof that the driver stored every byte");
    Require(!UnchangedSinceCollected(base + 2 * Block, Block, driverStored), "the separate CPU-write check missed a store after complete driver coverage");

    Require(!StoredAllSince_nid_no_patch(base, 0, before) && !StoredAllSince_nid_no_patch(base, 1, 0), "empty or unknown-baseline complete coverage is accepted");
    Require(!StoredAllSince_nid_no_patch(std::numeric_limits<std::uint64_t>::max(), 2, before), "overflowing complete coverage is accepted");
    Unwatch(base + 3 * Block, Block);
    Require(MarkWritten(base + 3 * Block, Block) == 0, "an unwatched whole store acquired trusted driver coverage");
    Require(!StoredAllSince_nid_no_patch(base + 3 * Block, Block, before), "unwatched stores prove complete coverage");
    Require(!StoredAllSince_nid_no_patch(base + 3 * Block - 1, 2, before), "a range entering unwatched memory has complete coverage");
    std::array<std::uint8_t, 16> outside{};
    const auto outsideAddress = reinterpret_cast<std::uint64_t>(outside.data());
    if (!Watched(outsideAddress, outside.size())) {
        MarkWritten(outsideAddress, outside.size());
        Require(!StoredAllSince_nid_no_patch(outsideAddress, outside.size(), before), "an outside untracked range has complete coverage");
    }
}

void CheckSharedBlockProofs() {
    constexpr std::size_t page = 4096;
    void* memory = AllocateWatched(2 * Block);
    auto* guest = static_cast<volatile std::uint8_t*>(memory);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto range = base + Block + 8 * page;
    constexpr std::size_t rangeBytes = 4 * page;
    std::memset(memory, 0x11, 2 * Block);
    const auto synced = CollectWrites(base, 2 * Block);
    Require(synced != 0, "the proof blocks are not collected");

    Require(MarkWritten(base + Block, 64) != 0, "a driver store into watched memory is not stamped");
    guest[Block + 2 * page + 5] = 0x22;
    CollectWritesUncached(base, 2 * Block);
    Require(UnchangedSince(range, rangeBytes, synced), "stores beside the range in its 64 KiB block count for it");
    const std::array<UnchangedQuery, 2> beside{{{range, rangeBytes, synced}, {base + Block + 4 * page, page, synced}}};
    Require(UnchangedSinceAll(beside), "a batch over ranges beside the stores reads as changed");

    const auto beforeCpu = TrackerGeneration();
    guest[Block + 9 * page + 5] = 0x33;
    CollectWritesUncached(base, 2 * Block);
    Require(!UnchangedSince(range, rangeBytes, beforeCpu), "a CPU store inside the range is not seen");
    const std::array<UnchangedQuery, 2> mixed{{{base + Block + 4 * page, page, beforeCpu}, {range, rangeBytes, beforeCpu}}};
    Require(!UnchangedSinceAll(mixed), "a batch with a range written by the CPU reads as unchanged");

    const auto beforeDriver = TrackerGeneration();
    MarkWritten(range + 100, 4);
    Require(!UnchangedSince(range, rangeBytes, beforeDriver), "a driver store inside the range is not seen");

    const auto beforeWhole = TrackerGeneration();
    MarkWritten(base + Block, Block);
    Require(!UnchangedSince(range, rangeBytes, beforeWhole), "a store over the whole block is not seen");

    const auto beforeOwn = TrackerGeneration();
    const auto stored = base + Block + 12 * page + 0x100;
    StoreOwnBytes(stored, 64, [&] {
        std::memset(reinterpret_cast<void*>(stored), 0x55, 64);
        guest[Block + 12 * page + 0x800] = 0x66;
    });
    Require(!UnchangedSince(base + Block + 12 * page + 0x800, 16, beforeOwn), "a guest store beside a driver store in its page during the store is not seen");
    Require(UnchangedSince(base + Block + 4 * page, page, beforeOwn), "a driver store counts for another page of its block");

    const auto beforeNoRecord = TrackerGeneration();
    guest[2 * page + 5] = 0x44;
    CollectWritesUncached(base, 2 * Block);
    Require(UnchangedSince(base + 8 * page, page, beforeNoRecord), "a CPU store beside the range in a block without driver stores counts for it");

    const auto beforeFirstWhole = TrackerGeneration();
    MarkWritten(base, Block);
    Require(!UnchangedSince(base + 8 * page, page, beforeFirstWhole), "a whole-block store in a block without earlier driver stores is not seen");
    MarkWritten(base, 64);
    Require(!UnchangedSince(base + 8 * page, page, beforeFirstWhole), "a partial store after a whole-block store hides it");

    const auto beforeSpan = TrackerGeneration();
    const std::vector<std::byte> span(6 * page, std::byte{0x77});
    Write(base + 2 * page, span);
    Require(!UnchangedSince(base + 4 * page, page, beforeSpan), "a store over several pages of a block is not seen");
    Require(UnchangedSince(base + 12 * page, page, beforeSpan), "a store over several pages of a block counts for the rest of it");

    const auto beforeExact = TrackerGeneration();
    const std::vector<std::byte> nearlyWhole(Block - 512, std::byte{0x5a});
    Write(base + Block + 256, nearlyWhole);
    Require(StoredAllSince_nid_no_patch(base + Block + 256, nearlyWhole.size(), beforeExact), "an own store lost its exact coverage");
    Require(!StoredAllSince_nid_no_patch(base + Block, Block, beforeExact), "the dirty pages of an own store prove its whole block stored");
    Require(!StoredAllSince_nid_no_patch(base + Block, 256, beforeExact), "the dirty first page of an own store proves the bytes before the store stored");

    const auto beforeClear = TrackerGeneration();
    MarkWritten(base + Block + 0x100, 64);
    const std::array<std::byte, 64> label{};
    for (const std::uint64_t at : {4u, 6u, 10u}) Write(base + Block + at * page + 0x200, label);
    Require(StoredAllSince_nid_no_patch(base + Block + 0x100, 64, beforeClear), "own stores elsewhere in the block forget a driver store's complete coverage");

    const auto beforeNeighbour = TrackerGeneration();
    Write(base + Block + 14 * page + 0x100, label);
    Require(!StoredOver(base + Block + 14 * page + 0x800, 16, beforeNeighbour), "a neighbour in the page of an own store reads as stored over");
    Require(!UnchangedSince(base + Block + 14 * page + 0x800, 16, beforeNeighbour), "a range in the page an own store dirtied reads as unchanged");
}

void CheckRepeatedPossibleStores() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto boundary = base + 2 * Block - 0x9000;
    std::memset(memory, 0x11, 3 * Block);

    const auto synced = CollectWrites(base, 3 * Block);
    Require(synced != 0, "the test block is not collected");

    const auto exact = TrackerGeneration();
    MarkWritten(base + Block, 0x100);
    for (std::uint64_t store = 0; store < 5; ++store) MarkPossiblyWritten(boundary + store * 0x1000, 0x100);
    Require(StoredAllSince_nid_no_patch(base + Block, 0x100, exact), "possible writes beyond the ring forgot an exact store's coverage");

    void* fresh = AllocateWatched(2 * Block);
    const auto freshBase = reinterpret_cast<std::uint64_t>(fresh);
    std::memset(fresh, 0x11, 2 * Block);
    const auto freshSynced = CollectWrites(freshBase, 2 * Block);
    const auto neighbour = freshBase + Block - 0x9000;
    MarkPossiblyWritten(neighbour, 0x100);
    const auto secondStored = MarkPossiblyWritten(neighbour + 0x1000, 0x100);
    const auto thirdStored = MarkPossiblyWritten(neighbour + 0x2000, 0x100);
    MarkPossiblyWritten(neighbour + 0x3000, 0x100);
    const auto settled = TrackerGeneration();
    MarkPossiblyWritten(neighbour, 0x100);
    MarkPossiblyWritten(neighbour, 0x100);
    MarkPossiblyWritten(neighbour + 0x5000, 0x100);
    Require(!StoredOver(freshBase, static_cast<std::size_t>(neighbour - freshBase), secondStored), "a full possible ring evicted a newer piece instead of its oldest");
    Require(StoredOver(freshBase, static_cast<std::size_t>(neighbour - freshBase), freshSynced), "an evicted possible write is not taken as a store over the rest of the block");

    const auto partial = TrackerGeneration();
    MarkPossiblyWritten(neighbour + 0x5080, 0x100);
    Require(StoredOver(neighbour + 0x5000, 0x10, settled) && StoredOver(neighbour + 0x5100, 0x10, partial), "a possible write that overlaps an earlier piece without containing it replaced it");
    Require(!StoredOver(freshBase, static_cast<std::size_t>(neighbour - freshBase), thirdStored), "a possible write that overlaps without containing evicted a newer piece instead of the oldest");
}

void CheckOwnStore() {
    void* memory = AllocateWatched(2 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto second = base + Block;
    std::memset(memory, 0x11, 2 * Block);
    const auto synced = CollectWrites(base, 2 * Block);
    Require(synced != 0, "the own-store block is not collected");

    std::array<std::byte, 8192> bytes{};
    bytes.fill(std::byte{0x44});
    Write(second, bytes);
    const auto stored = TrackerGeneration();
    Require(!UnchangedSince(second, bytes.size(), synced), "the driver store is not stamped");
    CollectWritesUncached(base, 2 * Block);
    Require(UnchangedSince(second, Block, stored), "a walk after a driver store reports the store again as a newer write");
    Require(UnchangedSinceCollected(second, bytes.size(), synced), "the driver store's own page faults are stamped as a CPU write");

    static_cast<volatile std::uint8_t*>(memory)[Block + 3 * 4096 + 8] = 0x22;
    const std::array<std::byte, 64> label{};
    Write(second + 3 * 4096 + 64, label);
    Require(!UnchangedSinceCollected(second + 3 * 4096, 4096, stored), "a CPU write before a driver store in its page is not stamped as one");

    const auto beforeCpu = TrackerGeneration();
    static_cast<volatile std::uint8_t*>(memory)[16] = 0x33;
    CollectWritesUncached(base, 2 * Block);
    Require(!UnchangedSince(base, 64, beforeCpu), "a CPU write after the driver store is not seen");
}

void CheckUnwatch() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto imported = base + Block;
    std::memset(memory, 0x11, 3 * Block);
    BumpCollectEpoch();
    const auto before = CollectWrites(base, 3 * Block);
    Require(before != 0 && UnchangedSince(imported, Block, before), "the import range was not initially watched");
    Unwatch(imported, Block);
    Require(!Watched(imported, Block) && !UnchangedSince(imported, Block, before), "unwatch kept trusting import stamps");
    Require(CollectWrites(base, 3 * Block) == 0 && CollectWritesUncached(imported, Block) == 0, "unwatch did not invalidate a cached collect");
    Require(Watched(base, Block) && Watched(base + 2 * Block, Block), "unwatch disabled unrelated memory");
    const auto beforeDriverWrite = TrackerGeneration();
    Require(MarkWritten(imported, Block) == 0, "an unwatched import still produces trusted stamps");
    Require(TrackerGeneration() > beforeDriverWrite, "an unwatched driver write did not invalidate byte comparisons");
    const auto beforeOwnWrite = TrackerGeneration();
    StoreOwnBytes(imported, 1, [&] { *reinterpret_cast<std::uint8_t*>(imported) = 0x42; });
    Require(TrackerGeneration() > beforeOwnWrite, "an unwatched CPU store did not invalidate byte comparisons");
    std::array<std::uint64_t, 1> generations{before};
    std::array<std::uint8_t, 1> changed{};
    Require(!ChangedBlocks(imported, Block, generations, changed), "an unwatched import still uses block stamps");
    Unwatch(imported, Block);
    const auto adjacent = CollectWrites(base, Block);
    Require(adjacent != 0, "unwatch broke the adjacent block's collect");
    static_cast<volatile std::uint8_t*>(memory)[0] = 0x33;
    CollectWritesUncached(base, Block);
    Require(!UnchangedSince(base, Block, adjacent), "unwatch hid a CPU edit in unrelated memory");
}

void CheckNewestStamp() {
    void* memory = AllocateWatched(4 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto first = base + Block;
    const auto second = base + 2 * Block;
    std::memset(memory, 0x11, 4 * Block);
    BumpCollectEpoch();
    CollectWrites(base, 4 * Block);
    Unwatch(first, 2 * Block);
    const auto firstBefore = NewestStamp(first, Block);
    const auto secondBefore = NewestStamp(second, Block);
    MarkWritten(second + 16, 4);
    Require(NewestStamp(second, Block) > secondBefore, "a driver store into unwatched memory did not advance its stamp");
    Require(NewestStamp(first, Block) == firstBefore, "a driver store advanced the stamp of other unwatched memory");
    const auto generation = TrackerGeneration();
    static_cast<volatile std::uint8_t*>(memory)[0] = 0x22;
    CollectWritesUncached(base, Block);
    Require(TrackerGeneration() > generation && NewestStamp(first, Block) == firstBefore, "a CPU store into watched memory advanced the stamp of unwatched memory");
    StoreOwnBytes(first, 1, [&] { *reinterpret_cast<std::uint8_t*>(first) = 0x42; });
    Require(NewestStamp(first, Block) > firstBefore, "an own store into unwatched memory did not advance its stamp");
    const auto partial = NewestStamp(base + 3 * Block - 8, Block + 8);
    static_cast<volatile std::uint8_t*>(memory)[3 * Block] = 0x33;
    CollectWritesUncached(base + 3 * Block, Block);
    Require(NewestStamp(base + 3 * Block - 8, Block + 8) > partial, "a CPU store into the watched part of a partly watched range did not advance its stamp");
}

void CheckPartlyWatchedStore() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto stored = base + Block;
    const auto unwatched = base + 2 * Block;
    std::memset(memory, 0x11, 3 * Block);
    BumpCollectEpoch();
    Unwatch(unwatched, Block);
    const auto before = CollectWrites(base, 2 * Block);
    Require(before != 0 && UnchangedSince(base, 2 * Block, before), "the watched blocks were not collected");
    Require(MarkWritten(unwatched - 64, 128) == 0, "a driver store into partly watched memory is trusted");
    Require(!UnchangedSince(stored, Block, before), "a driver store into partly watched memory left its watched block unchanged");
    Require(StoredOver(unwatched - 64, 64, before), "a driver store into partly watched memory is not seen over its watched bytes");
    Require(!StoredOver(stored, Block - 64, before), "a driver store into partly watched memory is seen over watched bytes it did not store");
    Require(UnchangedSince(base, Block, before), "a driver store into partly watched memory stamped a block it did not store into");
    const auto beforeOwn = TrackerGeneration();
    Require(StoreOwnBytes(unwatched - 64, 128, [&] { std::memset(reinterpret_cast<void*>(unwatched - 64), 0x22, 128); }) == 0, "an own store into partly watched memory is trusted");
    Require(!UnchangedSince(stored, Block, beforeOwn), "an own store into partly watched memory left its watched block unchanged");
    Require(UnchangedSince(base, Block, beforeOwn), "an own store into partly watched memory stamped a block it did not store into");
#ifndef _WIN32
    void* split = AllocateWatched(Block);
    const auto splitBase = reinterpret_cast<std::uint64_t>(split);
    const auto watchedHalf = splitBase + Block / 2;
    std::memset(split, 0x11, Block);
    Unwatch(splitBase, Block / 2);
    const auto beforeSplit = CollectWrites(watchedHalf, Block / 2);
    Require(beforeSplit != 0 && UnchangedSince(watchedHalf, Block / 2, beforeSplit), "the watched half of a split block was not collected");
    Require(MarkWritten(splitBase + 64, 128) == 0 && UnchangedSince(watchedHalf, Block / 2, beforeSplit), "a driver store into the unwatched half of a split block stamped its watched half");
    Require(MarkWritten(watchedHalf - 64, 128) == 0, "a driver store across the halves of a split block is trusted");
    Require(!UnchangedSince(watchedHalf, Block / 2, beforeSplit), "a driver store across the halves of a split block left its watched half unchanged");
#endif
}

#ifndef _WIN32
void CheckWatchOverlaps() {
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    void* memory = AllocateWatched(Block);
    const auto base = reinterpret_cast<std::uintptr_t>(memory);
    Require(GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(memory, page), "cannot unwatch the block's first page");
    Require(GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(reinterpret_cast<void*>(base + 3 * page), Block - 3 * page), "cannot unwatch the block's last pages");
    Require(!GuestWriteWatch::GuestWriteWatchCovers_nid_postfix(base, Block), "a block with unwatched pages is covered");
    Require(GuestWriteWatch::GuestWriteWatchOverlaps_nid_postfix(base, Block), "a block with watched pages inside does not overlap the watch");
    Require(GuestWriteWatch::GuestWriteWatchOverlaps_nid_postfix(base + page - 1, 2), "a range ending in a watched page does not overlap the watch");
    Require(GuestWriteWatch::GuestWriteWatchOverlaps_nid_postfix(base + 3 * page - 1, 2), "a range starting in a watched page does not overlap the watch");
    Require(!GuestWriteWatch::GuestWriteWatchOverlaps_nid_postfix(base, page), "an unwatched page before the watched ones overlaps the watch");
    Require(!GuestWriteWatch::GuestWriteWatchOverlaps_nid_postfix(base + 3 * page, Block - 3 * page), "unwatched pages after the watched ones overlap the watch");
    Require(!GuestWriteWatch::GuestWriteWatchOverlaps_nid_postfix(base + page, 0), "an empty range overlaps the watch");
}

void CheckWatchedMiddleStore() {
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    void* memory = AllocateWatched(Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto middle = base + page;
    std::memset(memory, 0x11, Block);
    Unwatch(base, page);
    Unwatch(base + 3 * page, Block - 3 * page);
    const auto before = CollectWrites(middle, 2 * page);
    Require(before != 0 && UnchangedSince(middle, 2 * page, before), "the watched middle of a block was not collected");
    Require(MarkWritten(base + 3 * page + 64, 128) == 0 && UnchangedSince(middle, 2 * page, before), "a driver store into an unwatched end of a block stamped its watched middle");
    Require(MarkWritten(base + page / 2, 3 * page) == 0, "a driver store around the watched middle of a block is trusted");
    Require(!UnchangedSince(middle, 2 * page, before), "a driver store around the watched middle of a block left it unchanged");
    Require(StoredOver(middle, 2 * page, before), "a driver store around the watched middle of a block is not seen over it");
    const auto beforeOwn = TrackerGeneration();
    Require(StoreOwnBytes(base + page / 2, 3 * page, [&] { std::memset(reinterpret_cast<void*>(base + page / 2), 0x22, 3 * page); }) == 0, "an own store around the watched middle of a block is trusted");
    Require(!UnchangedSince(middle, 2 * page, beforeOwn), "an own store around the watched middle of a block left it unchanged");
}
#endif

void CheckCoarseRuns() {
    constexpr std::size_t bytes = 192 * Block;
    void* memory = AllocateWatched(bytes);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x11, bytes);
    BumpCollectEpoch();
    const auto collected = CollectWrites(base, bytes);
    Require(collected != 0 && UnchangedSince(base, bytes, collected), "a range nothing wrote after its collect reports a change");
    const auto stored = MarkWritten(base + bytes / 2, 4);
    Require(stored > collected && !UnchangedSince(base, bytes, collected), "a driver store inside whole 4 MiB runs of the range is not seen");
    Require(UnchangedSince(base, bytes / 2, collected), "a driver store after a range made it changed");
    Require(UnchangedSince(base, bytes, stored), "a range checked at its newest stamp reports a change");
    static_cast<volatile std::uint8_t*>(memory)[bytes - 1] = 0x22;
    CollectWritesUncached(base + bytes - Block, Block);
    Require(!UnchangedSince(base, bytes, stored), "a CPU store into the last block of the range is not seen");
}

#ifdef _WIN32
void CheckDeferredSharedImports() {
    constexpr std::size_t bytes = 4 * Block;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
    Require(section != nullptr, "cannot create shared import backing");
    auto* memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block));
    Require(memory != nullptr, "cannot allocate shared import address");
    GuestArena::GuestArenaMap_nid_postfix(memory, bytes, section, 0, PAGE_READWRITE);
    const auto base = reinterpret_cast<std::uintptr_t>(memory);
    const auto protection = [&](std::size_t offset) {
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(memory + offset, &info, sizeof(info)) == sizeof(info), "cannot query shared import protection");
        return info.Protect;
    };
    Require(ImportWatched(base, bytes, [] { return true; }), "fresh shared import failed");
    Require(protection(0) == PAGE_READWRITE && protection(bytes - 1) == PAGE_READWRITE, "fresh import armed shared pages");
    const auto gpu = MarkWritten(base + 4096, 4);
    Require(gpu != 0 && protection(0) == PAGE_READONLY && protection(Block) == PAGE_READWRITE, "GPU write did not activate only its block");
    CollectWritesUncached(base, Block);
    Require(!WrittenSince(base + 4096, 4, gpu) && !StoredOver(base + 4096, 4, gpu), "first collect overwrote a GPU generation");
    static_cast<volatile std::uint8_t*>(memory)[4096] = 0x44;
    CollectWritesUncached(base, Block);
    Require(WrittenSince(base + 4096, 4, gpu) && StoredOver(base + 4096, 4, gpu), "CPU store after GPU write was missed");

    const auto second = base + Block;
    const auto before = CollectWritesUncached(second, 4096);
    Require(MarkWritten(second, 4) != 0, "GPU write next to fresh pages failed");
    Require(UnchangedSinceCollected(second, 4, before), "fresh neighbors were stamped as new CPU writes over a collected page");
    Require(StoredOver(second + 8 * 4096, 4096, before), "fresh neighbor history was lost");
    const std::array<std::uint64_t, 1> generations{before};
    std::array<std::uint8_t, 1> changed{};
    std::array<std::uint8_t, 1> cpu{};
    Require(ChangedBlocks(second, Block, generations, changed, cpu) && changed[0] == BlockWritten && cpu[0] == 0, "fresh collection set the block's CPU stamp");
    const auto generation = TrackerGeneration();
    Require(MarkWritten(second, 4) == generation + 1, "an already collected GPU block advanced twice");

    const auto third = base + 2 * Block;
    const auto cached = CollectWritesUncached(third, Block);
    Require(ImportWatched(third, Block, [&] {
        static_cast<volatile std::uint8_t*>(memory)[2 * Block] = 0x55;
        return true;
    }), "tracked import window failed");
    Require(!UnchangedSince(third, Block, cached) && WrittenSince(third, Block, cached), "a store of new bytes within a tracked import window was missed");
    const auto afterImport = CollectWritesUncached(third, Block);
    static_cast<volatile std::uint8_t*>(memory)[2 * Block] = 0x66;
    Require(ImportWatched(third, Block, [] { return true; }) && WrittenSince(third, Block, afterImport), "pre-import CPU store was not collected");

    GuestArena::GuestArenaReset_nid_postfix(memory + 3 * Block, 16384);
    const auto edge = base + 3 * Block + 16384;
    Require(ImportWatched(edge, Block - 16384, [] { return true; }), "unaligned shared import failed");
    const auto edgeGpu = MarkWritten(edge, 4);
    Require(edgeGpu != 0, "fresh collection rejected an unmapped block neighbor");
    CollectWritesUncached(edge, Block - 16384);
    Require(!WrittenSince(edge, 4, edgeGpu), "edge block collection overwrote GPU results");
    GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
    GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
    CloseHandle(section);
}


void CheckPrivateMappingReuse() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x11, 3 * Block);
    BumpCollectEpoch();
    const auto before = CollectWrites(base, 3 * Block);
    Require(before != 0, "the original backing was not collected");
    Unwatch(base + Block, Block);
    Require(!Watched(base + Block, Block), "the imported block stayed watched");
    GuestArena::GuestArenaCommit_nid_postfix(memory, 3 * Block, PAGE_READWRITE, 3 * Block);
    Require(!Watched(base + Block, Block), "a protection-only commit restored an imported block");
    const auto neighbour = CollectWrites(base, Block);
    Require(neighbour != 0, "the neighbouring block was not collected before remapping");
    GuestArena::GuestArenaReset_nid_postfix(memory, 3 * Block);
    GuestArena::GuestArenaRelease_nid_postfix(memory, 3 * Block);
    auto* replacement = GuestArena::GuestArenaAllocateAtOrAbove_nid_postfix(base, 3 * Block, Block);
    Require(replacement == memory, "the released arena address was not reused");
    GuestArena::GuestArenaCommit_nid_postfix(replacement, 3 * Block, PAGE_READWRITE, 3 * Block);
    Require(Watched(base, 3 * Block), "fresh backing at an excluded address is not watched");
    Require(!UnchangedSince(base + Block, Block, before), "fresh backing reused an old import generation");
    Require(!UnchangedSince(base, Block, neighbour), "fresh backing reused an adjacent block's old generation");
    const auto after = CollectWrites(base, 3 * Block);
    Require(after > before && UnchangedSince(base, 3 * Block, after), "fresh backing was not collected");
    static_cast<volatile std::uint8_t*>(replacement)[Block + 8] = 0x22;
    CollectWritesUncached(base, 3 * Block);
    Require(!UnchangedSince(base + Block, Block, after), "a CPU edit in fresh backing was missed");
    Unwatch(base + Block, Block);
    Require(!Watched(base + Block, Block) && CollectWrites(base + Block, Block) == 0, "a second import of fresh backing stayed watched");
    GuestArena::GuestArenaReset_nid_postfix(replacement, 3 * Block);
    GuestArena::GuestArenaRelease_nid_postfix(replacement, 3 * Block);
}

void CheckRewatch() {
    void* memory = GuestArena::GuestArenaAllocate_nid_postfix(4 * Block, Block);
    GuestArena::GuestArenaCommit_nid_postfix(memory, 4 * Block, PAGE_READWRITE, 16384);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    Require(Watched(base, 4 * Block), "the rewatch test block is not watched");
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    const auto imported = base + Block;
    std::memset(memory, 0x11, 4 * Block);
    BumpCollectEpoch();
    const auto before = CollectWrites(base, 4 * Block);
    Require(before != 0, "the test range was not collected");
    Unwatch(imported, 2 * Block);
    const auto unwatched = UnwatchSerial();
    Require(!Watched(imported, Block) && UntrackedHint(imported, Block) != 0, "the unwatched block does not read as untracked");
    bytes[Block + 8] = 0x22;
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> unaligned{{{imported + 1, imported + Block}}};
    Rewatch(unaligned);
    Require(!Watched(imported + 4096, Block - 4096) && UnwatchSerial() == unwatched, "a range that is not whole pages returned to the write watch");
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> partial{{{imported + 4096, imported + Block}}};
    Rewatch(partial);
    Require(Watched(imported + 4096, Block - 4096) && !Watched(imported, 4096) && !Watched(imported, Block), "rewatching part of a block did not return exactly its pages to the write watch");
    const auto partly = UnwatchSerial();
    Require(partly != unwatched, "rewatching part of a block did not invalidate what was cached while it was unwatched");
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> first{{{imported, imported + Block}}};
    Rewatch(first);
    Require(Watched(imported, Block) && !Watched(imported + Block, Block), "rewatching did not return exactly its block to the write watch");
    Require(UnwatchSerial() != partly, "rewatching did not invalidate what was cached while the block was unwatched");
    Require(UntrackedHint(imported, Block) == 0, "a rewatched block still reads as untracked");
    Require(!UnchangedSince(imported, Block, before) && StoredOver(imported, Block, before), "a block returned to the write watch reads as unchanged since before it left");
    Require(!UnchangedSinceCollected(imported, Block, before), "a CPU store made while the block was unwatched reads as no CPU store");
    Require(UnchangedSince(base, Block, before) && UnchangedSince(base + 3 * Block, Block, before), "rewatching stamped the blocks around it");
    BumpCollectEpoch();
    const auto after = CollectWrites(imported, Block);
    Require(after != 0 && UnchangedSince(imported, Block, after), "a rewatched block is not collected");
    bytes[Block + 16] = 0x33;
    CollectWritesUncached(imported, Block);
    Require(!UnchangedSince(imported, Block, after), "a CPU store after rewatching was missed");
    const auto serial = UnwatchSerial();
    Rewatch(first);
    Require(UnwatchSerial() == serial, "rewatching a watched block changed what it covers");
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> uncommitted{{{base + 2 * Block, base + 3 * Block}}};
    GuestArena::GuestArenaReset_nid_postfix(static_cast<std::uint8_t*>(memory) + 2 * Block, 16384);
    Rewatch(uncommitted);
    Require(!Watched(base + 2 * Block, Block), "a block with uncommitted pages returned to the write watch");
    GuestArena::GuestArenaReset_nid_postfix(memory, 4 * Block);
    GuestArena::GuestArenaRelease_nid_postfix(memory, 4 * Block);
}

using RewatchSpan = std::array<std::pair<std::uint64_t, std::uint64_t>, 1>;

RewatchSpan BlockSpan(std::uint64_t address, std::size_t bytes) {
    return {{{address & ~std::uint64_t{Block - 1}, (address + bytes + Block - 1) & ~std::uint64_t{Block - 1}}}};
}

RewatchSpan PageSpan(std::uint64_t address, std::size_t bytes) {
    return {{{address & ~std::uint64_t{4095}, (address + bytes + 4095) & ~std::uint64_t{4095}}}};
}

void PrintPageState(const char* page, const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    Require(VirtualQuery(address, &info, sizeof(info)) == sizeof(info), "cannot query a page next to an import");
    std::cout << page << ": State 0x" << std::hex << info.State << " Type 0x" << info.Type << " Protect 0x" << info.Protect << std::dec << '\n';
}

struct ImportFixture {
    std::uint8_t* memory;
    std::size_t reserved;
    ImportFixture(std::size_t reservedBytes, std::size_t committed) : memory(static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(reservedBytes, Block))), reserved(reservedBytes) {
        Require(memory != nullptr, "cannot reserve an import fixture");
        GuestArena::GuestArenaCommit_nid_postfix(memory, committed, PAGE_READWRITE, committed);
        std::memset(memory, 0x11, committed);
        BumpCollectEpoch();
        Require(CollectWrites(Base(), committed) != 0, "an import fixture was not collected");
    }
    ImportFixture(const ImportFixture&) = delete;
    ImportFixture& operator=(const ImportFixture&) = delete;
    ~ImportFixture() {
        GuestArena::GuestArenaReset_nid_postfix(memory, reserved);
        GuestArena::GuestArenaRelease_nid_postfix(memory, reserved);
    }
    std::uint64_t Base() const {
        return reinterpret_cast<std::uint64_t>(memory);
    }
};

void UnwatchImport(std::uint64_t address, std::size_t bytes) {
    Require(ImportWatched(address, bytes, [] { return true; }, false) && !Watched(address, bytes), "an import that leaves the watch stayed watched");
}

void RequireRewatchedStores(std::uint64_t address, std::size_t bytes) {
    BumpCollectEpoch();
    const auto collected = CollectWrites(address, bytes);
    Require(collected != 0 && UnchangedSince(address, bytes, collected), "a rewatched import is not collected");
    *reinterpret_cast<volatile std::uint8_t*>(address + bytes - 1) = 0x22;
    CollectWritesUncached(address, bytes);
    Require(!UnchangedSince(address + bytes - 1, 1, collected), "a CPU store into a rewatched import's last page was missed");
}

void CheckRewatchWholeBlockImport() {
    constexpr std::size_t bytes = 4 * Block;
    ImportFixture fixture(6 * Block, 6 * Block);
    const auto import = fixture.Base() + Block;
    const auto before = CollectWrites(fixture.Base(), 6 * Block);
    UnwatchImport(import, bytes);
    Require(PageSpan(import, bytes) == BlockSpan(import, bytes), "a whole-block import has a page span unlike its block span");
    Rewatch(PageSpan(import, bytes));
    Require(Watched(import, bytes), "a settled whole-block import did not return to the write watch");
    Require(!UnchangedSince(import, bytes, before) && UnchangedSince(fixture.Base(), Block, before) && UnchangedSince(import + bytes, Block, before), "rewatching a whole-block import did not stamp exactly its blocks");
    RequireRewatchedStores(import, bytes);
}

void CheckRewatchImportTail() {
    constexpr std::size_t bytes = 0xc4000;
    ImportFixture fixture(13 * Block, bytes);
    const auto import = fixture.Base();
    UnwatchImport(import, bytes);
    PrintPageState("page past a 0xc4000-byte import", fixture.memory + bytes);
    const auto unwatched = UnwatchSerial();
    Rewatch(BlockSpan(import, bytes));
    Require(!Watched(import, bytes) && UnwatchSerial() == unwatched, "the 64 KiB span past the import's committed pages returned to the write watch");
    Rewatch(PageSpan(import, bytes));
    Require(Watched(import, bytes), "a settled import whose size is not a whole block did not return to the write watch");
    Require(Watched(import + 12 * Block, bytes - 12 * Block) && !Watched(import, bytes + 1) && !Watched(import + bytes - 1, 2) && !Watched(import + bytes, 4096), "rewatching an import ending inside a block did not return exactly its pages");
    Require(UnwatchSerial() != unwatched && UntrackedHint(import, bytes) == 0, "a rewatched import still reads as unwatched");
    RequireRewatchedStores(import, bytes);
}

void CheckRewatchUnalignedImport() {
    constexpr std::size_t offset = 0x1000;
    constexpr std::size_t bytes = 0xc4000;
    constexpr std::size_t committed = 0xc8000;
    ImportFixture fixture(13 * Block, committed);
    const auto import = fixture.Base() + offset;
    UnwatchImport(import, bytes);
    PrintPageState("page past an import at block offset 0x1000", fixture.memory + committed);
    Rewatch(BlockSpan(import, bytes));
    Require(!Watched(import, bytes), "the 64 KiB span past the committed pages around an import returned to the write watch");
    Rewatch(PageSpan(import, bytes));
    Require(Watched(import, bytes), "a settled import that does not start on a block did not return to the write watch");
    Require(!Watched(fixture.Base(), offset) && !Watched(import + bytes, 4096), "rewatching an import returned pages outside it");
    RequireRewatchedStores(import, bytes);
}

void CheckRewatchImportBesideUnwatchedImport() {
    constexpr std::size_t bytes = 0xc4000;
    constexpr std::size_t otherBytes = 0x1c000;
    ImportFixture fixture(14 * Block, 14 * Block);
    const auto import = fixture.Base();
    const auto other = import + bytes;
    UnwatchImport(import, bytes);
    UnwatchImport(other, otherBytes);
    Rewatch(PageSpan(import, bytes));
    Require(Watched(import, bytes), "an import sharing its last block with another unwatched import did not return to the write watch");
    Require(!Watched(other, 4096) && !Watched(import + 13 * Block - 4096, 4096) && !Watched(other + otherBytes - 4096, 4096), "rewatching an import returned another unwatched import's pages in its last block");
    Require(UntrackedHint(other, otherBytes) != 0, "an unwatched import beside a rewatched one does not read as untracked");
    RequireRewatchedStores(import, bytes);
    Rewatch(PageSpan(other, otherBytes));
    Require(Watched(import, bytes + otherBytes), "rewatching the second import did not return both to the write watch");
}

void CheckRewatchImportPages() {
    CheckRewatchWholeBlockImport();
    CheckRewatchImportTail();
    CheckRewatchUnalignedImport();
    CheckRewatchImportBesideUnwatchedImport();
}

void CheckRewatchedDirectMemoryComparedWhenShared() {
    constexpr std::size_t bytes = 2 * Block;
    auto* memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(2 * bytes, Block));
    Require(memory != nullptr, "cannot reserve the compared sharing fixture");
    struct Cleanup {
        std::uint8_t* memory;
        HANDLE section = nullptr;
        ~Cleanup() {
            GuestArena::GuestArenaReset_nid_postfix(memory, 2 * bytes);
            GuestArena::GuestArenaRelease_nid_postfix(memory, 2 * bytes);
            if (section == nullptr) return;
            GuestArena::GuestArenaForgetSection_nid_postfix(section);
            CloseHandle(section);
        }
    } cleanup{memory};
    cleanup.section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, bytes, nullptr);
    Require(cleanup.section != nullptr, "cannot create the compared sharing backing");
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto native = [](const std::uint8_t* address) {
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(address, &info, sizeof(info)) == sizeof(info), "cannot query the compared sharing fixture");
        return info;
    };
    GuestArena::GuestArenaMapDirect_nid_postfix(memory, bytes, cleanup.section, 0, PAGE_READWRITE);
    Require(native(memory).Type == MEM_PRIVATE && Watched(base, bytes), "the direct memory is not resident and watched");
    Unwatch(base, Block);
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> imported{{{base, base + Block}}};
    Rewatch(imported);
    Require(Watched(base, Block), "the rewatched direct memory is not watched");
    BumpCollectEpoch();
    Require(CollectWritesUncached(base, Block) != 0, "rewatched resident direct memory is not collected");
    GuestArena::GuestArenaMapDirect_nid_postfix(memory + bytes, bytes, cleanup.section, 0, PAGE_READWRITE);
    Require(native(memory).Type == MEM_MAPPED && native(memory + bytes).Type == MEM_MAPPED, "a second mapping kept the direct memory resident");
    BumpCollectEpoch();
    Require(CollectWritesUncached(base, Block) == 0 && CollectWritesUncached(base + bytes, Block) == 0, "rewatched direct memory was collected once shared");
    Require(native(memory).Protect == PAGE_READWRITE && native(memory + bytes).Protect == PAGE_READWRITE, "rewatched direct memory was armed once shared");
    memory[8] = 0x5a;
    Require(memory[bytes + 8] == 0x5a, "the second mapping does not see a store into the first");
    Require(CollectWritesUncached(base + Block, Block) != 0, "shared direct memory that never left the watch stopped being collected");
    std::cout << "rewatched direct memory: compared once shared, not armed; other shared direct memory still collected\n";
}

struct ComparedSharing {
    static constexpr std::size_t bytes = 2 * Block;
    std::uint8_t* memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(2 * bytes, Block));
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, bytes, nullptr);
    std::uint64_t base = reinterpret_cast<std::uint64_t>(memory);

    ComparedSharing() {
        Require(memory != nullptr && section != nullptr, "cannot create the compared sharing generation fixture");
        GuestArena::GuestArenaForgetSection_nid_postfix(section);
        GuestArena::GuestArenaMapDirect_nid_postfix(memory, bytes, section, 0, PAGE_READWRITE);
        Require(Watched(base, bytes), "the compared sharing generation fixture is not watched");
        Unwatch(base, Block);
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> imported{{{base, base + Block}}};
        Rewatch(imported);
        Require(Watched(base, Block), "the rewatched direct memory of the generation fixture is not watched");
        BumpCollectEpoch();
        Require(CollectWritesUncached(base, Block) != 0, "rewatched resident direct memory of the generation fixture is not collected");
    }
    ~ComparedSharing() {
        GuestArena::GuestArenaReset_nid_postfix(memory, 2 * bytes);
        GuestArena::GuestArenaRelease_nid_postfix(memory, 2 * bytes);
        GuestArena::GuestArenaForgetSection_nid_postfix(section);
        CloseHandle(section);
    }
    ComparedSharing(const ComparedSharing&) = delete;
    ComparedSharing& operator=(const ComparedSharing&) = delete;

    void Share() {
        GuestArena::GuestArenaMapDirect_nid_postfix(memory + bytes, bytes, section, 0, PAGE_READWRITE);
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(memory, &info, sizeof(info)) == sizeof(info) && info.Type == MEM_MAPPED, "a second mapping kept the generation fixture resident");
    }
    std::uint64_t Collect(std::uint64_t address) {
        BumpCollectEpoch();
        return CollectWrites(address, Block);
    }
};

void CheckComparedSharingGenerations() {
    {
        ComparedSharing fixture;
        const auto g0 = TrackerGeneration();
        Require(UnchangedSince(fixture.base, Block, g0) && !WrittenSince(fixture.base, Block, g0), "rewatched resident direct memory reads as written before any store");
        fixture.memory[16] = 0x21;
        Require(fixture.Collect(fixture.base) != 0 && !UnchangedSince(fixture.base, Block, g0) && WrittenSince(fixture.base, Block, g0), "a store into rewatched resident direct memory was missed before it was shared");
        const auto cached = TrackerGeneration();
        Require(UnchangedSince(fixture.base, Block, cached), "rewatched resident direct memory reads as changed after its collect");
        fixture.Share();
        fixture.memory[8] = 0x5a;
        Require(fixture.memory[ComparedSharing::bytes + 8] == 0x5a, "the second mapping does not see the store into the generation fixture");
        Require(fixture.Collect(fixture.base) == 0, "rewatched direct memory was collected once shared");
        Require(!UnchangedSince(fixture.base, Block, cached), "a store into rewatched direct memory once shared reads as unchanged since a generation before the sharing");
        Require(WrittenSince(fixture.base, Block, cached), "a store into rewatched direct memory once shared reads as not written since a generation before the sharing");
    }
    {
        ComparedSharing fixture;
        const auto g0 = TrackerGeneration();
        fixture.Share();
        Require(fixture.Collect(fixture.base) == 0, "rewatched direct memory was collected once shared, before any store");
        Require(!UnchangedSince(fixture.base, Block, g0) && WrittenSince(fixture.base, Block, g0), "sharing rewatched direct memory did not report it written once");
        const auto g1 = TrackerGeneration();
        Require(fixture.Collect(fixture.base) == 0 && fixture.Collect(fixture.base) == 0, "rewatched direct memory was collected again once shared");
        std::array<std::uint64_t, 1> generations{g1};
        std::array<std::uint8_t, 1> changed{};
        std::array<std::uint8_t, 1> cpu{};
        Require(!WrittenSince(fixture.base, Block, g1) && ChangedBlocks(fixture.base, Block, generations, changed, cpu) && changed[0] == BlockUnchanged && cpu[0] == 0, "refused collects of shared compared memory reported it written again without a store");
        Require(!UnchangedSince(fixture.base, Block, g1), "shared compared memory read as unchanged since a generation taken after a refused collect");
        const auto neighbour = fixture.Collect(fixture.base + Block);
        const auto spanned = std::max(fixture.Collect(fixture.base), neighbour);
        Require(spanned != 0 && UnchangedSince(fixture.base + Block, Block, spanned), "shared direct memory that never left the watch is not collected beside compared memory");
        fixture.memory[24] = 0x3c;
        Require(fixture.Collect(fixture.base) == 0, "rewatched direct memory was collected once shared, after a store");
        Require(!UnchangedSince(fixture.base, Block, g1), "a store into shared compared memory reads as unchanged since a generation taken after a refused collect");
        Require(!UnchangedSince(fixture.base, Block, spanned), "a store into shared compared memory reads as unchanged since the newest generation of a collect over several ranges");
        Require(fixture.Collect(fixture.base + Block) != 0 && UnchangedSince(fixture.base + Block, Block, spanned), "a refused collect of compared memory made its never-compared neighbour read as changed");
        const std::array<UnchangedQuery, 2> queries{{{fixture.base + Block, Block, spanned}, {fixture.base, Block, spanned}}};
        Require(!UnchangedSinceAll(queries), "a batch of queries over shared compared memory read as unchanged");
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> committed{{{fixture.base, fixture.base + Block}}};
        Require(!UnchangedCommittedSince(committed, spanned), "committed ranges over shared compared memory read as unchanged");
    }
    std::cout << "shared compared memory: reported written once at the sharing, never unchanged since an older generation, its neighbour still trusted\n";
}

void CheckImportedDirectMemoryComparedWhenShared() {
    constexpr std::size_t bytes = 2 * Block;
    auto* memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(2 * bytes, Block));
    Require(memory != nullptr, "cannot reserve the imported sharing fixture");
    struct Cleanup {
        std::uint8_t* memory;
        HANDLE section = nullptr;
        ~Cleanup() {
            GuestArena::GuestArenaReset_nid_postfix(memory, 2 * bytes);
            GuestArena::GuestArenaRelease_nid_postfix(memory, 2 * bytes);
            if (section == nullptr) return;
            GuestArena::GuestArenaForgetSection_nid_postfix(section);
            CloseHandle(section);
        }
    } cleanup{memory};
    cleanup.section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, bytes, nullptr);
    Require(cleanup.section != nullptr, "cannot create the imported sharing backing");
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto native = [](const std::uint8_t* address) {
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(address, &info, sizeof(info)) == sizeof(info), "cannot query the imported sharing fixture");
        return info;
    };
    GuestArena::GuestArenaMapDirect_nid_postfix(memory, bytes, cleanup.section, 0, PAGE_READWRITE);
    Require(native(memory).Type == MEM_PRIVATE && Watched(base, bytes), "the imported direct memory is not resident and watched");
    Require(ImportWatched(base, Block, [] { return true; }), "the watched import of resident direct memory failed");
    Require(Watched(base, Block), "a watched import of resident direct memory left the write watch");
    BumpCollectEpoch();
    Require(CollectWritesUncached(base, Block) != 0, "imported resident direct memory is not collected");
    GuestArena::GuestArenaMapDirect_nid_postfix(memory + bytes, bytes, cleanup.section, 0, PAGE_READWRITE);
    Require(native(memory).Type == MEM_MAPPED && native(memory + bytes).Type == MEM_MAPPED, "a second mapping kept the imported direct memory resident");
    BumpCollectEpoch();
    Require(CollectWritesUncached(base, Block) == 0 && CollectWritesUncached(base + bytes, Block) == 0, "imported direct memory was collected once shared");
    Require(native(memory).Protect == PAGE_READWRITE && native(memory + bytes).Protect == PAGE_READWRITE, "imported direct memory was armed once shared");
    memory[8] = 0x5a;
    Require(memory[bytes + 8] == 0x5a, "the second mapping does not see a store into the imported first");
    Require(CollectWritesUncached(base + Block, Block) != 0, "shared direct memory that was never imported stopped being collected");
    std::cout << "imported direct memory: compared once shared, not armed; unimported shared direct memory still collected\n";
}

void CheckImportAliasDemotedComparedWhenShared() {
    constexpr std::size_t page = 0x4000;
    auto* memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(5 * Block, Block));
    Require(memory != nullptr, "cannot reserve the import alias fixture");
    struct Cleanup {
        std::uint8_t* memory;
        std::array<HANDLE, 3> sections{};
        std::array<void*, 3> aliases{};
        ~Cleanup() {
            for (const auto alias : aliases) GuestArena::GuestArenaUnmapAlias_nid_postfix(alias);
            GuestArena::GuestArenaReset_nid_postfix(memory, 5 * Block);
            GuestArena::GuestArenaRelease_nid_postfix(memory, 5 * Block);
            for (const auto section : sections) {
                if (section == nullptr) continue;
                GuestArena::GuestArenaForgetSection_nid_postfix(section);
                CloseHandle(section);
            }
        }
    } cleanup{memory};
    for (auto& section : cleanup.sections) {
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, Block, nullptr);
        Require(section != nullptr, "cannot create the import alias backing");
        GuestArena::GuestArenaForgetSection_nid_postfix(section);
    }
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto native = [](const std::uint8_t* address) {
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(address, &info, sizeof(info)) == sizeof(info), "cannot query the import alias fixture");
        return info;
    };
    const auto clearingCollect = [](std::uint64_t address) {
        std::array<void*, 4> pages{};
        std::size_t count = pages.size();
        return GuestArena::GuestArenaCollectWrites_nid_postfix(static_cast<std::uintptr_t>(address), page, pages.data(), &count, true);
    };
    GuestArena::GuestArenaMapDirect_nid_postfix(memory, page, cleanup.sections[0], 0, PAGE_READWRITE);
    Require(native(memory).Type == MEM_PRIVATE && Watched(base, page), "the aliased direct memory is not resident and watched");
    BumpCollectEpoch();
    Require(CollectWritesUncached(base, page) != 0, "resident direct memory is not collected before its import");
    cleanup.aliases[0] = GuestArena::GuestArenaMapAlias_nid_postfix(static_cast<std::uintptr_t>(base), page);
    Require(cleanup.aliases[0] != nullptr && native(memory).Type == MEM_MAPPED, "the import alias did not demote the resident run");
    Require(ImportWatched(base, page, [] { return true; }) && Watched(base, page), "the import of aliased direct memory did not stay watched");
    Require(native(memory).Protect == PAGE_READWRITE, "memory an import's alias demoted was armed by the import");
    Require(!clearingCollect(base), "a clearing collect over memory an import's alias demoted was not refused");
    Require(native(memory).Protect == PAGE_READWRITE, "memory an import's alias demoted was armed by a clearing collect");
    BumpCollectEpoch();
    Require(CollectWritesUncached(base, page) == 0, "memory an import's alias demoted was collected once shared");
    memory[8] = 0x5a;
    Require(static_cast<const std::uint8_t*>(cleanup.aliases[0])[8] == 0x5a, "the import alias does not see a store into the guest view");

    GuestArena::GuestArenaMapDirect_nid_postfix(memory + Block, page, cleanup.sections[1], 0, PAGE_READWRITE);
    GuestArena::GuestArenaMapDirect_nid_postfix(memory + 2 * Block, page, cleanup.sections[1], 0, PAGE_READWRITE);
    Require(native(memory + Block).Type == MEM_MAPPED && native(memory + 2 * Block).Type == MEM_MAPPED, "a second mapping kept the control direct memory resident");
    cleanup.aliases[1] = GuestArena::GuestArenaMapAlias_nid_postfix(static_cast<std::uintptr_t>(base + Block), page);
    Require(cleanup.aliases[1] != nullptr, "the control import alias was refused");
    Require(ImportWatched(base + Block, page, [] { return true; }) && Watched(base + Block, page), "the control import did not stay watched");
    Require(clearingCollect(base + Block), "memory shared by a second mapping before its import alias stopped being collected");
    Require(native(memory + Block).Protect == PAGE_READONLY && native(memory + 2 * Block).Protect == PAGE_READONLY, "memory shared by a second mapping before its import alias was not armed");

    GuestArena::GuestArenaMapDirect_nid_postfix(memory + 3 * Block, page, cleanup.sections[2], 0, PAGE_READWRITE);
    auto* failed = GuestArena::GuestArenaMapAlias_nid_postfix(static_cast<std::uintptr_t>(base + 3 * Block), page);
    Require(failed != nullptr && native(memory + 3 * Block).Type == MEM_MAPPED, "the alias of a failed import did not demote the resident run");
    GuestArena::GuestArenaUnmapAlias_nid_postfix(failed);
    GuestArena::GuestArenaMapDirect_nid_postfix(memory + 4 * Block, page, cleanup.sections[2], 0, PAGE_READWRITE);
    cleanup.aliases[2] = GuestArena::GuestArenaMapAlias_nid_postfix(static_cast<std::uintptr_t>(base + 3 * Block), page);
    Require(cleanup.aliases[2] != nullptr, "the import alias after a second mapping was refused");
    Require(ImportWatched(base + 3 * Block, page, [] { return true; }) && Watched(base + 3 * Block, page), "the import after a second mapping did not stay watched");
    Require(clearingCollect(base + 3 * Block), "memory a second mapping shared after an earlier alias demoted it stopped being collected");
    Require(native(memory + 3 * Block).Protect == PAGE_READONLY && native(memory + 4 * Block).Protect == PAGE_READONLY, "memory a second mapping shared after an earlier alias demoted it was not armed");
    std::cout << "direct memory an import's alias demoted: compared once shared, not armed; memory shared before the alias still collected\n";
}

void CheckResidentDirectMappingReuse() {
    constexpr std::size_t bytes = 5 * Block;
    auto* memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block));
    Require(memory != nullptr, "cannot reserve the resident remap fixture");
    struct Cleanup {
        std::uint8_t* memory;
        std::array<HANDLE, 3> sections{};
        ~Cleanup() {
            GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
            for (const auto section : sections) {
                if (section == nullptr) continue;
                GuestArena::GuestArenaForgetSection_nid_postfix(section);
                CloseHandle(section);
            }
        }
    } cleanup{memory};
    GuestArena::GuestArenaCommit_nid_postfix(memory, 3 * Block, PAGE_READWRITE, 3 * Block);
    GuestArena::GuestArenaCommit_nid_postfix(memory + 3 * Block, 2 * Block, PAGE_READWRITE, 2 * Block);
    Require(Watched(reinterpret_cast<std::uint64_t>(memory), bytes), "the resident remap fixture is not watched");
    for (auto& section : cleanup.sections) {
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, 3 * Block, nullptr);
        Require(section != nullptr, "cannot create the resident remap backing");
    }
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto nativeType = [&](std::size_t offset) {
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(memory + offset, &info, sizeof(info)) == sizeof(info), "cannot query the resident remap backing");
        return info.Type;
    };
    for (std::size_t use = 0; use < 2; ++use) {
        BumpCollectEpoch();
        const auto before = CollectWrites(base, 3 * Block);
        Require(before != 0, "the backing before a resident remap was not collected");
        Unwatch(base + Block, Block);
        Require(!Watched(base + Block, Block), "the resident remap exclusion was not established");
        const auto outside = CollectWrites(base + 3 * Block, 2 * Block);
        const auto mapping = GuestArena::GuestArenaCommitGeneration_nid_postfix();
        GuestArena::GuestArenaMapDirect_nid_postfix(memory, 3 * Block, cleanup.sections[use], 0, PAGE_READWRITE);
        Require(nativeType(0) == MEM_PRIVATE && nativeType(2 * Block) == MEM_PRIVATE, "the resident remap did not create private memory");
        Require(Watched(base, 3 * Block), "a resident direct remap retained an old import exclusion");
        Require(GuestArena::GuestArenaCommitGeneration_nid_postfix() > mapping, "a resident direct remap reused the mapping generation");
        Require(!UnchangedSince(base, Block, before) && !UnchangedSince(base + 2 * Block, Block, before), "a resident direct remap reused old neighbour stamps");
        Require(UnchangedSince(base + 3 * Block, 2 * Block, outside), "a resident direct remap invalidated memory outside its range");
        const auto after = CollectWritesUncached(base, 3 * Block);
        Require(after > before && UnchangedSince(base, 3 * Block, after), "the resident remap could not establish a current generation");
        memory[Block + 9] = static_cast<std::uint8_t>(0x51 + use);
        CollectWritesUncached(base, 3 * Block);
        Require(!UnchangedSince(base + Block, Block, after), "a CPU store after a resident direct remap was missed");
        std::cout << "resident direct remap " << use << ": private, watched, new mapping generation, CPU store collected\n";
    }
    Unwatch(base, 3 * Block);
    GuestArena::GuestArenaMap_nid_postfix(memory, 3 * Block, cleanup.sections[0], 0, PAGE_READWRITE);
    Require(nativeType(0) == MEM_MAPPED, "the shared remap did not create mapped memory");
    for (std::size_t block = 0; block < 3; ++block) Require(!Watched(base + block * Block, Block), "an ordinary shared remap restored a private import exclusion");
    GuestArena::GuestArenaMapDirect_nid_postfix(memory + 3 * Block, 2 * Block, cleanup.sections[2], 0, PAGE_READWRITE);
    GuestArena::GuestArenaMapDirect_nid_postfix(memory, 3 * Block, cleanup.sections[2], 0, PAGE_READWRITE);
    Require(nativeType(0) == MEM_MAPPED && nativeType(Block) == MEM_MAPPED && nativeType(2 * Block) == MEM_PRIVATE, "the mixed direct remap did not establish shared and private spans");
    Require(!Watched(base, Block) && !Watched(base + Block, Block) && Watched(base + 2 * Block, Block), "the mixed direct remap restored an exclusion outside its new private span");
    std::cout << "resident direct remap controls: shared exclusions retained, only the new private span restored\n";
    Unwatch(base, 3 * Block);
    bool refused = false;
    try {
        GuestArena::GuestArenaMapDirect_nid_postfix(memory, 3 * Block, cleanup.sections[1], 0, 0);
    } catch (const std::system_error& error) {
        Require(error.code().value() == ERROR_INVALID_PARAMETER, "the failed resident remap reported an unexpected error");
        refused = true;
    }
    std::uintptr_t createdBegin = 0;
    std::uintptr_t createdEnd = 0;
    Require(refused && GuestArena::GuestArenaResidentSpan_nid_postfix(base, 1, &createdBegin, &createdEnd), "the failing protection change did not leave a new private span");
    Require(createdBegin == base && createdEnd <= base + 3 * Block && Watched(base, createdEnd - base), "a failed resident remap did not report its completed private span");
    for (auto block = createdEnd; block < base + 3 * Block; block += Block) Require(!Watched(block, Block), "a failed resident remap restored an uncreated span");
    std::cout << "resident direct remap failure: completed private span watched, original mapping error retained\n";
}

void CheckUntrackedHint() {
    constexpr std::size_t bytes = 4 * Block;
    auto* memory = static_cast<volatile std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block));
    GuestArena::GuestArenaCommit_nid_postfix(const_cast<std::uint8_t*>(memory), bytes, PAGE_READWRITE, 16384);
    struct Cleanup {
        void* memory;
        ~Cleanup() {
            GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
        }
    } cleanup{const_cast<std::uint8_t*>(memory)};
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto hinted = base + Block;
    constexpr std::size_t hintedBytes = 2 * Block;
    for (std::size_t offset = 0; offset < bytes; offset += 4096) memory[offset] = 0x11;
    BumpCollectEpoch();
    Require(CollectWrites(base, bytes) != 0 && UntrackedHint(base, bytes) == 0, "watched memory has an untracked hint");
    Unwatch(hinted, hintedBytes);
    const auto stamp = NewestStamp(hinted, hintedBytes);
    const auto first = UntrackedHint(hinted, hintedBytes);
    Require(first != 0, "memory that left the watch has no untracked hint");
    Require(UntrackedHint(base, bytes) == 0 && UntrackedHint(hinted - 4096, 8192) == 0, "a range with watched blocks has an untracked hint");
    std::array<std::uint8_t, 64> host{};
    Require(UntrackedHint(reinterpret_cast<std::uint64_t>(host.data()), host.size()) == 0, "memory outside the arena has an untracked hint");
    Require(!Watched(hinted, hintedBytes) && CollectWrites(hinted, hintedBytes) == 0 && !UnchangedSince(hinted, hintedBytes, TrackerGeneration()), "a hinted range reads as tracked");
    std::array<std::uint64_t, 2> generations{first, first};
    std::array<std::uint8_t, 2> changed{};
    Require(!ChangedBlocks(hinted, hintedBytes, generations, changed), "a hinted range uses block stamps");
    for (unsigned epoch = 0; epoch < 8; ++epoch) {
        BumpCollectEpoch();
        Require(UntrackedHint(hinted, hintedBytes) == first, "an untracked hint moved without a store");
    }
    memory[Block + 3 * 16384 + 8] ^= 1;
    Require(UntrackedHint(hinted, hintedBytes) == first, "an untracked hint was walked twice in one epoch");
    BumpCollectEpoch();
    const auto stored = UntrackedHint(hinted, hintedBytes);
    Require(stored > first, "a CPU store into a hinted range did not move its hint");
    Require(UntrackedHint(hinted + Block, Block) == first, "a CPU store moved the hint of another block");
    BumpCollectEpoch();
    Require(UntrackedHint(hinted, hintedBytes) == stored, "an untracked hint moved again without a store");
    memory[Block + 8] = memory[Block + 8];
    BumpCollectEpoch();
    const auto rewritten = UntrackedHint(hinted, hintedBytes);
    Require(rewritten > stored, "a store of the bytes already there did not move the hint");
    std::thread([&] { memory[2 * Block + 5 * 4096] ^= 1; }).join();
    BumpCollectEpoch();
    const auto threaded = UntrackedHint(hinted, hintedBytes);
    Require(threaded > rewritten, "a store from another thread did not move the hint");
    Require(UntrackedHint(hinted + Block, Block) == threaded && UntrackedHint(hinted, Block) == rewritten, "a walk stamped a store into the wrong block");
    Require(NewestStamp(hinted, hintedBytes) == stamp, "untracked hint walks advanced the block stamps");
    Unwatch(hinted, hintedBytes);
    BumpCollectEpoch();
    Require(UntrackedHint(hinted, hintedBytes) == threaded, "unwatching a hinted range again moved its hint");
    Unwatch(base, Block);
    const auto own = UntrackedHint(base, Block);
    Require(own > threaded && UntrackedHint(hinted, hintedBytes) == threaded, "a range leaving the watch did not start a hint of its own");
    GuestArena::GuestArenaReset_nid_postfix(const_cast<std::uint8_t*>(memory) + Block, hintedBytes);
    GuestArena::GuestArenaCommit_nid_postfix(const_cast<std::uint8_t*>(memory) + Block, hintedBytes, PAGE_READWRITE, 16384);
    BumpCollectEpoch();
    const auto fresh = CollectWrites(hinted, hintedBytes);
    Require(Watched(hinted, hintedBytes) && fresh != 0 && UnchangedSince(hinted, hintedBytes, fresh), "fresh backing of a hinted range is not watched");
    memory[Block + 8] = 0x5a;
    BumpCollectEpoch();
    Require(UntrackedHint(hinted, hintedBytes) == 0, "watched fresh backing has an untracked hint");
    CollectWritesUncached(hinted, hintedBytes);
    Require(!UnchangedSince(hinted, Block, fresh), "an untracked hint walk took a CPU store from watched memory");
    Unwatch(hinted, hintedBytes);
    Require(UntrackedHint(hinted, hintedBytes) > own, "fresh backing leaving the watch did not start a new hint");
}

std::uint64_t ChildHostHole() {
    std::uintptr_t arena = 0;
    std::size_t arenaBytes = 0;
    GuestArena::GuestArenaRange_nid_postfix(&arena, &arenaBytes);
    return (arena + arenaBytes / 2) & ~(std::uint64_t{Block} - 1);
}

void CheckHostHoleInChild() {
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    Require(length != 0 && length < executable.size(), "cannot name the test executable");
    const std::wstring command = L"\"" + std::wstring(executable.data(), length) + L"\" " + HostHoleMode;
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable.data(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process) != 0, "cannot create the host-hole child");
    const bool reserved = VirtualAllocEx(process.hProcess, reinterpret_cast<void*>(ChildHostHole()), Block, MEM_RESERVE, PAGE_NOACCESS) != nullptr;
    const bool resumed = reserved && ResumeThread(process.hThread) != static_cast<DWORD>(-1);
    const bool exited = resumed && WaitForSingleObject(process.hProcess, 60000) == WAIT_OBJECT_0;
    if (!exited) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    DWORD exitCode = 1;
    const bool reported = GetExitCodeProcess(process.hProcess, &exitCode) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Require(reserved, "cannot reserve a host range in the child's arena interval");
    Require(resumed, "cannot resume the host-hole child");
    Require(exited, "the host-hole child did not exit");
    Require(reported && exitCode == 0, "the host-hole child failed");
}

void CheckHostHole() {
    constexpr std::size_t bytes = 64;
    const auto hole = ChildHostHole();
    Require(GuestArena::GuestArenaHostRegionOverlaps_nid_postfix(hole, bytes), "the range reserved before the arena is not an arena host region");
    const auto before = TrackerGeneration();
    Require(!Watched(hole, bytes), "an arena host hole reads as watched");
    Require(CollectWrites(hole, bytes) == 0 && CollectWritesUncached(hole, bytes) == 0, "an arena host hole was collected");
    auto* watched = static_cast<std::uint8_t*>(AllocateWatched(Block, true));
    struct Cleanup {
        void* memory;
        ~Cleanup() { GuestArena::GuestArenaRelease_nid_postfix(memory, Block); }
    } cleanup{watched};
    const auto base = reinterpret_cast<std::uint64_t>(watched);
    for (unsigned epoch = 0; epoch < 2; ++epoch) {
        BumpCollectEpoch();
        watched[7] ^= 1;
        Require(CollectWrites(base + 7, 1) != 0, "a warm host-hole check could not collect its watched range");
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
            Require(CollectWrites(base + 7, 1) != 0, "a warm host-hole check lost its watched range");
            Require(CollectWrites(hole, bytes) == 0, "a repeated host-hole query hit a collect memo");
            Require(CollectWrites(hole + 7, 1) == 0, "a page-rounded host-hole query hit a collect memo");
        }
    }
    Require(StoredOver(hole, bytes, before), "an arena host hole reads as not stored over");
    Require(!UnchangedSince(hole, bytes, before), "an arena host hole reads as unchanged");
    const std::array queries{UnchangedQuery{hole, bytes, before}};
    Require(!UnchangedSinceAll(queries), "an arena host hole reads as unchanged in a batch");
    Require(!UnchangedSinceCollected(hole, bytes, before), "an arena host hole reads as unchanged since a collect");
    Require(MarkWritten(hole, bytes) == 0, "a driver store into an arena host hole is stamped");
    Require(StoreOwnBytes(hole, bytes, [] {}) == 0, "an own store into an arena host hole is stamped");
    Require(!WrittenSince(hole, bytes, before), "an arena host hole reads its stamps as written");
    std::array<std::uint64_t, 1> generations{before};
    std::array<std::uint8_t, 1> changed{};
    std::array<std::uint8_t, 1> cpu{};
    Require(!ChangedBlocks(hole, bytes, generations, changed, cpu) && changed[0] == BlockWritten && cpu[0] == 1, "an arena host hole uses block stamps");
    bool imported = false;
    Require(ImportWatched(hole, bytes, [&] {
        imported = true;
        return true;
    }) && imported, "an import into an arena host hole did not run");
    Require(!Watched(hole, bytes), "an import made an arena host hole watched");
}

std::uint64_t failedCollectBegin = 0;
std::uint64_t failedCollectEnd = 0;
std::uint64_t unwatchHookBegin = 0;
std::uint64_t unwatchHookEnd = 0;

void RecordUnwatch(std::uint64_t address, std::size_t bytes) {
    unwatchHookBegin = address;
    unwatchHookEnd = address + bytes;
}

bool FailFreshCollect(std::uintptr_t address, std::size_t bytes, void (*)(void*, std::uintptr_t, std::uintptr_t), void*) {
    failedCollectBegin = address;
    failedCollectEnd = address + bytes;
    return false;
}

void CheckFailedCollectUnwatch() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x11, 3 * Block);
    Require(Watched(base + Block, Block), "the probe block was not watched");
    SetFreshCollectHook(&FailFreshCollect);
    SetUnwatchHook(&RecordUnwatch);
    const auto stored = MarkWritten(base + Block + 0x8000, 4);
    SetFreshCollectHook(nullptr);
    SetUnwatchHook(nullptr);
    Require(stored == 0, "a failed fresh collect produced trusted stamps");
    Require(!Watched(base + Block, Block), "a failed collect left its block watched");
    Require(Watched(base, Block) && Watched(base + 2 * Block, Block), "a failed collect unwatched unrelated blocks");
    Require(failedCollectBegin == base + Block && failedCollectEnd == base + 2 * Block, "the failed fresh collect did not cover its block");
    Require(unwatchHookBegin <= base + Block && unwatchHookEnd >= base + 2 * Block, "a failed collect did not run the unwatch hook over its block");
}
#endif

void CheckVersionRanges() {
    constexpr std::size_t count = 80;
    constexpr auto bytes = count * Block;
    auto* memory = static_cast<std::uint8_t*>(AllocateWatched(bytes, true));
    struct Cleanup {
        void* memory;
        ~Cleanup() {
#ifdef _WIN32
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
#else
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(memory, bytes);
            munmap(memory, bytes);
#endif
        }
    } cleanup{memory};
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    Require(UnchangedSince(base, bytes, TrackerGeneration()), "a new range has newer version stamps");
    std::memset(memory, 17, bytes);
    const auto initial = CollectWritesUncached(base, bytes);
    Require(initial != 0, "version ranges could not be collected");
    struct Write { std::size_t block; std::uint64_t generation; std::size_t begin; std::size_t end; };
    std::vector<Write> history;
    std::vector<std::uint64_t> checkpoints{initial};
    std::uint32_t random = 0x61387a29;
    const auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
    const auto blocksUnchanged = [&](const UnchangedQuery& query) {
        for (const auto& write : history) {
            const auto begin = base + write.block * Block;
            if (write.generation > query.generation && begin < query.address + query.bytes && query.address < begin + Block) return false;
        }
        return true;
    };
    const auto bytesUnchanged = [&](const UnchangedQuery& query) {
        for (const auto& write : history) {
            const auto begin = base + write.block * Block + write.begin;
            const auto end = base + write.block * Block + write.end;
            if (write.generation > query.generation && begin < query.address + query.bytes && query.address < end) return false;
        }
        return true;
    };
    for (unsigned step = 0; step < 512; ++step) {
        const auto block = step < 3 ? step : next() % count;
        std::uint64_t generation;
        Write write{block, 0, 13, 17};
        if (step % 3 == 0) {
            memory[block * Block + 9] ^= 1;
            generation = CollectWritesUncached(base + block * Block, Block);
            write.begin = 0;
            write.end = 4096;
        } else if (step % 7 == 1) {
            generation = MarkWritten(base + block * Block, Block);
            write.begin = 0;
            write.end = Block;
        } else generation = MarkWritten(base + block * Block + 13, 4);
        Require(generation > checkpoints.back(), "version range write did not advance the tracker");
        write.generation = generation;
        history.push_back(write);
        checkpoints.push_back(generation);
        for (unsigned check = 0; check < 16; ++check) {
            std::array<UnchangedQuery, 3> queries;
            bool all = true;
            for (auto& query : queries) {
                const auto offset = next() % bytes;
                query = {base + offset, 1 + next() % (bytes - offset), checkpoints[next() % checkpoints.size()]};
                const auto same = UnchangedSince(query.address, query.bytes, query.generation);
                Require(!same || bytesUnchanged(query), "version range reads as unchanged over a newer write in it");
                Require(same || !blocksUnchanged(query), "version range reads as changed while its blocks are unchanged");
                all = all && same;
            }
            Require(UnchangedSinceAll(queries) == all, "batched version ranges disagree with the single ranges");
        }
    }
    Require(!UnchangedSince(base, bytes, 0), "an unknown generation was accepted");
    Require(UnchangedSince(base, bytes, checkpoints.back()), "the latest checkpoint was rejected");
}

void CheckCollectBoundaries() {
    constexpr std::size_t bytes = 3 * Block;
    auto* memory = static_cast<std::uint8_t*>(AllocateWatched(bytes));
    struct Cleanup {
        void* memory;
        ~Cleanup() {
#ifdef _WIN32
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
#else
            Unwatch(reinterpret_cast<std::uint64_t>(memory), bytes);
            munmap(memory, bytes);
#endif
        }
    } cleanup{memory};
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 17, bytes);
    bool unboundedObserved = false;
    std::thread observer([&] {
        const auto before = CollectWrites(base + 7, 1);
        memory[7] = 19;
        const auto after = CollectWrites(base + 7, 1);
        unboundedObserved = before != 0 && after > before && !UnchangedSince(base + 7, 1, before);
    });
    observer.join();
    Require(unboundedObserved, "a thread without collect epochs reused a stale observation");
    for (const auto offset : {7u, 4096u + 3u, static_cast<unsigned>(Block - 8), static_cast<unsigned>(Block + 3)}) {
        BumpCollectEpoch();
        const auto before = CollectWrites(base + offset, 16);
        Require(before != 0, "an epoch lost watched coverage");
        memory[offset] ^= 1;
        BumpCollectEpoch();
        Require(CollectWrites(base + offset, 16) > before && !UnchangedSince(base + offset, 16, before), "a new epoch missed a CPU write");
        const auto observed = TrackerGeneration();
        memory[offset + 1] ^= 1;
        Require(CollectWritesUncached(base + offset, 16) > observed && !UnchangedSince(base + offset, 16, observed), "an uncached observation reused an epoch result");
        const auto stored = MarkWritten(base + offset + 2, 1);
        Require(CollectWrites(base + offset, 16) >= stored && !UnchangedSince(base + offset, 16, observed), "a memoized observation hid a driver write");
    }
#ifndef _WIN32
    BumpCollectEpoch();
    Require(CollectWrites(base, 1) != 0, "initial partial-block observation failed");
    Unwatch(base + 4096, 4096);
    Require(CollectWrites(base + 4096, 1) == 0, "an unwatched neighbor retained an epoch observation");
    BumpCollectEpoch();
    const auto before = CollectWrites(base, 16);
    Require(before != 0, "an unwatched neighbor prevented collecting a watched prefix");
    memory[0] ^= 1;
    BumpCollectEpoch();
    Require(CollectWrites(base, 16) > before && !UnchangedSince(base, 16, before), "partial-block collection missed a CPU write");
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(memory + 4096, 4096);
    Require(CollectWrites(base + 4096, 1) != 0, "a re-registered neighbor stayed unwatched");
#endif
}
}

void CheckImportWindow() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x11, 3 * Block);
    const auto synced = CollectWritesUncached(base, 3 * Block);
    Require(synced != 0, "the import window block is not collected");
    Require(ImportWatched(base, 3 * Block, [&] {
        bytes[0x100] = 0x11;
        bytes[Block + 0x2000] = 0x22;
        return true;
    }, true, false), "the import window import failed");
    Require(Watched(base, 3 * Block), "an import window left its range unwatched");
    const std::array<std::uint64_t, 3> generations{synced, synced, synced};
    std::array<std::uint8_t, 3> changed{};
    std::array<std::uint8_t, 3> cpu{};
    Require(ChangedBlocks(base, 3 * Block, generations, changed, cpu), "the import window block is not tracked");
    Require(changed[0] == BlockUnchanged && UnchangedSince(base, Block, synced), "a page an import window resolved without changing its bytes reads as written");
    Require(changed[1] == BlockWritten && WrittenSince(base + Block, Block, synced) && cpu[1] == 1, "a store of new bytes within an import window was missed");
    Require(changed[2] == BlockUnchanged && cpu[2] == 0, "a block the import window did not touch reads as written");
    Require(cpu[0] == 1, "a page an import window resolved no longer counts as possibly touched by the CPU");
    const auto after = CollectWritesUncached(base, 3 * Block);
    bytes[0x100] = 0x33;
    static_cast<void>(CollectWritesUncached(base, 3 * Block));
    Require(!UnchangedSince(base, Block, after), "a store after the import window was missed");
}

void CheckImportWindowOtherThread() {
    void* memory = AllocateWatched(2 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x11, 2 * Block);
    const auto synced = CollectWritesUncached(base, 2 * Block);
    Require(synced != 0, "the other-thread import window block is not collected");
    Require(ImportWatched(base, 2 * Block, [&] {
        std::thread store([&] {
            bytes[0x100] = 0x11;
            bytes[Block + 0x100] = 0x77;
        });
        store.join();
        return true;
    }), "the other-thread import window import failed");
    const std::array<std::uint64_t, 2> generations{synced, synced};
    std::array<std::uint8_t, 2> changed{};
    std::array<std::uint8_t, 2> cpu{};
    Require(ChangedBlocks(base, 2 * Block, generations, changed, cpu), "the other-thread import window block is not tracked");
    Require(changed[0] == BlockUnchanged && cpu[0] == 1, "a same-value store from another thread within an import window lost its possible CPU touch");
    Require(changed[1] == BlockWritten && WrittenSince(base + Block, Block, synced) && cpu[1] == 1, "a store of new bytes from another thread within an import window was missed");
}

void CheckImportWindowZeroPages() {
    void* memory = AllocateWatched(5 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    auto* plain = static_cast<std::uint8_t*>(memory);
    std::memset(plain, 0x11, Block);
    std::memset(plain + Block, 0, 2 * Block);
    std::memset(plain + 3 * Block, 0x44, Block);
    std::memset(plain + 4 * Block, 0x66, Block);
    const auto synced = CollectWritesUncached(base, 5 * Block);
    Require(synced != 0, "the zero page import window block is not collected");
    Require(ImportWatched(base, 5 * Block, [&] {
        bytes[0x100] = 0x11;
        bytes[Block + 0x100] = 0;
        bytes[2 * Block + 0x2000] = 0x22;
        bytes[3 * Block + 0x3000] = 0x44;
        bytes[4 * Block + 0x100] = 0x55;
        return true;
    }), "the zero page import window import failed");
    const std::array<std::uint64_t, 5> generations{synced, synced, synced, synced, synced};
    std::array<std::uint8_t, 5> changed{};
    std::array<std::uint8_t, 5> cpu{};
    Require(ChangedBlocks(base, 5 * Block, generations, changed, cpu), "the zero page import window block is not tracked");
    Require(changed[0] == BlockUnchanged && cpu[0] == 1, "a page before the zero pages of an import window reads as written");
    Require(changed[1] == BlockUnchanged && UnchangedSince(base + Block, Block, synced), "a zero page an import window resolved without changing its bytes reads as written");
    Require(changed[2] == BlockWritten && WrittenSince(base + 2 * Block, Block, synced), "a store of new bytes to a zero page within an import window was missed");
    Require(changed[3] == BlockUnchanged && UnchangedSince(base + 3 * Block, Block, synced), "a page after the zero pages of an import window was compared with other bytes");
    Require(changed[4] == BlockWritten && WrittenSince(base + 4 * Block, Block, synced), "a store of new bytes after the zero pages of an import window was missed");
}

void CheckImportWindowOverUnwatchedBlock() {
    void* memory = AllocateWatched(4 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x11, 4 * Block);
    Unwatch(base + Block, Block);
    const auto synced = CollectWritesUncached(base + 2 * Block, 2 * Block);
    const auto syncedFirst = CollectWritesUncached(base, Block);
    Require(synced != 0 && syncedFirst != 0, "the watched blocks around the unwatched one are not collected");
    bytes[0x500] = 0x44;
    Require(ImportWatched(base, 4 * Block, [&] {
        bytes[0x100] = 0x11;
        bytes[2 * Block + 0x100] = 0x11;
        bytes[3 * Block + 0x2000] = 0x22;
        return true;
    }), "the import over an unwatched block failed");
    Require(!Watched(base + Block, Block), "an import window made an unwatched block watched");
    Require(Watched(base, Block) && Watched(base + 2 * Block, 2 * Block), "an import window over an unwatched block took the watched blocks out of the write watch");
    const std::array<std::uint64_t, 2> generations{synced, synced};
    std::array<std::uint8_t, 2> changed{};
    std::array<std::uint8_t, 2> cpu{};
    Require(ChangedBlocks(base + 2 * Block, 2 * Block, generations, changed, cpu), "the watched blocks after an unwatched block are not tracked");
    Require(changed[0] == BlockUnchanged && UnchangedSince(base + 2 * Block, Block, synced), "a page an import window over an unwatched block resolved without changing its bytes reads as written");
    Require(changed[1] == BlockWritten && WrittenSince(base + 3 * Block, Block, synced), "a store of new bytes within an import window over an unwatched block was missed");
    Require(WrittenSince(base, Block, syncedFirst), "a store before an import window over an unwatched block was missed");
    const auto after = CollectWritesUncached(base, Block);
    bytes[0x100] = 0x33;
    static_cast<void>(CollectWritesUncached(base, Block));
    Require(after != 0 && !UnchangedSince(base, Block, after), "a store after an import window over an unwatched block was missed");
}

void CheckImportWindowFootprint() {
#ifdef __GLIBC__
    constexpr std::size_t bytes = std::size_t{64} << 20;
    void* memory = AllocateWatched(bytes);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x5a, bytes);
    Require(CollectWritesUncached(base, bytes) != 0, "the footprint import window block is not collected");
    const auto heapBytes = [] {
        const auto info = mallinfo2();
        return info.uordblks + info.hblkhd;
    };
    const auto before = heapBytes();
    std::size_t during = 0;
    Require(ImportWatched(base, bytes, [&] {
        during = heapBytes();
        return true;
    }), "the footprint import window import failed");
    Require(Watched(base, bytes), "the footprint import window left its range unwatched");
    const auto held = during > before ? during - before : 0;
    Require(held <= bytes / 64, "an import window over " + std::to_string(bytes >> 20) + " MiB held " + std::to_string(held >> 20) + " MiB of host memory during the import: it copies the range instead of hashing its pages");
    auto* plain = static_cast<volatile std::uint8_t*>(memory);
    const auto synced = CollectWritesUncached(base, bytes);
    Require(ImportWatched(base, bytes, [&] {
        plain[0x100] = 0x5a;
        plain[Block + 0x100] = 0x5b;
        return true;
    }), "the footprint import window re-import failed");
    Require(UnchangedSince(base, Block, synced), "a page a hashed import window resolved without changing its bytes reads as written");
    Require(WrittenSince(base + Block, Block, synced), "a store of new bytes within a hashed import window was missed");
#endif
}

void ProtectPage(std::uint64_t address, bool readable) {
#ifdef _WIN32
    DWORD previous = 0;
    Require(VirtualProtect(reinterpret_cast<void*>(address), 4096, readable ? PAGE_READWRITE : PAGE_NOACCESS, &previous) != 0, "cannot change the protection of a test page");
#else
    Require(mprotect(reinterpret_cast<void*>(address), 4096, readable ? PROT_READ | PROT_WRITE : PROT_NONE) == 0, "cannot change the protection of a test page");
#endif
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(static_cast<std::uintptr_t>(address), 4096);
}

void CheckStoreAcrossImports() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto boundary = base + 2 * Block;
    std::memset(memory, 0x11, 3 * Block);
    Require(ImportWatched(base, 2 * Block, [] { return true; }) && Watched(base, 2 * Block), "an import that stays watched left the watch");
    Require(ImportWatched(boundary, Block, [] { return true; }, false) && !Watched(boundary, Block), "an import that leaves the watch stayed watched");
    const auto before = CollectWrites(base, 2 * Block);
    Require(before != 0 && UnchangedSince(base, 2 * Block, before), "the watched import was not collected");
    Require(MarkWritten(boundary - 64, 128) == 0, "a GPU write across a watched import and an unwatched one is trusted");
    Require(!UnchangedSince(base + Block, Block, before), "a GPU write across a watched import and an unwatched one left the watched import's last block unchanged");
    Require(StoredOver(boundary - 64, 64, before) && !StoredOver(base + Block, Block - 64, before), "a GPU write across a watched import and an unwatched one is not seen over exactly its watched bytes");
    Require(UnchangedSince(base, Block, before), "a GPU write across a watched import and an unwatched one stamped a block it did not write");
    const auto beforeOwn = TrackerGeneration();
    Require(StoreOwnBytes(boundary - 64, 128, [&] { std::memset(reinterpret_cast<void*>(boundary - 64), 0x22, 128); }) == 0, "an own store across a watched import and an unwatched one is trusted");
    Require(!UnchangedSince(base + Block, Block, beforeOwn) && UnchangedSince(base, Block, beforeOwn), "an own store across a watched import and an unwatched one did not stamp exactly the watched block it wrote");
}

void CheckUnwatchedImport() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x11, 3 * Block);
    const auto synced = CollectWritesUncached(base, 3 * Block);
    Require(synced != 0, "the unwatched import block is not collected");
    const auto imported = base + Block / 2;
    const std::size_t importedBytes = Block + Block / 2;
    const auto unreadable = base + Block + 0x3000;
    ProtectPage(unreadable, false);
    bool threw = false;
    bool ran = false;
    try {
        static_cast<void>(ImportWatched(imported, importedBytes, [&] {
            ran = true;
            return true;
        }));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Require(threw && !ran, "an import that stays watched did not read its range before the import");
    threw = false;
    try {
        Require(ImportWatched(imported, importedBytes, [&] {
            bytes[Block / 2 + 0x100] = 0x22;
            ran = true;
            return true;
        }, false), "an import that leaves the watch failed");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Require(!threw && ran, "an import that leaves the watch read its range");
    ProtectPage(unreadable, true);
    Require(!Watched(imported, importedBytes) && !Watched(imported, 4096) && !Watched(base + Block, Block), "an import that leaves the watch is still watched");
    Require(Watched(base + 2 * Block, Block), "an import that leaves the watch unwatched a block outside its range");
    Require(UnchangedSince(base + 2 * Block, Block, synced), "an import that leaves the watch stamped a block outside its range");
#ifndef _WIN32
    Require(Watched(base, Block / 2), "an import that leaves the watch unwatched the pages before its range");
    const std::array<std::uint64_t, 1> generations{synced};
    std::array<std::uint8_t, 1> changed{};
    std::array<std::uint8_t, 1> cpu{};
    Require(ChangedBlocks(base, Block / 2, generations, changed, cpu), "the pages before an import that leaves the watch are not tracked");
    Require(changed[0] == BlockUnchanged && UnchangedSince(base, Block / 2, synced) && !WrittenSince(base, Block / 2, synced), "a store to a page that left the watch reads as a store to the watched pages of its block");
    Require(cpu[0] == 1, "a page resolved while an import left the watch no longer counts as possibly touched by the CPU");
    const auto after = CollectWritesUncached(base, Block / 2);
    bytes[0x100] = 0x33;
    static_cast<void>(CollectWritesUncached(base, Block / 2));
    Require(after != 0 && !UnchangedSince(base, Block / 2, after), "a store beside an import that left the watch was missed");
#endif
}

struct HookCall {
    std::uint64_t address;
    std::size_t bytes;
    bool watched;
};

std::vector<HookCall> hookCalls;
std::uint64_t hookProbe = 0;
std::size_t hookProbeBytes = 0;

void RecordHook(std::uint64_t address, std::size_t bytes) {
    hookCalls.push_back({address, bytes, Watched(hookProbe, hookProbeBytes)});
}

struct RecordedHook {
    RecordedHook() {
        hookCalls.clear();
        SetUnwatchHook(&RecordHook);
    }
    ~RecordedHook() { SetUnwatchHook(nullptr); }
    RecordedHook(const RecordedHook&) = delete;
    RecordedHook& operator=(const RecordedHook&) = delete;
};

void RequireHookOver(std::uint64_t address, std::size_t bytes, const std::string& path) {
    Require(hookCalls.size() == 1, path + " did not run the unwatch hook once");
    const auto& call = hookCalls.front();
    Require(call.address <= address && address + bytes <= call.address + call.bytes, path + " gave the unwatch hook a range that misses its own");
    Require(call.watched, path + " ran the unwatch hook after its range left the write watch");
}

void CheckUnwatchHooks() {
    void* memory = AllocateWatched(8 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x11, 8 * Block);
    Require(CollectWritesUncached(base, 8 * Block) != 0, "the hooked block is not collected");
    const RecordedHook recorded;
    hookProbe = base;
    hookProbeBytes = Block;
    Unwatch(base, Block);
    RequireHookOver(base, Block, "Unwatch");
    Require(!Watched(base, Block), "Unwatch left its range watched");

    hookCalls.clear();
    hookProbe = base + Block;
    bool hookedBeforeImport = false;
    Require(ImportWatched(base + Block, Block, [&] {
        hookedBeforeImport = hookCalls.size() == 1;
        return true;
    }, false), "an import that does not stay watched failed");
    RequireHookOver(base + Block, Block, "an import that does not stay watched");
    Require(hookedBeforeImport, "an import that does not stay watched ran the unwatch hook after the import");
    Require(!Watched(base + Block, Block), "an import that does not stay watched left its range watched");

    hookCalls.clear();
    Require(ImportWatched(base + 2 * Block, Block, [] { return true; }), "an import that stays watched failed");
    Require(hookCalls.empty() && Watched(base + 2 * Block, Block), "an import whose page walk vouched for its pages ran the unwatch hook or gave up its range");

#ifndef _WIN32
    hookCalls.clear();
    const auto late = base + 4 * Block;
    hookProbe = late;
    bool hookedBeforeLoss = false;
    Require(ImportWatched(late, 3 * Block, [&] {
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(reinterpret_cast<const void*>(late + 2 * Block), Block);
        hookedBeforeLoss = !hookCalls.empty();
        return true;
    }), "an import whose range lost a block during the import failed");
    RequireHookOver(late, 3 * Block, "an import whose page walk failed after the import");
    Require(!hookedBeforeLoss, "an import whose page walk failed after the import ran the unwatch hook before it");
    Require(!Watched(late, Block), "an import whose page walk failed after the import left its range watched");
#endif
}

class PartlyCommitted {
public:
    static constexpr std::size_t Bytes = 4 * Block;

    PartlyCommitted() {
#ifdef _WIN32
        memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocateAtOrAbove_nid_postfix(GuestArena::GuestArenaHighestFree_nid_no_patch(Bytes, Block), Bytes, Block));
#else
        void* raw = mmap(nullptr, Bytes + Block, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        Require(raw != MAP_FAILED, "cannot reserve the partly committed range");
        const auto begin = reinterpret_cast<std::uintptr_t>(raw);
        const auto aligned = (begin + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1);
        if (aligned != begin) munmap(raw, aligned - begin);
        if (aligned + Bytes != begin + Bytes + Block) munmap(reinterpret_cast<void*>(aligned + Bytes), begin + Block - aligned);
        memory = reinterpret_cast<std::uint8_t*>(aligned);
#endif
        Require(memory != nullptr, "cannot reserve the partly committed range");
    }

    ~PartlyCommitted() {
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(memory, Bytes);
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(Address(), Bytes);
        GuestArena::GuestArenaRelease_nid_postfix(memory, Bytes);
#else
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(memory, Bytes);
        munmap(memory, Bytes);
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(Address(), Bytes);
#endif
    }

    PartlyCommitted(const PartlyCommitted&) = delete;
    PartlyCommitted& operator=(const PartlyCommitted&) = delete;

    void Commit(std::size_t block) {
#ifdef _WIN32
        GuestArena::GuestArenaCommit_nid_postfix(memory + block * Block, Block, PAGE_READWRITE, Block);
#else
        Require(mmap(memory + block * Block, Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED, "cannot commit a block of the partly committed range");
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(memory + block * Block, Block);
#endif
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(Address(block), Block);
    }

    void Decommit(std::size_t block) {
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(memory + block * Block, Block);
#else
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(memory + block * Block, Block);
        Require(mmap(memory + block * Block, Block, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) != MAP_FAILED, "cannot decommit a block of the partly committed range");
#endif
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(Address(block), Block);
    }

    std::uint8_t* Data() const { return memory; }
    std::uint64_t Address(std::size_t block = 0) const { return reinterpret_cast<std::uint64_t>(memory) + block * Block; }

private:
    std::uint8_t* memory = nullptr;
};

using Runs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

std::uint64_t CollectCommitted(const PartlyCommitted& range, Runs& runs) {
    BumpCollectEpoch();
    return CollectCommittedWrites(range.Address(), PartlyCommitted::Bytes, runs);
}

void CheckCommittedRanges() {
    PartlyCommitted range;
    const auto base = range.Address();
    range.Commit(0);
    std::memset(range.Data(), 0x11, Block);
    Require(CollectWrites(base, PartlyCommitted::Bytes) == 0, "a partly committed range was collected as a whole");
    Runs runs;
    const Runs first{{base, base + Block}};
    const auto synced = CollectCommitted(range, runs);
    Require(synced != 0 && runs == first, "the committed part of a partly committed range was not collected");
    Require(UnchangedCommittedSince(runs, synced), "a collected committed part reads as changed");

    range.Data()[8] = 0x22;
    const auto written = CollectCommitted(range, runs);
    Require(written > synced && runs == first && !UnchangedCommittedSince(runs, synced) && UnchangedCommittedSince(runs, written), "a CPU write to the committed part was missed");
    Require(MarkWritten(base + 16, 4) != 0 && !UnchangedCommittedSince(first, written), "a driver store into the committed part was missed");
    Require(UnchangedCommittedSince(first, CollectCommitted(range, runs)), "a collect after a driver store reads the committed part as changed");

    const auto beforeCommit = CollectCommitted(range, runs);
    range.Commit(2);
    const Runs both{{base, base + Block}, {range.Address(2), range.Address(3)}};
    const auto grown = CollectCommitted(range, runs);
    Require(grown != 0 && runs == both, "a block committed inside the range is not described");
    Require(!UnchangedCommittedSince(runs, beforeCommit), "a block committed inside the range reads as unchanged since before the commit");
    Require(UnchangedCommittedSince(runs, grown), "the committed parts read as changed after their collect");

    range.Decommit(2);
    range.Commit(2);
    const auto recommitted = CollectCommitted(range, runs);
    Require(recommitted != 0 && runs == both && !UnchangedCommittedSince(runs, grown), "a block decommitted and committed again reads as unchanged");

    range.Decommit(2);
    Require(CollectCommitted(range, runs) != 0 && runs == first, "a decommitted block is still described as committed");

#ifdef _WIN32
    HANDLE firstSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, Block, nullptr);
    HANDLE secondSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, Block, nullptr);
    struct Sections {
        HANDLE first;
        HANDLE second;
        ~Sections() {
            if (first != nullptr) CloseHandle(first);
            if (second != nullptr) CloseHandle(second);
        }
    } sections{firstSection, secondSection};
    Require(firstSection != nullptr && secondSection != nullptr, "cannot create the shared backings of the partly committed range");
    const auto beforeMap = CollectCommitted(range, runs);
    GuestArena::GuestArenaMap_nid_postfix(range.Data() + 3 * Block, Block, firstSection, 0, PAGE_READWRITE);
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(range.Address(3), Block);
    const Runs viewed{{base, base + Block}, {range.Address(3), range.Address(4)}};
    const auto mapped = CollectCommitted(range, runs);
    Require(mapped != 0 && runs == viewed, "a view mapped inside the range is not described");
    Require(!UnchangedCommittedSince(runs, beforeMap), "a view mapped inside the range reads as unchanged since before the mapping");
    Require(UnchangedCommittedSince(runs, mapped), "a collected view reads as changed");
    GuestArena::GuestArenaReset_nid_postfix(range.Data() + 3 * Block, Block);
    GuestArena::GuestArenaMap_nid_postfix(range.Data() + 3 * Block, Block, secondSection, 0, PAGE_READWRITE);
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(range.Address(3), Block);
    Require(CollectCommitted(range, runs) != 0 && runs == viewed && !UnchangedCommittedSince(runs, mapped), "a view replaced inside the range reads as unchanged");
    GuestArena::GuestArenaReset_nid_postfix(range.Data() + 3 * Block, Block);
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(range.Address(3), Block);
    Require(CollectCommitted(range, runs) != 0 && runs == first, "an unmapped view is still described as committed");
#endif

    range.Decommit(0);
    Require(CollectCommitted(range, runs) != 0 && runs.empty() && UnchangedCommittedSince(runs, TrackerGeneration()), "a range with no committed page did not collect as empty");
    Require(!UnchangedCommittedSince(first, 0), "an unknown generation was accepted for the committed parts");
}

// APS5_ARCHB_FIX_ALLOC_CHURN: the epoch collect map keeps its nodes across epochs. A kept entry must
// never stand for a walk of a later epoch: a CPU store made before an epoch is always collected in
// it, while the ranges nobody wrote keep their generations, epoch after epoch.
void CheckAllocChurnMemo() {
    using Recorder = AgcDriver::ArchB::Recorder;
    Recorder::Boundary(true, 0, Recorder::Clock::now());
    const AgcDriver::ArchB::AllocChurnScope churn(true);
    constexpr std::size_t ranges = 8;
    void* memory = AllocateWatched(ranges * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0, ranges * Block);
    BumpCollectEpoch();
    std::vector<std::uint64_t> collected(ranges);
    for (std::size_t i = 0; i < ranges; ++i) {
        collected[i] = CollectWrites(base + i * Block, Block);
        Require(collected[i] != 0, "the churn fixture was not collected");
    }
    for (std::size_t epoch = 0; epoch < 64; ++epoch) {
        const auto written = epoch % ranges;
        bytes[written * Block + 64 + epoch] = static_cast<std::uint8_t>(epoch + 1);
        BumpCollectEpoch();
        for (std::size_t i = 0; i < ranges; ++i) {
            const auto range = base + i * Block;
            const auto generation = CollectWrites(range, Block);
            Require(generation != 0 && UnchangedSince(range, Block, generation), "a collect in a later epoch did not prove its range");
            if (i == written) {
                Require(!UnchangedSince(range, Block, collected[i]), "a kept memo entry hid a CPU store made before the epoch");
                collected[i] = generation;
            } else {
                Require(UnchangedSince(range, Block, collected[i]), "an unwritten range lost its generation");
            }
            Require(CollectWrites(range, Block) != 0 && UnchangedSince(range, Block, collected[i]), "a memoized collect in the same epoch failed");
        }
    }
    // The kept nodes were overwritten in place (the counter the gate requires).
    const auto counts = Recorder::Collect(Recorder::Clock::now()).counters;
    const auto reuses = counts.find("fix_alloc_churn_memo_reuses");
    Require(reuses != counts.end() && reuses->second >= 64 * ranges, "kept memo nodes were not reused in place");
}

void CheckFailedCollectMemo() {
    PartlyCommitted range;
    range.Commit(0);
    const auto base = range.Address();
    BumpCollectEpoch();
    for (int repeat = 0; repeat < 3; ++repeat) Require(CollectWrites(base, PartlyCommitted::Bytes) == 0, "a repeated partial collection manufactured a generation");
    const auto first = CollectWrites(base, Block);
    Require(first != 0 && UnchangedSince(base, Block, first), "a failed full collection poisoned its committed subset");
    Require(MarkWritten(base, 4) != 0 && CollectWrites(base, PartlyCommitted::Bytes) == 0 && !UnchangedSince(base, Block, first), "a driver write turned a failed collection into a proof or lost its stamp");
    const auto afterDriver = CollectWrites(base, Block);
    Require(afterDriver != 0 && UnchangedSince(base, Block, afterDriver), "the committed subset did not collect after a driver write");
    range.Data()[8] = 0x22;
    BumpCollectEpoch();
    Require(CollectWrites(base, PartlyCommitted::Bytes) == 0 && CollectWrites(base, Block) != 0 && !UnchangedSince(base, Block, afterDriver), "a failed full collection hid a CPU store from its subset");
    for (std::size_t block = 1; block < PartlyCommitted::Bytes / Block; ++block) range.Commit(block);
    Require(CollectWrites(base, PartlyCommitted::Bytes) != 0, "committing the missing pages did not invalidate a failed collection in the same epoch");
    range.Decommit(1);
    Require(CollectWrites(base, PartlyCommitted::Bytes) == 0 && CollectWritesUncached(base, PartlyCommitted::Bytes) == 0, "decommitting pages retained a successful collection");
    range.Commit(1);
    Require(CollectWrites(base, PartlyCommitted::Bytes) != 0, "recommitting pages retained a failed collection");
    Unwatch(base, Block);
    Require(CollectWrites(base, PartlyCommitted::Bytes) == 0, "unwatching pages retained a successful collection");
#ifdef _WIN32
    const Runs restored{{base, base + Block}};
    Rewatch(restored);
    Require(CollectWrites(base, PartlyCommitted::Bytes) != 0, "rewatching pages retained a failed collection");
#endif
}

#ifdef _WIN32
void CheckTargetCleanProof() {
    constexpr std::size_t bytes = 4 * Block;
    auto* memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(2 * bytes, Block));
    struct Cleanup {
        std::uint8_t* memory;
        HANDLE section = nullptr;
        ~Cleanup() {
            GuestArena::GuestArenaReset_nid_postfix(memory, 2 * bytes);
            GuestArena::GuestArenaRelease_nid_postfix(memory, 2 * bytes);
            if (section != nullptr) {
                GuestArena::GuestArenaForgetSection_nid_postfix(section);
                CloseHandle(section);
            }
        }
    } cleanup{memory};
    Require(memory != nullptr, "cannot reserve the clean proof fixture");
    cleanup.section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, bytes, nullptr);
    Require(cleanup.section != nullptr, "cannot create the clean proof backing");
    GuestArena::GuestArenaMapDirect_nid_postfix(memory, bytes, cleanup.section, 0, PAGE_READWRITE);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::array<UnchangedQuery, 2> queries{{{base + 1, 3 * Block - 2, 0}, {base + 3 * Block, Block, 0}}};
    const auto baseline = [&] {
        const auto generation = CollectWritesUncached(base, bytes);
        Require(generation != 0, "the clean proof fixture is not collected");
        for (auto& query : queries) query.generation = generation;
    };
    Require(!UnchangedSinceClean(queries), "a new target without a generation was proved clean");
    baseline();
    const auto unchanged = WriteGeneration();
    for (int epoch = 0; epoch < 3; ++epoch) {
        BumpCollectEpoch();
        Require(UnchangedSinceClean(queries), "a resident clean target was not proved across epochs");
    }
    Require(WriteGeneration() == unchanged, "a clean proof performed a resetting collect");
    for (const auto offset : {std::size_t{1}, Block + 4097, 3 * Block - 2, 3 * Block + 17}) {
        static_cast<volatile std::uint8_t*>(memory)[offset] = 0x31;
        BumpCollectEpoch();
        Require(!UnchangedSinceClean(queries) && !UnchangedSinceClean(queries), "a target or DCC CPU write was missed or consumed by the clean query");
        const auto collected = CollectWritesUncached(base, bytes);
        Require(collected != 0 && !UnchangedSinceClean(queries), "a CPU write consumed by another collector was missed");
        for (auto& query : queries) query.generation = collected;
        Require(UnchangedSinceClean(queries), "the refreshed target was not clean after its CPU write");
    }
    for (std::size_t offset = 0; offset < bytes; offset += 4096) static_cast<volatile std::uint8_t*>(memory)[offset] = 0x42;
    Require(!UnchangedSinceClean(queries), "a full write-watch result buffer was interpreted as clean");
    baseline();
    std::thread([&] { MarkWritten(base + Block, 4); }).join();
    Require(!UnchangedSinceClean(queries), "a GPU or DMA write stamp from another thread was missed");
    baseline();
    MarkPossiblyWritten(base + 3 * Block, 4);
    Require(!UnchangedSinceClean(queries), "a possible DCC write was missed");
    baseline();
    GuestArena::GuestArenaMapDirect_nid_postfix(memory, bytes, cleanup.section, 0, PAGE_READWRITE);
    Require(!UnchangedSinceClean(queries), "a remapped target retained its clean proof");
    baseline();
    GuestArena::GuestArenaMapDirect_nid_postfix(memory + bytes, bytes, cleanup.section, 0, PAGE_READWRITE);
    baseline();
    Require(UnchangedSinceClean(queries), "an armed shared target was not clean");
    Require(GuestArena::GuestArenaHandleWrite_nid_postfix(base + bytes + Block), "the writable alias was not armed");
    static_cast<volatile std::uint8_t*>(memory)[bytes + Block] = 0x53;
    Require(memory[Block] == 0x53 && !UnchangedSinceClean(queries), "a CPU store through a different guest alias was missed");
    baseline();
    {
        GuestArena::HostWrite write(memory, bytes);
        Require(write.Open() && !UnchangedSinceClean(queries), "an open host write was proved clean");
    }
    baseline();
    GuestArena::GuestArenaPinWritable_nid_postfix(memory, bytes);
    Require(!UnchangedSinceClean(queries), "a pinned writable alias was proved clean");
    GuestArena::GuestArenaUnpinWritable_nid_postfix(memory, bytes);
    baseline();
    Unwatch(base, Block);
    Require(!UnchangedSinceClean(queries), "an unwatched target was proved clean");
    Require(!UnchangedSinceClean({}), "an empty clean proof was accepted");
    const std::array<UnchangedQuery, 1> overflow{{{UINT64_MAX - 1, 4, 1}}};
    Require(!UnchangedSinceClean(overflow), "an overflowing clean proof was accepted");
}
#endif

void CheckAdoptLatestEpoch() {
    std::uint64_t bumped = 0;
    std::thread([&] {
        BumpCollectEpoch();
        bumped = CollectEpoch();
    }).join();
    Require(bumped != 0, "a bumped thread has no collect epoch");
    std::uint64_t before = 1;
    std::uint64_t adopted = 0;
    bool keptOwn = false;
    std::thread([&] {
        before = CollectEpoch();
        AdoptLatestCollectEpoch();
        adopted = CollectEpoch();
        BumpCollectEpoch();
        const auto own = CollectEpoch();
        AdoptLatestCollectEpoch();
        keptOwn = CollectEpoch() == own;
    }).join();
    Require(before == 0, "a thread that never bumped had a collect epoch");
    Require(adopted == bumped, "a thread that never bumped did not take the newest bumped collect epoch");
    Require(keptOwn, "adopting the newest collect epoch moved a thread off its own");
}

#ifdef _WIN32
void CheckVqMemo(std::wstring_view mode) {
    using Recorder = AgcDriver::ArchB::Recorder;
    Recorder::Boundary(true, 0, Recorder::Clock::now());
    constexpr std::size_t bytes = Block;
    auto* memory = static_cast<unsigned char*>(GuestArena::GuestArenaAllocate_nid_postfix(2 * bytes, bytes));
    const auto base = reinterpret_cast<std::uintptr_t>(memory);
    struct Release {
        void* memory;
        ~Release() {
            GuestArena::GuestArenaReset_nid_postfix(memory, 2 * bytes);
            GuestArena::GuestArenaRelease_nid_postfix(memory, 2 * bytes);
        }
    } release{memory};
    const GuestArena::QueryMemoContext context{&UnwatchSerial, &ForgetSerial, true, [](const char* name) { Recorder::Count(name); }};
    struct Result {
        bool collected;
        std::size_t count;
        std::array<void*, 32> pages{};
        bool operator==(const Result&) const = default;
    };
    const auto collect = [&](bool enabled = true, std::uintptr_t address = 0, std::size_t size = Block) {
        Result result{false, 32};
        result.collected = GuestArena::GuestArenaCollectWritesMemo_nid_no_patch(address == 0 ? base : address, size, result.pages.data(), &result.count, false, false, enabled ? &context : nullptr);
        return result;
    };
    const auto counters = [] {
        auto counts = Recorder::Collect(Recorder::Clock::now()).counters;
        Require(!counts.contains("fix_vq_memo_mismatches"), "query memo verification disagreed");
        return counts;
    };
    const auto requireCount = [](const auto& counts, const char* name) {
        const auto found = counts.find(name);
        if (found == counts.end() || found->second == 0) {
            for (const auto& [key, value] : counts) std::cerr << key << "=" << value << "\n";
        }
        Require(found != counts.end() && found->second != 0, std::string("missing query memo counter: ") + name);
    };
    const auto commit = [&] { GuestArena::GuestArenaCommit_nid_postfix(memory, bytes, PAGE_READWRITE, bytes); };
    const auto baseline = [&] {
        const auto first = collect();
        Require(collect() == first, "unchanged query changed the collected pages");
        const auto counts = counters();
        requireCount(counts, "fix_vq_memo_hits");
        requireCount(counts, "fix_vq_memo_verified");
        return first;
    };
    if (mode == L"reuse") {
        Require(!baseline().collected, "reserved memory was collectable");
        commit();
        memory[0] = 23;
        const auto first = baseline();
        Require(first.collected && first.count == 1 && first.pages[0] == memory, "private query lost dirty pages");
        memory[4096] = 17;
        const auto second = collect();
        Require(second.collected && second.count == 2, "query reuse hid a CPU store");
        static_cast<void>(counters());
        Require(collect(true, base, 2 * bytes) == collect(false, base, 2 * bytes), "a longer collection changed the query result");
        requireCount(counters(), "fix_vq_memo_hits");
    } else if (mode == L"commit") {
        baseline();
        commit();
        Require(collect().collected, "commit reused a reserved answer");
        requireCount(counters(), "fix_vq_memo_refused_commit");
        baseline();
        GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
        Require(!collect().collected, "decommit reused a committed answer");
        requireCount(counters(), "fix_vq_memo_refused_mapping");
        baseline();
        commit();
        Require(collect().collected, "recommit reused a reserved answer");
    } else if (mode == L"protect") {
        commit();
        baseline();
        DWORD previous = 0;
        Require(GuestArena::GuestArenaVirtualProtect_nid_no_patch(memory, 4096, PAGE_READONLY, &previous), "fixture tracked protect failed");
        Require(collect() == collect(false), "tracked protection changed collection output");
        requireCount(counters(), "fix_vq_memo_refused_protection");
        GuestArena::GuestArenaSetProtection_nid_postfix(base, bytes, PAGE_READONLY);
        Require(VirtualProtect(memory, bytes, PAGE_READONLY, &previous), "fixture protect failed");
        Require(collect() == collect(false), "protection change changed collection output");
        const auto counts = counters();
        requireCount(counts, "fix_vq_memo_refused_external_protection");
        Require(!counts.contains("fix_vq_memo_hits") || counts.at("fix_vq_memo_hits") == 0, "external protection retained query authority");
    } else if (mode == L"map") {
        baseline();
        const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
        Require(section != nullptr, "cannot create query memo section");
        GuestArena::GuestArenaMapDirect_nid_postfix(memory, bytes, section, 0, PAGE_READWRITE);
        Require(collect() == collect(false), "map reused a reserved answer");
        GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
        Require(!collect().collected, "unmap retained committed memory");
        requireCount(counters(), "fix_vq_memo_refused_commit");
        GuestArena::GuestArenaForgetSection_nid_postfix(section);
        CloseHandle(section);
        baseline();
    } else if (mode == L"unwatch") {
        baseline();
        Unwatch(base + bytes, bytes);
        Require(!collect().collected, "unwatch changed reserved collection output");
        requireCount(counters(), "fix_vq_memo_refused_unwatch");
    } else if (mode == L"forget") {
        static_cast<void>(CommittedRanges(base, bytes));
        baseline();
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(base, bytes);
        collect();
        requireCount(counters(), "fix_vq_memo_refused_forget");
        baseline();
        { GuestAllocations::Mutation mutation; }
        collect();
        requireCount(counters(), "fix_vq_memo_refused_mapping");
    } else if (mode == L"coverage") {
        commit();
        baseline();
        GuestArena::GuestArenaPinWritable_nid_postfix(memory, bytes);
        collect();
        GuestArena::GuestArenaUnpinWritable_nid_postfix(memory, bytes);
        collect();
        requireCount(counters(), "fix_vq_memo_refused_pinned");
        auto* host = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        Require(host != nullptr, "cannot allocate untracked memory");
        const auto address = reinterpret_cast<std::uintptr_t>(host);
        Require(collect(true, address) == collect(false, address), "untracked query changed collection output");
        Require(collect(true, address) == collect(false, address), "untracked query changed on repetition");
        requireCount(counters(), "fix_vq_memo_refused_untracked");
        VirtualFree(host, 0, MEM_RELEASE);
        std::uintptr_t arena = 0;
        std::size_t arenaBytes = 0;
        GuestArena::GuestArenaRange_nid_postfix(&arena, &arenaBytes);
        collect(true, arena + arenaBytes - 4096, 8192);
        requireCount(counters(), "fix_vq_memo_refused_untracked");
    } else if (mode == L"threads") {
        baseline();
        std::array<std::thread, 4> workers;
        for (auto& worker : workers) worker = std::thread([&] {
            for (unsigned i = 0; i < 32; ++i) {
                collect();
                GuestArena::GuestArenaCommit_nid_postfix(memory, bytes, PAGE_READWRITE, bytes);
                collect();
                GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
            }
        });
        for (auto& worker : workers) worker.join();
    } else if (mode == L"toggle") {
        const auto first = baseline();
        Require(collect(false) == first && collect() == first, "on/off/on changed reserved output");
        requireCount(counters(), "fix_vq_memo_hits");
        commit();
        const auto second = collect(false);
        Require(collect() == second, "an off-path commit left a stale on-path answer");
        requireCount(counters(), "fix_vq_memo_refused_commit");
        baseline();
        GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
        BumpCollectEpoch();
        Require(CollectWrites(base, bytes, true) == 0, "prelock collection accepted reserved memory");
        BumpCollectEpoch();
        Require(CollectWrites(base, bytes, false) == 0, "off collection accepted reserved memory");
        BumpCollectEpoch();
        Require(CollectWrites(base, bytes, true) == 0, "toggled collection accepted reserved memory");
        requireCount(counters(), "fix_vq_memo_hits");
    } else throw std::runtime_error("unknown query memo test");
    static_cast<void>(counters());
}
#endif

#ifdef _WIN32
// APS5_ARCHB_FIX_WRITE_TRACKING_BOOKKEEPING (run with it on; the verify variants also set
// APS5_VERIFY_WRITE_TRACKING_BOOKKEEPING=1, where every fast answer is checked against the original).
// A CPU write between two collects of one epoch is seen by the next epoch, exactly as with the
// original memo; an epoch boundary (a submission, a CPU-satisfiable label wait, a drain or a reap all
// call BumpCollectEpoch) always forces a fresh walk; unwatching, remapping and decommitting retire
// the cached answer; the generation a hit returns is the tracker's current one.
void CheckWriteTrackingBookkeeping(std::wstring_view mode) {
    using Recorder = AgcDriver::ArchB::Recorder;
    using AgcDriver::ArchB::Feature;
    using AgcDriver::ArchB::Switches;
    const bool verify = [] {
        const char* value = std::getenv("APS5_VERIFY_WRITE_TRACKING_BOOKKEEPING");
        return value != nullptr && std::string_view(value) == "1";
    }();
    std::filesystem::path control;
    if (mode == L"toggle" || mode == L"bench") {
        control = std::filesystem::temp_directory_path() / ("wt-bookkeeping-" + std::to_string(GetCurrentProcessId()) + ".txt");
        _putenv_s("APS5_ARCHB_GATE_RUN", "1");
        _putenv_s("APS5_ARCHB_CONTROL_FILE", control.string().c_str());
    }
    struct RemoveControl {
        std::filesystem::path path;
        ~RemoveControl() {
            if (!path.empty()) std::filesystem::remove(path);
        }
    } removeControl{control};
    Switches::Initialize();
    Require(Switches::Enabled(Feature::FixWriteTrackingBookkeeping), "the bookkeeping switch is off");
    Recorder::Boundary(true, 0, Recorder::Clock::now());
    std::map<std::string, double> totals;
    const auto collectCounters = [&] {
        for (const auto& [name, value] : Recorder::Collect(Recorder::Clock::now()).counters) totals[name] += value;
        Require(!totals.contains("fix_wt_bookkeeping_mismatches"), "a fast bookkeeping answer disagreed with the original path");
    };
    const auto requireCounter = [&](const char* name, double least) {
        collectCounters();
        if (totals[name] < least) {
            for (const auto& [key, value] : totals) std::cerr << key << "=" << value << "\n";
        }
        Require(totals[name] >= least, std::string("bookkeeping counter too low: ") + name);
    };
    // A path counter is added in batches of 256 per thread.
    const char* const hits = verify ? "fix_wt_bookkeeping_verified" : "fix_wt_bookkeeping_hits";
    constexpr std::size_t bytes = 4 * Block;
    constexpr std::size_t Page = 4096;
    auto* memory = static_cast<volatile std::uint8_t*>(AllocateWatched(bytes));
    const auto base = reinterpret_cast<std::uint64_t>(const_cast<std::uint8_t*>(memory));
    struct Release {
        void* memory;
        ~Release() {
            GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
            GuestAllocations::GuestAllocationsInvalidate_nid_postfix(reinterpret_cast<std::uint64_t>(memory), bytes);
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
        }
    } release{const_cast<std::uint8_t*>(memory)};
    std::memset(const_cast<std::uint8_t*>(memory), 0, bytes);
    const auto repeat = [&](std::uint64_t address, std::size_t size, std::uint64_t generation) {
        for (int i = 0; i < 300; ++i) {
            const auto again = CollectWrites(address, size);
            Require(again != 0 && UnchangedSince(address, size, again) && UnchangedSince(address, size, generation), "a repeated collect in one epoch lost its range");
        }
    };
    if (mode == L"epoch") {
        BumpCollectEpoch();
        const auto first = CollectWrites(base, bytes);
        Require(first != 0, "the fixture was not collected");
        repeat(base, bytes, first);
        repeat(base + Block, Block, first);
        requireCounter(hits, 512);
        memory[Block + 5] = 7;
        const auto sameEpoch = CollectWrites(base, bytes);
        Require(sameEpoch != 0 && UnchangedSince(base, bytes, first), "a collect in the same epoch did not keep the epoch's answer (the original memo's ordering)");
        BumpCollectEpoch();
        const auto nextEpoch = CollectWrites(base, bytes);
        Require(nextEpoch != 0 && !UnchangedSince(base, bytes, first) && UnchangedSince(base, bytes, nextEpoch), "a CPU write before an epoch boundary was not seen after it");
        memory[3 * Block + 1] = 9;
        BumpCollectEpoch();
        const auto subrange = CollectWrites(base + 3 * Block, Block);
        Require(subrange != 0 && !UnchangedSince(base + 3 * Block, Block, nextEpoch) && UnchangedSince(base, 3 * Block, nextEpoch), "a subrange collect after a boundary missed its write or invented one");
        repeat(base + 3 * Block, Block, subrange);
        memory[2 * Block] = 11;
        const auto uncached = CollectWritesUncached(base, bytes);
        Require(uncached != 0 && !UnchangedSince(base + 2 * Block, Block, subrange), "an uncached collect did not see a write at once");
        const auto hit = CollectWrites(base + 3 * Block, Block);
        Require(hit >= uncached && UnchangedSince(base, bytes, hit), "a hit after an uncached collect returned an old generation");
        AdoptLatestCollectEpoch();
        Require(CollectWrites(base, bytes) != 0, "a collect after adopting the newest epoch failed");
        requireCounter(hits, 768);
    } else if (mode == L"threads") {
        BumpCollectEpoch();
        const auto first = CollectWrites(base, bytes);
        repeat(base, bytes, first);
        memory[Block] = 1;
        std::uint64_t other = 0;
        bool otherSaw = false;
        std::thread([&] {
            BumpCollectEpoch();
            other = CollectWrites(base, bytes);
            otherSaw = !UnchangedSince(base, bytes, first);
        }).join();
        Require(other != 0 && otherSaw, "another thread's first collect of its epoch reused this thread's walk");
        const auto mine = CollectWrites(base, bytes);
        Require(mine >= other && !UnchangedSince(base, bytes, first), "a hit returned a generation older than another thread's stamp");
        std::thread([&] { memory[2 * Block + 3] = 2; }).join();
        BumpCollectEpoch();
        const auto after = CollectWrites(base, bytes);
        Require(after != 0 && !UnchangedSince(base + 2 * Block, Block, mine), "a write by another thread before the boundary was not seen");
        std::array<std::thread, 4> workers;
        for (std::size_t worker = 0; worker < workers.size(); ++worker) {
            workers[worker] = std::thread([&, worker] {
                for (int round = 0; round < 64; ++round) {
                    BumpCollectEpoch();
                    const auto range = base + worker * Block;
                    const auto before = CollectWrites(range, Block);
                    Require(before != 0, "a worker collect failed");
                    for (int i = 0; i < 8; ++i) Require(CollectWrites(range, Block) != 0, "a worker repeat failed");
                    memory[worker * Block + 64 + static_cast<std::size_t>(round)] = static_cast<std::uint8_t>(round + 1);
                    BumpCollectEpoch();
                    const auto seen = CollectWrites(range, Block);
                    Require(seen != 0 && !UnchangedSince(range, Block, before), "a worker's write before its boundary was missed");
                }
            });
        }
        for (auto& worker : workers) worker.join();
        collectCounters();
    } else if (mode == L"remap") {
        BumpCollectEpoch();
        const auto first = CollectWrites(base, Block);
        repeat(base, Block, first);
        Unwatch(base, Block);
        Require(CollectWrites(base, Block) == 0, "an unwatched range kept its cached collect");
        const Runs restored{{base, base + Block}};
        Rewatch(restored);
        const auto rewatched = CollectWrites(base, Block);
        Require(rewatched != 0 && !UnchangedSince(base, Block, first), "a rewatched range reused a collect from before the unwatch");
        repeat(base, Block, rewatched);
        // A direct mapping, its removal and a private commit in its place, all inside one epoch.
        auto* view = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(Block, Block));
        Require(view != nullptr, "cannot reserve the remap fixture");
        const auto viewBase = reinterpret_cast<std::uint64_t>(view);
        const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, Block, nullptr);
        Require(section != nullptr, "cannot create the remap section");
        GuestArena::GuestArenaCommit_nid_postfix(view, Block, PAGE_READWRITE, Block);
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(viewBase, Block);
        const auto privateFirst = CollectWrites(viewBase, Block);
        Require(privateFirst != 0, "the private remap fixture was not collected");
        repeat(viewBase, Block, privateFirst);
        GuestArena::GuestArenaReset_nid_postfix(view, Block);
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(viewBase, Block);
        Require(CollectWrites(viewBase, Block) == 0, "a decommitted range kept its cached collect");
        GuestArena::GuestArenaMapDirect_nid_postfix(view, Block, section, 0, PAGE_READWRITE);
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(viewBase, Block);
        const auto mapped = CollectWrites(viewBase, Block);
        Require(mapped != 0 && !UnchangedSince(viewBase, Block, privateFirst), "a remapped range kept its cached collect");
        repeat(viewBase, Block, mapped);
        GuestArena::GuestArenaReset_nid_postfix(view, Block);
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(viewBase, Block);
        Require(CollectWrites(viewBase, Block) == 0, "an unmapped range kept its cached collect");
        GuestArena::GuestArenaForgetSection_nid_postfix(section);
        CloseHandle(section);
        GuestArena::GuestArenaCommit_nid_postfix(view, Block, PAGE_READWRITE, Block);
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(viewBase, Block);
        const auto recommitted = CollectWrites(viewBase, Block);
        Require(recommitted != 0 && !UnchangedSince(viewBase, Block, mapped), "a recommitted range reused an old collect");
        GuestArena::GuestArenaReset_nid_postfix(view, Block);
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(viewBase, Block);
        GuestArena::GuestArenaRelease_nid_postfix(view, Block);
        CheckFailedCollectMemo();
        collectCounters();
    } else if (mode == L"reset") {
        BumpCollectEpoch();
        const auto first = CollectWrites(base, bytes);
        repeat(base, bytes, first);
        const auto stamped = MarkWritten(base + 8, 4);
        Require(stamped > first, "a driver store was not stamped");
        const auto afterStamp = CollectWrites(base, bytes);
        Require(afterStamp >= stamped && UnchangedSince(base, bytes, afterStamp) && !UnchangedSince(base, bytes, first), "a hit after a driver store returned a generation older than the store");
        const std::array<std::byte, 16> source{};
        const auto own = StoreOwnBytes(base + Block + 32, source);
        Require(own != 0, "an own store was not stamped");
        const auto afterOwn = CollectWrites(base, bytes);
        Require(afterOwn >= own && UnchangedSince(base, bytes, afterOwn), "a hit after an own store returned an old generation");
        Require(StoredOver(base + Block, 64, afterStamp), "an own store was not reported as stored over");
        collectCounters();
    } else if (mode == L"runs") {
        // Committed ranges come from the page-state runs: odd starts (the byte-wise head), whole
        // words, the four-word steps and the tail, over committed and reserved blocks.
        PartlyCommitted range;
        range.Commit(0);
        range.Commit(2);
        const auto partly = range.Address();
        for (int pass = 0; pass < 4; ++pass)
        for (std::size_t offset = 0; offset < PartlyCommitted::Bytes; offset += 3 * Page) {
            for (const std::size_t length : {Page, 7 * Page, 9 * Page, Block, 33 * Page + 5, PartlyCommitted::Bytes}) {
                if (offset + length > PartlyCommitted::Bytes) continue;
                const auto runs = CommittedRanges(partly + offset, length);
                std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
                for (const std::size_t block : {std::size_t{0}, std::size_t{2}}) {
                    const auto begin = std::max<std::uint64_t>(partly + offset, partly + block * Block);
                    const auto end = std::min<std::uint64_t>(partly + offset + length, partly + (block + 1) * Block);
                    if (begin < end) expected.emplace_back(begin, end);
                }
                Require(runs == expected, "committed ranges changed under the bookkeeping switch");
            }
        }
        // A run that crosses a page-state chunk (65536 pages) boundary, where one can be reserved.
        std::uintptr_t arena = 0;
        std::size_t arenaBytes = 0;
        GuestArena::GuestArenaRange_nid_postfix(&arena, &arenaBytes);
        constexpr std::uint64_t chunk = std::uint64_t{65536} * Page;
        bool crossed = false;
        for (std::uint64_t boundary = arena + chunk * 8; !crossed && boundary + Block < arena + arenaBytes; boundary += chunk * 7) {
            void* placed = GuestArena::GuestArenaAllocateAtOrAbove_nid_postfix(boundary - Block, 2 * Block, Block);
            if (placed == nullptr) continue;
            if (reinterpret_cast<std::uint64_t>(placed) != boundary - Block) {
                GuestArena::GuestArenaRelease_nid_postfix(placed, 2 * Block);
                continue;
            }
            GuestArena::GuestArenaCommit_nid_postfix(placed, 2 * Block, PAGE_READWRITE, 2 * Block);
            GuestAllocations::GuestAllocationsInvalidate_nid_postfix(boundary - Block, 2 * Block);
            for (std::uint64_t start = boundary - Block; start < boundary; start += 5 * Page) {
                const auto runs = CommittedRanges(start, static_cast<std::size_t>(boundary + Block - start));
                Require(runs.size() == 1 && runs[0].first == start && runs[0].second == boundary + Block, "a committed run across a page-state chunk boundary changed");
            }
            GuestArena::GuestArenaReset_nid_postfix(placed, 2 * Block);
            GuestAllocations::GuestAllocationsInvalidate_nid_postfix(boundary - Block, 2 * Block);
            GuestArena::GuestArenaRelease_nid_postfix(placed, 2 * Block);
            crossed = true;
        }
        if (!crossed) std::cout << "no page-state chunk boundary could be reserved: the crossing case was skipped\n";
        requireCounter(verify ? "fix_wt_bookkeeping_verified" : "fix_wt_bookkeeping_runs", 256);
    } else if (mode == L"regions") {
        std::uintptr_t arena = 0;
        std::size_t arenaBytes = 0;
        GuestArena::GuestArenaRange_nid_postfix(&arena, &arenaBytes);
        std::uint64_t state = 0x9e3779b97f4a7c15ull;
        std::size_t overlapping = 0;
        for (int i = 0; i < 20000; ++i) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            const auto address = i < 10000 ? arena + (arenaBytes / 10000) * static_cast<std::uint64_t>(i) : arena + state % arenaBytes;
            const std::size_t length = std::size_t{1} << (state >> 58) % 34;
            const bool scanned = GuestArena::GuestArenaHostRegionOverlaps_nid_postfix(address, length);
            Require(GuestArena::GuestArenaHostRegionOverlapsSorted_nid_no_patch(address, length) == scanned, "the sorted host-region test disagreed with the scan");
            overlapping += scanned ? 1 : 0;
        }
        // The exact edges: every region boundary of the arena (the host holes are regions of their
        // own), probed around with several lengths.
        std::size_t edges = 0;
        for (std::uint64_t at = arena; at < arena + arenaBytes;) {
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)) != sizeof(region) || region.RegionSize == 0) break;
            const auto edge = at;
            at = reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize;
            if (!GuestArena::GuestArenaHostRegionOverlaps_nid_postfix(edge, Page) && !GuestArena::GuestArenaHostRegionOverlaps_nid_postfix(at - Page, Page)) continue;
            ++edges;
            for (const auto point : {edge, at}) for (std::uint64_t probe = point - 3 * Page; probe <= point + 3 * Page; probe += Page / 2) {
                for (const std::size_t length : {std::size_t{1}, Page, 2 * Page, std::size_t{1} << 20, std::size_t{1} << 31}) {
                    const bool scanned = GuestArena::GuestArenaHostRegionOverlaps_nid_postfix(probe, length);
                    Require(GuestArena::GuestArenaHostRegionOverlapsSorted_nid_no_patch(probe, length) == scanned, "the sorted host-region test disagreed at a region edge");
                    overlapping += scanned ? 1 : 0;
                }
            }
        }
        std::cout << "host-region samples overlapping: " << overlapping << ", region edges probed: " << edges << "\n";
        for (int i = 0; i < 300; ++i) Require(Watched(base, bytes), "the fixture is not watched");
        requireCounter(verify ? "fix_wt_bookkeeping_verified" : "fix_wt_bookkeeping_regions", 256);
    } else if (mode == L"toggle") {
        const auto write = [&](const std::string& line) {
            std::ofstream(control, std::ios::binary | std::ios::trunc) << line << '\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            Switches::PollControl();
        };
        BumpCollectEpoch();
        const auto first = CollectWrites(base, bytes);
        repeat(base, bytes, first);
        write("1 FIX_WRITE_TRACKING_BOOKKEEPING=0");
        Require(!Switches::Enabled(Feature::FixWriteTrackingBookkeeping), "the bookkeeping switch did not turn off");
        memory[5] = 3;
        const auto off = CollectWrites(base, bytes);
        Require(off != 0 && UnchangedSince(base, bytes, first), "the original memo did not keep the epoch's answer");
        write("2 FIX_WRITE_TRACKING_BOOKKEEPING=1");
        Require(Switches::Enabled(Feature::FixWriteTrackingBookkeeping), "the bookkeeping switch did not turn back on");
        const auto on = CollectWrites(base, bytes);
        Require(on != 0 && UnchangedSince(base, bytes, first), "the table answered differently from the memo in the same epoch");
        BumpCollectEpoch();
        const auto next = CollectWrites(base, bytes);
        Require(next != 0 && !UnchangedSince(base, bytes, first), "a write made while the switch was off was missed after the boundary");
        repeat(base, bytes, next);
        collectCounters();
    } else if (mode == L"bench") {
        // Not a ctest: nanoseconds per operation with the switch off and on (control-file toggles),
        // for the replay's per-frame counts of table hits, page-state runs and host-region tests.
        const auto write = [&](const std::string& line) {
            std::ofstream(control, std::ios::binary | std::ios::trunc) << line << '\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            Switches::PollControl();
        };
        constexpr std::size_t ranges = 64;
        auto* wide = static_cast<std::uint8_t*>(AllocateWatched(ranges * Block));
        const auto wideBase = reinterpret_cast<std::uint64_t>(wide);
        std::memset(wide, 0, ranges * Block);
        const auto time = [](auto&& body, int count) {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < count; ++i) body(i);
            return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / count;
        };
        std::uint64_t sink = 0;
        const auto switchRead = time([&](int) { sink += Switches::Enabled(Feature::FixWriteTrackingBookkeeping) ? 1 : 0; }, 2000000);
        std::cout << "bench switch read " << switchRead << " ns\n";
        for (int round = 0; round < 3; ++round) {
            for (const bool on : {false, true}) {
                write(std::to_string(round * 2 + (on ? 2 : 1)) + " FIX_WRITE_TRACKING_BOOKKEEPING=" + (on ? "1" : "0"));
                BumpCollectEpoch();
                for (std::size_t i = 0; i < ranges; ++i) sink += CollectWrites(wideBase + i * Block, Block);
                const auto exact = time([&](int i) { sink += CollectWrites(wideBase + (static_cast<std::size_t>(i) % ranges) * Block, Block); }, 400000);
                const auto inner = time([&](int i) { sink += CollectWrites(wideBase + (static_cast<std::size_t>(i) % ranges) * Block + 4096, 8192); }, 400000);
                const auto committed = time([&](int i) { sink += CommittedRanges(wideBase + static_cast<std::uint64_t>(i % 7) * 4096, ranges * Block - 7 * 4096).size(); }, 20000);
                const auto watched = time([&](int i) { sink += Watched(wideBase + (static_cast<std::size_t>(i) % ranges) * Block, Block) ? 1 : 0; }, 400000);
                std::cout << "bench " << (on ? "on " : "off") << ": collect hit (exact) " << exact << " ns, collect hit (subrange) " << inner << " ns, committed ranges 4 MiB " << committed << " ns, watched " << watched << " ns\n";
            }
        }
        std::cout << "sink " << sink << "\n";
        GuestArena::GuestArenaReset_nid_postfix(wide, ranges * Block);
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(wideBase, ranges * Block);
        GuestArena::GuestArenaRelease_nid_postfix(wide, ranges * Block);
    } else throw std::runtime_error("unknown bookkeeping test");
}
#endif
// The tracker generation, its block, coarse, CPU-page and driver-piece stamps must not wrap at
// 2^32: a store made after the crossing must still read as newer than a stamp taken before it
// (with 32-bit stamps it read as older, so changed memory was reported unchanged).
void CheckGenerationWrap() {
    constexpr std::uint64_t Wrap = std::uint64_t{1} << 32u;
    constexpr std::size_t bytes = 192 * Block;
    void* memory = AllocateWatched(bytes);
    void* scratch = AllocateWatched(Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto other = reinterpret_cast<std::uint64_t>(scratch);
    std::memset(memory, 0x11, bytes);
    std::memset(scratch, 0x11, Block);
    BumpCollectEpoch();
    Require(CollectWrites(base, bytes) != 0 && CollectWrites(other, Block) != 0, "the wrap fixture was not collected");
    AdvanceTrackerGenerationForTest(Wrap - 4);
    BumpCollectEpoch();
    const auto before = CollectWritesUncached(base, bytes);
    Require(before >= Wrap - 4 && before < Wrap, "the stamp before the crossing was not taken just below 2^32");
    Require(UnchangedSince(base, bytes, before), "a range nothing wrote after its collect reports a change");
    for (int store = 0; store < 8; ++store) MarkWritten(other, 4);
    Require(TrackerGeneration() > Wrap, "the tracker generation did not cross 2^32");
    const auto middle = base + bytes / 2;
    const auto stored = MarkWritten(middle, 4);
    Require(stored > Wrap && stored > before, "a driver store after 2^32 returned a stamp not newer than one taken before");
    Require(!UnchangedSince(base, bytes, before), "a driver store after 2^32 was reported unchanged over whole coarse runs");
    Require(!UnchangedSince(middle, Block, before), "a driver store after 2^32 was reported unchanged in its block");
    Require(WrittenSince(base, bytes, before) && StoredOver(middle, 4, before), "a driver store after 2^32 was not seen as written since the earlier stamp");
    Require(NewestStamp(middle, Block) > before, "the newest stamp after 2^32 is not newer than the earlier stamp");
    Require(!StoredAllSince_nid_no_patch(middle, 4, stored) && StoredAllSince_nid_no_patch(middle, 4, before), "the driver piece of a store after 2^32 lost its generation");
    Require(UnchangedSince(base, bytes / 2, before) && UnchangedSince(base, bytes, stored), "a store after 2^32 changed other blocks or a later stamp");
    static_cast<volatile std::uint8_t*>(memory)[bytes - 1] = 0x22;
    const auto last = base + bytes - Block;
    Require(CollectWritesUncached(last, Block) > Wrap, "a collect after 2^32 returned a wrapped generation");
    Require(!UnchangedSince(last, Block, before) && !UnchangedSinceCollected(last, Block, before), "a CPU store after 2^32 was reported unchanged");
    std::array<std::uint64_t, 3> generations{before, before, before};
    std::array<std::uint8_t, 3> changed{};
    std::array<std::uint8_t, 3> cpu{};
    Require(ChangedBlocks(base + bytes - 3 * Block, 3 * Block, generations, changed, cpu), "the changed-block query over the wrap fixture failed");
    Require(changed[0] == BlockUnchanged && changed[2] == BlockWritten && cpu[0] == 0 && cpu[2] == 1, "per-block changes after 2^32 were misreported");
}

// Write generations, slice S1 (APS5_ARCHB_GEN_TABLE=1 APS5_ARCHB_GEN_SHADOW=1 APS5_ARCHB_GEN_VERIFY=1):
// the shadow over real tracker answers. Agreeing answers, the sweep model at tickets, an excluded
// range, the reset floor when the table is turned on again; no answer may be unsafe.
std::uint64_t genCounter(const std::string& name) {
    return Gen::CounterForTest(name.c_str());
}

std::uint64_t genUnsafeTotal() {
    std::uint64_t total = 0;
    for (std::size_t c = 0; c < static_cast<std::size_t>(Gen::Consumer::Count); ++c) {
        const std::string consumer = Gen::ConsumerName(static_cast<Gen::Consumer>(c));
        total += genCounter("gen_shadow." + consumer + ".unsafe") + genCounter("gen_verify." + consumer + ".unsafe");
    }
    return total;
}

void CheckGenShadow() {
    Require(Gen::ShadowActive(), "the shadow test needs APS5_ARCHB_GEN_TABLE=1 and APS5_ARCHB_GEN_SHADOW=1");
    constexpr std::size_t bytes = 8 * Block;
    void* memory = AllocateWatched(bytes);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x5a, bytes);
    BumpCollectEpoch(Gen::TicketReason::Submission);
    const auto ticket = Gen::Required();
    Require(ticket != 0 && genCounter("gen_tickets.submission") >= 1, "an ordering point issued no ticket");
    const auto calls = genCounter("gen_sweep_model.calls");
    const auto sweeps = genCounter("gen_sweep_model.sweeps");
    const auto since = CollectWrites(base, bytes);
    Require(since >= Gen::Global().ResetFloor() && since <= Gen::Global().Now(), "the tracker's generation is not a GenClock value above the floor");
    Require(genCounter("gen_sweep_model.calls") == calls + 1 && genCounter("gen_sweep_model.sweeps") == sweeps + 1, "the first collect after a ticket was not modeled as a sweep");
    Require(Gen::Global().Covered(base, base + bytes, ticket), "a completed walk did not cover its range at the ticket read before it");
    const auto covered = genCounter("gen_sweep_model.covered");
    static_cast<void>(CollectWrites(base + Block, Block));
    Require(genCounter("gen_sweep_model.covered") == covered + 1, "a collect inside a covering sweep was not modeled covered");
    {
        const Gen::ConsumerScope scope(Gen::Consumer::ResourceProof);
        const auto agree = genCounter("gen_shadow.resource_proof.agree_clean");
        Require(UnchangedSince(base, bytes, since), "an unwritten range reads changed");
        Require(genCounter("gen_shadow.resource_proof.agree_clean") == agree + 1, "an agreeing clean answer was not counted");
        {
            const Gen::ConsumerScope inner(Gen::Consumer::TextureLookup);
            Require(Gen::CurrentConsumer() == Gen::Consumer::ResourceProof, "an inner consumer scope took over the outer decision");
        }
        static_cast<volatile std::uint8_t*>(memory)[3 * Block + 5] = 0x11;
        BumpCollectEpoch(Gen::TicketReason::WaitCpu);
        Require(!Gen::Global().Covered(base, base + bytes, Gen::Required()), "a sweep made before a new ticket still covers it");
        static_cast<void>(CollectWrites(base, bytes));
        const auto changed = genCounter("gen_shadow.resource_proof.agree_changed");
        Require(!UnchangedSince(base, bytes, since), "the tracker missed a CPU store");
        Require(genCounter("gen_shadow.resource_proof.agree_changed") == changed + 1, "an agreeing changed answer was not counted");
        const std::array<UnchangedQuery, 2> queries{UnchangedQuery{base, Block, since}, UnchangedQuery{base + 3 * Block, Block, since}};
        Require(!UnchangedSinceAll(queries), "a query set over a written block reads unchanged");
        std::array<std::uint64_t, 4> generations{since, since, since, since};
        std::array<std::uint8_t, 4> blocks{};
        Require(ChangedBlocks(base, 4 * Block, generations, blocks) && blocks[3] == BlockWritten && blocks[0] == BlockUnchanged, "per-block changes misreported");
        Require(genCounter("gen_shadow.resource_proof.agree_changed") >= changed + 3, "the query set and the written block were not classified as changed");
    }
    {
        const auto since2 = CollectWrites(base, bytes);
        const Gen::ConsumerScope scope(Gen::Consumer::Snapshot);
        const auto excluded = genCounter("gen_shadow.snapshot.unknown_excluded");
        Unwatch(base + 6 * Block, 2 * Block);
        Require(!UnchangedSince(base + 6 * Block, Block, since2), "an unwatched range reads unchanged");
        Require(genCounter("gen_shadow.snapshot.unknown_excluded") == excluded + 1, "an excluded range was not unknown_excluded");
        Require(Gen::Global().Fields(base + 6 * Block).state == Gen::BlockState::Excluded, "an unwatched block is not Excluded in the table");
    }
    {
        const Gen::ConsumerScope scope(Gen::Consumer::Mirror);
        const auto since3 = CollectWrites(base, 4 * Block);
        Gen::ResetActivationForTest();
        Require(Gen::Active(), "the table did not activate again");
        Require(Gen::Global().ResetFloor() > since3, "turning the table on again did not raise the floor above the handed-out generations");
        const auto reset = genCounter("gen_shadow.mirror.unknown_reset");
        const auto conservative = genCounter("gen_shadow.mirror.conservative");
        Require(UnchangedSince(base, 4 * Block, since3), "the tracker's own answer changed with the floor");
        Require(genCounter("gen_shadow.mirror.unknown_reset") == reset + 1 && genCounter("gen_shadow.mirror.conservative") == conservative + 1, "a since before the floor was not unknown_reset and conservative");
        Require(CollectWritesUncached(base, 4 * Block) > Gen::Global().ResetFloor(), "a generation handed out after the floor is not above it");
        Require(genCounter("gen_tickets.uncached") >= 1, "an uncached collect issued no private ticket");
    }
    Require(genCounter("gen_events.cpu_sweep.count") >= 1 && genCounter("gen_events.unwatch.count") >= 1 && genCounter("gen_verify.resource_proof.agree_clean") >= 1, "events or verification were not counted");
    Require(genUnsafeTotal() == 0, "the shadow reported an unsafe disagreement");
}

// An event the table misses (a driver store with its table event suppressed through the test hook)
// leaves the tracker's answer 'changed' and the table's 'clean': the shadow counts it unsafe and the
// verifier (APS5_ARCHB_GEN_VERIFY=1) finds the missed stamp.
void CheckGenVerifyCatches() {
    Require(Gen::ShadowActive() && Gen::VerifyPeriod() == 1, "the verify test needs GEN_TABLE, GEN_SHADOW and GEN_VERIFY=1");
    constexpr std::size_t bytes = 4 * Block;
    void* memory = AllocateWatched(bytes);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x33, bytes);
    BumpCollectEpoch(Gen::TicketReason::Submission);
    const auto since = CollectWrites(base, bytes);
    Gen::SuppressEventForTest(Gen::Event::DriverStore, true);
    static_cast<void>(MarkWritten(base + Block + 64, 16));
    Gen::SuppressEventForTest(Gen::Event::DriverStore, false);
    const Gen::ConsumerScope scope(Gen::Consumer::TextureLookup);
    const auto unsafe = genCounter("gen_shadow.texture_lookup.unsafe");
    const auto verified = genCounter("gen_verify.texture_lookup.unsafe");
    Require(!UnchangedSince(base, bytes, since), "the tracker missed the driver store");
    Require(genCounter("gen_shadow.texture_lookup.unsafe") == unsafe + 1, "the shadow did not count the missed event unsafe");
    Require(genCounter("gen_verify.texture_lookup.unsafe") == verified + 1, "the verifier did not catch the missed tracker stamp");
    // With the event published, the same question agrees again.
    const auto stored = MarkWritten(base + Block + 64, 16);
    Require(Gen::Global().Fields(base + Block).data == stored, "the table did not take the driver store's generation");
    Require(!UnchangedSince(base, bytes, since) && genCounter("gen_shadow.texture_lookup.unsafe") == unsafe + 1, "a published driver store was counted unsafe");
}

// Write generations, slice S2 (APS5_ARCHB_GEN_TABLE=1 APS5_ARCHB_GEN_LOCKFREE=1 APS5_ARCHB_GEN_VERIFY=1):
// the tracker's query APIs answered from the table without the tracker mutex. Clean ranges are
// answered fast with the tracker's answer, written ranges go to the locked path, ranges outside
// write watching are refused fast, a refusal in the table forces the locked path, TrackerGeneration
// is the published generation, and every sampled fast answer agrees with the locked one.
std::uint64_t lockFreeFast(const char* api) {
    return genCounter(std::string("gen_lockfree.") + api + ".fast");
}

std::uint64_t lockFreeSlow(const char* api) {
    return genCounter(std::string("gen_lockfree.") + api + ".slow");
}

void CheckGenLockFree() {
    Require(Gen::LockFreeActive() && Gen::VerifyPeriod() == 1, "the lock-free test needs GEN_TABLE, GEN_LOCKFREE and GEN_VERIFY=1");
    constexpr std::size_t bytes = 8 * Block;
    auto* memory = static_cast<std::uint8_t*>(AllocateWatched(bytes));
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x5a, bytes);
    BumpCollectEpoch(Gen::TicketReason::Submission);
    const auto since = CollectWrites(base, bytes);
    Require(since != 0 && TrackerGeneration() == since, "the lock-free tracker generation is not the published generation after a collect");

    // Clean: answered from the table, the tracker's answer.
    auto fast = lockFreeFast("unchanged_since");
    Require(UnchangedSince(base, bytes, since) && lockFreeFast("unchanged_since") == fast + 1, "a clean range was not answered unchanged from the table");
    fast = lockFreeFast("unchanged_since_all");
    const std::array<UnchangedQuery, 2> clean{UnchangedQuery{base, Block, since}, UnchangedQuery{base + 4 * Block, 2 * Block, since}};
    Require(UnchangedSinceAll(clean) && lockFreeFast("unchanged_since_all") == fast + 1, "clean queries were not answered from the table");
    const std::array<UnchangedQuery, 2> zero{UnchangedQuery{base, Block, since}, UnchangedQuery{base + Block, Block, 0}};
    Require(!UnchangedSinceAll(zero) && lockFreeFast("unchanged_since_all") == fast + 2, "a query with generation 0 was not refused from the table");
    fast = lockFreeFast("written_since");
    Require(!WrittenSince(base, bytes, since) && lockFreeFast("written_since") == fast + 1, "a clean range was not answered unwritten from the table");
    fast = lockFreeFast("stored_over");
    Require(!StoredOver(base, bytes, since) && lockFreeFast("stored_over") == fast + 1, "a clean range was not answered not-stored-over from the table");
    fast = lockFreeFast("unchanged_committed_since");
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 2> committed{{{base, base + Block}, {base + 2 * Block, base + 3 * Block}}};
    Require(UnchangedCommittedSince(committed, since) && lockFreeFast("unchanged_committed_since") == fast + 1, "clean committed ranges were not answered from the table");
    {
        std::array<std::uint64_t, 4> generations{since, since, 0, since};
        std::array<std::uint8_t, 4> changed{};
        std::array<std::uint8_t, 4> cpu{};
        fast = lockFreeFast("changed_blocks");
        Require(ChangedBlocks(base, 4 * Block, generations, changed, cpu) && lockFreeFast("changed_blocks") == fast + 1, "clean blocks were not answered from the table");
        Require(changed[0] == BlockUnchanged && changed[1] == BlockUnchanged && changed[2] == BlockWritten && changed[3] == BlockUnchanged && cpu[0] == 0 && cpu[2] == 1, "per-block answers from the table are wrong");
    }

    // A CPU store, collected: the table data moved past `since`, so the locked path answers.
    memory[3 * Block + 7] = 0x11;
    BumpCollectEpoch(Gen::TicketReason::WaitCpu);
    const auto after = CollectWrites(base, bytes);
    auto slow = lockFreeSlow("unchanged_since");
    Require(!UnchangedSince(base, bytes, since) && lockFreeSlow("unchanged_since") == slow + 1, "a written range was not answered by the locked path");
    fast = lockFreeFast("unchanged_since");
    Require(UnchangedSince(base, 3 * Block, since) && lockFreeFast("unchanged_since") == fast + 1, "the unwritten blocks beside a store were not answered from the table");
    Require(UnchangedSince(base, bytes, after), "a range read as changed since the generation of the collect that saw its store");
    slow = lockFreeSlow("written_since");
    Require(WrittenSince(base, bytes, since) && lockFreeSlow("written_since") == slow + 1, "a written range was not answered written by the locked path");
    {
        std::array<std::uint64_t, 4> generations{since, since, since, since};
        std::array<std::uint8_t, 4> changed{};
        std::array<std::uint8_t, 4> cpu{};
        slow = lockFreeSlow("changed_blocks");
        Require(ChangedBlocks(base, 4 * Block, generations, changed, cpu) && lockFreeSlow("changed_blocks") == slow + 1, "a written block did not send the call to the locked path");
        Require(changed[3] == BlockWritten && cpu[3] == 1 && changed[0] == BlockUnchanged && cpu[0] == 0, "the locked path after an inconclusive table answer is wrong");
    }
    // A driver store over part of a block: the table says changed (block granularity), the locked
    // path's sub-block detail says the untouched part is unchanged.
    const auto stored = MarkWritten(base + 5 * Block + 64, 64);
    Require(stored > after && TrackerGeneration() == stored, "MarkWritten did not publish its generation");
    slow = lockFreeSlow("unchanged_since");
    Require(UnchangedSince(base + 5 * Block + 4096, 4096, after) && lockFreeSlow("unchanged_since") == slow + 1, "an untouched page of a stored block was not answered unchanged by the locked path");
    Require(!UnchangedSince(base + 5 * Block, 4096, after), "the stored page reads unchanged");
    Require(StoredOver(base + 5 * Block, 4096, after) && !StoredOver(base + 6 * Block, Block, after), "stored-over answers after a driver store are wrong");

    // A refusal in the table (as a compared-when-shared walk records it) forces the locked path.
    const auto refusedAt = CollectWrites(base, bytes);
    Gen::Global().Refuse(base + 7 * Block, base + 8 * Block, Gen::Global().Bump());
    slow = lockFreeSlow("unchanged_since");
    Require(UnchangedSince(base + 7 * Block, Block, refusedAt) && lockFreeSlow("unchanged_since") == slow + 1, "a block refused in the table was answered from the table");

    // Outside write watching: refused from the table without a stamp, as the locked path does.
    Unwatch(base + 6 * Block, Block);
    fast = lockFreeFast("unchanged_since");
    Require(!UnchangedSince(base + 6 * Block, Block, refusedAt) && lockFreeFast("unchanged_since") == fast + 1, "an unwatched range was not refused from the table");
    fast = lockFreeFast("stored_over");
    Require(StoredOver(base + 6 * Block, Block, refusedAt) && lockFreeFast("stored_over") == fast + 1, "an unwatched range was not stored-over from the table");
    Rewatch(std::array<std::pair<std::uint64_t, std::uint64_t>, 1>{{{base + 6 * Block, base + 7 * Block}}});
    Require(!UnchangedSince(base + 6 * Block, Block, refusedAt), "a rewatched range reads unchanged since a generation before its exclusion");

    // NewestStamp stays locked (counted), its value unchanged.
    slow = lockFreeSlow("newest_stamp");
    Require(NewestStamp(base + 5 * Block, Block) == stored && lockFreeSlow("newest_stamp") == slow + 1, "NewestStamp changed under GEN_LOCKFREE");
    Require(genCounter("gen_lockfree_unsafe") == 0 && genCounter("gen_lockfree_raced") == 0 && genCounter("gen_lockfree_verified") > 0, "verify found a lock-free answer that differs from the locked one");
}

// A table that misses a tracker stamp (the driver-store event suppressed through the test hook):
// the fast path answers unchanged, and verify mode (GEN_VERIFY=1) catches it under the mutex.
void CheckGenLockFreeVerifyCatches() {
    Require(Gen::LockFreeActive() && Gen::VerifyPeriod() == 1, "the verify test needs GEN_TABLE, GEN_LOCKFREE and GEN_VERIFY=1");
    constexpr std::size_t bytes = 4 * Block;
    auto* memory = static_cast<std::uint8_t*>(AllocateWatched(bytes));
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x33, bytes);
    BumpCollectEpoch(Gen::TicketReason::Submission);
    const auto since = CollectWrites(base, bytes);
    Gen::SuppressEventForTest(Gen::Event::DriverStore, true);
    static_cast<void>(MarkWritten(base + Block + 64, 16));
    Gen::SuppressEventForTest(Gen::Event::DriverStore, false);
    const auto unsafe = genCounter("gen_lockfree_unsafe");
    static_cast<void>(UnchangedSince(base + Block, Block, since));
    Require(genCounter("gen_lockfree_unsafe") == unsafe + 1, "verify did not catch a fast answer the tracker contradicts");
}

// Concurrent stamping. Writers stamp blocks (driver stores and CPU stores with collects) while
// checkers take a generation, wait for a store they ordered after it, and require the range to read
// changed; background readers keep the lock-free paths busy. Verify mode cross-checks every fast
// answer under the mutex: none may be unsafe.
void CheckGenLockFreeStress() {
    Require(Gen::LockFreeActive() && Gen::VerifyPeriod() != 0, "the stress test needs GEN_TABLE, GEN_LOCKFREE and GEN_VERIFY");
    constexpr std::size_t blocks = 64;
    constexpr std::size_t bytes = blocks * Block;
    auto* memory = static_cast<std::uint8_t*>(AllocateWatched(bytes));
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0, bytes);
    BumpCollectEpoch(Gen::TicketReason::Submission);
    static_cast<void>(CollectWrites(base, bytes));
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> missed{0};
    std::atomic<std::uint64_t> checks{0};
    std::vector<std::thread> threads;
    // Noise: driver stores and CPU stores with collects over the upper half.
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937_64 random(static_cast<std::uint64_t>(t) + 11u);
            while (!stop.load(std::memory_order_relaxed)) {
                const auto block = blocks / 2 + random() % (blocks / 2);
                const auto address = base + block * Block + (random() % 16) * 4096;
                if ((random() & 1u) == 0) {
                    static_cast<void>(MarkWritten(address, 64));
                } else {
                    memory[address - base] = static_cast<std::uint8_t>(random());
                    BumpCollectEpoch(Gen::TicketReason::Other);
                    static_cast<void>(CollectWrites(base + block * Block, Block));
                }
            }
        });
    }
    // Readers: every lock-free API over random ranges at random recent generations.
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937_64 random(static_cast<std::uint64_t>(t) + 101u);
            std::array<std::uint64_t, 4> generations{};
            std::array<std::uint8_t, 4> changed{};
            std::array<std::uint8_t, 4> cpu{};
            while (!stop.load(std::memory_order_relaxed)) {
                const auto block = random() % (blocks - 4);
                const auto since = TrackerGeneration() - random() % 4;
                const auto address = base + block * Block;
                static_cast<void>(UnchangedSince(address, 4 * Block, since));
                static_cast<void>(WrittenSince(address, 2 * Block, since));
                static_cast<void>(StoredOver(address, Block, since));
                generations.fill(since);
                static_cast<void>(ChangedBlocks(address, 4 * Block, generations, changed, cpu));
                const std::array<UnchangedQuery, 2> queries{UnchangedQuery{address, Block, since}, UnchangedQuery{address + 2 * Block, Block, since}};
                static_cast<void>(UnchangedSinceAll(queries));
            }
        });
    }
    // Checkers own the lower half (block per checker): generation, then an ordered store, then the check.
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937_64 random(static_cast<std::uint64_t>(t) + 1001u);
            std::atomic<int> phase{0};
            const auto block = static_cast<std::size_t>(t) * 4;
            const auto address = base + block * Block;
            while (!stop.load(std::memory_order_relaxed)) {
                BumpCollectEpoch(Gen::TicketReason::Other);
                const auto since = (random() & 1u) != 0 ? TrackerGeneration() : CollectWrites(address, 4 * Block);
                const bool cpuStore = (random() & 1u) != 0;
                const auto offset = (random() % 4) * Block + (random() % 16) * 4096;
                // The store on another thread, ordered after the generation by the handshake.
                std::thread writer([&] {
                    while (phase.load(std::memory_order_acquire) != 1) std::this_thread::yield();
                    if (cpuStore) memory[block * Block + offset] = static_cast<std::uint8_t>(memory[block * Block + offset] + 1);
                    else static_cast<void>(MarkWritten(address + offset, 32));
                    phase.store(2, std::memory_order_release);
                });
                phase.store(1, std::memory_order_release);
                while (phase.load(std::memory_order_acquire) != 2) std::this_thread::yield();
                writer.join();
                phase.store(0, std::memory_order_relaxed);
                // An ordering point after the store (a CPU store is seen by the next epoch's collect).
                BumpCollectEpoch(Gen::TicketReason::WaitCpu);
                static_cast<void>(CollectWrites(address, 4 * Block));
                if (UnchangedSince(address, 4 * Block, since) || UnchangedSince(address + offset, 32, since) || !WrittenSince(address, 4 * Block, since)) missed.fetch_add(1, std::memory_order_relaxed);
                checks.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(4));
    stop.store(true);
    for (auto& thread : threads) thread.join();
    std::cout << "lock-free stress: checks=" << checks.load() << " fast=" << genCounter("gen_lockfree_fast") << " slow=" << genCounter("gen_lockfree_slow") << " verified=" << genCounter("gen_lockfree_verified")
              << " raced=" << genCounter("gen_lockfree_raced") << " unsafe=" << genCounter("gen_lockfree_unsafe") << '\n';
    Require(checks.load() > 100, "the stress test made too few checks");
    Require(missed.load() == 0, "a store ordered after a generation was missed");
    Require(genCounter("gen_lockfree_unsafe") == 0, "verify found an unsafe lock-free answer under concurrent stamping");
    Require(genCounter("gen_lockfree_fast") > 0 && genCounter("gen_lockfree_verified") > 0, "the stress test exercised no fast answer");
}

int main(int argc, char** argv) {
    try {
#ifdef _WIN32
        if (std::wstring_view(GetCommandLineW()).ends_with(L"--metadata-commit")) {
            return CheckMetadataCommit() ? 0 : 77;
        }
#endif
#ifdef _WIN32
        const auto command = std::wstring_view(GetCommandLineW());
        const auto memo = command.rfind(L"--vq-memo-");
        if (memo != std::wstring_view::npos) {
            Require(WriteWatched(), "query memo test needs write watching");
            CheckVqMemo(command.substr(memo + 10));
            return 0;
        }
        const auto bookkeeping = command.rfind(L"--wt-bookkeeping-");
        if (bookkeeping != std::wstring_view::npos) {
            Require(WriteWatched(), "the bookkeeping test needs write watching");
            CheckWriteTrackingBookkeeping(command.substr(bookkeeping + 17));
            return 0;
        }
#endif
        if (!WriteWatched()) {
            std::cout << "no write watching: skipped\n";
            return 77;
        }
        // The draw-sampled switch, for the whole run on this thread (the suite with it on and off).
        const char* allocChurn = std::getenv("APS5_ARCHB_FIX_ALLOC_CHURN");
        const AgcDriver::ArchB::AllocChurnScope allocChurnScope(allocChurn != nullptr && std::string_view(allocChurn) == "1");
        if (argc == 2 && std::string_view(argv[1]) == "--failed-collect-memo") {
            CheckFailedCollectMemo();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--alloc-churn-memo") {
            CheckAllocChurnMemo();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--gen-shadow") {
            CheckGenShadow();
            std::cout << "write-generation shadow test passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--gen-verify-catches") {
            CheckGenVerifyCatches();
            std::cout << "write-generation verify test passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--gen-lockfree") {
            CheckGenLockFree();
            std::cout << "write-generation lock-free test passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--gen-lockfree-verify-catches") {
            CheckGenLockFreeVerifyCatches();
            std::cout << "write-generation lock-free verify test passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--gen-lockfree-stress") {
            CheckGenLockFreeStress();
            std::cout << "write-generation lock-free stress test passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--gen-tracker-wrap") {
            CheckGenerationWrap();
            std::cout << "generation wrap test passed\n";
            return 0;
        }
#ifdef _WIN32
        if (std::wstring_view(GetCommandLineW()).ends_with(L"--target-clean-proof")) {
            CheckTargetCleanProof();
            return 0;
        }
        if (std::wstring_view(GetCommandLineW()).ends_with(HostHoleMode)) {
            CheckHostHole();
            return 0;
        }
        if (std::wstring_view(GetCommandLineW()).ends_with(ResidentRemapMode)) {
            CheckResidentDirectMappingReuse();
            return 0;
        }
        if (std::wstring_view(GetCommandLineW()).ends_with(RewatchImportPagesMode)) {
            CheckRewatchImportPages();
            return 0;
        }
        if (std::wstring_view(GetCommandLineW()).ends_with(RewatchedSharedMode)) {
            CheckRewatchedDirectMemoryComparedWhenShared();
            CheckImportedDirectMemoryComparedWhenShared();
            return 0;
        }
        if (std::wstring_view(GetCommandLineW()).ends_with(ImportAliasSharedMode)) {
            CheckImportAliasDemotedComparedWhenShared();
            return 0;
        }
        if (std::wstring_view(GetCommandLineW()).ends_with(SharedGenerationsMode)) {
            CheckComparedSharingGenerations();
            return 0;
        }
#endif
        CheckSharedBlock();
        CheckStoredOverUsesPageEvidence();
        CheckStoredAllSince();
        CheckSharedBlockProofs();
        CheckRepeatedPossibleStores();
        CheckOwnStore();
        CheckUnwatch();
        CheckNewestStamp();
        CheckPartlyWatchedStore();
#ifndef _WIN32
        CheckWatchOverlaps();
        CheckWatchedMiddleStore();
#endif
        CheckCoarseRuns();
#ifdef _WIN32
        CheckSparseLeafBoundaries();
        CheckDeferredSharedImports();
        CheckPrivateMappingReuse();
        CheckRewatch();
        CheckUntrackedHint();
        CheckHostHoleInChild();
        CheckFailedCollectUnwatch();
#endif
        CheckVersionRanges();
        CheckCollectBoundaries();
        CheckImportWindow();
        CheckImportWindowZeroPages();
        CheckImportWindowOtherThread();
        CheckImportWindowOverUnwatchedBlock();
        CheckStoreAcrossImports();
        CheckImportWindowFootprint();
        CheckUnwatchedImport();
        CheckUnwatchHooks();
        CheckCommittedRanges();
        CheckAdoptLatestEpoch();
    } catch (const std::exception& error) {
        std::cerr << "write tracking test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "write tracking tests passed\n";
    return 0;
}
