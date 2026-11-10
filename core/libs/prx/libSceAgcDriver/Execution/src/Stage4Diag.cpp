#include "prx/libSceAgcDriver/Execution/include/Stage4Diag.hpp"
#include "prx/libSceAgcDriver/Execution/include/Stage4DiagModel.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#ifdef _WIN32
#include "prx/libc/include/GuestArena.hpp"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace AgcDriver::Stage4 {

namespace {

std::int64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

using Counter = std::atomic<std::uint64_t>;
void add(Counter& counter, std::int64_t value) noexcept {
    if (value > 0) counter.fetch_add(static_cast<std::uint64_t>(value), std::memory_order_relaxed);
}
void add(Counter& counter, std::uint64_t value) noexcept {
    if (value != 0) counter.fetch_add(value, std::memory_order_relaxed);
}

thread_local Role role = Role::None;
thread_local std::uint32_t waitDepth = 0;

constexpr std::size_t WaitKinds = static_cast<std::size_t>(Wait::Count);
constexpr std::size_t Reasons = static_cast<std::size_t>(LabelReason::Count);
constexpr std::size_t Writers = static_cast<std::size_t>(Writer::Count);
constexpr std::size_t LabelCells = WaitEnds * Reasons * Writers;

bool blockedState(std::size_t state) {
    const auto value = static_cast<CommitterState>(state);
    return value == CommitterState::Idle || value == CommitterState::GpuMutex || value == CommitterState::Fence || value == CommitterState::Other;
}

CommitterState stateFor(Wait kind) {
    switch (kind) {
    case Wait::Idle: return CommitterState::Idle;
    case Wait::GpuMutex: return CommitterState::GpuMutex;
    case Wait::Fence: return CommitterState::Fence;
    default: return CommitterState::Other;
    }
}

Stage4Model::StateClock<CommitterStates>& committerClock() {
    static auto* clock = new Stage4Model::StateClock<CommitterStates>();
    return *clock;
}

// M1 totals (cumulative; the report takes differences).
struct M1Totals {
    std::array<Counter, WaitKinds> count{};
    std::array<Counter, WaitKinds> waitNs{};
    std::array<Counter, WaitKinds> blockedNs{};
    // Committer blocked (idle, mutex, fence, other) during the wait's blocked part / the whole wait.
    std::array<Counter, WaitKinds> neitherNs{};
    std::array<Counter, WaitKinds> neitherAllNs{};
    std::array<std::array<Counter, CommitterStates>, WaitKinds> overlapNs{};
    std::array<Counter, LabelCells> labelCount{};
    std::array<Counter, LabelCells> labelWaitNs{};
    std::array<Counter, LabelCells> labelBlockedNs{};
    std::array<Counter, LabelCells> labelNeitherNs{};
    // Label waits whose writer was another queue (table or store), by queue id.
    std::array<Counter, 256> queueCount{};
    std::array<Counter, 256> queueWaitNs{};
    std::array<Counter, 256> queueNeitherNs{};
};
M1Totals& m1() {
    static auto* totals = new M1Totals();
    return *totals;
}

// M3 census.
struct M3State {
    Stage4Model::PageStampTable collected, hle, driver, any, blocks;
    std::atomic<std::uint32_t> frame{1};
    Counter freshCollected{0}, freshHle{0}, freshDriver{0}, freshAny{0}, repeatAny{0}, guestOnly{0}, freshBlocks{0};
    Counter hleCalls{0}, hleBytes{0}, driverCalls{0}, driverBytes{0}, driverLargeCalls{0}, driverLargeBytes{0}, collectRuns{0};
};
M3State& m3() {
    static auto* state = new M3State();
    return *state;
}
// Driver stores larger than this are counted (calls, bytes) but not stamped page by page: they are
// landed GPU results or whole-surface write-backs a fault tracker would unprotect up front.
constexpr std::size_t DriverStampLimit = std::size_t{256} << 10;

constexpr std::uint64_t PageShift = 12;

void stampAny(M3State& state, std::uint64_t page, std::uint32_t frame, std::uint64_t& fresh, std::uint64_t& repeat, std::uint64_t& blocks) {
    const auto result = Stage4Model::StampPage(state.any, page, frame);
    if (!result.fresh) return;
    ++fresh;
    if (result.repeat) ++repeat;
    if (Stage4Model::StampPage(state.blocks, page >> 4u, frame).fresh) ++blocks;
}

// M4 census: the committer's current item and the cumulative totals.
constexpr std::size_t ItemKinds = 6;  // DrawPipeline::ItemKind: other, draw, dispatch, label, dma, fill
constexpr const char* ItemKindNames[ItemKinds] = {"other", "draw", "dispatch", "label", "dma", "fill"};
struct CommitterItem {
    Stage4Model::ItemTimer<Buckets> timer;
    Stage4Model::ItemFootprint footprint;
    // Tracker queries since the last phase mark: the mark's bucket decides whether they were resolve
    // reads (resolve-class and mixed buckets) or apply-class bookkeeping (allReads only).
    Stage4Model::RangeSet phaseReads;
    Stage4Model::ConflictWindow window;
    std::uint8_t kind = 0;
    std::uint64_t pendingAtStart = 0;
};
thread_local CommitterItem* currentItem = nullptr;
CommitterItem& committerItem() {
    static auto* item = new CommitterItem();
    return *item;
}
struct ConflictTotals {
    Counter memoryForeign{0}, memoryDeclared{0};
    Counter items{0}, fine{0}, memory{0}, memoryAll{0}, templateBuilt{0}, templateShared{0}, pending{0}, proofs{0}, global{0};
};
struct M4Totals {
    std::array<Counter, Buckets> bucketNs{};
    std::array<Counter, ItemKinds> items{};
    std::array<Counter, ItemKinds> resolveNs{};
    std::array<Counter, ItemKinds> applyNs{};
    std::array<ConflictTotals, 2> conflicts{};  // K=3, K=8
    Counter overflowedReads{0}, overflowedWrites{0};
};
M4Totals& m4() {
    static auto* totals = new M4Totals();
    return *totals;
}
constexpr std::array<std::size_t, 2> WindowSizes{3, 8};

void settleReads(CommitterItem& item, Bucket bucket) {
    const auto index = static_cast<std::size_t>(bucket);
    if (index < FirstApplyBucket || (index >= FirstMixedBucket && bucket != Bucket::Wait)) item.footprint.reads.Merge(item.phaseReads);
    item.phaseReads.Clear();
}

}

