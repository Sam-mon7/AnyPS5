#include "prx/libSceAgcDriver/Execution/include/Driver/Replay/RegionRecognizer.hpp"

#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"

#include <algorithm>

namespace AgcDriver::DriverDetail::Replay {

namespace {

std::uint32_t kindOf(std::uint32_t header) {
    if (Pm4::FillerPacket(header)) return 0x200u;
    const auto type = header >> 30u;
    return type == 3u ? 0x300u | ((header >> 8u) & 0xffu) : type << 8u;
}

}

const RegionRecognizer::Region* RegionRecognizer::at(const Frame& frame, std::uint32_t queue, std::size_t index) {
    const auto found = frame.find(queue);
    return found != frame.end() && index < found->second.size() ? &found->second[index] : nullptr;
}

RegionReport RegionRecognizer::Observe(std::uint32_t queue, std::span<const std::uint32_t> commands) {
    RegionReport report;
    auto& regions = current[queue];
    const auto index = regions.size();
    const auto* before = at(previous, queue, index);
    const auto* earlier = at(older, queue, index);
    bool structure = before != nullptr;
    const bool sameLength = before != nullptr && before->words.size() == commands.size();
    Region region;
    region.words.assign(commands.begin(), commands.end());
    std::map<std::uint32_t, std::uint32_t> byKind;
    for (std::size_t cursor = 0; cursor < commands.size();) {
        const auto header = commands[cursor];
        const auto words = std::min<std::size_t>(Pm4::PacketWords(header), commands.size() - cursor);
        const auto kind = kindOf(header);
        ++report.packets;
        if ((kind >> 8u) == 3u) {
            const auto opcode = kind & 0xffu;
            if (Pm4::DrawOpcode(opcode)) ++report.draws;
            else if (opcode == 0x15u || opcode == 0x16u) ++report.dispatches;
        }
        if (structure && (cursor + words > before->words.size() || before->words[cursor] != header)) structure = false;
        if (structure) {
            ++report.prefixPackets;
            if ((kind >> 8u) == 3u && Pm4::DrawOpcode(kind & 0xffu)) ++report.prefixDraws;
            else if (kind == 0x315u || kind == 0x316u) ++report.prefixDispatches;
            for (std::size_t word = cursor + 1; word < cursor + words; ++word) {
                if (before->words[word] == commands[word]) continue;
                region.patchSites.push_back(static_cast<std::uint32_t>(word));
                ++byKind[kind];
                if (earlier != nullptr && word < earlier->words.size() && earlier->words[word] == commands[word]) ++report.alternatingWords;
            }
        }
        cursor += words;
    }
    if (report.prefixPackets != 0) {
        const bool whole = structure && sameLength;
        region.compared = true;
        report.match = !whole ? RegionMatch::Prefix : region.patchSites.empty() ? RegionMatch::Exact : RegionMatch::Structure;
        report.patchWords = static_cast<std::uint32_t>(region.patchSites.size());
        report.stablePatches = !region.patchSites.empty() && before->compared && before->patchSites == region.patchSites;
        report.patchWordsByKind.assign(byKind.begin(), byKind.end());
    } else {
        report.alternatingWords = 0;
    }
    regions.push_back(std::move(region));
    return report;
}

void RegionRecognizer::EndFrame() {
    older = std::move(previous);
    previous = std::move(current);
    current.clear();
}

}
