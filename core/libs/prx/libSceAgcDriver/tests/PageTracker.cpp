#include "prx/libSceAgcDriver/Execution/include/PageTracker.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
namespace {
thread_local bool handlingObservationFault = false;
thread_local int gpuThreadToken = 0;
std::atomic<unsigned> gpuLockCalls{0}, observationHookCalls{0}, protectedWriteFaults{0};
std::atomic<std::uintptr_t> observationAddress{0}, nestedAddress{0};
std::atomic<std::uint64_t> expectedFaultPages{0};
}

namespace AgcDriver::GuestMemory {
GpuMutexType& GpuMutex() { static GpuMutexType mutex; return mutex; }
void GpuMutexType::lock() {
    if (handlingObservationFault) std::abort();
    ++gpuLockCalls;
    mutex.lock();
    owner.store(&gpuThreadToken);
    ++depth;
}
void GpuMutexType::unlock() {
    if (--depth == 0) owner.store(nullptr);
    mutex.unlock();
}
bool GpuMutexType::HeldByThisThread() const { return owner.load() == &gpuThreadToken; }
}
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

void dirtyPages() {
    Tracker tracker;
    const Range range{Base + RegionBytes - 65 * PageBytes + 7, 67 * PageBytes - 13};
    tracker.RegisterRange({Base, 2 * RegionBytes}, Kind::Section, Bit(Reason::ProtectedSection));
    auto token = tracker.DirtyPagesSince(range, {}).token;
    auto dirty = tracker.DirtyPagesSince(range, token);
    require(dirty.precise && dirty.regions == std::vector<Bitmap>(2), "unchanged page query is dirty");
    tracker.MarkCpuWritten({Base + RegionBytes - 65 * PageBytes, 2 * PageBytes}, Source::DriverStore);
    tracker.MarkCpuWritten({Base + RegionBytes, 1}, Source::DriverStore);
    dirty = tracker.DirtyPagesSince(range, token);
    Bitmap first{}, second{};
    first[14] = 1ull << 63;
    first[15] = 1;
    second[0] = 1;
    require(dirty.precise && dirty.regions == std::vector<Bitmap>{first, second}, "page query missed bitmap or region boundaries");
    const auto older = token;
    token = dirty.token;
    tracker.MarkCpuWritten({Base + RegionBytes, 1}, Source::DriverStore);
    require(tracker.DirtyPagesSince(range, token).regions == std::vector<Bitmap>{Bitmap{}, second}, "repeated dirty page write was lost");
    require(tracker.DirtyPagesSince(range, older).regions == std::vector<Bitmap>{first, second}, "newer consumer consumed older page history");
    token = tracker.DirtyPagesSince(range, token).token;
    tracker.MarkCpuWritten({Base, 1}, Source::DriverStore);
    require(tracker.DirtyPagesSince(range, token).regions == std::vector<Bitmap>(2), "write outside range dirtied queried pages");
    tracker.InvalidateRange({Base + RegionBytes, PageBytes});
    tracker.SetCoverage({Base + RegionBytes, PageBytes}, Bit(Reason::ProtectedSection));
    require(!tracker.DirtyPagesSince(range, token).precise, "mapping invalidation reused page history");
    token = tracker.DirtyPagesSince(range, {}).token;
    tracker.MarkCpuWritten({Base + RegionBytes, 1}, Source::SectionFault);
    require(!tracker.DirtyPagesSince(range, token).token.covered, "open writer produced a page proof");
    tracker.SetCoverage({Base + RegionBytes, PageBytes}, Bit(Reason::ProtectedSection));
    require(tracker.DirtyPagesSince(range, token).precise && tracker.DirtyPagesSince(range, token).regions[1][0] == 1, "rearming lost the changed page");
    token = tracker.DirtyPagesSince(range, {}).token;
    tracker.SetCoverage({Base + RegionBytes, PageBytes}, Bit(Reason::Unsupported));
    tracker.SetCoverage({Base + RegionBytes, PageBytes}, Bit(Reason::ProtectedSection));
    require(!tracker.DirtyPagesSince(range, token).precise, "restored unknown coverage reused page history");
    token = tracker.DirtyPagesSince(range, {}).token;
    tracker.MarkGpuWritten({Base + RegionBytes, PageBytes}, Source::GpuDeclaration);
    require(!tracker.DirtyPagesSince(range, token).token.covered, "pending GPU work produced a page proof");
    require(!tracker.DirtyPagesSince({0, 0}, {}).precise, "invalid range produced a page proof");
    tracker.Poison();
    require(!tracker.DirtyPagesSince(range, token).token.covered, "poisoned page query proved clean");
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
    tracker.ObservePolledPage({Base, PageBytes});
    tracker.Checkpoint(true);
    require(tracker.Counts().uniquePolledPages == (ArmEnabled() ? 1 : 0) && tracker.Counts().framePolledPages == (ArmEnabled() ? 1 : 0), "unique polled pages per frame");
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
    const auto token = tracker.DirtyPagesSince(range, {}).token;
    const auto pages = tracker.DirtyPagesSince(range, token);
    const auto clean = !tracker.DirtySince(range, token) && pages.precise && pages.regions == std::vector<Bitmap>(1);
    tracker.ObserveRead(range, AgcDriver::GuestMemory::ReadSite::Capture);
    tracker.InvalidateRange(range, true);
    const auto invalid = tracker.DirtySince(range, token);
    MEMORY_BASIC_INFORMATION state{};
    VirtualQuery(memory, &state, sizeof(state));
    VirtualFree(memory, 0, MEM_RELEASE);
    require(clean && invalid && state.Protect == PAGE_NOACCESS, "query touched guest bytes or protections");
}

