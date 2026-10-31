#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace AgcDriver::ArchB {

namespace {

constexpr const char* bucketNames[] = {"lookup_proof", "mutex_wait", "targets_pass", "descriptors", "pipeline", "dynamic_state", "push_constants", "draw_commands", "barriers", "data_refresh", "bookkeeping"};
constexpr const char* metricNames[] = {"calls", "continuations", "vulkan_calls", "vkcmd_calls", "descriptor_writes", "descriptor_elements", "image_elements", "buffer_elements", "descriptor_sets", "descriptor_binds", "pipeline_binds", "distinct_pipelines", "dynamic_calls", "push_calls", "draw_calls", "pass_begins", "pass_ends", "pass_continued", "barriers", "image_barriers", "layout_transitions", "snapshot_bytes", "binding_preparations", "binding_rewrites", "plain_stages", "ordered_access", "descriptor_set_requests", "snapshot_copied_bytes", "redundant_pipeline_binds", "redundant_dynamic_calls", "redundant_push_calls", "h3_prelock_targets_ns", "h3_prelock_lookup_ns", "h3_prelock_surfaces_ns", "h3_prelock_proof_ns", "h3_binding_setup_ns", "h3_binding_selection_ns", "h3_snapshot_words_ns", "h3_snapshot_eligibility_ns", "h3_snapshot_unwritten_ns", "h3_snapshot_collect_ns", "h3_snapshot_lookup_ns", "h3_snapshot_copy_ns", "h3_snapshot_keep_ns", "h3_complete_images_ns", "h3_complete_upload_ns", "h3_complete_bda_ns", "h3_complete_descriptors_ns", "h3_complete_proof_ns", "h3_prelock_target_ranges", "h3_prelock_target_bytes", "h3_prelock_surface_ranges", "h3_prelock_surface_bytes", "h3_prelock_walked_bytes", "h3_binding_allocations", "h3_binding_read_ranges", "h3_snapshot_guest_ranges", "h3_snapshot_moved_ranges", "h3_snapshot_word_ranges", "h3_snapshot_word_bytes", "h3_snapshot_word_reused", "h3_snapshot_unwritten_hits", "h3_snapshot_collects", "h3_snapshot_reusable_hits", "h3_snapshot_uploads", "h3_snapshot_allocations", "h3_snapshot_pending_queries", "h3_bindings_previous", "h3_bindings_same", "h3_bindings_changed", "h3_bindings_expired", "h3_collect_requests", "h3_collect_memo_hits", "h3_collect_failed_memo_hits", "h3_collect_walk_bytes", "h3_collect_refused", "h3_complete_builds"};
constexpr const char* stateNames[] = {"pipeline", "descriptors", "targets", "dynamic", "push"};
constexpr const char* outcomeNames[] = {"Unclassified", "Hit", "Miss"};

Recorder::Bucket bucketOf(std::string_view name, std::string_view stage, Recorder::Bucket fallback) {
    using B = Recorder::Bucket;
    if (name == "GpuMutex.wait") return B::Wait;
    if (name == "Driver.DrawLookup" || name == "Draw.Key" || name == "Draw.Lookup" || name == "Resources.Proof" || name == "Resources.FullWalk") return B::Lookup;
    if (name == "Draw.Targets" || name == "Vulkan.Pass") return B::Targets;
    if (name == "Draw.Acquire") return stage == "pipeline" ? B::Pipeline : B::Targets;
    if (name == "Resources.Bind" || name == "Draw.Descriptors" || name == "Vulkan.Descriptors") return B::Descriptors;
    if (name == "Vulkan.Pipeline") return B::Pipeline;
    if (name == "Draw.PushConstants" || name == "Vulkan.Push") return B::Push;
    if (name == "Vulkan.Dynamic") return B::Dynamic;
    if (name == "Vulkan.Draw") return B::Draw;
    if (name == "Vulkan.Barriers") return B::Barriers;
    if (name == "Draw.Inputs" || name == "Draw.Prelock" || name == "Draw.ResolveResources" || name == "Resources.Prepare" || name == "Resources.Complete" || name == "Draw.BindingRefresh" || name == "Draw.UserPatch") return B::Refresh;
    if (name == "Draw.Record") {
        if (stage == "bindings") return B::Refresh;
        if (stage == "pass" || stage == "begin") return B::Targets;
        if (stage == "commands") return B::Draw;
        return B::Bookkeeping;
    }
    if (name == "Draw.KeepResources" || name == "Draw.ResourceRelease" || name.starts_with("Recorder.") || name.starts_with("Label")) return B::Bookkeeping;
    return fallback;
}

Recorder::Metric refreshMetric(std::string_view name, std::string_view stage) {
    using M = Recorder::Metric;
    if (name == "Draw.Prelock") {
        if (stage == "targets") return M::H3PrelockTargetsNs;
        if (stage == "lookup") return M::H3PrelockLookupNs;
        if (stage == "surfaces") return M::H3PrelockSurfacesNs;
        if (stage == "proof") return M::H3PrelockProofNs;
    }
    if (name == "Draw.BindingRefresh") {
        if (stage == "setup") return M::H3BindingSetupNs;
        if (stage == "selection") return M::H3BindingSelectionNs;
    }
    if (name == "Draw.Snapshot") {
        if (stage == "words") return M::H3SnapshotWordsNs;
        if (stage == "eligibility") return M::H3SnapshotEligibilityNs;
        if (stage == "unwritten") return M::H3SnapshotUnwrittenNs;
        if (stage == "collect") return M::H3SnapshotCollectNs;
        if (stage == "lookup") return M::H3SnapshotLookupNs;
        if (stage == "copy") return M::H3SnapshotCopyNs;
        if (stage == "keep") return M::H3SnapshotKeepNs;
    }
    if (name == "Resources.Complete") {
        if (stage == "images") return M::H3CompleteImagesNs;
        if (stage == "upload") return M::H3CompleteUploadNs;
        if (stage == "bda") return M::H3CompleteBdaNs;
        if (stage == "descriptors") return M::H3CompleteDescriptorsNs;
        if (stage == "proof") return M::H3CompleteProofNs;
    }
    return M::Count;
}

struct Value {
    double local = 0;
    std::atomic<double> published{0};
    double previous = 0;
    void Add(double value) { local += value; published.store(local, std::memory_order_relaxed); }
    double Take() {
        const auto now = published.load(std::memory_order_relaxed);
        const auto delta = now - previous;
        previous = now;
        return delta;
    }
};

struct State {
    std::mutex mutex;
    std::vector<std::shared_ptr<void>> threads;
    struct Counter {
        std::string name;
        const std::atomic<std::uint64_t>* value;
        std::uint64_t previous;
    };
    std::vector<Counter> counters;
    bool started = false;
    bool on = false;
    bool toggled = false;
    std::uint64_t sequence = 0;
    std::uint64_t frames = 0;
    Recorder::Clock::time_point begin{};
};

State& state() {
    static auto* value = new State;
    return *value;
}

double milliseconds(Recorder::Clock::duration value) {
    return std::chrono::duration<double, std::milli>(value).count();
}

void add(Recorder::Shader& to, const Recorder::Shader& from) {
    to.calls += from.calls;
    for (std::size_t i = 0; i < to.ms.size(); ++i) to.ms[i] += from.ms[i];
}

double cost(const Recorder::Shader& row) {
    return std::accumulate(row.ms.begin(), row.ms.end(), 0.0);
}

void merge(std::vector<Recorder::Shader>& rows, const Recorder::Shader& value) {
    const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.key == value.key; });
    if (found == rows.end()) rows.push_back(value);
    else add(*found, value);
}

