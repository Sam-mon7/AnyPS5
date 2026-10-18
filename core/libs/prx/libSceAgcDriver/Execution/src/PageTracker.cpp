#include "prx/libSceAgcDriver/Execution/include/PageTracker.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>
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
std::atomic<std::int64_t> observationStarted{0};
constexpr std::array readSites{"unknown", "capture", "dispatch-cache", "texture-compare", "texture-read", "buffer-upload", "index", "vertex", "registers", "indirect", "wait", "label", "scanout", "store", "mirror-refresh", "draw-cache"};

#ifdef _WIN32
struct ReadbackRequest {
    void* context;
    bool (*download)(void*, bool) noexcept;
    ReadbackRequest* next = nullptr;
    std::atomic<unsigned> done{0};
    bool result = false;
};
static_assert(std::atomic<ReadbackRequest*>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free);
struct ReadbackWorker {
    std::atomic<ReadbackRequest*> requests{nullptr};
    HANDLE wake = nullptr;
    decltype(&WaitOnAddress) wait = nullptr;
    decltype(&WakeByAddressSingle) signal = nullptr;
    ReadbackWorker() {
        const auto module = GetModuleHandleW(L"kernelbase.dll");
        wait = reinterpret_cast<decltype(wait)>(GetProcAddress(module, "WaitOnAddress"));
        signal = reinterpret_cast<decltype(signal)>(GetProcAddress(module, "WakeByAddressSingle"));
        if (wait == nullptr || signal == nullptr) throw std::runtime_error("readback address waits unavailable");
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (wake == nullptr) throw std::runtime_error("readback event creation failed");
        try {
            std::thread([this] {
                for (;;) {
                    WaitForSingleObject(wake, INFINITE);
                    auto* request = requests.exchange(nullptr, std::memory_order_acquire);
                    while (request != nullptr) {
                        auto* next = request->next;
                        {
                            std::lock_guard gpu(GuestMemory::GpuMutex());
                            request->result = request->download(request->context, false);
                        }
                        request->done.store(1, std::memory_order_release);
                        signal(&request->done);
                        request = next;
                    }
                }
            }).detach();
        } catch (...) { CloseHandle(wake); throw; }
    }
};
std::atomic<ReadbackWorker*> readbackWorker{nullptr};

bool redZoneMarker() {
    static const bool present = [] {
        const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
        if (base == nullptr) return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const auto& section = sections[i];
            if (std::memcmp(section.Name, ".redzone", 8) != 0 || section.Misc.VirtualSize < 6 || section.VirtualAddress > nt->OptionalHeader.SizeOfImage - 6) continue;
            if (std::memcmp(base + section.VirtualAddress, "APS5RZ", 6) == 0) return true;
        }
        return false;
    }();
    return present;
}

std::uint64_t percentile(const GuestArena::PageTiming& timing, unsigned percent) {
    const auto target = (timing.calls * percent + 99) / 100;
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < timing.histogram.size(); ++i) {
        seen += timing.histogram[i];
        if (target != 0 && seen >= target) return i + 1 == timing.histogram.size() ? timing.maxNs : std::min(timing.maxNs, (1ull << i) - 1);
    }
    return 0;
}

