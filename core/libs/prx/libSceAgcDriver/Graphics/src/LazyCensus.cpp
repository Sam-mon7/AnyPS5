#include "prx/libSceAgcDriver/Graphics/include/GpuAuthorityCensus.hpp"
#include "prx/libSceAgcDriver/Graphics/include/LazyCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Graphics::ArchC::LazyCensus {

namespace {

constexpr std::size_t Kinds = static_cast<std::size_t>(Kind::Count);
constexpr std::size_t Outcomes = static_cast<std::size_t>(Outcome::Count);
constexpr std::size_t Reasons = 32;
constexpr std::size_t Buckets = 5;
constexpr int Flips = 3;
constexpr std::size_t EagerPerSecond = 64;

constexpr std::array<const char*, Kinds> KindNames{"copyback", "readback", "writeback", "publish"};
constexpr std::array<const char*, Outcomes> OutcomeNames{"guest_read", "guest_write", "driver_read", "driver_write", "os", "forget", "staging_in", "carry", "gpu_read", "gpu_address_read", "gpu_write", "label_store", "republished", "expired", "unwatchable", "staging_in_same", "carry_cross", "staging_in_same_cross"};
constexpr std::array<const char*, Buckets> BucketNames{"lt1ms", "lt5ms", "lt20ms", "lt60ms", "ge60ms"};

struct Counters {
    std::atomic<std::uint64_t> count[Kinds][Outcomes]{};
    std::atomic<std::uint64_t> bytes[Kinds][Outcomes]{};
    std::atomic<std::uint64_t> published[Kinds]{};
    std::atomic<std::uint64_t> publishedBytes[Kinds]{};
    std::atomic<std::uint64_t> pages[Kinds]{};
    std::atomic<std::uint64_t> watchedPages[Kinds]{};
    std::atomic<std::uint64_t> latency[Kinds][Buckets]{};
    std::atomic<std::uint64_t> reasonCpuBytes[Kinds][Reasons]{};
    std::atomic<std::uint64_t> reasonQuietBytes[Kinds][Reasons]{};
    std::atomic<std::uint64_t> reasonCpu[Kinds][Reasons]{};
    std::atomic<std::uint64_t> reasonQuiet[Kinds][Reasons]{};
    std::atomic<std::uint64_t> faults{0};
    std::atomic<std::uint64_t> eagerTouches{0};
    std::atomic<std::uint64_t> labelFlushNs[2]{};
    std::atomic<std::uint64_t> labelFlushWaitNs[2]{};
    std::atomic<std::uint64_t> labelFlushes[2]{};
    std::atomic<std::uint64_t> fenceWaitNs[2]{};
    std::atomic<std::uint64_t> fenceWaits[2]{};
    std::atomic<std::uint64_t> watchFaults{0};
    std::atomic<std::uint64_t> watchHandlerNs{0};
    std::atomic<std::uint64_t> logLost{0};
    std::atomic<std::uint64_t> live{0};
    std::atomic<std::uint64_t> skipped[Kinds][GuestArena::WtWatchSkipCauses]{};
    std::atomic<std::uint64_t> labelStored[2]{};
    std::atomic<std::uint64_t> watchNs{0}, watchCalls{0}, splitRuns{0}, splitPieces{0};
    Counters() {
        const auto add = [](const std::string& name, const std::atomic<std::uint64_t>& counter) { AgcDriver::ArchB::Recorder::RegisterCounter((new std::string(name))->c_str(), counter); };
        for (std::size_t k = 0; k < Kinds; ++k) {
            const std::string kind = std::string("p5c_") + KindNames[k];
            for (std::size_t o = 0; o < Outcomes; ++o) {
                add(kind + "_" + OutcomeNames[o] + "_n", count[k][o]);
                add(kind + "_" + OutcomeNames[o] + "_bytes", bytes[k][o]);
            }
            add(kind + "_published_n", published[k]);
            add(kind + "_published_bytes", publishedBytes[k]);
            add(kind + "_pages", pages[k]);
            add(kind + "_watched_pages", watchedPages[k]);
            for (std::size_t b = 0; b < Buckets; ++b) add(kind + "_cpu_" + BucketNames[b], latency[k][b]);
            if (k == static_cast<std::size_t>(Kind::WriteBack) || k == static_cast<std::size_t>(Kind::ShadowPublish)) {
                for (std::size_t r = 0; r < Reasons; ++r) {
                    add(kind + "_reason" + std::to_string(r) + "_cpu_bytes", reasonCpuBytes[k][r]);
                    add(kind + "_reason" + std::to_string(r) + "_quiet_bytes", reasonQuietBytes[k][r]);
                    add(kind + "_reason" + std::to_string(r) + "_cpu_n", reasonCpu[k][r]);
                    add(kind + "_reason" + std::to_string(r) + "_quiet_n", reasonQuiet[k][r]);
                }
            }
        }
        add("p5c_faults", faults);
        add("p5c_eager_touches", eagerTouches);
        add("p5c_label_flush_ns_q0", labelFlushNs[0]);
        add("p5c_label_flush_ns_other", labelFlushNs[1]);
        add("p5c_label_flush_wait_ns_q0", labelFlushWaitNs[0]);
        add("p5c_label_flush_wait_ns_other", labelFlushWaitNs[1]);
        add("p5c_label_flushes_q0", labelFlushes[0]);
        add("p5c_label_flushes_other", labelFlushes[1]);
        add("p5c_fence_wait_ns_q0", fenceWaitNs[0]);
        add("p5c_fence_wait_ns_other", fenceWaitNs[1]);
        add("p5c_fence_waits_q0", fenceWaits[0]);
        add("p5c_fence_waits_other", fenceWaits[1]);
        add("p5c_watch_faults", watchFaults);
        add("p5c_watch_handler_ns", watchHandlerNs);
        add("p5c_log_lost", logLost);
        add("p5c_live", live);
        constexpr std::array<const char*, GuestArena::WtWatchSkipCauses> causeNames{"noleaf", "untracked", "excluded", "unchecked", "exec", "held", "busy", "watched", "protect"};
        for (std::size_t k = 0; k < Kinds; ++k) {
            for (std::size_t cause = 0; cause < causeNames.size(); ++cause) add(std::string("p5c_") + KindNames[k] + "_skip_" + causeNames[cause], skipped[k][cause]);
        }
        add("p5c_label_stored_q0", labelStored[0]);
        add("p5c_label_stored_other", labelStored[1]);
        add("p5c_watch_ns", watchNs);
        add("p5c_watch_calls", watchCalls);
        add("p5c_watch_split_runs", splitRuns);
        add("p5c_watch_split_pieces", splitPieces);
    }
};

Counters& counters() {
    static Counters made;
    return made;
}

struct Entry {
    std::uint64_t end;
    Kind kind;
    std::uint8_t reason;
    std::uint64_t ticks;
    int flips;
    std::uint64_t serial;
};

struct RangeKey {
    std::uint64_t begin;
    std::uint64_t end;
    bool operator==(const RangeKey&) const = default;
};

struct RangeKeyHash {
    std::size_t operator()(const RangeKey& key) const { return static_cast<std::size_t>((key.begin * 0x9e3779b97f4a7c15ull) ^ key.end); }
};

struct State {
    AgcDriver::Mutex mutex;
    std::map<std::uint64_t, Entry> live;
    std::uint64_t cursor = 0;
    double nsPerTick = 1.0;
    std::unordered_map<RangeKey, std::deque<std::uint64_t>, RangeKeyHash> touches;
    std::vector<std::uint64_t> entries = std::vector<std::uint64_t>(512);
    std::vector<std::uint64_t> stamps = std::vector<std::uint64_t>(512);
    State() {
        GuestArena::GuestWtFaultTicks_nid_no_patch(&nsPerTick);
        cursor = GuestArena::GuestWtFaultWatchLogHead_nid_no_patch();
    }
};

State& state() {
    static State made;
    return made;
}

struct Deferred {
    std::uint64_t begin;
    std::uint64_t end;
    Kind kind;
    std::uint8_t reason;
    std::uint64_t serial;
};

thread_local int scopeDepth = 0;
thread_local std::uint64_t threadFenceWaitNs = 0;
thread_local std::vector<Deferred>* deferred = nullptr;

bool cpuOutcome(Outcome outcome) {
    return outcome == Outcome::GuestRead || outcome == Outcome::GuestWrite || outcome == Outcome::DriverRead || outcome == Outcome::DriverWrite || outcome == Outcome::Os;
}

void closeEntry(State& s, std::map<std::uint64_t, Entry>::iterator it, Outcome outcome, std::uint64_t ticks) {
    auto& c = counters();
    const auto begin = it->first;
    const auto entry = it->second;
    s.live.erase(it);
    c.live.store(s.live.size(), std::memory_order_relaxed);
    const auto k = static_cast<std::size_t>(entry.kind);
    const auto bytes = entry.end - begin;
    if (outcome != Outcome::GuestRead && outcome != Outcome::DriverRead) GpuAuthorityCensus::WatchCoverage(begin, entry.end, false);
    if (outcome == Outcome::GuestRead || outcome == Outcome::DriverRead) GpuAuthorityCensus::CpuRead(begin, static_cast<std::size_t>(bytes), outcome == Outcome::GuestRead ? 32 : 33, ticks);
    if (outcome == Outcome::GuestWrite) GpuAuthorityCensus::Overwritten(begin, entry.end, ticks);
    c.count[k][static_cast<std::size_t>(outcome)].fetch_add(1, std::memory_order_relaxed);
    c.bytes[k][static_cast<std::size_t>(outcome)].fetch_add(bytes, std::memory_order_relaxed);
    const bool cpu = cpuOutcome(outcome);
    const auto r = std::min<std::size_t>(entry.reason, Reasons - 1);
    (cpu ? c.reasonCpuBytes : c.reasonQuietBytes)[k][r].fetch_add(bytes, std::memory_order_relaxed);
    (cpu ? c.reasonCpu : c.reasonQuiet)[k][r].fetch_add(1, std::memory_order_relaxed);
    if (cpu) {
        const auto ms = ticks > entry.ticks ? static_cast<double>(ticks - entry.ticks) * s.nsPerTick / 1e6 : 0.0;
        const std::size_t bucket = ms < 1 ? 0 : ms < 5 ? 1 : ms < 20 ? 2 : ms < 60 ? 3 : 4;
        c.latency[k][bucket].fetch_add(1, std::memory_order_relaxed);
        auto& seen = s.touches[{begin, entry.end}];
        const auto window = static_cast<std::uint64_t>(1e9 / s.nsPerTick);
        while (!seen.empty() && seen.front() + window < ticks) seen.pop_front();
        seen.push_back(ticks);
        if (seen.size() > EagerPerSecond) c.eagerTouches.fetch_add(1, std::memory_order_relaxed);
        else c.faults.fetch_add(1, std::memory_order_relaxed);
        if (s.touches.size() > 65536) s.touches.clear();
    }
    GuestArena::GuestWtFaultUnwatch_nid_no_patch(static_cast<std::uintptr_t>(begin), static_cast<std::size_t>(bytes));
}

std::map<std::uint64_t, Entry>::iterator firstOverlap(State& s, std::uint64_t begin) {
    auto it = s.live.upper_bound(begin);
    if (it != s.live.begin()) {
        auto previous = std::prev(it);
        if (previous->second.end > begin) return previous;
    }
    return it;
}

void closeOverlaps(State& s, std::uint64_t begin, std::uint64_t end, Outcome outcome, std::uint64_t ticks, std::uint64_t serial = 0) {
    for (auto it = firstOverlap(s, begin); it != s.live.end() && it->first < end;) {
        const auto next = std::next(it);
        if (it->second.end > begin) {
            auto closing = outcome;
            if (serial != 0 && it->second.serial != 0 && serial != it->second.serial) {
                if (outcome == Outcome::Carry) closing = Outcome::CarryCross;
                if (outcome == Outcome::StagingInSame) closing = Outcome::StagingInSameCross;
            }
            closeEntry(s, it, closing, ticks);
        }
        it = next;
    }
}

Outcome outcomeOf(std::uint64_t entry) {
    const bool driver = (entry & GuestArena::WtWatchDriver) != 0;
    switch (entry & GuestArena::WtWatchKindMask) {
    case GuestArena::WtWatchRead: return driver ? Outcome::DriverRead : Outcome::GuestRead;
    case GuestArena::WtWatchWrite: return driver ? Outcome::DriverWrite : Outcome::GuestWrite;
    case GuestArena::WtWatchForget: return Outcome::Forget;
    default: return Outcome::Os;
    }
}

void drainLocked(State& s) {
    if (GuestArena::GuestWtFaultWatchLogHead_nid_no_patch() == s.cursor) return;
    for (;;) {
        bool lost = false;
        const auto count = GuestArena::GuestWtFaultWatchLogRead_nid_no_patch(&s.cursor, s.entries.data(), s.stamps.data(), s.entries.size(), &lost);
        if (lost) counters().logLost.fetch_add(1, std::memory_order_relaxed);
        for (std::size_t i = 0; i < count; ++i) {
            const auto page = s.entries[i] & ~std::uint64_t{4095};
            auto it = s.live.upper_bound(page + 4095);
            if (it == s.live.begin()) continue;
            --it;
            if (it->second.end <= page) continue;
            closeEntry(s, it, outcomeOf(s.entries[i]), s.stamps[i]);
        }
        if (count < s.entries.size()) return;
    }
}

void commit(const Deferred& item) {
    auto& s = state();
    auto& c = counters();
    const auto k = static_cast<std::size_t>(item.kind);
    const auto now = GuestArena::GuestWtFaultTicks_nid_no_patch(nullptr);
    std::lock_guard lock(s.mutex);
    drainLocked(s);
    closeOverlaps(s, item.begin, item.end, Outcome::Republished, now);
    c.published[k].fetch_add(1, std::memory_order_relaxed);
    c.publishedBytes[k].fetch_add(item.end - item.begin, std::memory_order_relaxed);
    const auto pages = ((item.end + 4095) / 4096) - (item.begin / 4096);
    c.pages[k].fetch_add(pages, std::memory_order_relaxed);
    std::array<std::uint64_t, GuestArena::WtWatchSkipCauses> causes{};
    GuestArena::GuestWtFaultSetWatchSplit_nid_no_patch(AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::CWatchSplit));
    const auto watchStart = GuestArena::GuestWtFaultTicks_nid_no_patch(nullptr);
    const auto watched = GuestArena::GuestWtFaultWatchCauses_nid_no_patch(static_cast<std::uintptr_t>(item.begin), static_cast<std::size_t>(item.end - item.begin), causes.data());
    GpuAuthorityCensus::WatchCoverage(item.begin, item.end, watched == pages);
    c.watchNs.fetch_add(static_cast<std::uint64_t>(static_cast<double>(GuestArena::GuestWtFaultTicks_nid_no_patch(nullptr) - watchStart) * s.nsPerTick), std::memory_order_relaxed);
    c.watchCalls.fetch_add(1, std::memory_order_relaxed);
    c.watchedPages[k].fetch_add(watched, std::memory_order_relaxed);
    for (std::size_t cause = 0; cause < causes.size(); ++cause) {
        if (causes[cause] != 0) c.skipped[k][cause].fetch_add(causes[cause], std::memory_order_relaxed);
    }
    if (causes[GuestArena::WtWatchSkipUntracked] != 0) GuestArena::GuestWtFaultTrack_nid_no_patch(static_cast<std::uintptr_t>(item.begin), static_cast<std::size_t>(item.end - item.begin));
    if (watched == 0) {
        c.count[k][static_cast<std::size_t>(Outcome::Unwatchable)].fetch_add(1, std::memory_order_relaxed);
        c.bytes[k][static_cast<std::size_t>(Outcome::Unwatchable)].fetch_add(item.end - item.begin, std::memory_order_relaxed);
        return;
    }
    s.live[item.begin] = {item.end, item.kind, item.reason, now, Flips, item.serial};
    c.live.store(s.live.size(), std::memory_order_relaxed);
}

