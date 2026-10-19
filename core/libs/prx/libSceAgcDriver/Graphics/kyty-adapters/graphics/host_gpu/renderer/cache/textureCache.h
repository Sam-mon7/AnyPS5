#ifndef ANYPS5_KYTY_ADAPTERS_GRAPHICS_HOST_GPU_RENDERER_CACHE_TEXTURECACHE_H
#define ANYPS5_KYTY_ADAPTERS_GRAPHICS_HOST_GPU_RENDERER_CACHE_TEXTURECACHE_H

#include <cstdint>

namespace Libs::Graphics {

class TextureCache {
public:
    bool ClearMeta(std::uint64_t) { return false; }
    void InvalidateMemoryFromGPU(std::uint64_t, std::uint64_t) {}
    [[nodiscard]] bool FindImageFromRange(std::uint64_t, std::uint64_t) const { return false; }
};

}

#endif
