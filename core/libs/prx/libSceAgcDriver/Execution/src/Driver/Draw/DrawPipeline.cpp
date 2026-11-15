#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/CommitCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/ThreadScratch.hpp"
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
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"
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
// Stage 4.1/4.2 (Draw.cpp): speculated pipelines taken and refused.
std::string ResolvePipelineText();
}
#include <string>

namespace AgcDriver::DriverDetail {

namespace {

struct LabelHandoffState {
    DrawPipeline* pipeline = nullptr;
    std::uint64_t serial = 0, expectedSerial = 0, address = 0;
    std::array<std::byte, 8> bytes{};
    std::size_t size = 0;
    bool currentDraw = false, held = false;
    void Release(const char* reason) {
        if (!held) return;
        held = false;
        GuestMemory::GpuMutex().unlock();
        ArchB::Recorder::Count(reason);
    }
    ~LabelHandoffState() { Release("label_handoff_scope_release"); }
};
struct LabelHandoffSlot {};
using CurrentLabelHandoff = HostThreadSlot<LabelHandoffState*, LabelHandoffSlot>;

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

constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::CommitKind::Count)> CommitKindNames{"other", "draw", "indirect", "colormeta", "dispatch", "sampledump", "dmafill", "dmawrite", "dmacopy", "store", "endofpipe", "seal", "submitend", "completion", "bufferfill"};

std::atomic<unsigned> runningCommitters{0};
std::atomic<std::uint64_t> drawKeys{0};
std::atomic<std::uint64_t> drawKeysSameAsPrevious{0};
std::atomic<std::uint64_t> drawKeysSeenRecently{0};
std::atomic<std::uint64_t> drawShapesSameAsPrevious{0};
std::atomic<std::uint64_t> drawShapesSeenRecently{0};

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

std::atomic<std::uint64_t> heldReadRanges{0};
std::atomic<std::uint64_t> heldReadQueries{0};
std::atomic<std::uint64_t> heldReadHits{0};

// Stage 4.1/4.2 (APS5_ARCHB_PIPE_RESOLVE_SPLIT, _POOL): resolves by where they ran, the resolves the
// committer re-ran because an item applied in between wrote one of their ranges (conflicts), the
// committer's waits for a resolver still running its item, the resolve time by thread (resolver i, the
// committer last), the committer's item time while the split is on (apply, waits included), and the
// _VERIFY re-runs whose plan hash differed.
constexpr std::size_t ResolverSlots = 8;
struct ResolveCounters {
    std::atomic<std::uint64_t> items{0};
    std::atomic<std::uint64_t> pooled{0};
    std::atomic<std::uint64_t> inlined{0};
    std::atomic<std::uint64_t> conflicts{0};
    std::atomic<std::uint64_t> waits{0};
    std::atomic<std::uint64_t> waitNs{0};
    std::atomic<std::uint64_t> resolveNs{0};
    std::atomic<std::uint64_t> poolNs{0};
    std::atomic<std::uint64_t> applyNs{0};
    std::atomic<std::uint64_t> applyItems{0};
    std::atomic<std::uint64_t> verified{0};
    std::atomic<std::uint64_t> mismatches{0};
    std::array<std::atomic<std::uint64_t>, ResolverSlots + 1> threadNs{};
    std::array<std::atomic<std::uint64_t>, ResolverSlots + 1> threadItems{};
};

ResolveCounters& resolveCounters() {
    static auto* counters = new ResolveCounters();
    return *counters;
}

std::size_t resolverCount() {
    static const std::size_t count = [] {
        const char* value = std::getenv("APS5_PIPE_RESOLVERS");
        const auto parsed = value != nullptr ? std::strtoull(value, nullptr, 10) : 3ull;
        return static_cast<std::size_t>(std::clamp<unsigned long long>(parsed, 1ull, ResolverSlots));
    }();
    return count;
}

bool resolveVerify() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_PIPE_RESOLVE_SPLIT_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

std::size_t& resolverIndexThreadState() {
    struct State { std::size_t value = ResolverSlots; };
    struct Slot {};
    return AgcDriver::ThreadScratch<State, Slot>().value;
}

// Applied items' writes kept for the conflict check (a resolve older than the oldest kept counts as
// conflicting).
constexpr std::size_t AppliedKept = 512;

void runResolveSlot(DrawPipeline::ResolveSlot& slot) {
    const auto start = std::chrono::steady_clock::now();
    slot.reads.clear();
    slot.plan = slot.run(slot.reads);
    slot.ns = elapsedNs(start);
    auto& counters = resolveCounters();
    counters.items.fetch_add(1, std::memory_order_relaxed);
    counters.resolveNs.fetch_add(slot.ns, std::memory_order_relaxed);
    counters.threadNs[resolverIndexThreadState()].fetch_add(slot.ns, std::memory_order_relaxed);
    counters.threadItems[resolverIndexThreadState()].fetch_add(1, std::memory_order_relaxed);
}

template<typename TItem>
struct ClearOnExit {
    std::vector<TItem>& items;
    bool census = false;
    ~ClearOnExit() {
        if (!census) { items.clear(); return; }
        PerformanceTimer timing("J3.EnqueueReclaim");
        ArchB::Recorder::Count("j3b_enqueue_reclaimed_items", static_cast<double>(items.size()));
        items.clear();
    }
};

}

