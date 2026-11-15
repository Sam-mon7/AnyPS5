#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"
#include "prx/libc/include/WindowsMappings.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#ifndef _WIN32
#include <cerrno>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/mman.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace GuestArena {
#ifdef _WIN32
namespace {
std::atomic<std::uint64_t> queryProtectionSerial{1};
std::atomic<std::uint32_t> queryProtectionWriters{0};

struct LazyProtectionHold {
    LazyProtectionHold(std::uintptr_t address, std::size_t bytes) : address(address), bytes(GuestWtFaultLazyPreservationEnabled_nid_no_patch() ? bytes : 0) {
        if (this->bytes != 0) GuestWtFaultHold_nid_no_patch(address, this->bytes);
    }
    ~LazyProtectionHold() { if (bytes != 0) GuestWtFaultRelease_nid_no_patch(address, bytes); }
    std::uintptr_t address;
    std::size_t bytes;
};
}

std::uint64_t GuestArenaQueryProtectionSerial_nid_no_patch() {
    if (queryProtectionWriters.load(std::memory_order_acquire) != 0) return 0;
    const auto serial = queryProtectionSerial.load(std::memory_order_acquire);
    return queryProtectionWriters.load(std::memory_order_acquire) == 0 ? serial : 0;
}

BOOL GuestArenaVirtualProtect_nid_no_patch(void* address, SIZE_T bytes, DWORD protection, DWORD* previous) {
    const LazyProtectionHold hold(reinterpret_cast<std::uintptr_t>(address), bytes);
    return GuestArenaVirtualProtectInternal_nid_no_patch(address, bytes, protection, previous);
}

BOOL GuestArenaVirtualProtectInternal_nid_no_patch(void* address, SIZE_T bytes, DWORD protection, DWORD* previous) {
    queryProtectionWriters.fetch_add(1, std::memory_order_acq_rel);
    const auto result = VirtualProtect(address, bytes, protection, previous);
    // The write tracker's pages take the new protection as the guest's (APS5_ARCHB_WT_FAULT).
    GuestWtFaultForget_nid_no_patch(reinterpret_cast<std::uintptr_t>(address), bytes);
    queryProtectionSerial.fetch_add(1, std::memory_order_release);
    queryProtectionWriters.fetch_sub(1, std::memory_order_release);
    return result;
}
#endif

namespace {

std::atomic<void (*)(std::uintptr_t, std::size_t)> hostWriteHook{nullptr};

std::atomic<void (*)(std::uintptr_t, std::size_t)> hostReadHook{nullptr};

#ifdef _WIN32
std::atomic<std::uint64_t> commitGeneration{1};
std::atomic<void (*)(std::uintptr_t, std::size_t, std::uint64_t)> privateMappingObserver{nullptr};
#endif
std::atomic<void (*)(std::uintptr_t, std::size_t)> accessObserver{nullptr};
std::atomic<void (*)(std::uintptr_t, std::size_t, bool)> hostWriteObserver{nullptr};

void noteHostWrite(const void* pointer, std::size_t bytes, bool end) {
    if (const auto callback = hostWriteObserver.load(std::memory_order_acquire)) callback(reinterpret_cast<std::uintptr_t>(pointer), bytes, end);
}

void noteAccess(const void* pointer, std::size_t bytes) {
    if (const auto callback = accessObserver.load(std::memory_order_acquire)) callback(reinterpret_cast<std::uintptr_t>(pointer), bytes);
}

constexpr std::uintptr_t GuestArenaOffset = GuestArena::GuestAddressOffset;
constexpr std::uintptr_t DefaultAllocationStart = 0x0000000200000000ull + GuestArenaOffset;
#ifdef __APPLE__
constexpr std::uintptr_t ArenaStart = DefaultAllocationStart;
#else
constexpr std::uintptr_t ArenaStart = GuestArenaOffset == 0 ? 0x10000ull : DefaultAllocationStart;
#endif
constexpr std::uintptr_t SystemReservedStart = 0x00000007FFFFC000ull + GuestArenaOffset;
constexpr std::uintptr_t SystemReservedEnd = 0x0000001000000000ull + GuestArenaOffset;
constexpr std::uintptr_t ApplicationAreaEnd = 0x000000FC00000000ull + GuestArenaOffset;

#ifdef __EMSCRIPTEN__
constexpr std::size_t EmscriptenArenaAlignment = std::size_t{1} << 32;
constexpr std::size_t EmscriptenArenaBytes = std::size_t{8} << 30;
#endif

std::uintptr_t alignUp(std::uintptr_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
}

class Arena {
public:
    static Arena& Get() {
        static auto* arena = new Arena;
        return *arena;
    }

    bool Available() const {
        return _base != 0;
    }

    bool Contains(const void* pointer, std::size_t bytes) const {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        return _base != 0 && address >= _base && bytes <= _end - _base && address - _base <= (_end - _base) - bytes;
    }

    void* Allocate(std::size_t bytes, std::size_t alignment) {
        return AllocateAtOrAbove(0, bytes, alignment);
    }

    void* AllocateAtOrAbove(std::uintptr_t hint, std::size_t bytes, std::size_t alignment) {
        RequireAllocatable(alignment);
        std::lock_guard lock(_lock);
        if (hint >= _end) throw std::runtime_error("mapping address hint is above the guest address space arena");
        return Claim(FindAtOrAbove(hint, bytes, alignment), bytes);
    }

    void* AllocateFromHint(std::uintptr_t hint, std::size_t bytes, std::size_t alignment) {
        RequireAllocatable(alignment);
        std::lock_guard lock(_lock);
        auto candidate = hint < _end ? FindAtOrAbove(hint, bytes, alignment) : 0;
        if (candidate == 0 && hint > _base) candidate = FindAtOrAbove(hint < DefaultAllocationStart ? _base : DefaultAllocationStart, bytes, alignment);
        return Claim(candidate, bytes);
    }

    std::uintptr_t HighestFree(std::size_t bytes, std::size_t alignment) {
        if (_base == 0) throw std::runtime_error("guest address space arena is unavailable");
        if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) throw std::invalid_argument("invalid guest arena window");
        std::lock_guard lock(_lock);
        auto limit = _end;
        while (limit >= _base && bytes <= limit - _base) {
            const auto candidate = (limit - bytes) & ~(alignment - 1);
            if (candidate < _base) break;
            const auto next = _used.lower_bound(candidate + bytes);
            if (next == _used.begin() || std::prev(next)->second <= candidate) return candidate;
            limit = std::prev(next)->first;
        }
        return 0;
    }

