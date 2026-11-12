#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ImageEpochs.hpp"
#include "prx/libSceAgcDriver/Graphics/include/LazyCensus.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"
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
#include <barrier>
#include <atomic>
#include <chrono>
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
constexpr wchar_t MetadataCommitMode[] = L"--metadata-commit";
constexpr wchar_t HostHoleMode[] = L"--host-hole";
constexpr wchar_t ResidentRemapMode[] = L"--resident-direct-remap";
constexpr wchar_t RewatchImportPagesMode[] = L"--rewatch-import-pages";
constexpr wchar_t RewatchedSharedMode[] = L"--rewatched-direct-shared";
constexpr wchar_t ImportAliasSharedMode[] = L"--import-alias-shared";
constexpr wchar_t SharedGenerationsMode[] = L"--compared-shared-generations";
constexpr wchar_t CpuWritePinMode[] = L"--cpu-write-pin";
constexpr wchar_t FreshViewMode[] = L"--fresh-view";
constexpr wchar_t OwnStoreImagesMode[] = L"--own-store-images";
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
        const bool host = GuestArena::GuestArenaHostRegionOverlaps_nid_postfix(arena + offset, Block);
        Require(NewestStamp(arena + offset, Block) == 0, "an untouched leaf has a write stamp");
        Require(UnchangedSince(arena + offset, Block, generation) != host, host ? "an arena host region reads as unchanged" : "an untouched leaf reads as changed");
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

// APS5_ARCHB_FIX_SNAPSHOT_PAGE_REFRESH: ChangedPages lists exactly the pages UnchangedSince cannot clear
// on their own (CPU stores collected per page, partial and possible driver stores, a whole-block driver
// store), merged into ascending runs clipped to the range.
void CheckChangedPages() {
    constexpr std::size_t Page = 4096;
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
    const auto before = CollectWritesUncached(base, bytes);
    Require(before != 0, "the changed-pages fixture was not collected");
    std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
    Require(ChangedPages(base, bytes, before, runs) && runs.empty(), "an unwritten range reports changed pages");

    auto* volatile_memory = static_cast<volatile std::uint8_t*>(memory);
    volatile_memory[5] = 0x22;                          // block 0, page 0
    volatile_memory[3 * Page + 9] = 0x22;               // block 0, page 3
    volatile_memory[Block + 15 * Page + 1] = 0x22;      // block 1, last page
    volatile_memory[2 * Block + 3] = 0x22;              // block 2, first page (a run across the block boundary)
    CollectWritesUncached(base, bytes);
    MarkWritten(base + 3 * Block + 2 * Page + 100, 64); // block 3, page 2 (a driver piece)
    MarkPossiblyWritten(base + 3 * Block + 7 * Page, 16); // block 3, page 7 (a possible piece)

    const auto check = [&](std::uint64_t address, std::size_t length, std::uint64_t since, const char* what) {
        Require(ChangedPages(address, length, since, runs), what);
        for (std::size_t k = 0; k < runs.size(); ++k) {
            Require(runs[k].first < runs[k].second && runs[k].first >= address && runs[k].second <= address + length, "a changed-page run is empty or outside its range");
            if (k != 0) Require(runs[k - 1].second < runs[k].first, "changed-page runs are not merged and ascending");
        }
        for (auto at = address; at < address + length;) {
            const auto to = std::min<std::uint64_t>(address + length, (at / Page + 1) * Page);
            const bool listed = std::any_of(runs.begin(), runs.end(), [&](const auto& run) { return run.first <= at && to <= run.second; });
            Require(listed == !UnchangedSince(at, static_cast<std::size_t>(to - at), since), what);
            at = to;
        }
    };
    check(base, bytes, before, "a page listed by ChangedPages disagrees with UnchangedSince over the page");
    check(base + 100, bytes - 300, before, "an unaligned range: a page listed by ChangedPages disagrees with UnchangedSince");
    Require(ChangedPages(base, bytes, before, runs), "the written fixture was not answered");
    const auto listed = [&](std::uint64_t at) { return std::any_of(runs.begin(), runs.end(), [&](const auto& run) { return run.first <= at && at < run.second; }); };
    Require(listed(base) && listed(base + 3 * Page) && listed(base + Block + 15 * Page) && listed(base + 2 * Block) && listed(base + 3 * Block + 2 * Page) && listed(base + 3 * Block + 7 * Page), "a written page is missing from the changed pages");
    Require(!listed(base + Page) && !listed(base + 4 * Page) && !listed(base + Block) && !listed(base + 2 * Block + Page) && !listed(base + 3 * Block + 3 * Page), "an untouched page of a stamped block is listed as changed");
    Require(std::any_of(runs.begin(), runs.end(), [&](const auto& run) { return run.first == base + Block + 15 * Page && run.second == base + 2 * Block + Page; }), "adjacent changed pages across a block boundary are not one run");

    const auto mid = TrackerGeneration();
    MarkWritten(base + Block, Block);
    Require(ChangedPages(base, bytes, mid, runs) && runs.size() == 1 && runs.front() == std::pair<std::uint64_t, std::uint64_t>{base + Block, base + 2 * Block}, "a whole-block driver store does not mark every page of its block alone");
    check(base, bytes, mid, "after a whole-block store: a page listed by ChangedPages disagrees with UnchangedSince");

    Require(!ChangedPages(base, bytes, 0, runs) && runs.empty(), "a generation of 0 was answered");
    std::array<std::uint8_t, 64> unwatched{};
    const auto outside = reinterpret_cast<std::uint64_t>(unwatched.data());
    if (!Watched(outside, unwatched.size())) Require(!ChangedPages(outside, unwatched.size(), before, runs) && runs.empty(), "an unwatched range was answered");
}

void CheckRepeatedNeighbourStores() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto boundary = base + 2 * Block - 0x9000;
    const auto sharedFirst = boundary - (base + Block);
    const auto neighbourBytes = static_cast<std::size_t>(base + 3 * Block - boundary);
    std::memset(memory, 0x11, 3 * Block);

    const auto synced = CollectWrites(base, 3 * Block);
    Require(synced != 0, "the test block is not collected");
    for (int store = 0; store < 6; ++store) MarkWritten(boundary, neighbourBytes);
    Require(!StoredOver(base + Block, static_cast<std::size_t>(sharedFirst), synced), "repeated stores of the neighbour's bytes count for the first surface's part of the shared block");
    Require(StoredAllSince_nid_no_patch(boundary, neighbourBytes, synced), "repeated stores of the neighbour's bytes forget that they covered them");

    const auto pieces = TrackerGeneration();
    for (int store = 0; store < 3; ++store) MarkWritten(boundary + static_cast<std::uint64_t>(store) * 0x100, 4);
    MarkWritten(boundary, 0x400);
    Require(StoredOver(boundary + 0x200, 4, pieces) && !StoredOver(boundary + 0x400, 4, pieces), "a store over earlier pieces of the block answers for other bytes than its own");
    MarkWritten(boundary + 0x800, 4);
    MarkWritten(boundary + 0x900, 4);
    Require(!StoredOver(base + Block, static_cast<std::size_t>(sharedFirst), pieces), "a store over earlier pieces counts for the first surface's part of the shared block");

    void* fresh = AllocateWatched(2 * Block);
    const auto freshBase = reinterpret_cast<std::uint64_t>(fresh);
    std::memset(fresh, 0x11, 2 * Block);
    const auto freshSynced = CollectWrites(freshBase, 2 * Block);
    const auto neighbour = freshBase + Block - 0x9000;
    MarkWritten(neighbour, 0x100);
    const auto secondStored = MarkWritten(neighbour + 0x1000, 0x100);
    const auto thirdStored = MarkWritten(neighbour + 0x2000, 0x100);
    MarkWritten(neighbour + 0x3000, 0x100);
    const auto settled = TrackerGeneration();
    MarkWritten(neighbour, 0x100);
    MarkWritten(neighbour, 0x100);
    MarkWritten(neighbour + 0x5000, 0x100);
    Require(!StoredOver(freshBase, static_cast<std::size_t>(neighbour - freshBase), secondStored), "a full exact ring evicted a newer piece instead of its oldest");
    Require(StoredOver(freshBase, static_cast<std::size_t>(neighbour - freshBase), freshSynced), "a forgotten store is not taken as a store over the rest of the block");

    const auto partial = TrackerGeneration();
    MarkWritten(neighbour + 0x5080, 0x100);
    Require(StoredOver(neighbour + 0x5000, 0x10, settled) && StoredOver(neighbour + 0x5100, 0x10, partial), "a store that overlaps an earlier piece without containing it replaces it");
    Require(!StoredOver(freshBase, static_cast<std::size_t>(neighbour - freshBase), thirdStored), "a store that overlaps without containing evicted a newer piece instead of the oldest");
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

void CheckGpuWriteNotListed() {
    void* memory = AllocateWatched(Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x11, Block);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> written;
    const auto since = CollectWrittenBlocks(base, Block, std::numeric_limits<std::uint64_t>::max(), written);
    Require(since != 0 && written.empty(), "the block of the GPU write was not collected");
    Require(MarkWritten(base + 0x100, 4) > since, "the GPU write was not stamped");
    CollectWrittenBlocks(base, Block, since, written);
    Require(written.empty(), "a block only a GPU write stamped was listed as written by the CPU");
}

void CheckOwnStoreImages() {
    void* memory = AllocateWatched(Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* guest = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x11, Block);
    guest[0x4123] = 0x6d;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> written;
    auto since = CollectWrittenBlocks(base, Block, std::numeric_limits<std::uint64_t>::max(), written);
    Require(since != 0 && written.empty(), "the own-store image block was not collected");
    std::string failures;
    const auto expect = [&](const std::vector<std::pair<std::uint64_t, std::uint64_t>>& expected, const char* message) {
        since = CollectWrittenBlocks(base, Block, since, written);
        if (written != expected) failures += std::string(failures.empty() ? "" : "; ") + message;
    };
    const auto page = [&](std::uint64_t offset) { return std::vector<std::pair<std::uint64_t, std::uint64_t>>{{base + offset, base + offset + 4096}}; };
    StoreOwnBytes(base + 0xc100, 8, [&] { std::atomic_ref(*reinterpret_cast<std::uint64_t*>(base + 0xc100)).fetch_add(1); });
    expect({}, "a driver store made while no capture compares was listed");
    SetOwnStoreCompare(true);
    const std::array<std::byte, 8> eight{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    Write(base + 0x100, eight);
    expect({}, "a Write no other store met in its page was listed as a CPU write");
    StoreOwnBytes(base + 0x1100, std::span<const std::byte>(eight));
    expect({}, "a store of given bytes no other store met in its page was listed as a CPU write");
    AgcDriverStoreGuestLabel_nid_postfix(reinterpret_cast<std::uint64_t*>(base + 0x2100), 7);
    expect({}, "a guest label store no other store met in its page was listed as a CPU write");
    const std::vector<std::byte> original(4096, std::byte{0x11});
    auto current = original;
    current[0x10] = std::byte{0x5a};
    WriteChanged(base + 0x4000, current, original);
    expect({}, "a write-back no other store met in its page was listed as a CPU write");
    guest[0xd020] = 0x33;
    expect(page(0xd000), "a CPU write before a keep-guest write-back was not listed");
    auto kept = original;
    kept[0x20] = std::byte{0x5b};
    kept[0x30] = std::byte{0x5c};
    WriteChangedKeepingGuest(base + 0xd000, kept, original);
    if (guest[0xd020] != 0x33 || guest[0xd030] != 0x5c) failures += std::string(failures.empty() ? "" : "; ") + "the keep-guest write-back did not keep the guest byte and store the other";
    expect({}, "a keep-guest write-back that kept a guest byte was listed as a CPU write");
    StoreOwnBytes(base + 0x5f00, 0x200, [&](std::span<std::byte> image) {
        std::memset(reinterpret_cast<void*>(base + 0x5f00), 0x44, 0x200);
        if (!image.empty()) std::memset(image.data(), 0x44, image.size());
        std::thread([&] { guest[0x5800] = 0x7e; }).join();
    });
    expect(page(0x5000), "a store another thread made beside a driver store during the store did not list exactly its page");
    StoreOwnBytes(base + 0x8100, 4, [&](std::span<std::byte> image) {
        const std::uint32_t stored = 4;
        std::memcpy(reinterpret_cast<void*>(base + 0x8100), &stored, sizeof(stored));
        if (!image.empty()) std::memcpy(image.data(), &stored, sizeof(stored));
        std::thread([&] { *reinterpret_cast<volatile std::uint32_t*>(base + 0x8100) = 9; }).join();
    });
    expect(page(0x8000), "a store another thread made over a driver store's bytes during the store was not listed");
    StoreOwnBytes(base + 0xa100, 8, [&] {
        std::atomic_ref word(*reinterpret_cast<std::uint64_t*>(base + 0xa100));
        word.fetch_add(1);
        std::thread([&] { word.fetch_sub(1); }).join();
    });
    expect(page(0xa000), "a store another thread made that undid a driver store which gives no bytes was not listed");
    expect({}, "a page listed after a driver store was listed again");
    SetOwnStoreCompare(false);
    Require(failures.empty(), failures);
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


void CheckFreshViewPagesListed() {
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, Block, nullptr);
    Require(section != nullptr, "cannot create the fresh view backing");
    auto* memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(Block, Block));
    Require(memory != nullptr, "cannot allocate the fresh view address");
    GuestArena::GuestArenaMap_nid_postfix(memory, Block, section, 0, PAGE_READWRITE);
    const auto base = reinterpret_cast<std::uintptr_t>(memory);
    Require(ImportWatched(base, Block, [] { return true; }), "the fresh view import failed");
    const auto before = CollectWritesUncached(base, 4096);
    Require(before != 0, "the first page of the fresh view was not collected");
    Require(MarkWritten(base, 4) != 0, "the GPU write next to the fresh pages was not stamped");
    std::vector<std::pair<std::uint64_t, std::uint64_t>> written;
    CollectWrittenBlocks(base, Block, before, written);
    Require(written.size() == 1 && written.front() == std::pair<std::uint64_t, std::uint64_t>{base + 4096, base + Block}, "pages only a fresh collect took were not listed");
    GuestArena::GuestArenaReset_nid_postfix(memory, Block);
    GuestArena::GuestArenaRelease_nid_postfix(memory, Block);
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
    const bool pastWatched = Watched(import + bytes, 4096);
    UnwatchImport(import, bytes);
    Require(Watched(import + bytes, 4096) == pastWatched, "unwatching an import ending inside a block changed the page past it");
    PrintPageState("page past a 0xc4000-byte import", fixture.memory + bytes);
    const auto unwatched = UnwatchSerial();
    Rewatch(BlockSpan(import, bytes));
    Require(!Watched(import, bytes) && UnwatchSerial() == unwatched, "the 64 KiB span past the import's committed pages returned to the write watch");
    Rewatch(PageSpan(import, bytes));
    Require(Watched(import, bytes), "a settled import whose size is not a whole block did not return to the write watch");
    Require(Watched(import + 12 * Block, bytes - 12 * Block) && Watched(import + bytes, 4096) == pastWatched && Watched(import, bytes + 1) == pastWatched, "rewatching an import ending inside a block changed the page past it");
    Require(UnwatchSerial() != unwatched && UntrackedHint(import, bytes) == 0, "a rewatched import still reads as unwatched");
    RequireRewatchedStores(import, bytes);
}

