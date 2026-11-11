#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPIPELINE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPIPELINE_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

class DrawPipeline {
public:
    using Commit = std::function<void()>;
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    struct Watch {
        std::uint64_t retired = 0;
        std::size_t items = 0;
        std::vector<Range> writes;
    };
    // Transition: the first queue-0 submission after APS5_ARCHB_PIPE_Q0_COMMITTER turned off drains
    // what the pipelined submissions before it left queued.
    enum class DrainReason : std::uint8_t { Packet, Flush, Labels, Capture, Indirect, Submission, Transition, Count };
    enum class QueuedWrite : std::uint8_t { None, Stored, Unknown };
    enum class CommitKind : std::uint8_t { Other, Draw, IndirectDraw, ColorMetadata, Dispatch, SampleDump, DmaFill, DmaWrite, DmaCopy, Store, EndOfPipe, Seal, SubmitEnd, Completion, BufferFill, Count };
    // What an item commits, for the pipe_commits_* counters only (the `draw` flag of Enqueue keeps
    // its own meaning: a non-draw item breaks proofs after it runs).
    enum class ItemKind : std::uint8_t { Other, Draw, Dispatch, Label, Dma, Fill, Count };
    // Why a drain was needed, finer than DrainReason, for choosing the next drain cut (counters
    // pipe_drain_cause_<name>, counted only when the drain waited). None: not split.
    enum class DrainCause : std::uint8_t {
        None,
        IndirectArgs,          // indirect draw arguments overlap queued writes (writer not found: finisher)
        IndirectArgsDraw,      // ... and the newest overlapping item is a draw
        IndirectArgsDispatch,  // ... a dispatch item
        IndirectArgsLabel,     // ... a label or EOP item
        IndirectArgsOther,     // ... a DMA, fill or other item
        IndirectCount,         // its count word does (arguments clear)
        IndirectDisabled,      // APS5_NO_PIPELINED_INDIRECT
        IndirectCpuRecords,    // CPU-indirect draw: records before the last are drawn inline
        IndirectInline,        // indirect draw (or its last record) drawn inline: labels queued or the mutex held
        RegistersOverlap,      // 0x63/0x64/0x9f table overlaps queued writes (APS5_ARCHB_PIPE_KEEP_INDIRECT_REGS off)
        RegistersInvalid,      // invalid submission-time list read again, table overlaps queued writes
        WaitHeldMismatch,      // 0x3c/0x93: the pipeline holds the awaited label, its value fails the compare
        WaitOverlap,           // awaited memory overlaps queued writes, no held label value
        WaitUnsatisfied,       // no overlap, the compare fails on memory
        DispatchAhead,         // dispatch resolved ahead or resolving (not an item candidate)
        DispatchItemsOff,      // dispatch items off
        DispatchIndirect,      // indirect dispatch arguments
        DispatchLabels,        // labels queued outside the pipeline
        DispatchCopy,          // copy kernel (copied by the driver)
        DispatchMiss,          // candidate: no dispatch-cache entry (or the cache is off / stamps validate)
        DispatchVerify,        // candidate: APS5_VERIFY_DATA_HITS
        DispatchReads,         // candidate: the entry reads memory a queued item writes
        DispatchNoRecipe,      // cached, no recipe
        DispatchWrites,        // cached with a recipe, writes not bounded
        DispatchGate,          // the packet that opened the gate resolves dispatches ahead (items off)
        FillInputs,            // a fill reads its pattern or control from queued writes
        FillsUnordered,        // APS5_PIPELINE_DRAIN_FILLS
        MetadataPass,          // color metadata pass not pipelined
        CaptureVariant,        // a variant's runs overlap queued writes
        CapturePending,        // a capture reads memory a queued item writes
        CaptureShader,         // shader code written by a queued item
        IndirectCpuRead,       // APS5_ARCHB_PIPE_INDIRECT_GPU_ARGS: a CPU-side indirect draw reads arguments queued items write
        IndirectEmptySkip,     // ... the empty-draw skip of a known CPU-side program does
        Count
    };

