#ifndef ANYPS5_KYTY_GPU_SDL3_SDL_VULKAN_H
#define ANYPS5_KYTY_GPU_SDL3_SDL_VULKAN_H

#include "SDL3/SDL.h"
#include <cstdint>
#include <vulkan/vulkan.h>

const char* const* SDL_Vulkan_GetInstanceExtensions(std::uint32_t* count);
void* SDL_Vulkan_GetVkGetInstanceProcAddr();
bool SDL_Vulkan_CreateSurface(SDL_Window* window, VkInstance instance, const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface);

#endif
