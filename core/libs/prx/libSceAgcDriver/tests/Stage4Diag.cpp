// Stage 4 measurement helpers (Execution/include/Stage4DiagModel.hpp) and a smoke run of the driver
// hooks (Stage4Diag.cpp) with every measurement on.
#include "prx/libSceAgcDriver/Execution/include/Stage4DiagModel.hpp"
#include "prx/libSceAgcDriver/Execution/include/Stage4Diag.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace AgcDriver::Stage4Model;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void CheckStateClock() {
    StateClock<3> clock;
    Require(clock.Read(50) == StateClock<3>::Totals{}, "an unstarted clock counts time");
    clock.Set(0, 100);
    clock.Set(1, 150);
    auto totals = clock.Read(200);
    Require(totals[0] == 50 && totals[1] == 50 && totals[2] == 0, "state totals up to now");
    clock.Set(2, 260);
    totals = clock.Read(300);
    Require(totals[0] == 50 && totals[1] == 110 && totals[2] == 40, "state totals after a second transition");
    const auto before = clock.Read(270);
    clock.Set(0, 280);
    const auto after = clock.Read(290);
    const auto overlap = Overlap(before, after);
    Require(overlap[2] == 10 && overlap[0] == 10 && overlap[1] == 0, "overlap of an interval with the other thread's states");
    // A reader never sees a torn snapshot: totals plus the open state always sum to now - start.
    StateClock<2> live;
    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> clockNow{1000};
    live.Set(0, 1000);
    std::thread writer([&] {
        for (std::int64_t t = 1001; t < 200000; ++t) {
            live.Set(static_cast<std::size_t>(t & 1), t);
            clockNow.store(t, std::memory_order_release);
        }
        stop = true;
    });
    std::int64_t checks = 0;
    while (!stop) {
        const auto now = clockNow.load(std::memory_order_acquire) + 1'000'000;
        const auto read = live.Read(now);
        Require(read[0] + read[1] == now - 1000, "a torn state clock snapshot");
        ++checks;
    }
    writer.join();
    Require(checks > 0, "the reader never ran");
}

void CheckPageStamps() {
    PageStampTable table;
    Require(table.Leaves() == 0, "a fresh table holds leaves");
    auto result = StampPage(table, 0x12345, 1);
    Require(result.fresh && !result.repeat, "first stamp in frame 1");
    Require(!StampPage(table, 0x12345, 1).fresh, "second stamp in the same frame is fresh");
    result = StampPage(table, 0x12345, 2);
    Require(result.fresh && result.repeat, "a page written in consecutive frames is not a repeat");
    result = StampPage(table, 0x12345, 4);
    Require(result.fresh && !result.repeat, "a page skipping a frame is a repeat");
    Require(table.Load(0x12345) == 4 && table.Load(0x12346) == 0, "stamps read back");
    // Pages a TiB apart land in different leaves; pages past the 47-bit space are ignored.
    Require(StampPage(table, (std::uint64_t{1} << 40) >> 12, 1).fresh, "a far page");
    Require(table.Leaves() == 2, "leaves are allocated per GiB touched");
    Require(table.Exchange(std::uint64_t{1} << 40, 1) == 0 && table.Load(std::uint64_t{1} << 40) == 0, "a page beyond 47 bits is stored");
}

