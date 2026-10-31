#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <iomanip>

namespace AgcDriver::ArchB {

namespace {

constexpr const char* bucketNames[] = {"lookup_proof", "mutex_wait", "targets_pass", "descriptors", "pipeline", "dynamic_state", "push_constants", "draw_commands", "barriers", "data_refresh", "bookkeeping"};
constexpr const char* metricNames[] = {"calls", "continuations", "vulkan_calls", "vkcmd_calls", "descriptor_writes", "descriptor_elements", "image_elements", "buffer_elements", "descriptor_sets", "descriptor_binds", "pipeline_binds", "distinct_pipelines", "dynamic_calls", "push_calls", "draw_calls", "pass_begins", "pass_ends", "pass_continued", "barriers", "image_barriers", "layout_transitions", "snapshot_bytes", "binding_preparations", "binding_rewrites", "plain_stages", "ordered_access", "descriptor_set_requests", "snapshot_copied_bytes", "redundant_pipeline_binds", "redundant_dynamic_calls", "redundant_push_calls", "target_lookup", "target_view", "target_depth_lookup", "target_depth_created", "target_refresh", "target_refresh_proved", "target_collect", "target_alias_check", "target_flush", "target_keys", "target_validate", "target_upload", "target_clear_check", "target_depth_clear", "target_depth_sync", "target_depth_bookkeeping", "pass_hazard_check", "pass_compatibility", "rendering_info", "framebuffer_lookup", "target_created", "target_shadow_check", "target_clears", "target_layout_transitions", "target_lookup_ns", "target_refresh_ns", "target_wait_ns", "target_proof_ns", "target_collect_ns", "target_alias_ns", "target_flush_ns", "target_keys_ns", "target_validate_ns", "target_upload_ns", "target_clear_ns", "target_depth_lookup_ns", "target_depth_planes_ns", "target_depth_sync_ns", "target_depth_clear_ns", "target_depth_bookkeeping_ns", "pass_access_ns", "pass_hazard_ns", "pass_setup_ns", "pass_begin_ns", "pipeline_state_ns"};
static_assert(std::size(metricNames) == static_cast<std::size_t>(Recorder::Metric::Count));
constexpr const char* stateNames[] = {"pipeline", "descriptors", "targets", "dynamic", "push"};
constexpr const char* outcomeNames[] = {"Unclassified", "Hit", "Miss"};

Recorder::Bucket bucketOf(std::string_view name, std::string_view stage, Recorder::Bucket fallback) {
    using B = Recorder::Bucket;
    if (name == "GpuMutex.wait") return B::Wait;
    if (name == "Driver.DrawLookup" || name == "Draw.Key" || name == "Draw.Lookup" || name == "Resources.Proof" || name == "Resources.FullWalk") return B::Lookup;
    if (name == "Draw.Targets" || name.starts_with("H2.") || name == "Vulkan.Pass") return B::Targets;
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
        Value heldMs;
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

bool Recorder::MutexCensusEnabled() {
    static const bool requested = [] {
        const auto* value = std::getenv("APS5_ARCHB_MUTEX_CENSUS");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return requested && Enabled();
}

Recorder::MutexTimer::MutexTimer(const char* name) {
    if (MutexCensusEnabled()) scope.Begin(name, Clock::now());
}

void Recorder::Scope::Begin(const char* scope, Clock::time_point now, Phase phase) {
    if (!Enabled()) return;
    active = true;
    name = scope;
    previous = now;
    draw = currentDraw;
    mutexCensus = MutexCensusEnabled();
    parent = currentScope;
    if (draw != nullptr || mutexCensus) heldBefore = heldTime(now);
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
    const auto held = draw != nullptr || mutexCensus ? heldTime(now) : Clock::duration{};
    const auto inclusiveHeld = held - heldBefore;
    const auto exclusiveHeld = std::clamp(inclusiveHeld - heldChildren, Clock::duration{}, std::max(Clock::duration{}, exclusive));
    if (generation == Recorder::generation.load(std::memory_order_relaxed)) {
        Recorder::phase(name, stage, exclusive, mutexCensus ? exclusiveHeld : Clock::duration{});
        if (draw != nullptr && draw == currentDraw && draw->generation == generation) {
            const auto index = static_cast<std::size_t>(bucketOf(name, stage != nullptr ? stage : "", bucket));
            draw->elapsed[0][index] += exclusive - exclusiveHeld;
            draw->elapsed[1][index] += exclusiveHeld;
            if (metric != Metric::Count) draw->Count(metric, static_cast<std::uint64_t>(std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(exclusive).count())));
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

void Recorder::phase(const char* name, const char* stage, Clock::duration elapsed, Clock::duration held) {
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
        if (held != Clock::duration{}) row.heldMs.Add(milliseconds(held));
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
                const auto held = row.heldMs.Take();
                if (held != 0) {
                    const auto key = "M2.Held." + phase + "_ms";
                    result.threadCounters[t.name][key] += held;
                    result.counters[key] += held;
                }
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
    OwnershipCensus::Collect(result);
    return result;
}

std::string Recorder::Snapshot::Json() const {
    std::ostringstream out;
    if (OwnershipCensus::Enabled()) out << std::setprecision(17);
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

namespace AgcDriver::ArchB {
namespace {
using D2 = OwnershipCensus;
constexpr const char* d2Resources[] = {"buffer", "image", "upload", "conversion", "label", "host", "bda", "implicit"};
constexpr const char* d2Conflicts[] = {"d2_conflict_raw_ns", "d2_conflict_war_ns", "d2_conflict_waw_ns", "d2_conflict_layout_ns", "d2_conflict_label_ns", "d2_conflict_host_ns", "d2_candidate_independent_ns", "d2_unknown_ns"};
constexpr const char* d2Phases[] = {"other", "labels", "writers", "resources", "record", "bookkeeping", "seal", "submit", "gpu_wait", "callbacks", "publish", "release"};
constexpr const char* d2Metrics[] = {"d2_dispatch_commits", "d2_mixed_batches", "d2_pending_shadow_mismatches", "d2_early_retirement", "d2_overflow", "d2_accesses", "d2_unknown_accesses", "d2_submitted_accesses", "d2_finishing_accesses", "d2_published_accesses", "d2_discarded_accesses", "d2_requested_accesses", "d2_q0_wait_compute_ns", "d2_m2_reconciliation_mismatches", "d2_observer_ns"};
enum D2Metric : std::size_t { DispatchCommits, MixedBatches, ShadowMismatches, EarlyRetirement, Overflow, Accesses, UnknownAccesses, SubmittedAccesses, FinishingAccesses, PublishedAccesses, DiscardedAccesses, RequestedAccesses, WaitCompute, ReconciliationMismatches, ObserverNs };
constexpr std::size_t d2PhaseCount = static_cast<std::size_t>(D2::Phase::Count);
constexpr std::size_t d2ConflictCount = static_cast<std::size_t>(D2::Conflict::Count);
struct D2Totals {
    std::array<std::uint64_t, std::size(d2Metrics)> metrics{};
    std::array<std::uint64_t, d2ConflictCount> conflicts{};
    std::array<std::uint64_t, d2ConflictCount> tags{};
    std::array<std::uint64_t, d2PhaseCount> phases{};
    void Add(const D2Totals& other) {
        for (std::size_t i = 0; i < metrics.size(); ++i) metrics[i] += other.metrics[i];
        for (std::size_t i = 0; i < conflicts.size(); ++i) { conflicts[i] += other.conflicts[i]; tags[i] += other.tags[i]; }
        for (std::size_t i = 0; i < phases.size(); ++i) phases[i] += other.phases[i];
    }
};
struct D2State {
    struct Entry { D2::Observation observation; std::uint32_t next = UINT32_MAX; };
    struct Batch {
        std::uint64_t id = 0, recorder = 0, serial = 0;
        std::uint32_t first = UINT32_MAX, last = UINT32_MAX, queue = UINT32_MAX;
        D2::Life life = D2::Life::Open;
        bool mixed = false;
        bool truncated = false;
    };
    struct Waiter {
        bool active = false;
        D2::Clock::time_point begin{};
        D2::Summary requested;
        std::array<std::uint64_t, d2PhaseCount> phases{};
        std::uint64_t accounted = 0;
        D2Totals totals;
        struct Part { std::uint32_t queue; D2::Resource resource; D2Totals totals; };
        std::array<Part, 128> parts;
        std::size_t partCount = 0;
    };
    struct Row {
        bool used = false;
        std::uint32_t queue = UINT32_MAX;
        D2::Resource resource = D2::Resource::Implicit;
        D2Totals totals;
    };
    std::mutex mutex;
    std::array<Entry, 65536> entries;
    std::array<Batch, 1024> batches;
    std::array<Waiter, 16> waiters;
    std::array<Row, 256> rows;
    struct CommandSlot { bool used = false; D2::Summary summary; };
    std::array<CommandSlot, 128> commands;
    D2Totals totals;
    std::uint32_t free = 0;
    std::uint64_t nextBatch = 0, nextCommand = 0;
    bool held = false, compute = false;
    std::uint32_t queue = UINT32_MAX;
    D2::Phase phase = D2::Phase::Other;
    D2::Clock::time_point since{};
    D2::Summary observed;
    D2State() {
        for (std::uint32_t i = 0; i < entries.size(); ++i) entries[i].next = i + 1 < entries.size() ? i + 1 : UINT32_MAX;
    }
    Batch* Find(std::uint64_t id) {
        if (id == 0) return nullptr;
        auto& batch = batches[id % batches.size()];
        return batch.id == id ? &batch : nullptr;
    }
    D2Totals& RowFor(std::uint32_t origin, D2::Resource resource) {
        for (auto& row : rows) {
            if (!row.used) { row.used = true; row.queue = origin; row.resource = resource; }
            if (row.queue == origin && row.resource == resource) return row.totals;
        }
        ++totals.metrics[Overflow];
        static D2Totals dropped;
        return dropped;
    }
    void Count(D2Metric metric, std::uint32_t origin, D2::Resource resource, std::uint64_t value = 1) {
        totals.metrics[metric] += value;
        RowFor(origin, resource).metrics[metric] += value;
    }
    void Tick(D2::Clock::time_point now) {
        if (held && compute) {
            for (auto& waiter : waiters) {
                if (!waiter.active) continue;
                const auto start = std::max(since, waiter.begin);
                if (now > start) waiter.phases[static_cast<std::size_t>(phase)] += std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
            }
        }
        since = now;
    }
};
D2State& d2State() { static auto* state = new D2State; return *state; }
struct D2Cost {
    D2State& state;
    D2::Clock::time_point start = D2::Clock::now();
    ~D2Cost() { state.totals.metrics[ObserverNs] += std::chrono::duration_cast<std::chrono::nanoseconds>(D2::Clock::now() - start).count(); }
};
thread_local D2::Command* d2Command = nullptr;
thread_local D2::Resource d2Coverage = D2::Resource::Buffer;
thread_local bool d2Held = false;
thread_local unsigned d2Sync = 0;
D2::Phase d2Phase(D2::Phase phase) {
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    state.Tick(D2::Clock::now());
    return std::exchange(state.phase, phase);
}
void d2Assert(bool valid, D2Metric metric) {
    if (valid) return;
    ++d2State().totals.metrics[metric];
    std::fprintf(stderr, "[d2] ownership invariant failed: %s\n", d2Metrics[metric]);
    std::abort();
}
}

bool OwnershipCensus::Enabled() {
    static const bool enabled = [] {
        const auto one = [](const char* name) { const auto* value = std::getenv(name); return value != nullptr && std::string_view(value) == "1"; };
        return one("APS5_ARCHB_D2_OWNERSHIP_CENSUS") && one("APS5_ARCHB_MUTEX_CENSUS") && one("APS5_ARCHB_RECORDER");
    }();
    return enabled;
}

void OwnershipCensus::Summary::Add(Access access, Origin origin) {
    for (std::size_t i = 0; i < size; ++i) {
        auto& entry = accesses[i];
        if (entry.origin.queue == origin.queue && entry.access.begin == access.begin && entry.access.end == access.end &&
            entry.access.resource == access.resource && entry.access.backing == access.backing && entry.access.epoch == access.epoch &&
            entry.access.representation == access.representation && entry.access.layout == access.layout && entry.access.mip == access.mip && entry.access.layer == access.layer) {
            entry.access.flags |= access.flags;
            entry.access.complete &= access.complete;
            return;
        }
    }
    if (size == accesses.size()) { overflow = true; complete = false; return; }
    accesses[size++] = {access, origin};
}

OwnershipCensus::Classification OwnershipCensus::Classify(const Summary& requested, const Summary& observed) {
    Classification result;
    bool known = requested.complete && observed.complete && !requested.overflow && !observed.overflow;
    std::array<const Observation*, d2ConflictCount> reasons{};
    for (std::size_t i = 0; i < requested.size; ++i) {
        const auto& a = requested.accesses[i].access;
        known &= a.complete && a.backing != 0 && a.epoch != 0 && a.resource != Resource::Bda;
        for (std::size_t j = 0; j < observed.size; ++j) {
            const auto& entry = observed.accesses[j];
            const auto& b = entry.access;
            const bool sameBacking = a.backing != 0 && a.backing == b.backing && a.epoch == b.epoch && a.epoch != 0;
            if (a.backing != 0 && a.backing == b.backing && a.epoch != b.epoch) known = false;
            const bool overlap = a.begin < a.end && b.begin < b.end && a.begin < b.end && b.begin < a.end &&
                (a.backing == 0 || b.backing == 0 || sameBacking) && a.resource != Resource::Bda && b.resource != Resource::Bda;
            const bool image = a.representation != 0 && a.representation == b.representation && a.mip == b.mip && a.layer == b.layer;
            const auto reason = [&](Conflict conflict, bool matches) { if (matches) reasons[static_cast<std::size_t>(conflict)] = &entry; };
            reason(Conflict::Raw, overlap && (a.flags & Read) && (b.flags & Write));
            reason(Conflict::War, overlap && (a.flags & Write) && (b.flags & Read));
            reason(Conflict::Waw, overlap && (a.flags & Write) && (b.flags & Write));
            reason(Conflict::Layout, image && (a.flags & Layout) && (b.flags & Layout) && a.layout != b.layout);
            reason(Conflict::Label, overlap && ((a.flags | b.flags) & Label) && ((a.flags | b.flags) & Write));
            reason(Conflict::Host, overlap && ((a.flags | b.flags) & Host) && ((a.flags | b.flags) & Write));
        }
    }
    for (std::size_t i = 0; i < observed.size; ++i) {
        const auto& access = observed.accesses[i].access;
        known &= access.complete && access.backing != 0 && access.epoch != 0 && access.resource != Resource::Bda;
    }
    for (std::size_t i = 0; i < reasons.size(); ++i) if (reasons[i] != nullptr) result.tags |= 1u << i;
    for (const auto conflict : {Conflict::Label, Conflict::Host, Conflict::Layout, Conflict::Raw, Conflict::War, Conflict::Waw}) {
        const auto* entry = reasons[static_cast<std::size_t>(conflict)];
        if (entry == nullptr) continue;
        result.conflict = conflict;
        result.queue = entry->origin.queue;
        result.resource = entry->access.resource;
        return result;
    }
    result.conflict = known ? Conflict::Independent : Conflict::Unknown;
    if (observed.size != 0) {
        result.queue = observed.accesses[0].origin.queue;
        result.resource = observed.accesses[0].access.resource;
        for (std::size_t i = 0; i < observed.size; ++i) {
            if (!observed.accesses[i].access.complete) { result.queue = observed.accesses[i].origin.queue; result.resource = observed.accesses[i].access.resource; break; }
        }
    }
    return result;
}

OwnershipCensus::Command::Command(std::uint32_t queue, bool dispatch) {
    if (!Enabled()) return;
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    for (; slot < state.commands.size() && state.commands[slot].used; ++slot) {}
    if (slot < state.commands.size()) {
        state.commands[slot].used = true;
        requested = &state.commands[slot].summary;
        requested->size = 0;
        requested->complete = false;
        requested->overflow = false;
    } else ++state.totals.metrics[Overflow];
    active = true;
    previous = std::exchange(d2Command, this);
    origin.queue = queue;
    origin.command = ++state.nextCommand;
    if (dispatch) state.Count(DispatchCommits, queue, Resource::Implicit);
}
OwnershipCensus::Command::~Command() {
    if (!active) return;
    d2Command = previous;
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    if (slot < state.commands.size()) state.commands[slot].used = false;
}
OwnershipCensus::Origin OwnershipCensus::CurrentOrigin(std::uint32_t fallback) { return d2Command != nullptr ? d2Command->origin : Origin{fallback}; }
OwnershipCensus::Coverage::Coverage(Resource resource) : previous(d2Coverage) { if (Enabled()) d2Coverage = resource; }
OwnershipCensus::Coverage::~Coverage() { if (Enabled()) d2Coverage = previous; }

OwnershipCensus::PhaseScope::PhaseScope(Phase phase, bool syncOnly) {
    active = Enabled() && d2Held && (!syncOnly || d2Sync != 0);
    if (active) previous = d2Phase(phase);
}
OwnershipCensus::PhaseScope::~PhaseScope() { if (active) d2Phase(previous); }
void OwnershipCensus::PhaseScope::Mark(Phase phase) { if (active) d2Phase(phase); }
OwnershipCensus::Sync::Sync() {
    active = Enabled() && d2Held;
    if (active) { ++d2Sync; previous = d2Phase(Phase::Seal); }
}
OwnershipCensus::Sync::~Sync() { if (active) { d2Phase(previous); --d2Sync; } }

std::uint64_t OwnershipCensus::Open(std::uint64_t recorder) {
    if (!Enabled()) return 0;
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    for (std::size_t i = 0; i < state.batches.size(); ++i) {
        const auto id = ++state.nextBatch;
        auto& batch = state.batches[id % state.batches.size()];
        if (batch.id != 0) continue;
        batch = {};
        batch.id = id;
        batch.recorder = recorder;
        return id;
    }
    ++state.totals.metrics[Overflow];
    return 0;
}

void OwnershipCensus::Observe(std::uint64_t id, Access access, std::uint64_t note, std::uint32_t queue) {
    if (!Enabled()) return;
    if (access.resource == Resource::Buffer) access.resource = d2Coverage;
    auto origin = CurrentOrigin(queue);
    if (access.resource == Resource::Label && queue != UINT32_MAX && origin.queue != queue) { origin.queue = queue; origin.command = 0; }
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    D2Cost cost{state};
    auto* batch = state.Find(id);
    if (batch == nullptr) { ++state.totals.metrics[Overflow]; return; }
    origin.recorder = batch->recorder;
    origin.batch = batch->id;
    origin.serial = batch->serial;
    origin.note = note;
    if (batch->first == UINT32_MAX) batch->queue = origin.queue;
    else if (batch->queue != origin.queue && !batch->mixed) {
        batch->mixed = true;
        state.Count(MixedBatches, origin.queue, access.resource);
    }
    state.Count(Accesses, origin.queue, access.resource);
    if (!access.complete) state.Count(UnknownAccesses, origin.queue, access.resource);
    if (d2Held) {
        const bool overflow = state.observed.overflow;
        state.observed.Add(access.resource == Resource::Bda ? Access{.resource = Resource::Bda, .flags = access.flags} : access, origin);
        if (!overflow && state.observed.overflow) ++state.totals.metrics[Overflow];
    }
    if (access.resource == Resource::Bda && note == 0) {
        access.begin = access.end = 0;
        if (batch->last != UINT32_MAX) {
            auto& last = state.entries[batch->last].observation;
            if (last.access.resource == Resource::Bda && last.origin.note == 0 && last.origin.queue == origin.queue && last.origin.command == origin.command) {
                last.access.flags |= access.flags;
                return;
            }
        }
    }
    if (batch->truncated) return;
    if (state.free == UINT32_MAX) { ++state.totals.metrics[Overflow]; batch->truncated = true; return; }
    const auto index = state.free;
    auto& entry = state.entries[index];
    state.free = entry.next;
    entry = {{access, origin}, UINT32_MAX};
    if (batch->last != UINT32_MAX) state.entries[batch->last].next = index;
    else batch->first = index;
    batch->last = index;
}

void OwnershipCensus::Transition(std::uint64_t id, Life life, std::uint64_t serial) {
    if (!Enabled() || id == 0) return;
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    auto* batch = state.Find(id);
    d2Assert(batch != nullptr, ShadowMismatches);
    const bool valid = (life == Life::Submitted && batch->life == Life::Open && serial != 0) ||
        (life == Life::Finishing && batch->life == Life::Submitted) ||
        (life == Life::Published && batch->life == Life::Finishing) ||
        (life == Life::Discarded && batch->life == Life::Open) || (life == Life::Failed && batch->life == Life::Finishing);
    d2Assert(valid, EarlyRetirement);
    batch->life = life;
    if (life == Life::Submitted) batch->serial = serial;
    const auto metric = life == Life::Submitted ? SubmittedAccesses : life == Life::Finishing ? FinishingAccesses : life == Life::Published ? PublishedAccesses : DiscardedAccesses;
    for (auto i = batch->first; i != UINT32_MAX; i = state.entries[i].next) {
        auto& observation = state.entries[i].observation;
        observation.origin.serial = batch->serial;
        state.Count(metric, observation.origin.queue, observation.access.resource);
        if (life == Life::Finishing && d2Held) {
            const bool overflow = state.observed.overflow;
            state.observed.Add(observation.access.resource == Resource::Bda ? Access{.resource = Resource::Bda, .flags = observation.access.flags} : observation.access, observation.origin);
            if (!overflow && state.observed.overflow) ++state.totals.metrics[Overflow];
        }
    }
}

void OwnershipCensus::Retire(std::uint64_t id) {
    if (!Enabled() || id == 0) return;
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    auto* batch = state.Find(id);
    d2Assert(batch != nullptr, ShadowMismatches);
    d2Assert(batch->life == Life::Published || batch->life == Life::Discarded || batch->life == Life::Failed, EarlyRetirement);
    if (batch->last != UINT32_MAX) { state.entries[batch->last].next = state.free; state.free = batch->first; }
    batch->id = 0;
}

OwnershipCensus::Observation OwnershipCensus::Inspect(std::uint64_t id, std::size_t index) {
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    const auto* batch = state.Find(id);
    if (batch == nullptr) throw std::out_of_range("ownership batch");
    auto i = batch->first;
    for (; index != 0 && i != UINT32_MAX; --index) i = state.entries[i].next;
    if (i == UINT32_MAX) throw std::out_of_range("ownership access");
    return state.entries[i].observation;
}

void OwnershipCensus::Check(std::uint64_t id, std::span<const std::pair<std::uint64_t, std::uint64_t>> writes,
                            std::span<const std::uint64_t> notes, std::span<const std::pair<std::uint64_t, std::uint64_t>> snapshot, bool observable) {
    if (!Enabled() || id == 0) return;
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    D2Cost cost{state};
    const auto* batch = state.Find(id);
    d2Assert(batch != nullptr && writes.size() == notes.size(), ShadowMismatches);
    std::size_t index = 0;
    for (auto i = batch->first; i != UINT32_MAX; i = state.entries[i].next) {
        const auto& entry = state.entries[i].observation;
        if (entry.origin.note == 0) continue;
        d2Assert(index < notes.size() && notes[index] == entry.origin.note && writes[index].first == entry.access.begin && writes[index].second == entry.access.end, ShadowMismatches);
        if (observable && batch->life != Life::Published && batch->life != Life::Discarded) {
            const auto found = std::upper_bound(snapshot.begin(), snapshot.end(), entry.access.begin, [](auto address, const auto& range) { return address < range.first; });
            d2Assert(found != snapshot.begin() && std::prev(found)->second >= entry.access.end, EarlyRetirement);
        }
        ++index;
    }
    d2Assert(batch->truncated || index == notes.size(), ShadowMismatches);
}

void OwnershipCensus::CheckSnapshot(std::uint64_t recorder, std::span<const std::pair<std::uint64_t, std::uint64_t>> snapshot) {
    if (!Enabled()) return;
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    D2Cost cost{state};
    for (const auto& batch : state.batches) {
        if (batch.id == 0 || batch.recorder != recorder || batch.life == Life::Published || batch.life == Life::Discarded || batch.life == Life::Failed) continue;
        for (auto i = batch.first; i != UINT32_MAX; i = state.entries[i].next) {
            const auto& entry = state.entries[i].observation;
            if (entry.origin.note == 0) continue;
            const auto found = std::upper_bound(snapshot.begin(), snapshot.end(), entry.access.begin, [](auto address, const auto& range) { return address < range.first; });
            d2Assert(found != snapshot.begin() && std::prev(found)->second >= entry.access.end, EarlyRetirement);
        }
    }
}

void OwnershipCensus::HoldBegin(std::uint32_t queue, bool compute, Clock::time_point now) {
    if (!Enabled()) return;
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    state.held = true;
    state.compute = compute;
    state.queue = queue;
    state.since = now;
    state.phase = Phase::Other;
    state.observed.size = 0;
    state.observed.complete = false;
    state.observed.overflow = false;
    d2Held = true;
}

void OwnershipCensus::HoldEnd(Clock::time_point now) {
    if (!Enabled()) return;
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    D2Cost cost{state};
    state.Tick(now);
    if (state.compute) for (auto& waiter : state.waiters) {
        if (!waiter.active) continue;
        const auto classification = Classify(waiter.requested, state.observed);
        const auto origin = classification.queue == UINT32_MAX ? state.queue : classification.queue;
        std::size_t part = 0;
        for (; part < waiter.partCount; ++part) if (waiter.parts[part].queue == origin && waiter.parts[part].resource == classification.resource) break;
        if (part == waiter.partCount && part < waiter.parts.size()) waiter.parts[waiter.partCount++] = {origin, classification.resource, {}};
        if (part == waiter.parts.size()) ++state.totals.metrics[Overflow];
        D2Totals contribution;
        const auto ns = std::accumulate(waiter.phases.begin(), waiter.phases.end(), std::uint64_t{});
        waiter.accounted += ns;
        contribution.metrics[WaitCompute] = ns;
        const auto category = static_cast<std::size_t>(classification.conflict);
        contribution.conflicts[category] = ns;
        for (std::size_t i = 0; i < d2ConflictCount; ++i) if (classification.tags & (1u << i)) contribution.tags[i] = ns;
        contribution.phases = waiter.phases;
        waiter.totals.Add(contribution);
        if (part < waiter.parts.size()) waiter.parts[part].totals.Add(contribution);
        waiter.phases.fill(0);
    }
    state.held = false;
    d2Held = false;
}

int OwnershipCensus::WaitBegin(Clock::time_point now, Clock::time_point* measured) {
    if (!Enabled()) return -1;
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    if (measured != nullptr) *measured = now = Clock::now();
    for (std::size_t i = 0; i < state.waiters.size(); ++i) {
        auto& waiter = state.waiters[i];
        if (waiter.active) continue;
        waiter.active = true;
        waiter.begin = now;
        waiter.accounted = 0;
        waiter.totals = {};
        waiter.partCount = 0;
        waiter.phases.fill(0);
        waiter.requested.size = 0;
        waiter.requested.complete = false;
        waiter.requested.overflow = false;
        if (d2Command != nullptr && d2Command->Requested() != nullptr) waiter.requested = *d2Command->Requested();
        if (waiter.requested.overflow) ++state.totals.metrics[Overflow];
        state.Count(RequestedAccesses, 0, Resource::Implicit, waiter.requested.size);
        return static_cast<int>(i);
    }
    ++state.totals.metrics[Overflow];
    return -1;
}

void OwnershipCensus::WaitEnd(int index, Clock::time_point now, std::uint64_t expected) {
    if (index < 0) return;
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    auto& waiter = state.waiters.at(static_cast<std::size_t>(index));
    if (waiter.accounted != expected) ++state.totals.metrics[ReconciliationMismatches];
    state.totals.Add(waiter.totals);
    for (std::size_t i = 0; i < waiter.partCount; ++i) state.RowFor(waiter.parts[i].queue, waiter.parts[i].resource).Add(waiter.parts[i].totals);
    waiter.active = false;
}

void OwnershipCensus::Collect(Recorder::Snapshot& snapshot) {
    if (!Enabled()) return;
    auto& state = d2State();
    std::lock_guard lock(state.mutex);
    const auto take = [](auto& output, D2Totals& totals) {
        for (std::size_t i = 0; i < std::size(d2Metrics); ++i) output[d2Metrics[i]] += std::exchange(totals.metrics[i], 0);
        for (std::size_t i = 0; i < d2ConflictCount; ++i) {
            output[d2Conflicts[i]] += std::exchange(totals.conflicts[i], 0);
            output[std::string("d2_tag_") + d2Conflicts[i]] += std::exchange(totals.tags[i], 0);
        }
        for (std::size_t i = 0; i < d2PhaseCount; ++i) {
            const auto ns = std::exchange(totals.phases[i], 0);
            output[std::string("d2_wait_phase_ns.") + d2Phases[i]] += ns;
            if (i >= static_cast<std::size_t>(Phase::Seal)) output[std::string("d2_sync_") + d2Phases[i] + "_ns"] += ns;
        }
    };
    take(snapshot.counters, state.totals);
    snapshot.counters["d2_fixed_storage_bytes"] = sizeof(D2State);
    for (auto& row : state.rows) {
        if (!row.used) continue;
        char role[96];
        std::snprintf(role, sizeof role, "d2 origin q0x%x/%s", row.queue, d2Resources[static_cast<std::size_t>(row.resource)]);
        take(snapshot.threadCounters[role], row.totals);
    }
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

extern "C" bool AgcDriverRecorderTargets_nid_no_patch() {
    return AgcDriver::ArchB::Recorder::TargetDraw() != nullptr;
}

extern "C" void AgcDriverRecorderVulkanBegin_nid_no_patch(AgcDriver::ArchB::Recorder::Scope* scope, const char* name) {
    using Recorder = AgcDriver::ArchB::Recorder;
    auto* draw = Recorder::CurrentDraw();
    if (draw == nullptr) return;
    using M = Recorder::Metric;
    const std::string_view operation(name);
    if (operation.starts_with("vkCmdClear") && Recorder::TargetDraw() != nullptr) draw->Count(M::TargetClears);
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