void CheckRewatchUnalignedImport() {
    constexpr std::size_t offset = 0x1000;
    constexpr std::size_t bytes = 0xc4000;
    constexpr std::size_t committed = 0xc8000;
    ImportFixture fixture(13 * Block, committed);
    const auto import = fixture.Base() + offset;
    const bool beforeWatched = Watched(fixture.Base(), offset);
    const bool pastWatched = Watched(import + bytes, 4096);
    UnwatchImport(import, bytes);
    Require(Watched(fixture.Base(), offset) == beforeWatched && Watched(import + bytes, 4096) == pastWatched, "unwatching an import inside its blocks changed the pages around it");
    PrintPageState("page past an import at block offset 0x1000", fixture.memory + committed);
    Rewatch(BlockSpan(import, bytes));
    Require(!Watched(import, bytes), "the 64 KiB span past the committed pages around an import returned to the write watch");
    Rewatch(PageSpan(import, bytes));
    Require(Watched(import, bytes), "a settled import that does not start on a block did not return to the write watch");
    Require(Watched(fixture.Base(), offset) == beforeWatched && Watched(import + bytes, 4096) == pastWatched, "rewatching an import changed the pages around it");
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

std::uint64_t ProbedHostHole() {
    std::uintptr_t arena = 0;
    std::size_t arenaBytes = 0;
    GuestArena::GuestArenaRange_nid_postfix(&arena, &arenaBytes);
    return arena + ((arenaBytes / 2) & ~((std::uint64_t{1} << 32u) - 1));
}

void CheckHostHoleInChild(const wchar_t* mode, std::uint64_t hole) {
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    Require(length != 0 && length < executable.size(), "cannot name the test executable");
    const std::wstring command = L"\"" + std::wstring(executable.data(), length) + L"\" " + mode;
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
    const bool reserved = VirtualAllocEx(process.hProcess, reinterpret_cast<void*>(hole), Block, MEM_RESERVE, PAGE_NOACCESS) != nullptr;
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
bool unwatchHookProven = true;

void RecordUnwatch(std::uint64_t address, std::size_t bytes) {
    unwatchHookBegin = address;
    unwatchHookEnd = address + bytes;
    unwatchHookProven = UnwatchWritesProven();
}

bool FailFreshCollect(std::uintptr_t address, std::size_t bytes, void (*)(void*, std::uintptr_t, std::uintptr_t), void*) {
    failedCollectBegin = address;
    failedCollectEnd = address + bytes;
    return false;
}

#ifdef _WIN32
std::uint64_t failedCollectInterleavingAddress = 0;
std::uint64_t failedCollectInterleavingOrder = 0;
std::uint64_t failedCollectInterleavingProof = 0;
bool failedCollectInterleavingCalled = false;
bool failedCollectInterleavingFailed = false;

void MarkDuringFailedCollectUnwatch(std::uint64_t address, std::size_t bytes) {
    failedCollectInterleavingCalled = true;
    SetUnwatchHook(nullptr);
    try {
        Unwatch(address, bytes);
        failedCollectInterleavingProof = MarkPossiblyWritten(failedCollectInterleavingAddress, 4, failedCollectInterleavingOrder);
    } catch (...) {
        failedCollectInterleavingFailed = true;
    }
}
#endif

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
    Require(!unwatchHookProven, "a failed collect ran the unwatch hook as if the CPU writes to its block were proven");
}

struct CpuWritePinProbe;
CpuWritePinProbe* cpuWritePinProbe = nullptr;

struct CpuWritePinProbe {
    std::uint64_t address;
    std::uint64_t unrelated;
    std::atomic<bool> entered{false};
    std::atomic<bool> waited{false};
    std::atomic<bool> passed{false};
    std::atomic<bool> unrelatedPassed{false};
    std::atomic<bool> failed{false};
    std::thread mutation;
    std::thread other;

    void Join() {
        if (mutation.joinable()) mutation.join();
        if (other.joinable()) other.join();
    }

    ~CpuWritePinProbe() {
        Join();
        GuestAllocations::GuestAllocationsSetPinWaiter_nid_postfix(nullptr);
        cpuWritePinProbe = nullptr;
    }
};

bool ObserveCpuWritePin(std::uintptr_t address, std::size_t bytes) {
    auto& probe = *cpuWritePinProbe;
    if (address <= probe.address && probe.address < address + bytes) probe.waited.store(true);
    std::this_thread::yield();
    return false;
}

void CheckCpuWritePinDuringStore(std::uint64_t address, std::size_t bytes) {
    auto& probe = *cpuWritePinProbe;
    if (address != probe.address || bytes != 4 || probe.entered.exchange(true)) return;
    const auto attempt = [&](std::uint64_t target, std::atomic<bool>& passed) {
        try {
            GuestAllocations::Mutation mutation;
            mutation.RequireUnpinned(reinterpret_cast<const void*>(target), 4);
            passed.store(true);
        } catch (const std::exception&) {
            probe.failed.store(true);
        }
    };
    probe.mutation = std::thread(attempt, probe.address, std::ref(probe.passed));
    probe.other = std::thread(attempt, probe.unrelated, std::ref(probe.unrelatedPassed));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!probe.unrelatedPassed.load() || (!probe.waited.load() && !probe.passed.load())) && !probe.failed.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    Require(!probe.failed.load(), "the CPU store mutation probe threw");
    Require(probe.unrelatedPassed.load(), "a CPU store pinned an unrelated registered range");
    Require(probe.waited.load() && !probe.passed.load(), "a mapping mutation passed its unmap gate during an executing CPU store");
}