std::string quoted(std::string_view text) {
    std::string result;
    TraceFormat::AppendString(result, text);
    return result;
}

std::string shaderJson(const Recorder::Shader& row) {
    char code[17];
    std::snprintf(code, sizeof code, "%016llx", static_cast<unsigned long long>(row.key.code));
    std::ostringstream out;
    out << "{\"kind\":\"" << (row.key.kind == Recorder::Kind::Draw ? "draw" : "dispatch")
        << "\",\"code\":\"" << code << "\",\"calls\":" << row.calls;
    constexpr const char* names[] = {"prepare_ms", "record_ms", "proof_ms", "bind_ms"};
    for (std::size_t i = 0; i < row.ms.size(); ++i) out << ',' << quoted(names[i]) << ':' << row.ms[i];
    return out.str() + '}';
}

}

struct Recorder::Thread {
    explicit Thread(std::string name) : name(std::move(name)) {}
    struct ShaderEntry {
        std::atomic<bool> occupied{false};
        Key key;
        Value calls;
        std::array<Value, 4> ms;
    };
    struct PhaseEntry {
        std::atomic<const char*> name{nullptr};
        const char* stage = nullptr;
        Value ms;
    };
    struct CounterEntry {
        std::atomic<const char*> name{nullptr};
        Value value;
    };
    const std::string name;
    std::array<ShaderEntry, 1024> shaders;
    ShaderEntry other;
    std::array<PhaseEntry, 1024> phases;
    std::array<CounterEntry, 512> counters;
    Value phaseOverflow;
    Value counterOverflow;
    struct Census {
        std::array<std::array<Value, static_cast<std::size_t>(Bucket::Count)>, 2> elapsed;
        std::array<Value, static_cast<std::size_t>(Metric::Count)> counters;
        std::array<Value, static_cast<std::size_t>(State::Count)> same;
        std::array<Value, static_cast<std::size_t>(State::Count)> sameCommands;
        std::array<Value, static_cast<std::size_t>(State::Count)> unknown;
        std::array<Value, static_cast<std::size_t>(State::Count)> sameMs;
        std::array<Value, static_cast<std::size_t>(State::Count)> sameEmissionMs;
        std::array<Value, static_cast<std::size_t>(Bucket::Count)> emission;
        std::array<Value, static_cast<std::size_t>(Bucket::Count)> redundantEmission;
        Value pairs;
    };
    std::array<Census, 3> census;
    std::array<Sequence, static_cast<std::size_t>(State::Count)> previousStates;
    std::uint64_t previousGeneration = 0;
    std::uint64_t previousCommands = 0;
    std::uint64_t previousBatch = 0;
    bool previousHit = false;
    Value censusPublishMs;
};

