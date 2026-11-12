#include "prx/libSceAgcDriver/Execution/include/HostAllocator.hpp"

#ifdef _WIN32

#include "prx/libc/include/HostThreadSlot.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <malloc.h>
#include <new>
#include <windows.h>

namespace {

constexpr std::size_t Granule = 16;
constexpr std::size_t LargestCached = 1024;
constexpr std::size_t Classes = LargestCached / Granule;
constexpr std::size_t SlabBytes = 64 * 1024;
#ifndef APS5_HOST_SLAB_COUNT
#define APS5_HOST_SLAB_COUNT 65536
#endif
constexpr std::size_t SlabCount = APS5_HOST_SLAB_COUNT;
constexpr std::size_t ArenaBytes = SlabBytes * SlabCount;

constexpr std::uint16_t Depth(std::size_t sizeClass) {
    return sizeClass < 256 / Granule ? 64 : 16;
}

struct Cache {
    void* heads[Classes];
    std::uint16_t counts[Classes];
    void* slabHeads[Classes];
    std::uint16_t slabCounts[Classes];
};

constexpr std::uint16_t KeptLists = 8;

struct Depot {
    SRWLOCK lock = SRWLOCK_INIT;
    void* lists = nullptr;
    std::uint16_t count = 0;
};

struct SlabDepot {
    SRWLOCK lock = SRWLOCK_INIT;
    void* blocks = nullptr;
};

Depot depots[Classes];
SlabDepot slabDepots[Classes];
std::array<std::atomic<std::uint8_t>, SlabCount> slabClasses{};
std::atomic<std::byte*> arena{nullptr};
std::atomic<std::size_t> nextSlab{0};
std::atomic<std::uintptr_t> driverFirst{0};
std::atomic<std::uintptr_t> driverLast{0};
std::atomic<std::uintptr_t> executableFirst{0};
std::atomic<std::uintptr_t> executableLast{0};
SRWLOCK arenaLock = SRWLOCK_INIT;
SRWLOCK runtimeLock = SRWLOCK_INIT;
AgcDriver::HostAllocator::Counters counters;

using FreeFunction = void(__cdecl*)(void*);
using ReallocFunction = void*(__cdecl*)(void*, std::size_t);
std::atomic<FreeFunction> runtimeFree{nullptr};
std::atomic<ReallocFunction> runtimeRealloc{nullptr};
std::atomic<bool> runtimePatched{false};

bool ReleaseSlab(void* block) noexcept;
std::size_t SlabCapacity(const void* block) noexcept;
void __cdecl RuntimeFree(void* block);
void* __cdecl RuntimeRealloc(void* block, std::size_t bytes);

struct ActiveSlot {};
struct ScopeSlot {};

struct BlockHeader {
    const void* owner;
    std::uintptr_t reserved;
};

static_assert(sizeof(BlockHeader) == Granule);

const void* ThreadToken() noexcept {
    const void* block;
    __asm__("movq %%gs:0x30, %0" : "=r"(block));
    return block;
}

bool KeepList(std::size_t sizeClass, void* list) noexcept {
    auto& depot = depots[sizeClass];
    AcquireSRWLockExclusive(&depot.lock);
    const bool kept = depot.count < KeptLists;
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

void PushSlabList(std::size_t sizeClass, void* list) noexcept {
    if (list == nullptr) return;
    auto& depot = slabDepots[sizeClass];
    AcquireSRWLockExclusive(&depot.lock);
    void* tail = list;
    while (*static_cast<void**>(tail) != nullptr) tail = *static_cast<void**>(tail);
    *static_cast<void**>(tail) = depot.blocks;
    depot.blocks = list;
    ReleaseSRWLockExclusive(&depot.lock);
}

std::byte* EnsureArena() noexcept {
    auto* base = arena.load(std::memory_order_acquire);
    if (base != nullptr) return base;
    AcquireSRWLockExclusive(&arenaLock);
    base = arena.load(std::memory_order_relaxed);
    if (base == nullptr) {
        base = static_cast<std::byte*>(VirtualAlloc(nullptr, ArenaBytes, MEM_RESERVE, PAGE_NOACCESS));
        if (base != nullptr) arena.store(base, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&arenaLock);
    return base;
}

void EnsureDriverRange() noexcept {
    if (driverFirst.load(std::memory_order_acquire) != 0) return;
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<const char*>(&EnsureDriverRange), &module)) return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::byte*>(module) + dos->e_lfanew);
    const auto first = reinterpret_cast<std::uintptr_t>(module);
    driverLast.store(first + nt->OptionalHeader.SizeOfImage, std::memory_order_relaxed);
    driverFirst.store(first, std::memory_order_release);
    module = GetModuleHandleA(nullptr);
    if (module == nullptr) return;
    dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::byte*>(module) + dos->e_lfanew);
    const auto executable = reinterpret_cast<std::uintptr_t>(module);
    executableLast.store(executable + nt->OptionalHeader.SizeOfImage, std::memory_order_relaxed);
    executableFirst.store(executable, std::memory_order_release);
}

