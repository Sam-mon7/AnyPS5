#include "prx/libSceAgcDriver/Execution/include/PageTracker.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#endif

namespace AgcDriver::PageTracker {
namespace {

template<class Function>
void Words(std::uint64_t offset, std::uint64_t bytes, Function&& function) {
    const auto first = offset / PageBytes;
    const auto last = (offset + bytes + PageBytes - 1) / PageBytes;
    for (auto page = first; page < last;) {
        const auto stop = std::min(last, (page / 64 + 1) * 64);
        const auto low = ~std::uint64_t{0} << (page % 64);
        const auto high = ~std::uint64_t{0} >> ((64 - stop % 64) % 64);
        function(page / 64, low & high);
        page = stop;
    }
}

void Assign(Bitmap& bitmap, std::uint64_t offset, std::uint64_t bytes, bool set) {
    Words(offset, bytes, [&](auto word, auto mask) { if (set) bitmap[word] |= mask; else bitmap[word] &= ~mask; });
}

std::uint64_t Pages(Range range) {
    return range.Valid() ? (range.address % PageBytes + range.bytes - 1) / PageBytes + 1 : 0;
}

static_assert(Bit(Reason::PrivateWatch) == GuestArena::PagePrivateWatch && Bit(Reason::ProtectedSection) == GuestArena::PageProtectedSection && Bit(Reason::Immutable) == GuestArena::PageImmutable && Bit(Reason::OpenWriter) == GuestArena::PageOpenWriter && Bit(Reason::ExcludedImport) == GuestArena::PageExcludedImport && Bit(Reason::HostAlias) == GuestArena::PageHostAlias && Bit(Reason::Unsupported) == GuestArena::PageUnsupported && Bit(Reason::Unmapped) == GuestArena::PageUnmapped && Bit(Reason::PendingGpu) == GuestArena::PagePendingGpu);
std::atomic<bool> registered{false};

void mappingEvent(GuestArena::PageEvent event, std::uintptr_t address, std::size_t bytes, std::uint32_t reasons) noexcept {
    auto* tracker = Get();
    if (tracker == nullptr) return;
    try {
        const Range range{address, bytes};
        switch (event) {
        case GuestArena::PageEvent::PrivateMap: tracker->RegisterRange(range, Kind::Private, reasons); break;
        case GuestArena::PageEvent::PrivatePollRange: tracker->SetCoverage(range, reasons); break;
        case GuestArena::PageEvent::SectionMap: tracker->RegisterRange(range, Kind::Section, reasons); break;
        case GuestArena::PageEvent::Invalidate: tracker->InvalidateRange(range); break;
        case GuestArena::PageEvent::Unmap: tracker->InvalidateRange(range, true); break;
        case GuestArena::PageEvent::SectionState: tracker->SetCoverage(range, reasons); break;
        case GuestArena::PageEvent::WriteFault: tracker->MarkCpuWritten(range, Source::SectionFault); break;
        case GuestArena::PageEvent::HostWrite: tracker->MarkCpuWritten(range, Source::HostWrite); break;
        }
    } catch (...) { tracker->Poison(); }
}

void allocationEvent(std::uintptr_t address, std::size_t bytes) noexcept {
    mappingEvent(GuestArena::PageEvent::Invalidate, address, bytes, 0);
}

}

void Tracker::Advance(std::uint64_t& epoch) {
    if (epoch != 0) epoch = epoch == std::numeric_limits<std::uint64_t>::max() ? 0 : epoch + 1;
}

Tracker::Region* Tracker::GetRegion(std::uint64_t id, bool create) const {
    {
        std::shared_lock lock(directoryMutex);
        const auto found = directory.find(id);
        if (found != directory.end()) return found->second.get();
    }
    if (!create) return nullptr;
    std::unique_lock lock(directoryMutex);
    auto& region = directory[id];
    if (region == nullptr) {
        region = std::make_unique<Region>();
        region->reasons[static_cast<std::size_t>(Reason::Unmapped)].fill(~std::uint64_t{0});
        region->kinds[static_cast<std::size_t>(Kind::Unknown)].fill(~std::uint64_t{0});
    }
    return region.get();
}

void Tracker::RegisterRange(Range range, Kind kind, Reasons reasons) {
    Visit(range, true, [&](auto, auto offset, auto bytes, auto* region) {
        Advance(region->mapEpoch);
        Assign(region->cpuDirty, offset, bytes, true);
        Assign(region->gpuDirty, offset, bytes, false);
        for (std::size_t i = 0; i < region->kinds.size(); ++i) Assign(region->kinds[i], offset, bytes, i == static_cast<std::size_t>(kind));
        for (std::size_t i = 0; i < region->reasons.size(); ++i) Assign(region->reasons[i], offset, bytes, (reasons & (1u << i)) != 0);
        if (offset % PageBytes != 0) Assign(region->reasons[static_cast<std::size_t>(Reason::Unsupported)], offset, 1, true);
        if ((offset + bytes) % PageBytes != 0) Assign(region->reasons[static_cast<std::size_t>(Reason::Unsupported)], offset + bytes - 1, 1, true);
    });
}

void Tracker::InvalidateRange(Range range, bool unmapped) {
    Visit(range, true, [&](auto, auto offset, auto bytes, auto* region) {
        if (region == nullptr) return;
        Advance(region->mapEpoch);
        Assign(region->reasons[static_cast<std::size_t>(unmapped ? Reason::Unmapped : Reason::Unsupported)], offset, bytes, true);
        if (unmapped) {
            for (std::size_t i = 0; i < region->kinds.size(); ++i) Assign(region->kinds[i], offset, bytes, i == static_cast<std::size_t>(Kind::Unknown));
        }
    });
}

void Tracker::SetCoverage(Range range, Reasons reasons) {
    Visit(range, false, [&](auto, auto offset, auto bytes, auto* region) {
        if (region == nullptr) return;
        Advance(region->mapEpoch);
        for (std::size_t i = 0; i < region->reasons.size(); ++i) {
            if (i == static_cast<std::size_t>(Reason::ExcludedImport)) continue;
            if (i == static_cast<std::size_t>(Reason::PendingGpu) && (reasons & Bit(Reason::PendingGpu)) == 0) continue;
            Assign(region->reasons[i], offset, bytes, (reasons & (1u << i)) != 0);
        }
        if (offset % PageBytes != 0) Assign(region->reasons[static_cast<std::size_t>(Reason::Unsupported)], offset, 1, true);
        if ((offset + bytes) % PageBytes != 0) Assign(region->reasons[static_cast<std::size_t>(Reason::Unsupported)], offset + bytes - 1, 1, true);
    });
}

void Tracker::ExcludeImport(Range range) {
    Visit(range, true, [&](auto, auto offset, auto bytes, auto* region) {
        if (region == nullptr) return;
        Advance(region->mapEpoch);
        Assign(region->reasons[static_cast<std::size_t>(Reason::ExcludedImport)], offset, bytes, true);
    });
}

void Tracker::MarkCpuWritten(Range range, Source source) {
    Visit(range, source != Source::SectionFault && source != Source::HostWrite, [&](auto, auto offset, auto bytes, auto* region) {
        if (region == nullptr) return;
        Advance(region->cpuEpoch);
        Assign(region->cpuDirty, offset, bytes, true);
        if (source == Source::SectionFault || source == Source::HostWrite) Assign(region->reasons[static_cast<std::size_t>(Reason::OpenWriter)], offset, bytes, true);
    });
    std::lock_guard lock(countsMutex);
    counts.sources[static_cast<std::size_t>(source)] += Pages(range);
    if (source == Source::SectionFault) counts.faultPages += Pages(range);
}

void Tracker::MarkGpuWritten(Range range, Source source) {
    Visit(range, true, [&](auto, auto offset, auto bytes, auto* region) {
        if (region == nullptr) return;
        Advance(region->gpuEpoch);
        Assign(region->gpuDirty, offset, bytes, true);
        Assign(region->reasons[static_cast<std::size_t>(Reason::PendingGpu)], offset, bytes, true);
    });
    std::lock_guard lock(countsMutex);
    counts.sources[static_cast<std::size_t>(source)] += Pages(range);
    if (source == Source::GpuDeclaration) counts.gpuPages += Pages(range);
}

void Tracker::PublishGpuWrites(Range range) { MarkGpuWritten(range, Source::Copyback); }

Reasons Tracker::PageReasons(const Region& region, std::size_t page) {
    Reasons result = 0;
    for (std::size_t i = 0; i < region.reasons.size(); ++i) if ((region.reasons[i][page / 64] & (1ull << (page % 64))) != 0) result |= 1u << i;
    return result;
}

bool Tracker::Covered(Reasons reasons, bool proof) {
    constexpr auto mechanisms = Bit(Reason::PrivateWatch) | Bit(Reason::ProtectedSection) | Bit(Reason::Immutable);
    constexpr auto blockers = Bit(Reason::OpenWriter) | Bit(Reason::ExcludedImport) | Bit(Reason::HostAlias) | Bit(Reason::Unsupported) | Bit(Reason::Unmapped) | Bit(Reason::PendingGpu);
    return (reasons & mechanisms) != 0 && (reasons & blockers) == 0 && (!proof || (reasons & Bit(Reason::PrivateWatch)) == 0);
}

Epoch Tracker::CaptureEpoch(Range range) const {
    Epoch token{range, {}, range.Valid() && !poisoned.load(std::memory_order_acquire)};
    Visit(range, false, [&](auto id, auto offset, auto bytes, auto* region) {
        if (region == nullptr) { token.covered = false; token.regions.push_back({id}); return; }
        token.regions.push_back({id, region->mapEpoch, region->cpuEpoch, region->gpuEpoch});
        if (region->mapEpoch == 0 || region->cpuEpoch == 0 || region->gpuEpoch == 0) token.covered = false;
        for (auto page = offset / PageBytes; page < (offset + bytes + PageBytes - 1) / PageBytes; ++page) if (!Covered(PageReasons(*region, page), true)) token.covered = false;
    });
    return token;
}

bool Tracker::DirtySince(Range range, const Epoch& token) const {
    if (token.range != range || !token.covered) return true;
    const auto current = CaptureEpoch(range);
    return !current.covered || current.regions != token.regions;
}

void Tracker::ObserveRead(Range range, GuestMemory::ReadSite site) {
    if (static_cast<std::size_t>(site) >= counts.reads.size()) site = GuestMemory::ReadSite::Unknown;
    std::array<ReadBytes, static_cast<std::size_t>(Kind::Count)> read{};
    if (!range.Valid()) read[static_cast<std::size_t>(Kind::Unknown)].total = range.bytes;
    else Visit(range, false, [&](auto, auto offset, auto bytes, auto* region) {
        for (auto cursor = offset; cursor < offset + bytes;) {
            const auto stop = std::min(offset + bytes, (cursor / PageBytes + 1) * PageBytes);
            const auto page = cursor / PageBytes;
            auto kind = Kind::Unknown;
            Reasons reasons = Bit(Reason::Unmapped);
            if (region != nullptr) {
                reasons = PageReasons(*region, page);
                for (std::size_t i = 0; i < region->kinds.size(); ++i) if ((region->kinds[i][page / 64] & (1ull << (page % 64))) != 0) kind = static_cast<Kind>(i);
                if ((reasons & (Bit(Reason::ExcludedImport) | Bit(Reason::HostAlias))) != 0) kind = Kind::Import;
            }
            auto& entry = read[static_cast<std::size_t>(kind)];
            entry.total += stop - cursor;
            if (Covered(reasons, false) && !poisoned.load(std::memory_order_acquire)) entry.covered += stop - cursor;
            cursor = stop;
        }
    });
    std::lock_guard lock(countsMutex);
    for (std::size_t i = 0; i < read.size(); ++i) {
        counts.reads[static_cast<std::size_t>(site)][i].covered += read[i].covered;
        counts.reads[static_cast<std::size_t>(site)][i].total += read[i].total;
    }
}

void Tracker::NotePoll(std::uint64_t pages, std::uint64_t ns, bool complete) {
    std::lock_guard lock(countsMutex);
    counts.polledPages += pages;
    counts.pollNs += ns;
    ++counts.pollCalls;
    if (!complete) ++counts.pollErrors;
}

void Tracker::ObservePolledPage(Range range) {
    bool privatePage = false;
    Visit(range, false, [&](auto, auto offset, auto, auto* region) {
        if (region != nullptr) privatePage = (region->kinds[static_cast<std::size_t>(Kind::Private)][offset / PageBytes / 64] & (1ull << (offset / PageBytes % 64))) != 0;
    });
    MarkCpuWritten(range, privatePage ? Source::PrivatePoll : Source::SectionCollect);
}

Statistics Tracker::Counts() const {
    Statistics result;
    { std::lock_guard lock(countsMutex); result = counts; }
    std::shared_lock lock(directoryMutex);
    result.regions = directory.size();
    result.metadataBytes = sizeof(*this) + directory.size() * (sizeof(Region) + sizeof(decltype(directory)::value_type) + 4 * sizeof(void*));
    result.poisoned = poisoned.load(std::memory_order_acquire);
    return result;
}

std::string Tracker::ProfileLine() const {
    constexpr std::array sites{"unknown", "capture", "dispatch-cache", "texture-compare", "texture-read", "buffer-upload", "index", "vertex", "registers", "indirect", "wait", "label", "scanout", "store", "mirror-refresh", "draw-cache"};
    constexpr std::array kinds{"private", "section", "import", "unknown"};
    static_assert(sites.size() == static_cast<std::size_t>(GuestMemory::ReadSite::Count));
    const auto snapshot = Counts();
    std::uint64_t observerMetadata = 0;
#ifdef _WIN32
    observerMetadata = GuestArena::GuestArenaPageObserverMetadataBytes_nid_no_patch();
#endif
    std::ostringstream line;
    line << "[page-tracker] cumulative covered/total bytes";
    for (std::size_t site = 0; site < sites.size(); ++site) {
        line << ' ' << sites[site] << '=';
        for (std::size_t kind = 0; kind < kinds.size(); ++kind) {
            const auto& read = snapshot.reads[site][kind];
            if (kind != 0) line << ',';
            line << kinds[kind] << ':' << read.covered << '/' << read.total;
        }
    }
    line << " polled-pages=" << snapshot.polledPages << " fault-observed-pages=" << snapshot.faultPages << " gpu-declared-pages=" << snapshot.gpuPages
         << " poll-calls=" << snapshot.pollCalls << " poll-ms=" << snapshot.pollNs / 1000000.0 << " poll-errors=" << snapshot.pollErrors
         << " regions=" << snapshot.regions << " metadata-bytes=" << snapshot.metadataBytes + observerMetadata << " observer-metadata-bytes=" << observerMetadata << " metadata-estimated=1 poisoned=" << snapshot.poisoned;
    line << " source-pages=";
    constexpr std::array sources{"private-poll", "section-fault", "host-write", "driver-store", "gpu-declaration", "bda-report", "bda-overflow", "copyback", "section-collect"};
    for (std::size_t i = 0; i < sources.size(); ++i) line << (i == 0 ? "" : ",") << sources[i] << ':' << snapshot.sources[i];
    line << '\n';
    return line.str();
}

bool Enabled() {
    static const bool enabled = [] { const auto* text = std::getenv("APS5_PAGE_TRACKER"); return text != nullptr && std::string_view(text) == "1"; }();
    return enabled;
}

Tracker* Get() {
    if (!Enabled()) return nullptr;
    static auto* tracker = new Tracker;
    return tracker;
}

void Initialize() {
    if (!Enabled()) return;
    static std::once_flag once;
    std::call_once(once, [] {
        auto* tracker = Get();
        const auto ranges = GuestAllocations::GuestAllocationsAcquireAll_nid_postfix();
        GuestAllocations::GuestAllocationsSetPageInvalidator_nid_no_patch(&allocationEvent);
        for (const auto& range : ranges) {
            tracker->RegisterRange({range->address, range->bytes}, Kind::Unknown, Bit(Reason::Unsupported));
#ifdef _WIN32
            if (!GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<void*>(range->address), range->bytes) || !GuestArena::GuestArenaWriteWatched_nid_postfix()) continue;
            const auto end = range->address + range->bytes;
            for (auto cursor = range->address; cursor < end;) {
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(reinterpret_cast<void*>(cursor), &memory, sizeof(memory)) != sizeof(memory)) break;
                const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                if (stop <= cursor) break;
                if (memory.State == MEM_COMMIT && memory.Type == MEM_PRIVATE) tracker->RegisterRange({cursor, stop - cursor}, Kind::Private, Bit(Reason::PrivateWatch));
                cursor = stop;
            }
#endif
        }
#ifdef _WIN32
        GuestArena::GuestArenaSetPageObserver_nid_no_patch(&mappingEvent);
#endif
        registered.store(true, std::memory_order_release);
    });
}

