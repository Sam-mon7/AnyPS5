#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP

#ifdef _WIN32
#include "GuestArena.hpp"
#include "MemoryLockProbe.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <map>
#include <memory>
#include <optional>
#include <vector>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace GuestArena {

class WindowsMappings {
    using ObservationKey = std::pair<std::uintptr_t, std::uint64_t>;
public:
    enum class Collection { All, Armed, Fresh };

    MemoryLockProbe::Snapshot Probe() const {
        auto result = mutex.Read();
        result.mapDetails = mapProbe.Read();
        return result;
    }

    static WindowsMappings& Get() {
        static WindowsMappings mappings;
        return mappings;
    }

    bool HasPageObserver() const { return pageObserver.load(std::memory_order_acquire) != nullptr; }

    static bool ObservationRequested() {
        static const bool enabled = [] {
            const auto* tracker = std::getenv("APS5_PAGE_TRACKER");
            const auto* arm = std::getenv("APS5_PAGE_TRACKER_ARM");
            return tracker != nullptr && std::string_view(tracker) == "1" && arm != nullptr && std::string_view(arm) == "1";
        }();
        return enabled;
    }

    void EnablePageObservation(bool (*ready)() noexcept) {
        if (!ObservationRequested()) return;
        MemoryLockProbe::Lock lock(mutex, "page-observation-enable", 0);
        if (!HasPageObserver()) return;
        observationReady = ready;
        observationEnabled.store(true, std::memory_order_release);
        lastObservationCheckpoint = MemoryLockProbe::Now();
        for (auto& [key, page] : cpuObservations) armObservation(key, page.touched);
    }

    bool ArmPageObservation(std::uintptr_t address, std::size_t bytes) {
        if (!observationEnabled.load(std::memory_order_acquire)) return false;
        MemoryLockProbe::Lock lock(mutex, "page-observation-arm", bytes);
        if (!wholeGpuPages(address, bytes)) return refuseObservation(PageArmRefusal::InvalidRange);
        bool result = true;
        for (auto cursor = address; cursor < address + bytes;) {
            const auto base = cursor & ~(pageBytes - 1);
            const auto stop = std::min(address + bytes, base + pageBytes);
            const auto found = observationViews.find(base);
            if (found == observationViews.end()) result = refuseObservation(PageArmRefusal::UnknownMapping);
            else result = armObservation(found->second, subpagesOf(base, cursor, stop)) && result;
            cursor = stop;
        }
        return result;
    }

    bool PageObservationCheckpoint(bool frame) {
        if (!observationEnabled.load(std::memory_order_acquire)) return false;
        const auto start = MemoryLockProbe::Now();
        MemoryLockProbe::Lock lock(mutex, "page-observation-checkpoint", 0);
        if (!frame && start - lastObservationCheckpoint < 1000000000ull) return false;
        lastObservationCheckpoint = start;
        std::uint64_t written = observationRetiredPages;
        observationRetiredPages = 0;
        for (auto& [key, page] : cpuObservations) {
            written += std::popcount(page.written);
            page.written = 0;
            if (page.touched != 0) armObservation(key, page.touched);
        }
        const auto faults = observationFaultSerial.load(std::memory_order_relaxed);
        const auto faultDelta = faults - lastObservationFaultSerial;
        lastObservationFaultSerial = faults;
        if (frame) {
            ++observationCounts.frames;
            observationCounts.framePages += written;
            observationCounts.maxFramePages = std::max(observationCounts.maxFramePages, written);
            observationCounts.frameFaults += faultDelta;
            observationCounts.maxFrameFaults = std::max(observationCounts.maxFrameFaults, faultDelta);
            ++observationCounts.frameFaultHistogram[timingBucket(faultDelta)];
        } else {
            ++observationCounts.fallbackWindows;
            observationCounts.fallbackPages += written;
            observationCounts.fallbackFaults += faultDelta;
        }
        noteTiming(observationCounts.checkpoint, MemoryLockProbe::Now() - start);
        return true;
    }

    PageObservationStatistics PageObservationCounts() {
        if (!ObservationRequested()) return {};
        MemoryLockProbe::Lock lock(mutex, "page-observation-counts", 0);
        auto result = observationCounts;
        for (const auto& [key, page] : cpuObservations) result.outstanding += std::popcount(page.armed);
        std::lock_guard faultLock(observationFaultMutex);
        result.faults = observationFaultCounts;
        result.mappingWait = observationMappingWait;
        return result;
    }

    void SetPageObservationFaultHook(void (*hook)() noexcept) { observationFaultHook.store(hook, std::memory_order_release); }

    static bool PageReadFaultsRequested() {
        static const bool enabled = [] {
            for (const auto* name : {"APS5_BUFFER_CACHE_READ_FAULTS", "APS5_BUFFER_CACHE_WRITES", "APS5_BUFFER_CACHE", "APS5_PAGE_TRACKER", "APS5_PAGE_TRACKER_ARM"}) {
                const auto* value = std::getenv(name);
                if (value == nullptr || std::string_view(value) != "1") return false;
            }
            for (const auto* name : {"APS5_RESIDENT_HEAP_MIRRORS", "APS5_VRAM_WRITABLE", "APS5_VRAM_LAZY_WRITES"}) {
                const auto* value = std::getenv(name);
                if (value != nullptr && std::string_view(value) == "1") return false;
            }
            return true;
        }();
        return enabled;
    }

    PageArmRefusal ArmPageRead(std::uintptr_t address, std::shared_ptr<void> owner, const void* original, bool (*download)(void*) noexcept, void* context) {
        if (!PageReadFaultsRequested() || !observationEnabled.load(std::memory_order_acquire)) return PageArmRefusal::Metadata;
        MemoryLockProbe::Lock lock(mutex, "page-read-arm", subpageBytes);
        if (!wholeGpuPages(address, subpageBytes) || owner == nullptr || original == nullptr || download == nullptr) return PageArmRefusal::InvalidRange;
        const auto found = observationViews.find(address & ~(pageBytes - 1));
        if (found == observationViews.end()) return PageArmRefusal::UnknownMapping;
        if (observationReady == nullptr || !observationReady()) return PageArmRefusal::Metadata;
        if (compare == nullptr) return PageArmRefusal::Identity;
        const auto& key = found->second;
        const auto sub = (address % pageBytes) / subpageBytes;
        auto& slot = pageReads[key][sub];
        if (slot != nullptr) return PageArmRefusal::Metadata;
        if (observationOldAliases || observationHostAliases.contains(key)) return PageArmRefusal::HostAlias;
        for (const auto alias : observationPhysical.at(key)) {
            const auto& view = views.at(alias);
            if (!writable(view.protection)) return PageArmRefusal::Permission;
            if (view.page->pins != 0) return PageArmRefusal::Pinned;
            if (view.hostWrites != 0) return PageArmRefusal::HostWriter;
            if (view.page->hostAliases != 0) return PageArmRefusal::HostAlias;
            if (gpuOwner(alias + sub * subpageBytes) != 0) return PageArmRefusal::Metadata;
        }
        if (!armObservation(key, static_cast<std::uint8_t>(1u << sub))) return PageArmRefusal::Protection;
        const auto& view = views.at(address & ~(pageBytes - 1));
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto offset = view.offset + address % pageBytes;
        const auto lead = offset % system.dwAllocationGranularity;
        auto watch = std::make_shared<PageRead>();
        watch->alias = map(view.section->handle, GetCurrentProcess(), nullptr, offset - lead, lead + subpageBytes, 0, PAGE_READWRITE, nullptr, 0);
        if (watch->alias == nullptr) return PageArmRefusal::Protection;
        watch->unmap = unmap;
        watch->destination = static_cast<unsigned char*>(watch->alias) + lead;
        watch->original = static_cast<const unsigned char*>(original);
        watch->owner = std::move(owner);
        watch->download = download;
        watch->context = context;
        slot = std::move(watch);
        const auto mask = static_cast<std::uint8_t>(1u << sub);
        if (!applyObservation(key, mask)) {
            slot.reset();
            if (!applyObservation(key, mask)) fail("restore refused page read watch");
            return PageArmRefusal::Protection;
        }
        return PageArmRefusal::Count;
    }

    bool ResolvePageRead(std::uintptr_t address) {
        const auto handled = handlePageRead(address, false);
        return handled.has_value() && *handled;
    }

    bool PublishPageRead(std::uintptr_t address, const void* source) {
        MemoryLockProbe::Lock lock(mutex, "page-read-publish", subpageBytes);
        const auto found = observationViews.find(address & ~(pageBytes - 1));
        if (found == observationViews.end() || source == nullptr) return false;
        const auto sub = (address % pageBytes) / subpageBytes;
        const auto reads = pageReads.find(found->second);
        if (reads == pageReads.end()) return false;
        auto& slot = reads->second[sub];
        if (slot == nullptr) return false;
        const auto watch = slot;
        const auto* bytes = static_cast<const unsigned char*>(source);
        static_assert(std::atomic_ref<unsigned char>::is_always_lock_free);
        for (std::size_t at = 0; at < subpageBytes; ++at) {
            if (bytes[at] == watch->original[at]) continue;
            auto expected = watch->original[at];
            std::atomic_ref<unsigned char>(watch->destination[at]).compare_exchange_strong(expected, bytes[at], std::memory_order_relaxed);
        }
        const auto mask = static_cast<std::uint8_t>(1u << sub);
        ++views.at(address & ~(pageBytes - 1)).page->generation[sub];
        cleanRanges.clear();
        for (const auto alias : observationPhysical.at(found->second)) views.at(alias).armed &= static_cast<std::uint8_t>(~mask);
        observeWrite(address & ~(pageBytes - 1), mask, PageEvent::GpuReadback);
        slot.reset();
        if (!applyObservation(found->second, mask)) fail("restore downloaded page read watch");
        gpuProtectionSerial.fetch_add(1, std::memory_order_release);
        observeState(found->second);
        return true;
    }

    std::size_t PageObserverMetadataBytes() {
        MemoryLockProbe::Lock lock(mutex, "page-observer-size", 0);
        const auto nodes = [](const auto& map) { return map.size() * (sizeof(typename std::decay_t<decltype(map)>::value_type) + 4 * sizeof(void*)); };
        auto bytes = nodes(observationViews) + nodes(observationPhysical) + nodes(observationAliases) + nodes(observationHostAliases) + observationSections.capacity() * sizeof(std::shared_ptr<Section>);
        if (ObservationRequested()) bytes += nodes(cpuObservations);
        if (PageReadFaultsRequested()) {
            bytes += nodes(pageReads);
            for (const auto& [key, reads] : pageReads) for (const auto& read : reads) if (read != nullptr) bytes += sizeof(PageRead);
        }
        for (const auto& [key, aliases] : observationPhysical) bytes += aliases.capacity() * sizeof(std::uintptr_t);
        for (const auto& [key, aliases] : observationAliases) bytes += aliases.capacity() * sizeof(ObservationKey);
        return bytes;
    }

    void SetPageObserver(PageObserver callback) {
        MemoryLockProbe::Lock lock(mutex, "page-observer", 0);
        if (hasPageReads(1, std::numeric_limits<std::uintptr_t>::max() - 1)) throw std::runtime_error("cannot replace the page observer with active read watches");
        if (observationEnabled.exchange(false)) {
            for (auto& [key, page] : cpuObservations) {
                const auto armed = page.armed;
                page.armed = 0;
                applyObservation(key, armed);
            }
        }
        for (auto& [address, view] : views) view.cpuObservation = nullptr;
        cpuObservations.clear();
        pageReads.clear();
        pageObserver.store(callback, std::memory_order_release);
        observationViews.clear();
        observationPhysical.clear();
        observationSections.clear();
        observationAliases.clear();
        observationHostAliases.clear();
        observationOldAliases = untrackedAliases.load(std::memory_order_relaxed) != 0;
        if (callback == nullptr) return;
        for (const auto& [address, view] : views) observeMap(address);
        for (const auto& [key, aliases] : observationPhysical) observeState(key);
    }

    void VisitPageAliases(std::uintptr_t address, std::size_t bytes, void (*visitor)(std::uintptr_t, std::size_t, void*), void* context) {
        MemoryLockProbe::Lock lock(mutex, "page-aliases", bytes);
        if (bytes == 0 || bytes > std::numeric_limits<std::uintptr_t>::max() - address) return;
        for (auto cursor = address; cursor < address + bytes;) {
            const auto base = cursor & ~(pageBytes - 1);
            const auto found = observationViews.find(base);
            auto stop = std::min(address + bytes, base + pageBytes);
            if (found == observationViews.end()) {
                const auto next = observationViews.lower_bound(cursor);
                stop = next == observationViews.end() ? address + bytes : std::min(address + bytes, next->first);
                visitor(cursor, stop - cursor, context);
            } else for (const auto alias : observationPhysical.at(found->second)) visitor(alias + cursor - base, stop - cursor, context);
            cursor = stop;
        }
    }

