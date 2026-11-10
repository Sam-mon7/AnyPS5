#include "prx/libSceAgcDriver/Graphics/include/StreamCensus.hpp"

#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Graphics::StreamCensus {

namespace {

enum class Kind : std::size_t {
    Barrier,
    CopyBuffer,
    UpdateBuffer,
    FillBuffer,
    BindPipeline,
    BindSets,
    BindSetsShape,
    Push,
    PushShape,
    Dispatch,
    BeginPass,
    EndPass,
    Viewport,
    Scissor,
    DynamicState,
    VertexBuffers,
    IndexBuffer,
    Draw,
    ImageCopy,
    Count
};

constexpr std::array<const char*, static_cast<std::size_t>(Kind::Count)> Names{
    "barrier", "copy_buffer", "update_buffer", "fill_buffer", "bind_pipeline", "bind_sets", "bind_sets_shape", "push", "push_shape", "dispatch",
    "begin_pass", "end_pass", "viewport", "scissor", "dynamic_state", "vertex_buffers", "index_buffer", "draw", "image_copy",
};

struct Hash {
    std::uint64_t value = 0xcbf29ce484222325ull;

    Hash& Bytes(const void* data, std::size_t bytes) {
        const auto* at = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < bytes; ++i) value = (value ^ at[i]) * 0x100000001b3ull;
        return *this;
    }

    template<typename T>
    Hash& Add(const T& item) {
        return Bytes(&item, sizeof(item));
    }

    template<typename T>
    Hash& Array(const T* items, std::size_t count) {
        Add(count);
        return items != nullptr ? Bytes(items, count * sizeof(T)) : *this;
    }
};

struct State {
    std::mutex mutex;
    DeviceFunctions real;
    std::array<std::vector<std::uint64_t>, static_cast<std::size_t>(Kind::Count)> current;
    std::array<std::unordered_map<std::uint64_t, std::uint32_t>, static_cast<std::size_t>(Kind::Count)> previous;
    std::array<std::unordered_map<std::uint64_t, std::uint32_t>, static_cast<std::size_t>(Kind::Count)> older;
    std::uint64_t descriptorWrites = 0;
    std::uint64_t descriptorUpdates = 0;
    std::uint64_t setsAllocated = 0;
    std::vector<std::string> names;
};

State& state() {
    static auto* const value = new State;
    return *value;
}

void note(Kind kind, const Hash& hash) {
    auto& census = state();
    const std::lock_guard lock(census.mutex);
    census.current[static_cast<std::size_t>(kind)].push_back(hash.value);
}

const DeviceFunctions& real() {
    return state().real;
}

VKAPI_ATTR void VKAPI_CALL PipelineBarrier(VkCommandBuffer commands, VkPipelineStageFlags source, VkPipelineStageFlags destination, VkDependencyFlags flags, std::uint32_t memoryCount, const VkMemoryBarrier* memory, std::uint32_t bufferCount, const VkBufferMemoryBarrier* buffers, std::uint32_t imageCount, const VkImageMemoryBarrier* images) {
    Hash hash;
    hash.Add(source).Add(destination).Add(flags).Add(memoryCount).Add(bufferCount).Add(imageCount);
    for (std::uint32_t i = 0; i < memoryCount; ++i) hash.Add(memory[i].srcAccessMask).Add(memory[i].dstAccessMask);
    for (std::uint32_t i = 0; i < bufferCount; ++i) hash.Add(buffers[i].srcAccessMask).Add(buffers[i].dstAccessMask).Add(buffers[i].buffer).Add(buffers[i].offset).Add(buffers[i].size);
    for (std::uint32_t i = 0; i < imageCount; ++i) hash.Add(images[i].srcAccessMask).Add(images[i].dstAccessMask).Add(images[i].oldLayout).Add(images[i].newLayout).Add(images[i].image).Add(images[i].subresourceRange);
    note(Kind::Barrier, hash);
    real().cmdPipelineBarrier(commands, source, destination, flags, memoryCount, memory, bufferCount, buffers, imageCount, images);
}

VKAPI_ATTR void VKAPI_CALL CopyBuffer(VkCommandBuffer commands, VkBuffer source, VkBuffer destination, std::uint32_t count, const VkBufferCopy* regions) {
    note(Kind::CopyBuffer, Hash().Add(source).Add(destination).Array(regions, count));
    real().cmdCopyBuffer(commands, source, destination, count, regions);
}