Recorder::Thread& Recorder::thread() {
    if (local == nullptr) BindThread("unbound");
    return *local;
}

void Recorder::BindThread(const char* name) {
    if (local != nullptr && local->name == name) return;
    auto bound = std::make_shared<Thread>(name);
    local = bound.get();
    std::lock_guard lock(state().mutex);
    state().threads.push_back(std::move(bound));
}

void Recorder::Boundary(bool on, std::uint64_t sequence, Clock::time_point now) {
    auto& s = state();
    if (!s.started) {
        s.started = true;
        s.begin = now;
    } else {
        ++s.frames;
        s.toggled |= on != s.on || sequence != s.sequence;
    }
    if (on != s.on || sequence != s.sequence) generation.fetch_add(1, std::memory_order_relaxed);
    s.on = on;
    s.sequence = sequence;
    enabled.store(on, std::memory_order_relaxed);
}

void Recorder::Scope::Begin(const char* scope, Clock::time_point now, Phase phase) {
    if (!Enabled()) return;
    active = true;
    name = scope;
    previous = now;
    draw = currentDraw;
    parent = currentScope;
    if (draw != nullptr) {
        heldBefore = heldTime(now);
        bucket = bucketOf(scope, {}, parent != nullptr && parent->draw == draw ? parent->bucket : Bucket::Bookkeeping);
    }
    work = currentWork;
    category = phase != Phase::Inherit ? phase : parent != nullptr && parent->work == work ? parent->category : work != nullptr ? work->phase : Phase::Prepare;
    generation = Recorder::generation.load(std::memory_order_relaxed);
    currentScope = this;
}

