#include "prx/libc/include/GuestWriteFault.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace GuestArena {

#ifdef _WIN32
namespace {

constexpr std::uintptr_t PageBytes = 4096;
constexpr unsigned PageShift = 12;
constexpr unsigned LeafShift = 21;
constexpr std::size_t LeafPages = std::size_t{1} << (LeafShift - PageShift);

struct Leaf {
    std::atomic<std::uint64_t> words[LeafPages];
    // Verify only: the clock of the last resetting write-watch walk that covered each page, and of the
    // last one that covered the whole leaf (a page's reset clock is the larger of the two).
    std::atomic<std::uint64_t> resets[LeafPages];
    std::atomic<std::uint64_t> leafReset{0};
    // Driver-stored pages since the last arm (an arm leaves them open, see NoteDriverStore).
    std::atomic<std::uint64_t> hot[LeafPages / 64];
    std::atomic<std::uint64_t> cold[LeafPages / 64];
    std::atomic<std::uint64_t> stored[LeafPages / 64];
    std::atomic<std::uint64_t> storedLast[LeafPages / 64];
    std::atomic<std::uint64_t> watch[LeafPages / 64];
    std::atomic<std::uint64_t> lazy[LeafPages / 64];
    // Hint: may hold open pages (an arm scans the leaf). Set by every transition to Open.
    std::atomic<std::uint32_t> open{0};
    // Pages not armed (any state but Armed): 0 with `newest` at most a clock answers a whole leaf.
    std::atomic<std::uint32_t> notArmed{LeafPages};
    // Pages not untracked: LeafPages lets Track skip the leaf without the mutex.
    std::atomic<std::uint32_t> candidates{0};
    std::atomic<std::uint64_t> newest{0};
    std::uintptr_t base = 0;
    Leaf* next = nullptr;
};

struct Counters {
    std::atomic<std::uint64_t> faults{0}, driverFaults{0}, handlerTicks{0}, driverHandlerTicks{0}, retries{0}, foreign{0};
    std::atomic<std::uint64_t> armCalls{0}, armedPages{0}, armRuns{0}, armTicks{0}, armProtectTicks{0}, armQueries{0}, excluded{0}, keptHot{0};
    std::atomic<std::uint64_t> armLockTicks{0}, armScanTicks{0}, armLeaves{0}, leaves{0}, armQueryTicks{0}, keptDirty{0}, dirtyTicks{0};
    std::atomic<std::uint64_t> holds{0}, heldOpened{0}, opens{0}, forgotten{0}, tracked{0};
    std::atomic<std::uint64_t> verifyWalks{0}, verifyDirty{0}, verifyUntracked{0}, verifyMismatches{0}, queries{0};
    std::atomic<std::uint64_t> logAppends{0}, logLost{0}, coldRearmed{0};
    std::atomic<std::uint64_t> openSplitRuns{0}, openSplitPieces{0}, openRefused{0};
};

struct State {
    std::uintptr_t base = 0;
    std::uintptr_t end = 0;
    std::size_t leafCount = 0;
    std::atomic<Leaf*>* leaves = nullptr;
    bool verify = false;
    bool driverHot = true;
    bool guestHot = true;
    std::mutex mutex;
    Leaf* allocated = nullptr;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> held;
    struct Run {
        std::uintptr_t begin;
        std::uintptr_t end;
    };
    std::vector<Run> runs;
    std::vector<Run> armedRuns;
    std::vector<Run> cleanRuns;
    std::vector<Run> keptDirtyRuns;
    std::vector<void*> dirtyPages = std::vector<void*>(4096);
    Counters counters;
    double ticksToNs = 1.0;
};

// Constant-initialized: the handler reads them for every access violation in the process, also
// before (or without) GuestWtFaultEnable.
std::atomic<bool> g_enabled{false};
State* g_state = nullptr;
std::atomic<std::uint64_t> g_clock{1};

struct LogSlot {
    std::atomic<std::uint64_t> seq{0};
    std::atomic<std::uint64_t> entry{0};
};

LogSlot g_log[WtFaultLogSlots];
std::atomic<std::uint64_t> g_logHead{0};

void logAppend(std::uint64_t first, std::uint64_t second, bool pair) {
    const auto at = g_logHead.fetch_add(pair ? 2 : 1, std::memory_order_acq_rel);
    auto& slot = g_log[at % WtFaultLogSlots];
    slot.entry.store(first, std::memory_order_relaxed);
    slot.seq.store(at + 1, std::memory_order_release);
    if (pair) {
        auto& next = g_log[(at + 1) % WtFaultLogSlots];
        next.entry.store(second, std::memory_order_relaxed);
        next.seq.store(at + 2, std::memory_order_release);
    }
    if (g_state != nullptr) g_state->counters.logAppends.fetch_add(pair ? 2 : 1, std::memory_order_relaxed);
}

void logRange(std::uintptr_t begin, std::uintptr_t end, std::uint64_t kind) {
    logAppend(static_cast<std::uint64_t>(begin) | kind, static_cast<std::uint64_t>(end) | WtFaultLogForgetEnd, true);
}

bool coldPage(const Leaf& leaf, std::uintptr_t page) {
    const auto index = (page >> PageShift) & (LeafPages - 1);
    return ((leaf.cold[index / 64].load(std::memory_order_acquire) >> (index % 64)) & 1) != 0;
}

struct DriverThreadTag {};
using DriverThread = HostThreadSlot<std::uintptr_t, DriverThreadTag>;

constexpr std::size_t WatchLogSlots = 32768;

struct WatchSlot {
    std::atomic<std::uint64_t> seq{0};
    std::atomic<std::uint64_t> entry{0};
    std::atomic<std::uint64_t> ticks{0};
};

WatchSlot g_watchLog[WatchLogSlots];
std::atomic<std::uint64_t> g_watchHead{0};
std::atomic<std::uint64_t> g_watchedPages{0};
std::atomic<std::uint64_t> g_watchFaults{0};
std::atomic<std::uint64_t> g_watchFaultTicks{0};
// APS5_ARCHB_C_WATCH_SPLIT (set by the driver, GuestWtFaultSetWatchSplit): a watch run one VirtualProtect
// refuses is protected piece by piece. VirtualProtect takes one allocation per call, and live guest direct
// memory is committed in 16 KiB allocations (GuestArenaCommit's granule), so without it every watch range
// longer than an allocation fails whole, with no per-page cause (the P5.0 census's "clamp" refusals).
std::atomic<bool> g_watchSplit{false};
std::atomic<std::uint64_t> g_watchSplitRuns{0};
std::atomic<std::uint64_t> g_watchSplitPieces{0};

std::int64_t ticks();

void watchLog(std::uintptr_t page, std::uint64_t kind) {
    const auto at = g_watchHead.fetch_add(1, std::memory_order_acq_rel);
    auto& slot = g_watchLog[at % WatchLogSlots];
    slot.entry.store(static_cast<std::uint64_t>(page) | kind | (DriverThread::Get() != 0 ? WtWatchDriver : 0), std::memory_order_relaxed);
    slot.ticks.store(static_cast<std::uint64_t>(ticks()), std::memory_order_relaxed);
    slot.seq.store(at + 1, std::memory_order_release);
}

bool watchedPage(const Leaf& leaf, std::uintptr_t page) {
    const auto index = (page >> PageShift) & (LeafPages - 1);
    return ((leaf.watch[index / 64].load(std::memory_order_acquire) >> (index % 64)) & 1) != 0;
}

struct LazyServiceTag {};
using LazyServiceDepth = HostThreadSlot<std::uintptr_t, LazyServiceTag>;
struct LazyCauseTag {};
using LazyCause = HostThreadSlot<std::uintptr_t, LazyCauseTag>;
std::atomic<bool (*)(std::uintptr_t, std::size_t)> lazyPreservation{nullptr};

bool preserveLazy(std::uintptr_t address, std::size_t bytes) {
    const auto predicate = lazyPreservation.load(std::memory_order_acquire);
    return predicate != nullptr && predicate(address, bytes);
}

struct LazyCauseScope {
    explicit LazyCauseScope(WtLazyCause cause) : previous(LazyCause::Get()) { LazyCause::Set(static_cast<std::uintptr_t>(cause)); }
    ~LazyCauseScope() { LazyCause::Set(previous); }
    std::uintptr_t previous;
};
// The faulting instruction and stack pointer of the thread's last access violation while a watch
// exists (diagnostics: the lazy service names the driver code that touched a lazy range).
struct FaultIpTag {};
struct FaultSpTag {};
using FaultIp = HostThreadSlot<std::uintptr_t, FaultIpTag>;
using FaultSp = HostThreadSlot<std::uintptr_t, FaultSpTag>;
std::atomic<void (*)(std::uintptr_t, bool)> g_lazyService{nullptr};
std::atomic<std::uint64_t> g_lazyServices{0};
std::atomic<std::uint64_t> g_lazyNested{0};

bool lazyPage(const Leaf& leaf, std::uintptr_t page) {
    const auto index = (page >> PageShift) & (LeafPages - 1);
    return ((leaf.lazy[index / 64].load(std::memory_order_acquire) >> (index % 64)) & 1) != 0;
}

bool clearWatch(Leaf& leaf, std::uintptr_t page) {
    const auto index = (page >> PageShift) & (LeafPages - 1);
    const auto bit = std::uint64_t{1} << (index % 64);
    leaf.lazy[index / 64].fetch_and(~bit, std::memory_order_acq_rel);
    if ((leaf.watch[index / 64].fetch_and(~bit, std::memory_order_acq_rel) & bit) == 0) return false;
    g_watchedPages.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

std::int64_t ticks() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

constexpr std::uint64_t stateOf(std::uint64_t word) {
    return word & WtFaultStateMask;
}

constexpr std::uint64_t stampOf(std::uint64_t word) {
    return word >> WtFaultStampShift;
}

constexpr std::uint64_t make(std::uint64_t stamp, bool exec, std::uint64_t state) {
    return (stamp << WtFaultStampShift) | (exec ? WtFaultExecBit : 0) | state;
}

constexpr bool isExec(std::uint64_t word) {
    return (word & WtFaultExecBit) != 0;
}

// Protected by the tracker right now: armed, arming (its protect may have taken effect) or opening.
constexpr bool protectedState(std::uint64_t state) {
    return state == WtFaultArmed || state == WtFaultArming || state == WtFaultOpening;
}

bool writableProtection(DWORD protection) {
    const auto base = protection & 0xffu;
    return base == PAGE_READWRITE || base == PAGE_EXECUTE_READWRITE || base == PAGE_WRITECOPY || base == PAGE_EXECUTE_WRITECOPY;
}

Leaf* leafAt(const State& state, std::uintptr_t address) {
    return state.leaves[(address - state.base) >> LeafShift].load(std::memory_order_acquire);
}

std::atomic<std::uint64_t>& wordOf(Leaf& leaf, std::uintptr_t address) {
    return leaf.words[(address >> PageShift) & (LeafPages - 1)];
}

// Clamps [address, address + bytes) to the arena in whole pages; false when nothing is left.
bool clamp(const State& state, std::uintptr_t address, std::size_t bytes, std::uintptr_t& first, std::uintptr_t& stop) {
    if (bytes == 0 || address >= state.end || address + bytes <= state.base || address + bytes < address) return false;
    first = std::max(address, state.base) & ~(PageBytes - 1);
    stop = (std::min<std::uintptr_t>(address + bytes, state.end) + PageBytes - 1) & ~(PageBytes - 1);
    return first < stop;
}

// Under the mutex.
Leaf& leafFor(State& state, std::uintptr_t address) {
    auto& slot = state.leaves[(address - state.base) >> LeafShift];
    if (auto* leaf = slot.load(std::memory_order_acquire)) return *leaf;
    auto* leaf = new Leaf();
    for (auto& word : leaf->words) word.store(0, std::memory_order_relaxed);
    for (auto& reset : leaf->resets) reset.store(0, std::memory_order_relaxed);
    for (auto& bits : leaf->hot) bits.store(0, std::memory_order_relaxed);
    for (auto& bits : leaf->cold) bits.store(0, std::memory_order_relaxed);
    for (auto& bits : leaf->stored) bits.store(0, std::memory_order_relaxed);
    for (auto& bits : leaf->storedLast) bits.store(0, std::memory_order_relaxed);
    for (auto& bits : leaf->watch) bits.store(0, std::memory_order_relaxed);
    for (auto& bits : leaf->lazy) bits.store(0, std::memory_order_relaxed);
    leaf->base = state.base + (((address - state.base) >> LeafShift) << LeafShift);
    leaf->next = state.allocated;
    state.allocated = leaf;
    state.counters.leaves.fetch_add(1, std::memory_order_relaxed);
    slot.store(leaf, std::memory_order_release);
    return *leaf;
}

DWORD guestProtection(bool exec) {
    return exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
}

// Protects [begin, end) with one VirtualProtect; when that fails and `split`, once per VirtualQuery region
// (a region never spans allocations). `failed(pieceBegin, pieceEnd)` for what stayed unprotected.
template <typename Failed>
bool protectPieces(std::uintptr_t begin, std::uintptr_t end, DWORD protection, bool split, std::atomic<std::uint64_t>& splitRuns, std::atomic<std::uint64_t>& splitPieces, Failed&& failed) {
    DWORD previous;
    if (VirtualProtect(reinterpret_cast<void*>(begin), end - begin, protection, &previous) != 0) return true;
    if (!split) {
        failed(begin, end);
        return false;
    }
    splitRuns.fetch_add(1, std::memory_order_relaxed);
    bool all = true;
    for (auto cursor = begin; cursor < end;) {
        MEMORY_BASIC_INFORMATION memory{};
        auto pieceEnd = cursor + PageBytes;
        if (VirtualQuery(reinterpret_cast<void*>(cursor), &memory, sizeof(memory)) == sizeof(memory) && memory.RegionSize != 0) {
            pieceEnd = std::min(end, std::max(pieceEnd, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize));
        }
        splitPieces.fetch_add(1, std::memory_order_relaxed);
        if (VirtualProtect(reinterpret_cast<void*>(cursor), pieceEnd - cursor, protection, &previous) == 0) {
            failed(cursor, pieceEnd);
            all = false;
        }
        cursor = pieceEnd;
    }
    return all;
}

// Opening -> Open after restoring the guest protection of [begin, end) (all words already Opening,
// all with the same exec bit). Under the mutex or from the handler. A run of armed pages can span
// allocations (live direct memory is one 16 KiB allocation per PS5 page, and an arm protects each
// allocation apart), which one VirtualProtect refuses whole: the run is then restored one VirtualQuery
// region at a time. Only the pages of a piece whose protect failed go back to Untracked (they are not
// what the tracker armed any more); false when any did.
bool finishOpening(State& state, std::uintptr_t begin, std::uintptr_t end, bool exec) {
    const auto settle = [&](std::uintptr_t from, std::uintptr_t to, bool done) {
        for (auto page = from; page < to; page += PageBytes) {
            auto* leaf = leafAt(state, page);
            auto& word = wordOf(*leaf, page);
            auto expected = word.load(std::memory_order_acquire);
            if (stateOf(expected) != WtFaultOpening) continue;
            if (done) {
                if (word.compare_exchange_strong(expected, (expected & ~WtFaultStateMask) | WtFaultOpen, std::memory_order_acq_rel)) leaf->open.store(1, std::memory_order_release);
            } else if (word.compare_exchange_strong(expected, WtFaultUntracked, std::memory_order_acq_rel)) {
                leaf->candidates.fetch_sub(1, std::memory_order_relaxed);
                state.counters.openRefused.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    const bool done = protectPieces(begin, end, guestProtection(exec), true, state.counters.openSplitRuns, state.counters.openSplitPieces, [&](std::uintptr_t pieceBegin, std::uintptr_t pieceEnd) { settle(pieceBegin, pieceEnd, false); });
    settle(begin, end, true);
    return done;
}

// Opens every armed or arming page of [first, stop) (under the mutex): returns the pages opened.
std::size_t openRange(State& state, std::uintptr_t first, std::uintptr_t stop, std::uint64_t logKind) {
    std::size_t opened = 0;
    std::uintptr_t runBegin = 0;
    std::uintptr_t runEnd = 0;
    bool runExec = false;
    const auto flush = [&] {
        if (runEnd > runBegin) {
            logRange(runBegin, runEnd, logKind);
            finishOpening(state, runBegin, runEnd, runExec);
        }
        runBegin = runEnd = 0;
    };
    for (auto page = first; page < stop; page += PageBytes) {
        auto* leaf = leafAt(state, page);
        if (leaf == nullptr) {
            flush();
            page = std::min(stop, state.base + ((((page - state.base) >> LeafShift) + 1) << LeafShift)) - PageBytes;
            continue;
        }
        auto& word = wordOf(*leaf, page);
        for (unsigned spins = 0;; ++spins) {
            auto value = word.load(std::memory_order_acquire);
            const auto current = stateOf(value);
            if (current == WtFaultOpening) {
                // The handler on another thread is opening it; it needs no lock.
                if (spins < 1024) YieldProcessor();
                else SwitchToThread();
                continue;
            }
            if (current != WtFaultArmed && current != WtFaultArming) {
                flush();
                break;
            }
            if (!word.compare_exchange_weak(value, (value & ~WtFaultStateMask) | WtFaultOpening, std::memory_order_acq_rel)) continue;
            if (current == WtFaultArmed) leaf->notArmed.fetch_add(1, std::memory_order_relaxed);
            const bool exec = isExec(value);
            if (runEnd != page || exec != runExec) {
                flush();
                runBegin = page;
                runExec = exec;
            }
            runEnd = page + PageBytes;
            ++opened;
            break;
        }
    }
    flush();
    return opened;
}

bool heldPage(const State& state, std::uintptr_t page) {
    for (const auto& [begin, end] : state.held) {
        if (begin < page + PageBytes && page < end) return true;
    }
    return false;
}

std::atomic<bool> g_watchEver{false};

DWORD trackedProtection(std::uint64_t word) {
    return stateOf(word) == WtFaultArmed ? (isExec(word) ? PAGE_EXECUTE_READ : PAGE_READONLY) : guestProtection(isExec(word));
}

void spinOnce(unsigned spins) {
    if (spins < 1024) YieldProcessor();
    else SwitchToThread();
}

bool unwatchPage(Leaf& leaf, std::uintptr_t page, std::uint64_t kind) {
    auto& word = wordOf(leaf, page);
    for (unsigned spins = 0;; ++spins) {
        if (!watchedPage(leaf, page)) return false;
        auto value = word.load(std::memory_order_acquire);
        const auto current = stateOf(value);
        if (current == WtFaultOpening || current == WtFaultArming) {
            spinOnce(spins);
            continue;
        }
        if (current != WtFaultArmed && current != WtFaultOpen) {
            const bool had = clearWatch(leaf, page);
            if (had && kind != 0) watchLog(page, kind);
            return had;
        }
        const auto claim = (value & ~WtFaultStateMask) | WtFaultOpening;
        if (!word.compare_exchange_weak(value, claim, std::memory_order_acq_rel)) continue;
        if (current == WtFaultArmed) leaf.notArmed.fetch_add(1, std::memory_order_relaxed);
        const bool had = clearWatch(leaf, page);
        if (had) {
            DWORD previous;
            VirtualProtect(reinterpret_cast<void*>(page), PageBytes, trackedProtection(value), &previous);
            if (kind != 0) watchLog(page, kind);
        }
        auto expected = claim;
        if (word.compare_exchange_strong(expected, value, std::memory_order_acq_rel) && current == WtFaultArmed) leaf.notArmed.fetch_sub(1, std::memory_order_relaxed);
        return had;
    }
}

struct WatchClaim {
    std::uintptr_t page;
    std::uint64_t value;
    Leaf* leaf;
};

void releaseClaims(const std::vector<WatchClaim>& claims) {
    for (const auto& claim : claims) {
        auto& word = wordOf(*claim.leaf, claim.page);
        auto expected = (claim.value & ~WtFaultStateMask) | WtFaultOpening;
        if (word.compare_exchange_strong(expected, claim.value, std::memory_order_acq_rel) && stateOf(claim.value) == WtFaultArmed) claim.leaf->notArmed.fetch_sub(1, std::memory_order_relaxed);
    }
}

std::size_t unwatchRange(State& state, std::uintptr_t first, std::uintptr_t stop, std::uint64_t kind) {
    if (!g_watchEver.load(std::memory_order_acquire)) return 0;
    std::vector<WatchClaim> claims;
    std::size_t unwatched = 0;
    for (auto page = first; page < stop; page += PageBytes) {
        auto* leaf = leafAt(state, page);
        if (leaf == nullptr) {
            page = std::min(stop, state.base + ((((page - state.base) >> LeafShift) + 1) << LeafShift)) - PageBytes;
            continue;
        }
        auto& word = wordOf(*leaf, page);
        for (unsigned spins = 0;; ++spins) {
            if (!watchedPage(*leaf, page)) break;
            auto value = word.load(std::memory_order_acquire);
            const auto current = stateOf(value);
            if (current == WtFaultOpening || current == WtFaultArming) {
                spinOnce(spins);
                continue;
            }
            if (current != WtFaultArmed && current != WtFaultOpen) {
                if (clearWatch(*leaf, page)) {
                    ++unwatched;
                    if (kind != 0) watchLog(page, kind);
                }
                break;
            }
            if (!word.compare_exchange_weak(value, (value & ~WtFaultStateMask) | WtFaultOpening, std::memory_order_acq_rel)) continue;
            if (current == WtFaultArmed) leaf->notArmed.fetch_add(1, std::memory_order_relaxed);
            claims.push_back({page, value, leaf});
            break;
        }
    }
    for (std::size_t i = 0; i < claims.size();) {
        const auto protection = trackedProtection(claims[i].value);
        auto j = i + 1;
        while (j < claims.size() && claims[j].page == claims[j - 1].page + PageBytes && trackedProtection(claims[j].value) == protection) ++j;
        // Split on failure whatever the switch: adjacent ranges watched apart may lie in different allocations.
        protectPieces(claims[i].page, claims[j - 1].page + PageBytes, protection, true, g_watchSplitRuns, g_watchSplitPieces, [](std::uintptr_t, std::uintptr_t) {});
        for (auto k = i; k < j; ++k) {
            if (clearWatch(*claims[k].leaf, claims[k].page)) {
                ++unwatched;
                if (kind != 0) watchLog(claims[k].page, kind);
            }
        }
        i = j;
    }
    releaseClaims(claims);
    return unwatched;
}

}

void GuestWtFaultEnable_nid_no_patch(bool verify, std::uint64_t clockAbove) {
    auto clock = g_clock.load(std::memory_order_relaxed);
    while (clock < clockAbove && !g_clock.compare_exchange_weak(clock, clockAbove, std::memory_order_acq_rel, std::memory_order_relaxed)) {}
    if (g_enabled.load(std::memory_order_acquire)) return;
    static std::once_flag once;
    std::call_once(once, [verify] {
        auto* state = new State();
        std::size_t bytes = 0;
        GuestArenaRange_nid_postfix(&state->base, &bytes);
        state->base &= ~((std::uintptr_t{1} << LeafShift) - 1);
        state->end = state->base + ((bytes + (std::uintptr_t{1} << LeafShift) - 1) & ~((std::uintptr_t{1} << LeafShift) - 1));
        state->leafCount = (state->end - state->base) >> LeafShift;
        state->leaves = static_cast<std::atomic<Leaf*>*>(VirtualAlloc(nullptr, state->leafCount * sizeof(std::atomic<Leaf*>), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (state->leaves == nullptr) std::abort();
        state->verify = verify;
        char text[8] = {};
        if (GetEnvironmentVariableA("APS5_WT_FAULT_DRIVER_HOT", text, sizeof(text)) != 0 && text[0] == '0') state->driverHot = false;
        if (GetEnvironmentVariableA("APS5_WT_FAULT_GUEST_HOT", text, sizeof(text)) != 0 && text[0] == '0') state->guestHot = false;
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        state->ticksToNs = 1e9 / static_cast<double>(frequency.QuadPart);
        // The thread slot's TLS index is allocated here, never inside the handler.
        static_cast<void>(DriverThread::Get());
        static_cast<void>(LazyServiceDepth::Get());
        static_cast<void>(LazyCause::Get());
        static_cast<void>(FaultIp::Get());
        static_cast<void>(FaultSp::Get());
        g_state = state;
        g_enabled.store(true, std::memory_order_release);
    });
}

bool GuestWtFaultEnabled_nid_no_patch() {
    return g_enabled.load(std::memory_order_acquire);
}

void GuestWtFaultMarkDriverThread_nid_no_patch() {
    if (g_enabled.load(std::memory_order_acquire)) DriverThread::Set(1);
}

bool serveLazy(Leaf& leaf, std::uintptr_t page, bool write) {
    if (!lazyPage(leaf, page)) return false;
    const auto service = g_lazyService.load(std::memory_order_acquire);
    if (service == nullptr) return false;
    if (LazyServiceDepth::Get() != 0) {
        g_lazyNested.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    LazyServiceDepth::Set(1);
    service(page, write);
    LazyServiceDepth::Set(0);
    g_lazyServices.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool GuestWtFaultHandle_nid_no_patch(std::uintptr_t address) {
    if (!g_enabled.load(std::memory_order_acquire)) return false;
    auto& state = *g_state;
    if (address < state.base || address >= state.end) return false;
    auto* leaf = leafAt(state, address);
    if (leaf == nullptr) return false;
    const auto start = ticks();
    const auto page = address & ~(PageBytes - 1);
    if (serveLazy(*leaf, page, true)) return true;
    auto& word = wordOf(*leaf, page);
    bool waited = false;
    for (unsigned spins = 0;; ++spins) {
        auto value = word.load(std::memory_order_acquire);
        const auto current = stateOf(value);
        if (current == WtFaultArmed || current == WtFaultArming) {
            const auto opening = (g_clock.load(std::memory_order_acquire) << WtFaultStampShift) | (value & WtFaultFlagMask) | WtFaultOpening;
            if (!word.compare_exchange_weak(value, opening, std::memory_order_acq_rel)) continue;
            if (current == WtFaultArmed) leaf->notArmed.fetch_add(1, std::memory_order_relaxed);
            if (clearWatch(*leaf, page)) {
                watchLog(page, WtWatchWrite);
                g_watchFaults.fetch_add(1, std::memory_order_relaxed);
            }
            logAppend(static_cast<std::uint64_t>(page) | WtFaultLogStore, 0, false);
            if (!finishOpening(state, page, page + PageBytes, isExec(value))) return false;
            // A page stored in this frame stays open across the next flip (see GuestWtFaultNoteStored).
            {
                const auto index = (page >> PageShift) & (LeafPages - 1);
                leaf->stored[index / 64].fetch_or(std::uint64_t{1} << (index % 64), std::memory_order_relaxed);
            }
            if (state.guestHot && !coldPage(*leaf, page)) {
                const auto index = (page >> PageShift) & (LeafPages - 1);
                leaf->hot[index / 64].fetch_or(std::uint64_t{1} << (index % 64), std::memory_order_relaxed);
            }
            const auto elapsed = static_cast<std::uint64_t>(ticks() - start);
            state.counters.faults.fetch_add(1, std::memory_order_relaxed);
            state.counters.handlerTicks.fetch_add(elapsed, std::memory_order_relaxed);
            if (DriverThread::Get() != 0) {
                state.counters.driverFaults.fetch_add(1, std::memory_order_relaxed);
                state.counters.driverHandlerTicks.fetch_add(elapsed, std::memory_order_relaxed);
            }
            return true;
        }
        if (current == WtFaultOpening) {
            waited = true;
            if (spins < 1024) YieldProcessor();
            else SwitchToThread();
            continue;
        }
        if (current == WtFaultOpen && watchedPage(*leaf, page)) {
            unwatchPage(*leaf, page, WtWatchWrite);
            g_watchFaults.fetch_add(1, std::memory_order_relaxed);
            g_watchFaultTicks.fetch_add(static_cast<std::uint64_t>(ticks() - start), std::memory_order_relaxed);
            return true;
        }
        if (current == WtFaultOpen) {
            // Opened by another thread after this store faulted (the store re-executes), or a page the
            // guest made read-only itself after it was tracked (not ours).
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(reinterpret_cast<void*>(page), &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT && writableProtection(memory.Protect)) {
                state.counters.retries.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        if (waited) {
            state.counters.retries.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (current != WtFaultUntracked) state.counters.foreign.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

void GuestWtFaultTrack_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    auto& state = *g_state;
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop)) return;
    // Without the mutex: every leaf of the range already has every page a candidate.
    bool needed = false;
    for (auto at = first & ~((std::uintptr_t{1} << LeafShift) - 1); at < stop; at += std::uintptr_t{1} << LeafShift) {
        const auto* leaf = leafAt(state, at);
        if (leaf == nullptr || leaf->candidates.load(std::memory_order_acquire) != LeafPages) {
            needed = true;
            break;
        }
    }
    if (!needed) return;
    std::lock_guard lock(state.mutex);
    std::uint64_t made = 0;
    for (auto page = first; page < stop; page += PageBytes) {
        auto& leaf = leafFor(state, page);
        if (leaf.candidates.load(std::memory_order_relaxed) == LeafPages) {
            page = std::min(stop, leaf.base + (std::uintptr_t{1} << LeafShift)) - PageBytes;
            continue;
        }
        auto& word = wordOf(leaf, page);
        auto expected = WtFaultUntracked;
        if (word.compare_exchange_strong(expected, make(0, false, WtFaultOpen), std::memory_order_acq_rel)) {
            leaf.candidates.fetch_add(1, std::memory_order_relaxed);
            leaf.open.store(1, std::memory_order_release);
            ++made;
        }
    }
    state.counters.tracked.fetch_add(made, std::memory_order_relaxed);
}

void GuestWtFaultNoteStored_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    {
        auto& state = *g_state;
        std::uintptr_t first = 0;
        std::uintptr_t stop = 0;
        if (clamp(state, address, bytes, first, stop)) {
            for (auto page = first; page < stop; page += PageBytes) {
                auto* leaf = leafAt(state, page);
                if (leaf == nullptr) continue;
                const auto index = (page >> PageShift) & (LeafPages - 1);
                leaf->stored[index / 64].fetch_or(std::uint64_t{1} << (index % 64), std::memory_order_relaxed);
            }
        }
    }
    if (!g_state->guestHot) return;
    GuestWtFaultNoteDriverStore_nid_no_patch(address, bytes);
}

void GuestWtFaultNoteDriverStore_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    auto& state = *g_state;
    if (!state.driverHot && !state.guestHot) return;
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop)) return;
    for (auto page = first; page < stop; page += PageBytes) {
        auto* leaf = leafAt(state, page);
        if (leaf == nullptr) {
            page = std::min(stop, state.base + ((((page - state.base) >> LeafShift) + 1) << LeafShift)) - PageBytes;
            continue;
        }
        const auto index = (page >> PageShift) & (LeafPages - 1);
        leaf->hot[index / 64].fetch_or(std::uint64_t{1} << (index % 64), std::memory_order_relaxed);
    }
}

namespace {

void markOpenHint(const State& state, std::uintptr_t begin, std::uintptr_t end) {
    for (auto at = begin; at < end; at = (at & ~((std::uintptr_t{1} << LeafShift) - 1)) + (std::uintptr_t{1} << LeafShift)) leafAt(state, at)->open.store(1, std::memory_order_release);
}

std::size_t arm(std::uintptr_t* reported, std::size_t capacity, std::uintptr_t* dirtyReported, std::size_t dirtyCapacity, std::size_t* dirtyCount) {
    if (dirtyCount != nullptr) *dirtyCount = 0;
    if (!g_enabled.load(std::memory_order_acquire)) return 0;
    auto& state = *g_state;
    const auto start = ticks();
    std::lock_guard lock(state.mutex);
    const auto locked = ticks();
    auto& runs = state.runs;
    runs.clear();
    std::uint64_t kept = 0;
    std::uint64_t scanned = 0;
    for (auto* leaf = state.allocated; leaf != nullptr; leaf = leaf->next) {
        for (std::size_t group = 0; group < LeafPages / 64; ++group) leaf->storedLast[group].store(leaf->stored[group].exchange(0, std::memory_order_acq_rel), std::memory_order_release);
        if (leaf->open.exchange(0, std::memory_order_acq_rel) == 0) continue;
        ++scanned;
        bool stillOpen = false;
        for (std::size_t group = 0; group < LeafPages / 64; ++group) {
            const auto hot = leaf->hot[group].exchange(0, std::memory_order_acq_rel);
            for (std::size_t bit = 0; bit < 64; ++bit) {
                const auto index = group * 64 + bit;
                const auto page = leaf->base + (index << PageShift);
                if (stateOf(leaf->words[index].load(std::memory_order_acquire)) != WtFaultOpen) continue;
                if (watchedPage(*leaf, page)) {
                    ++kept;
                    stillOpen = true;
                    continue;
                }
                const bool cold = ((leaf->cold[group].load(std::memory_order_acquire) >> bit) & 1) != 0;
                const bool held = heldPage(state, page);
                if (cold && ((hot >> bit) & 1) != 0 && !held) state.counters.coldRearmed.fetch_add(1, std::memory_order_relaxed);
                if ((((hot >> bit) & 1) != 0 && !cold) || held) {
                    ++kept;
                    stillOpen = true;
                    continue;
                }
                if (!runs.empty() && runs.back().end == page) runs.back().end = page + PageBytes;
                else runs.push_back({page, page + PageBytes});
            }
        }
        if (stillOpen) leaf->open.store(1, std::memory_order_release);
    }
    const auto scannedAt = ticks();
    state.counters.armLockTicks.fetch_add(static_cast<std::uint64_t>(locked - start), std::memory_order_relaxed);
    state.counters.armScanTicks.fetch_add(static_cast<std::uint64_t>(scannedAt - locked), std::memory_order_relaxed);
    state.counters.armLeaves.fetch_add(scanned, std::memory_order_relaxed);
    if (dirtyCount != nullptr && dirtyCapacity != 0) {
        const auto dirtyStart = ticks();
        auto& clean = state.cleanRuns;
        clean.clear();
        auto& keptRuns = state.keptDirtyRuns;
        keptRuns.clear();
        const auto keep = [&](std::uintptr_t begin, std::uintptr_t end) {
            if (!keptRuns.empty() && keptRuns.back().end == begin) {
                keptRuns.back().end = end;
                return true;
            }
            if (keptRuns.size() == dirtyCapacity) return false;
            keptRuns.push_back({begin, end});
            return true;
        };
        std::uint64_t keptDirty = 0;
        for (const auto& run : runs) {
            ULONG_PTR count = state.dirtyPages.size();
            DWORD granularity = 0;
            if (GetWriteWatch(0, reinterpret_cast<void*>(run.begin), run.end - run.begin, state.dirtyPages.data(), &count, &granularity) != 0) {
                clean.push_back(run);
                continue;
            }
            if (count == state.dirtyPages.size()) {
                if (!keep(run.begin, run.end)) {
                    clean.push_back(run);
                    continue;
                }
                keptDirty += (run.end - run.begin) / PageBytes;
                markOpenHint(state, run.begin, run.end);
                continue;
            }
            auto cursor = run.begin;
            for (ULONG_PTR i = 0; i < count; ++i) {
                const auto page = reinterpret_cast<std::uintptr_t>(state.dirtyPages[i]) & ~(PageBytes - 1);
                if (page < cursor || page >= run.end) continue;
                if (!keep(page, page + PageBytes)) break;
                if (page > cursor) clean.push_back({cursor, page});
                leafAt(state, page)->open.store(1, std::memory_order_release);
                ++keptDirty;
                cursor = page + PageBytes;
            }
            if (cursor < run.end) clean.push_back({cursor, run.end});
        }
        runs.swap(clean);
        kept += keptDirty;
        for (std::size_t i = 0; i < keptRuns.size(); ++i) {
            dirtyReported[2 * i] = keptRuns[i].begin;
            dirtyReported[2 * i + 1] = keptRuns[i].end;
        }
        *dirtyCount = keptRuns.size();
        state.counters.keptDirty.fetch_add(keptDirty, std::memory_order_relaxed);
        state.counters.dirtyTicks.fetch_add(static_cast<std::uint64_t>(ticks() - dirtyStart), std::memory_order_relaxed);
    }
    // Check each run against the real mapping, mark its eligible pages arming and protect them.
    auto& armed = state.armedRuns;
    armed.clear();
    std::uint64_t queries = 0;
    std::uint64_t excluded = 0;
    std::int64_t protectTicks = 0;
    std::int64_t queryTicks = 0;
    // Pages an earlier arm checked keep their check until a forget (every mapping or protection change
    // forgets): they are protected without asking the OS. VirtualQuery of large write-watched private
    // regions costs 15-50 us per call (it sizes the region), so only newly tracked pages are queried.
    const auto checkedAt = [&](std::uintptr_t page) {
        const auto word = wordOf(*leafAt(state, page), page).load(std::memory_order_acquire);
        return stateOf(word) == WtFaultOpen && (word & WtFaultCheckedBit) != 0 ? 1 + (isExec(word) ? 1 : 0) : 0;
    };
    for (const auto& run : runs) {
        for (auto segment = run.begin; segment < run.end;) {
            const auto kind = checkedAt(segment);
            auto segmentEnd = segment + PageBytes;
            while (segmentEnd < run.end && checkedAt(segmentEnd) == kind) segmentEnd += PageBytes;
        for (auto cursor = segment; cursor < segmentEnd;) {
            MEMORY_BASIC_INFORMATION memory{};
            bool queried = true;
            if (kind == 0) {
                ++queries;
                const auto queryStart = ticks();
                queried = VirtualQuery(reinterpret_cast<void*>(cursor), &memory, sizeof(memory)) == sizeof(memory) && memory.RegionSize != 0;
                queryTicks += ticks() - queryStart;
            }
            const auto regionEnd = kind != 0 ? segmentEnd : queried ? std::min(segmentEnd, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize) : segmentEnd;
            const bool exec = kind != 0 ? kind == 2 : queried && memory.Protect == PAGE_EXECUTE_READWRITE;
            const bool eligible = kind != 0 || (queried && memory.State == MEM_COMMIT && memory.Type == MEM_PRIVATE && (memory.Protect == PAGE_READWRITE || exec));
            std::uintptr_t subBegin = 0;
            std::uintptr_t subEnd = 0;
            const auto protect = [&] {
                if (subEnd <= subBegin) return;
                DWORD previous;
                const auto before = ticks();
                const bool done = VirtualProtect(reinterpret_cast<void*>(subBegin), subEnd - subBegin, exec ? PAGE_EXECUTE_READ : PAGE_READONLY, &previous) != 0;
                protectTicks += ticks() - before;
                if (done) {
                    armed.push_back({subBegin, subEnd});
                } else {
                    for (auto page = subBegin; page < subEnd; page += PageBytes) {
                        auto* leaf = leafAt(state, page);
                        auto& word = wordOf(*leaf, page);
                        auto expected = word.load(std::memory_order_acquire);
                        if (stateOf(expected) == WtFaultArming && word.compare_exchange_strong(expected, make(0, false, WtFaultExcluded), std::memory_order_acq_rel)) ++excluded;
                    }
                }
                subBegin = subEnd = 0;
            };
            for (auto page = cursor; page < regionEnd; page += PageBytes) {
                auto* leaf = leafAt(state, page);
                auto& word = wordOf(*leaf, page);
                auto expected = word.load(std::memory_order_acquire);
                const auto claim = eligible ? (stampOf(expected) << WtFaultStampShift) | (exec ? WtFaultExecBit : 0) | WtFaultCheckedBit | WtFaultArming : make(0, false, WtFaultExcluded);
                const bool claimed = stateOf(expected) == WtFaultOpen && word.compare_exchange_strong(expected, claim, std::memory_order_acq_rel);
                if (claimed && !eligible) ++excluded;
                if (claimed && eligible) {
                    if (subEnd != page) {
                        protect();
                        subBegin = page;
                    }
                    subEnd = page + PageBytes;
                } else {
                    protect();
                }
            }
            protect();
            cursor = regionEnd;
        }
            segment = segmentEnd;
        }
    }
    // One stamp after every protect took effect: each store after it faults.
    const auto stamp = g_clock.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::uint64_t armedPages = 0;
    for (const auto& run : armed) {
        for (auto page = run.begin; page < run.end; page += PageBytes) {
            auto* leaf = leafAt(state, page);
            auto& word = wordOf(*leaf, page);
            auto expected = word.load(std::memory_order_acquire);
            if (stateOf(expected) != WtFaultArming) continue;
            if (!word.compare_exchange_strong(expected, (stamp << WtFaultStampShift) | (expected & WtFaultFlagMask) | WtFaultArmed, std::memory_order_acq_rel)) continue;
            leaf->notArmed.fetch_sub(1, std::memory_order_relaxed);
            auto newest = leaf->newest.load(std::memory_order_relaxed);
            while (newest < stamp && !leaf->newest.compare_exchange_weak(newest, stamp, std::memory_order_release, std::memory_order_relaxed)) {}
            ++armedPages;
        }
    }
    auto& counters = state.counters;
    counters.armCalls.fetch_add(1, std::memory_order_relaxed);
    counters.armedPages.fetch_add(armedPages, std::memory_order_relaxed);
    counters.armRuns.fetch_add(armed.size(), std::memory_order_relaxed);
    counters.armQueries.fetch_add(queries, std::memory_order_relaxed);
    counters.excluded.fetch_add(excluded, std::memory_order_relaxed);
    counters.keptHot.fetch_add(kept, std::memory_order_relaxed);
    counters.armProtectTicks.fetch_add(static_cast<std::uint64_t>(protectTicks), std::memory_order_relaxed);
    counters.armQueryTicks.fetch_add(static_cast<std::uint64_t>(queryTicks), std::memory_order_relaxed);
    counters.armTicks.fetch_add(static_cast<std::uint64_t>(ticks() - start), std::memory_order_relaxed);
    for (std::size_t i = 0; i < armed.size() && i < capacity; ++i) {
        reported[2 * i] = armed[i].begin;
        reported[2 * i + 1] = armed[i].end;
    }
    return armed.size();
}

}

std::size_t GuestWtFaultArm_nid_no_patch(std::uintptr_t* reported, std::size_t capacity) {
    return arm(reported, capacity, nullptr, 0, nullptr);
}

std::size_t GuestWtFaultArmKeepDirty_nid_no_patch(std::uintptr_t* reported, std::size_t capacity, std::uintptr_t* dirtyRuns, std::size_t dirtyCapacity, std::size_t* dirtyCount) {
    return arm(reported, capacity, dirtyRuns, dirtyCapacity, dirtyCount);
}

void GuestWtFaultForget_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    auto& state = *g_state;
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop)) return;
    bool any = false;
    for (auto at = first & ~((std::uintptr_t{1} << LeafShift) - 1); at < stop && !any; at += std::uintptr_t{1} << LeafShift) {
        const auto* leaf = leafAt(state, at);
        any = leaf != nullptr && leaf->candidates.load(std::memory_order_acquire) != 0;
    }
    if (!any) return;
    std::lock_guard lock(state.mutex);
    std::uint64_t forgotten = 0;
    for (auto page = first; page < stop; page += PageBytes) {
        auto* leaf = leafAt(state, page);
        if (leaf == nullptr) {
            page = std::min(stop, state.base + ((((page - state.base) >> LeafShift) + 1) << LeafShift)) - PageBytes;
            continue;
        }
        if (clearWatch(*leaf, page)) watchLog(page, WtWatchForget);
        const auto previous = wordOf(*leaf, page).exchange(WtFaultUntracked, std::memory_order_acq_rel);
        const auto current = stateOf(previous);
        if (current == WtFaultUntracked) continue;
        if (current == WtFaultArmed) leaf->notArmed.fetch_add(1, std::memory_order_relaxed);
        leaf->candidates.fetch_sub(1, std::memory_order_relaxed);
        ++forgotten;
    }
    state.counters.forgotten.fetch_add(forgotten, std::memory_order_relaxed);
    if (forgotten != 0) logRange(first, stop, WtFaultLogForgetBegin);
}

void GuestWtFaultHold_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    auto& state = *g_state;
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop)) return;
    std::unique_lock lock(state.mutex);
    state.held.emplace_back(first, stop);
    if (LazyServiceDepth::Get() == 0 && GuestWtFaultLazyPreservationEnabled_nid_no_patch()) {
        lock.unlock();
        if (preserveLazy(address, bytes)) {
            const LazyCauseScope cause(WtLazyCause::Hold);
            GuestWtFaultServeLazyRange_nid_no_patch(address, bytes);
        }
        lock.lock();
    }
    unwatchRange(state, first, stop, WtWatchHold);
    state.counters.holds.fetch_add(1, std::memory_order_relaxed);
    state.counters.heldOpened.fetch_add(openRange(state, first, stop, WtFaultLogHold), std::memory_order_relaxed);
}