bool DriverSite(const void* site) noexcept {
    const auto first = driverFirst.load(std::memory_order_acquire);
    const auto address = reinterpret_cast<std::uintptr_t>(site);
    const auto executable = executableFirst.load(std::memory_order_acquire);
    return (address >= first && address < driverLast.load(std::memory_order_relaxed)) || (address >= executable && address < executableLast.load(std::memory_order_relaxed));
}

void PatchRuntimeHeap() noexcept {
    if (runtimePatched.load(std::memory_order_acquire)) return;
    AcquireSRWLockExclusive(&runtimeLock);
    if (runtimePatched.load(std::memory_order_relaxed)) {
        ReleaseSRWLockExclusive(&runtimeLock);
        return;
    }
    auto* module = reinterpret_cast<std::byte*>(GetModuleHandleA("libstdc++-6.dll"));
    if (module == nullptr) {
        ReleaseSRWLockExclusive(&runtimeLock);
        return;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(module + directory.VirtualAddress);
    bool freePatched = false;
    for (; descriptor->Name != 0; ++descriptor) {
        if (descriptor->OriginalFirstThunk == 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(module + descriptor->OriginalFirstThunk);
        auto* thunks = reinterpret_cast<IMAGE_THUNK_DATA*>(module + descriptor->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++thunks) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* entry = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(module + names->u1.AddressOfData);
            void* replacement = nullptr;
            if (std::strcmp(reinterpret_cast<const char*>(entry->Name), "free") == 0) {
                runtimeFree.store(reinterpret_cast<FreeFunction>(thunks->u1.Function), std::memory_order_release);
                replacement = reinterpret_cast<void*>(&RuntimeFree);
            } else if (std::strcmp(reinterpret_cast<const char*>(entry->Name), "realloc") == 0) {
                runtimeRealloc.store(reinterpret_cast<ReallocFunction>(thunks->u1.Function), std::memory_order_release);
                replacement = reinterpret_cast<void*>(&RuntimeRealloc);
            }
            if (replacement == nullptr) continue;
            DWORD protection;
            if (!VirtualProtect(&thunks->u1.Function, sizeof(thunks->u1.Function), PAGE_READWRITE, &protection)) continue;
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&thunks->u1.Function), replacement);
            if (replacement == reinterpret_cast<void*>(&RuntimeFree)) freePatched = true;
            DWORD restored;
            VirtualProtect(&thunks->u1.Function, sizeof(thunks->u1.Function), protection, &restored);
        }
    }
    runtimePatched.store(freePatched, std::memory_order_release);
    ReleaseSRWLockExclusive(&runtimeLock);
}

