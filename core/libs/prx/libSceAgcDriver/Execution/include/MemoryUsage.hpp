#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MEMORYUSAGE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MEMORYUSAGE_HPP

#include <cstdint>
#include <vector>

namespace AgcDriver {

struct PoolMemoryUsage {
    std::uint64_t retainedBuffers = 0;
    std::uint64_t retainedImages = 0;
    std::uint64_t slabAllocated = 0;
    std::uint64_t slabUnused = 0;
};

struct HeapMemoryUsage {
    std::uint64_t size = 0;
    std::uint64_t budget = 0;
    std::uint64_t usage = 0;
    bool deviceLocal = false;
};

struct DriverMemoryUsage {
    bool device = false;
    bool budgetAvailable = false;
    PoolMemoryUsage pool;
    std::uint64_t importedGuest = 0;
    std::uint64_t openKept = 0;
    std::uint64_t inFlightKept = 0;
    std::vector<HeapMemoryUsage> heaps;
};

}

extern "C" AgcDriver::DriverMemoryUsage AgcDriverMemoryUsage_nid_no_patch();

#endif
