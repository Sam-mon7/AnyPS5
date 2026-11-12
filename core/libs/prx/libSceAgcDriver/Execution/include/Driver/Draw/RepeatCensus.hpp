#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_REPEATCENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_REPEATCENSUS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace AgcDriver::DriverDetail::RepeatCensus {

struct Draw {
    std::uint64_t site = 0;
    std::uint64_t coarse = 0;
    std::uint64_t structure = 0;
    std::uint64_t target = 0;
    std::uint64_t addresses = 0;
    std::uint64_t constants = 0;
    std::uint64_t arguments = 0;
    bool live = false;
};

struct Item {
    Draw draw;
    std::uint32_t opcode = 0;
    bool isDraw = false;
};

constexpr std::size_t SizeClasses = 4;
constexpr std::size_t MaxLag = 8;

struct Summary {
    std::uint64_t regions = 0;
    std::uint64_t regionsSameSequence = 0;
    std::uint64_t regionsIdentical = 0;
    std::uint64_t regionsValuesOnly = 0;
    std::uint64_t draws = 0;
    std::uint64_t drawsSameSequence = 0;
    std::uint64_t drawsIdentical = 0;
    std::uint64_t drawsValuesOnly = 0;
    std::uint64_t drawsStructure = 0;
    std::uint64_t drawsAddresses = 0;
    std::uint64_t drawsConstants = 0;
    std::uint64_t drawsArguments = 0;
    std::uint64_t drawsNotLive = 0;
    std::uint64_t drawsUnmatched = 0;
    std::uint64_t drawsUnmatchedPrefix = 0;
    std::uint64_t drawsInIdenticalRegions = 0;
    std::uint64_t drawsInValuesOnlyRegions = 0;
    std::uint64_t targetBreaks = 0;
    std::array<std::uint64_t, SizeClasses> regionsBySize{};
    std::array<std::uint64_t, SizeClasses> drawsBySize{};
    std::array<std::uint64_t, SizeClasses> sameSequenceDrawsBySize{};
    std::map<std::uint32_t, std::uint64_t> packetBreaks;
};

struct RegionRow {
    std::size_t first = 0;
    std::size_t draws = 0;
    std::uint64_t target = 0;
    std::uint32_t endOpcode = 0;
    bool endTarget = false;
    bool sameSequence = false;
    std::uint64_t identical = 0;
    std::uint64_t valuesOnly = 0;
    std::uint64_t structure = 0;
    std::uint64_t notLive = 0;
    std::uint64_t prefix = 0;
};

bool Enabled();
std::uint64_t Mix(std::uint64_t seed, std::uint64_t value);
Summary Compare(const std::vector<Item>& previous, const std::vector<Item>& current, std::vector<RegionRow>* rows = nullptr);
void NotePacket(std::uint32_t opcode);
void NoteDraw(const Draw& draw);
void EndFrame();

}

#endif
