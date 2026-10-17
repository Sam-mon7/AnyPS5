#define NOMINMAX
#include "prx/libc/include/WindowsMappings.hpp"
#include <array>
#include <atomic>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace {
using Mappings = GuestArena::WindowsMappings;
using Collection = Mappings::Collection;
constexpr std::size_t Small = 4096;
constexpr std::size_t Bytes = 65536;
std::atomic<unsigned> faults{0};
std::mutex gpuMutex;
std::map<std::uintptr_t, std::array<unsigned char, Small>> gpuBases;
std::map<std::uintptr_t, std::array<unsigned char, Small>> gpuResults;
std::atomic<unsigned> gpuCaptures{0};
std::atomic<std::uintptr_t> queuedPage{0};
std::atomic<unsigned> queuedFaults{0};
std::atomic<bool> releaseQueuedFaults{true};
std::atomic<unsigned> enteredGpuCallbacks{0};
std::atomic<bool> refuseGpuCapture{false};

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

LONG CALLBACK HandleWrite(EXCEPTION_POINTERS* exception) {
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[0] > 1) return EXCEPTION_CONTINUE_SEARCH;
    if ((record->ExceptionInformation[1] & ~(Small - 1)) == queuedPage.load(std::memory_order_acquire)) {
        queuedFaults.fetch_add(1, std::memory_order_release);
        while (!releaseQueuedFaults.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    const bool handled = record->ExceptionInformation[0] == 1 ? Mappings::Get().HandleWrite(record->ExceptionInformation[1]) : Mappings::Get().HandleTransition(record->ExceptionInformation[1]);
    if (!handled) return EXCEPTION_CONTINUE_SEARCH;
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

bool CaptureGpuPage(std::uintptr_t page, bool write) {
    enteredGpuCallbacks.fetch_add(1, std::memory_order_release);
    std::lock_guard lock(gpuMutex);
    if (refuseGpuCapture.load()) return false;
    std::uint32_t protection = 0;
    bool pending = false;
    if (Mappings::Get().GpuOverlayProtection(page, &protection, &pending) && pending) {
        const auto result = gpuResults.find(page);
        Require(result != gpuResults.end(), "protected fault has no GPU result");
        const auto base = gpuBases.find(page);
        Require(Mappings::Get().PublishGpuPage(page, result->second.data(), base == gpuBases.end() ? nullptr : base->second.data()), "cannot publish protected GPU page");
        gpuResults.erase(result);
        return true;
    }
    Require(write, "eager watch received a read fault");
    auto [found, inserted] = gpuBases.try_emplace(page);
    if (inserted) {
        std::memcpy(found->second.data(), reinterpret_cast<const void*>(page), Small);
        gpuCaptures.fetch_add(1);
    }
    return true;
}

struct GpuWatch {
    std::uintptr_t address;
    std::size_t bytes;
    GpuWatch(std::uintptr_t address, std::size_t bytes = Small) : address(address), bytes(bytes) {
        Require(Mappings::Get().ArmGpuWriteWatch(address, bytes), "cannot arm GPU write watch");
    }
    ~GpuWatch() { Mappings::Get().DisarmGpuWriteWatch(address, bytes); }
};

void CheckGpuEnableExisting() {
    Region region;
    auto* extra = static_cast<unsigned char*>(Mappings::Get().Reserve(nullptr, Bytes));
    Require(extra != nullptr, "cannot reserve existing GPU alias");
    HANDLE duplicate = nullptr;
    Require(DuplicateHandle(GetCurrentProcess(), region.section, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS), "cannot duplicate existing section handle");
    Mappings::Get().Map(extra, Bytes, duplicate, 0, PAGE_READWRITE);
    CloseHandle(duplicate);
    Mappings::Get().SetGpuPageFault(&CaptureGpuPage);
    gpuBases.clear();
    const auto address = reinterpret_cast<std::uintptr_t>(region.first);
    {
        GpuWatch watched(address);
        static_cast<volatile unsigned char*>(extra)[1] = 0x69;
        Require(gpuBases.contains(address) && region.first[1] == 0x69, "enabling GPU tracking missed an existing duplicate-handle alias");
    }
    Mappings::Get().Reset(extra, Bytes);
    VirtualFree(extra, 0, MEM_RELEASE);
    Mappings::Get().SetGpuPageFault(nullptr);
}

void CheckGpuHandleMigration() {
    auto& mappings = Mappings::Get();
    for (unsigned iteration = 0; iteration < 16; ++iteration) {
        Region first;
        const auto other = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, Bytes, nullptr);
        Require(other != nullptr, "cannot create unrelated migration section");
        CloseHandle(first.section);
        first.section = nullptr;
        mappings.SetGpuPageFault(&CaptureGpuPage);
        auto* extra = static_cast<unsigned char*>(mappings.Reserve(nullptr, Bytes));
        Require(extra != nullptr, "cannot reserve handle migration alias");
        mappings.Map(extra, Bytes, other, 0, PAGE_READWRITE);
        std::vector<std::pair<std::uintptr_t, std::size_t>> aliases;
        Require(mappings.VisitGpuAliases(reinterpret_cast<std::uintptr_t>(first.first), Small, [](std::uintptr_t alias, std::size_t bytes, void* context) {
            static_cast<std::vector<std::pair<std::uintptr_t, std::size_t>>*>(context)->emplace_back(alias, bytes);
            return true;
        }, &aliases), "cannot query aliases after handle migration");
        Require(aliases == std::vector<std::pair<std::uintptr_t, std::size_t>>{{reinterpret_cast<std::uintptr_t>(first.alias), Small}}, "reused section handle joined unrelated physical pages");
        mappings.Reset(extra, Bytes);
        VirtualFree(extra, 0, MEM_RELEASE);
        CloseHandle(other);
        mappings.SetGpuPageFault(nullptr);
    }
}

void CheckGpuShared() {
    Region region;
    const auto address = reinterpret_cast<std::uintptr_t>(region.first);
    std::memset(region.first, 0x11, Small);
    gpuBases.clear();
    const auto captures = gpuCaptures.load();
    GpuWatch watched(address);
    Require(region.Protection(0) == PAGE_READONLY && region.Protection(0, true) == PAGE_READONLY, "GPU watch did not protect shared aliases");
    std::uint32_t logical = 0;
    bool pending = true;
    Require(Mappings::Get().GpuOverlayProtection(reinterpret_cast<std::uintptr_t>(region.alias), &logical, &pending) && logical == PAGE_READWRITE && !pending, "GPU overlay lost logical shared protection");
    static_cast<volatile unsigned char*>(region.alias)[9] = 0x77;
    static_cast<volatile unsigned char*>(region.first)[11] = 0x88;
    Require(gpuCaptures == captures + 1 && gpuBases.contains(address), "alias fault did not capture the canonical page once");
    auto result = gpuBases.at(address);
    result[2] = 0x44;
    result[9] = 0x66;
    Require(Mappings::Get().PublishGpuPage(address, result.data(), gpuBases.at(address).data()), "shared GPU publication failed");
    Require(region.first[2] == 0x44 && region.first[9] == 0x77 && region.first[11] == 0x88 && region.alias[2] == 0x44, "shared GPU merge lost a CPU store or GPU byte");
    Require(!Mappings::Get().GpuOverlayProtection(address, &logical, &pending), "published shared page retained its watch");
}

void CheckGpuProtectedAccess() {
    Region region;
    auto& mappings = Mappings::Get();
    const auto address = reinterpret_cast<std::uintptr_t>(region.first);
    const auto aliasAddress = reinterpret_cast<std::uintptr_t>(region.alias);
    std::memset(region.first, 0x31, Small);
    gpuBases.clear();
    Require(mappings.CanProtectGpuAccess(address, Small), "shared GPU page is not protectable");
    {
        GpuWatch watched(address);
        gpuResults[address].fill(0x31);
        gpuResults[address][2] = 0x44;
        Require(mappings.ProtectGpuAccess(address, Small), "cannot protect shared GPU access");
        Require(region.Protection(0) == PAGE_NOACCESS && region.Protection(0, true) == PAGE_NOACCESS, "GPU access protection missed a shared alias");
        auto* added = static_cast<unsigned char*>(mappings.Reserve(nullptr, Bytes));
        Require(added != nullptr, "cannot reserve protected GPU alias");
        mappings.Map(added, Bytes, region.section, 0, PAGE_READWRITE);
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(added, &info, sizeof(info)) == sizeof(info) && info.Protect == PAGE_NOACCESS, "new shared alias bypassed GPU access protection");
        mappings.Reset(added, Bytes);
        VirtualFree(added, 0, MEM_RELEASE);
        region.Collect(Collection::Fresh);
        Require(region.Protection(0) == PAGE_NOACCESS && region.Protection(0, true) == PAGE_NOACCESS, "collection reopened GPU protected memory");
        mappings.SetProtection(aliasAddress, 16384, PAGE_READONLY);
        mappings.ApplyGpuProtection(aliasAddress, 16384, PAGE_READONLY);
        std::uint32_t protection = 0;
        bool pending = false;
        Require(mappings.GpuOverlayProtection(aliasAddress, &protection, &pending) && protection == PAGE_READONLY && pending, "protected alias lost logical read-only permissions");
        Require(!mappings.HandleWrite(aliasAddress), "protected logical read-only alias accepted a native store");
        Require(static_cast<volatile unsigned char*>(region.alias)[2] == 0x44, "native shared read missed protected GPU publication");
        Require(region.Protection(0, true) == PAGE_READONLY, "GPU publication reopened a logical read-only alias");
        region.Write(0, 0x62);
        Require(region.Collect(Collection::Armed).contains(0), "GPU publication lost ordinary write watching");
    }
    mappings.SetProtection(aliasAddress, 16384, PAGE_READWRITE);
    mappings.ApplyGpuProtection(aliasAddress, 16384, PAGE_READWRITE);
    {
        gpuBases.clear();
        GpuWatch watched(address);
        static_cast<volatile unsigned char*>(region.alias)[9] = 0x77;
        auto result = gpuBases.at(address);
        result[2] = 0x55;
        result[9] = 0x88;
        gpuResults[address] = result;
        Require(mappings.ProtectGpuAccess(address, Small), "cannot protect CPU-dirty shared page");
        static_cast<volatile unsigned char*>(region.alias)[11] = 0x99;
        Require(region.first[2] == 0x55 && region.first[9] == 0x77 && region.first[11] == 0x99, "protected write lost merged CPU or GPU bytes");
    }
    {
        gpuBases.clear();
        GpuWatch watched(address);
        std::array<unsigned char, Small> result;
        result.fill(0x45);
        Require(mappings.ProtectGpuAccess(address, Small), "cannot protect queued read page");
        queuedFaults.store(0);
        releaseQueuedFaults.store(false);
        queuedPage.store(aliasAddress, std::memory_order_release);
        unsigned char firstValue = 0;
        unsigned char secondValue = 0;
        std::thread first([&] { firstValue = static_cast<volatile unsigned char*>(region.alias)[2]; });
        std::thread second([&] { secondValue = static_cast<volatile unsigned char*>(region.alias)[3]; });
        while (queuedFaults.load(std::memory_order_acquire) < 2) std::this_thread::yield();
        const bool published = mappings.PublishGpuPage(address, result.data(), nullptr);
        releaseQueuedFaults.store(true, std::memory_order_release);
        first.join();
        second.join();
        queuedPage.store(0, std::memory_order_release);
        Require(published && firstValue == 0x45 && secondValue == 0x45, "queued native reads did not resume after publication");
    }
    {
        GpuWatch watched(address);
        Require(mappings.ProtectGpuAccess(address, Small), "cannot protect stale callback read page");
        std::unique_lock lock(gpuMutex);
        const auto entered = enteredGpuCallbacks.load();
        unsigned char value = 0;
        std::thread reader([&] { value = static_cast<volatile unsigned char*>(region.alias)[2]; });
        while (enteredGpuCallbacks.load(std::memory_order_acquire) == entered) std::this_thread::yield();
        std::array<unsigned char, Small> result;
        result.fill(0x56);
        const bool published = mappings.PublishGpuPage(address, result.data(), nullptr);
        refuseGpuCapture.store(true);
        lock.unlock();
        reader.join();
        refuseGpuCapture.store(false);
        Require(published && value == 0x56, "native read waiting inside callback did not resume after publication");
    }
    {
        GpuWatch watched(address);
        Require(mappings.ProtectGpuAccess(address, Small), "cannot protect logical no-access page");
        mappings.SetProtection(address, 16384, PAGE_NOACCESS);
        mappings.ApplyGpuProtection(address, 16384, PAGE_NOACCESS);
        Require(!mappings.HandleTransition(address), "GPU overlay bypassed logical no-access protection");
        std::array<unsigned char, Small> result;
        result.fill(0x67);
        Require(mappings.PublishGpuPage(address, result.data(), nullptr) && region.Protection(0) == PAGE_NOACCESS, "publication lost logical no-access protection");
    }
    mappings.SetProtection(address, 16384, PAGE_READWRITE);
    mappings.ApplyGpuProtection(address, 16384, PAGE_READWRITE);
    if (const auto* runs = std::getenv("APS5_RUN_GUEST_VIEWS"); runs != nullptr && std::string_view(runs) == "1") {
        GpuWatch watched(address);
        Require(mappings.ProtectGpuAccess(address, Small), "cannot protect kept run page");
        const auto before = region.Protection(4);
        bool refused = false;
        try { mappings.Reset(region.first + 16384, 16384); } catch (const std::runtime_error&) { refused = true; }
        Require(refused && region.Protection(0) == PAGE_NOACCESS && region.Protection(4) == before, "partial run split exposed GPU protected kept pages");
    }
}

