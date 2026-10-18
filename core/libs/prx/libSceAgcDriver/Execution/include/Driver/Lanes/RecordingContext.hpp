#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_RECORDINGCONTEXT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_LANES_RECORDINGCONTEXT_HPP

#include "WorkerPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"

namespace AgcDriver::DriverDetail::Lanes {

inline bool RecordersEnabled() {
    static const bool enabled = [] { const auto* value = std::getenv("APS5_GPU_LANE_RECORDERS"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    return Enabled() && enabled;
}

class RecordingContext {
public:
    explicit RecordingContext(const Graphics::Context& context) : context(context), owner(std::this_thread::get_id()) {
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.queueFamilyIndex = context.queueFamilyIndex;
        Graphics::Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &info, nullptr, &pool), "vkCreateCommandPool lane recorder");
        try {
            VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocation.commandPool = pool;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
            allocation.commandBufferCount = 1;
            Graphics::Check(context.Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(context.device, &allocation, &commands), "vkAllocateCommandBuffers lane recorder");
            VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            begin.pInheritanceInfo = &inheritance;
            Graphics::Check(context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin), "vkBeginCommandBuffer lane recorder");
        } catch (...) {
            context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, pool, nullptr);
            throw;
        }
    }
    ~RecordingContext() { context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, pool, nullptr); }
    RecordingContext(const RecordingContext&) = delete;
    RecordingContext& operator=(const RecordingContext&) = delete;
    VkCommandBuffer Commands() const {
        Graphics::Require(owner == std::this_thread::get_id() && !ended, "lane command buffer used outside its recording worker");
        return commands;
    }
    void End() {
        Graphics::Check(context.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(Commands()), "vkEndCommandBuffer lane recorder");
        ended = true;
    }
    void Execute(VkCommandBuffer primary) const {
        Graphics::Require(ended, "lane command buffer published before recording completed");
        context.Function<PFN_vkCmdExecuteCommands>("vkCmdExecuteCommands")(primary, 1, &commands);
    }

private:
    Graphics::Context context;
    std::thread::id owner;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    bool ended = false;
};

}
#endif