    bool NextUsed(std::uintptr_t address, std::uintptr_t* start, std::uintptr_t* end) {
        std::lock_guard lock(_lock);
        auto it = _used.upper_bound(address);
        if (it != _used.begin() && std::prev(it)->second > address) --it;
        if (it == _used.end()) return false;
        *start = it->first;
        *end = it->second;
        return true;
    }

    bool MarkUsedForCommit(const void* pointer, std::size_t bytes) {
        std::lock_guard lock(_lock);
        const auto start = reinterpret_cast<std::uintptr_t>(pointer);
        const auto end = start + bytes;
        const auto exact = _used.find(start);
        if (exact != _used.end() && exact->second >= end) return false;
        const auto next = _used.lower_bound(start);
        if ((next != _used.end() && next->first < end) || (next != _used.begin() && std::prev(next)->second > start)) {
            char message[160];
            std::snprintf(message, sizeof(message), "guest arena commit 0x%llx+0x%zx overlaps a used range", static_cast<unsigned long long>(start), bytes);
            throw std::runtime_error(message);
        }
        _used.emplace(start, end);
        return true;
    }

    void MarkUsed(const void* pointer, std::size_t bytes) {
        std::lock_guard lock(_lock);
        const auto start = reinterpret_cast<std::uintptr_t>(pointer);
        const auto next = _used.lower_bound(start);
        if ((next != _used.end() && next->first < start + bytes) || (next != _used.begin() && std::prev(next)->second > start)) {
            char message[160];
            std::snprintf(message, sizeof(message), "fixed guest mapping 0x%llx+0x%zx overlaps %s", static_cast<unsigned long long>(start), bytes, OverlapsHostRegion(start, bytes) ? "a host region in the guest arena" : "a guest arena range");
            throw std::runtime_error(message);
        }
        _used.emplace(start, start + bytes);
    }

    void Release(const void* pointer, std::size_t bytes) {
        std::lock_guard lock(_lock);
        const auto start = reinterpret_cast<std::uintptr_t>(pointer);
        const auto end = start + bytes;
#ifdef __EMSCRIPTEN__
        NoteDirtyLocked(start, end);
#endif
        _occupiedPrefixEnd = std::min(_occupiedPrefixEnd, std::max(_base, start));
        _occupiedDefaultPrefixEnd = std::min(_occupiedDefaultPrefixEnd, std::max(DefaultAllocationStart, start));
        auto it = _used.upper_bound(start);
        if (it != _used.begin()) --it;
        while (it != _used.end() && it->first < end) {
            const auto rangeStart = it->first;
            const auto rangeEnd = it->second;
            if (rangeEnd <= start) {
                ++it;
                continue;
            }
            it = _used.erase(it);
            if (rangeStart < start) _used.emplace(rangeStart, start);
            if (rangeEnd > end) _used.emplace(end, rangeEnd);
        }
        for (const auto& [holeStart, holeEnd] : _holes) {
            if (holeStart < end && start < holeEnd) _used.emplace(holeStart, holeEnd);
        }
    }

private:
    Arena() {
#if defined(__EMSCRIPTEN__)
        void* const region = std::aligned_alloc(EmscriptenArenaAlignment, EmscriptenArenaBytes);
        if (region == nullptr) throw std::runtime_error("guest arena: cannot reserve the guest range in WebAssembly memory");
        _base = reinterpret_cast<std::uintptr_t>(region);
        _end = _base + EmscriptenArenaBytes;
#elif defined(__linux__) || defined(__APPLE__)
        SeedHostUsedRanges();
#elif defined(_WIN32)
        _writeWatched = std::getenv("APS5_NO_WRITE_WATCH") == nullptr;
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const std::uintptr_t granularity = system.dwAllocationGranularity;
        const std::uintptr_t end = ApplicationAreaEnd;
        std::uint32_t retries = 0;
        for (std::uintptr_t cursor = ArenaStart; cursor < end;) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "query the guest arena range");
            const auto regionEnd = std::min(end, reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
            const auto first = info.State == MEM_FREE ? std::min(regionEnd, alignUp(cursor, granularity)) : regionEnd;
            const auto last = std::max(first, regionEnd & ~(granularity - 1));
            if (first < last && WindowsMappings::Get().Reserve(reinterpret_cast<void*>(first), last - first) == nullptr) {
                const auto error = GetLastError();
                if (error == ERROR_INVALID_ADDRESS && ++retries < 64u) continue;
                throw std::system_error(static_cast<int>(error), std::system_category(), "reserve the guest arena range");
            }
            retries = 0;
            if (cursor < first) _holes.emplace_back(cursor, first);
            if (last < regionEnd) _holes.emplace_back(last, regionEnd);
            cursor = regionEnd;
        }
        _hostRegions = _holes;
        _holes.emplace_back(SystemReservedStart, SystemReservedEnd);
        for (const auto& [holeStart, holeEnd] : _holes) _used.emplace(holeStart, holeEnd);
        _base = ArenaStart;
        _end = end;
#endif
    }

