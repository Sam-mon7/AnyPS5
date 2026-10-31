#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTMEMORY_GENTABLE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTMEMORY_GENTABLE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

// Stage 1 write generations, slice S1 (census/shadow mode): a lock-free generation table keyed by
// guest VA, its clocks and tickets, and the shadow census that compares what a generation check
// would have decided with the decision the driver actually took. Nothing in this slice changes a
// decision: the table never sweeps (libc Collect under the tracker mutex stays the only sweeper)
// and nothing reads its answers except the census. See docs/dev/debt/libSceAgcDriver.md and
// AnyPS5-redesign/specs/stage1-write-generations.md.
//
// APS5_ARCHB_GEN_TABLE=1 maintains the table: block generations bumped at the tracker's stamp
// points, mapping/unwatch/representation/pending events, tickets at every collect-epoch bump, and
// the coverage (swept tickets) of the existing walks. Turning it on (at start or at run time) sets
// a reset floor: every generation handed out before reads Unknown(Reset). While it is on, the
// tracker's own generation values are taken from the same clock (GenClock), so a tracker generation
// a consumer holds is directly a "since" of this table. Only the values change, never their order.
// APS5_ARCHB_GEN_SHADOW=1 (requires GEN_TABLE) classifies consumer decisions and models sweeps.
// APS5_ARCHB_GEN_VERIFY=<N> (needs GEN_SHADOW) cross-checks 1 in N shadow verdicts exhaustively
// against the tracker's own block stamps (no coarse shortcuts) and counts gen_verify.*.
namespace AgcDriver::GuestMemory::Gen {

using Generation = std::uint64_t;
using Ticket = std::uint64_t;
using ProducerId = std::uint64_t;

enum Kind : std::uint8_t { Data = 1, Cpu = 2, Pending = 4, Meta = 8, Any = 15 };
enum class Verdict : std::uint8_t { Clean, Changed, PendingOnly, Unknown };
enum class UnknownReason : std::uint8_t { None, HostRegion, Excluded, Compared, Uncommitted, Outside, NotSwept, Reset, NoSince, Untracked, Count };
// Per block. Untracked: never swept (or its leaf is missing). Every transition away from Tracked
// also bumps the block's meta generation and drops its coverage.
enum class BlockState : std::uint8_t { Untracked = 0, Tracked, Excluded, Compared, Uncommitted };

const char* VerdictName(Verdict verdict);
const char* UnknownReasonName(UnknownReason reason);

struct BlockFields {
    bool present = false;
    Generation born = 0;
    Generation data = 0;
    Generation cpu = 0;
    Generation pending = 0;
    Generation meta = 0;
    Ticket swept = 0;
    Ticket maskTicket = 0;
    std::uint16_t mask = 0;
    std::uint16_t sweeper = 0;
    BlockState state = BlockState::Untracked;
    Generation coarseChanged = 0;
    Ticket coarseSwept = 0;
};

// What EnsureSwept would have to do for a range at a ticket (the sweep model; nothing is swept).
struct Coverage {
    std::uint64_t blocks = 0;
    std::uint64_t pages = 0;
    std::uint64_t uncoveredBlocks = 0;
    std::uint64_t uncoveredPages = 0;
    // Maximal runs of consecutive uncovered pages: one OS query each if swept run by run.
    std::uint64_t uncoveredRuns = 0;
    // Covered blocks whose covering sweep was made by another sweeper (thread).
    std::uint64_t crossBlocks = 0;
    // Excluded, compared or uncommitted blocks: today's collect refuses them, so does the model.
    std::uint64_t unknownBlocks = 0;
};

struct JournalEntry {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    Generation generation = 0;
    ProducerId producer = 0;
};

// The table proper: platform neutral, no driver dependencies, instantiable by tests. Every field
// only grows (atomic max, release) except coverage, which events may drop to 0; readers never lock.
class Table {
public:
    static constexpr unsigned BlockShift = 16;
    static constexpr unsigned PageShift = 12;
    static constexpr unsigned AddressBits = 48;
    static constexpr std::size_t PagesPerBlock = std::size_t{1} << (BlockShift - PageShift);
    static constexpr std::size_t LeafBlocks = std::size_t{1} << 16;
    static constexpr std::size_t CoarseBlocks = 64;
    static constexpr std::size_t LeafCount = std::size_t{1} << (AddressBits - BlockShift - 16);
    static constexpr std::size_t JournalEntries = std::size_t{1} << 16;
    static constexpr Ticket NeverCovered = ~Ticket{0};

