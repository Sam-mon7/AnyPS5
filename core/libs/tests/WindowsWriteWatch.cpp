#define NOMINMAX
#include "prx/libc/include/WindowsMappings.hpp"
#include <array>
#include <atomic>
#include <iostream>
#include <set>
#include <thread>

namespace {
using Mappings = GuestArena::WindowsMappings;
using Collection = Mappings::Collection;
constexpr std::size_t Small = 4096;
constexpr std::size_t Bytes = 65536;
std::atomic<unsigned> faults{0};
std::uint64_t allocationGeneration = 1;

std::uint64_t AllocationGeneration() {
    return allocationGeneration;
}

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

LONG CALLBACK HandleWrite(EXCEPTION_POINTERS* exception) {
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    if (!Mappings::Get().HandleWrite(record->ExceptionInformation[1])) return EXCEPTION_CONTINUE_SEARCH;
    ++faults;
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct Region {
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, Bytes, nullptr);
    unsigned char* first = static_cast<unsigned char*>(Mappings::Get().Reserve(nullptr, Bytes));
    unsigned char* alias = static_cast<unsigned char*>(Mappings::Get().Reserve(nullptr, Bytes));

    Region() {
        Require(section && first && alias, "cannot allocate shared test memory");
        Mappings::Get().Map(first, Bytes, section, 0, PAGE_READWRITE);
        Mappings::Get().Map(alias, Bytes, section, 0, PAGE_READWRITE);
    }
    ~Region() {
        Mappings::Get().Reset(first, Bytes);
        Mappings::Get().Reset(alias, Bytes);
        VirtualFree(first, 0, MEM_RELEASE);
        VirtualFree(alias, 0, MEM_RELEASE);
        CloseHandle(section);
    }
    void Write(std::size_t page, unsigned char value) {
        static_cast<volatile unsigned char*>(first)[page * Small] = value;
    }
    std::set<std::size_t> Collect(Collection mode, std::size_t begin = 0, std::size_t count = 16, bool clear = true, bool useAlias = false, std::size_t capacity = 32) {
        auto* base = useAlias ? alias : first;
        const auto address = reinterpret_cast<std::uintptr_t>(base);
        auto cursor = address + begin * Small;
        const auto end = cursor + count * Small;
        std::set<std::size_t> result;
        while (cursor < end) {
            std::array<void*, 32> pages{};
            auto available = capacity;
            Require(Mappings::Get().Collect(cursor, end - cursor, pages.data(), &available, clear, mode), "shared collection failed");
            for (std::size_t i = 0; i < available; ++i) result.insert((reinterpret_cast<std::uintptr_t>(pages[i]) - address) / Small);
            if (available < capacity) break;
            cursor = reinterpret_cast<std::uintptr_t>(pages[available - 1]) + Small;
        }
        return result;
    }
    DWORD Protection(std::size_t page, bool useAlias = false) {
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery((useAlias ? alias : first) + page * Small, &info, sizeof(info)) == sizeof(info), "protection query failed");
        return info.Protect;
    }
};

std::set<std::size_t> Pages(std::size_t begin, std::size_t end) {
    std::set<std::size_t> result;
    for (auto page = begin; page < end; ++page) result.insert(page);
    return result;
}

void CheckArmed() {
    Region region;
    Require(region.Collect(Collection::Armed).empty(), "import collected fresh memory");
    const auto before = faults.load();
    region.Write(0, 1);
    region.Write(4, 2);
    Require(faults == before, "import armed fresh memory");
    Require(region.Collect(Collection::All, 0, 1) == Pages(0, 1), "first partial collect failed");
    Require(region.Protection(0) == PAGE_READONLY && region.Protection(3) == PAGE_READWRITE, "partial collect did not protect only its page");
    region.Write(0, 3);
    region.Write(4, 4);
    Require(region.Collect(Collection::Armed) == Pages(0, 1), "armed collect lost a write or consumed fresh pages");
    Require(region.Protection(4) == PAGE_READWRITE, "armed collect protected another fresh view");
    Require(region.Collect(Collection::All) == Pages(1, 16), "ordinary collect lost fresh pages");
    Require(region.Collect(Collection::All).empty(), "cleared memory reports writes");
}

