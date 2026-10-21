#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "execution/VulkanTestDevice.hpp"
#include <cstring>

namespace {

using namespace AgcDriver::Graphics;

std::vector<std::string> events;
std::vector<std::uint64_t> frames;
VkResult submitResult = VK_SUCCESS;

VKAPI_ATTR void VKAPI_CALL beginLabel(VkCommandBuffer, const VkDebugUtilsLabelEXT* label) {
    Require(label->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, "wrong label type");
    events.emplace_back(label->pLabelName);
}

VKAPI_ATTR void VKAPI_CALL endLabel(VkCommandBuffer) {
    events.emplace_back("end");
}

VKAPI_ATTR VkResult VKAPI_CALL submitFrame(VkQueue, std::uint32_t count, const VkSubmitInfo* submissions, VkFence fence) {
    Require(count == 1 && fence == VK_NULL_HANDLE, "unexpected frame submission");
    Require(submissions->sType == VK_STRUCTURE_TYPE_SUBMIT_INFO && submissions->commandBufferCount == 0 && submissions->waitSemaphoreCount == 0 && submissions->signalSemaphoreCount == 0, "frame marker changes submitted work");
    const auto* boundary = static_cast<const VkFrameBoundaryEXT*>(submissions->pNext);
    Require(boundary != nullptr && boundary->sType == VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT, "missing frame boundary");
    Require(boundary->flags == VK_FRAME_BOUNDARY_FRAME_END_BIT_EXT && boundary->pNext == nullptr, "wrong frame boundary flags");
    frames.push_back(boundary->frameID);
    return submitResult;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL resolve(VkDevice, const char* name) {
    Require(std::strcmp(name, "vkQueueSubmit") == 0, "unexpected Vulkan lookup");
    return reinterpret_cast<PFN_vkVoidFunction>(submitFrame);
}

void testAnnotations() {
    Context context{};
    {
        DebugLabel disabled(context, VK_NULL_HANDLE, "disabled");
        context.MarkFrameBoundary(1);
    }
    Require(events.empty() && frames.empty(), "disabled annotations called Vulkan");
    context.beginDebugLabel = beginLabel;
    {
        DebugLabel incomplete(context, VK_NULL_HANDLE, "incomplete");
    }
    Require(events.empty(), "a label began without an end function");
    context.endDebugLabel = endLabel;
    try {
        DebugLabel outer(context, VK_NULL_HANDLE, "draw submission=%llu", 42ull);
        DebugLabel inner(context, VK_NULL_HANDLE, "dispatch %u", 7u);
        inner.End();
        throw std::runtime_error("unwind");
    } catch (const std::runtime_error&) {
    }
    Require(events == std::vector<std::string>{"draw submission=42", "dispatch 7", "end", "end"}, "labels were not nested and balanced during unwinding");
    context.frameBoundary = true;
    context.deviceProc = resolve;
    context.MarkFrameBoundary(42);
    context.MarkFrameBoundary(43);
    Require(frames == std::vector<std::uint64_t>{42, 43}, "frame identifiers were lost");
    submitResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    bool failed = false;
    try {
        context.MarkFrameBoundary(44);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    Require(failed, "frame submission failure was ignored");
}

int testGpu() {
    const auto device = OpenVulkanTestDevice();
    if (!device) return VulkanTestSkipped;
    const auto context = device->graphicsContext();
    Require(context.beginDebugLabel != nullptr && context.endDebugLabel != nullptr, "GPU annotation test requires VK_EXT_debug_utils");
    Buffer buffer(context, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    for (std::uint32_t value = 1; value <= 3; ++value) {
        CommandBatch batch(context);
        {
            DebugLabel label(context, batch.Handle(), "AnyPS5 annotation smoke frame=%u", value);
            context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(batch.Handle(), buffer.Handle(), 0, 16, value);
            RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        }
        batch.SubmitAndWait();
        device->MarkFrameBoundary();
        std::uint32_t actual = 0;
        std::memcpy(&actual, buffer.Bytes().data(), sizeof(actual));
        Require(actual == value, "annotated GPU work produced the wrong data");
    }
    device->WaitIdle();
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--gpu") == 0) return testGpu();
        else testAnnotations();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
    std::puts("debug annotation tests passed");
}
