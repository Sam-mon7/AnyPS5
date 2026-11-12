#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/RepeatCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace AgcDriver::DriverDetail::RepeatCensus {

namespace {

struct Region {
    std::size_t first = 0;
    std::size_t count = 0;
    std::uint64_t sequence = 0;
    std::uint64_t target = 0;
    std::uint32_t endOpcode = 0;
    bool endTarget = false;
};

std::vector<Region> RegionsOf(const std::vector<Item>& items, Summary* summary) {
    std::vector<Region> regions;
    Region open;
    bool active = false;
    bool targetKnown = false;
    const auto close = [&](std::uint32_t opcode, bool target) {
        if (!active) return;
        open.endOpcode = opcode;
        open.endTarget = target;
        regions.push_back(open);
        active = false;
    };
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        if (!item.isDraw) {
            if (summary != nullptr) ++summary->packetBreaks[item.opcode];
            close(item.opcode, false);
            continue;
        }
        if (active && item.draw.live && targetKnown && item.draw.target != open.target) {
            if (summary != nullptr) ++summary->targetBreaks;
            close(0, true);
        }
        if (!active) {
            open = {};
            open.first = index;
            open.sequence = 0x9e3779b97f4a7c15ull;
            active = true;
            targetKnown = false;
        }
        if (item.draw.live && !targetKnown) {
            open.target = item.draw.target;
            targetKnown = true;
        }
        ++open.count;
        open.sequence = Mix(open.sequence, item.draw.site);
    }
    close(0, false);
    return regions;
}

std::size_t SizeClass(std::size_t draws) {
    return draws < 2 ? 0 : draws < 8 ? 1 : draws < 64 ? 2 : 3;
}

struct State {
    std::mutex mutex;
    std::deque<std::vector<Item>> history;
    std::vector<Item> coarsePrevious;
    std::vector<Item> current;
    bool tentative = false;
    std::uint64_t frames = 0;
};

State& Shared() {
    static State state;
    return state;
}

std::uint64_t TracePeriod() {
    static const std::uint64_t period = [] {
        const char* text = std::getenv("APS5_DRAW_REPEAT_CENSUS_TRACE");
        return text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
    }();
    return period;
}

void Emit(const Summary& summary) {
    using ArchB::Recorder;
    Recorder::Count("repeat_census_frames");
    Recorder::Count("repeat_census_regions", static_cast<double>(summary.regions));
    Recorder::Count("repeat_census_regions_same_sequence", static_cast<double>(summary.regionsSameSequence));
    Recorder::Count("repeat_census_regions_identical", static_cast<double>(summary.regionsIdentical));
    Recorder::Count("repeat_census_regions_values_only", static_cast<double>(summary.regionsValuesOnly));
    Recorder::Count("repeat_census_draws", static_cast<double>(summary.draws));
    Recorder::Count("repeat_census_draws_same_sequence", static_cast<double>(summary.drawsSameSequence));
    Recorder::Count("repeat_census_draws_identical", static_cast<double>(summary.drawsIdentical));
    Recorder::Count("repeat_census_draws_values_only", static_cast<double>(summary.drawsValuesOnly));
    Recorder::Count("repeat_census_draws_structure", static_cast<double>(summary.drawsStructure));
    Recorder::Count("repeat_census_draws_addresses", static_cast<double>(summary.drawsAddresses));
    Recorder::Count("repeat_census_draws_constants", static_cast<double>(summary.drawsConstants));
    Recorder::Count("repeat_census_draws_arguments", static_cast<double>(summary.drawsArguments));
    Recorder::Count("repeat_census_draws_not_live", static_cast<double>(summary.drawsNotLive));
    Recorder::Count("repeat_census_draws_unmatched", static_cast<double>(summary.drawsUnmatched));
    Recorder::Count("repeat_census_draws_unmatched_prefix", static_cast<double>(summary.drawsUnmatchedPrefix));
    Recorder::Count("repeat_census_draws_in_identical_regions", static_cast<double>(summary.drawsInIdenticalRegions));
    Recorder::Count("repeat_census_draws_in_values_only_regions", static_cast<double>(summary.drawsInValuesOnlyRegions));
    Recorder::Count("repeat_census_target_breaks", static_cast<double>(summary.targetBreaks));
    static const std::array<const char*, SizeClasses> regionNames{"repeat_census_regions_of_1", "repeat_census_regions_of_2_7", "repeat_census_regions_of_8_63", "repeat_census_regions_of_64_up"};
    static const std::array<const char*, SizeClasses> drawNames{"repeat_census_draws_in_1", "repeat_census_draws_in_2_7", "repeat_census_draws_in_8_63", "repeat_census_draws_in_64_up"};
    static const std::array<const char*, SizeClasses> sameNames{"repeat_census_same_sequence_draws_in_1", "repeat_census_same_sequence_draws_in_2_7", "repeat_census_same_sequence_draws_in_8_63", "repeat_census_same_sequence_draws_in_64_up"};
    for (std::size_t size = 0; size < SizeClasses; ++size) {
        Recorder::Count(regionNames[size], static_cast<double>(summary.regionsBySize[size]));
        Recorder::Count(drawNames[size], static_cast<double>(summary.drawsBySize[size]));
        Recorder::Count(sameNames[size], static_cast<double>(summary.sameSequenceDrawsBySize[size]));
    }
    static std::unordered_map<std::uint32_t, std::string> breakNames;
    for (const auto& [opcode, count] : summary.packetBreaks) {
        auto found = breakNames.find(opcode);
        if (found == breakNames.end()) {
            char name[48];
            std::snprintf(name, sizeof(name), "repeat_census_packets_%02x", opcode);
            found = breakNames.emplace(opcode, name).first;
        }
        Recorder::Count(found->second.c_str(), static_cast<double>(count));
    }
}

