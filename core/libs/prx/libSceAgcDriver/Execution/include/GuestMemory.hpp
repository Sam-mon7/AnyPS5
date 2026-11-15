#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTMEMORY_HPP

#include "prx/libc/include/HostThreadSlot.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory/GenTable.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <source_location>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::GuestMemory {

void CheckRange(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable = false);
// CheckRange without throwing: whether the whole range is mapped with the requested access.
bool Accessible(const void* pointer, std::size_t bytes, bool writable = false);
// The accessible parts [begin, end) of a range, in address order. GPU heaps are often bound whole while
// the guest commits their pages on demand, so a binding can cover reserved but uncommitted pages.
std::vector<std::pair<std::uint64_t, std::uint64_t>> CommittedRanges(std::uint64_t address, std::size_t bytes, bool writable = false);
// Both facts from one walk of the page tables: the accessible parts of a range and whether they cover
// all of it (`whole`, what Accessible answers). Reserved pages are queried each time, so asking twice
// costs two VirtualQuery per run.
struct Commitment {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    bool whole = false;
};
Commitment DescribeCommitted(std::uint64_t address, std::size_t bytes, bool writable = false);
// DescribeCommitted into the caller's vector (cleared first, its capacity kept); returns `whole`.
bool DescribeCommittedInto(std::uint64_t address, std::size_t bytes, bool writable, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges);
void CopyGuestBytes(void* destination, const void* source, std::size_t bytes);
void Read(std::uint64_t address, std::span<std::byte> destination, std::size_t alignment = 1);
// Variants for resources in heaps the guest commits on demand (partially resident textures, GPU
// scratch heaps): uncommitted pages read as zeros, are not compared, and are never stored.
void ReadCommitted(std::uint64_t address, std::span<std::byte> destination);
void ReadCommittedUnsynced(std::uint64_t address, std::span<std::byte> destination);
bool EqualsCommitted(std::uint64_t address, std::span<const std::byte> bytes);
// EqualsCommitted without the flush hook: the caller has decided that the recorded GPU work
// writing the range need not be waited for (the dispatch-cache validation in Driver.cpp).
bool EqualsCommittedUnsynced(std::uint64_t address, std::span<const std::byte> bytes);
// EqualsCommittedUnsynced for a caller that treats an unmapped page as a miss (the dispatch-cache
// validation's per-run compare): one walk of the page states, one memcmp per mapped run, no profile
// counter and no reason string. Unmapped: some page of the range is not readable (or the address
// space cannot be queried).
enum class Compare : std::uint8_t { Equal, Differs, Unmapped };
Compare CompareMapped(std::uint64_t address, std::span<const std::byte> bytes);
// CompareMapped's walk with a copy in place of the compare: the bytes at `address` are copied into
// `out` (Equal), or Unmapped as CompareMapped answers it (the dispatch-cache validation's masked
// re-read of a differing run).
Compare CopyMapped(std::uint64_t address, std::span<std::byte> out);
Compare CopyMapped(std::uint64_t address, std::span<std::byte> out, bool checked);
Compare CopyMappedGuarded(std::uint64_t address, std::span<std::byte> out, bool& owned);
void WriteChangedCommitted(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original);
void Write(std::uint64_t address, std::span<const std::byte> source, std::size_t alignment = 1);
// Stores the parts of `current` that differ from `original` (the guest bytes the GPU started from), so
// guest writes made meanwhile to untouched bytes survive. Compares in 256-byte blocks.
void WriteChanged(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original);
void WriteChanged(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original, std::uint32_t byteStoreLimit);
std::size_t WriteChangedIfHeld(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original, std::uint64_t unchangedSince);
std::uint32_t ChangedByteStoreLimit();
void WriteChangedKeepingGuest(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original);