VKAPI_ATTR void VKAPI_CALL UpdateBuffer(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, const void* data) {
    note(Kind::UpdateBuffer, Hash().Add(buffer).Add(offset).Add(size).Bytes(data, static_cast<std::size_t>(size)));
    real().cmdUpdateBuffer(commands, buffer, offset, size, data);
}

VKAPI_ATTR void VKAPI_CALL FillBuffer(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, std::uint32_t data) {
    note(Kind::FillBuffer, Hash().Add(buffer).Add(offset).Add(size).Add(data));
    real().cmdFillBuffer(commands, buffer, offset, size, data);
}

VKAPI_ATTR void VKAPI_CALL BindPipeline(VkCommandBuffer commands, VkPipelineBindPoint point, VkPipeline pipeline) {
    note(Kind::BindPipeline, Hash().Add(point).Add(pipeline));
    real().cmdBindPipeline(commands, point, pipeline);
}

VKAPI_ATTR void VKAPI_CALL BindDescriptorSets(VkCommandBuffer commands, VkPipelineBindPoint point, VkPipelineLayout layout, std::uint32_t first, std::uint32_t count, const VkDescriptorSet* sets, std::uint32_t offsetCount, const std::uint32_t* offsets) {
    note(Kind::BindSets, Hash().Add(point).Add(layout).Add(first).Array(sets, count).Array(offsets, offsetCount));
    note(Kind::BindSetsShape, Hash().Add(point).Add(layout).Add(first).Add(count).Add(offsetCount));
    real().cmdBindDescriptorSets(commands, point, layout, first, count, sets, offsetCount, offsets);
}

VKAPI_ATTR void VKAPI_CALL PushConstants(VkCommandBuffer commands, VkPipelineLayout layout, VkShaderStageFlags stages, std::uint32_t offset, std::uint32_t size, const void* values) {
    note(Kind::Push, Hash().Add(layout).Add(stages).Add(offset).Add(size).Bytes(values, size));
    note(Kind::PushShape, Hash().Add(layout).Add(stages).Add(offset).Add(size));
    real().cmdPushConstants(commands, layout, stages, offset, size, values);
}

VKAPI_ATTR void VKAPI_CALL Dispatch(VkCommandBuffer commands, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    note(Kind::Dispatch, Hash().Add(x).Add(y).Add(z));
    real().cmdDispatch(commands, x, y, z);
}

VKAPI_ATTR void VKAPI_CALL DispatchIndirect(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset) {
    note(Kind::Dispatch, Hash().Add(buffer).Add(offset));
    real().cmdDispatchIndirect(commands, buffer, offset);
}

VKAPI_ATTR void VKAPI_CALL BeginRenderPass(VkCommandBuffer commands, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    note(Kind::BeginPass, Hash().Add(info->renderPass).Add(info->framebuffer).Add(info->renderArea).Add(contents));
    real().cmdBeginRenderPass(commands, info, contents);
}

VKAPI_ATTR void VKAPI_CALL EndRenderPass(VkCommandBuffer commands) {
    note(Kind::EndPass, Hash());
    real().cmdEndRenderPass(commands);
}

VKAPI_ATTR void VKAPI_CALL BeginRendering(VkCommandBuffer commands, const VkRenderingInfoKHR* info) {
    Hash hash;
    hash.Add(info->flags).Add(info->renderArea).Add(info->layerCount).Add(info->colorAttachmentCount);
    const auto attachment = [&](const VkRenderingAttachmentInfoKHR* item) {
        if (item != nullptr) hash.Add(item->imageView).Add(item->imageLayout).Add(item->resolveImageView).Add(item->loadOp).Add(item->storeOp);
        else hash.Add(0u);
    };
    for (std::uint32_t i = 0; i < info->colorAttachmentCount; ++i) attachment(&info->pColorAttachments[i]);
    attachment(info->pDepthAttachment);
    attachment(info->pStencilAttachment);
    note(Kind::BeginPass, hash);
    real().cmdBeginRendering(commands, info);
}

VKAPI_ATTR void VKAPI_CALL EndRendering(VkCommandBuffer commands) {
    note(Kind::EndPass, Hash());
    real().cmdEndRendering(commands);
}