void GuestWtFaultRelease_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    auto& state = *g_state;
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop)) return;
    std::lock_guard lock(state.mutex);
    const auto found = std::find(state.held.begin(), state.held.end(), std::pair{first, stop});
    if (found != state.held.end()) state.held.erase(found);
}

std::size_t GuestWtFaultOpen_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (!g_enabled.load(std::memory_order_acquire)) return 0;
    auto& state = *g_state;
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop)) return 0;
    std::unique_lock lock(state.mutex);
    if (LazyServiceDepth::Get() == 0 && GuestWtFaultLazyPreservationEnabled_nid_no_patch()) {
        state.held.emplace_back(first, stop);
        lock.unlock();
        if (preserveLazy(address, bytes)) {
            const LazyCauseScope cause(WtLazyCause::Open);
            GuestWtFaultServeLazyRange_nid_no_patch(address, bytes);
        }
        lock.lock();
        const auto held = std::find(state.held.begin(), state.held.end(), std::pair{first, stop});
        if (held != state.held.end()) state.held.erase(held);
    }
    unwatchRange(state, first, stop, WtWatchOpen);
    const auto opened = openRange(state, first, stop, WtFaultLogOpen);
    state.counters.opens.fetch_add(opened, std::memory_order_relaxed);
    return opened;
}