// Write tracking. The guest arena is reserved with page write watching, so CollectWrites learns which
// pages of a range the CPU wrote since they were last collected, stamps their 64 KiB blocks with a new
// generation and returns it; a snapshot of the range taken after that call is current while
// UnchangedSince(range, that generation) holds. Every validation collects its own range first, so
// resources sharing pages see each other's writes. GPU writes into host-imported memory bypass the
// page tables and are reported with MarkWritten. Without write watching CollectWrites returns 0 and
// UnchangedSince is always false, so callers fall back to comparing bytes.
bool WriteWatched();
void Unwatch(std::uint64_t address, std::size_t bytes);
void Rewatch(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);
bool ImportWatched(std::uint64_t address, std::size_t bytes, const std::function<bool()>& import, bool staysWatched = true, bool importMayWriteGuest = true);
void SetUnwatchHook(void (*hook)(std::uint64_t address, std::size_t bytes));
bool UnwatchWritesProven();
void SetImportWalkFailureForTest(int walk, std::uint64_t span = 0);
#ifdef _WIN32
using FreshCollectHook = bool (*)(std::uintptr_t address, std::size_t bytes, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context);
void SetFreshCollectHook(FreshCollectHook hook);
#endif
bool Watched(std::uint64_t address, std::size_t bytes);
std::uint64_t CollectWrites(std::uint64_t address, std::size_t bytes, bool memoVirtualQuery = false, std::source_location site = std::source_location::current());
bool UnchangedSince(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site = std::source_location::current());
// UnchangedSince for several ranges under one tracker lock: true only when every one holds.
struct UnchangedQuery {
    std::uint64_t address;
    std::size_t bytes;
    std::uint64_t generation;
};
bool WrittenSince(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site = std::source_location::current());
bool UnchangedSinceAll(std::span<const UnchangedQuery> queries, std::source_location site = std::source_location::current());
bool UnchangedSinceClean(std::span<const UnchangedQuery> queries, std::source_location site = std::source_location::current());
std::uint64_t CollectCommittedWrites(std::uint64_t address, std::size_t bytes, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, std::source_location site = std::source_location::current());
// CollectCommittedWrites that fills `ranges` in place (keeping its capacity, for a reused scratch)
// instead of replacing it with a fresh vector; the same ranges and generation, empty on 0.
std::uint64_t CollectCommittedWritesInPlace(std::uint64_t address, std::size_t bytes, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, std::source_location site = std::source_location::current());
std::uint64_t WriteGeneration();
std::uint64_t CollectWrittenBlocks(std::uint64_t address, std::size_t bytes, std::uint64_t since, std::vector<std::pair<std::uint64_t, std::uint64_t>>& written, std::source_location site = std::source_location::current());
bool UnchangedCommittedSince(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uint64_t generation, std::source_location site = std::source_location::current());
// Returns the generation the blocks were stamped with (0 when the arena is not write-watched):
// UnchangedSince(range, it) holds until the next store over the range.
std::uint64_t MarkWritten(std::uint64_t address, std::size_t bytes, std::source_location site = std::source_location::current());
std::uint64_t MarkPossiblyWritten(std::uint64_t address, std::size_t bytes, std::source_location site = std::source_location::current());
std::uint64_t MarkPossiblyWritten(std::uint64_t address, std::size_t bytes, std::uint64_t& order, std::source_location site = std::source_location::current());
std::uint64_t StoreOwnBytes(std::uint64_t address, std::size_t bytes, const std::function<void()>& store);
std::uint64_t StoreOwnBytes(std::uint64_t address, std::size_t bytes, const std::function<void(std::span<std::byte>)>& store);
std::uint64_t StoreOwnBytes(std::uint64_t address, std::span<const std::byte> source);
void SetOwnStoreCompare(bool compare);
bool StoredOver(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site = std::source_location::current());
bool StoredAllSince_nid_no_patch(std::uint64_t address, std::size_t bytes, std::uint64_t generation);
// Collect epoch: within one epoch a range already collected is not walked again, CollectWrites
// returns the current generation instead. A guest write landing between two collects of the same
// epoch is seen by the next epoch, which is the ordering real hardware gives a CPU write made while a
// submission runs. The driver's own writes (Write, WriteChanged, MarkWritten) stamp their blocks, so
// they are never hidden. APS5_NO_COLLECT_MEMO=1 disables the memo. CollectWritesUncached always walks:
// for decisions about which blocks the CPU wrote while a GPU result was pending (a stale answer there
// stores old texels over reused memory).
// The epoch is per queue worker: BumpCollectEpoch advances the calling thread's epoch to a value no
// other thread ever holds (one worker's walk never satisfies another's check) at the hardware
// ordering points of its queue: the start of a submission, a WAIT_REG_MEM the CPU may have satisfied
// (at entry, polled, timed out, or by another queue's recorded label; a label this queue recorded
// itself orders no CPU write), a device drain, and before reaping completions outside a submission.
// A thread that never bumps (the presenter, game threads inside the flush hook) gets a fresh epoch
// for every collect: nothing is reused without an ordering point. APS5_PACKET_EPOCH=1 makes the
// workers bump before every packet as before. CollectEpochBumps counts the bumps ([guestmem] line).
// With APS5_ARCHB_GEN_TABLE=1 every bump also issues a write-generation ticket (Gen::Issue) for
// `reason`; Committer only adopts the ticket the commit carried (see DrawPipeline::FollowEpoch).
void BumpCollectEpoch(Gen::TicketReason reason = Gen::TicketReason::Other);
void AdoptLatestCollectEpoch();
// The queue-0 committer thread's memoized collects: calls, memo hits, walks and pages walked
// (recorder counters pipe_committer_collect*).
struct CommitterCollectCounters {
    std::atomic<std::uint64_t> collects{0};
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> walks{0};
    std::atomic<std::uint64_t> walkPages{0};
    // APS5_ARCHB_PIPE_COLLECT_SHARE (and its _VERIFY mode): committer walks whose every page queue 0
    // walked after the committed item's ordering point (covered; skipped unless verifying), the pages
    // of those and the covered pages of partly covered walks, time in the committer's walks and in the
    // covered ones (verify), covered walks that found written pages anyway (mismatches) and those
    // pages, sampled own-memo hits re-walked in verify mode and the ones that found written pages,
    // queue-0 walks recorded and their pages, and share-table resets (unwatch or mapping serials).
    std::atomic<std::uint64_t> shareCovered{0};
    std::atomic<std::uint64_t> shareCoveredPages{0};
    std::atomic<std::uint64_t> sharePartialPages{0};
    std::atomic<std::uint64_t> shareWalkNs{0};
    std::atomic<std::uint64_t> shareCoveredNs{0};
    std::atomic<std::uint64_t> shareMismatches{0};
    std::atomic<std::uint64_t> shareMismatchPages{0};
    std::atomic<std::uint64_t> shareMemoSampled{0};
    std::atomic<std::uint64_t> shareMemoDirty{0};
    std::atomic<std::uint64_t> shareRecorded{0};
    std::atomic<std::uint64_t> shareRecordedPages{0};
    std::atomic<std::uint64_t> shareResets{0};
};
CommitterCollectCounters& CommitterCollects();
void MarkCommitterThread();
// Stage 4.2: a draw resolver thread (DrawPipeline::runResolver); its walks feed the committer through
// the collect-share table while ResolveShareActive() (PIPE_RESOLVE_POOL, APS5_PIPE_RESOLVE_SHARE != 0).
void MarkResolverThread();
bool ResolveShareActive();
// APS5_ARCHB_WT_FAULT: true when every page of the range is armed by the fault tracker with an arm
// stamp at most `generation` (a tracker generation; the clocks are shared), so no CPU store reached
// the range since then (it would have faulted). Lock-free; false when tracking is off or a page is
// open, untracked or armed later. Stores the driver stamps without a fault (MarkWritten of GPU
// results) are not covered: pair it with UnchangedSince.
bool FaultUnchangedSince(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site = std::source_location::current());
std::uint64_t FaultClock();
bool FaultUntracked(std::uint64_t address, std::size_t bytes, std::uint64_t generation);
// The committer's tracker-mutex acquisitions that waited, and the nanoseconds waited.
std::atomic<std::uint64_t>& CommitterTrackerWaits();
std::atomic<std::uint64_t>& CommitterTrackerWaitNs();
// The same for the resolver threads together.
std::atomic<std::uint64_t>& ResolverTrackerWaits();
std::atomic<std::uint64_t>& ResolverTrackerWaitNs();
// APS5_ARCHB_WT_FAULT (see GuestMemory.cpp, libc GuestWriteFault.hpp): a driver thread's write faults
// count apart; WtFaultFlip protects the pages written since the last flip (Driver::Present).
void MarkDriverThread();
void WtFaultFlip(bool presenter = true);
struct WtFaultStatus {
    bool active = false;
    std::uint64_t mismatches = 0;
    std::uint64_t faults = 0;
    std::uint64_t armedPages = 0;
    std::uint64_t consumerUnchanged = 0;
    std::uint64_t consumerChanged = 0;
    std::uint64_t consumerFallback = 0;
    std::uint64_t consumerMismatches = 0;
};
// The fault tracker's cumulative state (tests, replay summaries).
WtFaultStatus WtFault();
// Whether the calling thread is the queue-0 committer (MarkCommitterThread).
bool IsCommitterThread();
// APS5_ARCHB_PIPE_COLLECT_SHARE: queue 0 marks itself while it runs a pipelined submission, and
// its completed CollectWrites walks are recorded per page with the collect clock read before the
// walk (the global epoch counter, CollectShareClock()). A committed item belongs to the ordering
// point queue 0 had reached when it enqueued it (its epoch, carried with the epoch token): any walk
// that started at or after that point saw every CPU write ordered before the item, exactly what the
// committer's own walk in the same epoch would establish. FollowCollectShare(clock) gives the
// committer that point (0: unknown, nothing shared); the committer's own ordering points (unlocked
// waits, adopting the latest epoch) raise it. A memoized committer collect whose every page is so
// covered is answered like a memo hit; APS5_ARCHB_PIPE_COLLECT_SHARE_VERIFY=1 walks anyway and
// counts covered walks that found written pages.
void SetCollectShareProducer(bool producer);
void FollowCollectShare(std::uint64_t clock);
// APS5_COLLECT_AHEAD_CENSUS=1 (diagnostic, read once): the committer's memoized collects split by
// phase, inside a draw commit's unlocked preparation (PrepareDraw: target and template precollects,
// whose ranges follow from the queued item alone) or anywhere else (under the GPU mutex, dispatches,
// labels): calls and time in them (memo hits included), walks, pages and time in walks. Per item
// (TakeCommitterItemCollects), the preparation's walks and the committer's ordering requirement when
// the preparation began, for DrawPipeline's enqueue-to-commit window census.
struct CollectAheadCensusCounters {
    std::atomic<std::uint64_t> prelockCalls{0};
    std::atomic<std::uint64_t> prelockNs{0};
    std::atomic<std::uint64_t> prelockWalks{0};
    std::atomic<std::uint64_t> prelockWalkPages{0};
    std::atomic<std::uint64_t> prelockWalkNs{0};
    std::atomic<std::uint64_t> otherCalls{0};
    std::atomic<std::uint64_t> otherNs{0};
    std::atomic<std::uint64_t> otherWalks{0};
    std::atomic<std::uint64_t> otherWalkPages{0};
    std::atomic<std::uint64_t> otherWalkNs{0};
};
bool CollectAheadCensus();
CollectAheadCensusCounters& CollectAheadCensusCounts();
// The committer enters (true) or leaves a draw's unlocked preparation.
void SetCommitterPrelock(bool prelock);
struct CommitterItemCollects {
    std::uint64_t walks = 0;
    std::uint64_t walkNs = 0;
    // max(the committed token's ordering point, the committer's own ordering points) when the
    // preparation began; Unknown (max) when the token's point was not known.
    std::uint64_t required = 0;
    bool prepared = false;
};
CommitterItemCollects TakeCommitterItemCollects();
// APS5_ARCHB_COMMITTER_COLLECT_CENSUS=1 (diagnostic, read once, default off): every collect the
// queue-0 committer makes (memoized or not), by calling site, the kind of item it commits and the
// phase (a draw's unlocked preparation or not): calls and time, memo hits by path, walks (pages,
// time) and failures. Each range is compared with the ranges the committer already collected
// (walked or answered) earlier in the same collect epoch and in the same item: exactly the same
// range, a subset of their union, or a partial overlap (the pages already collected), for walks
// (work a better memo or a narrower range would remove) and for memo hits (calls a per-item or
// per-epoch dedupe before collecting would remove). Walks are also keyed by their driver callers
// (module offsets, s3/symbolize.py). A "[collect-census]" block on stderr every 10 s (cumulative,
// with the flip count). The committer calls BeginCommitterCensusItem before each item.
bool CommitterCollectCensus();
// APS5_ARCHB_FIX_STABLE_FAILED_COLLECTS (runtime, default off): a memoized collect that failed
// (pages not collectable: outside the watched arena, not committed private memory, no-access or
// unwatched views) without refusing any compared-when-shared or unreported page is remembered per
// thread across collect epochs with the page-state serials read before it (mapping and commit
// generations, the protection, unwatch and forget serials); the same range under the same serials
// fails again without its walk (the result, 0, is what the walk would return, and such a failed
// walk has no side effect: libc decides collectability before it reports or resets anything). A
// failure that met compared or unreported pages (noteRefusedWalk stamps them and refreshes the
// refused blocks UnchangedSince reads) is never remembered. _VERIFY=1 walks anyway and counts a
// remembered failure that now succeeds or meets such pages (fix_stable_failed_collects_mismatches).
struct StableFailedCollectCounters {
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> inserts{0};
    std::atomic<std::uint64_t> refused{0};
    std::atomic<std::uint64_t> verified{0};
    std::atomic<std::uint64_t> mismatches{0};
};
StableFailedCollectCounters& StableFailedCollects();
void BeginCommitterCensusItem(std::uint8_t kind);
// Queue 0 at each flip (the census divides by flips).
void NoteCensusFlip();
std::uint64_t CensusFlips();
// The latest collect epoch any thread began (a walk starting now follows every bump up to it).
std::uint64_t CollectClock();
std::uint64_t ProofSerial();
std::uint64_t ResourceProofSerial();
bool ResourceEpochProofsEnabled();
void BreakProofs();
void MarkProofThread();
std::uint64_t CollectEpoch();
std::uint64_t CollectEpochBumps();
std::uint64_t ThreadCollectEpoch();
std::uint64_t UnwatchSerial();
std::uint64_t CollectWritesUncached(std::uint64_t address, std::size_t bytes, std::source_location site = std::source_location::current());
// Write-tracking census (APS5_ARCHB_WRITE_TRACKING_CENSUS=1): per queue thread and calling site, the
// collections, their walks (repeats within one collect epoch), OS write-watch calls, pages and time.
// The frame replay prints it per pass ([wt-census] lines) and resets it at the start of each pass.
void ReportWriteCensus(std::uint64_t pass, std::uint64_t frames);
void ResetWriteCensus();
// The tracker's current generation (every collect and MarkWritten bumps it): a stamp taken after
// a set of driver stores, for UnchangedSinceCollected to compare against later. Read under the
// tracker mutex, so every collect that bumped before it has finished its walk and every later
// collect stamps newer.
std::uint64_t TrackerGeneration();
// Tests only (agc_archb_gen_tracker_wrap; the driver never calls it): moves the tracker generation
// forward to `generation`, never back, so a test can cross 2^32 without 2^32 collects.
void AdvanceTrackerGenerationForTest(std::uint64_t generation);
std::uint64_t NewestStamp(std::uint64_t address, std::size_t bytes);
// NewestStamp split in its two parts: the newest block stamp of the range, and the newest of its
// unstamped-store slots (driver stores outside write watching, hashed by block modulo 4 GiB) with the
// block holding that slot and the last store that set it. For diagnostics (one tracker lock).
struct StampSplit {
    std::uint64_t block = 0;
    std::uint64_t unstamped = 0;
    std::uint64_t slotAddress = 0;
    std::uint64_t storeBegin = 0;
    std::uint64_t storeEnd = 0;
};
StampSplit SplitNewestStamp(std::uint64_t address, std::size_t bytes);
// APS5_ARCHB_FIX_UNSTAMPED_ALIAS (see lockedUnchangedCommittedSince in GuestMemory.cpp), cumulative and
// process-wide: committed-range answers only an aliased unstamped slot made "changed" (switch on or
// off), those served with the switch on, and newer slots whose last store overlapped the range in an
// unstamped block (left "changed"; must stay 0).
struct UnstampedAliasCounters {
    std::atomic<std::uint64_t> aliased{0};
    std::atomic<std::uint64_t> served{0};
    std::atomic<std::uint64_t> mismatches{0};
};
UnstampedAliasCounters& UnstampedAlias();
std::uint64_t UntrackedHint(std::uint64_t address, std::size_t bytes, std::source_location site = std::source_location::current());
// Whether no CPU write touched the range since `generation`: an uncached resetting collect of the
// range's pages first (a game store not collected yet becomes a block stamp), then the compare over
// the stamps collects make (dirty pages), not the MarkWritten stamps of the driver's own GPU label
// records, which share the title's label blocks. False when the arena is not write-watched.
bool UnchangedSinceCollected(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site = std::source_location::current());
bool UnchangedSinceLastCollect(std::uint64_t address, std::size_t bytes, std::uint64_t generation);
// Per 64 KiB tracker block of [address, address + bytes) (block 0 holds `address`): `changed[k]`
// receives whether the block was stamped after generations[k] (UnchangedSince over that block
// alone), `cpu[k]`, when given, whether a collect stamped it after (a CPU store, as
// UnchangedSinceCollected tells them apart from the driver's MarkWritten stamps, but without its
// walk: the caller collected the range first). One tracker lock for the whole range. Outside the
// watched arena, or for a generation of 0, a block reads as changed.
constexpr std::uint8_t BlockUnchanged = 0;
constexpr std::uint8_t BlockWritten = 1;
bool ChangedBlocks(std::uint64_t address, std::size_t bytes, std::span<const std::uint64_t> generations, std::span<std::uint8_t> changed, std::span<std::uint8_t> cpu = {}, std::source_location site = std::source_location::current());
// APS5_ARCHB_FIX_SNAPSHOT_PAGE_REFRESH: the parts of [address, address + bytes) that may have changed
// since `generation`, per 4 KiB page: a page is left out exactly when UnchangedSince over that page
// alone (clipped to the range) would answer true under the tracker mutex (no refusal in its block,
// and its block not stamped after the generation or no CPU, walk or driver store recorded over the
// page after it). Appended to `runs` as merged, ascending [begin, end) runs. One tracker lock. False
// (runs empty) when the range is not tracked or the generation is 0: the caller then treats the whole
// range as changed.
bool ChangedPages(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::vector<std::pair<std::uint64_t, std::uint64_t>>& runs, std::source_location site = std::source_location::current());