std::string observationProfile(const Statistics& reads) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    static GuestArena::PageObservationStatistics previous{};
    static Statistics previousReads{};
    static std::int64_t last = 0;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto since = last != 0 ? last : observationStarted.load(std::memory_order_acquire);
    const auto elapsed = since != 0 ? (now - since) / 1000000.0 : 0.0;
    const auto counts = GuestArena::GuestArenaPageObservationCounts_nid_no_patch();
    const auto frames = counts.frames - previous.frames;
    std::ostringstream line;
    line << " arm=1 cadence=completed-frame fallback-ms=1000 redzone-marker=" << (redZoneMarker() ? "APS5RZ" : "missing-warning")
         << " elapsed-ms=" << elapsed << " frames=" << frames << '/' << counts.frames << " fallback-windows=" << counts.fallbackWindows - previous.fallbackWindows << '/' << counts.fallbackWindows;
    constexpr std::array faults{"g3-only", "legacy-section", "k3-read", "k3-write", "rejected-av", "stale-retry", "nested"};
    line << " faults-window/total,latency-us-mean/p95-upper/max-total=";
    for (std::size_t i = 0; i < faults.size(); ++i) {
        const auto& timing = counts.faults[i];
        line << (i == 0 ? "" : ",") << faults[i] << ':' << timing.calls - previous.faults[i].calls << '/' << timing.calls << ':'
             << (timing.calls == 0 ? 0.0 : timing.ns / (1000.0 * timing.calls)) << '/' << percentile(timing, 95) / 1000.0 << '/' << timing.maxNs / 1000.0;
    }
    const auto timing = [&](const char* name, const GuestArena::PageTiming& value, const GuestArena::PageTiming& old) {
        line << ' ' << name << "-calls=" << value.calls - old.calls << '/' << value.calls << " ms=" << (value.ns - old.ns) / 1000000.0 << '/' << value.ns / 1000000.0
             << " us-mean/p95-upper/max-total=" << (value.calls == 0 ? 0.0 : value.ns / (1000.0 * value.calls)) << '/' << percentile(value, 95) / 1000.0 << '/' << value.maxNs / 1000.0;
    };
    timing("protect", counts.protection, previous.protection);
    timing("checkpoint", counts.checkpoint, previous.checkpoint);
    timing("mapping-lock-wait", counts.mappingWait, previous.mappingWait);
    line << " protect-pages=" << counts.protectionPages - previous.protectionPages << '/' << counts.protectionPages
         << " physical-cpu-pages=" << counts.physicalPages - previous.physicalPages << '/' << counts.physicalPages
         << " alias-page-events=" << counts.aliasPages - previous.aliasPages << '/' << counts.aliasPages
         << " armed=" << counts.armedPages - previous.armedPages << '/' << counts.armedPages << " rearmed=" << counts.rearmedPages - previous.rearmedPages << '/' << counts.rearmedPages << " outstanding=" << counts.outstanding
         << " fallback-pages=" << counts.fallbackPages - previous.fallbackPages << '/' << counts.fallbackPages << " fallback-faults=" << counts.fallbackFaults - previous.fallbackFaults << '/' << counts.fallbackFaults;
    line << " protect-pages/call=";
    if (counts.protection.calls == previous.protection.calls) line << "N/A";
    else line << (counts.protectionPages - previous.protectionPages) / static_cast<double>(counts.protection.calls - previous.protection.calls);
    line << " private-poll-unique-pages=" << reads.uniquePolledPages - previousReads.uniquePolledPages << '/' << reads.uniquePolledPages
         << " fallback-private-poll-pages=" << reads.fallbackPolledPages - previousReads.fallbackPolledPages << '/' << reads.fallbackPolledPages;
    line << " physical-pages/frame=";
    if (frames == 0) line << "N/A";
    else line << (counts.framePages - previous.framePages + reads.framePolledPages - previousReads.framePolledPages) / static_cast<double>(frames);
    line << " section-pages/frame=";
    if (frames == 0) line << "N/A";
    else line << (counts.framePages - previous.framePages) / static_cast<double>(frames);
    line << " private-poll-pages/frame=";
    if (frames == 0) line << "N/A";
    else line << (reads.framePolledPages - previousReads.framePolledPages) / static_cast<double>(frames);
    line << " faults/frame=";
    if (frames == 0) line << "N/A";
    else line << (counts.frameFaults - previous.frameFaults) / static_cast<double>(frames);
    GuestArena::PageTiming frameFaults{counts.frames, 0, counts.maxFrameFaults, counts.frameFaultHistogram};
    line << " faults/frame-p95-upper/max-total=" << percentile(frameFaults, 95) << '/' << counts.maxFrameFaults << " physical-pages/frame-max-total=" << counts.maxFramePages;
    constexpr std::array refusals{"invalid-range", "unknown-mapping", "identity", "metadata", "permission", "pinned", "host-writer", "host-alias", "protect-failed"};
    line << " arming-refusals-window/total=";
    for (std::size_t i = 0; i < refusals.size(); ++i) line << (i == 0 ? "" : ",") << refusals[i] << ':' << counts.refusals[i] - previous.refusals[i] << '/' << counts.refusals[i];
    constexpr std::array kinds{"private", "section", "import", "unknown", "immutable"};
    std::uint64_t totalBytes = 0, importBytes = 0;
    line << " window-covered/total-by-site=";
    for (std::size_t site = 0; site < reads.reads.size(); ++site) {
        line << (site == 0 ? "" : ";") << readSites[site] << ':';
        for (std::size_t kind = 0; kind < kinds.size(); ++kind) {
            const auto& value = reads.reads[site][kind];
            const auto& old = previousReads.reads[site][kind];
            const auto bytes = value.total - old.total;
            totalBytes += bytes;
            if (kind == static_cast<std::size_t>(Kind::Import)) importBytes += bytes;
            line << (kind == 0 ? "" : ",") << kinds[kind] << ':' << value.covered - old.covered << '/' << bytes;
        }
    }
    line << " import-read-share=";
    if (totalBytes == 0) line << "N/A";
    else line << 100.0 * importBytes / totalBytes << '%';
    previous = counts;
    previousReads = reads;
    last = now;
    return line.str();
}
#endif

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
        case GuestArena::PageEvent::ObservationFault: tracker->MarkCpuWritten(range, Source::SectionFault); break;
        case GuestArena::PageEvent::HostWrite: tracker->MarkCpuWritten(range, Source::HostWrite); break;
        case GuestArena::PageEvent::GpuReadback: tracker->ReadbackGpuWrites(range); break;
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
    mappingGeneration.fetch_add(1, std::memory_order_acq_rel);
    Visit(range, true, [&](auto, auto offset, auto bytes, auto* region) {
        Advance(region->mapEpoch);
        if (region->pages != nullptr) region->pages->invalidated = region->mapEpoch;
        Assign(region->declared, offset, bytes, false);
        Assign(region->cpuDirty, offset, bytes, true);
        Assign(region->gpuDirty, offset, bytes, false);
        for (std::size_t i = 0; i < region->kinds.size(); ++i) Assign(region->kinds[i], offset, bytes, i == static_cast<std::size_t>(kind));
        for (std::size_t i = 0; i < region->reasons.size(); ++i) Assign(region->reasons[i], offset, bytes, (reasons & (1u << i)) != 0);
        if (offset % PageBytes != 0) Assign(region->reasons[static_cast<std::size_t>(Reason::Unsupported)], offset, 1, true);
        if ((offset + bytes) % PageBytes != 0) Assign(region->reasons[static_cast<std::size_t>(Reason::Unsupported)], offset + bytes - 1, 1, true);
    });
}