void CheckRangeSet() {
    RangeSet set;
    set.Add(100, 200);
    set.Add(300, 400);
    set.Add(0, 50);
    Require(set.Size() == 3 && set[0].begin == 0 && set[1].begin == 100 && set[2].begin == 300, "ranges are kept sorted");
    set.Add(200, 250);
    Require(set.Size() == 3 && set[1].end == 250, "an adjacent range merges");
    set.Add(40, 320);
    Require(set.Size() == 1 && set[0].begin == 0 && set[0].end == 400, "a spanning range merges everything it touches");
    set.Add(10, 10);
    Require(set.Size() == 1, "an empty range is added");
    RangeSet other;
    other.Add(400, 500);
    Require(!set.Overlaps(other) && !other.Overlaps(set), "touching ranges overlap");
    other.Add(399, 400);
    Require(set.Overlaps(other) && other.Overlaps(set), "an overlap is missed");
    RangeSet full;
    for (std::uint64_t i = 0; i < RangeSet::Capacity; ++i) full.Add(i * 1000, i * 1000 + 10);
    Require(full.Size() == RangeSet::Capacity && !full.Overflowed(), "filling to capacity overflows");
    full.Add(5000000, 5000010);
    Require(full.Size() == RangeSet::Capacity && full.Overflowed(), "an overflow is not noted");
    RangeSet probe;
    probe.Add(5000000, 5000001);
    Require(full.Overlaps(probe), "the range added past capacity is lost");
    probe.Clear();
    probe.Add(0, 1);
    Require(full.Overlaps(probe), "an earlier range is lost by the overflow merge");
    RangeSet own;
    own.Add(105, 110);
    const auto foreign = set.Without(own);
    Require(foreign.Empty(), "Without keeps a range overlapping the other set");
    RangeSet two;
    two.Add(0, 10);
    two.Add(100, 110);
    const auto kept = two.Without(own);
    Require(kept.Size() == 1 && kept[0].begin == 0, "Without drops a range overlapping nothing");
    RangeSet merged;
    merged.Add(10, 20);
    merged.Merge(full);
    Require(merged.Overflowed() && merged.Overlaps(probe), "Merge loses ranges or the overflow flag");
}

ItemFootprint Item(std::uint64_t read, std::uint64_t write, const void* object = nullptr, bool built = false) {
    ItemFootprint item;
    if (read != 0) {
        item.reads.Add(read, read + 16);
        item.allReads.Add(read, read + 16);
    }
    if (write != 0) item.writes.Add(write, write + 16);
    item.templateObject = object;
    item.builtTemplate = built;
    item.resolves = true;
    return item;
}

void CheckConflictWindow() {
    ConflictWindow window;
    window.Push(Item(0, 0x1000));
    for (int i = 0; i < 3; ++i) window.Push(Item(0, 0x9000 + static_cast<std::uint64_t>(i) * 0x100));
    const auto reader = Item(0x1008, 0);
    Require(!window.Check(reader, 3).memory, "K=3 sees an item four back");
    Require(window.Check(reader, 8).memory, "K=8 misses an item four back");
    Require(!window.Check(reader, 1).memory, "K=1 checks earlier items");
    auto applyReader = Item(0, 0);
    applyReader.allReads.Add(0x1008, 0x1010);
    const auto applyOnly = window.Check(applyReader, 8);
    Require(applyOnly.memoryAll && !applyOnly.memory && !applyOnly.Fine(), "an apply-phase read counts as a resolve conflict");
    const int object = 0;
    ConflictWindow templates;
    templates.Push(Item(0, 0, &object, true));
    templates.Push(Item(0, 0));
    auto conflict = templates.Check(Item(0, 0, &object), 3);
    Require(conflict.templateShared && conflict.templateBuilt && conflict.Fine() && !conflict.memory, "a template built two back");
    conflict = templates.Check(Item(0, 0, &object), 2);
    Require(!conflict.templateShared && !conflict.Fine(), "K=2 reaches two back");
    ConflictWindow globals;
    auto pending = Item(0, 0);
    pending.pendingChanged = true;
    globals.Push(pending);
    auto label = Item(0, 0);
    label.resolves = false;
    label.breaksProofs = true;
    globals.Push(label);
    conflict = globals.Check(Item(0x10, 0), 3);
    Require(conflict.pending && conflict.proofs && conflict.Global() && !conflict.Fine(), "global stamps");
}

void CheckItemTimer() {
    ItemTimer<4> timer;
    timer.Mark(0, 5);
    Require(!timer.Active() && timer.Totals()[0] == 0, "an inactive timer counts");
    timer.Begin(0);
    timer.Mark(0, 10);
    const bool outer = timer.NestedBegin();
    const bool inner = timer.NestedBegin();
    Require(outer && !inner, "only the outermost nested scope counts");
    timer.NestedEnd(3, 13, 14, inner);
    timer.NestedEnd(3, 12, 15, outer);
    timer.Mark(1, 20);
    timer.End(2, 25);
    const auto& ns = timer.Totals();
    Require(ns[0] == 10 && ns[3] == 3 && ns[1] == 7 && ns[2] == 5, "phase and nested times");
    Require(!timer.Active(), "End leaves the timer active");
}