#if defined(__linux__)
    static std::vector<std::pair<std::uintptr_t, std::uintptr_t>> HostMappings() {
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> mappings;
        std::ifstream maps("/proc/self/maps");
        if (!maps.is_open()) throw std::runtime_error("guest arena: cannot open /proc/self/maps");
        std::string line;
        while (std::getline(maps, line)) {
            std::istringstream fields(line);
            std::uintptr_t begin = 0, end = 0;
            char dash = 0;
            if (!(fields >> std::hex >> begin >> dash >> end) || dash != '-' || end <= begin) throw std::runtime_error("guest arena: malformed /proc/self/maps line: " + line);
            mappings.emplace_back(begin, end);
        }
        return mappings;
    }

    static int ReserveNoReplace(std::uintptr_t address, std::size_t bytes) {
        void* const wanted = reinterpret_cast<void*>(address);
        void* const placed = mmap(wanted, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (placed == wanted) return 0;
        if (placed == MAP_FAILED) return errno;
        munmap(placed, bytes);
        return EINVAL;
    }
#elif defined(__APPLE__)
    static std::vector<std::pair<std::uintptr_t, std::uintptr_t>> HostMappings() {
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> mappings;
        mach_vm_address_t address = ArenaStart;
        while (address < ApplicationAreaEnd) {
            mach_vm_size_t size = 0;
            vm_region_basic_info_data_64_t info{};
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object = MACH_PORT_NULL;
            const kern_return_t result = mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
            if (result == KERN_INVALID_ADDRESS) break;
            if (result != KERN_SUCCESS) throw std::runtime_error("guest arena: mach_vm_region failed");
            mappings.emplace_back(static_cast<std::uintptr_t>(address), static_cast<std::uintptr_t>(address + size));
            address += size;
        }
        return mappings;
    }

    static int ReserveNoReplace(std::uintptr_t address, std::size_t bytes) {
        mach_vm_address_t wanted = address;
        const kern_return_t result = mach_vm_map(mach_task_self(), &wanted, bytes, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_DEFAULT);
        if (result == KERN_SUCCESS) return 0;
        return result == KERN_NO_SPACE ? EEXIST : ENOMEM;
    }
#endif

#if defined(__linux__) || defined(__APPLE__)
    void SeedHostUsedRanges() {
        for (const auto& [begin, end] : HostMappings()) {
            if (end <= ArenaStart || begin >= ApplicationAreaEnd) continue;
            const auto first = std::max(begin, ArenaStart);
            const auto last = std::min(end, ApplicationAreaEnd);
            _used.emplace(first, last);
            _hostRegions.emplace_back(first, last);
        }
        _used.emplace(SystemReservedStart, SystemReservedEnd);
        _hostRegions.emplace_back(SystemReservedStart, SystemReservedEnd);
        ReserveFreeRanges();
        _base = ArenaStart;
        _end = ApplicationAreaEnd;
    }

    static void TryReserve(std::vector<std::pair<std::uintptr_t, std::uintptr_t>>& occupied, std::uintptr_t from, std::uintptr_t to) {
        constexpr std::uintptr_t kChunk = 1ULL << 30;
        for (std::uintptr_t cursor = from; cursor < to; cursor += kChunk) {
            const auto end = std::min(cursor + kChunk, to);
            const int failed = ReserveNoReplace(cursor, end - cursor);
            if (failed == 0) continue;
            if (failed != EEXIST) {
                if (cursor < DefaultAllocationStart && (failed == EACCES || failed == EPERM)) {
                    occupied.emplace_back(cursor, end);
                    continue;
                }
                char message[128];
                std::snprintf(message, sizeof(message), "guest arena: cannot reserve 0x%llx+0x%llx",
                    static_cast<unsigned long long>(cursor), static_cast<unsigned long long>(end - cursor));
                throw std::system_error(failed, std::system_category(), message);
            }
            occupied.emplace_back(cursor, end);
        }
    }

    void ReserveFreeRanges() {
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> occupied;
        std::uintptr_t cursor = ArenaStart;
        for (const auto& [usedStart, usedEnd] : _used) {
            if (usedStart > ApplicationAreaEnd) break;
            if (usedStart > cursor) TryReserve(occupied, cursor, std::min(usedStart, ApplicationAreaEnd));
            if (usedEnd > cursor) cursor = usedEnd;
            if (cursor >= ApplicationAreaEnd) break;
        }
        if (cursor < ApplicationAreaEnd) TryReserve(occupied, cursor, ApplicationAreaEnd);
        for (const auto& [from, to] : occupied) {
            _used.emplace(from, to);
            _hostRegions.emplace_back(from, to);
        }
    }
#endif

#ifdef __EMSCRIPTEN__
    void NoteDirtyLocked(std::uintptr_t start, std::uintptr_t end) {
        auto it = _dirty.upper_bound(start);
        if (it != _dirty.begin() && std::prev(it)->second >= start) --it;
        while (it != _dirty.end() && it->first <= end) {
            start = std::min(start, it->first);
            end = std::max(end, it->second);
            it = _dirty.erase(it);
        }
        _dirty.emplace(start, end);
    }

public:
    void NoteDirty(const void* pointer, std::size_t bytes) {
        std::lock_guard lock(_lock);
        const auto start = reinterpret_cast<std::uintptr_t>(pointer);
        NoteDirtyLocked(start, start + bytes);
    }

    void ZeroDirty(void* pointer, std::size_t bytes) {
        std::lock_guard lock(_lock);
        const auto start = reinterpret_cast<std::uintptr_t>(pointer);
        const auto end = start + bytes;
        auto it = _dirty.upper_bound(start);
        if (it != _dirty.begin() && std::prev(it)->second > start) --it;
        while (it != _dirty.end() && it->first < end) {
            const auto [dirtyStart, dirtyEnd] = *it;
            const auto first = std::max(dirtyStart, start);
            const auto last = std::min(dirtyEnd, end);
            std::memset(reinterpret_cast<void*>(first), 0, last - first);
            it = _dirty.erase(it);
            if (dirtyStart < first) _dirty.emplace(dirtyStart, first);
            if (dirtyEnd > last) it = _dirty.emplace(last, dirtyEnd).first;
        }
    }

private:
    std::map<std::uintptr_t, std::uintptr_t> _dirty;
#endif

    std::mutex _lock;
    std::map<std::uintptr_t, std::uintptr_t> _used;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> _holes;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> _hostRegions;
    void RequireAllocatable(std::size_t alignment) const {
        if (_base == 0) throw std::runtime_error("guest address space arena is unavailable");
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) throw std::invalid_argument("invalid guest arena alignment");
    }

    std::uintptr_t FindAtOrAbove(std::uintptr_t hint, std::size_t bytes, std::size_t alignment) {
        const auto first = hint == 0 ? DefaultAllocationStart : std::max(_base, hint);
        auto& prefixEnd = first >= DefaultAllocationStart ? _occupiedDefaultPrefixEnd : _occupiedPrefixEnd;
        std::uintptr_t candidate = alignUp(bytes == 0 ? first : std::max(prefixEnd, first), alignment);
        auto it = _used.upper_bound(candidate);
        if (it != _used.begin() && std::prev(it)->second > candidate) --it;
        for (; it != _used.end(); ++it) {
            const auto& [start, end] = *it;
            if (candidate <= _end && bytes <= _end - candidate && candidate + bytes <= start) break;
            if (start <= prefixEnd) prefixEnd = std::max(prefixEnd, end);
            candidate = std::max(candidate, alignUp(end, alignment));
        }
        if (candidate > _end || bytes > _end - candidate) return 0;
        return candidate;
    }

    void* Claim(std::uintptr_t candidate, std::size_t bytes) {
        if (candidate == 0) throw std::runtime_error("guest address space arena exhausted");
        _used.emplace(candidate, candidate + bytes);
        return reinterpret_cast<void*>(candidate);
    }

    std::uintptr_t _base = 0;
    std::uintptr_t _end = 0;
    std::uintptr_t _occupiedPrefixEnd = ArenaStart;
    std::uintptr_t _occupiedDefaultPrefixEnd = DefaultAllocationStart;
    bool _writeWatched = false;