void CheckFresh() {
    Region region;
    Require(region.Collect(Collection::All, 0, 1) == Pages(0, 1), "cannot collect initial page");
    region.Write(0, 7);
    Require(region.Collect(Collection::Fresh) == Pages(1, 16), "fresh collect consumed an existing dirty page");
    Require(region.Collect(Collection::Fresh).empty(), "fresh collect repeated initial writes");
    Require(region.Collect(Collection::Armed) == Pages(0, 1), "fresh collect cleared an existing write");
    region.Write(9, 9);
    Require(region.Collect(Collection::Armed) == Pages(9, 10), "write after activation was missed");
}

void CheckAliasesAndCapacity() {
    Region region;
    Require(region.Collect(Collection::All, 0, 1) == Pages(0, 1), "initial collect failed");
    Require(region.Protection(0, true) == PAGE_READONLY, "alias was not protected");
    Require(region.Collect(Collection::Armed, 0, 16, true, true).empty(), "alias protection was mistaken for collection history");
    Require(region.Collect(Collection::Fresh, 0, 16, true, true, 1) == Pages(0, 16), "small output buffers lost fresh pages");
    Require(region.Collect(Collection::Fresh, 0, 16, true, true).empty(), "small output buffers did not finish fresh collection");
    static_cast<volatile unsigned char*>(region.alias)[0] = 8;
    Require(region.Collect(Collection::Armed) == Pages(0, 1), "alias write was missed or fresh neighbors were consumed");
    Require(region.Collect(Collection::Fresh) == Pages(1, 16), "alias collection changed another view's history");
    Mappings::Get().Reset(region.first, Bytes);
    Mappings::Get().Map(region.first, Bytes, region.section, 0, PAGE_READWRITE);
    Require(region.Collect(Collection::Armed).empty(), "remapped address kept old collection history");
    Require(region.Collect(Collection::Fresh) == Pages(0, 16), "remapped memory was not fresh");
}

void CheckPinsAndHostWrites() {
    Region region;
    const auto address = reinterpret_cast<std::uintptr_t>(region.first);
    Mappings::Get().Pin(address, 16384);
    Require(region.Collect(Collection::Fresh) == Pages(0, 16), "pinned fresh collection failed");
    Require(region.Protection(0) == PAGE_READWRITE, "pinned page was protected");
    Require(region.Collect(Collection::Armed) == Pages(0, 4), "pinned writes were trusted as clean");
    Mappings::Get().Unpin(address, 16384);
    Require(region.Collect(Collection::Armed) == Pages(0, 4), "unpin was not invalidated");
    Require(region.Collect(Collection::Armed).empty(), "unpin did not restore tracking");
    Require(Mappings::Get().BeginHostWrite(address, 16384), "host write refused");
    region.Write(0, 3);
    region.Collect(Collection::Armed);
    region.Write(0, 4);
    Mappings::Get().EndHostWrite(address, 16384);
    Require(region.Collect(Collection::Armed) == Pages(0, 4), "host write completion was missed");
    Require(region.Collect(Collection::Armed).empty(), "completed host write stayed dirty");
}

void CheckFailureAndHoles() {
    Region region;
    const auto address = reinterpret_cast<std::uintptr_t>(region.first);
    DWORD previous;
    Mappings::Get().SetProtection(address + 16384, 16384, PAGE_NOACCESS);
    Require(VirtualProtect(region.first + 16384, 16384, PAGE_NOACCESS, &previous), "cannot protect test page");
    std::array<void*, 32> pages{};
    auto count = pages.size();
    Require(!Mappings::Get().Collect(address, Bytes, pages.data(), &count, true, Collection::Fresh), "fresh inaccessible page accepted");
    Require(region.Protection(0) == PAGE_READWRITE, "failed collect changed an earlier page");
    Mappings::Get().SetProtection(address + 16384, 16384, PAGE_READWRITE);
    Require(VirtualProtect(region.first + 16384, 16384, PAGE_READWRITE, &previous), "cannot restore test page");
    Mappings::Get().Reset(region.first + 16384, 16384);
    Require(region.Collect(Collection::Fresh) == std::set<std::size_t>({0, 1, 2, 3, 8, 9, 10, 11, 12, 13, 14, 15}), "fresh collection did not tolerate an unmapped block neighbor");
    Require(region.Collect(Collection::Fresh).empty(), "holes caused fresh pages to repeat");
}