void Recorder::Scope::Mark(const char* stage, Clock::time_point now) {
    if (!active) return;
    const auto inclusive = now - previous;
    elapsed += inclusive;
    const auto exclusive = inclusive - children;
    const auto held = draw != nullptr ? heldTime(now) : Clock::duration{};
    const auto inclusiveHeld = held - heldBefore;
    const auto exclusiveHeld = std::clamp(inclusiveHeld - heldChildren, Clock::duration{}, std::max(Clock::duration{}, exclusive));
    if (generation == Recorder::generation.load(std::memory_order_relaxed)) {
        Recorder::phase(name, stage, exclusive);
        if (draw != nullptr && draw == currentDraw && draw->generation == generation) {
            const auto index = static_cast<std::size_t>(bucketOf(name, stage != nullptr ? stage : "", bucket));
            const auto refresh = static_cast<Bucket>(index) == Bucket::Refresh ? refreshMetric(name, stage != nullptr ? stage : "") : Metric::Count;
            if (refresh != Metric::Count) draw->Count(refresh, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(exclusive).count()));
            draw->elapsed[0][index] += exclusive - exclusiveHeld;
            draw->elapsed[1][index] += exclusiveHeld;
            if (std::string_view(name).starts_with("Vulkan.")) draw->emission[index] += exclusive;
            if (repeated) draw->redundantEmission[index] += exclusive;
        }
        if (work != nullptr && work->generation == generation) work->elapsed[static_cast<std::size_t>(category)] += exclusive;
    }
    if (parent != nullptr) {
        parent->children += inclusive;
        parent->heldChildren += inclusiveHeld;
    }
    previous = now;
    children = {};
    heldBefore = held;
    heldChildren = {};
}

void Recorder::Scope::End(Clock::time_point now) {
    if (!active) return;
    Mark(nullptr, now);
    if (counter != nullptr && generation == Recorder::generation.load(std::memory_order_relaxed)) Count(counter, milliseconds(elapsed));
    currentScope = parent;
    active = false;
}

Recorder::Work::Work(Key key, Phase phase, bool call)
    : previous(currentWork), key(key), phase(phase), call(call), generation(Recorder::generation.load(std::memory_order_relaxed)) {
    currentWork = this;
    if (key.kind == Kind::Dispatch && local != nullptr) local->previousCommands = 0;
}

Recorder::Work::~Work() {
    if (Enabled() && generation == Recorder::generation.load(std::memory_order_relaxed)) AddShader(key, call ? 1 : 0, elapsed);
    currentWork = previous;
}

Recorder::Draw::Draw(DrawOutcome outcome, bool call) : outcome(outcome) {
    static const bool requested = [] { const auto* value = std::getenv("APS5_ARCHB_HIT_CENSUS"); return value != nullptr && std::string_view(value) == "1"; }();
    if (!requested || !Enabled()) return;
    active = true;
    previous = currentDraw;
    currentDraw = this;
    generation = Generation();
    Count(call ? Metric::Calls : Metric::Continuations);
    auto& t = thread();
    if (t.previousHit && generation == t.previousGeneration) {
        priorStates = &t.previousStates;
        priorCommands = t.previousCommands;
        priorBatch = t.previousBatch;
    }
}