WtFaultAnswer GuestWtFaultChangedSince_nid_no_patch(std::uintptr_t address, std::size_t bytes, std::uint64_t since) {
    if (!g_enabled.load(std::memory_order_acquire)) return WtFaultAnswer::Untracked;
    auto& state = *g_state;
    state.counters.queries.fetch_add(1, std::memory_order_relaxed);
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop) || first != (address & ~(PageBytes - 1)) || stop < address + bytes) return WtFaultAnswer::Untracked;
    bool untracked = false;
    for (auto page = first; page < stop;) {
        const auto* leaf = leafAt(state, page);
        const auto leafEnd = state.base + ((((page - state.base) >> LeafShift) + 1) << LeafShift);
        if (leaf == nullptr) return WtFaultAnswer::Untracked;
        if ((page & ((std::uintptr_t{1} << LeafShift) - 1)) == 0 && leafEnd <= stop && leaf->notArmed.load(std::memory_order_acquire) == 0 && leaf->newest.load(std::memory_order_acquire) <= since) {
            page = leafEnd;
            continue;
        }
        const auto to = std::min(stop, leafEnd);
        for (; page < to; page += PageBytes) {
            const auto word = leaf->words[(page >> PageShift) & (LeafPages - 1)].load(std::memory_order_acquire);
            const auto current = stateOf(word);
            if (current == WtFaultArmed) {
                if (stampOf(word) > since) return WtFaultAnswer::Changed;
            } else if (current == WtFaultUntracked || current == WtFaultExcluded) {
                untracked = true;
            } else {
                return WtFaultAnswer::Changed;
            }
        }
    }
    return untracked ? WtFaultAnswer::Untracked : WtFaultAnswer::Unchanged;
}

