#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_LABELCENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_LABELCENSUS_HPP

// Arch C label census (APS5_ARCHB_C_LABEL_CENSUS=1, env only, diagnostic): the steps of recording a label
// (recordLabelsForPacket, recordDeferredLabels, WriteLabelOnGpu, the compute-sync prewait) and the label
// chain: when each label was decoded and recorded, by which thread, and for every WAIT_REG_MEM that blocked,
// which thread recorded the label it waited on and how its wait splits into "producer had not decoded the
// label yet", "label decoded but not recorded" (deferral, incl. the compute-sync prewait) and "label recorded,
// waiter not yet awake" (wake latency). Counters are per thread (ArchB recorder, c9_*).

#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"
#include "ThreadOwned.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::ArchB::LabelCensus {

inline bool On() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_ARCHB_C_LABEL_CENSUS");
        return value != nullptr && value[0] == '1';
    }();
    return on;
}

inline std::uint64_t Now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Per thread: the decode time of the deferred label being recorded (0: an immediate label), the
// compute-sync prewait of the current packet record, and the prewait's left-out ranges.
struct ThreadState {
    std::uint64_t decodeNs = 0;
    std::uint64_t syncNs = 0;
    // C_LABEL_LAZY_PREWAIT: the ranges the current packet record's prewait left out (for its _VERIFY check).
    std::vector<std::pair<std::uint64_t, std::uint64_t>> prewaitSkipped;
};

inline ThreadState& Thread() {
    struct Slot {};
    auto* state = HostThreadSlot<ThreadState*, Slot>::Get();
    if (state == nullptr) {
        ShaderRecompiler::ThreadOwned(state);
        HostThreadSlot<ThreadState*, Slot>::Set(state);
    }
    return *state;
}

enum Producer : std::uint8_t { PQ0, PCommitter, P20, P28, P30, P38, P40, P48, P50, POther, PNone, PCount };

inline Producer CurrentProducer() {
    if (GuestMemory::IsCommitterThread()) return PCommitter;
    const auto tag = GuestMemory::GpuLockThreadTag();
    if (tag == 0) return PQ0;
    if (tag >= 0x20 && tag <= 0x50 && (tag & 7u) == 0) return static_cast<Producer>(P20 + (tag - 0x20) / 8);
    return POther;
}

enum Metric : std::uint8_t { MN, MNs, MPreNs, MDeferredNs, MWakeNs, MSyncNs, MLateN, MLateNs, MCount };

#define APS5_C9_WAIT_NAMES(kind, p) {"c9_" kind "_" p "_n", "c9_" kind "_" p "_ns", "c9_" kind "_" p "_pre_ns", "c9_" kind "_" p "_deferred_ns", "c9_" kind "_" p "_wake_ns", "c9_" kind "_" p "_sync_ns", "c9_" kind "_" p "_late_n", "c9_" kind "_" p "_late_ns"}
#define APS5_C9_WAIT_TABLE(kind) {APS5_C9_WAIT_NAMES(kind, "q0"), APS5_C9_WAIT_NAMES(kind, "cmt"), APS5_C9_WAIT_NAMES(kind, "x20"), APS5_C9_WAIT_NAMES(kind, "x28"), APS5_C9_WAIT_NAMES(kind, "x30"), APS5_C9_WAIT_NAMES(kind, "x38"), APS5_C9_WAIT_NAMES(kind, "x40"), APS5_C9_WAIT_NAMES(kind, "x48"), APS5_C9_WAIT_NAMES(kind, "x50"), APS5_C9_WAIT_NAMES(kind, "other"), APS5_C9_WAIT_NAMES(kind, "none")}
inline constexpr const char* WaitNames[PCount][MCount] = APS5_C9_WAIT_TABLE("wait");
inline constexpr const char* LandNames[PCount][MCount] = APS5_C9_WAIT_TABLE("land");
#undef APS5_C9_WAIT_TABLE
#undef APS5_C9_WAIT_NAMES
inline constexpr const char* DeferNames[PCount] = {"c9_lbl_defer_ns_q0", "c9_lbl_defer_ns_cmt", "c9_lbl_defer_ns_x20", "c9_lbl_defer_ns_x28", "c9_lbl_defer_ns_x30", "c9_lbl_defer_ns_x38", "c9_lbl_defer_ns_x40", "c9_lbl_defer_ns_x48", "c9_lbl_defer_ns_x50", "c9_lbl_defer_ns_other", "c9_lbl_defer_ns_none"};
inline constexpr const char* ReasonNames[8] = {"c9_wl_r0", "c9_wl_r1", "c9_wl_r2", "c9_wl_r3", "c9_wl_r4", "c9_wl_r5", "c9_wl_r6", "c9_wl_r7"};

