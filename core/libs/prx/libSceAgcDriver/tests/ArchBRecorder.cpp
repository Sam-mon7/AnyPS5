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

void targetsCensus(bool enabled) {
    const Recorder::Clock::time_point start{};
    Recorder::BindThread("h2 fixture");
    Recorder::Boundary(enabled, 800, start);
    {
        Recorder::Draw draw(Recorder::DrawOutcome::Hit);
        require(Recorder::TargetDraw() == nullptr, "target work escaped its scope");
        Recorder::Scope outer, collect;
        outer.Begin("H2.TargetRefresh", start);
        require((Recorder::TargetDraw() != nullptr) == enabled, "target scope ignored recorder gating");
        if (auto* census = Recorder::TargetDraw()) census->Count(Recorder::Metric::TargetRefresh);
        collect.Measure(Recorder::Metric::TargetCollectNs);
        collect.Begin("H2.TargetCollect", start + 10ms);
        if (auto* census = Recorder::TargetDraw()) census->Count(Recorder::Metric::TargetCollect);
        collect.End(start + 30ms);
        outer.Measure(Recorder::Metric::TargetProofNs);
        outer.Mark("proof", start + 40ms);
        outer.Measure(Recorder::Metric::TargetKeysNs);
        outer.End(start + 50ms);
        require(Recorder::TargetDraw() == nullptr, "target work survived scope end");
    }
    const auto result = Recorder::Collect(start + 1s);
    if (!enabled) {
        require(!result.counters.contains("H1.Hit.target_refresh"), "disabled targets census published data");
        return;
    }
    const auto& values = result.threadCounters.at("h2 fixture");
    near(values.at("H1.Hit.target_refresh"), 1);
    near(values.at("H1.Hit.target_collect"), 1);
    near(values.at("H1.Hit.target_collect_ns"), 20000000);
    near(values.at("H1.Hit.target_proof_ns"), 20000000);
    near(values.at("H1.Hit.target_keys_ns"), 10000000);
    near(result.phases.at("h2 fixture").at("H1.Hit.Outside.targets_pass"), 50);
    near(result.accountedMs, 50);
    Recorder::Boundary(false, 801, start + 2s);
    {
        Recorder::Draw draw(Recorder::DrawOutcome::Hit);
        Recorder::Timer timer("H2.TargetRefresh", Recorder::Metric::TargetRefreshNs);
        require(Recorder::TargetDraw() == nullptr, "disabled interval retained a target scope");
    }
    Recorder::Boundary(true, 802, start + 3s);
    {
        Recorder::Draw draw(Recorder::DrawOutcome::Hit);
        Recorder::Scope scope;
        scope.Measure(Recorder::Metric::TargetProofNs);
        scope.Begin("H2.TargetRefresh", start + 3s);
        scope.End(start + 3s + 10ms);
    }
    near(Recorder::Collect(start + 4s).counters.at("H1.Hit.target_proof_ns"), 10000000);
}