Recorder::Draw::~Draw() {
    if (!active) return;
    currentDraw = previous;
    if (!Enabled() || generation != Generation()) return;
    const auto start = Clock::now();
    auto& t = thread();
    auto& row = t.census[static_cast<std::size_t>(outcome)];
    const auto& pipelines = states[static_cast<std::size_t>(State::Pipeline)];
    for (std::size_t i = 0; i < pipelines.size; ++i) {
        if (std::find(pipelines.words.begin(), pipelines.words.begin() + i, pipelines.words[i]) == pipelines.words.begin() + i) Count(Metric::DistinctPipelines);
    }
    for (std::size_t lock = 0; lock < elapsed.size(); ++lock) {
        for (std::size_t i = 0; i < elapsed[lock].size(); ++i) row.elapsed[lock][i].Add(milliseconds(elapsed[lock][i]));
    }
    for (std::size_t i = 0; i < counters.size(); ++i) row.counters[i].Add(static_cast<double>(counters[i]));
    for (std::size_t i = 0; i < emission.size(); ++i) {
        row.emission[i].Add(milliseconds(emission[i]));
        row.redundantEmission[i].Add(milliseconds(redundantEmission[i]));
    }
    const bool pair = outcome == DrawOutcome::Hit && t.previousHit && generation == t.previousGeneration;
    if (pair) row.pairs.Add(1);
    constexpr Bucket stateBuckets[] = {Bucket::Pipeline, Bucket::Descriptors, Bucket::Targets, Bucket::Dynamic, Bucket::Push};
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (states[i].overflow) row.unknown[i].Add(1);
        if (pair && states[i].Same(t.previousStates[i])) {
            row.same[i].Add(1);
            const auto b = static_cast<std::size_t>(stateBuckets[i]);
            row.sameMs[i].Add(milliseconds(elapsed[0][b] + elapsed[1][b]));
            row.sameEmissionMs[i].Add(milliseconds(emission[b]));
            if (!multipleCommands && commands != 0 && commands == t.previousCommands && batch == t.previousBatch) row.sameCommands[i].Add(1);
        }
        t.previousStates[i].size = states[i].size;
        t.previousStates[i].overflow = states[i].overflow;
        std::copy_n(states[i].words.begin(), states[i].size, t.previousStates[i].words.begin());
    }
    t.previousHit = outcome == DrawOutcome::Hit;
    t.previousGeneration = generation;
    t.previousCommands = multipleCommands ? 0 : commands;
    t.previousBatch = batch;
    t.censusPublishMs.Add(milliseconds(Clock::now() - start));
}

Recorder::Clock::duration Recorder::heldTime(Clock::time_point now) {
    return gpuElapsed + (gpuHeld ? now - gpuSince : Clock::duration{});
}

void Recorder::GpuLock(bool held, Clock::time_point now) {
    gpuElapsed = heldTime(now);
    gpuSince = now;
    gpuHeld = held;
}

void Recorder::phase(const char* name, const char* stage, Clock::duration elapsed) {
    auto& t = thread();
    const auto hash = (reinterpret_cast<std::uintptr_t>(name) ^ (reinterpret_cast<std::uintptr_t>(stage) >> 3)) * 0x9e3779b97f4a7c15ull;
    for (std::size_t probe = 0; probe < t.phases.size(); ++probe) {
        auto& row = t.phases[(hash + probe) % t.phases.size()];
        const auto* key = row.name.load(std::memory_order_relaxed);
        if (key == nullptr) {
            row.stage = stage;
            row.name.store(name, std::memory_order_release);
        } else if (key != name || row.stage != stage) continue;
        row.ms.Add(milliseconds(elapsed));
        return;
    }
    t.phaseOverflow.Add(milliseconds(elapsed));
}

void Recorder::AddShader(Key key, std::uint64_t calls, const std::array<Clock::duration, 4>& elapsed) {
    if (!Enabled()) return;
    auto& t = thread();
    auto* selected = &t.other;
    if (key.code != 0) {
        const auto hash = (key.code ^ static_cast<std::uint64_t>(key.kind)) * 0x9e3779b97f4a7c15ull;
        for (std::size_t probe = 0; probe < t.shaders.size(); ++probe) {
            auto& row = t.shaders[(hash + probe) % t.shaders.size()];
            if (!row.occupied.load(std::memory_order_relaxed)) {
                row.key = key;
                row.occupied.store(true, std::memory_order_release);
            } else if (row.key != key) continue;
            selected = &row;
            break;
        }
    }
    selected->calls.Add(static_cast<double>(calls));
    for (std::size_t i = 0; i < elapsed.size(); ++i) selected->ms[i].Add(milliseconds(elapsed[i]));
}

