#include "prx/libSceAgcDriver/Graphics/include/StructCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace AgcDriver::Graphics::StructCensus {

std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    hash ^= hash >> 31u;
    hash *= 0xbf58476d1ce4e5b9ull;
    hash ^= hash >> 29u;
    return hash;
}

std::uint64_t HashBytes(std::uint64_t hash, std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    for (; offset + 8 <= bytes.size(); offset += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, bytes.data() + offset, 8);
        hash = Mix(hash, word);
    }
    std::uint64_t tail = 0;
    if (offset < bytes.size()) std::memcpy(&tail, bytes.data() + offset, bytes.size() - offset);
    return Mix(hash, tail ^ (static_cast<std::uint64_t>(bytes.size()) << 56u));
}

std::array<std::uint64_t, Levels> Tracker::LevelHashes(const Commit& commit) {
    std::array<std::uint64_t, Levels> levels{};
    auto hash = Mix(static_cast<std::uint64_t>(commit.kind) + 1, commit.shape);
    levels[LevelShape] = hash;
    for (const auto component : {ComponentDynamic, ComponentParams, ComponentPush, ComponentBindings}) hash = Mix(hash, commit.components[component]);
    levels[LevelStructure] = hash;
    hash = Mix(hash, commit.components[ComponentAddresses]);
    levels[LevelAddresses] = hash;
    levels[LevelFull] = Mix(hash, commit.components[ComponentData]);
    return levels;
}

void Tracker::Note(const Commit& commit) {
    const auto index = current.entries.size();
    current.entries.push_back({LevelHashes(commit), commit.components, commit.kind});
    if (commit.kind != Kind::Draw) {
        current.passOpen = false;
        return;
    }
    if (commit.passBegin || !current.passOpen || current.passes.empty()) {
        current.passes.emplace_back(index, index + 1);
        current.passOpen = true;
    } else {
        current.passes.back().second = index + 1;
    }
}

void Tracker::EndFrame() {
    if (current.entries.empty()) return;
    if (havePrevious) {
        const auto& now = current.entries;
        const auto& before = previous.entries;
        ++totals.frames;
        totals.commits += now.size();
        for (const auto& entry : now) (entry.kind == Kind::Draw ? totals.draws : totals.dispatches) += 1;
        std::unordered_map<std::uint64_t, std::uint32_t> counts;
        std::unordered_map<std::uint64_t, std::vector<std::size_t>> positions;
        for (std::size_t level = 0; level < Levels; ++level) {
            counts.clear();
            positions.clear();
            for (std::size_t j = 0; j < before.size(); ++j) {
                ++counts[before[j].levels[level]];
                positions[before[j].levels[level]].push_back(j);
            }
            for (const auto& entry : now) {
                auto found = counts.find(entry.levels[level]);
                if (found != counts.end() && found->second != 0) {
                    --found->second;
                    ++totals.repeated[level];
                }
            }
            // Runs matching the previous frame in order: extend while the next previous commit
            // matches, else restart at the first later (or earliest) previous commit of equal hash.
            std::size_t run = 0;
            std::size_t at = 0;
            const auto close = [&] {
                if (run >= LongRun) totals.inLongRuns[level] += run;
                run = 0;
            };
            for (std::size_t i = 0; i < now.size(); ++i) {
                const auto hash = now[i].levels[level];
                if (run != 0 && at + 1 < before.size() && before[at + 1].levels[level] == hash) {
                    ++at;
                    ++run;
                } else {
                    const bool had = run != 0;
                    close();
                    const auto list = positions.find(hash);
                    if (list == positions.end()) continue;
                    std::size_t pick = list->second.front();
                    if (had) {
                        for (const auto position : list->second) {
                            if (position > at) {
                                pick = position;
                                break;
                            }
                        }
                    }
                    at = pick;
                    run = 1;
                }
                if (level == LevelShape) {
                    ++totals.aligned;
                    for (std::size_t component = 0; component < Components; ++component) {
                        if (now[i].components[component] == before[at].components[component]) ++totals.alignedSame[component];
                    }
                }
            }
            close();
            // Whole passes: the sequence of level hashes of their draws.
            const auto passHash = [&](const std::vector<Entry>& entries, std::pair<std::size_t, std::size_t> pass) {
                auto hash = Mix(0x51ed270b27f1c5a7ull, pass.second - pass.first);
                for (auto index = pass.first; index < pass.second; ++index) hash = Mix(hash, entries[index].levels[level]);
                return hash;
            };
            counts.clear();
            for (const auto& pass : previous.passes) ++counts[passHash(before, pass)];
            for (const auto& pass : current.passes) {
                auto found = counts.find(passHash(now, pass));
                if (found != counts.end() && found->second != 0) {
                    --found->second;
                    ++totals.passesRepeated[level];
                    totals.passDrawsRepeated[level] += pass.second - pass.first;
                }
            }
        }
        totals.passes += current.passes.size();
        for (const auto& pass : current.passes) totals.passDraws += pass.second - pass.first;
    }
    std::swap(previous, current);
    current.entries.clear();
    current.passes.clear();
    current.passOpen = false;
    havePrevious = true;
}