void Tracker::InvalidateRange(Range range, bool unmapped) {
    mappingGeneration.fetch_add(1, std::memory_order_acq_rel);
    Visit(range, true, [&](auto, auto offset, auto bytes, auto* region) {
        if (region == nullptr) return;
        Advance(region->mapEpoch);
        if (region->pages != nullptr) region->pages->invalidated = region->mapEpoch;
        Assign(region->declared, offset, bytes, false);
        Assign(region->reasons[static_cast<std::size_t>(unmapped ? Reason::Unmapped : Reason::Unsupported)], offset, bytes, true);
        if (unmapped) {
            for (std::size_t i = 0; i < region->kinds.size(); ++i) Assign(region->kinds[i], offset, bytes, i == static_cast<std::size_t>(Kind::Unknown));
        }
    });
}

void Tracker::SetCoverage(Range range, Reasons reasons) {
    mappingGeneration.fetch_add(1, std::memory_order_acq_rel);
    Visit(range, false, [&](auto, auto offset, auto bytes, auto* region) {
        if (region == nullptr) return;
        Assign(region->declared, offset, bytes, false);
        const bool privatePoll = ProofsEnabled() && (reasons & Bit(Reason::PrivateWatch)) != 0;
        bool changed = false;
        bool pageHistoryChanged = false;
        if (region->pages != nullptr) {
            for (auto page = offset / PageBytes; page < (offset + bytes + PageBytes - 1) / PageBytes; ++page) {
                const auto before = PageReasons(*region, page);
                const auto after = reasons | (before & (Bit(Reason::ExcludedImport) | Bit(Reason::PendingGpu)));
                if ((before & ~Bit(Reason::OpenWriter)) != (after & ~Bit(Reason::OpenWriter)) || ((before & Bit(Reason::OpenWriter)) == 0 && (after & Bit(Reason::OpenWriter)) != 0)) pageHistoryChanged = true;
            }
        }
        for (std::size_t i = 0; i < region->reasons.size(); ++i) {
            if (i == static_cast<std::size_t>(Reason::ExcludedImport)) continue;
            if (i == static_cast<std::size_t>(Reason::PendingGpu) && (reasons & Bit(Reason::PendingGpu)) == 0) continue;
            const bool set = (reasons & (1u << i)) != 0;
            if (privatePoll) Words(offset, bytes, [&](auto word, auto mask) { changed |= (region->reasons[i][word] & mask) != (set ? mask : 0); });
            Assign(region->reasons[i], offset, bytes, set);
        }
        if (offset % PageBytes != 0) Assign(region->reasons[static_cast<std::size_t>(Reason::Unsupported)], offset, 1, true);
        if ((offset + bytes) % PageBytes != 0) Assign(region->reasons[static_cast<std::size_t>(Reason::Unsupported)], offset + bytes - 1, 1, true);
        if (!privatePoll || changed || pageHistoryChanged) Advance(region->mapEpoch);
        if (pageHistoryChanged) region->pages->invalidated = region->mapEpoch;
    });
}