LONG CALLBACK observationException(EXCEPTION_POINTERS* exception) {
    const auto& record = *exception->ExceptionRecord;
    if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record.NumberParameters < 2 || record.ExceptionInformation[0] > 1) return EXCEPTION_CONTINUE_SEARCH;
    const bool write = record.ExceptionInformation[0] == 1;
    if (write) ++protectedWriteFaults;
    handlingObservationFault = true;
    const bool handled = write ? GuestArena::GuestArenaHandleWrite_nid_postfix(record.ExceptionInformation[1]) : GuestArena::GuestArenaHandleTransition_nid_postfix(record.ExceptionInformation[1]);
    handlingObservationFault = false;
    return handled ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

void observationHook() noexcept {
    if (!handlingObservationFault || AgcDriver::GuestMemory::GpuMutex().HeldByThisThread()) std::abort();
    MEMORY_BASIC_INFORMATION state{};
    const auto address = observationAddress.load();
    if (address != 0 && (VirtualQuery(reinterpret_cast<void*>(address), &state, sizeof(state)) != sizeof(state) || state.Protect != PAGE_READONLY || Get()->Counts().faultPages < expectedFaultPages.load())) std::abort();
    if (const auto nested = nestedAddress.exchange(0); nested != 0 && GuestArena::GuestArenaHandleWrite_nid_postfix(nested)) std::abort();
    ++observationHookCalls;
}

struct ObservationViews {
    static constexpr std::size_t bytes = 0x4000;
    void* a = GuestArena::GuestArenaAllocate_nid_postfix(bytes, RegionBytes);
    void* b = GuestArena::GuestArenaAllocate_nid_postfix(bytes, RegionBytes);
    ObservationViews() {
        const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
        require(section != nullptr, "observation section failed");
        HANDLE duplicate = nullptr;
        require(DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS), "observation duplicate failed");
        GuestArena::GuestArenaMap_nid_postfix(a, bytes, section, 0, PAGE_READWRITE);
        GuestArena::GuestArenaMap_nid_postfix(b, bytes, duplicate, 0, PAGE_READWRITE);
        CloseHandle(duplicate);
        CloseHandle(section);
    }
    ~ObservationViews() {
        for (auto* view : {a, b}) {
            GuestArena::GuestArenaReset_nid_postfix(view, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(view, bytes);
        }
    }
    Range A() const { return {reinterpret_cast<std::uintptr_t>(a), PageBytes}; }
    Range B() const { return {reinterpret_cast<std::uintptr_t>(b), PageBytes}; }
};