bool GuestWtFaultSync_nid_no_patch(std::uintptr_t first, std::uintptr_t stop, std::uint64_t clock, void** dirty, std::size_t* dirtyCount, std::uintptr_t* open, std::size_t* openCount) {
    static_cast<void>(dirty);
    const auto openCapacity = *openCount;
    *dirtyCount = 0;
    *openCount = 0;
    if (!g_enabled.load(std::memory_order_acquire)) return false;
    auto& state = *g_state;
    if (first >= stop || (first & (PageBytes - 1)) != 0 || (stop & (PageBytes - 1)) != 0 || first < state.base || stop > state.end) return false;
    constexpr std::uintptr_t LeafBytes = std::uintptr_t{1} << LeafShift;
    for (auto page = first; page < stop;) {
        const auto* leaf = leafAt(state, page);
        const auto leafEnd = state.base + ((((page - state.base) >> LeafShift) + 1) << LeafShift);
        if (leaf == nullptr) return false;
        const auto leafReset = leaf->leafReset.load(std::memory_order_acquire);
        // A whole leaf with every page armed no later than its last whole-leaf sync holds nothing new.
        if (page == leaf->base && leafEnd <= stop && leaf->notArmed.load(std::memory_order_acquire) == 0 && leaf->newest.load(std::memory_order_acquire) <= leafReset) {
            page = leafEnd;
            continue;
        }
        for (const auto limit = std::min(stop, leafEnd); page < limit; page += PageBytes) {
            const auto index = (page >> PageShift) & (LeafPages - 1);
            const auto word = leaf->words[index].load(std::memory_order_acquire);
            const auto current = stateOf(word);
            if (current == WtFaultUntracked || current == WtFaultExcluded) return false;
            // Armed no later than its last sync: nothing stored since (a store would have faulted).
            // Armed after it, or not armed: walked by write watching (exact, not assumed written).
            if (current == WtFaultArmed && stampOf(word) <= std::max(leafReset, leaf->resets[index].load(std::memory_order_acquire))) continue;
            if (*openCount != 0 && open[2 * *openCount - 1] == page) {
                open[2 * *openCount - 1] = page + PageBytes;
                continue;
            }
            if (*openCount == openCapacity) return false;
            open[2 * *openCount] = page;
            open[2 * *openCount + 1] = page + PageBytes;
            ++*openCount;
        }
    }
    const auto raise = [](std::atomic<std::uint64_t>& slot, std::uint64_t value) {
        auto current = slot.load(std::memory_order_relaxed);
        while (current < value && !slot.compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed)) {}
    };
    for (auto page = first; page < stop;) {
        auto* leaf = leafAt(state, page);
        const auto leafEnd = state.base + ((((page - state.base) >> LeafShift) + 1) << LeafShift);
        if (page == leaf->base && leafEnd <= stop) {
            raise(leaf->leafReset, clock);
            page = leafEnd;
            continue;
        }
        for (const auto limit = std::min(stop, leafEnd); page < limit; page += PageBytes) raise(leaf->resets[(page >> PageShift) & (LeafPages - 1)], clock);
    }
    static_cast<void>(LeafBytes);
    return true;
}