void CheckCpuWritePin() {
    auto* memory = static_cast<std::byte*>(AllocateWatched(4 * Block));
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    struct Cleanup {
        std::byte* memory;
        ~Cleanup() {
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(memory);
                mutation.Remove(memory + Block);
                mutation.Remove(memory + 3 * Block);
            }
            GuestArena::GuestArenaReset_nid_postfix(memory, 4 * Block);
            GuestArena::GuestArenaRelease_nid_postfix(memory, 4 * Block);
        }
    } cleanup{memory};
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(memory, Block, true, true, true);
        mutation.Add(memory + Block, Block, true, true, true);
        mutation.Add(memory + 3 * Block, Block, true, true, true);
    }
    const auto check = [&](std::uint64_t begin, std::size_t bytes, std::uint64_t target, std::uint64_t unrelated, bool throws) {
        CpuWritePinProbe probe{target, unrelated};
        cpuWritePinProbe = &probe;
        GuestAllocations::GuestAllocationsSetPinWaiter_nid_postfix(&ObserveCpuWritePin);
        const std::vector<std::byte> value(bytes, std::byte{0x6a});
        bool threw = false;
        try {
            StoreOwnBytes(begin, bytes, [&] {
                CheckCpuWritePinDuringStore(probe.address, 4);
                if (throws) throw std::logic_error("synthetic CPU store refusal");
                std::memcpy(reinterpret_cast<void*>(begin), value.data(), bytes);
            });
        } catch (const std::logic_error&) {
            threw = true;
        }
        probe.Join();
        Require(threw == throws, "the CPU store changed its callback exception");
        Require(probe.entered.load() && probe.waited.load() && probe.passed.load() && probe.unrelatedPassed.load() && !probe.failed.load(), "the CPU store did not release exactly its mapping after the write");
        if (!throws) Require(std::memcmp(reinterpret_cast<const void*>(begin), value.data(), bytes) == 0, "the pinned CPU store did not reach guest memory");
    };
    check(base + 32, 4, base + 32, base + Block, false);
    check(base, Block, base + 32, base + Block, false);
    check(base, 3 * Block, base + 32, base + 3 * Block, false);
    check(base, 3 * Block, base + Block + 32, base + 3 * Block, false);
    check(base + 32, 4, base + 32, base + Block, true);
    const std::array<std::byte, 4> value{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    Write(base, value, 4);
    const std::array<std::byte, 4> original{};
    WriteChanged(base + Block, value, original);
    constexpr std::uint64_t label = 0x123456789abcdef0;
    AgcDriverStoreGuestLabel_nid_postfix(reinterpret_cast<std::uint64_t*>(memory + 16), label);
    Require(*reinterpret_cast<const std::uint64_t*>(memory + 16) == label, "a pinned host label store did not reach guest memory");
    bool refused = false;
    try {
        Write(base + 1, value, 4);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    Require(refused, "a misaligned CPU write was accepted");
    {
        GuestAllocations::Mutation mutation;
        mutation.RequireUnpinned(memory, 4 * Block);
    }
    Require(std::memcmp(memory, value.data(), value.size()) == 0 && std::memcmp(memory + Block, value.data(), value.size()) == 0, "a checked CPU write did not reach guest memory");
    std::cout << "CPU stores pin both intersecting mappings, exclude adjacent ranges and release pins after success or refusal\n";
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

#ifdef _WIN32
void CheckWidenedCollectBesideUncommittedPages() {
    constexpr std::size_t reserved = std::size_t{1} << 30;
    constexpr std::size_t committed = Block - 4 * 4096;
    void* range = GuestArena::GuestArenaAllocate_nid_postfix(reserved, Block);
    struct Cleanup {
        void* range;
        ~Cleanup() {
            GuestArena::GuestArenaReset_nid_postfix(range, reserved);
            GuestArena::GuestArenaRelease_nid_postfix(range, reserved);
        }
    } cleanup{range};
    void* memory = static_cast<std::uint8_t*>(range) + reserved - Block;
    MEMORY_BASIC_INFORMATION tail{};
    Require(VirtualQuery(static_cast<std::uint8_t*>(memory) + committed, &tail, sizeof(tail)) == sizeof(tail) && tail.State == MEM_RESERVE, "the test block's tail is not a placeholder");
    GuestArena::GuestArenaCommit_nid_postfix(memory, committed, PAGE_READWRITE, committed);
    Require(VirtualQuery(static_cast<std::uint8_t*>(memory) + committed, &tail, sizeof(tail)) == sizeof(tail) && tail.State == MEM_RESERVE, "committing the test block's head committed its tail");
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 17, committed);
    BumpCollectEpoch();
    const auto before = CollectWrites(base + 4096, 64);
    Require(before != 0, "a collect beside uncommitted pages of its block failed");
    bytes[4096 + 8] = 19;
    BumpCollectEpoch();
    Require(CollectWrites(base + 4096, 64) > before && !UnchangedSince(base + 4096, 64, before), "a collect beside uncommitted pages missed a CPU write");
}
#endif

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
    std::vector<std::pair<std::uint64_t, std::uint64_t>> written;
    CollectWrittenBlocks(base, Block, synced, written);
    Require(written.size() == 1 && written.front() == std::pair<std::uint64_t, std::uint64_t>{base, base + 4096}, "a page only an import window stamped as a possible CPU touch was not listed");
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

void CheckImportWindowAcrossThreads() {
    constexpr std::size_t blocks = 264;
    constexpr std::size_t gap = 130;
    constexpr std::size_t pageBytes = 4096;
    void* memory = AllocateWatched(blocks * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    auto* plain = static_cast<std::uint8_t*>(memory);
    std::memset(plain, 0x11, blocks * Block);
    for (std::size_t page = 0; page < blocks * Block / pageBytes; ++page) {
        const std::uint64_t mark = (page + 1) * 0x9E3779B97F4A7C15ull;
        std::memcpy(plain + page * pageBytes, &mark, sizeof(mark));
    }
    Unwatch(base + gap * Block, Block);
    const auto synced = CollectWritesUncached(base, gap * Block);
    const auto syncedAfter = CollectWritesUncached(base + (gap + 1) * Block, (blocks - gap - 1) * Block);
    Require(synced != 0 && syncedAfter != 0, "the blocks of a large import window are not collected");
    constexpr std::array<std::size_t, 4> written{3, 70, 140, 250};
    Require(ImportWatched(base, blocks * Block, [&] {
        for (std::size_t page = 0; page < blocks * Block / pageBytes; ++page) {
            if (page * pageBytes / Block != gap) bytes[page * pageBytes + 0x100] = 0x11;
        }
        for (const auto block : written) bytes[block * Block + 0x3100] = 0x22;
        return true;
    }), "the large import window import failed");
    Require(Watched(base, gap * Block) && Watched(base + (gap + 1) * Block, (blocks - gap - 1) * Block) && !Watched(base + gap * Block, Block), "a large import window changed which blocks are watched");
    const auto since = [&](std::size_t block) { return block < gap ? synced : syncedAfter; };
    for (std::size_t block = 0; block < blocks; ++block) {
        if (block == gap) continue;
        const bool stored = std::find(written.begin(), written.end(), block) != written.end();
        if (stored) Require(WrittenSince(base + block * Block, Block, since(block)), "a store of new bytes within a large import window was missed in block " + std::to_string(block));
        else Require(UnchangedSince(base + block * Block, Block, since(block)), "a page a large import window resolved without changing its bytes reads as written in block " + std::to_string(block));
    }
}

#if defined(__linux__)
void CheckImportWindowOverAlias() {
    const int file = memfd_create("aps5-import-window-alias", 0);
    Require(file >= 0 && ftruncate(file, Block) == 0, "cannot create the aliased import window backing");
    void* raw = mmap(nullptr, 3 * Block, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(raw != MAP_FAILED, "cannot reserve the aliased import window");
    const auto low = (reinterpret_cast<std::uintptr_t>(raw) + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1);
    const auto high = low + Block;
    for (const auto view : {low, high}) {
        Require(mmap(reinterpret_cast<void*>(view), Block, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, file, 0) != MAP_FAILED, "cannot map an import window alias");
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(reinterpret_cast<void*>(view), Block);
    }
    if (GuestWriteWatch::GuestWriteWatchAlias_nid_postfix(reinterpret_cast<void*>(low), reinterpret_cast<void*>(high), Block)) {
        auto* bytes = reinterpret_cast<volatile std::uint8_t*>(low);
        std::memset(reinterpret_cast<void*>(low), 0x11, Block);
        const auto synced = CollectWritesUncached(low, 2 * Block);
        Require(synced != 0, "the aliased import window is not collected");
        Require(ImportWatched(high, Block, [&] {
            bytes[0x100] = 0x22;
            return true;
        }, true, false), "the aliased import window import failed");
        Require(Watched(low, 2 * Block), "an import window over an alias left a view unwatched");
        Require(WrittenSince(high, Block, synced) && WrittenSince(low, Block, synced), "a store through an alias during an import window was missed at one of its views");
    } else {
        std::cout << "write watch aliases unavailable: import window over an alias skipped\n";
    }
    GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(reinterpret_cast<void*>(low), 2 * Block);
    munmap(raw, 3 * Block);
    close(file);
}
#endif

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

struct ImportHookCall {
    std::uint64_t begin;
    std::uint64_t end;
    bool watched;
    bool proven;
};

std::vector<ImportHookCall> importHookCalls;
std::uint64_t importHookProbe = 0;

void RecordImportHook(std::uint64_t address, std::size_t bytes) {
    importHookCalls.push_back({address, address + bytes, Watched(importHookProbe, Block), UnwatchWritesProven()});
}

void CheckImportWalkUnwatchHooks() {
    void* memory = AllocateWatched(4 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x11, 4 * Block);
    Require(CollectWritesUncached(base, 4 * Block) != 0, "the import walk block is not collected");
    struct Reset {
        ~Reset() {
            SetImportWalkFailureForTest(0);
            SetUnwatchHook(nullptr);
        }
    } reset;
    SetUnwatchHook(&RecordImportHook);
    for (const int walk : {1, 2}) {
        const std::string what = walk == 1 ? "an import that stays watched whose page walk failed before the import" : "an import that stays watched whose page walk failed after the import";
        const auto imported = base + static_cast<std::uint64_t>(walk - 1) * 2 * Block;
        Require(Watched(imported, Block), what + ": its block was not watched before the import");
        importHookCalls.clear();
        importHookProbe = imported;
        bool hookedBeforeImport = false;
        SetImportWalkFailureForTest(walk);
        Require(ImportWatched(imported, Block, [&] {
            hookedBeforeImport = !importHookCalls.empty();
            return true;
        }), what + ": the import failed");
        SetImportWalkFailureForTest(0);
        Require(importHookCalls.size() == 1, what + " did not run the unwatch hook once");
        const auto& call = importHookCalls.front();
        Require(call.begin <= imported && imported + Block <= call.end, what + " gave the unwatch hook a range that misses its block");
        Require(call.watched, what + " ran the unwatch hook after its block left the write watch");
        Require(!call.proven, what + " ran the unwatch hook as if the CPU writes to its block were proven");
        Require(hookedBeforeImport == (walk == 1), what + (walk == 1 ? " ran the unwatch hook after the import" : " ran the unwatch hook before the import"));
        Require(!Watched(imported, Block), what + " left its block watched");
    }
    importHookCalls.clear();
    importHookProbe = base + 3 * Block;
    Unwatch(base + 3 * Block, Block);
    Require(importHookCalls.size() == 1 && importHookCalls.front().watched && importHookCalls.front().proven, "Unwatch did not run the unwatch hook once, while watched, with its CPU writes proven");
}

std::uint64_t collectDuringHookAddress = 0;
std::size_t collectDuringHookBytes = 0;
std::size_t collectDuringHookCalls = 0;

void CollectDuringHook(std::uint64_t, std::size_t) {
    if (collectDuringHookCalls++ == 0) static_cast<void>(CollectWritesUncached(collectDuringHookAddress, collectDuringHookBytes));
}

void CheckImportWalkHookKeepsLaterWindows() {
    void* memory = AllocateWatched(4 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x11, 4 * Block);
    Unwatch(base + Block, Block);
    const auto synced = CollectWritesUncached(base + 2 * Block, 2 * Block);
    Require(synced != 0 && CollectWritesUncached(base, Block) != 0, "the blocks around the unwatched one are not collected");
    struct Reset {
        ~Reset() {
            SetImportWalkFailureForTest(0);
            SetUnwatchHook(nullptr);
        }
    } reset;
    collectDuringHookAddress = base + 2 * Block;
    collectDuringHookBytes = 2 * Block;
    collectDuringHookCalls = 0;
    SetUnwatchHook(&CollectDuringHook);
    SetImportWalkFailureForTest(2, base);
    Require(ImportWatched(base, 4 * Block, [&] {
        bytes[0x100] = 0x11;
        bytes[2 * Block + 0x100] = 0x11;
        bytes[3 * Block + 0x2000] = 0x22;
        return true;
    }), "the import whose first span failed its page walk failed");
    SetImportWalkFailureForTest(0);
    SetUnwatchHook(nullptr);
    Require(collectDuringHookCalls == 1, "the span whose page walk failed after the import did not run the unwatch hook once");
    Require(!Watched(base, Block), "the span whose page walk failed after the import stayed watched");
    Require(Watched(base + 2 * Block, 2 * Block), "the span whose page walk succeeded left the write watch");
    const std::array<std::uint64_t, 2> generations{synced, synced};
    std::array<std::uint8_t, 2> changed{};
    std::array<std::uint8_t, 2> cpu{};
    Require(ChangedBlocks(base + 2 * Block, 2 * Block, generations, changed, cpu), "the span after the failed one is not tracked");
    Require(changed[0] == BlockUnchanged && UnchangedSince(base + 2 * Block, Block, synced), "a collect run by the unwatch hook of an earlier span counted a page the import touched without changing it as a CPU write");
    Require(cpu[0] == 1, "a page the import touched lost its import window walk to the unwatch hook of an earlier span");
    Require(changed[1] == BlockWritten && WrittenSince(base + 3 * Block, Block, synced), "a store of new bytes within the import was missed after the unwatch hook of an earlier span");
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

#ifdef _WIN32
// APS5_ARCHB_WT_FAULT (+ _VERIFY from the environment): write tracking by page protection in shadow
// mode. A walk makes its pages candidates and a flip protects them with one stamp newer than every
// generation handed out before; the first store faults once and opens its page; the next flip protects
// the opened pages again in contiguous runs; VERIFY counts no mismatch for stores the fault tracker
// saw and one for a store into a page unprotected behind its back; a fault on a page the tracker does
// not protect is not handled; a kernel-mode read into a protected page fails outside a host-write scope
// and succeeds inside one, which holds the page open across flips; small driver stores keep their pages
// open for one flip; a host import holds its pages open; resident direct memory that is mapped a
// second time leaves the tracker (its views are the shared-mapping tracker's), so a store through the
// read-write alias of the view never faults into the fault tracker.
void CheckWtFault() {
    using namespace GuestArena;
    using AgcDriver::ArchB::Feature;
    using AgcDriver::ArchB::Switches;
    Require(Switches::Enabled(Feature::WtFault), "the fault-tracking switch does not follow the environment");
    Require(WtFault().active, "the fault tracker did not turn on with write watching");
    const bool verify = Switches::Enabled(Feature::WtFaultVerify);
    const auto counters = [] {
        WtFaultCounters value{};
        GuestWtFaultCounters_nid_no_patch(&value);
        return value;
    };
    const auto state = [](std::uint64_t address) { return GuestWtFaultWord_nid_no_patch(address) & WtFaultStateMask; };
    const auto stamp = [](std::uint64_t address) { return GuestWtFaultWord_nid_no_patch(address) >> WtFaultStampShift; };
    const auto protection = [](std::uint64_t address) {
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == sizeof(info), "cannot query a fault-tracked page");
        return info.Protect;
    };
    constexpr std::uint64_t Page = 4096;
    PartlyCommitted range;
    range.Commit(0);
    range.Commit(1);
    const auto base = range.Address();
    volatile std::uint8_t* data = range.Data();
    std::memset(range.Data(), 0x11, 2 * Block);
    const auto mismatches0 = WtFault().mismatches;

    // A walk makes its pages candidates; the flip protects them, stamped after the protect.
    BumpCollectEpoch();
    const auto g0 = CollectWrites(base, 2 * Block);
    Require(g0 != 0 && state(base) == WtFaultOpen && state(base + 2 * Block - Page) == WtFaultOpen, "walked pages did not become candidates");
    WtFaultFlip();
    Require(state(base) == WtFaultArmed && state(base + 2 * Block - Page) == WtFaultArmed && stamp(base) > g0, "the flip did not protect the candidates with a stamp newer than the walk");
    Require(protection(base) == PAGE_READONLY, "an armed page is not read-only");
    Require(GuestWtFaultChangedSince_nid_no_patch(base, 2 * Block, GuestWtFaultClockBump_nid_no_patch()) == WtFaultAnswer::Unchanged, "armed pages read as changed since a later generation");
    Require(GuestWtFaultChangedSince_nid_no_patch(base, 2 * Block, g0) == WtFaultAnswer::Changed, "pages armed after a generation read as unchanged since it");
    Require(Accessible(reinterpret_cast<const void*>(base + Page), Page, true), "an armed page reads as not writable to the driver's probes");

    // The first store into a page faults once and restores the guest's protection.
    const auto beforeStores = counters();
    data[0x10] = 0x22;
    data[0x20] = 0x23;
    data[3 * Page] = 0x24;
    const auto afterStores = counters();
    Require(afterStores.faults == beforeStores.faults + 2, "two written pages did not fault exactly twice");
    Require(state(base) == WtFaultOpen && state(base + 3 * Page) == WtFaultOpen && state(base + Page) == WtFaultArmed, "a fault did not open exactly its page");
    Require(protection(base) == PAGE_READWRITE, "a faulted page did not get the guest's protection back");
    const auto now = GuestWtFaultClockBump_nid_no_patch();
    Require(GuestWtFaultChangedSince_nid_no_patch(base, Page, now) == WtFaultAnswer::Changed && GuestWtFaultChangedSince_nid_no_patch(base + Page, Page, now) == WtFaultAnswer::Unchanged, "an open page read as unchanged or an armed one as changed");
    BumpCollectEpoch();
    const auto g1 = CollectWrites(base, 2 * Block);
    Require(!UnchangedSince(base, Page, g0) && g1 > g0, "the reference tracker missed a store into a fault-tracked page");
    if (verify) Require(WtFault().mismatches == mismatches0 && counters().verifyDirty >= afterStores.verifyDirty + 2, "VERIFY did not see the stores or counted a mismatch for them");

    // The flip protects the opened pages again in contiguous runs: {0}, {3}, {8..15}, {20}.
    for (std::uint64_t page = 8; page < 16; ++page) data[page * Page] = 0x30;
    data[20 * Page] = 0x31;
    const auto beforeArm = counters();
    WtFaultFlip();
    const auto afterArm = counters();
    Require(afterArm.armedPages - beforeArm.armedPages == 11 && afterArm.armRuns - beforeArm.armRuns == 4, "the flip did not protect the 11 opened pages in 4 runs");
    for (const std::uint64_t page : {0ull, 3ull, 8ull, 15ull, 20ull}) Require(state(base + page * Page) == WtFaultArmed && stamp(base + page * Page) > g1, "an opened page was not protected again with a newer stamp");

    // VERIFY catches a store the fault tracker never saw (a page unprotected behind its back).
    if (verify) {
        DWORD previous = 0;
        Require(VirtualProtect(reinterpret_cast<void*>(base + 5 * Page), Page, PAGE_READWRITE, &previous) && previous == PAGE_READONLY, "cannot unprotect a page behind the tracker");
        const auto faults = counters().faults;
        data[5 * Page] = 0x55;
        Require(counters().faults == faults, "a store into a page unprotected behind the tracker faulted");
        BumpCollectEpoch();
        CollectWrites(base, 2 * Block);
        Require(WtFault().mismatches == mismatches0 + 1, "VERIFY did not count the store the fault tracker missed");
        GuestWtFaultForget_nid_no_patch(base + 5 * Page, Page);
    }

    // A fault on a page the tracker does not protect falls through to the next handler.
    GuestArenaSetProtection_nid_postfix(base + 6 * Page, Page, PAGE_READONLY);
    Require(state(base + 6 * Page) == WtFaultUntracked && !GuestWtFaultHandle_nid_no_patch(base + 6 * Page + 8), "a page the guest made read-only is still claimed by the fault tracker");
    GuestArenaSetProtection_nid_postfix(base + 6 * Page, Page, PAGE_READWRITE);

    // A kernel-mode read into a protected page fails; inside a host-write scope it succeeds and the page
    // stays open across flips until the scope ends.
    wchar_t directory[MAX_PATH];
    wchar_t path[MAX_PATH];
    Require(GetTempPathW(MAX_PATH, directory) != 0 && GetTempFileNameW(directory, L"wtf", 0, path) != 0, "cannot make a temporary file");
    {
        HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        Require(file != INVALID_HANDLE_VALUE, "cannot open the temporary file");
        std::vector<std::uint8_t> bytes(Page, 0x77);
        DWORD done = 0;
        Require(WriteFile(file, bytes.data(), static_cast<DWORD>(Page), &done, nullptr) && done == Page, "cannot fill the temporary file");
        auto* target = reinterpret_cast<void*>(base + 9 * Page);
        Require(state(base + 9 * Page) == WtFaultArmed, "the kernel-write target is not armed");
        SetFilePointer(file, 0, nullptr, FILE_BEGIN);
        const bool unscoped = ReadFile(file, target, static_cast<DWORD>(Page), &done, nullptr) != 0;
        const auto error = GetLastError();
        Require(!unscoped && error == ERROR_NOACCESS, "a kernel-mode read into a protected page did not fail with ERROR_NOACCESS");
        SetFilePointer(file, 0, nullptr, FILE_BEGIN);
        {
            const GuestArena::HostWrite scope(target, Page);
            Require(state(base + 9 * Page) == WtFaultOpen, "a host-write scope did not open its protected page");
            Require(ReadFile(file, target, static_cast<DWORD>(Page), &done, nullptr) && done == Page, "a kernel-mode read inside a host-write scope failed");
            WtFaultFlip();
            Require(state(base + 9 * Page) == WtFaultOpen, "a flip protected a page held open by a host-write scope");
        }
        CloseHandle(file);
        Require(data[9 * Page] == 0x77 && data[9 * Page + Page - 1] == 0x77, "the kernel-mode read did not land");
        WtFaultFlip();
        Require(state(base + 9 * Page) == WtFaultArmed, "a page was not protected again after its host-write scope ended");
    }

    // Small driver stores keep their pages open for the next flip only.
    data[12 * Page] = 0x40;
    Require(MarkWritten(base + 12 * Page, 64) != 0, "a driver store into a fault-tracked page was not stamped");
    WtFaultFlip();
    Require(state(base + 12 * Page) == WtFaultOpen, "a flip protected a page the driver stored since the previous flip");
    WtFaultFlip();
    Require(state(base + 12 * Page) == WtFaultArmed, "a page the driver stopped storing stayed open");

    // A host import holds its pages open.
    Require(GuestArenaBeginHostImport_nid_no_patch(reinterpret_cast<void*>(base + 14 * Page), Page), "the host-import scope refused a fault-tracked page");
    Require(state(base + 14 * Page) == WtFaultOpen && protection(base + 14 * Page) == PAGE_READWRITE, "a host import did not open its protected page");
    WtFaultFlip();
    Require(state(base + 14 * Page) == WtFaultOpen, "a flip protected a page held by a host import");
    GuestArenaEndHostImport_nid_no_patch(reinterpret_cast<void*>(base + 14 * Page), Page);
    WtFaultFlip();
    Require(state(base + 14 * Page) == WtFaultArmed, "a page was not protected again after its host import ended");

    // Resident direct memory is fault-tracked; mapped a second time, its views leave the tracker and a
    // store through the read-write alias of a view never faults into it.
    {
        constexpr std::size_t Bytes = Block;
        auto* memory = static_cast<std::uint8_t*>(GuestArenaAllocate_nid_postfix(2 * Bytes, Block));
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, Bytes, nullptr);
        Require(memory != nullptr && section != nullptr, "cannot create the direct memory fixture");
        GuestArenaForgetSection_nid_postfix(section);
        GuestArenaMapDirect_nid_postfix(memory, Bytes, section, 0, PAGE_READWRITE);
        const auto direct = reinterpret_cast<std::uint64_t>(memory);
        BumpCollectEpoch();
        Require(CollectWrites(direct, Bytes) != 0, "resident direct memory was not collected");
        WtFaultFlip();
        Require(state(direct) == WtFaultArmed, "resident direct memory was not protected");
        const auto faults = counters().faults;
        memory[8] = 0x61;
        Require(counters().faults == faults + 1 && state(direct) == WtFaultOpen, "a store into resident direct memory did not fault once");
        GuestArenaMapDirect_nid_postfix(memory + Bytes, Bytes, section, 0, PAGE_READWRITE);
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(memory, &info, sizeof(info)) == sizeof(info) && info.Type == MEM_MAPPED, "a second mapping kept the direct memory resident");
        BumpCollectEpoch();
        CollectWrites(direct, Bytes);
        WtFaultFlip();
        Require(state(direct) == WtFaultExcluded || state(direct) == WtFaultUntracked, "a shared view was protected by the fault tracker");
        auto* alias = static_cast<volatile std::uint8_t*>(GuestArenaMapAlias_nid_postfix(direct, Bytes));
        Require(alias != nullptr, "no read-write alias of the shared view");
        const auto aliasFaults = counters().faults;
        alias[16] = 0x62;
        memory[24] = 0x63;
        Require(counters().faults == aliasFaults && memory[16] == 0x62, "a store through the alias or the view faulted into the fault tracker");
        GuestArenaUnmapAlias_nid_postfix(const_cast<std::uint8_t*>(alias));
        GuestArenaReset_nid_postfix(memory, 2 * Bytes);
        GuestArenaRelease_nid_postfix(memory, 2 * Bytes);
        GuestArenaForgetSection_nid_postfix(section);
        CloseHandle(section);
    }
    if (verify) Require(WtFault().mismatches == mismatches0 + 1, "VERIFY counted mismatches beyond the provoked one");
    const auto final = counters();
    std::cout << "wt-fault: faults " << final.faults << " armed pages " << final.armedPages << " runs " << final.armRuns << " holds " << final.holds << " verify dirty " << final.verifyDirty << " untracked " << final.verifyUntracked << " mismatches " << final.verifyMismatches << "\n";
}

// APS5_ARCHB_WT_FAULT, host-side copies into armed guest memory laid out like live direct memory (one
// 16 KiB allocation per PS5 page, GuestArenaCommit's granule): an arm protects each allocation apart, so a
// host-write scope over several of them opens a run of armed pages one VirtualProtect refuses. The scope
// must still leave every page writable and open, so libkernel's 64 KiB staging memcpy (APR and file reads)
// and a kernel-mode read land without a fault; an explicit open the same; and after every step a page is
// read-only exactly when the tracker holds it protected, so a guest store after the next flip either
// faults into the tracker or does not fault at all (Demon's Souls, debug menu -> Title, 2026-11-05).
void CheckWtFaultHostCopy() {
    using namespace GuestArena;
    using AgcDriver::ArchB::Feature;
    using AgcDriver::ArchB::Switches;
    Require(Switches::Enabled(Feature::WtFault) && WtFault().active, "the fault tracker did not turn on with write watching");
    constexpr std::uint64_t Page = 4096;
    constexpr std::size_t Granule = 16384;
    constexpr std::size_t Bytes = 4 * Block;
    constexpr std::size_t Staging = 64 * 1024;
    const auto counters = [] {
        WtFaultCounters value{};
        GuestWtFaultCounters_nid_no_patch(&value);
        return value;
    };
    const auto state = [](std::uint64_t address) { return GuestWtFaultWord_nid_no_patch(address) & WtFaultStateMask; };
    const auto protection = [](std::uint64_t address) {
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == sizeof(info), "cannot query a fault-tracked page");
        return info.Protect;
    };
    const auto requireConsistent = [&](std::uint64_t base, const std::string& step) {
        for (std::uint64_t page = base; page < base + Bytes; page += Page) {
            const auto current = state(page);
            const bool tracked = current == WtFaultArmed || current == WtFaultArming || current == WtFaultOpening;
            const auto actual = protection(page);
            Require(tracked == (actual == PAGE_READONLY), step + ": page +0x" + std::to_string(page - base) + " is in state " + std::to_string(current) + " with protection 0x" + std::to_string(actual));
        }
    };
    struct Fixture {
        std::uint8_t* memory = nullptr;
        Fixture() {
            memory = static_cast<std::uint8_t*>(GuestArenaAllocate_nid_postfix(Bytes, Block));
            Require(memory != nullptr, "cannot allocate the host-copy fixture");
            GuestArenaCommit_nid_postfix(memory, Bytes, PAGE_READWRITE, Granule);
        }
        ~Fixture() {
            GuestArenaReset_nid_postfix(memory, Bytes);
            GuestArenaRelease_nid_postfix(memory, Bytes);
        }
        std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(memory); }
    };
    const auto armFresh = [&](Fixture& fixture) {
        const auto base = fixture.Address();
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(fixture.memory + Granule, &info, sizeof(info)) == sizeof(info) && info.AllocationBase == fixture.memory + Granule, "the fixture is not one allocation per 16 KiB");
        std::memset(fixture.memory, 0x11, Bytes);
        const auto armed = [&] {
            for (std::uint64_t page = base; page < base + Bytes; page += Page) {
                if (state(page) != WtFaultArmed || protection(page) != PAGE_READONLY) return false;
            }
            return true;
        };
        const auto before = counters();
        for (int flip = 0; flip < 4 && !armed(); ++flip) {
            BumpCollectEpoch();
            CollectWrites(base, Bytes);
            WtFaultFlip();
        }
        const auto after = counters();
        Require(armed(), "a fixture page was not armed within four flips");
        Require(after.armRuns - before.armRuns >= Bytes / Granule, "the arm protected the fixture across allocations");
        requireConsistent(base, "armed");
    };
    const auto storeEveryPage = [&](Fixture& fixture, std::uint8_t value, const std::string& step) {
        for (std::size_t offset = 0; offset < Bytes; offset += Page) static_cast<volatile std::uint8_t*>(fixture.memory)[offset + 7] = value;
        requireConsistent(fixture.Address(), step);
    };
    const auto start = counters();

    // The staging copy of libkernel's APR read, inside the scope it opens, over 16 allocations.
    {
        Fixture fixture;
        armFresh(fixture);
        const auto base = fixture.Address();
        const auto beforeScope = counters();
        {
            const HostWrite scope(fixture.memory, Bytes);
            Require(scope.Open(), "the host-write scope refused armed private memory");
            requireConsistent(base, "inside the host-write scope");
            for (std::uint64_t page = base; page < base + Bytes; page += Page) Require(state(page) == WtFaultOpen && protection(page) == PAGE_READWRITE, "a host-write scope over several allocations left a page protected");
            std::vector<std::uint8_t> staging(Staging);
            for (std::size_t offset = 0; offset < Bytes; offset += Staging) {
                std::fill(staging.begin(), staging.end(), static_cast<std::uint8_t>(0x40 + offset / Staging));
                std::memcpy(fixture.memory + offset, staging.data(), staging.size());
            }
            WtFaultFlip();
            for (std::uint64_t page = base; page < base + Bytes; page += Page) Require(state(page) == WtFaultOpen, "a flip protected a page held open by a host-write scope");
        }
        const auto afterScope = counters();
        Require(afterScope.faults == beforeScope.faults, "the host copy faulted inside its host-write scope");
        Require(afterScope.openSplitRuns > beforeScope.openSplitRuns && afterScope.openRefused == beforeScope.openRefused, "the scope's run across allocations was not restored region by region");
        for (std::size_t offset = 0; offset < Bytes; offset += Staging) Require(fixture.memory[offset] == 0x40 + offset / Staging && fixture.memory[offset + Staging - 1] == 0x40 + offset / Staging, "the host copy did not land");
        WtFaultFlip();
        requireConsistent(base, "after the scope's flip");
        storeEveryPage(fixture, 0x51, "after guest stores");
    }

    // A kernel-mode read (libkernel's file and AIO reads) into a scope over four allocations, unaligned.
    {
        Fixture fixture;
        armFresh(fixture);
        const auto base = fixture.Address();
        wchar_t directory[MAX_PATH];
        wchar_t path[MAX_PATH];
        Require(GetTempPathW(MAX_PATH, directory) != 0 && GetTempFileNameW(directory, L"wtc", 0, path) != 0, "cannot make a temporary file");
        HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        Require(file != INVALID_HANDLE_VALUE, "cannot open the temporary file");
        std::vector<std::uint8_t> bytes(Staging, 0x66);
        DWORD done = 0;
        Require(WriteFile(file, bytes.data(), static_cast<DWORD>(Staging), &done, nullptr) && done == Staging, "cannot fill the temporary file");
        SetFilePointer(file, 0, nullptr, FILE_BEGIN);
        auto* target = fixture.memory + Granule + 0x123;
        {
            const HostWrite scope(target, Staging);
            requireConsistent(base, "inside the kernel-read scope");
            const bool read = ReadFile(file, target, static_cast<DWORD>(Staging), &done, nullptr) != 0;
            Require(read && done == Staging, "a kernel-mode read inside a host-write scope over several allocations failed (error " + std::to_string(GetLastError()) + ")");
        }
        CloseHandle(file);
        Require(target[0] == 0x66 && target[Staging - 1] == 0x66, "the kernel-mode read did not land");
        WtFaultFlip();
        requireConsistent(base, "after the kernel read's flip");
        storeEveryPage(fixture, 0x52, "after guest stores");
    }

    // An explicit open of armed pages across allocations, then stores that must not fault.
    {
        Fixture fixture;
        armFresh(fixture);
        const auto base = fixture.Address();
        Require(GuestWtFaultOpen_nid_no_patch(base, Bytes) == Bytes / Page, "an open across allocations did not open every armed page");
        requireConsistent(base, "after an open");
        const auto faults = counters().faults;
        storeEveryPage(fixture, 0x53, "after stores into opened pages");
        Require(counters().faults == faults, "a store into a page an open restored faulted");
    }

    const auto end = counters();
    Require(end.openRefused == start.openRefused, "an open refused pages of live guest memory");
    std::cout << "wt-fault host copy: faults " << end.faults - start.faults << " split opens " << end.openSplitRuns - start.openSplitRuns << " regions " << end.openSplitPieces - start.openSplitPieces << " refused " << end.openRefused - start.openRefused << " excluded " << end.excluded - start.excluded << "\n";
}
#endif

