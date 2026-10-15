#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A guest texture descriptor may name more mip levels than its extent halves into: Demon's Souls
// (PPSA01342) samples a 28x1 BC3 array through a T# of 10 levels. A Vulkan image holds at most
// floor(log2(max extent)) + 1 = 5 of them (VUID-VkImageCreateInfo-mipLevels-00947); NVIDIA sizes a
// 28x1 BC3 image of 10 levels exactly like one of 5, so copying the guest's levels 5-9 into it
// wrote past its memory and lost the device (write fault at the end of the image). These tests run
// the sampled texture upload and a storage image's upload, write-back, views and fills against a
// recording mock device and check that the image and every command on it stay inside the levels
// the image can have, and that an access that would write a level past them throws.
namespace {

using namespace AgcDriver::Graphics;

constexpr std::uint32_t FormatBc3Srgb = 174;
constexpr std::uint32_t FormatR8G8B8A8Unorm = 56;

struct MockDevice {
    std::uintptr_t next = 0x10000;
    std::map<VkImage, VkImageCreateInfo> images;
    std::vector<std::pair<VkImage, VkImageSubresourceLayers>> copies;
    std::vector<std::pair<VkImage, VkImageSubresourceRange>> barriers;
    std::vector<std::pair<VkImage, VkImageSubresourceRange>> views;
    std::vector<std::pair<VkImage, VkImageSubresourceLayers>> readbacks;
    std::vector<std::pair<VkImage, VkImageSubresourceRange>> clears;
    std::map<VkBuffer, VkDeviceSize> bufferSizes;
    std::map<VkDeviceMemory, std::vector<std::byte>> memories;
};

MockDevice mock;

// floor(log2(max extent)) + 1: the most levels a Vulkan image of that extent may have.
std::uint32_t fullChain(const VkExtent3D& extent) {
    return static_cast<std::uint32_t>(std::bit_width(std::max({extent.width, extent.height, extent.depth, 1u})));
}

template<typename THandle>
THandle newHandle() {
    return reinterpret_cast<THandle>(mock.next += 0x10);
}

VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice, const VkImageCreateInfo* info, const VkAllocationCallbacks*, VkImage* image) {
    *image = newHandle<VkImage>();
    mock.images[*image] = *info;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL imageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements* requirements) {
    *requirements = {65536, 1024, 0x3};
}

