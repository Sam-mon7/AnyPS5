#ifndef ANYPS5_KYTY_GPU_SDL_VULKAN_H
#define ANYPS5_KYTY_GPU_SDL_VULKAN_H

#include "SDL.h"
#include <vulkan/vulkan.h>

SDL_bool SDL_Vulkan_GetInstanceExtensions(SDL_Window* window, unsigned int* count, const char** names);
void* SDL_Vulkan_GetVkGetInstanceProcAddr();
SDL_bool SDL_Vulkan_CreateSurface(SDL_Window* window, VkInstance instance, VkSurfaceKHR* surface);

#endif
