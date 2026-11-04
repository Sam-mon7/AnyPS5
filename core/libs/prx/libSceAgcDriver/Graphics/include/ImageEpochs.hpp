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

enum class EpochSource : std::uint8_t { CpuStore, Hold, Open, Forget, TrackerStamp, GpuNote, Pending, Depth, Unwatch, Count };

struct EpochCensus {
    std::uint64_t selfStamps = 0;
    std::uint64_t otherStamps = 0;
    std::uint64_t selfNotes = 0;
    std::uint64_t otherNotes = 0;
    std::uint64_t pending = 0;
    std::uint64_t depth = 0;
    bool registered = false;
};

class ImageEpochs {
public:
    static ImageEpochs& Get();
    static bool Active();
    static bool CensusEnabled();
    void Register(StorageTexture& image, std::span<const EpochRange> ranges);
    void RegisterOwner(const void* owner, std::atomic<std::uint64_t>& epoch, std::span<const EpochRange> ranges);
    void Unregister(const StorageTexture& image);
    void UnregisterOwner(const void* owner);
    static void RegisterImage(StorageTexture& image, std::uint64_t cmaskAddress, std::uint64_t cmaskBytes);
    void Invalidate(std::uint64_t begin, std::uint64_t end, EpochSource source, const void* except);
    bool DrainIfNew();
    std::uint64_t Loss() const;
    bool Registered(const StorageTexture& image);
    EpochCensus Census(const StorageTexture& image);
    std::size_t CountImagePages(bool (*open)(std::uint64_t page));
    static void InstallHooks();
    static void Note(std::uint64_t begin, std::uint64_t end, EpochSource source, const void* except);
    static void NoteDropped();
    static void InvalidateAll();
    static void SetSuppressed(EpochSource source, bool suppressed);
};

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