// Serializes device work: draws, dispatches, presentation and the deferred write-backs below. The
// mutex is recursive; it is wrapped so every acquisition (std::lock_guard at any site) measures how
// long it waited: the graphics worker's frame is mostly spent waiting for this lock behind the
// compute workers, and that wait has to stay visible. Under APS5_PROFILE_DRAW each thread reports its
// waits in a [lock] line every 10 s (APS5_NO_LOCK_PROFILE=1 turns the timing off); a queue worker tags
// its thread with TagGpuLockThread so the line names the queue.
class GpuMutexType {
public:
    void lock();
    bool try_lock();
    void unlock();
    // Whether the calling thread holds the mutex (at any depth): the APS5_ASSERT_GPU_LOCK checks
    // (AssertGpuLockHeld) and code that must know whether it runs inside the device lock.
    bool HeldByThisThread() const;
    // The calling thread's recursion depth (0 when it does not hold the mutex): 1 means its
    // current acquisition is the outermost, so code that releases the mutex around a wait
    // (Recorder::SyncThrough for the flush hook) gives up no caller's hold.
    std::uint32_t DepthOnThisThread() const;

private:
    void enter();
    bool tryEnter();
    void acquired();
    Mutex mutex;
    // The holder's thread token and recursion depth, written by the holder only; another thread
    // reads the owner just to see that it is not itself.
    std::atomic<const void*> owner{nullptr};
    std::uint32_t depth = 0;
};
GpuMutexType& GpuMutex();
// Called on the thread that just gave up its outermost hold, right after the mutex was released:
// work the hold deferred to run without it (the recorder's release of the objects completed batches
// kept). One hook, set once; it must not take GpuMutex itself.
void SetGpuUnlockHook(void (*hook)());
// APS5_HOLD_LOG item marks (diag): on a thread that called EnableHoldMarks, each mark adds a record
// {0xffffffff, mark, QPC, OS thread} to the hold log (readers that predate marks skip thread 0xffffffff).
// Without APS5_HOLD_LOG or on other threads a mark is a thread-local load and a branch.
enum class HoldMark : std::uint32_t { ItemTop = 1, ItemTaken, CommitBegin, CommitEnd, ItemDone, PrelockBegin, PrelockEnd, LockBegin, UnlockHookEnd };
void EnableHoldMarks();
void HoldMarkSlow(HoldMark mark);
struct HoldMarksSlot {};
inline bool HoldMarksOn() { return HostThreadSlot<bool, HoldMarksSlot>::Get(); }
inline void HoldMarksOn(bool value) { HostThreadSlot<bool, HoldMarksSlot>::Set(value); }
inline void HoldLogMark(HoldMark mark) {
    if (HoldMarksOn()) HoldMarkSlow(mark);
}
// APS5_ARCHB_PIPE_COMMITTER_MUTEX_PRIORITY (runtime, sampled per acquisition): `queued` says whether
// the queue-0 committer has work queued or in progress. While it does, a queue worker other than
// queue 0 (a thread tagged by TagGpuLockThread with a nonzero queue) waits before its outermost
// lock(), spinning up to GpuLockPriorityWindow(), so its holds fall into the committer's idle gaps
// rather than between two commits (2026-11-03 profile: the committer waited ~6.5 ms per frame
// behind compute-queue holds and idled ~19 ms). Queue 0, the committer, the presenter and untagged
// threads never wait; try_lock never waits. One hook, set once.
void SetGpuLockPriorityHook(bool (*queued)());
// The longest a queue worker waits for the committer (APS5_PIPE_MUTEX_PRIORITY_US, default 1000 us).
std::chrono::nanoseconds GpuLockPriorityWindow();
std::uint64_t GpuHoldSerial();
std::uint64_t ThreadGpuHeldNs();
// Debug aid (APS5_ASSERT_GPU_LOCK=1): aborts with a [lock] line naming `where` when the calling
// thread does not hold GpuMutex. Recording, submits, command batches and the detiler's pools must
// only be reached under it (a resource build's stage A runs without it, see ShaderResources).
void AssertGpuLockHeld(const char* where);
void TagGpuLockThread(std::uint32_t queue);
// The presenter stays untagged (its tag is a queue id to the recorder) but takes its own column
// of the [lock] holder matrix, apart from the game threads.
void MarkPresenterThread();
// The queue tag of the calling thread (0xffffffff when untagged), for reports that name the thread.
std::uint32_t GpuLockThreadTag();
// Lock sites (APS5_PROFILE_DRAW): a caller names the site of its next lock() so the [lock] line
// splits acquisitions and waits per site (whose holds queue 0 waits behind is then measurable). The
// tag is consumed by that acquisition, so a nested lock inside the flush hook is attributed to the
// hook, and an acquisition nobody tagged counts as "other". Try-only acquisitions are not counted
// as acquisitions; the hold they start is timed under "try". Holds are timed per site as well (the
// outermost acquisition of a thread until its unlock; APS5_NO_HOLD_PROFILE=1 turns that off), so
// the line shows who holds the mutex, not only who waits for it.
enum class GpuLockSite : std::uint8_t { Other = 0, Dispatch, Indirect, Draw, Hook, Wait, Label, Flush, Present, Fill, Copy, End, Try, Count };
void TagGpuLockSite(GpuLockSite site);
void NoteGpuLabel();
// A GPU wait (a fence or timeline wait that found its work unfinished) made while the calling
// thread holds GpuMutex: counted on the thread's [lock] line as 'locked GPU waits' (APS5_PROFILE_DRAW).
void NoteLockedGpuWait(double ms);
GpuLockSite GpuLockHoldSite();
void NoteUnlockedGpuWait(GpuLockSite site, double ms);
// Bytes the calling thread's write-watch collects walked so far (0 without the memory profile):
// a caller reads the difference across a span of its own work.
std::uint64_t ThreadCollectedBytes();
// A code address (a return address) as an offset into its module, in the form of the [guestmem]
// callers (symbolized with nm against the driver's image). Takes the loader lock: for reports only.
unsigned long long CodeOffset(const void* address);