    // Stage 4.1/4.2 (APS5_ARCHB_PIPE_RESOLVE_SPLIT, _POOL): an item's resolve runs before its commit,
    // inline on the committer (split) or ahead on a resolver thread (pool). `run` fills the ranges its
    // result depends on and returns a plan hash; it never throws and may run again. The committer
    // re-runs it inline when an item applied after the resolve began wrote one of those ranges.
    struct ResolveSlot {
        std::function<std::uint64_t(std::vector<Range>&)> run;
        std::atomic<std::uint8_t> state{0};
        std::uint64_t startedAt = 0;
        std::uint64_t criticalEnqueue = 0;
        std::uint64_t criticalDone = 0;
        std::uint64_t plan = 0;
        std::uint64_t ns = 0;
        std::vector<Range> reads;
    };
    static bool ResolveSplitActive();
    // A resolver thread starts a collect epoch of its own whenever the item it resolves carries a
    // newer epoch token than its last one (no-op on the committer, which follows tokens in commits):
    // its precollects for items of one token then share one memo, like the committer's.
    static void FollowResolveEpoch(std::uint64_t token);
    static bool ResolvePoolActive();
    static std::string ResolveText();

    static DrawPipeline& Queue0();
    // APS5_PIPELINED_DRAWS as read at startup (0 when unset or blocked); see SampleQueue0.
    static std::size_t Depth();
    // Whether the queue-0 submission starting now is pipelined, sampled once at its start. When
    // APS5_PIPELINED_DRAWS is set it alone decides (Depth() != 0, as before the switch existed);
    // otherwise APS5_ARCHB_PIPE_Q0_COMMITTER does (runtime; `viaSwitch` is set), unless one of the
    // variables that force Depth() to 0 is set.
    static bool SampleQueue0(bool& viaSwitch);
    // Items held before Enqueue waits for room: Depth(), or 8 when the switch pipelines.
    static std::size_t Capacity();
    // Whether the calling thread's current submission records eligible dispatches as pipeline items
    // (APS5_PIPELINE_DISPATCH, or the switch); set with Active() at submission start.
    static bool& DispatchItems();
    // Registers the pipe_* recorder counters (once; cheap to call again).
    static void RegisterCounters();
    static void CountSubmission();
    // APS5_ARCHB_PIPE_KEEP_INDIRECT_REGS: a 0x63/0x64/0x9f packet kept the busy pipeline (pipe_keep_indirect_regs).
    static void CountKeptIndirectRegisters();
    // APS5_ARCHB_PIPE_INDIRECT_GPU_ARGS: an indirect draw whose arguments (or count) queued writes
    // overlap was enqueued without draining (pipe_indirect_gpu_args_kept).
    static void CountKeptIndirectArguments();
    static std::uint64_t KeptIndirectArguments();
    // APS5_ARCHB_PIPE_NARROW_DRAW_WRITES: a queued draw's write set is bounded although it has a BDA
    // program (without BDA writes) or an HTILE depth target (unbounded without the switch)
    // (pipe_draw_writes_narrowed).
    static void CountNarrowedDrawWrites();
    // APS5_ARCHB_PIPE_NARROW_SLICED_DEPTH: a queued draw's write set is bounded although its single-sample
    // depth target's view names slices (pipe_sliced_depth_narrowed).
    static void CountNarrowedSlicedDepth();
    // APS5_ARCHB_PIPE_NARROW_DISPATCH_WRITES: a queue-0 dispatch item was enqueued because its BDA program
    // without BDA writes is bounded by its written bindings (pipe_dispatch_writes_narrowed).
    static void CountNarrowedDispatchWrites();
    // APS5_ARCHB_PIPE_KEEP_OWN_LABEL_WAITS: a queue-0 WAIT_REG_MEM whose compare fails on memory kept the
    // busy pipeline because queue 0's own earlier label store satisfies it (pipe_own_label_waits_kept).
    static void CountKeptOwnLabelWait();
    // One "[pipe] cumulative: ..." line (printed with the [draw] pipelined report and at shutdown).
    static std::string CumulativeText();
    // Fallback counts: drains requested while the calling thread held the GPU mutex (waited with the
    // hold given up), and enqueues past the capacity because the thread held the mutex.
    static std::uint64_t LockedDrains();
    static std::uint64_t FullOvercommits();
    static std::size_t QueuedItems();
    static bool& Active();
    static std::atomic<std::uint64_t>& EpochToken();
    // `ticket`: the issuing thread's write-generation ticket carried with the commit (0: none).
    // Several queue-0 bumps between two commits make one committer epoch (pipe_committer_bumps).
    static void FollowEpoch(std::uint64_t token, std::uint64_t ticket = 0);
    // Queue 0 advanced the token to `token` at the ordering point whose collect epoch is `clock`
    // (APS5_ARCHB_PIPE_COLLECT_SHARE: the committer following that token may stand on queue-0 walks
    // made at or after `clock`, GuestMemory::FollowCollectShare).
    static void NoteEpochClock(std::uint64_t token, std::uint64_t clock);
    // Queue 0 bumped its collect epoch (counters pipe_q0_bumps_<cause>).
    enum BumpCause : std::size_t { BumpSubmission, BumpWait, BumpDrain, BumpReap, BumpPacket, BumpCauses };
    static void CountEpochBump(BumpCause cause);
    static unsigned Committers();