void CheckGpuAliasOnlyMigration() {
    Region region;
    auto& mappings = Mappings::Get();
    const auto address = reinterpret_cast<std::uintptr_t>(region.first);
    auto* host = mappings.MapAlias(address, 16384);
    mappings.Reset(region.first, Bytes);
    mappings.Reset(region.alias, Bytes);
    mappings.SetGpuPageFault(nullptr);
    mappings.SetGpuPageFault(&CaptureGpuPage);
    mappings.Map(region.first, Bytes, region.section, 0, PAGE_READWRITE);
    mappings.Map(region.alias, Bytes, region.section, 0, PAGE_READWRITE);
    Require(!mappings.CanProtectGpuAccess(address, Small) && !mappings.ArmGpuWriteWatch(address, Small), "callback migration lost a host-alias-only physical page");
    mappings.UnmapAlias(host);
    Require(mappings.CanProtectGpuAccess(address, Small), "released host alias kept a shared page ineligible");
}

void CheckGpuProtectionAndAliases() {
    Region region;
    const auto address = reinterpret_cast<std::uintptr_t>(region.first);
    auto* host = Mappings::Get().MapAlias(address, 16384);
    Require(!Mappings::Get().CanProtectGpuAccess(address, Small), "GPU access accepted a live host alias");
    Require(!Mappings::Get().ArmGpuWriteWatch(address, Small), "GPU watch accepted an untracked writable alias");
    Mappings::Get().UnmapAlias(host);
    Require(Mappings::Get().BeginHostWrite(address, Small), "cannot open host write");
    Require(!Mappings::Get().ArmGpuWriteWatch(address, Small), "GPU watch accepted an open host write");
    Mappings::Get().EndHostWrite(address, Small);
    Mappings::Get().Pin(address, Small);
    Require(!Mappings::Get().ArmGpuWriteWatch(address, Small), "GPU watch accepted a writable pin");
    Mappings::Get().Unpin(address, Small);
    gpuBases.clear();
    GpuWatch watched(address);
    bool refused = false;
    try { Mappings::Get().MapAlias(address, 16384); } catch (const std::runtime_error&) { refused = true; }
    Require(refused, "host alias bypassed an active GPU watch");
    Require(!Mappings::Get().ArmGpuWriteWatch(reinterpret_cast<std::uintptr_t>(region.alias), Small), "second physical writer was accepted");
    Mappings::Get().SetProtection(reinterpret_cast<std::uintptr_t>(region.alias), 16384, PAGE_READWRITE);
    Require(Mappings::Get().ApplyGpuProtection(reinterpret_cast<std::uintptr_t>(region.alias), 16384, PAGE_READWRITE), "GPU protection transaction was not active");
    Require(region.Protection(0, true) == PAGE_READONLY, "guest protection change bypassed GPU watch");
    Mappings::Get().Commit(region.alias, 16384, PAGE_READWRITE, 16384, true);
    Require(region.Protection(0, true) == PAGE_READONLY, "commit bypassed GPU watch");
    std::vector<std::pair<std::uintptr_t, std::size_t>> aliases;
    Require(Mappings::Get().VisitGpuAliases(address, Small, [](std::uintptr_t alias, std::size_t bytes, void* context) {
        static_cast<std::vector<std::pair<std::uintptr_t, std::size_t>>*>(context)->emplace_back(alias, bytes);
        return true;
    }, &aliases), "cannot enumerate GPU aliases");
    Require(aliases == std::vector<std::pair<std::uintptr_t, std::size_t>>{{reinterpret_cast<std::uintptr_t>(region.alias), Small}}, "GPU alias enumeration differs from physical backing");
    auto* added = static_cast<unsigned char*>(Mappings::Get().Reserve(nullptr, Bytes));
    Require(added != nullptr, "cannot reserve new tracked alias");
    HANDLE duplicate = nullptr;
    Require(DuplicateHandle(GetCurrentProcess(), region.section, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS), "cannot duplicate shared section handle");
    Mappings::Get().Map(added, Bytes, duplicate, 0, PAGE_READWRITE);
    CloseHandle(duplicate);
    MEMORY_BASIC_INFORMATION info{};
    Require(VirtualQuery(added, &info, sizeof(info)) == sizeof(info) && info.Protect == PAGE_READONLY, "new tracked alias bypassed GPU watch");
    Mappings::Get().Reset(added, Bytes);
    VirtualFree(added, 0, MEM_RELEASE);
    Require(Mappings::Get().BeginHostWrite(address, Small), "GPU watched host write refused");
    Require(!Mappings::Get().ProtectGpuAccess(address, Small), "GPU access sealed an active host write");
    static_cast<volatile unsigned char*>(region.first)[4] = 0x55;
    Mappings::Get().EndHostWrite(address, Small);
    Require(gpuBases.contains(address) && region.first[4] == 0x55, "host write bypassed first-fault capture");
}