// Deferred GPU writes. The graphics layer keeps the results of storage images on the GPU until guest
// memory is read; it registers a hook that stores every pending result overlapping a range, which
// every read and store through this interface calls first. Code that reads guest memory directly
// calls FlushGpuWrites itself.
void SetFlushHook(void (*hook)(std::uint64_t address, std::size_t bytes));
auto CurrentFlushHook() -> void (*)(std::uint64_t address, std::size_t bytes);
// APS5_ARCHB_C_LAZY_STORE_HOOK: called by every driver store into guest memory (StoreOwnBytes, Write,
// WriteChanged) and every driver read through this interface (Read, ReadCommitted[Unsynced],
// EqualsCommittedUnsynced, CompareMapped, CopyMapped) before it touches the bytes, outside the tracker
// lock: lazy publication publishes and opens a lazy range there, so the access does not fault on it
// under the GPU mutex.
void SetLazyStoreHook(void (*hook)(std::uint64_t address, std::size_t bytes));
// Arch C P5c (C_LAZY_FENCE): called first by the same driver stores and reads, so a deferred writer wait is
// made before the access touches the range.
void SetDeferredFenceHook(void (*hook)(std::uint64_t address, std::size_t bytes));
enum class TrackerEvent : std::uint8_t { Stamp, Unwatch, Forget };
void SetStampObserver(void (*observer)(std::uint64_t begin, std::uint64_t end, TrackerEvent event));
void SetArmObserver(void (*observer)());
void SetWriteProfileHook(void (*hook)(std::uint64_t address, std::size_t bytes, std::uint32_t queue, std::uint32_t opcode));
void SetReadGuard(void (*guard)(std::uint64_t address, std::size_t bytes));
void GuardRead(std::uint64_t address, std::size_t bytes);
void FlushGpuWrites(std::uint64_t address, std::size_t bytes);
void PrepareHostWrite(std::uintptr_t address, std::size_t bytes);
bool StopHostWrites(std::uint32_t boundMs);
std::uint64_t LockedHostWrites();
struct HostWriteCounts {
    std::uint64_t calls = 0;
    std::uint64_t waited = 0;
    double waitedMs = 0;
    double maxWaitedMs = 0;
};
HostWriteCounts HostWritesCounted();
void SetWaitedMsProvider(double (*provider)());
std::string HostWriteReport();
// Moves with every ForgetPages call (the libc invalidator: memory unmapped or re-registered): odd
// while the call stores its page states, moved past it after, so a reader that loads an even
// value before consulting the page states and reading directly, and the same value after, knows
// the states it consulted were current and no page it read changed its mapping meanwhile.
std::uint64_t ForgetSerial();
std::uint64_t ForgetStamp(std::uint64_t address, std::size_t bytes);

