#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_VRAMOPTIONS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_VRAMOPTIONS_HPP

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace AgcDriver::Graphics {

inline bool VramOption(const char* name) {
    const auto* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

inline bool VramDescriptors() {
    static const bool enabled = VramOption("APS5_VRAM_DESCRIPTORS");
    return enabled;
}

inline void ValidateVramOptions() {
    if (VramDescriptors() && !VramOption("APS5_VRAM_MIRRORS"))
        throw std::runtime_error("APS5_VRAM_DESCRIPTORS requires APS5_VRAM_MIRRORS=1");
    if (VramOption("APS5_VRAM_MIRRORS") && VramOption("APS5_RESIDENT_HEAP_MIRRORS"))
        throw std::runtime_error("APS5_VRAM_MIRRORS requires APS5_RESIDENT_HEAP_MIRRORS to be off");
}

}

#endif
