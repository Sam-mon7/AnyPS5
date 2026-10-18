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

inline bool VramDeviceLocalMirrors() {
    static const bool enabled = VramOption("APS5_VRAM_DEVICE_LOCAL_MIRRORS");
    return enabled;
}

inline bool VramWritable() {
    static const bool enabled = VramOption("APS5_VRAM_WRITABLE");
    return enabled;
}

inline bool VramLazyWrites() {
    static const bool enabled = VramOption("APS5_VRAM_LAZY_WRITES");
    return enabled;
}

inline bool VramMirrorWrites() { return VramWritable() || VramLazyWrites(); }

inline bool VramWritableStrict() {
    static const bool enabled = VramOption("APS5_VRAM_WRITABLE_STRICT");
    return enabled;
}

inline bool VramDeferred() {
    static const bool enabled = VramOption("APS5_VRAM_DEFERRED");
    return enabled;
}

inline bool VramCleanup() {
    static const bool enabled = VramOption("APS5_VRAM_CLEANUP");
    return enabled;
}

inline bool VramTextures() {
    static const bool enabled = VramOption("APS5_VRAM_TEXTURES");
    return enabled;
}

inline bool TextureAgeEviction() {
    static const bool enabled = VramOption("APS5_TEXTURE_AGE_EVICTION");
    return enabled;
}

inline void ValidateVramOptions() {
    if (VramLazyWrites() && !VramDeviceLocalMirrors())
        throw std::runtime_error("APS5_VRAM_LAZY_WRITES requires APS5_VRAM_DEVICE_LOCAL_MIRRORS=1");
    if (VramLazyWrites() && VramWritable())
        throw std::runtime_error("APS5_VRAM_LAZY_WRITES replaces APS5_VRAM_WRITABLE; do not enable both");
    if (VramDeviceLocalMirrors() && !VramDescriptors())
        throw std::runtime_error("APS5_VRAM_DEVICE_LOCAL_MIRRORS requires APS5_VRAM_DESCRIPTORS=1");
    if (VramDeviceLocalMirrors() && VramOption("APS5_RESIDENT_HEAP_MIRRORS"))
        throw std::runtime_error("APS5_VRAM_DEVICE_LOCAL_MIRRORS requires APS5_RESIDENT_HEAP_MIRRORS=0");
    if (VramDescriptors() && !VramOption("APS5_VRAM_MIRRORS"))
        throw std::runtime_error("APS5_VRAM_DESCRIPTORS requires APS5_VRAM_MIRRORS=1");
    if (VramWritable() && !VramDescriptors())
        throw std::runtime_error("APS5_VRAM_WRITABLE requires APS5_VRAM_DESCRIPTORS=1");
    if (VramWritableStrict() && !VramWritable())
        throw std::runtime_error("APS5_VRAM_WRITABLE_STRICT requires APS5_VRAM_WRITABLE=1");
    if (VramDeferred() && !VramWritable())
        throw std::runtime_error("APS5_VRAM_DEFERRED requires APS5_VRAM_WRITABLE=1");
    if (VramCleanup() && !VramDescriptors())
        throw std::runtime_error("APS5_VRAM_CLEANUP requires APS5_VRAM_DESCRIPTORS=1");
    if (VramTextures() && !VramCleanup())
        throw std::runtime_error("APS5_VRAM_TEXTURES requires APS5_VRAM_CLEANUP=1");
    if (VramTextures() && !VramDeferred())
        throw std::runtime_error("APS5_VRAM_TEXTURES requires APS5_VRAM_DEFERRED=1");
#ifndef _WIN32
    if (VramMirrorWrites())
        throw std::runtime_error("APS5_VRAM_WRITABLE requires Windows guest page protection support");
#endif
    if (VramOption("APS5_VRAM_MIRRORS") && VramOption("APS5_RESIDENT_HEAP_MIRRORS"))
        throw std::runtime_error("APS5_VRAM_MIRRORS requires APS5_RESIDENT_HEAP_MIRRORS to be off");
}

}

#endif