VKAPI_ATTR void VKAPI_CALL SetViewport(VkCommandBuffer commands, std::uint32_t first, std::uint32_t count, const VkViewport* viewports) {
    note(Kind::Viewport, Hash().Add(first).Array(viewports, count));
    real().cmdSetViewport(commands, first, count, viewports);
}

VKAPI_ATTR void VKAPI_CALL SetScissor(VkCommandBuffer commands, std::uint32_t first, std::uint32_t count, const VkRect2D* scissors) {
    note(Kind::Scissor, Hash().Add(first).Array(scissors, count));
    real().cmdSetScissor(commands, first, count, scissors);
}

template<auto Member, std::uint32_t Tag, typename... TArguments>
VKAPI_ATTR void VKAPI_CALL Dynamic(VkCommandBuffer commands, TArguments... arguments) {
    Hash hash;
    hash.Add(Tag);
    (hash.Add(arguments), ...);
    note(Kind::DynamicState, hash);
    (real().*Member)(commands, arguments...);
}

VKAPI_ATTR void VKAPI_CALL BindVertexBuffers(VkCommandBuffer commands, std::uint32_t first, std::uint32_t count, const VkBuffer* buffers, const VkDeviceSize* offsets) {
    note(Kind::VertexBuffers, Hash().Add(first).Array(buffers, count).Array(offsets, count));
    real().cmdBindVertexBuffers(commands, first, count, buffers, offsets);
}

VKAPI_ATTR void VKAPI_CALL BindIndexBuffer(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
    note(Kind::IndexBuffer, Hash().Add(buffer).Add(offset).Add(type));
    real().cmdBindIndexBuffer(commands, buffer, offset, type);
}