    void* Reserve(void* address, std::size_t bytes) {
        return allocate(GetCurrentProcess(), address, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    }

    std::vector<std::pair<std::uintptr_t, std::size_t>> Commit(void* address, std::size_t bytes, DWORD protection, std::size_t granule, bool watched) {
        std::vector<std::pair<std::uintptr_t, std::size_t>> created;
        MemoryLockProbe::Lock lock(mutex, "commit", bytes);
        observe(PageEvent::Invalidate, reinterpret_cast<std::uintptr_t>(address), bytes);
        const auto end = reinterpret_cast<std::uintptr_t>(address) + bytes;
        for (auto cursor = reinterpret_cast<std::uintptr_t>(address); cursor < end;) {
            const auto memory = query(cursor);
            const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
            if (memory.State == MEM_RESERVE) {
                const auto limit = std::min(end, cursor + granule);
                auto placeholderEnd = stop;
                while (placeholderEnd < limit) {
                    const auto next = query(placeholderEnd);
                    if (next.State != MEM_RESERVE) break;
                    placeholderEnd = reinterpret_cast<std::uintptr_t>(next.BaseAddress) + next.RegionSize;
                }
                const auto size = std::min(limit, placeholderEnd) - cursor;
                reset(cursor, size);
                const DWORD flags = MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | (watched ? MEM_WRITE_WATCH : 0);
                if (!allocate(GetCurrentProcess(), reinterpret_cast<void*>(cursor), size, flags, protection, nullptr, 0)) fail("replace guest placeholder with private memory");
                observe(PageEvent::PrivateMap, cursor, size, watched && readable(protection) ? PagePrivateWatch : PageUnsupported);
                if (!created.empty() && created.back().first + created.back().second == cursor) created.back().second += size;
                else created.emplace_back(cursor, size);
                cursor += size;
            } else {
                if (memory.State != MEM_COMMIT) throw std::runtime_error("guest memory is not committed");
                for (auto mapped = views.lower_bound(cursor & ~(pageBytes - 1)); mapped != views.end() && mapped->first < stop; ++mapped) {
                    mapped->second.protection = protection;
                    mapped->second.armed = 0;
                    invalidate(*mapped->second.page);
                }
                if (gpuFault.load(std::memory_order_acquire) != nullptr || observationEnabled.load(std::memory_order_relaxed)) applyGpuProtection(cursor, stop - cursor, protection);
                else {
                    DWORD previous;
                    if (!protectMemory(reinterpret_cast<void*>(cursor), stop - cursor, protection, &previous)) fail("protect guest memory");
                }
                cursor = stop;
            }
        }
        return created;
    }

    void Reset(void* address, std::size_t bytes) {
        MemoryLockProbe::Lock lock(mutex, "reset", bytes);
        reset(reinterpret_cast<std::uintptr_t>(address), bytes);
    }

    bool TracksGpuAccess() const { return gpuTracking(); }

    void Map(void* address, std::size_t bytes, HANDLE section, std::uint64_t offset, DWORD protection) {
        const MemoryLockProbe::MapProbe::Start started;
        MemoryLockProbe::Lock lock(mutex, "map", bytes);
        if (HasPageObserver()) {
            for (const auto& previous : observationSections) {
                if (!sameSection(previous->handle, section)) continue;
                for (auto it = observationPhysical.lower_bound({reinterpret_cast<std::uintptr_t>(previous->handle), offset}); it != observationPhysical.end() && it->first.first == reinterpret_cast<std::uintptr_t>(previous->handle) && it->first.second < offset + bytes; ++it) {
                    for (const auto alias : it->second) observe(PageEvent::Invalidate, alias, pageBytes);
                }
                break;
            }
        }
        MemoryLockProbe::MapProbe::Timer timer(mapProbe, started, bytes);
        auto cursor = reinterpret_cast<std::uintptr_t>(address);
        reset(cursor, bytes);
        timer.Step(MemoryLockProbe::MapStep::Setup);
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) fail("keep shared guest section");
        auto owned = std::make_shared<Section>(duplicate);
        if (gpuTracking()) {
            owned = canonicalGpuSection(std::move(owned));
            section = owned->handle;
        }
        if (RunViews()) {
            const auto end = cursor + bytes;
            timer.Step(MemoryLockProbe::MapStep::Split);
            timer.Step(MemoryLockProbe::MapStep::MapView);
            if (!map(section, GetCurrentProcess(), address, offset, bytes, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0)) fail("map a run of shared guest pages");
            timer.Step(MemoryLockProbe::MapStep::QueryCommit);
            for (auto at = cursor; at < end;) {
                const auto memory = query(at);
                const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                if (memory.State == MEM_RESERVE && !VirtualAlloc(reinterpret_cast<void*>(at), stop - at, MEM_COMMIT, PAGE_EXECUTE_READWRITE)) fail("commit a run of shared guest pages");
                at = stop;
            }
            timer.Step(MemoryLockProbe::MapStep::Protect);
            DWORD previous;
            if (!protectMemory(address, bytes, protection, &previous)) fail("protect a run of shared guest pages");
            timer.Step(MemoryLockProbe::MapStep::Bookkeeping);
            for (std::size_t done = 0; done < bytes; done += pageBytes) {
                const auto key = std::make_pair(reinterpret_cast<std::uintptr_t>(section), offset + done);
                auto shared = physical[key].lock();
                if (!shared) {
                    shared = std::make_shared<SharedPage>();
                    if (gpuTracking()) shared->section = owned;
                    physical[key] = shared;
                }
                const auto base = cursor + done;
                shared->aliases.push_back(base);
                views.emplace(base, View{shared, protection, {}, 0, owned, offset + done, 0});
                if (HasPageObserver()) observeMap(base);
                for (std::size_t sub = 0; sub < subpages; ++sub) if (shared->gpuOwners[sub] != 0) applyGpuPage(base + sub * subpageBytes);
                remember(freshRanges, base, base + pageBytes);
                invalidate(*shared);
                timer.PageDone();
            }
            runs.emplace(cursor, Run{bytes, owned, offset});
            return;
        }
        const bool lean = LeanMap();
        for (std::size_t done = 0; done < bytes; done += pageBytes) {
            timer.Step(MemoryLockProbe::MapStep::Split);
            void* page = reinterpret_cast<void*>(cursor + done);
            if (!lean) split(cursor + done, pageBytes);
            else if (done + pageBytes < bytes && !VirtualFree(page, pageBytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split guest placeholder suffix");
            timer.Step(MemoryLockProbe::MapStep::MapView);
            if (!map(section, GetCurrentProcess(), page, offset + done, pageBytes, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0)) fail("map shared guest page");
            timer.Step(MemoryLockProbe::MapStep::QueryCommit);
            const auto key = std::make_pair(reinterpret_cast<std::uintptr_t>(section), offset + done);
            std::shared_ptr<SharedPage> shared;
            if (lean) shared = physical[key].lock();
            if (!shared) {
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(page, &memory, sizeof(memory)) != sizeof(memory)) fail("query shared guest page");
                if (memory.State == MEM_RESERVE && !VirtualAlloc(page, pageBytes, MEM_COMMIT, PAGE_EXECUTE_READWRITE)) fail("commit shared guest page");
            }
            DWORD previous;
            timer.Step(MemoryLockProbe::MapStep::Protect);
            if (!protectMemory(page, pageBytes, protection, &previous)) fail("protect shared guest page");
            timer.Step(MemoryLockProbe::MapStep::Bookkeeping);
            if (!lean) shared = physical[key].lock();
            if (!shared) {
                shared = std::make_shared<SharedPage>();
                if (gpuTracking()) shared->section = owned;
                physical[key] = shared;
            }
            const auto base = cursor + done;
            shared->aliases.push_back(base);
            views.emplace(base, View{shared, protection, {}, 0, owned, offset + done, 0});
            if (HasPageObserver()) observeMap(base);
            for (std::size_t sub = 0; sub < subpages; ++sub) if (shared->gpuOwners[sub] != 0) applyGpuPage(base + sub * subpageBytes);
            remember(freshRanges, base, base + pageBytes);
            invalidate(*shared);
            timer.PageDone();
        }
    }

    void SetProtection(std::uintptr_t address, std::size_t bytes, DWORD protection) {
        MemoryLockProbe::Lock lock(mutex, "protect", bytes);
        if (hasPageReads(address, bytes)) throw std::runtime_error("cannot change protection with an active page read watch");
        observe(PageEvent::Invalidate, address, bytes);
        for (auto it = views.lower_bound(address); it != views.end() && it->first < address + bytes; ++it) {
            it->second.protection = protection;
            it->second.armed = 0;
            if (observationEnabled.load(std::memory_order_relaxed)) openObservation(it->first, allSubpages);
            invalidate(*it->second.page);
            if (HasPageObserver()) {
                const auto found = observationViews.find(it->first);
                if (found != observationViews.end()) for (const auto alias : observationPhysical.at(found->second)) observe(PageEvent::Invalidate, alias, pageBytes);
            }
        }
        for (auto it = gpuPages.lower_bound(address); it != gpuPages.end() && it->first < address + bytes; ++it) it->second.protection = protection;
    }

    void Pin(std::uintptr_t address, std::size_t bytes) {
        MemoryLockProbe::Lock lock(mutex, "pin", bytes);
        if (hasPageReads(address, bytes)) throw std::runtime_error("cannot pin memory with an active page read watch");
        if (gpuTracking()) {
            for (auto page = address & ~(subpageBytes - 1); page < address + bytes; page += subpageBytes) if (gpuPages.contains(page)) throw std::runtime_error("cannot pin memory with an active GPU write watch");
        }
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) {
            if (std::any_of(it->second.page->gpuOwners.begin(), it->second.page->gpuOwners.end(), [](auto owner) { return owner != 0; })) throw std::runtime_error("cannot pin shared memory with an active GPU write watch");
        }
        if (gpuTracking()) {
            for (auto page = address & ~(subpageBytes - 1); page < address + bytes; page += subpageBytes) if (!views.contains(page & ~(pageBytes - 1))) ++gpuPins[page];
        }
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) {
            auto& page = *it->second.page;
            ++page.pins;
            if (observationEnabled.load(std::memory_order_relaxed)) openObservation(it->first, allSubpages);
            for (const auto alias : page.aliases) {
                auto& view = views.at(alias);
                if (view.armed == 0) continue;
                protectSubpages(alias, view.armed, view.protection, "pin shared guest page writable");
                view.armed = 0;
            }
            invalidate(page);
            if (HasPageObserver()) observeAliasesState(it->first);
        }
    }

    void Unpin(std::uintptr_t address, std::size_t bytes) {
        MemoryLockProbe::Lock lock(mutex, "unpin", bytes);
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) {
            auto& page = *it->second.page;
            if (page.pins != 0) --page.pins;
            invalidate(page);
            if (HasPageObserver()) observeAliasesState(it->first);
        }
        for (auto it = gpuPins.lower_bound(address & ~(subpageBytes - 1)); it != gpuPins.end() && it->first < address + bytes;) {
            if (--it->second == 0) it = gpuPins.erase(it);
            else ++it;
        }
    }

    bool HandleWrite(std::uintptr_t address) {
        FaultTimer timer(*this, PageFaultKind::Legacy);
        if (timer.active) {
            if (timer.nested) return false;
            if (const auto handled = handlePageRead(address, true)) {
                if (!*handled) return false;
                timer.kind = PageFaultKind::GpuWrite;
            }
            if (const auto handled = handleObservationFault(address, timer)) return *handled;
        }
        if (const auto handled = handleGpuFault(address, true, timer.publishedCpu)) return *handled;
        MemoryLockProbe::Lock lock(mutex, "write-fault", 1);
        const auto base = address & ~(pageBytes - 1);
        const auto found = views.find(base);
        if (found == views.end() || !writable(found->second.protection)) { timer.kind = PageFaultKind::Rejected; return false; }
        auto& view = found->second;
        const auto written = subpagesOf(base, address, address + 1);
        if (HasPageObserver()) observeWrite(base, written, PageEvent::WriteFault);
        invalidate(*view.page, written);
        if (timer.active) {
            view.armed &= static_cast<std::uint8_t>(~written);
            applyGpuView(base, written);
        } else {
            protectSubpages(base, written, view.protection, "resume shared memory write");
            view.armed &= static_cast<std::uint8_t>(~written);
        }
        if (HasPageObserver()) observeAliasesState(base);
        return true;
    }

    bool HandleTransition(std::uintptr_t address) {
        FaultTimer timer(*this, PageFaultKind::GpuRead);
        if (timer.nested) return false;
        if (PageReadFaultsRequested()) if (const auto handled = handlePageRead(address, false)) return *handled;
        if (gpuAccessProtected.load(std::memory_order_acquire)) if (const auto handled = handleGpuFault(address, false)) return *handled;
        timer.kind = PageFaultKind::Stale;
        if (transitionOwner.load() == GetCurrentThreadId()) return false;
        bool recent = false;
        for (std::size_t slot = 0; slot < transitionSlots && !recent; ++slot) recent = transitionFrom[slot].load() <= address && address < transitionTo[slot].load();
        if (!recent) { timer.kind = PageFaultKind::Rejected; return false; }
        thread_local std::uintptr_t lastAddress = 0;
        thread_local int repeats = 0;
        if (address != lastAddress) {
            lastAddress = address;
            repeats = 0;
        }
        if (++repeats > 8) {
            repeats = 0;
            return false;
        }
        MemoryLockProbe::Lock lock(mutex, "transition-fault", 1);
        return true;
    }

    bool BeginHostWrite(std::uintptr_t address, std::size_t bytes) {
        if (PageReadFaultsRequested() && bytes != 0 && bytes <= std::numeric_limits<std::uintptr_t>::max() - address) {
            for (auto page = address & ~(subpageBytes - 1); page < address + bytes; page += subpageBytes)
                if (const auto handled = handlePageRead(page, true); handled && !*handled) return false;
        }
        const bool reserved = gpuTracking();
        if (reserved) {
            if (bytes > std::numeric_limits<std::uintptr_t>::max() - address) return false;
            {
                MemoryLockProbe::Lock lock(mutex, "gpu-host-write-reserve", bytes);
                for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) if (!writable(it->second.protection)) return false;
                for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) ++it->second.hostWrites;
                for (auto page = address & ~(subpageBytes - 1); page < address + bytes; page += subpageBytes) if (!views.contains(page & ~(pageBytes - 1))) ++gpuHostWrites[page];
            }
            try {
                for (auto page = address & ~(subpageBytes - 1); page < address + bytes; page += subpageBytes) {
                    if (const auto handled = handleGpuFault(page, true); handled && !*handled) {
                        EndHostWrite(address, bytes);
                        return false;
                    }
                }
            } catch (...) {
                EndHostWrite(address, bytes);
                throw;
            }
        }
        MemoryLockProbe::Lock lock(mutex, "host-write-begin", bytes);
        const auto first = views.lower_bound(address & ~(pageBytes - 1));
        const auto end = address + bytes;
        for (auto it = first; it != views.end() && it->first < end; ++it) {
            if (!reserved && !writable(it->second.protection)) return false;
        }
        for (auto it = first; it != views.end() && it->first < end; ++it) {
            auto& view = it->second;
            if (!reserved) ++view.hostWrites;
            const auto written = subpagesOf(it->first, address, end);
            if (HasPageObserver()) observeWrite(it->first, written, PageEvent::HostWrite);
            if (observationEnabled.load(std::memory_order_relaxed)) {
                noteObservationWrite(it->first, written);
                openObservation(it->first, written);
            }
            invalidate(*view.page, written);
            const auto opened = static_cast<std::uint8_t>(view.armed & written);
            if (opened == 0) continue;
            protectSubpages(it->first, opened, view.protection, "open shared memory to a host write");
            view.armed &= static_cast<std::uint8_t>(~opened);
            if (HasPageObserver()) observeAliasesState(it->first);
        }
        return true;
    }

    void EndHostWrite(std::uintptr_t address, std::size_t bytes) {
        MemoryLockProbe::Lock lock(mutex, "host-write-end", bytes);
        const auto end = address + bytes;
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < end; ++it) {
            --it->second.hostWrites;
            invalidate(*it->second.page, subpagesOf(it->first, address, end));
            if (HasPageObserver()) {
                observeWrite(it->first, subpagesOf(it->first, address, end), PageEvent::HostWrite);
                observeAliasesState(it->first);
            }
        }
        for (auto it = gpuHostWrites.lower_bound(address & ~(subpageBytes - 1)); it != gpuHostWrites.end() && it->first < end;) {
            if (--it->second == 0) it = gpuHostWrites.erase(it);
            else ++it;
        }
    }

    void* MapAlias(std::uintptr_t address, std::size_t bytes) {
        MemoryLockProbe::Lock lock(mutex, "map-alias", bytes);
        const auto refuse = [&](const char* reason) {
            char text[192];
            std::snprintf(text, sizeof(text), "read-write alias of shared guest memory 0x%llx+0x%llx: %s", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), reason);
            return std::runtime_error(text);
        };
        if (address % pageBytes != 0 || bytes % pageBytes != 0 || bytes == 0) throw refuse("the range is not made of whole shared pages");
        auto view = views.find(address);
        if (view == views.end()) throw refuse("the range does not start at a shared view");
        const auto section = view->second.section;
        const auto offset = view->second.offset;
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        bool contiguous = true;
        for (std::size_t done = 0; done < bytes; done += pageBytes) {
            const auto page = views.find(address + done);
            if (page == views.end()) throw refuse("the range is not a run of shared views");
            if (std::any_of(page->second.page->gpuOwners.begin(), page->second.page->gpuOwners.end(), [](auto owner) { return owner != 0; })) throw refuse("a GPU write watch is active");
            if (hasPageReads(page->first, pageBytes)) throw refuse("a page read watch is active");
            if (page->second.offset != offset + done || (page->second.section != section && !sameSection(page->second.section->handle, section->handle))) contiguous = false;
        }
        if (!contiguous) {
            auto* alias = mapAliasRuns(address, bytes);
            rememberGpuAlias(alias, address, bytes);
            return alias;
        }
        const auto lead = offset % system.dwAllocationGranularity;
        void* alias = map(section->handle, GetCurrentProcess(), nullptr, offset - lead, lead + bytes, 0, PAGE_READWRITE, nullptr, 0);
        if (alias == nullptr) {
            char text[160];
            std::snprintf(text, sizeof(text), "MapViewOfFile3 of a read-write alias of shared guest memory 0x%llx+0x%llx", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), text);
        }
        auto* result = static_cast<char*>(alias) + lead;
        rememberGpuAlias(result, address, bytes);
        return result;
    }

    void UnmapAlias(void* alias) {
        if (alias == nullptr) return;
        {
            MemoryLockProbe::Lock lock(mutex, "unmap-alias", 0);
            if (const auto found = splitAliases.find(reinterpret_cast<std::uintptr_t>(alias)); found != splitAliases.end()) {
                const auto error = releaseAlias(found->first, found->second);
                splitAliases.erase(found);
                if (error != ERROR_SUCCESS) throw std::system_error(static_cast<int>(error), std::system_category(), "unmap a run of a shared guest alias");
                forgetGpuAlias(alias);
                return;
            }
            if (gpuAliases.contains(reinterpret_cast<std::uintptr_t>(alias))) {
                SYSTEM_INFO system{};
                GetSystemInfo(&system);
                const auto base = reinterpret_cast<std::uintptr_t>(alias) & ~(static_cast<std::uintptr_t>(system.dwAllocationGranularity) - 1);
                if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(base), 0)) fail("unmap shared GPU alias");
                forgetGpuAlias(alias);
                return;
            }
        }
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto base = reinterpret_cast<std::uintptr_t>(alias) & ~(static_cast<std::uintptr_t>(system.dwAllocationGranularity) - 1);
        if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(base), 0)) fail("unmap shared guest alias");
        if (observationEnabled.load(std::memory_order_acquire)) {
            MemoryLockProbe::Lock lock(mutex, "unmap-observation-alias", 0);
            if (HasPageObserver()) forgetObservationAlias(alias);
            untrackedAliases.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        if (HasPageObserver()) forgetObservationAlias(alias);
        untrackedAliases.fetch_sub(1, std::memory_order_relaxed);
    }

    bool Protection(std::uintptr_t address, std::uint32_t* protection) {
        MemoryLockProbe::Lock lock(mutex, "protection", 1);
        const auto found = views.find(address & ~(pageBytes - 1));
        if (found == views.end()) return false;
        *protection = found->second.protection;
        return true;
    }

    void SetGpuPageFault(bool (*callback)(std::uintptr_t, bool)) {
        MemoryLockProbe::Lock lock(mutex, "gpu-callback", 0);
        if (!gpuPages.empty() && callback != gpuFault.load()) throw std::runtime_error("cannot replace the GPU fault callback while pages are watched");
        if (callback != nullptr && untrackedAliases.load(std::memory_order_relaxed) != 0) throw std::runtime_error("close existing untracked host aliases before enabling GPU page watches");
        if (callback != nullptr && gpuFault.load() == nullptr) {
            std::map<std::pair<std::uintptr_t, std::uint64_t>, std::shared_ptr<SharedPage>> shared;
            for (const auto& [key, weak] : physical) if (auto page = weak.lock(); page != nullptr && page->section != nullptr && key.first == reinterpret_cast<std::uintptr_t>(page->section->handle)) shared.emplace(key, std::move(page));
            std::map<SharedPage*, std::shared_ptr<SharedPage>> moved;
            for (auto& [address, view] : views) {
                view.section = canonicalGpuSection(view.section);
                const auto key = std::make_pair(reinterpret_cast<std::uintptr_t>(view.section->handle), view.offset);
                auto [found, inserted] = shared.try_emplace(key, view.page);
                if (!moved.contains(view.page.get())) {
                    if (!inserted && found->second != view.page) {
                        found->second->pins += view.page->pins;
                        found->second->hostAliases += view.page->hostAliases;
                    }
                    moved.emplace(view.page.get(), found->second);
                }
                view.page = found->second;
                view.page->section = view.section;
                view.seen = {};
            }
            decltype(physical) canonical;
            for (const auto& [key, page] : shared) canonical.emplace(key, page);
            physical.swap(canonical);
            for (const auto& [key, page] : shared) page->aliases.clear();
            for (const auto& [address, view] : views) view.page->aliases.push_back(address);
            for (auto& [address, aliases] : gpuAliases) for (auto& page : aliases) if (const auto found = moved.find(page.get()); found != moved.end()) page = found->second;
            cleanRanges.clear();
        }
        gpuFault.store(callback, std::memory_order_release);
    }

    bool CanProtectGpuAccess(std::uintptr_t address, std::size_t bytes) {
        if (gpuFault.load(std::memory_order_acquire) == nullptr || !wholeGpuPages(address, bytes)) return false;
        MemoryLockProbe::Lock lock(mutex, "gpu-access-eligible", bytes);
        for (auto page = address; page < address + bytes; page += subpageBytes) {
            const auto view = views.find(page & ~(pageBytes - 1));
            if (view == views.end() || !writable(view->second.protection) || !canProtectGpuShared(*view->second.page)) return false;
        }
        return true;
    }

    bool ProtectGpuAccess(std::uintptr_t address, std::size_t bytes) {
        if (gpuFault.load(std::memory_order_acquire) == nullptr || !wholeGpuPages(address, bytes)) return false;
        MemoryLockProbe::Lock lock(mutex, "gpu-access-protect", bytes);
        std::vector<std::pair<std::uintptr_t, GpuPage>> changed;
        for (auto page = address; page < address + bytes; page += subpageBytes) {
            const auto found = gpuPages.find(page);
            if (found == gpuPages.end() || found->second.shared == nullptr || !canProtectGpuShared(*found->second.shared)) return false;
            changed.emplace_back(*found);
        }
        gpuAccessProtected.store(true, std::memory_order_release);
        for (const auto& [page, item] : changed) gpuPages.at(page).pending = true;
        try { applyGpuChanges(changed); }
        catch (...) {
            for (const auto& [page, item] : changed) gpuPages.at(page).pending = item.pending;
            applyGpuChanges(changed);
            throw;
        }
        return true;
    }

    bool HasGpuOverlay(std::uintptr_t address, std::size_t bytes) {
        if (gpuFault.load(std::memory_order_acquire) == nullptr || bytes == 0 || bytes > std::numeric_limits<std::uintptr_t>::max() - address) return false;
        MemoryLockProbe::Lock lock(mutex, "gpu-overlay-range", bytes);
        const auto first = address & ~(subpageBytes - 1);
        const auto found = gpuPages.lower_bound(first);
        if (found != gpuPages.end() && found->first < address + bytes) return true;
        for (auto view = views.lower_bound(address & ~(pageBytes - 1)); view != views.end() && view->first < address + bytes; ++view) {
            const auto mask = subpagesOf(view->first, address, address + bytes);
            for (std::size_t sub = 0; sub < subpages; ++sub) if ((mask & (1u << sub)) != 0 && view->second.page->gpuOwners[sub] != 0) return true;
        }
        return false;
    }

    bool ArmGpuWriteWatch(std::uintptr_t address, std::size_t bytes, bool access = false, std::uint64_t* calls = nullptr) {
        if (gpuFault.load(std::memory_order_acquire) == nullptr || !wholeGpuPages(address, bytes)) return false;
        MemoryLockProbe::Lock lock(mutex, "gpu-arm", bytes);
        std::vector<std::pair<std::uintptr_t, GpuPage>> added;
        std::map<SharedPage*, std::uint8_t> sharedPages;
        MEMORY_BASIC_INFORMATION memory{};
        std::uintptr_t memoryEnd = 0;
        for (auto page = address; page < address + bytes; page += subpageBytes) {
            if (gpuPages.contains(page)) return false;
            if (page >= memoryEnd) {
                memory = query(page);
                memoryEnd = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
            }
            if (memory.State != MEM_COMMIT) return false;
            const auto view = views.find(page & ~(pageBytes - 1));
            const DWORD protection = view == views.end() ? memory.Protect : view->second.protection;
            if (!writable(protection)) return false;
            GpuPage item{nullptr, protection, ++gpuNextSerial, true, access};
            if (view != views.end()) {
                item.shared = view->second.page;
                const auto sub = (page % pageBytes) / subpageBytes;
                if (item.shared->pins != 0 || item.shared->hostAliases != 0 || item.shared->gpuOwners[sub] != 0) return false;
                for (const auto alias : item.shared->aliases) if (views.at(alias).hostWrites != 0) return false;
                auto& mask = sharedPages[item.shared.get()];
                if ((mask & (1u << sub)) != 0) return false;
                mask |= static_cast<std::uint8_t>(1u << sub);
            } else if (access || memory.Type != MEM_PRIVATE || gpuHostWrites.contains(page) || gpuPins.contains(page)) return false;
            added.emplace_back(page, std::move(item));
        }
        try {
            for (const auto& [page, item] : added) {
                gpuPages.emplace(page, item);
                if (item.shared != nullptr) item.shared->gpuOwners[(page % pageBytes) / subpageBytes] = page;
            }
            if (access) gpuAccessProtected.store(true, std::memory_order_release);
            const auto before = gpuProtectionSerial.load(std::memory_order_relaxed);
            applyGpuChanges(added);
            if (calls != nullptr) *calls = gpuProtectionSerial.load(std::memory_order_relaxed) - before;
        } catch (...) {
            for (const auto& [page, item] : added) removeGpuPage(page);
            throw;
        }
        return true;
    }

    void DisarmGpuWriteWatch(std::uintptr_t address, std::size_t bytes) {
        if (!wholeGpuPages(address, bytes)) throw std::runtime_error("GPU write watch is not made of whole pages");
        MemoryLockProbe::Lock lock(mutex, "gpu-disarm", bytes);
        std::vector<std::pair<std::uintptr_t, GpuPage>> removed;
        for (auto it = gpuPages.lower_bound(address); it != gpuPages.end() && it->first < address + bytes;) {
            removed.emplace_back(*it);
            if (it->second.shared != nullptr) it->second.shared->gpuOwners[(it->first % pageBytes) / subpageBytes] = 0;
            it = gpuPages.erase(it);
        }
        applyGpuChanges(removed);
    }

    bool SnapshotGpuPage(std::uintptr_t address, void* bytes) {
        if (address % subpageBytes != 0 || bytes == nullptr) return false;
        MemoryLockProbe::Lock lock(mutex, "gpu-snapshot", subpageBytes);
        const auto found = gpuPages.find(address);
        if (found == gpuPages.end() || !found->second.pending || found->second.shared == nullptr) return false;
        const auto& view = views.at(address & ~(pageBytes - 1));
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto offset = view.offset + address % pageBytes;
        const auto lead = offset % system.dwAllocationGranularity;
        auto* alias = map(view.section->handle, GetCurrentProcess(), nullptr, offset - lead, lead + subpageBytes, 0, PAGE_READONLY, nullptr, 0);
        if (alias == nullptr) fail("map GPU snapshot alias");
        std::memcpy(bytes, static_cast<const unsigned char*>(alias) + lead, subpageBytes);
        unmap(GetCurrentProcess(), alias, 0);
        return true;
    }

    bool PublishGpuPage(std::uintptr_t address, const void* gpu, const void* firstCpuBase) {
        if (address % subpageBytes != 0 || gpu == nullptr) return false;
        MemoryLockProbe::Lock lock(mutex, "gpu-publish", subpageBytes);
        const auto found = gpuPages.find(address);
        if (found == gpuPages.end() || (!found->second.pending && !writable(found->second.protection)) || (!found->second.armed && firstCpuBase == nullptr)) return false;
        const auto shared = found->second.shared;
        const bool pending = found->second.pending;
        const auto releaseAlias = [this](void* alias) { if (alias != nullptr) unmap(GetCurrentProcess(), alias, 0); };
        std::unique_ptr<void, decltype(releaseAlias)> alias(nullptr, releaseAlias);
        auto* destination = reinterpret_cast<unsigned char*>(address);
        if (pending) {
            const auto& view = views.at(address & ~(pageBytes - 1));
            SYSTEM_INFO system{};
            GetSystemInfo(&system);
            const auto offset = view.offset + address % pageBytes;
            const auto lead = offset % system.dwAllocationGranularity;
            alias.reset(map(view.section->handle, GetCurrentProcess(), nullptr, offset - lead, lead + subpageBytes, 0, PAGE_READWRITE, nullptr, 0));
            if (alias == nullptr) fail("map GPU publication alias");
            destination = static_cast<unsigned char*>(alias.get()) + lead;
        }
        std::array<unsigned char, subpageBytes> untouched;
        const auto* base = static_cast<const unsigned char*>(firstCpuBase);
        if (base == nullptr) {
            std::memcpy(untouched.data(), destination, untouched.size());
            base = untouched.data();
        }
        const auto protection = found->second.protection;
        if (!pending && observationEnabled.load(std::memory_order_relaxed)) openObservation(address & ~(pageBytes - 1), static_cast<std::uint8_t>(1u << ((address % pageBytes) / subpageBytes)));
        if (!pending) removeGpuPage(address);
        if (shared != nullptr) {
            auto& view = views.at(address & ~(pageBytes - 1));
            if (!pending) view.armed &= static_cast<std::uint8_t>(~(1u << ((address % pageBytes) / subpageBytes)));
            invalidate(*shared, static_cast<std::uint8_t>(1u << ((address % pageBytes) / subpageBytes)));
        }
        if (!pending) {
            observe(PageEvent::Invalidate, address, subpageBytes);
            DWORD previous;
            if (!protectMemory(reinterpret_cast<void*>(address), subpageBytes, protection, &previous)) fail("open GPU publication page");
            gpuProtectionSerial.fetch_add(1, std::memory_order_release);
        }
        const auto* source = static_cast<const unsigned char*>(gpu);
        static_assert(std::atomic_ref<unsigned char>::is_always_lock_free);
        for (std::size_t at = 0; at < subpageBytes; ++at) {
            if (source[at] == base[at]) continue;
            auto expected = base[at];
            std::atomic_ref<unsigned char>(destination[at]).compare_exchange_strong(expected, source[at], std::memory_order_relaxed);
        }
        if (pending) {
            alias.reset();
            removeGpuPage(address);
        }
        return true;
    }

    bool GpuOverlayProtection(std::uintptr_t address, std::uint32_t* protection, bool* pending) {
        if (gpuFault.load(std::memory_order_acquire) == nullptr) return false;
        MemoryLockProbe::Lock lock(mutex, "gpu-protection", 1);
        const auto owner = gpuOwner(address);
        if (owner == 0) return false;
        const auto view = views.find(address & ~(pageBytes - 1));
        *protection = view == views.end() ? gpuPages.at(owner).protection : view->second.protection;
        if (pending != nullptr) *pending = gpuPages.at(owner).pending;
        return true;
    }

    bool ApplyGpuProtection(std::uintptr_t address, std::size_t bytes, DWORD protection) {
        if (gpuFault.load(std::memory_order_acquire) == nullptr && !observationEnabled.load(std::memory_order_acquire)) return false;
        MemoryLockProbe::Lock lock(mutex, "gpu-protect", bytes);
        applyGpuProtection(address, bytes, protection);
        if (observationEnabled.load(std::memory_order_relaxed)) {
            for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) observeAliasesState(it->first);
        }
        return true;
    }

    bool VisitGpuAliases(std::uintptr_t address, std::size_t bytes, bool (*visitor)(std::uintptr_t, std::size_t, void*), void* context) {
        if (bytes == 0 || bytes > std::numeric_limits<std::uintptr_t>::max() - address || visitor == nullptr) return false;
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> aliases;
        {
            MemoryLockProbe::Lock lock(mutex, "gpu-aliases", bytes);
            for (auto cursor = address; cursor < address + bytes;) {
                const auto base = cursor & ~(pageBytes - 1);
                const auto stop = std::min(address + bytes, base + pageBytes);
                const auto view = views.find(base);
                if (view != views.end()) {
                    for (const auto alias : view->second.page->aliases) if (alias != base) aliases.emplace_back(alias + cursor - base, alias + stop - base);
                } else {
                    const auto memory = query(cursor);
                    if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE) return false;
                }
                cursor = stop;
            }
        }
        std::sort(aliases.begin(), aliases.end());
        for (std::size_t index = 0; index < aliases.size();) {
            const auto begin = aliases[index].first;
            auto end = aliases[index++].second;
            while (index < aliases.size() && aliases[index].first <= end) end = std::max(end, aliases[index++].second);
            if (!visitor(begin, end - begin, context)) return false;
        }
        return true;
    }

    bool Collect(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear, Collection collection = Collection::All) {
        MemoryLockProbe::Lock lock(mutex, "collect", bytes);
        const auto capacity = *count;
        *count = 0;
        const auto end = address + bytes;
        std::optional<MEMORY_BASIC_INFORMATION> cachedQuery;
        if (collection == Collection::Fresh) {
            const auto fresh = freshRanges.upper_bound(address);
            if ((fresh == freshRanges.begin() || std::prev(fresh)->second <= address) && (fresh == freshRanges.end() || fresh->first >= end)) return true;
            for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < end; ++it) {
                if (it->second.protection != PAGE_NOACCESS) continue;
                for (auto at = std::max(address, it->first); at < std::min(end, it->first + pageBytes); at += 4096) {
                    if ((it->second.collected & (1u << ((at - it->first) / 4096))) == 0) return false;
                }
            }
        } else if (clear && !collectable(address, end, cachedQuery)) return false;
        for (auto cursor = address; cursor < end;) {
            const auto nextClean = cleanRanges.upper_bound(cursor);
            if (nextClean != cleanRanges.begin()) {
                const auto clean = std::prev(nextClean);
                if (cursor < clean->second) {
                    cursor = std::min(end, clean->second);
                    continue;
                }
            }
            const auto base = cursor & ~(pageBytes - 1);
            const auto found = views.find(base);
            if (found != views.end()) {
                auto& view = found->second;
                auto stop = std::min(std::min(end, base + pageBytes), nextClean == cleanRanges.end() ? end : nextClean->first);
                if (collection != Collection::All) {
                    const auto selected = [&](std::uintptr_t at) {
                        const bool collected = (view.collected & (1u << ((at - base) / 4096))) != 0;
                        return collection == Collection::Armed ? collected : !collected;
                    };
                    while (cursor < stop && !selected(cursor)) cursor = std::min(stop, cursor + 4096);
                    if (cursor == stop) continue;
                    for (auto at = cursor + 4096; at < stop; at += 4096) {
                        if (!selected(at)) {
                            stop = at;
                            break;
                        }
                    }
                }
                if (view.protection == PAGE_NOACCESS) return false;
                const bool pinned = view.page->pins != 0;
                std::uint8_t settled = 0;
                auto settledEnd = cursor;
                bool full = false;
                for (std::size_t sub = 0; sub < subpages; ++sub) {
                    const auto from = std::max(cursor, base + sub * subpageBytes);
                    const auto to = std::min(stop, base + (sub + 1) * subpageBytes);
                    if (from >= to) continue;
                    if (collection == Collection::Fresh || pinned || view.seen[sub] != view.page->generation[sub]) {
                        if (*count == capacity) {
                            full = true;
                            break;
                        }
                        pages[(*count)++] = reinterpret_cast<void*>(from);
                    }
                    settled |= static_cast<std::uint8_t>(1u << sub);
                    settledEnd = to;
                }
                if (clear) {
                    for (std::size_t sub = 0; sub < subpages; ++sub) {
                        const auto bit = static_cast<std::uint8_t>(1u << sub);
                        if ((settled & bit) == 0 || (view.collected & bit) != 0) continue;
                        view.collected |= bit;
                        forget(freshRanges, base + sub * subpageBytes, base + (sub + 1) * subpageBytes);
                    }
                }
                if (clear && !pinned && settled != 0) {
                    std::uint8_t untracked = 0;
                    for (const auto alias : view.page->aliases) {
                        auto& other = views.at(alias);
                        const auto arming = static_cast<std::uint8_t>(settled & ~other.armed);
                        if (!writable(other.protection) || arming == 0) continue;
                        if (other.hostWrites != 0) {
                            untracked |= arming;
                            continue;
                        }
                        if (gpuFault.load(std::memory_order_acquire) != nullptr) {
                            other.armed |= arming;
                            applyGpuView(alias, arming);
                        } else {
                            const DWORD protection = other.protection == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY;
                            protectSubpages(alias, arming, protection, "arm shared memory write tracking");
                            other.armed |= arming;
                        }
                    }
                    const auto tracked = static_cast<std::uint8_t>(settled & ~untracked);
                    auto cleanFrom = cursor;
                    auto cleanTo = cursor;
                    for (std::size_t sub = 0; sub < subpages; ++sub) {
                        if ((tracked & (1u << sub)) == 0) continue;
                        const auto from = std::max(cursor, base + sub * subpageBytes);
                        const auto to = std::min(settledEnd, base + (sub + 1) * subpageBytes);
                        if (from == base + sub * subpageBytes && to == base + (sub + 1) * subpageBytes) view.seen[sub] = view.page->generation[sub];
                        if (from != cleanTo) {
                            if (cleanFrom != cleanTo) rememberClean(cleanFrom, cleanTo);
                            cleanFrom = from;
                        }
                        cleanTo = to;
                    }
                    if (cleanFrom != cleanTo) rememberClean(cleanFrom, cleanTo);
                    if (HasPageObserver()) observeAliasesState(base);
                }
                if (full) return true;
                cursor = stop;
            } else {
                if (collection == Collection::Fresh) {
                    const auto next = views.lower_bound(cursor);
                    cursor = next == views.end() ? end : std::min(end, next->first);
                    continue;
                }
                const auto memory = query(cursor, cachedQuery);
                if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE) return false;
                const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                if (capacity == *count) return true;
                const auto touched = (((stop + 4095) & ~static_cast<std::uintptr_t>(4095)) - (cursor & ~static_cast<std::uintptr_t>(4095))) / 4096;
                ULONG_PTR available = std::min<ULONG_PTR>(capacity - *count, touched);
                DWORD granularity = 0;
                if (GetWriteWatch(clear ? WRITE_WATCH_FLAG_RESET : 0, reinterpret_cast<void*>(cursor), stop - cursor, pages + *count, &available, &granularity) != 0) fail("collect private guest writes");
                if (HasPageObserver()) observe(granularity == subpageBytes && available < capacity - *count ? PageEvent::PrivatePollRange : PageEvent::Invalidate, cursor, stop - cursor, readable(memory.Protect) ? PagePrivateWatch : PageUnsupported);
                *count += available;
                if (*count == capacity) return true;
                cursor = stop;
            }
        }
        return true;
    }