Outcome outcomeOf(GpuUse use) {
    switch (use) {
    case GpuUse::StagingIn: return Outcome::StagingIn;
    case GpuUse::Carry: return Outcome::Carry;
    case GpuUse::Read: return Outcome::GpuRead;
    case GpuUse::AddressRead: return Outcome::GpuAddressRead;
    case GpuUse::Write: return Outcome::GpuWrite;
    case GpuUse::StagingInSame: return Outcome::StagingInSame;
    default: return Outcome::LabelStore;
    }
}

}

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_C_LAZY_CENSUS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled && GuestArena::GuestWtFaultEnabled_nid_no_patch();
}

std::uint8_t ReasonFor(const char* flushReason) {
    static constexpr std::array<std::pair<const char*, std::uint8_t>, 12> names{{{"readback before label", 19}, {"readback before interrupt", 19}, {"label", 7}, {"interrupt", 7}, {"flip", 9}, {"memory access", 0}, {"sampled texture", 18}, {"written buffer range", 1}, {"in-place vertex input", 1}, {"imported buffer region", 1}, {"copied buffer region", 1}, {"idle", 15}}};
    if (flushReason == nullptr) return 31;
    for (const auto& [name, reason] : names) {
        if (std::strcmp(flushReason, name) == 0) return reason;
    }
    return 30;
}

