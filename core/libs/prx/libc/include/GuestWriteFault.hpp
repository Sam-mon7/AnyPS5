#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTWRITEFAULT_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTWRITEFAULT_HPP

#include <cstddef>
#include <cstdint>

// Write tracking by page protection (Stage 4 option 2, APS5_ARCHB_WT_FAULT; Windows only, every call a
// no-op elsewhere and until GuestWtFaultEnable).
//
// Tracked 4 KiB guest pages are write-protected (PAGE_READONLY, or PAGE_EXECUTE_READ for a guest
// PAGE_EXECUTE_READWRITE page). The first user-mode store faults; GuestWtFaultHandle (called first by
// libc's vectored handler, before any other) stamps the page and restores the guest's protection, so
// every later store runs at full speed. At flips GuestWtFaultArm protects the pages opened since in
// contiguous runs again. A page's state word lives in a lock-free two-level table (2 MiB leaves of
// 512 words, allocated outside the handler and never freed):
//
//   Untracked  never asked about (or forgotten after a mapping or protection change)
//   Open       writable; may be written at any moment, so it reads as changed
//   Arming     being protected by GuestWtFaultArm (still reads as changed)
//   Armed      protected; unchanged since its stamp, which GuestWtFaultArm takes from the clock after
//              the protection took effect (every store after the stamp would have faulted)
//   Opening    the handler or a hold is restoring the guest protection (reads as changed)
//   Excluded   not trackable (not committed private read-write memory); until its next forget
//
// Pages become candidates through GuestWtFaultTrack (the driver's write-tracker walks). Only private
// committed pages whose real protection is exactly PAGE_READWRITE / PAGE_EXECUTE_READWRITE are armed,
// so shared views (which WindowsMappings protects for its own shared-page tracking) are never touched.
//
// Kernel-mode writers fail instead of faulting on a protected page: they hold their destination open
// (GuestWtFaultHold/Release, done by GuestArenaBeginHostWrite/EndHostWrite and the host-import
// bracket). Mapping and protection changes forget the pages they touch (GuestArena entry points and
// GuestArenaVirtualProtect_nid_no_patch). The clock is shared with the driver's write-tracker
// generations (GuestWtFaultClock), so a page stamp compares directly with a generation.
namespace GuestArena {

#ifdef _WIN32
void RefreshFaultStacks(std::uintptr_t address, std::size_t bytes);
#endif

enum class WtFaultAnswer : std::uint8_t { Unchanged, Changed, Untracked };

struct WtFaultCounters {
    std::uint64_t faults = 0;          // faults the handler resolved (pages opened by a store)
    std::uint64_t driverFaults = 0;    // of which on threads marked as driver threads
    std::uint64_t handlerNs = 0;       // time inside the handler, all threads (excludes the kernel's dispatch)
    std::uint64_t driverHandlerNs = 0; // of which on driver threads
    std::uint64_t retries = 0;         // faults on a page another thread was opening (re-executed)
    std::uint64_t foreign = 0;         // write faults inside tracked leaves that were not ours
    std::uint64_t armCalls = 0;
    std::uint64_t armedPages = 0;
    std::uint64_t armRuns = 0;
    std::uint64_t armNs = 0;
    std::uint64_t armProtectNs = 0;    // of which VirtualProtect
    std::uint64_t armQueries = 0;      // VirtualQuery calls made to check runs
    std::uint64_t armLockNs = 0;       // of armNs, waiting for the tracker's mutex
    std::uint64_t armScanNs = 0;       // of armNs, scanning leaves for open pages
    std::uint64_t armQueryNs = 0;      // of armNs, VirtualQuery
    std::uint64_t keptDirty = 0;       // of keptHot, open pages kept because write watching reports them stored (GuestWtFaultArmKeepDirty)
    std::uint64_t armDirtyNs = 0;      // of armNs, the non-resetting write-watch queries for that
    std::uint64_t armLeaves = 0;       // leaves scanned
    std::uint64_t leaves = 0;          // leaves allocated (2 MiB of guest memory each)
    std::uint64_t excluded = 0;        // pages found untrackable by an arm
    std::uint64_t keptHot = 0;         // open pages an arm left open (driver-hot or held)
    std::uint64_t holds = 0;
    std::uint64_t heldOpened = 0;      // armed pages a hold opened
    std::uint64_t opens = 0;           // armed pages GuestWtFaultOpen opened
    std::uint64_t forgotten = 0;       // tracked pages forgotten by mapping or protection changes
    std::uint64_t tracked = 0;         // pages made candidates
    std::uint64_t verifyWalks = 0;
    std::uint64_t verifyDirty = 0;
    std::uint64_t verifyUntracked = 0; // dirty pages write watching reported that are not tracked
    std::uint64_t verifyMismatches = 0;
    std::uint64_t queries = 0;         // GuestWtFaultChangedSince calls
    std::uint64_t logAppends = 0;
    std::uint64_t logLost = 0;
    std::uint64_t coldRearmed = 0;
    std::uint64_t watchFaults = 0;
    std::uint64_t watchHandlerNs = 0;
    std::uint64_t watchedPages = 0;
    std::uint64_t openSplitRuns = 0;   // opened runs one VirtualProtect refused (they spanned allocations), restored region by region
    std::uint64_t openSplitPieces = 0; // regions those took
    std::uint64_t openRefused = 0;     // pages an open could not restore (left untracked)
};

struct WtFaultMismatch {
    std::uintptr_t page = 0;
    std::uint64_t word = 0;
    std::uint64_t lastReset = 0;
    std::uint64_t clock = 0;
};

extern "C" {

// Turns tracking on (idempotent). `verify`: keep the per-page reset clocks GuestWtFaultVerifyWalk needs.
// The clock is raised to at least `clockAbove` (the driver's current generation).
void GuestWtFaultEnable_nid_no_patch(bool verify, std::uint64_t clockAbove);
bool GuestWtFaultEnabled_nid_no_patch();
// The vectored handler's entry: a write access violation at `address`. True when the page was ours
// and execution may continue (the store re-executes); false otherwise (the next handler decides).
bool GuestWtFaultHandle_nid_no_patch(std::uintptr_t address);
// Marks the calling thread as a driver thread (its faults count apart).
void GuestWtFaultMarkDriverThread_nid_no_patch();
// Pages of the range become candidates (Untracked -> Open); the next arm protects them.
void GuestWtFaultTrack_nid_no_patch(std::uintptr_t address, std::size_t bytes);
// Pages of the range were stored by the driver: an arm in the next `frames` flips leaves them open
// (the driver keeps storing them, each first store after an arm would fault).
void GuestWtFaultNoteDriverStore_nid_no_patch(std::uintptr_t address, std::size_t bytes);
// Pages of the range were found stored in this frame (a write-watch walk reported them dirty): like a
// fault, this keeps them open across the next flip, so a page the guest stores every frame faults once
// instead of once per frame (APS5_WT_FAULT_GUEST_HOT=0 turns both off).
void GuestWtFaultNoteStored_nid_no_patch(std::uintptr_t address, std::size_t bytes);
// Protects every open candidate not held open, in contiguous runs; one clock value stamps them all.
// Returns the number of runs armed and stores the first `capacity` of them as [begin, end) pairs in
// `runs` (2 * capacity words; null with capacity 0 when not wanted).
std::size_t GuestWtFaultArm_nid_no_patch(std::uintptr_t* runs, std::size_t capacity);
// GuestWtFaultArm that first asks write watching (without resetting) about the candidates: pages stored
// since their last resetting walk stay open, and their runs (at most `dirtyCapacity`, coalesced; a page
// that does not fit is armed) go to `dirtyRuns`. The caller walks those runs with a resetting walk, so a
// page stays open only while it is stored between flips (APS5_ARCHB_WT_FAULT_DIRTY_ARM).
std::size_t GuestWtFaultArmKeepDirty_nid_no_patch(std::uintptr_t* runs, std::size_t capacity, std::uintptr_t* dirtyRuns, std::size_t dirtyCapacity, std::size_t* dirtyCount);
// Forgets the pages of the range (a mapping or protection change made their protection the caller's).
void GuestWtFaultForget_nid_no_patch(std::uintptr_t address, std::size_t bytes);
// Opens the armed pages of the range and keeps them open until the matching release (kernel-mode
// writes, host imports). Nests.
void GuestWtFaultHold_nid_no_patch(std::uintptr_t address, std::size_t bytes);
void GuestWtFaultRelease_nid_no_patch(std::uintptr_t address, std::size_t bytes);
// Opens the armed pages of the range now (an explicit store); returns the pages opened.
std::size_t GuestWtFaultOpen_nid_no_patch(std::uintptr_t address, std::size_t bytes);
// Whether the range is unchanged since clock value `since`: Unchanged when every page is armed with a
// stamp at most `since`, Untracked when some page is untracked or excluded, Changed otherwise.
WtFaultAnswer GuestWtFaultChangedSince_nid_no_patch(std::uintptr_t address, std::size_t bytes, std::uint64_t since);
// Consumers: which pages of [first, stop) may hold a store since their last sync. Pages armed no later
// than their last sync hold none (a store would have faulted); `open` receives the runs of every other
// page (open now, being armed or opened, or armed after the last sync), which need a write-watch walk
// of their own: the answer is exactly a whole-range walk's. `dirty` is unused (count 0). Then every page
// takes `clock` as its last sync. In: the capacities (pages, runs); out: the counts. False, with nothing changed, when a page is untracked or excluded or
// a list would overflow (the caller walks write watching instead). The sync clock is the reset clock
// GuestWtFaultVerifyWalk keeps.
bool GuestWtFaultSync_nid_no_patch(std::uintptr_t first, std::uintptr_t stop, std::uint64_t clock, void** dirty, std::size_t* dirtyCount, std::uintptr_t* open, std::size_t* openCount);
// The shared clock: GuestWtFaultClockBump returns a new value (the driver's tracker generations).
std::uint64_t GuestWtFaultClock_nid_no_patch();
std::uint64_t GuestWtFaultClockBump_nid_no_patch();
// Verify: a resetting write-watch walk of [first, stop) that began at clock `walkClock` reported the
// dirty `pages`. Each dirty tracked page must be open (or arming/opening) or armed with a stamp newer
// than the clock of the previous reset walk that covered it; otherwise it is a mismatch (a store the
// fault tracker never saw). Then every page of the range takes `walkClock` as its reset clock (also
// without verify: the consumers' sync clock). Returns the mismatches of this call; `mismatch`
// (optional) receives the first one.
std::uint64_t GuestWtFaultVerifyWalk_nid_no_patch(std::uintptr_t first, std::uintptr_t stop, void* const* pages, std::size_t count, std::uint64_t walkClock, WtFaultMismatch* mismatch);
// The protection a guest-memory probe should see: a protection read from VirtualQuery for
// [address, address + bytes) where every page is armed, arming or opening reads as the guest's
// writable protection; any other answer is returned unchanged.
unsigned long GuestWtFaultGuestProtection_nid_no_patch(std::uintptr_t address, std::size_t bytes, unsigned long protection);
// Whether any page of the range is protected by the tracker right now (armed, arming or opening).
bool GuestWtFaultProtects_nid_no_patch(std::uintptr_t address, std::size_t bytes);
// Cumulative counters (all threads).
void GuestWtFaultCounters_nid_no_patch(WtFaultCounters* counters);
// Test support: the raw state word of a page (0 when untracked).
std::uint64_t GuestWtFaultWord_nid_no_patch(std::uintptr_t address);
// Diagnostics: whether a sync would skip the page now (armed no later than its last sync).
bool GuestWtFaultSyncedClean_nid_no_patch(std::uintptr_t address);
std::uint64_t GuestWtFaultLogHead_nid_no_patch();
std::size_t GuestWtFaultLogRead_nid_no_patch(std::uint64_t* cursor, std::uint64_t* entries, std::size_t capacity, bool* lost);
void GuestWtFaultSetCold_nid_no_patch(std::uintptr_t address, std::size_t bytes, bool cold);
void GuestWtFaultLogAppendForTest_nid_no_patch(std::uint64_t entry);
bool GuestWtFaultStoredLastFrame_nid_no_patch(std::uintptr_t address);
std::size_t GuestWtFaultWatch_nid_no_patch(std::uintptr_t address, std::size_t bytes);
std::size_t GuestWtFaultWatchCauses_nid_no_patch(std::uintptr_t address, std::size_t bytes, std::uint64_t* causes);
std::size_t GuestWtFaultUnwatch_nid_no_patch(std::uintptr_t address, std::size_t bytes);
std::size_t GuestWtFaultWatchLazy_nid_no_patch(std::uintptr_t address, std::size_t bytes);
std::size_t GuestWtFaultWatchedPages_nid_no_patch(std::uintptr_t address, std::size_t bytes);
void GuestWtFaultSetLazyService_nid_no_patch(void (*service)(std::uintptr_t page, bool write));
void GuestWtFaultServeLazyRange_nid_no_patch(std::uintptr_t address, std::size_t bytes);
void GuestWtFaultLazyCounters_nid_no_patch(std::uint64_t* services, std::uint64_t* nested);
// APS5_ARCHB_C_WATCH_SPLIT: a watch run one VirtualProtect refuses (it spans allocations) is protected
// one VirtualQuery region at a time; the counters are the runs split and the pieces protected.
void GuestWtFaultSetWatchSplit_nid_no_patch(bool split);
// Diagnostics (the crash handler notes every access violation's context while a watch exists).
void GuestWtFaultNoteFaultContext_nid_no_patch(std::uintptr_t ip, std::uintptr_t sp);
void GuestWtFaultFaultContext_nid_no_patch(std::uintptr_t* ip, std::uintptr_t* sp);
// APS5_ARCHB_C_LAZY_STORE_HOOK: the driver publishes a lazy range itself, as the service would (a fault on
// a lazy page inside only opens it). Enter returns false when the thread is already inside a service
// (then Leave must not be called).
bool GuestWtFaultEnterLazyService_nid_no_patch();
void GuestWtFaultLeaveLazyService_nid_no_patch();
void GuestWtFaultWatchSplitCounters_nid_no_patch(std::uint64_t* runs, std::uint64_t* pieces);
bool GuestWtFaultHandleRead_nid_no_patch(std::uintptr_t address);
std::uint64_t GuestWtFaultWatchLogHead_nid_no_patch();
std::size_t GuestWtFaultWatchLogRead_nid_no_patch(std::uint64_t* cursor, std::uint64_t* entries, std::uint64_t* stamps, std::size_t capacity, bool* lost);
std::uint64_t GuestWtFaultTicks_nid_no_patch(double* nsPerTick);

}

// State bits of a page word: state in bits 0-2, guest executable in bit 3, checked in bit 4 (an arm
// found the page committed private read-write memory; any forget clears it), stamp from bit 5.
inline constexpr std::uint64_t WtFaultUntracked = 0;
inline constexpr std::uint64_t WtFaultOpen = 1;
inline constexpr std::uint64_t WtFaultArming = 2;
inline constexpr std::uint64_t WtFaultArmed = 3;
inline constexpr std::uint64_t WtFaultOpening = 4;
inline constexpr std::uint64_t WtFaultExcluded = 5;
inline constexpr std::uint64_t WtFaultStateMask = 7;
inline constexpr std::uint64_t WtFaultExecBit = 8;
inline constexpr std::uint64_t WtFaultCheckedBit = 16;
inline constexpr std::uint64_t WtFaultFlagMask = WtFaultExecBit | WtFaultCheckedBit;
inline constexpr unsigned WtFaultStampShift = 5;
inline constexpr std::uint64_t WtFaultLogStore = 1;
inline constexpr std::uint64_t WtFaultLogHold = 2;
inline constexpr std::uint64_t WtFaultLogOpen = 3;
inline constexpr std::uint64_t WtFaultLogForgetBegin = 4;
inline constexpr std::uint64_t WtFaultLogForgetEnd = 5;
inline constexpr std::size_t WtFaultLogSlots = 65536;
inline constexpr std::uint64_t WtWatchRead = 1;
inline constexpr std::uint64_t WtWatchWrite = 2;
inline constexpr std::uint64_t WtWatchHold = 3;
inline constexpr std::uint64_t WtWatchOpen = 4;
inline constexpr std::uint64_t WtWatchForget = 5;
inline constexpr std::uint64_t WtWatchKindMask = 7;
inline constexpr std::uint64_t WtWatchDriver = 8;
inline constexpr std::size_t WtWatchSkipNoLeaf = 0;
inline constexpr std::size_t WtWatchSkipUntracked = 1;
inline constexpr std::size_t WtWatchSkipExcluded = 2;
inline constexpr std::size_t WtWatchSkipUnchecked = 3;
inline constexpr std::size_t WtWatchSkipExec = 4;
inline constexpr std::size_t WtWatchSkipHeld = 5;
inline constexpr std::size_t WtWatchSkipBusy = 6;
inline constexpr std::size_t WtWatchSkipWatched = 7;
// Claimed, but its VirtualProtect failed (a run across allocations without C_WATCH_SPLIT).
inline constexpr std::size_t WtWatchSkipProtect = 8;
inline constexpr std::size_t WtWatchSkipCauses = 9;

}

#endif
