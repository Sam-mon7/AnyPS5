#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MEMORYLEDGER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MEMORYLEDGER_HPP

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>

namespace AgcDriver::Graphics {

PFN_vkGetDeviceProcAddr MemoryLedgerResolver(PFN_vkGetDeviceProcAddr resolver);
void MemoryLedgerSetProperties(const VkPhysicalDeviceMemoryProperties& properties);
bool MemoryLedgerEnabled();
std::string MemoryLedgerReport();

}

#endif