void protectedObservation() {
    ObservationViews views;
    auto* handler = AddVectoredExceptionHandler(1, observationException);
    require(handler != nullptr, "observation VEH failed");
    GuestArena::GuestArenaSetPageObservationFaultHook_nid_no_patch(observationHook);
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        require(AgcDriver::GuestMemory::GpuMutex().HeldByThisThread(), "GPU mutex test hook not owned");
    }
    const auto locks = gpuLockCalls.load();
    const auto faults = protectedWriteFaults.load();
    const auto before = GuestArena::GuestArenaPageObservationCounts_nid_no_patch();
    auto* tracker = Get();
    const auto tokenA = tracker != nullptr ? tracker->CaptureEpoch(views.A()) : Epoch{};
    const auto tokenB = tracker != nullptr ? tracker->CaptureEpoch(views.B()) : Epoch{};
    const auto regions = tracker != nullptr ? tracker->Counts().regions : 0;
    if (ArmEnabled()) {
        observationAddress.store(views.A().address);
        nestedAddress.store(views.A().address);
        expectedFaultPages.store(tracker->Counts().faultPages + 2);
    }
    *static_cast<volatile unsigned*>(views.a) = 17;
    require(*static_cast<volatile unsigned*>(views.b) == 17, "faulting write did not complete");
    *static_cast<volatile unsigned*>(views.b) = 23;
    const auto aliasesWritten = GuestArena::GuestArenaPageObservationCounts_nid_no_patch();
    if (aliasesWritten.fallbackWindows == before.fallbackWindows) require(protectedWriteFaults.load() == faults + (ArmEnabled() ? 1 : 0), "physical watch did not clear across aliases");
    if (ArmEnabled()) {
        require(tracker->DirtySince(views.A(), tokenA) && tracker->DirtySince(views.B(), tokenB), "G3 fault alias fanout missing");
        require(tracker->Counts().regions == regions, "G3 fault allocated region metadata");
    }
    CompletedFrame();
    const auto rearmed = tracker != nullptr ? tracker->CaptureEpoch(views.A()) : Epoch{};
    *static_cast<volatile unsigned*>(views.b) = 31;
    const auto after = GuestArena::GuestArenaPageObservationCounts_nid_no_patch();
    if (ArmEnabled()) {
        require(protectedWriteFaults.load() >= faults + 2 && observationHookCalls.load() >= 2, "G3 rearm did not fault");
        require(tracker->DirtySince(views.A(), rearmed), "rearmed write missed");
        const auto observed = after.faults[static_cast<std::size_t>(GuestArena::PageFaultKind::Observation)].calls - before.faults[static_cast<std::size_t>(GuestArena::PageFaultKind::Observation)].calls;
        require(after.physicalPages == before.physicalPages + observed && after.aliasPages == before.aliasPages + 2 * observed, "physical/alias page counters");
        require(after.rearmedPages > before.rearmedPages && after.frames == before.frames + 1, "frame rearm counters");
        require(observed >= 2, "G3-only fault kind");
        require(after.faults[static_cast<std::size_t>(GuestArena::PageFaultKind::Nested)].calls > before.faults[static_cast<std::size_t>(GuestArena::PageFaultKind::Nested)].calls, "nested fault not counted");
    } else {
        require(protectedWriteFaults.load() == faults && after.protection.calls == 0 && after.checkpoint.calls == 0, "off-mode faulted or protected memory");
    }
    require(gpuLockCalls.load() == locks, "observation fault took GpuMutex");
    if (ArmEnabled()) {
        const auto fallback = after.fallbackWindows;
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        GuestArena::GuestArenaPageObservationCheckpoint_nid_no_patch(false);
        require(GuestArena::GuestArenaPageObservationCounts_nid_no_patch().fallbackWindows > fallback, "timed fallback did not checkpoint");
        const auto beforeFallbackWrite = protectedWriteFaults.load();
        *static_cast<volatile unsigned*>(views.a) = 41;
        require(protectedWriteFaults.load() > beforeFallbackWrite, "timed fallback did not rearm");
        CompletedFrame();
        std::atomic<bool> start{false};
        std::thread first([&] { while (!start.load()) std::this_thread::yield(); static_cast<volatile unsigned*>(views.a)[0] = 43; });
        std::thread second([&] { while (!start.load()) std::this_thread::yield(); static_cast<volatile unsigned*>(views.b)[1] = 53; });
        start.store(true);
        first.join();
        second.join();
        require(static_cast<volatile unsigned*>(views.a)[0] == 43 && static_cast<volatile unsigned*>(views.a)[1] == 53, "competing alias writes did not complete");
        require(gpuLockCalls.load() == locks, "concurrent observation took GpuMutex");
    }
    GuestArena::GuestArenaSetPageObservationFaultHook_nid_no_patch(nullptr);
    observationAddress.store(0);
    RemoveVectoredExceptionHandler(handler);
}

