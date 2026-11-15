#include "prx/libSceAgcDriver/Graphics/include/GpuAuthorityCensus.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"
#include "prx/libSceAgcDriver/Graphics/include/LazyPublish.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"
#include "prx/libc/include/GuestArena.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <array>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics::ArchC::LazyPublish {

namespace {

constexpr std::size_t MaxEntries = 4096;
constexpr std::size_t EagerPerSecond = 64;
constexpr std::uint64_t PageBytes = 4096;

struct Counters {
    std::atomic<std::uint64_t> deferred{0}, renewed{0}, services{0}, serviceNs{0}, lockedServices{0}, lockedWrites{0}, storeHooks{0}, storeHookBytes{0}, storeHookOrphans{0}, storeHookNested{0}, pageServices{0}, partial{0}, eager{0}, eagerKeys{0}, dropped{0}, drained{0}, verified{0}, newer{0}, overlaps{0}, full{0};
    std::atomic<std::uint64_t> renewalFallbacks{0};
    std::array<std::atomic<std::uint64_t>, GuestArena::WtWatchSkipCauses> watchSkips{};
    Counters() {
        const auto add = [](const char* name, const std::atomic<std::uint64_t>& counter) { AgcDriver::ArchB::Recorder::RegisterCounter(name, counter); };
        add("c_lazy_publish_deferred", deferred);
        add("c_lazy_publish_renewed", renewed);
        add("c_lazy_publish_services", services);
        add("c_lazy_publish_service_ns", serviceNs);
        add("c_lazy_publish_locked_services", lockedServices);
        add("c_lazy_publish_locked_writes", lockedWrites);
        add("c_lazy_store_hooks", storeHooks);
        add("c_lazy_store_hook_bytes", storeHookBytes);
        add("c_lazy_store_hook_orphans", storeHookOrphans);
        add("c_lazy_store_hook_nested", storeHookNested);
        add("c_lazy_publish_page_services", pageServices);
        add("c_lazy_publish_partial", partial);
        add("c_lazy_publish_eager", eager);
        add("c_lazy_publish_eager_keys", eagerKeys);
        add("c_lazy_publish_dropped", dropped);
        add("c_lazy_publish_drained", drained);
        add("c_lazy_publish_verified", verified);
        add("c_lazy_publish_verify_newer", newer);
        add("c_lazy_publish_overlaps", overlaps);
        add("c_lazy_publish_full", full);
        add("n5a_watch_renewal_fallbacks", renewalFallbacks);
        constexpr std::array<const char*, GuestArena::WtWatchSkipCauses> names{"n5a_watch_skip_noleaf", "n5a_watch_skip_untracked", "n5a_watch_skip_excluded", "n5a_watch_skip_unchecked", "n5a_watch_skip_exec", "n5a_watch_skip_held", "n5a_watch_skip_busy", "n5a_watch_skip_watched", "n5a_watch_skip_protect"};
        for (std::size_t i = 0; i < names.size(); ++i) add(names[i], watchSkips[i]);
    }
};

Counters& counters() {
    static Counters made;
    return made;
}

struct Entry {
    std::uint64_t end;
    bool serving;
};

struct Key {
    std::uint64_t begin;
    std::uint64_t end;
    bool operator==(const Key&) const = default;
};

struct KeyHash {
    std::size_t operator()(const Key& key) const { return static_cast<std::size_t>((key.begin * 0x9e3779b97f4a7c15ull) ^ key.end); }
};

struct State {
    AgcDriver::Mutex mutex;
    std::map<std::uint64_t, Entry> live;
    std::unordered_map<Key, std::deque<std::uint64_t>, KeyHash> touches;
    std::unordered_set<Key, KeyHash> eager;
    std::uint64_t cursor = 0;
    double nsPerTick = 1.0;
    std::vector<std::uint64_t> entries = std::vector<std::uint64_t>(512);
    std::vector<std::uint64_t> stamps = std::vector<std::uint64_t>(512);
    State() {
        GuestArena::GuestWtFaultTicks_nid_no_patch(&nsPerTick);
        cursor = GuestArena::GuestWtFaultWatchLogHead_nid_no_patch();
    }
};

State& state() {
    static State made;
    return made;
}

// s.live.size(), readable without the mutex (the store hook's fast exit).
std::atomic<std::size_t> liveCount{0};
// Inside a publication of a lazy range on this thread (the service, or the store hook's own): a driver
// store then belongs to it and only needs its pages opened.
struct PublishingSlot {};
int publishing() { return HostThreadSlot<int, PublishingSlot>::Get(); }
void publishing(int value) { HostThreadSlot<int, PublishingSlot>::Set(value); }

void syncCount(const State& s) {
    liveCount.store(s.live.size(), std::memory_order_relaxed);
}

bool verify() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_ARCHB_C_LAZY_PUBLISH_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    return on;
}