private:
    static std::size_t timingBucket(std::uint64_t value) { return std::min<std::size_t>(31, std::bit_width(value)); }

    static void noteTiming(PageTiming& timing, std::uint64_t ns) {
        ++timing.calls;
        timing.ns += ns;
        timing.maxNs = std::max(timing.maxNs, ns);
        ++timing.histogram[timingBucket(ns)];
    }

    struct FaultTimer {
        WindowsMappings& owner;
        PageFaultKind kind;
        bool active, nested = false, publishedCpu = false;
        std::uint64_t start = 0;
        inline static thread_local unsigned depth = 0;
        FaultTimer(WindowsMappings& owner, PageFaultKind kind) : owner(owner), kind(kind), active(owner.observationEnabled.load(std::memory_order_acquire)) {
            if (!active) return;
            nested = depth++ != 0;
            if (nested) this->kind = PageFaultKind::Nested;
            start = MemoryLockProbe::Now();
        }
        ~FaultTimer() {
            if (!active) return;
            const auto elapsed = MemoryLockProbe::Now() - start;
            --depth;
            std::lock_guard lock(owner.observationFaultMutex);
            noteTiming(owner.observationFaultCounts[static_cast<std::size_t>(kind)], elapsed);
            owner.observationFaultSerial.fetch_add(1, std::memory_order_relaxed);
        }
        void Locked(std::uint64_t since) {
            if (!active) return;
            const auto elapsed = MemoryLockProbe::Now() - since;
            std::lock_guard lock(owner.observationFaultMutex);
            noteTiming(owner.observationMappingWait, elapsed);
        }
    };

    bool refuseObservation(PageArmRefusal reason) {
        ++observationCounts.refusals[static_cast<std::size_t>(reason)];
        return false;
    }

    BOOL protectMemory(void* address, std::size_t bytes, DWORD protection, DWORD* previous) {
        if (!observationEnabled.load(std::memory_order_relaxed)) return VirtualProtect(address, bytes, protection, previous);
        const auto start = MemoryLockProbe::Now();
        const auto result = VirtualProtect(address, bytes, protection, previous);
        noteTiming(observationCounts.protection, MemoryLockProbe::Now() - start);
        observationCounts.protectionPages += bytes / subpageBytes;
        return result;
    }

    bool protectObservation(std::uintptr_t address, std::size_t bytes, DWORD protection) {
        const auto start = MemoryLockProbe::Now();
        DWORD previous;
        const bool result = VirtualProtect(reinterpret_cast<void*>(address), bytes, protection, &previous) != 0;
        noteTiming(observationCounts.protection, MemoryLockProbe::Now() - start);
        observationCounts.protectionPages += bytes / subpageBytes;
        return result;
    }

    bool applyObservation(const ObservationKey& key, std::uint8_t mask) {
        bool result = true;
        for (const auto alias : observationPhysical.at(key)) {
            const auto& view = views.at(alias);
            for (std::size_t sub = 0; sub < subpages;) {
                if ((mask & (1u << sub)) == 0) { ++sub; continue; }
                const auto from = sub++;
                const auto protection = effectiveGpuProtection(alias + from * subpageBytes, view.protection);
                while (sub < subpages && (mask & (1u << sub)) != 0 && effectiveGpuProtection(alias + sub * subpageBytes, view.protection) == protection) ++sub;
                result = protectObservation(alias + from * subpageBytes, (sub - from) * subpageBytes, protection) && result;
            }
        }
        return result;
    }

    bool armObservation(const ObservationKey& key, std::uint8_t mask) {
        auto& page = cpuObservations.at(key);
        if (observationReady == nullptr || !observationReady()) return refuseObservation(PageArmRefusal::Metadata);
        if (compare == nullptr) return refuseObservation(PageArmRefusal::Identity);
        bool anyWritable = false;
        if (observationOldAliases || observationHostAliases.contains(key)) return refuseObservation(PageArmRefusal::HostAlias);
        for (const auto alias : observationPhysical.at(key)) {
            const auto& view = views.at(alias);
            anyWritable |= writable(view.protection);
            if (view.page->pins != 0) return refuseObservation(PageArmRefusal::Pinned);
            if (view.hostWrites != 0) return refuseObservation(PageArmRefusal::HostWriter);
            if (view.page->hostAliases != 0) return refuseObservation(PageArmRefusal::HostAlias);
        }
        if (!anyWritable) return refuseObservation(PageArmRefusal::Permission);
        mask &= static_cast<std::uint8_t>(~page.armed);
        if (mask == 0) return true;
        page.armed |= mask;
        if (!applyObservation(key, mask)) {
            page.armed &= static_cast<std::uint8_t>(~mask);
            page.touched |= mask;
            applyObservation(key, mask);
            for (const auto alias : observationPhysical.at(key)) observe(PageEvent::SectionState, alias, pageBytes, PageUnsupported);
            return refuseObservation(PageArmRefusal::Protection);
        }
        observationCounts.armedPages += std::popcount(mask);
        observationCounts.rearmedPages += std::popcount(static_cast<std::uint8_t>(mask & page.everArmed));
        page.everArmed |= mask;
        page.touched &= static_cast<std::uint8_t>(~mask);
        observeState(key);
        return true;
    }

    void openObservation(std::uintptr_t base, std::uint8_t mask) {
        const auto found = observationViews.find(base);
        if (found == observationViews.end()) return;
        auto& page = cpuObservations.at(found->second);
        const auto opened = static_cast<std::uint8_t>(page.armed & mask);
        page.armed &= static_cast<std::uint8_t>(~mask);
        page.touched |= mask;
        if (opened != 0 && !applyObservation(found->second, opened)) fail("open CPU observation page");
        observeState(found->second);
    }

    void noteObservationWrite(std::uintptr_t base, std::uint8_t mask) {
        auto* page = views.at(base).cpuObservation;
        if (page == nullptr) return;
        observationCounts.physicalPages += std::popcount(static_cast<std::uint8_t>(mask & ~page->written));
        observationCounts.aliasPages += observationPhysical.at(observationViews.at(base)).size() * std::popcount(mask);
        page->written |= mask;
        page->touched |= mask;
    }

    std::optional<bool> handleObservationFault(std::uintptr_t address, FaultTimer& timer) {
        const auto start = MemoryLockProbe::Now();
        MemoryLockProbe::Lock lock(mutex, "page-observation-fault", 1);
        timer.Locked(start);
        const auto base = address & ~(pageBytes - 1);
        const auto found = views.find(base);
        if (found == views.end()) {
            const auto owner = gpuOwner(address);
            timer.kind = owner != 0 ? PageFaultKind::GpuWrite : PageFaultKind::Rejected;
            return std::nullopt;
        }
        auto& view = found->second;
        if (!writable(view.protection)) { timer.kind = PageFaultKind::Rejected; return false; }
        const auto mask = subpagesOf(base, address, address + 1);
        const auto owner = gpuOwner(address);
        const bool gpu = owner != 0 && (gpuPages.at(owner).pending || gpuPages.at(owner).armed);
        const bool legacy = (view.armed & mask) != 0;
        auto* page = view.cpuObservation;
        const bool observed = page != nullptr && (page->armed & mask) != 0;
        timer.kind = gpu ? PageFaultKind::GpuWrite : legacy ? PageFaultKind::Legacy : observed ? PageFaultKind::Observation : PageFaultKind::Stale;
        if (observed) {
            observeWrite(base, mask, PageEvent::ObservationFault);
            timer.publishedCpu = true;
            if (const auto hook = observationFaultHook.load(std::memory_order_acquire)) hook();
            page->armed &= static_cast<std::uint8_t>(~mask);
            page->touched |= mask;
        }
        if (observed || legacy) noteObservationWrite(base, mask);
        if (gpu) return std::nullopt;
        if (legacy) {
            if (!observed) observeWrite(base, mask, PageEvent::WriteFault);
            invalidate(*view.page, mask);
            view.armed &= static_cast<std::uint8_t>(~mask);
        }
        if (observed || legacy) {
            if (!applyObservation(observationViews.at(base), mask)) { timer.kind = PageFaultKind::Rejected; return false; }
            observeAliasesState(base);
            return true;
        }
        const auto memory = query(address);
        if (memory.State == MEM_COMMIT && writable(memory.Protect)) return true;
        timer.kind = PageFaultKind::Rejected;
        return false;
    }

    void observe(PageEvent event, std::uintptr_t address, std::size_t bytes, std::uint32_t reasons = 0) const {
        if (const auto callback = pageObserver.load(std::memory_order_acquire)) callback(event, address, bytes, reasons);
    }

    void observeMap(std::uintptr_t address) {
        const auto& view = views.at(address);
        auto section = view.section;
        for (const auto& previous : observationSections) {
            if (previous == section || sameSection(previous->handle, section->handle)) { section = previous; break; }
        }
        if (std::find(observationSections.begin(), observationSections.end(), section) == observationSections.end()) observationSections.push_back(section);
        const ObservationKey key{reinterpret_cast<std::uintptr_t>(section->handle), view.offset};
        observationViews.emplace(address, key);
        observationPhysical[key].push_back(address);
        if (ObservationRequested()) views.at(address).cpuObservation = &cpuObservations[key];
        observe(PageEvent::SectionMap, address, pageBytes, PageUnsupported);
        if (observationEnabled.load(std::memory_order_relaxed)) {
            if (observationReady != nullptr && observationReady()) {
                if (!applyObservation(key, views.at(address).cpuObservation->armed)) fail("protect a new CPU observation alias");
                armObservation(key, allSubpages);
            } else {
                openObservation(address, allSubpages);
                refuseObservation(PageArmRefusal::Metadata);
            }
        }
        observeState(key);
    }

    void observeAliasesState(std::uintptr_t address) const {
        const auto found = observationViews.find(address);
        if (found != observationViews.end()) observeState(found->second);
    }

    void observeState(const ObservationKey& key) const {
        const auto& aliases = observationPhysical.at(key);
        for (std::size_t sub = 0; sub < subpages; ++sub) {
            bool anyWritable = false, open = false, hostAlias = observationOldAliases, pending = false;
            if (const auto host = observationHostAliases.find(key); host != observationHostAliases.end()) hostAlias |= host->second != 0;
            for (const auto alias : aliases) {
                const auto& view = views.at(alias);
                hostAlias |= view.page->hostAliases != 0;
                const auto owner = gpuOwner(alias + sub * subpageBytes);
                pending |= owner != 0 && gpuPages.at(owner).pending;
                anyWritable |= writable(view.protection);
                const bool watched = (view.armed & (1u << sub)) != 0 || (view.cpuObservation != nullptr && (view.cpuObservation->armed & (1u << sub)) != 0);
                open |= view.page->pins != 0 || view.hostWrites != 0 || (writable(view.protection) && !watched);
            }
            for (const auto alias : aliases) {
                const auto& view = views.at(alias);
                std::uint32_t reasons = !readable(view.protection) || compare == nullptr ? PageUnsupported : open ? PageOpenWriter : anyWritable ? PageProtectedSection : PageImmutable;
                if (hostAlias) reasons |= PageHostAlias;
                if (pending) reasons |= PagePendingGpu;
                observe(PageEvent::SectionState, alias + sub * subpageBytes, subpageBytes, reasons);
            }
        }
    }

    void observeWrite(std::uintptr_t address, std::uint8_t mask, PageEvent event) const {
        const auto found = observationViews.find(address);
        if (found == observationViews.end()) return;
        for (const auto alias : observationPhysical.at(found->second)) {
            for (std::size_t sub = 0; sub < subpages; ++sub) if ((mask & (1u << sub)) != 0) observe(event, alias + sub * subpageBytes, subpageBytes);
        }
    }

    void forgetObservationAlias(void* alias) {
        const auto found = observationAliases.find(reinterpret_cast<std::uintptr_t>(alias));
        if (found == observationAliases.end()) return;
        for (const auto& key : found->second) {
            --observationHostAliases.at(key);
            observeState(key);
            pruneObservationKey(key);
        }
        observationAliases.erase(found);
    }

    void pruneObservationKey(const ObservationKey& key) {
        const auto host = observationHostAliases.find(key);
        if (host != observationHostAliases.end() && host->second == 0) observationHostAliases.erase(host);
        if (!observationPhysical.at(key).empty() || observationHostAliases.contains(key)) return;
        observationPhysical.erase(key);
        cpuObservations.erase(key);
        pageReads.erase(key);
        const auto next = observationPhysical.lower_bound({key.first, 0});
        if (next == observationPhysical.end() || next->first.first != key.first) std::erase_if(observationSections, [&](const auto& section) { return reinterpret_cast<std::uintptr_t>(section->handle) == key.first; });
    }

    bool collectable(std::uintptr_t address, std::uintptr_t end, std::optional<MEMORY_BASIC_INFORMATION>& cachedQuery) {
        for (auto cursor = address; cursor < end;) {
            const auto clean = cleanRanges.upper_bound(cursor);
            if (clean != cleanRanges.begin()) {
                const auto cleanEnd = std::prev(clean)->second;
                if (cursor < cleanEnd) {
                    cursor = std::min(end, cleanEnd);
                    continue;
                }
            }
            const auto base = cursor & ~(pageBytes - 1);
            if (const auto found = views.find(base); found != views.end()) {
                if (found->second.protection == PAGE_NOACCESS) return false;
                cursor = std::min(end, base + pageBytes);
                continue;
            }
            const auto memory = query(cursor, cachedQuery);
            if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE) return false;
            cursor = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
        }
        return true;
    }

    static constexpr std::size_t pageBytes = 0x4000;
    static constexpr std::size_t subpageBytes = 0x1000;
    static_assert(subpageBytes == 4096, "Collect reports one page per subpage");
    static constexpr std::size_t subpages = pageBytes / subpageBytes;
    static constexpr std::uint8_t allSubpages = (1u << subpages) - 1;
    struct PageRead {
        std::shared_ptr<void> owner;
        void* alias = nullptr;
        decltype(&UnmapViewOfFile2) unmap = nullptr;
        unsigned char* destination = nullptr;
        const unsigned char* original = nullptr;
        bool (*download)(void*) noexcept = nullptr;
        void* context = nullptr;
        ~PageRead() { if (alias != nullptr && unmap != nullptr) unmap(GetCurrentProcess(), alias, 0); }
    };
    using PageReads = std::array<std::shared_ptr<PageRead>, subpages>;
    struct CpuObservation {
        std::uint8_t armed = 0, touched = allSubpages, written = 0, everArmed = 0;
    };
    struct Section;
    struct SharedPage {
        std::array<std::uint64_t, subpages> generation = [] {
            std::array<std::uint64_t, subpages> initial{};
            initial.fill(1);
            return initial;
        }();
        std::vector<std::uintptr_t> aliases;
        std::uint32_t pins = 0;
        std::array<std::uintptr_t, subpages> gpuOwners{};
        std::uint32_t hostAliases = 0;
        std::shared_ptr<Section> section;
    };
    struct Section {
        HANDLE handle;
        explicit Section(HANDLE handle) : handle(handle) {}
        Section(const Section&) = delete;
        Section& operator=(const Section&) = delete;
        ~Section() { CloseHandle(handle); }
    };
    struct View {
        std::shared_ptr<SharedPage> page;
        DWORD protection;
        std::array<std::uint64_t, subpages> seen;
        std::uint8_t armed;
        std::shared_ptr<Section> section;
        std::uint64_t offset;
        std::uint32_t hostWrites;
        std::uint8_t collected = 0;
        CpuObservation* cpuObservation = nullptr;
    };
    struct GpuPage {
        std::shared_ptr<SharedPage> shared;
        DWORD protection;
        std::uint64_t serial;
        bool armed;
        bool pending;
    };
    using Ranges = std::map<std::uintptr_t, std::uintptr_t>;

    static bool wholeGpuPages(std::uintptr_t address, std::size_t bytes) {
        return address != 0 && bytes != 0 && address % subpageBytes == 0 && bytes % subpageBytes == 0 && bytes <= std::numeric_limits<std::uintptr_t>::max() - address;
    }

    std::uintptr_t gpuOwner(std::uintptr_t address) const {
        const auto page = address & ~(subpageBytes - 1);
        const auto view = views.find(page & ~(pageBytes - 1));
        if (view != views.end()) return view->second.page->gpuOwners[(page % pageBytes) / subpageBytes];
        return gpuPages.contains(page) ? page : 0;
    }

    DWORD effectiveGpuProtection(std::uintptr_t page, DWORD protection) const {
        const auto owner = gpuOwner(page);
        if (owner != 0 && gpuPages.at(owner).pending) return PAGE_NOACCESS;
        const auto view = views.find(page & ~(pageBytes - 1));
        if (PageReadFaultsRequested() && !pageReads.empty() && view != views.end()) {
            const auto reads = pageReads.find(observationViews.at(page & ~(pageBytes - 1)));
            if (reads != pageReads.end() && reads->second[(page % pageBytes) / subpageBytes] != nullptr) return PAGE_NOACCESS;
        }
        const auto bit = static_cast<std::uint8_t>(1u << ((page % pageBytes) / subpageBytes));
        const bool watched = (owner != 0 && gpuPages.at(owner).armed) || (view != views.end() && ((view->second.armed & bit) != 0 || (view->second.cpuObservation != nullptr && (view->second.cpuObservation->armed & bit) != 0)));
        if (watched && writable(protection)) return protection == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY;
        return protection;
    }

    void protectGpuPage(std::uintptr_t page, DWORD protection) {
        observe(PageEvent::Invalidate, page, subpageBytes);
        DWORD previous;
        if (!protectMemory(reinterpret_cast<void*>(page), subpageBytes, protection, &previous)) fail("protect GPU watched page");
        gpuProtectionSerial.fetch_add(1, std::memory_order_release);
    }

    void applyGpuPage(std::uintptr_t page) {
        const auto view = views.find(page & ~(pageBytes - 1));
        if (view == views.end()) {
            const auto found = gpuPages.find(page);
            if (found != gpuPages.end()) protectGpuPage(page, effectiveGpuProtection(page, found->second.protection));
            return;
        }
        applyGpuShared(*view->second.page, static_cast<std::uint8_t>(1u << ((page % pageBytes) / subpageBytes)));
    }

    void applyGpuShared(SharedPage& page, std::uint8_t mask) {
        for (const auto alias : page.aliases) applyGpuView(alias, mask);
    }

    void applyGpuView(std::uintptr_t alias, std::uint8_t mask) {
        const auto logical = views.at(alias).protection;
        for (std::size_t sub = 0; sub < subpages;) {
            if ((mask & (1u << sub)) == 0) { ++sub; continue; }
            const auto protection = effectiveGpuProtection(alias + sub * subpageBytes, logical);
            std::uint8_t group = static_cast<std::uint8_t>(1u << sub++);
            while (sub < subpages && (mask & (1u << sub)) != 0 && effectiveGpuProtection(alias + sub * subpageBytes, logical) == protection) group |= static_cast<std::uint8_t>(1u << sub++);
            protectSubpages(alias, group, protection, "protect shared GPU watched pages");
            gpuProtectionSerial.fetch_add(1, std::memory_order_release);
        }
    }

    bool canProtectGpuShared(const SharedPage& page) const {
        if (page.pins != 0 || page.hostAliases != 0) return false;
        for (const auto alias : page.aliases) if (views.at(alias).hostWrites != 0) return false;
        return true;
    }

    void applyGpuChanges(const std::vector<std::pair<std::uintptr_t, GpuPage>>& changes) {
        std::map<SharedPage*, std::uint8_t> shared;
        for (std::size_t index = 0; index < changes.size();) {
            const auto& [page, item] = changes[index];
            if (item.shared != nullptr) {
                shared[item.shared.get()] |= static_cast<std::uint8_t>(1u << ((page % pageBytes) / subpageBytes));
                ++index;
                continue;
            }
            const auto protection = effectiveGpuProtection(page, item.protection);
            auto end = page + subpageBytes;
            ++index;
            while (index < changes.size() && changes[index].first == end && changes[index].second.shared == nullptr && effectiveGpuProtection(end, changes[index].second.protection) == protection) {
                end += subpageBytes;
                ++index;
            }
            DWORD previous;
            observe(PageEvent::Invalidate, page, end - page);
            if (!protectMemory(reinterpret_cast<void*>(page), end - page, protection, &previous)) fail("protect private GPU watched pages");
            gpuProtectionSerial.fetch_add(1, std::memory_order_release);
        }
        for (const auto& [page, mask] : shared) applyGpuShared(*page, mask);
    }

    void removeGpuPage(std::uintptr_t page) {
        const auto found = gpuPages.find(page);
        if (found == gpuPages.end()) return;
        const auto watched = found->second;
        if (watched.shared != nullptr) watched.shared->gpuOwners[(page % pageBytes) / subpageBytes] = 0;
        gpuPages.erase(found);
        if (watched.shared == nullptr) protectGpuPage(page, watched.protection);
        else applyGpuPage(page);
    }

    void applyGpuProtection(std::uintptr_t address, std::size_t bytes, DWORD protection) {
        if (bytes > std::numeric_limits<std::uintptr_t>::max() - address) throw std::runtime_error("GPU protection range overflow");
        for (auto page = address & ~(subpageBytes - 1); page < address + bytes; page += subpageBytes) {
            const auto memory = query(page);
            if (memory.State != MEM_COMMIT) continue;
            const auto view = views.find(page & ~(pageBytes - 1));
            const auto logical = view == views.end() ? protection : view->second.protection;
            if (auto found = gpuPages.find(page); found != gpuPages.end()) found->second.protection = logical;
            protectGpuPage(page, effectiveGpuProtection(page, logical));
        }
    }

    bool retryGpuFault(std::uintptr_t address, bool write) const {
        const auto serial = gpuProtectionSerial.load(std::memory_order_acquire);
        if (serial == 0) return false;
        thread_local std::uintptr_t lastAddress = 0;
        thread_local std::uint64_t lastSerial = 0;
        if (lastAddress == address && lastSerial == serial) return false;
        const auto memory = query(address);
        const auto protection = memory.Protect & 0xffu;
        if (memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0 || (write ? !writable(protection) : protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE)) return false;
        lastAddress = address;
        lastSerial = serial;
        return true;
    }

    bool hasPageReads(std::uintptr_t address, std::size_t bytes) const {
        if (!PageReadFaultsRequested() || pageReads.empty()) return false;
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) {
            const auto reads = pageReads.find(observationViews.at(it->first));
            if (reads != pageReads.end() && std::any_of(reads->second.begin(), reads->second.end(), [](const auto& read) { return read != nullptr; })) return true;
        }
        return false;
    }

    std::optional<bool> handlePageRead(std::uintptr_t address, bool write) {
        if (!PageReadFaultsRequested()) return std::nullopt;
        std::shared_ptr<PageRead> watch;
        {
            MemoryLockProbe::Lock lock(mutex, "page-read-fault", 1);
            const auto found = views.find(address & ~(pageBytes - 1));
            if (found == views.end() || found->second.cpuObservation == nullptr) return std::nullopt;
            const auto logical = found->second.protection;
            if (write ? !writable(logical) : !readable(logical)) return false;
            const auto reads = pageReads.find(observationViews.at(address & ~(pageBytes - 1)));
            if (reads != pageReads.end()) watch = reads->second[(address % pageBytes) / subpageBytes];
        }
        if (watch == nullptr) return retryGpuFault(address, write) ? std::optional<bool>{true} : std::nullopt;
        return watch->download(watch->context);
    }

    std::optional<bool> handleGpuFault(std::uintptr_t address, bool write, bool publishedCpu = false) {
        const auto callback = gpuFault.load(std::memory_order_acquire);
        if (callback == nullptr) return std::nullopt;
        std::uintptr_t owner = 0;
        std::uint64_t serial = 0;
        {
            MemoryLockProbe::Lock lock(mutex, "gpu-fault", 1);
            owner = gpuOwner(address);
            if (owner != 0) {
                const auto& page = gpuPages.at(owner);
                if (!page.pending && (!write || !page.armed)) return retryGpuFault(address, write) ? std::optional<bool>{true} : std::nullopt;
                const auto view = views.find(address & ~(pageBytes - 1));
                const auto logical = view == views.end() ? page.protection : view->second.protection;
                if (write ? !writable(logical) : !readable(logical)) return false;
                if (write && HasPageObserver() && !publishedCpu) {
                    const auto base = address & ~(pageBytes - 1);
                    if (view != views.end()) observeWrite(base, subpagesOf(base, address, address + 1), PageEvent::WriteFault);
                    else observe(PageEvent::WriteFault, address & ~(subpageBytes - 1), subpageBytes);
                }
                serial = page.serial;
            }
        }
        if (owner == 0) return retryGpuFault(address, write) ? std::optional<bool>{true} : std::nullopt;
        if (!callback(owner, write)) {
            MemoryLockProbe::Lock lock(mutex, "gpu-fault-stale", 1);
            const auto found = gpuPages.find(owner);
            if (found != gpuPages.end() && found->second.serial != serial) return true;
            if (found == gpuPages.end() && retryGpuFault(address, write)) return true;
            return false;
        }
        MemoryLockProbe::Lock lock(mutex, "gpu-fault-resume", 1);
        if (auto found = gpuPages.find(owner); found != gpuPages.end() && found->second.serial == serial) {
            if (found->second.pending) return false;
            found->second.armed = false;
            if (found->second.shared != nullptr) {
                const auto bit = static_cast<std::uint8_t>(1u << ((owner % pageBytes) / subpageBytes));
                invalidate(*found->second.shared, bit);
                const auto view = views.find(address & ~(pageBytes - 1));
                if (view != views.end()) view->second.armed &= static_cast<std::uint8_t>(~bit);
            }
            applyGpuPage(owner);
            if (HasPageObserver()) observeAliasesState(address & ~(pageBytes - 1));
        }
        return true;
    }

    bool gpuTracking() const {
        static const bool enabled = [] {
            const auto* text = std::getenv("APS5_VRAM_WRITABLE");
            return text != nullptr && std::string_view(text) == "1";
        }();
        return enabled || gpuFault.load(std::memory_order_acquire) != nullptr;
    }

    std::shared_ptr<Section> canonicalGpuSection(std::shared_ptr<Section> section) {
        if (compare == nullptr) throw std::runtime_error("GPU shared-page tracking requires CompareObjectHandles");
        for (auto it = gpuSections.begin(); it != gpuSections.end();) {
            const auto previous = it->lock();
            if (previous == nullptr) it = gpuSections.erase(it);
            else {
                if (sameSection(previous->handle, section->handle)) return previous;
                ++it;
            }
        }
        gpuSections.push_back(section);
        return section;
    }

    void rememberGpuAlias(void* alias, std::uintptr_t address, std::size_t bytes) {
        if (HasPageObserver()) {
            auto& keys = observationAliases[reinterpret_cast<std::uintptr_t>(alias)];
            for (auto cursor = address; cursor < address + bytes; cursor += pageBytes) {
                const auto key = observationViews.at(cursor);
                keys.push_back(key);
                ++observationHostAliases[key];
                if (observationEnabled.load(std::memory_order_relaxed)) openObservation(cursor, allSubpages);
                observeState(key);
            }
        }
        if (!gpuTracking()) {
            untrackedAliases.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::vector<std::shared_ptr<SharedPage>> pages;
        pages.reserve(bytes / pageBytes);
        for (std::size_t done = 0; done < bytes; done += pageBytes) {
            pages.push_back(views.at(address + done).page);
        }
        const auto [found, inserted] = gpuAliases.emplace(reinterpret_cast<std::uintptr_t>(alias), std::move(pages));
        if (!inserted) throw std::runtime_error("duplicate GPU host alias");
        for (const auto& page : found->second) ++page->hostAliases;
    }

    void forgetGpuAlias(void* alias) {
        if (HasPageObserver()) forgetObservationAlias(alias);
        const auto found = gpuAliases.find(reinterpret_cast<std::uintptr_t>(alias));
        if (found == gpuAliases.end()) {
            untrackedAliases.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        for (const auto& page : found->second) {
            --page->hostAliases;
            if (HasPageObserver()) for (const auto address : page->aliases) observeAliasesState(address);
        }
        gpuAliases.erase(found);
    }

    static void forget(Ranges& ranges, std::uintptr_t start, std::uintptr_t end) {
        auto it = ranges.lower_bound(start);
        if (it != ranges.begin() && std::prev(it)->second > start) --it;
        while (it != ranges.end() && it->first < end) {
            const auto first = it->first;
            const auto last = it->second;
            it = ranges.erase(it);
            if (first < start) ranges.emplace(first, start);
            if (last > end) it = ranges.emplace(end, last).first;
        }
    }

    static void remember(Ranges& ranges, std::uintptr_t start, std::uintptr_t end) {
        auto it = ranges.lower_bound(start);
        if (it != ranges.begin() && std::prev(it)->second >= start) --it;
        while (it != ranges.end() && it->first <= end) {
            start = std::min(start, it->first);
            end = std::max(end, it->second);
            it = ranges.erase(it);
        }
        ranges.emplace(start, end);
    }

    void forgetClean(std::uintptr_t start, std::uintptr_t end) {
        forget(cleanRanges, start, end);
    }

    void rememberClean(std::uintptr_t start, std::uintptr_t end) {
        remember(cleanRanges, start, end);
    }

    void invalidate(SharedPage& page, std::uint8_t mask = allSubpages) {
        for (std::size_t sub = 0; sub < subpages; ++sub) {
            if ((mask & (1u << sub)) == 0) continue;
            ++page.generation[sub];
            for (const auto alias : page.aliases) forgetClean(alias + sub * subpageBytes, alias + (sub + 1) * subpageBytes);
        }
    }

    static std::uint8_t subpagesOf(std::uintptr_t base, std::uintptr_t address, std::uintptr_t end) {
        const auto from = std::max(address, base);
        const auto to = std::min(end, base + pageBytes);
        if (to <= from) return 0;
        const auto first = (from - base) / subpageBytes;
        const auto last = (to - 1 - base) / subpageBytes;
        return static_cast<std::uint8_t>(((1u << (last + 1)) - 1) & ~((1u << first) - 1));
    }

    void protectSubpages(std::uintptr_t base, std::uint8_t mask, DWORD protection, const char* operation) {
        for (std::size_t sub = 0; sub < subpages;) {
            if ((mask & (1u << sub)) == 0) {
                ++sub;
                continue;
            }
            auto stop = sub;
            while (stop < subpages && (mask & (1u << stop)) != 0) ++stop;
            observe(PageEvent::Invalidate, base + sub * subpageBytes, (stop - sub) * subpageBytes);
            DWORD previous;
            if (!protectMemory(reinterpret_cast<void*>(base + sub * subpageBytes), (stop - sub) * subpageBytes, protection, &previous)) fail(operation);
            sub = stop;
        }
    }

    bool sameSection(HANDLE first, HANDLE second) const {
        return compare != nullptr && compare(first, second);
    }

    static bool writable(DWORD protection) {
        return protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE;
    }
    static bool readable(DWORD protection) {
        return protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE;
    }
    using AllocateFunction = PVOID (WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MapFunction = PVOID (WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using UnmapFunction = BOOL (WINAPI*)(HANDLE, PVOID, ULONG);
    using CompareFunction = BOOL (WINAPI*)(HANDLE, HANDLE);

    WindowsMappings() {
        const auto module = GetModuleHandleW(L"KernelBase.dll");
        if (!module) fail("load Windows memory API");
        allocate = reinterpret_cast<AllocateFunction>(GetProcAddress(module, "VirtualAlloc2"));
        map = reinterpret_cast<MapFunction>(GetProcAddress(module, "MapViewOfFile3"));
        unmap = reinterpret_cast<UnmapFunction>(GetProcAddress(module, "UnmapViewOfFile2"));
        if (!allocate || !map || !unmap) throw std::runtime_error("Windows placeholder memory APIs are required");
        compare = reinterpret_cast<CompareFunction>(GetProcAddress(module, "CompareObjectHandles"));
    }

    static bool LeanMap() {
        static const bool enabled = [] {
            const char* text = std::getenv("APS5_LEAN_GUEST_MAP");
            return text != nullptr && std::string_view(text) == "1";
        }();
        return enabled;
    }

    [[noreturn]] static void fail(const char* operation) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
    }

    static MEMORY_BASIC_INFORMATION query(std::uintptr_t address) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory)) fail("query guest memory");
        return memory;
    }

    static MEMORY_BASIC_INFORMATION query(std::uintptr_t address, std::optional<MEMORY_BASIC_INFORMATION>& cached) {
        static const bool enabled = std::getenv("APS5_CACHE_MAPPING_QUERIES") != nullptr;
        if (enabled && cached.has_value() && address >= reinterpret_cast<std::uintptr_t>(cached->BaseAddress) && address - reinterpret_cast<std::uintptr_t>(cached->BaseAddress) < cached->RegionSize) return *cached;
        const auto memory = query(address);
        if (enabled) cached = memory;
        return memory;
    }

    static void split(std::uintptr_t address, std::size_t bytes) {
        auto memory = query(address);
        memory = query(reinterpret_cast<std::uintptr_t>(memory.AllocationBase));
        if (memory.State != MEM_RESERVE) throw std::runtime_error("guest mapping requires a placeholder");
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (address != base) {
            if (!VirtualFree(reinterpret_cast<void*>(base), address - base, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split guest placeholder prefix");
            memory = query(address);
        }
        if (memory.RegionSize < bytes) throw std::runtime_error("guest placeholder is too small");
        if (memory.RegionSize != bytes && !VirtualFree(reinterpret_cast<void*>(address), bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split guest placeholder suffix");
    }

    void reset(std::uintptr_t address, std::size_t bytes) {
        if (hasPageReads(address, bytes)) throw std::runtime_error("cannot release memory with an active page read watch");
        observe(PageEvent::Unmap, address, bytes);
        const auto end = address + bytes;
        const auto watched = gpuPages.lower_bound(address);
        if (watched != gpuPages.end() && watched->first < end) throw std::runtime_error("cannot release memory with an active GPU write watch");
        if (!runs.empty()) splitRuns(address, end);
        forgetClean(address, end);
        for (auto cursor = address; cursor < end;) {
            const auto memory = query(cursor);
            if (memory.State == MEM_RESERVE) {
                cursor = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                continue;
            }
            if (reinterpret_cast<std::uintptr_t>(memory.AllocationBase) != cursor) throw std::runtime_error("cannot release part of a host allocation");
            auto allocationEnd = cursor;
            do {
                const auto part = query(allocationEnd);
                if (part.AllocationBase != memory.AllocationBase) break;
                allocationEnd = reinterpret_cast<std::uintptr_t>(part.BaseAddress) + part.RegionSize;
            } while (allocationEnd < end);
            if (allocationEnd > end || query(allocationEnd).AllocationBase == memory.AllocationBase) throw std::runtime_error("guest release truncates a host allocation");
            if (memory.Type == MEM_MAPPED) {
                if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(cursor), MEM_PRESERVE_PLACEHOLDER)) fail("unmap shared guest page");
                for (auto page = cursor; page < allocationEnd; page += pageBytes) forgetView(page);
                runs.erase(cursor);
            } else if (memory.Type == MEM_PRIVATE) {
                if (!VirtualFree(reinterpret_cast<void*>(cursor), allocationEnd - cursor, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("release private guest memory");
            } else {
                throw std::runtime_error("unsupported guest mapping type");
            }
            cursor = allocationEnd;
        }
        const auto last = query(reinterpret_cast<std::uintptr_t>(query(end - 1).AllocationBase));
        const auto lastBase = reinterpret_cast<std::uintptr_t>(last.BaseAddress);
        if (lastBase + last.RegionSize > end) split(lastBase, end - lastBase);
        const auto first = query(address);
        split(address, std::min(bytes, reinterpret_cast<std::uintptr_t>(first.BaseAddress) + first.RegionSize - address));
        if (query(address).RegionSize != bytes && !VirtualFree(reinterpret_cast<void*>(address), bytes, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) fail("coalesce guest placeholders");
    }

    static bool RunViews() {
        static const bool enabled = [] {
            const char* text = std::getenv("APS5_RUN_GUEST_VIEWS");
            return text != nullptr && std::string_view(text) == "1";
        }();
        return enabled;
    }

    struct Run {
        std::size_t bytes;
        std::shared_ptr<Section> section;
        std::uint64_t offset;
    };

    struct Transition {
        WindowsMappings& owner;
        Transition(WindowsMappings& owner, std::uintptr_t from, std::uintptr_t to) : owner(owner) {
            const auto slot = owner.transitionNext.fetch_add(1) % transitionSlots;
            owner.transitionTo[slot].store(0);
            owner.transitionFrom[slot].store(from);
            owner.transitionTo[slot].store(to);
            owner.transitionOwner.store(GetCurrentThreadId());
        }
        ~Transition() { owner.transitionOwner.store(0); }
        Transition(const Transition&) = delete;
        Transition& operator=(const Transition&) = delete;
    };

    void forgetView(std::uintptr_t page) {
        const auto found = views.find(page);
        if (found == views.end()) return;
        if (HasPageObserver()) {
            const auto observed = observationViews.find(page);
            if (observed != observationViews.end()) {
                const auto key = observed->second;
                std::erase(observationPhysical.at(key), page);
                if (ObservationRequested() && observationPhysical.at(key).empty()) {
                    auto& watched = cpuObservations.at(key);
                    observationRetiredPages += std::popcount(watched.written);
                    watched = {};
                }
                observationViews.erase(observed);
                observeState(key);
                pruneObservationKey(key);
            }
        }
        std::erase(found->second.page->aliases, page);
        views.erase(found);
        forget(freshRanges, page, page + pageBytes);
    }

    void splitRuns(std::uintptr_t address, std::uintptr_t end) {
        std::vector<std::pair<std::uintptr_t, Run>> partial;
        auto it = runs.upper_bound(address);
        if (it != runs.begin()) --it;
        for (; it != runs.end() && it->first < end; ++it) {
            const auto runEnd = it->first + it->second.bytes;
            if (runEnd <= address || (it->first >= address && runEnd <= end)) continue;
            partial.emplace_back(*it);
        }
        for (const auto& [base, run] : partial) splitRun(base, run, address, end);
    }

    void splitRun(std::uintptr_t base, const Run& run, std::uintptr_t address, std::uintptr_t end) {
        if (hasPageReads(base, run.bytes)) throw std::runtime_error("cannot split a shared run with an active page read watch");
        const auto runEnd = base + run.bytes;
        const auto cutFrom = std::max(base, address);
        const auto cutTo = std::min(runEnd, end);
        for (auto page = base; !gpuPages.empty() && page < runEnd; page += pageBytes) {
            if (page >= cutFrom && page < cutTo) continue;
            const auto& owners = views.at(page).page->gpuOwners;
            if (std::any_of(owners.begin(), owners.end(), [](auto owner) { return owner != 0; })) throw std::runtime_error("cannot split a shared run while kept pages have GPU watches");
        }
        const Transition transition(*this, base, runEnd);
        if (HasPageObserver()) for (auto page = base; page < runEnd; page += pageBytes) observeWrite(page, allSubpages, PageEvent::Invalidate);
        if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(base), MEM_PRESERVE_PLACEHOLDER)) fail("unmap a shared guest run to split it");
        runs.erase(base);
        remapPiece(base, cutFrom, run.section, run.offset);
        remapPiece(cutTo, runEnd, run.section, run.offset + (cutTo - base));
        for (auto page = cutFrom; page < cutTo; page += pageBytes) forgetView(page);
    }

    void remapPiece(std::uintptr_t from, std::uintptr_t to, const std::shared_ptr<Section>& section, std::uint64_t offset) {
        if (from >= to) return;
        split(from, to - from);
        if (!map(section->handle, GetCurrentProcess(), reinterpret_cast<void*>(from), offset, to - from, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0)) fail("remap a kept part of a shared guest run");
        for (auto page = from; page < to;) {
            const auto protection = views.at(page).protection;
            auto stop = page + pageBytes;
            while (stop < to && views.at(stop).protection == protection) stop += pageBytes;
            DWORD previous;
            if (!protectMemory(reinterpret_cast<void*>(page), stop - page, protection, &previous)) fail("restore kept shared guest pages");
            for (auto armed = page; armed < stop; armed += pageBytes) {
                const auto& view = views.at(armed);
                if (view.armed != 0) protectSubpages(armed, view.armed, protection == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY, "re-arm a kept shared guest page");
                if (view.cpuObservation != nullptr && !applyObservation(observationViews.at(armed), view.cpuObservation->armed)) fail("restore CPU observation protection");
                for (std::size_t sub = 0; sub < subpages; ++sub) if (view.page->gpuOwners[sub] != 0) applyGpuPage(armed + sub * subpageBytes);
                if (HasPageObserver()) observeAliasesState(armed);
            }
            page = stop;
        }
        runs.emplace(from, Run{to - from, section, offset});
    }

    void* mapAliasRuns(std::uintptr_t address, std::size_t bytes) {
        auto* reserved = allocate(GetCurrentProcess(), nullptr, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        if (reserved == nullptr) fail("reserve a read-write alias of shared guest memory");
        const auto base = reinterpret_cast<std::uintptr_t>(reserved);
        try {
            for (std::size_t done = 0; done < bytes;) {
                const auto& first = views.at(address + done);
                auto run = pageBytes;
                while (done + run < bytes) {
                    const auto& next = views.at(address + done + run);
                    if (next.offset != first.offset + run || (next.section != first.section && !sameSection(next.section->handle, first.section->handle))) break;
                    run += pageBytes;
                }
                split(base + done, run);
                if (!map(first.section->handle, GetCurrentProcess(), reinterpret_cast<void*>(base + done), first.offset, run, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) fail("map a run of a read-write alias of shared guest memory");
                done += run;
            }
        } catch (...) {
            releaseAlias(base, bytes);
            throw;
        }
        splitAliases.emplace(base, bytes);
        return reserved;
    }

    DWORD releaseAlias(std::uintptr_t base, std::size_t bytes) {
        DWORD error = ERROR_SUCCESS;
        for (auto cursor = base; cursor < base + bytes;) {
            const auto memory = query(cursor);
            const auto released = memory.State == MEM_RESERVE ? VirtualFree(memory.BaseAddress, 0, MEM_RELEASE) : unmap(GetCurrentProcess(), memory.BaseAddress, 0);
            if (!released && error == ERROR_SUCCESS) error = GetLastError();
            cursor = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        }
        return error;
    }

    Ranges cleanRanges;
    Ranges freshRanges;
    std::map<std::uintptr_t, View> views;
    std::map<std::pair<std::uintptr_t, std::uint64_t>, std::weak_ptr<SharedPage>> physical;
    std::map<std::uintptr_t, std::size_t> splitAliases;
    std::map<std::uintptr_t, std::vector<std::shared_ptr<SharedPage>>> gpuAliases;
    std::map<std::uintptr_t, GpuPage> gpuPages;
    std::map<std::uintptr_t, std::size_t> gpuHostWrites;
    std::map<std::uintptr_t, std::size_t> gpuPins;
    std::vector<std::weak_ptr<Section>> gpuSections;
    std::atomic<PageObserver> pageObserver{nullptr};
    std::map<std::uintptr_t, ObservationKey> observationViews;
    std::map<ObservationKey, std::vector<std::uintptr_t>> observationPhysical;
    std::vector<std::shared_ptr<Section>> observationSections;
    std::map<std::uintptr_t, std::vector<ObservationKey>> observationAliases;
    std::map<ObservationKey, std::size_t> observationHostAliases;
    std::map<ObservationKey, CpuObservation> cpuObservations;
    std::map<ObservationKey, PageReads> pageReads;
    std::atomic<bool> observationEnabled{false};
    bool (*observationReady)() noexcept = nullptr;
    std::atomic<void (*)() noexcept> observationFaultHook{nullptr};
    PageObservationStatistics observationCounts;
    std::mutex observationFaultMutex;
    std::array<PageTiming, static_cast<std::size_t>(PageFaultKind::Count)> observationFaultCounts{};
    PageTiming observationMappingWait;
    std::atomic<std::uint64_t> observationFaultSerial{0};
    std::uint64_t lastObservationCheckpoint = 0, lastObservationFaultSerial = 0, observationRetiredPages = 0;
    bool observationOldAliases = false;
    std::atomic<std::size_t> untrackedAliases{0};
    std::atomic<bool (*)(std::uintptr_t, bool)> gpuFault{nullptr};
    std::atomic<std::uint64_t> gpuProtectionSerial{0};
    std::atomic<bool> gpuAccessProtected{false};
    std::uint64_t gpuNextSerial = 0;
    std::map<std::uintptr_t, Run> runs;
    static constexpr std::size_t transitionSlots = 16;
    std::array<std::atomic<std::uintptr_t>, transitionSlots> transitionFrom{};
    std::array<std::atomic<std::uintptr_t>, transitionSlots> transitionTo{};
    std::atomic<std::size_t> transitionNext{0};
    std::atomic<DWORD> transitionOwner{0};
    MemoryLockProbe::Mutex mutex;
    MemoryLockProbe::MapProbe mapProbe;
    AllocateFunction allocate = nullptr;
    MapFunction map = nullptr;
    UnmapFunction unmap = nullptr;
    CompareFunction compare = nullptr;
};

}
#endif

#endif
