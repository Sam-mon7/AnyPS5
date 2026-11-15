#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP

#ifdef _WIN32
#include "MemoryLockProbe.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
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

namespace GuestArena {

class WindowsMappings {
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

    void* Reserve(void* address, std::size_t bytes) {
        return allocate(GetCurrentProcess(), address, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    }

    std::vector<std::pair<std::uintptr_t, std::size_t>> Commit(void* address, std::size_t bytes, DWORD protection, std::size_t granule, bool watched) {
        std::vector<std::pair<std::uintptr_t, std::size_t>> created;
        MemoryLockProbe::Lock lock(mutex, "commit", bytes);
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
                if (gpuFault.load(std::memory_order_acquire) != nullptr) applyGpuProtection(cursor, stop - cursor, protection);
                else {
                    DWORD previous;
                    if (!VirtualProtect(reinterpret_cast<void*>(cursor), stop - cursor, protection, &previous)) fail("protect guest memory");
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

    void Map(void* address, std::size_t bytes, HANDLE section, std::uint64_t offset, DWORD protection) {
        const MemoryLockProbe::MapProbe::Start started;
        MemoryLockProbe::Lock lock(mutex, "map", bytes);
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
            if (!VirtualProtect(address, bytes, protection, &previous)) fail("protect a run of shared guest pages");
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
            if (!VirtualProtect(page, pageBytes, protection, &previous)) fail("protect shared guest page");
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
            for (std::size_t sub = 0; sub < subpages; ++sub) if (shared->gpuOwners[sub] != 0) applyGpuPage(base + sub * subpageBytes);
            remember(freshRanges, base, base + pageBytes);
            invalidate(*shared);
            timer.PageDone();
        }
    }

    void SetProtection(std::uintptr_t address, std::size_t bytes, DWORD protection) {
        MemoryLockProbe::Lock lock(mutex, "protect", bytes);
        for (auto it = views.lower_bound(address); it != views.end() && it->first < address + bytes; ++it) {
            it->second.protection = protection;
            it->second.armed = 0;
            invalidate(*it->second.page);
        }
        for (auto it = gpuPages.lower_bound(address); it != gpuPages.end() && it->first < address + bytes; ++it) it->second.protection = protection;
    }

    void Pin(std::uintptr_t address, std::size_t bytes) {
        MemoryLockProbe::Lock lock(mutex, "pin", bytes);
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
            for (const auto alias : page.aliases) {
                auto& view = views.at(alias);
                if (view.armed == 0) continue;
                protectSubpages(alias, view.armed, view.protection, "pin shared guest page writable");
                view.armed = 0;
            }
            invalidate(page);
        }
    }

    void Unpin(std::uintptr_t address, std::size_t bytes) {
        MemoryLockProbe::Lock lock(mutex, "unpin", bytes);
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) {
            auto& page = *it->second.page;
            if (page.pins != 0) --page.pins;
            invalidate(page);
        }
        for (auto it = gpuPins.lower_bound(address & ~(subpageBytes - 1)); it != gpuPins.end() && it->first < address + bytes;) {
            if (--it->second == 0) it = gpuPins.erase(it);
            else ++it;
        }
    }

    bool HandleWrite(std::uintptr_t address) {
        if (const auto handled = handleGpuFault(address, true)) return *handled;
        MemoryLockProbe::Lock lock(mutex, "write-fault", 1);
        const auto base = address & ~(pageBytes - 1);
        const auto found = views.find(base);
        if (found == views.end() || !writable(found->second.protection)) return false;
        auto& view = found->second;
        const auto written = subpagesOf(base, address, address + 1);
        invalidate(*view.page, written);
        protectSubpages(base, written, view.protection, "resume shared memory write");
        view.armed &= static_cast<std::uint8_t>(~written);
        return true;
    }

    bool HandleTransition(std::uintptr_t address) {
        if (gpuAccessProtected.load(std::memory_order_acquire)) if (const auto handled = handleGpuFault(address, false)) return *handled;
        if (transitionOwner.load() == GetCurrentThreadId()) return false;
        bool recent = false;
        for (std::size_t slot = 0; slot < transitionSlots && !recent; ++slot) recent = transitionFrom[slot].load() <= address && address < transitionTo[slot].load();
        if (!recent) return false;
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
            invalidate(*view.page, written);
            const auto opened = static_cast<std::uint8_t>(view.armed & written);
            if (opened == 0) continue;
            protectSubpages(it->first, opened, view.protection, "open shared memory to a host write");
            view.armed &= static_cast<std::uint8_t>(~opened);
        }
        return true;
    }