std::map<std::uint64_t, Entry>::iterator containing(State& s, std::uint64_t address) {
    auto it = s.live.upper_bound(address);
    if (it == s.live.begin()) return s.live.end();
    --it;
    return it->second.end > address ? it : s.live.end();
}

void publish(std::uint64_t begin, std::uint64_t end) {
    const Recorder::FenceReasonScope fenceReason(Recorder::FenceReason::LazyPublish);
    const auto current = GuestMemory::CurrentReadSite();
    const GuestMemory::ReadSiteScope readSite(GuestArena::GuestWtFaultLazyPreservationEnabled_nid_no_patch() && current == GuestMemory::ReadSite::Store ? GuestMemory::ReadSite::Unknown : current);
    GuestMemory::FlushGpuWrites(begin, static_cast<std::size_t>(end - begin));
    GuestArena::GuestWtFaultUnwatch_nid_no_patch(static_cast<std::uintptr_t>(begin), static_cast<std::size_t>(end - begin));
}

void drainLocked(State& s, std::vector<std::pair<std::uint64_t, std::uint64_t>>& orphans) {
    if (GuestArena::GuestWtFaultWatchLogHead_nid_no_patch() == s.cursor) return;
    for (;;) {
        bool lost = false;
        const auto count = GuestArena::GuestWtFaultWatchLogRead_nid_no_patch(&s.cursor, s.entries.data(), s.stamps.data(), s.entries.size(), &lost);
        for (std::size_t i = 0; i < count; ++i) {
            const auto kind = s.entries[i] & GuestArena::WtWatchKindMask;
            if (kind == GuestArena::WtWatchRead || kind == GuestArena::WtWatchWrite) continue;
            const auto it = containing(s, s.entries[i] & ~(PageBytes - 1));
            if (it == s.live.end() || it->second.serving) continue;
            orphans.emplace_back(it->first, it->second.end);
            s.live.erase(it);
            syncCount(s);
            counters().dropped.fetch_add(1, std::memory_order_relaxed);
        }
        if (count < s.entries.size()) return;
    }
}

void publishOrphans(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& orphans) {
    if (orphans.empty()) return;
    const bool entered = GuestArena::GuestWtFaultEnterLazyService_nid_no_patch();
    publishing(publishing() + 1);
    for (const auto& [begin, end] : orphans) publish(begin, end);
    publishing(publishing() - 1);
    if (entered) GuestArena::GuestWtFaultLeaveLazyService_nid_no_patch();
}

// APS5_ARCHB_C_LAZY_PUBLISH_STACKS=N (diagnostic): the first N services on a thread holding the GPU
// mutex print their stack as module+offset (s3/symbolize.py), naming the driver code that touched a
// lazy range without a hook.
void traceLockedService(std::uintptr_t page, bool write) {
#ifdef _WIN32
    static std::atomic<int> left{[] {
        const char* text = std::getenv("APS5_ARCHB_C_LAZY_PUBLISH_STACKS");
        return text != nullptr ? std::atoi(text) : 0;
    }()};
    if (left.load(std::memory_order_relaxed) <= 0 || left.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
    // The handler runs without unwind tables (-fno-asynchronous-unwind-tables): the faulting instruction,
    // then the stack words that point into a module (return-address candidates, innermost first).
    std::uintptr_t ip = 0;
    std::uintptr_t sp = 0;
    GuestArena::GuestWtFaultFaultContext_nid_no_patch(&ip, &sp);
    std::string text = "[lazy-publish] locked service " + std::string(write ? "write" : "read") + " page 0x";
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%llx:", static_cast<unsigned long long>(page));
    text += buffer;
    const auto describe = [&](std::uintptr_t address) {
        HMODULE module = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) || module == nullptr) return false;
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(module, name, sizeof(name));
        const char* base = std::strrchr(name, '\\');
        std::snprintf(buffer, sizeof(buffer), " %s+0x%llx", base != nullptr ? base + 1 : name, static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)));
        text += buffer;
        return true;
    };
    if (ip != 0) describe(ip);
    text += " | stack:";
    int found = 0;
    for (std::uintptr_t at = sp; sp != 0 && at < sp + 4096 && found < 24; at += sizeof(std::uintptr_t)) {
        MEMORY_BASIC_INFORMATION memory{};
        if ((at & 4095) == 0 || at == sp) {
            if (VirtualQuery(reinterpret_cast<void*>(at), &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT || (memory.Protect & (PAGE_READWRITE | PAGE_READONLY)) == 0) break;
        }
        const auto value = *reinterpret_cast<const std::uintptr_t*>(at);
        if (value < 0x10000) continue;
        MEMORY_BASIC_INFORMATION code{};
        if (VirtualQuery(reinterpret_cast<void*>(value), &code, sizeof(code)) != sizeof(code) || code.Type != MEM_IMAGE || (code.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE | PAGE_EXECUTE_READWRITE)) == 0) continue;
        if (describe(value)) ++found;
    }
    std::fprintf(stderr, "%s\n", text.c_str());
