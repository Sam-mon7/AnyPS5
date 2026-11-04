#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/DrawWriteRanges.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/BufferCopy.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/HandoffSpin.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/Stage4Diag.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace AgcDriver::Graphics {
// APS5_DRAW_BINDINGS_DIAG (Draw.cpp): the committer's lookup steps; empty when off.
std::string DrawLookupDiagText();
// APS5_ARCHB_PIPE_DRAW_LOOKUP_MEMO (Draw.cpp): the committer's template memo counters.
std::string DrawLookupMemoText();
// APS5_ARCHB_PIPE_DRAW_LOOKUP_MEMO (Pipeline.cpp): the committer's pipeline memo counters.
std::string PipelineLookupMemoText();
}
#include <string>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::DrainReason::Count)> DrainNames{"packet", "flush", "labels", "capture", "indirect", "submission", "transition"};
constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::DrainReason::Count)> DrainCounterNames{"pipe_drains_packet", "pipe_drains_flush", "pipe_drains_labels", "pipe_drains_capture", "pipe_drains_indirect", "pipe_drains_submission", "pipe_drains_transition"};
constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::DrainCause::Count)> CauseNames{"none", "indirect_args", "indirect_args_draw", "indirect_args_dispatch", "indirect_args_label", "indirect_args_other", "indirect_count", "indirect_disabled", "indirect_cpu_records", "indirect_inline", "regs_overlap", "regs_invalid", "wait_held_mismatch", "wait_overlap", "wait_unsatisfied", "dispatch_ahead", "dispatch_items_off", "dispatch_indirect", "dispatch_labels", "dispatch_copy", "dispatch_miss", "dispatch_verify", "dispatch_reads", "dispatch_no_recipe", "dispatch_writes", "dispatch_gate", "fill_inputs", "fills_unordered", "metadata_pass", "capture_variant", "capture_pending", "capture_shader", "indirect_cpu_read", "indirect_empty_skip"};
constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::DrainCause::Count)> CauseCounterNames{"pipe_drain_cause_none", "pipe_drain_cause_indirect_args", "pipe_drain_cause_indirect_args_draw", "pipe_drain_cause_indirect_args_dispatch", "pipe_drain_cause_indirect_args_label", "pipe_drain_cause_indirect_args_other", "pipe_drain_cause_indirect_count", "pipe_drain_cause_indirect_disabled", "pipe_drain_cause_indirect_cpu_records", "pipe_drain_cause_indirect_inline", "pipe_drain_cause_regs_overlap", "pipe_drain_cause_regs_invalid", "pipe_drain_cause_wait_held_mismatch", "pipe_drain_cause_wait_overlap", "pipe_drain_cause_wait_unsatisfied", "pipe_drain_cause_dispatch_ahead", "pipe_drain_cause_dispatch_items_off", "pipe_drain_cause_dispatch_indirect", "pipe_drain_cause_dispatch_labels", "pipe_drain_cause_dispatch_copy", "pipe_drain_cause_dispatch_miss", "pipe_drain_cause_dispatch_verify", "pipe_drain_cause_dispatch_reads", "pipe_drain_cause_dispatch_no_recipe", "pipe_drain_cause_dispatch_writes", "pipe_drain_cause_dispatch_gate", "pipe_drain_cause_fill_inputs", "pipe_drain_cause_fills_unordered", "pipe_drain_cause_metadata_pass", "pipe_drain_cause_capture_variant", "pipe_drain_cause_capture_pending", "pipe_drain_cause_capture_shader", "pipe_drain_cause_indirect_cpu_read", "pipe_drain_cause_indirect_empty_skip"};
constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::ItemKind::Count)> KindNames{"other", "draw", "dispatch", "label", "dma", "fill"};
constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::ItemKind::Count)> CommitCounterNames{"pipe_commits_other", "pipe_commits_draw", "pipe_commits_dispatch", "pipe_commits_label", "pipe_commits_dma", "pipe_commits_fill"};
// Depth when APS5_ARCHB_PIPE_Q0_COMMITTER pipelines queue 0 (APS5_PIPELINED_DRAWS=1 selects the same).
constexpr std::size_t SwitchDepth = 8;
// PIPE_DEEP_QUEUE: queue 0 runs further ahead of the committer (A0-A2 profiles: ~315 full waits per frame at 8).
constexpr std::size_t DeepSwitchDepth = 32;

// Cumulative pipe_* counts (never reset; the recorder takes per-window differences). The _ns sums
// also go to the recorder as pipe_*_ms / pipe_enqueue_ns at the measuring site.
struct PipeCounters {
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DrawPipeline::ItemKind::Count)> commits{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DrawPipeline::DrainReason::Count)> drains{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DrawPipeline::DrainCause::Count)> causes{};
    std::atomic<std::uint64_t> enqueues{0};
    std::atomic<std::uint64_t> submissions{0};
    std::atomic<std::uint64_t> keptIndirectRegs{0};
    std::atomic<std::uint64_t> keptIndirectArguments{0};
    std::atomic<std::uint64_t> narrowedDrawWrites{0};
    std::atomic<std::uint64_t> narrowedSlicedDepth{0};
    std::atomic<std::uint64_t> narrowedDispatchWrites{0};
    std::atomic<std::uint64_t> keptOwnLabelWaits{0};
    std::atomic<std::uint64_t> lockedDrains{0};
    std::atomic<std::uint64_t> fullWaits{0};
    std::atomic<std::uint64_t> fullOvercommits{0};
    std::atomic<std::uint64_t> drainWaitNs{0};
    std::atomic<std::uint64_t> fullWaitNs{0};
    std::atomic<std::uint64_t> idleNs{0};
    std::atomic<std::uint64_t> enqueueNs{0};
    // Collect epochs: queue-0 bumps by cause, and the epochs the committer (and finisher) began.
    std::array<std::atomic<std::uint64_t>, DrawPipeline::BumpCauses> q0Bumps{};
    std::atomic<std::uint64_t> followBumps{0};
    // APS5_COLLECT_AHEAD_CENSUS: committed draw items by enqueue-to-commit window (buckets below),
    // those whose preparation walked and their walk time; the walk time a helper starting right
    // after the enqueue could have finished in the window (window >= walks + AheadLatencyNs) with its
    // walk still standing at commit (the committer's requirement at most the clock at enqueue), and
    // the walk time lost to a short window, a later requirement (the committer's own ordering
    // points) or an unknown one.
    std::atomic<std::uint64_t> aheadDraws{0};
    std::atomic<std::uint64_t> aheadWindowNs{0};
    std::array<std::atomic<std::uint64_t>, 5> aheadWindowItems{};
    std::array<std::atomic<std::uint64_t>, 5> aheadWindowWalkItems{};
    std::array<std::atomic<std::uint64_t>, 5> aheadWindowWalkNs{};
    std::atomic<std::uint64_t> aheadFeasibleNs{0};
    std::atomic<std::uint64_t> aheadLateNs{0};
    std::atomic<std::uint64_t> aheadInvalidNs{0};
    std::atomic<std::uint64_t> aheadUnknownNs{0};
};
// Window bucket upper bounds (us): < 50, < 150, < 400, < 1000, longer.
constexpr std::array<std::uint64_t, 4> AheadWindowBoundsUs{50, 150, 400, 1000};
// A parked helper's wake-up and hand-off before it can start walking (assumed).
constexpr std::uint64_t AheadLatencyNs = 50'000;

PipeCounters& pipeCounters() {
    static PipeCounters* counters = new PipeCounters();
    return *counters;
}

bool pipelineBlocked() {
    static const bool blocked = std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr || std::getenv("APS5_DRAW_DRAIN") != nullptr || std::getenv("APS5_DRAIN_ALL") != nullptr || std::getenv("APS5_NO_WORDWISE_CAPTURE") != nullptr;
    return blocked;
}

// Runs `wait` with the calling thread's GPU-mutex hold given up at every recursion level, and takes
// the mutex back to the same depth afterwards (also when `wait` throws). Waiting for the committer
// (or the finisher) while holding the mutex would never end: they take the mutex to commit.
template <typename TWait>
void waitWithoutGpuLock(TWait&& wait) {
    auto& gpu = GuestMemory::GpuMutex();
    struct Relock {
        GuestMemory::GpuMutexType& gpu;
        std::uint32_t depth;
        ~Relock() {
            for (std::uint32_t i = 0; i < depth; ++i) gpu.lock();
        }
    } relock{gpu, gpu.DepthOnThisThread()};
    for (std::uint32_t i = 0; i < relock.depth; ++i) gpu.unlock();
    wait();
}

void noteLockedDrain(const char* who, unsigned reason, unsigned opcode) {
    const auto count = pipeCounters().lockedDrains.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((count & (count - 1)) == 0) std::fprintf(stderr, "[pipe] %s drain requested while the thread holds the GPU mutex (reason %u, opcode 0x%x; %llu so far): waited with the hold given up\n", who, reason, opcode, static_cast<unsigned long long>(count));
}