bool EnvSwitch(const char* name) {
    const char* text = std::getenv(name);
    return text != nullptr && text[0] == '1' && text[1] == '\0';
}

void BindRole(Role value) {
    if (!Tracking() && !FaultCensus()) return;
    role = value;
    if (value != Role::Committer) return;
    if (IdleAttrib()) committerClock().Set(static_cast<std::size_t>(CommitterState::Run), nowNs());
    if (ResolveCensus()) currentItem = &committerItem();
}

// --- M1 -------------------------------------------------------------------------------------------

void WaitScope::begin(Wait waitKind, bool spinsFirst) noexcept {
    if (role == Role::None || waitDepth != 0) return;
    committer = role == Role::Committer;
    if (!committer && !IdleAttrib()) return;
    active = true;
    ++waitDepth;
    kind = waitKind;
    start = nowNs();
    if (committer) {
        if (IdleAttrib()) committerClock().Set(static_cast<std::size_t>(spinsFirst ? CommitterState::Spin : stateFor(waitKind)), start);
        if (currentItem != nullptr) outermostNested = currentItem->timer.NestedBegin();
        if (!spinsFirst) blockStart = start;
        return;
    }
    const auto snapshot = committerClock().Read(start);
    std::copy(snapshot.begin(), snapshot.end(), clockAtStart);
    if (!spinsFirst) {
        blockStart = start;
        std::copy(snapshot.begin(), snapshot.end(), clockAtBlock);
    }
}

void WaitScope::blocking() noexcept {
    blockStart = nowNs();
    if (committer) {
        if (IdleAttrib()) committerClock().Set(static_cast<std::size_t>(stateFor(kind)), blockStart);
        return;
    }
    const auto snapshot = committerClock().Read(blockStart);
    std::copy(snapshot.begin(), snapshot.end(), clockAtBlock);
}

void WaitScope::end() noexcept {
    active = false;
    if (waitDepth != 0) --waitDepth;
    const auto now = nowNs();
    if (committer) {
        if (IdleAttrib()) committerClock().Set(static_cast<std::size_t>(CommitterState::Run), now);
        if (currentItem != nullptr) currentItem->timer.NestedEnd(static_cast<std::size_t>(Bucket::Wait), start, now, outermostNested);
        return;
    }
    const auto after = committerClock().Read(now);
    std::array<std::int64_t, CommitterStates> atStart{};
    std::array<std::int64_t, CommitterStates> atBlock{};
    std::copy(clockAtStart, clockAtStart + CommitterStates, atStart.begin());
    std::copy(clockAtBlock, clockAtBlock + CommitterStates, atBlock.begin());
    const auto all = Stage4Model::Overlap(atStart, after);
    const auto blocked = blockStart != 0 ? Stage4Model::Overlap(atBlock, after) : std::array<std::int64_t, CommitterStates>{};
    std::int64_t neither = 0;
    std::int64_t neitherAll = 0;
    for (std::size_t state = 0; state < CommitterStates; ++state) {
        if (!blockedState(state)) continue;
        neither += blocked[state];
        neitherAll += all[state];
    }
    const auto waited = now - start;
    const auto blockedNs = blockStart != 0 ? now - blockStart : 0;
    auto& totals = m1();
    const auto k = static_cast<std::size_t>(kind);
    add(totals.count[k], std::uint64_t{1});
    add(totals.waitNs[k], waited);
    add(totals.blockedNs[k], blockedNs);
    add(totals.neitherNs[k], neither);
    add(totals.neitherAllNs[k], neitherAll);
    for (std::size_t state = 0; state < CommitterStates; ++state) add(totals.overlapNs[k][state], blocked[state]);
    if (kind != Wait::Label) return;
    const auto cell = (std::min<std::size_t>(waitEnd, WaitEnds - 1) * Reasons + static_cast<std::size_t>(reason)) * Writers + static_cast<std::size_t>(writer);
    add(totals.labelCount[cell], std::uint64_t{1});
    add(totals.labelWaitNs[cell], waited);
    add(totals.labelBlockedNs[cell], blockedNs);
    add(totals.labelNeitherNs[cell], neither);
    if (writer == Writer::OtherTable || writer == Writer::OtherStore) {
        const auto queue = writerQueue & 0xffu;
        add(totals.queueCount[queue], std::uint64_t{1});
        add(totals.queueWaitNs[queue], waited);
        add(totals.queueNeitherNs[queue], neither);
    }
}