#else
    static_cast<void>(page);
    static_cast<void>(write);
#endif
}

void service(std::uintptr_t page, bool write) {
    const auto cause = GuestArena::GuestWtFaultLazyServiceCause_nid_no_patch();
    const auto* name = write ? "lazy CPU write" : "lazy CPU read";
    if (cause == GuestArena::WtLazyCause::Range) name = "host write preservation";
    if (cause == GuestArena::WtLazyCause::Hold) name = "host hold";
    if (cause == GuestArena::WtLazyCause::Open) name = "driver page open";
    if (cause == GuestArena::WtLazyCause::Unwatch) name = "watch removal";
    const PublishSiteScope site(name);
    struct ReadObservation {
        std::uintptr_t page;
        bool write;
        ~ReadObservation() { if (!write) GpuAuthorityCensus::CpuRead(page, 4096, 35); }
    } readObservation{page, write};
    auto& s = state();
    auto& c = counters();
    const auto start = GuestArena::GuestWtFaultTicks_nid_no_patch(nullptr);
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> orphans;
    {
        std::lock_guard lock(s.mutex);
        drainLocked(s, orphans);
        const auto it = containing(s, page);
        if (it != s.live.end() && !it->second.serving) {
            it->second.serving = true;
            begin = it->first;
            end = it->second.end;
        }
    }
    publishOrphans(orphans);
    if (GuestMemory::GpuMutex().HeldByThisThread()) {
        c.lockedServices.fetch_add(1, std::memory_order_relaxed);
        if (write) c.lockedWrites.fetch_add(1, std::memory_order_relaxed);
        traceLockedService(page, write);
    }
    if (end == 0) {
        c.pageServices.fetch_add(1, std::memory_order_relaxed);
        publishing(publishing() + 1);
        publish(page, page + PageBytes);
        publishing(publishing() - 1);
        return;
    }
    if (verify() && (StorageTexture::PendingStoreInside(begin, static_cast<std::size_t>(end - begin)) || AnyShadowedOverlaps(begin, static_cast<std::size_t>(end - begin)))) c.newer.fetch_add(1, std::memory_order_relaxed);
    if (verify()) c.verified.fetch_add(1, std::memory_order_relaxed);
    publishing(publishing() + 1);
    publish(begin, end);
    publishing(publishing() - 1);
    const auto now = GuestArena::GuestWtFaultTicks_nid_no_patch(nullptr);
    {
        std::lock_guard lock(s.mutex);
        if (const auto it = s.live.find(begin); it != s.live.end() && it->second.serving) s.live.erase(it);
        syncCount(s);
        auto& seen = s.touches[{begin, end}];
        const auto window = static_cast<std::uint64_t>(1e9 / s.nsPerTick);
        while (!seen.empty() && seen.front() + window < now) seen.pop_front();
        seen.push_back(now);
        if (seen.size() > EagerPerSecond && s.eager.insert({begin, end}).second) c.eagerKeys.fetch_add(1, std::memory_order_relaxed);
        if (s.touches.size() > 65536) s.touches.clear();
    }
    c.services.fetch_add(1, std::memory_order_relaxed);
    c.serviceNs.fetch_add(static_cast<std::uint64_t>(static_cast<double>(now - start) * s.nsPerTick), std::memory_order_relaxed);
}

bool switchOn() {
    return AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::CLazyPublish) && GuestArena::GuestWtFaultEnabled_nid_no_patch();
}

}