public:
    std::uintptr_t Base() const { return _base; }
    std::size_t Size() const { return _end - _base; }
    bool WriteWatched() const { return _writeWatched; }
    bool OverlapsHostRegion(std::uintptr_t start, std::size_t bytes) const {
        for (const auto& [regionStart, regionEnd] : _hostRegions) {
            if (regionStart < start + bytes && start < regionEnd) return true;
        }
        return false;
    }

    // OverlapsHostRegion by binary search (APS5_ARCHB_FIX_WRITE_TRACKING_BOOKKEEPING): the host regions
    // are fixed once the arena is constructed, so they are sorted by start once, with the largest end
    // of every prefix, and the last region starting before the range's end decides.
    bool OverlapsHostRegionSorted(std::uintptr_t start, std::size_t bytes) const {
        if (!_sortedReady.load(std::memory_order_acquire)) {
            std::call_once(_sortedOnce, [&] {
                _sortedStarts.reserve(_hostRegions.size());
                auto sorted = _hostRegions;
                std::sort(sorted.begin(), sorted.end());
                std::uintptr_t reach = 0;
                for (const auto& [regionStart, regionEnd] : sorted) {
                    reach = std::max(reach, regionEnd);
                    _sortedStarts.push_back(regionStart);
                    _sortedReach.push_back(reach);
                }
                _sortedReady.store(true, std::memory_order_release);
            });
        }
        const auto end = start + bytes;
        const auto after = std::lower_bound(_sortedStarts.begin(), _sortedStarts.end(), end);
        if (after == _sortedStarts.begin()) return false;
        return _sortedReach[static_cast<std::size_t>(after - _sortedStarts.begin()) - 1] > start;
    }

#ifndef _WIN32
    void ReleaseHostRegion(const void* pointer, std::size_t bytes) {
#ifdef __EMSCRIPTEN__
        throw std::runtime_error("guest arena: host region release is unsupported in WebAssembly");
#else
        Release(pointer, bytes);
        std::lock_guard lock(_lock);
        const auto start = reinterpret_cast<std::uintptr_t>(pointer);
        const auto end = start + bytes;
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> kept;
        for (const auto& [regionStart, regionEnd] : _hostRegions) {
            if (regionEnd <= start || regionStart >= end) {
                kept.emplace_back(regionStart, regionEnd);
                continue;
            }
            if (regionStart < start) kept.emplace_back(regionStart, start);
            if (regionEnd > end) kept.emplace_back(end, regionEnd);
        }
        _hostRegions = std::move(kept);
        static_cast<void>(ReserveNoReplace(start, bytes));
#endif
    }
#endif

private:
    mutable std::once_flag _sortedOnce;
    mutable std::atomic<bool> _sortedReady{false};
    mutable std::vector<std::uintptr_t> _sortedStarts;
    mutable std::vector<std::uintptr_t> _sortedReach;
};

const bool g_reserved = (Arena::Get(), true);

}

bool GuestArenaAvailable_nid_postfix() {
    return Arena::Get().Available();
}

bool GuestArenaContains_nid_postfix(const void* pointer, std::size_t bytes) {
    return Arena::Get().Contains(pointer, bytes);
}

void* GuestArenaAllocate_nid_postfix(std::size_t bytes, std::size_t alignment) {
    return Arena::Get().Allocate(bytes, alignment);
}

void* GuestArenaAllocateAtOrAbove_nid_postfix(std::uintptr_t hint, std::size_t bytes, std::size_t alignment) {
    return Arena::Get().AllocateAtOrAbove(hint, bytes, alignment);
}

void* GuestArenaAllocateFromHint_nid_postfix(std::uintptr_t hint, std::size_t bytes, std::size_t alignment) {
    return Arena::Get().AllocateFromHint(hint, bytes, alignment);
}

std::uintptr_t GuestArenaHighestFree_nid_no_patch(std::size_t bytes, std::size_t alignment) {
    return Arena::Get().HighestFree(bytes, alignment);
}

bool GuestArenaNextUsed_nid_no_patch(std::uintptr_t address, std::uintptr_t* start, std::uintptr_t* end) {
    return Arena::Get().NextUsed(address, start, end);
}

void GuestArenaMarkUsed_nid_postfix(const void* pointer, std::size_t bytes) {
    Arena::Get().MarkUsed(pointer, bytes);
}

void GuestArenaRelease_nid_postfix(const void* pointer, std::size_t bytes) {
    Arena::Get().Release(pointer, bytes);
}

void GuestArenaRange_nid_postfix(std::uintptr_t* base, std::size_t* bytes) {
    *base = Arena::Get().Base();
    *bytes = Arena::Get().Size();
}

namespace {

std::invalid_argument OutsideArena(const char* operation, const void* pointer, std::size_t bytes) {
    char message[128];
    std::snprintf(message, sizeof(message), "%s 0x%llx+0x%zx outside the guest arena", operation, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pointer)), bytes);
    return std::invalid_argument(message);
}

}

#ifdef _WIN32
extern "C" {
int GuardedCopy(std::uintptr_t source, void* destination, std::size_t bytes);
int GuardedCompare(std::uintptr_t source, const void* expected, std::size_t bytes);
int GuardedCopyAvx2(std::uintptr_t source, void* destination, std::size_t bytes);
int GuardedCompareAvx2(std::uintptr_t source, const void* expected, std::size_t bytes);
extern const char GuardedCopyAvx2Load[];
extern const char GuardedCopyAvx2HighLoad[];
extern const char GuardedCompareAvx2Load[];
extern const char GuardedCompareAvx2HighLoad[];
extern const char GuardedCopyWideLoad[];
extern const char GuardedCopyByteLoad[];
extern const char GuardedCompareWideLoad[];
extern const char GuardedCompareByteLoad[];
extern const char GuardedReadFailed[];
extern const char GuardedReadFailedAvx2[];
}

