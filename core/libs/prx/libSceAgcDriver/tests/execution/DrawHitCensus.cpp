#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/CommitCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawHitCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using namespace AgcDriver;
using namespace AgcDriver::DriverDetail;
using Graphics::Require;

struct Fixture {
    std::shared_ptr<DrawEntry> entry = std::make_shared<DrawEntry>();
    std::vector<DrawProgram> programs{1};
    std::vector<std::shared_ptr<DispatchVariant>> matched{std::make_shared<DispatchVariant>()};
    std::array<std::uint32_t, 3> packet{0xc0012d00, 3, 2};
    Pm4::DrawParameters parameters{0, 3, 4, 1, 0, false};
    Graphics::State graphics{};

    Fixture() {
        graphics.stages.path = Graphics::ShaderPath::Vertex;
        programs[0].userData = {1, 2};
        matched[0]->compiled = std::make_shared<const ShaderRecompiler::RecompileResult>();
    }

    void Run(bool exact = true) {
        DrawHitCensus census(entry, true, packet, parameters, graphics, programs);
        census.Finish(exact, exact, matched);
    }

    void Seed() {
        entry->hitObservation.store(nullptr);
        Run();
        Require(entry->hitObservation.load() != nullptr, "census fixture could not record an exact observation");
        static_cast<void>(ArchB::Recorder::Collect(ArchB::Recorder::Clock::now()));
    }
};

double Count(const char* name) {
    const auto snapshot = ArchB::Recorder::Collect(ArchB::Recorder::Clock::now());
    const auto found = snapshot.counters.find(name);
    return found == snapshot.counters.end() ? 0 : found->second;
}

void CommitCensusChecks() {
    const bool enabled = DrawHitCensusEnabled();
    const auto before = GuestMemory::ThreadGpuMutexAcquireNs();
    for (std::size_t index = 0; index < static_cast<std::size_t>(CommitCensus::Kind::Count); ++index) {
        const auto kind = static_cast<CommitCensus::Kind>(index);
        CommitCensus census(kind);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(1);
        while (std::chrono::steady_clock::now() < deadline) {}
        Require(CommitCensus::LabelActive() == (enabled && kind == CommitCensus::Kind::Label), "committer census exposed the wrong active kind");
        if (kind == CommitCensus::Kind::Label) {
            std::lock_guard outer(GuestMemory::GpuMutex());
            const auto acquired = GuestMemory::ThreadGpuMutexAcquireNs();
            {
                std::lock_guard inner(GuestMemory::GpuMutex());
                Require(GuestMemory::ThreadGpuMutexAcquireNs() == acquired, "recursive GPU mutex hold counted another acquisition");
            }
        }
        census.Finish();
        census.Finish();
        Require(!CommitCensus::LabelActive(), "finished commit left a current label");
    }
    const auto snapshot = ArchB::Recorder::Collect(ArchB::Recorder::Clock::now());
    const auto count = [&](const std::string& name) { const auto found = snapshot.counters.find(name); return found == snapshot.counters.end() ? 0.0 : found->second; };
    for (const char* kind : {"replayed", "legacy", "recipe", "label", "dispatch", "copy", "fill", "other"}) {
        const auto prefix = std::string("j3c_") + kind;
        Require(count(prefix + "_count") == (enabled ? 1 : 0), "committer kind was counted twice or omitted");
        Require((count(prefix + "_commit_ns") > 0) == enabled, "committer timing ignored the census switch");
        Require(count(prefix + "_commit_ns") >= count(prefix + "_mutex_ns"), "GPU mutex acquisition exceeded the enclosing commit");
    }
    Require((count("j3c_label_mutex_ns") > 0) == enabled, "label GPU mutex acquisition timing is missing or enabled unexpectedly");
    const auto after = GuestMemory::ThreadGpuMutexAcquireNs();
    Require(enabled ? after > before : after == before, "GPU mutex acquisition accumulator ignored the census switch");
    {
        std::lock_guard lock(GuestMemory::GpuMutex());
    }
    Require(GuestMemory::ThreadGpuMutexAcquireNs() == after, "commit did not restore the disabled GPU mutex census");
    {
        CommitCensus outer(CommitCensus::Kind::Label);
        {
            CommitCensus inner(CommitCensus::Kind::Other);
            CommitCensus::SetKind(CommitCensus::Kind::Copy);
            Require(!CommitCensus::LabelActive(), "nested commit retained the outer kind");
        }
        Require(CommitCensus::LabelActive() == enabled, "nested commit did not restore its parent");
    }
    Require(Count("j3c_copy_count") == (enabled ? 1 : 0), "actual commit kind override was lost");
    try {
        CommitCensus census(CommitCensus::Kind::Label);
        throw std::runtime_error("census unwind fixture");
    } catch (const std::runtime_error&) {}
    Require(!CommitCensus::LabelActive(), "throwing commit left a current label");
    Require(Count("j3c_failed_commits") == (enabled ? 1 : 0), "failed commit was not counted");
}