void Recorder::Count(const char* name, double value) {
    if (!Enabled()) return;
    auto& t = thread();
    const auto hash = reinterpret_cast<std::uintptr_t>(name) * 0x9e3779b97f4a7c15ull;
    for (std::size_t probe = 0; probe < t.counters.size(); ++probe) {
        auto& row = t.counters[(hash + probe) % t.counters.size()];
        const auto* key = row.name.load(std::memory_order_relaxed);
        if (key == nullptr) row.name.store(name, std::memory_order_release);
        else if (key != name) continue;
        row.value.Add(value);
        return;
    }
    t.counterOverflow.Add(value);
}

void Recorder::RegisterCounter(const char* name, const std::atomic<std::uint64_t>& counter) {
    std::lock_guard lock(state().mutex);
    state().counters.push_back({name, &counter, counter.load(std::memory_order_relaxed)});
}

std::uint64_t Recorder::Digest(std::span<const std::uint32_t> code) {
    std::uint64_t value = 0xcbf29ce484222325ull;
    for (const auto word : code) value = (value ^ word) * 0x100000001b3ull;
    return value != 0 ? value : 1;
}

Recorder::Snapshot Recorder::Collect(Clock::time_point now) {
    auto& s = state();
    Snapshot result;
    result.frames = s.frames;
    result.presented = result.frames;
    result.startNs = std::chrono::duration_cast<std::chrono::nanoseconds>(s.begin.time_since_epoch()).count();
    result.endNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    result.sequence = s.sequence;
    result.enabled = s.on;
    result.toggled = s.toggled;
    s.frames = 0;
    s.begin = now;
    s.toggled = false;
    std::vector<std::shared_ptr<void>> threads;
    {
        std::lock_guard lock(s.mutex);
        threads = s.threads;
        for (auto& counter : s.counters) {
            const auto current = counter.value->load(std::memory_order_relaxed);
            result.counters[counter.name] += static_cast<double>(current - counter.previous);
            counter.previous = current;
        }
    }
    {
        for (auto& pointer : threads) {
            auto& t = *static_cast<Thread*>(pointer.get());
            double busy = 0;
            for (auto& row : t.phases) {
                const auto* name = row.name.load(std::memory_order_acquire);
                if (name == nullptr) continue;
                const auto phase = TraceFormat::Name(name, row.stage);
                const auto ms = row.ms.Take();
                result.phases[t.name][phase] += ms;
                if (phase.find("Wait") == std::string::npos && phase.find("wait") == std::string::npos) busy += ms;
            }
            const auto overflow = t.phaseOverflow.Take();
            result.phases[t.name]["other"] += overflow;
            result.accountedMs += busy + overflow;
            result.criticalPathMs = std::max(result.criticalPathMs, busy + overflow);
            for (auto& row : t.counters) {
                const auto* name = row.name.load(std::memory_order_acquire);
                if (name == nullptr) continue;
                const auto value = row.value.Take();
                result.counters[name] += value;
                result.threadCounters[t.name][name] += value;
            }
            result.counters["recorder_counter_overflow"] += t.counterOverflow.Take();
            for (std::size_t outcome = 0; outcome < t.census.size(); ++outcome) {
                auto& row = t.census[outcome];
                if (row.counters[0].local == 0 && row.counters[1].local == 0) continue;
                const auto prefix = std::string("H1.") + outcomeNames[outcome] + ".";
                const auto counter = [&](const std::string& name, Value& value) {
                    const auto delta = value.Take();
                    result.counters[prefix + name] += delta;
                    result.threadCounters[t.name][prefix + name] += delta;
                };
                for (std::size_t lock = 0; lock < row.elapsed.size(); ++lock) {
                    for (std::size_t i = 0; i < row.elapsed[lock].size(); ++i) result.phases[t.name][prefix + (lock ? "Locked." : "Outside.") + bucketNames[i]] += row.elapsed[lock][i].Take();
                }
                for (std::size_t i = 0; i < row.counters.size(); ++i) counter(metricNames[i], row.counters[i]);
                counter("consecutive_hits", row.pairs);
                for (std::size_t i = 0; i < row.emission.size(); ++i) {
                    counter(std::string("emission_") + bucketNames[i] + "_ms", row.emission[i]);
                    counter(std::string("redundant_emission_") + bucketNames[i] + "_ms", row.redundantEmission[i]);
                }
                for (std::size_t i = 0; i < row.same.size(); ++i) {
                    counter(std::string("same_") + stateNames[i], row.same[i]);
                    counter(std::string("same_commands_") + stateNames[i], row.sameCommands[i]);
                    counter(std::string("unknown_") + stateNames[i], row.unknown[i]);
                    counter(std::string("same_") + stateNames[i] + "_bucket_ms", row.sameMs[i]);
                    counter(std::string("same_") + stateNames[i] + "_emission_ms", row.sameEmissionMs[i]);
                }
            }
            if (t.censusPublishMs.local != 0) result.threadCounters[t.name]["h1_publish_ms"] += t.censusPublishMs.Take();
            const auto take = [](auto& row) {
                Shader value;
                value.key = row.key;
                value.calls = static_cast<std::uint64_t>(row.calls.Take());
                for (std::size_t i = 0; i < value.ms.size(); ++i) value.ms[i] = row.ms[i].Take();
                return value;
            };
            for (auto& row : t.shaders) {
                if (!row.occupied.load(std::memory_order_acquire)) continue;
                const auto value = take(row);
                if (value.calls == 0 && cost(value) == 0) continue;
                merge(result.shaders, value);
                merge(result.threadShaders[t.name], value);
                add(result.total, value);
            }
            const auto other = take(t.other);
            add(result.other, other);
            add(result.total, other);
            merge(result.threadShaders[t.name], other);
        }
    }
    std::sort(result.shaders.begin(), result.shaders.end(), [](const auto& a, const auto& b) { return cost(a) > cost(b); });
    for (std::size_t i = 40; i < result.shaders.size(); ++i) add(result.other, result.shaders[i]);
    if (result.shaders.size() > 40) result.shaders.resize(40);
    return result;
}