void Tracker::ExcludeImport(Range range) {
    Visit(range, true, [&](auto, auto offset, auto bytes, auto* region) {
        if (region == nullptr) return;
        Advance(region->mapEpoch);
        if (region->pages != nullptr) region->pages->invalidated = region->mapEpoch;
        Assign(region->reasons[static_cast<std::size_t>(Reason::ExcludedImport)], offset, bytes, true);
    });
}

void Tracker::MarkCpuWritten(Range range, Source source) {
    std::uint64_t uniquePolled = 0;
    Visit(range, source != Source::SectionFault && source != Source::HostWrite, [&](auto, auto offset, auto bytes, auto* region) {
        if (region == nullptr) return;
        Advance(region->cpuEpoch);
        if (region->pages != nullptr) {
            for (auto page = offset / PageBytes; page < (offset + bytes + PageBytes - 1) / PageBytes; ++page) region->pages->cpu[page] = region->cpuEpoch;
        }
        Assign(region->cpuDirty, offset, bytes, true);
        if (ArmEnabled() && source == Source::PrivatePoll) Words(offset, bytes, [&](auto word, auto mask) {
            uniquePolled += std::popcount(mask & ~region->observedPrivate[word]);
            region->observedPrivate[word] |= mask;
        });
        if (source == Source::SectionFault || source == Source::HostWrite) Assign(region->reasons[static_cast<std::size_t>(Reason::OpenWriter)], offset, bytes, true);
    });
    std::lock_guard lock(countsMutex);
    counts.sources[static_cast<std::size_t>(source)] += Pages(range);
    counts.uniquePolledPages += uniquePolled;
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

void Tracker::MarkGpuDeclared(Range range, Source source, std::uint64_t generation) {
    Visit(range, true, [&](auto, auto offset, auto bytes, auto* region) {
        if (region == nullptr) return;
        Advance(region->gpuEpoch);
        Assign(region->gpuDirty, offset, bytes, true);
        Assign(region->reasons[static_cast<std::size_t>(Reason::PendingGpu)], offset, bytes, true);
        if (MappingGeneration() == generation) Assign(region->declared, offset, bytes, true);
    });
    std::lock_guard lock(countsMutex);
    counts.sources[static_cast<std::size_t>(source)] += Pages(range);
    if (source == Source::GpuDeclaration) counts.gpuPages += Pages(range);
}

bool Tracker::DeclaredPending(Range range) const {
    if (!range.Valid() || Poisoned()) return false;
    bool pending = true;
    Visit(range, false, [&](auto, auto offset, auto bytes, auto* region) {
        if (!pending) return;
        if (region == nullptr) { pending = false; return; }
        const auto& gpu = region->reasons[static_cast<std::size_t>(Reason::PendingGpu)];
        Words(offset, bytes, [&](auto word, auto mask) { if ((region->declared[word] & gpu[word] & mask) != mask) pending = false; });
    });
    return pending;
}

void Tracker::NoteSkippedDeclaration(Range range) {
    std::lock_guard lock(countsMutex);
    counts.skippedGpuPages += Pages(range);
}

void Tracker::PublishGpuWrites(Range range) { MarkGpuWritten(range, Source::Copyback); }

void Tracker::ReadbackGpuWrites(Range range) {
    Visit(range, false, [&](auto, auto offset, auto bytes, auto* region) {
        if (region == nullptr) return;
        Advance(region->gpuEpoch);
        Assign(region->gpuDirty, offset, bytes, false);
        Assign(region->reasons[static_cast<std::size_t>(Reason::PendingGpu)], offset, bytes, false);
    });
}

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

Observation Tracker::Observe(std::span<const Range> ranges, Poll poll) {
    if (!ProofsEnabled() || Poisoned()) return {};
    const auto serial = observationSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (serial == 0) { Poison(); return {}; }
    Observation observation;
    observation.thread = std::this_thread::get_id();
    observation.ranges.assign(ranges.begin(), ranges.end());
    std::vector<Range> privateRanges;
    for (const auto range : ranges) {
        if (!range.Valid()) return {};
        Visit(range, false, [&](auto id, auto offset, auto bytes, auto* region) {
            observation.regions.push_back({id, region != nullptr ? region->mapEpoch : 0});
            if (region == nullptr) return;
            for (auto page = offset / PageBytes; page < (offset + bytes + PageBytes - 1) / PageBytes; ++page) {
                if ((PageReasons(*region, page) & Bit(Reason::PrivateWatch)) == 0) continue;
                const auto address = id * RegionBytes + page * PageBytes;
                if (!privateRanges.empty() && privateRanges.back().address + privateRanges.back().bytes == address) privateRanges.back().bytes += PageBytes;
                else privateRanges.push_back({address, PageBytes});
            }
        });
    }
    std::sort(observation.regions.begin(), observation.regions.end());
    for (std::size_t i = 1; i < observation.regions.size(); ++i) {
        if (observation.regions[i - 1].regionId == observation.regions[i].regionId && observation.regions[i - 1].mapEpoch != observation.regions[i].mapEpoch) return {};
    }
    observation.regions.erase(std::unique(observation.regions.begin(), observation.regions.end()), observation.regions.end());
    std::sort(privateRanges.begin(), privateRanges.end());
    std::vector<Range> polls;
    for (const auto range : privateRanges) {
        if (!polls.empty() && range.address <= polls.back().address + polls.back().bytes) {
            polls.back().bytes = std::max(polls.back().address + polls.back().bytes, range.address + range.bytes) - polls.back().address;
        } else polls.push_back(range);
    }
    try {
        for (const auto range : polls) if (poll == nullptr || !poll(range)) return {};
    } catch (...) { return {}; }
    if (Poisoned() || observationSerial.load(std::memory_order_acquire) != serial) return {};
    observation.tracker = this;
    observation.serial = serial;
    return observation;
}

bool Tracker::Current(Range range, const Observation* observation) const {
    if (observation == nullptr || observation->tracker != this || observation->serial == 0 || observation->thread != std::this_thread::get_id() || observationSerial.load(std::memory_order_acquire) != observation->serial || !range.Valid() || Poisoned()) return false;
    return std::any_of(observation->ranges.begin(), observation->ranges.end(), [&](const auto observed) {
        return observed.address <= range.address && range.address + range.bytes <= observed.address + observed.bytes;
    });
}

Epoch Tracker::CaptureEpoch(Range range, const Observation* observation) const {
    Epoch token{range, {}, range.Valid() && !poisoned.load(std::memory_order_acquire)};
    const bool observed = Current(range, observation);
    Visit(range, false, [&](auto id, auto offset, auto bytes, auto* region) {
        if (region == nullptr) { token.covered = false; token.regions.push_back({id}); return; }
        token.regions.push_back({id, region->mapEpoch, region->cpuEpoch, region->gpuEpoch});
        if (region->mapEpoch == 0 || region->cpuEpoch == 0 || region->gpuEpoch == 0) token.covered = false;
        bool privatePages = false;
        for (auto page = offset / PageBytes; page < (offset + bytes + PageBytes - 1) / PageBytes; ++page) {
            const auto reasons = PageReasons(*region, page);
            privatePages |= (reasons & Bit(Reason::PrivateWatch)) != 0;
            if (!Covered(reasons, !observed)) token.covered = false;
        }
        if (privatePages) {
            token.observationTracker = this;
            if (!observed || std::none_of(observation->regions.begin(), observation->regions.end(), [&](const auto& before) { return before.regionId == id && before.mapEpoch == region->mapEpoch; })) token.covered = false;
        }
    });
    if (token.observationTracker != nullptr && !Current(range, observation)) token.covered = false;
    return token;
}

bool Tracker::DirtySince(Range range, const Epoch& token, const Observation* observation) const {
    if (token.range != range || !token.covered) return true;
    if (token.observationTracker != nullptr && (token.observationTracker != this || !Current(range, observation))) return true;
    const auto current = CaptureEpoch(range, observation);
    return !current.covered || current.regions != token.regions;
}

DirtyPages Tracker::DirtyPagesSince(Range range, const Epoch& token) const {
    DirtyPages result;
    result.token = {range, {}, range.Valid() && !Poisoned()};
    result.precise = token.covered && token.range == range;
    std::size_t index = 0;
    Visit(range, false, [&](auto id, auto offset, auto bytes, auto* region) {
        auto& dirty = result.regions.emplace_back();
        if (region == nullptr) {
            result.token.regions.push_back({id});
            result.token.covered = result.precise = false;
            Assign(dirty, offset, bytes, true);
            ++index;
            return;
        }
        result.token.regions.push_back({id, region->mapEpoch, region->cpuEpoch, region->gpuEpoch});
        if (region->mapEpoch == 0 || region->cpuEpoch == 0 || region->gpuEpoch == 0) result.token.covered = false;
        const bool hasHistory = region->pages != nullptr;
        if (!hasHistory) {
            region->pages = std::make_unique<Region::PageHistory>();
            region->pages->cpu.fill(region->cpuEpoch);
            region->pages->invalidated = region->mapEpoch;
        }
        const auto before = index < token.regions.size() ? token.regions[index] : RegionToken{};
        const bool known = token.covered && token.range == range && before.regionId == id && before.mapEpoch != 0 &&
            before.mapEpoch >= region->pages->invalidated && before.mapEpoch <= region->mapEpoch &&
            before.cpuEpoch != 0 && before.cpuEpoch <= region->cpuEpoch && before.gpuEpoch == region->gpuEpoch &&
            (hasHistory || before.cpuEpoch == region->cpuEpoch);
        if (!known) result.precise = false;
        for (auto page = offset / PageBytes; page < (offset + bytes + PageBytes - 1) / PageBytes; ++page) {
            const bool covered = Covered(PageReasons(*region, page), true);
            if (!covered) result.token.covered = false;
            if (!known || !covered || region->pages->cpu[page] > before.cpuEpoch) dirty[page / 64] |= 1ull << (page % 64);
        }
        ++index;
    });
    result.precise = result.precise && result.token.covered && token.regions.size() == index;
    return result;
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
                else if (ArmEnabled() && (reasons & Bit(Reason::Immutable)) != 0) kind = Kind::Immutable;
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

void Tracker::Checkpoint(bool frame) {
    if (!ArmEnabled()) return;
    std::uint64_t pages = 0;
    {
        std::shared_lock lock(directoryMutex);
        for (const auto& [id, region] : directory) {
            std::lock_guard regionLock(region->mutex);
            for (auto& word : region->observedPrivate) { pages += std::popcount(word); word = 0; }
        }
    }
    std::lock_guard lock(countsMutex);
    if (frame) counts.framePolledPages += pages;
    else counts.fallbackPolledPages += pages;
}

Statistics Tracker::Counts() const {
    Statistics result;
    { std::lock_guard lock(countsMutex); result = counts; }
    std::shared_lock lock(directoryMutex);
    result.regions = directory.size();
    result.metadataBytes = sizeof(*this) + directory.size() * (sizeof(Region) + sizeof(decltype(directory)::value_type) + 4 * sizeof(void*));
    if (!ProofsEnabled()) result.metadataBytes -= sizeof(observationSerial);
    for (const auto& [id, region] : directory) {
        std::lock_guard regionLock(region->mutex);
        if (region->pages != nullptr) result.metadataBytes += sizeof(*region->pages);
        else result.metadataBytes -= sizeof(region->pages);
    }
    if (!ArmEnabled()) {
        constexpr auto armCountBytes = 3 * sizeof(std::uint64_t) + sizeof(ReadBytes) * static_cast<std::size_t>(GuestMemory::ReadSite::Count);
        constexpr auto armRegionBytes = 2 * sizeof(Bitmap);
        result.metadataBytes -= armCountBytes + directory.size() * armRegionBytes;
    }
    result.poisoned = poisoned.load(std::memory_order_acquire);
    return result;
}

std::string Tracker::ProfileLine() const {
    constexpr auto& sites = readSites;
    constexpr std::array kinds{"private", "section", "import", "unknown", "immutable"};
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
            if (kind == static_cast<std::size_t>(Kind::Immutable) && !ArmEnabled()) continue;
            const auto& read = snapshot.reads[site][kind];
            if (kind != 0) line << ',';
            line << kinds[kind] << ':' << read.covered << '/' << read.total;
        }
    }
    line << " polled-pages=" << snapshot.polledPages << " fault-observed-pages=" << snapshot.faultPages << " gpu-declared-pages=" << snapshot.gpuPages << " gpu-declaration-skipped-pages=" << snapshot.skippedGpuPages
         << " poll-calls=" << snapshot.pollCalls << " poll-ms=" << snapshot.pollNs / 1000000.0 << " poll-errors=" << snapshot.pollErrors
         << " regions=" << snapshot.regions << " metadata-bytes=" << snapshot.metadataBytes + observerMetadata << " observer-metadata-bytes=" << observerMetadata << " metadata-estimated=1 poisoned=" << snapshot.poisoned;
    line << " source-pages=";
    constexpr std::array sources{"private-poll", "section-fault", "host-write", "driver-store", "gpu-declaration", "bda-report", "bda-overflow", "copyback", "section-collect"};
    for (std::size_t i = 0; i < sources.size(); ++i) line << (i == 0 ? "" : ",") << sources[i] << ':' << snapshot.sources[i];
#ifdef _WIN32
    if (ArmEnabled() && this == Get()) line << observationProfile(snapshot);
#endif
    line << '\n';
    return line.str();
}