void noteOvercommit(const char* who, std::size_t queued) {
    const auto count = pipeCounters().fullOvercommits.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((count & (count - 1)) == 0) std::fprintf(stderr, "[pipe] %s full (%zu queued) while the enqueuing thread holds the GPU mutex (%llu so far): queued past the depth\n", who, queued, static_cast<unsigned long long>(count));
}

std::atomic<unsigned> runningCommitters{0};

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

std::atomic<std::uint64_t> heldReadRanges{0};
std::atomic<std::uint64_t> heldReadQueries{0};
std::atomic<std::uint64_t> heldReadHits{0};

}

namespace {
thread_local bool finisherThread = false;
thread_local bool finisherProducer = false;
}

DrawFinisher& DrawFinisher::Get() {
    static DrawFinisher* finisher = new DrawFinisher();
    return *finisher;
}

bool DrawFinisher::Enabled() {
    return Graphics::DrawPipelineEnabled();
}

bool DrawPipeline::MetadataPassesPipelined() {
    return Graphics::DrawPipelineEnabled();
}

namespace {
std::atomic<std::uint64_t> pipelinedMetadataPasses{0};
}

std::uint64_t DrawPipeline::PipelinedMetadataPasses() {
    return pipelinedMetadataPasses.load(std::memory_order_relaxed);
}

void DrawPipeline::CountPipelinedMetadataPass() {
    pipelinedMetadataPasses.fetch_add(1, std::memory_order_relaxed);
}

bool DrawFinisher::Overlap() {
    return Graphics::DrawPipelineEnabled();
}

bool DrawFinisher::PendingOverlaps(std::uint64_t begin, std::uint64_t end) {
    if (!Busy() || !finisherProducer) return false;
    std::lock_guard lock(mutex);
    for (const auto& held : pending) {
        if (held.unknown) return true;
        for (const auto& [first, last] : held.writes) {
            if (begin < last && first < end) return true;
        }
    }
    return false;
}

DrawFinisher::LabelLookup DrawFinisher::PendingLabel(std::uint64_t address, std::size_t bytes, std::uint64_t& value) {
    if (!Busy() || !finisherProducer) return LabelLookup::NotHeld;
    const auto end = address + bytes;
    std::lock_guard lock(mutex);
    for (auto held = pending.rbegin(); held != pending.rend(); ++held) {
        if (held->unknown) return LabelLookup::Unknown;
        const bool overlaps = std::any_of(held->writes.begin(), held->writes.end(), [&](const Range& range) { return address < range.second && range.first < end; });
        if (!overlaps) continue;
        if (held->labelBytes.empty() || address < held->labelAddress || end > held->labelAddress + held->labelBytes.size()) return LabelLookup::Unknown;
        value = 0;
        std::memcpy(&value, held->labelBytes.data() + (address - held->labelAddress), bytes);
        return LabelLookup::Value;
    }
    return LabelLookup::NotHeld;
}

bool DrawFinisher::PendingOverlapsAny(std::span<const Range> ranges) {
    if (!Busy() || !finisherProducer) return false;
    std::lock_guard lock(mutex);
    for (const auto& held : pending) {
        if (held.unknown) return true;
        for (const auto& [first, last] : held.writes) {
            for (const auto& [begin, end] : ranges) {
                if (begin < last && first < end) return true;
            }
        }
    }
    return false;
}

bool DrawFinisher::PendingKey(std::uint64_t key, std::uint64_t shape) {
    if (!Busy() || !finisherProducer) return false;
    std::lock_guard lock(mutex);
    return std::any_of(pending.begin(), pending.end(), [&](const Pending& held) { return held.key == key || (shape != 0 && held.shape == shape); });
}

namespace {
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DrawFinisher::DrainSite::Count)> finisherDrains{}, finisherSkips{};
}

void DrawFinisher::NoteDrain(DrainSite site, bool drained) {
    (drained ? finisherDrains : finisherSkips)[static_cast<std::size_t>(site)].fetch_add(1, std::memory_order_relaxed);
}

namespace {
std::array<std::atomic<std::uint64_t>, 257> drainedOpcodes{}, drainedBeside{};
}

void DrawFinisher::NoteDrainedOpcode(std::uint32_t opcode, bool besideCommits) {
    drainedOpcodes[std::min<std::uint32_t>(opcode, 256u)].fetch_add(1, std::memory_order_relaxed);
    if (besideCommits) drainedBeside[std::min<std::uint32_t>(opcode, 256u)].fetch_add(1, std::memory_order_relaxed);
}

std::string DrawFinisher::DrainText() {
    constexpr std::array<const char*, static_cast<std::size_t>(DrainSite::Count)> names{"lookup", "reads", "packet", "kept draw"};
    std::string text = "; worker drains of the finisher (drained/skipped):";
    for (std::size_t i = 0; i < names.size(); ++i) {
        char item[64];
        std::snprintf(item, sizeof(item), " %s %llu/%llu", names[i], static_cast<unsigned long long>(finisherDrains[i].exchange(0)), static_cast<unsigned long long>(finisherSkips[i].exchange(0)));
        text += item;
    }
    text += "; drained by opcode (of them beside commits):";
    for (int shown = 0; shown < 6; ++shown) {
        std::size_t top = 0;
        for (std::size_t i = 1; i < drainedOpcodes.size(); ++i) {
            if (drainedOpcodes[i].load() > drainedOpcodes[top].load()) top = i;
        }
        const auto count = drainedOpcodes[top].exchange(0);
        if (count == 0) break;
        char item[48];
        std::snprintf(item, sizeof(item), " 0x%02zx x%llu (%llu)", top, static_cast<unsigned long long>(count), static_cast<unsigned long long>(drainedBeside[top].exchange(0)));
        text += item;
    }
    return text;
}

bool DrawFinisher::OnFinisherThread() {
    return finisherThread;
}

void DrawFinisher::Enqueue(Job job) {
    Enqueue(std::move(job), Pending{});
}

void DrawFinisher::Enqueue(Job job, Pending held) {
    finisherProducer = true;
    constexpr std::size_t depth = 16;
    struct ReclaimStorage {};
    auto& reclaim = HostThreadLocal<std::vector<Job>, ReclaimStorage>();
    reclaim.clear();
    std::unique_lock lock(mutex);
    if (!ran.empty()) std::swap(ran, reclaim);
    if (failure != nullptr) std::rethrow_exception(std::exchange(failure, nullptr));
    if (jobs.size() >= depth) {
        // The finisher may need the GPU mutex to finish the job in front: waiting under it would never end.
        if (GuestMemory::GpuMutex().HeldByThisThread()) noteOvercommit("draw finisher", jobs.size());
        else idle.wait(lock, [&] { return jobs.size() < depth; });
    }
    jobs.push_back(std::move(job));
    pending.push_back(std::move(held));
    outstanding.fetch_add(1, std::memory_order_release);
    if (!running) {
        running = true;
        thread = std::thread([this] {
            finisherThread = true;
            run();
        });
    }
    if (sleeping) wake.notify_one();
}

void DrawFinisher::run() {
    PerformanceTrace::BindThread("draw finisher");
    for (;;) {
        Job* job = nullptr;
        bool failed = false;
        SpinUntil(HandoffSpinWindow(), [&] { return outstanding.load(std::memory_order_acquire) != 0; });
        {
            std::unique_lock lock(mutex);
            sleeping = true;
            wake.wait(lock, [&] { return !jobs.empty(); });
            sleeping = false;
            job = &jobs.front();
            failed = failure != nullptr;
        }
        std::exception_ptr error;
        try {
            if (!failed) (*job)();
        } catch (...) {
            error = std::current_exception();
        }
        std::lock_guard lock(mutex);
        if (error != nullptr && failure == nullptr) failure = error;
        ran.push_back(std::move(jobs.front()));
        jobs.pop_front();
        pending.pop_front();
        outstanding.fetch_sub(1, std::memory_order_release);
        idle.notify_all();
    }
}

void DrawFinisher::waitEmpty(bool rethrow) {
    if (rethrow) SpinUntil(HandoffSpinWindow() * 2, [&] { return outstanding.load(std::memory_order_acquire) == 0; });
    std::unique_lock lock(mutex);
    if (!jobs.empty()) idle.wait(lock, [&] { return jobs.empty(); });
    if (rethrow && failure != nullptr) std::rethrow_exception(std::exchange(failure, nullptr));
}

void DrawFinisher::Settle() {
    if (!Busy() || OnFinisherThread()) return;
    if (GuestMemory::GpuMutex().HeldByThisThread()) {
        noteLockedDrain("draw finisher settle:", 0, 0x100);
        waitWithoutGpuLock([&] { waitEmpty(false); });
        return;
    }
    waitEmpty(false);
}

void DrawFinisher::Drain() {
    if (OnFinisherThread()) return;
    if (Busy() && GuestMemory::GpuMutex().HeldByThisThread()) {
        noteLockedDrain("draw finisher", 0, 0x100);
        waitWithoutGpuLock([&] { waitEmpty(true); });
        return;
    }
    waitEmpty(true);
}

