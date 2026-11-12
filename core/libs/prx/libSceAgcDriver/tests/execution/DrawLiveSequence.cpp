#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/LiveSequence.hpp"
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

using Sequence = AgcDriver::DriverDetail::LiveSequence<int>;

void Expect(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}

std::vector<std::shared_ptr<int>> Entries(int count) {
    std::vector<std::shared_ptr<int>> out;
    for (int index = 0; index < count; ++index) out.push_back(std::make_shared<int>(index));
    return out;
}

void Draw(Sequence& sequence, std::uint64_t key, std::uint64_t frame, const std::vector<std::shared_ptr<int>>& entries) {
    for (const auto& entry : entries) sequence.Record(sequence.Next(key, frame), entry);
}

void NothingIsPredictedBeforeAFrameEnds() {
    Sequence sequence;
    const auto entries = Entries(3);
    for (const auto& entry : entries) {
        const auto position = sequence.Next(7, 1);
        Expect(sequence.Predict(position).count == 0, "a first frame predicted an entry");
        sequence.Record(position, entry);
    }
}

void TheSamePositionOfThePreviousFrameComesFirst() {
    Sequence sequence;
    const auto entries = Entries(20);
    Draw(sequence, 7, 1, entries);
    for (std::uint32_t ordinal = 0; ordinal < entries.size(); ++ordinal) {
        const auto position = sequence.Next(7, 2);
        Expect(position.valid && position.ordinal == ordinal, "positions do not count up within a frame");
        const auto predicted = sequence.Predict(position);
        Expect(predicted.count >= 1 && predicted.entries[0] == entries[ordinal], "the previous frame's entry at this position is not the first candidate");
        Expect(predicted.count <= Sequence::MaxCandidates, "more candidates than the limit");
        sequence.Record(position, predicted.entries[0]);
    }
}

void AnAlternatingKeyUsesTheFrameBefore() {
    Sequence sequence;
    const auto even = Entries(4), odd = Entries(4);
    Draw(sequence, 7, 1, odd);
    Draw(sequence, 7, 2, even);
    const auto position = sequence.Next(7, 3);
    const auto predicted = sequence.Predict(position);
    Expect(predicted.count >= 2 && predicted.entries[0] == even[0] && predicted.entries[1] == odd[0], "the two earlier frames are not tried in order");
}

void AKeyThatSkipsFramesKeepsItsLastFrame() {
    Sequence sequence;
    const auto entries = Entries(4);
    Draw(sequence, 7, 1, entries);
    Draw(sequence, 9, 2, Entries(2));
    const auto predicted = sequence.Predict(sequence.Next(7, 3));
    Expect(predicted.count >= 1 && predicted.entries[0] == entries[0], "a key that skipped a frame lost its sequence");
}

void NeighboursCoverAnInsertedOrRemovedDraw() {
    Sequence sequence;
    const auto entries = Entries(6);
    Draw(sequence, 7, 1, entries);
    sequence.Next(7, 2);
    const auto position = sequence.Next(7, 2);
    const auto predicted = sequence.Predict(position);
    Expect(predicted.Holds(entries[0].get()) && predicted.Holds(entries[1].get()) && predicted.Holds(entries[2].get()), "the neighbouring positions are not candidates");
    Expect(!predicted.Holds(entries[4].get()), "a distant position is a candidate");
}

void CandidatesAreDistinctAndSkipHoles() {
    Sequence sequence;
    const auto shared = std::make_shared<int>(1);
    Draw(sequence, 7, 1, {shared, shared, nullptr, shared});
    Draw(sequence, 7, 2, {shared, shared, nullptr, shared});
    sequence.Next(7, 3);
    const auto second = sequence.Predict(sequence.Next(7, 3));
    Expect(second.count == 1 && second.entries[0] == shared, "one entry is offered more than once");
    const auto third = sequence.Predict(sequence.Next(7, 3));
    Expect(third.count == 1 && third.entries[0] == shared, "a hole stops the neighbours from being offered");
}

void UnrecordedPositionsPredictNothingLater() {
    Sequence sequence;
    const auto entries = Entries(2);
    Draw(sequence, 7, 1, entries);
    sequence.Next(7, 2);
    sequence.Next(7, 2);
    sequence.Next(7, 3);
    const auto predicted = sequence.Predict(sequence.Next(7, 3));
    Expect(predicted.count == 1 && predicted.entries[0] == entries[1], "a frame that recorded nothing hid the frame before it");
}

void IdleKeysAreDropped() {
    Sequence sequence;
    Draw(sequence, 7, 1, Entries(2));
    for (std::uint64_t frame = 2; frame <= Sequence::PrunePeriod * 2; ++frame) Draw(sequence, 9, frame, Entries(1));
    Expect(sequence.Keys() == 1, "an idle key was kept");
    sequence.Clear();
    Expect(sequence.Keys() == 0, "clearing left keys behind");
}

void InvalidPositionsAreIgnored() {
    Sequence sequence;
    Sequence::Position position;
    sequence.Record(position, std::make_shared<int>(1));
    Expect(sequence.Predict(position).count == 0 && sequence.Keys() == 0, "an invalid position was used");
}

}

int main() {
    try {
        NothingIsPredictedBeforeAFrameEnds();
        TheSamePositionOfThePreviousFrameComesFirst();
        AnAlternatingKeyUsesTheFrameBefore();
        AKeyThatSkipsFramesKeepsItsLastFrame();
        NeighboursCoverAnInsertedOrRemovedDraw();
        CandidatesAreDistinctAndSkipHoles();
        UnrecordedPositionsPredictNothingLater();
        IdleKeysAreDropped();
        InvalidPositionsAreIgnored();
        std::cout << "The live sequence offers the entries earlier frames used at a draw's position before any other\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