bool ObserversRegistered() { return registered.load(std::memory_order_acquire); }

namespace {
void writeAliases(Range range, Source source, bool gpu) {
    auto* tracker = Get();
    if (tracker == nullptr || !range.Valid()) return;
    struct Event { Tracker* tracker; Source source; bool gpu; } event{tracker, source, gpu};
    const auto visit = [](std::uintptr_t address, std::size_t bytes, void* context) {
        auto& event = *static_cast<Event*>(context);
        if (event.gpu) event.tracker->MarkGpuWritten({address, bytes}, event.source);
        else event.tracker->MarkCpuWritten({address, bytes}, event.source);
    };
    try {
#ifdef _WIN32
        GuestArena::GuestArenaVisitPageAliases_nid_no_patch(range.address, range.bytes, visit, &event);
#else
        visit(range.address, range.bytes, &event);
#endif
    } catch (...) { tracker->Poison(); }
}
}

void CpuWrite(Range range, Source source) { writeAliases(range, source, false); }
void GpuWrite(Range range, Source source) { writeAliases(range, source, true); }
void Publish(Range range) { writeAliases(range, Source::Copyback, true); }

void ReportIfDue(void (*print)(const char*)) {
    auto* tracker = Get();
    if (tracker == nullptr) return;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    static std::atomic<std::int64_t> last{now};
    auto previous = last.load(std::memory_order_relaxed);
    if (now - previous < 10000 || !last.compare_exchange_strong(previous, now)) return;
    const auto line = tracker->ProfileLine();
    print(line.c_str());
}

}
