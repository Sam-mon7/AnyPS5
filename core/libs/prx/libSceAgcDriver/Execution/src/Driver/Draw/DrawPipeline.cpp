#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/DrawWriteRanges.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/HandoffSpin.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
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
    std::atomic<std::uint64_t> lockedDrains{0};
    std::atomic<std::uint64_t> fullWaits{0};
    std::atomic<std::uint64_t> fullOvercommits{0};
    std::atomic<std::uint64_t> drainWaitNs{0};
    std::atomic<std::uint64_t> fullWaitNs{0};
    std::atomic<std::uint64_t> idleNs{0};
    std::atomic<std::uint64_t> enqueueNs{0};
};

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
    return depth != 0 ? depth : SwitchDepth;
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
        ArchB::Recorder::RegisterCounter("pipe_drains_locked", counters.lockedDrains);
        ArchB::Recorder::RegisterCounter("pipe_full_waits", counters.fullWaits);
        ArchB::Recorder::RegisterCounter("pipe_full_overcommits", counters.fullOvercommits);
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

void DrawPipeline::FollowEpoch(std::uint64_t token, std::uint64_t ticket) {
    // Write generations: the committer checks at the ticket the issuing thread held when it enqueued
    // the commit (carried explicitly, rule V1/V3), not at a fresh one of its own.
    if (ticket != 0 && GuestMemory::Gen::Active()) GuestMemory::Gen::Follow(ticket);
    static thread_local std::uint64_t followed = 0;
    if (followed == token) return;
    followed = token;
    GuestMemory::BumpCollectEpoch(GuestMemory::Gen::TicketReason::Committer);
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
            resumeAt = capacity - 1;
            ++idleWaiters;
            idle.wait(lock, [&] { return items.size() <= resumeAt; });
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
    std::snprintf(item, sizeof(item), "; %llu indirect draws kept it over their arguments", static_cast<unsigned long long>(counters.keptIndirectArguments.load(std::memory_order_relaxed)));
    text += item;
    text += DrawWriteDiagText();
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
}


void DrawPipeline::run() {
    PerformanceTrace::BindThread("draw committer");
    StartCommitterSampler();
    static std::atomic<std::uint64_t> recorderCommits{0};
    static const bool registered = [] { ArchB::Recorder::RegisterCounter("draw_pipeline_commits", recorderCommits); return true; }();
    static_cast<void>(registered);
    for (;;) {
        Commit* commit = nullptr;
        bool isDraw = false;
        ItemKind kind = ItemKind::Other;
        // pipe_committer_idle_ms: from the end of one item until the next is taken (recorder only).
        const bool idleBefore = ArchB::Recorder::Enabled() && outstanding.load(std::memory_order_acquire) == 0;
        const auto idleStart = idleBefore ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        SpinUntil(HandoffSpinWindow(), [&] { return outstanding.load(std::memory_order_acquire) != 0; });
        {
            std::unique_lock lock(mutex);
            committerWaiting = true;
            wake.wait(lock, [&] { return !items.empty() || closing; });
            committerWaiting = false;
            if (items.empty()) break;
            commit = stopped ? nullptr : &items.front().commit;
            isDraw = items.front().draw;
            kind = items.front().kind;
        }
        const auto start = std::chrono::steady_clock::now();
        if (idleBefore) {
            const auto idleNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(start - idleStart).count());
            pipeCounters().idleNs.fetch_add(idleNs, std::memory_order_relaxed);
            ArchB::Recorder::Count("pipe_committer_idle_ms", static_cast<double>(idleNs) / 1e6);
        }
        std::exception_ptr fatal;
        bool failed = false;
        bool shutdown = false;
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
        if (fatal != nullptr && !stopped) {
            stopped = true;
            if (!shutdown) Driver::Get().ReportFailure(fatal);
        }
        std::lock_guard lock(mutex);
        countWrites(items.front().writes, false);
        retired.push_back(std::move(items.front()));
        items.pop_front();
        retiredItems.fetch_add(1, std::memory_order_release);
        ++commits;
        recorderCommits.fetch_add(1, std::memory_order_relaxed);
        pipeCounters().commits[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
        commitNs += spent;
        if (failed) ++commitErrors;
        if (fatal != nullptr && failure == nullptr) failure = fatal;
        outstanding.fetch_sub(1, std::memory_order_release);
        if (idleWaiters != 0 && (items.empty() || items.size() <= resumeAt)) idle.notify_all();
    }
    runningCommitters.fetch_sub(1, std::memory_order_acq_rel);
}

void DrawPipeline::Stop() {
    static const bool stats = std::getenv("APS5_PIPELINE_STATS") != nullptr || std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (stats && pipeCounters().enqueues.load(std::memory_order_relaxed) != 0) AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", CumulativeText().c_str());
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
    SpinUntil(HandoffSpinWindow() * 2, [&] { return !Busy(); });
    std::unique_lock lock(mutex);
    ++idleWaiters;
    idle.wait(lock, [&] { return items.empty(); });
    --idleWaiters;
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
    for (auto item = items.rbegin(); item != items.rend(); ++item) {
        const bool overlaps = std::any_of(item->writes.begin(), item->writes.end(), [&](const Range& range) { return address < range.second && range.first < end; });
        if (!overlaps) continue;
        if (item->labelBytes.empty() || address < item->labelAddress || end > item->labelAddress + item->labelBytes.size()) return std::nullopt;
        std::uint64_t value = 0;
        std::memcpy(&value, item->labelBytes.data() + (address - item->labelAddress), bytes);
        return value;
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
    const auto retiredNow = retiredItems.load(std::memory_order_relaxed);
    if (retiredNow == watch.retired) return false;
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