bool Provision(std::size_t sizeClass) noexcept {
    auto* base = EnsureArena();
    if (base == nullptr) return false;
    auto index = nextSlab.load(std::memory_order_relaxed);
    while (index < SlabCount && !nextSlab.compare_exchange_weak(index, index + 1, std::memory_order_relaxed)) {}
    if (index >= SlabCount) return false;
    auto* slab = static_cast<std::byte*>(VirtualAlloc(base + index * SlabBytes, SlabBytes, MEM_COMMIT, PAGE_READWRITE));
    if (slab == nullptr) return false;
    slabClasses[index].store(static_cast<std::uint8_t>(sizeClass + 1), std::memory_order_release);
    const auto stride = sizeof(BlockHeader) + (sizeClass + 1) * Granule;
    void* list = nullptr;
    for (std::size_t offset = 0; offset + stride <= SlabBytes; offset += stride) {
        auto* block = slab + offset + sizeof(BlockHeader);
        *reinterpret_cast<void**>(block) = list;
        list = block;
    }
    auto& depot = slabDepots[sizeClass];
    void* tail = list;
    while (*static_cast<void**>(tail) != nullptr) tail = *static_cast<void**>(tail);
    *static_cast<void**>(tail) = depot.blocks;
    depot.blocks = list;
    return true;
}

bool RefillSlab(Cache& cache, std::size_t sizeClass) noexcept {
    auto& depot = slabDepots[sizeClass];
    AcquireSRWLockExclusive(&depot.lock);
    if (depot.blocks == nullptr && !Provision(sizeClass)) {
        ReleaseSRWLockExclusive(&depot.lock);
        return false;
    }
    const auto wanted = Depth(sizeClass);
    while (depot.blocks != nullptr && cache.slabCounts[sizeClass] < wanted) {
        void* block = depot.blocks;
        depot.blocks = *static_cast<void**>(block);
        *static_cast<void**>(block) = cache.slabHeads[sizeClass];
        cache.slabHeads[sizeClass] = block;
        ++cache.slabCounts[sizeClass];
    }
    ReleaseSRWLockExclusive(&depot.lock);
    return cache.slabHeads[sizeClass] != nullptr;
}

Cache* const Closed = reinterpret_cast<Cache*>(std::uintptr_t{1});

constexpr unsigned long TlsSlotsOffset = 0x1480;
constexpr unsigned long TlsExpansionSlotsOffset = 0x1780;
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
        PushSlabList(sizeClass, cache->slabHeads[sizeClass]);
        cache->slabHeads[sizeClass] = nullptr;
        cache->slabCounts[sizeClass] = 0;
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
    auto* expansion = reinterpret_cast<Cache**>(__readgsqword(TlsExpansionSlotsOffset));
    return expansion != nullptr ? expansion[slot - DirectSlots] : nullptr;
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

void* TrySlabAllocate(std::size_t bytes) noexcept {
    if (bytes == 0) bytes = 1;
    if (bytes > LargestCached) return nullptr;
    const auto sizeClass = (bytes - 1) / Granule;
    auto* cache = CurrentCache();
    if (cache == nullptr) return nullptr;
    if (cache->slabHeads[sizeClass] == nullptr && !RefillSlab(*cache, sizeClass)) {
        counters.fallbackExhausted.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    void* block = cache->slabHeads[sizeClass];
    cache->slabHeads[sizeClass] = *static_cast<void**>(block);
    --cache->slabCounts[sizeClass];
    reinterpret_cast<BlockHeader*>(block)[-1].owner = ThreadToken();
    counters.served.fetch_add(1, std::memory_order_relaxed);
    return block;
}

void* TryAllocate(std::size_t bytes, void* site) noexcept {
    if (arena.load(std::memory_order_relaxed) != nullptr && AgcDriver::HostAllocator::Active() && DriverSite(site)) {
        if (void* block = TrySlabAllocate(bytes)) return block;
    }
    if (bytes == 0) bytes = 1;
    if (bytes <= LargestCached) {
        const auto sizeClass = (bytes - 1) / Granule;
        if (auto* cache = CurrentCache()) {
            if (cache->heads[sizeClass] == nullptr) {
                if (void* list = TakeList(sizeClass)) {
                    cache->heads[sizeClass] = list;
                    cache->counts[sizeClass] = Depth(sizeClass);
                }
            }
            if (void* block = cache->heads[sizeClass]) {
                cache->heads[sizeClass] = *static_cast<void**>(block);
                --cache->counts[sizeClass];
                return block;
            }
        }
        return std::malloc((sizeClass + 1) * Granule);
    }
    return std::malloc(bytes);
}

struct Site {
    void* address = nullptr;
    std::uint64_t calls = 0;
    std::uint64_t bytes = 0;
};

SRWLOCK sitesLock = SRWLOCK_INIT;
std::array<Site, 4096> sites{};

void ReportSites() {
    std::array<Site, 4096> copy;
    AcquireSRWLockShared(&sitesLock);
    copy = sites;
    ReleaseSRWLockShared(&sitesLock);
    std::sort(copy.begin(), copy.end(), [](const Site& left, const Site& right) { return left.calls > right.calls; });
    std::fprintf(stderr, "[host-alloc-sites] top allocations\n");
    for (std::size_t i = 0; i < 120 && copy[i].calls != 0; ++i) {
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<const char*>(copy[i].address), &module);
        const auto offset = reinterpret_cast<std::uintptr_t>(copy[i].address) - reinterpret_cast<std::uintptr_t>(module);
        char path[MAX_PATH]{};
        GetModuleFileNameA(module, path, sizeof(path));
        std::fprintf(stderr, "[host-alloc-sites] rank=%zu address=%p module=%s module_offset=0x%llx calls=%llu bytes=%llu\n", i + 1, copy[i].address, path, static_cast<unsigned long long>(offset), static_cast<unsigned long long>(copy[i].calls), static_cast<unsigned long long>(copy[i].bytes));
    }
}

