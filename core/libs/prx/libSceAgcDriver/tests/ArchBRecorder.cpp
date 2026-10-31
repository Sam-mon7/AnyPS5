#include "prx/libc/include/GuestArena.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "ArchBRecorderHandoff.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#undef near
#endif

using AgcDriver::ArchB::Recorder;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void near(double actual, double expected) {
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
    near(result.total.ms[0], 140);
    near(result.total.ms[2], 40);
    near(result.total.ms[3], 20);
    near(result.accountedMs, 200);
    near(result.criticalPathMs, 100);
    near(result.counters.at("fixture_counter"), 14);
    for (const auto* role : {"queue worker 0", "draw finisher"}) {
        const auto& phases = result.phases.at(role);
        near(phases.at("prepare.first"), 20);
        near(phases.at("prepare"), 50);
        near(phases.at("proof"), 20);
        near(phases.at("bind"), 10);
        near(sum(result.threadShaders.at(role).front().ms), 100);
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
    near(sum(actual.total.ms), phases);
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
    near(result.counters.at("fixture"), 3);
    near(result.counters.at("child_counter"), 11);
    Recorder::Boundary(true, 1, start + 3s);
    Recorder::Count("fixture", 4);
    result = Recorder::Collect(start + 4s);
    require(!result.toggled, "stable interval was tagged");
    near(result.counters.at("fixture"), 4);
    Recorder::Scope crossing;
    crossing.Begin("crossing", start + 4s);
    Recorder::Boundary(false, 2, start + 5s);
    crossing.End(start + 6s);
    Recorder::Count("fixture", 100);
    result = Recorder::Collect(start + 6s);
    require(result.toggled && !result.enabled, "off transition was not tagged");
    near(result.counters.at("fixture"), 0);
    Recorder::Boundary(true, 3, start + 7s);
    Recorder::Collect(start + 8s);
    Recorder::Boundary(true, 4, start + 9s);
    require(Recorder::Collect(start + 10s).toggled, "configuration transition was not tagged");
}

void hitCensus(bool enabled) {
    const Recorder::Clock::time_point start{};
    Recorder::BindThread("h1 fixture");
    Recorder::Boundary(enabled, 700, start);
    const auto run = [&](Recorder::DrawOutcome outcome, std::uint64_t pipeline, std::uint64_t batch, bool unknown) {
        Recorder::Draw draw;
        Recorder::Scope outer, proof, bind;
        outer.Begin("fixture", start);
        proof.Begin("Resources.Proof", start + 10ms);
        proof.End(start + 20ms);
        Recorder::GpuLock(true, start + 30ms);
        bind.Begin("Vulkan.Descriptors", start + 40ms);
        bind.End(start + 60ms);
        Recorder::GpuLock(false, start + 80ms);
        outer.End(start + 100ms);
        draw.Classify(outcome);
        draw.Count(Recorder::Metric::DescriptorWrites, 3);
        draw.Word(Recorder::State::Pipeline, pipeline);
        draw.Commands(12, batch);
        if (unknown) draw.Unknown(Recorder::State::Pipeline);
        return draw.SameOperation(Recorder::State::Pipeline, 0);
    };
    require(!run(Recorder::DrawOutcome::Hit, 1, 1, false), "first operation compared equal");
    require(run(Recorder::DrawOutcome::Hit, 1, 1, false) == enabled, "unchanged operation comparison disagrees");
    require(!run(Recorder::DrawOutcome::Hit, 1, 2, false), "new batch compared equal");
    require(!run(Recorder::DrawOutcome::Hit, 2, 2, false), "changed pipeline compared equal");
    require(!run(Recorder::DrawOutcome::Hit, 2, 2, true), "unknown pipeline compared equal");
    require(!run(Recorder::DrawOutcome::Miss, 2, 2, false), "miss compared equal");
    require(!run(Recorder::DrawOutcome::Hit, 2, 2, false), "hit after miss compared equal");
    const auto result = Recorder::Collect(start + 1s);
    if (!enabled) {
        require(!result.counters.contains("H1.Hit.calls"), "disabled census published data");
        return;
    }
    const auto& values = result.threadCounters.at("h1 fixture");
    near(values.at("H1.Hit.calls"), 6);
    near(values.at("H1.Hit.descriptor_writes"), 18);
    near(values.at("H1.Hit.consecutive_hits"), 4);
    near(values.at("H1.Hit.same_pipeline"), 2);
    near(values.at("H1.Hit.same_commands_pipeline"), 1);
    near(values.at("H1.Hit.unknown_pipeline"), 1);
    near(values.at("H1.Hit.emission_descriptors_ms"), 120);
    const auto& phases = result.phases.at("h1 fixture");
    near(phases.at("H1.Hit.Outside.lookup_proof"), 60);
    near(phases.at("H1.Hit.Locked.descriptors"), 120);
    near(phases.at("H1.Hit.Locked.bookkeeping"), 180);
    near(phases.at("H1.Hit.Outside.bookkeeping"), 240);
    near(result.accountedMs, 700);
    Recorder::Sequence first, second;
    first.Add(1);
    second.Add(1);
    require(first.Same(second), "equal bounded state did not compare");
    for (std::size_t i = 0; i <= 512; ++i) first.Add(i);
    require(first.overflow && !first.Same(second), "overflow state compared equal");
    {
        Recorder::Draw crossing;
        crossing.Classify(Recorder::DrawOutcome::Hit);
        Recorder::Boundary(false, 701, start + 2s);
    }
    const auto toggled = Recorder::Collect(start + 3s);
    near(toggled.counters.at("H1.Hit.calls"), 0);
}

void refreshCensus(bool enabled) {
    const Recorder::Clock::time_point start{};
    Recorder::BindThread("h3 fixture");
    Recorder::Boundary(enabled, 800, start);
    {
        Recorder::Draw draw;
        Recorder::Scope prelock, refresh, snapshot, complete;
        prelock.Begin("Draw.Prelock", start);
        prelock.Mark("targets", start + 1ms);
        prelock.Mark("lookup", start + 3ms);
        prelock.Mark("surfaces", start + 6ms);
        prelock.Mark("proof", start + 10ms);
        prelock.End(start + 10ms);
        refresh.Begin("Draw.BindingRefresh", start + 10ms);
        refresh.Mark("setup", start + 11ms);
        snapshot.Begin("Draw.Snapshot", start + 11ms);
        snapshot.Mark("eligibility", start + 13ms);
        snapshot.Mark("unwritten", start + 16ms);
        snapshot.Mark("collect", start + 20ms);
        snapshot.Mark("lookup", start + 25ms);
        snapshot.Mark("copy", start + 31ms);
        snapshot.Mark("keep", start + 38ms);
        snapshot.End(start + 38ms);
        refresh.Mark("selection", start + 39ms);
        refresh.End(start + 39ms);
        complete.Begin("Resources.Complete", start + 39ms);
        complete.Mark("images", start + 40ms);
        complete.Mark("upload", start + 42ms);
        complete.Mark("bda", start + 45ms);
        complete.Mark("descriptors", start + 49ms);
        complete.Mark("proof", start + 54ms);
        complete.End(start + 54ms);
        draw.Classify(Recorder::DrawOutcome::Hit);
    }
    const auto result = Recorder::Collect(start + 1s);
    if (!enabled) {
        require(!result.counters.contains("H1.Hit.h3_prelock_targets_ns"), "disabled refresh census published counters");
        return;
    }
    const auto& values = result.threadCounters.at("h3 fixture");
    near(values.at("H1.Hit.h3_prelock_targets_ns"), 1000000);
    near(values.at("H1.Hit.h3_prelock_lookup_ns"), 2000000);
    near(values.at("H1.Hit.h3_prelock_surfaces_ns"), 3000000);
    near(values.at("H1.Hit.h3_prelock_proof_ns"), 4000000);
    near(values.at("H1.Hit.h3_binding_setup_ns"), 1000000);
    near(values.at("H1.Hit.h3_binding_selection_ns"), 1000000);
    near(values.at("H1.Hit.h3_snapshot_eligibility_ns"), 2000000);
    near(values.at("H1.Hit.h3_snapshot_collect_ns"), 4000000);
    near(values.at("H1.Hit.h3_snapshot_lookup_ns"), 5000000);
    near(values.at("H1.Hit.h3_complete_images_ns"), 1000000);
    near(values.at("H1.Hit.h3_complete_proof_ns"), 5000000);
    near(result.phases.at("h3 fixture").at("H1.Hit.Outside.data_refresh"), 54);
    near(result.accountedMs, 54);
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
    near(sum(result.total.ms), 11000);
    near(sum(result.other.ms), 10600);
    auto total = sum(result.other.ms);
    for (const auto& row : result.shaders) total += sum(row.ms);
    near(total, sum(result.total.ms));
    near(sum(Recorder::Collect(start + 2s).total.ms), 0);
}

void writeWatchCensus(bool recorder, bool census) {
#ifdef _WIN32
    Recorder::Boundary(recorder, 0, Recorder::Clock::now());
    const bool active = recorder && census;
    require((GuestArena::GuestArenaCensusCallback_nid_no_patch() != nullptr) == active, "write-watch census gating failed");
    const auto exercise = [](const char* role) {
        Recorder::BindThread(role);
        auto* memory = static_cast<volatile unsigned char*>(VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
        require(memory != nullptr, "write-watch allocation failed");
        auto* base = const_cast<unsigned char*>(memory);
        require(ResetWriteWatch(base, 65536) == 0, "initial reset failed");
        for (unsigned caller = 0; caller < static_cast<unsigned>(Recorder::WatchCaller::Count); ++caller) {
            const Recorder::WatchScope scope(static_cast<Recorder::WatchCaller>(caller));
            memory[0] = 1;
            memory[4096] = 2;
            void* pages[16]{};
            ULONG_PTR count = 16;
            DWORD granularity = 0;
            require(GuestArena::GuestArenaCensusGetWriteWatch_nid_no_patch(WRITE_WATCH_FLAG_RESET, base, 65536, pages, &count, &granularity) == 0 && count == 2, "query/reset changed dirty results");
            count = 16;
            require(GuestArena::GuestArenaCensusGetWriteWatch_nid_no_patch(0, base, 65536, pages, &count, &granularity) == 0 && count == 0, "reset left dirty pages");
            memory[8192] = 3;
            count = 16;
            require(GuestArena::GuestArenaCensusGetWriteWatch_nid_no_patch(0, base, 65536, pages, &count, &granularity) == 0 && count == 1, "query missed a plain store");
            require(GuestArena::GuestArenaCensusResetWriteWatch_nid_no_patch(base, 65536) == 0, "explicit reset failed");
            MEMORY_BASIC_INFORMATION info{};
            require(GuestArena::GuestArenaCensusVirtualQuery_nid_no_patch(base, &info, sizeof(info)) == sizeof(info), "page query failed");
            DWORD previous = 0;
            require(GuestArena::GuestArenaCensusVirtualProtect_nid_no_patch(base, 4096, PAGE_READONLY, &previous), "page protection failed");
            require(GuestArena::GuestArenaCensusVirtualProtect_nid_no_patch(base, 4096, previous, &previous), "page restoration failed");
            {
                const GuestArena::CensusTimer outer(GuestArena::CensusOperation::Collection, 16);
                const GuestArena::CensusTimer inner(GuestArena::CensusOperation::PageStamps, 2);
                std::this_thread::sleep_for(1ms);
            }
        }
        require(VirtualFree(base, 0, MEM_RELEASE), "fixture allocation release failed");
    };
    std::thread graphics([&] { exercise("queue worker 0x0"); });
    std::thread compute([&] { exercise("queue worker 0x40"); });
    graphics.join();
    compute.join();
    std::thread unbound([] {
        void* pages[16]{};
        ULONG_PTR count = 16;
        DWORD granularity = 0;
        require(GuestArena::GuestArenaCensusGetWriteWatch_nid_no_patch(0, reinterpret_cast<void*>(4096), 4096, pages, &count, &granularity) != 0, "invalid write-watch region succeeded");
    });
    unbound.join();
    Recorder::BindThread("precision");
    { const GuestArena::CensusTimer count(GuestArena::CensusOperation::PageStamps, 123456789); }
    const auto result = Recorder::Collect(Recorder::Clock::now());
    if (!active) {
        for (const auto& [name, value] : result.counters) require(!name.starts_with("ww."), "disabled census published counters");
        return;
    }
    require(result.Json().find("\"ww.other.page_stamps.pages\":123456789") != std::string::npos, "large census count lost precision in JSON");
    near(result.threadCounters.at("unbound").at("ww.other.query.calls"), 1);
    near(result.threadCounters.at("unbound").at("ww.other.query.errors"), 1);
    require(!result.threadCounters.at("unbound").contains("ww.other.query.dirty"), "failed query counted input capacity as dirty pages");
    for (const auto* role : {"queue worker 0x0", "queue worker 0x40"}) {
        const auto& rows = result.threadCounters.at(role);
        for (const auto* caller : {"other", "prelock_targets", "prelock_surfaces", "target_collection", "snapshot_collection", "proofs", "dispatch", "uploads", "presents"}) {
            const auto prefix = std::string("ww.") + caller + ".";
            near(rows.at(prefix + "query_reset.calls"), 1);
            near(rows.at(prefix + "query_reset.pages"), 16);
            near(rows.at(prefix + "query_reset.dirty"), 2);
            near(rows.at(prefix + "query.calls"), 2);
            near(rows.at(prefix + "query.pages"), 32);
            near(rows.at(prefix + "query.dirty"), 1);
            near(rows.at(prefix + "reset.calls"), 1);
            near(rows.at(prefix + "reset.pages"), 16);
            near(rows.at(prefix + "virtual_query.calls"), 1);
            near(rows.at(prefix + "virtual_protect.calls"), 2);
            near(rows.at(prefix + "collection.self_ms") + rows.at(prefix + "page_stamps.self_ms"), rows.at(prefix + "collection.inclusive_ms"));
            require(rows.at(prefix + "query_reset.inclusive_ms") > 0, "OS timer did not record");
        }
    }
    Recorder::Boundary(false, 1, Recorder::Clock::now());
    require(GuestArena::GuestArenaCensusCallback_nid_no_patch() == nullptr, "recorder toggle left census active");
    exercise("disabled");
    for (const auto& [name, value] : Recorder::Collect(Recorder::Clock::now()).counters) if (name.starts_with("ww.")) near(value, 0);
    require(GuestArena::CensusTimer::Pages(4095, 2) == 2 && GuestArena::CensusTimer::Pages(4096, 4096) == 1, "unaligned page accounting failed");
#else
    static_cast<void>(recorder);
    static_cast<void>(census);
#endif
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
        else if (name == "refresh_census") refreshCensus(true);
        else if (name == "refresh_census_disabled") refreshCensus(false);
        else if (name == "write_watch_census") writeWatchCensus(true, true);
        else if (name == "write_watch_no_recorder") writeWatchCensus(false, true);
        else if (name == "write_watch_no_switch") writeWatchCensus(true, false);
        else if (name == "hit_census") hitCensus(true);
        else if (name == "hit_census_disabled") hitCensus(false);
        else throw std::runtime_error("unknown test case");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
