#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/CommitCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"
#include <algorithm>
#include <array>
#include <exception>

namespace AgcDriver::DriverDetail {
namespace {
struct CurrentCommitSlot {};
using CurrentCommit = HostThreadSlot<CommitCensus*, CurrentCommitSlot>;
struct Names { const char* count; const char* elapsed; const char* mutex; const char* gpu; };
constexpr std::array<Names, static_cast<std::size_t>(CommitCensus::Kind::Count)> names{{
    {"j3c_replayed_count", "j3c_replayed_commit_ns", "j3c_replayed_mutex_ns", "j3c_replayed_gpu_wait_ns"},
    {"j3c_legacy_count", "j3c_legacy_commit_ns", "j3c_legacy_mutex_ns", "j3c_legacy_gpu_wait_ns"},
    {"j3c_recipe_count", "j3c_recipe_commit_ns", "j3c_recipe_mutex_ns", "j3c_recipe_gpu_wait_ns"},
    {"j3c_label_count", "j3c_label_commit_ns", "j3c_label_mutex_ns", "j3c_label_gpu_wait_ns"},
    {"j3c_dispatch_count", "j3c_dispatch_commit_ns", "j3c_dispatch_mutex_ns", "j3c_dispatch_gpu_wait_ns"},
    {"j3c_copy_count", "j3c_copy_commit_ns", "j3c_copy_mutex_ns", "j3c_copy_gpu_wait_ns"},
    {"j3c_fill_count", "j3c_fill_commit_ns", "j3c_fill_mutex_ns", "j3c_fill_gpu_wait_ns"},
    {"j3c_other_count", "j3c_other_commit_ns", "j3c_other_mutex_ns", "j3c_other_gpu_wait_ns"},
}};
}

CommitCensus::CommitCensus(Kind kind) : kind(kind) {
    enabled = ArchB::Switches::Enabled(ArchB::Feature::DrawHitCensus) && ArchB::Recorder::Enabled();
    if (!enabled) return;
    exceptions = std::uncaught_exceptions();
    previous = CurrentCommit::Get();
    CurrentCommit::Set(this);
    previousMutexCensus = GuestMemory::SetGpuMutexCensusForThread(true);
    mutexNs = GuestMemory::ThreadGpuMutexAcquireNs();
    gpuWaitMs = Graphics::Recorder::ThreadWaitedMs();
    start = std::chrono::steady_clock::now();
}

CommitCensus::~CommitCensus() { Finish(std::uncaught_exceptions() > exceptions); }

void CommitCensus::Finish(bool failed) {
    if (!enabled) return;
    const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    const auto mutex = GuestMemory::ThreadGpuMutexAcquireNs() - mutexNs;
    const auto gpu = std::max(0.0, Graphics::Recorder::ThreadWaitedMs() - gpuWaitMs) * 1000000.0;
    CurrentCommit::Set(previous);
    GuestMemory::SetGpuMutexCensusForThread(previousMutexCensus);
    enabled = false;
    const auto& row = names[static_cast<std::size_t>(kind)];
    ArchB::Recorder::Count(row.count);
    ArchB::Recorder::Count(row.elapsed, elapsed);
    ArchB::Recorder::Count(row.mutex, static_cast<double>(mutex));
    ArchB::Recorder::Count(row.gpu, gpu);
    if (failed) ArchB::Recorder::Count("j3c_failed_commits");
}

void CommitCensus::SetKind(Kind kind) {
    if (auto* current = CurrentCommit::Get()) current->kind = kind;
}

bool CommitCensus::LabelActive() {
    const auto* current = CurrentCommit::Get();
    return current != nullptr && current->kind == Kind::Label;
}

}