std::uint64_t GuestWtFaultClock_nid_no_patch() {
    return g_clock.load(std::memory_order_acquire);
}

std::uint64_t GuestWtFaultClockBump_nid_no_patch() {
    return g_clock.fetch_add(1, std::memory_order_acq_rel) + 1;
}

std::uint64_t GuestWtFaultVerifyWalk_nid_no_patch(std::uintptr_t first, std::uintptr_t stop, void* const* pages, std::size_t count, std::uint64_t walkClock, WtFaultMismatch* mismatch) {
    if (!g_enabled.load(std::memory_order_acquire)) return 0;
    auto& state = *g_state;
    std::uint64_t mismatches = 0;
    std::uint64_t untracked = 0;
    for (std::size_t i = 0; i < count && state.verify; ++i) {
        const auto page = reinterpret_cast<std::uintptr_t>(pages[i]) & ~(PageBytes - 1);
        if (page < state.base || page >= state.end) {
            ++untracked;
            continue;
        }
        auto* leaf = leafAt(state, page);
        if (leaf == nullptr) {
            ++untracked;
            continue;
        }
        const auto index = (page >> PageShift) & (LeafPages - 1);
        const auto word = leaf->words[index].load(std::memory_order_acquire);
        const auto current = stateOf(word);
        if (current == WtFaultUntracked || current == WtFaultExcluded) {
            ++untracked;
            continue;
        }
        if (current != WtFaultArmed) continue;
        const auto reset = std::max(leaf->leafReset.load(std::memory_order_acquire), leaf->resets[index].load(std::memory_order_acquire));
        if (stampOf(word) > reset) continue;
        if (mismatches == 0 && mismatch != nullptr) *mismatch = {page, word, reset, g_clock.load(std::memory_order_acquire)};
        ++mismatches;
    }
    // The walk reset every page of the range at `walkClock`.
    std::uintptr_t from = 0;
    std::uintptr_t to = 0;
    if (clamp(state, first, stop > first ? stop - first : 0, from, to)) {
        const auto raise = [](std::atomic<std::uint64_t>& slot, std::uint64_t value) {
            auto current = slot.load(std::memory_order_relaxed);
            while (current < value && !slot.compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed)) {}
        };
        for (auto page = from; page < to;) {
            auto* leaf = leafAt(state, page);
            const auto leafEnd = state.base + ((((page - state.base) >> LeafShift) + 1) << LeafShift);
            if (leaf == nullptr) {
                page = std::min(to, leafEnd);
                continue;
            }
            if (page == leaf->base && leafEnd <= to) {
                raise(leaf->leafReset, walkClock);
                page = leafEnd;
                continue;
            }
            for (const auto limit = std::min(to, leafEnd); page < limit; page += PageBytes) raise(leaf->resets[(page >> PageShift) & (LeafPages - 1)], walkClock);
        }
    }
    if (!state.verify) return 0;
    auto& counters = state.counters;
    counters.verifyWalks.fetch_add(1, std::memory_order_relaxed);
    counters.verifyDirty.fetch_add(count, std::memory_order_relaxed);
    counters.verifyUntracked.fetch_add(untracked, std::memory_order_relaxed);
    counters.verifyMismatches.fetch_add(mismatches, std::memory_order_relaxed);
    return mismatches;
}