    Table();
    ~Table();
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    // GenClock: event generations. Now() is an acquire load (take it before reading guest bytes).
    Generation Now() const noexcept;
    Generation Bump() noexcept;
    // A fresh generation strictly above `floor` (the tracker's current value while the clocks merge).
    Generation BumpAbove(Generation floor) noexcept;
    void RaiseClock(Generation floor) noexcept;
    // TicketClock: ordering points.
    Ticket Issue() noexcept;
    Ticket LatestTicket() const noexcept;
    // Reset floor: every since below it reads Unknown(Reset), and no sweep that started before it
    // covers anything. Returns the floor (a fresh generation above `above` and above GenClock).
    Generation Reset(Generation above) noexcept;
    Generation ResetFloor() const noexcept;
    Ticket SweptFloor() const noexcept;

    // Applies `generation` to `kinds` of every block of [begin, end) and to their coarse units.
    // create=false leaves missing leaves alone (a leaf created later is born after this event).
    // Returns the blocks touched.
    std::uint64_t Apply(std::uint64_t begin, std::uint64_t end, std::uint8_t kinds, Generation generation, bool create = true) noexcept;
    // Moves the blocks to `state` (existing leaves only unless create); a state other than Tracked
    // drops coverage. The caller bumps meta in the same event.
    std::uint64_t SetState(std::uint64_t begin, std::uint64_t end, BlockState state, bool create) noexcept;
    void DropCoverage(std::uint64_t begin, std::uint64_t end) noexcept;
    // A completed resetting walk of [begin, end) (page aligned) that read `ticket` before its OS
    // query: whole blocks get swept := max(swept, ticket), partial ones the page mask. Single sweeper
    // (the tracker mutex) by contract.
    void RecordSweep(std::uint64_t begin, std::uint64_t end, Ticket ticket, std::uint16_t sweeper) noexcept;

    // Clean iff the floor is at most `since`, every block is Tracked (and covered at `required`,
    // unless required is 0) and every selected kind is at most `since`; Changed if data/cpu/meta
    // moved; PendingOnly if only pending moved; Unknown(reason) otherwise.
    Verdict Check(std::uint64_t begin, std::uint64_t end, Generation since, Ticket required, std::uint8_t kinds, UnknownReason* reason = nullptr) const noexcept;
    // Check without the coarse shortcut (the verifier's exhaustive pass); same answer by contract.
    Verdict CheckExhaustive(std::uint64_t begin, std::uint64_t end, Generation since, Ticket required, std::uint8_t kinds, UnknownReason* reason = nullptr) const noexcept;
    bool Covered(std::uint64_t begin, std::uint64_t end, Ticket required) const noexcept;
    Coverage Model(std::uint64_t begin, std::uint64_t end, Ticket required, std::uint16_t self) const noexcept;
    BlockFields Fields(std::uint64_t address) const noexcept;
    // Newest data generation over the range's blocks (0 where no leaf).
    Generation NewestData(std::uint64_t begin, std::uint64_t end) const noexcept;

    // Slice S2 (GEN_LOCKFREE). A refused (compared-when-shared) walk over the blocks of [begin, end)
    // at `generation`: the per-block refusal the tracker keeps in its map, readable without its mutex.
    void Refuse(std::uint64_t begin, std::uint64_t end, Generation generation) noexcept;
    // Whether every block of [begin, end) has data and refusal generations at most `since` (blocks
    // of missing leaves had no event since the table went on), the reset floor is at most `since`
    // and no event was lost to a failed leaf allocation. Lock-free: a true answer holds for the
    // table's state at the first block read (fields only grow).
    bool DataAtMost(std::uint64_t begin, std::uint64_t end, Generation since) const noexcept;
    // The data generation of the block holding `address` (0 without a leaf).
    Generation DataOf(std::uint64_t address) const noexcept;
    // False once an event could not reach a block (a leaf allocation failed): lock-free answers
    // are then refused for the rest of the run.
    bool Complete() const noexcept;

    // Producer-tagged pending writes: a bounded ring; on overflow ForeignPendingSince is true.
    void Journal(std::uint64_t begin, std::uint64_t end, Generation generation, ProducerId producer) noexcept;
    std::size_t RecentJournal(std::span<JournalEntry> out) const noexcept;
    bool ForeignPendingSince(std::uint64_t begin, std::uint64_t end, Generation since, ProducerId self) const noexcept;
    std::uint64_t JournalAppends() const noexcept;

    std::size_t Leaves() const noexcept;

private:
    struct Leaf;
    struct Slot;
    Leaf* leafFor(std::uint64_t block, bool create) const noexcept;
    template<bool Coarse>
    Verdict check(std::uint64_t begin, std::uint64_t end, Generation since, Ticket required, std::uint8_t kinds, UnknownReason* reason) const noexcept;