void DrawFinisher::ReleaseRan() {
    std::vector<Job> done;
    std::lock_guard lock(mutex);
    std::swap(done, ran);
}

DrawPipeline& DrawPipeline::Queue0() {
    static DrawPipeline* pipeline = new DrawPipeline();
    return *pipeline;
}

std::size_t DrawPipeline::Depth() {
    static const std::size_t depth = [] {
        const char* text = std::getenv("APS5_PIPELINED_DRAWS");
        if (pipelineBlocked()) return std::size_t{0};
        if (text == nullptr) return std::size_t{0};
        const auto value = std::strtoull(text, nullptr, 10);
        return value == 0 ? std::size_t{0} : value == 1 ? SwitchDepth : static_cast<std::size_t>(std::min<unsigned long long>(value, 256));
    }();
    return depth;
}

bool DrawPipeline::SampleQueue0(bool& viaSwitch) {
    viaSwitch = false;
    static const bool environment = std::getenv("APS5_PIPELINED_DRAWS") != nullptr;
    if (environment) return Depth() != 0;
    if (pipelineBlocked() || !ArchB::Switches::Enabled(ArchB::Feature::PipeQ0Committer)) return false;
    viaSwitch = true;
    return true;
}

std::size_t DrawPipeline::Capacity() {
    const auto depth = Depth();
    if (depth != 0) return depth;
    return ArchB::Switches::Enabled(ArchB::Feature::PipeDeepQueue) ? DeepSwitchDepth : SwitchDepth;
}

bool& DrawPipeline::DispatchItems() {
    static thread_local bool items = false;
    return items;
}

void DrawPipeline::RegisterCounters() {
    static const bool registered = [] {
        auto& counters = pipeCounters();
        for (std::size_t i = 0; i < CommitCounterNames.size(); ++i) ArchB::Recorder::RegisterCounter(CommitCounterNames[i], counters.commits[i]);
        for (std::size_t i = 0; i < DrainCounterNames.size(); ++i) ArchB::Recorder::RegisterCounter(DrainCounterNames[i], counters.drains[i]);
        for (std::size_t i = 0; i < CauseCounterNames.size(); ++i) ArchB::Recorder::RegisterCounter(CauseCounterNames[i], counters.causes[i]);
        ArchB::Recorder::RegisterCounter("pipe_enqueues", counters.enqueues);
        ArchB::Recorder::RegisterCounter("pipe_submissions", counters.submissions);
        ArchB::Recorder::RegisterCounter("pipe_keep_indirect_regs", counters.keptIndirectRegs);
        ArchB::Recorder::RegisterCounter("pipe_indirect_gpu_args_kept", counters.keptIndirectArguments);
        ArchB::Recorder::RegisterCounter("pipe_draw_writes_narrowed", counters.narrowedDrawWrites);
        ArchB::Recorder::RegisterCounter("pipe_sliced_depth_narrowed", counters.narrowedSlicedDepth);
        ArchB::Recorder::RegisterCounter("pipe_dispatch_writes_narrowed", counters.narrowedDispatchWrites);
        ArchB::Recorder::RegisterCounter("pipe_own_label_waits_kept", counters.keptOwnLabelWaits);
        ArchB::Recorder::RegisterCounter("pipe_drains_locked", counters.lockedDrains);
        ArchB::Recorder::RegisterCounter("pipe_full_waits", counters.fullWaits);
        ArchB::Recorder::RegisterCounter("pipe_full_overcommits", counters.fullOvercommits);
        static constexpr std::array<const char*, BumpCauses> bumpNames{"pipe_q0_bumps_submission", "pipe_q0_bumps_wait", "pipe_q0_bumps_drain", "pipe_q0_bumps_reap", "pipe_q0_bumps_packet"};
        for (std::size_t i = 0; i < bumpNames.size(); ++i) ArchB::Recorder::RegisterCounter(bumpNames[i], counters.q0Bumps[i]);
        ArchB::Recorder::RegisterCounter("pipe_committer_bumps", counters.followBumps);
        const auto& collects = GuestMemory::CommitterCollects();
        ArchB::Recorder::RegisterCounter("pipe_committer_collects", collects.collects);
        ArchB::Recorder::RegisterCounter("pipe_committer_collect_hits", collects.hits);
        ArchB::Recorder::RegisterCounter("pipe_committer_collect_walks", collects.walks);
        ArchB::Recorder::RegisterCounter("pipe_committer_collect_walk_pages", collects.walkPages);
        ArchB::Recorder::RegisterCounter("pipe_collect_share_covered", collects.shareCovered);
        ArchB::Recorder::RegisterCounter("pipe_collect_share_covered_pages", collects.shareCoveredPages);
        ArchB::Recorder::RegisterCounter("pipe_collect_share_partial_pages", collects.sharePartialPages);
        ArchB::Recorder::RegisterCounter("pipe_collect_share_walk_ns", collects.shareWalkNs);
        ArchB::Recorder::RegisterCounter("pipe_collect_share_covered_ns", collects.shareCoveredNs);
        ArchB::Recorder::RegisterCounter("pipe_collect_share_mismatches", collects.shareMismatches);
        ArchB::Recorder::RegisterCounter("pipe_collect_share_mismatch_pages", collects.shareMismatchPages);
        ArchB::Recorder::RegisterCounter("pipe_collect_share_memo_sampled", collects.shareMemoSampled);
        ArchB::Recorder::RegisterCounter("pipe_collect_share_memo_dirty", collects.shareMemoDirty);
        ArchB::Recorder::RegisterCounter("pipe_collect_share_recorded", collects.shareRecorded);
        ArchB::Recorder::RegisterCounter("pipe_collect_share_recorded_pages", collects.shareRecordedPages);
        ArchB::Recorder::RegisterCounter("pipe_collect_share_resets", collects.shareResets);
        if (GuestMemory::CollectAheadCensus()) {
            const auto& census = GuestMemory::CollectAheadCensusCounts();
            ArchB::Recorder::RegisterCounter("ahead_census_prelock_calls", census.prelockCalls);
            ArchB::Recorder::RegisterCounter("ahead_census_prelock_ns", census.prelockNs);
            ArchB::Recorder::RegisterCounter("ahead_census_prelock_walks", census.prelockWalks);
            ArchB::Recorder::RegisterCounter("ahead_census_prelock_walk_pages", census.prelockWalkPages);
            ArchB::Recorder::RegisterCounter("ahead_census_prelock_walk_ns", census.prelockWalkNs);
            ArchB::Recorder::RegisterCounter("ahead_census_other_calls", census.otherCalls);
            ArchB::Recorder::RegisterCounter("ahead_census_other_ns", census.otherNs);
            ArchB::Recorder::RegisterCounter("ahead_census_other_walks", census.otherWalks);
            ArchB::Recorder::RegisterCounter("ahead_census_other_walk_pages", census.otherWalkPages);
            ArchB::Recorder::RegisterCounter("ahead_census_other_walk_ns", census.otherWalkNs);
            ArchB::Recorder::RegisterCounter("ahead_census_draws", counters.aheadDraws);
            ArchB::Recorder::RegisterCounter("ahead_census_window_ns", counters.aheadWindowNs);
            static constexpr std::array<const char*, 5> itemNames{"ahead_census_win50_items", "ahead_census_win150_items", "ahead_census_win400_items", "ahead_census_win1000_items", "ahead_census_winlong_items"};
            static constexpr std::array<const char*, 5> walkItemNames{"ahead_census_win50_walk_items", "ahead_census_win150_walk_items", "ahead_census_win400_walk_items", "ahead_census_win1000_walk_items", "ahead_census_winlong_walk_items"};
            static constexpr std::array<const char*, 5> walkNsNames{"ahead_census_win50_walk_ns", "ahead_census_win150_walk_ns", "ahead_census_win400_walk_ns", "ahead_census_win1000_walk_ns", "ahead_census_winlong_walk_ns"};
            for (std::size_t i = 0; i < itemNames.size(); ++i) {
                ArchB::Recorder::RegisterCounter(itemNames[i], counters.aheadWindowItems[i]);
                ArchB::Recorder::RegisterCounter(walkItemNames[i], counters.aheadWindowWalkItems[i]);
                ArchB::Recorder::RegisterCounter(walkNsNames[i], counters.aheadWindowWalkNs[i]);
            }
            ArchB::Recorder::RegisterCounter("ahead_census_feasible_ns", counters.aheadFeasibleNs);
            ArchB::Recorder::RegisterCounter("ahead_census_late_ns", counters.aheadLateNs);
            ArchB::Recorder::RegisterCounter("ahead_census_invalid_ns", counters.aheadInvalidNs);
            ArchB::Recorder::RegisterCounter("ahead_census_unknown_ns", counters.aheadUnknownNs);
        }
        // APS5_ARCHB_PIPE_COMMITTER_MUTEX_PRIORITY: queue workers ask whether the committer is busy.
        GuestMemory::SetGpuLockPriorityHook([] { return Queue0().Outstanding() != 0; });
        return true;
    }();
    static_cast<void>(registered);
}