// --- M3 -------------------------------------------------------------------------------------------

void NoteCollectedPages(std::uint64_t begin, std::uint64_t end) {
    if (!FaultCensus() || end <= begin) return;
    auto& state = m3();
    const auto frame = state.frame.load(std::memory_order_relaxed);
    std::uint64_t fresh = 0, guest = 0, freshAny = 0, repeat = 0, blocks = 0;
    for (auto page = begin >> PageShift; page <= (end - 1) >> PageShift; ++page) {
        if (!Stage4Model::StampPage(state.collected, page, frame).fresh) continue;
        ++fresh;
        if (state.hle.Load(page) != frame && state.driver.Load(page) != frame) ++guest;
        stampAny(state, page, frame, freshAny, repeat, blocks);
    }
    add(state.collectRuns, std::uint64_t{1});
    add(state.freshCollected, fresh);
    add(state.guestOnly, guest);
    add(state.freshAny, freshAny);
    add(state.repeatAny, repeat);
    add(state.freshBlocks, blocks);
}

void NoteHostWrite(std::uint64_t address, std::size_t bytes) {
    if (!FaultCensus() || bytes == 0) return;
    auto& state = m3();
    const auto frame = state.frame.load(std::memory_order_relaxed);
    std::uint64_t fresh = 0, freshAny = 0, repeat = 0, blocks = 0;
    for (auto page = address >> PageShift; page <= (address + bytes - 1) >> PageShift; ++page) {
        if (!Stage4Model::StampPage(state.hle, page, frame).fresh) continue;
        ++fresh;
        stampAny(state, page, frame, freshAny, repeat, blocks);
    }
    add(state.hleCalls, std::uint64_t{1});
    add(state.hleBytes, static_cast<std::uint64_t>(bytes));
    add(state.freshHle, fresh);
    add(state.freshAny, freshAny);
    add(state.repeatAny, repeat);
    add(state.freshBlocks, blocks);
}

void NoteDriverWrite(std::uint64_t address, std::size_t bytes) {
    if (!FaultCensus() || bytes == 0) return;
    auto& state = m3();
    if (bytes > DriverStampLimit) {
        add(state.driverLargeCalls, std::uint64_t{1});
        add(state.driverLargeBytes, static_cast<std::uint64_t>(bytes));
        return;
    }
    const auto frame = state.frame.load(std::memory_order_relaxed);
    std::uint64_t fresh = 0, freshAny = 0, repeat = 0, blocks = 0;
    for (auto page = address >> PageShift; page <= (address + bytes - 1) >> PageShift; ++page) {
        if (!Stage4Model::StampPage(state.driver, page, frame).fresh) continue;
        ++fresh;
        stampAny(state, page, frame, freshAny, repeat, blocks);
    }
    add(state.driverCalls, std::uint64_t{1});
    add(state.driverBytes, static_cast<std::uint64_t>(bytes));
    add(state.freshDriver, fresh);
    add(state.freshAny, freshAny);
    add(state.repeatAny, repeat);
    add(state.freshBlocks, blocks);
}

// --- M4 -------------------------------------------------------------------------------------------

void BeginItem(std::uint8_t kind, std::span<const std::pair<std::uint64_t, std::uint64_t>> reads, std::span<const std::pair<std::uint64_t, std::uint64_t>> writes, std::uint64_t pendingSerial) {
    auto* item = currentItem;
    if (!ResolveCensus() || item == nullptr) return;
    item->timer.Begin(nowNs());
    item->kind = static_cast<std::uint8_t>(std::min<std::size_t>(kind, ItemKinds - 1));
    item->pendingAtStart = pendingSerial;
    auto& footprint = item->footprint;
    footprint.reads.Clear();
    footprint.allReads.Clear();
    footprint.declaredReads.Clear();
    footprint.writes.Clear();
    item->phaseReads.Clear();
    for (const auto& [begin, end] : reads) {
        footprint.reads.Add(begin, end);
        footprint.allReads.Add(begin, end);
        footprint.declaredReads.Add(begin, end);
    }
    for (const auto& [begin, end] : writes) footprint.writes.Add(begin, end);
    footprint.templateObject = nullptr;
    footprint.builtTemplate = false;
    footprint.pendingChanged = false;
    footprint.breaksProofs = false;
    footprint.resolves = item->kind == 1 || item->kind == 2;
}