UseScope::UseScope() : active(Enabled()) {
    if (active) ++scopeDepth;
}

UseScope::~UseScope() {
    if (!active || --scopeDepth != 0 || deferred == nullptr || deferred->empty()) return;
    std::vector<Deferred> items;
    items.swap(*deferred);
    for (const auto& item : items) commit(item);
}

void Published(std::uint64_t begin, std::uint64_t end, Kind kind, std::uint8_t reason, std::uint64_t serial) {
    if (!Enabled() || end <= begin) return;
    static const bool installed = [] {
        static_cast<void>(counters());
        AgcDriver::GuestMemory::SetArmObserver(&Flip);
        return true;
    }();
    static_cast<void>(installed);
    if (scopeDepth > 0) {
        if (deferred == nullptr) deferred = new std::vector<Deferred>();
        deferred->push_back({begin, end, kind, reason, serial});
        return;
    }
    commit({begin, end, kind, reason, serial});
}

void UsedByGpu(std::uint64_t begin, std::uint64_t end, GpuUse use, std::uint64_t serial) {
    if (use == GpuUse::Carry || use == GpuUse::StagingInSame) GpuAuthorityCensus::GpuRead(begin, end, static_cast<std::uint32_t>(use));
    if (GpuAuthorityCensus::Enabled() && use != GpuUse::Write && use != GpuUse::LabelStore) return;
    if (!Enabled() || end <= begin) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.live.empty()) return;
    drainLocked(s);
    closeOverlaps(s, begin, end, outcomeOf(use), GuestArena::GuestWtFaultTicks_nid_no_patch(nullptr), serial);
}

