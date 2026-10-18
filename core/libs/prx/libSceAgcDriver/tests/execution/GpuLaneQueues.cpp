#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Lanes/VulkanQueues.hpp"
#include <array>
#include <iostream>

using namespace AgcDriver::DriverDetail::Lanes;

namespace {

void Check(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("GPU lane Vulkan test failed: " + std::to_string(result));
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        auto queues = FindQueues(device->Device());
        if (!Enabled()) {
            if (queues) throw std::runtime_error("compute queues were published with the switch off");
            return 0;
        }
        if (!queues) throw std::runtime_error("compute queues were not published");
        if (!AsyncComputeEnabled() && queues->Dedicated()) throw std::runtime_error("compute-only queues were selected with the async switch off");
        if (!queues->Timeline()) return VulkanTestSkipped;
        const auto allocate = reinterpret_cast<PFN_vkAllocateCommandBuffers>(queues->Function("vkAllocateCommandBuffers"));
        const auto begin = reinterpret_cast<PFN_vkBeginCommandBuffer>(queues->Function("vkBeginCommandBuffer"));
        const auto end = reinterpret_cast<PFN_vkEndCommandBuffer>(queues->Function("vkEndCommandBuffer"));
        const auto createFence = reinterpret_cast<PFN_vkCreateFence>(queues->Function("vkCreateFence"));
        const auto wait = reinterpret_cast<PFN_vkWaitForFences>(queues->Function("vkWaitForFences"));
        const auto destroyFence = reinterpret_cast<PFN_vkDestroyFence>(queues->Function("vkDestroyFence"));
        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(createFence(device->Device(), &fenceInfo, nullptr, &fence));
        {
            WorkerPool pool;
            Dependency previous;
            std::array<std::future<void>, 8> work;
            for (std::size_t index = 0; index < work.size(); ++index) {
                const auto guest = index % 2 == 0 ? 0u : 0x20u + static_cast<std::uint32_t>(index / 2);
                work[index] = pool.Enqueue({guest, index, [&, guest, index](std::size_t worker) -> Commit {
                    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                    info.commandPool = queues->Pools(guest).Get(worker);
                    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                    info.commandBufferCount = 1;
                    VkCommandBuffer command = VK_NULL_HANDLE;
                    Check(allocate(device->Device(), &info, &command));
                    VkCommandBufferBeginInfo start{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    Check(begin(command, &start));
                    Check(end(command));
                    return [&, guest, command, index] {
                        const auto dependencies = previous.semaphore == VK_NULL_HANDLE ? std::span<const Dependency>{} : std::span<const Dependency>(&previous, 1);
                        previous = queues->Submit(guest, std::span(&command, 1), dependencies, index + 1 == work.size() ? fence : VK_NULL_HANDLE);
                    };
                }});
            }
            for (auto& result : work) result.get();
            Check(wait(device->Device(), 1, &fence, VK_TRUE, 5'000'000'000ull));
        }
        destroyFence(device->Device(), fence, nullptr);
        std::cout << "GPU lane semaphore chain passed; dedicated compute=" << queues->Dedicated() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
