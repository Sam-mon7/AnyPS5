#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace AgcDriver::ArchB {

namespace {

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
    parent = currentScope;
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
    if (generation == Recorder::generation.load(std::memory_order_relaxed)) {
        Recorder::phase(name, stage, exclusive);
        if (work != nullptr && work->generation == generation) work->elapsed[static_cast<std::size_t>(category)] += exclusive;
    }
    if (parent != nullptr) parent->children += inclusive;
    previous = now;
    children = {};
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
}

Recorder::Work::~Work() {
    if (Enabled() && generation == Recorder::generation.load(std::memory_order_relaxed)) AddShader(key, call ? 1 : 0, elapsed);
    currentWork = previous;
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
