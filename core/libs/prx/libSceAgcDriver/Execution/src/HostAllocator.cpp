#ifdef _WIN32

#include "prx/libSceAgcDriver/Execution/include/HostAllocator.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <intrin.h>
#include <malloc.h>
#include <new>
#include <windows.h>

namespace {

using namespace AgcDriver::HostAllocator;

bool Flag(const char* name) noexcept {
    const auto* value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

std::atomic<signed char> census{-1};
std::atomic<signed char> depotFix{-1};

bool Resolve(std::atomic<signed char>& state, const char* name) noexcept {
    auto value = state.load(std::memory_order_relaxed);
    if (value < 0) {
        value = Flag(name) ? 1 : 0;
        state.store(value, std::memory_order_relaxed);
    }
    return value > 0;
}

bool Census() noexcept {
    return Resolve(census, "APS5_ARCHB_ALLOCATOR_CENSUS");
}

[[gnu::noinline]] void CountSlow(Event event, std::size_t bucket) noexcept {
    if (Census()) counters[static_cast<std::size_t>(event)][bucket].fetch_add(1, std::memory_order_relaxed);
}

inline void Count(Event event, std::size_t bucket = 0) noexcept {
    if (census.load(std::memory_order_relaxed) != 0) [[unlikely]] CountSlow(event, bucket);
}

struct CensusSummary {
    ~CensusSummary() {
        if (!Census()) return;
        for (std::size_t event = 0; event < std::size(Names); ++event) {
            const auto buckets = event < BucketedEvents ? Classes + 1 : 1;
            for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
                const auto value = counters[event][bucket].load(std::memory_order_relaxed);
                if (value == 0) continue;
                std::fprintf(stderr, "[host-allocator] %s bucket=%zu count=%llu\n", Names[event],
                    event < BucketedEvents ? (bucket + 1) * Granule : 0, static_cast<unsigned long long>(value));
            }
        }
    }
} censusSummary;

constexpr std::uint16_t Depth(std::size_t sizeClass) {
    return sizeClass < SmallClasses ? 64 : 16;
}

struct Cache {
    void* heads[Classes];
    std::uint16_t counts[Classes];
};

struct Depot {
    SRWLOCK lock = SRWLOCK_INIT;
    void* lists = nullptr;
    std::uint16_t count = 0;
};

Depot depots[Classes];

bool KeepList(std::size_t sizeClass, void* list) noexcept {
    auto& depot = depots[sizeClass];
    const auto capacity = AgcDriver::HostAllocator::DepotCapacity(sizeClass);
    AcquireSRWLockExclusive(&depot.lock);
    const bool kept = depot.count < capacity;
    if (kept) {
        static_cast<void**>(list)[1] = depot.lists;
        depot.lists = list;
        ++depot.count;
    }
    ReleaseSRWLockExclusive(&depot.lock);
    return kept;
}

void* TakeList(std::size_t sizeClass) noexcept {
    auto& depot = depots[sizeClass];
    AcquireSRWLockExclusive(&depot.lock);
    void* list = depot.lists;
    if (list != nullptr) {
        depot.lists = static_cast<void**>(list)[1];
        --depot.count;
    }
    ReleaseSRWLockExclusive(&depot.lock);
    return list;
}

Cache* const Closed = reinterpret_cast<Cache*>(std::uintptr_t{1});

constexpr unsigned long TlsSlotsOffset = 0x1480;
constexpr DWORD DirectSlots = 64;

constexpr DWORD Unset = TLS_OUT_OF_INDEXES;
constexpr DWORD Failed = TLS_OUT_OF_INDEXES - 1;
std::atomic<DWORD> tlsSlot{Unset};
std::atomic<DWORD> flsSlot{FLS_OUT_OF_INDEXES};

void Drain(Cache* cache) noexcept {
    for (std::size_t sizeClass = 0; sizeClass < Classes; ++sizeClass) {
        while (void* block = cache->heads[sizeClass]) {
            cache->heads[sizeClass] = *static_cast<void**>(block);
            std::free(block);
        }
        cache->counts[sizeClass] = 0;
    }
}

void WINAPI ThreadEnded(void* data) {
    auto* cache = static_cast<Cache*>(data);
    if (cache == nullptr || cache == Closed) return;
    const auto slot = tlsSlot.load(std::memory_order_acquire);
    if (slot < Failed) TlsSetValue(slot, Closed);
    Drain(cache);
    std::free(cache);
}

DWORD Slot() noexcept {
    auto slot = tlsSlot.load(std::memory_order_acquire);
    if (slot != Unset) return slot;
    const DWORD error = GetLastError();
    const DWORD fls = FlsAlloc(ThreadEnded);
    DWORD tls = fls == FLS_OUT_OF_INDEXES ? TLS_OUT_OF_INDEXES : TlsAlloc();
    if (tls == TLS_OUT_OF_INDEXES || tls >= Failed) {
        if (tls != TLS_OUT_OF_INDEXES) TlsFree(tls);
        if (fls != FLS_OUT_OF_INDEXES) FlsFree(fls);
        tls = Failed;
    }
    DWORD expected = Unset;
    if (tlsSlot.compare_exchange_strong(expected, tls, std::memory_order_acq_rel)) {
        if (tls != Failed) flsSlot.store(fls, std::memory_order_release);
        SetLastError(error);
        return tls;
    }
    if (tls != Failed) {
        TlsFree(tls);
        FlsFree(fls);
    }
    SetLastError(error);
    return expected;
}

Cache* SlotValue(DWORD slot) noexcept {
    if (slot < DirectSlots) return reinterpret_cast<Cache*>(__readgsqword(TlsSlotsOffset + slot * sizeof(void*)));
    const DWORD error = GetLastError();
    auto* value = static_cast<Cache*>(TlsGetValue(slot));
    SetLastError(error);
    return value;
}

Cache* CreateCache(DWORD slot) noexcept {
    const auto fls = flsSlot.load(std::memory_order_acquire);
    if (fls == FLS_OUT_OF_INDEXES) return nullptr;
    auto* cache = static_cast<Cache*>(std::calloc(1, sizeof(Cache)));
    if (cache == nullptr) return nullptr;
    const DWORD error = GetLastError();
    if (!FlsSetValue(fls, cache) || !TlsSetValue(slot, cache)) {
        FlsSetValue(fls, nullptr);
        std::free(cache);
        cache = nullptr;
    }
    SetLastError(error);
    return cache;
}

Cache* CurrentCache() noexcept {
    const auto slot = Slot();
    if (slot == Failed) return nullptr;
    auto* cache = SlotValue(slot);
    if (cache == Closed) return nullptr;
    return cache != nullptr ? cache : CreateCache(slot);
}

void* TryAllocate(std::size_t bytes) noexcept {
    if (bytes == 0) bytes = 1;
    if (bytes <= LargestCached) {
        const auto sizeClass = (bytes - 1) / Granule;
        if (auto* cache = CurrentCache()) {
            if (cache->heads[sizeClass] == nullptr) {
                if (void* list = TakeList(sizeClass)) {
                    Count(Event::DepotTake, sizeClass);
                    cache->heads[sizeClass] = list;
                    cache->counts[sizeClass] = Depth(sizeClass);
                }
            }
            if (void* block = cache->heads[sizeClass]) {
                Count(Event::CacheHit, sizeClass);
                cache->heads[sizeClass] = *static_cast<void**>(block);
                --cache->counts[sizeClass];
                return block;
            }
        }
        Count(Event::Malloc, sizeClass);
        return std::malloc((sizeClass + 1) * Granule);
    }
    Count(Event::Malloc, Classes);
    return std::malloc(bytes);
}

void* Allocate(std::size_t bytes) {
    for (;;) {
        if (void* block = TryAllocate(bytes)) return block;
        const auto handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}

void Release(void* block, std::size_t bytes = 0) noexcept {
    if (block == nullptr) return;
    if (bytes == 0) Count(Event::UnsizedRelease);
    if (auto* cache = CurrentCache()) {
        const bool known = bytes != 0 && bytes % Granule == 0;
        if (!known) Count(Event::SizeQuery);
        const auto usable = known ? bytes : _msize(block);
        if (usable >= Granule && usable < LargestCached + Granule) {
            const auto sizeClass = usable / Granule - 1;
            if (cache->counts[sizeClass] == Depth(sizeClass) && KeepList(sizeClass, cache->heads[sizeClass])) {
                Count(Event::DepotKeep, sizeClass);
                cache->heads[sizeClass] = nullptr;
                cache->counts[sizeClass] = 0;
            }
            if (cache->counts[sizeClass] < Depth(sizeClass)) {
                *static_cast<void**>(block) = cache->heads[sizeClass];
                cache->heads[sizeClass] = block;
                ++cache->counts[sizeClass];
                return;
            }
            Count(Event::DepotFullFree, sizeClass);
        }
    } else {
        Count(Event::NoCacheFree);
    }
    std::free(block);
}

}

namespace AgcDriver::HostAllocator {

std::size_t DepotCapacity(std::size_t sizeClass) noexcept {
    return sizeClass < SmallClasses && Resolve(depotFix, "APS5_ARCHB_FIX_ALLOC_DEPOT") ? KeptSmallListsWithFix : KeptLists;
}

std::size_t DepotLists(std::size_t sizeClass) noexcept {
    auto& depot = depots[sizeClass];
    AcquireSRWLockShared(&depot.lock);
    const std::size_t count = depot.count;
    ReleaseSRWLockShared(&depot.lock);
    return count;
}

}

void* operator new(std::size_t bytes) { return Allocate(bytes); }
void* operator new[](std::size_t bytes) { return Allocate(bytes); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return Allocate(bytes);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return Allocate(bytes);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* block) noexcept { Release(block); }
void operator delete[](void* block) noexcept { Release(block); }
void operator delete(void* block, std::size_t bytes) noexcept { Release(block, bytes); }
void operator delete[](void* block, std::size_t bytes) noexcept { Release(block, bytes); }
void operator delete(void* block, const std::nothrow_t&) noexcept { Release(block); }
void operator delete[](void* block, const std::nothrow_t&) noexcept { Release(block); }

#endif