void EmitLag(std::size_t lag, const Summary& summary) {
    static const auto names = [] {
        std::array<std::array<std::string, 4>, MaxLag + 1> values;
        for (std::size_t index = 0; index < values.size(); ++index) {
            const auto prefix = "repeat_census_lag" + std::to_string(index);
            values[index] = {prefix + "_draws_same_sequence", prefix + "_draws_identical", prefix + "_draws_values_only", prefix + "_draws_structure"};
        }
        return values;
    }();
    using ArchB::Recorder;
    Recorder::Count(names[lag][0].c_str(), static_cast<double>(summary.drawsSameSequence));
    Recorder::Count(names[lag][1].c_str(), static_cast<double>(summary.drawsIdentical));
    Recorder::Count(names[lag][2].c_str(), static_cast<double>(summary.drawsValuesOnly));
    Recorder::Count(names[lag][3].c_str(), static_cast<double>(summary.drawsStructure));
}

void EmitBest(std::size_t lag, const Summary& summary) {
    static const auto names = [] {
        std::array<std::string, MaxLag + 1> values;
        for (std::size_t index = 0; index < values.size(); ++index) values[index] = "repeat_census_best_lag_is_" + std::to_string(index);
        return values;
    }();
    using ArchB::Recorder;
    Recorder::Count(names[lag].c_str());
    Recorder::Count("repeat_census_best_draws_same_sequence", static_cast<double>(summary.drawsSameSequence));
    Recorder::Count("repeat_census_best_draws_identical", static_cast<double>(summary.drawsIdentical));
    Recorder::Count("repeat_census_best_draws_values_only", static_cast<double>(summary.drawsValuesOnly));
    Recorder::Count("repeat_census_best_draws_structure", static_cast<double>(summary.drawsStructure));
    Recorder::Count("repeat_census_best_draws_not_live", static_cast<double>(summary.drawsNotLive));
    Recorder::Count("repeat_census_best_draws_in_replayable_regions", static_cast<double>(summary.drawsInIdenticalRegions + summary.drawsInValuesOnlyRegions));
}