void CheckPrivate() {
    auto& mappings = Mappings::Get();
    auto* memory = static_cast<unsigned char*>(mappings.Reserve(nullptr, Bytes));
    Require(memory != nullptr, "private reservation failed");
    mappings.Commit(memory, Bytes, PAGE_READWRITE, Bytes, true);
    const auto address = reinterpret_cast<std::uintptr_t>(memory);
    std::array<void*, 32> pages{};
    std::size_t count = pages.size();
    mappings.Collect(address, Bytes, pages.data(), &count, true);
    static_cast<volatile unsigned char*>(memory)[Small] = 3;
    count = pages.size();
    Require(mappings.Collect(address, Bytes, pages.data(), &count, true, Collection::Fresh) && count == 0, "fresh collect touched private memory");
    count = pages.size();
    Require(mappings.Collect(address, Bytes, pages.data(), &count, true, Collection::Armed) && count == 1 && pages[0] == memory + Small, "private write watch changed");
    mappings.Reset(memory, Bytes);
    VirtualFree(memory, 0, MEM_RELEASE);
}

std::set<std::size_t> CollectPrivate(unsigned char* memory, std::size_t bytes, bool clear = true, std::size_t capacity = 32) {
    const auto address = reinterpret_cast<std::uintptr_t>(memory);
    const auto end = address + bytes;
    std::set<std::size_t> result;
    for (auto cursor = address; cursor < end;) {
        std::array<void*, 32> pages{};
        auto count = capacity;
        Require(Mappings::Get().Collect(cursor, end - cursor, pages.data(), &count, clear), "private collection failed");
        for (std::size_t i = 0; i < count; ++i) result.insert((reinterpret_cast<std::uintptr_t>(pages[i]) - address) / Small);
        if (count < capacity) break;
        cursor = reinterpret_cast<std::uintptr_t>(pages[count - 1]) + Small;
    }
    return result;
}

void CheckPrivateRegions() {
    auto& mappings = Mappings::Get();
    const bool cached = std::getenv("APS5_NO_GUEST_REGION_CACHE") == nullptr;
    auto* memory = static_cast<unsigned char*>(mappings.Reserve(nullptr, 2 * Bytes));
    Require(memory != nullptr, "private regions reservation failed");
    mappings.Commit(memory, 2 * Bytes, PAGE_READWRITE, Bytes, true);
    CollectPrivate(memory, 2 * Bytes);
    const auto before = mappings.RegionCacheStats();
    static_cast<volatile unsigned char*>(memory)[Small] = 1;
    static_cast<volatile unsigned char*>(memory)[Bytes + Small] = 2;
    const std::set<std::size_t> written{1, 17};
    Require(CollectPrivate(memory, 2 * Bytes, false) == written, "cached collect lost a write across allocations");
    Require(CollectPrivate(memory, 2 * Bytes, false) == written, "nonreset collect consumed private writes");
    Require(CollectPrivate(memory, 2 * Bytes, true, 1) == written, "small output buffers lost private writes");
    Require(CollectPrivate(memory, 2 * Bytes).empty(), "cached collect did not reset private writes");
    const auto after = mappings.RegionCacheStats();
    Require(cached ? after.hits > before.hits && after.regionQueries == before.regionQueries
                   : after.hits == before.hits && after.regionQueries > before.regionQueries,
            "private region cache did not follow its enable flag");

    const auto address = reinterpret_cast<std::uintptr_t>(memory);
    mappings.SetProtection(address + 4 * Small, Small, PAGE_READONLY);
    DWORD previous;
    Require(VirtualProtect(memory + 4 * Small, Small, PAGE_READONLY, &previous), "cannot split private protection");
    static_cast<volatile unsigned char*>(memory)[5 * Small] = 3;
    Require(CollectPrivate(memory, Bytes) == Pages(5, 6), "protection split lost a private write");
    const auto protectedStats = mappings.RegionCacheStats();
    Require(protectedStats.regionQueries > after.regionQueries, "protection change kept cached extents");
    if (cached) Require(protectedStats.mappingInvalidations == after.mappingInvalidations + 1, "protection invalidation was not counted");

    mappings.Reset(memory, Bytes);
    std::array<void*, 32> pages{};
    auto count = pages.size();
    Require(!mappings.Collect(address, Bytes, pages.data(), &count, false), "reset private memory stayed committed in the cache");
    mappings.Commit(memory, Bytes, PAGE_READWRITE, Bytes, true);
    static_cast<volatile unsigned char*>(memory)[6 * Small] = 4;
    Require(CollectPrivate(memory, 2 * Bytes) == Pages(6, 7), "recommit retained a cached uncommitted region");

    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, Bytes, nullptr);
    Require(section != nullptr, "cannot allocate private replacement section");
    mappings.Map(memory, Bytes, section, 0, PAGE_READWRITE);
    Require(CollectPrivate(memory, Bytes) == Pages(0, 16), "mapping a section retained private collection state");
    mappings.Reset(memory, Bytes);
    mappings.Reset(memory + Bytes, Bytes);
    Require(VirtualFree(memory, 0, MEM_RELEASE) && VirtualFree(memory + Bytes, 0, MEM_RELEASE), "cannot release private regions");
    CloseHandle(section);
}