void armingRefusals() {
    if (!ArmEnabled()) return;
    ObservationViews views;
    const auto address = views.A().address;
    const auto before = GuestArena::GuestArenaPageObservationCounts_nid_no_patch();
    require(!GuestArena::GuestArenaArmPageObservation_nid_no_patch(address + 1, PageBytes), "partial observation page armed");
    require(!GuestArena::GuestArenaArmPageObservation_nid_no_patch(Base + 128 * RegionBytes, PageBytes), "unknown observation mapping armed");
    GuestArena::GuestArenaPinWritable_nid_postfix(views.a, views.bytes);
    require(!GuestArena::GuestArenaArmPageObservation_nid_no_patch(address, PageBytes), "pinned observation page armed");
    GuestArena::GuestArenaUnpinWritable_nid_postfix(views.a, views.bytes);
    require(GuestArena::GuestArenaArmPageObservation_nid_no_patch(address, PageBytes), "unpin observation rearm failed");
    require(GuestArena::GuestArenaBeginHostWrite_nid_postfix(views.a, PageBytes), "host write refused");
    require(!GuestArena::GuestArenaArmPageObservation_nid_no_patch(address, PageBytes), "open host writer armed");
    *static_cast<volatile unsigned*>(views.a) = 47;
    GuestArena::GuestArenaEndHostWrite_nid_postfix(views.a, PageBytes);
    auto* alias = GuestArena::GuestArenaMapAlias_nid_postfix(address, views.bytes);
    require(!GuestArena::GuestArenaArmPageObservation_nid_no_patch(address, PageBytes), "unnotified host alias armed");
    GuestArena::GuestArenaUnmapAlias_nid_postfix(alias);
    require(GuestArena::GuestArenaArmPageObservation_nid_no_patch(address, PageBytes), "closed host alias rearm failed");
    for (auto* pointer : {views.a, views.b}) {
        const auto base = reinterpret_cast<std::uintptr_t>(pointer);
        GuestArena::GuestArenaSetProtection_nid_postfix(base, views.bytes, PAGE_READONLY);
        require(GuestArena::GuestArenaApplyGpuProtection_nid_no_patch(base, views.bytes, PAGE_READONLY), "compose immutable protection");
    }
    require(!GuestArena::GuestArenaArmPageObservation_nid_no_patch(address, PageBytes), "immutable mapping armed");
    require(!GuestArena::GuestArenaHandleWrite_nid_postfix(address), "real permission violation handled");
    GuestArena::GuestArenaEnablePageObservation_nid_no_patch([]() noexcept { return false; });
    require(!GuestArena::GuestArenaArmPageObservation_nid_no_patch(address, PageBytes), "missing tracker metadata armed");
    GuestArena::GuestArenaEnablePageObservation_nid_no_patch([]() noexcept { return !Get()->Poisoned(); });
    const auto after = GuestArena::GuestArenaPageObservationCounts_nid_no_patch();
    for (const auto reason : {GuestArena::PageArmRefusal::InvalidRange, GuestArena::PageArmRefusal::UnknownMapping, GuestArena::PageArmRefusal::Metadata, GuestArena::PageArmRefusal::Pinned, GuestArena::PageArmRefusal::HostWriter, GuestArena::PageArmRefusal::HostAlias, GuestArena::PageArmRefusal::Permission}) {
        const auto i = static_cast<std::size_t>(reason);
        require(after.refusals[i] > before.refusals[i], "arming refusal reason not counted");
    }
    const auto line = Get()->ProfileLine();
    require(line.find("redzone-marker=") != std::string::npos && line.find("fallback-ms=1000") != std::string::npos && line.find("immutable:") != std::string::npos && line.find("import-read-share=") != std::string::npos, "armed diagnostics missing");
    GuestArena::GuestArenaSetProtection_nid_postfix(address, views.bytes, PAGE_EXECUTE_READWRITE);
    GuestArena::GuestArenaApplyGpuProtection_nid_no_patch(address, views.bytes, PAGE_EXECUTE_READWRITE);
    require(GuestArena::GuestArenaArmPageObservation_nid_no_patch(address, PageBytes), "executable observation rearm");
    MEMORY_BASIC_INFORMATION state{};
    VirtualQuery(views.a, &state, sizeof(state));
    require(state.Protect == PAGE_EXECUTE_READ, "CPU observation removed execute permission");
}

