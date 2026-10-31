#ifdef _WIN32

#include "prx/libSceAgcDriver/Execution/include/HostAllocator.hpp"
#include "prx/libc/include/specifics/itanium/UnwindAbi.hpp"
#include <algorithm>
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

// libc's DWARF unwinder: the driver is built without SEH unwind tables, so RtlCaptureStackBackTrace cannot walk it.
extern "C" {
_Unwind_Ptr APS5_VABI _Unwind_GetIP_nid_postfix(_Unwind_Context* context);
_Unwind_Reason_Code APS5_VABI _Unwind_Backtrace_nid_postfix(_Unwind_Trace_Fn trace, void* argument);
}

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
    // Allocation-site census state, only touched when APS5_ARCHB_ALLOC_SITE_CENSUS=1.
    std::uint32_t siteCountdown;
    std::uint32_t siteRandom;
    std::uint32_t siteThread;
    std::uint32_t siteSamples;
};

// Allocation-site census: see HostAllocator.hpp. Everything below runs only on sampled events; the table is
// VirtualAlloc'ed on the first sample and nothing here goes through operator new.
namespace Site {

constexpr std::size_t SampledClasses = SiteLargestSampled / Granule;
constexpr std::size_t Capacity = std::size_t{1} << 17;
constexpr std::size_t Probes = 64;
constexpr std::size_t MaxThreads = 1024;
constexpr std::size_t Parents = 2;
constexpr std::uint32_t Empty = 0, Claimed = 1, Ready = 2;
constexpr std::uint32_t DefaultWalkEvery = 4;
constexpr const char* KindNames[] = {"alloc_cached", "alloc_heap", "free_cached", "free_heap"};

struct Entry {
    std::atomic<std::uint32_t> state;
    std::uint32_t meta;  // kind | size class << 2 | thread row << 8
    std::uintptr_t frames[1 + Parents];  // caller, then parents (0 when the sample did not walk or the walk stopped)
    std::atomic<std::uint32_t> counts[SiteEpochs];
};

struct ThreadRow {
    std::atomic<DWORD> id;
    std::atomic<std::uint64_t> samples;
    char name[64];
};

std::atomic<signed char> enabled{-1};
std::atomic<Entry*> table{nullptr};
std::atomic<bool> tableFailed{false};
std::atomic<std::uint64_t> entries{0};
std::atomic<std::uint64_t> dropped{0};
std::atomic<std::uint64_t> walks{0};
std::atomic<std::uint64_t> noCache{0};
std::atomic<std::uint64_t> kinds[static_cast<std::size_t>(SiteKind::Count)]{};
std::atomic<std::uint32_t> walkEvery{~0u};
std::atomic<std::uint32_t> threadCount{0};
ThreadRow threads[MaxThreads];

bool On() noexcept {
    return Resolve(enabled, "APS5_ARCHB_ALLOC_SITE_CENSUS");
}

std::uint32_t WalkEvery() noexcept {
    auto value = walkEvery.load(std::memory_order_relaxed);
    if (value == ~0u) {
        value = DefaultWalkEvery;
        if (const auto* text = std::getenv("APS5_ARCHB_ALLOC_SITE_CENSUS_WALK_EVERY")) value = static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10));
        walkEvery.store(value, std::memory_order_relaxed);
    }
    return value;
}