void DrawPipeline::CountSubmission() {
    pipeCounters().submissions.fetch_add(1, std::memory_order_relaxed);
}

void DrawPipeline::CountKeptIndirectRegisters() {
    pipeCounters().keptIndirectRegs.fetch_add(1, std::memory_order_relaxed);
}

void DrawPipeline::CountKeptIndirectArguments() {
    pipeCounters().keptIndirectArguments.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t DrawPipeline::KeptIndirectArguments() {
    return pipeCounters().keptIndirectArguments.load(std::memory_order_relaxed);
}

void DrawPipeline::CountNarrowedDrawWrites() {
    pipeCounters().narrowedDrawWrites.fetch_add(1, std::memory_order_relaxed);
}

void DrawPipeline::CountNarrowedSlicedDepth() {
    pipeCounters().narrowedSlicedDepth.fetch_add(1, std::memory_order_relaxed);
}

void DrawPipeline::CountNarrowedDispatchWrites() {
    pipeCounters().narrowedDispatchWrites.fetch_add(1, std::memory_order_relaxed);
}

void DrawPipeline::CountKeptOwnLabelWait() {
    pipeCounters().keptOwnLabelWaits.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t DrawPipeline::LockedDrains() {
    return pipeCounters().lockedDrains.load(std::memory_order_relaxed);
}

std::uint64_t DrawPipeline::FullOvercommits() {
    return pipeCounters().fullOvercommits.load(std::memory_order_relaxed);
}

std::size_t DrawPipeline::QueuedItems() {
    auto& pipeline = Queue0();
    std::lock_guard lock(pipeline.mutex);
    return pipeline.items.size();
}

bool& DrawPipeline::Active() {
    static thread_local bool active = false;
    return active;
}

std::atomic<std::uint64_t>& DrawPipeline::EpochToken() {
    static std::atomic<std::uint64_t> token{1};
    return token;
}

void DrawPipeline::CountEpochBump(BumpCause cause) {
    if (cause < BumpCauses) pipeCounters().q0Bumps[cause].fetch_add(1, std::memory_order_relaxed);
}

namespace {
// Token -> queue-0 collect epoch at the token's ordering point (APS5_ARCHB_PIPE_COLLECT_SHARE). The
// committer trails queue 0 by at most a pipeline's depth of tokens; an overwritten slot reads as
// unknown (nothing shared).
struct EpochClocks {
    static constexpr std::size_t Slots = 1024;
    std::array<std::atomic<std::uint64_t>, Slots> tokens{};
    std::array<std::atomic<std::uint64_t>, Slots> clocks{};
};

EpochClocks& epochClocks() {
    static auto* clocks = new EpochClocks();
    return *clocks;
}

std::uint64_t epochClock(std::uint64_t token) {
    auto& table = epochClocks();
    const auto slot = token % EpochClocks::Slots;
    if (table.tokens[slot].load(std::memory_order_acquire) != token) return 0;
    const auto clock = table.clocks[slot].load(std::memory_order_acquire);
    return table.tokens[slot].load(std::memory_order_acquire) == token ? clock : 0;
}
}

void DrawPipeline::NoteEpochClock(std::uint64_t token, std::uint64_t clock) {
    auto& table = epochClocks();
    const auto slot = token % EpochClocks::Slots;
    table.tokens[slot].store(0, std::memory_order_release);
    table.clocks[slot].store(clock, std::memory_order_release);
    table.tokens[slot].store(token, std::memory_order_release);
}

void DrawPipeline::FollowEpoch(std::uint64_t token, std::uint64_t ticket) {
    // Write generations: the committer checks at the ticket the issuing thread held when it enqueued
    // the commit (carried explicitly, rule V1/V3), not at a fresh one of its own.
    if (ticket != 0 && GuestMemory::Gen::Active()) GuestMemory::Gen::Follow(ticket);
    static thread_local std::uint64_t followed = 0;
    if (followed == token) return;
    followed = token;
    pipeCounters().followBumps.fetch_add(1, std::memory_order_relaxed);
    GuestMemory::BumpCollectEpoch(GuestMemory::Gen::TicketReason::Committer);
    GuestMemory::FollowCollectShare(epochClock(token));
}

unsigned DrawPipeline::Committers() {
    return runningCommitters.load(std::memory_order_acquire);
}

void DrawPipeline::rethrowFailure() {
    if (failure == nullptr) return;
    auto error = std::exchange(failure, nullptr);
    std::rethrow_exception(error);
}

void DrawPipeline::Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress, std::vector<std::byte> labelBytes, std::vector<Range> reads, bool draw, ItemKind kind) {
    const bool recorder = ArchB::Recorder::Enabled();
    const auto enqueueStart = recorder ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (DrawFinisher::Busy() && !DrawFinisher::OnFinisherThread()) DrawFinisher::Get().Drain();
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    struct ReclaimStorage {};
    auto& reclaim = HostThreadLocal<std::vector<Item>, ReclaimStorage>();
    reclaim.clear();
    const auto lockStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::unique_lock lock(mutex);
    if (!retired.empty()) std::swap(retired, reclaim);
    if (profile) lockWaitNs += elapsedNs(lockStart);
    rethrowFailure();
    if (closing) {
        lock.unlock();
        commit();
        return;
    }
    const auto capacity = Capacity();
    if (items.size() >= capacity) {
        if (GuestMemory::GpuMutex().HeldByThisThread()) {
            // The committer takes the GPU mutex to commit the item in front, so waiting for room here
            // would never end. The capacity only bounds how far queue 0 runs ahead; FIFO order and the
            // overlap and label lookups (which walk every queued item) hold for any length.
            noteOvercommit("draw pipeline", items.size());
        } else {
            const auto start = std::chrono::steady_clock::now();
            // APS5_ARCHB_PIPE_FULL_HYSTERESIS (sampled per full wait): resume at half capacity rather
            // than at the first free slot, so a committer-bound stretch wakes the producer once per
            // capacity/2 commits instead of after every commit (2026-11-03 profile: ~315 full waits
            // per frame, each a committer notify and a producer context switch).
            resumeAt = ArchB::Switches::Enabled(ArchB::Feature::PipeFullHysteresis) ? capacity / 2 : capacity - 1;
            ++idleWaiters;
            {
                const Stage4::WaitScope stage4Wait(Stage4::Wait::PipeFull, false);
                idle.wait(lock, [&] { return items.size() <= resumeAt; });
            }
            --idleWaiters;
            resumeAt = 0;
            const auto waited = elapsedNs(start);
            fullWaitNs += waited;
            pipeCounters().fullWaits.fetch_add(1, std::memory_order_relaxed);
            pipeCounters().fullWaitNs.fetch_add(waited, std::memory_order_relaxed);
            if (recorder) ArchB::Recorder::Count("pipe_full_wait_ms", static_cast<double>(waited) / 1e6);
            rethrowFailure();
        }
    }
    depthSum += items.size();
    ++enqueued;
    heldReadRanges.fetch_add(reads.size(), std::memory_order_relaxed);
    countWrites(writes, true);
    items.push_back({std::move(commit), std::move(writes), labelAddress, std::move(labelBytes), std::move(reads), draw, kind});
    static const bool aheadCensus = GuestMemory::CollectAheadCensus();
    if (aheadCensus && kind == ItemKind::Draw) {
        items.back().enqueuedNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        items.back().enqueueClock = GuestMemory::CollectClock();
    }
    outstanding.fetch_add(1, std::memory_order_release);
    pipeCounters().enqueues.fetch_add(1, std::memory_order_relaxed);
    if (!thread.joinable()) {
        thread = std::thread([this] {
            GuestMemory::MarkProofThread();
            run();
        });
        runningCommitters.fetch_add(1, std::memory_order_acq_rel);
    }
    if (committerWaiting) {
        ++wakes;
        wake.notify_one();
    }
    static const bool stats = std::getenv("APS5_PIPELINE_STATS") != nullptr;
    if (recorder) {
        const auto spent = elapsedNs(enqueueStart);
        pipeCounters().enqueueNs.fetch_add(spent, std::memory_order_relaxed);
        ArchB::Recorder::Count("pipe_enqueue_ns", static_cast<double>(spent));
    }
    if (!profile && !stats) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= std::chrono::seconds(10)) report(now);
}