VKAPI_ATTR void VKAPI_CALL Draw(VkCommandBuffer commands, std::uint32_t vertices, std::uint32_t instances, std::uint32_t firstVertex, std::uint32_t firstInstance) {
    note(Kind::Draw, Hash().Add(0u).Add(vertices).Add(instances).Add(firstVertex).Add(firstInstance));
    real().cmdDraw(commands, vertices, instances, firstVertex, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL DrawIndexed(VkCommandBuffer commands, std::uint32_t indices, std::uint32_t instances, std::uint32_t firstIndex, std::int32_t vertexOffset, std::uint32_t firstInstance) {
    note(Kind::Draw, Hash().Add(1u).Add(indices).Add(instances).Add(firstIndex).Add(vertexOffset).Add(firstInstance));
    real().cmdDrawIndexed(commands, indices, instances, firstIndex, vertexOffset, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL DrawIndirect(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, std::uint32_t count, std::uint32_t stride) {
    note(Kind::Draw, Hash().Add(2u).Add(buffer).Add(offset).Add(count).Add(stride));
    real().cmdDrawIndirect(commands, buffer, offset, count, stride);
}

VKAPI_ATTR void VKAPI_CALL DrawIndexedIndirect(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, std::uint32_t count, std::uint32_t stride) {
    note(Kind::Draw, Hash().Add(3u).Add(buffer).Add(offset).Add(count).Add(stride));
    real().cmdDrawIndexedIndirect(commands, buffer, offset, count, stride);
}

VKAPI_ATTR void VKAPI_CALL CopyBufferToImage(VkCommandBuffer commands, VkBuffer source, VkImage destination, VkImageLayout layout, std::uint32_t count, const VkBufferImageCopy* regions) {
    note(Kind::ImageCopy, Hash().Add(0u).Add(source).Add(destination).Add(layout).Array(regions, count));
    real().cmdCopyBufferToImage(commands, source, destination, layout, count, regions);
}

VKAPI_ATTR void VKAPI_CALL CopyImageToBuffer(VkCommandBuffer commands, VkImage source, VkImageLayout layout, VkBuffer destination, std::uint32_t count, const VkBufferImageCopy* regions) {
    note(Kind::ImageCopy, Hash().Add(1u).Add(source).Add(layout).Add(destination).Array(regions, count));
    real().cmdCopyImageToBuffer(commands, source, layout, destination, count, regions);
}

VKAPI_ATTR void VKAPI_CALL ClearColorImage(VkCommandBuffer commands, VkImage image, VkImageLayout layout, const VkClearColorValue* color, std::uint32_t count, const VkImageSubresourceRange* ranges) {
    note(Kind::ImageCopy, Hash().Add(2u).Add(image).Add(layout).Bytes(color, sizeof(*color)).Array(ranges, count));
    real().cmdClearColorImage(commands, image, layout, color, count, ranges);
}

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(VkDevice device, std::uint32_t writes, const VkWriteDescriptorSet* descriptorWrites, std::uint32_t copies, const VkCopyDescriptorSet* descriptorCopies) {
    {
        auto& census = state();
        const std::lock_guard lock(census.mutex);
        ++census.descriptorUpdates;
        for (std::uint32_t i = 0; i < writes; ++i) census.descriptorWrites += descriptorWrites[i].descriptorCount;
    }
    real().updateDescriptorSets(device, writes, descriptorWrites, copies, descriptorCopies);
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* sets) {
    {
        auto& census = state();
        const std::lock_guard lock(census.mutex);
        census.setsAllocated += info->descriptorSetCount;
    }
    return real().allocateDescriptorSets(device, info, sets);
}

template<typename TFunction>
void wrap(TFunction& entry, TFunction replacement) {
    if (entry != nullptr) entry = replacement;
}

}

void Install(DeviceFunctions& functions) {
    static const bool enabled = std::getenv("APS5_VK_STREAM_CENSUS") != nullptr;
    if (!enabled) return;
    auto& census = state();
    {
        const std::lock_guard lock(census.mutex);
        census.real = functions;
        if (census.names.empty()) {
            for (const auto* name : Names) {
                census.names.push_back(std::string("vk_stream_") + name);
                census.names.push_back(std::string("vk_stream_") + name + "_same1");
                census.names.push_back(std::string("vk_stream_") + name + "_same2");
            }
        }
    }
    wrap(functions.cmdPipelineBarrier, &PipelineBarrier);
    wrap(functions.cmdCopyBuffer, &CopyBuffer);
    wrap(functions.cmdUpdateBuffer, &UpdateBuffer);
    wrap(functions.cmdFillBuffer, &FillBuffer);
    wrap(functions.cmdBindPipeline, &BindPipeline);
    wrap(functions.cmdBindDescriptorSets, &BindDescriptorSets);
    wrap(functions.cmdPushConstants, &PushConstants);
    wrap(functions.cmdDispatch, &Dispatch);
    wrap(functions.cmdDispatchIndirect, &DispatchIndirect);
    wrap(functions.cmdBeginRenderPass, &BeginRenderPass);
    wrap(functions.cmdEndRenderPass, &EndRenderPass);
    wrap(functions.cmdBeginRendering, &BeginRendering);
    wrap(functions.cmdEndRendering, &EndRendering);
    wrap(functions.cmdSetViewport, &SetViewport);
    wrap(functions.cmdSetScissor, &SetScissor);
    wrap<PFN_vkCmdSetDepthBounds>(functions.cmdSetDepthBounds, &Dynamic<&DeviceFunctions::cmdSetDepthBounds, 1u, float, float>);
    wrap<PFN_vkCmdSetDepthBias>(functions.cmdSetDepthBias, &Dynamic<&DeviceFunctions::cmdSetDepthBias, 2u, float, float, float>);
    wrap<PFN_vkCmdSetCullModeEXT>(functions.cmdSetCullMode, &Dynamic<&DeviceFunctions::cmdSetCullMode, 3u, VkCullModeFlags>);
    wrap<PFN_vkCmdSetFrontFaceEXT>(functions.cmdSetFrontFace, &Dynamic<&DeviceFunctions::cmdSetFrontFace, 4u, VkFrontFace>);
    wrap<PFN_vkCmdSetDepthTestEnableEXT>(functions.cmdSetDepthTestEnable, &Dynamic<&DeviceFunctions::cmdSetDepthTestEnable, 5u, VkBool32>);
    wrap<PFN_vkCmdSetDepthWriteEnableEXT>(functions.cmdSetDepthWriteEnable, &Dynamic<&DeviceFunctions::cmdSetDepthWriteEnable, 6u, VkBool32>);
    wrap<PFN_vkCmdSetDepthCompareOpEXT>(functions.cmdSetDepthCompareOp, &Dynamic<&DeviceFunctions::cmdSetDepthCompareOp, 7u, VkCompareOp>);
    wrap<PFN_vkCmdSetDepthBoundsTestEnableEXT>(functions.cmdSetDepthBoundsTestEnable, &Dynamic<&DeviceFunctions::cmdSetDepthBoundsTestEnable, 8u, VkBool32>);
    wrap<PFN_vkCmdSetStencilTestEnableEXT>(functions.cmdSetStencilTestEnable, &Dynamic<&DeviceFunctions::cmdSetStencilTestEnable, 9u, VkBool32>);
    wrap<PFN_vkCmdSetStencilOpEXT>(functions.cmdSetStencilOp, &Dynamic<&DeviceFunctions::cmdSetStencilOp, 10u, VkStencilFaceFlags, VkStencilOp, VkStencilOp, VkStencilOp, VkCompareOp>);
    wrap<PFN_vkCmdSetStencilCompareMask>(functions.cmdSetStencilCompareMask, &Dynamic<&DeviceFunctions::cmdSetStencilCompareMask, 11u, VkStencilFaceFlags, std::uint32_t>);
    wrap<PFN_vkCmdSetStencilWriteMask>(functions.cmdSetStencilWriteMask, &Dynamic<&DeviceFunctions::cmdSetStencilWriteMask, 12u, VkStencilFaceFlags, std::uint32_t>);
    wrap<PFN_vkCmdSetStencilReference>(functions.cmdSetStencilReference, &Dynamic<&DeviceFunctions::cmdSetStencilReference, 13u, VkStencilFaceFlags, std::uint32_t>);
    wrap(functions.cmdBindVertexBuffers, &BindVertexBuffers);
    wrap(functions.cmdBindIndexBuffer, &BindIndexBuffer);
    wrap(functions.cmdDraw, &Draw);
    wrap(functions.cmdDrawIndexed, &DrawIndexed);
    wrap(functions.cmdDrawIndirect, &DrawIndirect);
    wrap(functions.cmdDrawIndexedIndirect, &DrawIndexedIndirect);
    wrap(functions.cmdCopyBufferToImage, &CopyBufferToImage);
    wrap(functions.cmdCopyImageToBuffer, &CopyImageToBuffer);
    wrap(functions.cmdClearColorImage, &ClearColorImage);
    wrap(functions.updateDescriptorSets, &UpdateDescriptorSets);
    wrap(functions.allocateDescriptorSets, &AllocateDescriptorSets);
}

void EndFrame() {
    static const bool enabled = std::getenv("APS5_VK_STREAM_CENSUS") != nullptr;
    if (!enabled) return;
    auto& census = state();
    const std::lock_guard lock(census.mutex);
    if (census.names.empty()) return;
    for (std::size_t kind = 0; kind < static_cast<std::size_t>(Kind::Count); ++kind) {
        auto& commands = census.current[kind];
        const auto matched = [&](std::unordered_map<std::uint64_t, std::uint32_t> remaining) {
            std::uint64_t same = 0;
            for (const auto hash : commands) {
                const auto found = remaining.find(hash);
                if (found == remaining.end() || found->second == 0) continue;
                --found->second;
                ++same;
            }
            return same;
        };
        ArchB::Recorder::Count(census.names[kind * 3].c_str(), static_cast<double>(commands.size()));
        ArchB::Recorder::Count(census.names[kind * 3 + 1].c_str(), static_cast<double>(matched(census.previous[kind])));
        ArchB::Recorder::Count(census.names[kind * 3 + 2].c_str(), static_cast<double>(matched(census.older[kind])));
        census.older[kind] = std::move(census.previous[kind]);
        census.previous[kind].clear();
        for (const auto hash : commands) ++census.previous[kind][hash];
        commands.clear();
    }
    ArchB::Recorder::Count("vk_stream_descriptor_updates", static_cast<double>(census.descriptorUpdates));
    ArchB::Recorder::Count("vk_stream_descriptor_writes", static_cast<double>(census.descriptorWrites));
    ArchB::Recorder::Count("vk_stream_sets_allocated", static_cast<double>(census.setsAllocated));
    census.descriptorUpdates = census.descriptorWrites = census.setsAllocated = 0;
}

}