unsigned long GuestWtFaultGuestProtection_nid_no_patch(std::uintptr_t address, std::size_t bytes, unsigned long protection) {
    if (!g_enabled.load(std::memory_order_acquire)) return protection;
    if (protection == PAGE_NOACCESS && g_watchEver.load(std::memory_order_acquire)) {
        auto& state = *g_state;
        std::uintptr_t first = 0;
        std::uintptr_t stop = 0;
        if (!clamp(state, address, bytes, first, stop)) return protection;
        for (auto page = first; page < stop; page += PageBytes) {
            const auto* leaf = leafAt(state, page);
            if (leaf == nullptr || !watchedPage(*leaf, page)) return protection;
        }
        return PAGE_READWRITE;
    }
    if (protection != PAGE_READONLY && protection != PAGE_EXECUTE_READ) return protection;
    auto& state = *g_state;
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop)) return protection;
    for (auto page = first; page < stop; page += PageBytes) {
        const auto* leaf = leafAt(state, page);
        if (leaf == nullptr || !protectedState(stateOf(leaf->words[(page >> PageShift) & (LeafPages - 1)].load(std::memory_order_acquire)))) return protection;
    }
    return protection == PAGE_EXECUTE_READ ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
}

bool GuestWtFaultProtects_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (!g_enabled.load(std::memory_order_acquire)) return false;
    auto& state = *g_state;
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop)) return false;
    for (auto page = first; page < stop; page += PageBytes) {
        const auto* leaf = leafAt(state, page);
        if (leaf == nullptr) {
            page = std::min(stop, state.base + ((((page - state.base) >> LeafShift) + 1) << LeafShift)) - PageBytes;
            continue;
        }
        if (protectedState(stateOf(leaf->words[(page >> PageShift) & (LeafPages - 1)].load(std::memory_order_acquire))) || watchedPage(*leaf, page)) return true;
    }
    return false;
}

void GuestWtFaultCounters_nid_no_patch(WtFaultCounters* out) {
    *out = {};
    if (!g_enabled.load(std::memory_order_acquire)) return;
    const auto& state = *g_state;
    const auto& c = state.counters;
    const auto ns = [&](const std::atomic<std::uint64_t>& value) { return static_cast<std::uint64_t>(static_cast<double>(value.load(std::memory_order_relaxed)) * state.ticksToNs); };
    out->faults = c.faults.load(std::memory_order_relaxed);
    out->driverFaults = c.driverFaults.load(std::memory_order_relaxed);
    out->handlerNs = ns(c.handlerTicks);
    out->driverHandlerNs = ns(c.driverHandlerTicks);
    out->retries = c.retries.load(std::memory_order_relaxed);
    out->foreign = c.foreign.load(std::memory_order_relaxed);
    out->armCalls = c.armCalls.load(std::memory_order_relaxed);
    out->armedPages = c.armedPages.load(std::memory_order_relaxed);
    out->armRuns = c.armRuns.load(std::memory_order_relaxed);
    out->armNs = ns(c.armTicks);
    out->armProtectNs = ns(c.armProtectTicks);
    out->armQueries = c.armQueries.load(std::memory_order_relaxed);
    out->armLockNs = ns(c.armLockTicks);
    out->armScanNs = ns(c.armScanTicks);
    out->armQueryNs = ns(c.armQueryTicks);
    out->keptDirty = c.keptDirty.load(std::memory_order_relaxed);
    out->armDirtyNs = ns(c.dirtyTicks);
    out->armLeaves = c.armLeaves.load(std::memory_order_relaxed);
    out->leaves = c.leaves.load(std::memory_order_relaxed);
    out->excluded = c.excluded.load(std::memory_order_relaxed);
    out->keptHot = c.keptHot.load(std::memory_order_relaxed);
    out->holds = c.holds.load(std::memory_order_relaxed);
    out->heldOpened = c.heldOpened.load(std::memory_order_relaxed);
    out->opens = c.opens.load(std::memory_order_relaxed);
    out->forgotten = c.forgotten.load(std::memory_order_relaxed);
    out->tracked = c.tracked.load(std::memory_order_relaxed);
    out->verifyWalks = c.verifyWalks.load(std::memory_order_relaxed);
    out->verifyDirty = c.verifyDirty.load(std::memory_order_relaxed);
    out->verifyUntracked = c.verifyUntracked.load(std::memory_order_relaxed);
    out->verifyMismatches = c.verifyMismatches.load(std::memory_order_relaxed);
    out->queries = c.queries.load(std::memory_order_relaxed);
    out->logAppends = c.logAppends.load(std::memory_order_relaxed);
    out->logLost = c.logLost.load(std::memory_order_relaxed);
    out->coldRearmed = c.coldRearmed.load(std::memory_order_relaxed);
    out->watchFaults = g_watchFaults.load(std::memory_order_relaxed);
    out->watchHandlerNs = ns(g_watchFaultTicks);
    out->watchedPages = g_watchedPages.load(std::memory_order_relaxed);
    out->openSplitRuns = c.openSplitRuns.load(std::memory_order_relaxed);
    out->openSplitPieces = c.openSplitPieces.load(std::memory_order_relaxed);
    out->openRefused = c.openRefused.load(std::memory_order_relaxed);
}

bool GuestWtFaultSyncedClean_nid_no_patch(std::uintptr_t address) {
    if (!g_enabled.load(std::memory_order_acquire)) return false;
    const auto& state = *g_state;
    if (address < state.base || address >= state.end) return false;
    auto* leaf = leafAt(state, address);
    if (leaf == nullptr) return false;
    const auto index = (address >> PageShift) & (LeafPages - 1);
    const auto word = leaf->words[index].load(std::memory_order_acquire);
    return stateOf(word) == WtFaultArmed && stampOf(word) <= std::max(leaf->leafReset.load(std::memory_order_acquire), leaf->resets[index].load(std::memory_order_acquire));
}

std::uint64_t GuestWtFaultWord_nid_no_patch(std::uintptr_t address) {
    if (!g_enabled.load(std::memory_order_acquire)) return 0;
    const auto& state = *g_state;
    if (address < state.base || address >= state.end) return 0;
    const auto* leaf = leafAt(state, address);
    return leaf == nullptr ? 0 : leaf->words[(address >> PageShift) & (LeafPages - 1)].load(std::memory_order_acquire);
}

std::uint64_t GuestWtFaultLogHead_nid_no_patch() {
    return g_logHead.load(std::memory_order_acquire);
}

std::size_t GuestWtFaultLogRead_nid_no_patch(std::uint64_t* cursor, std::uint64_t* entries, std::size_t capacity, bool* lost) {
    *lost = false;
    const auto head = g_logHead.load(std::memory_order_acquire);
    if (head - *cursor > WtFaultLogSlots) {
        *lost = true;
        *cursor = head;
        if (g_state != nullptr) g_state->counters.logLost.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    std::size_t count = 0;
    while (*cursor < head && count < capacity) {
        const auto& slot = g_log[*cursor % WtFaultLogSlots];
        const auto want = *cursor + 1;
        unsigned spins = 0;
        auto seq = slot.seq.load(std::memory_order_acquire);
        while (seq < want && spins < 4096) {
            if (spins < 1024) YieldProcessor();
            else SwitchToThread();
            ++spins;
            seq = slot.seq.load(std::memory_order_acquire);
        }
        if (seq < want) break;
        const auto entry = slot.entry.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq != want || slot.seq.load(std::memory_order_relaxed) != want) {
            *lost = true;
            *cursor = g_logHead.load(std::memory_order_acquire);
            if (g_state != nullptr) g_state->counters.logLost.fetch_add(1, std::memory_order_relaxed);
            return count;
        }
        entries[count++] = entry;
        ++*cursor;
    }
    return count;
}