// Attribution of guest memory accesses (the [hooksync] line in Recorder.cpp and the read-site
// counts of the [guestmem] line, APS5_PROFILE_DRAW): a sync the flush hook makes for an access has
// to be charged to the PM4 packet and the driver code that made the access. A queue worker names
// the packet it executes at the top of its packet loop (SetCurrentPacket; opcode 0xffff is the
// flip); a thread that runs no packets (the presenter, game threads) reads back NoPacket. The main
// read paths name themselves with a ReadSiteScope; an access made by none of them is attributed by
// its return addresses instead (CaptureCallerOffsets).
struct PacketTag {
    std::uint32_t opcode;
    std::uint32_t queue;
};
constexpr std::uint32_t NoPacket = 0xfffffffeu;
void SetCurrentPacket(std::uint32_t opcode, std::uint32_t queue);
PacketTag CurrentPacket();

// Store: a CPU write (Write/WriteChanged) rather than a read; a hook sync made for one orders a
// write after a pending GPU write, so whether the bytes change says nothing about its necessity.
// MirrorRefresh: an address-based build bringing an image mirror up to date with guest memory
// (GuestBufferMemory.cpp: the flush before a writable mirror's compare, a new mirror's fill).
// DrawCache: a draw entry's per-stage value validation (Driver.cpp drawCache), where a draw
// capture's hook waits reappear on a hit.
// DispatchHandler, DispatchPrepare, DispatchDevice: a dispatch packet's handler outside the finer sites,
// its PrepareRecipe/PrepareDispatch, and its device call (resource proofs, refreshes, recording).
// Submission, Packet, PacketPrepare, PacketFlush: a queue worker's Driver::execute outside its packets,
// a packet's body outside the finer sites, preparePacketMemory, and flushBetweenPackets.
// Every enumerator needs its name in ReadSiteName's table (GuestMemory.cpp).
enum class ReadSite : std::uint8_t { Unknown = 0, Capture, DispatchCache, TextureCompare, TextureRead, BufferUpload, IndexBuffer, VertexBuffer, Registers, IndirectArguments, Wait, Label, Scanout, Store, MirrorRefresh, DrawCache, FillInput, DispatchHandler, DispatchPrepare, DispatchDevice, Submission, Packet, PacketPrepare, PacketFlush, Count };
const char* ReadSiteName(ReadSite site);
// Sets the calling thread's read site and returns the previous one (nested scopes restore it).
ReadSite SetReadSite(ReadSite site);
ReadSite CurrentReadSite();
class ReadSiteScope {
public:
    explicit ReadSiteScope(ReadSite site) : previous(SetReadSite(site)) {}
    ~ReadSiteScope() { SetReadSite(previous); }
    ReadSiteScope(const ReadSiteScope&) = delete;
    ReadSiteScope& operator=(const ReadSiteScope&) = delete;

private:
    ReadSite previous;
};
// Return addresses above the caller as offsets into the driver's module (the same form as the
// [guestmem] callers, symbolized with nm against the driver's image; an address outside the module
// is written as 0), newest first, skipping `skip` frames above the caller; returns how many were
// written. Stack unwinding, so only for rare, profiled events.
std::size_t CaptureCallerOffsets(std::span<unsigned long long> frames, unsigned skip);

}

extern "C" void AgcDriverCheckGuestMemory_nid_postfix(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable = false);
extern "C" void AgcDriverStoreGuestLabel_nid_postfix(std::uint64_t* pointer, std::uint64_t value);

#endif