void EndItem(std::uint64_t pendingSerial, bool breaksProofs) {
    auto* item = currentItem;
    if (!ResolveCensus() || item == nullptr || !item->timer.Active()) return;
    const auto tail = item->kind == 1 ? Bucket::DrawTail : item->kind == 2 ? Bucket::DispatchTail : Bucket::NonDraw;
    item->timer.End(static_cast<std::size_t>(tail), nowNs());
    settleReads(*item, tail);
    auto& totals = m4();
    const auto& ns = item->timer.Totals();
    std::int64_t resolve = 0;
    std::int64_t apply = 0;
    for (std::size_t bucket = 0; bucket < Buckets; ++bucket) {
        add(totals.bucketNs[bucket], ns[bucket]);
        if (bucket < FirstApplyBucket) resolve += ns[bucket];
        else if (bucket < FirstMixedBucket) apply += ns[bucket];
    }
    add(totals.items[item->kind], std::uint64_t{1});
    add(totals.resolveNs[item->kind], resolve);
    add(totals.applyNs[item->kind], apply);
    auto& footprint = item->footprint;
    footprint.foreignReads = footprint.reads.Without(footprint.writes);
    footprint.pendingChanged = pendingSerial != item->pendingAtStart;
    footprint.breaksProofs = breaksProofs;
    if (footprint.reads.Overflowed()) add(totals.overflowedReads, std::uint64_t{1});
    if (footprint.writes.Overflowed()) add(totals.overflowedWrites, std::uint64_t{1});
    if (footprint.resolves) {
        for (std::size_t window = 0; window < WindowSizes.size(); ++window) {
            const auto conflict = item->window.Check(footprint, WindowSizes[window]);
            auto& counts = totals.conflicts[window];
            add(counts.items, std::uint64_t{1});
            add(counts.fine, std::uint64_t{conflict.Fine()});
            add(counts.memory, std::uint64_t{conflict.memory});
            add(counts.memoryAll, std::uint64_t{conflict.memoryAll});
            add(counts.memoryForeign, std::uint64_t{conflict.memoryForeign});
            add(counts.memoryDeclared, std::uint64_t{conflict.memoryDeclared});
            add(counts.templateBuilt, std::uint64_t{conflict.templateBuilt});
            add(counts.templateShared, std::uint64_t{conflict.templateShared});
            add(counts.pending, std::uint64_t{conflict.pending});
            add(counts.proofs, std::uint64_t{conflict.proofs});
            add(counts.global, std::uint64_t{conflict.Global()});
        }
    }
    item->window.Push(footprint);
}

void MarkSlow(Bucket bucket) noexcept {
    auto* item = currentItem;
    if (item == nullptr || !item->timer.Active()) return;
    item->timer.Mark(static_cast<std::size_t>(bucket), nowNs());
    settleReads(*item, bucket);
}

void NoteTemplate(const void* object, bool built) noexcept {
    if (!ResolveCensus()) return;
    auto* item = currentItem;
    if (item == nullptr || !item->timer.Active()) return;
    item->footprint.templateObject = object;
    item->footprint.builtTemplate = item->footprint.builtTemplate || built;
}

void TrackerScope::begin(bool isStamp, std::uint64_t address, std::size_t bytes) noexcept {
    auto* item = currentItem;
    if (item == nullptr || !item->timer.Active()) return;
    active = true;
    stamp = isStamp;
    outermost = item->timer.NestedBegin();
    start = nowNs();
    if (bytes == 0) return;
    if (stamp) {
        item->footprint.writes.Add(address, address + bytes);
        return;
    }
    item->phaseReads.Add(address, address + bytes);
    item->footprint.allReads.Add(address, address + bytes);
}

void TrackerScope::AddRead(std::uint64_t address, std::size_t bytes) noexcept {
    if (!active || bytes == 0) return;
    if (auto* item = currentItem; item != nullptr) {
        item->phaseReads.Add(address, address + bytes);
        item->footprint.allReads.Add(address, address + bytes);
    }
}

void TrackerScope::end() noexcept {
    active = false;
    if (auto* item = currentItem; item != nullptr) item->timer.NestedEnd(static_cast<std::size_t>(stamp ? Bucket::WtApply : Bucket::WtResolve), start, nowNs(), outermost);
}

// --- report ---------------------------------------------------------------------------------------