void InputsRefuse() {
    Fixture fixture;
    fixture.Seed();
    fixture.Run();
    Require(Count("j3_prediction_verified") == 1, "unchanged hit was not checked against the old path");
    fixture.Seed();
    ++fixture.packet[1];
    fixture.Run();
    Require(Count("j3_refused_packet") == 1, "changed packet was predicted unchanged");
    fixture.Seed();
    ++fixture.parameters.indexAddress;
    fixture.Run();
    Require(Count("j3_refused_arguments") == 1, "changed buffer argument was predicted unchanged");
    fixture.Seed();
    ++fixture.programs[0].userData[0];
    fixture.Run();
    Require(Count("j3_refused_user_words") == 1, "changed user words were predicted unchanged");
    fixture.Seed();
    fixture.Run(false);
    Require(Count("j3_prediction_mismatch") == 1, "old-path disagreement was not counted");
}

#ifdef _WIN32
void MemoryRefuses() {
    constexpr std::size_t size = 65536;
    auto* memory = static_cast<std::uint32_t*>(GuestArena::GuestArenaAllocate_nid_postfix(size, size));
    Require(memory != nullptr, "cannot allocate watched census fixture");
    GuestArena::GuestArenaCommit_nid_postfix(memory, size, PAGE_READWRITE, size);
    std::memset(memory, 0, size);
    GuestAllocations::Mutation().Add(memory, size, true, true, true);
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(memory));
    Fixture fixture;
    fixture.matched[0]->runs = {{address, address + 8}, {address + 64, address + 80}, {address + 128, address + 132}};
    const auto seed = [&] {
        GuestMemory::BumpCollectEpoch();
        Require(GuestMemory::CollectWrites(address, size) != 0, "census fixture is not tracked");
        GuestArena::GuestWtFaultTrack_nid_no_patch(static_cast<std::uintptr_t>(address), size);
        GuestMemory::WtFaultFlip(false);
        Require(GuestMemory::FaultUnchangedSince(address, size, GuestMemory::FaultClock()), "census fixture was not armed");
        fixture.Seed();
    };
    seed();
    fixture.Run();
    Require(Count("j3_prediction_verified") == 1, "armed captured memory did not predict a hit");
    seed();
    memory[32] = 1;
    fixture.Run(false);
    Require(Count("j3_refused_cpu_written") == 1, "captured-memory mutation was not refused");
    seed();
    const std::uint64_t movedPointer = address + 256;
    std::memcpy(memory, &movedPointer, sizeof(movedPointer));
    fixture.Run(false);
    Require(Count("j3_refused_cpu_written") == 1, "nested-pointer mutation was not refused");
    seed();
    memory[16] += 16;
    fixture.Run(false);
    Require(Count("j3_refused_cpu_written") == 1, "descriptor buffer-base mutation was not refused");
    seed();
    GuestMemory::MarkWritten(address + 128, 4);
    fixture.Run(false);
    Require(Count("j3_refused_stored") == 1, "driver-only write stamp was not refused");
    seed();
    deferredLabels().labels.push_back({address, 4, {}});
    fixture.Run(false);
    deferredLabels().labels.clear();
    Require(Count("j3_refused_labels") == 1, "queued label was not refused");
    seed();
    GuestAllocations::Mutation().Remove(memory);
    GuestAllocations::Mutation().Add(memory, size, true, true, true);
    fixture.Run(false);
    Require(Count("j3_refused_forget") == 1, "registered-range replacement was not refused");
}
#endif

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        ArchB::Switches::Initialize();
        ArchB::Recorder::Boundary(true, 0, ArchB::Recorder::Clock::now());
        CommitCensusChecks();
        if (!DrawHitCensusEnabled()) {
            Fixture fixture;
            fixture.Run();
            Require(fixture.entry->hitObservation.load() == nullptr, "disabled census stored an observation");
            std::cout << "Disabled draw-hit census leaves no observation\n";
            return 0;
        }
        InputsRefuse();
#ifdef _WIN32
        MemoryRefuses();
#endif
        std::cout << "Draw-hit census input, memory and reference-disagreement checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
