#ifndef ANYPS5_KYTY_ADAPTERS_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H
#define ANYPS5_KYTY_ADAPTERS_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H

#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

namespace Libs::Graphics {

class BufferCache;

class FaultManager {
public:
    FaultManager(GraphicContext&, CommandScheduler&, BufferCache&) {}
    FaultManager(const FaultManager&) = delete;
    FaultManager& operator=(const FaultManager&) = delete;
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept { return nullptr; }
    void ProcessFaultBuffer() {}
};

}

#endif