namespace {
struct FinisherThreadSlot {};
struct FinisherProducerSlot {};
bool finisherThread() { return HostThreadSlot<bool, FinisherThreadSlot>::Get(); }
void finisherThread(bool value) { HostThreadSlot<bool, FinisherThreadSlot>::Set(value); }
bool finisherProducer() { return HostThreadSlot<bool, FinisherProducerSlot>::Get(); }
void finisherProducer(bool value) { HostThreadSlot<bool, FinisherProducerSlot>::Set(value); }
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
    if (!Busy() || !finisherProducer()) return false;
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
    if (!Busy() || !finisherProducer()) return LabelLookup::NotHeld;
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
    if (!Busy() || !finisherProducer()) return false;
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
    if (!Busy() || !finisherProducer()) return false;
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
    return finisherThread();
}

void DrawFinisher::Enqueue(Job job) {
    Enqueue(std::move(job), Pending{});
}

void DrawFinisher::Enqueue(Job job, Pending held) {
    finisherProducer(true);
    constexpr std::size_t depth = 16;
    struct ReclaimStorage {};
    auto& reclaim = HostThreadLocal<std::vector<Job>, ReclaimStorage>();
    const ClearOnExit<Job> clearReclaim{reclaim};
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
            finisherThread(true);
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
    struct ItemsStorage {};
    return AgcDriver::ThreadScratch<bool, ItemsStorage>();
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
        auto& resolve = resolveCounters();
        ArchB::Recorder::RegisterCounter("pipe_resolve_items", resolve.items);
        ArchB::Recorder::RegisterCounter("pipe_resolve_pool_items", resolve.pooled);
        ArchB::Recorder::RegisterCounter("pipe_resolve_inline", resolve.inlined);
        ArchB::Recorder::RegisterCounter("pipe_resolve_conflicts", resolve.conflicts);
        ArchB::Recorder::RegisterCounter("pipe_resolve_waits", resolve.waits);
        ArchB::Recorder::RegisterCounter("pipe_resolve_wait_ns", resolve.waitNs);
        ArchB::Recorder::RegisterCounter("pipe_resolve_ns", resolve.resolveNs);
        ArchB::Recorder::RegisterCounter("pipe_resolve_pool_ns", resolve.poolNs);
        ArchB::Recorder::RegisterCounter("pipe_apply_ns", resolve.applyNs);
        ArchB::Recorder::RegisterCounter("pipe_apply_items", resolve.applyItems);
        ArchB::Recorder::RegisterCounter("pipe_resolve_verified", resolve.verified);
        ArchB::Recorder::RegisterCounter("pipe_resolve_verify_mismatches", resolve.mismatches);
        ArchB::Recorder::RegisterCounter("pipe_committer_tracker_waits", GuestMemory::CommitterTrackerWaits());
        ArchB::Recorder::RegisterCounter("pipe_committer_tracker_wait_ns", GuestMemory::CommitterTrackerWaitNs());
        ArchB::Recorder::RegisterCounter("pipe_resolver_tracker_waits", GuestMemory::ResolverTrackerWaits());
        ArchB::Recorder::RegisterCounter("pipe_resolver_tracker_wait_ns", GuestMemory::ResolverTrackerWaitNs());
        static constexpr std::array<const char*, ResolverSlots + 1> threadNames{"pipe_resolver0_ns", "pipe_resolver1_ns", "pipe_resolver2_ns", "pipe_resolver3_ns", "pipe_resolver4_ns", "pipe_resolver5_ns", "pipe_resolver6_ns", "pipe_resolver7_ns", "pipe_resolve_committer_ns"};
        for (std::size_t i = 0; i < threadNames.size(); ++i) ArchB::Recorder::RegisterCounter(threadNames[i], resolve.threadNs[i]);
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
    struct ActiveStorage {};
    return AgcDriver::ThreadScratch<bool, ActiveStorage>();
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
    struct FollowedThreadStorage {};
    auto& followed = AgcDriver::ThreadScratch<std::uint64_t, FollowedThreadStorage>();
    if (followed == token) return;
    followed = token;
    pipeCounters().followBumps.fetch_add(1, std::memory_order_relaxed);
    GuestMemory::BumpCollectEpoch(GuestMemory::Gen::TicketReason::Committer);
    GuestMemory::FollowCollectShare(epochClock(token));
}

void DrawPipeline::FollowResolveEpoch(std::uint64_t token) {
    if (resolverIndexThreadState() == ResolverSlots) return;
    struct FollowedThreadStorage {};
    auto& followed = AgcDriver::ThreadScratch<std::uint64_t, FollowedThreadStorage>();
    if (followed == token) return;
    followed = token;
    GuestMemory::BumpCollectEpoch();
}