asm(".text\n"
    ".seh_proc GuardedCopy\n"
    "GuardedCopy:\n"
    ".seh_endprologue\n"
    "    leaq (%rcx,%r8), %r9\n"
    "    subq %rcx, %rdx\n"
    "    jmp 2f\n"
    "GuardedCopyWideLoad:\n"
    "1:  movdqu (%rcx), %xmm0\n"
    "    movdqu %xmm0, (%rcx,%rdx)\n"
    "    addq $16, %rcx\n"
    "GuardedCopyTail:\n"
    "2:  leaq 16(%rcx), %rax\n"
    "    cmpq %r9, %rax\n"
    "    jbe 1b\n"
    "    jmp 4f\n"
    "GuardedCopyByteLoad:\n"
    "3:  movzbl (%rcx), %eax\n"
    "    movb %al, (%rcx,%rdx)\n"
    "    incq %rcx\n"
    "4:  cmpq %r9, %rcx\n"
    "    jb 3b\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".seh_endproc\n"
    ".seh_proc GuardedCompare\n"
    "GuardedCompare:\n"
    ".seh_endprologue\n"
    "    leaq (%rcx,%r8), %r9\n"
    "    subq %rcx, %rdx\n"
    "    jmp 2f\n"
    "GuardedCompareWideLoad:\n"
    "1:  movdqu (%rcx), %xmm0\n"
    "    movdqu (%rcx,%rdx), %xmm1\n"
    "    pcmpeqb %xmm1, %xmm0\n"
    "    pmovmskb %xmm0, %eax\n"
    "    cmpl $0xffff, %eax\n"
    "    jne 5f\n"
    "    addq $16, %rcx\n"
    "GuardedCompareTail:\n"
    "2:  leaq 16(%rcx), %rax\n"
    "    cmpq %r9, %rax\n"
    "    jbe 1b\n"
    "    jmp 4f\n"
    "GuardedCompareByteLoad:\n"
    "3:  movzbl (%rcx), %eax\n"
    "    cmpb (%rcx,%rdx), %al\n"
    "    jne 5f\n"
    "    incq %rcx\n"
    "4:  cmpq %r9, %rcx\n"
    "    jb 3b\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    "5:  movl $1, %eax\n"
    "    ret\n"
    "GuardedReadFailed:\n"
    "    movl $-1, %eax\n"
    "    ret\n"
    ".seh_endproc\n"
    ".seh_proc GuardedCopyAvx2\n"
    "GuardedCopyAvx2:\n"
    ".seh_endprologue\n"
    "    leaq (%rcx,%r8), %r9\n"
    "    subq %rcx, %rdx\n"
    "    jmp 2f\n"
    "GuardedCopyAvx2Load:\n"
    "1:  vmovdqu (%rcx), %ymm0\n"
    "GuardedCopyAvx2HighLoad:\n"
    "    vmovdqu 32(%rcx), %ymm1\n"
    "    vmovdqu %ymm0, (%rcx,%rdx)\n"
    "    vmovdqu %ymm1, 32(%rcx,%rdx)\n"
    "    addq $64, %rcx\n"
    "2:  leaq 64(%rcx), %rax\n"
    "    cmpq %r9, %rax\n"
    "    jbe 1b\n"
    "    vzeroupper\n"
    "    jmp GuardedCopyTail\n"
    ".seh_endproc\n"
    ".seh_proc GuardedCompareAvx2\n"
    "GuardedCompareAvx2:\n"
    ".seh_endprologue\n"
    "    leaq (%rcx,%r8), %r9\n"
    "    subq %rcx, %rdx\n"
    "    jmp 2f\n"
    "GuardedCompareAvx2Load:\n"
    "1:  vmovdqu (%rcx), %ymm0\n"
    "GuardedCompareAvx2HighLoad:\n"
    "    vmovdqu 32(%rcx), %ymm1\n"
    "    vpcmpeqb (%rcx,%rdx), %ymm0, %ymm0\n"
    "    vpcmpeqb 32(%rcx,%rdx), %ymm1, %ymm1\n"
    "    vpand %ymm1, %ymm0, %ymm0\n"
    "    vpmovmskb %ymm0, %eax\n"
    "    cmpl $-1, %eax\n"
    "    jne 3f\n"
    "    addq $64, %rcx\n"
    "2:  leaq 64(%rcx), %rax\n"
    "    cmpq %r9, %rax\n"
    "    jbe 1b\n"
    "    vzeroupper\n"
    "    jmp GuardedCompareTail\n"
    "3:  vzeroupper\n"
    "    movl $1, %eax\n"
    "    ret\n"
    "GuardedReadFailedAvx2:\n"
    "    vzeroupper\n"
    "    movl $-1, %eax\n"
    "    ret\n"
    ".seh_endproc\n");

namespace {

bool HasAvx2() {
    static const bool avx2 = __builtin_cpu_supports("avx2");
    return avx2;
}

bool OwnedByGuest(std::uintptr_t address, std::size_t bytes) {
    const auto& arena = Arena::Get();
    return arena.Contains(reinterpret_cast<const void*>(address), bytes) && !arena.OverlapsHostRegion(address, bytes);
}

}

bool GuestArenaCopyOwned_nid_no_patch(std::uintptr_t source, void* destination, std::size_t bytes) {
    return OwnedByGuest(source, bytes) && (HasAvx2() ? GuardedCopyAvx2 : GuardedCopy)(source, destination, bytes) == 0;
}

bool GuestArenaCompareOwned_nid_no_patch(std::uintptr_t source, const void* expected, std::size_t bytes, bool* equal) {
    if (!OwnedByGuest(source, bytes)) return false;
    const auto result = (HasAvx2() ? GuardedCompareAvx2 : GuardedCompare)(source, expected, bytes);
    *equal = result == 0;
    return result >= 0;
}

bool GuestArenaRecoverRead_nid_no_patch(EXCEPTION_POINTERS* exception) {
    const auto* record = exception->ExceptionRecord;
    const auto code = record->ExceptionCode;
    const bool guard = code == STATUS_GUARD_PAGE_VIOLATION;
    if ((code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR && !guard) || record->NumberParameters < 2 || record->ExceptionInformation[0] != 0) return false;
    auto* context = exception->ContextRecord;
    const auto at = reinterpret_cast<const char*>(context->Rip);
    const bool avx2 = at == GuardedCopyAvx2Load || at == GuardedCopyAvx2HighLoad || at == GuardedCompareAvx2Load || at == GuardedCompareAvx2HighLoad;
    if (!avx2 && at != GuardedCopyWideLoad && at != GuardedCopyByteLoad && at != GuardedCompareWideLoad && at != GuardedCompareByteLoad) return false;
    const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
    if (address < context->Rcx || address >= context->R9) return false;
    if (guard) {
        const auto page = address & ~static_cast<std::uintptr_t>(0xfff);
        MEMORY_BASIC_INFORMATION memory{};
        DWORD previous = 0;
        if (VirtualQuery(reinterpret_cast<const void*>(page), &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT && (memory.Protect & PAGE_GUARD) == 0) VirtualProtect(reinterpret_cast<void*>(page), 0x1000, memory.Protect | PAGE_GUARD, &previous);
    }
    context->Rip = reinterpret_cast<DWORD64>(avx2 ? GuardedReadFailedAvx2 : GuardedReadFailed);
    return true;
}

void GuestArenaSetProtection_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uint32_t protection) {
    const LazyProtectionHold hold(address, bytes);
    WindowsMappings::Get().SetProtection(address, bytes, protection);
    GuestWtFaultForget_nid_no_patch(address, bytes);
    noteAccess(reinterpret_cast<const void*>(address), bytes);
}