void mutexPhases() {
    const Recorder::Clock::time_point start{};
    Recorder::Boundary(true, 900, start);
    Recorder::BindThread("mutex phases");
    require(Recorder::MutexCensusEnabled(), "mutex census is not enabled");
    Recorder::Scope outer, inner;
    outer.Begin("outer", start);
    Recorder::GpuLock(true, start + 10ms);
    inner.Begin("inner", start + 20ms);
    inner.End(start + 40ms);
    Recorder::GpuLock(false, start + 70ms);
    outer.End(start + 100ms);
    const auto result = Recorder::Collect(start + 100ms);
    near(result.counters.at("M2.Held.outer_ms"), 40);
    near(result.counters.at("M2.Held.inner_ms"), 20);
    near(result.accountedMs, 100);
    Recorder::Boundary(false, 901, start + 200ms);
    require(!Recorder::MutexCensusEnabled(), "mutex census escaped recorder gate");
    Recorder::MutexTimer off("disabled");
    off.Finish();
    require(!Recorder::Collect(start + 300ms).phases["mutex phases"].contains("disabled"), "disabled timer published");
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

void ownershipClassification() {
    using D2 = AgcDriver::ArchB::OwnershipCensus;
    D2::Summary requested, observed;
    requested.complete = observed.complete = true;
    const D2::Access buffer{.begin = 100, .end = 200, .backing = 1, .epoch = 7, .resource = D2::Resource::Buffer, .flags = D2::Read, .complete = true};
    requested.Add(buffer, {0, 1});
    auto producer = buffer;
    producer.flags = D2::Write;
    observed.Add(producer, {0x28, 2});
    require(D2::Classify(requested, observed).conflict == D2::Conflict::Raw, "RAW classification");
    requested.accesses[0].access.flags = D2::Write;
    observed.accesses[0].access.flags = D2::Read;
    require(D2::Classify(requested, observed).conflict == D2::Conflict::War, "WAR classification");
    observed.accesses[0].access.flags = D2::Write;
    require(D2::Classify(requested, observed).conflict == D2::Conflict::Waw, "WAW classification");
    observed.accesses[0].access.begin = 200;
    observed.accesses[0].access.end = 300;
    require(D2::Classify(requested, observed).conflict == D2::Conflict::Independent, "disjoint qualified ranges");
    observed.accesses[0].access.backing = 0;
    require(D2::Classify(requested, observed).conflict == D2::Conflict::Unknown, "different guest addresses do not establish backing independence");
    observed.accesses[0].access.backing = 1;
    observed.accesses[0].access.epoch = 8;
    require(D2::Classify(requested, observed).conflict == D2::Conflict::Unknown, "mapping reuse is not independent backing");
    observed.accesses[0].access = producer;
    observed.accesses[0].access.resource = D2::Resource::Bda;
    require(D2::Classify(requested, observed).conflict == D2::Conflict::Unknown, "BDA remains unknown even with a mapping identity");
    observed.accesses[0].access.complete = false;
    require(D2::Classify(requested, observed).conflict == D2::Conflict::Unknown, "a leased heap is not a known WAR or WAW footprint");
    observed.accesses[0].access = producer;
    observed.accesses[0].access.flags |= D2::Label | D2::Host;
    auto result = D2::Classify(requested, observed);
    require(result.conflict == D2::Conflict::Label && result.tags == ((1u << static_cast<unsigned>(D2::Conflict::Label)) | (1u << static_cast<unsigned>(D2::Conflict::Host)) | (1u << static_cast<unsigned>(D2::Conflict::Waw))), "exclusive precedence with overlapping tags");
    require(result.queue == 0x28, "producer queue retained");
    observed.accesses[0].access.flags &= ~D2::Label;
    require(D2::Classify(requested, observed).conflict == D2::Conflict::Host, "host publication classification");
    requested.accesses[0].access = {.begin = 100, .end = 150, .representation = 9, .layout = 1, .mip = 2, .resource = D2::Resource::Image, .flags = D2::Read | D2::Layout};
    observed.accesses[0].access = {.begin = 125, .end = 200, .representation = 9, .layout = 2, .mip = 2, .resource = D2::Resource::Image, .flags = D2::Write | D2::Layout};
    require(D2::Classify(requested, observed).conflict == D2::Conflict::Layout, "storage to sampling layout transition");
    observed.accesses[0].access.representation = 10;
    require(D2::Classify(requested, observed).conflict == D2::Conflict::Raw, "partial image alias still overlaps guest backing");
    observed.accesses[0].access.begin = 200;
    observed.accesses[0].access.end = 250;
    require(D2::Classify(requested, observed).conflict == D2::Conflict::Unknown, "depth storage alias provenance is unknown");
    requested.complete = false;
    observed.accesses[0].access = producer;
    requested.accesses[0].access = buffer;
    require(D2::Classify(requested, observed).conflict == D2::Conflict::Raw, "known conflict survives unknown extra coverage");
    requested.accesses[0].access.begin = 250;
    requested.accesses[0].access.end = 300;
    require(D2::Classify(requested, observed).conflict == D2::Conflict::Unknown, "incomplete prelock request never independent");
    requested.complete = observed.complete = true;
    requested.accesses[0].access = buffer;
    observed.accesses[0].access = producer;
    observed.accesses[0].access.begin = 250;
    observed.accesses[0].access.end = 300;
    requested.overflow = true;
    require(D2::Classify(requested, observed).conflict == D2::Conflict::Unknown, "overflow never qualifies as independent");
}

void ownershipLifecycle() {
    using D2 = AgcDriver::ArchB::OwnershipCensus;
    require(D2::Enabled(), "ownership census disabled");
    Recorder::Boundary(true, 0, Recorder::Clock::now());
    const auto batch = D2::Open(55);
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 2> writes{{{100, 200}, {300, 400}}};
    const std::array<std::uint64_t, 2> notes{17, 18};
    {
        D2::Command command(0x20, true);
        D2::Observe(batch, {.begin = 100, .end = 200, .resource = D2::Resource::Buffer, .flags = D2::Write}, 17);
    }
    {
        D2::Command command(0x28, true);
        D2::Observe(batch, {.begin = 300, .end = 400, .resource = D2::Resource::Label, .flags = D2::Write | D2::Label}, 18);
    }
    const auto a = D2::Inspect(batch, 0), b = D2::Inspect(batch, 1);
    require(a.origin.queue == 0x20 && b.origin.queue == 0x28 && a.origin.command != b.origin.command, "mixed batch command origins");
    require(a.origin.serial == 0 && b.origin.serial == 0 && a.origin.batch == b.origin.batch && a.origin.recorder == 55 && a.origin.note == 17, "open access has no submitted serial");
    D2::Check(batch, writes, notes, writes, true);
    D2::Transition(batch, D2::Life::Submitted, 9);
    require(D2::Inspect(batch, 0).origin.serial == 9 && D2::Inspect(batch, 1).origin.serial == 9, "actual submitted serial installed per command");
    D2::Transition(batch, D2::Life::Finishing);
    std::thread callback([&] {
        D2::Check(batch, writes, notes, writes, true);
        std::this_thread::sleep_for(2ms);
        D2::Check(batch, writes, notes, writes, true);
    });
    callback.join();
    D2::Transition(batch, D2::Life::Published);
    D2::Check(batch, writes, notes, {}, true);
    D2::Retire(batch);
    const auto reused = D2::Open(55);
    require(reused != batch, "batch identity survives pool reuse");
    D2::Transition(reused, D2::Life::Discarded);
    D2::Retire(reused);
    const auto bda = D2::Open(55);
    for (unsigned commandIndex = 0; commandIndex < 2; ++commandIndex) {
        D2::Command command(0x28, true);
        for (unsigned range = 0; range < 2048; ++range) D2::Observe(bda, {.begin = range * 4096ull, .end = (range + 1) * 4096ull, .resource = D2::Resource::Bda, .flags = D2::Read});
    }
    require(D2::Inspect(bda, 0).access.end == 0 && D2::Inspect(bda, 1).access.end == 0, "BDA lease coverage remains unknown without filling the ledger");
    require(D2::Inspect(bda, 0).origin.command != D2::Inspect(bda, 1).origin.command, "compressed unknown coverage preserves per-command origins");
    D2::Transition(bda, D2::Life::Discarded);
    D2::Retire(bda);
    {
        D2::Command request(0);
        request.Requested()->Add({.begin = 300, .end = 400, .resource = D2::Resource::Buffer, .flags = D2::Read});
        const auto start = Recorder::Clock::now();
        D2::HoldBegin(0x28, true, start);
        const auto waiter = D2::WaitBegin(start + 10ns);
        request.Requested()->Add({.begin = 100, .end = 200, .resource = D2::Resource::Buffer, .flags = D2::Read});
        const auto late = D2::Open(55);
        {
            D2::Command producer(0x28, true);
            D2::Observe(late, {.begin = 100, .end = 200, .resource = D2::Resource::Buffer, .flags = D2::Write});
        }
        D2::HoldEnd(start + 20ns);
        D2::WaitEnd(waiter, start + 30ns, 10);
        D2::Transition(late, D2::Life::Discarded);
        D2::Retire(late);
    }
    const auto result = Recorder::Collect(Recorder::Clock::now());
    near(result.counters.at("d2_unknown_ns"), 10);
    near(result.counters.at("d2_conflict_raw_ns"), 0);
    near(result.counters.at("d2_mixed_batches"), 1);
    near(result.counters.at("d2_pending_shadow_mismatches"), 0);
    near(result.counters.at("d2_early_retirement"), 0);
    near(result.counters.at("d2_overflow"), 0);
    std::cout << result.Json() << '\n';
}

}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected one test case");
        const std::string_view name = argv[1];
        if (name == "ownership_classification") ownershipClassification();
        else if (name == "ownership_lifecycle") ownershipLifecycle();
        else if (name == "mutex_phases") mutexPhases();
        else if (name == "reconciles") reconciles();
        else if (name == "no_path_change") noPathChange();
        else if (name == "toggle_interval_tagged") toggleIntervalTagged();
        else if (name == "shader_table_overflow") shaderTableOverflow();
        else if (name == "hit_census") hitCensus(true);
        else if (name == "hit_census_disabled") hitCensus(false);
        else if (name == "targets_census") targetsCensus(true);
        else if (name == "targets_census_disabled") targetsCensus(false);
        else throw std::runtime_error("unknown test case");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