namespace {
std::atomic<std::uint64_t> recorderCommitsAll{0};
// Stage 4.3 counters (recorder): labels parked, what committed them, and how long they stayed parked.
struct OwnerCounters {
    std::atomic<std::uint64_t> checks{0};       // label items the committer considered parking
    std::atomic<std::uint64_t> checksLocked{0}; // ... that had write-backs to look at under the GPU mutex
    std::atomic<std::uint64_t> parks{0};
    std::atomic<std::uint64_t> passed{0};       // items committed ahead of a parked label
    std::atomic<std::uint64_t> done{0}, idle{0}, barrier{0}, close{0};
    std::atomic<std::uint64_t> parkedUs{0};
    std::atomic<std::uint64_t> idleWaitUs{0};   // waitParked time (the committer had nothing else to do)
    std::atomic<std::uint64_t> barrierLabel{0}, barrierOther{0}, barrierOverlap{0};  // why an item did not pass
};
OwnerCounters& ownerCounters() {
    static OwnerCounters counters;
    static const bool registered = [] {
        ArchB::Recorder::RegisterCounter("owner_label_checks", counters.checks);
        ArchB::Recorder::RegisterCounter("owner_label_checks_locked", counters.checksLocked);
        ArchB::Recorder::RegisterCounter("owner_labels_parked", counters.parks);
        ArchB::Recorder::RegisterCounter("owner_items_passed", counters.passed);
        ArchB::Recorder::RegisterCounter("owner_park_commit_done", counters.done);
        ArchB::Recorder::RegisterCounter("owner_park_commit_idle", counters.idle);
        ArchB::Recorder::RegisterCounter("owner_park_commit_barrier", counters.barrier);
        ArchB::Recorder::RegisterCounter("owner_park_commit_close", counters.close);
        ArchB::Recorder::RegisterCounter("owner_parked_us", counters.parkedUs);
        ArchB::Recorder::RegisterCounter("owner_idle_wait_us", counters.idleWaitUs);
        ArchB::Recorder::RegisterCounter("owner_barrier_label", counters.barrierLabel);
        ArchB::Recorder::RegisterCounter("owner_barrier_other_kind", counters.barrierOther);
        ArchB::Recorder::RegisterCounter("owner_barrier_overlap", counters.barrierOverlap);
        return true;
    }();
    static_cast<void>(registered);
    return counters;
}

bool rangesOverlap(const std::vector<DrawPipeline::Range>& a, const std::vector<DrawPipeline::Range>& b) {
    for (const auto& [begin, end] : a) {
        for (const auto& [otherBegin, otherEnd] : b) {
            if (begin < otherEnd && otherBegin < end) return true;
        }
    }
    return false;
}
}

std::string DrawPipeline::CumulativeText() {
    auto& counters = pipeCounters();
    std::string text = "[pipe] cumulative: commits";
    char item[128];
    for (std::size_t i = 0; i < KindNames.size(); ++i) {
        std::snprintf(item, sizeof(item), " %s %llu", KindNames[i], static_cast<unsigned long long>(counters.commits[i].load(std::memory_order_relaxed)));
        text += item;
    }
    text += "; drains";
    for (std::size_t i = 0; i < DrainNames.size(); ++i) {
        std::snprintf(item, sizeof(item), " %s %llu", DrainNames[i], static_cast<unsigned long long>(counters.drains[i].load(std::memory_order_relaxed)));
        text += item;
    }
    text += "; by cause";
    for (std::size_t i = 0; i < CauseNames.size(); ++i) {
        const auto count = counters.causes[i].load(std::memory_order_relaxed);
        if (count == 0) continue;
        std::snprintf(item, sizeof(item), " %s %llu", CauseNames[i], static_cast<unsigned long long>(count));
        text += item;
    }
    std::snprintf(item, sizeof(item), "; drain wait %.1f ms; %llu enqueues, %llu submissions", static_cast<double>(counters.drainWaitNs.load(std::memory_order_relaxed)) / 1e6, static_cast<unsigned long long>(counters.enqueues.load(std::memory_order_relaxed)), static_cast<unsigned long long>(counters.submissions.load(std::memory_order_relaxed)));
    text += item;
    std::snprintf(item, sizeof(item), "; %llu register packets kept the pipeline", static_cast<unsigned long long>(counters.keptIndirectRegs.load(std::memory_order_relaxed)));
    text += item;
    std::snprintf(item, sizeof(item), "; %llu indirect draws kept it over their arguments; %llu draw write sets narrowed", static_cast<unsigned long long>(counters.keptIndirectArguments.load(std::memory_order_relaxed)), static_cast<unsigned long long>(counters.narrowedDrawWrites.load(std::memory_order_relaxed)));
    text += item;
    std::snprintf(item, sizeof(item), "; %llu sliced depth write sets narrowed; %llu dispatch write sets narrowed", static_cast<unsigned long long>(counters.narrowedSlicedDepth.load(std::memory_order_relaxed)), static_cast<unsigned long long>(counters.narrowedDispatchWrites.load(std::memory_order_relaxed)));
    text += item;
    std::snprintf(item, sizeof(item), "; %llu waits kept on own labels", static_cast<unsigned long long>(counters.keptOwnLabelWaits.load(std::memory_order_relaxed)));
    text += item;
    {
        const auto copy = [](CopyItemCounter counter) { return static_cast<unsigned long long>(copyItemCounts[counter].load(std::memory_order_relaxed)); };
        char line[256];
        std::snprintf(line, sizeof(line), "; copy items %llu (copied %llu, fallback %llu; refused kind %llu shape %llu record %llu labels %llu; verified %llu, mismatches %llu)", copy(CopyItemEnqueued), copy(CopyItemCopied), copy(CopyItemFallback), copy(CopyItemRefusedKind), copy(CopyItemRefusedShape), copy(CopyItemRefusedRecord), copy(CopyItemRefusedLabels), copy(CopyItemVerified), copy(CopyItemVerifyMismatch));
        text += line;
    }
    text += DrawWriteDiagText();
    text += DrainDiagText();
    {
        auto& owner = ownerCounters();
        const auto load = [](const std::atomic<std::uint64_t>& value) { return static_cast<unsigned long long>(value.load(std::memory_order_relaxed)); };
        char line[400];
        std::snprintf(line, sizeof(line), "; owner: %llu label checks (%llu locked), %llu parked, %llu items passed, committed done %llu / idle %llu / barrier %llu (label %llu, other kind %llu, overlap %llu) / close %llu, parked %.1f ms, idle wait %.1f ms", load(owner.checks), load(owner.checksLocked), load(owner.parks), load(owner.passed), load(owner.done), load(owner.idle), load(owner.barrier), load(owner.barrierLabel), load(owner.barrierOther), load(owner.barrierOverlap), load(owner.close), static_cast<double>(load(owner.parkedUs)) / 1e3, static_cast<double>(load(owner.idleWaitUs)) / 1e3);
        text += line;
    }
    std::snprintf(item, sizeof(item), "; full: %llu waits (%.1f ms), %llu overcommits; locked drains %llu", static_cast<unsigned long long>(counters.fullWaits.load(std::memory_order_relaxed)), static_cast<double>(counters.fullWaitNs.load(std::memory_order_relaxed)) / 1e6, static_cast<unsigned long long>(counters.fullOvercommits.load(std::memory_order_relaxed)), static_cast<unsigned long long>(counters.lockedDrains.load(std::memory_order_relaxed)));
    text += item;
    return text;
}

