#include "prx/libc/include/CriticalPathTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/Timeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/EpochMemoMap.hpp"
#include "prx/libSceAgcDriver/Execution/include/HandoffSpin.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/Stage4Diag.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"
#include "prx/libSceAgcDriver/Execution/include/WriteWatchCoverage.hpp"
#include "prx/libSceAgcDriver/Execution/include/ThreadScratch.hpp"
#include "ThreadOwned.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"
#include <mutex>
#include <array>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <bit>
#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdint>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#ifndef __APPLE__
#include <link.h>
#endif
#include <pthread.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <unistd.h>
#include "prx/libc/include/specifics/linux/HostMapping.hpp"
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#endif
#endif

namespace AgcDriver::GuestMemory {
namespace {
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("AGC driver: ") + reason);
}

// APS5_ARCHB_FIX_WRITE_TRACKING_BOOKKEEPING: cheaper bookkeeping around the write tracker with the same
// answers: a per-thread exact-range table in front of the collect memo, page-state runs compared a
// word (eight pages) at a time inside each chunk, and the host-region overlap test by binary search.
// APS5_VERIFY_WRITE_TRACKING_BOOKKEEPING=1 also takes the original path every time, counts every
// disagreement (fix_wt_bookkeeping_mismatches) and uses the original answer. The path counters are
// added in batches of 256 per thread, so the recorder never sees a hit cost more than a few loads.
namespace bookkeeping {

enum Counter : std::uint8_t { Hits, Runs, Regions, Verified, CounterCount };
constexpr const char* CounterNames[CounterCount] = {"fix_wt_bookkeeping_hits", "fix_wt_bookkeeping_runs", "fix_wt_bookkeeping_regions", "fix_wt_bookkeeping_verified"};
constexpr std::uint32_t CountBatch = 256;

struct Pending {
    std::array<std::uint32_t, CounterCount> counts{};
};
struct PendingSlot {};

bool active() {
    return ArchB::Switches::Enabled(ArchB::Feature::FixWriteTrackingBookkeeping);
}

bool verify() {
    static const bool enabled = [] {
        const auto* value = std::getenv("APS5_VERIFY_WRITE_TRACKING_BOOKKEEPING");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

void count(Counter counter) {
    auto* pending = HostThreadSlot<Pending*, PendingSlot>::Get();
    if (pending == nullptr) {
        ShaderRecompiler::ThreadOwned(pending);
        HostThreadSlot<Pending*, PendingSlot>::Set(pending);
    }
    if (++pending->counts[counter] < CountBatch) return;
    pending->counts[counter] = 0;
    ArchB::Recorder::Count(CounterNames[counter], CountBatch);
}

void mismatch(const char* what) {
    ArchB::Recorder::Count("fix_wt_bookkeeping_mismatches");
    ArchB::Recorder::Count(what);
}

#ifdef _WIN32
bool hostRegionOverlaps(std::uintptr_t address, std::size_t bytes) {
    if (!active()) {
        if (ArchB::Switches::Enabled(ArchB::Feature::PageStateMemo)) return GuestArena::GuestArenaHostRegionOverlapsSorted_nid_no_patch(address, bytes);
        return GuestArena::GuestArenaHostRegionOverlaps_nid_postfix(address, bytes);
    }
    const bool sorted = GuestArena::GuestArenaHostRegionOverlapsSorted_nid_no_patch(address, bytes);
    count(Regions);
    if (!verify()) return sorted;
    const bool scanned = GuestArena::GuestArenaHostRegionOverlaps_nid_postfix(address, bytes);
    count(Verified);
    if (sorted != scanned) mismatch("fix_wt_bookkeeping_region_mismatches");
    return scanned;
}
#endif

}

// A code address relative to its module, for symbolizing debug traces with nm.
unsigned long long ModuleOffset(const void* address) {
#ifdef _WIN32
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module);
    return reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module);
#else
    Dl_info info{};
    if (dladdr(address, &info) == 0 || info.dli_fbase == nullptr) return reinterpret_cast<std::uintptr_t>(address);
    return reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(info.dli_fbase);
#endif
}

#ifdef _WIN32
bool readableProtection(DWORD protection) {
    return protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool writableProtection(DWORD protection) {
    return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

std::uintptr_t userAddressLimit() {
    static const auto limit = [] {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        return reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
    }();
    return limit;
}
#endif

}

namespace {
std::atomic<std::uint64_t> forgetCalls{0};
// See ForgetSerial: odd while a ForgetPages call stores its page states.
std::atomic<std::uint64_t> forgetSerial{0};
constexpr std::size_t ForgetSlotCount = 65536;
constexpr std::uint64_t ForgetBlockBytes = 65536;
std::array<std::atomic<std::uint64_t>, ForgetSlotCount> forgetSlots{};
std::atomic<std::uint64_t> wideForget{0};

void raiseForgetSlot(std::atomic<std::uint64_t>& slot, std::uint64_t serial) {
    auto current = slot.load(std::memory_order_relaxed);
    while (current < serial && !slot.compare_exchange_weak(current, serial, std::memory_order_release, std::memory_order_relaxed)) {}
}
std::atomic<void (*)(std::uint64_t, std::uint64_t, TrackerEvent)> stampObserver{nullptr};
std::array<std::atomic<void (*)()>, 4> armObservers{};
std::atomic<std::uint64_t> forgetBytes{0};
std::atomic<std::uint64_t> collectMemoHits{0};
std::atomic<std::uint64_t> failedCollectMemoHits{0};
std::atomic<std::uint64_t> collectEpochBumps{0};
std::atomic<std::uint64_t> unwatchSerial{0};
// Walks that reported at least one written page (the ones the probe pass used to double), and the
// tracker mutex acquisitions that had to wait (APS5_PROFILE_DRAW; see lockTracker).
std::atomic<std::uint64_t> collectDirty{0};
#ifndef _WIN32
std::atomic<std::uint64_t> collectDirtyRuns{0};
#endif
std::atomic<std::uint64_t> trackerWaits{0};
std::atomic<std::uint64_t> trackerAcquisitions{0};
// Stage 4.2: the committer's tracker-mutex acquisitions that waited and the time waited (resolver
// walks hold the same mutex).
struct TrackerTimedWaitsSlot {};
struct TrackerTimedWaitNsSlot {};
std::atomic<std::uint64_t>* trackerTimedWaits() { return HostThreadSlot<std::atomic<std::uint64_t>*, TrackerTimedWaitsSlot>::Get(); }
void trackerTimedWaits(std::atomic<std::uint64_t>* value) { HostThreadSlot<std::atomic<std::uint64_t>*, TrackerTimedWaitsSlot>::Set(value); }
std::atomic<std::uint64_t>* trackerTimedWaitNs() { return HostThreadSlot<std::atomic<std::uint64_t>*, TrackerTimedWaitNsSlot>::Get(); }
void trackerTimedWaitNs(std::atomic<std::uint64_t>* value) { HostThreadSlot<std::atomic<std::uint64_t>*, TrackerTimedWaitNsSlot>::Set(value); }
std::atomic<std::uint64_t> committerTrackerWaits{0};
std::atomic<std::uint64_t> committerTrackerWaitNs{0};
std::atomic<std::uint64_t> resolverTrackerWaits{0};
std::atomic<std::uint64_t> resolverTrackerWaitNs{0};
std::uintptr_t PagesBase();
std::size_t PagesSize();
std::uintptr_t ImagePagesBase();
std::size_t ImagePagesSize();
}

// Profile (APS5_PROFILE_DRAW): calls, bytes and time of the guest memory copies and comparisons the
// driver makes, printed every 10 s as [guestmem]. Accumulated per thread (the calling thread is the
// only writer: plain loads and stores, no locked RMW on a cache line every queue worker shares) and
// folded at report time; a finished thread's totals move to `retired`. Nothing is counted without
// the profile switch (the counters are only ever printed under it).
// Debug aid: APS5_SHARED_MEMORY_COUNTERS=1 accumulates into one shared set with atomic RMWs as before.
enum MemoryCounterKind { CounterRead, CounterCompare, CounterWrite, CounterChangedWrite, CounterCollect, CounterVerify, CounterQuery, MemoryCounterCount };
constexpr const char* MemoryCounterNames[MemoryCounterCount] = {"read", "compare", "write", "write-changed", "collect", "verify", "query"};

struct MemoryCounter {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> nanoseconds{0};

    void Add(std::uint64_t moreBytes, std::uint64_t moreNanoseconds, bool shared) {
        if (shared) {
            calls.fetch_add(1, std::memory_order_relaxed);
            bytes.fetch_add(moreBytes, std::memory_order_relaxed);
            nanoseconds.fetch_add(moreNanoseconds, std::memory_order_relaxed);
            return;
        }
        calls.store(calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        bytes.store(bytes.load(std::memory_order_relaxed) + moreBytes, std::memory_order_relaxed);
        nanoseconds.store(nanoseconds.load(std::memory_order_relaxed) + moreNanoseconds, std::memory_order_relaxed);
    }
};

struct MemoryCounters {
    std::array<MemoryCounter, MemoryCounterCount> counters;

    void FoldInto(std::array<std::array<std::uint64_t, 3>, MemoryCounterCount>& totals) const {
        for (std::size_t i = 0; i < MemoryCounterCount; ++i) {
            totals[i][0] += counters[i].calls.load(std::memory_order_relaxed);
            totals[i][1] += counters[i].bytes.load(std::memory_order_relaxed);
            totals[i][2] += counters[i].nanoseconds.load(std::memory_order_relaxed);
        }
    }
    // Under the profile's threadsMutex (the only writer of `retired`).
    void AddFrom(const MemoryCounters& other) {
        for (std::size_t i = 0; i < MemoryCounterCount; ++i) {
            counters[i].calls.store(counters[i].calls.load(std::memory_order_relaxed) + other.counters[i].calls.load(std::memory_order_relaxed), std::memory_order_relaxed);
            counters[i].bytes.store(counters[i].bytes.load(std::memory_order_relaxed) + other.counters[i].bytes.load(std::memory_order_relaxed), std::memory_order_relaxed);
            counters[i].nanoseconds.store(counters[i].nanoseconds.load(std::memory_order_relaxed) + other.counters[i].nanoseconds.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
    }
};

struct MemoryProfile {
    AgcDriver::Mutex threadsMutex;
    std::vector<const MemoryCounters*> threads;
    MemoryCounters retired;
    MemoryCounters shared;
    std::atomic<std::int64_t> lastReport{0};
    // Sampled callers of Read and Write (module offsets), to name what touches guest memory most.
    AgcDriver::Mutex callersMutex;
    std::array<std::pair<unsigned long long, std::uint64_t>, 16> callers{};
    std::array<std::pair<unsigned long long, std::uint64_t>, 16> writeCallers{};
};

// Never destroyed: worker threads' counters unregister at thread exit, which can follow static
// destruction at process exit.
MemoryProfile& Profile() {
    static MemoryProfile& profile = *new MemoryProfile();
    return profile;
}

bool SharedMemoryCounters() {
    static const bool shared = std::getenv("APS5_SHARED_MEMORY_COUNTERS") != nullptr;
    return shared;
}

struct ThreadMemoryCounters {
    MemoryCounters counters;

    ThreadMemoryCounters() {
        auto& profile = Profile();
        std::lock_guard lock(profile.threadsMutex);
        profile.threads.push_back(&counters);
    }
    ~ThreadMemoryCounters() {
        auto& profile = Profile();
        std::lock_guard lock(profile.threadsMutex);
        std::erase(profile.threads, &counters);
        profile.retired.AddFrom(counters);
    }
};

MemoryCounter& CounterFor(MemoryCounterKind kind) {
    if (SharedMemoryCounters()) return Profile().shared.counters[kind];
    struct Slot {};
    return ThreadScratch<ThreadMemoryCounters, Slot>().counters.counters[kind];
}

// Every thread's counters, the retired and the shared ones summed: calls, bytes, nanoseconds per kind.
std::array<std::array<std::uint64_t, 3>, MemoryCounterCount> FoldMemoryCounters() {
    auto& profile = Profile();
    std::array<std::array<std::uint64_t, 3>, MemoryCounterCount> totals{};
    std::lock_guard lock(profile.threadsMutex);
    for (const auto* thread : profile.threads) thread->FoldInto(totals);
    profile.retired.FoldInto(totals);
    profile.shared.FoldInto(totals);
    return totals;
}

void CountCaller(std::array<std::pair<unsigned long long, std::uint64_t>, 16>& callers, std::uint32_t& sampled, std::uint32_t every, const void* returnAddress) {
    if (++sampled % every != 0) return;
    auto& state = Profile();
    const auto offset = ModuleOffset(returnAddress);
    std::lock_guard lock(state.callersMutex);
    for (auto& [caller, count] : callers) {
        if (caller == offset || count == 0) {
            caller = offset;
            count += every;
            return;
        }
    }
}

struct CurrentPacketSlot {};
struct CurrentReadSiteSlot {};
constexpr std::uint64_t NoPacketBits = NoPacket | (std::uint64_t{0xffffffffu} << 32u);
// Sampled reads by site (every 256th Read/ReadCommitted, like the callers), for the [guestmem] line.
std::atomic<std::uint64_t> readSiteSamples[static_cast<std::size_t>(ReadSite::Count)] = {};

void SetCurrentPacket(std::uint32_t opcode, std::uint32_t queue) {
    HostThreadSlot<std::uint64_t, CurrentPacketSlot>::Set((opcode | (std::uint64_t{queue} << 32u)) ^ NoPacketBits);
}

PacketTag CurrentPacket() {
    const auto bits = HostThreadSlot<std::uint64_t, CurrentPacketSlot>::Get() ^ NoPacketBits;
    return {static_cast<std::uint32_t>(bits), static_cast<std::uint32_t>(bits >> 32u)};
}

const char* ReadSiteName(ReadSite site) {
    static const char* const names[static_cast<std::size_t>(ReadSite::Count)] = {"unknown", "capture", "dispatch-cache", "texture-compare", "texture-read", "buffer-upload", "index-buffer", "vertex-buffer", "registers", "indirect-args", "wait", "label", "scanout", "store", "mirror-refresh", "draw-cache", "fill-input", "dispatch-handler", "dispatch-prepare", "dispatch-device", "submission", "packet", "packet-prepare", "packet-flush"};
    const auto index = static_cast<std::size_t>(site);
    return index < static_cast<std::size_t>(ReadSite::Count) ? names[index] : "?";
}

ReadSite SetReadSite(ReadSite site) {
    const auto previous = HostThreadSlot<ReadSite, CurrentReadSiteSlot>::Get();
    HostThreadSlot<ReadSite, CurrentReadSiteSlot>::Set(site);
    return previous;
}

ReadSite CurrentReadSite() {
    return HostThreadSlot<ReadSite, CurrentReadSiteSlot>::Get();
}

std::size_t CaptureCallerOffsets(std::span<unsigned long long> frames, unsigned skip) {
    if (frames.empty()) return 0;
#ifdef _WIN32
    // Walks the x64 unwind tables, so it needs no frame pointers (unlike __builtin_return_address(n)
    // for n > 0); frame 0 of the capture is this function's caller. Frames outside the driver (the
    // guest memory layer, exception dispatch) are skipped, so the driver code that made the access
    // fills the slots even when it is several frames up.
    void* raw[32];
    const auto captured = RtlCaptureStackBackTrace(1 + skip, static_cast<DWORD>(std::size(raw)), raw, nullptr);
    // The driver's module is resolved once (this function lives in it): a per-frame
    // GetModuleHandleEx would take the loader lock several times per sync under the GpuMutex.
    static const auto module = [] {
        HMODULE handle = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&CaptureCallerOffsets), &handle);
        std::uintptr_t size = 0;
        if (handle != nullptr) {
            // The image size comes from the mapped PE headers (no psapi dependency).
            const auto* base = reinterpret_cast<const std::byte*>(handle);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            size = nt->OptionalHeader.SizeOfImage;
        }
        return std::pair(reinterpret_cast<std::uintptr_t>(handle), size);
    }();
    std::size_t kept = 0;
    for (std::size_t i = 0; i < captured && kept < frames.size(); ++i) {
        const auto address = reinterpret_cast<std::uintptr_t>(raw[i]);
        if (module.first != 0 && address >= module.first && address - module.first < module.second) frames[kept++] = address - module.first;
    }
    return kept;
#else
    static_cast<void>(skip);
    frames[0] = ModuleOffset(__builtin_return_address(0));
    return 1;
#endif
}

void CountReadCaller(const void* returnAddress) {
    struct SampledThreadStorage {};
    auto& sampled = AgcDriver::ThreadScratch<std::uint32_t, SampledThreadStorage>();
    // Sampled at the same rate as the callers, in step with them (CountCaller advances the counter).
    if ((sampled + 1) % 256 == 0) readSiteSamples[static_cast<std::size_t>(CurrentReadSite())].fetch_add(256, std::memory_order_relaxed);
    CountCaller(Profile().callers, sampled, 256, returnAddress);
}

void CountWriteCaller(const void* returnAddress) {
    struct SampledThreadStorage {};
    auto& sampled = AgcDriver::ThreadScratch<std::uint32_t, SampledThreadStorage>();
    CountCaller(Profile().writeCallers, sampled, 16, returnAddress);
}

bool MemoryProfiled() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

class TimedAccess {
public:
    TimedAccess(MemoryCounterKind kind, std::uint64_t bytes) : kind(kind), bytes(bytes), start(MemoryProfiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~TimedAccess() {
        if (!MemoryProfiled()) return;
        const auto now = std::chrono::steady_clock::now();
        CounterFor(kind).Add(bytes, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count()), SharedMemoryCounters());
        auto& state = Profile();
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        auto last = state.lastReport.load();
        if (nowMs - last < 10000 || !state.lastReport.compare_exchange_strong(last, nowMs)) return;
        const auto totals = FoldMemoryCounters();
        AgcDriver::ProfilePrint_nid_no_patch("[guestmem]");
        for (std::size_t i = 0; i < MemoryCounterCount; ++i) {
            AgcDriver::ProfilePrint_nid_no_patch(" %s %llu calls %.0f MiB %.1f s", MemoryCounterNames[i], static_cast<unsigned long long>(totals[i][0]), totals[i][1] / 1048576.0, totals[i][2] / 1e9);
        }
        AgcDriver::ProfilePrint_nid_no_patch(" collect-memo hits %llu, collect epochs %llu", static_cast<unsigned long long>(collectMemoHits.load()), static_cast<unsigned long long>(collectEpochBumps.load()));
        AgcDriver::ProfilePrint_nid_no_patch(" failed-collect memo hits %llu", static_cast<unsigned long long>(failedCollectMemoHits.load()));
        AgcDriver::ProfilePrint_nid_no_patch(" collect-dirty %llu tracker waits %llu / %llu", static_cast<unsigned long long>(collectDirty.load()), static_cast<unsigned long long>(trackerWaits.load()), static_cast<unsigned long long>(trackerAcquisitions.load()));
        AgcDriver::ProfilePrint_nid_no_patch(" | arena 0x%llx+0x%llx image 0x%llx+0x%llx forgets %llu (%.0f MiB)", static_cast<unsigned long long>(PagesBase()), static_cast<unsigned long long>(PagesSize()), static_cast<unsigned long long>(ImagePagesBase()), static_cast<unsigned long long>(ImagePagesSize()), static_cast<unsigned long long>(forgetCalls.load()), forgetBytes.load() / 1048576.0);
        {
            std::lock_guard lock(state.callersMutex);
            AgcDriver::ProfilePrint_nid_no_patch(" | read callers:");
            for (const auto& [caller, count] : state.callers) {
                if (count != 0) AgcDriver::ProfilePrint_nid_no_patch(" +0x%llx=%llu", caller, static_cast<unsigned long long>(count));
            }
            AgcDriver::ProfilePrint_nid_no_patch(" | write callers:");
            for (const auto& [caller, count] : state.writeCallers) {
                if (count != 0) AgcDriver::ProfilePrint_nid_no_patch(" +0x%llx=%llu", caller, static_cast<unsigned long long>(count));
            }
        }
        AgcDriver::ProfilePrint_nid_no_patch(" | read sites:");
        for (std::size_t site = 0; site < static_cast<std::size_t>(ReadSite::Count); ++site) {
            const auto count = readSiteSamples[site].load(std::memory_order_relaxed);
            if (count != 0) AgcDriver::ProfilePrint_nid_no_patch(" %s=%llu", ReadSiteName(static_cast<ReadSite>(site)), static_cast<unsigned long long>(count));
        }
        AgcDriver::ProfilePrint_nid_no_patch("\n");
    }
private:
    MemoryCounterKind kind;
    std::uint64_t bytes;
    std::chrono::steady_clock::time_point start;
};

namespace {

// Page-state cache. VirtualQuery finds the extent of a run of equally mapped pages by walking page
// tables, which costs tens of milliseconds inside the game's committed gigabyte heap, and GPU workers
// verify ranges thousands of times per second. Committed pages of the guest arena are therefore
// remembered, one byte of access flags per 4 KiB page (the table is committed lazily, so only the
// pages ever verified cost memory). libc reports every range whose mapping or protection changed
// (registry mutations and guest heap decommits) through the invalidator, which forgets those pages.
// A second table covers the main guest image (the PE at 0x7ff6b...): its protections change only
// through registry mutations (mprotect replaces the registered range and invalidates through the same
// callback), and address-based builds verify every image section on each build, so without the table
// each of those verifications was a VirtualQuery. APS5_NO_IMAGE_PAGE_CACHE=1 leaves the image uncached.
constexpr std::size_t PageBytes = 4096;
constexpr std::size_t BlockPages = 64;
constexpr std::uint8_t PageReadable = 1;
constexpr std::uint8_t PageWritable = 2;
constexpr std::uint8_t PageInaccessible = 4;

struct PageSpan {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    // Per page: access flags, zero while unknown.
    std::uint8_t* pages = nullptr;
    std::uint8_t* blocks = nullptr;
    static constexpr std::size_t ChunkEntries = 65536;
    struct alignas(8) Chunk {
        std::array<std::uint8_t, ChunkEntries> entries{};
        std::array<std::uint8_t, ChunkEntries / BlockPages> blocks{};
    };
    std::atomic<Chunk*>* chunks = nullptr;

    Chunk* chunkAt(std::size_t index, bool create) const {
        auto* chunk = chunks[index].load(std::memory_order_acquire);
        if (chunk == nullptr && create) {
            auto fresh = std::unique_ptr<Chunk>(new (std::nothrow) Chunk{});
            if (fresh != nullptr && chunks[index].compare_exchange_strong(chunk, fresh.get(), std::memory_order_acq_rel, std::memory_order_acquire)) chunk = fresh.release();
        }
        return chunk;
    }

    std::uint8_t* entry(std::size_t index, bool create) const {
        if (chunks == nullptr) return pages + index;
        auto* chunk = chunkAt(index / ChunkEntries, create);
        return chunk == nullptr ? nullptr : chunk->entries.data() + index % ChunkEntries;
    }

    std::uint8_t* blockEntry(std::size_t block, bool create) const {
        if (chunks == nullptr) return blocks + block;
        auto* chunk = chunkAt(block * BlockPages / ChunkEntries, create);
        return chunk == nullptr ? nullptr : chunk->blocks.data() + block % (ChunkEntries / BlockPages);
    }

    bool allocate() {
        if (size == 0) return false;
        const auto count = (size / PageBytes + BlockPages) & ~(BlockPages - 1);
#ifdef _WIN32
        const auto chunkCount = (count + ChunkEntries - 1) / ChunkEntries;
        chunks = new (std::nothrow) std::atomic<Chunk*>[chunkCount]{};
        if (chunks == nullptr) size = 0;
        return chunks != nullptr;
#else
        const auto blockCount = (count / BlockPages + 8) & ~static_cast<std::size_t>(7);
        pages = static_cast<std::uint8_t*>(std::calloc(count + blockCount, 1));
        if (pages == nullptr) size = 0;
        else blocks = pages + count;
        return pages != nullptr;
#endif
    }

    bool covers(std::uintptr_t address) const {
        return size != 0 && address >= base && address - base < size;
    }

    static std::uint8_t loadByte(const std::uint8_t* at) {
        return std::atomic_ref<std::uint8_t>(*const_cast<std::uint8_t*>(at)).load(std::memory_order_relaxed);
    }

    static void storeByte(std::uint8_t* at, std::uint8_t value) {
        std::atomic_ref<std::uint8_t>(*at).store(value, std::memory_order_relaxed);
    }

    static std::uint64_t loadWide(const std::uint8_t* at) {
        return std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(const_cast<std::uint8_t*>(at))).load(std::memory_order_relaxed);
    }

    std::uint8_t load(std::uintptr_t address) const {
        auto* value = entry((address - base) / PageBytes, false);
        return value == nullptr ? 0 : loadByte(value);
    }

    std::uint8_t common(std::size_t block) const {
        const auto* at = entry(block * BlockPages, false);
        if (at == nullptr) return 0;
        const auto value = loadByte(at);
        const std::uint64_t wide = 0x0101010101010101ull * value;
        for (std::size_t offset = 0; offset < BlockPages; offset += 8) {
            if (loadWide(at + offset) != wide) return 0;
        }
        return value;
    }

    void settle(std::size_t first, std::size_t last) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        for (auto block = first / BlockPages; block <= (last - 1) / BlockPages; ++block) {
            const auto value = common(block);
            auto* summary = blockEntry(block, false);
            if (summary == nullptr || loadByte(summary) == value) continue;
            storeByte(summary, value);
            if (value == 0) continue;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (common(block) != value) storeByte(summary, 0);
        }
    }

    void store(std::uintptr_t address, std::uint8_t value) {
        store(address, address + PageBytes, value);
    }

    void store(std::uintptr_t first, std::uintptr_t last, std::uint8_t value) {
        if (first >= last) return;
        const auto begin = (first - base) / PageBytes;
        const auto end = (last - 1 - base) / PageBytes + 1;
        auto index = begin;
        while (index < end) {
            const auto next = std::min(end, (index / ChunkEntries + 1) * ChunkEntries);
            if (auto* at = entry(index, value != 0 && value != PageInaccessible)) {
                for (; index < next; ++index, ++at) storeByte(at, value);
            } else index = next;
        }
        settle(begin, end);
    }

    // End of the run of pages equal to `value` that starts at `address`, at most `limit`.
    std::uintptr_t runEnd(std::uintptr_t address, std::uint8_t value, std::uintptr_t limit) const {
        if (!bookkeeping::active()) return runEndScan(address, value, limit);
        const auto wide = runEndWide(address, value, limit);
        bookkeeping::count(bookkeeping::Runs);
        if (!bookkeeping::verify()) return wide;
        const auto scanned = runEndScan(address, value, limit);
        bookkeeping::count(bookkeeping::Verified);
        if (wide != scanned) bookkeeping::mismatch("fix_wt_bookkeeping_run_mismatches");
        return scanned;
    }

    // runEnd with the chunk looked up once per chunk and eight pages compared per load once aligned
    // (four loads per step), the first differing page found from the differing bytes.
    std::uintptr_t runEndWide(std::uintptr_t address, std::uint8_t value, std::uintptr_t limit) const {
        auto index = (address - base) / PageBytes;
        const auto stop = std::min<std::uintptr_t>((std::min(limit, base + size) - base + PageBytes - 1) / PageBytes, size / PageBytes);
        const std::uint64_t wide = 0x0101010101010101ull * value;
        const auto finish = [&](std::size_t at) { return std::min(limit, base + at * PageBytes); };
        while (index < stop) {
            std::uint8_t* data = pages;
            std::size_t first = 0;
            std::size_t chunkEnd = stop;
            if (chunks != nullptr) {
                const auto chunk = index / ChunkEntries;
                first = chunk * ChunkEntries;
                chunkEnd = std::min<std::size_t>(stop, first + ChunkEntries);
                if (auto* found = chunkAt(chunk, false)) data = found->entries.data();
                else data = nullptr;
                if (data == nullptr) {
                    if (value != 0) break;
                    index = chunkEnd;
                    continue;
                }
            }
            // Entries of this chunk by page index (`first` is the chunk's first page).
            const auto byteAt = [&](std::size_t at) { return std::atomic_ref<std::uint8_t>(data[at - first]).load(std::memory_order_relaxed); };
            const auto wordAt = [&](std::size_t at) { return std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(data + (at - first))).load(std::memory_order_relaxed); };
            for (; index < chunkEnd && index % 8 != 0; ++index) {
                if (byteAt(index) != value) return finish(index);
            }
            for (; index + 32 <= chunkEnd; index += 32) {
                const auto a = wordAt(index) ^ wide;
                const auto b = wordAt(index + 8) ^ wide;
                const auto c = wordAt(index + 16) ^ wide;
                const auto d = wordAt(index + 24) ^ wide;
                if ((a | b | c | d) == 0) continue;
                const auto differing = a != 0 ? a : b != 0 ? b : c != 0 ? c : d;
                const std::size_t word = a != 0 ? 0 : b != 0 ? 8 : c != 0 ? 16 : 24;
                return finish(index + word + static_cast<std::size_t>(std::countr_zero(differing)) / 8);
            }
            for (; index + 8 <= chunkEnd; index += 8) {
                if (const auto differing = wordAt(index) ^ wide; differing != 0) return finish(index + static_cast<std::size_t>(std::countr_zero(differing)) / 8);
            }
            for (; index < chunkEnd; ++index) {
                if (byteAt(index) != value) return finish(index);
            }
        }
        return finish(index);
    }

    std::uintptr_t runEndScan(std::uintptr_t address, std::uint8_t value, std::uintptr_t limit) const {
        auto index = (address - base) / PageBytes;
        const auto stop = std::min<std::uintptr_t>((std::min(limit, base + size) - base + PageBytes - 1) / PageBytes, size / PageBytes);
        const std::uint64_t wide = 0x0101010101010101ull * value;
        while (index < stop) {
            auto* at = entry(index, false);
            if (at == nullptr) {
                if (value != 0) break;
                index = std::min(stop, (index / ChunkEntries + 1) * ChunkEntries);
                continue;
            }
            if (index % BlockPages == 0) {
                const auto block = index / BlockPages;
                const auto* summary = blockEntry(block, false);
                if (block % 8 == 0 && index + 8 * BlockPages <= stop && loadWide(summary) == wide) {
                    index += 8 * BlockPages;
                    continue;
                }
                if (index + BlockPages <= stop && loadByte(summary) == value) {
                    index += BlockPages;
                    continue;
                }
            }
            if (index % 8 == 0 && index + 8 <= stop && loadWide(at) == wide) {
                index += 8;
                continue;
            }
            if (loadByte(at) != value) break;
            ++index;
        }
        return std::min(limit, base + index * PageBytes);
    }

    void forget(std::uintptr_t address, std::size_t bytes) {
        if (size == 0 || bytes == 0 || address >= base + size || address + bytes <= base) return;
        const auto first = (std::max(address, base) - base) / PageBytes;
        const auto last = (std::min(address + bytes, base + size) - 1 - base) / PageBytes;
        store(base + first * PageBytes, base + (last + 1) * PageBytes, 0);
    }
};

struct PageStates {
    std::once_flag once;
    std::atomic<bool> ready{false};
    PageSpan arena;
    PageSpan image;

    void initialize();

    PageSpan* spanOf(std::uintptr_t address) {
        if (arena.covers(address)) return &arena;
        if (image.covers(address)) return &image;
        return nullptr;
    }

    void forget(std::uintptr_t address, std::size_t bytes) {
        arena.forget(address, bytes);
        image.forget(address, bytes);
    }

    std::atomic<std::uint64_t> accessSerial{0};

    void record(std::uintptr_t base, std::uintptr_t regionEnd, bool readable, bool writable, std::uint64_t generation, std::uint64_t access) {
        for (PageSpan* span : {&arena, &image}) {
            if (span->size == 0 || regionEnd <= span->base || base >= span->base + span->size) continue;
            const std::uint8_t value = readable ? static_cast<std::uint8_t>(PageReadable | (writable ? PageWritable : 0u)) : PageInaccessible;
            const auto first = std::max(base, span->base);
            const auto last = std::min(regionEnd, span->base + span->size);
            span->store(first, last, value);
            if (GuestAllocations::GuestAllocationsGeneration_nid_postfix() != generation || accessSerial.load(std::memory_order_acquire) != access) span->forget(first, static_cast<std::size_t>(last - first));
        }
    }

    void forgetInaccessible(std::uintptr_t address, std::size_t bytes) {
        for (PageSpan* span : {&arena, &image}) {
            if (span->size == 0 || bytes == 0 || address >= span->base + span->size || address + bytes <= span->base) continue;
            const auto last = std::min(address + bytes, span->base + span->size);
            for (auto at = std::max(address, span->base) & ~static_cast<std::uintptr_t>(PageBytes - 1); at < last; at += PageBytes) {
                if (span->load(at) == PageInaccessible) span->store(at, at + PageBytes, 0);
            }
        }
    }
};

PageStates& Pages() {
    static PageStates pages;
    return pages;
}

std::uintptr_t PagesBase() {
    return Pages().arena.base;
}

std::size_t PagesSize() {
    return Pages().arena.size;
}

std::uintptr_t ImagePagesBase() {
    return Pages().image.base;
}

std::size_t ImagePagesSize() {
    return Pages().image.size;
}

void ForgetPages(std::uintptr_t address, std::size_t bytes) {
    forgetCalls.fetch_add(1, std::memory_order_relaxed);
    forgetBytes.fetch_add(bytes, std::memory_order_relaxed);
    const auto started = forgetSerial.fetch_add(1, std::memory_order_release) + 1;
    Pages().forget(address, bytes);
    const auto done = started + 1;
    if (bytes != 0) {
        const auto first = address / ForgetBlockBytes;
        const auto last = (address + bytes - 1) / ForgetBlockBytes;
        if (last - first >= ForgetSlotCount) raiseForgetSlot(wideForget, done);
        else for (auto block = first; block <= last; ++block) raiseForgetSlot(forgetSlots[block % ForgetSlotCount], done);
    }
    forgetSerial.fetch_add(1, std::memory_order_release);
    if (const auto observer = stampObserver.load(std::memory_order_acquire); observer != nullptr && bytes != 0) observer(address, address + bytes, TrackerEvent::Forget);
    // Write generations: a mapping change (meta + data) published before libc returns; the blocks'
    // sweeps no longer cover them. Leaves never touched stay missing (a later leaf is born after).
    if (bytes != 0 && Gen::Active()) {
        static_cast<void>(Gen::Note(Gen::Event::Forget, address, address + bytes, Gen::Meta | Gen::Data, 0, false));
        Gen::Global().DropCoverage(address, address + bytes);
    }
}

void ForgetInaccessiblePages(std::uintptr_t address, std::size_t bytes) {
    Pages().accessSerial.fetch_add(1, std::memory_order_acq_rel);
    Pages().forgetInaccessible(address, bytes);
    if (bytes != 0 && Gen::Active()) {
        static_cast<void>(Gen::Note(Gen::Event::Inaccessible, address, address + bytes, Gen::Meta, 0, false));
        Gen::Global().DropCoverage(address, address + bytes);
    }
}

#ifdef __linux__
int mainImageExtent(dl_phdr_info* info, std::size_t, void* data) {
    constexpr auto mask = static_cast<std::uintptr_t>(PageBytes - 1);
    auto& span = *static_cast<PageSpan*>(data);
    std::uintptr_t first = std::numeric_limits<std::uintptr_t>::max();
    std::uintptr_t last = 0;
    for (int index = 0; index < info->dlpi_phnum; ++index) {
        const auto& header = info->dlpi_phdr[index];
        if (header.p_type != PT_LOAD || header.p_memsz == 0) continue;
        first = std::min(first, static_cast<std::uintptr_t>(info->dlpi_addr + header.p_vaddr) & ~mask);
        last = std::max(last, static_cast<std::uintptr_t>(info->dlpi_addr + header.p_vaddr + header.p_memsz + mask) & ~mask);
    }
    if (first < last) {
        span.base = first;
        span.size = static_cast<std::size_t>(last - first);
    }
    return 1;
}
#endif

void PageStates::initialize() {
    if (ready.load(std::memory_order_acquire)) return;
    std::call_once(once, [&] {
        GuestArena::GuestArenaRange_nid_postfix(&arena.base, &arena.size);
        const bool arenaCached = arena.allocate();
        bool imageCached = false;
#ifdef _WIN32
        // The image extent is the run of regions sharing the module's allocation base, as the registry
        // walks it when it registers the main image.
        static const bool noImageCache = std::getenv("APS5_NO_IMAGE_PAGE_CACHE") != nullptr;
        if (const auto module = noImageCache ? nullptr : GetModuleHandleW(nullptr)) {
            const auto start = reinterpret_cast<std::uintptr_t>(module);
            auto cursor = start;
            for (;;) {
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory) || memory.AllocationBase != module || memory.RegionSize == 0 || memory.RegionSize > std::numeric_limits<std::uintptr_t>::max() - cursor) break;
                cursor += memory.RegionSize;
            }
            image.base = start;
            image.size = cursor - start;
            imageCached = image.allocate();
        }
#elif defined(__linux__)
        static const bool noImageCache = std::getenv("APS5_NO_IMAGE_PAGE_CACHE") != nullptr;
        if (!noImageCache) {
            dl_iterate_phdr(mainImageExtent, &image);
            imageCached = image.allocate();
        }
#endif
        if (arenaCached || imageCached) {
            GuestAllocations::GuestAllocationsSetInvalidator_nid_postfix(&ForgetPages);
            GuestArena::GuestArenaSetAccessObserver_nid_postfix(&ForgetInaccessiblePages);
        }
    });
    ready.store(true, std::memory_order_release);
}

struct PageRun {
    std::uintptr_t begin;
    std::uintptr_t end;
    bool readable;
    bool writable;
};


// APS5_ARCHB_PAGE_STATE_MEMO: a range whose cached page states form one known run answers the same
// run until a page state is forgotten (ForgetPages, ForgetInaccessiblePages) or the allocation
// registry changes, so the per-page scan is not repeated for it. Per thread, exact ranges.
namespace pageMemo {
struct Entry {
    std::uintptr_t address = 0;
    std::uintptr_t end = 0;
    std::uint64_t forget = 0;
    std::uint64_t access = 0;
    std::uint64_t mapping = 0;
    std::uint8_t value = 0;
};

struct Cache {
    static constexpr std::size_t Entries = 1024;
    std::array<Entry, Entries> entries{};
    std::uint32_t hits = 0;
    std::uint32_t misses = 0;
};

struct Serials {
    std::uint64_t forget;
    std::uint64_t access;
    std::uint64_t mapping;
    bool operator==(const Serials&) const = default;
};

struct Counters {
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
};

Counters& counters() {
    static Counters* const made = [] {
        auto* created = new Counters();
        ArchB::Recorder::RegisterCounter("page_memo_hits", created->hits);
        ArchB::Recorder::RegisterCounter("page_memo_misses", created->misses);
        return created;
    }();
    return *made;
}

bool active() {
    return ArchB::Switches::Enabled(ArchB::Feature::PageStateMemo);
}

Cache& cache() {
    struct Slot {};
    auto* found = HostThreadSlot<Cache*, Slot>::Get();
    if (found == nullptr) {
        ShaderRecompiler::ThreadOwned(found);
        HostThreadSlot<Cache*, Slot>::Set(found);
        static_cast<void>(counters());
    }
    return *found;
}

Entry& slot(Cache& table, std::uintptr_t address, std::uintptr_t end) {
    auto key = (address >> 6) * 0x9e3779b97f4a7c15ull ^ (end >> 6) * 0xc2b2ae3d27d4eb4full;
    key ^= key >> 31;
    return table.entries[static_cast<std::size_t>(key % Cache::Entries)];
}

Serials read(const PageStates& pages) {
    return {forgetSerial.load(std::memory_order_acquire), pages.accessSerial.load(std::memory_order_acquire), GuestAllocations::GuestAllocationsGeneration_nid_postfix()};
}

void countHit(Cache& table) {
    if (++table.hits < 256) return;
    counters().hits.fetch_add(table.hits, std::memory_order_relaxed);
    table.hits = 0;
}

void countMiss(Cache& table) {
    if (++table.misses < 256) return;
    counters().misses.fetch_add(table.misses, std::memory_order_relaxed);
    table.misses = 0;
}
}

// Calls `emit` with consecutive runs of uniform accessibility covering [address, address + bytes) in
// order, stopping early when it returns false. Returns false when the address space cannot be queried.
template <class Emit>
bool describePages(std::uintptr_t address, std::size_t bytes, Emit&& emit) {
    auto& pages = Pages();
    pages.initialize();
    const auto end = address + bytes;
    auto cursor = address;
    pageMemo::Cache* memo = nullptr;
    pageMemo::Serials serials{};
    if (bytes != 0 && pageMemo::active()) {
        memo = &pageMemo::cache();
        serials = pageMemo::read(pages);
        const auto& entry = pageMemo::slot(*memo, address, end);
        if ((serials.forget & 1u) == 0 && entry.value != 0 && entry.address == address && entry.end == end && entry.forget == serials.forget && entry.access == serials.access && entry.mapping == serials.mapping) {
            pageMemo::countHit(*memo);
            static_cast<void>(emit(PageRun{address, end, (entry.value & PageReadable) != 0, (entry.value & PageWritable) != 0}));
            return true;
        }
        pageMemo::countMiss(*memo);
    }
    while (cursor < end) {
        if (const auto* span = pages.spanOf(cursor)) {
            const auto value = span->load(cursor);
            if (value != 0) {
                const auto next = span->runEnd(cursor, value, end);
                if (memo != nullptr && cursor == address && next == end && (serials.forget & 1u) == 0 && pageMemo::read(pages) == serials) {
                    auto& entry = pageMemo::slot(*memo, address, end);
                    entry = {address, end, serials.forget, serials.access, serials.mapping, value};
                }
                if (!emit(PageRun{cursor, next, (value & PageReadable) != 0, (value & PageWritable) != 0})) return true;
                cursor = next;
                continue;
            }
        }
#ifdef _WIN32
        const TimedAccess timed(CounterQuery, 0);
        MEMORY_BASIC_INFORMATION memory{};
        const auto queryStart = std::chrono::steady_clock::now();
        // A mutation that changes these pages while they are being queried bumps the generation
        // before it invalidates, so a generation change across the query and the store below means
        // the stored pages may describe the old state and are forgotten again.
        const auto generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        const auto access = pages.accessSerial.load(std::memory_order_acquire);
        if (cursor > userAddressLimit()) {
            static_cast<void>(emit(PageRun{cursor, end, false, false}));
            return true;
        }
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory)) return false;
        // Debug aid: APS5_TRACE_QUERY prints every 16th page query with its cost.
        static const bool traceQuery = std::getenv("APS5_TRACE_QUERY") != nullptr;
        if (traceQuery) {
            static std::atomic<std::uint32_t> queries{0};
            if (queries.fetch_add(1) % 16 == 0) {
                const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - queryStart).count();
                std::fprintf(stderr, "[query] 0x%llx range 0x%zx: base 0x%llx size 0x%llx state 0x%lx protect 0x%lx type 0x%lx %.0f us gen %llu from +0x%llx\n", static_cast<unsigned long long>(cursor), bytes, reinterpret_cast<unsigned long long>(memory.BaseAddress), static_cast<unsigned long long>(memory.RegionSize), memory.State, memory.Protect, memory.Type, us, static_cast<unsigned long long>(GuestAllocations::GuestAllocationsGeneration_nid_postfix()), ModuleOffset(__builtin_return_address(0)));
            }
        }
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base || base + memory.RegionSize <= cursor) return false;
        const auto regionEnd = base + memory.RegionSize;
        std::uint32_t logicalProtection = memory.Protect;
        GuestArena::GuestArenaProtection_nid_postfix(cursor, &logicalProtection);
        // Pages the write tracker protected (APS5_ARCHB_WT_FAULT) keep the guest's writable protection,
        // so the cached state does not change when they are armed or opened.
        logicalProtection = static_cast<std::uint32_t>(GuestArena::GuestWtFaultGuestProtection_nid_no_patch(base, static_cast<std::size_t>(regionEnd - base), logicalProtection));
        const auto protection = logicalProtection & 0xffu;
        const bool committed = memory.State == MEM_COMMIT && (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
        const bool readable = committed && readableProtection(protection);
        const bool writable = readable && writableProtection(protection);
        if ((memory.Protect & PAGE_GUARD) == 0) pages.record(base, regionEnd, readable, writable, generation, access);
        const auto next = std::min(end, regionEnd);
        if (!emit(PageRun{cursor, next, readable, writable})) return true;
        cursor = next;
#else
        const auto generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        const auto access = pages.accessSerial.load(std::memory_order_acquire);
        const auto recordRun = [&](std::uintptr_t next, bool readable, bool writable) {
            constexpr auto mask = static_cast<std::uintptr_t>(PageBytes - 1);
            pages.record(cursor & ~mask, (next + mask) & ~mask, readable, writable, generation, access);
        };
        if (const int fd = HostMapping::QueryFd(); fd >= 0) {
            procmap_query query{};
            query.size = sizeof(query);
            query.query_flags = PROCMAP_QUERY_COVERING_OR_NEXT_VMA;
            query.query_addr = cursor;
            if (ioctl(fd, PROCMAP_QUERY, &query) == 0) {
                if (query.vma_end <= cursor || query.vma_start >= query.vma_end) return false;
                if (query.vma_start > cursor) {
                    const auto gapEnd = std::min<std::uintptr_t>(end, query.vma_start);
                    recordRun(gapEnd, false, false);
                    if (!emit(PageRun{cursor, gapEnd, false, false})) return true;
                    cursor = gapEnd;
                    continue;
                }
                const bool readable = (query.vma_flags & PROCMAP_QUERY_VMA_READABLE) != 0;
                const bool writable = readable && (query.vma_flags & PROCMAP_QUERY_VMA_WRITABLE) != 0;
                const auto next = std::min<std::uintptr_t>(end, query.vma_end);
                recordRun(next, readable, writable);
                if (!emit(PageRun{cursor, next, readable, writable})) return true;
                cursor = next;
                continue;
            }
            if (errno == ENOENT) {
                static_cast<void>(emit(PageRun{cursor, end, false, false}));
                return true;
            }
            return false;
        }
#ifdef __APPLE__
        {
            mach_vm_address_t region = cursor;
            mach_vm_size_t regionSize = 0;
            vm_region_basic_info_data_64_t info{};
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object = MACH_PORT_NULL;
            const auto status = mach_vm_region(mach_task_self(), &region, &regionSize, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
            if (status == KERN_INVALID_ADDRESS) {
                static_cast<void>(emit(PageRun{cursor, end, false, false}));
                return true;
            }
            if (status != KERN_SUCCESS || regionSize == 0) return false;
            if (region > cursor) {
                const auto gapEnd = std::min<std::uintptr_t>(end, region);
                if (!emit(PageRun{cursor, gapEnd, false, false})) return true;
                cursor = gapEnd;
                continue;
            }
            bool readable = (info.protection & VM_PROT_READ) != 0;
            bool writable = readable && (info.protection & VM_PROT_WRITE) != 0;
            auto next = std::min<std::uintptr_t>(end, region + regionSize);
            std::uintptr_t watchedEnd = next;
            const int logical = GuestWriteWatch::GuestWriteWatchProtection_nid_postfix(cursor, next, &watchedEnd);
            if (logical >= 0) {
                readable = readable && (logical & PROT_READ) != 0;
                writable = readable && (logical & PROT_WRITE) != 0;
            }
            if (watchedEnd > cursor && watchedEnd < next) next = watchedEnd;
            recordRun(next, readable, writable);
            if (!emit(PageRun{cursor, next, readable, writable})) return true;
            cursor = next;
            continue;
        }
#endif
        std::ifstream maps("/proc/self/maps");
        if (!maps.is_open()) return false;
        std::string line;
        bool found = false;
        while (std::getline(maps, line)) {
            std::istringstream fields(line);
            std::uintptr_t first = 0;
            std::uintptr_t last = 0;
            char separator = 0;
            std::string permissions;
            if (!(fields >> std::hex >> first >> separator >> last >> permissions) || separator != '-' || first >= last || permissions.size() < 2) return false;
            if (last <= cursor) continue;
            if (first > cursor) {
                const auto gapEnd = std::min(end, first);
                recordRun(gapEnd, false, false);
                if (!emit(PageRun{cursor, gapEnd, false, false})) return true;
                cursor = gapEnd;
                found = true;
                break;
            }
            const bool readable = permissions[0] == 'r';
            const bool writable = readable && permissions[1] == 'w';
            const auto next = std::min(end, last);
            recordRun(next, readable, writable);
            if (!emit(PageRun{cursor, next, readable, writable})) return true;
            cursor = next;
            found = true;
            break;
        }
        if (!found) {
            static_cast<void>(emit(PageRun{cursor, end, false, false}));
            return true;
        }
#endif
    }
    return true;
}

bool onOwnLiveStack(std::uintptr_t address, std::size_t bytes) {
    struct Bounds {
        std::uintptr_t low = 0;
        std::uintptr_t high = 0;
        Bounds() {
#ifdef _WIN32
            ULONG_PTR lowLimit = 0;
            ULONG_PTR highLimit = 0;
            GetCurrentThreadStackLimits(&lowLimit, &highLimit);
            low = static_cast<std::uintptr_t>(lowLimit);
            high = static_cast<std::uintptr_t>(highLimit);
#elif defined(__APPLE__)
            const auto self = pthread_self();
            high = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(self));
            low = high - pthread_get_stacksize_np(self);
#else
            pthread_attr_t attributes;
            if (pthread_getattr_np(pthread_self(), &attributes) != 0) return;
            void* base = nullptr;
            std::size_t size = 0;
            if (pthread_attr_getstack(&attributes, &base, &size) == 0 && base != nullptr) {
                low = reinterpret_cast<std::uintptr_t>(base);
                high = low + size;
            }
            pthread_attr_destroy(&attributes);
#endif
        }
    };
    struct Slot {};
    const auto& bounds = ThreadScratch<Bounds, Slot>();
    const auto frame = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    return bounds.high != 0 && frame >= bounds.low && frame < bounds.high && address >= frame && address < bounds.high && bytes <= bounds.high - address;
}

#ifdef __linux__
bool pageStateRecorded(std::uintptr_t address, std::size_t bytes) {
    auto& pages = Pages();
    pages.initialize();
    for (auto at = address & ~static_cast<std::uintptr_t>(PageBytes - 1); at < address + bytes; at += PageBytes) {
        if (const auto* span = pages.spanOf(at); span != nullptr && span->load(at) != 0) return true;
    }
    return false;
}

bool readableWithoutQuery(std::uintptr_t address, std::size_t bytes) {
    constexpr std::size_t limit = 4096;
    if (bytes > limit || HostMapping::QueryFd() >= 0 || pageStateRecorded(address, bytes)) return false;
    struct ScratchThreadStorage {};
    auto& scratch = AgcDriver::ThreadScratch<std::array<std::byte, limit>, ScratchThreadStorage>();
    iovec local{scratch.data(), bytes};
    iovec remote{reinterpret_cast<void*>(address), bytes};
    static const pid_t self = getpid();
    return ::process_vm_readv(self, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(bytes);
}
#endif

namespace census {
std::uint64_t pageStart();
void pageEnd(std::uint64_t start, const char* entry, bool clean);
}

// Checks that [address, address + bytes) is mapped with read (and, if asked, write) access. Returns
// an empty string when it is, otherwise why it is not.
std::string verify(std::uintptr_t address, std::size_t bytes, bool writable) {
    const TimedAccess timed(CounterVerify, bytes);
    struct PageCensus {
        std::uint64_t start = census::pageStart();
        bool clean = false;
        ~PageCensus() {
            if (start != 0) census::pageEnd(start, "page_verify", clean);
        }
    } pageCensus;
    if (onOwnLiveStack(address, bytes)) return {};
#ifdef __linux__
    if (!writable && readableWithoutQuery(address, bytes)) return {};
#endif
    std::string reason;
    const bool queried = describePages(address, bytes, [&](const PageRun& run) {
        if (!run.readable) {
            char text[128];
            std::snprintf(text, sizeof(text), "guest memory is not readable at 0x%llx (range 0x%llx+0x%zx)", static_cast<unsigned long long>(run.begin), static_cast<unsigned long long>(address), bytes);
            reason = text;
        } else if (writable && !run.writable) {
            reason = "guest memory has no write permission";
        }
        return reason.empty();
    });
    if (!queried && reason.empty()) reason = "cannot query guest memory";
    pageCensus.clean = reason.empty();
    return reason;
}

}

void CheckRange(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable) {
    require(alignment != 0, "zero guest memory alignment");
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    require(address != 0 && address % alignment == 0, "null or misaligned address");
    require(bytes <= std::numeric_limits<std::uintptr_t>::max() - address, "address range overflow");
    const auto reason = verify(address, bytes, writable);
    if (!reason.empty()) {
        // Debug aid: APS5_TRACE_UNREADABLE names the code that checked an inaccessible range.
        static const bool trace = std::getenv("APS5_TRACE_UNREADABLE") != nullptr;
        if (trace) std::fprintf(stderr, "[unreadable] %s from +0x%llx\n", reason.c_str(), ModuleOffset(__builtin_return_address(0)));
        require(false, reason.c_str());
    }
}

bool Accessible(const void* pointer, std::size_t bytes, bool writable) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return address != 0 && bytes <= std::numeric_limits<std::uintptr_t>::max() - address && verify(address, bytes, writable).empty();
}

std::uint64_t ForgetSerial() {
    return forgetSerial.load(std::memory_order_acquire);
}

std::uint64_t ForgetStamp(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0) return 0;
    const auto first = address / ForgetBlockBytes;
    const auto last = (address + bytes - 1) / ForgetBlockBytes;
    if (last - first >= ForgetSlotCount) return forgetSerial.load(std::memory_order_acquire);
    auto newest = wideForget.load(std::memory_order_acquire);
    for (auto block = first; block <= last; ++block) newest = std::max(newest, forgetSlots[block % ForgetSlotCount].load(std::memory_order_acquire));
    return newest;
}

bool DescribeCommittedInto(std::uint64_t address, std::size_t bytes, bool writable, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) {
    ranges.clear();
    require(bytes <= std::numeric_limits<std::uint64_t>::max() - address, "address range overflow");
    const TimedAccess timed(CounterVerify, bytes);
    const auto censusStart = census::pageStart();
    const bool queried = describePages(static_cast<std::uintptr_t>(address), bytes, [&](const PageRun& run) {
        if (run.readable && (!writable || run.writable)) {
            if (!ranges.empty() && ranges.back().second == run.begin) ranges.back().second = run.end;
            else ranges.emplace_back(run.begin, run.end);
        }
        return true;
    });
    require(queried, "cannot query guest memory");
    const bool whole = bytes == 0 || (ranges.size() == 1 && ranges.front().first == address && ranges.front().second == address + bytes);
    if (censusStart != 0) census::pageEnd(censusStart, "page_describe", whole);
    return whole;
}

Commitment DescribeCommitted(std::uint64_t address, std::size_t bytes, bool writable) {
    Commitment result;
    result.whole = DescribeCommittedInto(address, bytes, writable, result.ranges);
    return result;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> CommittedRanges(std::uint64_t address, std::size_t bytes, bool writable) {
    return DescribeCommitted(address, bytes, writable).ranges;
}

namespace {

constexpr std::size_t WriteBlockBytes = 65536;
constexpr std::size_t WritePageBytes = 4096;
constexpr std::size_t WritePagesPerBlock = WriteBlockBytes / WritePageBytes;
constexpr std::size_t ImportHashPagesPerThread = 1024;
constexpr unsigned ImportHashThreads = 4;

enum class StampKind : std::uint8_t { Cpu, Driver, ImportWindow, Fresh };

std::uint64_t HashPage(const std::byte* page) {
    constexpr std::uint64_t prime1 = 0x9E3779B185EBCA87ull;
    constexpr std::uint64_t prime2 = 0xC2B2AE3D27D4EB4Full;
    constexpr std::uint64_t prime3 = 0x165667B19E3779F9ull;
    constexpr std::uint64_t prime4 = 0x85EBCA77C2B2AE63ull;
    const auto round = [](std::uint64_t accumulator, std::uint64_t word) { return std::rotl(accumulator + word * prime2, 31) * prime1; };
    std::array<std::uint64_t, 4> lanes{prime1 + prime2, prime2, 0, 0 - prime1};
    for (std::size_t offset = 0; offset < WritePageBytes; offset += 4 * sizeof(std::uint64_t)) {
        std::array<std::uint64_t, 4> words;
        std::memcpy(words.data(), page + offset, sizeof(words));
        for (std::size_t lane = 0; lane < 4; ++lane) lanes[lane] = round(lanes[lane], words[lane]);
    }
    std::uint64_t hash = std::rotl(lanes[0], 1) + std::rotl(lanes[1], 7) + std::rotl(lanes[2], 12) + std::rotl(lanes[3], 18);
    for (const auto lane : lanes) hash = (hash ^ round(0, lane)) * prime1 + prime4;
    hash ^= hash >> 33;
    hash *= prime2;
    hash ^= hash >> 29;
    hash *= prime3;
    return hash ^ (hash >> 32);
}

template<typename Range>
void SplitPages(std::size_t pages, const Range& range) {
    static const unsigned hostThreads = std::max(1u, std::thread::hardware_concurrency() / 2);
    const auto workers = std::min<std::size_t>({ImportHashThreads, hostThreads, pages / ImportHashPagesPerThread});
    if (workers < 2) {
        range(std::size_t{0}, pages);
        return;
    }
    std::vector<std::jthread> threads;
    threads.reserve(workers - 1);
    for (std::size_t worker = 1; worker < workers; ++worker) {
        const auto from = pages * worker / workers;
        const auto to = pages * (worker + 1) / workers;
        try {
            threads.emplace_back(range, from, to);
        } catch (const std::system_error&) {
            range(from, to);
        }
    }
    range(std::size_t{0}, pages / workers);
}

std::vector<std::uint64_t> PagesOf(std::span<const std::pair<std::uint64_t, std::uint64_t>> spans) {
    std::vector<std::uint64_t> pages;
    for (const auto& [begin, end] : spans) {
        for (auto page = begin; page < end; page += WritePageBytes) pages.push_back(page);
    }
    return pages;
}

void HashPages(std::uint64_t first, std::span<const std::pair<std::uint64_t, std::uint64_t>> spans, std::vector<std::uint64_t>& hashes) {
    const auto pages = PagesOf(spans);
    SplitPages(pages.size(), [&](std::size_t from, std::size_t to) {
        for (auto index = from; index < to; ++index) hashes[static_cast<std::size_t>((pages[index] - first) / WritePageBytes)] = HashPage(reinterpret_cast<const std::byte*>(pages[index]));
    });
}

struct WindowCopy {
    std::uint64_t first;
    std::span<const std::uint64_t> hashes;
    std::span<const std::byte> image;
    StampKind held = StampKind::ImportWindow;

    bool Holds(std::uintptr_t page) const {
        if (held == StampKind::Driver) return page >= first && page - first <= image.size() && image.size() - (page - first) >= WritePageBytes && CompareMapped(page, image.subspan(page - first, WritePageBytes)) == Compare::Equal;
        if (page < first || (page - first) / WritePageBytes >= hashes.size()) return false;
        return HashPage(reinterpret_cast<const std::byte*>(page)) == hashes[(page - first) / WritePageBytes];
    }
};

#ifdef _WIN32
void watchPrivateMapping(std::uintptr_t address, std::size_t bytes, std::uint64_t generation);
#endif

// Write generations: host writes (file I/O, AIO, network) into guest memory. Meta at the start, so
// a capture made while the host write runs never reads clean; data and cpu once it finished.
void noteHostWriteGen(std::uintptr_t address, std::size_t bytes, bool end) {
    // Stage 4 M3 (APS5_WT_FAULT_CENSUS): pages an HLE host write (ReadFile, AIO, ...) stored, at its end.
    if (end) Stage4::NoteHostWrite(address, bytes);
    else GuestArena::GuestWtFaultServeLazyRange_nid_no_patch(address, bytes);
    if (bytes == 0 || bytes > std::numeric_limits<std::uintptr_t>::max() - address || !Gen::Active()) return;
    if (end) static_cast<void>(Gen::Note(Gen::Event::HostWriteEnd, address, address + bytes, Gen::Data | Gen::Cpu, 0, false));
    else static_cast<void>(Gen::Note(Gen::Event::HostWriteBegin, address, address + bytes, Gen::Meta, 0, false));
}

// APS5_ARCHB_WT_FAULT (Stage 4 option 2; libc GuestWriteFault.hpp): write tracking by page protection.
// Shadow mode: the tracker's walks make their pages candidates, the flip protects the pages written
// since in contiguous runs, a write fault opens a page again, and GetWriteWatch stays the source of
// truth. The tracker's generations come from the fault tracker's clock, so a page's arm stamp compares
// directly with a generation. APS5_ARCHB_WT_FAULT_VERIFY checks every dirty page a resetting walk
// reports against the fault states (wt_fault_mismatches must stay 0). APS5_ARCHB_WT_FAULT_CONSUMERS
// answers the commit-time freshness checks from the fault states (see consumerAnswer).
namespace wtFault {
bool switchOn() {
    static const bool on = ArchB::Switches::Enabled(ArchB::Feature::WtFault);
    return on;
}
bool verifyOn() {
    static const bool on = ArchB::Switches::Enabled(ArchB::Feature::WtFaultVerify);
    return on;
}
// Set once the write tracker turned the fault tracker on (its first initialize with write watching).
std::atomic<bool> active{false};
bool consumersOn() {
    return active.load(std::memory_order_relaxed) && ArchB::Switches::Enabled(ArchB::Feature::WtFaultConsumers);
}
// Driver stores up to this size keep their pages open across flips (the driver stores them again
// every frame; larger ranges are landed GPU results, which never fault).
constexpr std::size_t DriverHotBytes = 256u << 10u;

struct Counters {
    std::atomic<std::uint64_t> faults{0}, driverFaults{0}, handlerNs{0}, driverHandlerNs{0}, retries{0}, foreign{0};
    std::atomic<std::uint64_t> armCalls{0}, armedPages{0}, armRuns{0}, armNs{0}, armProtectNs{0}, excluded{0}, keptHot{0};
    std::atomic<std::uint64_t> holds{0}, heldOpened{0}, forgotten{0}, tracked{0};
    std::atomic<std::uint64_t> verifyWalks{0}, verifyDirty{0}, verifyUntracked{0}, mismatches{0};
    std::atomic<std::uint64_t> consumerUnchanged{0}, consumerChanged{0}, consumerFallback{0}, consumerVerified{0}, consumerMismatches{0};
    std::atomic<std::uint64_t> keptDirty{0}, armDirtyNs{0}, dirtyWalks{0}, dirtyWalkNs{0}, armedWalkNs{0};
    std::atomic<std::uint64_t> logAppends{0}, logLost{0}, coldRearmed{0};
};
bool dirtyArmOn() {
    return active.load(std::memory_order_relaxed) && ArchB::Switches::Enabled(ArchB::Feature::WtFaultDirtyArm);
}
bool& flipDirtyWalkThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<bool, Slot>();
}
Counters& counters() {
    static auto* made = [] {
        auto* c = new Counters();
        const std::pair<const char*, std::atomic<std::uint64_t>*> named[] = {
            {"wt_fault_faults", &c->faults}, {"wt_fault_driver_faults", &c->driverFaults}, {"wt_fault_handler_ns", &c->handlerNs},
            {"wt_fault_driver_handler_ns", &c->driverHandlerNs}, {"wt_fault_retries", &c->retries}, {"wt_fault_foreign", &c->foreign},
            {"wt_fault_arms", &c->armCalls}, {"wt_fault_armed_pages", &c->armedPages}, {"wt_fault_arm_runs", &c->armRuns}, {"wt_fault_arm_ns", &c->armNs},
            {"wt_fault_arm_protect_ns", &c->armProtectNs}, {"wt_fault_excluded", &c->excluded}, {"wt_fault_kept_hot", &c->keptHot},
            {"wt_fault_holds", &c->holds}, {"wt_fault_held_opened", &c->heldOpened}, {"wt_fault_forgotten", &c->forgotten}, {"wt_fault_tracked", &c->tracked},
            {"wt_fault_verify_walks", &c->verifyWalks}, {"wt_fault_verify_dirty", &c->verifyDirty}, {"wt_fault_verify_untracked", &c->verifyUntracked},
            {"wt_fault_mismatches", &c->mismatches}, {"wt_fault_consumer_unchanged", &c->consumerUnchanged}, {"wt_fault_consumer_changed", &c->consumerChanged},
            {"wt_fault_consumer_fallback", &c->consumerFallback}, {"wt_fault_consumer_verified", &c->consumerVerified}, {"wt_fault_consumer_mismatches", &c->consumerMismatches},
            {"wt_fault_kept_dirty", &c->keptDirty}, {"wt_fault_arm_dirty_ns", &c->armDirtyNs}, {"wt_fault_dirty_walks", &c->dirtyWalks}, {"wt_fault_dirty_walk_ns", &c->dirtyWalkNs},
            {"wt_fault_armed_walk_ns", &c->armedWalkNs}, {"wt_fault_log_appends", &c->logAppends}, {"wt_fault_log_lost", &c->logLost}, {"wt_fault_cold_rearmed", &c->coldRearmed}};
        for (const auto& [name, counter] : named) ArchB::Recorder::RegisterCounter(name, *counter);
        return c;
    }();
    return *made;
}

// Copies the fault tracker's cumulative counters into the recorder's (at flips and on demand).
void publish() {
    if (!active.load(std::memory_order_acquire)) return;
    GuestArena::WtFaultCounters raw{};
    GuestArena::GuestWtFaultCounters_nid_no_patch(&raw);
    auto& c = counters();
    c.faults.store(raw.faults, std::memory_order_relaxed);
    c.driverFaults.store(raw.driverFaults, std::memory_order_relaxed);
    c.handlerNs.store(raw.handlerNs, std::memory_order_relaxed);
    c.driverHandlerNs.store(raw.driverHandlerNs, std::memory_order_relaxed);
    c.retries.store(raw.retries, std::memory_order_relaxed);
    c.foreign.store(raw.foreign, std::memory_order_relaxed);
    c.armCalls.store(raw.armCalls, std::memory_order_relaxed);
    c.armedPages.store(raw.armedPages, std::memory_order_relaxed);
    c.armRuns.store(raw.armRuns, std::memory_order_relaxed);
    c.armNs.store(raw.armNs, std::memory_order_relaxed);
    c.armProtectNs.store(raw.armProtectNs, std::memory_order_relaxed);
    c.excluded.store(raw.excluded, std::memory_order_relaxed);
    c.keptHot.store(raw.keptHot, std::memory_order_relaxed);
    c.holds.store(raw.holds, std::memory_order_relaxed);
    c.heldOpened.store(raw.heldOpened, std::memory_order_relaxed);
    c.forgotten.store(raw.forgotten, std::memory_order_relaxed);
    c.tracked.store(raw.tracked, std::memory_order_relaxed);
    c.verifyWalks.store(raw.verifyWalks, std::memory_order_relaxed);
    c.verifyDirty.store(raw.verifyDirty, std::memory_order_relaxed);
    c.verifyUntracked.store(raw.verifyUntracked, std::memory_order_relaxed);
    c.mismatches.store(raw.verifyMismatches, std::memory_order_relaxed);
    c.keptDirty.store(raw.keptDirty, std::memory_order_relaxed);
    c.armDirtyNs.store(raw.armDirtyNs, std::memory_order_relaxed);
    c.logAppends.store(raw.logAppends, std::memory_order_relaxed);
    c.logLost.store(raw.logLost, std::memory_order_relaxed);
    c.coldRearmed.store(raw.coldRearmed, std::memory_order_relaxed);
}

// The dirty pages of one walk, for VERIFY (filled under the tracker mutex).
std::vector<void*>& dirtyScratch() {
    static std::vector<void*> pages;
    return pages;
}

void reportMismatches(std::uint64_t first, std::uint64_t stop, std::uint64_t mismatches, const GuestArena::WtFaultMismatch& mismatch, std::uint64_t walkClock) {
    static std::atomic<std::uint32_t> reported{0};
    if (reported.fetch_add(1, std::memory_order_relaxed) >= 32) return;
    const auto state = mismatch.word & GuestArena::WtFaultStateMask;
    std::fprintf(stderr, "[wt-fault] MISMATCH: walk 0x%llx..0x%llx at %llu reported %llu dirty page(s) the fault tracker reads as unchanged; first 0x%llx state %llu stamp %llu last reset %llu clock %llu\n",
                 static_cast<unsigned long long>(first), static_cast<unsigned long long>(stop), static_cast<unsigned long long>(walkClock), static_cast<unsigned long long>(mismatches),
                 static_cast<unsigned long long>(mismatch.page), static_cast<unsigned long long>(state), static_cast<unsigned long long>(mismatch.word >> GuestArena::WtFaultStampShift),
                 static_cast<unsigned long long>(mismatch.lastReset), static_cast<unsigned long long>(mismatch.clock));
}
}

// APS5_ARCHB_FIX_UNSTAMPED_ALIAS (see lockedUnchangedCommittedSince): the unstamped-store slots never
// decide for a range that is write-watched in full.
bool unstampedAliasActive() {
    return ArchB::Switches::Enabled(ArchB::Feature::FixUnstampedAlias);
}

struct WriteTracker {
    AgcDriver::Mutex mutex;
    bool initialized = false;
    bool watched = false;
    // APS5_ARCHB_WT_FAULT: generations come from the fault tracker's clock (set by initialize()).
    bool faultMode = false;
    static constexpr std::size_t CoarseBlocks = 64;
    static constexpr std::size_t LeafBlocks = std::size_t{1} << 16;
    struct Leaf {
        std::array<std::uint64_t, LeafBlocks> blocks{};
        std::array<std::uint64_t, LeafBlocks> cpuBlocks{};
        std::array<std::uint64_t, LeafBlocks / CoarseBlocks> coarseBlocks{};
#ifdef _WIN32
        std::array<std::uint64_t, LeafBlocks> hintBlocks{};
#endif
    };
    std::vector<std::unique_ptr<Leaf>> leaves;
#ifdef _WIN32
    std::uintptr_t base = 0;
    std::size_t size = 0;
    WriteWatchCoverage coverage;
#else
    static constexpr std::size_t LeafCount = std::size_t{1} << 15;
#endif
    // Atomic only so a per-thread memo hit can read it without the mutex (every mutation and every
    // stamp still happen under it): a hit must return the current value, including MarkWritten
    // bumps, or an image refreshed after a stamp would keep failing UnchangedSince until the next
    // walk of its range and re-upload at every use of the epoch.
    std::atomic<std::uint64_t> generation{1};
    std::atomic<std::uint64_t> stable{1};
    // Set once initialize() found a write-watched arena the generation table can mirror: the
    // GEN_LOCKFREE answers then need nothing initialize() sets up under the mutex.
    std::atomic<bool> ready{false};
    // A new tracker generation (under the mutex). With APS5_ARCHB_GEN_TABLE=1 it comes from the
    // write-generation clock (GenClock), so the stamps and the generations handed to consumers are
    // values of that clock and every table event is ordered against them; only values change, the
    // order of every stamp and compare stays the same (no code does arithmetic on generations).
    std::uint64_t bump() {
        if (faultMode) {
            const auto next = GuestArena::GuestWtFaultClockBump_nid_no_patch();
            generation.store(next, std::memory_order_release);
            return next;
        }
        if (Gen::Active()) {
            // Mapping changes reach the table through the page-state invalidator (ForgetPages),
            // installed with the page-state cache: the driver installs it at its first range check,
            // the table makes sure it is there before its first generation (one acquire load after).
            Pages().initialize();
            const auto next = Gen::Global().BumpAbove(generation.load(std::memory_order_relaxed));
            generation.store(next, std::memory_order_release);
            return next;
        }
        return ++generation;
    }
    std::vector<void*> pages;
    // Collect memo: page-rounded ranges walked during the current epoch (see BumpCollectEpoch). A
    // collect fully inside one skips GetWriteWatch, whose cost is proportional to the range's pages,
    // and the same surfaces are collected several times per epoch (each storage mip binding, a sampled
    // view plus its storage refresh, a write-back followed by a refresh, and every dispatch of the
    // epoch binding the same targets). Entries carry the walking thread's epoch, so no entry matches
    // another thread: the ring consulted first is per thread (ThreadCollectMemo, no mutex, no
    // eviction by other threads' walks); this shared ring (8 workers with epochs of several packets
    // hold a few hundred distinct ranges) serves APS5_SHARED_COLLECT_MEMO=1.
    struct Memo {
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        std::uint64_t epoch = 0;
        std::uint64_t unwatched = 0;
        std::uint64_t mapping = 0;
    };
    std::array<Memo, 256> memo{};
    std::size_t nextMemo = 0;

    void initialize() {
        if (initialized) return;
        initialized = true;
#ifdef _WIN32
        GuestArena::GuestArenaRange_nid_postfix(&base, &size);
        watched = size != 0 && GuestArena::GuestArenaWriteWatched_nid_postfix();
        if (!watched) return;
        leaves.resize((size / WriteBlockBytes + LeafBlocks) / LeafBlocks);
        coverage.Initialize(base, size);
        pages.resize(1u << 16);
        GuestArena::GuestArenaSetPrivateMappingObserver_nid_postfix(&watchPrivateMapping);
        GuestArena::GuestArenaSetHostWriteObserver_nid_no_patch(&noteHostWriteGen);
        if (wtFault::switchOn()) {
            if (Gen::Active()) {
                std::fprintf(stderr, "[wt-fault] off: the write-generation table (GEN_TABLE) owns the tracker's generations\n");
            } else {
                GuestArena::GuestWtFaultEnable_nid_no_patch(wtFault::verifyOn(), generation.load(std::memory_order_relaxed));
                faultMode = true;
                static_cast<void>(wtFault::counters());
                wtFault::active.store(true, std::memory_order_release);
                GuestArena::GuestWtFaultMarkDriverThread_nid_no_patch();
                std::fprintf(stderr, "[wt-fault] on: verify=%d consumers=%d\n", wtFault::verifyOn() ? 1 : 0, ArchB::Switches::Enabled(ArchB::Feature::WtFaultConsumers) ? 1 : 0);
            }
        }
        // Lock-free answers (slice S2) index the generation table by absolute 64 KiB blocks, which
        // are the tracker's blocks only when the arena starts on a block boundary.
        ready.store(base % WriteBlockBytes == 0, std::memory_order_release);
#else
        watched = GuestWriteWatch::GuestWriteWatchAvailable_nid_postfix();
        if (watched) leaves.resize(LeafCount);
        if (watched) GuestArena::GuestArenaSetHostWriteObserver_nid_no_patch(&noteHostWriteGen);
#endif
    }

    bool covers(std::uint64_t address, std::size_t bytes) const {
#ifdef _WIN32
        return coverage.Covers(address, bytes) && !bookkeeping::hostRegionOverlaps(static_cast<std::uintptr_t>(address), bytes);
#else
        return address + bytes <= LeafCount * LeafBlocks * WriteBlockBytes && GuestWriteWatch::GuestWriteWatchCovers_nid_postfix(static_cast<std::uintptr_t>(address), bytes);
#endif
    }

    std::uint64_t blockOf(std::uint64_t address) const {
#ifdef _WIN32
        return (address - base) / WriteBlockBytes;
#else
        return address / WriteBlockBytes;
#endif
    }

    std::uint64_t blockBegin(std::uint64_t block) const {
#ifdef _WIN32
        return base + block * WriteBlockBytes;
#else
        return block * WriteBlockBytes;
#endif
    }

#ifdef _WIN32
    void startHints(std::uint64_t address, std::size_t bytes) {
        const auto first = std::max<std::uint64_t>(address, base);
        const auto stop = std::min<std::uint64_t>(address + bytes, base + size);
        if (first >= stop) return;
        bool started = false;
        for (auto block = blockOf(first); block <= blockOf(stop - 1); ++block) {
            if (!coverage.Covers(blockBegin(block), static_cast<std::size_t>(std::min<std::uint64_t>(WriteBlockBytes, base + size - blockBegin(block))))) continue;
            if (!started) bump();
            started = true;
            stampHint(block);
        }
    }

    bool hinted(std::uint64_t first, std::uint64_t stop) const {
        if (first >= stop || first < base || stop > base + size) return false;
        for (auto block = blockOf(first); block <= blockOf(stop - 1); ++block) {
            if (hintOf(block) == 0 || coverage.Covers(blockBegin(block), static_cast<std::size_t>(std::min<std::uint64_t>(WriteBlockBytes, base + size - blockBegin(block))))) return false;
        }
        return !bookkeeping::hostRegionOverlaps(static_cast<std::uintptr_t>(first), static_cast<std::size_t>(stop - first));
    }

    std::uint64_t newestHint(std::uint64_t first, std::uint64_t stop) const {
        std::uint64_t newest = 0;
        for (auto block = blockOf(first); block <= blockOf(stop - 1); ++block) newest = std::max(newest, hintOf(block));
        return newest;
    }
#endif

    struct DriverPiece {
        std::uint64_t generation = 0;
        std::uint32_t begin = 0;
        std::uint32_t end = 0;
    };
    struct DriverPieces {
        std::uint64_t whole = 0;
        std::uint64_t dropped = 0;
        std::array<DriverPiece, 4> pieces{};
        std::uint64_t possibleWhole = 0;
        std::uint64_t possibleDropped = 0;
        std::array<DriverPiece, 4> possiblePieces{};

        bool covers(std::uint32_t begin, std::uint32_t end, std::uint64_t generation) const {
            if (generation == 0 || begin >= end || dropped > generation) return false;
            if (whole > generation) return true;
            while (begin < end) {
                auto covered = begin;
                for (const auto& piece : pieces) {
                    if (piece.generation > generation && piece.begin <= begin && piece.end > covered) covered = piece.end;
                }
                if (covered == begin) return false;
                begin = covered;
            }
            return true;
        }
    };
    std::unordered_map<std::uint64_t, DriverPieces> driverPieces;

    struct CpuPages {
        std::array<std::uint64_t, WritePagesPerBlock> pages{};
    };
    std::unordered_map<std::uint64_t, CpuPages> cpuPages;
    std::unordered_map<std::uint64_t, CpuPages> walkPages;
    std::unordered_map<std::uint64_t, CpuPages> windowPages;
    std::map<std::uint64_t, std::uint64_t> refused;

    void noteCpuStore(std::uint64_t begin, std::uint64_t end, std::uint64_t stampGeneration) {
        notePages(cpuPages, begin, end, stampGeneration);
    }

    void noteWalkStore(std::uint64_t begin, std::uint64_t end, std::uint64_t stampGeneration) {
        notePages(walkPages, begin, end, stampGeneration);
    }

    void notePages(std::unordered_map<std::uint64_t, CpuPages>& pages, std::uint64_t begin, std::uint64_t end, std::uint64_t stampGeneration) {
        for (auto block = blockOf(begin); block <= blockOf(end - 1); ++block) {
            const auto blockStart = blockBegin(block);
            const auto from = std::max(begin, blockStart) - blockStart;
            const auto to = std::min<std::uint64_t>(end, blockStart + WriteBlockBytes) - blockStart;
            auto& entry = pages[block];
            for (auto page = from / WritePageBytes; page <= (to - 1) / WritePageBytes; ++page) {
                auto& stamp = entry.pages[page];
                stamp = std::max(stamp, stampGeneration);
            }
        }
    }

    bool blockPartWatched(std::uint64_t first, std::uint64_t stop) const {
#ifdef _WIN32
        for (auto page = first; page < stop; page = (page / WritePageBytes + 1) * WritePageBytes) {
            if (covers(page, 1)) return true;
        }
        return false;
#else
        return GuestWriteWatch::GuestWriteWatchOverlaps_nid_postfix(static_cast<std::uintptr_t>(first), static_cast<std::size_t>(stop - first));
#endif
    }

    void stampWatchedPart(std::uint64_t address, std::uint64_t end, std::uint64_t stampGeneration, bool complete) {
#ifdef _WIN32
        address = std::max<std::uint64_t>(address, base);
        end = std::min<std::uint64_t>(end, base + size);
        if (address >= end) return;
#else
        end = std::min<std::uint64_t>(end, LeafCount * LeafBlocks * WriteBlockBytes);
        if (address >= end || !GuestWriteWatch::GuestWriteWatchOverlaps_nid_postfix(static_cast<std::uintptr_t>(address), static_cast<std::size_t>(end - address))) return;
#endif
        for (auto block = blockOf(address); block <= blockOf(end - 1); ++block) {
            const auto first = std::max(address, blockBegin(block));
            const auto stop = std::min<std::uint64_t>(end, blockBegin(block) + WriteBlockBytes);
            if (!blockPartWatched(first, stop)) continue;
            stamp(block, stampGeneration, StampKind::Driver);
            noteDriverStore(block, address, end, stampGeneration, complete);
        }
    }

    void noteDriverStore(std::uint64_t block, std::uint64_t address, std::uint64_t end, std::uint64_t stampGeneration, bool complete) {
        const auto begin = blockBegin(block);
        const auto from = static_cast<std::uint32_t>(std::max(address, begin) - begin);
        const auto to = static_cast<std::uint32_t>(std::min<std::uint64_t>(end, begin + WriteBlockBytes) - begin);
        if (from == 0 && to == WriteBlockBytes) {
            auto& entry = driverPieces[block];
            if (complete) {
                entry = DriverPieces{};
                entry.whole = stampGeneration;
            } else {
                entry.possibleWhole = stampGeneration;
                entry.possibleDropped = 0;
                entry.possiblePieces = {};
            }
            return;
        }
        auto& entry = driverPieces[block];
        auto& pieces = complete ? entry.pieces : entry.possiblePieces;
        auto& dropped = complete ? entry.dropped : entry.possibleDropped;
        DriverPiece* contained = nullptr;
        for (auto& piece : pieces) {
            if (piece.generation == 0 || piece.begin < from || piece.end > to) continue;
            if (contained == nullptr) contained = &piece;
            else piece = {};
        }
        if (contained != nullptr) {
            *contained = {stampGeneration, from, to};
            return;
        }
        auto* oldest = &pieces.front();
        for (auto& piece : pieces) {
            if (piece.generation < oldest->generation) oldest = &piece;
        }
        dropped = std::max(dropped, oldest->generation);
        *oldest = {stampGeneration, from, to};
    }

    Leaf& leafFor(std::uint64_t block) {
        auto& leaf = leaves[block / LeafBlocks];
        if (leaf == nullptr) leaf = std::make_unique<Leaf>();
        return *leaf;
    }

#ifdef _WIN32
    void stampHint(std::uint64_t block) {
        leafFor(block).hintBlocks[block % LeafBlocks] = generation;
    }

    std::uint64_t hintOf(std::uint64_t block) const {
        const auto& leaf = leaves[block / LeafBlocks];
        return leaf != nullptr ? leaf->hintBlocks[block % LeafBlocks] : 0;
    }
#endif

    std::uint64_t stampOf(std::uint64_t block) const {
        const auto& leaf = leaves[block / LeafBlocks];
        return leaf != nullptr ? leaf->blocks[block % LeafBlocks] : 0;
    }

    // Driver stores outside write watching, hashed by absolute block: 2^16 slots by default (one slot
    // per 64 KiB block modulo 4 GiB), so a store into unwatched memory also advances the slot of every
    // block 4 GiB * k away from it, watched or not. APS5_UNSTAMPED_SLOTS_LOG2 (4..20) narrows or widens
    // the table (tests and the in-game alias reproduction: 12 makes every unwatched store alias a block
    // of any range of 256 MiB or more). Written under the mutex, read without it by the GEN_LOCKFREE
    // answers: allocated once and published with release.
    static std::size_t unstampedSlotCount() {
        static const std::size_t count = [] {
            const char* text = std::getenv("APS5_UNSTAMPED_SLOTS_LOG2");
            const auto log2 = text != nullptr && *text != '\0' ? std::clamp<unsigned long>(std::strtoul(text, nullptr, 10), 4ul, 20ul) : 16ul;
            return std::size_t{1} << log2;
        }();
        return count;
    }
    std::atomic<std::atomic<std::uint64_t>*> unstampedStores{nullptr};
    // The last store that set each slot ([begin, end), under the mutex): the FIX_UNSTAMPED_ALIAS verify
    // check and the large-texture diag name it.
    std::unique_ptr<std::pair<std::uint64_t, std::uint64_t>[]> unstampedRanges;

    void noteUnstampedStore(std::uint64_t address, std::uint64_t end) {
        const auto count = unstampedSlotCount();
        auto* slots = unstampedStores.load(std::memory_order_relaxed);
        if (slots == nullptr) {
            slots = new std::atomic<std::uint64_t>[count]();
            unstampedRanges = std::make_unique<std::pair<std::uint64_t, std::uint64_t>[]>(count);
            unstampedStores.store(slots, std::memory_order_release);
        }
        const auto value = generation.load(std::memory_order_relaxed);
        const auto first = address / WriteBlockBytes;
        const auto last = (end - 1) / WriteBlockBytes;
        if (last - first >= count) {
            for (std::size_t i = 0; i < count; ++i) {
                slots[i].store(value, std::memory_order_release);
                unstampedRanges[i] = {address, end};
            }
            return;
        }
        for (auto block = first; block <= last; ++block) {
            slots[block % count].store(value, std::memory_order_release);
            unstampedRanges[block % count] = {address, end};
        }
    }

    std::uint64_t newestUnstamped(std::uint64_t address, std::uint64_t end) const {
        const auto* slots = unstampedStores.load(std::memory_order_acquire);
        if (slots == nullptr || end <= address) return 0;
        const auto count = unstampedSlotCount();
        std::uint64_t newest = 0;
        const auto first = address / WriteBlockBytes;
        const auto last = (end - 1) / WriteBlockBytes;
        if (last - first >= count) {
            for (std::size_t i = 0; i < count; ++i) newest = std::max(newest, slots[i].load(std::memory_order_acquire));
        } else {
            for (auto block = first; block <= last; ++block) newest = std::max(newest, slots[block % count].load(std::memory_order_acquire));
        }
        return newest;
    }

    // The block of [address, end) whose unstamped slot is newest, the slot's value and the last store
    // that set it (under the mutex); value 0 when no slot of the range was ever set.
    struct UnstampedSlot {
        std::uint64_t block = 0;
        std::uint64_t value = 0;
        std::pair<std::uint64_t, std::uint64_t> store{};
    };
    UnstampedSlot newestUnstampedSlot(std::uint64_t address, std::uint64_t end) const {
        const auto* slots = unstampedStores.load(std::memory_order_acquire);
        if (slots == nullptr || end <= address) return {};
        const auto count = unstampedSlotCount();
        UnstampedSlot found;
        const auto first = address / WriteBlockBytes;
        const auto last = std::min<std::uint64_t>((end - 1) / WriteBlockBytes, first + count - 1);
        for (auto block = first; block <= last; ++block) {
            const auto value = slots[block % count].load(std::memory_order_acquire);
            if (value > found.value) found = {block, value, unstampedRanges[block % count]};
        }
        return found;
    }

    // newestStamp without the unstamped slots: the newest block stamp of [address, end).
    std::uint64_t newestBlockStamp(std::uint64_t address, std::uint64_t end) const {
        if (!watched) return 0;
#ifdef _WIN32
        address = std::max<std::uint64_t>(address, base);
        end = std::min<std::uint64_t>(end, base + size);
#else
        end = std::min<std::uint64_t>(end, LeafCount * LeafBlocks * WriteBlockBytes);
#endif
        std::uint64_t newest = 0;
        if (address >= end) return newest;
        for (auto block = blockOf(address); block <= blockOf(end - 1); ++block) newest = std::max(newest, stampOf(block));
        return newest;
    }

    // With FIX_UNSTAMPED_ALIAS the slots count only for a range not write-watched in full (a watched
    // block's own stores stamp it; its slot holds only aliases of unwatched stores).
    std::uint64_t newestStamp(std::uint64_t address, std::uint64_t end) const {
        std::uint64_t newest = unstampedAliasActive() && watched && end > address && covers(address, static_cast<std::size_t>(end - address)) ? 0 : newestUnstamped(address, end);
        if (!watched) return newest;
#ifdef _WIN32
        address = std::max<std::uint64_t>(address, base);
        end = std::min<std::uint64_t>(end, base + size);
#else
        end = std::min<std::uint64_t>(end, LeafCount * LeafBlocks * WriteBlockBytes);
#endif
        if (address >= end) return newest;
        for (auto block = blockOf(address); block <= blockOf(end - 1); ++block) newest = std::max(newest, stampOf(block));
        return newest;
    }

    std::uint64_t coarseStampOf(std::uint64_t block) const {
        const auto& leaf = leaves[block / LeafBlocks];
        return leaf != nullptr ? leaf->coarseBlocks[(block % LeafBlocks) / CoarseBlocks] : 0;
    }

    bool unchanged(std::uint64_t first, std::uint64_t last, std::uint64_t since) const {
        if (first == last) return stampOf(first) <= since;
        const auto newer = [since](std::uint64_t stamp) { return stamp > since; };
        while (first <= last) {
            const auto& leaf = leaves[first / LeafBlocks];
            if (leaf == nullptr) {
                first = (first / LeafBlocks + 1) * LeafBlocks;
                continue;
            }
            const auto end = std::min<std::uint64_t>(last + 1, (first / CoarseBlocks + 1) * CoarseBlocks);
            if (end - first < CoarseBlocks || coarseStampOf(first) > since) {
                const auto begin = leaf->blocks.begin() + first % LeafBlocks;
                if (std::any_of(begin, begin + (end - first), newer)) return false;
            }
            first = end;
        }
        return true;
    }

    std::uint64_t cpuStampOf(std::uint64_t block) const {
        const auto& leaf = leaves[block / LeafBlocks];
        return leaf != nullptr ? leaf->cpuBlocks[block % LeafBlocks] : 0;
    }

    bool storedOver(std::uint64_t address, std::uint64_t end, std::uint64_t generation, bool walked) const {
        for (auto block = blockOf(address); block <= blockOf(end - 1); ++block) {
            if (stampOf(block) <= generation) continue;
            const auto begin = blockBegin(block);
            const auto from = std::max(address, begin) - begin;
            const auto to = std::min<std::uint64_t>(end, begin + WriteBlockBytes) - begin;
            const auto newerPage = [&](const std::unordered_map<std::uint64_t, CpuPages>& pages, std::uint64_t first, std::uint64_t stop) {
                const auto found = pages.find(block);
                if (found == pages.end()) return false;
                for (auto page = first / WritePageBytes; page <= (stop - 1) / WritePageBytes; ++page) {
                    if (found->second.pages[page] > generation) return true;
                }
                return false;
            };
            if (cpuPages.find(block) == cpuPages.end() && cpuStampOf(block) > generation) return true;
            if (newerPage(cpuPages, from, to)) return true;
            if (walked && newerPage(walkPages, from, to)) return true;
            const auto found = driverPieces.find(block);
            if (found == driverPieces.end()) {
                if (!walked && newerPage(walkPages, 0, WriteBlockBytes)) return true;
                continue;
            }
            const auto& entry = found->second;
            if (entry.whole > generation || entry.dropped > generation || entry.possibleWhole > generation || entry.possibleDropped > generation) return true;
            for (const auto& piece : entry.pieces) {
                if (piece.generation > generation && piece.begin < to && from < piece.end) return true;
            }
            for (const auto& piece : entry.possiblePieces) {
                if (piece.generation > generation && piece.begin < to && from < piece.end) return true;
            }
        }
        return false;
    }

    bool refusedSince(std::uint64_t address, std::uint64_t end, std::uint64_t since) const {
        const auto last = blockOf(end - 1);
        for (auto it = refused.lower_bound(blockOf(address)); it != refused.end() && it->first <= last; ++it) {
            if (it->second > since) return true;
        }
        return false;
    }

    // APS5_ARCHB_FIX_SNAPSHOT_PAGE_REFRESH: rangeUnchanged per 4 KiB page of [address, end), with each
    // block's evidence looked up once. storedOver decomposes over the pages of a block (its whole-block
    // terms report every page, its page and piece terms only the pages they overlap), so a page is left
    // out exactly when rangeUnchanged(page) holds. Pages that may have changed are appended to `runs`.
    void changedPages(std::uint64_t address, std::uint64_t end, std::uint64_t since, std::vector<std::pair<std::uint64_t, std::uint64_t>>& runs) const {
        const auto add = [&](std::uint64_t from, std::uint64_t to) {
            if (!runs.empty() && runs.back().second == from) runs.back().second = to;
            else runs.emplace_back(from, to);
        };
        for (auto block = blockOf(address); block <= blockOf(end - 1); ++block) {
            const auto begin = blockBegin(block);
            const auto from = std::max(address, begin);
            const auto to = std::min<std::uint64_t>(end, begin + WriteBlockBytes);
            if (refusedSince(from, to, since)) {
                add(from, to);
                continue;
            }
            if (stampOf(block) <= since) continue;
            const auto cpu = cpuPages.find(block);
            const auto walk = walkPages.find(block);
            const auto driver = driverPieces.find(block);
            bool whole = cpu == cpuPages.end() && cpuStampOf(block) > since;
            if (!whole && driver != driverPieces.end()) {
                const auto& entry = driver->second;
                whole = entry.whole > since || entry.dropped > since || entry.possibleWhole > since || entry.possibleDropped > since;
            }
            if (whole) {
                add(from, to);
                continue;
            }
            for (auto page = (from - begin) / WritePageBytes; page <= (to - 1 - begin) / WritePageBytes; ++page) {
                const auto pageFrom = std::max<std::uint64_t>(from, begin + page * WritePageBytes);
                const auto pageTo = std::min<std::uint64_t>(to, begin + (page + 1) * WritePageBytes);
                bool stored = (cpu != cpuPages.end() && cpu->second.pages[page] > since) || (walk != walkPages.end() && walk->second.pages[page] > since);
                if (!stored && driver != driverPieces.end()) {
                    const auto lo = pageFrom - begin;
                    const auto hi = pageTo - begin;
                    for (const auto& piece : driver->second.pieces) stored = stored || (piece.generation > since && piece.begin < hi && lo < piece.end);
                    for (const auto& piece : driver->second.possiblePieces) stored = stored || (piece.generation > since && piece.begin < hi && lo < piece.end);
                }
                if (stored) add(pageFrom, pageTo);
            }
        }
    }

    bool rangeUnchanged(std::uint64_t address, std::size_t bytes, std::uint64_t since, bool walked = true) const {
        if (refusedSince(address, address + bytes, since)) return false;
        return unchanged(blockOf(address), blockOf(address + bytes - 1), since) || !storedOver(address, address + bytes, since, walked);
    }

    bool cpuUnchanged(std::uint64_t address, std::size_t bytes, std::uint64_t generation) const {
        const auto first = blockOf(address);
        const auto last = blockOf(address + bytes - 1u);
        const auto end = address + bytes;
        for (auto block = first; block <= last; ++block) {
            if (const auto found = cpuPages.find(block); found != cpuPages.end()) {
                const auto begin = blockBegin(block);
                const auto from = std::max(address, begin) - begin;
                const auto to = std::min<std::uint64_t>(end, begin + WriteBlockBytes) - begin;
                for (auto page = from / WritePageBytes; page <= (to - 1u) / WritePageBytes; ++page) {
                    if (found->second.pages[page] > generation) return false;
                }
            } else if (cpuStampOf(block) > generation) {
                return false;
            }
        }
        return true;
    }

    void stamp(std::uint64_t block, std::uint64_t stampGeneration, StampKind kind) {
        if (const auto observer = stampObserver.load(std::memory_order_acquire)) observer(blockBegin(block), blockBegin(block) + WriteBlockBytes, TrackerEvent::Stamp);
        const bool cpu = kind == StampKind::Cpu || kind == StampKind::ImportWindow;
        const bool stored = kind != StampKind::ImportWindow;
        auto& leaf = leafFor(block);
        if (stored) {
            leaf.blocks[block % LeafBlocks] = stampGeneration;
            auto& coarse = leaf.coarseBlocks[(block % LeafBlocks) / CoarseBlocks];
            coarse = std::max(coarse, stampGeneration);
        }
        if (cpu) leaf.cpuBlocks[block % LeafBlocks] = stampGeneration;
    }
};

WriteTracker& Tracker() {
    static WriteTracker tracker;
    return tracker;
}

// Takes the tracker mutex; under APS5_PROFILE_DRAW an acquisition that found it held is counted
// ('tracker waits N / M' in [guestmem]), which says whether walking outside the lock would pay.
namespace census {
bool enabled();
bool timeLock(AgcDriver::Mutex& mutex);
void liveFlip();
}

class TrackerLock {
public:
    explicit TrackerLock(WriteTracker& tracker) : tracker(tracker), lock(tracker.mutex, std::defer_lock) {
        static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
        if (!profile) {
            if (census::enabled() && census::timeLock(tracker.mutex)) {
                lock = std::unique_lock<AgcDriver::Mutex>(tracker.mutex, std::adopt_lock);
                return;
            }
            auto* waits = trackerTimedWaits();
            if (waits != nullptr && !lock.try_lock()) {
                const bool cheap = ArchB::CheapClock::Active();
                const auto start = ArchB::CheapClock::Now(cheap);
                lock.lock();
                waits->fetch_add(1, std::memory_order_relaxed);
                trackerTimedWaitNs()->fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(ArchB::CheapClock::Now(cheap) - start).count()), std::memory_order_relaxed);
                return;
            }
            if (!lock.owns_lock()) lock.lock();
            return;
        }
        if (!lock.try_lock()) {
            trackerWaits.fetch_add(1, std::memory_order_relaxed);
            lock.lock();
        }
        trackerAcquisitions.fetch_add(1, std::memory_order_relaxed);
    }
    ~TrackerLock() {
        if (lock.owns_lock()) publish();
    }
    TrackerLock(const TrackerLock&) = delete;
    TrackerLock& operator=(const TrackerLock&) = delete;

    void Unlock() {
        publish();
        lock.unlock();
    }

    void Suspend() {
        lock.unlock();
    }

    void Resume() {
        lock.lock();
    }

    class Suspension {
    public:
        explicit Suspension(TrackerLock& owner) : owner(owner) {
            owner.Suspend();
        }

        ~Suspension() {
            owner.Resume();
        }

        Suspension(const Suspension&) = delete;
        Suspension& operator=(const Suspension&) = delete;

    private:
        TrackerLock& owner;
    };

private:
    void publish() {
        const auto current = tracker.generation.load(std::memory_order_relaxed);
        if (tracker.stable.load(std::memory_order_relaxed) != current) tracker.stable.store(current, std::memory_order_release);
    }

    WriteTracker& tracker;
    std::unique_lock<AgcDriver::Mutex> lock;
};

TrackerLock lockTracker(WriteTracker& tracker) {
    return TrackerLock(tracker);
}

// Write-generation shadow (APS5_ARCHB_GEN_SHADOW): every answer of the tracker's query APIs is also
// asked of the generation table (lock-free Check over data, meta and pending at the consumer's own
// since, which is a GenClock value while the table is on) and classified against the answer the
// tracker gave, for the consumer named by the calling thread's Gen::ConsumerScope. Under the
// tracker mutex like the query itself, so the stamps it compares were all published.
struct ShadowVerdict {
    Gen::Verdict verdict = Gen::Verdict::Clean;
    Gen::UnknownReason reason = Gen::UnknownReason::None;
    bool needsSweep = false;
};

Gen::UnknownReason uncoveredReason(const WriteTracker& tracker, std::uint64_t address, std::size_t bytes) {
    if (!tracker.watched) return Gen::UnknownReason::Outside;
#ifdef _WIN32
    if (address < tracker.base || bytes > tracker.size || address - tracker.base > tracker.size - bytes) return Gen::UnknownReason::Outside;
    if (!tracker.coverage.Covers(address, bytes)) return Gen::UnknownReason::Excluded;
    return Gen::UnknownReason::HostRegion;
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    return Gen::UnknownReason::Outside;
#endif
}

// Changed > Unknown > PendingOnly > Clean.
void combineVerdict(ShadowVerdict& into, const ShadowVerdict& next) {
    const auto rank = [](Gen::Verdict verdict) {
        switch (verdict) {
        case Gen::Verdict::Changed: return 3;
        case Gen::Verdict::Unknown: return 2;
        case Gen::Verdict::PendingOnly: return 1;
        case Gen::Verdict::Clean: return 0;
        }
        return 3;
    };
    if (rank(next.verdict) > rank(into.verdict)) {
        into.verdict = next.verdict;
        into.reason = next.reason;
    }
    into.needsSweep = into.needsSweep || next.needsSweep;
}

ShadowVerdict shadowCheck(const WriteTracker& tracker, std::uint64_t address, std::size_t bytes, std::uint64_t since, bool covered, bool exhaustive = false, std::uint8_t kinds = Gen::Data | Gen::Meta | Gen::Pending) {
    ShadowVerdict result;
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) {
        result.verdict = Gen::Verdict::Unknown;
        result.reason = Gen::UnknownReason::Outside;
        return result;
    }
    if (!covered) {
        result.verdict = Gen::Verdict::Unknown;
        result.reason = since == 0 ? Gen::UnknownReason::NoSince : uncoveredReason(tracker, address, bytes);
        return result;
    }
    auto& table = Gen::Global();
    result.verdict = exhaustive ? table.CheckExhaustive(address, address + bytes, since, 0, kinds, &result.reason) : table.Check(address, address + bytes, since, 0, kinds, &result.reason);
    const auto required = Gen::Required();
    result.needsSweep = required == 0 || !table.Covered(address, address + bytes, required);
    // Check at the context's ticket: without a covering sweep it would sweep first, so fields that
    // read clean are only 'clean unless the sweep finds a write' (unknown_not_swept), never Clean.
    if (result.needsSweep && result.verdict == Gen::Verdict::Clean) {
        result.verdict = Gen::Verdict::Unknown;
        result.reason = Gen::UnknownReason::NotSwept;
    }
    return result;
}

// The verifier (APS5_ARCHB_GEN_VERIFY=N, 1 in N shadow verdicts): the exhaustive check must give
// the same verdict, and no block may carry a tracker stamp newer than its table data (a stamp the
// table missed). Stamps from before the reset floor (the table was off) are not compared.
// One range of a decision, with whether the tracker covered it (watched, a since, inside coverage).
struct ShadowRange {
    std::uint64_t address;
    std::size_t bytes;
    std::uint64_t since;
    bool covered;
};

void verifyShadow(const WriteTracker& tracker, Gen::Consumer consumer, std::span<const ShadowRange> ranges, bool unchanged, const ShadowVerdict& fast) {
    ShadowVerdict slow;
    bool missed = false;
    const auto floor = Gen::Global().ResetFloor();
    for (const auto& range : ranges) {
        combineVerdict(slow, shadowCheck(tracker, range.address, range.bytes, range.since, range.covered, true));
        if (!range.covered || range.bytes == 0) continue;
        for (auto block = tracker.blockOf(range.address); block <= tracker.blockOf(range.address + range.bytes - 1); ++block) {
            const auto stamp = tracker.stampOf(block);
            if (stamp >= floor && Gen::Global().Fields(tracker.blockBegin(block)).data < stamp) missed = true;
        }
    }
    // The fast verdict may have been lowered to unknown_not_swept by libc's unswept-write probe
    // (UnchangedSinceClean); the exhaustive pass agrees when its fields read clean or pending.
    if (fast.verdict == Gen::Verdict::Unknown && fast.reason == Gen::UnknownReason::NotSwept && (slow.verdict == Gen::Verdict::Clean || slow.verdict == Gen::Verdict::PendingOnly)) slow = fast;
    Gen::Outcome outcome;
    if (slow.verdict == Gen::Verdict::Clean) outcome = unchanged ? Gen::Outcome::AgreeClean : Gen::Outcome::Unsafe;
    else outcome = unchanged ? Gen::Outcome::Conservative : Gen::Outcome::AgreeChanged;
    const bool differs = slow.verdict != fast.verdict;
    if (missed || differs) outcome = Gen::Outcome::Unsafe;
    Gen::CountVerify(consumer, outcome);
    const auto& first = ranges.front();
    if (outcome == Gen::Outcome::Unsafe) Gen::ReportUnsafe(consumer, first.address, first.address + first.bytes, first.since, missed ? "verify: the table missed a tracker stamp" : differs ? "verify: exhaustive verdict differs" : "verify: clean while the tracker found a change");
}

void classifyShadow(const WriteTracker& tracker, std::span<const ShadowRange> ranges, bool unchanged, const ShadowVerdict& verdict, const std::source_location& site) {
    if (ranges.empty()) return;
    const auto consumer = Gen::CurrentConsumer();
    if (consumer == Gen::Consumer::Other) Gen::NoteOtherSite(site.file_name(), site.line());
    const auto outcome = Gen::Classify(consumer, verdict.verdict, verdict.reason, unchanged, verdict.needsSweep);
    // A conservative Changed: whether data alone would have read clean (the change was meta).
    if (outcome == Gen::Outcome::Conservative && verdict.verdict == Gen::Verdict::Changed) {
        bool dataMoved = false;
        for (const auto& range : ranges) dataMoved = dataMoved || shadowCheck(tracker, range.address, range.bytes, range.since, range.covered, false, Gen::Data | Gen::Cpu).verdict == Gen::Verdict::Changed;
        if (!dataMoved) Gen::CountConservativeMeta(consumer);
    }
    const auto& first = ranges.front();
    if (outcome == Gen::Outcome::Unsafe) Gen::ReportUnsafe(consumer, first.address, first.address + first.bytes, first.since, "clean while the tracker found a change");
    if (Gen::VerifySample()) verifyShadow(tracker, consumer, ranges, unchanged, verdict);
}

// One tracker answer over one range.
void shadowQuery(const WriteTracker& tracker, std::uint64_t address, std::size_t bytes, std::uint64_t since, bool covered, bool unchanged, const std::source_location& site) {
    const std::array<ShadowRange, 1> ranges{ShadowRange{address, bytes, since, covered}};
    classifyShadow(tracker, ranges, unchanged, shadowCheck(tracker, address, bytes, since, covered), site);
}

// Several ranges, one decision (UnchangedSinceAll, UnchangedSinceClean, UnchangedCommittedSince),
// gathered in a per-thread scratch vector (no allocation once it has grown).
void shadowRanges(const WriteTracker& tracker, std::span<const ShadowRange> ranges, bool unchanged, const std::source_location& site) {
    ShadowVerdict combined;
    for (const auto& range : ranges) combineVerdict(combined, shadowCheck(tracker, range.address, range.bytes, range.since, range.covered));
    classifyShadow(tracker, ranges, unchanged, combined, site);
}

std::vector<ShadowRange>& shadowScratch() {
    struct ScratchStorage {};
    return AgcDriver::ThreadScratch<std::vector<ShadowRange>, ScratchStorage>();
}

#ifdef _WIN32
void watchPrivateMapping(std::uintptr_t address, std::size_t bytes, std::uint64_t generation) {
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    if (!tracker.watched || bytes == 0 || bytes > tracker.size || address < tracker.base || address - tracker.base > tracker.size - bytes) return;
    // The generation first, published to the table before the coverage it restores and before the
    // tracker's stamps: a lock-free reader that sees the restored coverage also sees the new data.
    tracker.bump();
    const bool gen = Gen::Active();
    if (gen) static_cast<void>(Gen::Note(Gen::Event::WatchMapping, address, address + bytes, Gen::Data | Gen::Cpu | Gen::Meta, tracker.generation.load(std::memory_order_relaxed)));
    tracker.coverage.Restore(address, bytes, generation);
    tracker.noteCpuStore(address, address + bytes, tracker.generation);
    for (auto block = tracker.blockOf(address); block <= tracker.blockOf(address + bytes - 1); ++block) {
        if (const auto& leaf = tracker.leaves[block / WriteTracker::LeafBlocks]; leaf != nullptr) leaf->hintBlocks[block % WriteTracker::LeafBlocks] = 0;
        if (const auto found = tracker.driverPieces.find(block); found != tracker.driverPieces.end()) found->second.dropped = std::max<std::uint64_t>(found->second.dropped, tracker.generation);
        tracker.stamp(block, tracker.generation, StampKind::Cpu);
    }
    if (gen) {
        static_cast<void>(Gen::Global().SetState(address, address + bytes, Gen::BlockState::Tracked, true));
        Gen::Global().DropCoverage(address, address + bytes);
    }
    unwatchSerial.fetch_add(1, std::memory_order_release);
}
#endif

// Collect epochs are per thread and globally unique: every bump takes a fresh value from one counter,
// so a memo entry (stamped with the epoch of its walk) can only match the thread that made it, and
// only until that thread's next ordering point (the queue workers bump at theirs, see the header). A
// thread that never bumped is given a new epoch for every collect: its walks are never reused (the
// presenter, game threads inside the flush hook), as nothing orders the CPU's writes for it.
std::atomic<std::uint64_t> nextCollectEpoch{1};
struct CollectEpochSlot {};
std::atomic<std::uint64_t> lastBumpedEpoch{0};

std::uint64_t currentCollectEpoch() {
    if (const auto epoch = HostThreadSlot<std::uint64_t, CollectEpochSlot>::Get(); epoch != 0) return epoch;
    return nextCollectEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool collectMemoEnabled() {
    static const bool disabled = std::getenv("APS5_NO_COLLECT_MEMO") != nullptr;
    return !disabled;
}

// The per-thread memo ring (see WriteTracker::Memo): sized to a packet's working set (a dispatch
// collects each of its few dozen image surfaces in stage A and again in stage B). Entries of a
// thread that never bumps its epoch never match (each collect gets a fresh epoch), as intended.
// APS5_SHARED_COLLECT_MEMO=1 uses the tracker's shared ring under its mutex instead.
struct ThreadCollectMemo {
    std::array<WriteTracker::Memo, 64> entries{};
    std::size_t next = 0;
};
ThreadCollectMemo& threadCollectMemo() {
    struct Slot {};
    auto* memo = HostThreadSlot<ThreadCollectMemo*, Slot>::Get();
    if (memo == nullptr) {
        ShaderRecompiler::ThreadOwned(memo);
        HostThreadSlot<ThreadCollectMemo*, Slot>::Set(memo);
    }
    return *memo;
}

// Both maps enter the caller's epoch together. By default an epoch change clears them (freeing every
// node); under APS5_ARCHB_FIX_ALLOC_CHURN the nodes are pooled and reused by the next epoch's walks
// (EpochMemoMap: an entry of another epoch is never found, and the pool stays bounded).
struct EpochCollectMap {
    using Ranges = std::map<std::uintptr_t, std::uintptr_t>;
    std::uint64_t epoch = 0;
    std::uint64_t unwatched = 0;
    std::uint64_t mapping = 0;
    Ranges ranges;
    std::vector<Ranges::node_type> pool;
    bool keep = false;
    EpochMemoMap<WriteTracker::Memo> failedRanges;

    bool Covers(std::uintptr_t begin, std::uintptr_t end) const {
        if (end <= begin) return false;
        const auto next = ranges.upper_bound(begin);
        return next != ranges.begin() && std::prev(next)->second >= end;
    }

    void Record(std::uintptr_t begin, std::uintptr_t end) {
        auto next = ranges.upper_bound(begin);
        if (next != ranges.begin()) {
            const auto previous = std::prev(next);
            if (previous->second >= begin) {
                previous->second = std::max(end, previous->second);
                while (next != ranges.end() && next->first <= previous->second) {
                    previous->second = std::max(previous->second, next->second);
                    next = Drop(next);
                }
                return;
            }
        }
        while (next != ranges.end() && next->first <= end) {
            end = std::max(end, next->second);
            next = Drop(next);
        }
        Insert(next, begin, end);
    }

    void Reset(bool keepNodes) {
        keep = keepNodes;
        if (!keep) {
            ranges.clear();
            pool.clear();
            return;
        }
        while (!ranges.empty() && pool.size() < EpochMemoMap<WriteTracker::Memo>::KeepCap) pool.push_back(ranges.extract(ranges.begin()));
        ranges.clear();
    }

private:
    Ranges::iterator Drop(Ranges::iterator at) {
        if (!keep || pool.size() >= EpochMemoMap<WriteTracker::Memo>::KeepCap) return ranges.erase(at);
        const auto next = std::next(at);
        pool.push_back(ranges.extract(at));
        return next;
    }

    void Insert(Ranges::iterator hint, std::uintptr_t begin, std::uintptr_t end) {
        if (pool.empty()) {
            ranges.emplace_hint(hint, begin, end);
            return;
        }
        auto node = std::move(pool.back());
        pool.pop_back();
        node.key() = begin;
        node.mapped() = end;
        ranges.insert(hint, std::move(node));
        ArchB::Recorder::Count("fix_alloc_churn_memo_reuses");
    }
};

EpochCollectMap& epochCollectMap(std::uint64_t epoch, std::uint64_t unwatched, std::uint64_t mapping) {
    struct Slot {};
    auto& map = ThreadScratch<EpochCollectMap, Slot>();
    if (map.epoch != epoch || map.unwatched != unwatched || map.mapping != mapping) {
        // Decided per epoch at its first collect, which often comes from a packet outside any draw
        // (the epoch starts at an ordering point), so the live switch counts as well as the draw's.
        const bool keep = ArchB::AllocChurnActive() || ArchB::Switches::Enabled(ArchB::Feature::FixAllocChurn);
        map.Reset(keep);
        map.failedRanges.Enter(epoch, keep);
        map.epoch = epoch;
        map.unwatched = unwatched;
        map.mapping = mapping;
    }
    return map;
}

// An insert that took a node kept from an earlier epoch (no allocation).
void noteMemoReuse(EpochMemoMap<WriteTracker::Memo>::SlotState state) {
    if (state == EpochMemoMap<WriteTracker::Memo>::SlotState::Reused) ArchB::Recorder::Count("fix_alloc_churn_memo_reuses");
}

bool epochCollectMapEnabled() {
    static const bool enabled = std::getenv("APS5_NO_EPOCH_COLLECT_MAP") == nullptr;
    return enabled;
}

bool failedCollectMemoEnabled() {
    static const bool enabled = std::getenv("APS5_NO_FAILED_COLLECT_MEMO") == nullptr;
    return enabled;
}

bool sharedCollectMemo() {
    static const bool shared = std::getenv("APS5_SHARED_COLLECT_MEMO") != nullptr;
    return shared;
}

// Write-tracking census (APS5_ARCHB_WRITE_TRACKING_CENSUS=1, see ReportWriteCensus): the outermost
// tracking entry on a thread opens a scope keyed by queue tag, entry and calling site; the libc
// write-watch work done inside it (GuestArenaWriteWatchCensus deltas) and each walk's overlap with
// the walks of the same collect epoch on that thread are added to it.
namespace census {

bool enabled() {
    static const bool on = [] {
        const auto* value = std::getenv("APS5_ARCHB_WRITE_TRACKING_CENSUS");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return on;
}

struct Row {
    std::uint64_t calls = 0;
    std::uint64_t walks = 0;
    std::uint64_t exact = 0;
    std::uint64_t covered = 0;
    std::uint64_t partial = 0;
    std::uint64_t walkPages = 0;
    std::uint64_t repeatPages = 0;
    std::uint64_t unbumped = 0;
    std::uint64_t collects = 0;
    std::uint64_t queries = 0;
    std::uint64_t resets = 0;
    std::uint64_t osPages = 0;
    std::uint64_t dirty = 0;
    std::uint64_t osNs = 0;
    std::uint64_t ns = 0;
    std::uint64_t widenCovered = 0;
    std::uint64_t widenCalls = 0;
    std::uint64_t widenPages = 0;
    std::uint64_t lockWaits = 0;
    std::uint64_t lockNs = 0;
    std::array<std::uint64_t, 6> outcomes{};
    std::array<std::uint64_t, 6> outcomeNs{};

    void add(const Row& other) {
        for (std::size_t i = 0; i < outcomes.size(); ++i) {
            outcomes[i] += other.outcomes[i];
            outcomeNs[i] += other.outcomeNs[i];
        }
        lockWaits += other.lockWaits;
        lockNs += other.lockNs;
        widenCovered += other.widenCovered;
        widenCalls += other.widenCalls;
        widenPages += other.widenPages;
        calls += other.calls;
        walks += other.walks;
        exact += other.exact;
        covered += other.covered;
        partial += other.partial;
        walkPages += other.walkPages;
        repeatPages += other.repeatPages;
        unbumped += other.unbumped;
        collects += other.collects;
        queries += other.queries;
        resets += other.resets;
        osPages += other.osPages;
        dirty += other.dirty;
        osNs += other.osNs;
        ns += other.ns;
    }
};

struct Key {
    std::uint32_t tag;
    const char* entry;
    const char* file;
    std::uint32_t line;
    const char* function;
    bool operator<(const Key& other) const {
        return std::tie(tag, entry, file, line) < std::tie(other.tag, other.entry, other.file, other.line);
    }
};

std::mutex& tableMutex() {
    static auto* mutex = new std::mutex;
    return *mutex;
}

std::map<Key, Row>& table() {
    static auto* rows = new std::map<Key, Row>;
    return *rows;
}

struct EpochWalks {
    std::uint64_t epoch = 0;
    std::map<std::uint64_t, std::uint64_t> pages;
    std::map<std::uint64_t, std::uint64_t> widened;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> exact;
};

std::uint64_t overlapPages(const std::map<std::uint64_t, std::uint64_t>& ranges, std::uint64_t first, std::uint64_t stop) {
    std::uint64_t seen = 0;
    auto it = ranges.upper_bound(first);
    if (it != ranges.begin()) --it;
    for (; it != ranges.end() && it->first < stop; ++it) {
        const auto from = std::max(first, it->first);
        const auto to = std::min(stop, it->second);
        if (from < to) seen += (to - from) / WritePageBytes;
    }
    return seen;
}

void insertRange(std::map<std::uint64_t, std::uint64_t>& ranges, std::uint64_t begin, std::uint64_t end) {
    auto merge = ranges.lower_bound(begin);
    if (merge != ranges.begin() && std::prev(merge)->second >= begin) --merge;
    while (merge != ranges.end() && merge->first <= end) {
        begin = std::min(begin, merge->first);
        end = std::max(end, merge->second);
        merge = ranges.erase(merge);
    }
    ranges.emplace(begin, end);
}

enum Outcome : int { OutcomeHit = 0, OutcomeClean = 1, OutcomeChanged = 2, OutcomeFailed = 3, OutcomeSkip = 4, OutcomeOther = 5 };
constexpr const char* OutcomeNames[6] = {"hit", "clean", "changed", "failed", "skip", "other"};

class Scope;
struct ActiveScopeSlot {};
Scope* activeScope() { return HostThreadSlot<Scope*, ActiveScopeSlot>::Get(); }
void activeScope(Scope* value) { HostThreadSlot<Scope*, ActiveScopeSlot>::Set(value); }

std::uint32_t threadTag() {
    return IsCommitterThread() ? 0xc0c0c0c0u : GpuLockThreadTag();
}

class Scope {
public:
    Scope(const char* entry, const std::source_location* site = nullptr) {
        if (!enabled() || activeScope() != nullptr) return;
        on = true;
        activeScope(this);
        key = {threadTag(), entry, site != nullptr ? site->file_name() : "", site != nullptr ? site->line() : 0u, site != nullptr ? site->function_name() : ""};
#ifdef _WIN32
        before = GuestArena::GuestArenaWriteWatchCensus_nid_no_patch();
#endif
        start = std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (!on) return;
        activeScope(nullptr);
        row.calls = 1;
        row.ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        const auto kind = static_cast<std::size_t>(result < 0 ? OutcomeOther : result);
        row.outcomes[kind] = 1;
        row.outcomeNs[kind] = row.ns;
#ifdef _WIN32
        const auto after = GuestArena::GuestArenaWriteWatchCensus_nid_no_patch();
        row.collects = after.collects - before.collects;
        row.queries = after.queries - before.queries;
        row.resets = after.resets - before.resets;
        row.osPages = after.pages - before.pages;
        row.dirty = after.dirty - before.dirty;
        row.osNs = after.ns - before.ns;
#endif
        std::lock_guard lock(tableMutex());
        table()[key].add(row);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    static int rank(int outcome) {
        constexpr int ranks[6] = {0, 2, 3, 4, 1, 5};
        return outcome < 0 ? -1 : ranks[outcome];
    }
    void set(int outcome) {
        if (on && rank(outcome) > rank(result)) result = outcome;
    }

    Row row;
    int result = -1;

private:
    bool on = false;
    Key key{};
#ifdef _WIN32
    GuestArena::WriteWatchCensus before{};
#endif
    std::chrono::steady_clock::time_point start{};
};

void note(int outcome) {
    if (activeScope() != nullptr) activeScope()->set(outcome);
}

void noteIfUnset(int outcome) {
    if (activeScope() != nullptr && activeScope()->result < 0) activeScope()->set(outcome);
}

bool answer(bool unchanged) {
    note(unchanged ? OutcomeClean : OutcomeChanged);
    return unchanged;
}

std::uint64_t threadEpoch() {
    return HostThreadSlot<std::uint64_t, CollectEpochSlot>::Get();
}

EpochWalks* currentWalks() {
    const auto epoch = threadEpoch();
    if (epoch == 0) return nullptr;
    struct WalksStorage {};
    auto& walks = HostThreadLocal<EpochWalks, WalksStorage>();
    if (walks.epoch != epoch) {
        walks.epoch = epoch;
        walks.pages.clear();
        walks.widened.clear();
        walks.exact.clear();
    }
    return &walks;
}

// A walk of [first, stop) (4 KiB aligned): repeats are measured against this thread's completed
// clearing walks of the same collect epoch (exact range, fully covered by their union, or
// overlapping it), the evidence a collect memo of the epoch could reuse.
void noteWalk(std::uint64_t first, std::uint64_t stop) {
    if (activeScope() == nullptr || stop <= first) return;
    auto& row = activeScope()->row;
    ++row.walks;
    const auto pages = (stop - first) / WritePageBytes;
    row.walkPages += pages;
    auto* current = currentWalks();
    if (current == nullptr) {
        ++row.unbumped;
        return;
    }
    auto& walks = *current;
    if (std::find(walks.exact.begin(), walks.exact.end(), std::pair{first, stop}) != walks.exact.end()) ++row.exact;
    std::uint64_t seen = 0;
    auto it = walks.pages.upper_bound(first);
    if (it != walks.pages.begin()) --it;
    for (; it != walks.pages.end() && it->first < stop; ++it) {
        const auto from = std::max(first, it->first);
        const auto to = std::min(stop, it->second);
        if (from < to) seen += (to - from) / WritePageBytes;
    }
    row.repeatPages += seen;
    if (seen == pages) ++row.covered;
    else if (seen != 0) ++row.partial;
    if (seen != pages && overlapPages(walks.widened, first, stop) == pages) ++row.widenCovered;
}

// A completed clearing walk (all pages, stamped as CPU or driver stores).
void noteWalked(std::uint64_t first, std::uint64_t stop) {
    if (activeScope() == nullptr || stop <= first) return;
    auto* current = currentWalks();
    if (current == nullptr) return;
    auto& walks = *current;
    if (walks.exact.size() < 4096) walks.exact.emplace_back(first, stop);
    // What widening a small walk to its 64 KiB blocks would have covered (the extra pages it queries).
    if (stop - first <= WriteBlockBytes) {
        const auto wideFirst = first & ~std::uint64_t{WriteBlockBytes - 1};
        const auto wideStop = (stop + WriteBlockBytes - 1) & ~std::uint64_t{WriteBlockBytes - 1};
        ++activeScope()->row.widenCalls;
        activeScope()->row.widenPages += (wideStop - wideFirst - (stop - first)) / WritePageBytes;
        insertRange(walks.widened, wideFirst, wideStop);
    } else insertRange(walks.widened, first, stop);
    auto begin = first;
    auto end = stop;
    auto merge = walks.pages.lower_bound(begin);
    if (merge != walks.pages.begin() && std::prev(merge)->second >= begin) --merge;
    while (merge != walks.pages.end() && merge->first <= end) {
        begin = std::min(begin, merge->first);
        end = std::max(end, merge->second);
        merge = walks.pages.erase(merge);
    }
    walks.pages.emplace(begin, end);
}

// A contended tracker lock acquisition inside a census scope: the time spent waiting.
bool timeLock(AgcDriver::Mutex& mutex) {
    if (activeScope() == nullptr) return false;
    if (mutex.try_lock()) return true;
    const auto start = std::chrono::steady_clock::now();
    mutex.lock();
    ++activeScope()->row.lockWaits;
    activeScope()->row.lockNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    return true;
}

std::uint64_t pageStart() {
    if (!enabled() || activeScope() != nullptr) return 0;
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

void pageEnd(std::uint64_t start, const char* entry, bool clean) {
    const auto now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::duration(static_cast<std::chrono::steady_clock::rep>(now - start))).count());
    Row row;
    row.calls = 1;
    row.ns = ns;
    const auto kind = clean ? OutcomeClean : OutcomeChanged;
    row.outcomes[kind] = 1;
    row.outcomeNs[kind] = ns;
    std::lock_guard lock(tableMutex());
    table()[Key{threadTag(), entry, ReadSiteName(CurrentReadSite()), 0, ""}].add(row);
}

void noteBump() {
    if (!enabled()) return;
    Row row;
    row.calls = 1;
    std::lock_guard lock(tableMutex());
    table()[Key{GpuLockThreadTag(), "epoch_bump", "", 0, ""}].add(row);
}

}

struct StampRuns {
    WriteTracker& tracker;
    StampKind kind;
    std::size_t stamped = 0;
    const WindowCopy* window = nullptr;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> windowRuns;
    // APS5_ARCHB_WT_FAULT_VERIFY: the dirty pages the walk reported.
    std::vector<void*>* dirty = nullptr;
};

// Write generations (APS5_ARCHB_GEN_TABLE): the table event for a tracker stamp, with the stamp's
// own generation (a GenClock value while the table is on). Import-window pages (bytes equal to the
// pre-import hash) bump data too, which is conservative.
void noteStampGen(StampKind kind, std::uint64_t begin, std::uint64_t end, std::uint64_t generation) {
    switch (kind) {
    case StampKind::Cpu: Gen::Note(Gen::Event::CpuSweep, begin, end, Gen::Data | Gen::Cpu, generation); break;
    case StampKind::Driver: Gen::Note(Gen::Event::DriverSweep, begin, end, Gen::Data, generation); break;
    case StampKind::ImportWindow: Gen::Note(Gen::Event::ImportWindow, begin, end, Gen::Data | Gen::Cpu, generation); break;
    case StampKind::Fresh: Gen::Note(Gen::Event::FreshCollect, begin, end, Gen::Data | Gen::Cpu, generation); break;
    }
}

// A driver store or landed GPU write over [begin, end), stamped with `generation` (after the bytes).
void noteDriverStoreGen(std::uint64_t begin, std::uint64_t end, std::uint64_t generation) {
    if (end > begin && Gen::Active()) static_cast<void>(Gen::Note(Gen::Event::DriverStore, begin, end, Gen::Data, generation));
}

namespace stampTrace {
const void*& walkCallerThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<const void*, Slot>();
}
int& walkTagThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<int, Slot>();
}
std::pair<std::uint64_t, std::uint64_t> range() {
    static const auto parsed = [] {
        const char* text = std::getenv("APS5_WT_TRACE_RANGE");
        std::pair<std::uint64_t, std::uint64_t> value{0, 0};
        if (text == nullptr) return value;
        char* rest = nullptr;
        value.first = std::strtoull(text, &rest, 16);
        if (rest != nullptr && *rest == ':') value.second = std::strtoull(rest + 1, nullptr, 16);
        return value;
    }();
    return parsed;
}
void note(const char* what, std::uint64_t begin, std::uint64_t end, std::uint64_t generation, int kind) {
    const auto [first, stop] = range();
    if (stop <= first || end <= first || begin >= stop) return;
    std::fprintf(stderr, "[wt-trace] %s 0x%llx..0x%llx gen %llu kind %d tag %d caller +0x%llx thread %lu\n", what, static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end), static_cast<unsigned long long>(generation), kind, walkTagThreadState(),
                 walkCallerThreadState() != nullptr ? ModuleOffset(walkCallerThreadState()) : 0ull,
#ifdef _WIN32
                 static_cast<unsigned long>(GetCurrentThreadId()));
#else
                 0ul);
#endif
}
}

void noteWrittenRun(StampRuns& runs, std::uintptr_t begin, std::uintptr_t end, std::uint64_t generation) {
    if (runs.kind != StampKind::Driver) runs.tracker.noteCpuStore(begin, end, generation);
    else runs.tracker.noteWalkStore(begin, end, generation);
}

void stampWrittenRun(void* context, std::uintptr_t begin, std::uintptr_t end) {
    auto& runs = *static_cast<StampRuns*>(context);
    auto& tracker = runs.tracker;
    if (end <= begin) return;
    stampTrace::note("stamp", begin, end, tracker.generation.load(std::memory_order_relaxed), static_cast<int>(runs.kind));
    ++runs.stamped;
    // Stage 4 M3 (APS5_WT_FAULT_CENSUS): every dirty page the OS reported, whoever stored it.
    Stage4::NoteCollectedPages(begin, end);
    if (runs.dirty != nullptr) {
        for (auto page = begin & ~std::uintptr_t{WritePageBytes - 1}; page < end; page += WritePageBytes) runs.dirty->push_back(reinterpret_cast<void*>(page));
    }
    // APS5_ARCHB_WT_FAULT: a page found stored stays open across the next flip (hot pages count as
    // changed and do not fault every frame).
    if (tracker.faultMode && !wtFault::flipDirtyWalkThreadState()) GuestArena::GuestWtFaultNoteStored_nid_no_patch(static_cast<std::uintptr_t>(begin), static_cast<std::size_t>(end - begin));
    if (runs.window != nullptr) runs.windowRuns.emplace_back(begin, end);
    else {
        const auto generation = tracker.generation.load(std::memory_order_relaxed);
        noteWrittenRun(runs, begin, end, generation);
        // The table's event first, then the tracker's stamp (both under the tracker mutex): a reader
        // without the mutex (GEN_LOCKFREE) never sees a tracker stamp the table does not hold.
        if (Gen::Active()) noteStampGen(runs.kind, begin, end, generation);
        for (auto block = tracker.blockOf(begin); block <= tracker.blockOf(end - 1); ++block) tracker.stamp(block, generation, runs.kind);
    }
#ifndef _WIN32
    collectDirtyRuns.fetch_add(1, std::memory_order_relaxed);
#endif
}

void stampWindowRuns(StampRuns& runs) {
    const auto pages = PagesOf(runs.windowRuns);
    std::vector<std::uint8_t> held(pages.size(), 0);
    SplitPages(pages.size(), [&](std::size_t from, std::size_t to) {
        for (auto index = from; index < to; ++index) held[index] = runs.window->Holds(static_cast<std::uintptr_t>(pages[index]));
    });
    auto& tracker = runs.tracker;
    const auto generation = tracker.generation.load(std::memory_order_relaxed);
    const bool gen = Gen::Active();
    // A page of a window walk bumps data and cpu in the table whether it keeps the walk's kind
    // or reads as an unchanged import-window page (conservative), so one event covers the run.
    const bool wholeRun = runs.kind == StampKind::Cpu || runs.kind == StampKind::Fresh;
    std::size_t index = 0;
    for (const auto& [begin, end] : runs.windowRuns) {
        noteWrittenRun(runs, static_cast<std::uintptr_t>(begin), static_cast<std::uintptr_t>(end), generation);
        if (gen && wholeRun) static_cast<void>(Gen::Note(Gen::Event::ImportWindow, begin, end, Gen::Data | Gen::Cpu, generation));
        for (auto page = begin; page < end; page += WritePageBytes) {
            const bool kept = held[index++] != 0;
            const auto pageKind = kept ? runs.window->held : runs.kind;
            if (!kept) tracker.notePages(tracker.windowPages, page, page + WritePageBytes, generation);
            if (gen && !wholeRun) noteStampGen(pageKind, page, page + WritePageBytes, generation);
            tracker.stamp(tracker.blockOf(page), generation, pageKind);
        }
    }
}

// APS5_ARCHB_FIX_STABLE_FAILED_COLLECTS: the unreported and compared pages noteRefusedWalk met on
// this thread (a failed walk that met none has no side effect and may be remembered).
std::uint64_t& refusedWalkPagesThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<std::uint64_t, Slot>();
}

#ifdef _WIN32
std::atomic<FreshCollectHook> freshCollectHook{nullptr};

bool collectWindowsWritesWalk(StampRuns& runs, std::uint64_t first, std::uint64_t stop, bool armedOnly, bool memoVirtualQuery);

bool collectWindowsWrites(StampRuns& runs, std::uint64_t first, std::uint64_t stop, bool armedOnly, bool memoVirtualQuery = false) {
    census::noteWalk(first, stop);
    const bool collected = collectWindowsWritesWalk(runs, first, stop, armedOnly, memoVirtualQuery);
    if (collected && !armedOnly && (runs.kind == StampKind::Cpu || runs.kind == StampKind::Driver)) census::noteWalked(first, stop);
    return collected;
}

bool collectWindowsWritesWalk(StampRuns& runs, std::uint64_t first, std::uint64_t stop, bool armedOnly, bool memoVirtualQuery) {
    auto& tracker = runs.tracker;
    const bool fresh = runs.kind == StampKind::Fresh;
    if (fresh) {
        if (const auto hook = freshCollectHook.load(std::memory_order_acquire)) return hook(static_cast<std::uintptr_t>(first), static_cast<std::size_t>(stop - first), &stampWrittenRun, &runs);
    }
    const auto collect = fresh ? &GuestArena::GuestArenaCollectFreshWrites_nid_postfix : armedOnly ? &GuestArena::GuestArenaCollectArmedWrites_nid_postfix : &GuestArena::GuestArenaCollectWrites_nid_postfix;
    static const bool verifyMemo = [] {
        const auto* value = std::getenv("APS5_VERIFY_VQ_MEMO");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    const GuestArena::QueryMemoContext memo{&UnwatchSerial, &ForgetSerial, verifyMemo, [](const char* name) { AgcDriver::ArchB::Recorder::Count(name); }};
    static const bool singlePass = std::getenv("APS5_NO_SINGLE_PASS_COLLECT") == nullptr;
    const auto walk = [&](bool clear) {
        auto cursor = first;
        while (cursor < stop) {
            std::size_t count = tracker.pages.size();
            const bool collected = memoVirtualQuery && !fresh ?
                GuestArena::GuestArenaCollectWritesMemo_nid_no_patch(cursor, static_cast<std::size_t>(stop - cursor), tracker.pages.data(), &count, clear, armedOnly, &memo) :
                collect(cursor, static_cast<std::size_t>(stop - cursor), tracker.pages.data(), &count, clear);
            if (!collected) return false;
            for (std::size_t i = 0; i < count; ++i) {
                const auto page = reinterpret_cast<std::uintptr_t>(tracker.pages[i]);
                stampWrittenRun(&runs, page, page + WritePageBytes);
            }
            if (count < tracker.pages.size()) break;
            cursor = reinterpret_cast<std::uintptr_t>(tracker.pages[count - 1]) + WritePageBytes;
        }
        return true;
    };
    if (!walk(singlePass || fresh)) return false;
    if (runs.stamped != 0) collectDirty.fetch_add(1, std::memory_order_relaxed);
    return fresh || singlePass || runs.stamped == 0 || walk(true);
}

void noteRefusedWalk(WriteTracker& tracker, std::uint64_t first, std::uint64_t stop) {
    const auto pagesOf = [&](auto list, auto&& each) {
        auto cursor = first;
        while (cursor < stop) {
            std::size_t count = tracker.pages.size();
            list(static_cast<std::uintptr_t>(cursor), static_cast<std::size_t>(stop - cursor), tracker.pages.data(), &count);
            for (std::size_t i = 0; i < count; ++i) each(reinterpret_cast<std::uintptr_t>(tracker.pages[i]));
            if (count < tracker.pages.size()) break;
            cursor = reinterpret_cast<std::uintptr_t>(tracker.pages[count - 1]) + WritePageBytes;
        }
    };
    StampRuns runs{tracker, StampKind::Cpu};
    pagesOf(&GuestArena::GuestArenaTakeUnreportedPages_nid_no_patch, [&](std::uintptr_t page) {
        ++refusedWalkPagesThreadState();
        stampWrittenRun(&runs, page, page + WritePageBytes);
    });
    const auto generation = tracker.generation.load(std::memory_order_relaxed);
    const bool gen = Gen::Active();
    pagesOf(&GuestArena::GuestArenaComparedPages_nid_no_patch, [&](std::uintptr_t page) {
        ++refusedWalkPagesThreadState();
        // Compared-when-shared pages: the block reads Unknown(compared) until a walk tracks it again,
        // and its refusal is a per-block table field (lock-free answers), stored before the map's.
        if (gen) {
            Gen::Global().Refuse(page, page + WritePageBytes, generation);
            static_cast<void>(Gen::NoteState(Gen::Event::Refused, page, page + WritePageBytes, Gen::BlockState::Compared, Gen::Meta, generation));
        }
        tracker.refused[tracker.blockOf(page)] = generation;
        if (const auto observer = stampObserver.load(std::memory_order_acquire)) observer(page, page + WritePageBytes, TrackerEvent::Stamp);
    });
}
#endif

#ifdef _WIN32
// APS5_ARCHB_WT_FAULT_CONSUMERS: a walk from the fault states instead of write watching over the whole
// range. Pages armed no later than their last sync hold no store (it would have faulted) and cost no OS
// call; every other page (open now, or armed after its last sync) gets a resetting write-watch walk of
// its run, so the answer is exactly what a whole-range walk gives. The flip walks the runs it armed, so
// in steady state only open pages are walked here. False
// (nothing stamped) when a page is untracked or excluded or the lists overflow: the caller walks the
// whole range. With VERIFY the whole range is walked as well, and every dirty page write watching
// reports must be among the pages this walk stamped or walked (wt_fault_consumer_mismatches).
bool faultSync(StampRuns& runs, std::uint64_t first, std::uint64_t stop, std::uint64_t walkClock) {
    auto& c = wtFault::counters();
    static std::vector<void*> dirty(4096);
    static std::vector<std::uintptr_t> open(2 * 1024);
    std::size_t dirtyCount = dirty.size();
    std::size_t openCount = open.size() / 2;
    if (!GuestArena::GuestWtFaultSync_nid_no_patch(static_cast<std::uintptr_t>(first), static_cast<std::uintptr_t>(stop), walkClock, dirty.data(), &dirtyCount, open.data(), &openCount)) {
        c.consumerFallback.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto savedTag = stampTrace::walkTagThreadState();
    stampTrace::walkTagThreadState() = 3;
    struct TagRestore {
        int saved;
        ~TagRestore() { stampTrace::walkTagThreadState() = saved; }
    } tagRestore{savedTag};
    for (std::size_t i = 0; i < openCount; ++i) {
        if (!collectWindowsWrites(runs, open[2 * i], open[2 * i + 1], false)) {
            c.consumerFallback.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    for (std::size_t i = 0; i < dirtyCount; ++i) {
        const auto page = reinterpret_cast<std::uintptr_t>(dirty[i]);
        stampWrittenRun(&runs, page, page + WritePageBytes);
    }
    (dirtyCount != 0 || runs.stamped != 0 ? c.consumerChanged : c.consumerUnchanged).fetch_add(1, std::memory_order_relaxed);
    static const bool syncCheck = [] {
        const char* text = std::getenv("APS5_WT_FAULT_SYNC_CHECK");
        return text != nullptr && text[0] == '1';
    }();
    if (syncCheck) {
        static std::vector<void*> written(4096);
        static std::atomic<std::uint64_t> checks{0}, misses{0};
        for (auto cursor = first; cursor < stop;) {
            ULONG_PTR count = written.size();
            DWORD granularity = 0;
            if (GetWriteWatch(0, reinterpret_cast<void*>(cursor), static_cast<SIZE_T>(stop - cursor), written.data(), &count, &granularity) != 0) break;
            for (ULONG_PTR i = 0; i < count; ++i) {
                const auto page = reinterpret_cast<std::uintptr_t>(written[i]);
                if (!GuestArena::GuestWtFaultSyncedClean_nid_no_patch(page)) continue;
                const auto missed = misses.fetch_add(1, std::memory_order_relaxed);
                if (missed < 32) {
                    const auto word = GuestArena::GuestWtFaultWord_nid_no_patch(page);
                    std::fprintf(stderr, "[wt-fault] SYNC MISS: page 0x%llx is dirty but armed no later than its last sync; stamp %llu walk clock %llu range 0x%llx..0x%llx\n", static_cast<unsigned long long>(page),
                                 static_cast<unsigned long long>(word >> GuestArena::WtFaultStampShift), static_cast<unsigned long long>(walkClock), static_cast<unsigned long long>(first), static_cast<unsigned long long>(stop));
                }
            }
            if (count < written.size()) break;
            cursor = reinterpret_cast<std::uintptr_t>(written[count - 1]) + WritePageBytes;
        }
        if (checks.fetch_add(1, std::memory_order_relaxed) % 20000 == 19999) std::fprintf(stderr, "[wt-fault] sync check: %llu syncs checked, %llu misses\n", static_cast<unsigned long long>(checks.load()), static_cast<unsigned long long>(misses.load()));
    }
    if (wtFault::verifyOn()) {
        std::vector<void*> reported;
        StampRuns check{runs.tracker, runs.kind, 0, nullptr, {}, &reported};
        if (collectWindowsWrites(check, first, stop, false)) {
            c.consumerVerified.fetch_add(1, std::memory_order_relaxed);
            for (const auto* pointer : reported) {
                const auto page = reinterpret_cast<std::uintptr_t>(pointer);
                const bool synced = std::find(dirty.begin(), dirty.begin() + static_cast<std::ptrdiff_t>(dirtyCount), pointer) != dirty.begin() + static_cast<std::ptrdiff_t>(dirtyCount);
                bool walked = false;
                for (std::size_t i = 0; i < openCount && !walked; ++i) walked = page >= open[2 * i] && page < open[2 * i + 1];
                if (synced || walked) continue;
                if (c.consumerMismatches.fetch_add(1, std::memory_order_relaxed) < 32) {
                    const auto word = GuestArena::GuestWtFaultWord_nid_no_patch(page);
                    std::fprintf(stderr, "[wt-fault] CONSUMER MISMATCH: page 0x%llx written (write watching) but neither synced nor open; state %llu stamp %llu walk clock %llu range 0x%llx..0x%llx\n",
                                 static_cast<unsigned long long>(page), static_cast<unsigned long long>(word & GuestArena::WtFaultStateMask), static_cast<unsigned long long>(word >> GuestArena::WtFaultStampShift),
                                 static_cast<unsigned long long>(walkClock), static_cast<unsigned long long>(first), static_cast<unsigned long long>(stop));
                }
            }
        }
    }
    return true;
}

// APS5_ARCHB_WT_SYNC_SKIP: the clock of each range's last complete CPU walk (exact ranges, under the
// tracker mutex). A range whose every page is still armed with a stamp at most that clock had no store
// since (any store faults and opens its page), so its next consumer walk is skipped: no bump, no
// stamps, no sync. APS5_ARCHB_WT_SYNC_SKIP_VERIFY=1 walks anyway and counts walks that found writes.
namespace syncSkip {
struct Entry {
    std::uint64_t first = 0;
    std::uint64_t stop = 0;
    std::uint64_t clock = 0;
    std::uint64_t unwatched = 0;
    std::uint64_t mapping = 0;
};

struct Table {
    static constexpr std::size_t Entries = 8192;
    std::array<Entry, Entries> entries{};
    Entry& at(std::uint64_t first, std::uint64_t stop) {
        auto key = (first >> 12) * 0x9e3779b97f4a7c15ull ^ (stop >> 12) * 0xc2b2ae3d27d4eb4full;
        key ^= key >> 29;
        return entries[static_cast<std::size_t>(key % Entries)];
    }
};

Table& table() {
    static auto* made = new Table();
    return *made;
}

struct Counters {
    std::atomic<std::uint64_t> skips{0}, refused{0}, recorded{0}, verified{0}, mismatches{0}, verifiedNs{0};
};

Counters& counters() {
    static auto* made = [] {
        auto* c = new Counters();
        const std::pair<const char*, std::atomic<std::uint64_t>*> named[] = {
            {"wt_sync_skips", &c->skips}, {"wt_sync_skip_refused", &c->refused}, {"wt_sync_skip_recorded", &c->recorded},
            {"wt_sync_skip_verified", &c->verified}, {"wt_sync_skip_mismatches", &c->mismatches}, {"wt_sync_skip_verified_ns", &c->verifiedNs}};
        for (const auto& [name, counter] : named) ArchB::Recorder::RegisterCounter(name, *counter);
        return c;
    }();
    return *made;
}

bool active() {
    return ArchB::Switches::Enabled(ArchB::Feature::WtSyncSkip);
}

bool verify() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_WT_SYNC_SKIP_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

std::uint64_t mapping() {
    return GuestAllocations::GuestAllocationsGeneration_nid_postfix();
}

void record(std::uint64_t first, std::uint64_t stop, std::uint64_t clock, std::uint64_t unwatched, std::uint64_t mappingSerial) {
    table().at(first, stop) = {first, stop, clock, unwatched, mappingSerial};
    counters().recorded.fetch_add(1, std::memory_order_relaxed);
}
}
#endif

bool walkWrites(WriteTracker& tracker, std::uint64_t first, std::uint64_t stop, StampKind kind, bool armedOnly = false, const WindowCopy* window = nullptr, bool memoVirtualQuery = false, std::uint64_t* stamped = nullptr, bool osWalk = false) {
    struct CallerScope {
        const void* saved = stampTrace::walkCallerThreadState();
        explicit CallerScope(const void* caller) {
            if (saved == nullptr) stampTrace::walkCallerThreadState() = caller;
        }
        ~CallerScope() { stampTrace::walkCallerThreadState() = saved; }
    } callerScope{__builtin_return_address(0)};
#ifdef _WIN32
    const bool skipCandidate = tracker.faultMode && !armedOnly && kind == StampKind::Cpu && !osWalk && window == nullptr && !Gen::Active() && !wtFault::verifyOn() && wtFault::consumersOn() && syncSkip::active();
    const auto skipUnwatched = skipCandidate ? unwatchSerial.load(std::memory_order_acquire) : 0;
    const auto skipMapping = skipCandidate ? syncSkip::mapping() : 0;
    bool skipVerifying = false;
    if (skipCandidate) {
        const auto& entry = syncSkip::table().at(first, stop);
        if (entry.first == first && entry.stop == stop && entry.unwatched == skipUnwatched && entry.mapping == skipMapping) {
            if (GuestArena::GuestWtFaultChangedSince_nid_no_patch(static_cast<std::uintptr_t>(first), static_cast<std::size_t>(stop - first), entry.clock) == GuestArena::WtFaultAnswer::Unchanged) {
                if (!syncSkip::verify()) {
                    tracker.bump();
                    census::note(census::OutcomeSkip);
                    syncSkip::counters().skips.fetch_add(1, std::memory_order_relaxed);
                    if (stamped != nullptr) *stamped = 0;
                    return true;
                }
                skipVerifying = true;
            } else {
                syncSkip::counters().refused.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    const auto skipStart = skipVerifying ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    struct SkipVerify {
        bool on;
        std::chrono::steady_clock::time_point start;
        std::uint64_t found = 0;
        bool completed = false;
        ~SkipVerify() {
            if (!on) return;
            auto& c = syncSkip::counters();
            c.verified.fetch_add(1, std::memory_order_relaxed);
            c.verifiedNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
            if (!completed || found != 0) c.mismatches.fetch_add(1, std::memory_order_relaxed);
        }
    } skipVerify{skipVerifying, skipStart};
#endif
    tracker.bump();
    // APS5_ARCHB_WT_FAULT: the walked pages become candidates of the fault tracker (the next flip
    // protects them); VERIFY checks the walk's dirty pages against the fault states. The walk's clock
    // is the generation just bumped, read before the OS query resets any dirty bit.
    const bool faultWalk = tracker.faultMode && !armedOnly && (kind == StampKind::Cpu || kind == StampKind::Driver);
    const auto walkClock = tracker.generation.load(std::memory_order_relaxed);
    if (faultWalk) GuestArena::GuestWtFaultTrack_nid_no_patch(static_cast<std::uintptr_t>(first), static_cast<std::size_t>(stop - first));
    std::vector<void*>* dirty = faultWalk && wtFault::verifyOn() ? &wtFault::dirtyScratch() : nullptr;
    if (dirty != nullptr) dirty->clear();
    StampRuns runs{tracker, kind, 0, window, {}, dirty};
    struct ReportStamped {
        const StampRuns& runs;
        std::uint64_t* stamped;
        ~ReportStamped() {
            if (stamped != nullptr) *stamped = runs.stamped;
        }
    } reportStamped{runs, stamped};
    // Write generations: a complete resetting walk covers its pages at the ticket read before its
    // OS query (rule V2); a failed one leaves its blocks unknown until a later walk succeeds (no
    // meta bump: the dirty bits it did not reset are reported by that walk).
    const bool gen = Gen::Active();
    const bool sweep = gen && !armedOnly && (kind == StampKind::Cpu || kind == StampKind::Driver);
    const auto ticket = sweep ? Gen::Global().LatestTicket() : 0;
    const auto failed = [&] {
        if (!gen) return;
        static_cast<void>(Gen::Global().SetState(first, stop, Gen::BlockState::Uncommitted, false));
        static_cast<void>(Gen::Note(Gen::Event::FailedWalk, first, stop, 0, tracker.generation.load(std::memory_order_relaxed), false));
    };
#ifdef _WIN32
    if (faultWalk && !osWalk && window == nullptr && wtFault::consumersOn()) {
        runs.dirty = nullptr;
        if (faultSync(runs, first, stop, walkClock)) {
            if (skipCandidate) syncSkip::record(first, stop, walkClock, skipUnwatched, skipMapping);
            skipVerify.completed = true;
            skipVerify.found = runs.stamped;
            return true;
        }
        runs.dirty = dirty;
    }
    const bool complete = collectWindowsWrites(runs, first, stop, armedOnly, memoVirtualQuery);
    if (window != nullptr) stampWindowRuns(runs);
    if (complete) {
        if (skipCandidate) syncSkip::record(first, stop, walkClock, skipUnwatched, skipMapping);
        skipVerify.completed = true;
        skipVerify.found = runs.stamped;
        if (sweep) Gen::NoteSweep(first, stop, ticket);
        // The walk consumed every store made before its query: its pages take the walk's clock as their
        // sync clock (CONSUMERS) and VERIFY checks its dirty pages against the fault states.
        if (dirty != nullptr || (faultWalk && wtFault::consumersOn())) {
            GuestArena::WtFaultMismatch mismatch{};
            const auto mismatches = GuestArena::GuestWtFaultVerifyWalk_nid_no_patch(static_cast<std::uintptr_t>(first), static_cast<std::uintptr_t>(stop), dirty != nullptr ? dirty->data() : nullptr, dirty != nullptr ? dirty->size() : 0, walkClock, &mismatch);
            if (mismatches != 0) wtFault::reportMismatches(first, stop, mismatches, mismatch, walkClock);
        }
        return true;
    }
    failed();
    noteRefusedWalk(tracker, first, stop);
    return false;
#else
    static_cast<void>(memoVirtualQuery);
    const auto dirtyRuns = collectDirtyRuns.load(std::memory_order_relaxed);
    const auto collect = armedOnly ? &GuestWriteWatch::GuestWriteWatchCollectArmed_nid_postfix : &GuestWriteWatch::GuestWriteWatchCollect_nid_postfix;
    const bool complete = collect(static_cast<std::uintptr_t>(first), static_cast<std::size_t>(stop - first), &stampWrittenRun, &runs);
    if (collectDirtyRuns.load(std::memory_order_relaxed) != dirtyRuns) collectDirty.fetch_add(1, std::memory_order_relaxed);
    if (window != nullptr) stampWindowRuns(runs);
    if (complete && sweep) Gen::NoteSweep(first, stop, ticket);
    else if (!complete) failed();
    return complete;
#endif
}

// APS5_ARCHB_FIX_WRITE_TRACKING_BOOKKEEPING: the ranges this thread collected in its current epoch, by
// exact page-rounded range, in a direct-mapped table reached through a TEB slot. An entry is made
// only where the original memo answers for that range (a completed walk of it, or a hit in the epoch
// maps or the ring), with the epoch and the unwatch and mapping serials that answer was given for,
// so a repeat that matches all of them is the memo hit the maps would find, without their lookups
// (the epoch map's thread-local and probe, the ring's 64 compares). An epoch boundary (a submission,
// a CPU-satisfiable wait, a drain or a reap) gives the thread a new epoch, so no entry made before
// it ever matches again: the next collect walks and takes fresh evidence.
struct CollectHitCache {
    static constexpr std::size_t Entries = 512;
    std::array<WriteTracker::Memo, Entries> entries{};

    static std::size_t index(std::uint64_t first, std::uint64_t stop) {
        auto key = (first >> 12) * 0x9e3779b97f4a7c15ull ^ (stop >> 12) * 0xc2b2ae3d27d4eb4full;
        key ^= key >> 29;
        return static_cast<std::size_t>(key % Entries);
    }

    WriteTracker::Memo& at(std::uint64_t first, std::uint64_t stop) {
        return entries[index(first, stop)];
    }
};

CollectHitCache& collectHitCache() {
    struct Slot {};
    auto* cache = HostThreadSlot<CollectHitCache*, Slot>::Get();
    if (cache == nullptr) {
        ShaderRecompiler::ThreadOwned(cache);
        HostThreadSlot<CollectHitCache*, Slot>::Set(cache);
    }
    return *cache;
}

struct CommitterThreadSlot {};
bool committerThread() { return HostThreadSlot<bool, CommitterThreadSlot>::Get(); }
void committerThread(bool value) { HostThreadSlot<bool, CommitterThreadSlot>::Set(value); }
// Stage 4.2 (APS5_ARCHB_PIPE_RESOLVE_POOL): a draw resolver thread. Its walks (made after the item's
// ordering point, like queue 0's) go into the collect-share table, which the committer then reads
// even with PIPE_COLLECT_SHARE off; APS5_PIPE_RESOLVE_SHARE=0 leaves both out.
bool& resolverThreadThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<bool, Slot>();
}

bool resolveShareActive() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_PIPE_RESOLVE_SHARE");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled && ArchB::Switches::Enabled(ArchB::Feature::PipeResolvePool) && ArchB::Switches::Enabled(ArchB::Feature::PipeResolveSplit);
}

// APS5_ARCHB_PIPE_COLLECT_SHARE (see GuestMemory.hpp): queue 0's completed walks, per page, with the
// collect clock (nextCollectEpoch) each walk read before its OS query, under the tracker mutex. A
// page holding clock c was walked after every epoch bump numbered <= c, so it covers an item whose
// ordering point is such a bump. Each page also carries the table generation it was recorded in: a
// walk under newer unwatch or mapping serials starts a new generation, retiring every older entry
// (the clocks of older generations are kept in the top 16 bits' place, never compared).
namespace collectShare {
bool& producerThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<bool, Slot>();
}
constexpr std::uint64_t Unknown = std::numeric_limits<std::uint64_t>::max();
// The committer's requirement: the ordering point of the item it commits (Unknown: nothing shared),
// raised by its own ordering points (floor).
std::uint64_t& requiredThreadState() {
    struct State { std::uint64_t value = Unknown; };
    struct Slot {};
    return AgcDriver::ThreadScratch<State, Slot>().value;
}
std::uint64_t& floorThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<std::uint64_t, Slot>();
}
std::uint64_t& memoHitsThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<std::uint64_t, Slot>();
}

bool active() {
    return ArchB::Switches::Enabled(ArchB::Feature::PipeCollectShare);
}

bool verify() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_PIPE_COLLECT_SHARE_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

#ifdef _WIN32
struct Table {
    static constexpr std::size_t ChunkPages = 16384;
    static constexpr unsigned GenerationShift = 48;
    static constexpr std::uint64_t ClockMask = (std::uint64_t{1} << GenerationShift) - 1;
    std::vector<std::unique_ptr<std::uint64_t[]>> chunks;
    std::uintptr_t base = 0;
    bool initialized = false;
    bool serialsKnown = false;
    std::uint64_t unwatched = 0;
    std::uint64_t mapping = 0;
    std::uint64_t generation = 0;
};

Table& table() {
    static auto* shared = new Table();
    return *shared;
}

// Under the tracker mutex, with the tracker initialized and watched and [first, stop) inside it.
void record(const WriteTracker& tracker, std::uint64_t first, std::uint64_t stop, std::uint64_t clock, std::uint64_t unwatched, std::uint64_t mapping) {
    auto& shared = table();
    if (!shared.initialized) {
        shared.initialized = true;
        shared.base = tracker.base;
        shared.chunks.resize(tracker.size / (Table::ChunkPages * WritePageBytes) + 1);
    }
    if (!shared.serialsKnown || unwatched != shared.unwatched || mapping != shared.mapping) {
        // Serials only grow: a walk that read older ones is not recorded at all.
        if (shared.serialsKnown && (unwatched < shared.unwatched || mapping < shared.mapping)) return;
        // A new generation retires every entry; this walk (made under the new serials) is the first.
        shared.serialsKnown = true;
        shared.unwatched = unwatched;
        shared.mapping = mapping;
        shared.generation = (shared.generation + 1) & 0xffffu;
        CommitterCollects().shareResets.fetch_add(1, std::memory_order_relaxed);
    }
    if (clock > Table::ClockMask) return;
    const auto value = shared.generation << Table::GenerationShift | clock;
    for (auto page = (first - shared.base) / WritePageBytes, last = (stop - shared.base) / WritePageBytes; page < last;) {
        auto& chunk = shared.chunks[page / Table::ChunkPages];
        if (chunk == nullptr) chunk = std::make_unique<std::uint64_t[]>(Table::ChunkPages);
        const auto end = std::min<std::uint64_t>(last, (page / Table::ChunkPages + 1) * Table::ChunkPages);
        std::fill(chunk.get() + page % Table::ChunkPages, chunk.get() + page % Table::ChunkPages + (end - page), value);
        page = end;
    }
    CommitterCollects().shareRecorded.fetch_add(1, std::memory_order_relaxed);
    CommitterCollects().shareRecordedPages.fetch_add((stop - first) / WritePageBytes, std::memory_order_relaxed);
}

// Pages of [first, stop) walked at or after the committer's requirement under the current serials;
// stops at the first uncovered page unless every page is to be counted.
std::uint64_t covered(std::uint64_t first, std::uint64_t stop, std::uint64_t unwatched, std::uint64_t mapping, bool countAll) {
    auto& shared = table();
    if (requiredThreadState() == Unknown || requiredThreadState() > Table::ClockMask || !shared.serialsKnown || unwatched != shared.unwatched || mapping != shared.mapping || first < shared.base) return 0;
    // Same generation and a clock at or above the requirement: one compare against a packed bound.
    const auto low = shared.generation << Table::GenerationShift | requiredThreadState();
    const auto high = shared.generation << Table::GenerationShift | Table::ClockMask;
    std::uint64_t pages = 0;
    for (auto page = (first - shared.base) / WritePageBytes, last = (stop - shared.base) / WritePageBytes; page < last; ++page) {
        const auto chunk = page / Table::ChunkPages;
        const auto value = chunk < shared.chunks.size() && shared.chunks[chunk] != nullptr ? shared.chunks[chunk][page % Table::ChunkPages] : 0;
        const bool hit = value >= low && value <= high;
        if (hit) ++pages;
        else if (!countAll) return pages;
    }
    return pages;
}
#endif
}

// APS5_COLLECT_AHEAD_CENSUS (see GuestMemory.hpp).
namespace aheadCensus {
thread_local bool prelock = false;
thread_local CommitterItemCollects item{};

bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_COLLECT_AHEAD_CENSUS");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return on;
}

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One memoized committer collect: its time (memo hits included), and its walk if it made one.
struct Scope {
    bool on;
    std::uint64_t start = 0;
    explicit Scope(bool active) : on(active) {
        if (on) start = nowNs();
    }
    void walked(std::uint64_t pages, std::uint64_t ns) const {
        if (!on) return;
        auto& counts = CollectAheadCensusCounts();
        if (prelock) {
            counts.prelockWalks.fetch_add(1, std::memory_order_relaxed);
            counts.prelockWalkPages.fetch_add(pages, std::memory_order_relaxed);
            counts.prelockWalkNs.fetch_add(ns, std::memory_order_relaxed);
            ++item.walks;
            item.walkNs += ns;
        } else {
            counts.otherWalks.fetch_add(1, std::memory_order_relaxed);
            counts.otherWalkPages.fetch_add(pages, std::memory_order_relaxed);
            counts.otherWalkNs.fetch_add(ns, std::memory_order_relaxed);
        }
    }
    ~Scope() {
        if (!on) return;
        const auto ns = nowNs() - start;
        auto& counts = CollectAheadCensusCounts();
        (prelock ? counts.prelockCalls : counts.otherCalls).fetch_add(1, std::memory_order_relaxed);
        (prelock ? counts.prelockNs : counts.otherNs).fetch_add(ns, std::memory_order_relaxed);
    }
};
}

// APS5_ARCHB_COMMITTER_COLLECT_CENSUS (see GuestMemory.hpp). Committer thread only: its tables are
// thread-local and printed by the committer itself.
namespace committerCensus {
std::atomic<std::uint64_t> flips{0};

bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_ARCHB_COMMITTER_COLLECT_CENSUS");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return on;
}

enum Outcome : std::uint8_t { OutcomeNone, OutcomeHitCache, OutcomeHitMap, OutcomeHitRing, OutcomeHitShare, OutcomeFailedHit, OutcomeWalk, OutcomeFailed };
// A range against the ranges collected earlier in a scope (epoch or item).
enum Seen : std::uint8_t { SeenNo, SeenPartial, SeenSubset, SeenExact, SeenKinds };

struct SiteKey {
    const char* file;
    std::uint32_t line;
    std::uint8_t kind;
    bool prelock;
    bool memoized;
    bool operator<(const SiteKey& other) const {
        return std::tie(file, line, kind, prelock, memoized) < std::tie(other.file, other.line, other.kind, other.prelock, other.memoized);
    }
};

struct SiteRow {
    std::uint64_t calls = 0;
    std::uint64_t ns = 0;
    std::uint64_t hits = 0;
    std::uint64_t hitNs = 0;
    std::array<std::uint64_t, 4> hitPaths{};  // cache, epoch map, ring, share
    std::uint64_t failedHits = 0;
    std::uint64_t failed = 0;
    std::uint64_t walks = 0;
    std::uint64_t walkPages = 0;
    std::uint64_t walkNs = 0;
    std::uint64_t walkCallNs = 0;
    // Walks: the range against this epoch's / this item's earlier collected ranges.
    std::array<std::uint64_t, SeenKinds> walkEpoch{};
    std::array<std::uint64_t, SeenKinds> walkItem{};
    std::uint64_t walkEpochSeenPages = 0;  // pages of walks already collected in the epoch
    std::uint64_t walkEpochSeenNs = 0;     // walk time of walks that were exact or subset in the epoch
    std::uint64_t walkItemSeenNs = 0;      // ... in the item
    // Memo hits: the range against this item's earlier ranges, and (when not in the item) the epoch's.
    std::array<std::uint64_t, SeenKinds> hitItem{};
    std::array<std::uint64_t, SeenKinds> hitEpoch{};
    std::uint64_t hitItemSeenNs = 0;  // call time of hits that were exact or subset in the item
};

struct ChainKey {
    const char* file;
    std::uint32_t line;
    std::array<unsigned long long, 6> frames;
    bool operator<(const ChainKey& other) const {
        return std::tie(file, line, frames) < std::tie(other.file, other.line, other.frames);
    }
};

struct ChainRow {
    std::uint64_t walks = 0;
    std::uint64_t pages = 0;
    std::uint64_t ns = 0;
    std::uint64_t epochSeen = 0;  // walks exact or subset in the epoch
    std::uint64_t itemSeen = 0;
    bool prelock = false;
    std::uint8_t kind = 0;
};

// The ranges collected in one scope: exact pairs and their union.
struct Scope {
    std::uint64_t serial = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> exact;
    std::map<std::uint64_t, std::uint64_t> merged;

    void reset(std::uint64_t value) {
        serial = value;
        exact.clear();
        merged.clear();
    }
    // How [first, stop) relates to the ranges so far, and the pages of it already collected.
    Seen classify(std::uint64_t first, std::uint64_t stop, std::uint64_t& seenPages) const {
        seenPages = 0;
        auto it = merged.upper_bound(first);
        if (it != merged.begin()) --it;
        for (; it != merged.end() && it->first < stop; ++it) {
            const auto from = std::max(first, it->first);
            const auto to = std::min(stop, it->second);
            if (from < to) seenPages += (to - from) / 4096;
        }
        const auto pages = (stop - first) / 4096;
        if (seenPages == 0) return SeenNo;
        if (seenPages < pages) return SeenPartial;
        // Exact pairs are looked up only for covered ranges (the common case is a short list per item).
        if (exact.size() < 8192 && std::find(exact.begin(), exact.end(), std::pair{first, stop}) != exact.end()) return SeenExact;
        return SeenSubset;
    }
    void add(std::uint64_t first, std::uint64_t stop) {
        if (exact.size() < 8192) exact.emplace_back(first, stop);
        auto begin = first;
        auto end = stop;
        auto merge = merged.lower_bound(begin);
        if (merge != merged.begin() && std::prev(merge)->second >= begin) --merge;
        while (merge != merged.end() && merge->first <= end) {
            begin = std::min(begin, merge->first);
            end = std::max(end, merge->second);
            merge = merged.erase(merge);
        }
        merged.emplace(begin, end);
    }
};

struct State {
    std::map<SiteKey, SiteRow> sites;
    std::map<ChainKey, ChainRow> chains;
    Scope epoch;
    Scope item;
    std::uint64_t itemSerial = 0;
    std::uint8_t itemKind = 0;
    std::uint64_t items[8]{};
    std::uint64_t calls = 0;
    std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
};

State& state() {
    struct SlotThreadStorage {};
    auto& slot = AgcDriver::ThreadScratch<State*, SlotThreadStorage>();
    if (slot == nullptr) slot = new State();
    return *slot;
}

void report(State& census) {
    const auto now = std::chrono::steady_clock::now();
    if (now - census.reported < std::chrono::seconds(10)) return;
    census.reported = now;
    static constexpr const char* kindNames[] = {"other", "draw", "dispatch", "label", "dma", "fill", "?", "?"};
    std::string text;
    char line[2048];
    std::snprintf(line, sizeof(line), "[collect-census] begin flips=%llu items other=%llu draw=%llu dispatch=%llu label=%llu dma=%llu fill=%llu\n", static_cast<unsigned long long>(flips.load(std::memory_order_relaxed)), static_cast<unsigned long long>(census.items[0]), static_cast<unsigned long long>(census.items[1]), static_cast<unsigned long long>(census.items[2]), static_cast<unsigned long long>(census.items[3]), static_cast<unsigned long long>(census.items[4]), static_cast<unsigned long long>(census.items[5]));
    text += line;
    for (const auto& [key, row] : census.sites) {
        const char* file = std::strrchr(key.file, '/');
        const char* back = std::strrchr(key.file, '\\');
        if (back != nullptr && (file == nullptr || back > file)) file = back;
        file = file != nullptr ? file + 1 : key.file;
        std::snprintf(line, sizeof(line),
                      "[collect-census] site %s:%u kind=%s phase=%s memo=%d calls=%llu ns=%llu hits=%llu hit_ns=%llu paths=%llu/%llu/%llu/%llu failed_hits=%llu failed=%llu walks=%llu pages=%llu walk_ns=%llu walk_call_ns=%llu "
                      "walk_epoch=%llu/%llu/%llu/%llu walk_item=%llu/%llu/%llu/%llu walk_epoch_seen_pages=%llu walk_epoch_seen_ns=%llu walk_item_seen_ns=%llu hit_item=%llu/%llu/%llu/%llu hit_epoch=%llu/%llu/%llu/%llu hit_item_seen_ns=%llu\n",
                      file, key.line, kindNames[key.kind & 7u], key.prelock ? "prelock" : "other", key.memoized ? 1 : 0, static_cast<unsigned long long>(row.calls), static_cast<unsigned long long>(row.ns), static_cast<unsigned long long>(row.hits), static_cast<unsigned long long>(row.hitNs),
                      static_cast<unsigned long long>(row.hitPaths[0]), static_cast<unsigned long long>(row.hitPaths[1]), static_cast<unsigned long long>(row.hitPaths[2]), static_cast<unsigned long long>(row.hitPaths[3]), static_cast<unsigned long long>(row.failedHits), static_cast<unsigned long long>(row.failed),
                      static_cast<unsigned long long>(row.walks), static_cast<unsigned long long>(row.walkPages), static_cast<unsigned long long>(row.walkNs), static_cast<unsigned long long>(row.walkCallNs),
                      static_cast<unsigned long long>(row.walkEpoch[0]), static_cast<unsigned long long>(row.walkEpoch[1]), static_cast<unsigned long long>(row.walkEpoch[2]), static_cast<unsigned long long>(row.walkEpoch[3]),
                      static_cast<unsigned long long>(row.walkItem[0]), static_cast<unsigned long long>(row.walkItem[1]), static_cast<unsigned long long>(row.walkItem[2]), static_cast<unsigned long long>(row.walkItem[3]),
                      static_cast<unsigned long long>(row.walkEpochSeenPages), static_cast<unsigned long long>(row.walkEpochSeenNs), static_cast<unsigned long long>(row.walkItemSeenNs),
                      static_cast<unsigned long long>(row.hitItem[0]), static_cast<unsigned long long>(row.hitItem[1]), static_cast<unsigned long long>(row.hitItem[2]), static_cast<unsigned long long>(row.hitItem[3]),
                      static_cast<unsigned long long>(row.hitEpoch[0]), static_cast<unsigned long long>(row.hitEpoch[1]), static_cast<unsigned long long>(row.hitEpoch[2]), static_cast<unsigned long long>(row.hitEpoch[3]), static_cast<unsigned long long>(row.hitItemSeenNs));
        text += line;
    }
    // The walk chains by walk time, top 48.
    std::vector<std::pair<const ChainKey*, const ChainRow*>> chains;
    chains.reserve(census.chains.size());
    for (const auto& [key, row] : census.chains) chains.emplace_back(&key, &row);
    std::sort(chains.begin(), chains.end(), [](const auto& a, const auto& b) { return a.second->ns > b.second->ns; });
    if (chains.size() > 48) chains.resize(48);
    for (const auto& [key, row] : chains) {
        const char* file = std::strrchr(key->file, '\\');
        if (const char* slash = std::strrchr(key->file, '/'); slash != nullptr && (file == nullptr || slash > file)) file = slash;
        file = file != nullptr ? file + 1 : key->file;
        std::snprintf(line, sizeof(line), "[collect-census] chain %s:%u kind=%s phase=%s walks=%llu pages=%llu ns=%llu epoch_seen=%llu item_seen=%llu frames=+%llx,+%llx,+%llx,+%llx,+%llx,+%llx\n", file, key->line, kindNames[row->kind & 7u], row->prelock ? "prelock" : "other",
                      static_cast<unsigned long long>(row->walks), static_cast<unsigned long long>(row->pages), static_cast<unsigned long long>(row->ns), static_cast<unsigned long long>(row->epochSeen), static_cast<unsigned long long>(row->itemSeen),
                      key->frames[0], key->frames[1], key->frames[2], key->frames[3], key->frames[4], key->frames[5]);
        text += line;
    }
    text += "[collect-census] end\n";
    std::fputs(text.c_str(), stderr);
    std::fflush(stderr);
}

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One committer collect.
class Call {
public:
    Call(bool active, const std::source_location& site, bool memoized) : on(active), site(site), memoized(memoized) {
        if (on) start = nowNs();
    }
    void range(std::uint64_t begin, std::uint64_t end, std::uint64_t epochValue) {
        first = begin;
        stop = end;
        epoch = epochValue;
    }
    void hit(Outcome path) {
        if (on) outcome = path;
    }
    void walked(bool ok, std::uint64_t ns) {
        if (!on) return;
        outcome = ok ? OutcomeWalk : OutcomeFailed;
        walkNs = ns;
        if (ok) chain = CaptureCallerOffsets(frames, 0);
    }
    void failed() {
        if (on && outcome == OutcomeNone) outcome = OutcomeFailed;
    }
    ~Call() {
        if (!on) return;
        const auto ns = nowNs() - start;
        auto& census = state();
        if (census.epoch.serial != epoch) census.epoch.reset(epoch);
        if (census.item.serial != census.itemSerial) census.item.reset(census.itemSerial);
        auto& row = census.sites[SiteKey{site.file_name(), site.line(), census.itemKind, aheadCensus::prelock, memoized}];
        ++row.calls;
        row.ns += ns;
        const bool ranged = stop > first;
        std::uint64_t epochPages = 0;
        std::uint64_t itemPages = 0;
        const auto inEpoch = ranged ? census.epoch.classify(first, stop, epochPages) : SeenNo;
        const auto inItem = ranged ? census.item.classify(first, stop, itemPages) : SeenNo;
        const auto seen = [](Seen value) { return value == SeenExact || value == SeenSubset; };
        switch (outcome) {
            case OutcomeHitCache:
            case OutcomeHitMap:
            case OutcomeHitRing:
            case OutcomeHitShare:
                ++row.hits;
                row.hitNs += ns;
                ++row.hitPaths[outcome - OutcomeHitCache];
                ++row.hitItem[inItem];
                if (!seen(inItem)) ++row.hitEpoch[inEpoch];
                else row.hitItemSeenNs += ns;
                break;
            case OutcomeFailedHit: ++row.failedHits; break;
            case OutcomeWalk: {
                ++row.walks;
                row.walkPages += (stop - first) / 4096;
                row.walkNs += walkNs;
                row.walkCallNs += ns;
                ++row.walkEpoch[inEpoch];
                ++row.walkItem[inItem];
                row.walkEpochSeenPages += epochPages;
                if (seen(inEpoch)) row.walkEpochSeenNs += walkNs;
                if (seen(inItem)) row.walkItemSeenNs += walkNs;
                ChainKey key{site.file_name(), site.line(), {}};
                for (std::size_t i = 0; i < chain && i < key.frames.size(); ++i) key.frames[i] = frames[i];
                auto& chainRow = census.chains[key];
                ++chainRow.walks;
                chainRow.pages += (stop - first) / 4096;
                chainRow.ns += walkNs;
                if (seen(inEpoch)) ++chainRow.epochSeen;
                if (seen(inItem)) ++chainRow.itemSeen;
                chainRow.prelock = aheadCensus::prelock;
                chainRow.kind = census.itemKind;
                break;
            }
            default: ++row.failed; break;
        }
        if (ranged && outcome != OutcomeFailed && outcome != OutcomeFailedHit && outcome != OutcomeNone) {
            census.epoch.add(first, stop);
            census.item.add(first, stop);
        }
        if ((++census.calls & 1023u) == 0) report(census);
    }
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;

    const bool on;

private:
    const std::source_location& site;
    bool memoized;
    std::uint64_t start = 0;
    std::uint64_t first = 0;
    std::uint64_t stop = 0;
    std::uint64_t epoch = 0;
    std::uint64_t walkNs = 0;
    Outcome outcome = OutcomeNone;
    std::array<unsigned long long, 6> frames{};
    std::size_t chain = 0;
};
}

// APS5_ARCHB_FIX_STABLE_FAILED_COLLECTS (see GuestMemory.hpp): this thread's remembered failures,
// direct-mapped by exact page-rounded range, each with the serials read before its walk.
namespace stableFailed {
bool active() {
    return ArchB::Switches::Enabled(ArchB::Feature::FixStableFailedCollects);
}

bool verify() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_FIX_STABLE_FAILED_COLLECTS_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

struct Serials {
    std::uint64_t unwatched = 0;
    std::uint64_t mapping = 0;
    std::uint64_t committed = 0;
    std::uint64_t protection = 0;
    std::uint64_t forgotten = 0;
    bool operator==(const Serials&) const = default;
    // Page states are in flux (a protection change or ForgetPages under way): nothing is remembered.
    bool settled() const {
        return protection != 0 && (forgotten & 1u) == 0;
    }
};

Serials read(std::uint64_t unwatched, std::uint64_t mapping) {
    Serials serials{unwatched, mapping};
#ifdef _WIN32
    serials.committed = GuestArena::GuestArenaCommitGeneration_nid_postfix();
    serials.protection = GuestArena::GuestArenaQueryProtectionSerial_nid_no_patch();
#else
    serials.protection = 0;
#endif
    serials.forgotten = ForgetSerial();
    return serials;
}

struct Entry {
    std::uint64_t first = 0;
    std::uint64_t stop = 0;
    Serials serials{};
};

// APS5_ARCHB_FIX_STABLE_FAILED_WIDE (sampled per lookup): 8192 slots instead of 1024. A live census
// (records/b1b2-count-1103-0754, APS5_STABLE_FAILED_DIAG) saw ~125 inserts/f with no page-state serial
// moving at all: the re-inserts are ranges that lost their slot to another range. An entry carries its
// range, so a lookup through the other width simply misses.
struct Table {
    static constexpr std::size_t Entries = 1024;
    static constexpr std::size_t WideEntries = 8192;
    std::array<Entry, WideEntries> entries{};
    Entry& at(std::uint64_t first, std::uint64_t stop) {
        auto key = (first >> 12) * 0x9e3779b97f4a7c15ull ^ (stop >> 12) * 0xc2b2ae3d27d4eb4full;
        key ^= key >> 29;
        const auto slots = ArchB::Switches::Enabled(ArchB::Feature::FixStableFailedWide) ? WideEntries : Entries;
        return entries[static_cast<std::size_t>(key % slots)];
    }
};

Table& table() {
    struct Slot {};
    auto* owned = HostThreadSlot<Table*, Slot>::Get();
    if (owned == nullptr) {
        ShaderRecompiler::ThreadOwned(owned);
        HostThreadSlot<Table*, Slot>::Set(owned);
    }
    return *owned;
}

// APS5_STABLE_FAILED_DIAG (diagnostic, read once): why a settled lookup missed the table: an empty
// slot, a slot holding another range, or this range under serials that moved (by serial; several
// can move at once), or the serials were not settled.
bool diagEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_STABLE_FAILED_DIAG");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

struct DiagCounters {
    std::atomic<std::uint64_t> empty{0}, otherRange{0}, unwatched{0}, mapping{0}, committed{0}, protection{0}, forgotten{0}, unsettled{0};
};

DiagCounters& diagCounters() {
    static auto* made = [] {
        auto* counts = new DiagCounters();
        ArchB::Recorder::RegisterCounter("fix_stable_failed_diag_empty", counts->empty);
        ArchB::Recorder::RegisterCounter("fix_stable_failed_diag_other_range", counts->otherRange);
        ArchB::Recorder::RegisterCounter("fix_stable_failed_diag_unwatched", counts->unwatched);
        ArchB::Recorder::RegisterCounter("fix_stable_failed_diag_mapping", counts->mapping);
        ArchB::Recorder::RegisterCounter("fix_stable_failed_diag_committed", counts->committed);
        ArchB::Recorder::RegisterCounter("fix_stable_failed_diag_protection", counts->protection);
        ArchB::Recorder::RegisterCounter("fix_stable_failed_diag_forgotten", counts->forgotten);
        ArchB::Recorder::RegisterCounter("fix_stable_failed_diag_unsettled", counts->unsettled);
        return counts;
    }();
    return *made;
}

StableFailedCollectCounters& counters() {
    static auto* made = [] {
        auto* counts = new StableFailedCollectCounters();
        ArchB::Recorder::RegisterCounter("fix_stable_failed_collects_hits", counts->hits);
        ArchB::Recorder::RegisterCounter("fix_stable_failed_collects_inserts", counts->inserts);
        ArchB::Recorder::RegisterCounter("fix_stable_failed_collects_refused", counts->refused);
        ArchB::Recorder::RegisterCounter("fix_stable_failed_collects_verified", counts->verified);
        ArchB::Recorder::RegisterCounter("fix_stable_failed_collects_mismatches", counts->mismatches);
        return counts;
    }();
    return *made;
}
}

std::uint64_t collectWrites(std::uint64_t address, std::size_t bytes, bool memoized, bool memoVirtualQuery, const std::source_location& site) {
    const census::Scope censusScope(memoized ? "collect" : "collect_uncached", &site);
    const Stage4::TrackerScope stage4Scope(false, address, bytes);
    const aheadCensus::Scope aheadScope(committerThread() && memoized && aheadCensus::enabled());
    committerCensus::Call committerCall(committerThread() && committerCensus::enabled(), site, memoized);
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return 0;
    auto& tracker = Tracker();
    // Whole pages, so a page shared with the next range is collected with either.
    constexpr std::uint64_t page = 4096;
    auto first = address & ~(page - 1);
    auto stop = (address + bytes + page - 1) & ~(page - 1);
    if (stop <= first) return 0;
    // One epoch for the lookup and the entry made after the walk: an unbumped thread's fresh epoch
    // must not differ between them.
    const auto epoch = currentCollectEpoch();
    const auto unwatched = unwatchSerial.load(std::memory_order_acquire);
    const auto mapping = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    const bool useMemo = memoized && collectMemoEnabled() && bytes != 0;
    if (committerCall.on) committerCall.range(first, stop, epoch);
    if (committerThread() && useMemo) CommitterCollects().collects.fetch_add(1, std::memory_order_relaxed);
    // Write generations: an uncached collect sweeps at a private fresh ticket (rule V4); a memoized
    // one is modeled under the shadow: what EnsureSwept would sweep at the thread's ticket, judged
    // before this call's own walk can cover anything.
    Gen::Coverage model{};
    bool modeled = false;
    if (!memoized && Gen::Active()) static_cast<void>(Gen::IssuePrivate(Gen::TicketReason::Uncached));
    if (memoized && Gen::ShadowActive()) {
        const auto required = Gen::Required();
        const bool unbumped = required == 0 || HostThreadSlot<std::uint64_t, CollectEpochSlot>::Get() == 0;
        model = Gen::Global().Model(first, stop, unbumped ? Gen::Table::NeverCovered : required, Gen::SweeperId());
        Gen::NoteModel(model, unbumped);
        modeled = true;
    }
    // Only a thread that bumps its epoch reuses anything (an unbumped thread's epoch is fresh per call).
    WriteTracker::Memo* cached = nullptr;
    bool verifyHit = false;
    if (useMemo && !sharedCollectMemo() && HostThreadSlot<std::uint64_t, CollectEpochSlot>::Get() != 0 && bookkeeping::active()) {
        cached = &collectHitCache().at(first, stop);
        if (cached->begin == first && cached->end == stop && cached->epoch == epoch && cached->unwatched == unwatched && cached->mapping == mapping) {
            if (!bookkeeping::verify()) {
                bookkeeping::count(bookkeeping::Hits);
                if (MemoryProfiled()) collectMemoHits.fetch_add(1, std::memory_order_relaxed);
                if (committerThread()) CommitterCollects().hits.fetch_add(1, std::memory_order_relaxed);
                committerCall.hit(committerCensus::OutcomeHitCache);
                census::note(census::OutcomeHit);
                return tracker.stable.load(std::memory_order_acquire);
            }
            verifyHit = true;
        }
    }
    const auto memoHit = [&] {
        census::note(census::OutcomeHit);
        if (cached != nullptr) *cached = {first, stop, epoch, unwatched, mapping};
        if (verifyHit) bookkeeping::count(bookkeeping::Verified);
        collectMemoHits.fetch_add(1, std::memory_order_relaxed);
        if (committerThread()) {
            CommitterCollects().hits.fetch_add(1, std::memory_order_relaxed);
            // APS5_ARCHB_PIPE_COLLECT_SHARE_VERIFY: every 32nd own-memo hit is walked anyway, the
            // baseline rate of writes landing inside an epoch for the covered-walk mismatches.
            if (collectShare::verify() && collectShare::active() && (++collectShare::memoHitsThreadState() & 31u) == 0) {
                const auto lock = lockTracker(tracker);
                std::uint64_t stamped = 0;
                if (walkWrites(tracker, first, stop, StampKind::Cpu, false, nullptr, memoVirtualQuery, &stamped)) {
                    CommitterCollects().shareMemoSampled.fetch_add(1, std::memory_order_relaxed);
                    if (stamped != 0) CommitterCollects().shareMemoDirty.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        return tracker.stable.load(std::memory_order_acquire);
    };
    const bool epochal = HostThreadSlot<std::uint64_t, CollectEpochSlot>::Get() != 0;
    if (useMemo && epochal && !sharedCollectMemo()) {
        // An entry exists only for a completed walk of an in-arena range, so the tracker is
        // initialized and watched; nothing below the lock needs asking.
        if (epochCollectMapEnabled()) {
            const auto& map = epochCollectMap(epoch, unwatched, mapping);
            if (map.Covers(first, stop)) {
                committerCall.hit(committerCensus::OutcomeHitMap);
                return memoHit();
            }
        }
        for (const auto& entry : threadCollectMemo().entries) {
            if (entry.epoch == epoch && entry.unwatched == unwatched && entry.mapping == mapping && entry.begin <= first && stop <= entry.end) {
                // The current generation, not the memoized one: blocks stamped since (MarkWritten, an
                // overlapping collect) were written before this caller reads, and an older value would
                // make every later UnchangedSince fail until the epoch ends.
                committerCall.hit(committerCensus::OutcomeHitRing);
                return memoHit();
            }
        }
    }
    // A table entry the memo does not confirm (verification only): the original path continues.
    if (verifyHit) bookkeeping::mismatch("fix_wt_bookkeeping_hit_mismatches");
    const bool memoFailures = useMemo && HostThreadSlot<std::uint64_t, CollectEpochSlot>::Get() != 0 && !sharedCollectMemo() && epochCollectMapEnabled() && failedCollectMemoEnabled();
    if (memoFailures) {
        const auto& failed = epochCollectMap(epoch, unwatched, mapping).failedRanges;
        if (const auto* found = failed.Find(first); found != nullptr && found->end == stop && found->unwatched == unwatched && found->mapping == mapping) {
            if (MemoryProfiled()) failedCollectMemoHits.fetch_add(1, std::memory_order_relaxed);
            committerCall.hit(committerCensus::OutcomeFailedHit);
            census::note(census::OutcomeHit);
            return 0;
        }
    }
    const auto failedFirst = first;
    const auto failedStop = stop;
    // APS5_ARCHB_FIX_STABLE_FAILED_COLLECTS: a failure remembered from an earlier epoch under the same
    // serials (read before the lock, so a change made during a walk never matches later).
    const bool stableFailures = memoFailures && !Gen::Active() && stableFailed::active();
    const auto stableSerials = stableFailures ? stableFailed::read(unwatched, mapping) : stableFailed::Serials{};
    bool stableVerify = false;
    if (stableFailures && !stableSerials.settled() && stableFailed::diagEnabled()) stableFailed::diagCounters().unsettled.fetch_add(1, std::memory_order_relaxed);
    if (stableFailures && stableSerials.settled()) {
        const auto& entry = stableFailed::table().at(first, stop);
        if (stableFailed::diagEnabled() && !(entry.first == first && entry.stop == stop && entry.serials == stableSerials)) {
            auto& diag = stableFailed::diagCounters();
            if (entry.stop == 0) {
                diag.empty.fetch_add(1, std::memory_order_relaxed);
            } else if (entry.first != first || entry.stop != stop) {
                diag.otherRange.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (entry.serials.unwatched != stableSerials.unwatched) diag.unwatched.fetch_add(1, std::memory_order_relaxed);
                if (entry.serials.mapping != stableSerials.mapping) diag.mapping.fetch_add(1, std::memory_order_relaxed);
                if (entry.serials.committed != stableSerials.committed) diag.committed.fetch_add(1, std::memory_order_relaxed);
                if (entry.serials.protection != stableSerials.protection) diag.protection.fetch_add(1, std::memory_order_relaxed);
                if (entry.serials.forgotten != stableSerials.forgotten) diag.forgotten.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (entry.first == first && entry.stop == stop && entry.serials == stableSerials) {
            if (!stableFailed::verify()) {
                stableFailed::counters().hits.fetch_add(1, std::memory_order_relaxed);
                committerCall.hit(committerCensus::OutcomeFailedHit);
                census::note(census::OutcomeHit);
                EpochMemoMap<WriteTracker::Memo>::SlotState state;
                epochCollectMap(epoch, unwatched, mapping).failedRanges.Slot(first, state) = {first, stop, epoch, unwatched, mapping};
                noteMemoReuse(state);
                return 0;
            }
            stableVerify = true;
        }
    }
    const auto refusedBefore = refusedWalkPagesThreadState();
    const auto fail = [&]() -> std::uint64_t {
        census::note(census::OutcomeFailed);
        if (memoFailures) {
            EpochMemoMap<WriteTracker::Memo>::SlotState state;
            epochCollectMap(epoch, unwatched, mapping).failedRanges.Slot(failedFirst, state) = {failedFirst, failedStop, epoch, unwatched, mapping};
            noteMemoReuse(state);
        }
        if (stableFailures && stableSerials.settled()) {
            auto& counts = stableFailed::counters();
            if (refusedWalkPagesThreadState() != refusedBefore) {
                // Compared or unreported pages: their walk has effects, never remembered.
                counts.refused.fetch_add(1, std::memory_order_relaxed);
                if (stableVerify) counts.mismatches.fetch_add(1, std::memory_order_relaxed);
                stableFailed::table().at(failedFirst, failedStop) = {};
            } else {
                if (stableVerify) counts.verified.fetch_add(1, std::memory_order_relaxed);
                else counts.inserts.fetch_add(1, std::memory_order_relaxed);
                stableFailed::table().at(failedFirst, failedStop) = {failedFirst, failedStop, stableSerials};
            }
        }
        return 0;
    };
    // A remembered failure whose walk now succeeds (verification only).
    const auto stableSucceeded = [&] {
        if (!stableVerify) return;
        stableFailed::counters().mismatches.fetch_add(1, std::memory_order_relaxed);
        stableFailed::table().at(failedFirst, failedStop) = {};
    };
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address || !tracker.covers(address, bytes)) return fail();
    auto cursor = first;
    if (useMemo && sharedCollectMemo()) {
        for (const auto& entry : tracker.memo) {
            if (entry.epoch == epoch && entry.unwatched == unwatchSerial.load(std::memory_order_relaxed) && entry.mapping == mapping && entry.begin <= cursor && stop <= entry.end) {
                collectMemoHits.fetch_add(1, std::memory_order_relaxed);
                census::note(census::OutcomeHit);
                return tracker.generation;
            }
        }
    }
    const TimedAccess timed(CounterCollect, bytes);
    const auto requestedFirst = first;
    const auto requestedStop = stop;
    if (useMemo && HostThreadSlot<std::uint64_t, CollectEpochSlot>::Get() != 0 && bytes <= WriteBlockBytes) {
        const auto begin = first & ~(WriteBlockBytes - 1);
        const auto end = (stop + WriteBlockBytes - 1) & ~(WriteBlockBytes - 1);
        if (end > begin && tracker.covers(begin, end - begin)) {
            first = begin;
            stop = end;
        }
    }
    if (modeled && model.uncoveredPages == 0 && model.unknownBlocks == 0 && model.crossBlocks != 0) Gen::NoteModelCrossWalk();
    // Only a completed walk is remembered (or a range queue 0 walked for this epoch, below); a failed
    // one (uncommitted pages) returns 0.
    const auto remember = [&] {
    stableSucceeded();
    if (collectMemoEnabled()) {
        const auto serial = unwatchSerial.load(std::memory_order_relaxed);
        if (sharedCollectMemo()) tracker.memo[tracker.nextMemo++ % tracker.memo.size()] = {first, stop, epoch, serial, mapping};
        else {
            if (useMemo && epochal) {
                auto& memo = threadCollectMemo();
                WriteTracker::Memo merged{first, stop, epoch, serial, mapping};
                static const bool mergeMemo = std::getenv("APS5_NO_COLLECT_MEMO_MERGE") == nullptr;
                if (mergeMemo) {
                    for (auto& entry : memo.entries) {
                        if (entry.epoch != epoch || entry.unwatched != serial || entry.mapping != mapping || entry.begin > merged.end || merged.begin > entry.end) continue;
                        merged.begin = std::min(merged.begin, entry.begin);
                        merged.end = std::max(merged.end, entry.end);
                        entry = {};
                    }
                }
                memo.entries[memo.next++ % memo.entries.size()] = merged;
            }
            if (cached != nullptr) *cached = {failedFirst, failedStop, epoch, serial, mapping};
            if (epochal && epochCollectMapEnabled()) {
                epochCollectMap(epoch, serial, mapping).Record(first, stop);
            }
        }
    }
    };
    // APS5_ARCHB_PIPE_COLLECT_SHARE: the committer skips a walk queue 0 already made for this epoch;
    // queue 0 records its walks. Not with the write-generation table (its sweep model follows walks).
    const bool shareCommitter = committerThread() && useMemo && !Gen::Active() && (collectShare::active() || resolveShareActive());
    const bool shareProducer = !committerThread() && !Gen::Active() && ((collectShare::producerThreadState() && collectShare::active()) || (resolverThreadThreadState() && resolveShareActive()));
    bool shareCovered = false;
#ifdef _WIN32
    if (shareCommitter) {
        const auto pages = (stop - first) / page;
        const bool verifying = collectShare::verify();
        const auto coveredPages = collectShare::covered(first, stop, unwatched, mapping, verifying);
        shareCovered = pages != 0 && coveredPages == pages;
        auto& counters = CommitterCollects();
        if (shareCovered) {
            counters.shareCovered.fetch_add(1, std::memory_order_relaxed);
            counters.shareCoveredPages.fetch_add(pages, std::memory_order_relaxed);
            if (!verifying) {
                // Answered like a memo hit (the current generation; queue 0's walk stamped what it found).
                committerCall.hit(committerCensus::OutcomeHitShare);
                census::note(census::OutcomeHit);
                remember();
                return tracker.generation.load(std::memory_order_relaxed);
            }
        } else counters.sharePartialPages.fetch_add(coveredPages, std::memory_order_relaxed);
    }
#endif
    if (committerThread() && useMemo) {
        CommitterCollects().walks.fetch_add(1, std::memory_order_relaxed);
        CommitterCollects().walkPages.fetch_add((stop - first) / page, std::memory_order_relaxed);
    }
    // The clock before the OS query: every bump at or below it happened before this walk.
    const auto shareClock = shareProducer ? nextCollectEpoch.load(std::memory_order_acquire) : 0;
    const auto walkStart = shareCommitter ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto aheadWalkStart = (aheadScope.on && useMemo) || committerCall.on ? aheadCensus::nowNs() : 0;
    std::uint64_t stamped = 0;
    bool walkedOk = walkWrites(tracker, first, stop, StampKind::Cpu, false, nullptr, memoVirtualQuery, &stamped);
#ifdef _WIN32
    if (!walkedOk && (first != requestedFirst || stop != requestedStop)) {
        first = requestedFirst;
        stop = requestedStop;
        walkedOk = walkWrites(tracker, first, stop, StampKind::Cpu, false, nullptr, memoVirtualQuery, &stamped);
    }
#endif
    if (aheadScope.on && useMemo) aheadScope.walked((stop - first) / page, aheadCensus::nowNs() - aheadWalkStart);
    if (committerCall.on) committerCall.walked(walkedOk, aheadCensus::nowNs() - aheadWalkStart);
    if (!walkedOk) return fail();
    if (stamped != 0) census::note(census::OutcomeChanged);
    else census::noteIfUnset(census::OutcomeClean);
    if (shareCommitter) {
        auto& counters = CommitterCollects();
        const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - walkStart).count());
        counters.shareWalkNs.fetch_add(ns, std::memory_order_relaxed);
        if (shareCovered) {
            counters.shareCoveredNs.fetch_add(ns, std::memory_order_relaxed);
            if (stamped != 0) {
                counters.shareMismatches.fetch_add(1, std::memory_order_relaxed);
                counters.shareMismatchPages.fetch_add(stamped, std::memory_order_relaxed);
            }
        }
    }
#ifdef _WIN32
    if (shareProducer) collectShare::record(tracker, first, stop, shareClock, unwatched, mapping);
#else
    static_cast<void>(shareClock);
    static_cast<void>(shareCovered);
#endif
    remember();
    return tracker.generation;
}

}

std::uint64_t CollectWrites(std::uint64_t address, std::size_t bytes, bool memoVirtualQuery, std::source_location site) {
    return collectWrites(address, bytes, true, memoVirtualQuery, site);
}

std::uint64_t CollectWritesUncached(std::uint64_t address, std::size_t bytes, std::source_location site) {
    return collectWrites(address, bytes, false, false, site);
}

std::uint64_t WriteGeneration() {
    return Tracker().generation.load(std::memory_order_acquire);
}

std::uint64_t CollectWrittenBlocks(std::uint64_t address, std::size_t bytes, std::uint64_t since, std::vector<std::pair<std::uint64_t, std::uint64_t>>& written, std::source_location site) {
    const census::Scope censusScope("written_blocks", &site);
    written.clear();
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return 0;
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched) return 0;
#ifdef _WIN32
    if (address < tracker.base || bytes > tracker.size || address - tracker.base > tracker.size - bytes) return 0;
#endif
    const auto end = address + bytes;
    const auto first = tracker.blockOf(address);
    const auto last = tracker.blockOf(end - 1);
    const auto clip = [&](std::uint64_t block) {
        const auto begin = tracker.blockBegin(block);
        return std::pair{std::max(address, begin), std::min<std::uint64_t>(end, begin + WriteBlockBytes)};
    };
    std::uint64_t spanBegin = 0;
    std::uint64_t spanEnd = 0;
    const auto walkSpan = [&] {
        if (spanEnd > spanBegin) walkWrites(tracker, spanBegin & ~std::uint64_t{WritePageBytes - 1}, (spanEnd + WritePageBytes - 1) & ~std::uint64_t{WritePageBytes - 1}, StampKind::Cpu);
        spanBegin = spanEnd = 0;
    };
    for (auto block = first; block <= last; ++block) {
        const auto [begin, stop] = clip(block);
        if (!tracker.covers(begin, stop - begin)) {
            walkSpan();
            continue;
        }
        if (spanEnd != begin) {
            walkSpan();
            spanBegin = begin;
        }
        spanEnd = stop;
    }
    walkSpan();
    const auto append = [&](std::uint64_t begin, std::uint64_t stop) {
        if (!written.empty() && written.back().second == begin) written.back().second = stop;
        else written.emplace_back(begin, stop);
    };
    for (auto block = first; block <= last; ++block) {
        const bool cpu = tracker.cpuStampOf(block) > since;
        if (!cpu && tracker.stampOf(block) <= since) continue;
        const auto [begin, stop] = clip(block);
        const auto blockStart = tracker.blockBegin(block);
        const auto noted = tracker.cpuPages.find(block);
        const auto window = tracker.windowPages.find(block);
        std::array<bool, WritePagesPerBlock> listed{};
        bool any = false;
        for (std::size_t page = 0; page < WritePagesPerBlock; ++page) {
            if (std::max<std::uint64_t>(begin, blockStart + page * WritePageBytes) >= std::min<std::uint64_t>(stop, blockStart + (page + 1) * WritePageBytes)) continue;
            const bool stored = noted != tracker.cpuPages.end() && noted->second.pages[page] > since;
            any = any || stored;
            listed[page] = stored || (window != tracker.windowPages.end() && window->second.pages[page] > since);
        }
        if (cpu && !any) {
            append(begin, stop);
            continue;
        }
        for (std::size_t page = 0; page < WritePagesPerBlock; ++page) {
            if (listed[page]) append(std::max<std::uint64_t>(begin, blockStart + page * WritePageBytes), std::min<std::uint64_t>(stop, blockStart + (page + 1) * WritePageBytes));
        }
    }
    return tracker.generation.load(std::memory_order_relaxed);
}

void BumpCollectEpoch(Gen::TicketReason reason) {
    census::noteBump();
    const auto epoch = nextCollectEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
    HostThreadSlot<std::uint64_t, CollectEpochSlot>::Set(epoch);
    auto last = lastBumpedEpoch.load(std::memory_order_relaxed);
    while (last < epoch && !lastBumpedEpoch.compare_exchange_weak(last, epoch, std::memory_order_release, std::memory_order_relaxed)) {}
    collectEpochBumps.fetch_add(1, std::memory_order_relaxed);
    // APS5_ARCHB_PIPE_COLLECT_SHARE: a committer's own ordering point (an unlocked wait) also bounds
    // the queue-0 walks that may stand in for its own; following a commit's token resets the
    // requirement until FollowCollectShare names the token's point.
    if (committerThread()) {
        if (reason == Gen::TicketReason::Committer) collectShare::requiredThreadState() = collectShare::Unknown;
        else {
            collectShare::floorThreadState() = std::max(collectShare::floorThreadState(), epoch);
            if (collectShare::requiredThreadState() != collectShare::Unknown) collectShare::requiredThreadState() = std::max(collectShare::requiredThreadState(), epoch);
        }
    }
    // Rule V1: a ticket at every ordering point, after the observation (the same points as the
    // epoch). A committer's ticket is the one its commit carried (Gen::Follow), not a fresh one.
    if (reason != Gen::TicketReason::Committer && Gen::Active()) static_cast<void>(Gen::Issue(reason));
}

std::uint64_t ThreadCollectEpoch() {
    return collectMemoEnabled() ? HostThreadSlot<std::uint64_t, CollectEpochSlot>::Get() : 0;
}

CommitterCollectCounters& CommitterCollects() {
    static auto* counters = new CommitterCollectCounters();
    return *counters;
}

void MarkCommitterThread() {
    committerThread(true);
    trackerTimedWaits(&committerTrackerWaits);
    trackerTimedWaitNs(&committerTrackerWaitNs);
    MarkDriverThread();
}

void MarkResolverThread() {
    resolverThreadThreadState() = true;
    trackerTimedWaits(&resolverTrackerWaits);
    trackerTimedWaitNs(&resolverTrackerWaitNs);
    MarkDriverThread();
}

bool ResolveShareActive() {
    return resolveShareActive();
}

static bool faultUnchangedSinceImpl(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site);

bool FaultUnchangedSince(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site) {
    census::Scope censusScope("fault_unchanged", &site);
    const auto result = faultUnchangedSinceImpl(address, bytes, generation, site);
    census::answer(result);
    return result;
}

static bool faultUnchangedSinceImpl(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site) {
    static_cast<void>(site);
#ifdef _WIN32
    if (bytes == 0 || !wtFault::active.load(std::memory_order_acquire)) return false;
    return GuestArena::GuestWtFaultChangedSince_nid_no_patch(static_cast<std::uintptr_t>(address), bytes, generation) == GuestArena::WtFaultAnswer::Unchanged;
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(generation);
    return false;
#endif
}

bool FaultUntracked(std::uint64_t address, std::size_t bytes, std::uint64_t generation) {
#ifdef _WIN32
    if (bytes == 0 || !wtFault::active.load(std::memory_order_acquire)) return true;
    return GuestArena::GuestWtFaultChangedSince_nid_no_patch(static_cast<std::uintptr_t>(address), bytes, generation) == GuestArena::WtFaultAnswer::Untracked;
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(generation);
    return true;
#endif
}

std::uint64_t FaultClock() {
#ifdef _WIN32
    if (!wtFault::active.load(std::memory_order_acquire)) return 0;
    return GuestArena::GuestWtFaultClock_nid_no_patch();
#else
    return 0;
#endif
}

std::atomic<std::uint64_t>& CommitterTrackerWaits() {
    return committerTrackerWaits;
}

std::atomic<std::uint64_t>& CommitterTrackerWaitNs() {
    return committerTrackerWaitNs;
}

std::atomic<std::uint64_t>& ResolverTrackerWaits() {
    return resolverTrackerWaits;
}

std::atomic<std::uint64_t>& ResolverTrackerWaitNs() {
    return resolverTrackerWaitNs;
}

void MarkDriverThread() {
    if (wtFault::switchOn()) GuestArena::GuestWtFaultMarkDriverThread_nid_no_patch();
}

namespace walkCost {
struct Counters {
    std::atomic<std::uint64_t> vqCalls{0}, vqNs{0}, gwwCalls{0}, gwwNs{0}, gwwPages{0};
};
void flip() {
    static auto* made = [] {
        auto* c = new Counters();
        const std::pair<const char*, std::atomic<std::uint64_t>*> named[] = {
            {"walk_vq_calls", &c->vqCalls}, {"walk_vq_ns", &c->vqNs}, {"walk_gww_calls", &c->gwwCalls}, {"walk_gww_ns", &c->gwwNs}, {"walk_gww_pages", &c->gwwPages}};
        for (const auto& [name, counter] : named) ArchB::Recorder::RegisterCounter(name, *counter);
        return c;
    }();
#ifdef _WIN32
    const auto now = GuestArena::GuestArenaWalkCost_nid_no_patch();
    made->vqCalls.store(now.vqCalls, std::memory_order_relaxed);
    made->vqNs.store(now.vqNs, std::memory_order_relaxed);
    made->gwwCalls.store(now.gwwCalls, std::memory_order_relaxed);
    made->gwwNs.store(now.gwwNs, std::memory_order_relaxed);
    made->gwwPages.store(now.gwwPages, std::memory_order_relaxed);
    static const std::uint64_t every = [] {
        const char* text = std::getenv("APS5_WALK_COST_REPORT_FLIPS");
        return text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
    }();
    if (every == 0) return;
    static std::uint64_t flips = 0;
    static GuestArena::WalkCost last{};
    if (++flips % every != 0) return;
    const double frames = static_cast<double>(every);
    const auto vqCalls = static_cast<double>(now.vqCalls - last.vqCalls);
    const auto vqNs = static_cast<double>(now.vqNs - last.vqNs);
    const auto gwwCalls = static_cast<double>(now.gwwCalls - last.gwwCalls);
    const auto gwwNs = static_cast<double>(now.gwwNs - last.gwwNs);
    std::fprintf(stderr, "[walk-cost] per frame over %llu flips: VirtualQuery %.1f calls %.1f us (%.2f us/call), GetWriteWatch %.1f calls %.1f us (%.2f us/call, %.0f pages/call)\n", static_cast<unsigned long long>(every), vqCalls / frames,
                 vqNs / frames / 1000.0, vqCalls > 0 ? vqNs / vqCalls / 1000.0 : 0.0, gwwCalls / frames, gwwNs / frames / 1000.0, gwwCalls > 0 ? gwwNs / gwwCalls / 1000.0 : 0.0,
                 gwwCalls > 0 ? static_cast<double>(now.gwwPages - last.gwwPages) / gwwCalls : 0.0);
    last = now;
#endif
}
}

void WtFaultFlip(bool presenter) {
    // The presenter arms once it exists (off queue 0); without one (frame replay) queue 0 does at the
    // flip packet.
    static std::atomic<bool> presenterSeen{false};
    if (presenter) presenterSeen.store(true, std::memory_order_relaxed);
    else if (presenterSeen.load(std::memory_order_relaxed)) return;
    if (presenter) census::liveFlip();
    walkCost::flip();
    if (!wtFault::active.load(std::memory_order_acquire)) return;
    // The runs just armed are walked by write watching (resetting, stamping what was stored before the
    // protect, one tracker lock per run), so each armed page's sync clock passes its stamp. With
    // CONSUMERS this keeps the fault-state answers exact: without it, every page stored while open would
    // read as changed once more at its first collect after the flip (no write watching tells whether it
    // was stored after its last walk), and in frame replays those extra re-reads change the display
    // buffers. With VERIFY a later store that did not fault (a page unprotected behind the tracker's
    // back) reads as a mismatch at its next walk.
    constexpr std::size_t ArmedRuns = 4096;
    static std::vector<std::uintptr_t> runs(2 * ArmedRuns);
    const bool walkArmed = wtFault::verifyOn() || wtFault::consumersOn();
    static std::vector<std::uintptr_t> dirtyRuns(2 * ArmedRuns);
    std::size_t dirtyCount = 0;
    const auto armed = wtFault::dirtyArmOn() ? GuestArena::GuestWtFaultArmKeepDirty_nid_no_patch(runs.data(), walkArmed ? ArmedRuns : 0, dirtyRuns.data(), ArmedRuns, &dirtyCount) :
                                               GuestArena::GuestWtFaultArm_nid_no_patch(runs.data(), walkArmed ? ArmedRuns : 0);
    auto& wc = wtFault::counters();
    if (walkArmed && armed != 0) {
        const auto walkStart = std::chrono::steady_clock::now();
        auto& tracker = Tracker();
        stampTrace::walkTagThreadState() = 1;
        for (std::size_t i = 0; i < armed && i < ArmedRuns; ++i) {
            const auto lock = lockTracker(tracker);
            walkWrites(tracker, runs[2 * i], runs[2 * i + 1], StampKind::Cpu, false, nullptr, false, nullptr, true);
        }
        stampTrace::walkTagThreadState() = 0;
        wc.armedWalkNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - walkStart).count()), std::memory_order_relaxed);
    }
    if (dirtyCount != 0) {
        const auto walkStart = std::chrono::steady_clock::now();
        auto& tracker = Tracker();
        wtFault::flipDirtyWalkThreadState() = true;
        stampTrace::walkTagThreadState() = 2;
        for (std::size_t i = 0; i < dirtyCount; ++i) {
            const auto lock = lockTracker(tracker);
            walkWrites(tracker, dirtyRuns[2 * i], dirtyRuns[2 * i + 1], StampKind::Cpu, false, nullptr, false, nullptr, true);
        }
        wtFault::flipDirtyWalkThreadState() = false;
        stampTrace::walkTagThreadState() = 0;
        wc.dirtyWalks.fetch_add(dirtyCount, std::memory_order_relaxed);
        wc.dirtyWalkNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - walkStart).count()), std::memory_order_relaxed);
    }
    wtFault::publish();
    for (const auto& slot : armObservers) {
        if (const auto observer = slot.load(std::memory_order_acquire)) observer();
    }
    // A per-frame line every APS5_WT_FAULT_REPORT_FLIPS flips (default 300).
    static const std::uint64_t every = [] {
        const char* text = std::getenv("APS5_WT_FAULT_REPORT_FLIPS");
        const auto value = text != nullptr ? std::strtoull(text, nullptr, 10) : 300ull;
        return value == 0 ? 300ull : value;
    }();
    static std::uint64_t flips = 0;
    static GuestArena::WtFaultCounters last{};
    static auto lastTime = std::chrono::steady_clock::now();
    if (++flips % every != 0) return;
    GuestArena::WtFaultCounters now{};
    GuestArena::GuestWtFaultCounters_nid_no_patch(&now);
    const auto time = std::chrono::steady_clock::now();
    const double frames = static_cast<double>(every);
    const auto per = [&](std::uint64_t value, std::uint64_t before) { return static_cast<double>(value - before) / frames; };
    const auto& c = wtFault::counters();
    static std::uint64_t lastUnchanged = 0, lastChanged = 0, lastFallback = 0, lastDirtyWalks = 0, lastDirtyWalkNs = 0, lastArmedWalkNs = 0;
    const auto unchanged = c.consumerUnchanged.load(std::memory_order_relaxed);
    const auto changed = c.consumerChanged.load(std::memory_order_relaxed);
    const auto fallback = c.consumerFallback.load(std::memory_order_relaxed);
    const auto dirtyWalks = c.dirtyWalks.load(std::memory_order_relaxed);
    const auto dirtyWalkNs = c.dirtyWalkNs.load(std::memory_order_relaxed);
    const auto armedWalkNs = c.armedWalkNs.load(std::memory_order_relaxed);
    std::fprintf(stderr, "[wt-fault] flip walks per frame: armed %.1f us, kept-dirty %.1f runs %.1f us\n", per(armedWalkNs, lastArmedWalkNs) / 1000.0, per(dirtyWalks, lastDirtyWalks), per(dirtyWalkNs, lastDirtyWalkNs) / 1000.0);
    lastDirtyWalks = dirtyWalks;
    lastDirtyWalkNs = dirtyWalkNs;
    lastArmedWalkNs = armedWalkNs;
    std::fprintf(stderr, "[wt-fault] per frame over %llu flips (%.2f ms/f): faults %.1f (driver %.1f) handler %.1f us (driver %.1f) retries %.2f foreign %.2f | arm %.1f us (protect %.1f query %.1f lock %.1f scan %.1f, %.1f leaves of %llu, %.1f queries) pages %.1f runs %.1f kept-hot %.1f (dirty %.1f, %.1f us) excluded %.2f | holds %.2f forgotten %.1f tracked %.1f | verify dirty %.1f untracked %.1f mismatches %llu total | consumers unchanged %.1f changed %.1f fallback %.1f mismatches %llu total\n",
                 static_cast<unsigned long long>(every), std::chrono::duration<double, std::milli>(time - lastTime).count() / frames,
                 per(now.faults, last.faults), per(now.driverFaults, last.driverFaults), per(now.handlerNs, last.handlerNs) / 1000.0, per(now.driverHandlerNs, last.driverHandlerNs) / 1000.0,
                 per(now.retries, last.retries), per(now.foreign, last.foreign), per(now.armNs, last.armNs) / 1000.0, per(now.armProtectNs, last.armProtectNs) / 1000.0, per(now.armQueryNs, last.armQueryNs) / 1000.0,
                 per(now.armLockNs, last.armLockNs) / 1000.0, per(now.armScanNs, last.armScanNs) / 1000.0, per(now.armLeaves, last.armLeaves), static_cast<unsigned long long>(now.leaves), per(now.armQueries, last.armQueries),
                 per(now.armedPages, last.armedPages), per(now.armRuns, last.armRuns), per(now.keptHot, last.keptHot), per(now.keptDirty, last.keptDirty), per(now.armDirtyNs, last.armDirtyNs) / 1000.0, per(now.excluded, last.excluded),
                 per(now.holds, last.holds), per(now.forgotten, last.forgotten), per(now.tracked, last.tracked), per(now.verifyDirty, last.verifyDirty),
                 per(now.verifyUntracked, last.verifyUntracked), static_cast<unsigned long long>(now.verifyMismatches),
                 per(unchanged, lastUnchanged), per(changed, lastChanged), per(fallback, lastFallback), static_cast<unsigned long long>(c.consumerMismatches.load(std::memory_order_relaxed)));
    if (now.openSplitRuns != last.openSplitRuns || now.openRefused != last.openRefused) {
        std::fprintf(stderr, "[wt-fault] opens across allocations: %llu runs restored in %llu regions, %llu pages refused (totals)\n", static_cast<unsigned long long>(now.openSplitRuns), static_cast<unsigned long long>(now.openSplitPieces), static_cast<unsigned long long>(now.openRefused));
    }
    last = now;
    lastTime = time;
    lastUnchanged = unchanged;
    lastChanged = changed;
    lastFallback = fallback;
}

WtFaultStatus WtFault() {
    WtFaultStatus status;
    status.active = wtFault::active.load(std::memory_order_acquire);
    if (!status.active) return status;
    wtFault::publish();
    const auto& c = wtFault::counters();
    status.mismatches = c.mismatches.load(std::memory_order_relaxed);
    status.faults = c.faults.load(std::memory_order_relaxed);
    status.armedPages = c.armedPages.load(std::memory_order_relaxed);
    status.consumerUnchanged = c.consumerUnchanged.load(std::memory_order_relaxed);
    status.consumerChanged = c.consumerChanged.load(std::memory_order_relaxed);
    status.consumerFallback = c.consumerFallback.load(std::memory_order_relaxed);
    status.consumerMismatches = c.consumerMismatches.load(std::memory_order_relaxed);
    return status;
}

bool IsCommitterThread() {
    return committerThread();
}

void SetCollectShareProducer(bool producer) {
    collectShare::producerThreadState() = producer;
}

void FollowCollectShare(std::uint64_t clock) {
    if (!committerThread()) return;
    collectShare::requiredThreadState() = clock == 0 ? collectShare::Unknown : std::max(clock, collectShare::floorThreadState());
}

bool CollectAheadCensus() {
    return aheadCensus::enabled();
}

StableFailedCollectCounters& StableFailedCollects() {
    return stableFailed::counters();
}

bool CommitterCollectCensus() {
    return committerCensus::enabled();
}

void BeginCommitterCensusItem(std::uint8_t kind) {
    if (!committerThread() || !committerCensus::enabled()) return;
    auto& census = committerCensus::state();
    ++census.itemSerial;
    census.itemKind = kind;
    ++census.items[kind & 7u];
}

void NoteCensusFlip() {
    WtFaultFlip(false);
    committerCensus::flips.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t CensusFlips() {
    return committerCensus::flips.load(std::memory_order_relaxed);
}

CollectAheadCensusCounters& CollectAheadCensusCounts() {
    static auto* counters = new CollectAheadCensusCounters();
    return *counters;
}

void SetCommitterPrelock(bool prelock) {
    if (!committerThread()) return;
    aheadCensus::prelock = prelock;
    if (prelock && !aheadCensus::item.prepared) {
        aheadCensus::item.prepared = true;
        aheadCensus::item.required = collectShare::requiredThreadState() == collectShare::Unknown ? collectShare::Unknown : std::max(collectShare::requiredThreadState(), collectShare::floorThreadState());
    }
}

CommitterItemCollects TakeCommitterItemCollects() {
    const auto taken = aheadCensus::item;
    aheadCensus::item = {};
    return taken;
}

std::uint64_t CollectClock() {
    return nextCollectEpoch.load(std::memory_order_acquire);
}

std::uint64_t UnwatchSerial() {
    return unwatchSerial.load(std::memory_order_acquire);
}

void AdoptLatestCollectEpoch() {
    const auto latest = lastBumpedEpoch.load(std::memory_order_acquire);
    if (latest > HostThreadSlot<std::uint64_t, CollectEpochSlot>::Get()) HostThreadSlot<std::uint64_t, CollectEpochSlot>::Set(latest);
    if (committerThread()) {
        collectShare::floorThreadState() = std::max(collectShare::floorThreadState(), latest);
        if (collectShare::requiredThreadState() != collectShare::Unknown) collectShare::requiredThreadState() = std::max(collectShare::requiredThreadState(), latest);
    }
    if (Gen::Active()) Gen::AdoptLatest();
}

namespace Gen::detail {
std::uint64_t CurrentTrackerGeneration() noexcept {
    return Tracker().generation.load(std::memory_order_relaxed);
}

std::uint64_t StableTrackerGeneration() noexcept {
    return Tracker().stable.load(std::memory_order_acquire);
}
}

std::uint64_t CollectEpochBumps() {
    return collectEpochBumps.load(std::memory_order_relaxed);
}

void ResetWriteCensus() {
    if (Gen::Active()) Gen::ResetCensus();
    if (!census::enabled()) return;
    std::lock_guard lock(census::tableMutex());
    census::table().clear();
}

namespace {
namespace census {
void printRows(const std::map<Key, Row>& rows, const char* prefix, const char* what, std::uint64_t pass, std::uint64_t frames) {
    const auto base = [](const char* path) {
        const char* name = path;
        for (const char* at = path; *at != 0; ++at) if (*at == '/' || *at == '\\') name = at + 1;
        return name;
    };
    for (const auto& [key, row] : rows) {
        std::string outcomes;
        for (std::size_t i = 0; i < row.outcomes.size(); ++i) {
            char text[64];
            std::snprintf(text, sizeof(text), " %s=%llu/%llu", OutcomeNames[i], static_cast<unsigned long long>(row.outcomes[i]), static_cast<unsigned long long>(row.outcomeNs[i]));
            outcomes += text;
        }
        std::fprintf(stderr, "%s %s=%llu frames=%llu tag=0x%x entry=%s site=%s:%u calls=%llu walks=%llu lock_waits=%llu lock_ns=%llu widen_covered=%llu widen_calls=%llu widen_pages=%llu exact=%llu covered=%llu partial=%llu walk_pages=%llu repeat_pages=%llu unbumped=%llu collects=%llu queries=%llu resets=%llu os_pages=%llu dirty=%llu os_ns=%llu ns=%llu%s fn=%s\n",
            prefix, what, static_cast<unsigned long long>(pass), static_cast<unsigned long long>(frames), key.tag, key.entry, base(key.file), key.line,
            static_cast<unsigned long long>(row.calls), static_cast<unsigned long long>(row.walks), static_cast<unsigned long long>(row.lockWaits), static_cast<unsigned long long>(row.lockNs), static_cast<unsigned long long>(row.widenCovered), static_cast<unsigned long long>(row.widenCalls), static_cast<unsigned long long>(row.widenPages), static_cast<unsigned long long>(row.exact), static_cast<unsigned long long>(row.covered),
            static_cast<unsigned long long>(row.partial), static_cast<unsigned long long>(row.walkPages), static_cast<unsigned long long>(row.repeatPages), static_cast<unsigned long long>(row.unbumped),
            static_cast<unsigned long long>(row.collects), static_cast<unsigned long long>(row.queries), static_cast<unsigned long long>(row.resets), static_cast<unsigned long long>(row.osPages),
            static_cast<unsigned long long>(row.dirty), static_cast<unsigned long long>(row.osNs), static_cast<unsigned long long>(row.ns), outcomes.c_str(), key.function);
    }
}

void liveFlip() {
    if (!enabled()) return;
    static std::atomic<std::uint64_t> flips{0};
    static std::atomic<std::int64_t> lastReport{0};
    static const auto started = std::chrono::steady_clock::now();
    const auto count = flips.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    auto last = lastReport.load(std::memory_order_relaxed);
    if (now - last < 10000 || !lastReport.compare_exchange_strong(last, now)) return;
    std::map<Key, Row> rows;
    {
        std::lock_guard lock(tableMutex());
        rows = table();
    }
    std::fprintf(stderr, "[wt-census-live] begin t_ms=%lld flips=%llu rows=%zu\n", static_cast<long long>(now), static_cast<unsigned long long>(count), rows.size());
    printRows(rows, "[wt-census-live]", "t_ms", static_cast<std::uint64_t>(now), count);
    std::fprintf(stderr, "[wt-census-live] end t_ms=%lld\n", static_cast<long long>(now));
    std::fflush(stderr);
}
}
}

void ReportWriteCensus(std::uint64_t pass, std::uint64_t frames) {
    if (Gen::Active()) Gen::ReportCensus(pass, frames);
    if (!census::enabled()) return;
    std::map<census::Key, census::Row> rows;
    {
        std::lock_guard lock(census::tableMutex());
        rows.swap(census::table());
    }
    census::printRows(rows, "[wt-census]", "pass", pass, frames);
#ifdef _WIN32
    const auto sweep = GuestArena::GuestArenaWriteWatchSweep_nid_no_patch();
    std::fprintf(stderr, "[wt-census-sweep] pass=%llu regions=%llu bytes=%llu dirty=%llu refused=%llu queries=%llu resident_runs=%llu resident_bytes=%llu walk_ns=%llu query_ns=%llu\n",
        static_cast<unsigned long long>(pass), static_cast<unsigned long long>(sweep.regions), static_cast<unsigned long long>(sweep.bytes), static_cast<unsigned long long>(sweep.dirty),
        static_cast<unsigned long long>(sweep.refused), static_cast<unsigned long long>(sweep.queries), static_cast<unsigned long long>(sweep.residentRuns), static_cast<unsigned long long>(sweep.residentBytes),
        static_cast<unsigned long long>(sweep.walkNs), static_cast<unsigned long long>(sweep.queryNs));
#endif
    std::fflush(stderr);
}

namespace {
std::atomic<std::uint64_t> proofSerial{1};
std::atomic<std::uint64_t> resourceProofSerial{1};
bool& proofThreadThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<bool, Slot>();
}
}

std::uint64_t ProofSerial() {
    return proofSerial.load(std::memory_order_acquire);
}

bool ResourceEpochProofsEnabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("APS5_RESOURCE_EPOCH_PROOFS");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

std::uint64_t ResourceProofSerial() {
    return resourceProofSerial.load(std::memory_order_acquire);
}

void BreakProofs() {
    proofSerial.fetch_add(1, std::memory_order_acq_rel);
    if (ResourceEpochProofsEnabled()) resourceProofSerial.fetch_add(1, std::memory_order_acq_rel);
}

void MarkProofThread() {
    proofThreadThreadState() = true;
}

std::uint64_t CollectEpoch() {
    return collectMemoEnabled() ? HostThreadSlot<std::uint64_t, CollectEpochSlot>::Get() : 0;
}

namespace {

std::atomic<void (*)(std::uint64_t, std::size_t)> unwatchHook{nullptr};

bool& unwatchWritesProvenThreadState() {
    struct State { bool value = true; };
    struct Slot {};
    return AgcDriver::ThreadScratch<State, Slot>().value;
}
std::atomic<int> importWalkFailureForTest{0};
std::atomic<std::uint64_t> importWalkFailureSpanForTest{0};

void beforeUnwatch(std::uint64_t address, std::size_t bytes) {
    const auto hook = unwatchHook.load(std::memory_order_acquire);
    if (hook == nullptr) return;
#ifdef _WIN32
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    {
        auto& tracker = Tracker();
        const auto lock = lockTracker(tracker);
        tracker.initialize();
        if (!tracker.watched || !tracker.coverage.ExcludedSpan(address, bytes, &begin, &end)) return;
    }
    hook(begin, static_cast<std::size_t>(end - begin));
#else
    hook(address, bytes);
#endif
}

void beforeUnwatchUnproven(std::uint64_t address, std::size_t bytes) {
    struct Unproven {
        Unproven() { unwatchWritesProvenThreadState() = false; }
        ~Unproven() { unwatchWritesProvenThreadState() = true; }
        Unproven(const Unproven&) = delete;
        Unproven& operator=(const Unproven&) = delete;
    } unproven;
    beforeUnwatch(address, bytes);
}

void unwatchLocked(WriteTracker& tracker, std::uint64_t address, std::size_t bytes) {
    if (!tracker.watched) return;
    if (const auto observer = stampObserver.load(std::memory_order_acquire); observer != nullptr && bytes != 0) observer(address, address + bytes, TrackerEvent::Unwatch);
    // Write generations: excluded blocks read Unknown(excluded) until coverage is restored.
    if (bytes != 0 && Gen::Active()) static_cast<void>(Gen::NoteState(Gen::Event::Unwatch, address, address + bytes, Gen::BlockState::Excluded, Gen::Meta, 0, false));
#ifdef _WIN32
    tracker.startHints(address, bytes);
    if (tracker.coverage.Exclude(address, bytes, GuestArena::GuestArenaCommitGeneration_nid_postfix())) unwatchSerial.fetch_add(1, std::memory_order_release);
#else
    if (GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(reinterpret_cast<const void*>(address), bytes)) unwatchSerial.fetch_add(1, std::memory_order_release);
#endif
}

bool& ownHostWriteThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<bool, Slot>();
}
std::atomic<std::uint64_t> hostWritesInFlight{0};
std::atomic<bool> hostWritesStopped{false};

class HostWriteInFlight {
public:
    HostWriteInFlight() { hostWritesInFlight.fetch_add(1, std::memory_order_seq_cst); }
    ~HostWriteInFlight() { hostWritesInFlight.fetch_sub(1, std::memory_order_seq_cst); }
    HostWriteInFlight(const HostWriteInFlight&) = delete;
    HostWriteInFlight& operator=(const HostWriteInFlight&) = delete;
};

class OwnHostWrite {
public:
    OwnHostWrite() : outer(std::exchange(ownHostWriteThreadState(), true)) {}
    ~OwnHostWrite() { ownHostWriteThreadState() = outer; }
    OwnHostWrite(const OwnHostWrite&) = delete;
    OwnHostWrite& operator=(const OwnHostWrite&) = delete;

private:
    bool outer;
};

}

void SetUnwatchHook(void (*hook)(std::uint64_t, std::size_t)) {
    unwatchHook.store(hook, std::memory_order_release);
}

bool UnwatchWritesProven() {
    return unwatchWritesProvenThreadState();
}

void SetImportWalkFailureForTest(int walk, std::uint64_t span) {
    importWalkFailureSpanForTest.store(span, std::memory_order_release);
    importWalkFailureForTest.store(walk, std::memory_order_release);
}

#ifdef _WIN32
void SetFreshCollectHook(FreshCollectHook hook) {
    freshCollectHook.store(hook, std::memory_order_release);
}
#endif

void Unwatch(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return;
    beforeUnwatch(address, bytes);
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    unwatchLocked(tracker, address, bytes);
}

void Rewatch(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
#ifdef _WIN32
    const census::Scope censusScope("rewatch");
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched) return;
    bool restored = false;
    for (const auto& [begin, end] : ranges) {
        if (end <= begin || begin < tracker.base || end - tracker.base > tracker.size || (begin - tracker.base) % WritePageBytes != 0 || (end - tracker.base) % WritePageBytes != 0) continue;
        const auto bytes = static_cast<std::size_t>(end - begin);
        if (tracker.coverage.Covers(begin, bytes)) continue;
        GuestArena::GuestArenaCompareWhenShared_nid_no_patch(static_cast<std::uintptr_t>(begin), bytes);
        if (!walkWrites(tracker, begin, end, StampKind::Cpu)) continue;
        // The new generation reaches the table before the restored coverage (see watchPrivateMapping);
        // a restore that changes nothing leaves only a spare generation behind.
        tracker.bump();
        const bool gen = Gen::Active();
        if (gen) static_cast<void>(Gen::Note(Gen::Event::Rewatch, begin, end, Gen::Data | Gen::Cpu | Gen::Meta, tracker.generation.load(std::memory_order_relaxed)));
        if (!tracker.coverage.Restore(begin, bytes)) continue;
        tracker.noteCpuStore(begin, end, tracker.generation);
        for (auto block = tracker.blockOf(begin); block <= tracker.blockOf(end - 1); ++block) {
            tracker.driverPieces.erase(block);
            tracker.stamp(block, tracker.generation, StampKind::Cpu);
        }
        if (gen) {
            static_cast<void>(Gen::Global().SetState(begin, end, Gen::BlockState::Tracked, true));
            Gen::Global().DropCoverage(begin, end);
        }
        restored = true;
    }
    if (restored) unwatchSerial.fetch_add(1, std::memory_order_release);
#else
    static_cast<void>(ranges);
#endif
}

bool ImportWatched(std::uint64_t address, std::size_t bytes, const std::function<bool()>& import, bool staysWatched, bool importMayWriteGuest) {
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return import();
    const census::Scope censusScope("import_watched");
    if (!staysWatched) {
        constexpr std::uint64_t page = 4096;
        const auto first = address & ~(page - 1);
        beforeUnwatch(first, static_cast<std::size_t>(((address + bytes + page - 1) & ~(page - 1)) - first));
    }
    auto& tracker = Tracker();
    auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched) {
        lock.Unlock();
        return import();
    }
    constexpr std::uint64_t page = 4096;
    const auto first = address & ~(page - 1);
    const auto stop = (address + bytes + page - 1) & ~(page - 1);
    const bool covered = tracker.covers(first, static_cast<std::size_t>(stop - first));
    const auto runImport = [&] {
#ifdef __APPLE__
        const OwnHostWrite own;
        std::optional<GuestArena::HostWrite> open;
        if (importMayWriteGuest) open.emplace(reinterpret_cast<void*>(first), static_cast<std::size_t>(stop - first));
#endif
#ifndef __APPLE__
        static_cast<void>(importMayWriteGuest);
#endif
        return import();
    };
    if (!staysWatched) {
        const bool before = covered && walkWrites(tracker, first, stop, StampKind::Cpu, true);
        if (!runImport()) return false;
        if (!before || !walkWrites(tracker, first, stop, StampKind::ImportWindow, true)) unwatchLocked(tracker, first, static_cast<std::size_t>(stop - first));
        unwatchLocked(tracker, address, bytes);
        return true;
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
    if (covered) spans.emplace_back(first, stop);
    else {
        for (auto begin = first; begin < stop;) {
            const auto end = std::min<std::uint64_t>((begin / WriteBlockBytes + 1) * WriteBlockBytes, stop);
            if (tracker.covers(begin, static_cast<std::size_t>(end - begin))) {
                if (!spans.empty() && spans.back().second == begin) spans.back().second = end;
                else spans.emplace_back(begin, end);
            }
            begin = end;
        }
    }
    std::vector<std::uint64_t> hashes(static_cast<std::size_t>((stop - first) / page), 0);
    for (const auto& [begin, end] : spans) CheckRange(reinterpret_cast<const void*>(begin), static_cast<std::size_t>(end - begin), 1, false);
    HashPages(first, spans, hashes);
    const auto failWalk = importWalkFailureForTest.load(std::memory_order_acquire);
    const auto failSpan = importWalkFailureSpanForTest.load(std::memory_order_acquire);
    const auto failsWalk = [&](int walk, std::uint64_t begin, std::uint64_t end) {
        return failWalk == walk && (failSpan == 0 || (begin <= failSpan && failSpan < end));
    };
    const auto leave = [&](const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) {
        if (ranges.empty()) return;
        const TrackerLock::Suspension suspended{lock};
        for (const auto& [begin, end] : ranges) beforeUnwatchUnproven(begin, static_cast<std::size_t>(end - begin));
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> leaving;
    std::erase_if(spans, [&](const auto& span) {
        if (!failsWalk(1, span.first, span.second) && walkWrites(tracker, span.first, span.second, StampKind::Cpu, true)) return false;
        leaving.push_back(span);
        return true;
    });
    leave(leaving);
    if (!runImport()) return false;
    const WindowCopy window{first, hashes};
    leaving.clear();
    std::erase_if(spans, [&](const auto& span) {
        if (failsWalk(2, span.first, span.second) || !walkWrites(tracker, span.first, span.second, StampKind::Cpu, true, &window)) {
            leaving.push_back(span);
            return true;
        }
#ifdef _WIN32
        GuestArena::GuestArenaCompareResidentWhenShared_nid_no_patch(static_cast<std::uintptr_t>(span.first), static_cast<std::size_t>(span.second - span.first));
#endif
        return false;
    });
    leave(leaving);
    auto unwatchedFrom = first;
    for (const auto& [begin, end] : spans) {
        if (unwatchedFrom < begin) unwatchLocked(tracker, unwatchedFrom, static_cast<std::size_t>(begin - unwatchedFrom));
        unwatchedFrom = end;
    }
    if (unwatchedFrom < stop) unwatchLocked(tracker, unwatchedFrom, static_cast<std::size_t>(stop - unwatchedFrom));
    return true;
}

namespace {
// Slice S2 (APS5_ARCHB_GEN_LOCKFREE, requires GEN_TABLE): answers of the tracker's query APIs taken
// from the generation table without the tracker mutex when they are conclusive. The table mirrors
// the tracker from above: every tracker stamp and refusal is published to the table, with the same
// generation, before the tracker stores it (both under the mutex), so at every instant each block's
// table data and refusal generations are at least the tracker's (coverage restores come after their
// table event too). "Every block at most `since`" therefore implies the tracker's own answer; any
// other table answer is inconclusive and the locked path decides, except the answers that need no
// stamp at all (a generation of 0, a range outside write watching), which the locked path gives
// without looking at a stamp either. Coverage (WriteWatchCoverage) and the unstamped-store slots
// are atomics written under the mutex, the arena's host regions are fixed. See the debt notes.
enum class FastAnswer : std::uint8_t { Inconclusive, True, False };

FastAnswer fastUnchanged(const WriteTracker& tracker, std::uint64_t address, std::size_t bytes, std::uint64_t since) {
    if (since == 0 || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address || !tracker.covers(address, bytes)) return FastAnswer::False;
    return Gen::Global().DataAtMost(address, address + bytes, since) ? FastAnswer::True : FastAnswer::Inconclusive;
}

// Whether the calling query may use the lock-free answers at all.
bool lockFree(const WriteTracker& tracker) {
#ifdef _WIN32
    return Gen::LockFreeActive() && tracker.ready.load(std::memory_order_acquire);
#else
    // Write watching outside Windows answers coverage through libc under the tracker mutex.
    static_cast<void>(tracker);
    return false;
#endif
}

// Verify mode (APS5_ARCHB_GEN_VERIFY=N) for a sampled fast answer, under the tracker mutex: every
// tracker section is complete there, so the table's answer must imply the tracker's (unsafe
// otherwise); a lock-free answer that differs while the table under the mutex agrees saw a stamp
// land in between (raced).
void verifyFast(Gen::LockFreeApi api, bool fast, FastAnswer underLock, bool locked, std::uint64_t address, std::uint64_t bytes, std::uint64_t since) {
    Gen::LockFreeVerify outcome = Gen::LockFreeVerify::Agree;
    if (underLock != FastAnswer::Inconclusive && (underLock == FastAnswer::True) != locked) outcome = Gen::LockFreeVerify::Unsafe;
    else if (fast != locked) outcome = Gen::LockFreeVerify::Raced;
    Gen::CountLockFreeVerify(api, outcome, address, bytes, since);
}
}

bool WriteWatched() {
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    return tracker.watched;
}

bool Watched(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return false;
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    return tracker.watched && bytes != 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address && tracker.covers(address, bytes);
}

namespace {
bool lockedUnchangedSince(const WriteTracker& tracker, std::uint64_t address, std::size_t bytes, std::uint64_t generation) {
    if (!tracker.watched || generation == 0 || bytes == 0 || !tracker.covers(address, bytes)) return false;
    return tracker.rangeUnchanged(address, bytes, generation);
}
}

static bool unchangedSinceImpl(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site);

bool UnchangedSince(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site) {
    census::Scope censusScope("unchanged_since", &site);
    const auto result = unchangedSinceImpl(address, bytes, generation, site);
    census::answer(result);
    return result;
}

static bool unchangedSinceImpl(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site) {
    const Stage4::TrackerScope stage4Scope(false, address, bytes);
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return false;
    auto& tracker = Tracker();
    if (lockFree(tracker)) {
        if (const auto fast = fastUnchanged(tracker, address, bytes, generation); fast != FastAnswer::Inconclusive) {
            Gen::CountLockFree(Gen::LockFreeApi::UnchangedSince, true);
            const bool answer = fast == FastAnswer::True;
            const bool shadow = Gen::ShadowActive();
            const bool verify = Gen::VerifyPeriod() != 0 && Gen::VerifySample();
            if (shadow || verify) {
                const auto lock = lockTracker(tracker);
                if (verify) verifyFast(Gen::LockFreeApi::UnchangedSince, answer, fastUnchanged(tracker, address, bytes, generation), lockedUnchangedSince(tracker, address, bytes, generation), address, bytes, generation);
                if (shadow) shadowQuery(tracker, address, bytes, generation, generation != 0 && tracker.covers(address, bytes), answer, site);
            }
            return answer;
        }
        Gen::CountLockFree(Gen::LockFreeApi::UnchangedSince, false);
    }
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    const bool shadow = Gen::ShadowActive();
    if (!tracker.watched || generation == 0 || bytes == 0 || !tracker.covers(address, bytes)) {
        if (shadow) shadowQuery(tracker, address, bytes, generation, false, false, site);
        return false;
    }
    const bool unchanged = tracker.rangeUnchanged(address, bytes, generation);
    if (shadow) shadowQuery(tracker, address, bytes, generation, true, unchanged, site);
    return unchanged;
}

namespace {
// UnchangedSinceAll / UnchangedSinceClean: one decision over every query.
// `writesClean`: UnchangedSinceClean also asks libc whether the pages saw no write since their last
// collect (a non-resetting probe, and no pinned writable alias): a range it reports written is what
// a sweep at the ticket would find, so the table's verdict there is unknown_not_swept, never Clean.
void shadowQueries(const WriteTracker& tracker, std::span<const UnchangedQuery> queries, bool unchanged, const std::source_location& site, bool writesClean = false) {
    if (queries.empty()) return;
    auto& ranges = shadowScratch();
    ranges.clear();
    bool unswept = false;
    for (const auto& [address, bytes, generation] : queries) {
        const bool covered = tracker.watched && generation != 0 && bytes != 0 && bytes <= UINT64_MAX - address && tracker.covers(address, bytes);
        ranges.push_back({address, bytes, generation, covered});
#ifdef _WIN32
        if (writesClean && covered && !GuestArena::GuestArenaWritesClean_nid_no_patch(static_cast<std::uintptr_t>(address), bytes)) unswept = true;
#endif
    }
    if (!unswept) {
        shadowRanges(tracker, ranges, unchanged, site);
        return;
    }
    ShadowVerdict combined;
    for (const auto& range : ranges) combineVerdict(combined, shadowCheck(tracker, range.address, range.bytes, range.since, range.covered));
    if (combined.verdict == Gen::Verdict::Clean || combined.verdict == Gen::Verdict::PendingOnly) {
        combined.verdict = Gen::Verdict::Unknown;
        combined.reason = Gen::UnknownReason::NotSwept;
        combined.needsSweep = true;
    }
    classifyShadow(tracker, ranges, unchanged, combined, site);
}
}

namespace {
bool lockedUnchangedSinceAll(const WriteTracker& tracker, std::span<const UnchangedQuery> queries) {
    if (!tracker.watched) return false;
    for (const auto& [address, bytes, generation] : queries) {
        if (generation == 0 || bytes == 0 || !tracker.covers(address, bytes)) return false;
        if (!tracker.rangeUnchanged(address, bytes, generation)) return false;
    }
    return true;
}

// False as soon as one query is conclusively false (the conjunction is false whatever the others
// say), true only when every query is conclusively true.
FastAnswer fastUnchangedAll(const WriteTracker& tracker, std::span<const UnchangedQuery> queries) {
    bool inconclusive = false;
    for (const auto& [address, bytes, generation] : queries) {
        const auto answer = fastUnchanged(tracker, address, bytes, generation);
        if (answer == FastAnswer::False) return FastAnswer::False;
        inconclusive = inconclusive || answer == FastAnswer::Inconclusive;
    }
    return inconclusive ? FastAnswer::Inconclusive : FastAnswer::True;
}
}

static bool unchangedSinceAllImpl(std::span<const UnchangedQuery> queries, std::source_location site);

bool UnchangedSinceAll(std::span<const UnchangedQuery> queries, std::source_location site) {
    census::Scope censusScope("unchanged_since_all", &site);
    const auto result = unchangedSinceAllImpl(queries, site);
    census::answer(result);
    return result;
}

static bool unchangedSinceAllImpl(std::span<const UnchangedQuery> queries, std::source_location site) {
    Stage4::TrackerScope stage4Scope(false, 0, 0);
    if (Stage4::ResolveCensus()) for (const auto& query : queries) stage4Scope.AddRead(query.address, query.bytes);
    auto& tracker = Tracker();
    if (lockFree(tracker)) {
        if (const auto fast = fastUnchangedAll(tracker, queries); fast != FastAnswer::Inconclusive) {
            Gen::CountLockFree(Gen::LockFreeApi::UnchangedSinceAll, true);
            const bool answer = fast == FastAnswer::True;
            const bool shadow = Gen::ShadowActive();
            const bool verify = Gen::VerifyPeriod() != 0 && Gen::VerifySample();
            if (shadow || verify) {
                const auto lock = lockTracker(tracker);
                if (verify) verifyFast(Gen::LockFreeApi::UnchangedSinceAll, answer, fastUnchangedAll(tracker, queries), lockedUnchangedSinceAll(tracker, queries), queries.empty() ? 0 : queries.front().address, queries.empty() ? 0 : queries.front().bytes, queries.empty() ? 0 : queries.front().generation);
                if (shadow) shadowQueries(tracker, queries, answer, site);
            }
            return answer;
        }
        Gen::CountLockFree(Gen::LockFreeApi::UnchangedSinceAll, false);
    }
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    const auto answer = lockedUnchangedSinceAll(tracker, queries);
    if (Gen::ShadowActive()) shadowQueries(tracker, queries, answer, site);
    return answer;
}

bool UnchangedSinceClean(std::span<const UnchangedQuery> queries, std::source_location site) {
#ifdef _WIN32
    const census::Scope censusScope("unchanged_clean", &site);
    Stage4::TrackerScope stage4Scope(false, 0, 0);
    if (Stage4::ResolveCensus()) for (const auto& query : queries) stage4Scope.AddRead(query.address, query.bytes);
    if (queries.empty()) return false;
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched) return false;
    const auto mapping = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    const auto committed = GuestArena::GuestArenaCommitGeneration_nid_postfix();
    const auto forgotten = ForgetSerial();
    if ((forgotten & 1) != 0) return false;
    const auto answer = [&]() {
        for (const auto& [address, bytes, generation] : queries) {
            if (generation == 0 || bytes == 0 || bytes > UINT64_MAX - address || !tracker.covers(address, bytes)) return false;
            if (!tracker.rangeUnchanged(address, bytes, generation) || !GuestArena::GuestArenaWritesClean_nid_no_patch(static_cast<std::uintptr_t>(address), bytes)) return false;
        }
        return mapping == GuestAllocations::GuestAllocationsGeneration_nid_postfix() && committed == GuestArena::GuestArenaCommitGeneration_nid_postfix() && forgotten == ForgetSerial();
    }();
    if (Gen::ShadowActive()) shadowQueries(tracker, queries, answer, site, true);
    return answer;
#else
    static_cast<void>(queries);
    static_cast<void>(site);
    return false;
#endif
}

std::uint64_t CollectCommittedWrites(std::uint64_t address, std::size_t bytes, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, std::source_location site) {
    const census::Scope censusScope("committed", &site);
    ranges.clear();
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address || !WriteWatched()) return 0;
    auto committed = CommittedRanges(address, bytes);
    auto generation = TrackerGeneration();
    for (const auto& [begin, end] : committed) {
        generation = CollectWrites(begin, static_cast<std::size_t>(end - begin));
        if (generation == 0) return 0;
    }
    ranges = std::move(committed);
    return generation;
}

std::uint64_t CollectCommittedWritesInPlace(std::uint64_t address, std::size_t bytes, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, std::source_location site) {
    const census::Scope censusScope("committed", &site);
    ranges.clear();
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address || !WriteWatched()) return 0;
    static_cast<void>(DescribeCommittedInto(address, bytes, false, ranges));
    auto generation = TrackerGeneration();
    for (const auto& [begin, end] : ranges) {
        generation = CollectWrites(begin, static_cast<std::size_t>(end - begin));
        if (generation == 0) {
            ranges.clear();
            return 0;
        }
    }
    return generation;
}

namespace {
// APS5_ARCHB_FIX_UNSTAMPED_ALIAS: UnchangedCommittedSince reads a committed range as changed when any
// of its blocks' unstamped-store slots is newer than the generation. Those slots record driver stores
// into memory outside write watching, hashed by block modulo 4 GiB (see noteUnstampedStore), so a
// store into unwatched memory 4 GiB * k away from a watched range advances its slots too, and a GPU
// write made every frame at such an address reads a large partially resident snapshot (the zoo's 352
// MiB texture pool at 0x1009000000) as changed every frame: it is snapshotted, detiled and uploaded
// again per frame (60 -> 205 ms/frame) in the sessions whose unwatched GPU targets land on such an
// address. The ranges asked about are write-watched in full (checked first), and a store into watched
// memory always stamps the blocks it touches (MarkWritten and StoreOwnBytes stamp the watched part of a
// partly watched store; a coverage restore, Rewatch or a new private mapping, stamps every block it
// restores after any store made while the range was unwatched), so for these ranges the block stamps
// alone decide; the slots add only aliases. With the switch on they are ignored here; a slot that is
// newer is still checked: when the store that last set it overlaps the range in a block not stamped at
// or after it (impossible by the argument above) the answer stays "changed" and the mismatch is counted
// (fix_unstamped_alias_mismatches). fix_unstamped_alias_aliased counts the answers only the slots made
// "changed" (on or off), fix_unstamped_alias_served those served with the switch on. The same switch
// keeps the slots out of NewestStamp (a cache key) and StoredAllSince for ranges watched in full.
UnstampedAliasCounters& unstampedAliasCounters() {
    static auto* made = [] {
        auto* counts = new UnstampedAliasCounters();
        ArchB::Recorder::RegisterCounter("fix_unstamped_alias_aliased", counts->aliased);
        ArchB::Recorder::RegisterCounter("fix_unstamped_alias_served", counts->served);
        ArchB::Recorder::RegisterCounter("fix_unstamped_alias_mismatches", counts->mismatches);
        return counts;
    }();
    return *made;
}

// Whether the newer slots of a fully watched committed range are aliases: no block holding one has its
// last recorded store overlapping the range's part of the block without a block stamp at or after it.
bool unstampedSlotsAlias(const WriteTracker& tracker, std::uint64_t begin, std::uint64_t end, std::uint64_t generation) {
    const auto* slots = tracker.unstampedStores.load(std::memory_order_acquire);
    if (slots == nullptr) return true;
    const auto count = WriteTracker::unstampedSlotCount();
    for (auto block = begin / WriteBlockBytes; block <= (end - 1) / WriteBlockBytes; ++block) {
        const auto value = slots[block % count].load(std::memory_order_acquire);
        if (value <= generation) continue;
        const auto [storeBegin, storeEnd] = tracker.unstampedRanges[block % count];
        const auto from = std::max<std::uint64_t>(begin, block * WriteBlockBytes);
        const auto to = std::min<std::uint64_t>(end, (block + 1) * WriteBlockBytes);
        if (storeBegin < to && from < storeEnd && tracker.stampOf(tracker.blockOf(from)) < value) return false;
    }
    return true;
}

void reportUnstampedAlias(const WriteTracker& tracker, std::uint64_t begin, std::uint64_t end, std::uint64_t generation, std::uint64_t served) {
    if (served > 8 && served % 4096 != 0) return;
    const auto slot = tracker.newestUnstampedSlot(begin, end);
    std::fprintf(stderr, "[guestmem] FIX_UNSTAMPED_ALIAS: committed range 0x%llx..0x%llx (%llu KiB) unchanged since generation %llu (newest block stamp %llu) read as changed only by the unstamped slot of block 0x%llx (%llu), last set by a driver store to unwatched 0x%llx..0x%llx (%llu MiB away): served; %llu served so far\n",
                 static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end), static_cast<unsigned long long>((end - begin) >> 10u), static_cast<unsigned long long>(generation),
                 static_cast<unsigned long long>(tracker.newestBlockStamp(begin, end)), static_cast<unsigned long long>(slot.block * WriteBlockBytes), static_cast<unsigned long long>(slot.value),
                 static_cast<unsigned long long>(slot.store.first), static_cast<unsigned long long>(slot.store.second),
                 static_cast<unsigned long long>((slot.store.first > slot.block * WriteBlockBytes ? slot.store.first - slot.block * WriteBlockBytes : slot.block * WriteBlockBytes - slot.store.first) >> 20u),
                 static_cast<unsigned long long>(served));
}

bool lockedUnchangedCommittedSince(const WriteTracker& tracker, std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uint64_t generation) {
    if (!tracker.watched || generation == 0) return false;
    std::pair<std::uint64_t, std::uint64_t> aliased{};
    for (const auto& [begin, end] : ranges) {
        if (end <= begin || !tracker.covers(begin, static_cast<std::size_t>(end - begin)) || tracker.newestBlockStamp(begin, end) > generation || tracker.refusedSince(begin, end, generation)) return false;
        if (tracker.newestUnstamped(begin, end) <= generation) continue;
        auto& counts = unstampedAliasCounters();
        counts.aliased.fetch_add(1, std::memory_order_relaxed);
        if (!unstampedAliasActive()) return false;
        if (!unstampedSlotsAlias(tracker, begin, end, generation)) {
            counts.mismatches.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (aliased.second == 0) aliased = {begin, end};
    }
    if (aliased.second != 0) reportUnstampedAlias(tracker, aliased.first, aliased.second, generation, unstampedAliasCounters().served.fetch_add(1, std::memory_order_relaxed) + 1);
    return true;
}

// newestStamp also reads the unstamped-store slots (driver stores outside write watching, hashed by
// block); they are atomics, read here as the locked path reads them.
FastAnswer fastUnchangedCommittedSince(const WriteTracker& tracker, std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uint64_t generation) {
    if (generation == 0) return FastAnswer::False;
    bool inconclusive = false;
    for (const auto& [begin, end] : ranges) {
        if (end <= begin || !tracker.covers(begin, static_cast<std::size_t>(end - begin))) return FastAnswer::False;
        if (!inconclusive && (!Gen::Global().DataAtMost(begin, end, generation) || tracker.newestUnstamped(begin, end) > generation)) inconclusive = true;
    }
    return inconclusive ? FastAnswer::Inconclusive : FastAnswer::True;
}
}

static bool unchangedCommittedSinceImpl(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uint64_t generation, std::source_location site);

bool UnchangedCommittedSince(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uint64_t generation, std::source_location site) {
    census::Scope censusScope("unchanged_committed", &site);
    const auto result = unchangedCommittedSinceImpl(ranges, generation, site);
    census::answer(result);
    return result;
}

static bool unchangedCommittedSinceImpl(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uint64_t generation, std::source_location site) {
    auto& tracker = Tracker();
    const auto shadowFast = [&](bool answer) {
        if (!Gen::ShadowActive() || ranges.empty()) return;
        auto& shadow = shadowScratch();
        shadow.clear();
        for (const auto& [begin, end] : ranges) {
            const auto bytes = static_cast<std::size_t>(end > begin ? end - begin : 0);
            const bool covered = tracker.watched && generation != 0 && bytes != 0 && tracker.covers(begin, bytes);
            shadow.push_back({begin, bytes, generation, covered});
        }
        shadowRanges(tracker, shadow, answer, site);
    };
    if (lockFree(tracker)) {
        if (const auto fast = fastUnchangedCommittedSince(tracker, ranges, generation); fast != FastAnswer::Inconclusive) {
            Gen::CountLockFree(Gen::LockFreeApi::UnchangedCommittedSince, true);
            const bool answer = fast == FastAnswer::True;
            const bool shadow = Gen::ShadowActive();
            const bool verify = Gen::VerifyPeriod() != 0 && Gen::VerifySample();
            if (shadow || verify) {
                const auto lock = lockTracker(tracker);
                if (verify) verifyFast(Gen::LockFreeApi::UnchangedCommittedSince, answer, fastUnchangedCommittedSince(tracker, ranges, generation), lockedUnchangedCommittedSince(tracker, ranges, generation), ranges.empty() ? 0 : ranges.front().first, ranges.empty() ? 0 : ranges.front().second - ranges.front().first, generation);
                shadowFast(answer);
            }
            return answer;
        }
        Gen::CountLockFree(Gen::LockFreeApi::UnchangedCommittedSince, false);
    }
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    const auto answer = lockedUnchangedCommittedSince(tracker, ranges, generation);
    shadowFast(answer);
    return answer;
}

namespace {
std::atomic<void (*)(std::uint64_t, std::size_t)> lazyStoreHook{nullptr};
std::atomic<void (*)(std::uint64_t, std::size_t)> deferredFenceHook{nullptr};
std::atomic<bool> ownStoreCompare{false};
}

void SetLazyStoreHook(void (*hook)(std::uint64_t, std::size_t)) {
    lazyStoreHook.store(hook, std::memory_order_release);
}

void SetDeferredFenceHook(void (*hook)(std::uint64_t, std::size_t)) {
    deferredFenceHook.store(hook, std::memory_order_release);
}

void SetOwnStoreCompare(bool compare) {
    ownStoreCompare.store(compare, std::memory_order_release);
}

static GuestAllocations::Lease::Ranges pinStoreRanges(std::uint64_t address, std::size_t bytes) {
    GuestAllocations::Lease::Ranges pinned;
    if (bytes > std::numeric_limits<std::uint64_t>::max() - address) return pinned;
    const auto end = address + bytes;
    for (auto at = address; at < end;) {
        auto range = GuestAllocations::GuestAllocationsLookup_nid_postfix(at, true);
        if (range == nullptr || range->address >= end) break;
        at = range->address + range->bytes;
        pinned.push_back(std::move(range));
    }
    return pinned;
}

std::uint64_t storeOwn(std::uint64_t address, std::size_t bytes, bool imaged, const std::function<std::pair<std::uint64_t, std::uint64_t>(std::span<std::byte>)>& store, bool complete = true) {
    if (bytes == 0) return 0;
    const auto pinned = pinStoreRanges(address, bytes);
    if (const auto hook = lazyStoreHook.load(std::memory_order_acquire)) hook(address, bytes);
    if (const auto fence = deferredFenceHook.load(std::memory_order_acquire)) fence(address, bytes);
    const census::Scope censusScope("store_own");
    const Stage4::TrackerScope stage4Scope(true, address, bytes);
    Stage4::NoteDriverWrite(address, bytes);
    if (bytes <= wtFault::DriverHotBytes && wtFault::active.load(std::memory_order_relaxed)) GuestArena::GuestWtFaultNoteDriverStore_nid_no_patch(static_cast<std::uintptr_t>(address), bytes);
    BreakProofs();
    const auto stampStored = [complete](WriteTracker& tracker, std::pair<std::uint64_t, std::uint64_t> stored) -> std::uint64_t {
        if (stored.second <= stored.first) return 0;
        tracker.bump();
        stampTrace::note("store-own", stored.first, stored.second, tracker.generation.load(std::memory_order_relaxed), -1);
        noteDriverStoreGen(stored.first, stored.second, tracker.generation.load(std::memory_order_relaxed));
        if (!tracker.watched || !tracker.covers(stored.first, static_cast<std::size_t>(stored.second - stored.first))) {
            tracker.noteUnstampedStore(stored.first, stored.second);
            if (tracker.watched) tracker.stampWatchedPart(stored.first, stored.second, tracker.generation, complete);
            return 0;
        }
        for (auto block = tracker.blockOf(stored.first); block <= tracker.blockOf(stored.second - 1); ++block) {
            tracker.stamp(block, tracker.generation, StampKind::Driver);
            tracker.noteDriverStore(block, stored.first, stored.second, tracker.generation, complete);
        }
        return tracker.generation;
    };
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched || bytes > std::numeric_limits<std::uint64_t>::max() - address || !tracker.covers(address, bytes)) return stampStored(tracker, store({}));
    constexpr std::uint64_t page = 4096;
    const auto first = address & ~(page - 1);
    const auto stop = (address + bytes + page - 1) & ~(page - 1);
    const bool compare = ownStoreCompare.load(std::memory_order_acquire);
    std::vector<std::byte> image;
    std::optional<WindowCopy> window;
    if (compare) {
        if (imaged) {
            image.resize(static_cast<std::size_t>(stop - first));
            static_cast<void>(CopyMapped(first, image));
        }
        window = WindowCopy{first, {}, image, StampKind::Driver};
    }
    if (!walkWrites(tracker, first, stop, StampKind::Cpu)) return stampStored(tracker, store({}));
    const auto stored = store(image.empty() ? std::span<std::byte>{} : std::span<std::byte>(image).subspan(static_cast<std::size_t>(address - first), bytes));
    walkWrites(tracker, first, stop, StampKind::Driver, false, window ? &*window : nullptr);
    return stampStored(tracker, stored);
}

std::uint64_t StoreOwnBytes(std::uint64_t address, std::size_t bytes, const std::function<void()>& store) {
    return storeOwn(address, bytes, false, [&](std::span<std::byte>) {
        store();
        return std::pair<std::uint64_t, std::uint64_t>{address, address + bytes};
    });
}

std::uint64_t StoreOwnBytes(std::uint64_t address, std::size_t bytes, const std::function<void(std::span<std::byte>)>& store) {
    return storeOwn(address, bytes, true, [&](std::span<std::byte> image) {
        store(image);
        return std::pair<std::uint64_t, std::uint64_t>{address, address + bytes};
    });
}

static void copyOwnBytes(std::uint64_t address, std::span<const std::byte> source) {
#ifndef _WIN32
    if (GuestWriteWatch::GuestWriteWatchAvailable_nid_postfix() && GuestArena::GuestArenaWriteSharedBacking_nid_no_patch(address, source.data(), source.size())) return;
#endif
    std::memcpy(reinterpret_cast<void*>(address), source.data(), source.size());
}

std::uint64_t StoreOwnBytes(std::uint64_t address, std::span<const std::byte> source) {
    return StoreOwnBytes(address, source.size(), [&](std::span<std::byte> image) {
        copyOwnBytes(address, source);
        if (!image.empty()) std::memcpy(image.data(), source.data(), source.size());
    });
}

static std::uint64_t markWritten(std::uint64_t address, std::size_t bytes, bool complete, std::uint64_t* order = nullptr) {
    BreakProofs();
    auto& tracker = Tracker();
    auto lock = lockTracker(tracker);
    if (order != nullptr) *order = 0;
    tracker.initialize();
    if (bytes == 0) return 0;
    tracker.bump();
    if (!tracker.watched || !tracker.covers(address, bytes)) {
        noteDriverStoreGen(address, address + bytes, tracker.generation.load(std::memory_order_relaxed));
        tracker.noteUnstampedStore(address, address + bytes);
        if (tracker.watched) tracker.stampWatchedPart(address, address + bytes, tracker.generation, complete);
        if (order != nullptr) *order = tracker.generation.load(std::memory_order_relaxed);
        return 0;
    }
    {
        const auto blockFirst = address & ~(std::uint64_t{WriteBlockBytes} - 1);
        const auto blockStop = (address + bytes + WriteBlockBytes - 1) & ~(std::uint64_t{WriteBlockBytes} - 1);
        StampRuns runs{tracker, StampKind::Fresh};
#ifdef _WIN32
        const bool collected = collectWindowsWrites(runs, blockFirst, blockStop, false);
#else
        const bool collected = GuestWriteWatch::GuestWriteWatchCollectFresh_nid_postfix(static_cast<std::uintptr_t>(blockFirst), static_cast<std::size_t>(blockStop - blockFirst), &stampWrittenRun, &runs);
#endif
        if (!collected) {
            {
                const TrackerLock::Suspension suspended{lock};
                beforeUnwatchUnproven(blockFirst, static_cast<std::size_t>(blockStop - blockFirst));
            }
            tracker.bump();
            unwatchLocked(tracker, blockFirst, static_cast<std::size_t>(blockStop - blockFirst));
            // Another holder may have published the generation bumped above while the mutex was
            // released (TrackerGeneration, collect memo hits): stamp with a fresh one.
            tracker.bump();
            noteDriverStoreGen(address, address + bytes, tracker.generation.load(std::memory_order_relaxed));
            tracker.noteUnstampedStore(address, address + bytes);
            if (order != nullptr) *order = tracker.generation.load(std::memory_order_relaxed);
            return 0;
        }
        if (runs.stamped != 0) tracker.bump();
    }
    const auto first = tracker.blockOf(address);
    const auto last = tracker.blockOf(address + bytes - 1);
    stampTrace::note("mark-written", address, address + bytes, tracker.generation.load(std::memory_order_relaxed), complete ? -2 : -3);
    noteDriverStoreGen(address, address + bytes, tracker.generation.load(std::memory_order_relaxed));
    for (auto block = first; block <= last; ++block) {
        tracker.stamp(block, tracker.generation, StampKind::Driver);
        tracker.noteDriverStore(block, address, address + bytes, tracker.generation, complete);
    }
    const auto generation = tracker.generation.load(std::memory_order_relaxed);
    if (order != nullptr) *order = generation;
    return generation;
}

std::uint64_t MarkWritten(std::uint64_t address, std::size_t bytes, std::source_location site) {
    const census::Scope censusScope("mark_written", &site);
    const Stage4::TrackerScope stage4Scope(true, address, bytes);
    Stage4::NoteDriverWrite(address, bytes);
    if (bytes <= wtFault::DriverHotBytes && wtFault::active.load(std::memory_order_relaxed)) GuestArena::GuestWtFaultNoteDriverStore_nid_no_patch(static_cast<std::uintptr_t>(address), bytes);
    return markWritten(address, bytes, true);
}

std::uint64_t MarkPossiblyWritten(std::uint64_t address, std::size_t bytes, std::source_location site) {
    const census::Scope censusScope("mark_written", &site);
    const Stage4::TrackerScope stage4Scope(true, address, bytes);
    Stage4::NoteDriverWrite(address, bytes);
    if (bytes <= wtFault::DriverHotBytes && wtFault::active.load(std::memory_order_relaxed)) GuestArena::GuestWtFaultNoteDriverStore_nid_no_patch(static_cast<std::uintptr_t>(address), bytes);
    return markWritten(address, bytes, false);
}

std::uint64_t MarkPossiblyWritten(std::uint64_t address, std::size_t bytes, std::uint64_t& order, std::source_location site) {
    const census::Scope censusScope("mark_written", &site);
    const Stage4::TrackerScope stage4Scope(true, address, bytes);
    Stage4::NoteDriverWrite(address, bytes);
    if (bytes <= wtFault::DriverHotBytes && wtFault::active.load(std::memory_order_relaxed)) GuestArena::GuestWtFaultNoteDriverStore_nid_no_patch(static_cast<std::uintptr_t>(address), bytes);
    return markWritten(address, bytes, false, &order);
}

namespace {
bool lockedStoredOver(const WriteTracker& tracker, std::uint64_t address, std::size_t bytes, std::uint64_t generation) {
    if (!tracker.watched || generation == 0 || bytes == 0 || !tracker.covers(address, bytes)) return true;
#ifdef _WIN32
    if (bookkeeping::hostRegionOverlaps(address, bytes)) return true;
#endif
    return tracker.storedOver(address, address + bytes, generation, false);
}

// storedOver only looks at blocks stamped after the generation: none (every block at most it) is
// "not stored over".
FastAnswer fastStoredOver(const WriteTracker& tracker, std::uint64_t address, std::size_t bytes, std::uint64_t generation) {
    if (generation == 0 || bytes == 0 || !tracker.covers(address, bytes)) return FastAnswer::True;
    return Gen::Global().DataAtMost(address, address + bytes, generation) ? FastAnswer::False : FastAnswer::Inconclusive;
}
}

static bool storedOverImpl(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site);

bool StoredOver(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site) {
    census::Scope censusScope("stored_over", &site);
    const auto result = storedOverImpl(address, bytes, generation, site);
    census::answer(!result);
    return result;
}

static bool storedOverImpl(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site) {
    static_cast<void>(site);
    auto& tracker = Tracker();
    if (lockFree(tracker)) {
        if (const auto fast = fastStoredOver(tracker, address, bytes, generation); fast != FastAnswer::Inconclusive) {
            Gen::CountLockFree(Gen::LockFreeApi::StoredOver, true);
            const bool answer = fast == FastAnswer::True;
            if (Gen::VerifyPeriod() != 0 && Gen::VerifySample()) {
                const auto lock = lockTracker(tracker);
                verifyFast(Gen::LockFreeApi::StoredOver, answer, fastStoredOver(tracker, address, bytes, generation), lockedStoredOver(tracker, address, bytes, generation), address, bytes, generation);
            }
            return answer;
        }
        Gen::CountLockFree(Gen::LockFreeApi::StoredOver, false);
    }
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    return lockedStoredOver(tracker, address, bytes, generation);
}

bool StoredAllSince_nid_no_patch(std::uint64_t address, std::size_t bytes, std::uint64_t generation) {
    if (generation == 0 || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return false;
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched || !tracker.covers(address, bytes)) return false;
    const auto end = address + bytes;
    // The range is watched in full: with FIX_UNSTAMPED_ALIAS its slots (aliases only) do not decide.
    if (!unstampedAliasActive() && tracker.newestUnstamped(address, end) > generation) return false;
    for (auto block = tracker.blockOf(address); block <= tracker.blockOf(end - 1u); ++block) {
        const auto found = tracker.driverPieces.find(block);
        if (found == tracker.driverPieces.end()) return false;
        const auto start = tracker.blockBegin(block);
        const auto from = static_cast<std::uint32_t>(std::max(address, start) - start);
        const auto to = static_cast<std::uint32_t>(std::min<std::uint64_t>(end, start + WriteBlockBytes) - start);
        if (!found->second.covers(from, to, generation)) return false;
    }
    return true;
}

std::uint64_t TrackerGeneration() {
    auto& tracker = Tracker();
    // GEN_LOCKFREE: the generation published at the last mutex release (TrackerLock::publish), which
    // is what the locked read returns once the section running now (if any) has finished: every
    // stamp at or below it is complete and a walk still running stamps above it. Not GenClock's
    // current value: a walk takes its generation before its OS query, so a clock value read while
    // it runs would let it stamp stores made after the read with a generation not newer than it.
    if (Gen::LockFreeActive()) {
        Gen::CountLockFree(Gen::LockFreeApi::TrackerGeneration, true);
        return tracker.stable.load(std::memory_order_acquire);
    }
    // Under the mutex: a collect bumps the generation before its walk, and a walk still running
    // when the value is read absorbs stores made after it with a stamp not newer than the value.
    const auto lock = lockTracker(tracker);
    return tracker.generation;
}

void AdvanceTrackerGenerationForTest(std::uint64_t generation) {
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    if (generation > tracker.generation.load(std::memory_order_relaxed)) tracker.generation.store(generation, std::memory_order_relaxed);
    // The fault tracker's clock follows (it hands out the generations in APS5_ARCHB_WT_FAULT mode).
    if (tracker.faultMode) GuestArena::GuestWtFaultEnable_nid_no_patch(wtFault::verifyOn(), generation);
}

std::uint64_t NewestStamp(std::uint64_t address, std::size_t bytes) {
    auto& tracker = Tracker();
    // A value, compared for equality as a cache key: the table's newest data is an upper bound of
    // the tracker's stamp, not the stamp, so no lock-free answer is conclusive (counted slow).
    if (lockFree(tracker)) Gen::CountLockFree(Gen::LockFreeApi::NewestStamp, false);
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    return bytes == 0 ? 0 : tracker.newestStamp(address, address + bytes);
}

StampSplit SplitNewestStamp(std::uint64_t address, std::size_t bytes) {
    StampSplit split;
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return split;
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    split.block = tracker.newestBlockStamp(address, address + bytes);
    const auto slot = tracker.newestUnstampedSlot(address, address + bytes);
    split.unstamped = slot.value;
    split.slotAddress = slot.block * WriteBlockBytes;
    split.storeBegin = slot.store.first;
    split.storeEnd = slot.store.second;
    return split;
}

UnstampedAliasCounters& UnstampedAlias() {
    return unstampedAliasCounters();
}

#ifdef _WIN32
namespace {

ThreadCollectMemo& threadHintMemoThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<ThreadCollectMemo, Slot>();
}

bool walkHints(WriteTracker& tracker, std::uint64_t first, std::uint64_t stop) {
    census::noteWalk(first, stop);
    auto cursor = first;
    bool marked = false;
    StampRuns runs{tracker, StampKind::Cpu};
    while (cursor < stop) {
        std::size_t count = tracker.pages.size();
        if (!GuestArena::GuestArenaCollectWrites_nid_postfix(static_cast<std::uintptr_t>(cursor), static_cast<std::size_t>(stop - cursor), tracker.pages.data(), &count, true)) return false;
        if (count != 0 && !marked) tracker.bump();
        marked = marked || count != 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto page = reinterpret_cast<std::uintptr_t>(tracker.pages[i]);
            tracker.stampHint(tracker.blockOf(page));
            if (tracker.coverage.Covers(page, WritePageBytes)) stampWrittenRun(&runs, page, page + WritePageBytes);
        }
        if (count < tracker.pages.size()) break;
        cursor = reinterpret_cast<std::uintptr_t>(tracker.pages[count - 1]) + WritePageBytes;
    }
    if (marked) collectDirty.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}
#endif

std::uint64_t UntrackedHint(std::uint64_t address, std::size_t bytes, std::source_location site) {
#ifdef _WIN32
    const census::Scope censusScope("untracked_hint", &site);
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return 0;
    constexpr std::uint64_t page = 4096;
    const auto first = address & ~(page - 1);
    const auto stop = (address + bytes + page - 1) & ~(page - 1);
    const auto epoch = currentCollectEpoch();
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched || !tracker.hinted(first, stop)) return 0;
    const auto unwatched = unwatchSerial.load(std::memory_order_relaxed);
    const auto mapping = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    const bool memoized = collectMemoEnabled() && std::any_of(threadHintMemoThreadState().entries.begin(), threadHintMemoThreadState().entries.end(), [&](const WriteTracker::Memo& entry) {
        return entry.epoch == epoch && entry.unwatched == unwatched && entry.mapping == mapping && entry.begin <= first && stop <= entry.end;
    });
    if (memoized) {
        collectMemoHits.fetch_add(1, std::memory_order_relaxed);
    } else {
        const TimedAccess timed(CounterCollect, bytes);
        if (!walkHints(tracker, first, stop)) return 0;
        if (collectMemoEnabled()) threadHintMemoThreadState().entries[threadHintMemoThreadState().next++ % threadHintMemoThreadState().entries.size()] = {first, stop, epoch, unwatched, mapping};
    }
    return tracker.newestHint(first, stop);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(site);
    return 0;
#endif
}

bool UnchangedSinceCollected(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site) {
    const census::Scope censusScope("unchanged_since_collected", &site);
    const Stage4::TrackerScope stage4Scope(false, address, bytes);
    if (CollectWritesUncached(address, bytes) == 0) return false;
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    if (!tracker.watched || generation == 0 || bytes == 0 || !tracker.covers(address, bytes)) return false;
    return tracker.cpuUnchanged(address, bytes, generation);
}

bool UnchangedSinceLastCollect(std::uint64_t address, std::size_t bytes, std::uint64_t generation) {
    if (bytes == 0u || bytes > std::numeric_limits<std::uint64_t>::max() - address) return false;
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched || generation == 0u || !tracker.covers(address, bytes)) return false;
    return tracker.cpuUnchanged(address, bytes, generation) && tracker.rangeUnchanged(address, bytes, generation, false);
}

namespace {
bool lockedWrittenSince(const WriteTracker& tracker, std::uint64_t address, std::size_t bytes, std::uint64_t generation) {
    if (!tracker.watched || generation == 0 || bytes == 0 || !tracker.covers(address, bytes)) return false;
    const auto first = tracker.blockOf(address);
    const auto last = tracker.blockOf(address + bytes - 1);
    for (auto block = first; block <= last; ++block) {
        if (tracker.stampOf(block) > generation) return true;
    }
    return false;
}

FastAnswer fastWrittenSince(const WriteTracker& tracker, std::uint64_t address, std::size_t bytes, std::uint64_t generation) {
    if (generation == 0 || bytes == 0 || !tracker.covers(address, bytes)) return FastAnswer::False;
    return Gen::Global().DataAtMost(address, address + bytes, generation) ? FastAnswer::False : FastAnswer::Inconclusive;
}
}

static bool writtenSinceImpl(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site);

bool WrittenSince(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site) {
    census::Scope censusScope("written_since", &site);
    const auto result = writtenSinceImpl(address, bytes, generation, site);
    census::answer(!result);
    return result;
}

static bool writtenSinceImpl(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::source_location site) {
    static_cast<void>(site);
    auto& tracker = Tracker();
    if (lockFree(tracker)) {
        if (const auto fast = fastWrittenSince(tracker, address, bytes, generation); fast != FastAnswer::Inconclusive) {
            Gen::CountLockFree(Gen::LockFreeApi::WrittenSince, true);
            const bool answer = fast == FastAnswer::True;
            if (Gen::VerifyPeriod() != 0 && Gen::VerifySample()) {
                const auto lock = lockTracker(tracker);
                verifyFast(Gen::LockFreeApi::WrittenSince, answer, fastWrittenSince(tracker, address, bytes, generation), lockedWrittenSince(tracker, address, bytes, generation), address, bytes, generation);
            }
            return answer;
        }
        Gen::CountLockFree(Gen::LockFreeApi::WrittenSince, false);
    }
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    return lockedWrittenSince(tracker, address, bytes, generation);
}

namespace {
// Per block: a generation of 0 reads written without a stamp; a block whose table data is at most
// its generation is unchanged and not CPU-written (every table cpu event also bumps data, and the
// tracker's cpu stamps are a subset of its stamps). Any other block makes the whole call
// inconclusive (its outputs are refilled by the locked path).
FastAnswer fastChangedBlocks(const WriteTracker& tracker, std::uint64_t address, std::size_t bytes, std::span<const std::uint64_t> generations, std::span<std::uint8_t> changed, std::span<std::uint8_t> cpu) {
    if (bytes == 0 || !tracker.covers(address, bytes)) return FastAnswer::False;
    auto& table = Gen::Global();
    const auto floor = table.ResetFloor();
    const auto first = tracker.blockOf(address);
    const auto last = tracker.blockOf(address + bytes - 1);
    for (auto block = first; block <= last; ++block) {
        const auto k = block - first;
        if (k >= generations.size() || k >= changed.size()) break;
        const auto generation = generations[k];
        if (generation == 0) {
            changed[k] = BlockWritten;
            if (k < cpu.size()) cpu[k] = 1;
            continue;
        }
        if (floor > generation || table.DataOf(tracker.blockBegin(block)) > generation) return FastAnswer::Inconclusive;
        changed[k] = BlockUnchanged;
        if (k < cpu.size()) cpu[k] = 0;
    }
    return FastAnswer::True;
}
}

static bool changedBlocksImpl(std::uint64_t address, std::size_t bytes, std::span<const std::uint64_t> generations, std::span<std::uint8_t> changed, std::span<std::uint8_t> cpu, std::source_location site);

bool ChangedBlocks(std::uint64_t address, std::size_t bytes, std::span<const std::uint64_t> generations, std::span<std::uint8_t> changed, std::span<std::uint8_t> cpu, std::source_location site) {
    census::Scope censusScope("changed_blocks", &site);
    const auto result = changedBlocksImpl(address, bytes, generations, changed, cpu, site);
    census::answer(result && std::none_of(changed.begin(), changed.end(), [](std::uint8_t value) { return value != BlockUnchanged; }));
    return result;
}

static bool changedBlocksImpl(std::uint64_t address, std::size_t bytes, std::span<const std::uint64_t> generations, std::span<std::uint8_t> changed, std::span<std::uint8_t> cpu, std::source_location site) {
    const Stage4::TrackerScope stage4Scope(false, address, bytes);
    std::fill(changed.begin(), changed.end(), BlockWritten);
    std::fill(cpu.begin(), cpu.end(), std::uint8_t{1});
    auto& tracker = Tracker();
    if (lockFree(tracker)) {
        const auto fast = fastChangedBlocks(tracker, address, bytes, generations, changed, cpu);
        if (fast != FastAnswer::Inconclusive) {
            Gen::CountLockFree(Gen::LockFreeApi::ChangedBlocks, true);
            const bool shadow = Gen::ShadowActive() && fast == FastAnswer::True;
            const bool verify = Gen::VerifyPeriod() != 0 && Gen::VerifySample();
            if (shadow || verify) {
                const auto lock = lockTracker(tracker);
                const auto end = address + bytes;
                const auto first = tracker.blockOf(address);
                bool unsafe = false;
                bool raced = false;
                for (auto block = first; fast == FastAnswer::True && block <= tracker.blockOf(end - 1); ++block) {
                    const auto k = block - first;
                    if (k >= generations.size() || k >= changed.size()) break;
                    const auto generation = generations[k];
                    if (verify) {
                        const bool written = generation == 0 || tracker.stampOf(block) > generation;
                        const bool cpuWritten = generation == 0 || tracker.cpuStampOf(block) > generation;
                        const bool tableClean = generation != 0 && Gen::Global().ResetFloor() <= generation && Gen::Global().DataOf(tracker.blockBegin(block)) <= generation;
                        const bool mismatch = written != (changed[k] == BlockWritten) || (k < cpu.size() && cpuWritten != (cpu[k] != 0));
                        if (mismatch && tableClean) unsafe = true;
                        else if (mismatch) raced = true;
                    }
                    if (shadow) {
                        const auto from = std::max<std::uint64_t>(address, tracker.blockBegin(block));
                        const auto to = std::min<std::uint64_t>(end, tracker.blockBegin(block) + WriteBlockBytes);
                        shadowQuery(tracker, from, static_cast<std::size_t>(to - from), generation, generation != 0, changed[k] == BlockUnchanged, site);
                    }
                }
                if (verify) Gen::CountLockFreeVerify(Gen::LockFreeApi::ChangedBlocks, unsafe ? Gen::LockFreeVerify::Unsafe : raced ? Gen::LockFreeVerify::Raced : Gen::LockFreeVerify::Agree, address, bytes, generations.empty() ? 0 : generations.front());
            }
            return fast == FastAnswer::True;
        }
        Gen::CountLockFree(Gen::LockFreeApi::ChangedBlocks, false);
        std::fill(changed.begin(), changed.end(), BlockWritten);
        std::fill(cpu.begin(), cpu.end(), std::uint8_t{1});
    }
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched || bytes == 0 || !tracker.covers(address, bytes)) return false;
    const auto first = tracker.blockOf(address);
    const auto last = tracker.blockOf(address + bytes - 1);
    const bool shadow = Gen::ShadowActive();
    const auto end = address + bytes;
    for (auto block = first; block <= last; ++block) {
        const auto k = block - first;
        if (k >= generations.size() || k >= changed.size()) break;
        const auto generation = generations[k];
        changed[k] = generation == 0 || tracker.stampOf(block) > generation ? BlockWritten : BlockUnchanged;
        if (k < cpu.size()) cpu[k] = generation == 0 || tracker.cpuStampOf(block) > generation ? 1 : 0;
        if (shadow) {
            const auto from = std::max<std::uint64_t>(address, tracker.blockBegin(block));
            const auto to = std::min<std::uint64_t>(end, tracker.blockBegin(block) + WriteBlockBytes);
            shadowQuery(tracker, from, static_cast<std::size_t>(to - from), generation, generation != 0, changed[k] == BlockUnchanged, site);
        }
    }
    return true;
}

static bool changedPagesImpl(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::vector<std::pair<std::uint64_t, std::uint64_t>>& runs, std::source_location site);

bool ChangedPages(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::vector<std::pair<std::uint64_t, std::uint64_t>>& runs, std::source_location site) {
    census::Scope censusScope("changed_pages", &site);
    const auto result = changedPagesImpl(address, bytes, generation, runs, site);
    census::answer(result && runs.empty());
    return result;
}

static bool changedPagesImpl(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::vector<std::pair<std::uint64_t, std::uint64_t>>& runs, std::source_location site) {
    static_cast<void>(site);
    const Stage4::TrackerScope stage4Scope(false, address, bytes);
    runs.clear();
    if (bytes == 0 || generation == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return false;
    auto& tracker = Tracker();
    const auto lock = lockTracker(tracker);
    tracker.initialize();
    if (!tracker.watched || !tracker.covers(address, bytes)) return false;
    tracker.changedPages(address, address + bytes, generation, runs);
    return true;
}

namespace {

// Per-thread GpuMutex wait accounting (APS5_PROFILE_DRAW): a queue worker tags its thread, other
// threads (the presenter) report untagged.
constexpr std::size_t GpuLockSiteCount = static_cast<std::size_t>(GpuLockSite::Count);
constexpr const char* GpuLockSiteNames[GpuLockSiteCount] = {"other", "dispatch", "indirect", "draw", "hook", "wait", "label", "flush", "present", "fill", "copy", "end", "try"};

// Hold-side holder matrix: nanoseconds held per (holder thread, site), added by EndHold. Column 0
// is every untagged thread (the presenter, game threads); the queue workers take a column each on
// their first hold. A contended lock() snapshots the matrix before and after its wait and credits
// the deltas (the holds that ended during the wait) to the waiter's site.
constexpr std::size_t HolderColumns = 12;
constexpr std::uint32_t UnassignedHolder = 0xfffffffeu;
constexpr std::uint32_t PresenterHolderTag = 0xfffffffdu;
constexpr int PresenterColumn = 1;
using HolderMatrixValues = std::array<std::array<std::uint64_t, GpuLockSiteCount>, HolderColumns>;
struct HolderMatrix {
    std::array<std::atomic<std::uint32_t>, HolderColumns> tags{};
    std::array<std::array<std::atomic<std::uint64_t>, GpuLockSiteCount>, HolderColumns> heldNs{};
    HolderMatrix() {
        for (auto& tag : tags) tag.store(UnassignedHolder, std::memory_order_relaxed);
        tags[0].store(0xffffffffu, std::memory_order_relaxed);
        tags[PresenterColumn].store(PresenterHolderTag, std::memory_order_relaxed);
    }
    void Snapshot(HolderMatrixValues& into) const {
        for (std::size_t h = 0; h < HolderColumns; ++h) {
            for (std::size_t s = 0; s < GpuLockSiteCount; ++s) into[h][s] = heldNs[h][s].load(std::memory_order_relaxed);
        }
    }
};

HolderMatrix& Holders() {
    static HolderMatrix matrix;
    return matrix;
}

std::size_t HolderColumnFor(std::uint32_t tag) {
    if (tag == 0xffffffffu) return 0;
    auto& holders = Holders();
    for (std::size_t i = 1; i < HolderColumns; ++i) {
        auto expected = UnassignedHolder;
        if (holders.tags[i].load(std::memory_order_acquire) == tag) return i;
        if (holders.tags[i].compare_exchange_strong(expected, tag, std::memory_order_acq_rel)) return i;
        if (expected == tag) return i;
    }
    return 0;
}

struct GpuLockStats {
    std::uint32_t tag = 0xffffffffu;
    std::uint64_t acquisitions = 0;
    std::uint64_t waits = 0;
    double waitedMs = 0;
    // The same split by the site tagged before the acquisition (TagGpuLockSite); the tag of the
    // next lock() is kept here and consumed by it.
    std::array<std::uint64_t, GpuLockSiteCount> siteAcquisitions{};
    std::array<std::uint64_t, GpuLockSiteCount> siteWaits{};
    std::array<double, GpuLockSiteCount> siteWaitedMs{};
    GpuLockSite nextSite = GpuLockSite::Other;
    // Holds: the outermost (depth 0 -> 1) acquisition of this thread until the matching unlock,
    // charged to that acquisition's site (a nested hook lock inside a dispatch's hold extends the
    // dispatch's hold). The waits above say who waits; these say whom they wait for.
    std::array<std::uint64_t, GpuLockSiteCount> siteHolds{};
    std::array<double, GpuLockSiteCount> siteHeldMs{};
    std::array<double, GpuLockSiteCount> siteMaxHeldMs{};
    std::array<std::uint64_t, GpuLockSiteCount> siteLabels{};
    std::uint64_t labelsInHold = 0;
    std::size_t holdSite = 0;
    std::chrono::steady_clock::time_point holdStart;
    bool holdCheap = false;
    std::uint64_t heldNs = 0;
    // This thread's column of the holder matrix (-1 until its first hold), and what its contended
    // acquisitions waited behind: holds by (holder, site) that ended during the waits.
    int holderColumn = -1;
    HolderMatrixValues waitedBehindNs{};
    std::uint64_t tries = 0;
    std::uint64_t triesFailed = 0;
    std::uint64_t lockedGpuWaits = 0;
    double lockedGpuWaitMs = 0;
    std::array<std::uint64_t, GpuLockSiteCount> siteLockedGpuWaits{}, siteUnlockedGpuWaits{};
    std::array<double, GpuLockSiteCount> siteLockedGpuWaitMs{}, siteUnlockedGpuWaitMs{};
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

GpuLockStats& LockStats() {
    struct Slot {};
    auto* stats = HostThreadSlot<GpuLockStats*, Slot>::Get();
    if (stats == nullptr) {
        ShaderRecompiler::ThreadOwned(stats);
        HostThreadSlot<GpuLockStats*, Slot>::Set(stats);
    }
    return *stats;
}

bool GpuLockProfiled() {
    static const bool profiled = std::getenv("APS5_PROFILE_DRAW") != nullptr && std::getenv("APS5_NO_LOCK_PROFILE") == nullptr;
    return profiled;
}

// Hold timing costs two clock reads per outermost acquisition (~20000 per 10 s on queue 0, well
// under a millisecond); APS5_NO_HOLD_PROFILE=1 keeps only the wait timing.
bool GpuHoldProfiled() {
    static const bool profiled = GpuLockProfiled() && std::getenv("APS5_NO_HOLD_PROFILE") == nullptr;
    return profiled;
}

void BeginHold(std::size_t site) {
    auto& stats = LockStats();
    stats.holdSite = site;
    stats.labelsInHold = 0;
    stats.holdCheap = ArchB::CheapClock::Active();
    stats.holdStart = ArchB::CheapClock::Now(stats.holdCheap);
}

void EndHold() {
    auto& stats = LockStats();
    const auto heldNs = std::chrono::duration_cast<std::chrono::nanoseconds>(ArchB::CheapClock::Now(stats.holdCheap) - stats.holdStart).count();
    const auto heldMs = static_cast<double>(heldNs) / 1e6;
    stats.heldNs += static_cast<std::uint64_t>(heldNs);
    ++stats.siteHolds[stats.holdSite];
    stats.siteLabels[stats.holdSite] += stats.labelsInHold;
    stats.siteHeldMs[stats.holdSite] += heldMs;
    stats.siteMaxHeldMs[stats.holdSite] = std::max(stats.siteMaxHeldMs[stats.holdSite], heldMs);
    if (stats.holderColumn < 0) stats.holderColumn = static_cast<int>(HolderColumnFor(stats.tag));
    Holders().heldNs[static_cast<std::size_t>(stats.holderColumn)][stats.holdSite].fetch_add(static_cast<std::uint64_t>(heldNs), std::memory_order_relaxed);
}

const char* HolderName(std::size_t column, char* text, std::size_t size) {
    const auto tag = Holders().tags[column].load(std::memory_order_relaxed);
    if (tag == 0xffffffffu) std::snprintf(text, size, "untagged");
    else if (tag == PresenterHolderTag) std::snprintf(text, size, "presenter");
    else std::snprintf(text, size, "q0x%x", tag);
    return text;
}

// The waiter's matrix as text, largest first, entries of at least a millisecond.
std::string WaitedBehindReport(const HolderMatrixValues& waited) {
    std::vector<std::tuple<std::uint64_t, std::size_t, std::size_t>> entries;
    for (std::size_t h = 0; h < HolderColumns; ++h) {
        for (std::size_t s = 0; s < GpuLockSiteCount; ++s) {
            if (waited[h][s] >= 1000000) entries.emplace_back(waited[h][s], h, s);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
    std::string report;
    char text[96];
    char holder[24];
    for (std::size_t i = 0; i < entries.size() && i < 16; ++i) {
        const auto& [ns, h, s] = entries[i];
        std::snprintf(text, sizeof(text), " %s/%s %.0f", HolderName(h, holder, sizeof(holder)), GpuLockSiteNames[s], static_cast<double>(ns) / 1e6);
        report += text;
    }
    return report;
}

}

namespace {
#ifdef _WIN32
struct HoldLogRecord {
    std::uint32_t thread, tag;
    std::uint64_t begin, end;
};

struct FlushAttribution {
    std::uint64_t caller = 0, address = 0, bytes = 0;
};
thread_local FlushAttribution flushAttribution;

class HoldLog {
public:
    static HoldLog* Instance() {
        static HoldLog* const log = []() -> HoldLog* {
            const char* path = std::getenv("APS5_HOLD_LOG");
            if (path == nullptr || *path == '\0') return nullptr;
            return new HoldLog(path);
        }();
        return log;
    }

    static std::uint64_t Now() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return static_cast<std::uint64_t>(now.QuadPart);
    }

    bool Attribution() const { return attribution; }

    void Push(std::uint32_t tag, std::uint64_t begin, std::uint64_t end, std::size_t site = 0, std::uint64_t caller = 0) {
        auto& ring = local();
        append(ring, {ring.thread, attribution ? encoded(site, false) : tag, begin, end});
        if (attribution) append(ring, {0xfffffff9u, ring.thread, begin, caller});
    }

    void Wait(std::size_t site, std::uint64_t begin, std::uint64_t end, const void* initialOwner) {
        auto& ring = local();
        append(ring, {ring.thread, encoded(site, true), begin, end});
        append(ring, {0xfffffffbu, ring.thread, begin, reinterpret_cast<std::uint64_t>(initialOwner)});
    }

    void LockedWait(double ms) {
        if (!attribution || ms < 0.05) return;
        auto& ring = local();
        const auto end = Now();
        append(ring, {0xfffffff6u, ring.thread, end, static_cast<std::uint64_t>(ms * 1e6)});
        append(ring, {0xfffffff5u, ring.thread, end, flushAttribution.caller});
        append(ring, {0xfffffff4u, ring.thread, flushAttribution.address, flushAttribution.bytes});
    }

    void Mark(std::uint32_t mark) {
        auto& ring = local();
        append(ring, {0xffffffffu, mark, Now(), ring.thread});
    }

private:
    struct Ring {
        static constexpr std::size_t Capacity = std::size_t{1} << 15;
        std::uint32_t thread = 0;
        std::unique_ptr<HoldLogRecord[]> records = std::make_unique<HoldLogRecord[]>(Capacity);
        alignas(64) std::atomic<std::size_t> written{0};
        alignas(64) std::atomic<std::size_t> consumed{0};
        std::atomic<std::uint64_t> lost{0};
    };

    static void append(Ring& ring, HoldLogRecord record) {
        const auto next = ring.written.load(std::memory_order_relaxed);
        if (next - ring.consumed.load(std::memory_order_acquire) == Ring::Capacity) {
            ring.lost.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ring.records[next % Ring::Capacity] = record;
        ring.written.store(next + 1, std::memory_order_release);
    }

    static std::uint32_t encoded(std::size_t site, bool wait) {
        const auto& stats = LockStats();
        const auto role = stats.tag != 0xffffffffu ? stats.tag & 0xffffu : IsCommitterThread() ? 0xfffdu : stats.holderColumn == PresenterColumn ? 0xfffeu : 0xffffu;
        return role | (static_cast<std::uint32_t>(site) << 16u) | (wait ? 0x80000000u : 0u);
    }

    using RegisterFn = ULONG(WINAPI*)(const GUID*, void*, void*, std::uint64_t*);
    using WriteFn = ULONG(WINAPI*)(std::uint64_t, const void*, ULONG, void*);
    struct Descriptor {
        std::uint16_t id;
        std::uint8_t version, channel, level, opcode;
        std::uint16_t task;
        std::uint64_t keyword;
    };

    explicit HoldLog(const char* path) : file(std::fopen(path, "wb")), attribution(std::getenv("APS5_MUTEX_ATTRIBUTION") != nullptr && GpuHoldProfiled()) {
        if (file == nullptr) throw std::runtime_error("cannot open APS5_HOLD_LOG output");
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const HoldLogRecord header{0xffffffffu, 0, static_cast<std::uint64_t>(frequency.QuadPart), attribution ? 1u : 0u};
        std::fwrite(&header, sizeof(header), 1, file);
        if (attribution) {
            const HoldLogRecord clock{0xfffffffau, 0, Now(), static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())};
            std::fwrite(&clock, sizeof(clock), 1, file);
            HMODULE module = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&GpuMutex), &module) == 0) throw std::runtime_error("cannot locate mutex attribution module");
            const HoldLogRecord image{0xfffffff8u, 0, reinterpret_cast<std::uint64_t>(module), 0};
            std::fwrite(&image, sizeof(image), 1, file);
        }
        if (auto* advapi = LoadLibraryA("advapi32.dll"); advapi != nullptr) {
            const auto registerProvider = reinterpret_cast<RegisterFn>(reinterpret_cast<void*>(GetProcAddress(advapi, "EventRegister")));
            write = reinterpret_cast<WriteFn>(reinterpret_cast<void*>(GetProcAddress(advapi, "EventWrite")));
            static const GUID provider{0xaf6e87a6, 0x484f, 0x453b, {0xa9, 0x42, 0xbf, 0x4c, 0x0e, 0x9a, 0xda, 0x47}};
            if (registerProvider == nullptr || write == nullptr || registerProvider(&provider, nullptr, nullptr, &handle) != 0) write = nullptr;
        }
        std::thread([this] { run(); }).detach();
    }

    Ring& local() {
        struct RingThreadStorage {};
        auto& ring = AgcDriver::ThreadScratch<Ring*, RingThreadStorage>();
        if (ring == nullptr) {
            auto owned = std::make_unique<Ring>();
            owned->thread = static_cast<std::uint32_t>(GetCurrentThreadId());
            if (attribution) append(*owned, {0xfffffffcu, owned->thread, reinterpret_cast<std::uint64_t>(CurrentThreadToken()), encoded(0, false)});
            ring = owned.get();
            std::lock_guard lock(mutex);
            rings.push_back(std::move(owned));
        }
        return *ring;
    }

    void run() {
        std::uint32_t sequence = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ++sequence;
            const Descriptor descriptor{static_cast<std::uint16_t>(1 + sequence % 60000), 0, 0, 4, 0, 0, 1};
            const auto before = Now();
            if (write != nullptr) write(handle, &descriptor, 0, nullptr);
            const HoldLogRecord sync{0, sequence, before, Now()};
            std::fwrite(&sync, sizeof(sync), 1, file);
            std::lock_guard lock(mutex);
            for (auto& ring : rings) {
                auto first = ring->consumed.load(std::memory_order_relaxed);
                const auto last = ring->written.load(std::memory_order_acquire);
                while (first != last) {
                    const auto count = std::min(last - first, Ring::Capacity - first % Ring::Capacity);
                    std::fwrite(ring->records.get() + first % Ring::Capacity, sizeof(HoldLogRecord), count, file);
                    first += count;
                    ring->consumed.store(first, std::memory_order_release);
                }
                if (const auto lost = ring->lost.exchange(0, std::memory_order_relaxed); lost != 0) {
                    const HoldLogRecord dropped{ring->thread, 0xffffffffu, lost, 0};
                    std::fwrite(&dropped, sizeof(dropped), 1, file);
                }
            }
            if (sequence % 10 == 0) std::fflush(file);
        }
    }

    std::FILE* file;
    bool attribution;
    WriteFn write = nullptr;
    std::uint64_t handle = 0;
    std::mutex mutex;
    std::vector<std::unique_ptr<Ring>> rings;
};
thread_local std::uint64_t holdLogBegin = 0;
thread_local std::uint64_t holdLogCaller = 0;
#endif

std::atomic<std::uint64_t> gpuHoldSerial{0};
struct RecorderHoldStartSlot {};
struct RecorderHoldGenerationSlot {};
struct RecorderHoldCheapSlot {};
std::uint64_t recorderHoldStart() { return HostThreadSlot<std::uint64_t, RecorderHoldStartSlot>::Get(); }
void recorderHoldStart(std::uint64_t value) { HostThreadSlot<std::uint64_t, RecorderHoldStartSlot>::Set(value); }
std::uint64_t recorderHoldGeneration() { return HostThreadSlot<std::uint64_t, RecorderHoldGenerationSlot>::Get(); }
void recorderHoldGeneration(std::uint64_t value) { HostThreadSlot<std::uint64_t, RecorderHoldGenerationSlot>::Set(value); }
bool recorderHoldCheap() { return HostThreadSlot<bool, RecorderHoldCheapSlot>::Get(); }
void recorderHoldCheap(bool value) { HostThreadSlot<bool, RecorderHoldCheapSlot>::Set(value); }

// APS5_ARCHB_PIPE_COMMITTER_MUTEX_PRIORITY (GuestMemory.hpp, SetGpuLockPriorityHook).
std::atomic<bool (*)()> gpuLockPriorityHook{nullptr};
struct GpuLockPriorityCounters {
    std::atomic<std::uint64_t> waits{0};      // acquisitions that found the committer busy and waited
    std::atomic<std::uint64_t> cleared{0};    // ... and saw it go idle within the window
    std::atomic<std::uint64_t> timeouts{0};   // ... and took the mutex at the end of the window
    std::atomic<std::uint64_t> waitedUs{0};
};
GpuLockPriorityCounters& priorityCounters() {
    static GpuLockPriorityCounters counters;
    return counters;
}

void DeferToCommitter(const GpuMutexType& mutex) {
    const auto hook = gpuLockPriorityHook.load(std::memory_order_acquire);
    if (hook == nullptr || !ArchB::Switches::Enabled(ArchB::Feature::PipeCommitterMutexPriority)) return;
    const auto tag = LockStats().tag;
    if (tag == 0 || tag == 0xffffffffu || mutex.HeldByThisThread() || !hook()) return;
    const auto start = std::chrono::steady_clock::now();
    const bool idle = SpinUntil(GpuLockPriorityWindow(), [&] { return !hook(); });
    auto& counters = priorityCounters();
    counters.waits.fetch_add(1, std::memory_order_relaxed);
    (idle ? counters.cleared : counters.timeouts).fetch_add(1, std::memory_order_relaxed);
    counters.waitedUs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
}
}

void SetGpuLockPriorityHook(bool (*queued)()) {
    static const bool registered = [] {
        auto& counters = priorityCounters();
        ArchB::Recorder::RegisterCounter("pipe_mutex_priority_waits", counters.waits);
        ArchB::Recorder::RegisterCounter("pipe_mutex_priority_cleared", counters.cleared);
        ArchB::Recorder::RegisterCounter("pipe_mutex_priority_timeouts", counters.timeouts);
        ArchB::Recorder::RegisterCounter("pipe_mutex_priority_waited_us", counters.waitedUs);
        return true;
    }();
    static_cast<void>(registered);
    gpuLockPriorityHook.store(queued, std::memory_order_release);
}

std::chrono::nanoseconds GpuLockPriorityWindow() {
    static const std::chrono::nanoseconds window = [] {
        const char* text = std::getenv("APS5_PIPE_MUTEX_PRIORITY_US");
        const long long us = text != nullptr && *text != '\0' ? std::clamp<long long>(std::strtoll(text, nullptr, 10), 0, 10000) : 1000;
        return std::chrono::nanoseconds(std::chrono::microseconds(us));
    }();
    return window;
}

// LockSpinning with the Stage 4 wait scope (M1/M4) around a contended acquisition.
static void lockSpinningTracked(Mutex& mutex, const std::uint64_t& criticalRelease) {
    if (CriticalPath::Enabled()) {
        if (mutex.try_lock()) return;
        CriticalPath::Wait critical("GpuMutex", CriticalPath::Object(&mutex));
        if (!Stage4::Tracking()) LockSpinning(mutex, HandoffSpinWindow());
        else {
            Stage4::WaitScope wait(Stage4::Wait::GpuMutex, true);
            if (!SpinUntil(HandoffSpinWindow(), [&] { return mutex.try_lock(); })) {
                wait.Blocking();
                mutex.lock();
            }
        }
        critical.End(criticalRelease, "mutex.release");
        return;
    }
    if (!Stage4::Tracking()) {
        LockSpinning(mutex, HandoffSpinWindow());
        return;
    }
    if (mutex.try_lock()) return;
    Stage4::WaitScope wait(Stage4::Wait::GpuMutex, true);
    if (SpinUntil(HandoffSpinWindow(), [&] { return mutex.try_lock(); })) return;
    wait.Blocking();
    mutex.lock();
}

void GpuMutexType::enter() {
    if (!HeldByThisThread()) lockSpinningTracked(mutex, criticalRelease);
}

bool GpuMutexType::tryEnter() {
    return HeldByThisThread() || mutex.try_lock();
}

void GpuMutexType::acquired() {
    owner.store(CurrentThreadToken(), std::memory_order_relaxed);
    if (++depth == 1) {
        if (CriticalPath::Enabled()) CriticalPath::Emit(CriticalPath::Kind::Point, "mutex.acquire", CriticalPath::Object(&mutex));
#ifdef _WIN32
        if (HoldLog::Instance() != nullptr) holdLogBegin = HoldLog::Now();
#endif
        if (ArchB::Recorder::Enabled()) {
            recorderHoldGeneration(ArchB::Recorder::Generation());
            const bool cheap = ArchB::CheapClock::Active();
            recorderHoldCheap(cheap);
            recorderHoldStart(static_cast<std::uint64_t>(ArchB::CheapClock::Now(cheap).time_since_epoch().count()));
        } else recorderHoldStart(0);
        if (Timeline::Enabled) Timeline::Emit(Timeline::Kind::LockAcquired, 0, 0, 0, 0, 0, Timeline::LockSite());
        gpuHoldSerial.fetch_add(1, std::memory_order_relaxed);
        const bool skipInvalidation = ArchB::Switches::Enabled(ArchB::Feature::UbNoLockInvalidation);
        bool skipped = false;
        if (!proofThreadThreadState()) {
            if (skipInvalidation) skipped = true;
            else proofSerial.fetch_add(1, std::memory_order_acq_rel);
        }
        if (ResourceEpochProofsEnabled()) {
            static const void* resourceProofOwner = nullptr;
            const auto token = CurrentThreadToken();
            if (resourceProofOwner != token) {
                if (skipInvalidation) skipped = true;
                else resourceProofSerial.fetch_add(1, std::memory_order_acq_rel);
            }
            resourceProofOwner = token;
        }
        if (skipped) ArchB::Switches::CountUpperBoundHit(ArchB::Feature::UbNoLockInvalidation);
    }
}

bool GpuMutexType::try_lock() {
    const bool timelineOuter = Timeline::Enabled && !HeldByThisThread();
    if (timelineOuter) {
        const auto site = LockStats().nextSite;
        Timeline::LockSite() = static_cast<std::uint32_t>(site == GpuLockSite::Other ? GpuLockSite::Try : site) + 1;
        Timeline::Emit(Timeline::Kind::LockBegin, 1, 0, 0, 0, 0, Timeline::LockSite());
    }
    struct TimelineTry {
        bool outer;
        GpuMutexType& mutex;
        ~TimelineTry() { if (outer && !mutex.HeldByThisThread()) Timeline::Emit(Timeline::Kind::LockFailed); }
    } timelineTry{timelineOuter, *this};
    if (!GpuLockProfiled()) {
        if (timelineOuter) LockStats().nextSite = GpuLockSite::Other;
        if (!tryEnter()) return false;
        acquired();
        return true;
    }
    // The tag is consumed whether or not the try succeeds (a failed try must not name the next
    // acquisition); an untagged try's hold is charged to 'try' (a boundary reap, an idle worker).
    auto& stats = LockStats();
    const auto tagged = std::exchange(stats.nextSite, GpuLockSite::Other);
    ++stats.tries;
    if (!tryEnter()) {
        ++stats.triesFailed;
        return false;
    }
    acquired();
    if (depth == 1) {
#ifdef _WIN32
        if (auto* log = HoldLog::Instance(); log != nullptr && log->Attribution()) holdLogCaller = reinterpret_cast<std::uint64_t>(__builtin_return_address(0));
#endif
        stats.holdSite = static_cast<std::size_t>(tagged == GpuLockSite::Other ? GpuLockSite::Try : tagged);
        if (GpuHoldProfiled()) BeginHold(stats.holdSite);
    }
    return true;
}

namespace {
std::atomic<void (*)()> gpuUnlockHook{nullptr};
}

void SetGpuUnlockHook(void (*hook)()) {
    gpuUnlockHook.store(hook, std::memory_order_release);
}

void EnableHoldMarks() {
#ifdef _WIN32
    HoldMarksOn(HoldLog::Instance() != nullptr);
#endif
}

void HoldMarkSlow(HoldMark mark) {
#ifdef _WIN32
    if (auto* log = HoldLog::Instance(); log != nullptr) log->Mark(static_cast<std::uint32_t>(mark));
#else
    static_cast<void>(mark);
#endif
}

std::uint64_t GpuHoldSerial() {
    return gpuHoldSerial.load(std::memory_order_relaxed);
}

std::uint64_t ThreadGpuHeldNs() {
    if (!GpuHoldProfiled()) return 0;
    const auto& stats = LockStats();
    const auto current = GpuMutex().HeldByThisThread() ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - stats.holdStart).count() : 0;
    return stats.heldNs + static_cast<std::uint64_t>(current);
}

void GpuMutexType::unlock() {
    const bool outermost = --depth == 0;
    if (outermost) {
        if (const auto start = recorderHoldStart(); start != 0 && recorderHoldGeneration() == ArchB::Recorder::Generation()) {
            const auto now = static_cast<std::uint64_t>(ArchB::CheapClock::Now(recorderHoldCheap()).time_since_epoch().count());
            ArchB::Recorder::Count("gpu_mutex_hold_ms", static_cast<double>(now - start) / 1e6);
            recorderHoldStart(0);
        }
        owner.store(nullptr, std::memory_order_relaxed);
        // Still the holder here: the hold ends when the mutex is given up, not before.
        if (GpuHoldProfiled()) EndHold();
        if (Timeline::Enabled) Timeline::Emit(Timeline::Kind::LockRelease, 0, 0, 0, 0, 0, Timeline::LockSite());
#ifdef _WIN32
        if (auto* log = HoldLog::Instance(); log != nullptr && holdLogBegin != 0) {
            log->Push(LockStats().tag, holdLogBegin, HoldLog::Now(), LockStats().holdSite, holdLogCaller);
            holdLogBegin = 0;
        }
#endif
        if (CriticalPath::Enabled()) criticalRelease = CriticalPath::Emit(CriticalPath::Kind::Release, "mutex.release", CriticalPath::Object(&mutex));
        mutex.unlock();
    }
    // After the release, so what the hook does (destroying a batch's kept objects) is neither part
    // of the hold nor waited for by the next holder.
    if (outermost) {
        if (auto* hook = gpuUnlockHook.load(std::memory_order_acquire); hook != nullptr) hook();
        HoldLogMark(HoldMark::UnlockHookEnd);
    }
}

bool GpuMutexType::HeldByThisThread() const {
    return owner.load(std::memory_order_relaxed) == CurrentThreadToken();
}

std::uint32_t GpuMutexType::DepthOnThisThread() const {
    // `depth` is written by the holder only, so it is read only when that is this thread.
    return HeldByThisThread() ? depth : 0;
}

void AssertGpuLockHeld(const char* where) {
    static const bool enabled = std::getenv("APS5_ASSERT_GPU_LOCK") != nullptr;
    if (!enabled || GpuMutex().HeldByThisThread()) return;
    std::fprintf(stderr, "[lock] ASSERT: %s reached without GuestMemory::GpuMutex on thread tagged 0x%x\n", where, GpuLockThreadTag());
    std::abort();
}

void GpuMutexType::lock() {
    if (HoldMarksOn() && !HeldByThisThread()) HoldMarkSlow(HoldMark::LockBegin);
    // Before the wait timer: time given to the committer is counted apart (pipe_mutex_priority_*).
    DeferToCommitter(*this);
    PerformanceTimer recorderTiming("GpuMutex.wait");
    recorderTiming.RecorderCounter("gpu_mutex_wait_ms");
    if (Timeline::Enabled && !HeldByThisThread()) {
        Timeline::LockSite() = static_cast<std::uint32_t>(LockStats().nextSite) + 1;
        Timeline::Emit(Timeline::Kind::LockBegin, 0, 0, 0, 0, 0, Timeline::LockSite());
    }
    if (!GpuLockProfiled()) {
        auto* trace = PerformanceTrace::Recording() ? PerformanceTrace::Thread() : nullptr;
        if (trace == nullptr) {
            if (Timeline::Enabled && !HeldByThisThread()) LockStats().nextSite = GpuLockSite::Other;
            enter();
        } else {
            const auto site = static_cast<std::size_t>(std::exchange(LockStats().nextSite, GpuLockSite::Other));
            if (!tryEnter()) {
                const auto start = std::chrono::steady_clock::now();
                lockSpinningTracked(mutex, criticalRelease);
                trace->Add("GpuMutex.wait", GpuLockSiteNames[site], start, std::chrono::steady_clock::now());
            }
        }
        acquired();
        return;
    }
    auto& stats = LockStats();
    ++stats.acquisitions;
    const auto site = static_cast<std::size_t>(std::exchange(stats.nextSite, GpuLockSite::Other));
    ++stats.siteAcquisitions[site];
    // An uncontended (or recursive) acquisition costs no clock reads for the wait; only a wait is
    // timed. The hold timer (outermost acquisitions only) starts once the mutex is held.
    if (!tryEnter()) {
        const bool holders = GpuHoldProfiled();
        HolderMatrixValues before{};
        if (holders) Holders().Snapshot(before);
#ifdef _WIN32
        auto* attribution = HoldLog::Instance();
        const auto waitBegin = attribution != nullptr && attribution->Attribution() ? HoldLog::Now() : 0;
        const auto* initialOwner = waitBegin != 0 ? owner.load(std::memory_order_acquire) : nullptr;
#endif
        const auto start = std::chrono::steady_clock::now();
        lockSpinningTracked(mutex, criticalRelease);
        acquired();
        if (depth == 1) stats.holdSite = site;
        const auto now = std::chrono::steady_clock::now();
#ifdef _WIN32
        if (waitBegin != 0) attribution->Wait(site, waitBegin, HoldLog::Now(), initialOwner);
        if (depth == 1 && attribution != nullptr && attribution->Attribution()) holdLogCaller = reinterpret_cast<std::uint64_t>(__builtin_return_address(0));
#endif
        if (auto* trace = PerformanceTrace::Recording() ? PerformanceTrace::Thread() : nullptr) trace->Add("GpuMutex.wait", GpuLockSiteNames[site], start, now);
        if (holders) {
            HolderMatrixValues after{};
            Holders().Snapshot(after);
            for (std::size_t h = 0; h < HolderColumns; ++h) {
                for (std::size_t s = 0; s < GpuLockSiteCount; ++s) stats.waitedBehindNs[h][s] += after[h][s] - before[h][s];
            }
        }
        if (depth == 1 && holders) {
            stats.holdSite = site;
            stats.holdStart = now;
            stats.labelsInHold = 0;
        }
        const auto waitedMs = std::chrono::duration<double, std::milli>(now - start).count();
        stats.waitedMs += waitedMs;
        ++stats.waits;
        stats.siteWaitedMs[site] += waitedMs;
        ++stats.siteWaits[site];
        if (now - stats.lastReport < std::chrono::seconds(10)) return;
        // The line is printed from inside a contended acquisition, so a thread that rarely waits
        // reports a longer span than 10 s: the real interval is printed.
        const auto interval = std::chrono::duration<double>(now - stats.lastReport).count();
        stats.lastReport = now;
        std::string sites;
        for (std::size_t i = 0; i < GpuLockSiteCount; ++i) {
            if (stats.siteAcquisitions[i] == 0) continue;
            char text[96];
            std::snprintf(text, sizeof(text), " %s %llu/%llu %.0f ms", GpuLockSiteNames[i], static_cast<unsigned long long>(stats.siteAcquisitions[i]), static_cast<unsigned long long>(stats.siteWaits[i]), stats.siteWaitedMs[i]);
            sites += text;
        }
        // Holds by site, longest total first: the sites whose holds the other threads wait behind.
        std::string holds;
        if (GpuHoldProfiled()) {
            std::array<std::size_t, GpuLockSiteCount> order{};
            for (std::size_t i = 0; i < GpuLockSiteCount; ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return stats.siteHeldMs[a] > stats.siteHeldMs[b]; });
            double heldMs = 0;
            for (std::size_t i = 0; i < GpuLockSiteCount; ++i) heldMs += stats.siteHeldMs[i];
            char text[112];
            std::snprintf(text, sizeof(text), "; held %.0f ms in total, by site (holds, held, max):", heldMs);
            holds += text;
            for (const auto i : order) {
                if (stats.siteHolds[i] == 0) continue;
                std::snprintf(text, sizeof(text), " %s %llu %.0f ms (max %.1f)", GpuLockSiteNames[i], static_cast<unsigned long long>(stats.siteHolds[i]), stats.siteHeldMs[i], stats.siteMaxHeldMs[i]);
                holds += text;
            }
            holds += "; waited behind (holder/site ms):" + WaitedBehindReport(stats.waitedBehindNs);
        }
        char counts[160];
        std::snprintf(counts, sizeof(counts), "; lock() %llu / try %llu / try failed %llu; locked GPU waits %llu / %.0f ms", static_cast<unsigned long long>(stats.acquisitions), static_cast<unsigned long long>(stats.tries), static_cast<unsigned long long>(stats.triesFailed), static_cast<unsigned long long>(stats.lockedGpuWaits), stats.lockedGpuWaitMs);
        holds += counts;
        holds += "; GPU waits by site (locked count/ms, unlocked count/ms):";
        for (std::size_t i = 0; i < GpuLockSiteCount; ++i) {
            if (stats.siteLockedGpuWaits[i] == 0 && stats.siteUnlockedGpuWaits[i] == 0) continue;
            char text[160];
            std::snprintf(text, sizeof(text), " %s %llu/%.1f %llu/%.1f", GpuLockSiteNames[i], static_cast<unsigned long long>(stats.siteLockedGpuWaits[i]), stats.siteLockedGpuWaitMs[i], static_cast<unsigned long long>(stats.siteUnlockedGpuWaits[i]), stats.siteUnlockedGpuWaitMs[i]);
            holds += text;
        }
        if (stats.tag == 0xffffffffu) AgcDriver::ProfilePrint_nid_no_patch("[lock] %s waited %.0f ms for the GPU mutex in %llu of %llu acquisitions (%.0f s); by site (acquisitions/waits, waited):%s%s\n", IsCommitterThread() ? "draw committer" : stats.holderColumn == PresenterColumn ? "presenter" : "untagged thread", stats.waitedMs, static_cast<unsigned long long>(stats.waits), static_cast<unsigned long long>(stats.acquisitions), interval, sites.c_str(), holds.c_str());
        else AgcDriver::ProfilePrint_nid_no_patch("[lock] queue 0x%x waited %.0f ms for the GPU mutex in %llu of %llu acquisitions (%.0f s); by site (acquisitions/waits, waited):%s%s\n", stats.tag, stats.waitedMs, static_cast<unsigned long long>(stats.waits), static_cast<unsigned long long>(stats.acquisitions), interval, sites.c_str(), holds.c_str());
        if (GpuHoldProfiled()) {
            for (std::size_t i = 0; i < GpuLockSiteCount; ++i) {
                const auto count = stats.siteHolds[i];
                if (count == 0 || (stats.siteLabels[i] == 0 && i != static_cast<std::size_t>(GpuLockSite::Label))) continue;
                AgcDriver::ProfilePrint_nid_no_patch("[label-holds] thread 0x%x site %s: %llu holds, %llu labels, %.2f labels/hold, %.2f us/hold, %.0f ms total (%.0f s)\n", stats.tag, GpuLockSiteNames[i], static_cast<unsigned long long>(count), static_cast<unsigned long long>(stats.siteLabels[i]), static_cast<double>(stats.siteLabels[i]) / count, stats.siteHeldMs[i] * 1000.0 / count, stats.siteHeldMs[i], interval);
            }
        }
        stats.waitedMs = 0;
        stats.waits = 0;
        stats.acquisitions = 0;
        stats.siteAcquisitions.fill(0);
        stats.siteWaits.fill(0);
        stats.siteWaitedMs.fill(0);
        stats.siteHolds.fill(0);
        stats.siteHeldMs.fill(0);
        stats.siteMaxHeldMs.fill(0);
        stats.siteLabels.fill(0);
        for (auto& row : stats.waitedBehindNs) row.fill(0);
        stats.tries = 0;
        stats.triesFailed = 0;
        stats.lockedGpuWaits = 0;
        stats.lockedGpuWaitMs = 0;
        stats.siteLockedGpuWaits.fill(0);
        stats.siteLockedGpuWaitMs.fill(0);
        stats.siteUnlockedGpuWaits.fill(0);
        stats.siteUnlockedGpuWaitMs.fill(0);
        return;
    }
    acquired();
    if (depth == 1) stats.holdSite = site;
#ifdef _WIN32
    if (depth == 1) {
        if (auto* log = HoldLog::Instance(); log != nullptr && log->Attribution()) holdLogCaller = reinterpret_cast<std::uint64_t>(__builtin_return_address(0));
    }
#endif
    if (depth == 1 && GpuHoldProfiled()) BeginHold(site);
}

void TagGpuLockSite(GpuLockSite site) {
    // Off the profiled path the tag is never read, so the thread-local is not touched either.
    if (!GpuLockProfiled() && !PerformanceTrace::Recording() && !Timeline::Enabled) return;
    LockStats().nextSite = site;
}

void NoteGpuLabel() {
    if (GpuHoldProfiled() && GpuMutex().HeldByThisThread()) ++LockStats().labelsInHold;
}

void MarkPresenterThread() {
    LockStats().holderColumn = PresenterColumn;
}

void NoteLockedGpuWait(double ms) {
    if (!GpuLockProfiled() || GpuMutex().DepthOnThisThread() == 0) return;
#ifdef _WIN32
    if (auto* log = HoldLog::Instance(); log != nullptr) log->LockedWait(ms);
#endif
    auto& stats = LockStats();
    ++stats.lockedGpuWaits;
    stats.lockedGpuWaitMs += ms;
    ++stats.siteLockedGpuWaits[stats.holdSite];
    stats.siteLockedGpuWaitMs[stats.holdSite] += ms;
}

GpuLockSite GpuLockHoldSite() {
    return GpuLockProfiled() && GpuMutex().HeldByThisThread() ? static_cast<GpuLockSite>(LockStats().holdSite) : GpuLockSite::Other;
}

void NoteUnlockedGpuWait(GpuLockSite site, double ms) {
    if (!GpuLockProfiled()) return;
    auto& stats = LockStats();
    const auto index = static_cast<std::size_t>(site);
    ++stats.siteUnlockedGpuWaits[index];
    stats.siteUnlockedGpuWaitMs[index] += ms;
}

std::uint64_t ThreadCollectedBytes() {
    if (!MemoryProfiled()) return 0;
    return CounterFor(CounterCollect).bytes.load(std::memory_order_relaxed);
}

std::uint32_t GpuLockThreadTag() {
    return LockStats().tag;
}

GpuMutexType& GpuMutex() {
#ifdef __APPLE__
    static auto* mutex = new GpuMutexType;
    return *mutex;
#else
    static GpuMutexType mutex;
    return mutex;
#endif
}

void TagGpuLockThread(std::uint32_t queue) {
    LockStats().tag = queue;
}

unsigned long long CodeOffset(const void* address) {
    return ModuleOffset(address);
}

namespace {
std::atomic<void (*)(std::uint64_t, std::size_t)> flushHook{nullptr};
std::atomic<void (*)(std::uint64_t, std::size_t, std::uint32_t, std::uint32_t)> writeProfileHook{nullptr};
std::atomic<void (*)(std::uint64_t, std::size_t)> readGuard{nullptr};

void PrepareHostRead(std::uintptr_t address, std::size_t bytes) {
    if (const auto guard = readGuard.load(std::memory_order_acquire)) guard(address, bytes);
    FlushGpuWrites(address, bytes);
}
}

void SetWriteProfileHook(void (*hook)(std::uint64_t, std::size_t, std::uint32_t, std::uint32_t)) {
    writeProfileHook.store(hook, std::memory_order_release);
}

void SetStampObserver(void (*observer)(std::uint64_t, std::uint64_t, TrackerEvent)) {
    stampObserver.store(observer, std::memory_order_release);
}

void SetArmObserver(void (*observer)()) {
    for (auto& slot : armObservers) {
        void (*expected)() = nullptr;
        if (slot.load(std::memory_order_acquire) == observer || slot.compare_exchange_strong(expected, observer, std::memory_order_acq_rel)) return;
    }
}

void SetFlushHook(void (*hook)(std::uint64_t, std::size_t)) {
    flushHook.store(hook, std::memory_order_release);
    GuestArena::GuestArenaSetHostReadHook_nid_no_patch(&PrepareHostRead);
}

auto CurrentFlushHook() -> void (*)(std::uint64_t, std::size_t) {
    return flushHook.load(std::memory_order_acquire);
}

void SetReadGuard(void (*guard)(std::uint64_t address, std::size_t bytes)) {
    readGuard.store(guard, std::memory_order_release);
    GuestArena::GuestArenaSetHostReadHook_nid_no_patch(&PrepareHostRead);
}

void GuardRead(std::uint64_t address, std::size_t bytes) {
    if (const auto guard = readGuard.load(std::memory_order_acquire)) guard(address, bytes);
}

void FlushGpuWrites(std::uint64_t address, std::size_t bytes) {
    if (const auto hook = flushHook.load(std::memory_order_acquire)) {
#ifdef _WIN32
        if (auto* log = HoldLog::Instance(); log != nullptr && log->Attribution()) {
            struct Restore {
                FlushAttribution before;
                ~Restore() { flushAttribution = before; }
            } restore{flushAttribution};
            flushAttribution = {reinterpret_cast<std::uint64_t>(__builtin_return_address(0)), address, bytes};
            hook(address, bytes);
            return;
        }
#endif
        hook(address, bytes);
    }
}

void PrepareDriverRead_nid_no_patch(std::uint64_t address, std::size_t bytes) {
    if (const auto hook = lazyStoreHook.load(std::memory_order_acquire)) hook(address, bytes);
    if (const auto fence = deferredFenceHook.load(std::memory_order_acquire)) fence(address, bytes);
    GuardRead(address, bytes);
    FlushGpuWrites(address, bytes);
}

namespace {

std::atomic<std::uint64_t> lockedHostWrites{0};
std::atomic<bool> lockedHostWriteLogged{false};

void noteLockedHostWrite(std::uintptr_t address, std::size_t bytes) {
    static const bool assertLock = std::getenv("APS5_ASSERT_GPU_LOCK") != nullptr;
    if (assertLock) {
        std::fprintf(stderr, "[lock] ASSERT: a host write of 0x%zx bytes at 0x%llx was made under GuestMemory::GpuMutex on thread tagged 0x%x\n", bytes, static_cast<unsigned long long>(address), GpuLockThreadTag());
        std::abort();
    }
    lockedHostWrites.fetch_add(1, std::memory_order_relaxed);
    if (!lockedHostWriteLogged.exchange(true, std::memory_order_relaxed)) std::fprintf(stderr, "[lock] a host write of 0x%zx bytes at 0x%llx was made under GuestMemory::GpuMutex on thread tagged 0x%x; it waits for GPU work with the mutex held (counted, logged once)\n", bytes, static_cast<unsigned long long>(address), GpuLockThreadTag());
}

std::atomic<double (*)()> waitedMsProvider{nullptr};

double threadWaitedMs() {
    const auto provider = waitedMsProvider.load(std::memory_order_acquire);
    return provider != nullptr ? provider() : 0.0;
}

struct HostWriteTotals {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> waited{0};
    std::atomic<std::uint64_t> waitedNanoseconds{0};
    std::atomic<std::uint64_t> maxWaitedNanoseconds{0};
    std::atomic<std::int64_t> lastReport{0};
};

HostWriteTotals& HostWrites() {
    static HostWriteTotals totals;
    return totals;
}

void noteHostWriteWait(double waitedMs) {
    auto& totals = HostWrites();
    totals.calls.fetch_add(1, std::memory_order_relaxed);
    if (waitedMs > 0) {
        const auto nanoseconds = static_cast<std::uint64_t>(waitedMs * 1e6);
        totals.waited.fetch_add(1, std::memory_order_relaxed);
        totals.waitedNanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
        auto most = totals.maxWaitedNanoseconds.load(std::memory_order_relaxed);
        while (nanoseconds > most && !totals.maxWaitedNanoseconds.compare_exchange_weak(most, nanoseconds, std::memory_order_relaxed)) {
        }
    }
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = totals.lastReport.load(std::memory_order_relaxed);
    if (nowMs - last < 10000 || !totals.lastReport.compare_exchange_strong(last, nowMs)) return;
    AgcDriver::ProfilePrint_nid_no_patch("%s", HostWriteReport().c_str());
}

}

std::uint64_t LockedHostWrites() {
    return lockedHostWrites.load(std::memory_order_relaxed);
}

void SetWaitedMsProvider(double (*provider)()) {
    waitedMsProvider.store(provider, std::memory_order_release);
}

HostWriteCounts HostWritesCounted() {
    const auto& totals = HostWrites();
    return {totals.calls.load(std::memory_order_relaxed), totals.waited.load(std::memory_order_relaxed), totals.waitedNanoseconds.load(std::memory_order_relaxed) / 1e6, totals.maxWaitedNanoseconds.load(std::memory_order_relaxed) / 1e6};
}

std::string HostWriteReport() {
    const auto counts = HostWritesCounted();
    char line[256];
    std::snprintf(line, sizeof(line), "[hostwrite] %llu host writes, %llu waited for GPU work over their range: %.2f ms in total, %.2f ms at most\n", static_cast<unsigned long long>(counts.calls), static_cast<unsigned long long>(counts.waited), counts.waitedMs, counts.maxWaitedMs);
    return line;
}

void PrepareHostWrite(std::uintptr_t address, std::size_t bytes) {
    if (ownHostWriteThreadState() || bytes == 0) return;
    if (GpuMutex().HeldByThisThread()) noteLockedHostWrite(address, bytes);
    const HostWriteInFlight inFlight;
    if (hostWritesStopped.load(std::memory_order_seq_cst)) return;
    const ReadSiteScope site(ReadSite::Store);
    if (!MemoryProfiled()) {
        FlushGpuWrites(address, bytes);
        return;
    }
    const auto waitedBefore = threadWaitedMs();
    FlushGpuWrites(address, bytes);
    noteHostWriteWait(threadWaitedMs() - waitedBefore);
}

bool StopHostWrites(std::uint32_t boundMs) {
    hostWritesStopped.store(true, std::memory_order_seq_cst);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(boundMs);
    while (hostWritesInFlight.load(std::memory_order_seq_cst) != 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void Read(std::uint64_t address, std::span<std::byte> destination, std::size_t alignment) {
    if (destination.empty()) return;
    if (const auto hook = lazyStoreHook.load(std::memory_order_acquire)) hook(address, destination.size());
    if (const auto fence = deferredFenceHook.load(std::memory_order_acquire)) fence(address, destination.size());
    GuardRead(address, destination.size());
    FlushGpuWrites(address, destination.size());
    const TimedAccess timed(CounterRead, destination.size());
    CountReadCaller(__builtin_return_address(0));
    const auto* source = reinterpret_cast<const void*>(address);
    try {
        CheckRange(source, destination.size(), alignment);
    } catch (const std::runtime_error&) {
        static const bool trace = std::getenv("APS5_TRACE_UNREADABLE") != nullptr;
        if (trace) std::fprintf(stderr, "[unreadable]   read from +0x%llx\n", ModuleOffset(__builtin_return_address(0)));
        throw;
    }
    CopyGuestBytes(destination.data(), source, destination.size());
}

#ifdef ANYPS5_ENABLE_ASAN
[[gnu::no_sanitize("address")]] void CopyGuestBytes(void* destination, const void* source, std::size_t bytes) {
    using UnalignedWord [[gnu::aligned(1), gnu::may_alias]] = std::uint64_t;
    auto* to = static_cast<unsigned char*>(destination);
    const auto* from = static_cast<const unsigned char*>(source);
    std::size_t index = 0;
    for (; index + sizeof(std::uint64_t) <= bytes; index += sizeof(std::uint64_t)) {
        const std::uint64_t word = *reinterpret_cast<const volatile UnalignedWord*>(from + index);
        std::memcpy(to + index, &word, sizeof(word));
    }
    for (; index < bytes; ++index) to[index] = static_cast<const volatile unsigned char*>(from)[index];
}
#else
void CopyGuestBytes(void* destination, const void* source, std::size_t bytes) {
    std::memcpy(destination, source, bytes);
}
#endif

namespace {

#ifdef _WIN32
bool readPages(std::byte* destination, std::uint64_t source, std::size_t bytes) {
    SIZE_T copied = 0;
    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(source), destination, bytes, &copied) != 0) {
        require(copied == bytes, "short committed guest memory read");
        return true;
    }
    const auto error = GetLastError();
    require(error == ERROR_PARTIAL_COPY || error == ERROR_NOACCESS || error == ERROR_INVALID_ADDRESS, "cannot read committed guest memory");
    return false;
}

template <class Visit>
void visitReadablePages(std::byte* destination, std::uint64_t source, std::size_t bytes, Visit&& visit) {
    if (readPages(destination, source, bytes)) {
        visit(std::size_t{0}, bytes, true);
        return;
    }
    ForgetPages(static_cast<std::uintptr_t>(source), bytes);
    for (std::size_t offset = 0; offset < bytes;) {
        const auto cursor = source + offset;
        const auto chunk = std::min<std::size_t>(bytes - offset, PageBytes - static_cast<std::size_t>(cursor % PageBytes));
        MEMORY_BASIC_INFORMATION memory{};
        require(VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == sizeof(memory), "cannot query guest memory after a failed committed read");
        const bool readable = memory.State == MEM_COMMIT && (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 && readableProtection(memory.Protect & 0xffu);
        visit(offset, chunk, readable && readPages(destination + offset, cursor, chunk));
        offset += chunk;
    }
}
#endif

void copyCommittedBytes(std::byte* destination, std::uint64_t source, std::size_t bytes) {
#ifdef _WIN32
    if (GuestArena::GuestArenaCopyOwned_nid_no_patch(source, destination, bytes)) return;
    visitReadablePages(destination, source, bytes, [&](std::size_t offset, std::size_t count, bool read) {
        if (!read) std::memset(destination + offset, 0, count);
    });
#else
    std::memcpy(destination, reinterpret_cast<const void*>(source), bytes);
#endif
}

bool equalsCommittedBytes(std::uint64_t address, const std::byte* expected, std::size_t bytes) {
#ifdef _WIN32
    bool same = false;
    if (GuestArena::GuestArenaCompareOwned_nid_no_patch(address, expected, bytes, &same)) return same;
    constexpr std::size_t ChunkBytes = 64 * PageBytes;
    struct CompareChunkStorage {};
    auto& current = HostThreadLocal<std::vector<std::byte>, CompareChunkStorage>();
    if (current.size() < ChunkBytes) current.resize(ChunkBytes);
    for (std::size_t done = 0; done < bytes; done += ChunkBytes) {
        const auto count = std::min(ChunkBytes, bytes - done);
        bool equal = true;
        visitReadablePages(current.data(), address + done, count, [&](std::size_t offset, std::size_t length, bool read) {
            if (read && equal && std::memcmp(current.data() + offset, expected + done + offset, length) != 0) equal = false;
        });
        if (!equal) return false;
    }
    return true;
#else
    return std::memcmp(reinterpret_cast<const void*>(address), expected, bytes) == 0;
#endif
}

void copyUnlessUnmapped(std::byte* destination, std::uint64_t source, std::size_t bytes) {
#ifdef _WIN32
    static const bool plain = std::getenv("APS5_NO_GUARDED_PARTIAL_READS") != nullptr;
    if (plain) {
        std::memcpy(destination, reinterpret_cast<const void*>(source), bytes);
        return;
    }
#endif
    copyCommittedBytes(destination, source, bytes);
}

void copyCommitted(std::uint64_t address, std::span<std::byte> destination) {
    if (Accessible(reinterpret_cast<const void*>(address), destination.size())) {
        copyCommittedBytes(destination.data(), address, destination.size());
        return;
    }
    static const bool zeroWhole = std::getenv("APS5_ZERO_WHOLE_COMMITTED_READS") != nullptr;
    if (zeroWhole) std::memset(destination.data(), 0, destination.size());
    auto zeroedTo = address;
    for (const auto& [begin, end] : CommittedRanges(address, destination.size())) {
        if (!zeroWhole && begin > zeroedTo) std::memset(destination.data() + (zeroedTo - address), 0, static_cast<std::size_t>(begin - zeroedTo));
        copyUnlessUnmapped(destination.data() + (begin - address), begin, static_cast<std::size_t>(end - begin));
        zeroedTo = end;
    }
    if (!zeroWhole && zeroedTo < address + destination.size()) std::memset(destination.data() + (zeroedTo - address), 0, static_cast<std::size_t>(address + destination.size() - zeroedTo));
}

}

void ReadCommitted(std::uint64_t address, std::span<std::byte> destination) {
    if (destination.empty()) return;
    if (const auto hook = lazyStoreHook.load(std::memory_order_acquire)) hook(address, destination.size());
    if (const auto fence = deferredFenceHook.load(std::memory_order_acquire)) fence(address, destination.size());
    GuardRead(address, destination.size());
    FlushGpuWrites(address, destination.size());
    const TimedAccess timed(CounterRead, destination.size());
    CountReadCaller(__builtin_return_address(0));
    copyCommitted(address, destination);
}

void ReadCommittedUnsynced(std::uint64_t address, std::span<std::byte> destination) {
    if (destination.empty()) return;
    if (const auto hook = lazyStoreHook.load(std::memory_order_acquire)) hook(address, destination.size());
    if (const auto fence = deferredFenceHook.load(std::memory_order_acquire)) fence(address, destination.size());
    GuardRead(address, destination.size());
    const TimedAccess timed(CounterRead, destination.size());
    CountReadCaller(__builtin_return_address(0));
    copyCommitted(address, destination);
}

bool EqualsCommitted(std::uint64_t address, std::span<const std::byte> bytes) {
    if (bytes.empty()) return true;
    FlushGpuWrites(address, bytes.size());
    return EqualsCommittedUnsynced(address, bytes);
}

bool EqualsCommittedUnsynced(std::uint64_t address, std::span<const std::byte> bytes) {
    if (bytes.empty()) return true;
    if (const auto hook = lazyStoreHook.load(std::memory_order_acquire)) hook(address, bytes.size());
    if (const auto fence = deferredFenceHook.load(std::memory_order_acquire)) fence(address, bytes.size());
    const TimedAccess timed(CounterCompare, bytes.size());
    if (Accessible(reinterpret_cast<const void*>(address), bytes.size())) return equalsCommittedBytes(address, bytes.data(), bytes.size());
    for (const auto& [begin, end] : CommittedRanges(address, bytes.size())) {
        if (!equalsCommittedBytes(begin, bytes.data() + (begin - address), static_cast<std::size_t>(end - begin))) return false;
    }
    return true;
}

Compare CompareMapped(std::uint64_t address, std::span<const std::byte> bytes) {
    if (bytes.empty()) return Compare::Equal;
    if (const auto hook = lazyStoreHook.load(std::memory_order_acquire)) hook(address, bytes.size());
    if (const auto fence = deferredFenceHook.load(std::memory_order_acquire)) fence(address, bytes.size());
    if (address == 0 || bytes.size() > std::numeric_limits<std::uintptr_t>::max() - address) return Compare::Unmapped;
    auto outcome = Compare::Equal;
    // Each run's page state is consulted right before its bytes are compared.
    const bool queried = describePages(static_cast<std::uintptr_t>(address), bytes.size(), [&](const PageRun& run) {
        if (!run.readable) outcome = Compare::Unmapped;
        else if (std::memcmp(reinterpret_cast<const void*>(run.begin), bytes.data() + (run.begin - address), static_cast<std::size_t>(run.end - run.begin)) != 0) outcome = Compare::Differs;
        return outcome == Compare::Equal;
    });
    return queried ? outcome : Compare::Unmapped;
}

Compare CopyMapped(std::uint64_t address, std::span<std::byte> out) {
    if (out.empty()) return Compare::Equal;
    if (const auto hook = lazyStoreHook.load(std::memory_order_acquire)) hook(address, out.size());
    if (const auto fence = deferredFenceHook.load(std::memory_order_acquire)) fence(address, out.size());
    if (address == 0 || out.size() > std::numeric_limits<std::uintptr_t>::max() - address) return Compare::Unmapped;
    auto outcome = Compare::Equal;
    const bool queried = describePages(static_cast<std::uintptr_t>(address), out.size(), [&](const PageRun& run) {
        if (!run.readable) outcome = Compare::Unmapped;
        else std::memcpy(out.data() + (run.begin - address), reinterpret_cast<const void*>(run.begin), static_cast<std::size_t>(run.end - run.begin));
        return outcome == Compare::Equal;
    });
    return queried ? outcome : Compare::Unmapped;
}

Compare CopyMapped(std::uint64_t address, std::span<std::byte> out, bool checked) {
    if (!checked) return CopyMapped(address, out);
    if (out.empty()) return Compare::Equal;
    if (const auto hook = lazyStoreHook.load(std::memory_order_acquire)) hook(address, out.size());
    if (const auto fence = deferredFenceHook.load(std::memory_order_acquire)) fence(address, out.size());
    if (address == 0 || out.size() > std::numeric_limits<std::uintptr_t>::max() - address) return Compare::Unmapped;
#ifdef _WIN32
    if (address > userAddressLimit() || out.size() - 1 > userAddressLimit() - address) return Compare::Unmapped;
    return readPages(out.data(), address, out.size()) ? Compare::Equal : Compare::Unmapped;
#else
    return GuestCopyIn_nid_no_patch(out.data(), reinterpret_cast<const void*>(address), out.size()) ? Compare::Equal : Compare::Unmapped;
#endif
}

Compare CopyMappedGuarded(std::uint64_t address, std::span<std::byte> out, bool& owned) {
    owned = false;
#ifdef _WIN32
    if (out.empty()) return Compare::Equal;
    if (address == 0 || out.size() > std::numeric_limits<std::uintptr_t>::max() - address) return Compare::Unmapped;
    if (const auto hook = lazyStoreHook.load(std::memory_order_acquire)) hook(address, out.size());
    if (const auto fence = deferredFenceHook.load(std::memory_order_acquire)) fence(address, out.size());
    if (GuestArena::GuestArenaCopyOwned_nid_no_patch(static_cast<std::uintptr_t>(address), out.data(), out.size())) {
        owned = true;
        return Compare::Equal;
    }
    if (address > userAddressLimit() || out.size() - 1 > userAddressLimit() - address) return Compare::Unmapped;
    return readPages(out.data(), address, out.size()) ? Compare::Equal : Compare::Unmapped;
#else
    return CopyMapped(address, out, true);
#endif
}

void WriteChangedCommitted(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original) {
    require(current.size() == original.size(), "write-back snapshot sizes differ");
    if (Accessible(reinterpret_cast<const void*>(address), current.size(), true)) {
        WriteChanged(address, current, original);
        return;
    }
    for (const auto& [begin, end] : CommittedRanges(address, current.size(), true)) {
        const auto offset = static_cast<std::size_t>(begin - address);
        const auto length = static_cast<std::size_t>(end - begin);
        WriteChanged(begin, current.subspan(offset, length), original.subspan(offset, length));
    }
}

#if defined(__x86_64__) || defined(_M_X64)
static std::uint32_t changedByteCount(__m128i same) {
    const auto sums = _mm_sad_epu8(_mm_and_si128(same, _mm_set1_epi8(1)), _mm_setzero_si128());
    return 16u - static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_add_epi32(sums, _mm_unpackhi_epi64(sums, sums))));
}
#endif

std::uint32_t ChangedByteStoreLimit() {
#if defined(__x86_64__) || defined(_M_X64)
    static const auto limit = [] {
#if defined(_MSC_VER) && !defined(__clang__)
        int registers[4]{};
        __cpuid(registers, 0);
        const auto ebx = static_cast<unsigned>(registers[1]), ecx = static_cast<unsigned>(registers[2]), edx = static_cast<unsigned>(registers[3]);
#else
        unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
        if (__get_cpuid(0, &eax, &ebx, &ecx, &edx) == 0) return 4u;
#endif
        return ebx == 0x68747541u && edx == 0x69746e65u && ecx == 0x444d4163u ? 16u : 4u;
    }();
    return limit;
#else
    return 16u;
#endif
}

void WriteChanged(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original) {
    WriteChanged(address, current, original, ChangedByteStoreLimit());
}

static void writeChanged(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original, [[maybe_unused]] std::uint32_t byteStoreLimit, bool keepGuest) {
    require(current.size() == original.size(), "write-back snapshot sizes differ");
    BreakProofs();
    if (current.empty()) return;
    // A store: the hook sync it makes orders this write after a pending GPU write, whatever a read
    // site above it was doing (the [hooksync] line counts stores apart).
    const ReadSiteScope site(ReadSite::Store);
    FlushGpuWrites(address, current.size());
    const TimedAccess timed(CounterChangedWrite, current.size());
    auto* destination = reinterpret_cast<std::byte*>(address);
    const auto size = current.size();
    // A byte the guest rewrote since the snapshot (a CPU store, or a file read into memory the game
    // reused after waiting for the GPU) is newer than this result and keeps its value, so only bytes
    // still holding the snapshot are written. APS5_NO_WRITEBACK_MERGE=1 writes every changed byte.
    static const bool merge = std::getenv("APS5_NO_WRITEBACK_MERGE") == nullptr;
    keepGuest = keepGuest && merge;
    std::size_t kept = 0;
    // Stamped like a GPU write: a collect memoized for this packet would not see the page fault.
    storeOwn(address, size, true, [&](std::span<std::byte> image) {
        CheckRange(destination, size, 1, true);
        std::size_t firstChanged = size;
        std::size_t lastChanged = 0;
        std::size_t at = 0;
#if defined(__x86_64__) || defined(_M_X64)
        bool masked = false;
        for (; at + 16 <= size; at += 16) {
            const auto bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(current.data() + at));
            const auto uploaded = _mm_loadu_si128(reinterpret_cast<const __m128i*>(original.data() + at));
            auto same = _mm_cmpeq_epi8(bytes, uploaded);
            if (keepGuest) {
                const auto rewritten = _mm_xor_si128(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(destination + at)), uploaded), _mm_set1_epi8(-1));
                kept += static_cast<std::size_t>(std::popcount(static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_andnot_si128(same, rewritten)))));
                same = _mm_or_si128(same, rewritten);
            }
            const auto equal = static_cast<std::uint32_t>(_mm_movemask_epi8(same));
            if (equal == 0xffffu) continue;
            const auto changed = ~equal & 0xffffu;
            if (equal == 0) _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + at), bytes);
            else if (byteStoreLimit < 16 && changedByteCount(same) > byteStoreLimit) {
                _mm_maskmoveu_si128(bytes, _mm_xor_si128(same, _mm_set1_epi8(-1)), reinterpret_cast<char*>(destination + at));
                masked = true;
            } else {
                for (auto bits = changed; bits != 0; bits &= bits - 1) {
                    const auto index = at + static_cast<std::size_t>(std::countr_zero(bits));
                    destination[index] = current[index];
                }
            }
            firstChanged = std::min(firstChanged, at + static_cast<std::size_t>(std::countr_zero(changed)));
            lastChanged = std::max(lastChanged, at + 32 - static_cast<std::size_t>(std::countl_zero(changed)));
        }
        if (masked) _mm_sfence();
#endif
        for (; at < size; ++at) {
            if (current[at] == original[at]) continue;
            if (keepGuest && destination[at] != original[at]) {
                ++kept;
                continue;
            }
            destination[at] = current[at];
            firstChanged = std::min(firstChanged, at);
            lastChanged = std::max(lastChanged, at + 1);
        }
        for (std::size_t index = 0; index < image.size(); ++index) {
            image[index] = current[index] != original[index] && !(keepGuest && image[index] != original[index]) ? current[index] : image[index];
        }
        return firstChanged < lastChanged ? std::pair<std::uint64_t, std::uint64_t>{address + firstChanged, address + lastChanged} : std::pair<std::uint64_t, std::uint64_t>{0, 0};
    });
    static std::atomic<std::uint32_t> reported{0};
    if (kept != 0 && reported.fetch_add(1, std::memory_order_relaxed) < 20) std::fprintf(stderr, "[guestmem] write-back of 0x%llx+0x%zx kept %zu bytes the guest rewrote since the snapshot\n", static_cast<unsigned long long>(address), size, kept);
}

void WriteChanged(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original, std::uint32_t byteStoreLimit) {
    writeChanged(address, current, original, byteStoreLimit, false);
}

void WriteChangedKeepingGuest(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original) {
    writeChanged(address, current, original, ChangedByteStoreLimit(), true);
}

static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free && std::atomic_ref<std::uint8_t>::is_always_lock_free);

std::size_t WriteChangedIfHeld(std::uint64_t address, std::span<const std::byte> current, std::span<const std::byte> original, std::uint64_t unchangedSince) {
    require(current.size() == original.size(), "write-back snapshot sizes differ");
    BreakProofs();
    if (current.empty()) return 0;
    const ReadSiteScope site(ReadSite::Store);
    FlushGpuWrites(address, current.size());
    const TimedAccess timed(CounterChangedWrite, current.size());
    auto* destination = reinterpret_cast<std::uint8_t*>(address);
    const auto size = current.size();
    std::size_t lost = 0;
    storeOwn(address, size, false, [&](std::span<std::byte>) {
        CheckRange(destination, size, 1, true);
        if (unchangedSince != 0u) {
            auto& tracker = Tracker();
            if (!tracker.cpuUnchanged(address, size, unchangedSince) || tracker.refusedSince(address, address + size, unchangedSince)) {
                lost = size;
                return std::pair<std::uint64_t, std::uint64_t>{0, 0};
            }
        }
        std::size_t firstChanged = size;
        std::size_t lastChanged = 0;
        const auto exchange = [&](std::size_t index) {
            if (current[index] == original[index]) return;
            auto expected = std::to_integer<std::uint8_t>(original[index]);
            firstChanged = std::min(firstChanged, index);
            lastChanged = std::max(lastChanged, index + 1);
            if (!std::atomic_ref<std::uint8_t>(destination[index]).compare_exchange_strong(expected, std::to_integer<std::uint8_t>(current[index]))) ++lost;
        };
        std::size_t at = 0;
        for (; at < size && lost == 0 && ((address + at) & 7u) != 0u; ++at) exchange(at);
        for (; at + 8 <= size && lost == 0; at += 8) {
            std::uint64_t wanted, held;
            std::memcpy(&wanted, current.data() + at, 8);
            std::memcpy(&held, original.data() + at, 8);
            if (wanted == held) continue;
            firstChanged = std::min(firstChanged, at);
            lastChanged = std::max(lastChanged, at + 8);
            if (std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(destination + at)).compare_exchange_strong(held, wanted)) continue;
            for (std::size_t index = at; index < at + 8; ++index) exchange(index);
        }
        for (; at < size && lost == 0; ++at) exchange(at);
        return firstChanged < lastChanged ? std::pair<std::uint64_t, std::uint64_t>{address + firstChanged, address + lastChanged} : std::pair<std::uint64_t, std::uint64_t>{0, 0};
    }, false);
    return lost;
}

void Write(std::uint64_t address, std::span<const std::byte> source, std::size_t alignment) {
    if (source.empty()) return;
    // A store, as in WriteChanged.
    const ReadSiteScope site(ReadSite::Store);
    FlushGpuWrites(address, source.size());
    const TimedAccess timed(CounterWrite, source.size());
    CountWriteCaller(__builtin_return_address(0));
    auto* destination = reinterpret_cast<void*>(address);
    // Stamped like a GPU write: a collect memoized for this packet would not see the page fault.
    StoreOwnBytes(address, source.size(), [&](std::span<std::byte> image) {
        CheckRange(destination, source.size(), alignment, true);
        if (source.size() >= PageBytes) copyOwnBytes(address, source);
        else std::memcpy(destination, source.data(), source.size());
        if (!image.empty()) std::memcpy(image.data(), source.data(), source.size());
    });
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (profile) {
        if (const auto hook = writeProfileHook.load(std::memory_order_acquire)) {
            const auto packet = CurrentPacket();
            hook(address, source.size(), packet.queue, packet.opcode);
        }
    }
}

}

extern "C" void AgcDriverCheckGuestMemory_nid_postfix(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable) {
    AgcDriver::GuestMemory::CheckRange(pointer, bytes, alignment, writable);
}

extern "C" void AgcDriverStoreGuestLabel_nid_postfix(std::uint64_t* pointer, std::uint64_t value) {
    AgcDriver::GuestMemory::StoreOwnBytes(reinterpret_cast<std::uintptr_t>(pointer), sizeof(*pointer), [&](std::span<std::byte> image) {
        AgcDriver::GuestMemory::CheckRange(pointer, sizeof(*pointer), std::atomic_ref<std::uint64_t>::required_alignment, true);
        std::atomic_ref(*pointer).store(value, std::memory_order_release);
        if (!image.empty()) std::memcpy(image.data(), &value, sizeof(value));
    });
}
