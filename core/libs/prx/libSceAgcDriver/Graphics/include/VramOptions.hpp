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

inline bool VramWritable() {
    static const bool enabled = VramOption("APS5_VRAM_WRITABLE");
    return enabled;
}

inline bool VramCleanup() {
    static const bool enabled = VramOption("APS5_VRAM_CLEANUP");
    return enabled;
}

inline void ValidateVramOptions() {
    if (VramDescriptors() && !VramOption("APS5_VRAM_MIRRORS"))
        throw std::runtime_error("APS5_VRAM_DESCRIPTORS requires APS5_VRAM_MIRRORS=1");
    if (VramWritable() && !VramDescriptors())
        throw std::runtime_error("APS5_VRAM_WRITABLE requires APS5_VRAM_DESCRIPTORS=1");
    if (VramCleanup() && !VramOption("APS5_VRAM_DEFERRED"))
        throw std::runtime_error("APS5_VRAM_CLEANUP requires APS5_VRAM_DEFERRED=1");
#ifndef _WIN32
    if (VramWritable())
        throw std::runtime_error("APS5_VRAM_WRITABLE requires Windows guest page protection support");
#endif
    if (VramOption("APS5_VRAM_MIRRORS") && VramOption("APS5_RESIDENT_HEAP_MIRRORS"))
        throw std::runtime_error("APS5_VRAM_MIRRORS requires APS5_RESIDENT_HEAP_MIRRORS to be off");
}

}

#endif
