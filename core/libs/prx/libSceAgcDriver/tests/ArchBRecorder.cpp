#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "ArchBRecorderHandoff.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

using AgcDriver::ArchB::Recorder;
using namespace std::chrono_literals;

#ifdef ARCHB_TEST_EMUTLS
static std::atomic<bool> trackEmulatedTls{false};
static std::atomic<unsigned> emulatedTlsCalls{0};
extern "C" void* __real___emutls_get_address(void* object);
extern "C" void* __wrap___emutls_get_address(void* object) {
    if (trackEmulatedTls.load(std::memory_order_relaxed)) emulatedTlsCalls.fetch_add(1, std::memory_order_relaxed);
    return __real___emutls_get_address(object);
}
#endif

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void expectNear(double actual, double expected) {
    require(std::abs(actual - expected) <= std::max(0.000001, expected * 0.02), "cost did not reconcile within 2%");
}

double sum(const std::array<double, 4>& values) {
    return values[0] + values[1] + values[2] + values[3];
}

void synthetic(const char* role, bool call) {
    Recorder::BindThread(role);
    Recorder::Work work({Recorder::Kind::Draw, 123}, Recorder::Phase::Prepare, call);
    const Recorder::Clock::time_point start{};
    Recorder::Scope outer, proof, bind;
    outer.Begin("prepare", start);
    proof.Begin("proof", start + 10ms, Recorder::Phase::Proof);
    bind.Begin("bind", start + 20ms, Recorder::Phase::Bind);
    bind.End(start + 30ms);
    proof.End(start + 40ms);
    outer.Mark("first", start + 50ms);
    outer.End(start + 100ms);
    Recorder::Count("fixture_counter", 7);
}

void reconciles() {
    const Recorder::Clock::time_point start{};
    Recorder::Boundary(true, 0, start);
    std::thread worker([] { synthetic("queue worker 0", true); });
    std::thread finisher([] { synthetic("draw finisher", false); });
    worker.join();
    finisher.join();
    Recorder::Boundary(true, 0, start + 100ms);
    const auto result = Recorder::Collect(start + 100ms);
    require(result.frames == 1 && result.total.calls == 1, "handoff counted an extra draw");
    expectNear(result.total.ms[0], 140);
    expectNear(result.total.ms[2], 40);
    expectNear(result.total.ms[3], 20);
    expectNear(result.accountedMs, 200);
    expectNear(result.criticalPathMs, 100);
    expectNear(result.counters.at("fixture_counter"), 14);
    for (const auto* role : {"queue worker 0", "draw finisher"}) {
        const auto& phases = result.phases.at(role);
        expectNear(phases.at("prepare.first"), 20);
        expectNear(phases.at("prepare"), 50);
        expectNear(phases.at("proof"), 20);
        expectNear(phases.at("bind"), 10);
        expectNear(sum(result.threadShaders.at(role).front().ms), 100);
    }
    Recorder::BindThread("timer integration");
    {
        Recorder::Work work({Recorder::Kind::Dispatch, 456});
        AgcDriver::PerformanceTimer outer("outer");
        {
            AgcDriver::PerformanceTimer inner("inner", Recorder::Phase::Proof);
            std::this_thread::sleep_for(2ms);
        }
        outer.Mark("prepare");
    }
    const auto actual = Recorder::Collect(start + 200ms);
    double phases = 0;
    for (const auto& [name, ms] : actual.phases.at("timer integration")) phases += ms;
    expectNear(sum(actual.total.ms), phases);
    require(actual.total.ms[2] > 0 && actual.total.calls == 1, "PerformanceTimer did not record");
    std::cout << result.Json() << '\n';
}

void noPathChange() {
    const auto start = Recorder::Clock::now();
    std::array<bool, std::size_t{1} << RecorderFixtureHandoffCount> off{};
    for (unsigned enabled = 0; enabled != 2; ++enabled) {
        Recorder::Boundary(enabled != 0, enabled, start);
        for (unsigned bits = 0; bits != off.size(); ++bits) {
            std::array<bool, RecorderFixtureHandoffCount> values{};
            for (unsigned i = 0; i != values.size(); ++i) values[i] = (bits & (1u << i)) != 0;
            Recorder::Work work({Recorder::Kind::Draw, bits + 1u});
            AgcDriver::PerformanceTimer timer("fixture draw");
            const bool decision = RecorderFixtureHandoff(values);
            if (enabled == 0) off[bits] = decision;
            else require(off[bits] == decision, "recorder changed the production handoff predicate");
        }
    }
    require(std::count(off.begin(), off.end(), true) == 1, "fixture did not exercise both handoff outcomes");
}

