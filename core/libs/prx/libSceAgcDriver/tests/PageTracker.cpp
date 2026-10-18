#include "prx/libSceAgcDriver/Execution/include/PageTracker.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace AgcDriver::PageTracker;
constexpr std::uint64_t Base = 0x200000000ull;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void geometry() {
    static_assert(PageBytes == 4096 && RegionBytes == 4 * 1024 * 1024 && RegionPages == 1024 && sizeof(Bitmap) == 128);
    std::vector<std::array<std::uint64_t, 3>> pieces;
    require(Split({Base + RegionBytes - 3, RegionBytes + 8}, [&](auto id, auto offset, auto bytes) { pieces.push_back({id, offset, bytes}); }), "valid split rejected");
    require(pieces == std::vector<std::array<std::uint64_t, 3>>{{Base / RegionBytes, RegionBytes - 3, 3}, {Base / RegionBytes + 1, 0, RegionBytes}, {Base / RegionBytes + 2, 0, 5}}, "region split geometry");
    require(!Split({Base, 0}, [](auto, auto, auto) {}) && !Split({0, 1}, [](auto, auto, auto) {}) && !Split({UINT64_MAX - 2, 3}, [](auto, auto, auto) {}), "invalid split accepted");
    Tracker tracker;
    const Range range{Base + 63 * PageBytes, 2 * PageBytes};
    tracker.RegisterRange(range, Kind::Section, Bit(Reason::ProtectedSection));
    const auto token = tracker.CaptureEpoch(range);
    require(!tracker.DirtySince(range, token), "bitmap word boundary is unknown");
    require(tracker.DirtySince({range.address - 1, range.bytes + 1}, tracker.CaptureEpoch({range.address - 1, range.bytes + 1})), "partial page coverage proved clean");
    require(tracker.DirtySince({range.address + range.bytes, 1}, tracker.CaptureEpoch({range.address + range.bytes, 1})), "bitmap spill covered adjacent page");
    tracker.RegisterRange({Base + 4 * PageBytes + 1, PageBytes - 1}, Kind::Section, Bit(Reason::Immutable));
    require(tracker.DirtySince({Base + 4 * PageBytes + 1, 1}, tracker.CaptureEpoch({Base + 4 * PageBytes + 1, 1})), "partial registration proved clean");
}

void epochs() {
    Tracker tracker;
    const Range range{Base + RegionBytes - PageBytes, 2 * PageBytes};
    require(tracker.DirtySince(range, tracker.CaptureEpoch(range)), "unknown range is clean");
    require(tracker.Counts().regions == 0, "query allocated metadata");
    tracker.RegisterRange(range, Kind::Section, Bit(Reason::ProtectedSection));
    auto token = tracker.CaptureEpoch(range);
    require(token.regions.size() == 2 && token.regions[0].regionId + 1 == token.regions[1].regionId, "multi-region token is not ordered");
    require(!tracker.DirtySince(range, token), "unchanged range dirty");
    tracker.MarkCpuWritten({range.address + PageBytes, 1}, Source::DriverStore);
    require(tracker.DirtySince(range, token), "second region write missed");
    token = tracker.CaptureEpoch(range);
    tracker.MarkCpuWritten({range.address + PageBytes, 1}, Source::DriverStore);
    require(tracker.DirtySince(range, token), "already dirty bit did not move epoch");
    token = tracker.CaptureEpoch(range);
    token.regions.pop_back();
    require(tracker.DirtySince(range, token), "missing region token proved clean");
    token = tracker.CaptureEpoch(range);
    token.regions[0].cpuEpoch = 0;
    require(tracker.DirtySince(range, token), "zero token proved clean");
    token = tracker.CaptureEpoch(range);
    tracker.InvalidateRange({range.address, PageBytes}, true);
    tracker.RegisterRange({range.address, PageBytes}, Kind::Section, Bit(Reason::ProtectedSection));
    require(tracker.DirtySince(range, token), "remap resurrected token");
    token = tracker.CaptureEpoch(range);
    tracker.InvalidateRange({range.address, PageBytes});
    require(tracker.DirtySince(range, token), "protection transition proved clean");
    tracker.SetCoverage(range, Bit(Reason::ProtectedSection));
    token = tracker.CaptureEpoch(range);
    tracker.MarkGpuWritten({range.address, PageBytes}, Source::GpuDeclaration);
    require(tracker.DirtySince(range, token), "GPU declaration missed");
    tracker.PublishGpuWrites({range.address, PageBytes});
    require(tracker.DirtySince(range, tracker.CaptureEpoch(range)), "publication cleared unresolved GPU work");
    tracker.RegisterRange(range, Kind::Private, Bit(Reason::PrivateWatch));
    require(tracker.DirtySince(range, tracker.CaptureEpoch(range)), "private epoch without fresh observation boundary proved clean");
    tracker.RegisterRange(range, Kind::Section, Bit(Reason::OpenWriter));
    require(tracker.DirtySince(range, tracker.CaptureEpoch(range)), "open section writer proved clean");
    tracker.SetCoverage(range, Bit(Reason::ProtectedSection));
    token = tracker.CaptureEpoch(range);
    tracker.ExcludeImport({range.address, 1});
    tracker.SetCoverage(range, Bit(Reason::ProtectedSection));
    require(tracker.DirtySince(range, token), "collection erased import exclusion");
    require(tracker.DirtySince({0, 0}, {}) && tracker.DirtySince({UINT64_MAX, 2}, {}), "invalid query clean");
    tracker.Poison();
    require(tracker.DirtySince(range, tracker.CaptureEpoch(range)), "dropped observation proved clean");
}

