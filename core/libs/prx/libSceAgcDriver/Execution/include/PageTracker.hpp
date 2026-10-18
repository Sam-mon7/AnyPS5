// SPDX-License-Identifier: GPL-2.0-or-later
// Geometry and range/bitmap iteration adapted from Kyty/SuperMedo graphics/host_gpu/
// regionDefinitions.h, memoryTracker.h and regionManager.h (no copyright headers in those files).
#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PAGETRACKER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PAGETRACKER_HPP

#include "GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>

namespace AgcDriver::PageTracker {

constexpr std::uint64_t PageShift = 12, RegionShift = 22;
constexpr std::uint64_t PageBytes = 1ull << PageShift, RegionBytes = 1ull << RegionShift;
constexpr std::size_t RegionPages = RegionBytes / PageBytes, BitmapWords = RegionPages / 64;
using Bitmap = std::array<std::uint64_t, BitmapWords>;
struct Range {
    std::uint64_t address = 0, bytes = 0;
    bool Valid() const { return address != 0 && bytes != 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address; }
    auto operator<=>(const Range&) const = default;
};

template<class Function>
bool Split(Range range, Function&& function) {
    if (!range.Valid()) return false;
    auto remaining = range.bytes;
    auto index = range.address / RegionBytes;
    auto offset = range.address % RegionBytes;
    while (remaining != 0) {
        const auto bytes = std::min(RegionBytes - offset, remaining);
        function(index++, offset, bytes);
        remaining -= bytes;
        offset = 0;
    }
    return true;
}

enum class Kind : std::uint8_t { Private, Section, Import, Unknown, Immutable, Count };
enum class Reason : std::uint8_t { PrivateWatch, ProtectedSection, Immutable, OpenWriter, ExcludedImport, HostAlias, Unsupported, Unmapped, PendingGpu, Count };
enum class Source : std::uint8_t { PrivatePoll, SectionFault, HostWrite, DriverStore, GpuDeclaration, BdaReport, BdaOverflow, Copyback, SectionCollect, Count };
using Reasons = std::uint32_t;
constexpr Reasons Bit(Reason reason) { return 1u << static_cast<unsigned>(reason); }

struct RegionToken {
    std::uint64_t regionId = 0, mapEpoch = 0, cpuEpoch = 0, gpuEpoch = 0;
    auto operator<=>(const RegionToken&) const = default;
};
class Tracker;
class Observation {
public:
    Observation() = default;
private:
    friend class Tracker;
    const Tracker* tracker = nullptr;
    std::uint64_t serial = 0;
    std::thread::id thread;
    std::vector<Range> ranges;
    std::vector<RegionToken> regions;
};
using Poll = bool (*)(Range);
struct Epoch {
    Range range;
    std::vector<RegionToken> regions;
    bool covered = false;
    const Tracker* observationTracker = nullptr;
};
struct DirtyPages {
    Epoch token;
    std::vector<Bitmap> regions;
    bool precise = false;
};
struct ReadBytes { std::uint64_t covered = 0, total = 0; };
struct Statistics {
    std::array<std::array<ReadBytes, static_cast<std::size_t>(Kind::Count)>, static_cast<std::size_t>(GuestMemory::ReadSite::Count)> reads{};
    std::array<std::uint64_t, static_cast<std::size_t>(Source::Count)> sources{};
    std::uint64_t polledPages = 0, faultPages = 0, gpuPages = 0, pollCalls = 0, pollNs = 0, pollErrors = 0;
    std::uint64_t regions = 0, metadataBytes = 0;
    std::uint64_t uniquePolledPages = 0, framePolledPages = 0, fallbackPolledPages = 0;
    bool poisoned = false;
};

class Tracker {
public:
    void RegisterRange(Range range, Kind kind, Reasons reasons);
    void InvalidateRange(Range range, bool unmapped = false);
    void SetCoverage(Range range, Reasons reasons);
    void ExcludeImport(Range range);
    void MarkCpuWritten(Range range, Source source);
    void MarkGpuWritten(Range range, Source source);
    void PublishGpuWrites(Range range);
    void ReadbackGpuWrites(Range range);
    bool Current(Range range, const Observation* observation) const;
    Epoch CaptureEpoch(Range range, const Observation* observation = nullptr) const;
    bool DirtySince(Range range, const Epoch& token, const Observation* observation = nullptr) const;
    DirtyPages DirtyPagesSince(Range range, const Epoch& token) const;
    void ObserveRead(Range range, GuestMemory::ReadSite site);
    void NotePoll(std::uint64_t pages, std::uint64_t ns, bool complete);
    void ObservePolledPage(Range range);
    void Checkpoint(bool frame);
    Statistics Counts() const;
    std::string ProfileLine() const;
    void Poison() noexcept { poisoned.store(true, std::memory_order_release); }
    bool Poisoned() const noexcept { return poisoned.load(std::memory_order_acquire); }

private:
    friend Observation PollCpuWrites(std::span<const Range> ranges);
    Observation Observe(std::span<const Range> ranges, Poll poll);
    struct Region {
        struct PageHistory {
            std::array<std::uint64_t, RegionPages> cpu{};
            std::uint64_t invalidated = 0;
        };
        mutable std::mutex mutex;
        std::unique_ptr<PageHistory> pages;
        Bitmap cpuDirty{}, gpuDirty{}, observedPrivate{};
        std::array<Bitmap, static_cast<std::size_t>(Reason::Count)> reasons{};
        std::array<Bitmap, static_cast<std::size_t>(Kind::Count)> kinds{};
        std::uint64_t mapEpoch = 1, cpuEpoch = 1, gpuEpoch = 1;
    };
    Region* GetRegion(std::uint64_t id, bool create) const;
    template<class Function> void Visit(Range range, bool create, Function&& function) const {
        Split(range, [&](auto id, auto offset, auto bytes) {
            auto* region = GetRegion(id, create);
            if (region == nullptr) { function(id, offset, bytes, region); return; }
            std::lock_guard lock(region->mutex);
            function(id, offset, bytes, region);
        });
    }
    static void Advance(std::uint64_t& epoch);
    static Reasons PageReasons(const Region& region, std::size_t page);
    static bool Covered(Reasons reasons, bool proof);
    mutable std::shared_mutex directoryMutex;
    mutable std::map<std::uint64_t, std::unique_ptr<Region>> directory;
    mutable std::mutex countsMutex;
    Statistics counts;
    std::atomic<bool> poisoned{false};
    std::atomic<std::uint64_t> observationSerial{0};
};

bool Enabled();
bool ProofsEnabled();
Observation PollCpuWrites(std::span<const Range> ranges);
bool ArmEnabled();
bool PrepareReadbacks();
bool ResolveReadback(void* context, bool (*download)(void*, bool) noexcept) noexcept;
void CompletedFrame();
Tracker* Get();
void Initialize();
void ReportIfDue(void (*print)(const char*));
bool ObserversRegistered();
void CpuWrite(Range range, Source source = Source::DriverStore);
void GpuWrite(Range range, Source source = Source::GpuDeclaration);
void Publish(Range range);

}
#endif
