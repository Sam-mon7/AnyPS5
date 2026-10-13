#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

namespace {
using namespace AgcDriver::Graphics;

struct Allocation {
    const Context& context;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    ~Allocation() {
        if (buffer) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
        if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
        if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
    }

    void Allocate(const VkMemoryRequirements& requirements, bool addressable) {
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = addressable ? &flags : nullptr;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, 0);
        Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocate, nullptr, &memory), "binding smoke allocation");
    }
};

bool HasObject(const GpuAddressBindings::Matches& matches, VkObjectType type, std::uint64_t handle, VkDeviceAddressBindingTypeEXT operation, std::uint64_t after) {
    return std::any_of(matches.entries.begin(), matches.entries.begin() + matches.count, [&](const auto& entry) { return entry.serial > after && entry.objectType == type && entry.object == handle && entry.bindingType == operation && entry.size != 0; });
}
}

int main(int argc, char** argv) {
    try {
        const bool overlap = argc == 2 && std::strcmp(argv[1], "--overlap-compile-only") == 0;
        Require(argc == 1 || overlap, "usage: agc_driver_gpu_address_bindings_tests [--overlap-compile-only]");
#ifdef _WIN32
        Require(_putenv_s("APS5_GPU_ADDRESS_BINDINGS", "1") == 0 && _putenv_s("APS5_GPU_CHECKPOINTS", "") == 0, "cannot set binding-only diagnostic environment");
#else
        Require(setenv("APS5_GPU_ADDRESS_BINDINGS", "1", 1) == 0 && unsetenv("APS5_GPU_CHECKPOINTS") == 0, "cannot set binding-only diagnostic environment");
#endif
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::shared_ptr<GpuAddressBindings> history;
        std::uint64_t beforeDestroy = 0;
        {
            auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
            history = context.gpuCheckpoints ? context.gpuCheckpoints->AddressBindings() : nullptr;
            if (!history || !context.bufferDeviceAddress) {
                std::puts("skipped: native binding report/debug-utils feature or buffer device address unavailable");
                return VulkanTestSkipped;
            }
            VkFormatProperties properties{};
            context.formatProperties(context.physical, VK_FORMAT_D32_SFLOAT, &properties);
            if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
                std::puts("skipped: D32 depth attachment unavailable");
                return VulkanTestSkipped;
            }
            std::uint64_t bufferHandle = 0, imageHandle = 0;
            std::uint64_t beforeRelease = 0;
            VkDeviceAddress address = 0;
            {
                const auto beforeBuffer = history->Find(0, UINT64_MAX).newest;
                Allocation buffer{context};
                VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                create.size = 4096;
                create.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &create, nullptr, &buffer.buffer), "binding smoke buffer");
                VkMemoryRequirements requirements{};
                context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer.buffer, &requirements);
                buffer.Allocate(requirements, true);
                Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer.buffer, buffer.memory, 0), "binding smoke buffer bind");
                VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
                info.buffer = buffer.buffer;
                address = context.Function<PFN_vkGetBufferDeviceAddressKHR>("vkGetBufferDeviceAddressKHR")(context.device, &info);
                bufferHandle = (std::uint64_t)buffer.buffer;
                Require(address != 0 && HasObject(history->Find(address, address, VK_OBJECT_TYPE_BUFFER, bufferHandle), VK_OBJECT_TYPE_BUFFER, bufferHandle, VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT, beforeBuffer), "supported binding callback did not cover the known buffer handle and device address");
                const auto beforeImage = history->Find(0, UINT64_MAX).newest;
                Allocation image{context};
                VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                imageInfo.imageType = VK_IMAGE_TYPE_2D;
                imageInfo.format = VK_FORMAT_D32_SFLOAT;
                imageInfo.extent = {16, 16, 1};
                imageInfo.mipLevels = imageInfo.arrayLayers = 1;
                imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
                imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
                imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &imageInfo, nullptr, &image.image), "binding smoke depth image");
                context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image.image, &requirements);
                image.Allocate(requirements, false);
                Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image.image, image.memory, 0), "binding smoke depth image bind");
                imageHandle = (std::uint64_t)image.image;
                Require(HasObject(history->Find(0, UINT64_MAX, VK_OBJECT_TYPE_IMAGE, imageHandle), VK_OBJECT_TYPE_IMAGE, imageHandle, VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT, beforeImage), "supported binding callback did not report the known depth image handle");
                std::printf("binding smoke: buffer=0x%llx address=0x%llx depth-image=0x%llx matched real callbacks\n", static_cast<unsigned long long>(bufferHandle), static_cast<unsigned long long>(address), static_cast<unsigned long long>(imageHandle));
                beforeRelease = history->Find(0, UINT64_MAX).newest;
            }
            Require(HasObject(history->Find(address, address, VK_OBJECT_TYPE_BUFFER, bufferHandle), VK_OBJECT_TYPE_BUFFER, bufferHandle, VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT, beforeRelease), "buffer unbind callback missing after destruction");
            Require(HasObject(history->Find(0, UINT64_MAX, VK_OBJECT_TYPE_IMAGE, imageHandle), VK_OBJECT_TYPE_IMAGE, imageHandle, VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT, beforeRelease), "depth image unbind callback missing after destruction");
            beforeDestroy = history->Find(0, UINT64_MAX).newest;
        }
        std::unique_ptr<AgcDriver::VulkanDevice> compileOnly;
        if (overlap) {
            compileOnly = std::make_unique<AgcDriver::VulkanDevice>(AgcDriver::VulkanDevice::CompileOnly{});
            std::fprintf(stderr, "[binding-overlap] both devices alive\n");
            std::fprintf(stderr, "[binding-overlap] ordinary destroy begin\n");
        }
        device.reset();
        if (overlap) std::fprintf(stderr, "[binding-overlap] ordinary destroyed; compile-only remains\n");
        const auto afterDestroy = history->Find(0, UINT64_MAX);
        Require(afterDestroy.newest > beforeDestroy && afterDestroy.droppedCallbacks == 0 && afterDestroy.omittedObjects == 0, "callback state must remain alive through device-owned resource destruction");
        if (overlap) {
            std::fprintf(stderr, "[binding-overlap] compile-only destroy begin\n");
            compileOnly.reset();
            std::fprintf(stderr, "[binding-overlap] compile-only destroyed\n");
        } else {
            AgcDriver::VulkanDevice sequentialCompileOnly(AgcDriver::VulkanDevice::CompileOnly{});
        }
        std::puts("address binding native buffer, depth image, unbind and device/compile-only teardown checks passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