void coverage() {
    Tracker tracker;
    tracker.RegisterRange({Base, PageBytes}, Kind::Private, Bit(Reason::PrivateWatch));
    tracker.RegisterRange({Base + PageBytes, PageBytes}, Kind::Section, Bit(Reason::ProtectedSection));
    tracker.RegisterRange({Base + 2 * PageBytes, PageBytes}, Kind::Private, Bit(Reason::PrivateWatch));
    tracker.ExcludeImport({Base + 2 * PageBytes, PageBytes});
    const auto site = AgcDriver::GuestMemory::ReadSite::TextureRead;
    tracker.ObserveRead({Base + PageBytes - 3, 2 * PageBytes + 8}, site);
    const auto counts = tracker.Counts();
    const auto& read = counts.reads[static_cast<std::size_t>(site)];
    require(read[static_cast<std::size_t>(Kind::Private)].covered == 3 && read[static_cast<std::size_t>(Kind::Private)].total == 3, "private intersection");
    require(read[static_cast<std::size_t>(Kind::Section)].covered == PageBytes && read[static_cast<std::size_t>(Kind::Section)].total == PageBytes, "section intersection");
    require(read[static_cast<std::size_t>(Kind::Import)].covered == 0 && read[static_cast<std::size_t>(Kind::Import)].total == PageBytes, "import intersection");
    require(read[static_cast<std::size_t>(Kind::Unknown)].covered == 0 && read[static_cast<std::size_t>(Kind::Unknown)].total == 5, "unknown intersection");
    const auto line = tracker.ProfileLine();
    require(line.starts_with("[page-tracker]") && line.find("metadata-bytes") != std::string::npos, "profile fields missing");
    tracker.NotePoll(4, 123, false);
    tracker.ObservePolledPage({Base, PageBytes});
    require(tracker.Counts().polledPages == 4 && tracker.Counts().pollErrors == 1 && tracker.Counts().sources[static_cast<std::size_t>(Source::PrivatePoll)] == 1, "poll accounting");
}

void concurrentEpochs() {
    Tracker tracker;
    const Range range{Base, 2 * RegionBytes};
    tracker.RegisterRange(range, Kind::Section, Bit(Reason::Immutable));
    const auto before = tracker.CaptureEpoch(range);
    std::thread first([&] { for (int i = 0; i < 100; ++i) tracker.MarkCpuWritten({Base, 1}, Source::DriverStore); });
    std::thread second([&] { for (int i = 0; i < 100; ++i) tracker.MarkCpuWritten({Base + RegionBytes, 1}, Source::DriverStore); });
    for (int i = 0; i < 100; ++i) static_cast<void>(tracker.CaptureEpoch(range));
    first.join();
    second.join();
    const auto after = tracker.CaptureEpoch(range);
    require(after.regions[0].cpuEpoch == before.regions[0].cpuEpoch + 100 && after.regions[1].cpuEpoch == before.regions[1].cpuEpoch + 100, "concurrent write batches lost");
}