// APS5_ARCHB_C_LAZY_STORE_HOOK: every driver store into guest memory (GuestMemory::storeOwn) and every
// driver read through GuestMemory (ReadCommitted for a storage texture's upload, EqualsCommittedUnsynced
// for writeBackLayers' untracked compare: the readers P5b's fault trace named) calls this first. A store into a lazy range would fault there, under the GPU mutex, and the fault would run the
// service: publish the whole range (re-entering the flush hook) and open it. The hook does the same
// work explicitly, before the store: the overlapping entries leave the table and are published with
// their pages opened by each publishing store right before it writes (the nested calls below), so no
// page faults. Lazy pages without an entry (a range a hook or service dropped while a page stayed
// watched) are published page-range-wise, as the service's page path would.
void beforeDriverAccess(std::uint64_t address, std::size_t bytes) {
    if (liveCount.load(std::memory_order_relaxed) == 0 && publishing() == 0) return;
    if (!AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::CLazyStoreHook) && !GuestArena::GuestWtFaultLazyPreservationEnabled_nid_no_patch()) return;
    if (GuestArena::GuestWtFaultWatchedPages_nid_no_patch(static_cast<std::uintptr_t>(address), bytes) == 0) return;
    auto& c = counters();
    if (publishing() != 0) {
        // An access of the publication in progress on this thread: open its pages, then it proceeds.
        c.storeHookNested.fetch_add(1, std::memory_order_relaxed);
        GuestArena::GuestWtFaultUnwatch_nid_no_patch(static_cast<std::uintptr_t>(address), bytes);
        return;
    }
    auto& s = state();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> orphans;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> taken;
    const auto end = address + bytes;
    {
        std::lock_guard lock(s.mutex);
        drainLocked(s, orphans);
        auto it = s.live.upper_bound(address);
        if (it != s.live.begin() && std::prev(it)->second.end > address) --it;
        while (it != s.live.end() && it->first < end) {
            if (it->second.end > address && !it->second.serving) {
                taken.emplace_back(it->first, it->second.end);
                it = s.live.erase(it);
            } else {
                ++it;
            }
        }
        syncCount(s);
    }
    publishOrphans(orphans);
    if (taken.empty()) {
        c.storeHookOrphans.fetch_add(1, std::memory_order_relaxed);
        taken.emplace_back(address & ~(PageBytes - 1), (end + PageBytes - 1) & ~(PageBytes - 1));
    }
    const bool entered = GuestArena::GuestWtFaultEnterLazyService_nid_no_patch();
    publishing(publishing() + 1);
    for (const auto& [begin, stop] : taken) {
        publish(begin, stop);
        c.storeHooks.fetch_add(1, std::memory_order_relaxed);
        c.storeHookBytes.fetch_add(stop - begin, std::memory_order_relaxed);
    }
    publishing(publishing() - 1);
    if (entered) GuestArena::GuestWtFaultLeaveLazyService_nid_no_patch();
}

bool Active() {
    if (!switchOn()) return false;
    static const bool installed = [] {
        static_cast<void>(counters());
        static_cast<void>(state());
        GuestArena::GuestWtFaultSetLazyService_nid_no_patch(&service);
        GuestMemory::SetLazyStoreHook(&beforeDriverAccess);
        return true;
    }();
    return installed;
}

std::size_t Entries() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.live.size();
}

void PublishAll() {
    auto& s = state();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> all;
    {
        std::lock_guard lock(s.mutex);
        for (auto it = s.live.begin(); it != s.live.end();) {
            if (it->second.serving) {
                ++it;
                continue;
            }
            all.emplace_back(it->first, it->second.end);
            it = s.live.erase(it);
        }
        syncCount(s);
    }
    counters().drained.fetch_add(all.size(), std::memory_order_relaxed);
    publishOrphans(all);
}

void BeforeLabelStores() {
    static std::atomic<bool> everOn{false};
    if (switchOn()) {
        everOn.store(true, std::memory_order_relaxed);
        return;
    }
    if (everOn.load(std::memory_order_relaxed) && Entries() != 0) PublishAll();
}

