#include "prx/libSceAgcDriver/Execution/include/Driver/Replay/RegionRecognizer.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::DriverDetail::Replay::RegionMatch;
using AgcDriver::DriverDetail::Replay::RegionRecognizer;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint32_t Type3(std::uint32_t opcode, std::uint32_t bodyWords) {
    return 0xc0000000u | ((bodyWords - 1u) << 16u) | (opcode << 8u);
}

constexpr std::uint32_t DrawIndexAuto = 0x2du;
constexpr std::uint32_t Dispatch = 0x15u;
constexpr std::uint32_t SetContext = 0x69u;
constexpr std::uint32_t Filler = 0x80000000u;

std::vector<std::uint32_t> Stream(std::uint32_t registerValue, std::uint32_t vertices) {
    return {Type3(SetContext, 2), 0x80u, registerValue, Type3(DrawIndexAuto, 2), vertices, 2u, Filler, Type3(Dispatch, 4), 8u, 8u, 1u, 0u};
}

void repeats() {
    RegionRecognizer recognizer;
    const auto first = recognizer.Observe(0, Stream(1, 3));
    Require(first.match == RegionMatch::New && first.packets == 4 && first.draws == 1 && first.dispatches == 1, "a first frame's region was not counted as new with its work");
    recognizer.EndFrame();
    const auto second = recognizer.Observe(0, Stream(1, 3));
    Require(second.match == RegionMatch::Exact && second.patchWords == 0 && !second.stablePatches, "an identical region was not exact");
    recognizer.EndFrame();
    const auto third = recognizer.Observe(0, Stream(2, 3));
    Require(third.match == RegionMatch::Structure && third.patchWords == 1 && !third.stablePatches, "one changed register value was not one patch word");
    Require(third.patchWordsByKind.size() == 1 && third.patchWordsByKind[0].first == (0x300u | SetContext) && third.patchWordsByKind[0].second == 1, "the patch word was not attributed to its packet kind");
    Require(third.alternatingWords == 0, "a value never seen was called alternating");
    recognizer.EndFrame();
    const auto fourth = recognizer.Observe(0, Stream(3, 3));
    Require(fourth.match == RegionMatch::Structure && fourth.patchWords == 1 && fourth.stablePatches, "the same word changing again was not a stable patch site");
    recognizer.EndFrame();
    const auto fifth = recognizer.Observe(0, Stream(3, 6));
    Require(fifth.match == RegionMatch::Structure && fifth.patchWords == 1 && !fifth.stablePatches, "a different word changing kept the earlier patch map");
    Require(fifth.patchWordsByKind[0].first == (0x300u | DrawIndexAuto), "the moved patch word kept its old packet kind");
}

void alternates() {
    RegionRecognizer recognizer;
    for (int frame = 0; frame < 2; ++frame) {
        recognizer.Observe(0, Stream(frame == 0 ? 0x1000u : 0x2000u, 3));
        recognizer.EndFrame();
    }
    const auto third = recognizer.Observe(0, Stream(0x1000u, 3));
    Require(third.match == RegionMatch::Structure && third.patchWords == 1 && third.alternatingWords == 1, "a value equal to the frame before last was not alternating");
    recognizer.EndFrame();
    const auto fourth = recognizer.Observe(0, Stream(0x2000u, 3));
    Require(fourth.alternatingWords == 1 && fourth.stablePatches, "a two-frame cycle was not a stable alternating site");
}

void structureChanges() {
    RegionRecognizer recognizer;
    recognizer.Observe(0, Stream(1, 3));
    recognizer.EndFrame();
    auto longer = Stream(1, 3);
    longer.push_back(Filler);
    const auto grown = recognizer.Observe(0, longer);
    Require(grown.match == RegionMatch::New && grown.patchWords == 0 && grown.packets == 5, "a region of another length was compared");
    recognizer.EndFrame();
    auto other = longer;
    other[3] = Type3(0x35u, 2);
    const auto swapped = recognizer.Observe(0, other);
    Require(swapped.match == RegionMatch::New && swapped.patchWords == 0 && swapped.patchWordsByKind.empty(), "a region with another packet header was matched");
    recognizer.EndFrame();
    const auto after = recognizer.Observe(0, other);
    Require(after.match == RegionMatch::Exact, "a region after a structure change did not match its own repeat");
}

void positionsAndQueues() {
    RegionRecognizer recognizer;
    recognizer.Observe(0, Stream(1, 3));
    recognizer.Observe(0, Stream(2, 3));
    recognizer.Observe(0x20, Stream(7, 3));
    recognizer.EndFrame();
    Require(recognizer.Observe(0, Stream(1, 3)).match == RegionMatch::Exact, "the first region of a queue did not match the first of the frame before");
    Require(recognizer.Observe(0x20, Stream(7, 3)).match == RegionMatch::Exact, "a second queue's region was compared with the first queue's");
    Require(recognizer.Observe(0, Stream(2, 3)).match == RegionMatch::Exact, "the second region of a queue did not match by position");
    Require(recognizer.Observe(0, Stream(2, 3)).match == RegionMatch::New, "a region past the previous frame's count was matched");
    recognizer.EndFrame();
    Require(recognizer.Observe(0x28, Stream(1, 3)).match == RegionMatch::New, "a queue with no earlier frame was matched");
}

void truncated() {
    RegionRecognizer recognizer;
    const std::vector<std::uint32_t> cut{Type3(DrawIndexAuto, 2), 3u};
    const auto first = recognizer.Observe(0, cut);
    Require(first.packets == 1 && first.draws == 1, "a packet running past its region was not counted once");
    recognizer.EndFrame();
    Require(recognizer.Observe(0, cut).match == RegionMatch::Exact, "a truncated region did not match its repeat");
    Require(recognizer.Observe(0, {}).match == RegionMatch::New && recognizer.Observe(0, {}).packets == 0, "an empty region was not handled");
}

}

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "repeats") repeats();
        else if (mode == "alternates") alternates();
        else if (mode == "structure") structureChanges();
        else if (mode == "positions") positionsAndQueues();
        else if (mode == "truncated") truncated();
        else throw std::runtime_error("unknown case");
        std::cout << "replay region recognizer: " << mode << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