struct Entry {
    std::uint64_t decodeNs;
    std::uint64_t recordNs;
    std::uint64_t syncNs;
    Producer producer;
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, Entry> dwords;
};

inline Registry& registry() {
    static Registry* made = new Registry;
    return *made;
}

// A label became visible to other queues (recorded into the recorder's table, or stored by the CPU), on the
// recording thread.
inline void Recorded(std::uint64_t address, std::size_t bytes) {
    auto& thread = Thread();
    const auto now = Now();
    const auto decode = thread.decodeNs != 0 ? thread.decodeNs : now;
    const auto producer = CurrentProducer();
    Recorder::Count("c9_lbl_n");
    if (thread.decodeNs != 0) {
        Recorder::Count("c9_lbl_deferred_n");
        Recorder::Count(DeferNames[producer], static_cast<double>(now - decode));
        if (thread.syncNs != 0) Recorder::Count("c9_lbl_after_sync_n");
    }
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    for (std::uint64_t dword = address & ~3ull; dword < address + std::min<std::size_t>(bytes, 32); dword += 4) r.dwords[dword] = Entry{decode, now, thread.syncNs, producer};
}

// One WAIT_REG_MEM on the waiting thread; `Blocked()` once it was not satisfied at entry.
class WaitScope {
public:
    WaitScope(std::uint64_t address, bool requireMemory) : on(On()), landing(requireMemory), address(address), start(on ? Now() : 0) {}
    void Blocked() { blocked = on; }
    ~WaitScope() {
        if (!on) return;
        if (!blocked) {
            Recorder::Count(landing ? "c9_land_entry_n" : "c9_wait_entry_n");
            return;
        }
        const auto end = Now();
        Entry entry{0, 0, 0, PNone};
        bool found = false;
        {
            auto& r = registry();
            std::lock_guard lock(r.mutex);
            if (const auto it = r.dwords.find(address & ~3ull); it != r.dwords.end()) {
                entry = it->second;
                found = true;
            }
        }
        const auto& names = landing ? LandNames : WaitNames;
        const auto producer = found ? entry.producer : PNone;
        const auto& row = names[producer];
        Recorder::Count(row[MN]);
        Recorder::Count(row[MNs], static_cast<double>(end - start));
        if (!found) return;
        if (entry.recordNs <= start || entry.recordNs > end) {
            // Recorded before the wait began (or after it ended): the wait was for the GPU, the memory or
            // another store, not for this label's record.
            Recorder::Count(row[MLateN]);
            Recorder::Count(row[MLateNs], static_cast<double>(end - start));
            return;
        }
        const auto decode = std::clamp(entry.decodeNs, start, entry.recordNs);
        const auto pre = decode - start;
        const auto deferred = entry.recordNs - decode;
        Recorder::Count(row[MPreNs], static_cast<double>(pre));
        Recorder::Count(row[MDeferredNs], static_cast<double>(deferred));
        Recorder::Count(row[MWakeNs], static_cast<double>(end - entry.recordNs));
        if (entry.syncNs != 0) Recorder::Count(row[MSyncNs], static_cast<double>(std::min(entry.syncNs, deferred)));
    }

private:
    bool on;
    bool landing;
    bool blocked = false;
    std::uint64_t address;
    std::uint64_t start;
};

// Times consecutive steps of a scope into named counters (each Step adds the time since the last mark).
class Steps {
public:
    Steps() : on(On()), mark(on ? Now() : 0), first(mark) {}
    void Step(const char* name) {
        if (!on) return;
        const auto now = Now();
        Recorder::Count(name, static_cast<double>(now - mark));
        mark = now;
    }
    std::uint64_t Total() const { return on ? Now() - first : 0; }
    bool Active() const { return on; }

private:
    bool on;
    std::uint64_t mark;
    std::uint64_t first;
};

}

#endif