namespace {

struct FrameValues {
    std::uint64_t any, collected, hle, driver, guest, fresh;
};

class Reporter {
public:
    void Flip() {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        const auto now = nowNs();
        if (windowStart == 0) {
            windowStart = now;
            m1Base = takeM1(now);
            m4Base = takeM4();
            m3Last = m3Report = takeM3Cumulative();
            return;
        }
        ++frames;
        if (FaultCensus()) flipM3();
        static const std::int64_t interval = [] {
            const char* text = std::getenv("APS5_STAGE4_REPORT_SECONDS");
            const long long seconds = text != nullptr ? std::atoll(text) : 5;
            return static_cast<std::int64_t>(std::clamp<long long>(seconds, 1, 600)) * 1'000'000'000;
        }();
        if (now - windowStart < interval) return;
        report(now);
        windowStart = now;
        frames = 0;
    }

private:
    // M1 snapshot: every cumulative counter and the committer clock.
    struct M1Snapshot {
        std::array<std::uint64_t, WaitKinds> count{}, waitNs{}, blockedNs{}, neitherNs{}, neitherAllNs{};
        std::array<std::array<std::uint64_t, CommitterStates>, WaitKinds> overlapNs{};
        std::array<std::uint64_t, LabelCells> labelCount{}, labelWaitNs{}, labelBlockedNs{}, labelNeitherNs{};
        std::array<std::uint64_t, 256> queueCount{}, queueWaitNs{}, queueNeitherNs{};
        std::array<std::int64_t, CommitterStates> clock{};
    };
    struct M3Cumulative {
        std::uint64_t collected, hle, driver, any, repeat, guest, blocks, hleCalls, hleBytes, driverCalls, driverBytes, largeCalls, largeBytes;
    };
    struct M4Snapshot {
        std::array<std::uint64_t, Buckets> bucketNs{};
        std::array<std::uint64_t, ItemKinds> items{}, resolveNs{}, applyNs{};
        std::array<std::array<std::uint64_t, 11>, 2> conflicts{};
        std::uint64_t overflowedReads = 0, overflowedWrites = 0;
    };

    template<std::size_t N>
    static std::array<std::uint64_t, N> load(const std::array<Counter, N>& from) {
        std::array<std::uint64_t, N> out{};
        for (std::size_t i = 0; i < N; ++i) out[i] = from[i].load(std::memory_order_relaxed);
        return out;
    }
    static M1Snapshot takeM1(std::int64_t now) {
        M1Snapshot s;
        if (!IdleAttrib()) return s;
        auto& t = m1();
        s.count = load(t.count);
        s.waitNs = load(t.waitNs);
        s.blockedNs = load(t.blockedNs);
        s.neitherNs = load(t.neitherNs);
        s.neitherAllNs = load(t.neitherAllNs);
        for (std::size_t k = 0; k < WaitKinds; ++k) s.overlapNs[k] = load(t.overlapNs[k]);
        s.labelCount = load(t.labelCount);
        s.labelWaitNs = load(t.labelWaitNs);
        s.labelBlockedNs = load(t.labelBlockedNs);
        s.labelNeitherNs = load(t.labelNeitherNs);
        s.queueCount = load(t.queueCount);
        s.queueWaitNs = load(t.queueWaitNs);
        s.queueNeitherNs = load(t.queueNeitherNs);
        s.clock = committerClock().Read(now);
        return s;
    }
    static M3Cumulative takeM3Cumulative() {
        if (!FaultCensus()) return {};
        auto& s = m3();
        const auto l = [](const Counter& c) { return c.load(std::memory_order_relaxed); };
        return {l(s.freshCollected), l(s.freshHle), l(s.freshDriver), l(s.freshAny), l(s.repeatAny), l(s.guestOnly), l(s.freshBlocks), l(s.hleCalls), l(s.hleBytes), l(s.driverCalls), l(s.driverBytes), l(s.driverLargeCalls), l(s.driverLargeBytes)};
    }
    static M4Snapshot takeM4() {
        M4Snapshot s;
        if (!ResolveCensus()) return s;
        auto& t = m4();
        s.bucketNs = load(t.bucketNs);
        s.items = load(t.items);
        s.resolveNs = load(t.resolveNs);
        s.applyNs = load(t.applyNs);
        for (std::size_t w = 0; w < 2; ++w) {
            const auto& c = t.conflicts[w];
            s.conflicts[w] = {c.items.load(), c.fine.load(), c.memory.load(), c.templateBuilt.load(), c.templateShared.load(), c.pending.load(), c.proofs.load(), c.global.load(), c.memoryAll.load(), c.memoryForeign.load(), c.memoryDeclared.load()};
        }
        s.overflowedReads = t.overflowedReads.load();
        s.overflowedWrites = t.overflowedWrites.load();
        return s;
    }

