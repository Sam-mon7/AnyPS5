#ifndef ANYPS5_KYTY_ADAPTERS_GRAPHICS_HOST_GPU_GRAPHICCONTEXT_H
#define ANYPS5_KYTY_ADAPTERS_GRAPHICS_HOST_GPU_GRAPHICCONTEXT_H

#include "graphics/host_gpu/vulkanCommon.h"
#include <cstdint>
#include <functional>
#include <utility>

VK_DEFINE_HANDLE(VmaAllocator)

namespace Libs::Graphics {

struct GraphicContext {
    vk::Device device;
    vk::PhysicalDevice physical_device;
    VmaAllocator allocator = nullptr;
    vk::PhysicalDeviceProperties physical_device_properties;
    std::function<std::pair<std::uint64_t, std::uint64_t>()> memory;

    [[nodiscard]] bool CanReportMemoryUsage() const { return static_cast<bool>(memory); }
    [[nodiscard]] std::uint64_t GetTotalMemoryBudget() const { return memory ? memory().first : 0; }
    [[nodiscard]] std::uint64_t GetDeviceMemoryUsage() const { return memory ? memory().second : 0; }
    void LogMemoryBudget() const {}
};

}

#endif