#ifdef _WIN32
// APS5_ARCHB_WT_FAULT_CONSUMERS (+ _VERIFY from the environment): collects answer from the fault
// states. A fully armed range collects without a single GetWriteWatch query and reads as unchanged; a
// written (open) page costs one walk of its own run and reads as changed once; protected again at a
// flip (which walks the runs it armed), only a store made while open reads as changed and the collect
// after makes no OS query again; committed-range answers follow; a range with a page the tracker does not track (a
// shared view) walks write watching as before; VERIFY finds no store the fault states missed and
// counts one for a page unprotected behind the tracker's back.
void CheckWtFaultConsumers() {
    using namespace GuestArena;
    using AgcDriver::ArchB::Feature;
    using AgcDriver::ArchB::Switches;
    Require(Switches::Enabled(Feature::WtFaultConsumers) && WtFault().active, "the consumers switch does not follow the environment");
    AgcDriver::ArchB::Recorder::Boundary(true, 0, AgcDriver::ArchB::Recorder::Clock::now());
    const bool verify = Switches::Enabled(Feature::WtFaultVerify);
    const auto queries = [] { return GuestArenaWriteWatchCensus_nid_no_patch().queries; };
    constexpr std::uint64_t Page = 4096;
    PartlyCommitted range;
    range.Commit(0);
    range.Commit(1);
    const auto base = range.Address();
    volatile std::uint8_t* data = range.Data();
    std::memset(range.Data(), 0x11, 2 * Block);
    const auto collect = [&] {
        BumpCollectEpoch();
        return CollectWrites(base, 2 * Block);
    };
    const auto start = WtFault();
    collect();
    WtFaultFlip();
    const auto g1 = collect();
    // A fully armed range: no OS query, unchanged.
    auto before = queries();
    const auto g2 = collect();
    Require(g2 > g1 && UnchangedSince(base, 2 * Block, g1), "a fully armed range read as changed");
    if (!verify) Require(queries() == before, "a fully armed range was collected with write-watch queries");
    // A written page: one walk of its own run, changed once.
    data[4 * Page] = 0x21;
    before = queries();
    const auto g3 = collect();
    Require(!UnchangedSince(base, 2 * Block, g2) && !UnchangedSince(base + 4 * Page, Page, g2) && UnchangedSince(base + 5 * Page, Page, g2), "the written page did not read as changed, or its neighbour did");
    if (!verify) Require(queries() == before + 1, "the open page was not walked by itself");
    const auto g4 = collect();
    Require(UnchangedSince(base, 2 * Block, g3), "an open page that was not written again read as changed");
    // Protected again at the flip: a store made while open reads as changed at the next collect; a page
    // not stored since its last walk reads as unchanged (the flip walks the runs it armed).
    data[6 * Page] = 0x22;
    WtFaultFlip();
    const auto g5 = collect();
    Require(!UnchangedSince(base + 6 * Page, Page, g4) && UnchangedSince(base + 5 * Page, Page, g4), "a store made while open was missed after the flip, or an untouched page read as changed");
    Require(UnchangedSince(base + 4 * Page, Page, g4), "a page not stored since its last walk read as changed after the flip's walk");
    before = queries();
    const auto g6 = collect();
    Require(UnchangedSince(base, 2 * Block, g5), "a protected page read as changed twice");
    if (!verify) Require(queries() == before, "a range protected again was collected with write-watch queries");
    // Committed-range answers.
    Runs runs;
    const auto committed = CollectCommitted(range, runs);
    Require(committed > g6 && UnchangedCommittedSince(runs, committed), "a committed range read as changed right after its collect");
    data[Block + 8] = 0x31;
    const auto after = CollectCommitted(range, runs);
    Require(!UnchangedCommittedSince(runs, committed) && UnchangedCommittedSince(runs, after), "a store into the committed range was missed or kept reading as changed");
    // Shared views (protected by the shared-mapping tracker, which forgets them here) are walked by
    // write watching, whole or as open runs.
    const auto fallback = WtFault().consumerFallback;
    {
        constexpr std::size_t Bytes = Block;
        auto* memory = static_cast<std::uint8_t*>(GuestArenaAllocate_nid_postfix(2 * Bytes, Block));
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, Bytes, nullptr);
        Require(memory != nullptr && section != nullptr, "cannot create the shared fixture");
        GuestArenaForgetSection_nid_postfix(section);
        GuestArenaMapDirect_nid_postfix(memory, Bytes, section, 0, PAGE_READWRITE);
        GuestArenaMapDirect_nid_postfix(memory + Bytes, Bytes, section, 0, PAGE_READWRITE);
        const auto view = reinterpret_cast<std::uint64_t>(memory);
        BumpCollectEpoch();
        CollectWrites(view, Bytes);
        WtFaultFlip();
        BumpCollectEpoch();
        const auto viewGeneration = CollectWrites(view, Bytes);
        memory[64] = 0x41;
        BumpCollectEpoch();
        CollectWrites(view, Bytes);
        Require(!UnchangedSince(view, Bytes, viewGeneration), "a store into a range of shared views was missed");
        static_cast<void>(fallback);
        GuestArenaReset_nid_postfix(memory, 2 * Bytes);
        GuestArenaRelease_nid_postfix(memory, 2 * Bytes);
        GuestArenaForgetSection_nid_postfix(section);
        CloseHandle(section);
    }
    if (verify) {
        Require(WtFault().consumerMismatches == start.consumerMismatches, "VERIFY counted a store the fault states saw as missed");
        WtFaultFlip();
        collect();
        DWORD previous = 0;
        Require(VirtualProtect(reinterpret_cast<void*>(base + 7 * Page), Page, PAGE_READWRITE, &previous) && previous == PAGE_READONLY, "cannot unprotect a page behind the tracker");
        data[7 * Page] = 0x51;
        const auto g7 = collect();
        Require(WtFault().consumerMismatches == start.consumerMismatches + 1, "VERIFY did not count the store the fault states missed");
        Require(g7 != 0 && !UnchangedSince(base + 7 * Page, Page, g6), "VERIFY did not stamp the missed store");
        GuestWtFaultForget_nid_no_patch(base + 7 * Page, Page);
    }
    if (Switches::Enabled(Feature::WtSyncSkip) && !verify) {
        std::map<std::string, double> totals;
        for (const auto& [name, value] : AgcDriver::ArchB::Recorder::Collect(AgcDriver::ArchB::Recorder::Clock::now()).counters) totals[name] += value;
        const char* skipVerify = std::getenv("APS5_ARCHB_WT_SYNC_SKIP_VERIFY");
        const bool verifying = skipVerify != nullptr && std::string_view(skipVerify) == "1";
        std::cout << "wt-sync-skip: skips " << totals["wt_sync_skips"] << " verified " << totals["wt_sync_skip_verified"] << " refused " << totals["wt_sync_skip_refused"] << " mismatches " << totals["wt_sync_skip_mismatches"] << "\n";
        Require(totals[verifying ? "wt_sync_skip_verified" : "wt_sync_skips"] >= 2, "unchanged armed ranges were walked again instead of skipped");
        Require(totals["wt_sync_skip_refused"] >= 1, "a range with a store since its last walk was not refused");
        Require(totals["wt_sync_skip_mismatches"] == 0, "a skipped walk would have found writes");
    }
    const auto end = WtFault();
    std::cout << "wt-fault consumers: unchanged " << end.consumerUnchanged - start.consumerUnchanged << " changed " << end.consumerChanged - start.consumerChanged << " fallback " << end.consumerFallback - start.consumerFallback << " mismatches " << end.consumerMismatches - start.consumerMismatches << "\n";
}
#endif