void Trace(std::uint64_t frame, const Summary& summary, const std::vector<RegionRow>& rows) {
    std::fprintf(stderr, "[repeat-census] frame %llu: %llu regions (%llu same sequence, %llu identical, %llu values only), %llu draws (%llu same sequence, %llu identical, %llu values only, %llu structure, %llu not live, %llu unmatched of which %llu in a common prefix), %llu target breaks\n",
        static_cast<unsigned long long>(frame), static_cast<unsigned long long>(summary.regions), static_cast<unsigned long long>(summary.regionsSameSequence), static_cast<unsigned long long>(summary.regionsIdentical), static_cast<unsigned long long>(summary.regionsValuesOnly),
        static_cast<unsigned long long>(summary.draws), static_cast<unsigned long long>(summary.drawsSameSequence), static_cast<unsigned long long>(summary.drawsIdentical), static_cast<unsigned long long>(summary.drawsValuesOnly), static_cast<unsigned long long>(summary.drawsStructure),
        static_cast<unsigned long long>(summary.drawsNotLive), static_cast<unsigned long long>(summary.drawsUnmatched), static_cast<unsigned long long>(summary.drawsUnmatchedPrefix), static_cast<unsigned long long>(summary.targetBreaks));
    std::size_t printed = 0;
    for (std::size_t index = 0; index < rows.size() && printed < 600; ++index) {
        const auto& row = rows[index];
        if (row.draws < 2 && row.sameSequence) continue;
        ++printed;
        std::fprintf(stderr, "[repeat-census-region] frame %llu region %zu draws %zu target %llx end %s%02x %s identical %llu values %llu structure %llu not_live %llu prefix %llu\n",
            static_cast<unsigned long long>(frame), index, row.draws, static_cast<unsigned long long>(row.target), row.endTarget ? "target " : "packet ", row.endOpcode, row.sameSequence ? "same" : "new",
            static_cast<unsigned long long>(row.identical), static_cast<unsigned long long>(row.valuesOnly), static_cast<unsigned long long>(row.structure), static_cast<unsigned long long>(row.notLive), static_cast<unsigned long long>(row.prefix));
    }
}

}

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_DRAW_REPEAT_CENSUS");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

std::uint64_t Mix(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
    seed *= 0xff51afd7ed558ccdull;
    return seed ^ (seed >> 33u);
}

Summary Compare(const std::vector<Item>& previous, const std::vector<Item>& current, std::vector<RegionRow>* rows) {
    Summary summary;
    const auto before = RegionsOf(previous, nullptr);
    const auto now = RegionsOf(current, &summary);
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> known;
    for (std::size_t index = before.size(); index-- > 0;) known[Mix(before[index].sequence, before[index].count)].push_back(index);
    if (rows != nullptr) rows->clear();
    for (std::size_t index = 0; index < now.size(); ++index) {
        const auto& region = now[index];
        const auto size = SizeClass(region.count);
        ++summary.regions;
        summary.draws += region.count;
        ++summary.regionsBySize[size];
        summary.drawsBySize[size] += region.count;
        RegionRow row;
        row.first = region.first;
        row.draws = region.count;
        row.target = region.target;
        row.endOpcode = region.endOpcode;
        row.endTarget = region.endTarget;
        const Region* match = nullptr;
        if (const auto found = known.find(Mix(region.sequence, region.count)); found != known.end() && !found->second.empty()) {
            const auto candidate = found->second.back();
            bool same = before[candidate].count == region.count;
            for (std::size_t draw = 0; same && draw < region.count; ++draw) same = previous[before[candidate].first + draw].draw.site == current[region.first + draw].draw.site;
            if (same) {
                match = &before[candidate];
                found->second.pop_back();
            }
        }
        if (match == nullptr) {
            summary.drawsUnmatched += region.count;
            if (index < before.size()) {
                const auto& other = before[index];
                std::size_t prefix = 0;
                while (prefix < region.count && prefix < other.count && previous[other.first + prefix].draw.site == current[region.first + prefix].draw.site) ++prefix;
                summary.drawsUnmatchedPrefix += prefix;
                row.prefix = prefix;
            }
            for (std::size_t draw = 0; draw < region.count; ++draw) {
                if (!current[region.first + draw].draw.live) {
                    ++summary.drawsNotLive;
                    ++row.notLive;
                }
            }
            if (rows != nullptr) rows->push_back(row);
            continue;
        }
        row.sameSequence = true;
        ++summary.regionsSameSequence;
        summary.drawsSameSequence += region.count;
        summary.sameSequenceDrawsBySize[size] += region.count;
        for (std::size_t draw = 0; draw < region.count; ++draw) {
            const auto& old = previous[match->first + draw].draw;
            const auto& fresh = current[region.first + draw].draw;
            if (!fresh.live) {
                ++summary.drawsNotLive;
                ++row.notLive;
            } else if (!old.live || old.structure != fresh.structure) {
                ++summary.drawsStructure;
                ++row.structure;
            } else if (old.addresses == fresh.addresses && old.constants == fresh.constants && old.arguments == fresh.arguments) {
                ++summary.drawsIdentical;
                ++row.identical;
            } else {
                ++summary.drawsValuesOnly;
                ++row.valuesOnly;
                if (old.addresses != fresh.addresses) ++summary.drawsAddresses;
                if (old.constants != fresh.constants) ++summary.drawsConstants;
                if (old.arguments != fresh.arguments) ++summary.drawsArguments;
            }
        }
        if (row.identical == region.count) {
            ++summary.regionsIdentical;
            summary.drawsInIdenticalRegions += region.count;
        } else if (row.identical + row.valuesOnly == region.count) {
            ++summary.regionsValuesOnly;
            summary.drawsInValuesOnlyRegions += region.count;
        }
        if (rows != nullptr) rows->push_back(row);
    }
    return summary;
}