bool GuestArenaHandleWrite_nid_postfix(std::uintptr_t address) {
    return WindowsMappings::Get().HandleWrite(address);
}

bool GuestArenaHandleRead_nid_postfix(std::uintptr_t address) {
    return WindowsMappings::Get().HandleRead(address);
}

void GuestArenaPinWritable_nid_postfix(const void* pointer, std::size_t bytes) {
    if (!Arena::Get().Contains(pointer, bytes)) return;
    WindowsMappings::Get().Pin(reinterpret_cast<std::uintptr_t>(pointer), bytes);
}

void GuestArenaUnpinWritable_nid_postfix(const void* pointer, std::size_t bytes) {
    if (!Arena::Get().Contains(pointer, bytes)) return;
    WindowsMappings::Get().Unpin(reinterpret_cast<std::uintptr_t>(pointer), bytes);
}

bool GuestArenaProtection_nid_postfix(std::uintptr_t address, std::uint32_t* protection) {
    return WindowsMappings::Get().Protection(address, protection);
}

bool GuestArenaCollectWrites_nid_postfix(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear) {
    return WindowsMappings::Get().Collect(address, bytes, pages, count, clear);
}

bool GuestArenaCollectWritesMemo_nid_no_patch(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear, bool armed, const QueryMemoContext* memo) {
    return WindowsMappings::Get().Collect(address, bytes, pages, count, clear, armed ? WindowsMappings::Collection::Armed : WindowsMappings::Collection::All, memo);
}

bool GuestArenaCollectArmedWrites_nid_postfix(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear) {
    return WindowsMappings::Get().Collect(address, bytes, pages, count, clear, WindowsMappings::Collection::Armed);
}

bool GuestArenaCollectFreshWrites_nid_postfix(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear) {
    return WindowsMappings::Get().Collect(address, bytes, pages, count, clear, WindowsMappings::Collection::Fresh);
}

bool GuestArenaWritesClean_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    return WindowsMappings::Get().WritesClean(address, bytes);
}

WriteWatchTiming GuestArenaWriteWatchTiming_nid_no_patch() {
    return WindowsMappings::ThreadTiming();
}

WriteWatchCensus GuestArenaWriteWatchCensus_nid_no_patch() {
    return WindowsMappings::ThreadCensus();
}

WriteWatchSweep GuestArenaWriteWatchSweep_nid_no_patch() {
    return WindowsMappings::Get().Sweep();
}

WalkCost GuestArenaWalkCost_nid_no_patch() {
    return WindowsMappings::Cost();
}

bool GuestArenaHostRegionOverlaps_nid_postfix(std::uintptr_t address, std::size_t bytes) {
    return Arena::Get().OverlapsHostRegion(address, bytes);
}

bool GuestArenaHostRegionOverlapsSorted_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    return Arena::Get().OverlapsHostRegionSorted(address, bytes);
}

std::uint64_t GuestArenaCommitGeneration_nid_postfix() {
    return commitGeneration.load(std::memory_order_acquire);
}

void GuestArenaSetPrivateMappingObserver_nid_postfix(void (*callback)(std::uintptr_t, std::size_t, std::uint64_t)) {
    privateMappingObserver.store(callback, std::memory_order_release);
}

void GuestArenaCommit_nid_postfix(void* pointer, std::size_t bytes, std::uint32_t protection, std::size_t granule) {
    if (!Arena::Get().Contains(pointer, bytes)) throw OutsideArena("commit", pointer, bytes);
    const auto generation = commitGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    const auto created = WindowsMappings::Get().Commit(pointer, bytes, protection, granule, Arena::Get().WriteWatched());
    GuestWtFaultForget_nid_no_patch(reinterpret_cast<std::uintptr_t>(pointer), bytes);
    if (const auto callback = privateMappingObserver.load(std::memory_order_acquire)) {
        for (const auto& [address, size] : created) callback(address, size, generation);
    }
    noteAccess(pointer, bytes);
}

void GuestArenaReset_nid_postfix(void* pointer, std::size_t bytes) {
    if (!Arena::Get().Contains(pointer, bytes)) throw OutsideArena("reset", pointer, bytes);
    WindowsMappings::Get().Reset(pointer, bytes);
    GuestWtFaultForget_nid_no_patch(reinterpret_cast<std::uintptr_t>(pointer), bytes);
}

void GuestArenaMap_nid_postfix(void* pointer, std::size_t bytes, void* section, std::uint64_t offset, std::uint32_t protection) {
    if (!Arena::Get().Contains(pointer, bytes)) throw OutsideArena("shared mapping", pointer, bytes);
    WindowsMappings::Get().Map(pointer, bytes, section, offset, protection, Arena::Get().WriteWatched(), false);
    GuestWtFaultForget_nid_no_patch(reinterpret_cast<std::uintptr_t>(pointer), bytes);
    noteAccess(pointer, bytes);
}

void GuestArenaMapZero_nid_postfix(void* pointer, std::size_t bytes) {
    if (!Arena::Get().Contains(pointer, bytes)) throw OutsideArena("zero page mapping", pointer, bytes);
    const auto generation = commitGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    WindowsMappings::Get().MapZero(pointer, bytes);
    GuestWtFaultForget_nid_no_patch(reinterpret_cast<std::uintptr_t>(pointer), bytes);
    if (const auto callback = privateMappingObserver.load(std::memory_order_acquire)) callback(reinterpret_cast<std::uintptr_t>(pointer), bytes, generation);
    noteAccess(pointer, bytes);
}

