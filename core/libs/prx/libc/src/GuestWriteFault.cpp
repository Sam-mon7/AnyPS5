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
    std::atomic<std::uint64_t> armLockTicks{0}, armScanTicks{0}, armLeaves{0}, leaves{0}, armQueryTicks{0};
    std::atomic<std::uint64_t> holds{0}, heldOpened{0}, opens{0}, forgotten{0}, tracked{0};
    std::atomic<std::uint64_t> verifyWalks{0}, verifyDirty{0}, verifyUntracked{0}, verifyMismatches{0}, queries{0};
};

struct State {
    std::uintptr_t base = 0;
    std::uintptr_t end = 0;
    std::size_t leafCount = 0;
    std::atomic<Leaf*>* leaves = nullptr;
    bool verify = false;
    bool driverHot = true;
    std::mutex mutex;
    Leaf* allocated = nullptr;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> held;
    struct Run {
        std::uintptr_t begin;
        std::uintptr_t end;
    };
    std::vector<Run> runs;
    std::vector<Run> armedRuns;
    Counters counters;
    double ticksToNs = 1.0;
};

// Constant-initialized: the handler reads them for every access violation in the process, also
// before (or without) GuestWtFaultEnable.
std::atomic<bool> g_enabled{false};
State* g_state = nullptr;
std::atomic<std::uint64_t> g_clock{1};

struct DriverThreadTag {};
using DriverThread = HostThreadSlot<std::uintptr_t, DriverThreadTag>;

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

// Opening -> Open after restoring the guest protection of [begin, end) (all words already Opening,
// all with the same exec bit). Under the mutex or from the handler. False when the protect failed:
// the words go back to Untracked (the page is not what the tracker armed any more).
bool finishOpening(State& state, std::uintptr_t begin, std::uintptr_t end, bool exec) {
    DWORD previous;
    const bool done = VirtualProtect(reinterpret_cast<void*>(begin), end - begin, guestProtection(exec), &previous) != 0;
    for (auto page = begin; page < end; page += PageBytes) {
        auto* leaf = leafAt(state, page);
        auto& word = wordOf(*leaf, page);
        auto expected = word.load(std::memory_order_acquire);
        if (stateOf(expected) != WtFaultOpening) continue;
        if (done) {
            if (word.compare_exchange_strong(expected, (expected & ~WtFaultStateMask) | WtFaultOpen, std::memory_order_acq_rel)) leaf->open.store(1, std::memory_order_release);
        } else if (word.compare_exchange_strong(expected, WtFaultUntracked, std::memory_order_acq_rel)) {
            leaf->candidates.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    return done;
}

// Opens every armed or arming page of [first, stop) (under the mutex): returns the pages opened.
std::size_t openRange(State& state, std::uintptr_t first, std::uintptr_t stop) {
    std::size_t opened = 0;
    std::uintptr_t runBegin = 0;
    std::uintptr_t runEnd = 0;
    bool runExec = false;
    const auto flush = [&] {
        if (runEnd > runBegin) finishOpening(state, runBegin, runEnd, runExec);
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
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        state->ticksToNs = 1e9 / static_cast<double>(frequency.QuadPart);
        // The thread slot's TLS index is allocated here, never inside the handler.
        static_cast<void>(DriverThread::Get());
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

bool GuestWtFaultHandle_nid_no_patch(std::uintptr_t address) {
    if (!g_enabled.load(std::memory_order_acquire)) return false;
    auto& state = *g_state;
    if (address < state.base || address >= state.end) return false;
    auto* leaf = leafAt(state, address);
    if (leaf == nullptr) return false;
    const auto start = ticks();
    const auto page = address & ~(PageBytes - 1);
    auto& word = wordOf(*leaf, page);
    bool waited = false;
    for (unsigned spins = 0;; ++spins) {
        auto value = word.load(std::memory_order_acquire);
        const auto current = stateOf(value);
        if (current == WtFaultArmed || current == WtFaultArming) {
            const auto opening = (g_clock.load(std::memory_order_acquire) << WtFaultStampShift) | (value & WtFaultFlagMask) | WtFaultOpening;
            if (!word.compare_exchange_weak(value, opening, std::memory_order_acq_rel)) continue;
            if (current == WtFaultArmed) leaf->notArmed.fetch_add(1, std::memory_order_relaxed);
            if (!finishOpening(state, page, page + PageBytes, isExec(value))) return false;
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

void GuestWtFaultNoteDriverStore_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    auto& state = *g_state;
    if (!state.driverHot) return;
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

std::size_t GuestWtFaultArm_nid_no_patch(std::uintptr_t* reported, std::size_t capacity) {
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
        if (leaf->open.exchange(0, std::memory_order_acq_rel) == 0) continue;
        ++scanned;
        bool stillOpen = false;
        for (std::size_t group = 0; group < LeafPages / 64; ++group) {
            const auto hot = leaf->hot[group].exchange(0, std::memory_order_acq_rel);
            for (std::size_t bit = 0; bit < 64; ++bit) {
                const auto index = group * 64 + bit;
                const auto page = leaf->base + (index << PageShift);
                if (stateOf(leaf->words[index].load(std::memory_order_acquire)) != WtFaultOpen) continue;
                if (((hot >> bit) & 1) != 0 || heldPage(state, page)) {
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
        const auto previous = wordOf(*leaf, page).exchange(WtFaultUntracked, std::memory_order_acq_rel);
        const auto current = stateOf(previous);
        if (current == WtFaultUntracked) continue;
        if (current == WtFaultArmed) leaf->notArmed.fetch_add(1, std::memory_order_relaxed);
        leaf->candidates.fetch_sub(1, std::memory_order_relaxed);
        ++forgotten;
    }
    state.counters.forgotten.fetch_add(forgotten, std::memory_order_relaxed);
}

void GuestWtFaultHold_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    auto& state = *g_state;
    std::uintptr_t first = 0;
    std::uintptr_t stop = 0;
    if (!clamp(state, address, bytes, first, stop)) return;
    std::lock_guard lock(state.mutex);
    state.held.emplace_back(first, stop);
    state.counters.holds.fetch_add(1, std::memory_order_relaxed);
    state.counters.heldOpened.fetch_add(openRange(state, first, stop), std::memory_order_relaxed);
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
    std::lock_guard lock(state.mutex);
    const auto opened = openRange(state, first, stop);
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
        if (protectedState(stateOf(leaf->words[(page >> PageShift) & (LeafPages - 1)].load(std::memory_order_acquire)))) return true;
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
}

std::uint64_t GuestWtFaultWord_nid_no_patch(std::uintptr_t address) {
    if (!g_enabled.load(std::memory_order_acquire)) return 0;
    const auto& state = *g_state;
    if (address < state.base || address >= state.end) return 0;
    const auto* leaf = leafAt(state, address);
    return leaf == nullptr ? 0 : leaf->words[(address >> PageShift) & (LeafPages - 1)].load(std::memory_order_acquire);
}

#else

void GuestWtFaultEnable_nid_no_patch(bool, std::uint64_t) {}
bool GuestWtFaultEnabled_nid_no_patch() { return false; }
bool GuestWtFaultHandle_nid_no_patch(std::uintptr_t) { return false; }
void GuestWtFaultMarkDriverThread_nid_no_patch() {}
void GuestWtFaultTrack_nid_no_patch(std::uintptr_t, std::size_t) {}
void GuestWtFaultNoteDriverStore_nid_no_patch(std::uintptr_t, std::size_t) {}
std::size_t GuestWtFaultArm_nid_no_patch(std::uintptr_t*, std::size_t) { return 0; }
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

#endif

}