void mixedProtections() {
    if (!ArmEnabled()) return;
    ObservationViews views;
    auto* handler = AddVectoredExceptionHandler(1, observationException);
    require(handler != nullptr, "mixed protection VEH failed");
    require(GuestArena::GuestArenaBeginHostWrite_nid_postfix(views.a, PageBytes), "mixed host write begin failed");
    GuestArena::GuestArenaEndHostWrite_nid_postfix(views.a, PageBytes);
    std::array<void*, 16> dirty{};
    std::size_t count = dirty.size();
    require(GuestArena::GuestArenaCollectWrites_nid_postfix(views.A().address, PageBytes, dirty.data(), &count, true), "legacy observation arm failed");
    GuestArena::GuestArenaSetGpuPageFault_nid_no_patch([](std::uintptr_t owner, bool write) {
        if (AgcDriver::GuestMemory::GpuMutex().HeldByThisThread()) std::abort();
        MEMORY_BASIC_INFORMATION state{};
        VirtualQuery(reinterpret_cast<void*>(owner), &state, sizeof(state));
        if (state.Protect != PAGE_NOACCESS) std::abort();
        GuestArena::GuestArenaDisarmGpuWriteWatch_nid_no_patch(owner, PageBytes);
        return true;
    });
    require(GuestArena::GuestArenaArmGpuWriteWatch_nid_no_patch(views.A().address, PageBytes), "mixed GPU arm failed");
    require(GuestArena::GuestArenaProtectGpuAccess_nid_no_patch(views.A().address, PageBytes), "mixed pending GPU protection failed");
    CompletedFrame();
    MEMORY_BASIC_INFORMATION pending{};
    VirtualQuery(views.a, &pending, sizeof(pending));
    require(pending.Protect == PAGE_NOACCESS, "G3 rearm removed pending GPU protection");
    const auto before = GuestArena::GuestArenaPageObservationCounts_nid_no_patch();
    const auto token = Get()->CaptureEpoch(views.B());
    *static_cast<volatile unsigned*>(views.a) = 61;
    require(*static_cast<volatile unsigned*>(views.b) == 61 && Get()->DirtySince(views.B(), token), "mixed GPU/CPU watch did not finish write");
    const auto after = GuestArena::GuestArenaPageObservationCounts_nid_no_patch();
    require(after.faults[static_cast<std::size_t>(GuestArena::PageFaultKind::GpuWrite)].calls > before.faults[static_cast<std::size_t>(GuestArena::PageFaultKind::GpuWrite)].calls, "mixed fault lost GPU classification");
    require(GuestArena::GuestArenaArmGpuWriteWatch_nid_no_patch(views.A().address, PageBytes), "read GPU arm failed");
    require(GuestArena::GuestArenaProtectGpuAccess_nid_no_patch(views.A().address, PageBytes), "pending GPU read protection failed");
    const auto readToken = Get()->CaptureEpoch(views.A());
    require(*static_cast<volatile unsigned*>(views.a) == 61, "GPU pending read did not complete");
    const auto readAfter = GuestArena::GuestArenaPageObservationCounts_nid_no_patch();
    require(readAfter.faults[static_cast<std::size_t>(GuestArena::PageFaultKind::GpuRead)].calls > after.faults[static_cast<std::size_t>(GuestArena::PageFaultKind::GpuRead)].calls && Get()->CaptureEpoch(views.A()).regions.front().cpuEpoch == readToken.regions.front().cpuEpoch, "GPU read classified as CPU write");
    GuestArena::GuestArenaSetGpuPageFault_nid_no_patch(nullptr);
    RemoveVectoredExceptionHandler(handler);
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
    require(state.Protect == (ArmEnabled() ? PAGE_READONLY : PAGE_READWRITE), "section arming switch semantics");
    if (enabled) {
        auto* tracker = Get();
        require(tracker->DirtySince(a, tracker->CaptureEpoch(a)) != ArmEnabled(), "section observation coverage");
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
        require(ArmEnabled() == (enabled && std::getenv("APS5_PAGE_TRACKER_ARM") != nullptr && std::string_view(std::getenv("APS5_PAGE_TRACKER_ARM")) == "1"), "arm switch semantics");
        require(Enabled() == enabled && (Get() != nullptr) == enabled && ObserversRegistered() == enabled, "switch semantics");
        require(GuestAllocations::GuestAllocationsHasPageInvalidator_nid_no_patch() == enabled, "switch-off registered registry observer");
#ifdef _WIN32
        require(GuestArena::GuestArenaHasPageObserver_nid_no_patch() == enabled, "switch-off registered mapping observer");
        inaccessible();
        aliases(enabled);
        protectedObservation();
        armingRefusals();
        mixedProtections();
#endif
        geometry();
        epochs();
        dirtyPages();
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