std::string Recorder::Snapshot::Json() const {
    std::ostringstream out;
    const double denominator = frames != 0 ? static_cast<double>(frames) : 1;
    out << "{\"frames\":" << frames << ",\"presented\":" << presented << ",\"span_ms\":" << (endNs - startNs) / 1e6
        << ",\"start_ns\":" << startNs << ",\"end_ns\":" << endNs << ",\"toggle_seq\":" << sequence
        << ",\"recorder_enabled\":" << (enabled ? "true" : "false") << ",\"recorder_toggled\":" << (toggled ? "true" : "false");
    const auto values = [&](const auto& entries) {
        out << '{';
        bool first = true;
        for (const auto& [name, value] : entries) {
            if (!first) out << ',';
            first = false;
            out << quoted(name) << ':' << value;
        }
        out << '}';
    };
    const auto roles = [&](const auto& entries) {
        out << '{';
        bool first = true;
        for (const auto& [name, rows] : entries) {
            if (!first) out << ',';
            first = false;
            out << quoted(name) << ':';
            values(rows);
        }
        out << '}';
    };
    const auto shaderRows = [&](const auto& rows) {
        out << '[';
        bool first = true;
        for (const auto& row : rows) {
            if (!first) out << ',';
            first = false;
            out << shaderJson(row);
        }
        out << ']';
    };
    out << ",\"phases_ms\":";
    roles(phases);
    out << ",\"counters\":";
    values(counters);
    out << ",\"counters_by_thread\":";
    roles(threadCounters);
    out << ",\"shaders\":{\"top\":";
    shaderRows(shaders);
    out << ",\"other\":" << shaderJson(other) << ",\"total\":" << shaderJson(total) << "},\"shaders_by_thread\":{";
    bool first = true;
    for (const auto& [name, rows] : threadShaders) {
        if (!first) out << ',';
        first = false;
        out << quoted(name) << ':';
        shaderRows(rows);
    }
    out << "},\"accounted_cpu_ms_per_frame\":" << accountedMs / denominator
        << ",\"unaccounted_ms_per_frame\":" << ((endNs - startNs) / 1e6 - criticalPathMs) / denominator << '}';
    return out.str();
}