void CheckPercentile() {
    std::array<std::uint64_t, 8> values{5, 1, 4, 2, 3, 0, 0, 0};
    Require(Percentile(values, 5, 0.5) == 3, "median");
    Require(Percentile(values, 5, 0.0) == 1 && Percentile(values, 5, 1.0) == 5, "extremes");
    Require(Percentile(values, 0, 0.5) == 0, "empty sample");
}

// Every measurement on: the hooks run on a committer and a queue-0 thread and the report prints.
void SmokeHooks() {
    namespace S = AgcDriver::Stage4;
    Require(S::IdleAttrib() && S::FaultCensus() && S::ResolveCensus() && S::Tracking(), "the environment did not turn the measurements on");
    std::atomic<bool> committerIdle{false};
    std::atomic<bool> done{false};
    std::atomic<int> items{0};
    std::thread committer([&] {
        S::BindRole(S::Role::Committer);
        for (int item = 0; !done; ++item) {
            {
                S::WaitScope idle(S::Wait::Idle, true);
                committerIdle = true;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                idle.Blocking();
                std::this_thread::sleep_for(std::chrono::microseconds(300));
                committerIdle = false;
            }
            const std::vector<std::pair<std::uint64_t, std::uint64_t>> reads{{0x10000, 0x10100}};
            const std::vector<std::pair<std::uint64_t, std::uint64_t>> writes{{0x20000, 0x20100}};
            S::BeginItem(1, reads, writes, 7);
            S::Mark(S::Bucket::Pre);
            {
                const S::TrackerScope query(false, 0x30000, 0x1000);
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            S::Mark(S::Bucket::Lookup);
            S::NoteTemplate(&committerIdle, item % 3 == 0);
            {
                const S::TrackerScope stamp(true, 0x20000, 0x100);
            }
            S::Mark(S::Bucket::Record);
            S::EndItem(item % 2 == 0 ? 7 : 8, false);
            ++items;
        }
    });
    std::thread queue0([&] {
        S::BindRole(S::Role::Queue0);
        while (!done) {
            S::WaitScope wait(S::Wait::Label, true);
            wait.SetEnd(1);
            wait.SetReason(S::LabelReason::BehindCompletion);
            wait.SetWriter(S::Writer::OtherTable, 0x28);
            wait.Blocking();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    // 1.35 s of 30 ms frames: one report window (APS5_STAGE4_REPORT_SECONDS=1) closes.
    for (int frame = 0; frame < 45; ++frame) {
        S::NoteCollectedPages(0x40000000, 0x40004000);
        S::NoteHostWrite(0x40002000, 0x3000);
        S::NoteDriverWrite(0x50000000, 0x1000);
        S::NoteDriverWrite(0x60000000, std::size_t{1} << 20);
        S::Flip();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    done = true;
    committer.join();
    queue0.join();
    S::Flip();
    Require(items > 0, "the committer committed nothing");
}

}

int main() {
    // Before any Stage 4 call: the switches are read once.
    SetEnv("APS5_PIPE_IDLE_ATTRIB", "1");
    SetEnv("APS5_WT_FAULT_CENSUS", "1");
    SetEnv("APS5_RESOLVE_CENSUS", "1");
    SetEnv("APS5_STAGE4_REPORT_SECONDS", "1");
    try {
        CheckStateClock();
        CheckPageStamps();
        CheckRangeSet();
        CheckConflictWindow();
        CheckItemTimer();
        CheckPercentile();
        SmokeHooks();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "stage4 diag tests failed: %s\n", error.what());
        return 1;
    }
    std::fprintf(stderr, "stage4 diag tests passed\n");
    return 0;
}
