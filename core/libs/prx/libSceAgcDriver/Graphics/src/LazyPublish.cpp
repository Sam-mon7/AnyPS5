#include "prx/libSceAgcDriver/Graphics/include/LazyPublish.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"
#include "prx/libc/include/GuestArena.hpp"

#include <cstdio>

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
    std::atomic<std::uint64_t> deferred{0}, renewed{0}, services{0}, serviceNs{0}, lockedServices{0}, pageServices{0}, partial{0}, eager{0}, eagerKeys{0}, dropped{0}, drained{0}, verified{0}, newer{0}, overlaps{0}, full{0};
    Counters() {
        const auto add = [](const char* name, const std::atomic<std::uint64_t>& counter) { AgcDriver::ArchB::Recorder::RegisterCounter(name, counter); };
        add("c_lazy_publish_deferred", deferred);
        add("c_lazy_publish_renewed", renewed);
        add("c_lazy_publish_services", services);
        add("c_lazy_publish_service_ns", serviceNs);
        add("c_lazy_publish_locked_services", lockedServices);
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
            counters().dropped.fetch_add(1, std::memory_order_relaxed);
        }
        if (count < s.entries.size()) return;
    }
}

void publishOrphans(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& orphans) {
    for (const auto& [begin, end] : orphans) publish(begin, end);
}

void service(std::uintptr_t page, bool) {
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
    if (GuestMemory::GpuMutex().HeldByThisThread()) c.lockedServices.fetch_add(1, std::memory_order_relaxed);
    if (end == 0) {
        c.pageServices.fetch_add(1, std::memory_order_relaxed);
        publish(page, page + PageBytes);
        return;
    }
    if (verify() && (StorageTexture::PendingStoreInside(begin, static_cast<std::size_t>(end - begin)) || AnyShadowedOverlaps(begin, static_cast<std::size_t>(end - begin)))) c.newer.fetch_add(1, std::memory_order_relaxed);
    if (verify()) c.verified.fetch_add(1, std::memory_order_relaxed);
    publish(begin, end);
    const auto now = GuestArena::GuestWtFaultTicks_nid_no_patch(nullptr);
    {
        std::lock_guard lock(s.mutex);
        if (const auto it = s.live.find(begin); it != s.live.end() && it->second.serving) s.live.erase(it);
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

bool Active() {
    if (!switchOn()) return false;
    static const bool installed = [] {
        static_cast<void>(counters());
        static_cast<void>(state());
        GuestArena::GuestWtFaultSetLazyService_nid_no_patch(&service);
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

bool DeferLabelStore(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || !Active()) return false;
    auto& c = counters();
    if (!StorageTexture::PendingStoreInside(address, bytes) && !AnyShadowedOverlaps(address, bytes)) return false;
    if (verify()) static_cast<void>(StorageTexture::FlushPending(address, bytes, nullptr, "readback before label"));
    const auto end = address + bytes;
    auto& s = state();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> orphans;
    bool deferred = false;
    {
        std::lock_guard lock(s.mutex);
        drainLocked(s, orphans);
        if (s.eager.contains({address, end})) {
            c.eager.fetch_add(1, std::memory_order_relaxed);
        } else if (const auto it = containing(s, address); it != s.live.end() && end <= it->second.end) {
            if (!it->second.serving) {
                c.renewed.fetch_add(1, std::memory_order_relaxed);
                deferred = true;
            }
        } else {
            auto next = s.live.lower_bound(address);
            if ((next != s.live.end() && next->first < end) || it != s.live.end()) {
                c.overlaps.fetch_add(1, std::memory_order_relaxed);
            } else if (s.live.size() >= MaxEntries) {
                c.full.fetch_add(1, std::memory_order_relaxed);
            } else {
                const auto pages = ((end + PageBytes - 1) / PageBytes) - (address / PageBytes);
                const auto watched = GuestArena::GuestWtFaultWatchLazy_nid_no_patch(static_cast<std::uintptr_t>(address), bytes);
                if (watched != pages) {
                    GuestArena::GuestWtFaultUnwatch_nid_no_patch(static_cast<std::uintptr_t>(address), bytes);
                    c.partial.fetch_add(1, std::memory_order_relaxed);
                    static std::atomic<int> traces{[] {
                        const char* text = std::getenv("APS5_ARCHB_C_LAZY_PUBLISH_TRACE");
                        return text != nullptr ? std::atoi(text) : 0;
                    }()};
                    if (traces.load(std::memory_order_relaxed) > 0 && traces.fetch_sub(1, std::memory_order_relaxed) > 0) {
                        std::uintptr_t arenaBase = 0;
                        std::size_t arenaBytes = 0;
                        GuestArena::GuestArenaRange_nid_postfix(&arenaBase, &arenaBytes);
                        std::array<std::uint64_t, GuestArena::WtWatchSkipCauses> causes{};
                        GuestArena::GuestWtFaultWatchCauses_nid_no_patch(static_cast<std::uintptr_t>(address), bytes, causes.data());
                        GuestArena::GuestWtFaultUnwatch_nid_no_patch(static_cast<std::uintptr_t>(address), bytes);
                        std::fprintf(stderr, "[lazy-publish] not protectable 0x%llx+0x%zx: %llu of %llu pages watchable (untracked %llu excluded %llu unchecked %llu held %llu busy %llu noleaf %llu watched %llu, word 0x%llx); arena 0x%llx-0x%llx\n", static_cast<unsigned long long>(address), bytes, static_cast<unsigned long long>(watched), static_cast<unsigned long long>(pages), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipUntracked]), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipExcluded]), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipUnchecked]), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipHeld]), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipBusy]), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipNoLeaf]), static_cast<unsigned long long>(causes[GuestArena::WtWatchSkipWatched]), static_cast<unsigned long long>(GuestArena::GuestWtFaultWord_nid_no_patch(static_cast<std::uintptr_t>(address))), static_cast<unsigned long long>(arenaBase), static_cast<unsigned long long>(arenaBase + arenaBytes));
                    }
                } else {
                    s.live[address] = {end, false};
                    c.deferred.fetch_add(1, std::memory_order_relaxed);
                    deferred = true;
                }
            }
        }
    }
    publishOrphans(orphans);
    return deferred || verify();
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
    return counts;
}

}
