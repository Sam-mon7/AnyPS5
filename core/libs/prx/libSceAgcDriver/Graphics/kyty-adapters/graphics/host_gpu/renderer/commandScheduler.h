#ifndef ANYPS5_KYTY_ADAPTERS_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H
#define ANYPS5_KYTY_ADAPTERS_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H

#include "graphics/host_gpu/vulkanCommon.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace Libs::Graphics {

class CommandScheduler;

class CommandBuffer {
public:
    explicit CommandBuffer(CommandScheduler& scheduler) : scheduler(&scheduler) {}
    [[nodiscard]] vk::CommandBuffer Handle();
    [[nodiscard]] bool IsInvalid() const;
    void EndRendering() {}

private:
    CommandScheduler* scheduler;
};

class GpuThread {
public:
    template <typename Command>
    void SendCommandSync(Command&& command) {
        command();
    }
};

class RenderContext {
public:
    GpuThread& GetGpu() { return gpu; }

private:
    GpuThread gpu;
};

class CommandScheduler {
public:
    CommandScheduler() : current(*this) {}
    CommandScheduler(const CommandScheduler&) = delete;
    CommandScheduler& operator=(const CommandScheduler&) = delete;

    CommandBuffer& Current() { return current; }
    void EndRendering() {}
    template <typename Operation>
    void DeferOperation(Operation&& operation) {
        auto held = std::make_shared<std::decay_t<Operation>>(std::forward<Operation>(operation));
        defer([held] { (*held)(); });
    }
    template <typename Operation>
    void DeferPriorityOperation(Operation&& operation) {
        DeferOperation(std::forward<Operation>(operation));
    }
    void Wait(std::uint64_t tick);
    void WaitPriorityOperations(std::uint64_t tick) { Wait(tick); }
    void PopPendingOperations();
    [[nodiscard]] std::uint64_t CurrentTick();
    [[nodiscard]] bool IsFree(std::uint64_t tick) const { return tick <= completed->load(std::memory_order_acquire); }
    [[nodiscard]] bool Active() const;
    RenderContext& Context() { return context; }
    [[nodiscard]] static bool InDeferredOperation();

private:
    void defer(std::function<void()> operation);

    CommandBuffer current;
    RenderContext context;
    std::uint64_t watched = 0;
    std::shared_ptr<std::atomic<std::uint64_t>> completed = std::make_shared<std::atomic<std::uint64_t>>(0);
};

}

#endif