    void flipM3() {
        auto& state = m3();
        // Close the frame first: stamps from here on belong to the next one.
        state.frame.fetch_add(1, std::memory_order_relaxed);
        const auto current = takeM3Cumulative();
        if (frameCount < perFrame.size()) {
            perFrame[frameCount++] = {current.any - m3Last.any, current.collected - m3Last.collected, current.hle - m3Last.hle, current.driver - m3Last.driver, current.guest - m3Last.guest, (current.any - m3Last.any) - (current.repeat - m3Last.repeat)};
        }
        m3Last = current;
        static const std::uint64_t sweepEvery = [] {
            const char* text = std::getenv("APS5_WT_FAULT_CENSUS_SWEEP");
            return text != nullptr ? static_cast<std::uint64_t>(std::strtoull(text, nullptr, 10)) : 0ull;
        }();
#ifdef _WIN32
        if (sweepEvery != 0 && ++flipsSinceSweep >= sweepEvery) {
            flipsSinceSweep = 0;
            const auto sweep = ::GuestArena::GuestArenaWriteWatchSweep_nid_no_patch();
            ++sweeps;
            sweepDirty += sweep.dirty;
            sweepNs += sweep.walkNs + sweep.queryNs;
        }
#else
        static_cast<void>(sweepEvery);
#endif
    }

    void report(std::int64_t now) {
        const double f = frames != 0 ? static_cast<double>(frames) : 1.0;
        const double spanMs = static_cast<double>(now - windowStart) / 1e6;
        const auto ms = [&](std::uint64_t ns) { return static_cast<double>(ns) / 1e6 / f; };
        char item[512];
        if (IdleAttrib()) {
            const auto s = takeM1(now);
            const auto d = [](std::uint64_t a, std::uint64_t b) { return a - b; };
            std::string line = "[stage4-m1] frames=" + std::to_string(frames);
            std::snprintf(item, sizeof(item), " ms_per_frame=%.2f committer(ms/f):", spanMs / f);
            line += item;
            double committerBlocked = 0;
            for (std::size_t state = 0; state < CommitterStates; ++state) {
                const auto value = static_cast<double>(std::max<std::int64_t>(0, s.clock[state] - m1Base.clock[state])) / 1e6 / f;
                if (blockedState(state)) committerBlocked += value;
                std::snprintf(item, sizeof(item), " %s=%.2f", CommitterStateNames[state], value);
                line += item;
            }
            std::snprintf(item, sizeof(item), " blocked=%.2f | q0 waits (n/f wait blocked neither neither_all ms/f):", committerBlocked);
            line += item;
            double neither = 0;
            double neitherAll = 0;
            for (std::size_t k = 0; k < WaitKinds; ++k) {
                const auto n = static_cast<double>(d(s.count[k], m1Base.count[k])) / f;
                const auto w = ms(d(s.waitNs[k], m1Base.waitNs[k]));
                const auto b = ms(d(s.blockedNs[k], m1Base.blockedNs[k]));
                const auto x = ms(d(s.neitherNs[k], m1Base.neitherNs[k]));
                const auto xa = ms(d(s.neitherAllNs[k], m1Base.neitherAllNs[k]));
                neither += x;
                neitherAll += xa;
                if (n == 0) continue;
                std::snprintf(item, sizeof(item), " %s=%.1f/%.2f/%.2f/%.2f/%.2f", WaitNames[k], n, w, b, x, xa);
                line += item;
            }
            std::snprintf(item, sizeof(item), " | neither=%.2f neither_all=%.2f committer_blocked_q0_not_waiting=%.2f", neither, neitherAll, std::max(0.0, committerBlocked - neitherAll));
            line += item;
            AgcDriver::ProfilePrint_nid_no_patch("%s\n", line.c_str());

            line = "[stage4-m1-matrix] q0 blocked part x committer state (ms/f):";
            for (std::size_t k = 0; k < WaitKinds; ++k) {
                if (s.count[k] == m1Base.count[k]) continue;
                line += std::string(" ") + WaitNames[k] + "{";
                for (std::size_t state = 0; state < CommitterStates; ++state) {
                    std::snprintf(item, sizeof(item), "%s%s=%.2f", state == 0 ? "" : " ", CommitterStateNames[state], ms(d(s.overlapNs[k][state], m1Base.overlapNs[k][state])));
                    line += item;
                }
                line += "}";
            }
            AgcDriver::ProfilePrint_nid_no_patch("%s\n", line.c_str());

            struct Cell {
                std::size_t index;
                double neither;
            };
            std::vector<Cell> cells;
            for (std::size_t c = 0; c < LabelCells; ++c) {
                if (s.labelCount[c] == m1Base.labelCount[c]) continue;
                cells.push_back({c, ms(d(s.labelNeitherNs[c], m1Base.labelNeitherNs[c]))});
            }
            std::sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) { return a.neither > b.neither; });
            line = "[stage4-m1-label] end/reason/writer n/f wait blocked neither (ms/f):";
            for (std::size_t i = 0; i < std::min<std::size_t>(cells.size(), 20); ++i) {
                const auto c = cells[i].index;
                const auto writer = c % Writers;
                const auto reason = (c / Writers) % Reasons;
                const auto end = c / Writers / Reasons;
                std::snprintf(item, sizeof(item), " %s/%s/%s=%.2f/%.3f/%.3f/%.3f", WaitEndNames[end], LabelReasonNames[reason], WriterNames[writer], static_cast<double>(d(s.labelCount[c], m1Base.labelCount[c])) / f, ms(d(s.labelWaitNs[c], m1Base.labelWaitNs[c])), ms(d(s.labelBlockedNs[c], m1Base.labelBlockedNs[c])), cells[i].neither);
                line += item;
            }
            AgcDriver::ProfilePrint_nid_no_patch("%s\n", line.c_str());