bool Enabled() {
    static const bool enabled = [] { const auto* text = std::getenv("APS5_PAGE_TRACKER"); return text != nullptr && std::string_view(text) == "1"; }();
    return enabled;
}

bool ProofsEnabled() {
    static const bool enabled = [] { const auto* value = std::getenv("APS5_EPOCH_PROOFS"); return Enabled() && value != nullptr && std::string_view(value) == "1"; }();
    return enabled;
}

bool ArmEnabled() {
    static const bool enabled = [] { const auto* text = std::getenv("APS5_PAGE_TRACKER_ARM"); return Enabled() && text != nullptr && std::string_view(text) == "1"; }();
    return enabled;
}

bool PrepareReadbacks() {
#ifdef _WIN32
    static const bool enabled = [] {
        if (!ArmEnabled()) return false;
        for (const auto* name : {"APS5_BUFFER_CACHE_READ_FAULTS", "APS5_BUFFER_CACHE_WRITES", "APS5_BUFFER_CACHE"}) {
            const auto* value = std::getenv(name);
            if (value == nullptr || std::string_view(value) != "1") return false;
        }
        return true;
    }();
    if (!enabled) return false;
    try {
        static auto* worker = new ReadbackWorker;
        readbackWorker.store(worker, std::memory_order_release);
        return true;
    } catch (const std::exception&) { return false; }
#else
    return false;
#endif
}

