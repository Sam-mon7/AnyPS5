#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ImageMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context Context(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

namespace {

using namespace AgcDriver::Graphics;

constexpr std::uint64_t MiB = std::uint64_t{1} << 20u;
constexpr VkFormat TestFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2D = 9;

struct TestImage {
    explicit TestImage(const Context& context) : context(context) {}
    TestImage(const TestImage&) = delete;
    TestImage& operator=(const TestImage&) = delete;
    std::size_t Bytes() const { return static_cast<std::size_t>(side) * side * 4u; }

    Context context;
    VkImage image = VK_NULL_HANDLE;
    std::shared_ptr<OwnedImage> owned;
    std::uint32_t side = 0;
    std::uint32_t seed = 0;
};

std::unique_ptr<TestImage> MakeImage(const Context& context, std::uint32_t side, std::uint32_t seed) {
    auto result = std::make_unique<TestImage>(context);
    result->side = side;
    result->seed = seed;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = TestFormat;
    info.extent = {side, side, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &result->image), "vkCreateImage");
    result->owned = std::make_shared<OwnedImage>(context, result->image, VK_NULL_HANDLE);
    VkMemoryRequirements requirements{};
    context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, result->image, &requirements);
    const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, ImageSuballocationLevel() == 0 ? context.DeviceMemoryType(requirements.memoryTypeBits, requirements.size, "test image") : 0};
    result->owned->Allocate(context, allocation, requirements, "vkAllocateMemory test image");
    Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, result->image, result->owned->memory, result->owned->memoryRange.Offset()), "vkBindImageMemory test image");
    return result;
}

std::byte Pattern(const TestImage& image, std::size_t index) {
    return static_cast<std::byte>((index * 31u + image.seed * 17u + (index >> 8u)) & 0xffu);
}