namespace {

bool enroll(std::uint64_t address, std::size_t bytes, bool shadow = false) {
    auto& c = counters();
    const auto end = address + bytes;
    auto& s = state();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> orphans;
    bool deferred = false;
    bool unwatch = false;
    {
        std::lock_guard lock(s.mutex);
        drainLocked(s, orphans);
        if (s.eager.contains({address, end})) {
            c.eager.fetch_add(1, std::memory_order_relaxed);
        } else if (const auto it = containing(s, address); it != s.live.end() && end <= it->second.end) {
            if (!it->second.serving) {
                const auto pages = ((end + PageBytes - 1) / PageBytes) - (address / PageBytes);
                if (GuestArena::GuestWtFaultLazyPages_nid_no_patch(address, bytes) == pages) {
                    c.renewed.fetch_add(1, std::memory_order_relaxed);
                    deferred = true;
                } else {
                    c.renewalFallbacks.fetch_add(1, std::memory_order_relaxed);
                    s.live.erase(it);
                    syncCount(s);
                    c.dropped.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } else {
            auto next = s.live.lower_bound(address);
            if ((next != s.live.end() && next->first < end) || it != s.live.end()) {
                c.overlaps.fetch_add(1, std::memory_order_relaxed);
            } else if (s.live.size() >= MaxEntries) {
                c.full.fetch_add(1, std::memory_order_relaxed);
            } else {
                const auto pages = ((end + PageBytes - 1) / PageBytes) - (address / PageBytes);
                GuestArena::GuestWtFaultSetWatchSplit_nid_no_patch(AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::CWatchSplit));
                std::array<std::uint64_t, GuestArena::WtWatchSkipCauses> causes{};
                const auto watched = GuestArena::GuestWtFaultWatchLazyCauses_nid_no_patch(static_cast<std::uintptr_t>(address), bytes, causes.data());
                if (shadow) for (std::size_t i = 0; i < causes.size(); ++i) c.watchSkips[i].fetch_add(causes[i], std::memory_order_relaxed);
                if (watched != pages) {
                    unwatch = watched != 0;
                    c.partial.fetch_add(1, std::memory_order_relaxed);
                    static std::atomic<int> traces{[] {
                        const char* text = std::getenv("APS5_ARCHB_C_LAZY_PUBLISH_TRACE");
                        return text != nullptr ? std::atoi(text) : 0;
                    }()};
                    if (traces.load(std::memory_order_relaxed) > 0 && traces.fetch_sub(1, std::memory_order_relaxed) > 0) {
                        std::uintptr_t arenaBase = 0;
                        std::size_t arenaBytes = 0;
                        GuestArena::GuestArenaRange_nid_postfix(&arenaBase, &arenaBytes);
                        std::fprintf(stderr, "[lazy-publish] not protectable 0x%llx+0x%zx: %llu of %llu pages watchable (untracked %llu excluded %llu unchecked %llu held %llu busy %llu noleaf %llu watched %llu protect %llu, word 0x%llx); arena 0x%llx-0x%llx\n", static_cast<unsigned long long>(address), bytes, static_cast<unsigned long long>(watched), static_cast<unsigned long long>(pages), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipUntracked]), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipExcluded]), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipUnchecked]), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipHeld]), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipBusy]), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipNoLeaf]), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipWatched]), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipProtect]), static_cast<unsigned long long>(GuestArena::GuestWtFaultWord_nid_no_patch(static_cast<std::uintptr_t>(address))), static_cast<unsigned long long>(arenaBase), static_cast<unsigned long long>(arenaBase + arenaBytes));
                    }
                } else {
                    s.live[address] = {end, false};
                    syncCount(s);
                    c.deferred.fetch_add(1, std::memory_order_relaxed);
                    deferred = true;
                }
            }
        }
    }
    if (unwatch) {
        const PublishSiteScope site("watch fallback");
        publishOrphans({{address, end}});
    }
    publishOrphans(orphans);
    return deferred;
}

}

bool DeferLabelStore(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || !Active()) return false;
    if (!StorageTexture::PendingStoreInside(address, bytes) && !AnyShadowedOverlaps(address, bytes)) return false;
    if (verify()) static_cast<void>(StorageTexture::FlushPending(address, bytes, nullptr, "readback before label"));
    return enroll(address, bytes) || verify();
}

bool DeferShadowRange(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || !Active()) return false;
    return enroll(address, bytes, true);
}

LabelStoreOutcome PeekLabelStore(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || !Active()) return LabelStoreOutcome::Eager;
    if (!StorageTexture::PendingStoreInside(address, bytes) && !AnyShadowedOverlaps(address, bytes)) return LabelStoreOutcome::Nothing;
    const auto end = address + bytes;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.eager.contains({address, end})) return LabelStoreOutcome::Eager;
    const auto it = containing(s, address);
    if (it != s.live.end() && end <= it->second.end) return it->second.serving ? LabelStoreOutcome::Eager : LabelStoreOutcome::Renew;
    const auto next = s.live.lower_bound(address);
    if ((next != s.live.end() && next->first < end) || it != s.live.end() || s.live.size() >= MaxEntries) return LabelStoreOutcome::Eager;
    return LabelStoreOutcome::New;
}

Counts Totals() {
    auto& c = counters();
    Counts counts;
    counts.deferred = c.deferred.load();
    counts.renewed = c.renewed.load();
    counts.services = c.services.load() + c.pageServices.load();
    counts.partial = c.partial.load();
    counts.eager = c.eager.load();
    counts.dropped = c.dropped.load();
    counts.verified = c.verified.load();
    counts.newer = c.newer.load();
    counts.lockedServices = c.lockedServices.load();
    counts.storeHooks = c.storeHooks.load();
    return counts;
}

}