void GuestArenaMapDirect_nid_postfix(void* pointer, std::size_t bytes, void* section, std::uint64_t offset, std::uint32_t protection) {
    if (!Arena::Get().Contains(pointer, bytes)) throw OutsideArena("direct memory mapping", pointer, bytes);
    const auto generation = commitGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::vector<std::pair<std::uintptr_t, std::size_t>> created;
    std::exception_ptr failure;
    try {
        WindowsMappings::Get().Map(pointer, bytes, section, offset, protection, Arena::Get().WriteWatched(), true, PAGE_EXECUTE_READWRITE, SIZE_MAX, &created);
    } catch (...) {
        failure = std::current_exception();
    }
    GuestWtFaultForget_nid_no_patch(reinterpret_cast<std::uintptr_t>(pointer), bytes);
    try {
        if (const auto callback = privateMappingObserver.load(std::memory_order_acquire)) {
            for (const auto& [address, size] : created) callback(address, size, generation);
        }
    } catch (...) {
        if (failure != nullptr) std::rethrow_exception(failure);
        throw;
    }
    if (failure != nullptr) std::rethrow_exception(failure);
    noteAccess(pointer, bytes);
}

void GuestArenaMapFile_nid_postfix(void* pointer, std::size_t bytes, void* section, std::uint64_t offset, std::uint32_t protection, std::uint32_t viewProtection, std::size_t sectionBytes) {
    if (!Arena::Get().Contains(pointer, bytes)) throw OutsideArena("file mapping", pointer, bytes);
    WindowsMappings::Get().Map(pointer, bytes, section, offset, protection, Arena::Get().WriteWatched(), false, viewProtection, sectionBytes);
    GuestWtFaultForget_nid_no_patch(reinterpret_cast<std::uintptr_t>(pointer), bytes);
}

void* GuestArenaMapAlias_nid_postfix(std::uintptr_t address, std::size_t bytes) {
    return WindowsMappings::Get().MapAlias(address, bytes);
}

void* GuestArenaMapAliasForImport_nid_no_patch(std::uintptr_t address, std::size_t bytes, bool* boundsRefused) {
    return WindowsMappings::Get().MapAlias(address, bytes, boundsRefused);
}

void GuestArenaUnmapAlias_nid_postfix(void* alias) {
    WindowsMappings::Get().UnmapAlias(alias);
}

bool GuestArenaResidentSpan_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uintptr_t* start, std::uintptr_t* end) {
    return WindowsMappings::Get().ResidentSpan(address, bytes, start, end);
}

void GuestArenaCompareWhenShared_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    WindowsMappings::Get().CompareWhenShared(address, bytes);
}

void GuestArenaCompareResidentWhenShared_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    WindowsMappings::Get().CompareWhenShared(address, bytes, true);
}

void GuestArenaComparedPages_nid_no_patch(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count) {
    WindowsMappings::Get().ComparedPages(address, bytes, pages, count, false);
}

void GuestArenaTakeUnreportedPages_nid_no_patch(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count) {
    WindowsMappings::Get().ComparedPages(address, bytes, pages, count, true);
}

void GuestArenaForgetSection_nid_postfix(void* section) {
    WindowsMappings::Get().ForgetSection(section);
}

bool GuestArenaForgetReusableSection_nid_no_patch(void* section) {
    return WindowsMappings::Get().ForgetSection(section);
}
#endif

#if defined(__linux__) || defined(__APPLE__)
namespace {
int PosixProtection(std::uint32_t windowsProtection) {
    switch (windowsProtection) {
    case 0x01: return PROT_NONE;
    case 0x02: return PROT_READ;
    case 0x04: return PROT_READ | PROT_WRITE;
    case 0x08: return PROT_READ | PROT_WRITE;
    case 0x10: return PROT_EXEC;
    case 0x20: return PROT_READ | PROT_EXEC;
    case 0x40: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: throw std::runtime_error("guest arena: unmapped protection value");
    }
}

void* PlaceAt(void* pointer, std::size_t bytes, std::uint32_t protection) {
    return mmap(pointer, bytes, PosixProtection(protection), MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
}
}
#endif

#if defined(__linux__) || defined(__APPLE__)
void GuestArenaSetProtection_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uint32_t protection) {
    if (!Arena::Get().Contains(reinterpret_cast<void*>(address), bytes)) throw OutsideArena("set protection", reinterpret_cast<void*>(address), bytes);
#ifdef __APPLE__
    if (const int error = GuestWriteWatch::GuestWriteWatchProtect_nid_postfix(reinterpret_cast<void*>(address), bytes, PosixProtection(protection)); error != 0)
        throw std::system_error(error, std::generic_category(), "guest arena protection failed");
#else
    if (mprotect(reinterpret_cast<void*>(address), bytes, PosixProtection(protection)) != 0)
        throw std::system_error(errno, std::generic_category(), "guest arena protection failed");
#endif
    noteAccess(reinterpret_cast<const void*>(address), bytes);
}

void GuestArenaCommit_nid_postfix(void* pointer, std::size_t bytes, std::uint32_t protection, std::size_t granule) {
    (void)granule;
    if (!Arena::Get().Contains(pointer, bytes)) throw OutsideArena("commit", pointer, bytes);
    const bool marked = Arena::Get().MarkUsedForCommit(pointer, bytes);
    try {
        if (PlaceAt(pointer, bytes, protection) == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "guest arena commit failed");
    } catch (...) {
        if (marked) Arena::Get().Release(pointer, bytes);
        throw;
    }
    noteAccess(pointer, bytes);
}

void GuestArenaReset_nid_postfix(void* pointer, std::size_t bytes) {
    if (!Arena::Get().Contains(pointer, bytes)) throw OutsideArena("reset", pointer, bytes);
    if (PlaceAt(pointer, bytes, 0x01) == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "guest arena reset failed");
}
#elif defined(__EMSCRIPTEN__)
void GuestArenaSetProtection_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uint32_t protection) {
    static_cast<void>(protection);
    if (!Arena::Get().Contains(reinterpret_cast<void*>(address), bytes)) throw OutsideArena("set protection", reinterpret_cast<void*>(address), bytes);
}

void GuestArenaCommit_nid_postfix(void* pointer, std::size_t bytes, std::uint32_t protection, std::size_t granule) {
    static_cast<void>(protection);
    static_cast<void>(granule);
    if (!Arena::Get().Contains(pointer, bytes)) throw OutsideArena("commit", pointer, bytes);
    Arena::Get().MarkUsedForCommit(pointer, bytes);
    Arena::Get().ZeroDirty(pointer, bytes);
}

void GuestArenaReset_nid_postfix(void* pointer, std::size_t bytes) {
    if (!Arena::Get().Contains(pointer, bytes)) throw OutsideArena("reset", pointer, bytes);
    Arena::Get().NoteDirty(pointer, bytes);
}
#endif

