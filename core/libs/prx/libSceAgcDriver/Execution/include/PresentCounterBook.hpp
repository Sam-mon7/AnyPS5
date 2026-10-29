#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PRESENTCOUNTERBOOK_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PRESENTCOUNTERBOOK_HPP

#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include <atomic>

namespace AgcDriver {

class PresentCounterBook {
public:
    void Attempt() {
        presentAttempts.fetch_add(1, std::memory_order_relaxed);
    }

    void Acquire(VkResult result) {
        if (result == VK_NOT_READY || result == VK_TIMEOUT) acquireDropped.fetch_add(1, std::memory_order_relaxed);
        else if (result == VK_ERROR_OUT_OF_DATE_KHR) outOfDate.fetch_add(1, std::memory_order_relaxed);
    }

    void Present(VkResult result) {
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) presented.fetch_add(1, std::memory_order_relaxed);
    }

    PresentCounters Snapshot() const {
        return {presentAttempts.load(std::memory_order_relaxed), presented.load(std::memory_order_relaxed),
            acquireDropped.load(std::memory_order_relaxed), outOfDate.load(std::memory_order_relaxed)};
    }

private:
    std::atomic<std::uint64_t> presented{0};
    std::atomic<std::uint64_t> presentAttempts{0};
    std::atomic<std::uint64_t> acquireDropped{0};
    std::atomic<std::uint64_t> outOfDate{0};
};

}

#endif