#ifdef _WIN32
void CheckArchcSwitches() {
    using AgcDriver::ArchB::Feature;
    using AgcDriver::ArchB::Switches;
    const AgcDriver::ArchB::SwitchInfo* epochs = nullptr;
    const AgcDriver::ArchB::SwitchInfo* fastpath = nullptr;
    for (const auto& entry : Switches::Table()) {
        if (entry.feature == Feature::ArchcImageEpochs) epochs = &entry;
        if (entry.feature == Feature::ArchcTargetFastpath) fastpath = &entry;
    }
    Require(epochs != nullptr && fastpath != nullptr, "the Arch C switches are not in the table");
    Require(std::string_view(epochs->name) == "C_IMAGE_EPOCHS" && std::string_view(fastpath->name) == "C_TARGET_FASTPATH", "the Arch C switch names changed");
    Require(!epochs->defaultOn && !fastpath->defaultOn && epochs->runtime && fastpath->runtime, "the Arch C switches are not default-off runtime switches");
    Require(epochs->prerequisites.size() == 2 && epochs->prerequisites[0] == Feature::WtFault && epochs->prerequisites[1] == Feature::WtFaultConsumers, "C_IMAGE_EPOCHS does not require WT_FAULT and WT_FAULT_CONSUMERS");
    Require(fastpath->prerequisites.size() == 1 && fastpath->prerequisites[0] == Feature::ArchcImageEpochs, "C_TARGET_FASTPATH does not require C_IMAGE_EPOCHS");
    const char* fast = std::getenv("APS5_ARCHB_C_TARGET_FASTPATH");
    const char* image = std::getenv("APS5_ARCHB_C_IMAGE_EPOCHS");
    if (fast != nullptr && std::string_view(fast) == "1" && (image == nullptr || std::string_view(image) != "1")) Require(!Switches::Enabled(Feature::ArchcTargetFastpath), "C_TARGET_FASTPATH stayed on without C_IMAGE_EPOCHS");
    std::cout << "agc_archc_switches: ok\n";
}

void CheckArchcLog() {
    using namespace GuestArena;
    Require(WtFault().active, "the fault tracker did not turn on with write watching");
    const auto state = [](std::uint64_t address) { return GuestWtFaultWord_nid_no_patch(address) & WtFaultStateMask; };
    constexpr std::uint64_t Page = 4096;
    PartlyCommitted range;
    range.Commit(0);
    const auto base = range.Address();
    volatile std::uint8_t* data = range.Data();
    std::memset(range.Data(), 0x11, Block);
    BumpCollectEpoch();
    CollectWrites(base, Block);
    WtFaultFlip();
    WtFaultFlip();
    Require(state(base) == WtFaultArmed && state(base + Page) == WtFaultArmed, "the log fixture was not armed");
    std::array<std::uint64_t, 16> entries{};
    bool lost = false;

    auto cursor = GuestWtFaultLogHead_nid_no_patch();
    data[0x40] = 0x22;
    data[0x80] = 0x23;
    Require(GuestWtFaultLogHead_nid_no_patch() == cursor + 1, "one store into an armed page did not append exactly one log entry");
    auto count = GuestWtFaultLogRead_nid_no_patch(&cursor, entries.data(), entries.size(), &lost);
    Require(count == 1 && !lost && entries[0] == (base | WtFaultLogStore), "the log entry is not a Store of the faulted page");
    Require(GuestWtFaultLogRead_nid_no_patch(&cursor, entries.data(), entries.size(), &lost) == 0 && !lost, "a drained log read again");
    std::cout << "agc_archc_log_append_on_fault: ok\n";

    GuestWtFaultForget_nid_no_patch(base + Page, Page);
    count = GuestWtFaultLogRead_nid_no_patch(&cursor, entries.data(), entries.size(), &lost);
    Require(count == 2 && entries[0] == ((base + Page) | WtFaultLogForgetBegin) && entries[1] == ((base + 2 * Page) | WtFaultLogForgetEnd), "a forget of a tracked page was not logged as a begin/end pair");
    GuestWtFaultForget_nid_no_patch(base + Page, Page);
    Require(GuestWtFaultLogHead_nid_no_patch() == cursor, "a forget of an untracked page was logged");
    std::cout << "agc_archc_forget_logged: ok\n";

    {
        const GuestArena::HostWrite scope(reinterpret_cast<void*>(base + 2 * Page), Page);
        count = GuestWtFaultLogRead_nid_no_patch(&cursor, entries.data(), entries.size(), &lost);
        Require(count == 2 && entries[0] == ((base + 2 * Page) | WtFaultLogHold) && entries[1] == ((base + 3 * Page) | WtFaultLogForgetEnd), "a hold of an armed page was not logged as a run");
    }

    const auto start = GuestWtFaultLogHead_nid_no_patch();
    auto stale = start;
    for (std::size_t i = 0; i <= WtFaultLogSlots; ++i) GuestWtFaultLogAppendForTest_nid_no_patch(base | WtFaultLogStore);
    count = GuestWtFaultLogRead_nid_no_patch(&stale, entries.data(), entries.size(), &lost);
    Require(lost && count == 0 && stale == GuestWtFaultLogHead_nid_no_patch(), "a reader 65,537 entries behind was not told it lost entries");
    std::cout << "agc_archc_log_overflow_lost: ok\n";

    const auto cold = base + 4 * Page;
    const auto hot = base + 5 * Page;
    GuestWtFaultSetCold_nid_no_patch(cold, Page, true);
    data[4 * Page] = 1;
    data[5 * Page] = 1;
    Require(state(cold) == WtFaultOpen && state(hot) == WtFaultOpen, "the cold fixture stores did not open their pages");
    GuestWtFaultNoteDriverStore_nid_no_patch(cold, Page);
    GuestWtFaultNoteStored_nid_no_patch(cold, Page);
    GuestWtFaultNoteDriverStore_nid_no_patch(hot, Page);
    WtFaultCounters before{};
    GuestWtFaultCounters_nid_no_patch(&before);
    WtFaultFlip();
    WtFaultCounters after{};
    GuestWtFaultCounters_nid_no_patch(&after);
    Require(state(cold) == WtFaultArmed, "a stored cold page was kept open by its hot marks");
    Require(state(hot) == WtFaultOpen, "a driver-stored page that is not cold was armed");
    Require(after.coldRearmed > before.coldRearmed, "the cold re-arm was not counted");
    cursor = GuestWtFaultLogHead_nid_no_patch();
    data[4 * Page + 8] = 2;
    Require(GuestWtFaultLogHead_nid_no_patch() == cursor + 1, "the next store into a re-armed cold page did not fault into the log");
    GuestWtFaultSetCold_nid_no_patch(cold, Page, false);
    std::cout << "agc_archc_cold_page_rearmed: ok\n";
}

void CheckArchcWatch() {
    using namespace GuestArena;
    using namespace AgcDriver::Graphics::ArchC;
    Require(WtFault().active, "the fault tracker did not turn on with write watching");
    const auto state = [](std::uint64_t address) { return GuestWtFaultWord_nid_no_patch(address) & WtFaultStateMask; };
    const auto protection = [](std::uint64_t address) {
        MEMORY_BASIC_INFORMATION memory{};
        VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory));
        return memory.Protect;
    };
    constexpr std::uint64_t Page = 4096;
    PartlyCommitted range;
    range.Commit(0);
    const auto base = range.Address();
    volatile std::uint8_t* data = range.Data();
    std::memset(range.Data(), 0x11, Block);
    BumpCollectEpoch();
    CollectWrites(base, Block);
    WtFaultFlip();
    WtFaultFlip();
    Require(state(base) == WtFaultArmed && state(base + 4 * Page) == WtFaultArmed, "the watch fixture was not armed");
    std::array<std::uint64_t, 16> entries{};
    std::array<std::uint64_t, 16> stamps{};
    bool lost = false;
    auto cursor = GuestWtFaultWatchLogHead_nid_no_patch();

    Require(GuestWtFaultWatch_nid_no_patch(base, 2 * Page) == 2, "two armed pages were not watched");
    Require(protection(base) == PAGE_NOACCESS && state(base) == WtFaultArmed, "a watched armed page is not PAGE_NOACCESS and armed");
    Require(GuestWtFaultGuestProtection_nid_no_patch(base, Page, PAGE_NOACCESS) == PAGE_READWRITE, "a probe of a watched page does not read the guest protection");
    Require(data[0x10] == 0x11, "a watched page read the wrong byte");
    auto count = GuestWtFaultWatchLogRead_nid_no_patch(&cursor, entries.data(), stamps.data(), entries.size(), &lost);
    Require(count == 1 && !lost && (entries[0] & ~WtWatchDriver) == (base | WtWatchRead) && stamps[0] != 0, "a read of a watched page was not logged once as a read");
    Require(state(base) == WtFaultArmed && protection(base) == PAGE_READONLY, "a read of a watched page did not leave it armed and read-only");
    const auto storeCursor = GuestWtFaultLogHead_nid_no_patch();
    data[Page + 8] = 0x33;
    count = GuestWtFaultWatchLogRead_nid_no_patch(&cursor, entries.data(), stamps.data(), entries.size(), &lost);
    Require(count == 1 && (entries[0] & ~WtWatchDriver) == ((base + Page) | WtWatchWrite), "a store into a watched armed page was not logged as a write");
    Require(GuestWtFaultLogHead_nid_no_patch() == storeCursor + 1 && state(base + Page) == WtFaultOpen && data[Page + 8] == 0x33, "a store into a watched armed page did not open it through the write tracker");
    std::cout << "agc_archc_watch_read_write: ok\n";

    Require(GuestWtFaultWatch_nid_no_patch(base + Page, Page) == 1 && protection(base + Page) == PAGE_NOACCESS, "an open page was not watched");
    WtFaultFlip();
    Require(state(base + Page) == WtFaultOpen && protection(base + Page) == PAGE_NOACCESS, "an arm took a watched open page");
    data[Page + 16] = 0x44;
    count = GuestWtFaultWatchLogRead_nid_no_patch(&cursor, entries.data(), stamps.data(), entries.size(), &lost);
    Require(count == 1 && (entries[0] & ~WtWatchDriver) == ((base + Page) | WtWatchWrite) && state(base + Page) == WtFaultOpen && data[Page + 16] == 0x44, "a store into a watched open page was not logged or did not land");
    std::cout << "agc_archc_watch_open_page: ok\n";

    BumpCollectEpoch();
    CollectWrites(base, Block);
    Require(state(base + Page) == WtFaultOpen, "the lazy arm fixture needs a clean open page");
    Require(GuestWtFaultWatch_nid_no_patch(base + Page, Page) == 1 && protection(base + Page) == PAGE_NOACCESS, "the clean open page was not watched");
    GuestWtFaultSetLazyPreservation_nid_no_patch(+[](std::uintptr_t, std::size_t) { return false; });
    WtFaultFlip();
    WtFaultFlip();
    Require(state(base + Page) == WtFaultArmed && protection(base + Page) == PAGE_NOACCESS, "with lazy preservation an arm did not take a clean watched open page, or it changed the watch protection");
    const auto armedClock = GuestWtFaultClock_nid_no_patch();
    Require(GuestWtFaultChangedSince_nid_no_patch(base + Page, Page, armedClock) == WtFaultAnswer::Unchanged, "an armed watched page does not answer unchanged");
    Require(data[Page + 16] == 0x44, "an armed watched page read the wrong byte");
    Require(state(base + Page) == WtFaultArmed && protection(base + Page) == PAGE_READONLY, "a read of an armed watched page did not leave it armed and read-only");
    const auto armedCursor = GuestWtFaultLogHead_nid_no_patch();
    data[Page + 24] = 0x66;
    Require(GuestWtFaultLogHead_nid_no_patch() == armedCursor + 1 && state(base + Page) == WtFaultOpen && data[Page + 24] == 0x66, "a store into a page armed under a watch did not fault into the log");
    Require(GuestWtFaultChangedSince_nid_no_patch(base + Page, Page, armedClock) == WtFaultAnswer::Changed, "a store into a page armed under a watch was not reported as a change");
    GuestWtFaultSetLazyPreservation_nid_no_patch(nullptr);
    cursor = GuestWtFaultWatchLogHead_nid_no_patch();
    std::cout << "agc_archc_watch_open_page_armed_with_lazy: ok\n";

    Require(GuestWtFaultWatch_nid_no_patch(base + 2 * Page, 2 * Page) == 2, "the hold fixture was not watched");
    {
        const GuestArena::HostWrite scope(reinterpret_cast<void*>(base + 2 * Page), Page);
        data[2 * Page] = 0x55;
    }
    count = GuestWtFaultWatchLogRead_nid_no_patch(&cursor, entries.data(), stamps.data(), entries.size(), &lost);
    Require(count == 1 && (entries[0] & ~WtWatchDriver) == ((base + 2 * Page) | WtWatchHold), "a hold over a watched page was not logged as an OS access");
    Require(GuestWtFaultUnwatch_nid_no_patch(base + 3 * Page, Page) == 1 && protection(base + 3 * Page) == PAGE_READONLY && state(base + 3 * Page) == WtFaultArmed, "an unwatch did not restore the armed protection");
    Require(GuestWtFaultWatchLogHead_nid_no_patch() == cursor, "an unwatch was logged");
    Require(data[3 * Page] == 0x11, "an unwatched page read the wrong byte");
    Require(GuestWtFaultWatchLogHead_nid_no_patch() == cursor, "a read after an unwatch faulted into the log");
    std::cout << "agc_archc_watch_hold_unwatch: ok\n";

    // APS5_ARCHB_C_WATCH_SPLIT: live direct memory is committed in 16 KiB allocations (one VirtualAlloc2
    // per PS5 page), and one VirtualProtect cannot span two allocations.
    {
        const auto split = base + 2 * Block;
        GuestArenaCommit_nid_postfix(range.Data() + 2 * Block, Block, PAGE_READWRITE, 0x4000);
        std::memset(range.Data() + 2 * Block, 0x22, Block);
        BumpCollectEpoch();
        CollectWrites(split, Block);
        WtFaultFlip();
        WtFaultFlip();
        Require(state(split + 2 * Page) == WtFaultArmed && state(split + 5 * Page) == WtFaultArmed, "the 16 KiB-allocation fixture was not armed");
        std::array<std::uint64_t, WtWatchSkipCauses> causes{};
        GuestWtFaultSetWatchSplit_nid_no_patch(false);
        Require(GuestWtFaultWatchCauses_nid_no_patch(split + 2 * Page, 4 * Page, causes.data()) == 0 && causes[WtWatchSkipProtect] == 4, "a watch across two allocations did not fail whole as a protect refusal without the split");
        Require(protection(split + 2 * Page) == PAGE_READONLY && protection(split + 4 * Page) == PAGE_READONLY && GuestWtFaultWatchedPages_nid_no_patch(split, Block) == 0, "a refused watch left a page watched or protected");
        GuestWtFaultSetWatchSplit_nid_no_patch(true);
        causes = {};
        Require(GuestWtFaultWatchCauses_nid_no_patch(split + 2 * Page, 4 * Page, causes.data()) == 4 && causes[WtWatchSkipProtect] == 0, "a split watch across two allocations did not watch every page");
        Require(protection(split + 2 * Page) == PAGE_NOACCESS && protection(split + 5 * Page) == PAGE_NOACCESS && protection(split + 6 * Page) == PAGE_READONLY, "a split watch did not protect exactly its pages");
        Require(GuestWtFaultUnwatch_nid_no_patch(split + 2 * Page, 4 * Page) == 4 && protection(split + 2 * Page) == PAGE_READONLY && protection(split + 5 * Page) == PAGE_READONLY, "an unwatch across two allocations did not restore both");
        Require(GuestWtFaultWatchCauses_nid_no_patch(split + 2 * Page, 4 * Page, causes.data()) == 4, "the split range could not be watched again");
        auto splitCursor = GuestWtFaultWatchLogHead_nid_no_patch();
        Require(data[2 * Block + 5 * Page + 1] == 0x22, "a split-watched page read the wrong byte");
        count = GuestWtFaultWatchLogRead_nid_no_patch(&splitCursor, entries.data(), stamps.data(), entries.size(), &lost);
        Require(count == 1 && (entries[0] & ~WtWatchDriver) == ((split + 5 * Page) | WtWatchRead) && protection(split + 5 * Page) == PAGE_READONLY && protection(split + 2 * Page) == PAGE_NOACCESS, "a read of one split-watched page was not logged or unwatched only it");
        Require(GuestWtFaultUnwatch_nid_no_patch(split, Block) == 3 && protection(split + 2 * Page) == PAGE_READONLY && protection(split + 4 * Page) == PAGE_READONLY, "the rest of the split range did not unwatch");
        cursor = GuestWtFaultWatchLogHead_nid_no_patch();
        GuestWtFaultSetWatchSplit_nid_no_patch(false);
        std::cout << "agc_archc_watch_split: ok\n";
    }

    if (!LazyCensus::Enabled()) return;
    LazyCensus::ResetForTest();
    const auto copyBack = base + 8 * Page;
    const auto twice = base + 10 * Page;
    const auto aged = base + 12 * Page;
    WtFaultFlip();
    LazyCensus::Published(copyBack, copyBack + 2 * Page, LazyCensus::Kind::StagingCopyBack, 30);
    LazyCensus::Published(twice, twice + Page, LazyCensus::Kind::WriteBack, 7);
    {
        const LazyCensus::UseScope scope;
        LazyCensus::Published(aged, aged + Page, LazyCensus::Kind::ShadowPublish, 7);
        LazyCensus::UsedByGpu(aged, aged + Page, LazyCensus::GpuUse::Write);
    }
    Require(data[8 * Page + Page + 4] == 0x11, "a published page read the wrong byte");
    LazyCensus::UsedByGpu(copyBack, copyBack + Page, LazyCensus::GpuUse::StagingIn);
    LazyCensus::Published(twice, twice + Page, LazyCensus::Kind::WriteBack, 7);
    LazyCensus::UsedByGpu(twice, twice + Page, LazyCensus::GpuUse::StagingIn);
    LazyCensus::Flip();
    LazyCensus::Flip();
    LazyCensus::Flip();
    const auto totals = LazyCensus::Snapshot();
    const auto at = [&](LazyCensus::Kind kind, LazyCensus::Outcome outcome) { return totals.count[static_cast<std::size_t>(kind)][static_cast<std::size_t>(outcome)]; };
    Require(at(LazyCensus::Kind::StagingCopyBack, LazyCensus::Outcome::GuestRead) + at(LazyCensus::Kind::StagingCopyBack, LazyCensus::Outcome::DriverRead) == 1 && at(LazyCensus::Kind::StagingCopyBack, LazyCensus::Outcome::StagingIn) == 0, "a guest read of a copy-back was not its first touch");
    Require(at(LazyCensus::Kind::WriteBack, LazyCensus::Outcome::Republished) == 1 && at(LazyCensus::Kind::WriteBack, LazyCensus::Outcome::StagingIn) == 1, "a write-back published again and then copied in was not counted as such");
    Require(at(LazyCensus::Kind::ShadowPublish, LazyCensus::Outcome::Expired) == 1, "a publish untouched for three flips (its own use's write excluded) did not expire");
    Require(totals.faults == 1 && protection(copyBack) == PAGE_READONLY, "the census did not count one fault and unwatch the range");
    std::cout << "agc_archc_lazy_census: ok\n";
}