void GuestWtFaultSetCold_nid_no_patch(std::uintptr_t address, std::size_t bytes, bool cold) {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    GuestWtFaultTrack_nid_no_patch(address, bytes);
    auto& state = *g_state;
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop)) return;
    std::lock_guard lock(state.mutex);
    for (auto page = first; page < stop; page += PageBytes) {
        auto& leaf = leafFor(state, page);
        const auto index = (page >> PageShift) & (LeafPages - 1);
        const auto bit = std::uint64_t{1} << (index % 64);
        if (cold) leaf.cold[index / 64].fetch_or(bit, std::memory_order_acq_rel);
        else leaf.cold[index / 64].fetch_and(~bit, std::memory_order_acq_rel);
    }
}

void GuestWtFaultLogAppendForTest_nid_no_patch(std::uint64_t entry) {
    logAppend(entry, 0, false);
}

bool GuestWtFaultStoredLastFrame_nid_no_patch(std::uintptr_t address) {
    if (!g_enabled.load(std::memory_order_acquire)) return false;
    const auto& state = *g_state;
    if (address < state.base || address >= state.end) return false;
    const auto* leaf = leafAt(state, address);
    if (leaf == nullptr) return false;
    const auto index = (address >> PageShift) & (LeafPages - 1);
    return ((leaf->storedLast[index / 64].load(std::memory_order_acquire) >> (index % 64)) & 1) != 0;
}

namespace {

std::size_t watchCauses(std::uintptr_t address, std::size_t bytes, std::uint64_t* causes, bool lazy) {
    if (!g_enabled.load(std::memory_order_acquire)) return 0;
    auto& state = *g_state;
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop)) return 0;
    std::lock_guard lock(state.mutex);
    g_watchEver.store(true, std::memory_order_release);
    std::vector<WatchClaim> claims;
    const auto skip = [&](std::size_t cause, std::uint64_t pages) {
        if (causes != nullptr) causes[cause] += pages;
    };
    std::uintptr_t queriedEnd = 0;
    bool queriedOk = false;
    for (auto page = first; page < stop; page += PageBytes) {
        auto* leaf = leafAt(state, page);
        if (leaf == nullptr) {
            const auto next = std::min(stop, state.base + ((((page - state.base) >> LeafShift) + 1) << LeafShift));
            skip(WtWatchSkipNoLeaf, (next - page) / PageBytes);
            page = next - PageBytes;
            continue;
        }
        if (watchedPage(*leaf, page)) {
            skip(WtWatchSkipWatched, 1);
            continue;
        }
        if (heldPage(state, page)) {
            skip(WtWatchSkipHeld, 1);
            continue;
        }
        auto& word = wordOf(*leaf, page);
        auto value = word.load(std::memory_order_acquire);
        const auto current = stateOf(value);
        if (current == WtFaultUntracked) {
            skip(WtWatchSkipUntracked, 1);
            continue;
        }
        if (current == WtFaultExcluded) {
            skip(WtWatchSkipExcluded, 1);
            continue;
        }
        if (current != WtFaultArmed && current != WtFaultOpen) {
            skip(WtWatchSkipBusy, 1);
            continue;
        }
        if ((value & WtFaultCheckedBit) == 0) {
            if (current == WtFaultOpen && page >= queriedEnd) {
                MEMORY_BASIC_INFORMATION memory{};
                queriedOk = VirtualQuery(reinterpret_cast<void*>(page), &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT && memory.Type == MEM_PRIVATE && memory.Protect == PAGE_READWRITE;
                queriedEnd = queriedOk ? reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize : page + PageBytes;
            }
            const bool eligible = current == WtFaultOpen && queriedOk;
            if (!eligible || !word.compare_exchange_strong(value, value | WtFaultCheckedBit, std::memory_order_acq_rel)) {
                skip(WtWatchSkipUnchecked, 1);
                continue;
            }
            value |= WtFaultCheckedBit;
        }
        if (isExec(value)) {
            skip(WtWatchSkipExec, 1);
            continue;
        }
        if (!word.compare_exchange_strong(value, (value & ~WtFaultStateMask) | WtFaultOpening, std::memory_order_acq_rel)) {
            skip(WtWatchSkipBusy, 1);
            continue;
        }
        if (current == WtFaultArmed) leaf->notArmed.fetch_add(1, std::memory_order_relaxed);
        claims.push_back({page, value, leaf});
    }
    std::size_t watched = 0;
    for (std::size_t i = 0; i < claims.size();) {
        auto j = i + 1;
        while (j < claims.size() && claims[j].page == claims[j - 1].page + PageBytes) ++j;
        for (auto k = i; k < j; ++k) {
            const auto index = (claims[k].page >> PageShift) & (LeafPages - 1);
            if (lazy) claims[k].leaf->lazy[index / 64].fetch_or(std::uint64_t{1} << (index % 64), std::memory_order_acq_rel);
            claims[k].leaf->watch[index / 64].fetch_or(std::uint64_t{1} << (index % 64), std::memory_order_acq_rel);
        }
        g_watchedPages.fetch_add(j - i, std::memory_order_relaxed);
        auto done = j - i;
        protectPieces(claims[i].page, claims[j - 1].page + PageBytes, PAGE_NOACCESS, g_watchSplit.load(std::memory_order_relaxed), g_watchSplitRuns, g_watchSplitPieces, [&](std::uintptr_t begin, std::uintptr_t end) {
            for (auto k = i; k < j; ++k) {
                if (claims[k].page < begin || claims[k].page >= end) continue;
                clearWatch(*claims[k].leaf, claims[k].page);
                skip(WtWatchSkipProtect, 1);
                --done;
            }
        });
        watched += done;
        i = j;
    }
    releaseClaims(claims);
    return watched;
}

}

std::size_t GuestWtFaultWatchCauses_nid_no_patch(std::uintptr_t address, std::size_t bytes, std::uint64_t* causes) {
    return watchCauses(address, bytes, causes, false);
}

std::size_t GuestWtFaultWatch_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    return GuestWtFaultWatchCauses_nid_no_patch(address, bytes, nullptr);
}

std::size_t GuestWtFaultWatchLazy_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    return watchCauses(address, bytes, nullptr, true);
}

std::size_t GuestWtFaultWatchLazyCauses_nid_no_patch(std::uintptr_t address, std::size_t bytes, std::uint64_t* causes) {
    return watchCauses(address, bytes, causes, true);
}

namespace {

std::size_t countWatchedPages(std::uintptr_t address, std::size_t bytes, bool lazy) {
    if (!g_enabled.load(std::memory_order_acquire) || !g_watchEver.load(std::memory_order_acquire)) return 0;
    auto& state = *g_state;
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop)) return 0;
    std::size_t watched = 0;
    for (auto page = first; page < stop; page += PageBytes) {
        const auto* leaf = leafAt(state, page);
        if (leaf != nullptr && watchedPage(*leaf, page) && (!lazy || lazyPage(*leaf, page))) ++watched;
    }
    return watched;
}

}

std::size_t GuestWtFaultWatchedPages_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    return countWatchedPages(address, bytes, false);
}

std::size_t GuestWtFaultLazyPages_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    return countWatchedPages(address, bytes, true);
}

void GuestWtFaultSetLazyService_nid_no_patch(void (*service)(std::uintptr_t page, bool write)) {
    if (g_enabled.load(std::memory_order_acquire)) static_cast<void>(LazyServiceDepth::Get());
    if (g_enabled.load(std::memory_order_acquire)) static_cast<void>(LazyCause::Get());
    g_lazyService.store(service, std::memory_order_release);
}

void GuestWtFaultServeLazyRange_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (!g_enabled.load(std::memory_order_acquire) || !g_watchEver.load(std::memory_order_acquire) || g_lazyService.load(std::memory_order_acquire) == nullptr || LazyServiceDepth::Get() != 0) return;
    auto& state = *g_state;
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop)) return;
    const LazyCauseScope cause(LazyCause::Get() == 0 ? WtLazyCause::Range : static_cast<WtLazyCause>(LazyCause::Get()));
    for (auto page = first; page < stop; page += PageBytes) {
        auto* leaf = leafAt(state, page);
        if (leaf == nullptr) {
            page = std::min(stop, state.base + ((((page - state.base) >> LeafShift) + 1) << LeafShift)) - PageBytes;
            continue;
        }
        if (lazyPage(*leaf, page)) serveLazy(*leaf, page, true);
    }
}

WtLazyCause GuestWtFaultLazyServiceCause_nid_no_patch() {
    return g_enabled.load(std::memory_order_acquire) ? static_cast<WtLazyCause>(LazyCause::Get()) : WtLazyCause::Fault;
}

void GuestWtFaultSetLazyPreservation_nid_no_patch(bool (*predicate)(std::uintptr_t, std::size_t)) {
    lazyPreservation.store(predicate, std::memory_order_release);
}

bool GuestWtFaultLazyPreservationEnabled_nid_no_patch() {
    return lazyPreservation.load(std::memory_order_acquire) != nullptr;
}

void GuestWtFaultLazyCounters_nid_no_patch(std::uint64_t* services, std::uint64_t* nested) {
    *services = g_lazyServices.load(std::memory_order_relaxed);
    *nested = g_lazyNested.load(std::memory_order_relaxed);
}

bool GuestWtFaultEnterLazyService_nid_no_patch() {
    if (!g_enabled.load(std::memory_order_acquire)) return false;
    if (LazyServiceDepth::Get() != 0) return false;
    LazyServiceDepth::Set(1);
    return true;
}

void GuestWtFaultLeaveLazyService_nid_no_patch() {
    LazyServiceDepth::Set(0);
}

void GuestWtFaultNoteFaultContext_nid_no_patch(std::uintptr_t ip, std::uintptr_t sp) {
    if (!g_enabled.load(std::memory_order_acquire) || !g_watchEver.load(std::memory_order_acquire)) return;
    FaultIp::Set(ip);
    FaultSp::Set(sp);
}

void GuestWtFaultFaultContext_nid_no_patch(std::uintptr_t* ip, std::uintptr_t* sp) {
    *ip = g_enabled.load(std::memory_order_acquire) ? FaultIp::Get() : 0;
    *sp = g_enabled.load(std::memory_order_acquire) ? FaultSp::Get() : 0;
}

void GuestWtFaultSetWatchSplit_nid_no_patch(bool split) {
    if (g_watchSplit.load(std::memory_order_relaxed) != split) g_watchSplit.store(split, std::memory_order_relaxed);
}