void NotePacket(std::uint32_t opcode) {
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    Item item;
    item.opcode = opcode;
    state.current.push_back(item);
    state.tentative = true;
}

void NoteDraw(const Draw& draw) {
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    Item item;
    item.draw = draw;
    item.isDraw = true;
    if (state.tentative && !state.current.empty()) state.current.back() = item;
    else state.current.push_back(item);
    state.tentative = false;
}

void EndFrame() {
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    state.tentative = false;
    ++state.frames;
    const auto period = TracePeriod();
    const bool trace = period != 0 && state.frames % period == 0;
    std::vector<RegionRow> rows;
    static const std::vector<Item> none;
    const auto summary = Compare(state.history.empty() ? none : state.history.front(), state.current, trace ? &rows : nullptr);
    Emit(summary);
    if (trace) Trace(state.frames, summary, rows);
    std::size_t best = 1;
    auto bestSummary = summary;
    for (std::size_t lag = 2; lag <= state.history.size(); ++lag) {
        const auto older = Compare(state.history[lag - 1], state.current, nullptr);
        EmitLag(lag, older);
        if (trace) std::fprintf(stderr, "[repeat-census-lag] frame %llu lag %zu: %llu draws same sequence, %llu identical, %llu values only, %llu structure\n", static_cast<unsigned long long>(state.frames), lag, static_cast<unsigned long long>(older.drawsSameSequence), static_cast<unsigned long long>(older.drawsIdentical), static_cast<unsigned long long>(older.drawsValuesOnly), static_cast<unsigned long long>(older.drawsStructure));
        if (older.drawsIdentical + older.drawsValuesOnly > bestSummary.drawsIdentical + bestSummary.drawsValuesOnly) {
            best = lag;
            bestSummary = older;
        }
    }
    EmitBest(best, bestSummary);
    auto coarse = state.current;
    for (auto& item : coarse) item.draw.site = item.draw.coarse;
    const auto coarseSummary = Compare(state.coarsePrevious, coarse, nullptr);
    ArchB::Recorder::Count("repeat_census_coarse_draws_same_sequence", static_cast<double>(coarseSummary.drawsSameSequence));
    ArchB::Recorder::Count("repeat_census_coarse_draws_unmatched_prefix", static_cast<double>(coarseSummary.drawsUnmatchedPrefix));
    state.coarsePrevious = std::move(coarse);
    state.history.push_front(std::move(state.current));
    if (state.history.size() > MaxLag) state.history.pop_back();
    state.current.clear();
}

}