void RecordSite(void* address, std::size_t bytes) noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_HOST_ALLOC_SITES");
        if (value == nullptr || value[0] != '1' || value[1] != '\0') return false;
        std::atexit(ReportSites);
        return true;
    }();
    if (!enabled || !AgcDriver::HostAllocator::InsideScope()) return;
    AcquireSRWLockExclusive(&sitesLock);
    for (std::size_t probe = 0, slot = (reinterpret_cast<std::uintptr_t>(address) >> 2) % sites.size(); probe < 64; ++probe, slot = (slot + 1) % sites.size()) {
        auto& site = sites[slot];
        if (site.address == nullptr) site.address = address;
        if (site.address != address) continue;
        ++site.calls;
        site.bytes += bytes;
        break;
    }
    ReleaseSRWLockExclusive(&sitesLock);
    static std::atomic<std::uint64_t> recorded{0};
    if ((recorded.fetch_add(1, std::memory_order_relaxed) + 1) % 4000000u == 0) ReportSites();
}

void* Allocate(std::size_t bytes, void* site) {
    for (;;) {
        if (void* block = TryAllocate(bytes, site)) {
            RecordSite(site, bytes);
            return block;
        }
        const auto handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}

bool ReleaseSlab(void* block) noexcept {
    auto* base = arena.load(std::memory_order_acquire);
    if (base == nullptr) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(block);
    const auto first = reinterpret_cast<std::uintptr_t>(base);
    if (address < first || address >= first + ArenaBytes) return false;
    const auto index = (address - first) / SlabBytes;
    const auto encoded = slabClasses[index].load(std::memory_order_acquire);
    if (encoded == 0) return false;
    const auto sizeClass = static_cast<std::size_t>(encoded - 1);
    counters.releaseOwned.fetch_add(1, std::memory_order_relaxed);
    const bool remote = reinterpret_cast<BlockHeader*>(block)[-1].owner != ThreadToken();
    if (remote) counters.remoteFrees.fetch_add(1, std::memory_order_relaxed);
    auto* cache = remote ? nullptr : CurrentCache();
    if (cache == nullptr) {
        *static_cast<void**>(block) = nullptr;
        PushSlabList(sizeClass, block);
        return true;
    }
    if (cache->slabCounts[sizeClass] == Depth(sizeClass)) {
        PushSlabList(sizeClass, cache->slabHeads[sizeClass]);
        cache->slabHeads[sizeClass] = nullptr;
        cache->slabCounts[sizeClass] = 0;
    }
    *static_cast<void**>(block) = cache->slabHeads[sizeClass];
    cache->slabHeads[sizeClass] = block;
    ++cache->slabCounts[sizeClass];
    return true;
}

std::size_t SlabCapacity(const void* block) noexcept {
    auto* base = arena.load(std::memory_order_acquire);
    if (base == nullptr) return 0;
    const auto address = reinterpret_cast<std::uintptr_t>(block);
    const auto first = reinterpret_cast<std::uintptr_t>(base);
    if (address < first || address >= first + ArenaBytes) return 0;
    const auto encoded = slabClasses[(address - first) / SlabBytes].load(std::memory_order_acquire);
    return encoded == 0 ? 0 : static_cast<std::size_t>(encoded) * Granule;
}

void __cdecl RuntimeFree(void* block) {
    if (block == nullptr || ReleaseSlab(block)) return;
    runtimeFree.load(std::memory_order_acquire)(block);
}

void* __cdecl RuntimeRealloc(void* block, std::size_t bytes) {
    const auto capacity = SlabCapacity(block);
    if (capacity == 0) return runtimeRealloc.load(std::memory_order_acquire)(block, bytes);
    if (bytes == 0) {
        ReleaseSlab(block);
        return nullptr;
    }
    void* replacement = runtimeRealloc.load(std::memory_order_acquire)(nullptr, bytes);
    if (replacement == nullptr) return nullptr;
    std::memcpy(replacement, block, std::min(capacity, bytes));
    ReleaseSlab(block);
    return replacement;
}

void Release(void* block, std::size_t = 0) noexcept {
    if (block == nullptr) return;
    if (ReleaseSlab(block)) return;
    if (arena.load(std::memory_order_relaxed) != nullptr) counters.releaseForeign.fetch_add(1, std::memory_order_relaxed);
    if (auto* cache = CurrentCache()) {
        const auto usable = _msize(block);
        if (usable >= Granule && usable < LargestCached + Granule) {
            const auto sizeClass = usable / Granule - 1;
            if (cache->counts[sizeClass] == Depth(sizeClass) && KeepList(sizeClass, cache->heads[sizeClass])) {
                cache->heads[sizeClass] = nullptr;
                cache->counts[sizeClass] = 0;
            }
            if (cache->counts[sizeClass] < Depth(sizeClass)) {
                *static_cast<void**>(block) = cache->heads[sizeClass];
                cache->heads[sizeClass] = block;
                ++cache->counts[sizeClass];
                return;
            }
        }
    }
    std::free(block);
}

}

