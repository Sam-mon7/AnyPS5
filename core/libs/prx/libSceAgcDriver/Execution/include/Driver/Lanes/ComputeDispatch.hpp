#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_COMPUTEDISPATCH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_COMPUTEDISPATCH_HPP

#include "VulkanQueues.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "Optimization/include/Optimization/ShaderStageInputInfo.hpp"

namespace AgcDriver::DriverDetail::Lanes {

inline std::vector<VkBuffer> ImportedDispatchBuffers(const Graphics::Context& context, const Graphics::ShaderResources& resources, const ShaderRecompiler::RecompileResult& shader) {
    if (!resources.Reusable() || resources.NeedsCompletion() || resources.HoldsLease() || resources.UsesAddressTable()) return {};
    const auto reads = resources.InPlaceReads();
    std::vector<VkBuffer> buffers;
    for (const auto& binding : shader.bindings) {
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers || binding.kind != ShaderRecompiler::DescriptorKind::StorageBuffer || binding.guestDescriptor.size() != binding.count * 4ull) return {};
        for (std::size_t index = 0; index < binding.count; ++index) {
            const auto* words = binding.guestDescriptor.data() + index * 4;
            const ShaderRecompiler::ShaderBufferResource resource{{words[0], words[1], words[2], words[3]}};
            const auto address = resource.Base48(), bytes = resource.GetSize();
            if (address == 0 || bytes == 0 || address + bytes < address) return {};
            if (std::none_of(reads.begin(), reads.end(), [&](const auto& range) { return range.first <= address && address + bytes <= range.second; })) return {};
            const auto* import = Graphics::HostImportFor(context, address, bytes);
            if (import == nullptr) return {};
            if (std::find(buffers.begin(), buffers.end(), import->buffer) == buffers.end()) buffers.push_back(import->buffer);
        }
    }
    return buffers;
}

class ComputeDispatch {
public:
    ComputeDispatch(const Graphics::Context& context, const std::shared_ptr<VulkanQueues>& queues, std::uint32_t guest) : context(context), queues(queues) {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = queues->Family(guest);
        Graphics::Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateCommandPool lane dispatch");
        try {
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            Graphics::Check(context.Function<PFN_vkCreateFence>("vkCreateFence")(context.device, &fenceInfo, nullptr, &fence), "vkCreateFence lane dispatch");
            VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            info.commandPool = pool;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            info.commandBufferCount = 1;
            Graphics::Check(context.Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(context.device, &info, &commands), "vkAllocateCommandBuffers lane dispatch");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            Graphics::Check(context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin), "vkBeginCommandBuffer lane dispatch");
        } catch (...) {
            if (fence != VK_NULL_HANDLE) context.Function<PFN_vkDestroyFence>("vkDestroyFence")(context.device, fence, nullptr);
            context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, pool, nullptr);
            throw;
        }
    }
    ~ComputeDispatch() {
        if (pending) {
            const auto result = context.Function<PFN_vkWaitForFences>("vkWaitForFences")(context.device, 1, &fence, VK_TRUE, UINT64_MAX);
            if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) std::terminate();
        }
        context.Function<PFN_vkDestroyFence>("vkDestroyFence")(context.device, fence, nullptr);
        context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, pool, nullptr);
    }
    ComputeDispatch(const ComputeDispatch&) = delete;
    ComputeDispatch& operator=(const ComputeDispatch&) = delete;
    VkCommandBuffer Commands() const { return commands; }
    void Submit(std::uint32_t guest, Graphics::Recorder& recorder, std::span<const VkBuffer> buffers) {
        Graphics::Check(context.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands), "vkEndCommandBuffer lane dispatch");
        TransferBuffers(*queues, recorder.Commands(), buffers, queues->Family(0), queues->Family(guest), true);
        recorder.Submit();
        auto dependencies = queues->LabelDependencies(guest);
        dependencies.push_back(queues->Submit(0, {}));
        const auto done = queues->Submit(guest, std::span(&commands, 1), dependencies, fence);
        pending = true;
        queues->Submit(0, {}, std::span(&done, 1));
        TransferBuffers(*queues, recorder.Commands(), buffers, queues->Family(guest), queues->Family(0), false);
        queues->NoteRoutedDispatch();
    }
private:
    Graphics::Context context;
    std::shared_ptr<VulkanQueues> queues;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
};

}
#endif
