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

constexpr const char* transitions[8][8]{
    {"j3t_replayed_to_replayed_count", "j3t_replayed_to_legacy_count", "j3t_replayed_to_recipe_count", "j3t_replayed_to_label_count", "j3t_replayed_to_dispatch_count", "j3t_replayed_to_copy_count", "j3t_replayed_to_fill_count", "j3t_replayed_to_other_count"},
    {"j3t_legacy_to_replayed_count", "j3t_legacy_to_legacy_count", "j3t_legacy_to_recipe_count", "j3t_legacy_to_label_count", "j3t_legacy_to_dispatch_count", "j3t_legacy_to_copy_count", "j3t_legacy_to_fill_count", "j3t_legacy_to_other_count"},
    {"j3t_recipe_to_replayed_count", "j3t_recipe_to_legacy_count", "j3t_recipe_to_recipe_count", "j3t_recipe_to_label_count", "j3t_recipe_to_dispatch_count", "j3t_recipe_to_copy_count", "j3t_recipe_to_fill_count", "j3t_recipe_to_other_count"},
    {"j3t_label_to_replayed_count", "j3t_label_to_legacy_count", "j3t_label_to_recipe_count", "j3t_label_to_label_count", "j3t_label_to_dispatch_count", "j3t_label_to_copy_count", "j3t_label_to_fill_count", "j3t_label_to_other_count"},
    {"j3t_dispatch_to_replayed_count", "j3t_dispatch_to_legacy_count", "j3t_dispatch_to_recipe_count", "j3t_dispatch_to_label_count", "j3t_dispatch_to_dispatch_count", "j3t_dispatch_to_copy_count", "j3t_dispatch_to_fill_count", "j3t_dispatch_to_other_count"},
    {"j3t_copy_to_replayed_count", "j3t_copy_to_legacy_count", "j3t_copy_to_recipe_count", "j3t_copy_to_label_count", "j3t_copy_to_dispatch_count", "j3t_copy_to_copy_count", "j3t_copy_to_fill_count", "j3t_copy_to_other_count"},
    {"j3t_fill_to_replayed_count", "j3t_fill_to_legacy_count", "j3t_fill_to_recipe_count", "j3t_fill_to_label_count", "j3t_fill_to_dispatch_count", "j3t_fill_to_copy_count", "j3t_fill_to_fill_count", "j3t_fill_to_other_count"},
    {"j3t_other_to_replayed_count", "j3t_other_to_legacy_count", "j3t_other_to_recipe_count", "j3t_other_to_label_count", "j3t_other_to_dispatch_count", "j3t_other_to_copy_count", "j3t_other_to_fill_count", "j3t_other_to_other_count"},
};
struct LabelPredecessorNames { const char* readyCount; const char* readyMutex; const char* gapCount; const char* gapMutex; };
constexpr std::array<LabelPredecessorNames, 9> labelPredecessors{{
    {"j3t_label_prev_replayed_ready_count", "j3t_label_prev_replayed_ready_mutex_ns", "j3t_label_prev_replayed_gap_count", "j3t_label_prev_replayed_gap_mutex_ns"},
    {"j3t_label_prev_legacy_ready_count", "j3t_label_prev_legacy_ready_mutex_ns", "j3t_label_prev_legacy_gap_count", "j3t_label_prev_legacy_gap_mutex_ns"},
    {"j3t_label_prev_recipe_ready_count", "j3t_label_prev_recipe_ready_mutex_ns", "j3t_label_prev_recipe_gap_count", "j3t_label_prev_recipe_gap_mutex_ns"},
    {"j3t_label_prev_label_ready_count", "j3t_label_prev_label_ready_mutex_ns", "j3t_label_prev_label_gap_count", "j3t_label_prev_label_gap_mutex_ns"},
    {"j3t_label_prev_dispatch_ready_count", "j3t_label_prev_dispatch_ready_mutex_ns", "j3t_label_prev_dispatch_gap_count", "j3t_label_prev_dispatch_gap_mutex_ns"},
    {"j3t_label_prev_copy_ready_count", "j3t_label_prev_copy_ready_mutex_ns", "j3t_label_prev_copy_gap_count", "j3t_label_prev_copy_gap_mutex_ns"},
    {"j3t_label_prev_fill_ready_count", "j3t_label_prev_fill_ready_mutex_ns", "j3t_label_prev_fill_gap_count", "j3t_label_prev_fill_gap_mutex_ns"},
    {"j3t_label_prev_other_ready_count", "j3t_label_prev_other_ready_mutex_ns", "j3t_label_prev_other_gap_count", "j3t_label_prev_other_gap_mutex_ns"},
    {"j3t_label_prev_unknown_ready_count", "j3t_label_prev_unknown_ready_mutex_ns", "j3t_label_prev_unknown_gap_count", "j3t_label_prev_unknown_gap_mutex_ns"},
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
    else { resultKind = kind; resultMutexNs = mutex; }
}

void CommitCensus::RecordSequence(Sequence& sequence, std::uint8_t arrival, bool onlyItem, bool failed) {
    if (sequenceRecorded) return;
    sequenceRecorded = true;
    if (failed || resultKind == Kind::Count) {
        sequence.predecessor = Kind::Count;
        return;
    }
    const auto predecessor = static_cast<std::size_t>(sequence.predecessor);
    const auto current = static_cast<std::size_t>(resultKind);
    if (sequence.predecessor == Kind::Count) ArchB::Recorder::Count("j3t_unknown_predecessor_count");
    else ArchB::Recorder::Count(transitions[predecessor][current]);
    if (resultKind == Kind::Label) {
        const auto& row = labelPredecessors[predecessor];
        ArchB::Recorder::Count(sequence.emptyAfterPredecessor ? row.gapCount : row.readyCount);
        ArchB::Recorder::Count(sequence.emptyAfterPredecessor ? row.gapMutex : row.readyMutex, static_cast<double>(resultMutexNs));
        constexpr std::array<const char*, 3> arrivalCounts{"j3t_label_arrival_unknown_count", "j3t_label_arrival_empty_count", "j3t_label_arrival_occupied_count"};
        constexpr std::array<const char*, 3> arrivalMutex{"j3t_label_arrival_unknown_mutex_ns", "j3t_label_arrival_empty_mutex_ns", "j3t_label_arrival_occupied_mutex_ns"};
        ArchB::Recorder::Count(arrivalCounts.at(arrival));
        ArchB::Recorder::Count(arrivalMutex.at(arrival), static_cast<double>(resultMutexNs));
        ArchB::Recorder::Count(onlyItem ? "j3t_label_take_only_count" : "j3t_label_take_more_count");
        ArchB::Recorder::Count(onlyItem ? "j3t_label_take_only_mutex_ns" : "j3t_label_take_more_mutex_ns", static_cast<double>(resultMutexNs));
    }
    sequence.predecessor = resultKind;
}

void CommitCensus::SetKind(Kind kind) {
    if (auto* current = CurrentCommit::Get()) current->kind = kind;
}

bool CommitCensus::LabelActive() {
    const auto* current = CurrentCommit::Get();
    return current != nullptr && current->kind == Kind::Label;
}

}
