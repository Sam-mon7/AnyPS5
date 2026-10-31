#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HOSTALLOCATOR_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HOSTALLOCATOR_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace AgcDriver::HostAllocator {

constexpr std::size_t Granule = 16;
constexpr std::size_t LargestCached = 1024;
constexpr std::size_t Classes = LargestCached / Granule;
constexpr std::size_t SmallClasses = 256 / Granule;
constexpr std::uint16_t KeptLists = 8;
constexpr std::uint16_t KeptSmallListsWithFix = 128;

enum class Event {
    CacheHit, DepotTake, DepotKeep, Malloc, DepotFullFree,
    UnsizedRelease, SizeQuery, NoCacheFree, Count
};

constexpr const char* Names[] = {
    "host_allocator_cache_hits", "host_allocator_depot_takes", "host_allocator_depot_keeps", "host_allocator_malloc_fallthroughs",
    "host_allocator_depot_full_frees", "host_allocator_unsized_releases", "host_allocator_size_queries", "host_allocator_no_cache_frees"
};
constexpr std::size_t BucketedEvents = 5;
static_assert(std::size(Names) == static_cast<std::size_t>(Event::Count));
inline constinit std::array<std::array<std::atomic<std::uint64_t>, Classes + 1>, static_cast<std::size_t>(Event::Count)> counters{};

// Allocation-site census (APS5_ARCHB_ALLOC_SITE_CENSUS=1, Windows only). Samples about one in SiteMeanInterval
// allocations and frees of at most SiteLargestSampled bytes per thread and records the caller (operator new/delete's
// return address), up to two parent frames, size class, thread and outcome (cache or heap) into a fixed table that is
// written at exit to APS5_ARCHB_ALLOC_SITE_CENSUS_PATH (default alloc-sites-<pid>.tsv in APS5_TRACE_DIR or the current
// directory). Samples are split by the number of flips handed to video outputs so far (GpuReady, counted here),
// so warm frames can be told from setup.
constexpr std::size_t SiteLargestSampled = 128;
constexpr std::uint32_t SiteMeanInterval = 64;
constexpr std::size_t SiteEpochs = 32;
inline constinit std::atomic<std::uint32_t> siteFlips{0};

#ifdef _WIN32
std::size_t DepotCapacity(std::size_t sizeClass) noexcept;
std::size_t DepotLists(std::size_t sizeClass) noexcept;

enum class SiteKind : std::uint32_t { AllocCached, AllocHeap, FreeCached, FreeHeap, Count };
struct SiteTotals {
    std::uint64_t entries = 0;
    std::uint64_t dropped = 0;
    std::uint64_t walks = 0;
    std::uint64_t samples[static_cast<std::size_t>(SiteKind::Count)]{};
};
// Sampled counts so far whose caller lies in [low, high) (all kinds when kind is Count), and whether a parent frame did.
std::uint64_t SiteSamples(SiteKind kind, const void* low, const void* high, bool parent = false) noexcept;
SiteTotals SiteCensusTotals() noexcept;
bool WriteSiteCensus(const char* path) noexcept;
#endif

}

#endif