namespace AgcDriver::HostAllocator {

bool Active() noexcept { return HostThreadSlot<bool, ActiveSlot>::Get(); }
void SetActive(bool active) noexcept {
    if (active) {
        EnsureDriverRange();
        PatchRuntimeHeap();
        static_cast<void>(EnsureArena());
    }
    HostThreadSlot<bool, ActiveSlot>::Set(active);
}
bool InsideScope() noexcept { return HostThreadSlot<bool, ScopeSlot>::Get(); }
void SetInsideScope(bool inside) noexcept { HostThreadSlot<bool, ScopeSlot>::Set(inside); }
Counters& GetCounters() noexcept { return counters; }

bool Owns(const void* block) noexcept {
    auto* base = arena.load(std::memory_order_acquire);
    if (base == nullptr) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(block);
    const auto first = reinterpret_cast<std::uintptr_t>(base);
    return address >= first && address < first + ArenaBytes && slabClasses[(address - first) / SlabBytes].load(std::memory_order_acquire) != 0;
}

std::size_t Capacity() noexcept { return ArenaBytes; }

}

void* operator new(std::size_t bytes) { return Allocate(bytes, __builtin_return_address(0)); }
void* operator new[](std::size_t bytes) { return Allocate(bytes, __builtin_return_address(0)); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return Allocate(bytes, __builtin_return_address(0));
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return Allocate(bytes, __builtin_return_address(0));
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

#else

namespace AgcDriver::HostAllocator {

namespace {
thread_local bool active = false;
thread_local bool insideScope = false;
Counters counters;
}

bool Active() noexcept { return active; }
void SetActive(bool value) noexcept { active = value; }
bool InsideScope() noexcept { return insideScope; }
void SetInsideScope(bool value) noexcept { insideScope = value; }
Counters& GetCounters() noexcept { return counters; }
bool Owns(const void*) noexcept { return false; }
std::size_t Capacity() noexcept { return 0; }

}

#endif