void DrawPipeline::report(std::chrono::steady_clock::time_point now) {
    lastReport = now;
    std::string drainText;
    for (std::size_t i = 0; i < drains.size(); ++i) {
        char item[64];
        std::snprintf(item, sizeof(item), " %s %llu (%.1f ms)", DrainNames[i], static_cast<unsigned long long>(drains[i]), static_cast<double>(drainWaitNs[i]) / 1e6);
        drainText += item;
    }
    drainText += "; packet drains by opcode:";
    for (int shown = 0; shown < 5; ++shown) {
        const auto top = std::max_element(drainOpcodes.begin(), drainOpcodes.begin() + 0x100);
        if (*top == 0) break;
        char item[32];
        std::snprintf(item, sizeof(item), " 0x%02x x%llu", static_cast<unsigned>(top - drainOpcodes.begin()), static_cast<unsigned long long>(*top));
        drainText += item;
        *top = 0;
    }
    drainOpcodes = {};
    const auto perCommit = commits == 0 ? 0.0 : static_cast<double>(commitNs) / 1e3 / static_cast<double>(commits);
    const auto perDraw = enqueued == 0 ? 0.0 : static_cast<double>(fullWaitNs) / 1e3 / static_cast<double>(enqueued);
    const auto queued = enqueued == 0 ? 0.0 : static_cast<double>(depthSum) / static_cast<double>(enqueued);
    const auto decodeDrainCount = decodeDrains.load(std::memory_order_relaxed);
    char reads[96];
    std::snprintf(reads, sizeof(reads), " %llu read ranges, %llu stores held, %llu decode drains", static_cast<unsigned long long>(heldReadRanges.load(std::memory_order_relaxed)), static_cast<unsigned long long>(heldReadHits.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(decodeDrainCount - reportedDecodeDrains));
    reportedDecodeDrains = decodeDrainCount;
    drainText += reads;
    AgcDriver::ProfilePrint_nid_no_patch("[draw] pipelined (10 s, depth %zu): %llu commits at %.1f us each on the committer, %llu failed; %llu enqueued behind %.2f on average, waiting %.1f us per draw for room, %.2f us for the lock; %llu committer wakes;%s; drains:%s\n", Capacity(), static_cast<unsigned long long>(commits), perCommit, static_cast<unsigned long long>(commitErrors), static_cast<unsigned long long>(enqueued), queued, perDraw, enqueued == 0 ? 0.0 : static_cast<double>(lockWaitNs) / 1e3 / static_cast<double>(enqueued), static_cast<unsigned long long>(wakes), reads, drainText.c_str());
    commits = commitNs = commitErrors = enqueued = fullWaitNs = depthSum = wakes = lockWaitNs = 0;
    drains = {};
    drainWaitNs = {};
    AgcDriver::ProfilePrint_nid_no_patch("%s\n", CumulativeText().c_str());
    AgcDriver::ProfilePrint_nid_no_patch("%s\n", Graphics::ShaderResources::DrawSetPoolText().c_str());
    AgcDriver::ProfilePrint_nid_no_patch("%s\n", Graphics::ShaderResources::DrawReuseText().c_str());
    if (const auto fixes = Graphics::ShaderResources::SnapshotFixesText(); !fixes.empty()) AgcDriver::ProfilePrint_nid_no_patch("%s\n", fixes.c_str());
    if (const auto diag = Graphics::ShaderResources::DrawBindingsDiagText(); !diag.empty()) AgcDriver::ProfilePrint_nid_no_patch("%s\n", diag.c_str());
    if (const auto diag = Graphics::DrawLookupDiagText(); !diag.empty()) AgcDriver::ProfilePrint_nid_no_patch("%s\n", diag.c_str());
    AgcDriver::ProfilePrint_nid_no_patch("%s\n", Graphics::DrawLookupMemoText().c_str());
    AgcDriver::ProfilePrint_nid_no_patch("%s\n", Graphics::PipelineLookupMemoText().c_str());
}


namespace {
// APS5_COLLECT_AHEAD_CENSUS: one committed draw item (see PipeCounters).
void countAheadWindow(std::uint64_t enqueuedNs, std::uint64_t enqueueClock, std::chrono::steady_clock::time_point started) {
    const auto collects = GuestMemory::TakeCommitterItemCollects();
    const auto startedNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(started.time_since_epoch()).count());
    const auto window = startedNs > enqueuedNs ? startedNs - enqueuedNs : 0;
    auto& counters = pipeCounters();
    counters.aheadDraws.fetch_add(1, std::memory_order_relaxed);
    counters.aheadWindowNs.fetch_add(window, std::memory_order_relaxed);
    std::size_t bucket = 0;
    while (bucket < AheadWindowBoundsUs.size() && window >= AheadWindowBoundsUs[bucket] * 1000) ++bucket;
    counters.aheadWindowItems[bucket].fetch_add(1, std::memory_order_relaxed);
    if (collects.walks == 0) return;
    counters.aheadWindowWalkItems[bucket].fetch_add(1, std::memory_order_relaxed);
    counters.aheadWindowWalkNs[bucket].fetch_add(collects.walkNs, std::memory_order_relaxed);
    if (collects.required == std::numeric_limits<std::uint64_t>::max()) counters.aheadUnknownNs.fetch_add(collects.walkNs, std::memory_order_relaxed);
    else if (collects.required > enqueueClock) counters.aheadInvalidNs.fetch_add(collects.walkNs, std::memory_order_relaxed);
    else if (window < collects.walkNs + AheadLatencyNs) counters.aheadLateNs.fetch_add(collects.walkNs, std::memory_order_relaxed);
    else counters.aheadFeasibleNs.fetch_add(collects.walkNs, std::memory_order_relaxed);
}
}

void DrawPipeline::run() {
    PerformanceTrace::BindThread("draw committer");
    GuestMemory::MarkCommitterThread();
    Stage4::BindRole(Stage4::Role::Committer);
    // Stage 4 M4 (APS5_RESOLVE_CENSUS): the item's declared reads and writes, copied under the mutex
    // (ReleaseRunningReads clears the running item's reads during its commit).
    std::vector<Range> stage4Reads;
    std::vector<Range> stage4Writes;
    StartCommitterSampler();
    static const bool registered = [] { ArchB::Recorder::RegisterCounter("draw_pipeline_commits", recorderCommitsAll); return true; }();
    static_cast<void>(registered);
    for (;;) {
        Commit* commit = nullptr;
        bool isDraw = false;
        ItemKind kind = ItemKind::Other;
        std::uint64_t enqueuedNs = 0;
        std::uint64_t enqueueClock = 0;
        // APS5_ARCHB_PIPE_DEVICE_OWNER: a parked label commits as soon as its batch completed; idle
        // with one parked, the committer waits for that batch (without the GPU mutex) until an item
        // arrives. The parked item counts in `outstanding`.
        while (!parked.empty()) {
            if (parkedDone()) commitParked("done");
            else if (outstanding.load(std::memory_order_acquire) <= parked.size() && waitParked()) commitParked("idle");
            else break;
        }
        const std::size_t parkedCount = parked.size();
        // pipe_committer_idle_ms: from the end of one item until the next is taken (recorder only).
        const bool idleBefore = ArchB::Recorder::Enabled() && outstanding.load(std::memory_order_acquire) == parkedCount;
        const auto idleStart = idleBefore ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        // Stage 4 M1: the committer idles (spins, then sleeps until queue 0 enqueues).
        std::optional<Stage4::WaitScope> stage4Idle;
        if (Stage4::Tracking() && outstanding.load(std::memory_order_acquire) == parkedCount) stage4Idle.emplace(Stage4::Wait::Idle, true);
        SpinUntil(HandoffSpinWindow(), [&] { return outstanding.load(std::memory_order_acquire) > parkedCount; });
        bool commitParkedFirst = false;
        bool parkFront = false;
        {
            std::unique_lock lock(mutex);
            if (stage4Idle && items.empty() && !closing) stage4Idle->Blocking();
            committerWaiting = true;
            wake.wait(lock, [&] { return !items.empty() || closing; });
            committerWaiting = false;
            if (items.empty()) {
                if (parked.empty()) break;
                // Closing with a label parked: it commits before the committer exits.
                lock.unlock();
                stage4Idle.reset();
                commitParked("close");
                continue;
            }
            // A label behind parked ones parks too (in order; up to ParkLimit), whatever it stores.
            constexpr std::size_t ParkLimit = 16;
            const bool label = !stopped && items.front().kind == ItemKind::Label;
            if (!parked.empty() && label && parked.size() < ParkLimit) parkFront = true;
            else if (!parked.empty() && !mayPassParked(items.front())) commitParkedFirst = true;
            else if (parked.empty() && label && ArchB::Switches::Enabled(ArchB::Feature::PipeDeviceOwner)) parkFront = true;
            commit = stopped ? nullptr : &items.front().commit;
            isDraw = items.front().draw;
            kind = items.front().kind;
            enqueuedNs = items.front().enqueuedNs;
            enqueueClock = items.front().enqueueClock;
            if (Stage4::ResolveCensus()) {
                stage4Reads.assign(items.front().reads.begin(), items.front().reads.end());
                stage4Writes.assign(items.front().writes.begin(), items.front().writes.end());
            }
        }
        stage4Idle.reset();
        // APS5_ARCHB_PIPE_DEVICE_OWNER: an item that may not pass the parked label commits it first
        // (the front item stays in place: only this thread pops it); a label may park.
        if (commitParkedFirst) {
            while (!parked.empty()) commitParked("barrier");
        }
        if (parkFront && tryPark(!parked.empty())) continue;
        if (Stage4::ResolveCensus()) Stage4::BeginItem(static_cast<std::uint8_t>(kind), stage4Reads, stage4Writes, Graphics::StorageTexture::PendingSerial());
        const auto start = std::chrono::steady_clock::now();
        if (idleBefore) {
            const auto idleNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(start - idleStart).count());
            pipeCounters().idleNs.fetch_add(idleNs, std::memory_order_relaxed);
            ArchB::Recorder::Count("pipe_committer_idle_ms", static_cast<double>(idleNs) / 1e6);
        }
        std::exception_ptr fatal;
        bool failed = false;
        bool shutdown = false;
        static const bool collectCensus = GuestMemory::CommitterCollectCensus();
        if (collectCensus) GuestMemory::BeginCommitterCensusItem(static_cast<std::uint8_t>(kind));
        try {
            if (commit != nullptr) (*commit)();
            if (!isDraw) GuestMemory::BreakProofs();
        } catch (const std::exception&) {
            failed = true;
            fatal = std::current_exception();
        } catch (const ProcessShutdown&) {
            shutdown = true;
            fatal = std::current_exception();
        } catch (...) {
            fatal = std::current_exception();
        }
        const auto spent = elapsedNs(start);
        if (!parked.empty()) ownerCounters().passed.fetch_add(1, std::memory_order_relaxed);
        if (Stage4::ResolveCensus()) Stage4::EndItem(Graphics::StorageTexture::PendingSerial(), !isDraw);
        if (enqueuedNs != 0) countAheadWindow(enqueuedNs, enqueueClock, start);
        if (fatal != nullptr && !stopped) {
            stopped = true;
            if (!shutdown) Driver::Get().ReportFailure(fatal);
        }
        std::lock_guard lock(mutex);
        retire(items.front(), spent, failed, fatal, kind);
        items.pop_front();
        if (idleWaiters != 0 && (items.empty() || items.size() <= resumeAt)) idle.notify_all();
    }
    runningCommitters.fetch_sub(1, std::memory_order_acq_rel);
}

