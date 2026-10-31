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

#ifdef _WIN32
std::size_t DepotCapacity(std::size_t sizeClass) noexcept;
std::size_t DepotLists(std::size_t sizeClass) noexcept;
#endif

}

#endif