            // By writer class (summed over ends and reasons) and, for other queues, by queue id.
            line = "[stage4-m1-writer] label waits by writer n/f wait neither (ms/f):";
            for (std::size_t writer = 0; writer < Writers; ++writer) {
                std::uint64_t n = 0, w = 0, x = 0;
                for (std::size_t c = writer; c < LabelCells; c += Writers) {
                    n += d(s.labelCount[c], m1Base.labelCount[c]);
                    w += d(s.labelWaitNs[c], m1Base.labelWaitNs[c]);
                    x += d(s.labelNeitherNs[c], m1Base.labelNeitherNs[c]);
                }
                if (n == 0) continue;
                std::snprintf(item, sizeof(item), " %s=%.2f/%.3f/%.3f", WriterNames[writer], static_cast<double>(n) / f, ms(w), ms(x));
                line += item;
            }
            line += " | other queues:";
            for (std::size_t queue = 0; queue < 256; ++queue) {
                const auto n = d(s.queueCount[queue], m1Base.queueCount[queue]);
                if (n == 0) continue;
                std::snprintf(item, sizeof(item), " 0x%zx=%.2f/%.3f/%.3f", queue, static_cast<double>(n) / f, ms(d(s.queueWaitNs[queue], m1Base.queueWaitNs[queue])), ms(d(s.queueNeitherNs[queue], m1Base.queueNeitherNs[queue])));
                line += item;
            }
            AgcDriver::ProfilePrint_nid_no_patch("%s\n", line.c_str());
            m1Base = s;
        }
        if (FaultCensus()) {
            const auto count = frameCount;
            std::array<std::uint64_t, 1024> values{};
            const auto column = [&](auto member) {
                for (std::size_t i = 0; i < count; ++i) values[i] = perFrame[i].*member;
            };
            const auto mean = [&](auto member) {
                double sum = 0;
                for (std::size_t i = 0; i < count; ++i) sum += static_cast<double>(perFrame[i].*member);
                return count != 0 ? sum / static_cast<double>(count) : 0.0;
            };
            column(&FrameValues::any);
            const auto p50 = Stage4Model::Percentile(values, count, 0.5);
            const auto p90 = Stage4Model::Percentile(values, count, 0.9);
            const auto max = count != 0 ? *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count)) : 0;
            column(&FrameValues::fresh);
            const auto freshP90 = Stage4Model::Percentile(values, count, 0.9);
            const auto c = takeM3Cumulative();
            std::snprintf(item, sizeof(item), "[stage4-m3] frames=%zu pages/f(4KiB): any mean=%.0f p50=%llu p90=%llu max=%llu; not-written-last-frame mean=%.0f p90=%llu;", count, mean(&FrameValues::any), static_cast<unsigned long long>(p50), static_cast<unsigned long long>(p90), static_cast<unsigned long long>(max), mean(&FrameValues::fresh), static_cast<unsigned long long>(freshP90));
            std::string line = item;
            std::snprintf(item, sizeof(item), " by writer: collected=%.0f (guest-only=%.0f) hle=%.0f driver=%.0f; blocks64k/f=%.0f;", mean(&FrameValues::collected), mean(&FrameValues::guest), mean(&FrameValues::hle), mean(&FrameValues::driver), static_cast<double>(c.blocks - m3Report.blocks) / f);
            line += item;
            std::snprintf(item, sizeof(item), " hle calls/f=%.1f KiB/f=%.0f; driver stamped calls/f=%.1f KiB/f=%.0f, large (>256KiB, not stamped) calls/f=%.2f MiB/f=%.1f; leaves=%zu",
                          static_cast<double>(c.hleCalls - m3Report.hleCalls) / f, static_cast<double>(c.hleBytes - m3Report.hleBytes) / 1024.0 / f,
                          static_cast<double>(c.driverCalls - m3Report.driverCalls) / f, static_cast<double>(c.driverBytes - m3Report.driverBytes) / 1024.0 / f,
                          static_cast<double>(c.largeCalls - m3Report.largeCalls) / f, static_cast<double>(c.largeBytes - m3Report.largeBytes) / 1048576.0 / f, m3().any.Leaves());
            line += item;
            if (sweeps != 0) {
                std::snprintf(item, sizeof(item), "; sweep: %llu sweeps, dirty-at-flip mean=%.0f pages, %.1f ms each", static_cast<unsigned long long>(sweeps), static_cast<double>(sweepDirty) / static_cast<double>(sweeps), static_cast<double>(sweepNs) / 1e6 / static_cast<double>(sweeps));
                line += item;
                sweeps = sweepDirty = sweepNs = 0;
            }
            AgcDriver::ProfilePrint_nid_no_patch("%s\n", line.c_str());
            m3Report = c;
            frameCount = 0;
        }
        if (ResolveCensus()) {
            const auto s = takeM4();
            std::string line = "[stage4-m4] frames=" + std::to_string(frames) + " items/f:";
            std::uint64_t totalItems = 0;
            for (std::size_t k = 0; k < ItemKinds; ++k) {
                const auto n = s.items[k] - m4Base.items[k];
                totalItems += n;
                std::snprintf(item, sizeof(item), " %s=%.1f", ItemKindNames[k], static_cast<double>(n) / f);
                line += item;
            }
            double resolve = 0, apply = 0, mixed = 0, wait = 0;
            std::string buckets;
            for (std::size_t b = 0; b < Buckets; ++b) {
                const auto value = ms(s.bucketNs[b] - m4Base.bucketNs[b]);
                if (b < FirstApplyBucket) resolve += value;
                else if (b < FirstMixedBucket) apply += value;
                else if (b == static_cast<std::size_t>(Bucket::Wait)) wait += value;
                else mixed += value;
                std::snprintf(item, sizeof(item), "%s%s=%.2f", b == 0 ? "" : " ", BucketNames[b], value);
                buckets += item;
                if (b + 1 == FirstApplyBucket) buckets += " |";
                if (b + 1 == FirstMixedBucket) buckets += " |";
            }
            std::snprintf(item, sizeof(item), "; ms/f resolve=%.2f apply=%.2f mixed=%.2f wait=%.2f; per kind resolve/apply us per item:", resolve, apply, mixed, wait);
            line += item;
            for (std::size_t k = 0; k < ItemKinds; ++k) {
                const auto n = s.items[k] - m4Base.items[k];
                if (n == 0) continue;
                std::snprintf(item, sizeof(item), " %s=%.1f/%.1f", ItemKindNames[k], static_cast<double>(s.resolveNs[k] - m4Base.resolveNs[k]) / 1e3 / static_cast<double>(n), static_cast<double>(s.applyNs[k] - m4Base.applyNs[k]) / 1e3 / static_cast<double>(n));
                line += item;
            }
            line += "; buckets ms/f: " + buckets;
            AgcDriver::ProfilePrint_nid_no_patch("%s\n", line.c_str());
            line = "[stage4-m4-conflict] resolving items (draw+dispatch), % conflicting:";
            for (std::size_t w = 0; w < 2; ++w) {
                std::array<std::uint64_t, 11> v{};
                for (std::size_t i = 0; i < v.size(); ++i) v[i] = s.conflicts[w][i] - m4Base.conflicts[w][i];
                const double items = v[0] != 0 ? static_cast<double>(v[0]) : 1.0;
                const auto pct = [&](std::uint64_t value) { return 100.0 * static_cast<double>(value) / items; };
                std::snprintf(item, sizeof(item), " K=%zu items/f=%.1f fine=%.1f%% (memory=%.1f%% template-built=%.1f%%) memory-any-phase=%.1f%% memory-foreign=%.1f%% memory-declared=%.1f%% template-shared=%.1f%% pending-serial=%.1f%% proof-break=%.1f%% global=%.1f%%;", WindowSizes[w], static_cast<double>(v[0]) / f, pct(v[1]), pct(v[2]), pct(v[3]), pct(v[8]), pct(v[9]), pct(v[10]), pct(v[4]), pct(v[5]), pct(v[6]), pct(v[7]));
                line += item;
            }
            std::snprintf(item, sizeof(item), " range sets overflowed: reads %llu writes %llu of %llu items", static_cast<unsigned long long>(s.overflowedReads - m4Base.overflowedReads), static_cast<unsigned long long>(s.overflowedWrites - m4Base.overflowedWrites), static_cast<unsigned long long>(totalItems));
            line += item;
            AgcDriver::ProfilePrint_nid_no_patch("%s\n", line.c_str());
            m4Base = s;
        }
    }

    std::mutex mutex;
    std::int64_t windowStart = 0;
    std::uint64_t frames = 0;
    M1Snapshot m1Base;
    M4Snapshot m4Base;
    M3Cumulative m3Last{};
    M3Cumulative m3Report{};
    std::array<FrameValues, 1024> perFrame{};
    std::size_t frameCount = 0;
    std::uint64_t flipsSinceSweep = 0;
    std::uint64_t sweeps = 0;
    std::uint64_t sweepDirty = 0;
    std::uint64_t sweepNs = 0;
};

Reporter& reporter() {
    static auto* value = new Reporter();
    return *value;
}

}

void Flip() {
    if (!Tracking() && !FaultCensus()) return;
    reporter().Flip();
}

}