void CheckGpuPrivate() {
    auto& mappings = Mappings::Get();
    auto* memory = static_cast<unsigned char*>(mappings.Reserve(nullptr, Bytes));
    Require(memory != nullptr, "cannot reserve private GPU test memory");
    mappings.Commit(memory, Bytes, PAGE_READWRITE, Bytes, true);
    const auto address = reinterpret_cast<std::uintptr_t>(memory);
    Require(!mappings.CanProtectGpuAccess(address, Small), "private page accepted safe alias protection");
    std::memset(memory, 0x21, Small);
    gpuBases.clear();
    Require(mappings.BeginHostWrite(address, Small), "cannot open private host write");
    Require(!mappings.ArmGpuWriteWatch(address, Small), "GPU watch accepted an open private host write");
    mappings.EndHostWrite(address, Small);
    mappings.Pin(address, Small);
    Require(!mappings.ArmGpuWriteWatch(address, Small), "GPU watch accepted a pinned private page");
    mappings.Unpin(address, Small);
    {
        GpuWatch watched(address);
        Require(!mappings.ProtectGpuAccess(address, Small), "private page accepted GPU no-access protection");
        std::uint32_t protection = 0;
        bool pending = true;
        Require(mappings.GpuOverlayProtection(address, &protection, &pending) && protection == PAGE_READWRITE && !pending, "private overlay lost logical protection");
        static_cast<volatile unsigned char*>(memory)[5] = 0x77;
        auto result = gpuBases.at(address);
        result[1] = 0x66;
        result[5] = 0x88;
        Require(mappings.PublishGpuPage(address, result.data(), gpuBases.at(address).data()), "private GPU publication failed");
        Require(memory[1] == 0x66 && memory[5] == 0x77, "private merge lost concurrent CPU data");
    }
    {
        GpuWatch watched(address);
        std::array<unsigned char, Small> result{};
        result.fill(0x33);
        queuedFaults.store(0);
        releaseQueuedFaults.store(false);
        queuedPage.store(address, std::memory_order_release);
        std::thread first([&] { static_cast<volatile unsigned char*>(memory)[7] = 0x71; });
        std::thread second([&] { static_cast<volatile unsigned char*>(memory)[8] = 0x72; });
        while (queuedFaults.load(std::memory_order_acquire) < 2) std::this_thread::yield();
        const bool published = mappings.PublishGpuPage(address, result.data(), nullptr);
        releaseQueuedFaults.store(true, std::memory_order_release);
        first.join();
        second.join();
        queuedPage.store(0, std::memory_order_release);
        Require(published && memory[0] == 0x33 && memory[7] == 0x71 && memory[8] == 0x72, "queued private faults did not resume after publication");
    }
    {
        GpuWatch watched(address);
        std::unique_lock lock(gpuMutex);
        const auto entered = enteredGpuCallbacks.load();
        std::thread writer([&] { static_cast<volatile unsigned char*>(memory)[10] = 0x79; });
        while (enteredGpuCallbacks.load(std::memory_order_acquire) == entered) std::this_thread::yield();
        mappings.DisarmGpuWriteWatch(address, Small);
        refuseGpuCapture.store(true);
        lock.unlock();
        writer.join();
        refuseGpuCapture.store(false);
        Require(memory[10] == 0x79, "fault waiting inside callback did not resume after disarm");
    }
    mappings.Reset(memory, Bytes);
    VirtualFree(memory, 0, MEM_RELEASE);
}
}

int main() {
    auto* handler = AddVectoredExceptionHandler(1, HandleWrite);
    try {
        Require(handler != nullptr, "cannot install test handler");
        CheckArmed();
        CheckFresh();
        CheckAliasesAndCapacity();
        CheckPinsAndHostWrites();
        CheckFailureAndHoles();
        CheckPrivate();
        CheckConcurrentWrites();
        CheckGpuEnableExisting();
        CheckGpuHandleMigration();
        Mappings::Get().SetGpuPageFault(&CaptureGpuPage);
        CheckGpuShared();
        CheckGpuProtectedAccess();
        CheckGpuAliasOnlyMigration();
        CheckGpuProtectionAndAliases();
        CheckGpuPrivate();
        Mappings::Get().SetGpuPageFault(nullptr);
        std::cout << "Windows deferred write-watch tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    RemoveVectoredExceptionHandler(handler);
}