bool DrawPipeline::ResolveSplitActive() {
    return ArchB::Switches::Enabled(ArchB::Feature::PipeResolveSplit);
}

bool DrawPipeline::ResolvePoolActive() {
    return ArchB::Switches::Enabled(ArchB::Feature::PipeResolvePool) && ArchB::Switches::Enabled(ArchB::Feature::PipeResolveSplit);
}

std::string DrawPipeline::ResolveText() {
    auto& counters = resolveCounters();
    const auto get = [](const std::atomic<std::uint64_t>& value) { return static_cast<unsigned long long>(value.load(std::memory_order_relaxed)); };
    char line[512];
    std::snprintf(line, sizeof(line), "[pipe-resolve] cumulative: resolves %llu (pool %llu, inline %llu), conflicts %llu, waits %llu (%.1f ms), resolve %.1f ms, apply %.1f ms over %llu items, verified %llu, mismatches %llu; by thread ms:", get(counters.items), get(counters.pooled), get(counters.inlined), get(counters.conflicts), get(counters.waits), static_cast<double>(get(counters.waitNs)) / 1e6, static_cast<double>(get(counters.resolveNs)) / 1e6, static_cast<double>(get(counters.applyNs)) / 1e6, get(counters.applyItems), get(counters.verified), get(counters.mismatches));
    std::string text = line;
    for (std::size_t i = 0; i <= ResolverSlots; ++i) {
        if (get(counters.threadItems[i]) == 0) continue;
        std::snprintf(line, sizeof(line), " %s=%.1f/%llu", i == ResolverSlots ? "committer" : ("r" + std::to_string(i)).c_str(), static_cast<double>(get(counters.threadNs[i])) / 1e6, get(counters.threadItems[i]));
        text += line;
    }
    return text;
}

// Under `mutex`.
void DrawPipeline::startResolvers() {
    const auto count = resolverCount();
    resolvers.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        resolvers.emplace_back([this, i] {
            resolverIndexThreadState() = i;
            PerformanceTrace::BindThread("draw resolver");
            GuestMemory::MarkResolverThread();
            runResolver();
        });
    }
}

// A resolver takes the oldest queued item whose resolve nobody claimed, past the front item (which the
// committer takes next or is applying; a hand-off there would only add latency), and resolves it with
// the count of items applied so far: the items from there up to it are the ones whose writes the
// committer checks against the resolve's ranges.
void DrawPipeline::runResolver() {
    const auto claimable = [this]() -> std::shared_ptr<ResolveSlot> {
        for (std::size_t index = 1; index < items.size(); ++index) {
            auto& slot = items[index].resolve;
            if (slot != nullptr && slot->state.load(std::memory_order_acquire) == 0) return slot;
        }
        return nullptr;
    };
    for (;;) {
        std::shared_ptr<ResolveSlot> slot;
        {
            std::unique_lock lock(mutex);
            resolveWake.wait(lock, [&] { return closing || (slot = claimable()) != nullptr; });
            if (closing) break;
            std::uint8_t expected = 0;
            if (!slot->state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) continue;
            slot->startedAt = retiredItems.load(std::memory_order_relaxed);
            if (claimable() != nullptr) resolveWake.notify_one();
        }
        runResolveSlot(*slot);
        resolveCounters().poolNs.fetch_add(slot->ns, std::memory_order_relaxed);
        {
            std::lock_guard lock(mutex);
            slot->state.store(2, std::memory_order_release);
        }
        resolveDone.notify_all();
    }
}

// Under `mutex`: the writes of the item applied as number `serial` (its label store included).
void DrawPipeline::noteApplied(const Item& item, std::uint64_t serial) {
    Applied entry{serial, item.writes};
    if (item.labelAddress != 0 && !item.labelBytes.empty()) entry.writes.emplace_back(item.labelAddress, item.labelAddress + item.labelBytes.size());
    applied.push_back(std::move(entry));
    while (applied.size() > AppliedKept) applied.pop_front();
}

// Under `mutex`: whether an item applied after the resolve began and before item `serial` wrote one of
// the resolve's ranges (or such an item is no longer known).
bool DrawPipeline::appliedOverlap(const ResolveSlot& slot, std::uint64_t serial) {
    if (slot.startedAt >= serial) return false;
    if (applied.empty() || applied.front().serial > slot.startedAt) return true;
    for (auto entry = applied.rbegin(); entry != applied.rend() && entry->serial >= slot.startedAt; ++entry) {
        if (entry->serial >= serial) continue;
        for (const auto& write : entry->writes) {
            for (const auto& read : slot.reads) {
                if (read.first < write.second && write.first < read.second) return true;
            }
        }
    }
    return false;
}