    void EndHostWrite(std::uintptr_t address, std::size_t bytes) {
        MemoryLockProbe::Lock lock(mutex, "host-write-end", bytes);
        const auto end = address + bytes;
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < end; ++it) {
            --it->second.hostWrites;
            invalidate(*it->second.page, subpagesOf(it->first, address, end));
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

    bool ArmGpuWriteWatch(std::uintptr_t address, std::size_t bytes) {
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
            GpuPage item{nullptr, protection, ++gpuNextSerial, true, false};
            if (view != views.end()) {
                item.shared = view->second.page;
                const auto sub = (page % pageBytes) / subpageBytes;
                if (item.shared->pins != 0 || item.shared->hostAliases != 0 || item.shared->gpuOwners[sub] != 0) return false;
                for (const auto alias : item.shared->aliases) if (views.at(alias).hostWrites != 0) return false;
                auto& mask = sharedPages[item.shared.get()];
                if ((mask & (1u << sub)) != 0) return false;
                mask |= static_cast<std::uint8_t>(1u << sub);
            } else if (memory.Type != MEM_PRIVATE || gpuHostWrites.contains(page) || gpuPins.contains(page)) return false;
            added.emplace_back(page, std::move(item));
        }
        try {
            for (const auto& [page, item] : added) {
                gpuPages.emplace(page, item);
                if (item.shared != nullptr) item.shared->gpuOwners[(page % pageBytes) / subpageBytes] = page;
            }
            applyGpuChanges(added);
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
        if (!pending) removeGpuPage(address);
        if (shared != nullptr) {
            auto& view = views.at(address & ~(pageBytes - 1));
            if (!pending) view.armed &= static_cast<std::uint8_t>(~(1u << ((address % pageBytes) / subpageBytes)));
            invalidate(*shared, static_cast<std::uint8_t>(1u << ((address % pageBytes) / subpageBytes)));
        }
        if (!pending) {
            DWORD previous;
            if (!VirtualProtect(reinterpret_cast<void*>(address), subpageBytes, protection, &previous)) fail("open GPU publication page");
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
        if (gpuFault.load(std::memory_order_acquire) == nullptr) return false;
        MemoryLockProbe::Lock lock(mutex, "gpu-protect", bytes);
        applyGpuProtection(address, bytes, protection);
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
        if (collection == Collection::Fresh) {
            const auto fresh = freshRanges.upper_bound(address);
            if ((fresh == freshRanges.begin() || std::prev(fresh)->second <= address) && (fresh == freshRanges.end() || fresh->first >= end)) return true;
            for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < end; ++it) {
                if (it->second.protection != PAGE_NOACCESS) continue;
                for (auto at = std::max(address, it->first); at < std::min(end, it->first + pageBytes); at += 4096) {
                    if ((it->second.collected & (1u << ((at - it->first) / 4096))) == 0) return false;
                }
            }
        } else if (clear && !collectable(address, end)) return false;
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
                }
                if (full) return true;
                cursor = stop;
            } else {
                if (collection == Collection::Fresh) {
                    const auto next = views.lower_bound(cursor);
                    cursor = next == views.end() ? end : std::min(end, next->first);
                    continue;
                }
                const auto memory = query(cursor);
                if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE) return false;
                const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                if (capacity == *count) return true;
                const auto touched = (((stop + 4095) & ~static_cast<std::uintptr_t>(4095)) - (cursor & ~static_cast<std::uintptr_t>(4095))) / 4096;
                ULONG_PTR available = std::min<ULONG_PTR>(capacity - *count, touched);
                DWORD granularity = 0;
                if (GetWriteWatch(clear ? WRITE_WATCH_FLAG_RESET : 0, reinterpret_cast<void*>(cursor), stop - cursor, pages + *count, &available, &granularity) != 0) fail("collect private guest writes");
                *count += available;
                if (*count == capacity) return true;
                cursor = stop;
            }
        }
        return true;
    }

private:
    bool collectable(std::uintptr_t address, std::uintptr_t end) {
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
            const auto memory = query(cursor);
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
        const auto bit = static_cast<std::uint8_t>(1u << ((page % pageBytes) / subpageBytes));
        const bool watched = (owner != 0 && gpuPages.at(owner).armed) || (view != views.end() && (view->second.armed & bit) != 0);
        if (watched && writable(protection)) return protection == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY;
        return protection;
    }

    void protectGpuPage(std::uintptr_t page, DWORD protection) {
        DWORD previous;
        if (!VirtualProtect(reinterpret_cast<void*>(page), subpageBytes, protection, &previous)) fail("protect GPU watched page");
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
            if (!VirtualProtect(reinterpret_cast<void*>(page), end - page, protection, &previous)) fail("protect private GPU watched pages");
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

    std::optional<bool> handleGpuFault(std::uintptr_t address, bool write) {
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
        const auto found = gpuAliases.find(reinterpret_cast<std::uintptr_t>(alias));
        if (found == gpuAliases.end()) {
            untrackedAliases.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        for (const auto& page : found->second) --page->hostAliases;
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

    static void protectSubpages(std::uintptr_t base, std::uint8_t mask, DWORD protection, const char* operation) {
        for (std::size_t sub = 0; sub < subpages;) {
            if ((mask & (1u << sub)) == 0) {
                ++sub;
                continue;
            }
            auto stop = sub;
            while (stop < subpages && (mask & (1u << stop)) != 0) ++stop;
            DWORD previous;
            if (!VirtualProtect(reinterpret_cast<void*>(base + sub * subpageBytes), (stop - sub) * subpageBytes, protection, &previous)) fail(operation);
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
        const auto runEnd = base + run.bytes;
        const auto cutFrom = std::max(base, address);
        const auto cutTo = std::min(runEnd, end);
        for (auto page = base; !gpuPages.empty() && page < runEnd; page += pageBytes) {
            if (page >= cutFrom && page < cutTo) continue;
            const auto& owners = views.at(page).page->gpuOwners;
            if (std::any_of(owners.begin(), owners.end(), [](auto owner) { return owner != 0; })) throw std::runtime_error("cannot split a shared run while kept pages have GPU watches");
        }
        const Transition transition(*this, base, runEnd);
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
            if (!VirtualProtect(reinterpret_cast<void*>(page), stop - page, protection, &previous)) fail("restore kept shared guest pages");
            for (auto armed = page; armed < stop; armed += pageBytes) {
                const auto& view = views.at(armed);
                if (view.armed != 0) protectSubpages(armed, view.armed, protection == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY, "re-arm a kept shared guest page");
                for (std::size_t sub = 0; sub < subpages; ++sub) if (view.page->gpuOwners[sub] != 0) applyGpuPage(armed + sub * subpageBytes);
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
