#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/RepeatCensus.hpp"
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using namespace AgcDriver::DriverDetail::RepeatCensus;

void Expect(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}

Item Drawn(std::uint64_t site, std::uint64_t target, std::uint64_t structure = 1, std::uint64_t addresses = 1, std::uint64_t constants = 1, std::uint64_t arguments = 1, bool live = true) {
    Item item;
    item.isDraw = true;
    item.draw = {site, site, structure, target, addresses, constants, arguments, live};
    return item;
}

Item Packet(std::uint32_t opcode) {
    Item item;
    item.opcode = opcode;
    return item;
}

void IdenticalFramesAreIdenticalRegions() {
    const std::vector<Item> frame{Drawn(1, 10), Drawn(2, 10), Packet(0x49), Drawn(3, 20), Drawn(4, 20), Drawn(5, 20)};
    std::vector<RegionRow> rows;
    const auto summary = Compare(frame, frame, &rows);
    Expect(summary.regions == 2 && summary.regionsSameSequence == 2 && summary.regionsIdentical == 2, "identical frames give identical regions");
    Expect(summary.draws == 5 && summary.drawsIdentical == 5 && summary.drawsInIdenticalRegions == 5 && summary.drawsUnmatched == 0, "identical frames give identical draws");
    Expect(summary.packetBreaks.at(0x49) == 1 && summary.targetBreaks == 0, "the packet between the regions is counted once");
    Expect(rows.size() == 2 && rows[0].draws == 2 && rows[0].endOpcode == 0x49 && rows[1].draws == 3, "rows describe the regions");
}

void TargetChangeSplitsARegion() {
    const std::vector<Item> frame{Drawn(1, 10), Drawn(2, 20), Drawn(3, 20)};
    const auto summary = Compare(frame, frame);
    Expect(summary.regions == 2 && summary.targetBreaks == 1 && summary.regionsBySize[0] == 1 && summary.regionsBySize[1] == 1, "a target change ends the region");
}

void ValueChangesKeepTheSequence() {
    const std::vector<Item> before{Drawn(1, 10), Drawn(2, 10), Drawn(3, 10), Drawn(4, 10)};
    const std::vector<Item> after{Drawn(1, 10), Drawn(2, 10, 1, 7), Drawn(3, 10, 1, 1, 7, 7), Drawn(4, 10, 9)};
    const auto summary = Compare(before, after);
    Expect(summary.regionsSameSequence == 1 && summary.regionsIdentical == 0 && summary.regionsValuesOnly == 0, "a structure change keeps the region out of the replayable classes");
    Expect(summary.drawsIdentical == 1 && summary.drawsValuesOnly == 2 && summary.drawsStructure == 1, "draws are classified by what changed");
    Expect(summary.drawsAddresses == 1 && summary.drawsConstants == 1 && summary.drawsArguments == 1, "value classes are counted separately");
    const std::vector<Item> values{Drawn(1, 10), Drawn(2, 10, 1, 7), Drawn(3, 10), Drawn(4, 10)};
    const auto valuesOnly = Compare(before, values);
    Expect(valuesOnly.regionsValuesOnly == 1 && valuesOnly.drawsInValuesOnlyRegions == 4, "a region whose draws differ only in values is values-only");
}

void ChangedSequenceIsUnmatchedWithItsPrefix() {
    const std::vector<Item> before{Drawn(1, 10), Drawn(2, 10), Drawn(3, 10), Packet(0x10), Drawn(8, 10)};
    const std::vector<Item> after{Drawn(1, 10), Drawn(2, 10), Drawn(4, 10), Drawn(3, 10), Packet(0x10), Drawn(8, 10)};
    const auto summary = Compare(before, after);
    Expect(summary.regions == 2 && summary.regionsSameSequence == 1, "only the unchanged region matches");
    Expect(summary.drawsUnmatched == 4 && summary.drawsUnmatchedPrefix == 2 && summary.drawsSameSequence == 1, "the changed region reports its common prefix");
}

void RegionsMatchOutOfOrderOnce() {
    const std::vector<Item> before{Drawn(1, 10), Drawn(2, 10), Packet(0x10), Drawn(5, 10), Drawn(6, 10)};
    const std::vector<Item> after{Drawn(5, 10), Drawn(6, 10), Packet(0x10), Drawn(1, 10), Drawn(2, 10), Packet(0x10), Drawn(1, 10), Drawn(2, 10)};
    const auto summary = Compare(before, after);
    Expect(summary.regions == 3 && summary.regionsSameSequence == 2 && summary.drawsUnmatched == 2, "each earlier region matches at most one later region");
}

void DrawsOffTheLivePathAreCounted() {
    const std::vector<Item> before{Drawn(1, 10), Drawn(2, 0, 0, 0, 0, 0, false)};
    const std::vector<Item> after{Drawn(1, 10), Drawn(2, 0, 0, 0, 0, 0, false)};
    const auto summary = Compare(before, after);
    Expect(summary.regions == 1 && summary.targetBreaks == 0, "a draw off the live path has no known target and does not split the region");
    Expect(summary.drawsNotLive == 1 && summary.drawsIdentical == 1 && summary.regionsIdentical == 0, "a draw off the live path keeps its region from being identical");
    const std::vector<Item> learned{Drawn(1, 10), Drawn(2, 10)};
    Expect(Compare(before, learned).drawsStructure == 1, "a draw that just became live is a structure change");
}

}

int main() {
    try {
        IdenticalFramesAreIdenticalRegions();
        TargetChangeSplitsARegion();
        ValueChangesKeepTheSequence();
        ChangedSequenceIsUnmatchedWithItsPrefix();
        RegionsMatchOutOfOrderOnce();
        DrawsOffTheLivePathAreCounted();
        std::cout << "The repeat census matches regions by draw sequence and classifies draws by what changed since the previous frame\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