void GuestWtFaultWatchSplitCounters_nid_no_patch(std::uint64_t* runs, std::uint64_t* pieces) {
    *runs = g_watchSplitRuns.load(std::memory_order_relaxed);
    *pieces = g_watchSplitPieces.load(std::memory_order_relaxed);
}

std::size_t GuestWtFaultUnwatch_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (!g_enabled.load(std::memory_order_acquire) || !g_watchEver.load(std::memory_order_acquire)) return 0;
    auto& state = *g_state;
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop)) return 0;
    std::unique_lock lock(state.mutex);
    if (LazyServiceDepth::Get() == 0 && GuestWtFaultLazyPreservationEnabled_nid_no_patch()) {
        state.held.emplace_back(first, stop);
        lock.unlock();
        if (preserveLazy(address, bytes)) {
            const LazyCauseScope cause(WtLazyCause::Unwatch);
            GuestWtFaultServeLazyRange_nid_no_patch(address, bytes);
        }
        lock.lock();
        const auto held = std::find(state.held.begin(), state.held.end(), std::pair{first, stop});
        if (held != state.held.end()) state.held.erase(held);
    }
    return unwatchRange(state, first, stop, 0);
}

bool GuestWtFaultHandleRead_nid_no_patch(std::uintptr_t address) {
    if (!g_enabled.load(std::memory_order_acquire) || !g_watchEver.load(std::memory_order_acquire)) return false;
    auto& state = *g_state;
    if (address < state.base || address >= state.end) return false;
    auto* leaf = leafAt(state, address);
    if (leaf == nullptr) return false;
    const auto start = ticks();
    const auto page = address & ~(PageBytes - 1);
    if (serveLazy(*leaf, page, false)) return true;
    for (unsigned spins = 0;; ++spins) {
        if (watchedPage(*leaf, page)) {
            unwatchPage(*leaf, page, WtWatchRead);
            g_watchFaults.fetch_add(1, std::memory_order_relaxed);
            g_watchFaultTicks.fetch_add(static_cast<std::uint64_t>(ticks() - start), std::memory_order_relaxed);
            return true;
        }
        const auto current = stateOf(wordOf(*leaf, page).load(std::memory_order_acquire));
        if (current == WtFaultOpening || current == WtFaultArming) {
            spinOnce(spins);
            continue;
        }
        if (current != WtFaultArmed && current != WtFaultOpen) return false;
        MEMORY_BASIC_INFORMATION memory{};
        return VirtualQuery(reinterpret_cast<void*>(page), &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT && (memory.Protect & 0xffu) != PAGE_NOACCESS && (memory.Protect & PAGE_GUARD) == 0;
    }
}

std::uint64_t GuestWtFaultWatchLogHead_nid_no_patch() {
    return g_watchHead.load(std::memory_order_acquire);
}

std::size_t GuestWtFaultWatchLogRead_nid_no_patch(std::uint64_t* cursor, std::uint64_t* entries, std::uint64_t* stamps, std::size_t capacity, bool* lost) {
    *lost = false;
    const auto head = g_watchHead.load(std::memory_order_acquire);
    if (head - *cursor > WatchLogSlots) {
        *lost = true;
        *cursor = head;
        return 0;
    }
    std::size_t count = 0;
    while (*cursor < head && count < capacity) {
        const auto& slot = g_watchLog[*cursor % WatchLogSlots];
        const auto want = *cursor + 1;
        unsigned spins = 0;
        auto seq = slot.seq.load(std::memory_order_acquire);
        while (seq < want && spins < 4096) {
            spinOnce(spins);
            ++spins;
            seq = slot.seq.load(std::memory_order_acquire);
        }
        if (seq < want) break;
        const auto entry = slot.entry.load(std::memory_order_relaxed);
        const auto stamp = slot.ticks.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.seq.load(std::memory_order_relaxed) != want) {
            *lost = true;
            *cursor = g_watchHead.load(std::memory_order_acquire);
            return count;
        }
        entries[count] = entry;
        stamps[count] = stamp;
        ++count;
        ++*cursor;
    }
    return count;
}

std::uint64_t GuestWtFaultTicks_nid_no_patch(double* nsPerTick) {
    if (nsPerTick != nullptr) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        *nsPerTick = 1e9 / static_cast<double>(frequency.QuadPart);
    }
    return static_cast<std::uint64_t>(ticks());
}

#else

void GuestWtFaultEnable_nid_no_patch(bool, std::uint64_t) {}
bool GuestWtFaultEnabled_nid_no_patch() { return false; }
bool GuestWtFaultHandle_nid_no_patch(std::uintptr_t) { return false; }
void GuestWtFaultMarkDriverThread_nid_no_patch() {}
void GuestWtFaultTrack_nid_no_patch(std::uintptr_t, std::size_t) {}
void GuestWtFaultNoteDriverStore_nid_no_patch(std::uintptr_t, std::size_t) {}
void GuestWtFaultNoteStored_nid_no_patch(std::uintptr_t, std::size_t) {}
std::size_t GuestWtFaultArm_nid_no_patch(std::uintptr_t*, std::size_t) { return 0; }
std::size_t GuestWtFaultArmKeepDirty_nid_no_patch(std::uintptr_t*, std::size_t, std::uintptr_t*, std::size_t, std::size_t* dirtyCount) {
    *dirtyCount = 0;
    return 0;
}
void GuestWtFaultForget_nid_no_patch(std::uintptr_t, std::size_t) {}
void GuestWtFaultHold_nid_no_patch(std::uintptr_t, std::size_t) {}
void GuestWtFaultRelease_nid_no_patch(std::uintptr_t, std::size_t) {}
std::size_t GuestWtFaultOpen_nid_no_patch(std::uintptr_t, std::size_t) { return 0; }
WtFaultAnswer GuestWtFaultChangedSince_nid_no_patch(std::uintptr_t, std::size_t, std::uint64_t) { return WtFaultAnswer::Untracked; }
bool GuestWtFaultSync_nid_no_patch(std::uintptr_t, std::uintptr_t, std::uint64_t, void**, std::size_t* dirtyCount, std::uintptr_t*, std::size_t* openCount) {
    *dirtyCount = 0;
    *openCount = 0;
    return false;
}
std::uint64_t GuestWtFaultClock_nid_no_patch() { return 0; }
std::uint64_t GuestWtFaultClockBump_nid_no_patch() { return 0; }
std::uint64_t GuestWtFaultVerifyWalk_nid_no_patch(std::uintptr_t, std::uintptr_t, void* const*, std::size_t, std::uint64_t, WtFaultMismatch*) { return 0; }
unsigned long GuestWtFaultGuestProtection_nid_no_patch(std::uintptr_t, std::size_t, unsigned long protection) { return protection; }
bool GuestWtFaultProtects_nid_no_patch(std::uintptr_t, std::size_t) { return false; }
void GuestWtFaultCounters_nid_no_patch(WtFaultCounters* counters) { *counters = {}; }
std::uint64_t GuestWtFaultWord_nid_no_patch(std::uintptr_t) { return 0; }
bool GuestWtFaultSyncedClean_nid_no_patch(std::uintptr_t) { return false; }
std::uint64_t GuestWtFaultLogHead_nid_no_patch() { return 0; }
std::size_t GuestWtFaultLogRead_nid_no_patch(std::uint64_t*, std::uint64_t*, std::size_t, bool* lost) {
    *lost = false;
    return 0;
}
void GuestWtFaultSetCold_nid_no_patch(std::uintptr_t, std::size_t, bool) {}
void GuestWtFaultLogAppendForTest_nid_no_patch(std::uint64_t) {}
bool GuestWtFaultStoredLastFrame_nid_no_patch(std::uintptr_t) { return false; }
std::size_t GuestWtFaultWatch_nid_no_patch(std::uintptr_t, std::size_t) { return 0; }
std::size_t GuestWtFaultWatchCauses_nid_no_patch(std::uintptr_t, std::size_t, std::uint64_t*) { return 0; }
std::size_t GuestWtFaultUnwatch_nid_no_patch(std::uintptr_t, std::size_t) { return 0; }
std::size_t GuestWtFaultWatchLazy_nid_no_patch(std::uintptr_t, std::size_t) { return 0; }
std::size_t GuestWtFaultWatchLazyCauses_nid_no_patch(std::uintptr_t, std::size_t, std::uint64_t*) { return 0; }
std::size_t GuestWtFaultWatchedPages_nid_no_patch(std::uintptr_t, std::size_t) { return 0; }
std::size_t GuestWtFaultLazyPages_nid_no_patch(std::uintptr_t, std::size_t) { return 0; }
void GuestWtFaultSetLazyService_nid_no_patch(void (*)(std::uintptr_t, bool)) {}
void GuestWtFaultServeLazyRange_nid_no_patch(std::uintptr_t, std::size_t) {}
WtLazyCause GuestWtFaultLazyServiceCause_nid_no_patch() { return WtLazyCause::Fault; }
void GuestWtFaultSetLazyPreservation_nid_no_patch(bool (*)(std::uintptr_t, std::size_t)) {}
bool GuestWtFaultLazyPreservationEnabled_nid_no_patch() { return false; }
void GuestWtFaultLazyCounters_nid_no_patch(std::uint64_t* services, std::uint64_t* nested) {
    *services = 0;
    *nested = 0;
}
void GuestWtFaultSetWatchSplit_nid_no_patch(bool) {}
void GuestWtFaultNoteFaultContext_nid_no_patch(std::uintptr_t, std::uintptr_t) {}
void GuestWtFaultFaultContext_nid_no_patch(std::uintptr_t* ip, std::uintptr_t* sp) {
    *ip = 0;
    *sp = 0;
}
bool GuestWtFaultEnterLazyService_nid_no_patch() { return false; }
void GuestWtFaultLeaveLazyService_nid_no_patch() {}
void GuestWtFaultWatchSplitCounters_nid_no_patch(std::uint64_t* runs, std::uint64_t* pieces) {
    *runs = 0;
    *pieces = 0;
}
bool GuestWtFaultHandleRead_nid_no_patch(std::uintptr_t) { return false; }
std::uint64_t GuestWtFaultWatchLogHead_nid_no_patch() { return 0; }
std::size_t GuestWtFaultWatchLogRead_nid_no_patch(std::uint64_t*, std::uint64_t*, std::uint64_t*, std::size_t, bool* lost) {
    *lost = false;
    return 0;
}
std::uint64_t GuestWtFaultTicks_nid_no_patch(double* nsPerTick) {
    if (nsPerTick != nullptr) *nsPerTick = 1.0;
    return 0;
}

#endif

}