// Under `mutex`: the bookkeeping of a committed item (the front one, or the parked label).
void DrawPipeline::retire(Item& item, std::uint64_t spent, bool failed, std::exception_ptr fatal, ItemKind kind) {
    countWrites(item.writes, false);
    retired.push_back(std::move(item));
    retiredItems.fetch_add(1, std::memory_order_release);
    ++commits;
    recorderCommitsAll.fetch_add(1, std::memory_order_relaxed);
    pipeCounters().commits[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
    commitNs += spent;
    if (failed) ++commitErrors;
    if (fatal != nullptr && failure == nullptr) failure = fatal;
    outstanding.fetch_sub(1, std::memory_order_release);
}

bool DrawPipeline::mayPassParked(const Item& item) const {
    // Only draws and dispatches pass, and only when neither their declared writes nor their reads
    // touch what a parked label stores (its write-back ranges and the label itself).
    auto& counters = ownerCounters();
    if (item.kind != ItemKind::Draw && item.kind != ItemKind::Dispatch) {
        (item.kind == ItemKind::Label ? counters.barrierLabel : counters.barrierOther).fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    for (const auto& entry : parked) {
        if (rangesOverlap(item.writes, entry.stores) || rangesOverlap(item.reads, entry.stores) || rangesOverlap(item.writes, entry.item.writes) || rangesOverlap(item.reads, entry.item.writes)) {
            counters.barrierOverlap.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    return true;
}

bool DrawPipeline::tryPark(bool force) {
    auto& counters = ownerCounters();
    counters.checks.fetch_add(1, std::memory_order_relaxed);
    Parked entry;
    try {
        if (!tryParkChecked(force, entry)) return false;
    } catch (...) {
        // A failure here (a lost device in Reap) is left to the item's own commit, which reports it.
        // A label that must stay behind parked ones parks as storing everything, so nothing passes it.
        if (!force) return false;
        entry.stores.assign(1, Range{0, std::numeric_limits<std::uint64_t>::max()});
        entry.serial = 0;
    }
    entry.at = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(mutex);
        entry.item = std::move(items.front());
        items.pop_front();
        parked.push_back(std::move(entry));
        ++parkEvents;
        if (idleWaiters != 0 && items.size() <= resumeAt) idle.notify_all();
    }
    counters.parks.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool DrawPipeline::tryParkChecked(bool force, Parked& entry) {
    auto& counters = ownerCounters();
    if (Graphics::StorageTexture::AnyLabelStores(true)) {
        counters.checksLocked.fetch_add(1, std::memory_order_relaxed);
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
        std::lock_guard gpu(GuestMemory::GpuMutex());
        auto* recorder = Graphics::Recorder::Active();
        if (recorder == nullptr || !recorder->HasTimeline() || Graphics::Recorder::InCompletion()) {
            if (!force) return false;
        } else {
            // As the label's own FlushPending would: batches already finished retire first.
            recorder->Reap();
            const auto found = Graphics::StorageTexture::LabelStoreWriter(true, entry.stores);
            if (!found.has_value()) {
                // It stores more than its ranges (APS5_STORE_AT_LABEL): it cannot be parked.
                if (!force) return false;
                entry.stores.assign(1, Range{0, std::numeric_limits<std::uint64_t>::max()});
            } else if (*found > recorder->CompletedThrough()) {
                entry.serial = *found;
                // The open batch goes out now (the unparked commit would Sync it), so the timeline reaches it.
                if (entry.serial >= recorder->OpenSerial()) recorder->Submit();
            }
        }
    }
    return force || entry.serial != 0;
}

bool DrawPipeline::parkedDone() {
    const auto serial = parked.front().serial;
    if (serial == 0) return true;
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
    std::unique_lock gpu(GuestMemory::GpuMutex(), std::try_to_lock);
    if (!gpu.owns_lock()) return false;
    auto* recorder = Graphics::Recorder::Active();
    if (recorder == nullptr || serial <= recorder->CompletedThrough()) return true;
    Graphics::Recorder::TimelineWatch watch;
    return watch.Begin(*recorder, serial) ? watch.Wait(0) : true;
}

bool DrawPipeline::waitParked() {
    const auto serial = parked.front().serial;
    if (serial == 0) return true;
    Graphics::Recorder::TimelineWatch watch;
    {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
        std::lock_guard gpu(GuestMemory::GpuMutex());
        auto* recorder = Graphics::Recorder::Active();
        if (recorder == nullptr || serial <= recorder->CompletedThrough()) return true;
        if (!watch.Begin(*recorder, serial)) return true;
    }
    const auto start = std::chrono::steady_clock::now();
    const Stage4::WaitScope stage4Wait(Stage4::Wait::Fence, true);
    bool done = false;
    // Spin on the timeline (status queries) while queue 0 may enqueue soon, then block in slices.
    const auto spinFor = HandoffSpinWindow();
    const auto waiting = parked.size();
    for (;;) {
        if (watch.Wait(0)) {
            done = true;
            break;
        }
        if (outstanding.load(std::memory_order_acquire) > waiting) break;
        if (std::chrono::steady_clock::now() - start < spinFor) {
            std::this_thread::yield();
            continue;
        }
        if (watch.Wait(200'000)) {
            done = true;
            break;
        }
        std::lock_guard lock(mutex);
        if (closing || !items.empty()) break;
    }
    watch.End();
    ownerCounters().idleWaitUs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
    return done;
}

void DrawPipeline::commitParked(const char* when) {
    auto& counters = ownerCounters();
    auto& front = parked.front();
    (std::strcmp(when, "done") == 0 ? counters.done : std::strcmp(when, "idle") == 0 ? counters.idle : std::strcmp(when, "barrier") == 0 ? counters.barrier : counters.close).fetch_add(1, std::memory_order_relaxed);
    counters.parkedUs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - front.at).count()), std::memory_order_relaxed);
    const auto start = std::chrono::steady_clock::now();
    std::exception_ptr fatal;
    bool failed = false;
    bool shutdown = false;
    try {
        if (!stopped) {
            // Only the write-backs it was parked for: the items that passed it store their own.
            const Graphics::StorageTexture::LabelStoreScope scope(&front.stores);
            front.item.commit();
        }
        GuestMemory::BreakProofs();
    } catch (const std::exception&) {
        failed = true;
        fatal = std::current_exception();
    } catch (const ProcessShutdown&) {
        shutdown = true;
        fatal = std::current_exception();
    } catch (...) {
        fatal = std::current_exception();
    }
    const auto spent = elapsedNs(start);
    if (fatal != nullptr && !stopped) {
        stopped = true;
        if (!shutdown) Driver::Get().ReportFailure(fatal);
    }
    std::lock_guard lock(mutex);
    const auto kind = front.item.kind;
    retire(front.item, spent, failed, fatal, kind);
    parked.pop_front();
    ++parkEvents;
    if (idleWaiters != 0 && (items.empty() || items.size() <= resumeAt)) idle.notify_all();
}

void DrawPipeline::Stop() {
    static const bool stats = std::getenv("APS5_PIPELINE_STATS") != nullptr || std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (stats && pipeCounters().enqueues.load(std::memory_order_relaxed) != 0) {
        AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", CumulativeText().c_str());
        AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", Graphics::ShaderResources::DrawSetPoolText().c_str());
        AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", Graphics::ShaderResources::DrawReuseText().c_str());
        if (const auto fixes = Graphics::ShaderResources::SnapshotFixesText(); !fixes.empty()) AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", fixes.c_str());
        if (const auto diag = Graphics::ShaderResources::DrawBindingsDiagText(); !diag.empty()) AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", diag.c_str());
        if (const auto diag = Graphics::DrawLookupDiagText(); !diag.empty()) AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", diag.c_str());
        AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", Graphics::DrawLookupMemoText().c_str());
        AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", Graphics::PipelineLookupMemoText().c_str());
    }
    std::thread committer;
    {
        std::lock_guard lock(mutex);
        closing = true;
        committer = std::move(thread);
    }
    wake.notify_all();
    if (!committer.joinable()) return;
    if (committer.get_id() == std::this_thread::get_id()) committer.detach();
    else committer.join();
}

void DrawPipeline::ReleaseRetired() {
    std::vector<Item> done;
    std::lock_guard lock(mutex);
    std::swap(done, retired);
}

void DrawPipeline::Drain(DrainReason reason, std::uint32_t opcode, DrainCause cause) {
    drainRequests[std::min<std::uint32_t>(opcode, 0x100)].fetch_add(1, std::memory_order_relaxed);
    const bool finisherBusy = DrawFinisher::Busy() && !DrawFinisher::OnFinisherThread();
    if ((finisherBusy || Busy()) && GuestMemory::GpuMutex().HeldByThisThread()) {
        // Queue 0 drains before it takes the GPU mutex (A1 notes in docs/dev/debt/libSceAgcDriver.md);
        // a caller that did not is counted and waits with its hold given up, because the committer
        // needs the mutex to empty the pipeline.
        noteLockedDrain("draw pipeline", static_cast<unsigned>(reason), opcode);
        waitWithoutGpuLock([&] { drainUnlocked(reason, opcode, cause, finisherBusy); });
        return;
    }
    drainUnlocked(reason, opcode, cause, finisherBusy);
}

void DrawPipeline::drainUnlocked(DrainReason reason, std::uint32_t opcode, DrainCause cause, bool finisherBusy) {
    if (finisherBusy) DrawFinisher::Get().Drain();
    if (!Busy()) return;
    const auto start = std::chrono::steady_clock::now();
    std::optional<Stage4::WaitScope> stage4Wait;
    if (Stage4::Tracking()) stage4Wait.emplace(Stage4::Wait::PipeDrain, true);
    SpinUntil(HandoffSpinWindow() * 2, [&] { return !Busy(); });
    std::unique_lock lock(mutex);
    if (stage4Wait && (!items.empty() || !parked.empty())) stage4Wait->Blocking();
    ++idleWaiters;
    idle.wait(lock, [&] { return items.empty() && parked.empty(); });
    --idleWaiters;
    stage4Wait.reset();
    const auto index = static_cast<std::size_t>(reason);
    const auto waited = elapsedNs(start);
    ++drains[index];
    ++drainOpcodes[std::min<std::uint32_t>(opcode, 0x100)];
    drainWaitNs[index] += waited;
    auto& counters = pipeCounters();
    counters.drains[index].fetch_add(1, std::memory_order_relaxed);
    counters.causes[static_cast<std::size_t>(cause)].fetch_add(1, std::memory_order_relaxed);
    counters.drainWaitNs.fetch_add(waited, std::memory_order_relaxed);
    if (ArchB::Recorder::Enabled()) ArchB::Recorder::Count("pipe_drain_wait_ms", static_cast<double>(waited) / 1e6);
    rethrowFailure();
}

std::uint64_t DrawPipeline::DrainRequestsByOpcode(std::uint32_t opcode) {
    return Queue0().drainRequests[std::min<std::uint32_t>(opcode, 0x100)].load(std::memory_order_relaxed);
}

bool DrawPipeline::HeldReads(std::uint64_t address, std::size_t bytes) {
    static const bool unordered = std::getenv("APS5_NO_QUEUED_DRAW_READ_ORDER") != nullptr;
    if (bytes == 0 || unordered) return false;
    const auto end = address + bytes;
    auto& pipeline = Queue0();
    std::lock_guard lock(pipeline.mutex);
    heldReadQueries.fetch_add(1, std::memory_order_relaxed);
    for (const auto& item : pipeline.items) {
        for (const auto& [begin, limit] : item.reads) {
            if (address >= limit || begin >= end) continue;
            heldReadHits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    for (const auto& entry : pipeline.parked) {
        for (const auto& [begin, limit] : entry.item.reads) {
            if (address >= limit || begin >= end) continue;
            heldReadHits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

std::uint64_t DrawPipeline::HeldReadQueries() {
    return heldReadQueries.load(std::memory_order_relaxed);
}

void DrawPipeline::ReleaseRunningReads() {
    std::lock_guard lock(mutex);
    if (!items.empty()) items.front().reads.clear();
}

std::optional<std::uint64_t> DrawPipeline::PendingLabel(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || bytes > 8) return std::nullopt;
    if (DrawFinisher::Overlap()) {
        std::uint64_t held = 0;
        const auto lookup = DrawFinisher::Get().PendingLabel(address, bytes, held);
        if (lookup == DrawFinisher::LabelLookup::Value) return held;
        if (lookup == DrawFinisher::LabelLookup::Unknown) return std::nullopt;
    }
    const auto end = address + bytes;
    std::lock_guard lock(mutex);
    // Newest first; a parked label is older than every queued item.
    const auto held = [&](const Item& item) -> std::optional<std::optional<std::uint64_t>> {
        const bool overlaps = std::any_of(item.writes.begin(), item.writes.end(), [&](const Range& range) { return address < range.second && range.first < end; });
        if (!overlaps) return std::nullopt;
        if (item.labelBytes.empty() || address < item.labelAddress || end > item.labelAddress + item.labelBytes.size()) return std::optional<std::uint64_t>{};
        std::uint64_t value = 0;
        std::memcpy(&value, item.labelBytes.data() + (address - item.labelAddress), bytes);
        return std::optional<std::uint64_t>{value};
    };
    for (auto item = items.rbegin(); item != items.rend(); ++item) {
        if (auto found = held(*item)) return *found;
    }
    for (auto entry = parked.rbegin(); entry != parked.rend(); ++entry) {
        if (auto found = held(entry->item)) return *found;
    }
    return std::nullopt;
}

void DrawPipeline::countWrites(const std::vector<Range>& writes, bool added) {
    for (const auto& range : writes) {
        const auto found = std::find_if(queuedWrites.begin(), queuedWrites.end(), [&](const auto& entry) { return entry.first == range; });
        if (added) {
            if (found != queuedWrites.end()) ++found->second;
            else queuedWrites.emplace_back(range, 1u);
        } else if (found != queuedWrites.end() && --found->second == 0) {
            *found = queuedWrites.back();
            queuedWrites.pop_back();
        }
    }
}

DrawPipeline::Watch DrawPipeline::WatchWrites() {
    if (!Busy()) return {Retired(), 0, {}};
    std::lock_guard lock(mutex);
    Watch watch{retiredItems.load(std::memory_order_relaxed), items.size(), {}};
    for (const auto& item : items) watch.writes.insert(watch.writes.end(), item.writes.begin(), item.writes.end());
    watch.parks = parkEvents;
    watch.parked = !parked.empty();
    for (const auto& entry : parked) {
        ++watch.items;
        watch.writes.insert(watch.writes.end(), entry.item.writes.begin(), entry.item.writes.end());
    }
    return watch;
}

bool DrawPipeline::WrittenSince(const Watch& watch, const std::vector<Range>& reads) {
    const auto overlaps = [&reads](const std::vector<Range>& writes) {
        return std::any_of(reads.begin(), reads.end(), [&writes](const Range& read) {
            return std::any_of(writes.begin(), writes.end(), [&read](const Range& write) { return read.first < write.second && write.first < read.second; });
        });
    };
    if (DrawFinisher::Overlap() && DrawFinisher::Get().PendingOverlapsAny(reads)) return true;
    std::lock_guard lock(mutex);
    if (std::any_of(items.begin(), items.end(), [&](const Item& item) { return overlaps(item.writes); })) return true;
    if (std::any_of(parked.begin(), parked.end(), [&](const Parked& entry) { return overlaps(entry.item.writes); })) return true;
    const auto retiredNow = retiredItems.load(std::memory_order_relaxed);
    if (retiredNow == watch.retired) return false;
    // Retirement out of queue order around a parked label: which items retired is unknown.
    if (watch.parked || watch.parks != parkEvents) return true;
    return retiredNow - watch.retired > watch.items || overlaps(watch.writes);
}

bool DrawPipeline::Busy() const {
    return outstanding.load(std::memory_order_acquire) != 0 || (finisherProducer && DrawFinisher::Busy() && DrawFinisher::Overlap());
}

bool DrawPipeline::OverlapsAny(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
    if (DrawFinisher::Overlap() && DrawFinisher::Get().PendingOverlapsAny(ranges)) return true;
    std::lock_guard lock(mutex);
    for (const auto& [range, count] : queuedWrites) {
        for (const auto& [address, end] : ranges) {
            if (address < range.second && range.first < end) return true;
        }
    }
    return false;
}

std::optional<DrawPipeline::ItemKind> DrawPipeline::NewestWriter(std::uint64_t address, std::size_t bytes, Range* overlapped) {
    const auto end = address + bytes;
    std::lock_guard lock(mutex);
    for (auto item = items.rbegin(); item != items.rend(); ++item) {
        const auto write = std::find_if(item->writes.begin(), item->writes.end(), [&](const Range& range) { return address < range.second && range.first < end; });
        if (write == item->writes.end()) continue;
        if (overlapped != nullptr) *overlapped = *write;
        return item->kind;
    }
    for (auto entry = parked.rbegin(); entry != parked.rend(); ++entry) {
        const auto write = std::find_if(entry->item.writes.begin(), entry->item.writes.end(), [&](const Range& range) { return address < range.second && range.first < end; });
        if (write != entry->item.writes.end()) {
            if (overlapped != nullptr) *overlapped = *write;
            return entry->item.kind;
        }
    }
    return std::nullopt;
}

bool DrawPipeline::Overlaps(std::uint64_t address, std::size_t bytes) {
    const auto end = address + bytes;
    if (DrawFinisher::Overlap() && DrawFinisher::Get().PendingOverlaps(address, end)) return true;
    std::lock_guard lock(mutex);
    for (const auto& [range, count] : queuedWrites) {
        if (address < range.second && range.first < end) return true;
    }
    return false;
}

}