void CheckPrivateGeneration() {
    auto& mappings = Mappings::Get();
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, Bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
    Require(memory != nullptr, "generation test allocation failed");
    ++allocationGeneration;
    CollectPrivate(memory, Bytes);
    const auto before = mappings.RegionCacheStats();
    Require(VirtualFree(memory, Bytes, MEM_DECOMMIT), "cannot decommit private test memory");
    ++allocationGeneration;
    std::array<void*, 32> pages{};
    auto count = pages.size();
    Require(!mappings.Collect(reinterpret_cast<std::uintptr_t>(memory), Bytes, pages.data(), &count, false), "generation change kept a decommitted region");
    if (std::getenv("APS5_NO_GUEST_REGION_CACHE") == nullptr) {
        Require(mappings.RegionCacheStats().generationInvalidations == before.generationInvalidations + 1, "generation invalidation was not counted");
    }
    Require(VirtualAlloc(memory, Bytes, MEM_COMMIT, PAGE_READWRITE) == memory, "cannot recommit private test memory");
    ++allocationGeneration;
    static_cast<volatile unsigned char*>(memory)[Small] = 5;
    Require(CollectPrivate(memory, Bytes) == Pages(1, 2), "generation change retained a negative cache entry");
    Require(VirtualFree(memory, 0, MEM_RELEASE), "cannot release private test memory");
    Require(VirtualAlloc(memory, Bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE) == memory, "cannot reuse private test address");
    ++allocationGeneration;
    static_cast<volatile unsigned char*>(memory)[2 * Small] = 6;
    Require(CollectPrivate(memory, Bytes) == Pages(2, 3), "address reuse lost a private write");
    Require(VirtualFree(memory, 0, MEM_RELEASE), "cannot release reused private memory");
    ++allocationGeneration;
}