// The committer before applying item `serial`: resolves it inline unless a resolver claimed it, waits
// for a running resolve, and re-resolves inline when an item applied meanwhile wrote one of its ranges.
void DrawPipeline::resolveForApply(ResolveSlot& slot, std::uint64_t serial) {
    auto& counters = resolveCounters();
    std::uint8_t expected = 0;
    if (slot.state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        slot.startedAt = serial;
        runResolveSlot(slot);
        slot.state.store(2, std::memory_order_release);
        counters.inlined.fetch_add(1, std::memory_order_relaxed);
    } else {
        if (slot.state.load(std::memory_order_acquire) != 2) {
            const auto waitStart = std::chrono::steady_clock::now();
            SpinUntil(HandoffSpinWindow(), [&] { return slot.state.load(std::memory_order_acquire) == 2; });
            if (slot.state.load(std::memory_order_acquire) != 2) {
                std::unique_lock lock(mutex);
                resolveDone.wait(lock, [&] { return slot.state.load(std::memory_order_acquire) == 2; });
            }
            counters.waits.fetch_add(1, std::memory_order_relaxed);
            counters.waitNs.fetch_add(elapsedNs(waitStart), std::memory_order_relaxed);
        }
        bool conflict = false;
        {
            std::lock_guard lock(mutex);
            conflict = appliedOverlap(slot, serial);
        }
        if (conflict) {
            counters.conflicts.fetch_add(1, std::memory_order_relaxed);
            slot.startedAt = serial;
            runResolveSlot(slot);
        } else counters.pooled.fetch_add(1, std::memory_order_relaxed);
    }
    if (!resolveVerify()) return;
    const auto plan = slot.plan;
    runResolveSlot(slot);
    counters.verified.fetch_add(1, std::memory_order_relaxed);
    if (slot.plan != plan) {
        const auto count = counters.mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((count & (count - 1)) == 0) std::fprintf(stderr, "[pipe-resolve] VERIFY: a re-resolved plan differs (%llu so far)\n", static_cast<unsigned long long>(count));
    }
}

unsigned DrawPipeline::Committers() {
    return runningCommitters.load(std::memory_order_acquire);
}

void DrawPipeline::rethrowFailure() {
    if (failure == nullptr) return;
    auto error = std::exchange(failure, nullptr);
    std::rethrow_exception(error);
}

void DrawPipeline::Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress, std::vector<std::byte> labelBytes, std::vector<Range> reads, bool draw, ItemKind kind, std::shared_ptr<ResolveSlot> resolve) {
    Item item{std::move(commit), std::move(writes), labelAddress, std::move(labelBytes), std::move(reads), draw};
    item.kind = kind;
    item.resolve = std::move(resolve);
    enqueue(std::move(item));
}

void DrawPipeline::Enqueue(CommitKind kind, Commit commit, std::vector<Range> writes, std::uint64_t labelAddress, std::vector<std::byte> labelBytes, std::vector<Range> reads, bool draw, ItemKind itemKind, std::shared_ptr<ResolveSlot> resolve) {
    Item item{std::move(commit), std::move(writes), labelAddress, std::move(labelBytes), std::move(reads), draw};
    item.commitKind = kind;
    item.kind = itemKind;
    item.resolve = std::move(resolve);
    if (item.kind == ItemKind::Other && (kind == CommitKind::Draw || kind == CommitKind::IndirectDraw)) item.kind = ItemKind::Draw;
    else if (item.kind == ItemKind::Other && kind == CommitKind::Dispatch) item.kind = ItemKind::Dispatch;
    else if (item.kind == ItemKind::Other && (kind == CommitKind::DmaFill || kind == CommitKind::DmaWrite || kind == CommitKind::DmaCopy)) item.kind = ItemKind::Dma;
    else if (item.kind == ItemKind::Other && kind == CommitKind::BufferFill) item.kind = ItemKind::Fill;
    else if (item.kind == ItemKind::Other && (kind == CommitKind::Store || kind == CommitKind::EndOfPipe)) item.kind = ItemKind::Label;
    enqueue(std::move(item));
}

void DrawPipeline::NoteDrawKey(std::uint64_t key, std::uint64_t shape) {
    constexpr std::size_t RecentSlots = 4096;
    struct RecentThreadStorage {};
    auto& recent = AgcDriver::ThreadScratch<std::array<std::uint64_t, RecentSlots>, RecentThreadStorage>();
    struct PreviousThreadStorage {};
    auto& previous = AgcDriver::ThreadScratch<std::uint64_t, PreviousThreadStorage>();
    struct RecentShapesThreadStorage {};
    auto& recentShapes = AgcDriver::ThreadScratch<std::array<std::uint64_t, RecentSlots>, RecentShapesThreadStorage>();
    struct PreviousShapeThreadStorage {};
    auto& previousShape = AgcDriver::ThreadScratch<std::uint64_t, PreviousShapeThreadStorage>();
    const auto shapeSlot = static_cast<std::size_t>((shape ^ (shape >> 29u)) % RecentSlots);
    const auto slot = static_cast<std::size_t>((key ^ (key >> 29u)) % RecentSlots);
    drawKeys.fetch_add(1, std::memory_order_relaxed);
    if (key == previous) drawKeysSameAsPrevious.fetch_add(1, std::memory_order_relaxed);
    if (recent[slot] == key) drawKeysSeenRecently.fetch_add(1, std::memory_order_relaxed);
    if (shape == previousShape) drawShapesSameAsPrevious.fetch_add(1, std::memory_order_relaxed);
    if (recentShapes[shapeSlot] == shape) drawShapesSeenRecently.fetch_add(1, std::memory_order_relaxed);
    recent[slot] = key;
    previous = key;
    recentShapes[shapeSlot] = shape;
    previousShape = shape;
}