    std::unique_ptr<std::atomic<Leaf*>[]> leaves;
    mutable std::atomic<std::size_t> leafCount{0};
    std::atomic<Generation> clock{1};
    std::atomic<Ticket> tickets{1};
    std::atomic<Generation> resetGeneration{0};
    std::atomic<Ticket> sweptFloor{0};
    std::unique_ptr<Slot[]> journal;
    std::atomic<std::uint64_t> journalHead{0};
    mutable std::atomic<bool> incomplete{false};
};

// ----- Driver integration (one global table) -----

Table& Global();
// GEN_TABLE: whether the table is maintained. The first call that sees it turned on raises GenClock
// above the tracker's generation and sets the reset floor.
bool Active() noexcept;
// GEN_SHADOW (requires GEN_TABLE): consumer classification, sweep model, hit runs, serial moves.
bool ShadowActive() noexcept;
// APS5_ARCHB_GEN_VERIFY=N: 0 off, else 1 in N shadow verdicts get the exhaustive cross-check.
std::uint32_t VerifyPeriod() noexcept;
bool VerifyAbortOnUnsafe() noexcept;

// Slice S2, GEN_LOCKFREE (requires GEN_TABLE): the tracker's query APIs answer from the table
// without the tracker mutex when that answer is conclusive (GuestMemory.cpp). Once a lock-free or
// draw-validation consumer has relied on the table, the table stays maintained even if GEN_TABLE
// is turned off at run time (a consumer reading a table nobody maintains could miss a stamp).
bool LockFreeActive() noexcept;
enum class LockFreeApi : std::uint8_t { UnchangedSince, UnchangedSinceAll, ChangedBlocks, WrittenSince, NewestStamp, UnchangedCommittedSince, StoredOver, TrackerGeneration, Count };
const char* LockFreeApiName(LockFreeApi api);
// One query: answered from the table (fast) or by the locked tracker path (slow).
void CountLockFree(LockFreeApi api, bool fast) noexcept;
// Verify mode (APS5_ARCHB_GEN_VERIFY=N): a sampled fast answer recomputed under the tracker mutex.
// unsafe: the table's answer under the mutex contradicts the tracker's; raced: the lock-free answer
// differs only because a stamp landed between the two (the table under the mutex agrees).
enum class LockFreeVerify : std::uint8_t { Agree, Raced, Unsafe };
void CountLockFreeVerify(LockFreeApi api, LockFreeVerify outcome, std::uint64_t address, std::uint64_t bytes, std::uint64_t since) noexcept;

// Ordering points (rule V1): the calling context's required ticket.
enum class TicketReason : std::uint8_t { Submission, Packet, WaitEntry, WaitCpu, WaitGpuLabel, WaitOwn, Drain, Reap, UnlockedWait, Committer, Present, Uncached, Other, Count };
const char* TicketReasonName(TicketReason reason);
Ticket Issue(TicketReason reason) noexcept;
// A private fresh ticket (rule V4, uncached collects): counted, the context's ticket unchanged.
Ticket IssuePrivate(TicketReason reason) noexcept;
// Counts an ordering point that issues no ticket (a wait on the queue's own recorded label).
void NoteTicket(TicketReason reason) noexcept;
// The calling thread's required ticket: 0 when it never reached an ordering point.
Ticket Required() noexcept;
// A committer adopting the ticket its issuing thread carried with the commit.
void Follow(Ticket ticket) noexcept;
// The presenter adopting the newest ticket (AdoptLatestCollectEpoch).
void AdoptLatest() noexcept;

// Events (§2.2). The kinds are the fields bumped; `generation` 0 allocates a fresh one.
enum class Event : std::uint8_t { CpuSweep, DriverSweep, ImportWindow, FreshCollect, DriverStore, HostWriteBegin, HostWriteEnd, Pending, Forget, Inaccessible, WatchMapping, Rewatch, Unwatch, Representation, ShadowPublish, Refused, FailedWalk, Count };
const char* EventName(Event event);
Generation Note(Event event, std::uint64_t begin, std::uint64_t end, std::uint8_t kinds, Generation generation = 0, bool create = true) noexcept;
// Note plus a state change of the blocks (meta bump, coverage dropped unless Tracked).
Generation NoteState(Event event, std::uint64_t begin, std::uint64_t end, BlockState state, std::uint8_t kinds, Generation generation = 0, bool create = true) noexcept;
// A GPU write announced (pending + journal entry).
void NotePending(std::uint64_t begin, std::uint64_t end, ProducerId producer) noexcept;
// A resetting walk completed (records coverage at the ticket read before its OS call).
void NoteSweep(std::uint64_t begin, std::uint64_t end, Ticket ticket) noexcept;
// The calling thread's sweeper id (a small hash of the thread).
std::uint16_t SweeperId() noexcept;

// Consumers of "unchanged" evidence whose decisions the shadow classifies.
enum class Consumer : std::uint8_t { DrawValidate, DispatchValidate, DispatchSpans, ResourceProof, TextureLookup, StorageRefresh, Prelock, Snapshot, DrawInput, Mirror, Presentation, Other, Count };
const char* ConsumerName(Consumer consumer);
Consumer CurrentConsumer() noexcept;
class ConsumerScope {
public:
    explicit ConsumerScope(Consumer consumer) noexcept;
    ~ConsumerScope();
    ConsumerScope(const ConsumerScope&) = delete;
    ConsumerScope& operator=(const ConsumerScope&) = delete;

private:
    std::uint8_t previous;
};

// The classification of one decision: `verdict` is what Check would have said, `unchanged` is the
// decision taken (true: the evidence was reused / the bytes matched). unsafe = Clean while the
// decision found a change.
enum class Outcome : std::uint8_t { AgreeClean, AgreeChanged, Conservative, Unsafe };
Outcome Classify(Consumer consumer, Verdict verdict, UnknownReason reason, bool unchanged, bool needsSweep) noexcept;
// A Changed verdict whose byte compare found the bytes equal (CompareAndRestamp would restamp).
void CountRestamp(Consumer consumer) noexcept;
// A decision refused before or around its byte compare (a gate, a flush, a moved serial): not classified.
void CountRefused(Consumer consumer) noexcept;
// A conservative verdict whose change was only meta or pending (data and cpu alone read clean).
void CountConservativeMeta(Consumer consumer) noexcept;
// A decision of Consumer::Other, by its call site (the census prints the most frequent).
void NoteOtherSite(const char* file, std::uint32_t line) noexcept;
// The verifier's outcome for a sampled verdict.
void CountVerify(Consumer consumer, Outcome outcome) noexcept;
bool VerifySample() noexcept;
// One unsafe disagreement: logged with block fields and the last journal entries (first 20).
void ReportUnsafe(Consumer consumer, std::uint64_t begin, std::uint64_t end, Generation since, const char* detail) noexcept;

// Consecutive queue-0 draw-cache hits whose footprints are all Clean (Stage 2 eligibility).
void NoteDrawLookup(bool queue0, bool hit, bool clean) noexcept;
// Sweep model for one memoized collect: what EnsureSwept would need at the thread's ticket.
void NoteModel(const Coverage& coverage, bool unbumped) noexcept;
// A model call that the real path then walked while the model found it covered by another sweeper.
void NoteModelCrossWalk() noexcept;
// A WAIT_REG_MEM satisfied by another queue's recorded label: whether V5 would admit inheritance.
void NoteWaitInheritance(bool fromTable, bool admitted) noexcept;
// A resource proof refused by a global serial while its footprint stayed clean.
void NoteGlobalOnlyRefusal() noexcept;

// Global serials sampled once per present (gen_serial_moves.*).
enum class Serial : std::uint8_t { Proof, ResourceProof, Pending, Publish, Forget, Unwatch, Allocations, Commit, Depth, Tracker, Count };
void SampleSerials(std::span<const std::uint64_t, static_cast<std::size_t>(Serial::Count)> values) noexcept;

// Counters: per-thread, folded for the recorder (registered gen.* / gen.q0.* counters) and for the
// frame replay ([gen-census] lines, per queue tag, reset at each pass start).
void PublishCounters() noexcept;
void ResetCensus() noexcept;
void ReportCensus(std::uint64_t pass, std::uint64_t frames) noexcept;
// Tests: a counter by name summed over every thread (0 if unknown), and a hook that drops an event
// kind (to prove the shadow catches a missed event).
std::uint64_t CounterForTest(const char* name) noexcept;
void SuppressEventForTest(Event event, bool suppressed) noexcept;
// Tests: forget the activation so the next Active() applies a new reset floor.
void ResetActivationForTest() noexcept;

namespace detail {
// Implemented by GuestMemory.cpp: the tracker generation (relaxed), for the activation floor.
std::uint64_t CurrentTrackerGeneration() noexcept;
// Implemented by VulkanDevice.cpp: samples the global serials into SampleSerials (per present, and
// at the start and end of each frame-replay pass).
void SampleDriverSerials() noexcept;
}

}

#endif