bool ResolveReadback(void* context, bool (*download)(void*, bool) noexcept) noexcept {
#ifdef _WIN32
    if (GuestMemory::GpuMutex().HeldByThisThread()) return download(context, true);
    auto* worker = readbackWorker.load(std::memory_order_acquire);
    if (worker == nullptr) return false;
    ReadbackRequest request{context, download};
    auto* head = worker->requests.load(std::memory_order_relaxed);
    do { request.next = head; } while (!worker->requests.compare_exchange_weak(head, &request, std::memory_order_release, std::memory_order_relaxed));
    SetEvent(worker->wake);
    unsigned waiting = 0;
    while (request.done.load(std::memory_order_acquire) == 0) worker->wait(&request.done, &waiting, sizeof(waiting), INFINITE);
    return request.result;
#else
    return false;
#endif
}

void CompletedFrame() {
#ifdef _WIN32
    if (ArmEnabled() && GuestArena::GuestArenaPageObservationCheckpoint_nid_no_patch(true)) Get()->Checkpoint(true);
#endif
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
        if (ArmEnabled()) {
            observationStarted.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_release);
            std::fprintf(stderr, "[page-tracker] arm=1 cadence=completed-frame fallback-ms=1000 redzone-marker=%s%s\n", redZoneMarker() ? "APS5RZ" : "missing", redZoneMarker() ? "" : " warning=executable-lacks-.redzone-APS5RZ");
            GuestArena::GuestArenaEnablePageObservation_nid_no_patch([]() noexcept { return !Get()->Poisoned(); });
            static std::jthread fallback([](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (stop.stop_requested()) break;
                    try {
                        if (GuestArena::GuestArenaPageObservationCheckpoint_nid_no_patch(false)) Get()->Checkpoint(false);
                        ReportIfDue([](const char* line) { std::fputs(line, stderr); });
                    } catch (...) { Get()->Poison(); }
                }
            });
        }
