#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/CheapClock.hpp"
#include "ArchBRecorderHandoff.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

using AgcDriver::ArchB::Recorder;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void reconcileNear(double actual, double expected) {
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
    reconcileNear(result.total.ms[0], 140);
    reconcileNear(result.total.ms[2], 40);
    reconcileNear(result.total.ms[3], 20);
    reconcileNear(result.accountedMs, 200);
    reconcileNear(result.criticalPathMs, 100);
    reconcileNear(result.counters.at("fixture_counter"), 14);
    for (const auto* role : {"queue worker 0", "draw finisher"}) {
        const auto& phases = result.phases.at(role);
        reconcileNear(phases.at("prepare.first"), 20);
        reconcileNear(phases.at("prepare"), 50);
        reconcileNear(phases.at("proof"), 20);
        reconcileNear(phases.at("bind"), 10);
        reconcileNear(sum(result.threadShaders.at(role).front().ms), 100);
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
    reconcileNear(sum(actual.total.ms), phases);
    require(actual.total.ms[2] > 0 && actual.total.calls == 1, "PerformanceTimer did not record");
    std::cout << result.Json() << '\n';
}

void noPathChange() {
    const auto start = Recorder::Clock::now();
    std::array<bool, 1024> off{};
    for (unsigned enabled = 0; enabled != 2; ++enabled) {
        Recorder::Boundary(enabled != 0, enabled, start);
        for (unsigned bits = 0; bits != off.size(); ++bits) {
            std::array<bool, 10> values{};
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
    reconcileNear(result.counters.at("fixture"), 3);
    reconcileNear(result.counters.at("child_counter"), 11);
    Recorder::Boundary(true, 1, start + 3s);
    Recorder::Count("fixture", 4);
    result = Recorder::Collect(start + 4s);
    require(!result.toggled, "stable interval was tagged");
    reconcileNear(result.counters.at("fixture"), 4);
    Recorder::Scope crossing;
    crossing.Begin("crossing", start + 4s);
    Recorder::Boundary(false, 2, start + 5s);
    crossing.End(start + 6s);
    Recorder::Count("fixture", 100);
    result = Recorder::Collect(start + 6s);
    require(result.toggled && !result.enabled, "off transition was not tagged");
    reconcileNear(result.counters.at("fixture"), 0);
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
    reconcileNear(sum(result.total.ms), 11000);
    reconcileNear(sum(result.other.ms), 10600);
    auto total = sum(result.other.ms);
    for (const auto& row : result.shaders) total += sum(row.ms);
    reconcileNear(total, sum(result.total.ms));
    reconcileNear(sum(Recorder::Collect(start + 2s).total.ms), 0);
}

void cheapClock() {
    const auto first = AgcDriver::ArchB::CheapClock::Now(true);
    auto previous = first;
    for (int i = 0; i < 10000; ++i) {
        const auto current = AgcDriver::ArchB::CheapClock::Now(true);
        require(current >= previous, "cheap clock moved backwards");
        previous = current;
    }
#ifdef _WIN32
    LARGE_INTEGER frequency, qpcBegin, qpcEnd;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&qpcBegin);
    const auto cheapBegin = AgcDriver::ArchB::CheapClock::Now(true);
    Sleep(100);
    const auto cheapEnd = AgcDriver::ArchB::CheapClock::Now(true);
    QueryPerformanceCounter(&qpcEnd);
    const auto qpcNs = static_cast<double>(qpcEnd.QuadPart - qpcBegin.QuadPart) * 1e9 / static_cast<double>(frequency.QuadPart);
    const auto cheapNs = std::chrono::duration<double, std::nano>(cheapEnd - cheapBegin).count();
    const auto& calibration = AgcDriver::ArchB::CheapClock::GetCalibration();
    std::cout << "cheap_clock rdtsc=" << calibration.rdtsc << " frequency=" << calibration.frequency << " qpc_ns=" << qpcNs << " cheap_ns=" << cheapNs << '\n';
    require(std::abs(cheapNs - qpcNs) / qpcNs <= 0.01, "cheap clock differs from QPC by more than 1%");
#endif
}

void cheapTimerFlip() {
    const auto start = Recorder::Clock::now();
    Recorder::Boundary(true, 100, start);
    Recorder::BindThread("cheap timer flip");
    {
        AgcDriver::ArchB::CheapClock::Scope selected(true);
        AgcDriver::PerformanceTimer timer("crossing cheap timer");
        {
            AgcDriver::ArchB::CheapClock::Scope flipped(false);
            std::this_thread::sleep_for(2ms);
            timer.Finish();
        }
    }
    const auto result = Recorder::Collect(Recorder::Clock::now());
    require(result.counters.at("fix_cheap_timers_scopes") != 0, "cheap timer scope was not counted");
    require(result.phases.at("cheap timer flip").at("crossing cheap timer") > 0, "timer spanning a switch flip was lost");
}

double timerBench(bool cheap) {
    constexpr std::size_t iterations = 100000;
    AgcDriver::ArchB::CheapClock::Scope selected(cheap);
    static_cast<void>(AgcDriver::ArchB::CheapClock::Now(cheap));
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        AgcDriver::PerformanceTimer timer("timer benchmark");
    }
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() / iterations;
}

double countBench(bool cheap) {
    constexpr std::size_t iterations = 1000000;
    AgcDriver::ArchB::CheapClock::Scope selected(cheap);
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) Recorder::Count("count benchmark");
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() / iterations;
}

void cheapBenchmark() {
    Recorder::Boundary(true, 101, Recorder::Clock::now());
    Recorder::BindThread("cheap benchmark");
    const auto timerOff = timerBench(false);
    const auto timerOn = timerBench(true);
    const auto countOff = countBench(false);
    const auto countOn = countBench(true);
    std::cout << "performance_timer off_ns=" << timerOff << " on_ns=" << timerOn << '\n';
    std::cout << "recorder_count off_ns=" << countOff << " on_ns=" << countOn << '\n';
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
        else if (name == "cheap_clock") cheapClock();
        else if (name == "cheap_timer_flip") cheapTimerFlip();
        else if (name == "cheap_benchmark") cheapBenchmark();
        else throw std::runtime_error("unknown test case");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