void Barrier(const Context& context, VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags source, VkAccessFlags destination) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = source;
    barrier.dstAccessMask = destination;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void Upload(const Context& context, TestImage& image) {
    Buffer staging(context, image.Bytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const auto bytes = staging.Bytes();
    for (std::size_t i = 0; i < image.Bytes(); ++i) bytes[i] = Pattern(image, i);
    CommandBatch batch(context);
    const auto commands = batch.Handle();
    Barrier(context, commands, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {image.side, image.side, 1};
    context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, staging.Handle(), image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    Barrier(context, commands, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    batch.SubmitAndWait();
}

std::size_t Mismatches(const Context& context, TestImage& image) {
    Buffer readback(context, image.Bytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    CommandBatch batch(context);
    const auto commands = batch.Handle();
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {image.side, image.side, 1};
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.Handle(), 1, &region);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    batch.SubmitAndWait();
    const auto bytes = readback.Bytes();
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < image.Bytes(); ++i) wrong += bytes[i] != Pattern(image, i) ? 1u : 0u;
    return wrong;
}

void RequireIntact(const Context& context, const std::vector<std::unique_ptr<TestImage>>& images, const std::string& when) {
    for (std::size_t i = 0; i < images.size(); ++i) {
        if (images[i] == nullptr) continue;
        const auto wrong = Mismatches(context, *images[i]);
        Require(wrong == 0, when + ": image " + std::to_string(i) + " lost " + std::to_string(wrong) + " bytes to another image sharing its block");
    }
}

constexpr std::array<std::uint32_t, 4> Sides{64, 128, 256, 512};

void RawImages(const Context& context) {
    auto pool = context.bufferPool;
    Require(pool != nullptr, "the device's context has no buffer pool");
    const auto before = pool->ImageStats();
    std::vector<std::unique_ptr<TestImage>> images;
    for (std::uint32_t i = 0; i < 12; ++i) {
        images.push_back(MakeImage(context, Sides[i % Sides.size()], i + 1));
        Upload(context, *images.back());
    }
    RequireIntact(context, images, "after the first 12 images");
    for (std::size_t i = 0; i < images.size(); i += 2) images[i].reset();
    for (std::uint32_t i = 0; i < 6; ++i) {
        images.push_back(MakeImage(context, Sides[(i + 1) % Sides.size()], 100 + i));
        Upload(context, *images.back());
    }
    RequireIntact(context, images, "after replacing half of the images");
    const auto middle = pool->ImageStats();
    std::uint64_t fallbacks = 0;
    for (std::size_t reason = 0; reason < static_cast<std::size_t>(ImageFallback::Count); ++reason) fallbacks += middle.fallbacks[reason] - before.fallbacks[reason];
    if (ImageSuballocationLevel() != 0) Require(middle.allocations - before.allocations + fallbacks == 18, "images were not all suballocated or counted as fallbacks");
    else Require(middle.blocksMade == 0 && fallbacks == 0, "disabled path used image blocks");
    images.clear();
    const auto after = pool->ImageStats();
    Require(after.used == 0 && after.blocks <= 1, "freeing every image left ranges in use or more than one spare block");
    std::printf("raw images: %llu suballocated in %llu blocks made, %llu blocks released\n", static_cast<unsigned long long>(middle.allocations - before.allocations), static_cast<unsigned long long>(middle.blocksMade - before.blocksMade), static_cast<unsigned long long>(after.blocksReleased - before.blocksReleased));
}

std::array<std::uint32_t, 8> Descriptor(std::uint64_t address, std::uint32_t side) {
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32UInt << 20u) | (((side - 1u) & 3u) << 30u),
        ((side - 1u) >> 2u) | ((side - 1u) << 14u),
        0xfacu | (TileR64KBX << 20u) | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

std::unique_ptr<Texture> MakeTexture(const Context& context, std::uint64_t address, std::uint32_t side, std::byte fill) {
    const auto resource = DecodeTextureResource(Descriptor(address, side));
    const std::vector<std::byte> snapshot(static_cast<std::size_t>(DescribeSurface(resource).guestBytes), fill);
    const VkComponentMapping identity{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    return std::make_unique<Texture>(context, *context.detiler, resource, identity, snapshot);
}

std::uint64_t Dedicated(const ImageBlockStats& stats, ImageFallback reason) {
    return stats.fallbacks[static_cast<std::size_t>(reason)];
}

void Textures(const Context& context, AgcDriver::VulkanDevice& device) {
    const auto drain = [&] {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        device.WaitIdle();
    };
    auto pool = context.bufferPool;
    constexpr std::size_t Count = 6;
    const auto before = pool->ImageStats();
    const auto memoryBefore = SampledTextureMemory();
    std::vector<std::unique_ptr<Texture>> textures;
    VkDeviceSize requested = 0;
    for (std::size_t i = 0; i < Count; ++i) {
        textures.push_back(MakeTexture(context, 0x400000000ull + i * 0x1000000ull, 128u << (i % 3u), std::byte{0x5a}));
        requested += textures.back()->AllocationBytes();
    }
    Require(SampledTextureMemory() - memoryBefore == requested, "the sampled texture memory counter does not follow the images' sizes");
    const auto during = pool->ImageStats();
    const auto suballocated = during.allocations - before.allocations;
    std::uint64_t dedicated = 0;
    for (std::size_t reason = 0; reason < static_cast<std::size_t>(ImageFallback::Count); ++reason) dedicated += during.fallbacks[reason] - before.fallbacks[reason];
    if (ImageSuballocationLevel() != 0) {
        Require(suballocated + dedicated == Count, "textures were neither suballocated nor counted as dedicated");
        VkDeviceSize slack = 0;
        for (std::uint32_t heap = 0; heap < context.memory.memoryHeapCount; ++heap) slack += ImageBlockSlack(context, heap);
        Require(slack == during.reserved - during.used, "the slack is not the unused part of the blocks");
        if (ImageSuballocationLevel() == 2) Require(Dedicated(during, ImageFallback::Preferred) == Dedicated(before, ImageFallback::Preferred), "a preferred dedicated allocation was made with the preference ignored");
    } else {
        Require(suballocated == 0 && dedicated == 0 && during.blocks == before.blocks, "textures used image blocks with the switch off");
    }
    std::printf("%zu textures: %llu suballocated, %llu dedicated (large %llu, required %llu, preferred %llu, no block %llu), %llu blocks, slack %llu MiB\n", Count, static_cast<unsigned long long>(suballocated), static_cast<unsigned long long>(dedicated),
        static_cast<unsigned long long>(Dedicated(during, ImageFallback::Large) - Dedicated(before, ImageFallback::Large)),
        static_cast<unsigned long long>(Dedicated(during, ImageFallback::Required) - Dedicated(before, ImageFallback::Required)),
        static_cast<unsigned long long>(Dedicated(during, ImageFallback::Preferred) - Dedicated(before, ImageFallback::Preferred)),
        static_cast<unsigned long long>(Dedicated(during, ImageFallback::NoBlock) - Dedicated(before, ImageFallback::NoBlock)),
        static_cast<unsigned long long>(during.blocks), static_cast<unsigned long long>(ImageBlockSlack(context, LargestDeviceLocalHeap(context.memory).value_or(0)) / MiB));
    textures.erase(textures.begin(), textures.begin() + static_cast<std::ptrdiff_t>(Count / 2));
    drain();
    for (std::size_t i = 0; i < Count / 2; ++i) textures.push_back(MakeTexture(context, 0x500000000ull + i * 0x1000000ull, 256, std::byte{0x33}));
    const auto reused = pool->ImageStats();
    if (ImageSuballocationLevel() != 0) Require(reused.blocksMade == during.blocksMade, "textures made after others were destroyed made new blocks instead of reusing the freed ranges");
    textures.clear();
    drain();
    Require(SampledTextureMemory() == memoryBefore, "the sampled texture memory counter did not return to its start");
    const auto after = pool->ImageStats();
    Require(after.used == before.used && after.allocations - before.allocations == after.frees - before.frees, "destroying the textures left ranges in use");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto context = AgcDriver::VulkanDeviceTestAccess::Context(*device);
        GetBufferPool(context);
        Require(context.detiler != nullptr, "the device's context has no texture detiler");
        RawImages(context);
        Textures(context, *device);
        std::puts("image suballocation tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
