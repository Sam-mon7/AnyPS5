#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_VULKANQUEUES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_VULKANQUEUES_HPP

#include "WorkerPool.hpp"
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <map>
#include <span>
#include <utility>
#include "prx/libc/include/HostThreadLocal.hpp"

namespace AgcDriver::DriverDetail::Lanes {

inline bool AsyncComputeEnabled() {
    static const bool enabled = [] { const auto* value = std::getenv("APS5_ASYNC_COMPUTE_QUEUES"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    return Enabled() && enabled;
}

inline constexpr std::size_t AsyncQueueCount = 0x38;
struct QueuePlan {
    std::uint32_t graphics = 0, compute = 0, count = 1;
    bool Dedicated() const { return graphics != compute; }
};

inline QueuePlan SelectQueues(std::span<const VkQueueFamilyProperties> families, std::uint32_t graphics, bool enabled) {
    QueuePlan result{graphics, graphics, 1};
    if (!enabled) return result;
    for (std::uint32_t index = 0; index < families.size(); ++index) {
        const auto& family = families[index];
        if (family.queueCount != 0 && (family.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == VK_QUEUE_COMPUTE_BIT) {
            result.compute = index;
            result.count = std::min<std::uint32_t>(family.queueCount, AsyncQueueCount);
            break;
        }
    }
    return result;
}

class CommandPools {
public:
    CommandPools(VkDevice device, std::uint32_t family, PFN_vkGetDeviceProcAddr proc, std::size_t workers = 4)
        : device(device), family(family), proc(proc), slots(workers) {}
    ~CommandPools() { Close(); }
    void Close() {
        if (device == VK_NULL_HANDLE) return;
        for (auto& slot : slots) if (slot.pool != VK_NULL_HANDLE) {
            reinterpret_cast<PFN_vkDestroyCommandPool>(proc(device, "vkDestroyCommandPool"))(device, slot.pool, nullptr);
            slot.pool = VK_NULL_HANDLE;
        }
        device = VK_NULL_HANDLE;
    }
    CommandPools(const CommandPools&) = delete;
    CommandPools& operator=(const CommandPools&) = delete;
    VkCommandPool Get(std::size_t worker) {
        if (device == VK_NULL_HANDLE) throw std::runtime_error("GPU lanes command pools are closed");
        auto& slot = slots.at(worker);
        const auto thread = std::this_thread::get_id();
        if (slot.pool != VK_NULL_HANDLE) {
            if (slot.owner != thread) throw std::runtime_error("GPU lanes command pool used by another worker");
            return slot.pool;
        }
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = family;
        if (reinterpret_cast<PFN_vkCreateCommandPool>(proc(device, "vkCreateCommandPool"))(device, &info, nullptr, &slot.pool) != VK_SUCCESS) throw std::runtime_error("GPU lanes vkCreateCommandPool failed");
        slot.owner = thread;
        return slot.pool;
    }
private:
    struct Slot { VkCommandPool pool = VK_NULL_HANDLE; std::thread::id owner; };
    VkDevice device;
    std::uint32_t family;
    PFN_vkGetDeviceProcAddr proc;
    std::vector<Slot> slots;
};

struct Dependency {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    std::uint64_t value = 0;
};

inline std::uint32_t& RecordingQueue() {
    struct RecordingQueueStorage {};
    return HostThreadLocal<std::uint32_t, RecordingQueueStorage>();
}

struct QueueScope {
    explicit QueueScope(std::uint32_t queue) : previous(RecordingQueue()) { RecordingQueue() = queue; }
    ~QueueScope() { RecordingQueue() = previous; }
    std::uint32_t previous;
};

class VulkanQueues {
public:
    using OrderedCall = std::function<VkResult(const std::function<VkResult()>&)>;
    VulkanQueues(VkDevice device, QueuePlan plan, PFN_vkGetDeviceProcAddr proc, bool timeline, OrderedCall orderedCall)
        : device(device), plan(plan), proc(proc), timeline(timeline), orderedCall(std::move(orderedCall)), graphicsPools(device, plan.graphics, proc), computePools(device, plan.compute, proc) {
        auto get = reinterpret_cast<PFN_vkGetDeviceQueue>(proc(device, "vkGetDeviceQueue"));
        get(device, plan.graphics, 0, &graphics.queue);
        if (plan.Dedicated()) {
            compute.resize(plan.count);
            for (std::uint32_t index = 0; index < plan.count; ++index) get(device, plan.compute, index, &compute[index].queue);
        }
    }
    ~VulkanQueues() { Close(); }
    void Close() {
        std::lock_guard lock(mutex);
        if (device == VK_NULL_HANDLE) return;
        auto destroy = reinterpret_cast<PFN_vkDestroySemaphore>(proc(device, "vkDestroySemaphore"));
        if (graphics.semaphore != VK_NULL_HANDLE) destroy(device, graphics.semaphore, nullptr);
        for (const auto& queue : compute) if (queue.semaphore != VK_NULL_HANDLE) destroy(device, queue.semaphore, nullptr);
        graphicsPools.Close();
        computePools.Close();
        device = VK_NULL_HANDLE;
    }
    VkQueue Queue(std::uint32_t guest) { return select(guest).queue; }
    std::uint32_t Family(std::uint32_t guest) const { return guest == 0 || !plan.Dedicated() ? plan.graphics : plan.compute; }
    bool Dedicated() const { return plan.Dedicated(); }
    bool Timeline() const { return timeline; }
    CommandPools& Pools(std::uint32_t guest) { return guest == 0 || !plan.Dedicated() ? graphicsPools : computePools; }
    PFN_vkVoidFunction Function(const char* name) const { return proc(device, name); }
    void NoteLabel(std::uint64_t address, std::span<const std::byte> bytes, Dependency dependency) {
        std::lock_guard lock(mutex);
        for (std::size_t offset = 0; offset + 4 <= bytes.size(); offset += 4) {
            if (labels.size() >= 4096) labels.clear();
            std::uint32_t value;
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            labels[address + offset] = {value, dependency};
        }
    }
    void WaitLabel(std::uint32_t guest, std::uint64_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        auto& waits = labelWaits[guest];
        for (std::size_t offset = 0; offset + 4 <= bytes; offset += 4) {
            const auto found = labels.find(address + offset);
            if (found == labels.end() || found->second.value != *reinterpret_cast<const volatile std::uint32_t*>(address + offset)) continue;
            const auto dependency = found->second.dependency;
            const auto previous = std::find_if(waits.begin(), waits.end(), [&](const Dependency& wait) { return wait.semaphore == dependency.semaphore; });
            if (previous == waits.end()) waits.push_back(dependency);
            else previous->value = std::max(previous->value, dependency.value);
        }
    }
    std::vector<Dependency> LabelDependencies(std::uint32_t guest) {
        std::lock_guard lock(mutex);
        return std::exchange(labelWaits[guest], {});
    }
    void NoteRoutedDispatch() { routedDispatches.fetch_add(1, std::memory_order_relaxed); }
    std::uint64_t RoutedDispatches() const { return routedDispatches.load(std::memory_order_relaxed); }

    Dependency Submit(std::uint32_t guest, std::span<const VkCommandBuffer> commands, std::span<const Dependency> dependencies = {}, VkFence fence = VK_NULL_HANDLE) {
        std::lock_guard lock(mutex);
        if (device == VK_NULL_HANDLE) throw std::runtime_error("GPU lanes queues are closed");
        if (!timeline) throw std::runtime_error("GPU lanes queue dependencies require timeline semaphores");
        auto& queue = select(guest);
        if (queue.semaphore == VK_NULL_HANDLE) {
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
            if (reinterpret_cast<PFN_vkCreateSemaphore>(proc(device, "vkCreateSemaphore"))(device, &info, nullptr, &queue.semaphore) != VK_SUCCESS) throw std::runtime_error("GPU lanes vkCreateSemaphore failed");
        }
        std::map<VkSemaphore, std::uint64_t> merged;
        for (const auto& dependency : dependencies) {
            if (dependency.semaphore == VK_NULL_HANDLE || dependency.value == 0) throw std::runtime_error("GPU lanes invalid queue dependency");
            auto& value = merged[dependency.semaphore];
            value = std::max(value, dependency.value);
        }
        std::vector<VkSemaphore> waits;
        std::vector<std::uint64_t> values;
        std::vector<VkPipelineStageFlags> stages(merged.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        for (const auto& [semaphore, value] : merged) {
            waits.push_back(semaphore);
            values.push_back(value);
        }
        const auto value = queue.value + 1;
        if (value == 0) throw std::runtime_error("GPU lanes timeline overflow");
        VkTimelineSemaphoreSubmitInfo timelineInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timelineInfo.waitSemaphoreValueCount = values.size();
        timelineInfo.pWaitSemaphoreValues = values.data();
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues = &value;
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timelineInfo};
        info.waitSemaphoreCount = waits.size();
        info.pWaitSemaphores = waits.data();
        info.pWaitDstStageMask = stages.data();
        info.commandBufferCount = commands.size();
        info.pCommandBuffers = commands.data();
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &queue.semaphore;
        const auto result = orderedCall([&] { return reinterpret_cast<PFN_vkQueueSubmit>(proc(device, "vkQueueSubmit"))(queue.queue, 1, &info, fence); });
        if (result != VK_SUCCESS) throw std::runtime_error("GPU lanes vkQueueSubmit failed: " + std::to_string(result));
        queue.value = value;
        return {queue.semaphore, value};
    }

private:
    struct QueueState { VkQueue queue = VK_NULL_HANDLE; VkSemaphore semaphore = VK_NULL_HANDLE; std::uint64_t value = 0; };
    QueueState& select(std::uint32_t guest) {
        if (guest == 0) return graphics;
        if (guest < 0x20 || guest >= 0x58) throw std::runtime_error("GPU lanes invalid guest queue");
        return plan.Dedicated() ? compute[(guest - 0x20) % compute.size()] : graphics;
    }
    VkDevice device;
    QueuePlan plan;
    PFN_vkGetDeviceProcAddr proc;
    bool timeline;
    OrderedCall orderedCall;
    std::mutex mutex;
    struct Label { std::uint32_t value; Dependency dependency; };
    std::map<std::uint64_t, Label> labels;
    std::map<std::uint32_t, std::vector<Dependency>> labelWaits;
    std::atomic<std::uint64_t> routedDispatches{0};
    QueueState graphics;
    std::vector<QueueState> compute;
    CommandPools graphicsPools, computePools;
};

inline void TransferBuffers(VulkanQueues& queues, VkCommandBuffer commands, std::span<const VkBuffer> buffers, std::uint32_t source, std::uint32_t destination, bool release) {
    std::vector<VkBufferMemoryBarrier> barriers;
    for (const auto buffer : buffers) {
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = release ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0;
        barrier.dstAccessMask = release ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.srcQueueFamilyIndex = source;
        barrier.dstQueueFamilyIndex = destination;
        barrier.buffer = buffer;
        barrier.size = VK_WHOLE_SIZE;
        barriers.push_back(barrier);
    }
    reinterpret_cast<PFN_vkCmdPipelineBarrier>(queues.Function("vkCmdPipelineBarrier"))(commands,
        release ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        release ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, barriers.size(), barriers.data(), 0, nullptr);
}

inline std::mutex registryMutex;
inline std::map<VkDevice, std::weak_ptr<VulkanQueues>> registry;
inline void PublishQueues(VkDevice device, const std::shared_ptr<VulkanQueues>& queues) {
    std::lock_guard lock(registryMutex);
    if (queues) registry[device] = queues;
    else registry.erase(device);
}
std::shared_ptr<VulkanQueues> FindQueues(VkDevice device);

}
#endif