bool Enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_ARCHB_STRUCT_REPEAT_CENSUS");
        return text != nullptr && text[0] != '\0' && text[0] != '0';
    }();
    return enabled;
}

namespace {
std::atomic<std::uint64_t> frameSerial{0};
std::mutex censusMutex;
Tracker tracker;
std::uint64_t trackedFrame = 0;
std::chrono::steady_clock::time_point reportedAt = std::chrono::steady_clock::now();
}

void NoteFrame() {
    frameSerial.fetch_add(1, std::memory_order_acq_rel);
}

void NoteCommit(const Commit& commit) {
    const auto frame = frameSerial.load(std::memory_order_acquire);
    std::lock_guard lock(censusMutex);
    if (frame != trackedFrame) {
        trackedFrame = frame;
        tracker.EndFrame();
        const auto now = std::chrono::steady_clock::now();
        if (now - reportedAt >= std::chrono::seconds(10) && tracker.Totals().frames != 0) {
            reportedAt = now;
            std::fprintf(stderr, "%s\n", Describe(tracker.Totals()).c_str());
            tracker.ResetTotals();
        }
    }
    tracker.Note(commit);
}

std::string Describe(const Stats& stats) {
    const auto frames = static_cast<double>(stats.frames == 0 ? 1 : stats.frames);
    const auto percent = [](std::uint64_t part, std::uint64_t whole) { return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole); };
    static constexpr const char* levelNames[Levels] = {"shape", "structure", "addresses", "full"};
    static constexpr const char* componentNames[Components] = {"dynamic", "params", "push", "bindings", "addresses", "data"};
    char line[512];
    std::snprintf(line, sizeof(line), "[struct-census] %llu frames vs their predecessor: %.1f commits/frame (%.1f draws, %.1f dispatches), %.1f passes/frame (%.1f draws in passes)", static_cast<unsigned long long>(stats.frames), static_cast<double>(stats.commits) / frames, static_cast<double>(stats.draws) / frames, static_cast<double>(stats.dispatches) / frames, static_cast<double>(stats.passes) / frames, static_cast<double>(stats.passDraws) / frames);
    std::string text = line;
    for (std::size_t level = 0; level < Levels; ++level) {
        std::snprintf(line, sizeof(line), "; %s: commits repeated %.1f%%, in runs >= %zu %.1f%%, passes repeated %.1f%% (draws in them %.1f%%)", levelNames[level], percent(stats.repeated[level], stats.commits), Tracker::LongRun, percent(stats.inLongRuns[level], stats.commits), percent(stats.passesRepeated[level], stats.passes), percent(stats.passDrawsRepeated[level], stats.passDraws));
        text += line;
    }
    std::snprintf(line, sizeof(line), "; shape-aligned %.1f%% of commits, equal there:", percent(stats.aligned, stats.commits));
    text += line;
    for (std::size_t component = 0; component < Components; ++component) {
        std::snprintf(line, sizeof(line), " %s %.1f%%", componentNames[component], percent(stats.alignedSame[component], stats.aligned));
        text += line;
    }
    return text;
}

bool ReuseTracker::Note(std::uint64_t structure) {
    const auto occurrence = current[structure]++;
    if (!havePrevious) return false;
    const auto found = previous.find(structure);
    return found != previous.end() && found->second > occurrence;
}

void ReuseTracker::EndFrame() {
    if (current.empty()) return;
    std::swap(previous, current);
    current.clear();
    havePrevious = true;
}