VKAPI_ATTR VkResult VKAPI_CALL bindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) {
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice, VkImage, const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL createImageView(VkDevice, const VkImageViewCreateInfo* info, const VkAllocationCallbacks*, VkImageView* view) {
    *view = newHandle<VkImageView>();
    mock.views.emplace_back(info->image, info->subresourceRange);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL createBuffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*, VkBuffer* buffer) {
    *buffer = newHandle<VkBuffer>();
    mock.bufferSizes[*buffer] = info->size;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL bufferMemoryRequirements(VkDevice, VkBuffer buffer, VkMemoryRequirements* requirements) {
    *requirements = {(mock.bufferSizes.at(buffer) + 255u) / 256u * 256u, 256, 0x3};
}

VKAPI_ATTR VkResult VKAPI_CALL allocateMemory(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* memory) {
    *memory = newHandle<VkDeviceMemory>();
    mock.memories[*memory].resize(static_cast<std::size_t>(info->allocationSize));
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL bindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize, VkMemoryMapFlags, void** data) {
    *data = mock.memories.at(memory).data() + offset;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL unmapMemory(VkDevice, VkDeviceMemory) {}
VKAPI_ATTR void VKAPI_CALL destroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*) { mock.bufferSizes.erase(buffer); }
VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) { mock.memories.erase(memory); }

VKAPI_ATTR VkResult VKAPI_CALL createDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* layout) {
    *layout = newHandle<VkDescriptorSetLayout>();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL createPipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout* layout) {
    *layout = newHandle<VkPipelineLayout>();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL createShaderModule(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule* module) {
    *module = newHandle<VkShaderModule>();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL createComputePipelines(VkDevice, VkPipelineCache, std::uint32_t count, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline* pipelines) {
    for (std::uint32_t index = 0; index < count; ++index) pipelines[index] = newHandle<VkPipeline>();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL createDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool* pool) {
    *pool = newHandle<VkDescriptorPool>();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroyDescriptorPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL resetDescriptorPool(VkDevice, VkDescriptorPool, VkDescriptorPoolResetFlags) { return VK_SUCCESS; }

VKAPI_ATTR VkResult VKAPI_CALL allocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* sets) {
    for (std::uint32_t index = 0; index < info->descriptorSetCount; ++index) sets[index] = newHandle<VkDescriptorSet>();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL updateDescriptorSets(VkDevice, std::uint32_t, const VkWriteDescriptorSet*, std::uint32_t, const VkCopyDescriptorSet*) {}

VKAPI_ATTR VkResult VKAPI_CALL allocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* commands) {
    for (std::uint32_t index = 0; index < info->commandBufferCount; ++index) commands[index] = newHandle<VkCommandBuffer>();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL freeCommandBuffers(VkDevice, VkCommandPool, std::uint32_t, const VkCommandBuffer*) {}

VKAPI_ATTR VkResult VKAPI_CALL createFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence) {
    *fence = newHandle<VkFence>();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroyFence(VkDevice, VkFence, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL beginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL endCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL queueSubmit(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL waitForFences(VkDevice, std::uint32_t, const VkFence*, VkBool32, std::uint64_t) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL getFenceStatus(VkDevice, VkFence) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL queueWaitIdle(VkQueue) { return VK_SUCCESS; }

VKAPI_ATTR void VKAPI_CALL cmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t count, const VkImageMemoryBarrier* barriers) {
    for (std::uint32_t index = 0; index < count; ++index) mock.barriers.emplace_back(barriers[index].image, barriers[index].subresourceRange);
}

VKAPI_ATTR void VKAPI_CALL cmdCopyBuffer(VkCommandBuffer, VkBuffer, VkBuffer, std::uint32_t, const VkBufferCopy*) {}

VKAPI_ATTR void VKAPI_CALL cmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage image, VkImageLayout, std::uint32_t count, const VkBufferImageCopy* regions) {
    for (std::uint32_t index = 0; index < count; ++index) mock.copies.emplace_back(image, regions[index].imageSubresource);
}

VKAPI_ATTR void VKAPI_CALL cmdCopyImageToBuffer(VkCommandBuffer, VkImage image, VkImageLayout, VkBuffer, std::uint32_t count, const VkBufferImageCopy* regions) {
    for (std::uint32_t index = 0; index < count; ++index) mock.readbacks.emplace_back(image, regions[index].imageSubresource);
}

VKAPI_ATTR void VKAPI_CALL cmdClearColorImage(VkCommandBuffer, VkImage image, VkImageLayout, const VkClearColorValue*, std::uint32_t count, const VkImageSubresourceRange* ranges) {
    for (std::uint32_t index = 0; index < count; ++index) mock.clears.emplace_back(image, ranges[index]);
}

VKAPI_ATTR void VKAPI_CALL cmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL cmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, std::uint32_t, std::uint32_t, const VkDescriptorSet*, std::uint32_t, const std::uint32_t*) {}
VKAPI_ATTR void VKAPI_CALL cmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t, std::uint32_t, const void*) {}
VKAPI_ATTR void VKAPI_CALL cmdDispatch(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t) {}

PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkCreateImage", reinterpret_cast<PFN_vkVoidFunction>(createImage)},
        {"vkGetImageMemoryRequirements", reinterpret_cast<PFN_vkVoidFunction>(imageMemoryRequirements)},
        {"vkBindImageMemory", reinterpret_cast<PFN_vkVoidFunction>(bindImageMemory)},
        {"vkDestroyImage", reinterpret_cast<PFN_vkVoidFunction>(destroyImage)},
        {"vkCreateImageView", reinterpret_cast<PFN_vkVoidFunction>(createImageView)},
        {"vkDestroyImageView", reinterpret_cast<PFN_vkVoidFunction>(destroyImageView)},
        {"vkCreateBuffer", reinterpret_cast<PFN_vkVoidFunction>(createBuffer)},
        {"vkGetBufferMemoryRequirements", reinterpret_cast<PFN_vkVoidFunction>(bufferMemoryRequirements)},
        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(allocateMemory)},
        {"vkBindBufferMemory", reinterpret_cast<PFN_vkVoidFunction>(bindBufferMemory)},
        {"vkMapMemory", reinterpret_cast<PFN_vkVoidFunction>(mapMemory)},
        {"vkUnmapMemory", reinterpret_cast<PFN_vkVoidFunction>(unmapMemory)},
        {"vkDestroyBuffer", reinterpret_cast<PFN_vkVoidFunction>(destroyBuffer)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(freeMemory)},
        {"vkCreateDescriptorSetLayout", reinterpret_cast<PFN_vkVoidFunction>(createDescriptorSetLayout)},
        {"vkDestroyDescriptorSetLayout", reinterpret_cast<PFN_vkVoidFunction>(destroyDescriptorSetLayout)},
        {"vkCreatePipelineLayout", reinterpret_cast<PFN_vkVoidFunction>(createPipelineLayout)},
        {"vkDestroyPipelineLayout", reinterpret_cast<PFN_vkVoidFunction>(destroyPipelineLayout)},
        {"vkCreateShaderModule", reinterpret_cast<PFN_vkVoidFunction>(createShaderModule)},
        {"vkDestroyShaderModule", reinterpret_cast<PFN_vkVoidFunction>(destroyShaderModule)},
        {"vkCreateComputePipelines", reinterpret_cast<PFN_vkVoidFunction>(createComputePipelines)},
        {"vkDestroyPipeline", reinterpret_cast<PFN_vkVoidFunction>(destroyPipeline)},
        {"vkCreateDescriptorPool", reinterpret_cast<PFN_vkVoidFunction>(createDescriptorPool)},
        {"vkDestroyDescriptorPool", reinterpret_cast<PFN_vkVoidFunction>(destroyDescriptorPool)},
        {"vkResetDescriptorPool", reinterpret_cast<PFN_vkVoidFunction>(resetDescriptorPool)},
        {"vkAllocateDescriptorSets", reinterpret_cast<PFN_vkVoidFunction>(allocateDescriptorSets)},
        {"vkUpdateDescriptorSets", reinterpret_cast<PFN_vkVoidFunction>(updateDescriptorSets)},
        {"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(allocateCommandBuffers)},
        {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(freeCommandBuffers)},
        {"vkCreateFence", reinterpret_cast<PFN_vkVoidFunction>(createFence)},
        {"vkDestroyFence", reinterpret_cast<PFN_vkVoidFunction>(destroyFence)},
        {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(beginCommandBuffer)},
        {"vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(endCommandBuffer)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(queueSubmit)},
        {"vkWaitForFences", reinterpret_cast<PFN_vkVoidFunction>(waitForFences)},
        {"vkGetFenceStatus", reinterpret_cast<PFN_vkVoidFunction>(getFenceStatus)},
        {"vkQueueWaitIdle", reinterpret_cast<PFN_vkVoidFunction>(queueWaitIdle)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(cmdPipelineBarrier)},
        {"vkCmdCopyBuffer", reinterpret_cast<PFN_vkVoidFunction>(cmdCopyBuffer)},
        {"vkCmdCopyBufferToImage", reinterpret_cast<PFN_vkVoidFunction>(cmdCopyBufferToImage)},
        {"vkCmdCopyImageToBuffer", reinterpret_cast<PFN_vkVoidFunction>(cmdCopyImageToBuffer)},
        {"vkCmdClearColorImage", reinterpret_cast<PFN_vkVoidFunction>(cmdClearColorImage)},
        {"vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(cmdBindPipeline)},
        {"vkCmdBindDescriptorSets", reinterpret_cast<PFN_vkVoidFunction>(cmdBindDescriptorSets)},
        {"vkCmdPushConstants", reinterpret_cast<PFN_vkVoidFunction>(cmdPushConstants)},
        {"vkCmdDispatch", reinterpret_cast<PFN_vkVoidFunction>(cmdDispatch)},
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

void formatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties* properties) {
    *properties = {};
    properties->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
}

Context mockContext() {
    static int deviceTag = 0;
    static int physicalTag = 0;
    Context context{};
    context.device = reinterpret_cast<VkDevice>(&deviceTag);
    context.physical = reinterpret_cast<VkPhysicalDevice>(&physicalTag);
    context.formatProperties = formatProperties;
    context.deviceProc = deviceProc;
    context.memory.memoryTypeCount = 2;
    context.memory.memoryTypes[0] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    context.memory.memoryTypes[1] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1};
    context.memory.memoryHeapCount = 2;
    context.memory.memoryHeaps[0] = {std::uint64_t{1} << 32u, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    context.memory.memoryHeaps[1] = {std::uint64_t{1} << 32u, 0};
    context.limits.maxMemoryAllocationCount = 4096;
    context.limits.nonCoherentAtomSize = 64;
    context.limits.minStorageBufferOffsetAlignment = 16;
    context.limits.maxStorageBufferRange = 1u << 30u;
    context.limits.maxImageDimension1D = 16384;
    context.limits.maxImageDimension2D = 16384;
    context.limits.maxImageDimension3D = 2048;
    context.limits.maxImageDimensionCube = 16384;
    context.limits.maxImageArrayLayers = 2048;
    context.textureCompressionBC = true;
    return context;
}

GuestTextureResource arrayTexture(std::uint64_t address, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t layers, std::uint32_t mipCount) {
    GuestTextureResource descriptor{};
    descriptor.baseAddress = address;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.depthOrLastArray = layers - 1u;
    descriptor.mipCount = mipCount;
    descriptor.lastLevel = mipCount - 1u;
    descriptor.tileMode = TextureTileMode::kStandard64KB;
    descriptor.dimension = TextureDimension::k2DArray;
    descriptor.format = format;
    descriptor.dstSelX = 4;
    descriptor.dstSelY = 5;
    descriptor.dstSelZ = 6;
    descriptor.dstSelW = 7;
    return descriptor;
}

void clearRecords() {
    mock.images.clear();
    mock.copies.clear();
    mock.barriers.clear();
    mock.views.clear();
    mock.readbacks.clear();
    mock.clears.clear();
}

// Checks that every copy into or out of `image`, and every barrier, clear and view of it, stays
// inside the levels and layers it was created with, and that it has no more levels than its extent.
void checkInsideImage(VkImage image, const VkImageCreateInfo& info, const std::string& what) {
    const auto levels = std::to_string(info.mipLevels);
    Require(info.mipLevels <= fullChain(info.extent), what + ": the image was created with " + levels + " mip levels, more than its " + std::to_string(info.extent.width) + "x" + std::to_string(info.extent.height) + " extent can hold (" + std::to_string(fullChain(info.extent)) + ")");
    const auto checkLayers = [&](const VkImageSubresourceLayers& layers, const char* access) {
        Require(layers.mipLevel < info.mipLevels, what + ": " + access + " level " + std::to_string(layers.mipLevel) + " of an image of " + levels + " levels");
        Require(layers.baseArrayLayer + layers.layerCount <= info.arrayLayers, what + ": " + access + " past the image's layers");
    };
    const auto checkRange = [&](const VkImageSubresourceRange& range, const char* access) {
        Require(range.levelCount != VK_REMAINING_MIP_LEVELS && range.levelCount != 0 && range.baseMipLevel + range.levelCount <= info.mipLevels, what + ": " + access + " names levels " + std::to_string(range.baseMipLevel) + "+" + std::to_string(range.levelCount) + " of an image of " + levels + " levels");
    };
    for (const auto& [target, layers] : mock.copies) {
        if (target == image) checkLayers(layers, "a copy wrote");
    }
    for (const auto& [target, layers] : mock.readbacks) {
        if (target == image) checkLayers(layers, "a copy read");
    }
    for (const auto& [target, range] : mock.barriers) {
        if (target == image) checkRange(range, "a barrier");
    }
    for (const auto& [target, range] : mock.clears) {
        if (target == image) checkRange(range, "a clear");
    }
    for (const auto& [target, range] : mock.views) {
        if (target == image) checkRange(range, "a view");
    }
}

// Uploads `descriptor` through the sampled texture constructor and returns its image's create info,
// after checking that every copy, barrier and view of that image stays inside its levels and layers.
VkImageCreateInfo upload(const Context& context, TextureDetiler& detiler, const GuestTextureResource& descriptor, const std::string& what) {
    clearRecords();
    const auto geometry = DescribeSurface(descriptor);
    const std::vector<std::byte> snapshot(static_cast<std::size_t>(geometry.guestBytes));
    VkImageCreateInfo info{};
    {
        const auto texture = std::make_shared<Texture>(context, detiler, descriptor, VkComponentMapping{}, snapshot);
        Require(mock.images.size() == 1, what + ": the upload did not create exactly one image");
        const auto image = mock.images.begin()->first;
        info = mock.images.begin()->second;
        for (const auto& [target, layers] : mock.copies) Require(target == image, what + ": a copy went to another image");
        Require(!mock.views.empty(), what + ": the texture made no view");
        for (const auto& [target, range] : mock.views) Require(target == image, what + ": a view names another image");
        checkInsideImage(image, info, what);
    }
    return info;
}

std::size_t copiesOfLevel(std::uint32_t level) {
    std::size_t count = 0;
    for (const auto& [target, layers] : mock.copies) count += layers.mipLevel == level ? 1u : 0u;
    return count;
}

void TestLevelsPastTheFullChain(const Context& context, TextureDetiler& detiler) {
    // The descriptor of the fault: 28x1 BC3 sRGB, 10 levels (4 layers here, 256 in the title).
    constexpr std::uint32_t layers = 4;
    const auto descriptor = arrayTexture(0x1003000000, FormatBc3Srgb, 28, 1, layers, 10);
    const auto info = upload(context, detiler, descriptor, "28x1 BC3 with 10 levels");
    Require(info.mipLevels == 5, "a 28x1 BC3 texture of 10 guest levels must keep the 5 levels its extent has");
    for (std::uint32_t level = 0; level < 5; ++level) Require(copiesOfLevel(level) == layers, "level " + std::to_string(level) + " of every layer must be uploaded");

    // A view of guest levels past the image's (base 7..9) reads the image's last level.
    auto pastChain = descriptor;
    pastChain.baseLevel = 7;
    upload(context, detiler, pastChain, "a view of levels 7-9 of a 28x1 surface");
    Require(mock.views.front().second.baseMipLevel == 4 && mock.views.front().second.levelCount == 1, "a view of levels past the image's last one must read its last level");

    // 4x1 BC3 with 11 levels (ds-c0078/0080/0081): 3 levels.
    const auto narrow = upload(context, detiler, arrayTexture(0x1021400000, FormatBc3Srgb, 4, 1, 2, 11), "4x1 BC3 with 11 levels");
    Require(narrow.mipLevels == 3, "a 4x1 texture of 11 guest levels must keep 3 levels");
}

void TestFullChainsAreUnchanged(const Context& context, TextureDetiler& detiler) {
    const auto full = upload(context, detiler, arrayTexture(0x1068400000, FormatBc3Srgb, 64, 64, 2, 7), "64x64 BC3 with 7 levels");
    Require(full.mipLevels == 7, "a full 64x64 chain of 7 levels must keep all of them");
    for (std::uint32_t level = 0; level < 7; ++level) Require(copiesOfLevel(level) == 2, "every level of a full chain must be uploaded");
    const auto partial = upload(context, detiler, arrayTexture(0x1068500000, FormatR8G8B8A8Unorm, 64, 32, 1, 3), "64x32 RGBA8 with 3 levels");
    Require(partial.mipLevels == 3, "a partial chain must keep its own level count");
}

// Committed, registered host memory standing for a guest surface (storage images read and store
// guest memory, and drop results over memory no allocation registers).
class GuestSurface {
public:
    explicit GuestSurface(std::size_t bytes) : bytes((bytes + Alignment - 1) / Alignment * Alignment), data(::operator new(this->bytes, std::align_val_t{Alignment})) {
        std::memset(data, 0x5a, this->bytes);
        GuestAllocations::Mutation mutation;
        mutation.Add(data, this->bytes, true, true, true);
    }
    ~GuestSurface() {
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(data);
        }
        ::operator delete(data, std::align_val_t{Alignment});
    }
    GuestSurface(const GuestSurface&) = delete;
    GuestSurface& operator=(const GuestSurface&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }

private:
    static constexpr std::size_t Alignment = 65536;
    std::size_t bytes;
    void* data;
};

GuestTextureResource storageSurface(std::uint32_t width, std::uint32_t height, std::uint32_t mipCount) {
    GuestTextureResource descriptor{};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.mipCount = mipCount;
    descriptor.lastLevel = mipCount - 1u;
    descriptor.tileMode = TextureTileMode::kStandard64KB;
    descriptor.dimension = TextureDimension::k2D;
    descriptor.format = FormatR8G8B8A8Unorm;
    descriptor.dstSelX = 4;
    descriptor.dstSelY = 5;
    descriptor.dstSelZ = 6;
    descriptor.dstSelW = 7;
    return descriptor;
}

void requireThrows(const std::function<void()>& action, const std::string& expected, const std::string& what) {
    try {
        action();
    } catch (const std::exception& error) {
        Require(std::string_view(error.what()).find(expected) != std::string_view::npos, what + ": threw \"" + error.what() + "\" instead of \"" + expected + "\"");
        return;
    }
    Require(false, what + ": did not throw");
}

// The single image the storage texture made: its handle and create info.
std::pair<VkImage, VkImageCreateInfo> storageImage(const std::string& what) {
    Require(mock.images.size() == 1, what + ": the storage texture did not create exactly one image");
    return *mock.images.begin();
}

void TestStorageLevelsPastTheFullChain(const Context& context, TextureDetiler& detiler) {
    // A 28x1 RGBA8 storage surface of 10 levels: the image holds levels 0-4, the last of them 1x1.
    auto descriptor = storageSurface(28, 1, 10);
    GuestSurface memory(static_cast<std::size_t>(DescribeSurface(descriptor).guestBytes));
    descriptor.baseAddress = memory.Address();
    clearRecords();
    {
        const auto texture = StorageTexture::Create(context, detiler, descriptor, 0);
        const auto [image, info] = storageImage("28x1 storage with 10 levels");
        Require(info.mipLevels == 5, "a 28x1 storage image of 10 guest levels must keep the 5 levels its extent has, not " + std::to_string(info.mipLevels));
        for (std::uint32_t level = 0; level < 5; ++level) Require(copiesOfLevel(level) == 1, "level " + std::to_string(level) + " of the storage surface must be uploaded");
        checkInsideImage(image, info, "28x1 storage upload");

        static_cast<void>(texture->StorageView(4, false));
        checkInsideImage(image, info, "a storage view of the last level the image holds");
        requireThrows([&] { static_cast<void>(texture->StorageView(5, false)); }, "a storage image view of mip level 5 of a 28x1x1 surface of 10 levels is not implemented: a Vulkan image of that extent holds 5 levels", "a storage view of level 5 of a 28x1 surface");
        Require(texture->Attachable(), "the mock RGBA8 storage image must be attachable");
        requireThrows([&] { static_cast<void>(texture->AttachmentView(VK_FORMAT_R8G8B8A8_UNORM, 7)); }, "a color target of mip level 7", "a color target at level 7 of a 28x1 surface");
        checkInsideImage(image, info, "28x1 storage views");

        // A fill of the whole surface reaches levels 5-9 too, which a clear of the image would not.
        const std::array<std::uint32_t, 4> pattern{0, 0, 0, 0};
        const char* refusal = nullptr;
        Require(!texture->FillClear(pattern, StorageTexture::WholeImage, refusal), "a fill of a surface with levels past its image must not become a clear of the image");
        Require(refusal != nullptr && std::string_view(refusal) == "levels past the image", std::string("the fill was refused as \"") + (refusal != nullptr ? refusal : "") + "\", not for the levels past the image");

        // So does a buffer copy of another such surface over this one.
        auto otherDescriptor = descriptor;
        GuestSurface otherMemory(static_cast<std::size_t>(DescribeSurface(otherDescriptor).guestBytes));
        otherDescriptor.baseAddress = otherMemory.Address();
        const auto other = StorageTexture::Create(context, detiler, otherDescriptor, 0);
        refusal = nullptr;
        Require(!texture->CopyFrom(*other, refusal), "a buffer copy between surfaces with levels past their images must not become an image copy");
        Require(refusal != nullptr && std::string_view(refusal) == "levels past the image", std::string("the copy was refused as \"") + (refusal != nullptr ? refusal : "") + "\", not for the levels past the image");

        // The write-back reads the levels the image holds, and nothing past them.
        mock.readbacks.clear();
        texture->MarkDirty();
        texture->WriteBack();
        for (std::uint32_t level = 0; level < 5; ++level) {
            Require(std::any_of(mock.readbacks.begin(), mock.readbacks.end(), [&](const auto& readback) { return readback.first == image && readback.second.mipLevel == level; }), "the write-back must read level " + std::to_string(level) + " of the storage image");
        }
        checkInsideImage(image, info, "28x1 storage write-back");
    }

    // Creating the storage image for a write of level 7 throws before any image exists.
    clearRecords();
    requireThrows([&] { static_cast<void>(StorageTexture::Create(context, detiler, descriptor, 7)); }, "a storage image access of mip level 7 of a 28x1x1 surface of 10 levels is not implemented", "a storage image for level 7 of a 28x1 surface");
    Require(mock.images.empty(), "a storage image for a level past the image's must not be created");
}

void TestStorageFullChainIsUnchanged(const Context& context, TextureDetiler& detiler) {
    auto descriptor = storageSurface(64, 32, 7);
    GuestSurface memory(static_cast<std::size_t>(DescribeSurface(descriptor).guestBytes));
    descriptor.baseAddress = memory.Address();
    clearRecords();
    const auto texture = StorageTexture::Create(context, detiler, descriptor, 6);
    const auto [image, info] = storageImage("64x32 storage with 7 levels");
    Require(info.mipLevels == 7, "a full 64x32 storage chain of 7 levels must keep all of them");
    for (std::uint32_t level = 0; level < 7; ++level) Require(copiesOfLevel(level) == 1, "every level of a full storage chain must be uploaded");
    checkInsideImage(image, info, "64x32 storage upload");
}

}

void RunTextureMipChainTests() {
    Require(Recorder::Active() == nullptr, "the texture mip chain tests record into their own command batches");
    mock = MockDevice{};
    const auto context = mockContext();
    {
        TextureDetiler detiler(context);
        TestLevelsPastTheFullChain(context, detiler);
        TestFullChainsAreUnchanged(context, detiler);
        TestStorageLevelsPastTheFullChain(context, detiler);
        TestStorageFullChainIsUnchanged(context, detiler);
    }
    mock = MockDevice{};
}