std::string Recorder::Snapshot::Line() const {
    std::vector<std::pair<double, std::string>> order;
    for (const auto& [role, rows] : phases) for (const auto& [name, ms] : rows) order.emplace_back(ms, role + ":" + name);
    std::sort(order.rbegin(), order.rend());
    std::ostringstream out;
    out << "[archb] recorder frames=" << frames << " ms_per_frame=" << (frames != 0 ? accountedMs / frames : 0)
        << " recorder_toggled=" << toggled;
    for (std::size_t i = 0; i < std::min<std::size_t>(5, order.size()); ++i) out << ' ' << order[i].second << '=' << order[i].first;
    return out.str();
}

}

extern "C" void AgcDriverRecorderScopeBegin_nid_no_patch(AgcDriver::ArchB::Recorder::Scope* scope, const char* name, std::int64_t now, std::uint8_t phase) {
    scope->Begin(name, AgcDriver::ArchB::Recorder::Clock::time_point(AgcDriver::ArchB::Recorder::Clock::duration(now)), static_cast<AgcDriver::ArchB::Recorder::Phase>(phase));
}

extern "C" void AgcDriverRecorderScopeMark_nid_no_patch(AgcDriver::ArchB::Recorder::Scope* scope, const char* stage, std::int64_t now) {
    scope->Mark(stage, AgcDriver::ArchB::Recorder::Clock::time_point(AgcDriver::ArchB::Recorder::Clock::duration(now)));
}

extern "C" void AgcDriverRecorderScopeEnd_nid_no_patch(AgcDriver::ArchB::Recorder::Scope* scope, std::int64_t now) {
    scope->End(AgcDriver::ArchB::Recorder::Clock::time_point(AgcDriver::ArchB::Recorder::Clock::duration(now)));
}

extern "C" AgcDriver::ArchB::Recorder::Draw* AgcDriverRecorderDraw_nid_no_patch() {
    return AgcDriver::ArchB::Recorder::CurrentDraw();
}

extern "C" void AgcDriverRecorderVulkanBegin_nid_no_patch(AgcDriver::ArchB::Recorder::Scope* scope, const char* name) {
    using Recorder = AgcDriver::ArchB::Recorder;
    auto* draw = Recorder::CurrentDraw();
    if (draw == nullptr) return;
    using M = Recorder::Metric;
    const std::string_view operation(name);
    draw->Count(M::VulkanCalls);
    const char* phase = "Vulkan.Other";
    if (operation.starts_with("vkCmd")) draw->Count(M::VkcmdCalls);
    if (operation.starts_with("vkCmdDraw")) {
        draw->Count(M::DrawCalls);
        phase = "Vulkan.Draw";
    } else if (operation.starts_with("vkCmdPipelineBarrier")) phase = "Vulkan.Barriers";
    else if (operation == "vkCmdBindPipeline") { draw->Count(M::PipelineBinds); phase = "Vulkan.Pipeline"; }
    else if (operation == "vkCmdBindDescriptorSets") { draw->Count(M::DescriptorBinds); phase = "Vulkan.Descriptors"; }
    else if (operation == "vkUpdateDescriptorSets" || operation == "vkAllocateDescriptorSets") phase = "Vulkan.Descriptors";
    else if (operation.starts_with("vkCmdSet") || operation == "vkCmdPushConstants") {
        draw->Count(operation == "vkCmdPushConstants" ? M::PushCalls : M::DynamicCalls);
        phase = operation == "vkCmdPushConstants" ? "Vulkan.Push" : "Vulkan.Dynamic";
    } else if (operation.starts_with("vkCmdBeginRender")) { draw->Count(M::PassBegins); phase = "Vulkan.Pass"; }
    else if (operation.starts_with("vkCmdEndRender")) { draw->Count(M::PassEnds); phase = "Vulkan.Pass"; }
    scope->Begin(phase, Recorder::Clock::now(), Recorder::Phase::Record);
}