    // Never waits while the calling thread holds the GPU mutex: a full pipeline then takes the item
    // past its capacity (the committer needs the mutex to make room).
    void Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress = 0, std::vector<std::byte> labelBytes = {}, std::vector<Range> reads = {}, bool draw = false, ItemKind kind = ItemKind::Other, std::shared_ptr<ResolveSlot> resolve = nullptr);
    void Enqueue(CommitKind kind, Commit commit, std::vector<Range> writes, std::uint64_t labelAddress = 0, std::vector<std::byte> labelBytes = {}, std::vector<Range> reads = {}, bool draw = false, ItemKind itemKind = ItemKind::Other, std::shared_ptr<ResolveSlot> resolve = nullptr);
    void EnqueueStore(Commit commit, std::uint64_t address, std::vector<std::byte> bytes);
    static void NoteDrawKey(std::uint64_t key, std::uint64_t shape);
    // Waits until every queued item has committed. A caller holding the GPU mutex must drain before
    // taking it; if one does not, the wait runs with its hold given up (counted, see LockedDrains).
    void Drain(DrainReason reason, std::uint32_t opcode = 0x100, DrainCause cause = DrainCause::None);
    void Stop();
    void ReleaseRetired();
    static std::uint64_t DrainRequestsByOpcode(std::uint32_t opcode);
    static std::uint64_t DrainRequestsByReason(DrainReason reason);
    bool Busy() const;
    std::size_t Outstanding() const { return outstanding.load(std::memory_order_acquire); }
    bool Overlaps(std::uint64_t address, std::size_t bytes);
    // The kind of the newest queued item whose writes overlap the range (for drain causes only), and
    // its first overlapping write range (diagnostics).
    std::optional<ItemKind> NewestWriter(std::uint64_t address, std::size_t bytes, Range* overlapped = nullptr);
    bool OverlapsAny(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);
    Watch WatchWrites();
    bool WrittenSince(const Watch& watch, const std::vector<Range>& reads);
    std::uint64_t Retired() const { return retiredItems.load(std::memory_order_acquire); }
    void NoteDecodeDrain() { decodeDrains.fetch_add(1, std::memory_order_relaxed); }
    std::uint64_t DecodeDrains() const { return decodeDrains.load(std::memory_order_relaxed); }
    std::optional<std::uint64_t> PendingLabel(std::uint64_t address, std::size_t bytes);
    static bool HeldReads(std::uint64_t address, std::size_t bytes);
    static std::uint64_t HeldReadQueries();
    void ReleaseRunningReads();
    QueuedWrite StoredBytes(std::uint64_t address, std::size_t bytes, std::span<std::byte> out);

    static bool MetadataPassesPipelined();
    static void CountPipelinedMetadataPass();
    static std::uint64_t PipelinedMetadataPasses();
