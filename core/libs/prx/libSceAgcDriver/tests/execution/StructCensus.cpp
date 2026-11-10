// APS5_ARCHB_STRUCT_REPEAT_CENSUS: the frame-by-frame comparison (StructCensus::Tracker) on synthetic
// commit streams: identical frames, frames whose per-draw data changes, a pass that gains a draw, and
// the component split of shape-aligned commits.
#include "prx/libSceAgcDriver/Graphics/include/StructCensus.hpp"
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace AgcDriver::Graphics::StructCensus;

void check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("struct census: " + what);
}

Commit Draw(std::uint64_t shape, bool passBegin, std::uint64_t data = 0, std::uint64_t push = 0) {
    Commit commit;
    commit.kind = Kind::Draw;
    commit.passBegin = passBegin;
    commit.shape = shape;
    commit.components[ComponentPush] = push;
    commit.components[ComponentData] = data;
    return commit;
}

Commit Dispatch(std::uint64_t shape) {
    Commit commit;
    commit.kind = Kind::Dispatch;
    commit.shape = shape;
    return commit;
}

// Two passes of 10 draws and a dispatch between them.
void Frame(Tracker& tracker, std::uint64_t data, bool extraDraw = false, std::uint64_t pushOf3 = 0) {
    for (std::uint64_t i = 0; i < 10; ++i) tracker.Note(Draw(100 + i, i == 0, data + i, i == 3 ? pushOf3 : 0));
    tracker.Note(Dispatch(7));
    for (std::uint64_t i = 0; i < 10; ++i) {
        tracker.Note(Draw(200 + i, i == 0, data + 50 + i));
        if (extraDraw && i == 4) tracker.Note(Draw(999, false, data));
    }
    tracker.EndFrame();
}

void IdenticalFrames() {
    Tracker tracker;
    Frame(tracker, 0);
    check(tracker.Totals().frames == 0, "the first frame was compared");
    Frame(tracker, 0);
    const auto& t = tracker.Totals();
    check(t.frames == 1 && t.commits == 21 && t.draws == 20 && t.dispatches == 1, "counts");
    check(t.passes == 2 && t.passDraws == 20, "passes " + std::to_string(t.passes));
    for (std::size_t level = 0; level < Levels; ++level) {
        check(t.repeated[level] == 21, "identical frames: level " + std::to_string(level) + " repeated " + std::to_string(t.repeated[level]));
        check(t.inLongRuns[level] == 21, "identical frames: level " + std::to_string(level) + " in long runs " + std::to_string(t.inLongRuns[level]));
        check(t.passesRepeated[level] == 2 && t.passDrawsRepeated[level] == 20, "identical frames: passes repeated at level " + std::to_string(level));
    }
    check(t.aligned == 21 && t.alignedSame[ComponentData] == 21, "identical frames: alignment");
}

void DataChanges() {
    Tracker tracker;
    Frame(tracker, 0);
    Frame(tracker, 1000);
    const auto& t = tracker.Totals();
    check(t.repeated[LevelStructure] == 21 && t.repeated[LevelAddresses] == 21, "changed data: structure and addresses repeat");
    check(t.repeated[LevelFull] == 1, "changed data: only the dispatch repeats in full (" + std::to_string(t.repeated[LevelFull]) + ")");
    check(t.passesRepeated[LevelStructure] == 2 && t.passesRepeated[LevelFull] == 0, "changed data: passes");
    check(t.alignedSame[ComponentData] == 1 && t.alignedSame[ComponentPush] == 21, "changed data: component split");
}

void PassGainsADraw() {
    Tracker tracker;
    Frame(tracker, 0);
    Frame(tracker, 0, true, 5);
    const auto& t = tracker.Totals();
    check(t.commits == 22, "a gained draw: commits");
    // Every commit but the new one repeats in shape; draw 3 of the first pass differs in push only.
    check(t.repeated[LevelShape] == 21 && t.repeated[LevelStructure] == 20, "a gained draw: repeated " + std::to_string(t.repeated[LevelShape]) + "/" + std::to_string(t.repeated[LevelStructure]));
    check(t.passesRepeated[LevelShape] == 1 && t.passDrawsRepeated[LevelShape] == 10, "a gained draw: only the first pass repeats in shape");
    check(t.passesRepeated[LevelStructure] == 0, "a gained draw: no pass repeats in structure");
    // Runs >= 8 in shape: the first pass, the dispatch and the second pass up to the new draw (16); then 5.
    check(t.inLongRuns[LevelShape] == 16, "a gained draw: long shape runs " + std::to_string(t.inLongRuns[LevelShape]));
    check(t.aligned == 21 && t.alignedSame[ComponentPush] == 20, "a gained draw: alignment " + std::to_string(t.aligned) + " push " + std::to_string(t.alignedSame[ComponentPush]));
}

// APS5_ARCHB_DRAW_REUSE_CENSUS (B0): a draw would hit when the previous frame drew its structure
// at least occurrence+1 times; nothing hits in the first frame; an empty frame keeps the previous.
void ReuseWouldHit() {
    ReuseTracker tracker;
    check(!tracker.Note(1) && !tracker.Note(1) && !tracker.Note(2), "first frame: no hits");
    tracker.EndFrame();
    check(tracker.Note(1), "second frame: first occurrence of 1 hits");
    check(tracker.Note(1), "second frame: second occurrence of 1 hits");
    check(!tracker.Note(1), "second frame: a third occurrence of 1 misses");
    check(!tracker.Note(3), "second frame: a new structure misses");
    check(tracker.Note(2), "second frame: 2 hits");
    tracker.EndFrame();
    tracker.EndFrame();  // an empty frame: the previous frame stays
    check(tracker.Note(1) && tracker.Note(1) && tracker.Note(1), "third frame: 1 drawn three times before");
    check(tracker.Note(3) && !tracker.Note(4), "third frame: 3 hits, 4 misses");
    tracker.EndFrame();
    check(!tracker.Note(2), "fourth frame: 2 was not in the third frame");
}

}

int main() {
    try {
        IdenticalFrames();
        DataChanges();
        PassGainsADraw();
        ReuseWouldHit();
        std::puts("struct census tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