Entry* Table() noexcept {
    if (auto* current = table.load(std::memory_order_acquire)) return current;
    if (tableFailed.load(std::memory_order_relaxed)) return nullptr;
    auto* fresh = static_cast<Entry*>(VirtualAlloc(nullptr, Capacity * sizeof(Entry), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (fresh == nullptr) {
        tableFailed.store(true, std::memory_order_relaxed);
        return nullptr;
    }
    Entry* expected = nullptr;
    if (table.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel)) return fresh;
    VirtualFree(fresh, 0, MEM_RELEASE);
    return expected;
}

using GetThreadDescriptionFunction = HRESULT(WINAPI*)(HANDLE, PWSTR*);

GetThreadDescriptionFunction Describer() noexcept {
    static const auto function = [] {
        GetThreadDescriptionFunction result = nullptr;
        if (const auto kernel = GetModuleHandleW(L"Kernel32.dll")) {
            const FARPROC address = GetProcAddress(kernel, "GetThreadDescription");
            static_assert(sizeof(result) == sizeof(address));
            std::memcpy(&result, &address, sizeof(result));
        }
        return result;
    }();
    return function;
}

void Describe(HANDLE thread, char (&name)[64]) noexcept {
    const auto describe = Describer();
    PWSTR wide = nullptr;
    if (describe == nullptr || FAILED(describe(thread, &wide)) || wide == nullptr) return;
    std::size_t length = 0;
    for (; wide[length] != L'\0' && length + 1 < sizeof(name); ++length) {
        const auto c = wide[length];
        name[length] = c > 0x20 && c < 0x7f ? static_cast<char>(c) : '_';
    }
    if (length != 0) name[length] = '\0';
    LocalFree(wide);
}

struct Walk {
    std::uintptr_t caller;
    std::uintptr_t* parents;
    unsigned depth;
    unsigned found;
};

_Unwind_Reason_Code APS5_VABI WalkFrame(_Unwind_Context* context, void* argument) {
    auto& walk = *static_cast<Walk*>(argument);
    const auto ip = _Unwind_GetIP_nid_postfix(context);
    if (walk.found == 0) {
        if (ip == walk.caller) walk.found = 1;
    } else {
        walk.parents[walk.found++ - 1] = ip;
        if (walk.found > Parents) return _URC_NORMAL_STOP;
    }
    return ++walk.depth >= 16 ? _URC_NORMAL_STOP : _URC_NO_REASON;
}

std::uint64_t Mix(std::uint64_t value) noexcept {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    return value ^ (value >> 33);
}

void Record(std::uint32_t meta, const std::uintptr_t (&frames)[1 + Parents]) noexcept {
    auto* const rows = Table();
    if (rows == nullptr) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto epoch = std::min<std::size_t>(siteFlips.load(std::memory_order_relaxed), SiteEpochs - 1);
    const auto hash = Mix(frames[0] ^ Mix(frames[1] + meta) ^ (frames[2] * 0x9e3779b97f4a7c15ull));
    for (std::size_t probe = 0; probe < Probes; ++probe) {
        auto& row = rows[(hash + probe) & (Capacity - 1)];
        auto state = row.state.load(std::memory_order_acquire);
        if (state == Empty) {
            std::uint32_t expected = Empty;
            if (row.state.compare_exchange_strong(expected, Claimed, std::memory_order_acquire)) {
                row.meta = meta;
                for (std::size_t i = 0; i <= Parents; ++i) row.frames[i] = frames[i];
                row.counts[epoch].fetch_add(1, std::memory_order_relaxed);
                row.state.store(Ready, std::memory_order_release);
                entries.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            state = expected;
        }
        // A row still being claimed is skipped; the dump may then hold the same key twice, which readers sum.
        if (state == Ready && row.meta == meta && row.frames[0] == frames[0] && row.frames[1] == frames[1] && row.frames[2] == frames[2]) {
            row.counts[epoch].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    dropped.fetch_add(1, std::memory_order_relaxed);
}

[[gnu::noinline]] void Sample(Cache* cache, SiteKind kind, std::size_t sizeClass, void* caller) noexcept {
    if (!On() || sizeClass >= SampledClasses) return;
    if (cache == nullptr) {
        noCache.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (cache->siteCountdown > 1) {
        --cache->siteCountdown;
        return;
    }
    // Random gaps (uniform in 1..2*mean-1) so loops with a fixed allocation pattern do not alias with the sampling.
    auto random = cache->siteRandom;
    if (random == 0) random = static_cast<std::uint32_t>(Mix(GetCurrentThreadId() + reinterpret_cast<std::uintptr_t>(cache))) | 1u;
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    cache->siteRandom = random;
    cache->siteCountdown = 1 + random % (2 * SiteMeanInterval - 1);
    const DWORD error = GetLastError();
    if (cache->siteThread == 0) {
        const auto row = threadCount.fetch_add(1, std::memory_order_relaxed);
        cache->siteThread = row < MaxThreads ? row + 1 : MaxThreads + 1;
        if (row < MaxThreads) threads[row].id.store(GetCurrentThreadId(), std::memory_order_release);
    }
    const auto thread = cache->siteThread - 1;
    const auto samples = ++cache->siteSamples;
    if (thread < MaxThreads) {
        threads[thread].samples.fetch_add(1, std::memory_order_relaxed);
        // Names are often set after a thread's first allocations, so they are refreshed now and then.
        if ((samples & (samples - 1)) == 0 || (samples & 4095) == 0) Describe(GetCurrentThread(), threads[thread].name);
    }
    std::uintptr_t frames[1 + Parents]{reinterpret_cast<std::uintptr_t>(caller)};
    const auto every = WalkEvery();
    if (every != 0 && samples % every == 0) {
        Walk walk{frames[0], frames + 1, 0, 0};
        _Unwind_Backtrace_nid_postfix(WalkFrame, &walk);
        walks.fetch_add(1, std::memory_order_relaxed);
    }
    kinds[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
    Record(static_cast<std::uint32_t>(kind) | static_cast<std::uint32_t>(sizeClass) << 2 | static_cast<std::uint32_t>(thread) << 8, frames);
    SetLastError(error);
}

inline void Note(Cache* cache, SiteKind kind, std::size_t sizeClass, void* caller) noexcept {
    if (enabled.load(std::memory_order_relaxed) != 0) [[unlikely]] Sample(cache, kind, sizeClass, caller);
}

struct Module {
    std::uintptr_t base;
    std::uintptr_t size;
    char path[MAX_PATH];
};

int ModuleOf(std::uintptr_t address, Module* modules, int& count, int capacity) noexcept {
    if (address == 0) return -1;
    for (int i = 0; i < count; ++i) {
        if (address >= modules[i].base && address - modules[i].base < modules[i].size) return i;
    }
    HMODULE handle = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &handle) || handle == nullptr) return -1;
    if (count == capacity) return -1;
    auto& module = modules[count];
    module.base = reinterpret_cast<std::uintptr_t>(handle);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module.base + dos->e_lfanew);
    module.size = nt->OptionalHeader.SizeOfImage;
    const auto length = GetModuleFileNameA(handle, module.path, sizeof(module.path));
    if (length == 0 || length >= sizeof(module.path)) std::snprintf(module.path, sizeof(module.path), "?");
    if (address - module.base >= module.size) return -1;
    return count++;
}

bool Write(const char* path) noexcept {
    auto* const rows = table.load(std::memory_order_acquire);
    std::FILE* out = std::fopen(path, "w");
    if (out == nullptr) return false;
    const auto threadRows = std::min<std::size_t>(threadCount.load(std::memory_order_relaxed), MaxThreads);
    std::fprintf(out, "# aps5 allocation-site census v1: counts are samples (about 1 in %u events of <= %zu bytes per thread); "
        "epoch e counts samples taken after e flips (the last epoch holds the rest)\n", SiteMeanInterval, SiteLargestSampled);
    std::fprintf(out, "# site <kind> <class bytes> <thread row> <caller> <module> <parent1> <module> <parent2> <module> <total> <epoch 0..%zu>\n", SiteEpochs - 1);
    std::fprintf(out, "meta mean_interval=%u largest=%zu epochs=%zu flips=%u capacity=%zu entries=%llu dropped=%llu walks=%llu walk_every=%u no_cache=%llu threads=%zu",
        SiteMeanInterval, SiteLargestSampled, SiteEpochs, siteFlips.load(std::memory_order_relaxed), Capacity,
        static_cast<unsigned long long>(entries.load(std::memory_order_relaxed)), static_cast<unsigned long long>(dropped.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(walks.load(std::memory_order_relaxed)), WalkEvery(), static_cast<unsigned long long>(noCache.load(std::memory_order_relaxed)), threadRows);
    for (std::size_t kind = 0; kind < std::size(KindNames); ++kind) std::fprintf(out, " %s=%llu", KindNames[kind], static_cast<unsigned long long>(kinds[kind].load(std::memory_order_relaxed)));
    std::fprintf(out, "\n");
    for (std::size_t row = 0; row < threadRows; ++row) {
        auto& thread = threads[row];
        const auto id = thread.id.load(std::memory_order_acquire);
        if (thread.name[0] == '\0') {
            if (const auto handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id)) {
                Describe(handle, thread.name);
                CloseHandle(handle);
            }
        }
        std::fprintf(out, "thread %zu %lu %llu %s\n", row, static_cast<unsigned long>(id), static_cast<unsigned long long>(thread.samples.load(std::memory_order_relaxed)), thread.name[0] != '\0' ? thread.name : "-");
    }
    static Module modules[256];
    int moduleCount = 0;
    if (rows != nullptr) {
        for (std::size_t index = 0; index < Capacity; ++index) {
            auto& row = rows[index];
            if (row.state.load(std::memory_order_acquire) != Ready) continue;
            int module[1 + Parents];
            for (std::size_t i = 0; i <= Parents; ++i) module[i] = ModuleOf(row.frames[i], modules, moduleCount, static_cast<int>(std::size(modules)));
            std::uint64_t total = 0;
            for (const auto& count : row.counts) total += count.load(std::memory_order_relaxed);
            std::fprintf(out, "site %s %zu %u", KindNames[row.meta & 3u], (((row.meta >> 2) & 63u) + 1) * Granule, row.meta >> 8);
            for (std::size_t i = 0; i <= Parents; ++i) std::fprintf(out, " %llx %d", static_cast<unsigned long long>(row.frames[i]), module[i]);
            std::fprintf(out, " %llu", static_cast<unsigned long long>(total));
            for (const auto& count : row.counts) std::fprintf(out, " %u", count.load(std::memory_order_relaxed));
            std::fprintf(out, "\n");
        }
    }
    for (int i = 0; i < moduleCount; ++i) std::fprintf(out, "module %d %llx %llx %s\n", i, static_cast<unsigned long long>(modules[i].base), static_cast<unsigned long long>(modules[i].size), modules[i].path);
    const bool written = std::ferror(out) == 0;
    return std::fclose(out) == 0 && written;
}

struct Summary {
    ~Summary() {
        if (!On()) return;
        char path[MAX_PATH];
        const auto* chosen = std::getenv("APS5_ARCHB_ALLOC_SITE_CENSUS_PATH");
        if (chosen == nullptr || chosen[0] == '\0') {
            const auto* directory = std::getenv("APS5_TRACE_DIR");
            if (directory != nullptr && directory[0] != '\0') std::snprintf(path, sizeof(path), "%s/alloc-sites-%lu.tsv", directory, GetCurrentProcessId());
            else std::snprintf(path, sizeof(path), "alloc-sites-%lu.tsv", GetCurrentProcessId());
            chosen = path;
        }
        const bool written = Write(chosen);
        std::fprintf(stderr, "[host-allocator] allocation-site census %s %s (%llu entries, %llu dropped)\n", written ? "written to" : "could not be written to", chosen,
            static_cast<unsigned long long>(entries.load(std::memory_order_relaxed)), static_cast<unsigned long long>(dropped.load(std::memory_order_relaxed)));
    }
} summary;

}

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

void* TryAllocate(std::size_t bytes, void* caller) noexcept {
    if (bytes == 0) bytes = 1;
    if (bytes <= LargestCached) {
        const auto sizeClass = (bytes - 1) / Granule;
        auto* const cache = CurrentCache();
        if (cache != nullptr) {
            if (cache->heads[sizeClass] == nullptr) {
                if (void* list = TakeList(sizeClass)) {
                    Count(Event::DepotTake, sizeClass);
                    cache->heads[sizeClass] = list;
                    cache->counts[sizeClass] = Depth(sizeClass);
                }
            }
            if (void* block = cache->heads[sizeClass]) {
                Count(Event::CacheHit, sizeClass);
                Site::Note(cache, SiteKind::AllocCached, sizeClass, caller);
                cache->heads[sizeClass] = *static_cast<void**>(block);
                --cache->counts[sizeClass];
                return block;
            }
        }
        Count(Event::Malloc, sizeClass);
        Site::Note(cache, SiteKind::AllocHeap, sizeClass, caller);
        return std::malloc((sizeClass + 1) * Granule);
    }
    Count(Event::Malloc, Classes);
    return std::malloc(bytes);
}

void* Allocate(std::size_t bytes, void* caller) {
    for (;;) {
        if (void* block = TryAllocate(bytes, caller)) return block;
        const auto handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}

void Release(void* block, void* caller, std::size_t bytes = 0) noexcept {
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
                Site::Note(cache, SiteKind::FreeCached, sizeClass, caller);
                return;
            }
            Count(Event::DepotFullFree, sizeClass);
            Site::Note(cache, SiteKind::FreeHeap, sizeClass, caller);
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

std::uint64_t SiteSamples(SiteKind kind, const void* low, const void* high, bool parent) noexcept {
    auto* const rows = Site::table.load(std::memory_order_acquire);
    if (rows == nullptr) return 0;
    const auto from = reinterpret_cast<std::uintptr_t>(low);
    const auto to = reinterpret_cast<std::uintptr_t>(high);
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < Site::Capacity; ++index) {
        auto& row = rows[index];
        if (row.state.load(std::memory_order_acquire) != Site::Ready) continue;
        if (kind != SiteKind::Count && (row.meta & 3u) != static_cast<std::uint32_t>(kind)) continue;
        bool inside = false;
        for (std::size_t i = parent ? 1 : 0; i <= (parent ? Site::Parents : 0); ++i) inside |= row.frames[i] >= from && row.frames[i] < to;
        if (!inside) continue;
        for (const auto& count : row.counts) total += count.load(std::memory_order_relaxed);
    }
    return total;
}

SiteTotals SiteCensusTotals() noexcept {
    SiteTotals totals;
    totals.entries = Site::entries.load(std::memory_order_relaxed);
    totals.dropped = Site::dropped.load(std::memory_order_relaxed);
    totals.walks = Site::walks.load(std::memory_order_relaxed);
    for (std::size_t kind = 0; kind < std::size(totals.samples); ++kind) totals.samples[kind] = Site::kinds[kind].load(std::memory_order_relaxed);
    return totals;
}

bool WriteSiteCensus(const char* path) noexcept {
    return Site::Write(path);
}

}

// The caller is operator new/delete's own return address: std::allocator and friends inline into their users,
// so it is usually the allocating code itself (out-of-line helpers such as vector growth show up as themselves).
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
void operator delete(void* block) noexcept { Release(block, __builtin_return_address(0)); }
void operator delete[](void* block) noexcept { Release(block, __builtin_return_address(0)); }
void operator delete(void* block, std::size_t bytes) noexcept { Release(block, __builtin_return_address(0), bytes); }
void operator delete[](void* block, std::size_t bytes) noexcept { Release(block, __builtin_return_address(0), bytes); }
void operator delete(void* block, const std::nothrow_t&) noexcept { Release(block, __builtin_return_address(0)); }
void operator delete[](void* block, const std::nothrow_t&) noexcept { Release(block, __builtin_return_address(0)); }

#endif
