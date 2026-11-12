#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_IMAGEEPOCHS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_IMAGEEPOCHS_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace AgcDriver::Graphics {

class StorageTexture;

namespace ArchC {

struct EpochRange {
    std::uint64_t begin;
    std::uint64_t end;
};

// PendingInsert (Arch C P3b): an image entering the pending registry (also noted as Pending), the only event
// that can make a range newly overlap a pending image.
enum class EpochSource : std::uint8_t { CpuStore, Hold, Open, Forget, TrackerStamp, GpuNote, Pending, Depth, Unwatch, PendingInsert, Count };

constexpr std::uint32_t SourceBit(EpochSource source) {
    return std::uint32_t{1} << static_cast<unsigned>(source);
}

constexpr std::uint32_t AllSources = (std::uint32_t{1} << static_cast<unsigned>(EpochSource::Count)) - 1u;

struct EpochCensus {
    std::uint64_t selfStamps = 0;
    std::uint64_t otherStamps = 0;
    std::uint64_t selfNotes = 0;
    std::uint64_t otherNotes = 0;
    std::uint64_t pending = 0;
    std::uint64_t depth = 0;
    bool registered = false;
};

enum class EpochPublication : std::uint8_t { Inactive, Suppressed, NoRecipients, Published };

class ImageEpochs {
public:
    static ImageEpochs& Get();
    static bool Active();
    static bool CensusEnabled();
    void Register(StorageTexture& image, std::span<const EpochRange> ranges);
    void RegisterOwner(const void* owner, std::atomic<std::uint64_t>& epoch, std::span<const EpochRange> ranges, std::uint32_t sources = AllSources);
    void Unregister(const StorageTexture& image);
    void UnregisterOwner(const void* owner);
    static void RegisterImage(StorageTexture& image, std::uint64_t cmaskAddress, std::uint64_t cmaskBytes);
    EpochPublication Invalidate(std::uint64_t begin, std::uint64_t end, EpochSource source, const void* except);
    bool DrainIfNew();
    std::uint64_t Loss() const;
    bool Registered(const StorageTexture& image);
    EpochCensus Census(const StorageTexture& image);
    std::size_t CountImagePages(bool (*open)(std::uint64_t page));
    static void InstallHooks();
    static EpochPublication Note(std::uint64_t begin, std::uint64_t end, EpochSource source, const void* except);
    static void NoteDropped();
    static void InvalidateAll();
    static void SetSuppressed(EpochSource source, bool suppressed);
};

// Arch C P3b: sub-step probes of a draw replay (APS5_ARCHB_C_DRAW_REPLAY_STEPS=2; committer only;
// counters archc_replay_probe_ns_<name> and _n_<name> hold the samples under 40 us, _out_<name> the rest).
enum class Probe : std::uint8_t { RvPrelude, RvComplete, RvStamp, RvScan, RvImports, RvStaging, RvFinish, DvHtile, DvSettle, DvFind, DvPlanes, DvSync, DvWritten, DvStale, DvNote, InValidate, InIndex, BdReuse, BdFlush, BdLinear, BdPrepare, RcStart, RcTiming, RcBegin, RcBeginNew, RcBind, RcPush, RcDraw, RcLeave, KpKept, KpStorage, KpMarkGpu, KpComplete, KpDirty, Count };
extern std::atomic<bool> probing;
void ProbeMark(Probe probe);
inline void ProbeAt(Probe probe) {
    if (probing.load(std::memory_order_relaxed)) ProbeMark(probe);
}

class SelfScope {
public:
    explicit SelfScope(const StorageTexture* image);
    ~SelfScope();
    SelfScope(const SelfScope&) = delete;
    SelfScope& operator=(const SelfScope&) = delete;
    static const StorageTexture* Current();

private:
    const StorageTexture* previous;
};

}

}

#endif