void CheckPrivateRetry() {
    auto& mappings = Mappings::Get();
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 2 * Bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
    Require(memory != nullptr, "retry test allocation failed");
    ++allocationGeneration;
    CollectPrivate(memory, 2 * Bytes);
    const auto before = mappings.RegionCacheStats();
    Require(VirtualFree(memory, 0, MEM_RELEASE), "cannot release retry test memory");
    Require(VirtualAlloc(memory, Bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE) == memory, "cannot reuse first half of retry allocation");
    Require(VirtualAlloc(memory + Bytes, Bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE) == memory + Bytes, "cannot reuse second half of retry allocation");
    static_cast<volatile unsigned char*>(memory)[Small] = 7;
    static_cast<volatile unsigned char*>(memory)[Bytes + Small] = 8;
    const std::set<std::size_t> written{1, 17};
    Require(CollectPrivate(memory, 2 * Bytes, false) == written, "stale extent retry lost private writes");
    const auto after = mappings.RegionCacheStats();
    Require(after.writeWatchRetries - before.writeWatchRetries == (std::getenv("APS5_NO_GUEST_REGION_CACHE") == nullptr ? 1u : 0u), "stale extent did not retry exactly once");
    Require(CollectPrivate(memory, 2 * Bytes) == written, "retry consumed private writes without reset");
    Require(CollectPrivate(memory, 2 * Bytes).empty(), "retried private writes did not reset");
    Require(VirtualFree(memory, 0, MEM_RELEASE) && VirtualFree(memory + Bytes, 0, MEM_RELEASE), "cannot release retry test allocations");
    ++allocationGeneration;
}

void CheckForgottenSectionKeepsNoPages() {
    auto& mappings = Mappings::Get();
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, Bytes, nullptr);
    auto* first = static_cast<unsigned char*>(mappings.Reserve(nullptr, Bytes));
    auto* second = static_cast<unsigned char*>(mappings.Reserve(nullptr, Bytes));
    Require(section && first && second, "cannot allocate forgotten section test memory");
    const auto resident = [&](unsigned char* address) {
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(address, &info, sizeof(info)) == sizeof(info), "resident query failed");
        return info.State == MEM_COMMIT && info.Type == MEM_PRIVATE;
    };
    mappings.Map(first, Bytes, section, 0, PAGE_READWRITE, true, true);
    mappings.Map(second, Bytes, section, 0, PAGE_READWRITE, true, true);
    Require(!resident(first) && !resident(second), "a second mapping kept the first resident");
    mappings.ForgetSection(section);
    mappings.Reset(first, Bytes);
    mappings.Reset(second, Bytes);
    mappings.Map(first, Bytes, section, 0, PAGE_READWRITE, true, true);
    Require(resident(first), "a forgotten section's handle value kept its pages in the section");
    first[0] = 7;
    mappings.ForgetSection(section);
    mappings.Reset(first, Bytes);
    mappings.Map(first, Bytes, section, 0, PAGE_READWRITE, true, true);
    Require(resident(first) && first[0] == 0, "a forgotten section's handle value kept its unmapped bytes");
    mappings.Reset(first, Bytes);
    VirtualFree(first, 0, MEM_RELEASE);
    VirtualFree(second, 0, MEM_RELEASE);
    CloseHandle(section);
}

void CheckConcurrentWrites() {
    Region region;
    std::atomic<bool> start{false};
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::size_t page = 0; page < 16; ++page) region.Write(page, static_cast<unsigned char>(page + 1));
    });
    start.store(true, std::memory_order_release);
    region.Collect(Collection::Fresh);
    writer.join();
    for (std::size_t page = 0; page < 16; ++page) Require(region.first[page * Small] == page + 1, "activation lost a concurrent write");
    region.Collect(Collection::Armed);
    Require(region.Collect(Collection::Armed).empty(), "concurrent writes did not settle");
}
}

int main(int argc, char** argv) {
    _putenv_s("APS5_PROFILE_GUEST_REGIONS", "1");
    _putenv_s("APS5_NO_GUEST_REGION_CACHE", argc == 2 && std::string(argv[1]) == "--no-region-cache" ? "0" : "");
    Mappings::Get().CacheRegionsUntil(AllocationGeneration);
    auto* handler = AddVectoredExceptionHandler(1, HandleWrite);
    try {
        Require(handler != nullptr, "cannot install test handler");
        CheckArmed();
        CheckFresh();
        CheckAliasesAndCapacity();
        CheckPinsAndHostWrites();
        CheckFailureAndHoles();
        CheckPrivate();
        CheckPrivateRegions();
        CheckPrivateGeneration();
        CheckPrivateRetry();
        CheckConcurrentWrites();
        CheckForgottenSectionKeepsNoPages();
        Mappings::Get().CacheRegionsUntil(nullptr);
        std::cout << "Windows deferred write-watch tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    RemoveVectoredExceptionHandler(handler);
}