void UsedByGpu(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, GpuUse use) {
    if (GpuAuthorityCensus::Enabled()) {
        if (use != GpuUse::Write && use != GpuUse::LabelStore) return;
    }
    if (!Enabled() || ranges.empty()) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.live.empty()) return;
    drainLocked(s);
    const auto now = GuestArena::GuestWtFaultTicks_nid_no_patch(nullptr);
    for (const auto& [begin, end] : ranges) {
        if (s.live.empty()) return;
        if (end > begin) closeOverlaps(s, begin, end, outcomeOf(use), now);
    }
}

void LabelStored(bool queue0) {
    if (Enabled()) counters().labelStored[queue0 ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
}

void LabelFlush(std::uint64_t ns, std::uint64_t waitedNs, bool queue0) {
    if (!Enabled()) return;
    auto& c = counters();
    const std::size_t at = queue0 ? 0 : 1;
    c.labelFlushNs[at].fetch_add(ns, std::memory_order_relaxed);
    c.labelFlushWaitNs[at].fetch_add(waitedNs, std::memory_order_relaxed);
    c.labelFlushes[at].fetch_add(1, std::memory_order_relaxed);
}

void NoteFenceWait(std::uint64_t ns) {
    if (!Enabled()) return;
    threadFenceWaitNs += ns;
    const std::size_t at = AgcDriver::GuestMemory::GpuLockThreadTag() == 0 ? 0 : 1;
    auto& c = counters();
    c.fenceWaitNs[at].fetch_add(ns, std::memory_order_relaxed);
    c.fenceWaits[at].fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t ThreadFenceWaitNs() {
    return threadFenceWaitNs;
}

void Drain() {
    if (!Enabled()) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    drainLocked(s);
}

void Flip() {
    if (!Enabled()) return;
    auto& s = state();
    auto& c = counters();
    const auto now = GuestArena::GuestWtFaultTicks_nid_no_patch(nullptr);
    {
        std::lock_guard lock(s.mutex);
        drainLocked(s);
        for (auto it = s.live.begin(); it != s.live.end();) {
            const auto next = std::next(it);
            if (--it->second.flips <= 0) closeEntry(s, it, Outcome::Expired, now);
            it = next;
        }
    }
    GuestArena::WtFaultCounters libc{};
    GuestArena::GuestWtFaultCounters_nid_no_patch(&libc);
    c.watchFaults.store(libc.watchFaults, std::memory_order_relaxed);
    c.watchHandlerNs.store(libc.watchHandlerNs, std::memory_order_relaxed);
    std::uint64_t runs = 0;
    std::uint64_t pieces = 0;
    GuestArena::GuestWtFaultWatchSplitCounters_nid_no_patch(&runs, &pieces);
    c.splitRuns.store(runs, std::memory_order_relaxed);
    c.splitPieces.store(pieces, std::memory_order_relaxed);
}

Totals Snapshot() {
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        drainLocked(s);
    }
    auto& c = counters();
    Totals totals{};
    for (std::size_t k = 0; k < Kinds; ++k) {
        for (std::size_t o = 0; o < Outcomes; ++o) {
            totals.count[k][o] = c.count[k][o].load(std::memory_order_relaxed);
            totals.bytes[k][o] = c.bytes[k][o].load(std::memory_order_relaxed);
        }
        totals.watchedPages += c.watchedPages[k].load(std::memory_order_relaxed);
        totals.pages += c.pages[k].load(std::memory_order_relaxed);
    }
    totals.faults = c.faults.load(std::memory_order_relaxed);
    totals.eagerTouches = c.eagerTouches.load(std::memory_order_relaxed);
    return totals;
}

void ResetForTest() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto now = GuestArena::GuestWtFaultTicks_nid_no_patch(nullptr);
    while (!s.live.empty()) closeEntry(s, s.live.begin(), Outcome::Expired, now);
    s.touches.clear();
    s.cursor = GuestArena::GuestWtFaultWatchLogHead_nid_no_patch();
    auto& c = counters();
    for (std::size_t k = 0; k < Kinds; ++k) {
        for (std::size_t o = 0; o < Outcomes; ++o) {
            c.count[k][o].store(0, std::memory_order_relaxed);
            c.bytes[k][o].store(0, std::memory_order_relaxed);
        }
        c.watchedPages[k].store(0, std::memory_order_relaxed);
        c.pages[k].store(0, std::memory_order_relaxed);
    }
    c.faults.store(0, std::memory_order_relaxed);
    c.eagerTouches.store(0, std::memory_order_relaxed);
}

}