void DrawPipeline::EnqueueStore(Commit commit, std::uint64_t address, std::vector<std::byte> bytes) {
    const auto end = address + bytes.size();
    Item item{std::move(commit), {{address, end}}, address, std::move(bytes)};
    item.store = true;
    item.commitKind = CommitKind::Store;
    item.kind = ItemKind::Label;
    enqueue(std::move(item));
}

void DrawPipeline::enqueue(Item item) {
    const bool recorder = ArchB::Recorder::Enabled();
    const auto enqueueStart = recorder ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (DrawFinisher::Busy() && !DrawFinisher::OnFinisherThread()) DrawFinisher::Get().Drain();
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    struct ReclaimStorage {};
    auto& reclaim = HostThreadLocal<std::vector<Item>, ReclaimStorage>();
    const bool census = ArchB::Switches::Enabled(ArchB::Feature::DrawHitCensus);
    const ClearOnExit<Item> clearReclaim{reclaim, census};
    const auto lockStart = profile || census ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::unique_lock lock(mutex);
    const auto lockNs = profile || census ? elapsedNs(lockStart) : 0;
    if (!retired.empty()) std::swap(retired, reclaim);
    if (profile) lockWaitNs += elapsedNs(lockStart);
    if (census) {
        ArchB::Recorder::Count("j3b_enqueue_mutex_wait_ns", static_cast<double>(lockNs));
        ArchB::Recorder::Count("j3b_enqueue_calls");
        if (item.kind == ItemKind::Draw) ArchB::Recorder::Count("j3b_enqueue_draw_calls");
    }
    rethrowFailure();
    if (closing) {
        lock.unlock();
        if (item.resolve != nullptr) runResolveSlot(*item.resolve);
        item.commit();
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
    heldReadRanges.fetch_add(item.reads.size(), std::memory_order_relaxed);
    countWrites(item.writes, true);
    const bool pooledResolve = item.resolve != nullptr && ResolvePoolActive();
    if (census) item.censusArrival = items.empty() ? 1 : 2;
    items.push_back(std::move(item));
    static const bool aheadCensus = GuestMemory::CollectAheadCensus();
    if (aheadCensus && items.back().kind == ItemKind::Draw) {
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
    if (pooledResolve) {
        if (resolvers.empty()) startResolvers();
        if (items.size() > 1) resolveWake.notify_one();
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
    AgcDriver::ProfilePrint_nid_no_patch("[draw] pipelined (10 s, depth %zu): %llu commits at %.1f us each on the committer, %llu failed; %llu enqueued behind %.2f on average, waiting %.1f us per draw for room, %.2f us for the lock; %llu committer wakes;%s; drains:%s\n", Capacity(), static_cast<unsigned long long>(commits), perCommit, static_cast<unsigned long long>(commitErrors), static_cast<unsigned long long>(enqueued), queued, perDraw, enqueued == 0 ? 0.0 : static_cast<double>(lockWaitNs) / 1e3 / static_cast<double>(enqueued), static_cast<unsigned long long>(wakes), reads, drainText.c_str());
    std::string kindText;
    for (std::size_t i = 0; i < kindCommits.size(); ++i) {
        const auto count = kindCommits[i].exchange(0, std::memory_order_relaxed);
        const auto ns = kindNs[i].exchange(0, std::memory_order_relaxed);
        if (count == 0) continue;
        char item[64];
        std::snprintf(item, sizeof(item), " %s %llu (%.1f us)", CommitKindNames[i], static_cast<unsigned long long>(count), static_cast<double>(ns) / 1e3 / static_cast<double>(count));
        kindText += item;
    }
    AgcDriver::ProfilePrint_nid_no_patch("[commits] by kind (10 s):%s\n", kindText.c_str());
    const auto vk = Graphics::TakeVkDrawCounts();
    AgcDriver::ProfilePrint_nid_no_patch("[vkdraw] (10 s): %llu vkCmdDraw, %llu vkCmdDrawIndexed, %llu vkCmdDrawIndirect calls, %llu mesh; draw keys %llu, same as the previous draw %llu, seen within the last 4096 draws %llu; draw shapes (state without per-draw user words) same as the previous %llu, seen within the last 4096 %llu\n", static_cast<unsigned long long>(vk.direct), static_cast<unsigned long long>(vk.indexed), static_cast<unsigned long long>(vk.indirect), static_cast<unsigned long long>(vk.mesh), static_cast<unsigned long long>(drawKeys.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(drawKeysSameAsPrevious.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(drawKeysSeenRecently.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(drawShapesSameAsPrevious.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(drawShapesSeenRecently.exchange(0, std::memory_order_relaxed)));
    commits = commitNs = commitErrors = enqueued = fullWaitNs = depthSum = wakes = lockWaitNs = 0;
    drains = {};
    drainWaitNs = {};
    AgcDriver::ProfilePrint_nid_no_patch("%s\n", CumulativeText().c_str());
    if (resolveCounters().items.load(std::memory_order_relaxed) != 0) AgcDriver::ProfilePrint_nid_no_patch("%s\n%s\n", ResolveText().c_str(), Graphics::ResolvePipelineText().c_str());
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

void DrawPipeline::RetainGpuMutexForLabel() {
    if (!ArchB::Switches::Enabled(ArchB::Feature::FixLabelHandoff) || std::uncaught_exceptions() != 0) return;
    auto* state = CurrentLabelHandoff::Get();
    if (state == nullptr || !state->currentDraw || state->held || GuestMemory::GpuMutex().DepthOnThisThread() != 1) return;
    auto& pipeline = *state->pipeline;
    std::unique_lock lock(pipeline.mutex, std::try_to_lock);
    if (!lock.owns_lock()) { ArchB::Recorder::Count("label_handoff_peek_busy"); return; }
    if (pipeline.closing || pipeline.stopped || pipeline.items.size() < 2) { ArchB::Recorder::Count("label_handoff_no_successor"); return; }
    const auto& next = pipeline.items[1];
    if (next.kind != ItemKind::Label || next.commitKind != CommitKind::EndOfPipe || next.resolve != nullptr || (next.labelBytes.size() != 4 && next.labelBytes.size() != 8) || next.labelAddress % 4 != 0) {
        ArchB::Recorder::Count("label_handoff_other_successor");
        return;
    }
    state->expectedSerial = state->serial + 1;
    state->address = next.labelAddress;
    state->size = next.labelBytes.size();
    std::copy(next.labelBytes.begin(), next.labelBytes.end(), state->bytes.begin());
    GuestMemory::GpuMutex().lock();
    state->held = true;
    ArchB::Recorder::Count("label_handoff_retained");
}

bool DrawPipeline::ConsumeLabelGpuMutex(std::uint32_t queue, std::uint64_t address, std::span<const std::byte> bytes) {
    auto* state = CurrentLabelHandoff::Get();
    if (state == nullptr || !state->held) return false;
    const bool match = queue == 0 && !state->currentDraw && state->serial == state->expectedSerial && address == state->address && bytes.size() == state->size && std::equal(bytes.begin(), bytes.end(), state->bytes.begin()) && GuestMemory::GpuMutex().DepthOnThisThread() == 1;
    if (!match || !ArchB::Switches::Enabled(ArchB::Feature::FixLabelHandoff)) {
        state->Release("label_handoff_refused");
        return false;
    }
    ArchB::Recorder::Count("label_handoff_consumed");
    static const bool verify = [] { const auto* value = std::getenv("APS5_ARCHB_FIX_LABEL_HANDOFF_VERIFY"); return value != nullptr && std::strcmp(value, "0") != 0; }();
    if (verify) ArchB::Recorder::Count("label_handoff_plan_verified");
    state->held = false;
    return true;
}

void DrawPipeline::run() {
    CommitCensus::Sequence sequence;
    LabelHandoffState handoff;
    handoff.pipeline = this;
    CurrentLabelHandoff::Set(&handoff);
    struct ClearCurrentHandoff {
        ~ClearCurrentHandoff() { CurrentLabelHandoff::Set(nullptr); }
    } clearCurrentHandoff;
    const auto lockQueue = [&] {
        std::unique_lock lock(mutex, std::defer_lock);
        if (handoff.held && !lock.try_lock()) handoff.Release("label_handoff_queue_contended");
        if (!lock.owns_lock()) lock.lock();
        return lock;
    };
    PerformanceTrace::BindThread("draw committer");
    GuestMemory::MarkCommitterThread();
    Stage4::BindRole(Stage4::Role::Committer);
    GuestMemory::EnableHoldMarks();
    // Stage 4 M4 (APS5_RESOLVE_CENSUS): the item's declared reads and writes, copied under the mutex
    // (ReleaseRunningReads clears the running item's reads during its commit).
    std::vector<Range> stage4Reads;
    std::vector<Range> stage4Writes;
    StartCommitterSampler();
    static std::atomic<std::uint64_t> recorderCommits{0};
    static const bool registered = [] { ArchB::Recorder::RegisterCounter("draw_pipeline_commits", recorderCommits); return true; }();
    static_cast<void>(registered);
    for (;;) {
        Commit* commit = nullptr;
        bool isDraw = false;
        ItemKind kind = ItemKind::Other;
        CommitKind commitKind = CommitKind::Other;
        std::uint64_t enqueuedNs = 0;
        std::uint64_t enqueueClock = 0;
        std::shared_ptr<ResolveSlot> resolveSlot;
        std::uint64_t serial = 0;
        std::uint8_t arrival = 0;
        bool onlyItem = false;
        bool census = false;
        GuestMemory::HoldLogMark(GuestMemory::HoldMark::ItemTop);
        // pipe_committer_idle_ms: from the end of one item until the next is taken (recorder only).
        if (handoff.held && outstanding.load(std::memory_order_acquire) == 0) handoff.Release("label_handoff_idle_release");
        const bool idleBefore = ArchB::Recorder::Enabled() && outstanding.load(std::memory_order_acquire) == 0;
        const auto idleStart = idleBefore ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        // Stage 4 M1: the committer idles (spins, then sleeps until queue 0 enqueues).
        std::optional<Stage4::WaitScope> stage4Idle;
        if (Stage4::Tracking() && outstanding.load(std::memory_order_acquire) == 0) stage4Idle.emplace(Stage4::Wait::Idle, true);
        SpinUntil(HandoffSpinWindow(), [&] { return outstanding.load(std::memory_order_acquire) != 0; });
        {
            auto lock = lockQueue();
            if (handoff.held && (items.empty() || stopped || closing || items.front().kind != ItemKind::Label || items.front().commitKind != CommitKind::EndOfPipe || items.front().resolve != nullptr || retiredItems.load(std::memory_order_relaxed) != handoff.expectedSerial)) {
                lock.unlock();
                handoff.Release("label_handoff_successor_release");
                lock.lock();
            }
            if (stage4Idle && items.empty() && !closing) stage4Idle->Blocking();
            committerWaiting = true;
            wake.wait(lock, [&] { return !items.empty() || closing; });
            committerWaiting = false;
            if (items.empty()) break;
            census = ArchB::Switches::Enabled(ArchB::Feature::DrawHitCensus);
            commit = stopped ? nullptr : &items.front().commit;
            isDraw = items.front().draw;
            kind = items.front().kind;
            commitKind = items.front().commitKind;
            enqueuedNs = items.front().enqueuedNs;
            enqueueClock = items.front().enqueueClock;
            resolveSlot = items.front().resolve;
            serial = retiredItems.load(std::memory_order_relaxed);
            if (census) { arrival = items.front().censusArrival; onlyItem = items.size() == 1; }
            if (Stage4::ResolveCensus()) {
                stage4Reads.assign(items.front().reads.begin(), items.front().reads.end());
                stage4Writes.assign(items.front().writes.begin(), items.front().writes.end());
            }
        }
        handoff.serial = serial;
        handoff.currentDraw = kind == ItemKind::Draw;
        stage4Idle.reset();
        GuestMemory::HoldLogMark(GuestMemory::HoldMark::ItemTaken);
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
        GuestMemory::HoldLogMark(GuestMemory::HoldMark::CommitBegin);
        std::optional<CommitCensus> commitCensus;
        try {
            if (commit == nullptr || resolveSlot != nullptr) handoff.Release("label_handoff_skipped_release");
            if (commit != nullptr && resolveSlot != nullptr) {
                std::optional<PerformanceTimer> resolveTiming;
                if (census) resolveTiming.emplace("J3.CommitResolve");
                resolveForApply(*resolveSlot, serial);
            }
            if (commit != nullptr) {
                using Kind = CommitCensus::Kind;
                const auto classified = kind == ItemKind::Draw ? Kind::Legacy : kind == ItemKind::Label ? Kind::Label : commitKind == CommitKind::DmaCopy ? Kind::Copy : (commitKind == CommitKind::DmaFill || commitKind == CommitKind::DmaWrite || commitKind == CommitKind::BufferFill) ? Kind::Fill : kind == ItemKind::Dispatch ? Kind::Dispatch : Kind::Other;
                commitCensus.emplace(classified);
                if (census && kind == ItemKind::Label) ArchB::Recorder::Count(commitKind == CommitKind::Store ? "j3c_label_store_items" : "j3c_label_eop_items");
                if (census && commitKind == CommitKind::DmaWrite) ArchB::Recorder::Count("j3c_inline_dma_writes");
                (*commit)();
                commitCensus->Finish();
            } else if (census) ArchB::Recorder::Count("j3c_stopped_items");
            GuestMemory::HoldLogMark(GuestMemory::HoldMark::CommitEnd);
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
        if (fatal != nullptr || !handoff.currentDraw) handoff.Release("label_handoff_unused_release");
        if (commitCensus) commitCensus->Finish(fatal != nullptr);
        if (census && commitCensus) commitCensus->RecordSequence(sequence, arrival, onlyItem, fatal != nullptr);
        else sequence.predecessor = CommitCensus::Kind::Count;
        const auto spent = elapsedNs(start);
        if (resolveSlot != nullptr) {
            resolveCounters().applyNs.fetch_add(spent, std::memory_order_relaxed);
            resolveCounters().applyItems.fetch_add(1, std::memory_order_relaxed);
        }
        if (Stage4::ResolveCensus()) Stage4::EndItem(Graphics::StorageTexture::PendingSerial(), !isDraw);
        if (enqueuedNs != 0) countAheadWindow(enqueuedNs, enqueueClock, start);
        if (fatal != nullptr && !stopped) {
            stopped = true;
            if (!shutdown) Driver::Get().ReportFailure(fatal);
        }
        auto lock = lockQueue();
        countWrites(items.front().writes, false);
        if (!resolvers.empty()) noteApplied(items.front(), serial);
        const auto kindIndex = static_cast<std::size_t>(items.front().commitKind);
        kindCommits[kindIndex].fetch_add(1, std::memory_order_relaxed);
        kindNs[kindIndex].fetch_add(spent, std::memory_order_relaxed);
        retired.push_back(std::move(items.front()));
        items.pop_front();
        sequence.emptyAfterPredecessor = items.empty();
        retiredItems.fetch_add(1, std::memory_order_release);
        ++commits;
        recorderCommits.fetch_add(1, std::memory_order_relaxed);
        pipeCounters().commits[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
        commitNs += spent;
        if (failed) ++commitErrors;
        if (fatal != nullptr && failure == nullptr) failure = fatal;
        outstanding.fetch_sub(1, std::memory_order_release);
        if (idleWaiters != 0 && (items.empty() || items.size() <= resumeAt)) idle.notify_all();
        GuestMemory::HoldLogMark(GuestMemory::HoldMark::ItemDone);
    }
    runningCommitters.fetch_sub(1, std::memory_order_acq_rel);
}

void DrawPipeline::Stop() {
    static const bool stats = std::getenv("APS5_PIPELINE_STATS") != nullptr || std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (stats && pipeCounters().enqueues.load(std::memory_order_relaxed) != 0) {
        AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", CumulativeText().c_str());
        if (resolveCounters().items.load(std::memory_order_relaxed) != 0) AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n%s (at shutdown)\n", ResolveText().c_str(), Graphics::ResolvePipelineText().c_str());
        AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", Graphics::ShaderResources::DrawSetPoolText().c_str());
        AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", Graphics::ShaderResources::DrawReuseText().c_str());
        if (const auto fixes = Graphics::ShaderResources::SnapshotFixesText(); !fixes.empty()) AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", fixes.c_str());
        if (const auto diag = Graphics::ShaderResources::DrawBindingsDiagText(); !diag.empty()) AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", diag.c_str());
        if (const auto diag = Graphics::DrawLookupDiagText(); !diag.empty()) AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", diag.c_str());
        AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", Graphics::DrawLookupMemoText().c_str());
        AgcDriver::ProfilePrint_nid_no_patch("%s (at shutdown)\n", Graphics::PipelineLookupMemoText().c_str());
    }
    std::thread committer;
    std::vector<std::thread> stoppedResolvers;
    {
        std::lock_guard lock(mutex);
        closing = true;
        committer = std::move(thread);
        stoppedResolvers = std::move(resolvers);
        resolvers.clear();
    }
    wake.notify_all();
    resolveWake.notify_all();
    resolveDone.notify_all();
    for (auto& resolver : stoppedResolvers) {
        if (resolver.get_id() == std::this_thread::get_id()) resolver.detach();
        else if (resolver.joinable()) resolver.join();
    }
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
    reasonRequests[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
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
    if (stage4Wait && !items.empty()) stage4Wait->Blocking();
    ++idleWaiters;
    idle.wait(lock, [&] { return items.empty(); });
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
    return false;
}

std::uint64_t DrawPipeline::HeldReadQueries() {
    return heldReadQueries.load(std::memory_order_relaxed);
}

void DrawPipeline::ReleaseRunningReads() {
    std::lock_guard lock(mutex);
    if (!items.empty()) items.front().reads.clear();
}

std::uint64_t DrawPipeline::DrainRequestsByReason(DrainReason reason) {
    return Queue0().reasonRequests[static_cast<std::size_t>(reason)].load(std::memory_order_relaxed);
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

DrawPipeline::QueuedWrite DrawPipeline::StoredBytes(std::uint64_t address, std::size_t bytes, std::span<std::byte> out) {
    const auto end = address + bytes;
    if (DrawFinisher::Overlap() && DrawFinisher::Get().PendingOverlaps(address, end)) return QueuedWrite::Unknown;
    auto answer = QueuedWrite::None;
    std::lock_guard lock(mutex);
    for (auto item = items.rbegin(); item != items.rend(); ++item) {
        if (std::none_of(item->writes.begin(), item->writes.end(), [&](const Range& range) { return address < range.second && range.first < end; })) continue;
        if (!item->store) return QueuedWrite::Unknown;
        if (out.empty()) {
            answer = QueuedWrite::Stored;
            continue;
        }
        if (out.size() != bytes || address < item->labelAddress || end > item->labelAddress + item->labelBytes.size()) return QueuedWrite::Unknown;
        std::memcpy(out.data(), item->labelBytes.data() + (address - item->labelAddress), bytes);
        return QueuedWrite::Stored;
    }
    return answer;
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
    return outstanding.load(std::memory_order_acquire) != 0 || (finisherProducer() && DrawFinisher::Busy() && DrawFinisher::Overlap());
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