bool ReuseCensusEnabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_ARCHB_DRAW_REUSE_CENSUS");
        return text != nullptr && text[0] != '\0' && text[0] != '0';
    }();
    return enabled;
}

namespace {
struct ReuseCounters {
    std::atomic<std::uint64_t> draws{0};
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> recipeDraws{0};
    std::atomic<std::uint64_t> recipeHits{0};
    std::atomic<std::uint64_t> hitTotalNs{0};
    std::atomic<std::uint64_t> missTotalNs{0};
    std::array<std::atomic<std::uint64_t>, ReuseStages> hitNs{};
    std::array<std::atomic<std::uint64_t>, ReuseStages> missNs{};
};

ReuseCounters& reuseCounters() {
    static auto* counters = [] {
        auto* made = new ReuseCounters();
        using AgcDriver::ArchB::Recorder;
        Recorder::RegisterCounter("reuse_census_draws", made->draws);
        Recorder::RegisterCounter("reuse_census_hits", made->hits);
        Recorder::RegisterCounter("reuse_census_recipe_draws", made->recipeDraws);
        Recorder::RegisterCounter("reuse_census_recipe_hits", made->recipeHits);
        Recorder::RegisterCounter("reuse_census_hit_total_ns", made->hitTotalNs);
        Recorder::RegisterCounter("reuse_census_miss_total_ns", made->missTotalNs);
        static std::array<std::string, ReuseStages * 2> names;
        for (std::size_t stage = 0; stage < ReuseStages; ++stage) {
            names[stage * 2] = std::string("reuse_census_hit_") + ReuseStageNames[stage] + "_ns";
            names[stage * 2 + 1] = std::string("reuse_census_miss_") + ReuseStageNames[stage] + "_ns";
            Recorder::RegisterCounter(names[stage * 2].c_str(), made->hitNs[stage]);
            Recorder::RegisterCounter(names[stage * 2 + 1].c_str(), made->missNs[stage]);
        }
        return made;
    }();
    return *counters;
}

std::mutex reuseMutex;
ReuseTracker reuseTracker;
std::uint64_t reuseFrame = 0;
std::uint64_t reuseFrames = 0;
std::chrono::steady_clock::time_point reuseReportedAt = std::chrono::steady_clock::now();
}

void NoteReuseDraw(std::uint64_t structure, bool recipe, const std::array<std::uint64_t, ReuseStages>& ns, std::uint64_t totalNs, std::uint64_t frame) {
    auto& counters = reuseCounters();
    bool hit = false;
    {
        std::lock_guard lock(reuseMutex);
        if (frame != reuseFrame) {
            reuseFrame = frame;
            reuseTracker.EndFrame();
            ++reuseFrames;
        }
        hit = reuseTracker.Note(structure);
    }
    counters.draws.fetch_add(1, std::memory_order_relaxed);
    if (recipe) counters.recipeDraws.fetch_add(1, std::memory_order_relaxed);
    if (hit) {
        counters.hits.fetch_add(1, std::memory_order_relaxed);
        if (recipe) counters.recipeHits.fetch_add(1, std::memory_order_relaxed);
    }
    (hit ? counters.hitTotalNs : counters.missTotalNs).fetch_add(totalNs, std::memory_order_relaxed);
    auto& stages = hit ? counters.hitNs : counters.missNs;
    for (std::size_t stage = 0; stage < ReuseStages; ++stage) stages[stage].fetch_add(ns[stage], std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(reuseMutex);
    if (now - reuseReportedAt < std::chrono::seconds(10)) return;
    reuseReportedAt = now;
    std::string text = "[reuse-census] cumulative frames=" + std::to_string(reuseFrames) + " draws=" + std::to_string(counters.draws.load()) + " hits=" + std::to_string(counters.hits.load()) + " recipe_draws=" + std::to_string(counters.recipeDraws.load()) + " recipe_hits=" + std::to_string(counters.recipeHits.load()) + " hit_total_ns=" + std::to_string(counters.hitTotalNs.load()) + " miss_total_ns=" + std::to_string(counters.missTotalNs.load());
    for (std::size_t stage = 0; stage < ReuseStages; ++stage) text += std::string(" hit_") + ReuseStageNames[stage] + "=" + std::to_string(counters.hitNs[stage].load()) + " miss_" + ReuseStageNames[stage] + "=" + std::to_string(counters.missNs[stage].load());
    std::fprintf(stderr, "%s\n", text.c_str());
}

}
