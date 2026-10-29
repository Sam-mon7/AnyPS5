#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PRESENTATION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PRESENTATION_HPP

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdint>
#include <exception>
#include <span>
#include <memory>
#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"

namespace AgcDriver {

class FrameTiming;

struct PresentCounters {
    std::uint64_t attempts, presented, acquireDropped, outOfDate;
};

struct PresentationWindow {
    void* context;
    std::span<const char* const> extensions;
    VkSurfaceKHR (*createSurface)(void* context, VkInstance instance);
    void (*getDrawableSize)(void* context, std::uint32_t* width, std::uint32_t* height);
    std::uint32_t width;
    std::uint32_t height;
    std::shared_ptr<FrameTiming> timing;
};

}

extern "C" void AgcDriverAttachWindow_nid_postfix(const AgcDriver::PresentationWindow& window);
extern "C" void AgcDriverPresentClear_nid_postfix(const AgcDriver::PresentationWindow& window, bool opaque, void (*gpuReady)(void*), void* context);
extern "C" void AgcDriverPresentBuffer_nid_postfix(const AgcDriver::PresentationWindow& window, const AgcDriver::DisplayBuffer& buffer, void (*gpuReady)(void*), void* context);
extern "C" void AgcDriverPresentCounters_nid_postfix(AgcDriver::PresentCounters* counters);
extern "C" void AgcDriverReleaseWindow_nid_postfix(void* window);
extern "C" void AgcDriverTogglePerformanceOverlay_nid_postfix();
extern "C" void AgcDriverReportFailure_nid_postfix(std::exception_ptr error);

#endif
