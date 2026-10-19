#ifndef ANYPS5_KYTY_ADAPTERS_GRAPHICS_HOST_GPU_VULKANCOMMON_H
#define ANYPS5_KYTY_ADAPTERS_GRAPHICS_HOST_GPU_VULKANCOMMON_H

#include <vulkan/vulkan.hpp>

namespace Libs::Graphics {

template <typename Handle, typename... Arguments>
void SetVulkanObjectNameF(const vk::Device&, const Handle&, Arguments&&...) {}

void RequireVulkanSuccess(vk::Result result, const char* operation);

}

#endif