void CheckArchcEpochs() {
    using namespace GuestArena;
    using AgcDriver::Graphics::ArchC::EpochRange;
    using AgcDriver::Graphics::ArchC::EpochSource;
    using AgcDriver::Graphics::ArchC::ImageEpochs;
    Require(WtFault().active, "the fault tracker did not turn on with write watching");
    Require(ImageEpochs::Active(), "IMAGE_EPOCHS is not on");
    constexpr std::uint64_t Page = 4096;
    PartlyCommitted range;
    range.Commit(0);
    range.Commit(1);
    const auto base = range.Address();
    volatile std::uint8_t* data = range.Data();
    std::memset(range.Data(), 0x11, 2 * Block);
    auto& index = ImageEpochs::Get();
    std::atomic<std::uint64_t> first{1};
    std::atomic<std::uint64_t> second{1};
    char ownerA = 0;
    char ownerB = 0;
    const std::array rangesA{EpochRange{base, base + Block}};
    const std::array rangesB{EpochRange{base + Block / 2, base + Block + Page}};
    index.RegisterOwner(&ownerA, first, rangesA);
    index.RegisterOwner(&ownerB, second, rangesB);
    BumpCollectEpoch();
    CollectWrites(base, 2 * Block);
    WtFaultFlip();
    WtFaultFlip();
    Require(index.DrainIfNew(), "the log did not drain");

    auto a = first.load();
    auto b = second.load();
    data[Page] = 0x33;
    Require(first.load() == a, "a CPU store bumped the epoch before the drain");
    Require(index.DrainIfNew() && first.load() > a && second.load() == b, "a drained CPU store did not bump exactly the owner whose range it hit");
    std::cout << "agc_archc_epochs_cpu_store_invalidates: ok\n";

    a = first.load();
    b = second.load();
    MarkWritten(base + Block + 64, 64);
    Require(first.load() == a && second.load() > b, "a tracker stamp did not bump exactly the owner whose block it stamped");
    std::cout << "agc_archc_epochs_tracker_stamp_invalidates: ok\n";

    a = first.load();
    b = second.load();
    ImageEpochs::Note(base + Block / 2, base + Block / 2 + 64, EpochSource::GpuNote, &ownerA);
    Require(first.load() == a && second.load() > b, "a GPU write note bumped its writer or missed another owner");
    std::cout << "agc_archc_epochs_gpu_note_except_writer: ok\n";

    a = first.load();
    b = second.load();
    ImageEpochs::Note(base, base + Block, EpochSource::Pending, &ownerA);
    Require(first.load() == a && second.load() > b, "a pending transition bumped the image itself or missed an overlapping one");
    std::cout << "agc_archc_epochs_pending_transition_except_self: ok\n";

    a = first.load();
    b = second.load();
    ImageEpochs::Note(base + Page, base + 2 * Page, EpochSource::Depth, nullptr);
    Require(first.load() > a && second.load() == b, "a depth-surface event did not bump exactly the overlapping owner");
    std::cout << "agc_archc_epochs_depth_surface_invalidates: ok\n";

    index.UnregisterOwner(&ownerA);
    a = first.load();
    b = second.load();
    ImageEpochs::Note(base, base + 2 * Block, EpochSource::Depth, nullptr);
    Require(first.load() == a && second.load() > b, "an unregistered owner was still bumped");
    index.UnregisterOwner(&ownerB);
    std::cout << "agc_archc_epochs_unregister_on_destroy: ok\n";
}

// APS5_ARCHB_WT_FAULT hot pages (APS5_WT_FAULT_GUEST_HOT, default on): a page that faulted stays open
// across the next flip; a page a walk finds stored while open stays open across the flip after; a page
// neither faulted nor found stored since the last flip is protected again, and its next store faults.
void CheckWtFaultHot() {
    using namespace GuestArena;
    Require(WtFault().active, "the fault tracker did not turn on with write watching");
    const auto state = [](std::uint64_t address) { return GuestWtFaultWord_nid_no_patch(address) & WtFaultStateMask; };
    const auto faults = [] {
        WtFaultCounters value{};
        GuestWtFaultCounters_nid_no_patch(&value);
        return value.faults;
    };
    constexpr std::uint64_t Page = 4096;
    PartlyCommitted range;
    range.Commit(0);
    const auto base = range.Address();
    volatile std::uint8_t* data = range.Data();
    std::memset(range.Data(), 0x11, Block);
    const auto collect = [&] {
        BumpCollectEpoch();
        return CollectWrites(base, Block);
    };
    collect();
    WtFaultFlip();
    Require(state(base) == WtFaultOpen, "pages the walk found stored (the fixture's fill) were protected at the next flip");
    WtFaultFlip();
    Require(state(base) == WtFaultArmed && state(base + Page) == WtFaultArmed, "the hot-page fixture was not protected");
    const auto before = faults();
    data[0] = 1;
    data[Page] = 1;
    Require(faults() == before + 2, "the first stores did not fault");
    collect();
    WtFaultFlip();
    Require(state(base) == WtFaultOpen && state(base + Page) == WtFaultOpen, "a page that faulted was protected at the next flip");
    data[0] = 2;
    collect();
    WtFaultFlip();
    Require(state(base) == WtFaultOpen, "a page found stored while open was protected at the flip");
    Require(state(base + Page) == WtFaultArmed, "a page not stored since the last flip stayed open");
    data[0] = 3;
    Require(faults() == before + 2, "a store into a hot page faulted");
    data[Page] = 3;
    Require(faults() == before + 3 && state(base + Page) == WtFaultOpen, "a store into a page protected again did not fault");
}

// APS5_ARCHB_WT_FAULT_DIRTY_ARM: an open page that write watching reports stored since its last walk stays
// open across the flip, and the flip walks it (resetting, stamped), so a page not stored again before the
// next flip is protected then: the open set holds only pages stored between flips.
void CheckWtFaultDirtyArm() {
    using namespace GuestArena;
    using AgcDriver::ArchB::Feature;
    using AgcDriver::ArchB::Switches;
    Require(WtFault().active && Switches::Enabled(Feature::WtFaultDirtyArm), "the dirty-arm switch does not follow the environment");
    const auto state = [](std::uint64_t address) { return GuestWtFaultWord_nid_no_patch(address) & WtFaultStateMask; };
    const auto counters = [] {
        WtFaultCounters value{};
        GuestWtFaultCounters_nid_no_patch(&value);
        return value;
    };
    constexpr std::uint64_t Page = 4096;
    PartlyCommitted range;
    range.Commit(0);
    const auto base = range.Address();
    volatile std::uint8_t* data = range.Data();
    std::memset(range.Data(), 0x11, Block);
    BumpCollectEpoch();
    CollectWrites(base, Block);
    WtFaultFlip();
    WtFaultFlip();
    Require(state(base) == WtFaultArmed && state(base + Page) == WtFaultArmed, "the dirty-arm fixture was not protected");
    data[0] = 1;
    data[Page] = 1;
    WtFaultFlip();
    Require(state(base) == WtFaultOpen && state(base + Page) == WtFaultOpen, "a page that faulted was protected at the next flip");
    BumpCollectEpoch();
    CollectWrites(base, Block);
    const auto since = GuestWtFaultClockBump_nid_no_patch();
    data[0] = 2;
    WtFaultFlip();
    Require(state(base) == WtFaultOpen && state(base + Page) == WtFaultOpen, "a page the walk found stored was protected at the next flip");
    const auto before = counters();
    WtFaultFlip();
    const auto after = counters();
    Require(state(base) == WtFaultOpen && after.keptDirty == before.keptDirty + 1, "a page stored since its last walk was not kept open by the dirty arm");
    Require(state(base + Page) == WtFaultArmed, "a page stored before its last walk only was kept open");
    Require(!UnchangedSince(base, Page, since), "the flip's walk of the kept page did not stamp its store");
    WtFaultFlip();
    Require(state(base) == WtFaultArmed && counters().keptDirty == after.keptDirty, "a kept page not stored again stayed open (its dirty bit was not consumed)");
    const auto faulted = counters().faults;
    data[0] = 3;
    Require(counters().faults == faulted + 1 && state(base) == WtFaultOpen, "a store into the page protected again did not fault");
}
#endif

// APS5_ARCHB_FIX_UNSTAMPED_ALIAS (mode from the environment): a driver store into unwatched memory 4 GiB
// away from a collected committed range sets the unstamped-store slots of the range's blocks (hashed
// by block modulo 4 GiB). Off, the range then reads as changed (counted as aliased); on, it reads as
// unchanged (counted as served), while a real store into the range (whole or partly watched) and a
// collect of a CPU write still read as changed, and the split names the aliasing store.
void CheckUnstampedAlias() {
    const char* value = std::getenv("APS5_ARCHB_FIX_UNSTAMPED_ALIAS");
    const bool on = value != nullptr && std::string_view(value) == "1";
    Require(AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixUnstampedAlias) == on, "the unstamped alias switch does not follow the environment");
    PartlyCommitted range;
    const auto base = range.Address();
    range.Commit(0);
    range.Commit(1);
    std::memset(range.Data(), 0x11, 2 * Block);
    Runs runs;
    const Runs committed{{base, base + 2 * Block}};
    const auto synced = CollectCommitted(range, runs);
    Require(synced != 0 && runs == committed && UnchangedCommittedSince(runs, synced), "the committed fixture was not collected");

    // An unwatched address that shares the slot of the range's second block.
    constexpr std::uint64_t Period = std::uint64_t{1} << 32u;
    const auto target = base + Block + 0x40;
    std::uint64_t alias = 0;
    for (const auto candidate : {target + Period, target + 2 * Period, target + 3 * Period, target >= Period ? target - Period : target + 4 * Period}) {
        if (!Watched(candidate, 16)) {
            alias = candidate;
            break;
        }
    }
    Require(alias != 0, "no unwatched address 4 GiB away from the fixture");
    auto& counts = UnstampedAlias();
    const auto aliased = counts.aliased.load();
    const auto served = counts.served.load();
    Require(MarkWritten(alias, 16) == 0, "a driver store into unwatched memory was stamped as watched");
    const auto split = SplitNewestStamp(base, 2 * Block);
    Require(split.unstamped > synced && split.block <= synced && split.slotAddress == (base + Block) / Block * Block && split.storeBegin == alias && split.storeEnd == alias + 16, "the stamp split does not name the aliasing store");
    Require((NewestStamp(base, 2 * Block) > synced) == !on, on ? "an aliased slot advanced the newest stamp of a watched range" : "the aliasing store did not advance the newest stamp with the fix off");
    Require(UnchangedSince(base, 2 * Block, synced), "the aliasing store changed the whole-range answer, which never reads the slots");
    Require(UnchangedCommittedSince(runs, synced) == on, on ? "an aliased unstamped slot still reads the committed range as changed" : "an aliased unstamped slot no longer reads the committed range as changed with the fix off");
    Require(counts.aliased.load() == aliased + 1 && counts.served.load() == served + (on ? 1 : 0) && counts.mismatches.load() == 0, "the alias counters do not follow the answer");

    // Real changes still read as changed.
    Require(MarkWritten(base + Block + 0x80, 4) != 0 && !UnchangedCommittedSince(runs, synced), "a driver store into the committed range reads as unchanged");
    const auto stored = CollectCommitted(range, runs);
    Require(stored > synced && UnchangedCommittedSince(runs, stored), "a collect after the driver store reads the range as changed");
    Require(MarkWritten(alias, 16) == 0 && UnchangedCommittedSince(runs, stored) == on, "a second aliasing store does not follow the switch");
    range.Data()[Block + 8] = 0x22;
    const auto written = CollectCommitted(range, runs);
    Require(written > stored && !UnchangedCommittedSince(runs, stored) && UnchangedCommittedSince(runs, written), "a CPU write into the committed range was missed");
    // A store over the range's end and the unwatched block after it (partly watched): the watched part
    // is stamped, so the range reads as changed although the store also set its slots.
    Unwatch(range.Address(2), Block);
    const auto beforePartial = CollectCommitted(range, runs);
    Require(MarkWritten(base + 2 * Block - 32, 64) == 0 && !UnchangedCommittedSince(runs, beforePartial), "a partly watched store into the committed range reads as unchanged");
    // StoredAllSince (the HTILE clear check): a complete driver store over the block, then an alias.
    const auto beforeClear = TrackerGeneration();
    Require(MarkWritten(base, Block) != 0 && StoredAllSince_nid_no_patch(base, Block, beforeClear), "a complete driver store over the block is not seen");
    Require(MarkWritten(alias - Block, 16) == 0 && StoredAllSince_nid_no_patch(base, Block, beforeClear) == on, "an aliasing store does not follow the switch in StoredAllSince");
    Require(counts.mismatches.load() == 0, "a newer slot was set by a store into the range");
    range.Decommit(1);
    range.Decommit(0);
}