#endif
        registered.store(true, std::memory_order_release);
    });
}

bool ObserversRegistered() { return registered.load(std::memory_order_acquire); }

namespace {
void declareGpuAliases(Tracker& tracker, Range range, Source source) {
    const auto generation = tracker.MappingGeneration();
    if (tracker.DeclaredPending(range)) {
        tracker.NoteSkippedDeclaration(range);
        return;
    }
    std::vector<Range> aliases;
#ifdef _WIN32
    const auto collect = [](std::uintptr_t address, std::size_t bytes, void* context) {
        auto& ranges = *static_cast<std::vector<Range>*>(context);
        if (!ranges.empty() && ranges.back().address + ranges.back().bytes == address) ranges.back().bytes += bytes;
        else ranges.push_back({address, bytes});
    };
    GuestArena::GuestArenaVisitPageAliases_nid_no_patch(range.address, range.bytes, collect, &aliases);
    std::sort(aliases.begin(), aliases.end());
    std::vector<Range> merged;
    for (const auto alias : aliases) {
        if (!merged.empty() && alias.address <= merged.back().address + merged.back().bytes) merged.back().bytes = std::max(merged.back().address + merged.back().bytes, alias.address + alias.bytes) - merged.back().address;
        else merged.push_back(alias);
    }
    aliases = std::move(merged);
#else
    aliases.push_back(range);
#endif
    for (const auto alias : aliases) tracker.MarkGpuDeclared(alias, source, generation);
}

void writeAliases(Range range, Source source, bool gpu) {
    auto* tracker = Get();
    if (tracker == nullptr || !range.Valid()) return;
    if (gpu) {
        try { declareGpuAliases(*tracker, range, source); } catch (...) { tracker->Poison(); }
        return;
    }
    struct Event { Tracker* tracker; Source source; } event{tracker, source};
    const auto visit = [](std::uintptr_t address, std::size_t bytes, void* context) {
        auto& event = *static_cast<Event*>(context);
        event.tracker->MarkCpuWritten({address, bytes}, event.source);
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