private:
    struct Item {
        Commit commit;
        std::vector<Range> writes;
        std::uint64_t labelAddress = 0;
        std::vector<std::byte> labelBytes;
        std::vector<Range> reads;
        bool draw = false;
        bool store = false;
        CommitKind commitKind = CommitKind::Other;
        ItemKind kind = ItemKind::Other;
        // APS5_COLLECT_AHEAD_CENSUS: when queue 0 enqueued the item, and the collect clock then.
        std::uint64_t enqueuedNs = 0;
        std::uint64_t enqueueClock = 0;
        std::shared_ptr<ResolveSlot> resolve;
        std::uint64_t criticalEnqueue = 0;
        std::shared_ptr<std::uint64_t> criticalRetirement;
    };
    struct Applied {
        std::uint64_t serial = 0;
        std::vector<Range> writes;
    };
    DrawPipeline() = default;
    void enqueue(Item item);
    void run();
    void runResolver();
    void startResolvers();
    void resolveForApply(ResolveSlot& slot, std::uint64_t serial);
    bool appliedOverlap(const ResolveSlot& slot, std::uint64_t serial);
    void noteApplied(const Item& item, std::uint64_t serial);
    void drainUnlocked(DrainReason reason, std::uint32_t opcode, DrainCause cause, bool finisherBusy);
    void rethrowFailure();
    void report(std::chrono::steady_clock::time_point now);
    void countWrites(const std::vector<Range>& writes, bool added);

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Item> items;
    std::vector<std::pair<Range, std::uint32_t>> queuedWrites;
    std::vector<Item> retired;
    std::exception_ptr failure;
    std::atomic<std::size_t> outstanding{0};
    std::atomic<std::uint64_t> retiredItems{0};
    std::atomic<std::uint64_t> decodeDrains{0};
    std::uint64_t reportedDecodeDrains = 0;
    std::uint32_t idleWaiters = 0;
    std::size_t resumeAt = 0;
    bool committerWaiting = false;
    bool stopped = false;
    bool closing = false;
    std::uint64_t wakes = 0;
    std::uint64_t lockWaitNs = 0;
    std::thread thread;
    std::vector<std::thread> resolvers;
    std::condition_variable resolveWake;
    std::condition_variable resolveDone;
    std::deque<Applied> applied;
    std::uint64_t commits = 0;
    std::uint64_t commitNs = 0;
    std::uint64_t commitErrors = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t fullWaitNs = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrainReason::Count)> drains{};
    std::array<std::uint64_t, static_cast<std::size_t>(DrainReason::Count)> drainWaitNs{};
    std::array<std::uint64_t, 257> drainOpcodes{};
    std::array<std::atomic<std::uint64_t>, 257> drainRequests{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DrainReason::Count)> reasonRequests{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(CommitKind::Count)> kindCommits{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(CommitKind::Count)> kindNs{};
    std::uint64_t depthSum = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

class DrawFinisher {
public:
    using Job = std::function<void()>;
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    struct Pending {
        std::uint64_t key = 0;
        std::uint64_t shape = 0;
        bool unknown = true;
        std::vector<Range> writes;
        std::uint64_t labelAddress = 0;
        std::vector<std::byte> labelBytes;
    };
    enum class LabelLookup : std::uint8_t { NotHeld, Value, Unknown };
    static bool Overlap();
    bool PendingOverlaps(std::uint64_t begin, std::uint64_t end);
    LabelLookup PendingLabel(std::uint64_t address, std::size_t bytes, std::uint64_t& value);
    bool PendingOverlapsAny(std::span<const Range> ranges);
    bool PendingKey(std::uint64_t key, std::uint64_t shape);
    enum class DrainSite : std::uint32_t { Lookup, Reads, Packet, Kept, Count };
    static void NoteDrain(DrainSite site, bool drained);
    static std::string DrainText();
    static void NoteDrainedOpcode(std::uint32_t opcode, bool besideCommits);
    static DrawFinisher& Get();
    static bool Enabled();
    static bool OnFinisherThread();
    static bool Busy() { return outstanding.load(std::memory_order_acquire) != 0; }
    // Like DrawPipeline: never waits while the caller holds the GPU mutex (Enqueue goes past the
    // depth; Drain and Settle wait with the hold given up).
    void Enqueue(Job job);
    void Enqueue(Job job, Pending held);
    void Drain();
    void Settle();
    void ReleaseRan();

private:
    DrawFinisher() = default;
    void run();
    void waitEmpty(bool rethrow);
    inline static std::atomic<std::size_t> outstanding{0};
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Job> jobs;
    std::deque<Pending> pending;
    std::vector<Job> ran;
    std::exception_ptr failure;
    bool running = false;
    bool sleeping = false;
    std::thread thread;
};

}

#endif