#ifdef _WIN32
void inaccessible() {
    auto* memory = VirtualAlloc(nullptr, PageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    require(memory != nullptr, "noaccess allocation failed");
    Tracker tracker;
    const Range range{reinterpret_cast<std::uintptr_t>(memory), PageBytes};
    tracker.RegisterRange(range, Kind::Section, Bit(Reason::Immutable));
    const auto token = tracker.CaptureEpoch(range);
    const auto clean = !tracker.DirtySince(range, token);
    tracker.ObserveRead(range, AgcDriver::GuestMemory::ReadSite::Capture);
    tracker.InvalidateRange(range, true);
    const auto invalid = tracker.DirtySince(range, token);
    MEMORY_BASIC_INFORMATION state{};
    VirtualQuery(memory, &state, sizeof(state));
    VirtualFree(memory, 0, MEM_RELEASE);
    require(clean && invalid && state.Protect == PAGE_NOACCESS, "query touched guest bytes or protections");
}

void aliases(bool enabled) {
    constexpr std::size_t bytes = 0x4000;
    auto* first = GuestArena::GuestArenaAllocate_nid_postfix(bytes, RegionBytes);
    auto* second = GuestArena::GuestArenaAllocate_nid_postfix(bytes, RegionBytes);
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
    require(section != nullptr, "test section failed");
    HANDLE duplicate = nullptr;
    require(DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS), "duplicate test section failed");
    GuestArena::GuestArenaMap_nid_postfix(first, bytes, section, 0, PAGE_READWRITE);
    GuestArena::GuestArenaMap_nid_postfix(second, bytes, duplicate, 0, PAGE_READWRITE);
    CloseHandle(duplicate);
    CloseHandle(section);
    const Range a{reinterpret_cast<std::uintptr_t>(first), PageBytes}, b{reinterpret_cast<std::uintptr_t>(second), PageBytes};
    Initialize();
    MEMORY_BASIC_INFORMATION state{};
    VirtualQuery(first, &state, sizeof(state));
    require(state.Protect == PAGE_READWRITE, "observer armed a section mapping");
    if (enabled) {
        auto* tracker = Get();
        require(tracker->DirtySince(a, tracker->CaptureEpoch(a)), "unarmed section proved clean");
        std::array<void*, 16> pages{};
        for (const auto range : {a, b}) {
            std::size_t count = pages.size();
            require(GuestArena::GuestArenaCollectWrites_nid_postfix(range.address, range.bytes, pages.data(), &count, true), "existing section collection failed");
        }
        const auto tokenA = tracker->CaptureEpoch(a), tokenB = tracker->CaptureEpoch(b);
        require(!tracker->DirtySince(a, tokenA) && !tracker->DirtySince(b, tokenB), "closed alias watches not observed");
        auto* alias = GuestArena::GuestArenaMapAlias_nid_postfix(a.address, bytes);
        require(tracker->DirtySince(a, tokenA) && tracker->DirtySince(b, tokenB), "persistent host alias proved clean");
        tracker->ObserveRead(a, AgcDriver::GuestMemory::ReadSite::BufferUpload);
        require(tracker->Counts().reads[static_cast<std::size_t>(AgcDriver::GuestMemory::ReadSite::BufferUpload)][static_cast<std::size_t>(Kind::Import)].total >= a.bytes, "host import read kind missing");
        GuestArena::GuestArenaUnmapAlias_nid_postfix(alias);
        const auto closedA = tracker->CaptureEpoch(a), closedB = tracker->CaptureEpoch(b);
        require(!tracker->DirtySince(a, closedA) && !tracker->DirtySince(b, closedB), "closed host alias stayed unprovable");
        const auto regions = tracker->Counts().regions;
        require(GuestArena::GuestArenaHandleWrite_nid_postfix(a.address), "existing write fault rejected");
        require(tracker->DirtySince(a, tokenA) && tracker->DirtySince(b, tokenB), "fault did not fan out duplicated backing handles across regions");
        require(tracker->Counts().regions == regions, "write fault allocated regions");
        const auto before = tracker->CaptureEpoch(b);
        GpuWrite(a);
        const auto after = tracker->CaptureEpoch(b);
        require(after.regions[0].gpuEpoch > before.regions[0].gpuEpoch, "GPU declaration alias fanout");
    }
    GuestArena::GuestArenaReset_nid_postfix(first, bytes);
    GuestArena::GuestArenaReset_nid_postfix(second, bytes);
    GuestArena::GuestArenaRelease_nid_postfix(first, bytes);
    GuestArena::GuestArenaRelease_nid_postfix(second, bytes);
}
#endif
}

int main() {
    try {
        const auto* value = std::getenv("APS5_PAGE_TRACKER");
        const bool enabled = value != nullptr && std::string_view(value) == "1";
        require(!ObserversRegistered() && !GuestAllocations::GuestAllocationsHasPageInvalidator_nid_no_patch(), "observer installed before initialization");
#ifdef _WIN32
        aliases(enabled);
#endif
        Initialize();
        Initialize();
        require(Enabled() == enabled && (Get() != nullptr) == enabled && ObserversRegistered() == enabled, "switch semantics");
        require(GuestAllocations::GuestAllocationsHasPageInvalidator_nid_no_patch() == enabled, "switch-off registered registry observer");
#ifdef _WIN32
        require(GuestArena::GuestArenaHasPageObserver_nid_no_patch() == enabled, "switch-off registered mapping observer");
        inaccessible();
        aliases(enabled);
#endif
        geometry();
        epochs();
        coverage();
        concurrentEpochs();
        if (!enabled) {
            CpuWrite({Base, PageBytes});
            GpuWrite({Base, PageBytes});
            Publish({Base, PageBytes});
            ReportIfDue([](const char*) { throw std::runtime_error("off-mode printed"); });
            require(Get() == nullptr, "off-mode created tracker");
        }
        std::cout << "page tracker " << (enabled ? "on" : "off") << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