// APS5_ARCHB_FIX_ALLOC_CHURN: the epoch collect map keeps its nodes across epochs. A kept entry must
// never stand for a walk of a later epoch: a CPU store made before an epoch is always collected in
// it, while the ranges nobody wrote keep their generations, epoch after epoch.
void CheckAllocChurnMemo() {
    using Recorder = AgcDriver::ArchB::Recorder;
    Recorder::Boundary(true, 0, Recorder::Clock::now());
    const AgcDriver::ArchB::AllocChurnScope churn(true);
    constexpr std::size_t ranges = 8;
    for (const std::size_t stride : {1u, 2u}) {
        void* memory = AllocateWatched(stride * ranges * Block);
        const auto base = reinterpret_cast<std::uint64_t>(memory);
        auto* bytes = static_cast<volatile std::uint8_t*>(memory);
        std::memset(memory, 0, stride * ranges * Block);
        BumpCollectEpoch();
        std::vector<std::uint64_t> collected(ranges);
        for (std::size_t i = 0; i < ranges; ++i) {
            collected[i] = CollectWrites(base + i * stride * Block, Block);
            Require(collected[i] != 0, "the churn fixture was not collected");
        }
        static_cast<void>(Recorder::Collect(Recorder::Clock::now()));
        for (std::size_t epoch = 0; epoch < 64; ++epoch) {
            const auto written = epoch % ranges;
            bytes[written * stride * Block + 64 + epoch] = static_cast<std::uint8_t>(epoch + 1);
            BumpCollectEpoch();
            for (std::size_t i = 0; i < ranges; ++i) {
                const auto range = base + i * stride * Block;
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
        const auto nodes = stride == 1 ? std::size_t{1} : ranges;
        Require(reuses != counts.end() && reuses->second >= 64 * nodes, "kept coalesced memo nodes were not reused in place");
        std::cout << "allocation memo stride " << stride << ": " << reuses->second << " node reuses for " << 64 * nodes << " required\n";
    }
}

void CheckFailedCollectMemo() {
    // APS5_ARCHB_FIX_STABLE_FAILED_COLLECTS (+ _VERIFY): a failure is remembered across epochs while
    // the page-state serials hold; every check below holds with it on, off and verified.
    // It builds on the per-epoch failed memo (APS5_NO_FAILED_COLLECT_MEMO and the memo/map kill switches turn it off).
    const bool stable = AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixStableFailedCollects) && std::getenv("APS5_NO_FAILED_COLLECT_MEMO") == nullptr && std::getenv("APS5_NO_COLLECT_MEMO") == nullptr && std::getenv("APS5_NO_EPOCH_COLLECT_MAP") == nullptr && std::getenv("APS5_SHARED_COLLECT_MEMO") == nullptr;
    const char* verifyText = std::getenv("APS5_ARCHB_FIX_STABLE_FAILED_COLLECTS_VERIFY");
    const bool verifying = verifyText != nullptr && std::string_view(verifyText) == "1";
    auto& stableCounts = StableFailedCollects();
    PartlyCommitted range;
    range.Commit(0);
    const auto base = range.Address();
    BumpCollectEpoch();
    for (int repeat = 0; repeat < 3; ++repeat) Require(CollectWrites(base, PartlyCommitted::Bytes) == 0, "a repeated partial collection manufactured a generation");
    {
        const auto hits = stableCounts.hits.load();
        const auto verified = stableCounts.verified.load();
        const auto inserts = stableCounts.inserts.load();
        BumpCollectEpoch();
        Require(CollectWrites(base, PartlyCommitted::Bytes) == 0, "a failed collection succeeded in the next epoch without a page change");
        if (!stable) Require(stableCounts.hits.load() == hits && stableCounts.inserts.load() == 0, "failures were remembered with the switch off");
        else if (verifying) Require(stableCounts.verified.load() == verified + 1 && stableCounts.hits.load() == hits, "a remembered failure was not verified by its walk");
        else Require(inserts != 0 && stableCounts.hits.load() == hits + 1, "a failure was not remembered across epochs");
    }
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
    BumpCollectEpoch();
    Require(CollectWrites(base, PartlyCommitted::Bytes) == 0, "a decommitted range collected in the next epoch");
    range.Commit(1);
    Require(CollectWrites(base, PartlyCommitted::Bytes) != 0, "recommitting pages retained a failed collection");
    Unwatch(base, Block);
    Require(CollectWrites(base, PartlyCommitted::Bytes) == 0, "unwatching pages retained a successful collection");
    BumpCollectEpoch();
    Require(CollectWrites(base, PartlyCommitted::Bytes) == 0, "an unwatched range collected in the next epoch");
#ifdef _WIN32
    const Runs restored{{base, base + Block}};
    Rewatch(restored);
    Require(CollectWrites(base, PartlyCommitted::Bytes) != 0, "rewatching pages retained a failed collection");
#endif
    Require(stableCounts.mismatches.load() == 0, "a remembered failure no longer held (verify)");
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
    const GuestArena::QueryMemoContext context{&UnwatchSerial, &ForgetSerial, true, nullptr};
    struct Result {
        bool collected;
        std::size_t count;
        std::array<void*, 32> pages{};
        bool operator==(const Result&) const = default;
    };
    std::atomic<std::uint64_t> onCalls{0}, offCalls{0}, onQueries{0}, offQueries{0};
    std::atomic<bool> equivalent{true};
    const auto collect = [&](bool enabled = AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixVqMemo), std::uintptr_t address = 0, std::size_t size = Block) {
        const auto read = [&](bool memo) {
            Result result{false, 32};
            const auto queries = GuestArena::GuestArenaWalkCost_nid_no_patch().vqCalls;
            result.collected = memo ?
                GuestArena::GuestArenaCollectWritesMemo_nid_no_patch(address == 0 ? base : address, size, result.pages.data(), &result.count, false, false, &context) :
                GuestArena::GuestArenaCollectWrites_nid_postfix(address == 0 ? base : address, size, result.pages.data(), &result.count, false);
            const auto added = GuestArena::GuestArenaWalkCost_nid_no_patch().vqCalls - queries;
            (memo ? onCalls : offCalls).fetch_add(1, std::memory_order_relaxed);
            (memo ? onQueries : offQueries).fetch_add(added, std::memory_order_relaxed);
            return result;
        };
        const auto result = read(enabled);
        if (result != read(!enabled)) equivalent.store(false, std::memory_order_relaxed);
        return result;
    };
    const auto commit = [&] { GuestArena::GuestArenaCommit_nid_postfix(memory, bytes, PAGE_READWRITE, bytes); };
    const auto baseline = [&] {
        const auto first = collect();
        Require(collect() == first, "unchanged query changed the collected pages");
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
        Require(collect(true, base, 2 * bytes) == collect(false, base, 2 * bytes), "a longer collection changed the query result");
    } else if (mode == L"commit") {
        baseline();
        commit();
        Require(collect().collected, "commit reused a reserved answer");
        baseline();
        GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
        Require(!collect().collected, "decommit reused a committed answer");
        baseline();
        commit();
        Require(collect().collected, "recommit reused a reserved answer");
    } else if (mode == L"protect") {
        commit();
        baseline();
        DWORD previous = 0;
        Require(GuestArena::GuestArenaVirtualProtect_nid_no_patch(memory, 4096, PAGE_READONLY, &previous), "fixture tracked protect failed");
        Require(collect() == collect(false), "tracked protection changed collection output");
        GuestArena::GuestArenaSetProtection_nid_postfix(base, bytes, PAGE_READONLY);
        Require(VirtualProtect(memory, bytes, PAGE_READONLY, &previous), "fixture protect failed");
        Require(collect() == collect(false), "protection change changed collection output");
    } else if (mode == L"map") {
        baseline();
        const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
        Require(section != nullptr, "cannot create query memo section");
        GuestArena::GuestArenaMapDirect_nid_postfix(memory, bytes, section, 0, PAGE_READWRITE);
        Require(collect() == collect(false), "map reused a reserved answer");
        GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
        Require(!collect().collected, "unmap retained committed memory");
        GuestArena::GuestArenaForgetSection_nid_postfix(section);
        CloseHandle(section);
        baseline();
    } else if (mode == L"unwatch") {
        baseline();
        Unwatch(base + bytes, bytes);
        Require(!collect().collected, "unwatch changed reserved collection output");
    } else if (mode == L"forget") {
        static_cast<void>(CommittedRanges(base, bytes));
        baseline();
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(base, bytes);
        collect();
        baseline();
        { GuestAllocations::Mutation mutation; }
        collect();
    } else if (mode == L"coverage") {
        commit();
        baseline();
        GuestArena::GuestArenaPinWritable_nid_postfix(memory, bytes);
        collect();
        GuestArena::GuestArenaUnpinWritable_nid_postfix(memory, bytes);
        collect();
        auto* host = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        Require(host != nullptr, "cannot allocate untracked memory");
        const auto address = reinterpret_cast<std::uintptr_t>(host);
        Require(collect(true, address) == collect(false, address), "untracked query changed collection output");
        Require(collect(true, address) == collect(false, address), "untracked query changed on repetition");
        VirtualFree(host, 0, MEM_RELEASE);
        std::uintptr_t arena = 0;
        std::size_t arenaBytes = 0;
        GuestArena::GuestArenaRange_nid_postfix(&arena, &arenaBytes);
        collect(true, arena + arenaBytes - 4096, 8192);
    } else if (mode == L"threads") {
        baseline();
        std::array<std::thread, 4> workers;
        std::barrier phase(static_cast<std::ptrdiff_t>(workers.size()));
        for (auto& worker : workers) worker = std::thread([&] {
            for (unsigned i = 0; i < 32; ++i) {
                phase.arrive_and_wait();
                collect();
                phase.arrive_and_wait();
                GuestArena::GuestArenaCommit_nid_postfix(memory, bytes, PAGE_READWRITE, bytes);
                phase.arrive_and_wait();
                collect();
                phase.arrive_and_wait();
                GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
            }
            for (unsigned i = 0; i < 32; ++i) {
                Result before{false, 32};
                GuestArena::GuestArenaCollectWritesMemo_nid_no_patch(base, bytes, before.pages.data(), &before.count, false, false, &context);
                GuestArena::GuestArenaCommit_nid_postfix(memory, bytes, PAGE_READWRITE, bytes);
                Result after{false, 32};
                GuestArena::GuestArenaCollectWrites_nid_postfix(base, bytes, after.pages.data(), &after.count, false);
                GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
            }
        });
        for (auto& worker : workers) worker.join();
    } else if (mode == L"toggle") {
        const auto first = baseline();
        Require(collect(false) == first && collect() == first, "on/off/on changed reserved output");
        commit();
        const auto second = collect(false);
        Require(collect() == second, "an off-path commit left a stale on-path answer");
        baseline();
        GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
        BumpCollectEpoch();
        Require(CollectWrites(base, bytes, true) == 0, "prelock collection accepted reserved memory");
        BumpCollectEpoch();
        Require(CollectWrites(base, bytes, false) == 0, "off collection accepted reserved memory");
        BumpCollectEpoch();
        Require(CollectWrites(base, bytes, true) == 0, "toggled collection accepted reserved memory");
    } else throw std::runtime_error("unknown query memo test");
    std::cout << "collection " << std::string(mode.begin(), mode.end()) << ": on " << onCalls.load() << " calls, " << onQueries.load() << " mapping queries; off " << offCalls.load() << " calls, " << offQueries.load() << " mapping queries\n";
    Require(onCalls.load() != 0 && offCalls.load() != 0, "collection did not exercise both query memo switch paths");
    Require(onQueries.load() == 0 && offQueries.load() == 0, "collection queried a mapping with the query memo switch on or off");
    Require(equivalent.load(), "the query memo switch changed collection results");
}
#endif