void toggleIntervalTagged() {
    const Recorder::Clock::time_point start{};
    static std::atomic<std::uint64_t> child{0};
    Recorder::RegisterCounter("child_counter", child);
    Recorder::Boundary(false, 0, start);
    child.fetch_add(11, std::memory_order_relaxed);
    Recorder::Count("fixture", 100);
    Recorder::Boundary(true, 1, start + 1s);
    Recorder::Count("fixture", 3);
    auto result = Recorder::Collect(start + 2s);
    require(result.toggled && result.enabled && result.sequence == 1, "on transition was not tagged");
    expectNear(result.counters.at("fixture"), 3);
    expectNear(result.counters.at("child_counter"), 11);
    Recorder::Boundary(true, 1, start + 3s);
    Recorder::Count("fixture", 4);
    result = Recorder::Collect(start + 4s);
    require(!result.toggled, "stable interval was tagged");
    expectNear(result.counters.at("fixture"), 4);
    Recorder::Scope crossing;
    crossing.Begin("crossing", start + 4s);
    Recorder::Boundary(false, 2, start + 5s);
    crossing.End(start + 6s);
    Recorder::Count("fixture", 100);
    result = Recorder::Collect(start + 6s);
    require(result.toggled && !result.enabled, "off transition was not tagged");
    expectNear(result.counters.at("fixture"), 0);
    Recorder::Boundary(true, 3, start + 7s);
    Recorder::Collect(start + 8s);
    Recorder::Boundary(true, 4, start + 9s);
    require(Recorder::Collect(start + 10s).toggled, "configuration transition was not tagged");
}

void shaderTableOverflow() {
    const Recorder::Clock::time_point start{};
    Recorder::Boundary(true, 0, start);
    const std::array<Recorder::Clock::duration, 4> elapsed{1ms, 2ms, 3ms, 4ms};
    for (std::uint64_t i = 1; i <= 1100; ++i) Recorder::AddShader({i % 2 ? Recorder::Kind::Draw : Recorder::Kind::Dispatch, i}, 1, elapsed);
    Recorder::Boundary(true, 0, start + 1s);
    const auto result = Recorder::Collect(start + 1s);
    require(result.shaders.size() == 40 && result.total.calls == 1100, "table overflow lost calls");
    require(result.other.calls == 1060, "other did not include overflow and the tail");
    expectNear(sum(result.total.ms), 11000);
    expectNear(sum(result.other.ms), 10600);
    auto total = sum(result.other.ms);
    for (const auto& row : result.shaders) total += sum(row.ms);
    expectNear(total, sum(result.total.ms));
    expectNear(sum(Recorder::Collect(start + 2s).total.ms), 0);
}

void hotThreadState() {
    using AgcDriver::PerformanceContext;
    using AgcDriver::FrameTiming;
    const auto start = Recorder::Clock::now();
    Recorder::Boundary(true, 0, start);
    Recorder::BindThread("hot thread state");
    FrameTiming frame(1);
    require(PerformanceContext::Current() == nullptr, "initial context was not empty");
    {
        PerformanceContext outer(&frame);
        require(PerformanceContext::Current() == &frame, "outer context was not installed");
        std::thread worker([] {
            require(PerformanceContext::Current() == nullptr, "context leaked into another thread");
            FrameTiming nestedFrame(2);
            PerformanceContext nested(&nestedFrame);
            require(PerformanceContext::Current() == &nestedFrame, "worker context was not installed");
        });
        worker.join();
        {
            PerformanceContext inner(nullptr);
            require(PerformanceContext::Current() == nullptr, "nested context was not installed");
        }
        require(PerformanceContext::Current() == &frame, "nested context was not restored");
    }
    require(PerformanceContext::Current() == nullptr, "outer context was not restored");
    const auto exercise = [] {
        PerformanceContext context(nullptr);
        Recorder::Work work({Recorder::Kind::Draw, 123});
        AgcDriver::PerformanceTimer timer("hot thread state");
        Recorder::Count("hot thread state");
        timer.Mark("step");
    };
    exercise();
#ifdef ARCHB_TEST_EMUTLS
    trackEmulatedTls.store(true, std::memory_order_relaxed);
#endif
    for (unsigned i = 0; i != 100; ++i) exercise();
#ifdef ARCHB_TEST_EMUTLS
    trackEmulatedTls.store(false, std::memory_order_relaxed);
    std::cout << "hot emulated TLS calls: " << emulatedTlsCalls.load() << '\n';
    require(emulatedTlsCalls.load() == 0, "hot recorder state used emulated TLS");
#endif
    const auto result = Recorder::Collect(Recorder::Clock::now());
    require(result.total.calls == 101, "hot loop lost work calls");
    expectNear(result.counters.at("hot thread state"), 101);
}

}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected one test case");
        const std::string_view name = argv[1];
        if (name == "reconciles") reconciles();
        else if (name == "no_path_change") noPathChange();
        else if (name == "toggle_interval_tagged") toggleIntervalTagged();
        else if (name == "shader_table_overflow") shaderTableOverflow();
        else if (name == "hot_thread_state") hotThreadState();
        else throw std::runtime_error("unknown test case");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