bool GuestArenaArmOwned_nid_no_patch(std::uintptr_t address, std::size_t bytes, std::uint64_t* words, std::size_t count, unsigned flags) {
#ifdef _WIN32
    if (bytes == 0 || bytes > std::numeric_limits<std::uintptr_t>::max() - address) return false;
    const auto first = address & ~std::uintptr_t{65535};
    const auto last = (address + bytes - 1) | std::uintptr_t{65535};
    if (last == std::numeric_limits<std::uintptr_t>::max() || !Arena::Get().Contains(reinterpret_cast<void*>(first), last + 1 - first) || Arena::Get().OverlapsHostRegion(first, last + 1 - first)) return false;
    return WindowsMappings::Get().ArmOwned(address, bytes, words, count, flags);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(words);
    static_cast<void>(count);
    static_cast<void>(flags);
    return false;
#endif
}

bool GuestArenaOwnedUnchanged_nid_no_patch(std::uintptr_t address, std::size_t bytes, const std::uint64_t* words, std::size_t count) {
#ifdef _WIN32
    return WindowsMappings::Get().OwnedUnchanged(address, bytes, words, count);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(words);
    static_cast<void>(count);
    return false;
#endif
}

bool GuestArenaWriteWatched_nid_postfix() {
    return Arena::Get().WriteWatched();
}

void GuestArenaSetHostWriteHook_nid_no_patch(void (*hook)(std::uintptr_t, std::size_t)) {
    hostWriteHook.store(hook, std::memory_order_release);
}

void GuestArenaSetAccessObserver_nid_postfix(void (*callback)(std::uintptr_t address, std::size_t bytes)) {
    accessObserver.store(callback, std::memory_order_release);
}

void GuestArenaSetHostReadHook_nid_no_patch(void (*hook)(std::uintptr_t, std::size_t)) {
    hostReadHook.store(hook, std::memory_order_release);
}

void GuestArenaPrepareHostRead_nid_no_patch(const void* pointer, std::size_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (address == 0 || bytes == 0 || bytes > std::numeric_limits<std::uintptr_t>::max() - address) return;
    if (const auto hook = hostReadHook.load(std::memory_order_acquire)) hook(address, bytes);
}

void GuestArenaSetHostWriteObserver_nid_no_patch(void (*observer)(std::uintptr_t address, std::size_t bytes, bool end)) {
    hostWriteObserver.store(observer, std::memory_order_release);
}

bool GuestArenaBeginHostWrite_nid_postfix(void* pointer, std::size_t bytes) {
    if (const auto hook = hostWriteHook.load(std::memory_order_acquire); hook != nullptr && bytes != 0) hook(reinterpret_cast<std::uintptr_t>(pointer), bytes);
    noteHostWrite(pointer, bytes, false);
#ifdef _WIN32
    // A kernel-mode write fails on a page the write tracker protected instead of faulting: the range
    // stays open until EndHostWrite (APS5_ARCHB_WT_FAULT).
    GuestWtFaultHold_nid_no_patch(reinterpret_cast<std::uintptr_t>(pointer), bytes);
    return WindowsMappings::Get().BeginHostWrite(reinterpret_cast<std::uintptr_t>(pointer), bytes);
#else
    GuestWriteWatch::GuestWriteWatchBeginHostWrite_nid_postfix(pointer, bytes);
    return true;
#endif
}

#ifdef _WIN32
bool GuestArenaBeginHostImport_nid_no_patch(void* pointer, std::size_t bytes) {
    // A host-pointer import probes and locks the pages: they stay open for its duration (APS5_ARCHB_WT_FAULT).
    GuestWtFaultHold_nid_no_patch(reinterpret_cast<std::uintptr_t>(pointer), bytes);
    return WindowsMappings::Get().BeginHostWrite(reinterpret_cast<std::uintptr_t>(pointer), bytes, false);
}

void GuestArenaEndHostImport_nid_no_patch(void* pointer, std::size_t bytes) {
    WindowsMappings::Get().EndHostWrite(reinterpret_cast<std::uintptr_t>(pointer), bytes, false);
    GuestWtFaultRelease_nid_no_patch(reinterpret_cast<std::uintptr_t>(pointer), bytes);
}
#endif

void GuestArenaEndHostWrite_nid_postfix(void* pointer, std::size_t bytes) {
#ifdef _WIN32
    WindowsMappings::Get().EndHostWrite(reinterpret_cast<std::uintptr_t>(pointer), bytes);
    GuestWtFaultRelease_nid_no_patch(reinterpret_cast<std::uintptr_t>(pointer), bytes);
#else
    GuestWriteWatch::GuestWriteWatchEndHostWrite_nid_postfix(pointer, bytes);
#endif
    noteHostWrite(pointer, bytes, true);
}

#ifndef _WIN32
namespace {

std::atomic<SharedBackingResolver> sharedBackingResolver{nullptr};
std::atomic<SharedBackingWriter> sharedBackingWriter{nullptr};
std::atomic<SharedBackingRunsResolver> sharedBackingRunsResolver{nullptr};

}

void GuestArenaSetSharedBacking_nid_postfix(SharedBackingResolver resolver) {
    sharedBackingResolver.store(resolver, std::memory_order_release);
}

void GuestArenaSetSharedBackingWriter_nid_no_patch(SharedBackingWriter writer) {
    sharedBackingWriter.store(writer, std::memory_order_release);
}

bool GuestArenaWriteSharedBacking_nid_no_patch(std::uintptr_t address, const void* source, std::size_t bytes) {
    const auto writer = sharedBackingWriter.load(std::memory_order_acquire);
    return writer != nullptr && writer(address, source, bytes);
}

bool GuestArenaSharedBacking_nid_postfix(std::uintptr_t address, std::size_t bytes, int* file, std::uint64_t* offset) {
    const auto resolver = sharedBackingResolver.load(std::memory_order_acquire);
    return resolver != nullptr && resolver(address, bytes, file, offset);
}

void GuestArenaSetSharedBackingRuns_nid_postfix(SharedBackingRunsResolver resolver) {
    sharedBackingRunsResolver.store(resolver, std::memory_order_release);
}

bool GuestArenaSharedBackingRuns_nid_postfix(std::uintptr_t address, std::size_t bytes, int* file, SharedBackingRun* runs, std::size_t capacity, std::size_t* count) {
    const auto resolver = sharedBackingRunsResolver.load(std::memory_order_acquire);
    return resolver != nullptr && resolver(address, bytes, file, runs, capacity, count);
}

bool GuestArenaOverlapsHostRegion_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    return Arena::Get().OverlapsHostRegion(address, bytes);
}

void GuestArenaReleaseHostRegion_nid_no_patch(const void* pointer, std::size_t bytes) {
    Arena::Get().ReleaseHostRegion(pointer, bytes);
}
#endif

}