#ifdef _WIN32
// APS5_ARCHB_PIPE_COLLECT_SHARE (run with APS5_ARCHB_PIPE_Q0_COMMITTER=1 and the switch at 1, or 0 for
// "off"; "verify" also sets APS5_ARCHB_PIPE_COLLECT_SHARE_VERIFY=1). Queue 0 (this thread, marked as
// the producer) walks ranges; the committer (a separate thread per check, marked once) follows an
// ordering point and collects. A committer collect is answered from queue 0's walks only when every
// page of it was walked at or after the item's ordering point, under the same unwatch and mapping
// serials; a write queue 0's walk found stays visible to the committer.
void CheckCollectShare(std::wstring_view mode) {
    using AgcDriver::ArchB::Feature;
    using AgcDriver::ArchB::Switches;
    Switches::Initialize();
    const bool on = mode != L"off";
    const bool verify = mode == L"verify";
    Require(Switches::Enabled(Feature::PipeCollectShare) == on, "the collect-share switch is not as the test mode needs");
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
    SetCollectShareProducer(true);
    auto& counters = CommitterCollects();
    struct Snapshot {
        std::uint64_t walks, covered, mismatches, recorded;
    };
    const auto snapshot = [&] {
        return Snapshot{counters.walks.load(), counters.shareCovered.load(), counters.shareMismatches.load(), counters.shareRecorded.load()};
    };
    // Queue 0's ordering point (an epoch bump) and the clock the token carries for it.
    const auto orderingPoint = [] {
        BumpCollectEpoch();
        return CollectEpoch();
    };
    // One committer collect after following `clock`, on a thread of its own; `before` runs first on it
    // (an own ordering point), `since` is checked against the answer.
    struct Committed {
        std::uint64_t generation = 0;
        bool unchangedSince = false;
        Snapshot delta{};
    };
    const auto commit = [&](std::uint64_t clock, std::uint64_t address, std::size_t size, std::uint64_t since, bool ownBump = false) {
        Committed result;
        const auto start = snapshot();
        std::thread([&] {
            MarkCommitterThread();
            BumpCollectEpoch(Gen::TicketReason::Committer);
            FollowCollectShare(clock);
            if (ownBump) BumpCollectEpoch(Gen::TicketReason::UnlockedWait);
            result.generation = CollectWrites(address, size);
            result.unchangedSince = UnchangedSince(address, size, since);
        }).join();
        const auto end = snapshot();
        result.delta = {end.walks - start.walks, end.covered - start.covered, end.mismatches - start.mismatches, end.recorded - start.recorded};
        return result;
    };
    // A covered collect skips its walk (verify: walks anyway and counts it as covered).
    const auto requireShared = [&](const Committed& result, const char* what) {
        Require(result.generation != 0, std::string(what) + ": the committer collect failed");
        if (!on) {
            Require(result.delta.walks == 1 && result.delta.covered == 0, std::string(what) + ": the switch is off but the committer did not walk");
            return;
        }
        Require(result.delta.covered == 1, std::string(what) + ": the committer collect was not covered by queue 0's walk");
        Require(result.delta.walks == (verify ? 1u : 0u), std::string(what) + ": the covered collect walked (or verify did not)");
    };
    const auto requireWalked = [&](const Committed& result, const char* what) {
        Require(result.generation != 0, std::string(what) + ": the committer collect failed");
        Require(result.delta.walks == 1 && result.delta.covered == 0, std::string(what) + ": the committer reused a walk that does not cover its item");
    };

    // Queue 0 walks after the ordering point; a write before the point is seen through its walk.
    auto clock = orderingPoint();
    const auto before = CollectWrites(base, bytes);
    memory[Block + 1] = 1;
    clock = orderingPoint();
    auto start = snapshot();
    const auto walked = CollectWrites(base, bytes);
    Require(snapshot().recorded - start.recorded == (on ? 1u : 0u), "queue 0's walk was not recorded as the switch says");
    auto result = commit(clock, base + Block, Block, before);
    requireShared(result, "same epoch");
    Require(!result.unchangedSince && result.generation >= walked, "the committer missed a write queue 0's walk found");

    // A walk made before the item's ordering point never covers it, and a write between them is seen.
    memory[2 * Block + 7] = 2;
    const auto oldClock = clock;
    clock = orderingPoint();
    result = commit(clock, base + 2 * Block, Block, walked);
    requireWalked(result, "walk before the ordering point");
    Require(!result.unchangedSince, "a write before the item's ordering point was missed");

    // A later queue-0 walk covers an item of an earlier ordering point.
    CollectWrites(base, bytes);
    result = commit(oldClock, base, Block, walked);
    if (on) requireShared(result, "later walk");

    // Partly covered: queue 0 walked only the first block of the range.
    clock = orderingPoint();
    CollectWrites(base, Block);
    result = commit(clock, base, 2 * Block, walked);
    requireWalked(result, "partly covered");
    result = commit(clock, base + Page, Block - Page, walked);
    if (on) requireShared(result, "inside the walked block");

    // Unknown ordering point (token slot overwritten), or the committer's own later ordering point.
    clock = orderingPoint();
    CollectWrites(base, bytes);
    requireWalked(commit(0, base, Block, walked), "unknown ordering point");
    requireWalked(commit(clock, base, Block, walked, true), "committer's own ordering point");

    // Unwatch and rewatch, then a mapping change: entries from before never count again.
    clock = orderingPoint();
    CollectWrites(base, bytes);
    Unwatch(base + 3 * Block, Block);
    const Runs restored{{base + 3 * Block, base + 4 * Block}};
    Rewatch(restored);
    requireWalked(commit(clock, base, Block, walked), "after an unwatch");
    clock = orderingPoint();
    CollectWrites(base, bytes);
    CollectWrites(base, bytes);
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(base + 3 * Block, Block);
    requireWalked(commit(clock, base, Block, walked), "after a mapping change");
    clock = orderingPoint();
    CollectWrites(base, bytes);
    CollectWrites(base, bytes);
    if (on) requireShared(commit(clock, base, Block, walked), "recorded again after the serials moved");

    // Verify: a write after queue 0's walk inside the epoch is a counted mismatch, and is seen.
    if (verify) {
        clock = orderingPoint();
        const auto fresh = CollectWrites(base, bytes);
        memory[5] = 3;
        result = commit(clock, base, Block, fresh);
        requireShared(result, "verify write after the walk");
        Require(result.delta.mismatches == 1 && !result.unchangedSince, "verify did not count (or hid) a write the shared answer would miss");
    }
    // An uncached committer collect always walks.
    clock = orderingPoint();
    CollectWrites(base, bytes);
    start = snapshot();
    std::thread([&] {
        MarkCommitterThread();
        BumpCollectEpoch(Gen::TicketReason::Committer);
        FollowCollectShare(clock);
        Require(CollectWritesUncached(base, Block) != 0, "an uncached committer collect failed");
    }).join();
    Require(snapshot().covered == start.covered, "an uncached committer collect was answered from queue 0's walks");
    SetCollectShareProducer(false);
}
#endif

#ifdef _WIN32
// APS5_ARCHB_FIX_WRITE_TRACKING_BOOKKEEPING (run with it on; the verify variants also set
// APS5_VERIFY_WRITE_TRACKING_BOOKKEEPING=1, where every fast answer is checked against the original).
// A CPU write between two collects of one epoch is seen by the next epoch, exactly as with the
// original memo; an epoch boundary (a submission, a CPU-satisfiable label wait, a drain or a reap all
// call BumpCollectEpoch) always forces a fresh walk; unwatching, remapping and decommitting retire
// the cached answer; the generation a hit returns is the tracker's current one.
void CheckPageStateMemo() {
    using Recorder = AgcDriver::ArchB::Recorder;
    using AgcDriver::ArchB::Feature;
    using AgcDriver::ArchB::Switches;
    Switches::Initialize();
    Require(Switches::Enabled(Feature::PageStateMemo), "the page-state memo switch is off");
    Recorder::Boundary(true, 0, Recorder::Clock::now());
    constexpr std::size_t Page = 4096;
    PartlyCommitted range;
    const auto at = range.Address();
    const auto whole = [&](std::uint64_t address, std::size_t bytes) {
        const auto runs = CommittedRanges(address, bytes);
        return runs.size() == 1 && runs[0].first == address && runs[0].second == address + bytes;
    };
    range.Commit(1);
    for (int pass = 0; pass < 600; ++pass) {
        Require(whole(at + Block, Block), "a committed block lost its run");
        Require(whole(at + Block + 3, 5 * Page + 1), "an unaligned committed range lost its run");
        Require(Accessible(reinterpret_cast<const void*>(at + Block + 8), 64, true), "a committed range is not accessible");
        Require(CommittedRanges(at, Block).empty(), "a reserved block has a committed run");
    }
    range.Decommit(1);
    Require(CommittedRanges(at + Block, Block).empty(), "a decommitted block kept its memoized run");
    Require(!Accessible(reinterpret_cast<const void*>(at + Block + 8), 64, false), "a decommitted range stayed accessible");
    range.Commit(1);
    Require(whole(at + Block, Block), "a recommitted block has no run");
    range.Commit(2);
    Require(whole(at + Block, 2 * Block), "two committed blocks are not one run");
    range.Decommit(2);
    const auto runs = CommittedRanges(at + Block, 2 * Block);
    Require(runs.size() == 1 && runs[0].first == at + Block && runs[0].second == at + 2 * Block, "a partly decommitted range kept its memoized run");
    Require(whole(at + Block, Block), "the remaining block lost its run");
    std::map<std::string, double> totals;
    for (const auto& [name, value] : Recorder::Collect(Recorder::Clock::now()).counters) totals[name] += value;
    Require(totals["page_memo_hits"] >= 256, "the page-state memo did not answer repeated ranges");
    std::cout << "page-state memo test passed (" << totals["page_memo_hits"] << " hits, " << totals["page_memo_misses"] << " misses)\n";
}

// The control file of the bookkeeping toggle and bench modes. Set before anything initializes the switches: the
// tracker's initialization reads them (WT_FAULT), and Switches::Initialize runs once.
std::filesystem::path BookkeepingControl(std::wstring_view mode) {
    if (mode != L"toggle" && mode != L"bench") return {};
    auto control = std::filesystem::temp_directory_path() / ("wt-bookkeeping-" + std::to_string(GetCurrentProcessId()) + ".txt");
    _putenv_s("APS5_ARCHB_GATE_RUN", "1");
    _putenv_s("APS5_ARCHB_CONTROL_FILE", control.string().c_str());
    return control;
}

void CheckWriteTrackingBookkeeping(std::wstring_view mode, const std::filesystem::path& control) {
    using Recorder = AgcDriver::ArchB::Recorder;
    using AgcDriver::ArchB::Feature;
    using AgcDriver::ArchB::Switches;
    const bool verify = [] {
        const char* value = std::getenv("APS5_VERIFY_WRITE_TRACKING_BOOKKEEPING");
        return value != nullptr && std::string_view(value) == "1";
    }();
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

void CheckMarkPossiblyWrittenOrder() {
    auto* memory = static_cast<std::uint8_t*>(AllocateWatched(2 * Block));
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x11, 2 * Block);
    Require(CollectWrites(base, 2 * Block) != 0, "the mark-order fixture was not collected");

    std::uint64_t emptyOrder = 1;
    Require(MarkPossiblyWritten(base, 0, emptyOrder) == 0 && emptyOrder == 0, "an empty possible write returned a tracker order");
    std::uint64_t trackedOrder = 0;
    const auto trackedProof = MarkPossiblyWritten(base + 8, 4, trackedOrder);
    Require(trackedProof != 0 && trackedOrder == trackedProof, "a tracked possible write did not return its stamped order");
    const auto later = MarkWritten(base + Block + 8, 4);
    Require(later > trackedOrder && trackedOrder == trackedProof, "a later generation increment changed the captured mark order");

    Unwatch(base, Block);
    Require(MarkPossiblyWritten(base + 16, 4) == 0, "the two-argument possible write trusted an untracked range");
    const auto beforeUntracked = TrackerGeneration();
    std::uint64_t untrackedOrder = 0;
    Require(MarkPossiblyWritten(base + 16, 4, untrackedOrder) == 0, "the ordered possible write trusted an untracked range");
    Require(untrackedOrder > beforeUntracked && untrackedOrder == TrackerGeneration(), "an untracked possible write did not return its atomic tracker order");

    constexpr std::size_t MarkCount = 128;
    std::array<std::uint64_t, MarkCount> proofs{};
    std::array<std::uint64_t, MarkCount> orders{};
    std::atomic<bool> start{false};
    std::thread marker([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::size_t i = 0; i < MarkCount; ++i) proofs[i] = MarkPossiblyWritten(base + Block + 16, 4, orders[i]);
    });
    std::thread advancing([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::size_t i = 0; i < MarkCount; ++i) MarkWritten(base + Block + 64, 4);
    });
    start.store(true, std::memory_order_release);
    marker.join();
    advancing.join();
    for (std::size_t i = 0; i < MarkCount; ++i) Require(proofs[i] != 0 && proofs[i] == orders[i], "a concurrent generation increment changed a possible-write order");

#ifdef _WIN32
    std::array<std::uint8_t, 4> interleavingBuffer{};
    failedCollectInterleavingAddress = reinterpret_cast<std::uint64_t>(interleavingBuffer.data());
    failedCollectInterleavingOrder = 0;
    failedCollectInterleavingProof = 1;
    failedCollectInterleavingCalled = false;
    failedCollectInterleavingFailed = false;
    SetFreshCollectHook(&FailFreshCollect);
    SetUnwatchHook(&MarkDuringFailedCollectUnwatch);
    std::uint64_t failedCollectOrder = 0;
    const auto failedCollectProof = MarkPossiblyWritten(base + Block + 8, 4, failedCollectOrder);
    SetFreshCollectHook(nullptr);
    SetUnwatchHook(nullptr);
    Require(failedCollectProof == 0 && failedCollectBegin == base + Block && failedCollectEnd == base + 2 * Block, "the failed fresh collect did not reach the intended watched block");
    Require(failedCollectInterleavingCalled && !failedCollectInterleavingFailed, "the failed fresh collect did not allow the intervening mark to complete");
    Require(failedCollectInterleavingProof == 0, "the intervening mark unexpectedly produced a proof");
    Require(failedCollectOrder > failedCollectInterleavingOrder, "a failed fresh collect reused the intervening writer's mark order");
#endif
}

int main(int argc, char** argv) {
    try {
#ifdef _WIN32
        if (std::wstring_view(GetCommandLineW()).ends_with(MetadataCommitMode)) {
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
        const auto share = command.rfind(L"--collect-share-");
        if (share != std::wstring_view::npos) {
            Require(WriteWatched(), "the collect-share test needs write watching");
            CheckCollectShare(command.substr(share + 16));
            std::cout << "collect-share test passed\n";
            return 0;
        }
        if (command.rfind(L"--page-state-memo") != std::wstring_view::npos) {
            CheckPageStateMemo();
            return 0;
        }
        const auto bookkeeping = command.rfind(L"--wt-bookkeeping-");
        if (bookkeeping != std::wstring_view::npos) {
            const auto mode = command.substr(bookkeeping + 17);
            const auto control = BookkeepingControl(mode);
            Require(WriteWatched(), "the bookkeeping test needs write watching");
            CheckWriteTrackingBookkeeping(mode, control);
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
        if (argc == 2 && std::string_view(argv[1]) == "--generation-wrap") {
            CheckGenerationWrap();
            std::cout << "generation wrap test passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--mark-order") {
            CheckMarkPossiblyWrittenOrder();
            std::cout << "mark order test passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--changed-pages") {
            CheckChangedPages();
            std::cout << "changed pages test passed\n";
            return 0;
        }
#ifdef _WIN32
        if (argc == 2 && std::string_view(argv[1]) == "--archc-switches") {
            CheckArchcSwitches();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--archc-log") {
            CheckArchcLog();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--archc-watch") {
            CheckArchcWatch();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--archc-epochs") {
            CheckArchcEpochs();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--wt-fault-hot") {
            CheckWtFaultHot();
            std::cout << "fault tracking hot pages test passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--wt-fault-dirty-arm") {
            CheckWtFaultDirtyArm();
            std::cout << "fault tracking dirty-arm test passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--wt-fault-consumers") {
            CheckWtFaultConsumers();
            std::cout << "fault tracking consumers test passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--wt-fault") {
            CheckWtFault();
            std::cout << "fault tracking test passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--wt-fault-host-copy") {
            CheckWtFaultHostCopy();
            std::cout << "fault tracking host copy test passed\n";
            return 0;
        }
#endif
        if (argc == 2 && std::string_view(argv[1]) == "--unstamped-alias") {
            CheckUnstampedAlias();
            std::cout << "unstamped alias test passed\n";
            return 0;
        }
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
        if (std::wstring_view(GetCommandLineW()).ends_with(CpuWritePinMode)) {
            CheckCpuWritePin();
            return 0;
        }
        if (std::wstring_view(GetCommandLineW()).ends_with(FreshViewMode)) {
            CheckFreshViewPagesListed();
            return 0;
        }
        if (std::wstring_view(GetCommandLineW()).ends_with(OwnStoreImagesMode)) {
            CheckOwnStoreImages();
            return 0;
        }
#endif
        CheckSharedBlock();
        CheckStoredOverUsesPageEvidence();
        CheckRepeatedNeighbourStores();
        CheckStoredAllSince();
        CheckSharedBlockProofs();
        CheckRepeatedPossibleStores();
        CheckOwnStore();
        CheckGpuWriteNotListed();
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
        CheckHostHoleInChild(HostHoleMode, ChildHostHole());
        CheckHostHoleInChild(MetadataCommitMode, ProbedHostHole());
        CheckFailedCollectUnwatch();
#endif
        CheckVersionRanges();
        CheckCollectBoundaries();
#ifdef _WIN32
        CheckWidenedCollectBesideUncommittedPages();
#endif
        CheckImportWindow();
        CheckImportWindowZeroPages();
        CheckImportWindowOtherThread();
        CheckImportWindowOverUnwatchedBlock();
        CheckImportWindowAcrossThreads();
#if defined(__linux__)
        CheckImportWindowOverAlias();
#endif
        CheckStoreAcrossImports();
        CheckImportWindowFootprint();
        CheckUnwatchedImport();
        CheckImportWalkUnwatchHooks();
        CheckImportWalkHookKeepsLaterWindows();
        CheckUnwatchHooks();
        CheckCommittedRanges();
        CheckAdoptLatestEpoch();
#ifndef _WIN32
        CheckOwnStoreImages();
#endif
    } catch (const std::exception& error) {
        std::cerr << "write tracking test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "write tracking tests passed\n";
    return 0;
}
