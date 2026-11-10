#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_STAGE4DIAG_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_STAGE4DIAG_HPP

// Stage 4 measurements (AnyPS5-redesign/specs/stage4-driver-critical-path.md, M1/M3/M4). Diagnostic
// only, every part default off and read once from the environment; off, each hook is one load of a
// function-local static and a branch. Lines go to stderr every APS5_STAGE4_REPORT_SECONDS (default 5)
// at a flip, with every value per frame over the lines' window:
//
// M1, APS5_PIPE_IDLE_ATTRIB=1: the time neither queue 0 nor the draw committer runs. The committer
//   keeps a state clock (run, spin, idle, GPU mutex, fence, other); every wait queue 0 makes (guest
//   submissions, label waits, pipeline full/drain, GPU mutex, fence) is charged with the committer
//   states that overlapped its blocking part, and label waits are split by how they ended
//   (Recorder::WaitEnd), why the label table refused them and who wrote the label.
//   Lines: [stage4-m1], [stage4-m1-matrix], [stage4-m1-label], [stage4-m1-writer].
// M3, APS5_WT_FAULT_CENSUS=1: unique 4 KiB guest pages first written per frame (between flips) by
//   writer: dirty pages the write tracker's walks found (CPU stores, of which those no HLE bracket or
//   driver store covered this frame: guest-only), HLE host-write brackets (ReadFile, AIO, ...) and
//   driver stores (MarkWritten), with the pages also written the frame before (hot) and 64 KiB blocks.
//   APS5_WT_FAULT_CENSUS_SWEEP=N adds a non-resetting write-watch sweep every N flips (dirty pages not
//   collected yet; costs ~10+ ms on the presenter). Line: [stage4-m3].
// M4, APS5_RESOLVE_CENSUS=1: per committed item, time in resolve-class phases (write tracking,
//   PrepareDrawBindings, template lookup/proof, resources, inputs/setup/pipeline lookup, dispatch
//   preparation) apart from apply-class phases (targets, recording, keep, sync, labels, stamps), and
//   cross-item conflicts within K=3 and K=8 windows. Lines: [stage4-m4], [stage4-m4-conflict].

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace AgcDriver::Stage4 {

// "1" in the environment variable (read once per switch).
bool EnvSwitch(const char* name);
inline bool IdleAttrib() {
    static const bool on = EnvSwitch("APS5_PIPE_IDLE_ATTRIB");
    return on;
}
inline bool FaultCensus() {
    static const bool on = EnvSwitch("APS5_WT_FAULT_CENSUS");
    return on;
}
inline bool ResolveCensus() {
    static const bool on = EnvSwitch("APS5_RESOLVE_CENSUS");
    return on;
}
// IdleAttrib() || ResolveCensus(): the wait scopes run.
inline bool Tracking() {
    static const bool on = IdleAttrib() || ResolveCensus();
    return on;
}

enum class Role : std::uint8_t { None, Queue0, Committer };
// The calling thread's role (no-op unless a measurement is on).
void BindRole(Role role);

// M1 wait kinds (queue 0's categories; on the committer they set its clock state).
enum class Wait : std::uint8_t { Submission, Label, PipeFull, PipeDrain, GpuMutex, Fence, Idle, Count };
inline constexpr const char* WaitNames[] = {"submit", "label", "full", "drain", "mutex", "fence", "idle"};
// The committer's clock states. Blocked: Idle, GpuMutex, Fence, Other.
enum class CommitterState : std::uint8_t { Run, Spin, Idle, GpuMutex, Fence, Other, Count };
inline constexpr const char* CommitterStateNames[] = {"run", "spin", "idle", "mutex", "fence", "other"};
inline constexpr std::size_t CommitterStates = static_cast<std::size_t>(CommitterState::Count);

// Why the label table did not satisfy a label wait (the last reason seen during the wait).
enum class LabelReason : std::uint8_t { NotConsulted, NoEntry, TrustOff, Queued, Overwritten, Unclosed, BehindCompletion, ValueMismatch, LateCpuStore, Count };
inline constexpr const char* LabelReasonNames[] = {"not-consulted", "no-entry", "trust-off", "queued", "overwritten", "unclosed", "behind-completion", "value-mismatch", "late-cpu-store"};
// Who wrote the label the wait took: a label recorded in the recorder's table (GPU) by queue 0 or
// another queue, a label store in the history (a queue's packet, done on the CPU or by a completion),
// recorded GPU work that settled (fence), memory changed with recorded GPU work pending over it, or
// memory changed with no driver record at all (a guest thread).
enum class Writer : std::uint8_t { Unknown, OwnTable, OtherTable, OwnStore, OtherStore, Gpu, Guest, Count };
inline constexpr const char* WriterNames[] = {"?", "q0-table", "other-table", "q0-store", "other-store", "gpu", "guest"};
inline constexpr std::size_t WaitEnds = 7;  // Graphics::Recorder::WaitEnd
inline constexpr const char* WaitEndNames[] = {"entry", "polled", "recorder", "fence", "history", "held", "aborted"};

// A wait on queue 0 or the committer. `spinsFirst`: the wait spins before it blocks (Blocking()
// marks the switch); otherwise it counts as blocked from the start.
class WaitScope {
public:
    WaitScope(Wait kind, bool spinsFirst) noexcept {
        if (Tracking()) begin(kind, spinsFirst);
    }
    ~WaitScope() {
        if (active) end();
    }
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;
    void Blocking() noexcept {
        if (active && blockStart == 0) blocking();
    }
    bool Active() const noexcept { return active; }
    // Label-wait detail (queue 0 only; ignored otherwise).
    void SetEnd(std::uint8_t value) noexcept { waitEnd = value; }
    void SetReason(LabelReason value) noexcept { reason = value; }
    void SetWriter(Writer value, std::uint32_t queue) noexcept {
        writer = value;
        writerQueue = queue;
    }

private:
    void begin(Wait kind, bool spinsFirst) noexcept;
    void blocking() noexcept;
    void end() noexcept;
    bool active = false;
    bool committer = false;
    bool outermostNested = false;
    Wait kind = Wait::Count;
    std::uint8_t waitEnd = 6;
    LabelReason reason = LabelReason::NotConsulted;
    Writer writer = Writer::Unknown;
    std::uint32_t writerQueue = 0;
    std::int64_t start = 0;
    std::int64_t blockStart = 0;
    // Committer clock snapshots (queue 0 only; written before they are read).
    std::int64_t clockAtStart[CommitterStates];
    std::int64_t clockAtBlock[CommitterStates];
};

// M3 notes (no-ops unless FaultCensus()).
void NoteCollectedPages(std::uint64_t begin, std::uint64_t end);
void NoteHostWrite(std::uint64_t address, std::size_t bytes);
void NoteDriverWrite(std::uint64_t address, std::size_t bytes);

// M4 buckets. Resolve-class first, then apply-class, then mixed and waits.
enum class Bucket : std::uint8_t {
    // resolve
    Pre, Prelock, Validate, Vertex, Setup, Lookup, Resources, Pipeline, Pdb, DispatchPrepare, WtResolve,
    // apply
    ReadTarget, Prepare, Record, Keep, Sync, WriteBack, Describe, WtApply, Labels, NonDraw,
    // mixed / unsplit
    DispatchDevice, DrawTail, DispatchTail,
    // excluded
    Wait,
    Count
};
inline constexpr std::size_t Buckets = static_cast<std::size_t>(Bucket::Count);
inline constexpr std::size_t FirstApplyBucket = static_cast<std::size_t>(Bucket::ReadTarget);
inline constexpr std::size_t FirstMixedBucket = static_cast<std::size_t>(Bucket::DispatchDevice);
inline constexpr const char* BucketNames[] = {"pre", "prelock", "validate", "vertex", "setup", "lookup", "resources", "pipeline", "pdb", "dispatch_prepare", "wt",
                                              "read_target", "prepare", "record", "keep", "sync", "write_back", "describe", "wt_stamp", "labels", "non_draw",
                                              "dispatch_device", "draw_tail", "dispatch_tail", "wait"};

// The committer around each item (DrawPipeline::run). `kind`: DrawPipeline::ItemKind.
void BeginItem(std::uint8_t kind, std::span<const std::pair<std::uint64_t, std::uint64_t>> reads, std::span<const std::pair<std::uint64_t, std::uint64_t>> writes, std::uint64_t pendingSerial);
void EndItem(std::uint64_t pendingSerial, bool breaksProofs);
// A phase boundary inside the current item (no-op off the committer or outside an item).
void MarkSlow(Bucket bucket) noexcept;
inline void Mark(Bucket bucket) noexcept {
    if (ResolveCensus()) MarkSlow(bucket);
}
// The template object the item's resolve used, and whether the item built it (not a cache hit).
void NoteTemplate(const void* object, bool built) noexcept;

// A write-tracker call inside an item: its time goes to WtResolve (a query) or WtApply (a stamp of
// the driver's own write) and its range to the item's read or write set.
class TrackerScope {
public:
    TrackerScope(bool isStamp, std::uint64_t address, std::size_t bytes) noexcept {
        if (ResolveCensus()) begin(isStamp, address, bytes);
    }
    ~TrackerScope() {
        if (active) end();
    }
    TrackerScope(const TrackerScope&) = delete;
    TrackerScope& operator=(const TrackerScope&) = delete;
    // One more range read by the same call (UnchangedSinceAll and friends).
    void AddRead(std::uint64_t address, std::size_t bytes) noexcept;

private:
    void begin(bool isStamp, std::uint64_t address, std::size_t bytes) noexcept;
    void end() noexcept;
    bool active = false;
    bool outermost = false;
    bool stamp = false;
    std::int64_t start = 0;
};

// At every flip (Driver::Present, presenter thread): frame boundary of the M3 census and the report.
void Flip();

static_assert(sizeof(WaitNames) / sizeof(WaitNames[0]) == static_cast<std::size_t>(Wait::Count));
static_assert(sizeof(CommitterStateNames) / sizeof(CommitterStateNames[0]) == CommitterStates);
static_assert(sizeof(LabelReasonNames) / sizeof(LabelReasonNames[0]) == static_cast<std::size_t>(LabelReason::Count));
static_assert(sizeof(WriterNames) / sizeof(WriterNames[0]) == static_cast<std::size_t>(Writer::Count));
static_assert(sizeof(WaitEndNames) / sizeof(WaitEndNames[0]) == WaitEnds);
static_assert(sizeof(BucketNames) / sizeof(BucketNames[0]) == Buckets);

}

#endif
