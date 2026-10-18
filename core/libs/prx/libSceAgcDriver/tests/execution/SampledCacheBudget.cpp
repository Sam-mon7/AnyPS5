#include "prx/libSceAgcDriver/Execution/include/BdaFeatures.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDuplicates.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PlaceholderImages.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramOptions.hpp"
#include "SampleLod_spv.h"
#include "SampleArray_spv.h"
#include "VulkanTestDevice.hpp"
#include <SDL_loadso.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

constexpr std::uint32_t Side = 256;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t SurfaceBytes = std::size_t{Side} * Side * 4u;
constexpr std::size_t Surfaces = 12;
constexpr std::size_t BlockBytes = SurfaceBytes * Surfaces;
constexpr std::size_t RegistrationGapBytes = 65536;
constexpr std::size_t AllocationBytes = BlockBytes + RegistrationGapBytes;
constexpr std::uint64_t Budget = 1ull << 20u;
constexpr std::uint64_t Ceiling = Budget + Budget / 4u;

class Device {
public:
    explicit Device(bool resourceImages = false, bool mirrorTextures = false) {
#ifdef _WIN32
        library = SDL_LoadObject("vulkan-1.dll");
#else
        library = SDL_LoadObject("libvulkan.so.1");
#endif
        Require(library != nullptr, "cannot load Vulkan");
        try {
            instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(library, "vkGetInstanceProcAddr"));
            Require(instanceProc != nullptr, "missing Vulkan instance resolver");
            VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            application.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pApplicationInfo = &application;
            Check(function<PFN_vkCreateInstance>("vkCreateInstance")(&info, nullptr, &instance), "vkCreateInstance");
            std::uint32_t count = 0;
            const auto enumerate = function<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
            Check(enumerate(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
            Require(count != 0, "no Vulkan device");
            std::vector<VkPhysicalDevice> devices(count);
            Check(enumerate(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
            context.physical = devices.front();
            const auto extensions = function<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
            Check(extensions(context.physical, nullptr, &count, nullptr), "vkEnumerateDeviceExtensionProperties");
            std::vector<VkExtensionProperties> available(count);
            Check(extensions(context.physical, nullptr, &count, available.data()), "vkEnumerateDeviceExtensionProperties");
            const auto hasExtension = [&](const char* name) {
                for (const auto& extension : available) {
                    if (std::strcmp(extension.extensionName, name) == 0) return true;
                }
                return false;
            };
            auto bytes = AgcDriver::QueryBdaByteFeatures(context.physical, function<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2"), available);
            auto address = AgcDriver::QueryBdaFeatures(context.physical, function<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2"), available);
            const auto queues = function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
            queues(context.physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            queues(context.physical, &count, families.data());
            std::uint32_t family = 0;
            while (family < count && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) ++family;
            Require(family < count, "no Vulkan compute queue");
            const float priority = 1;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            VkPhysicalDeviceFeatures enabled{};
            enabled.shaderInt64 = VK_TRUE;
            if (resourceImages) {
                VkPhysicalDeviceFeatures supported{};
                function<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(context.physical, &supported);
                enabled.shaderStorageImageMultisample = supported.shaderStorageImageMultisample;
            }
            if (mirrorTextures) {
                VkPhysicalDeviceFeatures supported{};
                function<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(context.physical, &supported);
                Require(supported.textureCompressionBC, "mirror texture tests require BC compression");
                enabled.textureCompressionBC = VK_TRUE;
                context.textureCompressionBC = true;
            }
            address.pNext = &bytes;
            std::vector<const char*> extensionsEnabled{VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_KHR_8BIT_STORAGE_EXTENSION_NAME};
            if (hasExtension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
                extensionsEnabled.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
                VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
                VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hostProperties};
                function<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(context.physical, &properties);
                context.hostImportAlignment = hostProperties.minImportedHostPointerAlignment;
            }
            VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &address};
            device.queueCreateInfoCount = 1;
            device.pQueueCreateInfos = &queue;
            device.enabledExtensionCount = static_cast<std::uint32_t>(extensionsEnabled.size());
            device.ppEnabledExtensionNames = extensionsEnabled.data();
            device.pEnabledFeatures = &enabled;
            Check(function<PFN_vkCreateDevice>("vkCreateDevice")(context.physical, &device, nullptr, &context.device), "vkCreateDevice");
            context.deviceProc = function<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
            function<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(context.physical, &context.memory);
            VkPhysicalDeviceProperties properties{};
            function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(context.physical, &properties);
            context.limits = properties.limits;
            context.bufferDeviceAddress = true;
            context.formatProperties = function<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
            context.imageFormatProperties = function<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties");
            if (hasExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) context.memoryProperties2 = function<PFN_vkGetPhysicalDeviceMemoryProperties2>("vkGetPhysicalDeviceMemoryProperties2");
            context.Function<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(context.device, family, 0, &context.queue);
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = family;
            Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &pool, nullptr, &context.pool), "vkCreateCommandPool");
        } catch (...) {
            release();
            throw;
        }
    }

    ~Device() { release(); }
    const Context& GetContext() const { return context; }

private:
    template<typename TFunction>
    TFunction function(const char* name) const {
        const auto result = reinterpret_cast<TFunction>(instanceProc(instance, name));
        Require(result != nullptr, name);
        return result;
    }

    void release() noexcept {
        if (context.pool != VK_NULL_HANDLE) context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, context.pool, nullptr);
        context.bufferPool.reset();
        if (context.device != VK_NULL_HANDLE) function<PFN_vkDestroyDevice>("vkDestroyDevice")(context.device, nullptr);
        if (instance != VK_NULL_HANDLE) function<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        if (library != nullptr) SDL_UnloadObject(library);
    }

    void* library = nullptr;
    PFN_vkGetInstanceProcAddr instanceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    Context context{};
};

class Block {
public:
    explicit Block(const Context& context) : context(context) {
#ifdef _WIN32
        memory = VirtualAlloc(nullptr, AllocationBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        memory = std::aligned_alloc(65536, AllocationBytes);
#endif
        Require(memory != nullptr, "cannot allocate the texture block");
        std::memset(memory, 0x5a, BlockBytes);
        GuestAllocations::Mutation().Add(memory, BlockBytes, true, true, true);
    }

    ~Block() {
        ClearCachedTextures(context.device);
        GuestAllocations::Mutation().Remove(memory);
        HostImportFor(context, Surface(0), BlockBytes);
#ifdef _WIN32
        VirtualFree(memory, 0, MEM_RELEASE);
#else
        std::free(memory);
#endif
    }

    std::uint64_t Surface(std::size_t surface) const { return reinterpret_cast<std::uint64_t>(memory) + surface * SurfaceBytes; }

private:
    const Context& context;
    void* memory = nullptr;
};

std::array<std::uint32_t, 8> Descriptor(std::uint64_t address, std::uint32_t side = Side) {
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

std::shared_ptr<Texture> Sampled(const Context& context, std::uint64_t address, std::uint32_t side = Side) {
    const auto words = Descriptor(address, side);
    return CachedSampledTexture(context, words);
}

GuestTextureResource Resource(std::uint64_t address, std::uint32_t side = Side) {
    const auto words = Descriptor(address, side);
    return DecodeTextureResource(words);
}

std::shared_ptr<ShaderResources> ImageResources(const Context& context, const Block& block, std::size_t first, std::uint32_t count, bool sampled) {
    ShaderRecompiler::DescriptorBinding binding{};
    binding.kind = sampled ? ShaderRecompiler::DescriptorKind::SampledImage : ShaderRecompiler::DescriptorKind::StorageImage;
    binding.role = ShaderRecompiler::DescriptorRole::GuestImages;
    binding.descriptorSet = 0;
    binding.binding = (sampled ? ShaderRecompiler::RuntimeAbi::FirstImageBinding : ShaderRecompiler::RuntimeAbi::FirstStorageImageBinding) + PlaceholderDimensionsPerClass + 2u;
    binding.count = count;
    binding.imageShape = ShaderRecompiler::DescriptorImageShape::Image2D;
    binding.imageWritten.assign(count, false);
    binding.imageSamplers.assign(count, 0u);
    for (std::uint32_t element = 0; element < count; ++element) {
        const auto words = Descriptor(block.Surface(first + element));
        binding.guestDescriptor.insert(binding.guestDescriptor.end(), words.begin(), words.end());
    }
    ShaderRecompiler::RecompileResult program;
    program.bindings.push_back(std::move(binding));
    const CompiledShader shader{ShaderRecompiler::ShaderStage::Compute, &program, 0};
    return std::make_shared<ShaderResources>(context, shader);
}

void recorderEvictionTests(const Context& context, const Block& block) {
    auto& cache = SharedResourceCache();
    cache.Clear();
    ClearCachedTextures(context.device);
    auto sampled = ImageResources(context, block, 0, 1, true);
    const auto images = sampled->StorageImages();
    Require(images.size() == 1, "the eviction fixture has no sampled storage image");
    const std::weak_ptr<ShaderResources> resources = sampled;
    const std::weak_ptr<Texture> view = sampled->SampledTextures().front();
    const std::weak_ptr<StorageTexture> image = sampled->SampledTextures().front()->SharedStorageSource();
    cache.Insert({1}, sampled);
    Require(cache.Find({1}) == sampled, "the recorder fixture's descriptor set was not cached");
    sampled.reset();
    Buffer readback(context, sizeof(std::uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    {
        Recorder recorder(context);
        recorder.Activate();
        recorder.Commands();
        for (std::size_t surface = 1; surface < Surfaces; ++surface) CachedStorageSurface(context, Resource(block.Surface(surface)));
        Require(cache.Find({1}) == nullptr, "storage eviction left the recorded descriptor set cached");
        Require(!resources.expired() && !view.expired() && !image.expired(), "eviction destroyed the recorder's descriptor set, view or image");
        Require(!image.lock()->Cached(), "the recorder fixture did not evict the source image");
        const auto commands = recorder.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {1, 1, 1};
        context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, images.front().first, VK_IMAGE_LAYOUT_GENERAL, readback.Handle(), 1, &copy);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Sync();
        std::uint32_t pixel = 0;
        std::memcpy(&pixel, readback.Bytes().data(), sizeof(pixel));
        Require(pixel == 0x5a5a5a5a, "eviction changed the recorder's storage image pixel");
    }
    Require(resources.expired() && view.expired() && image.expired(), "an evicted image outlived its completed recorder");
    std::puts("storage eviction recorder lifetime and pixel readback passed");
}

bool resourceCacheTests(Context context, const std::string& mode) {
    PlaceholderImages placeholders(context);
    context.placeholderImages = &placeholders;
    TextureCache textures(context);
    context.textureCache = &textures;
    Block block(context);
    if (context.hostImportAlignment == 0 || HostImportFor(context, block.Surface(0), BlockBytes) == nullptr) {
        Require(std::getenv("ANYPS5_REQUIRE_VULKAN") == nullptr, "resource cache tests require host imports");
        std::puts("skipped, resource cache tests require host imports");
        return false;
    }
    auto& cache = SharedResourceCache();
    cache.Clear();
    try {
        if (mode == "resource-clear") {
            auto storage = ImageResources(context, block, 0, 1, false);
            auto sampled = ImageResources(context, block, 0, 1, true);
            Require(sampled->SampledTextures().front()->ViewsStorageImage(), "the sampled resource is not a storage-image view");
            const std::weak_ptr<StorageTexture> image = StorageTexture::FindLive(block.Surface(0), SurfaceBytes);
            const std::weak_ptr<ShaderResources> storageSet = storage;
            const std::weak_ptr<ShaderResources> sampledSet = sampled;
            cache.Insert({1}, storage);
            cache.Insert({2}, sampled);
            Require(cache.Size() == 2, "the resource cache did not accept the live descriptor sets");
            sampled.reset();
            ClearCachedTextures(context.device);
            Require(cache.Find({1}) == nullptr && cache.Find({2}) == nullptr && cache.Size() == 0, "clearing storage images left their resources cached");
            Require(sampledSet.expired(), "cache clear kept the sampled descriptor set alive");
            Require(!image.expired() && !storageSet.expired(), "cache clear destroyed an active caller's resources");
            storage.reset();
            Require(image.expired() && storageSet.expired(), "cleared storage image outlived its last active resource");
        } else if (mode == "resource-oversized") {
            for (const bool sampledImages : {false, true}) {
                const std::weak_ptr<StorageTexture> early = CachedStorageSurface(context, Resource(block.Surface(0)));
                auto oversized = ImageResources(context, block, 0, static_cast<std::uint32_t>(Surfaces), sampledImages);
                const std::weak_ptr<ShaderResources> rejected = oversized;
                auto image = early.lock();
                Require(oversized->Reusable(), "the oversized descriptor set is not eligible for resource caching");
                Require(image != nullptr && !image->Cached(), "the oversized descriptor set did not evict an early image while building");
                if (sampledImages) Require(oversized->SampledTextures().size() == Surfaces && oversized->SampledTextures().front()->StorageSource() == image.get(), "the oversized sampled descriptor set does not retain the early storage image");
                std::vector<std::shared_ptr<ShaderResources>> displaced;
                cache.Insert({1}, oversized, &displaced);
                Require(cache.Size() == 0 && displaced.empty(), sampledImages ? "an oversized sampled descriptor set entered the resource cache" : "an oversized storage descriptor set entered the resource cache");
                image.reset();
                oversized.reset();
                Require(rejected.expired() && early.expired(), "the rejected oversized descriptor set retained an evicted storage image");
                std::puts(sampledImages ? "resource cache oversized sampled lifetime passed" : "resource cache oversized storage lifetime passed");
                ClearCachedTextures(context.device);
            }
        } else {
            auto storage = ImageResources(context, block, 0, 1, false);
            auto sampled = ImageResources(context, block, 0, 1, true);
            Require(storage->Reusable() && sampled->Reusable(), "the stale descriptor sets are not eligible for resource caching");
            Require(sampled->SampledTextures().front()->ViewsStorageImage(), "the sampled resource is not a storage-image view");
            auto image = StorageTexture::FindLive(block.Surface(0), SurfaceBytes);
            const std::weak_ptr<StorageTexture> staleImage = image;
            const std::weak_ptr<ShaderResources> storageSet = storage;
            const std::weak_ptr<ShaderResources> sampledSet = sampled;
            Require(image != nullptr && image->Cached(), "the resource has no cached storage image");
            for (std::size_t surface = 1; surface < Surfaces && image->Cached(); ++surface) CachedStorageSurface(context, Resource(block.Surface(surface)));
            Require(!image->Cached(), "the test never evicted the oldest storage image");
            auto live = ImageResources(context, block, Surfaces - 1, 1, false);
            cache.Insert({1}, live);
            Require(cache.Find({1}) == live, "the resource cache rejected a live descriptor set");
            std::vector<std::shared_ptr<ShaderResources>> displaced;
            for (const auto& stale : {storage, sampled}) {
                cache.Insert({2}, stale, &displaced);
                Require(cache.Find({2}) == nullptr && cache.Size() == 1 && displaced.empty(), "an uncached image was admitted to the resource cache");
                cache.Insert({1}, stale, &displaced);
                Require(cache.Find({1}) == live && cache.Size() == 1 && displaced.empty(), "an uncached image replaced a live resource-cache entry");
            }
            image.reset();
            storage.reset();
            Require(storageSet.expired() && !sampledSet.expired() && !staleImage.expired(), "stale storage release did not preserve the sampled caller's image");
            std::puts("resource cache stale storage caller release passed");
            sampled.reset();
            Require(sampledSet.expired() && staleImage.expired(), "the rejected descriptor sets retained an evicted storage image");
            std::puts("resource cache stale sampled caller release passed");
        }
        if (mode == "resource-admission") recorderEvictionTests(context, block);
    } catch (...) {
        cache.Clear();
        throw;
    }
    cache.Clear();
    return true;
}

void viewTests(const Context& context) {
    if (context.hostImportAlignment == 0) {
        std::puts("host imports unavailable: views of storage images not tested");
        return;
    }
    Block block(context);
    if (HostImportFor(context, block.Surface(0), BlockBytes) == nullptr) {
        std::puts("host import of the texture block refused: views of storage images not tested");
        return;
    }
    const auto before = TextureCacheUsage();
    auto view = Sampled(context, block.Surface(0));
    Require(view->ViewsStorageImage(), "a sampled texture in host-imported memory is not a view of its storage image");
    const std::weak_ptr<Texture> viewed = view;
    const std::weak_ptr<StorageTexture> storage = view->SharedStorageSource();
    const auto after = TextureCacheUsage();
    Require(after.sampledEntries == before.sampledEntries + 1u, "the view did not enter the sampled texture cache");
    Require(after.sampledBytes == before.sampledBytes, "a view of a cached storage image counted " + std::to_string(after.sampledBytes - before.sampledBytes) + " bytes against the sampled texture cache");
    Require(after.storageBytes > before.storageBytes, "the storage image of the view is not counted by the storage image cache");
    Require(Sampled(context, block.Surface(0)) == view, "the second lookup of the surface did not hit its view");
    view.reset();
    for (std::size_t surface = 1; surface < Surfaces; ++surface) CachedStorageSurface(context, Resource(block.Surface(surface)));
    Require(viewed.expired() && storage.expired(), "the sampled texture cache kept the view of an evicted storage image");
    const auto evicted = TextureCacheUsage();
    Require(evicted.sampledEntries == after.sampledEntries - 1u && evicted.sampledBytes == before.sampledBytes, "dropping the view of the evicted storage image changed other sampled entries");
    auto again = Sampled(context, block.Surface(0));
    Require(again->ViewsStorageImage() && again->StorageSource() != nullptr && StorageImageCached(context, again->StorageSource()), "the surface's next lookup does not view its new cached storage image");
    const std::weak_ptr<Texture> heldView = again;
    const std::weak_ptr<StorageTexture> heldStorage = again->SharedStorageSource();
    for (std::size_t surface = 1; surface < Surfaces; ++surface) CachedStorageSurface(context, Resource(block.Surface(surface)));
    Require(!StorageImageCached(context, again->StorageSource()), "the held view's source was not evicted");
    Require(!heldView.expired() && !heldStorage.expired(), "eviction destroyed an active caller's view or image");
    again.reset();
    Require(heldView.expired() && heldStorage.expired(), "the evicted image outlived its last caller without a sampled lookup");
}

void reportedBudgetTests(const Context& context) {
    if (context.memoryProperties2 == nullptr) {
        std::puts("VK_EXT_memory_budget unavailable: the sampled texture cache keeps a quarter of the device-local heap");
        return;
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT reported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    properties.pNext = &reported;
    context.memoryProperties2(context.physical, &properties);
    const auto budget = SampledTextureBudget(context.memory, &reported, 0, 0);
    Require(budget >= (2048ull << 20u), "the sampled texture cache budget is below the 2 GiB floor");
    for (std::uint32_t heap = 0; heap < context.memory.memoryHeapCount; ++heap) {
        if ((context.memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
        std::printf("device-local heap %u: %llu MiB, VK_EXT_memory_budget budget %llu MiB, usage %llu MiB\n", heap, static_cast<unsigned long long>(context.memory.memoryHeaps[heap].size >> 20u), static_cast<unsigned long long>(reported.heapBudget[heap] >> 20u), static_cast<unsigned long long>(reported.heapUsage[heap] >> 20u));
    }
    std::printf("sampled texture cache budget %llu MiB, storage image cache budget %llu MiB\n", static_cast<unsigned long long>(budget >> 20u), static_cast<unsigned long long>(TextureCacheBudget(context.memory) >> 20u));
}

void snapshotTests(const Context& context) {
    Block block(context);
    auto first = Sampled(context, block.Surface(0));
    Require(!first->ViewsStorageImage(), "a sampled texture outside host-imported memory is not a snapshot");
    const auto held = TextureCacheUsage().sampledBytes;
    Require(held == first->AllocationBytes(), "a snapshot counted " + std::to_string(held) + " bytes, not its image allocation of " + std::to_string(first->AllocationBytes()));
    const std::weak_ptr<Texture> oldest = first;
    first.reset();
    const auto capacity = static_cast<std::size_t>(Budget / held);
    Require(capacity >= 2u && capacity + 1u < Surfaces, "a snapshot of the 256 KiB surface counts " + std::to_string(held) + " bytes, which the test's 1 MiB budget does not fit as planned");
    for (std::size_t surface = 1; surface < capacity; ++surface) Sampled(context, block.Surface(surface));
    Require(!oldest.expired() && TextureCacheUsage().sampledEntries == capacity, "the sampled texture cache did not keep " + std::to_string(capacity) + " snapshots within its budget");
    Sampled(context, block.Surface(capacity));
    Require(oldest.expired(), "the least recently used snapshot was not evicted past the budget");
    Require(TextureCacheUsage().sampledBytes <= Budget, "the sampled texture cache holds more than its budget");
    ClearCachedTextures(context.device);
    constexpr std::uint32_t TinySide = 16;
    const auto tiny = Sampled(context, block.Surface(0), TinySide);
    const auto guestBytes = DescribeSurface(Resource(block.Surface(0), TinySide)).guestBytes;
    if (tiny->AllocationBytes() >= guestBytes) {
        std::printf("the 16x16 snapshot's image takes %llu bytes, not less than its %llu guest bytes: counting the allocation alone not tested\n", static_cast<unsigned long long>(tiny->AllocationBytes()), static_cast<unsigned long long>(guestBytes));
        return;
    }
    Require(TextureCacheUsage().sampledBytes == tiny->AllocationBytes(), "a snapshot whose " + std::to_string(tiny->AllocationBytes()) + "-byte image is smaller than its " + std::to_string(guestBytes) + " guest bytes counted " + std::to_string(TextureCacheUsage().sampledBytes) + " bytes, not its allocation");
}

void heldTests(const Context& context) {
    Block block(context);
    auto held = Sampled(context, block.Surface(0));
    const auto bytes = TextureCacheUsage().sampledBytes;
    const auto capacity = static_cast<std::size_t>(Budget / bytes);
    Require((capacity + 1u) * bytes <= Ceiling && capacity + 2u < Surfaces, "a snapshot of the 256 KiB surface counts " + std::to_string(bytes) + " bytes, which the test's ceiling does not fit as planned");
    std::vector<std::weak_ptr<Texture>> released;
    for (std::size_t surface = 1; surface < capacity; ++surface) released.push_back(Sampled(context, block.Surface(surface)));
    Sampled(context, block.Surface(capacity));
    Require(Sampled(context, block.Surface(0)) == held, "the sampled texture cache evicted a texture its caller still holds");
    Require(released.front().expired(), "the least recently used texture nobody holds was not evicted in place of the held one");
    held.reset();
    ClearCachedTextures(context.device);
    std::vector<std::shared_ptr<Texture>> all;
    for (std::size_t surface = 0; surface <= capacity; ++surface) all.push_back(Sampled(context, block.Surface(surface)));
    Require(TextureCacheUsage().sampledEntries == capacity + 1u && TextureCacheUsage().sampledBytes > Budget, "textures their callers hold left a cache over its budget");
    for (std::size_t surface = 0; surface < all.size(); ++surface) Require(Sampled(context, block.Surface(surface)) == all[surface], "surface " + std::to_string(surface) + " was made again while its texture was held");
    all.push_back(Sampled(context, block.Surface(capacity + 1u)));
    Require(TextureCacheUsage().sampledEntries == capacity + 1u && TextureCacheUsage().sampledBytes <= Ceiling, "held textures took the sampled texture cache past a quarter over its budget");
    for (std::size_t surface = 1; surface < all.size(); ++surface) Require(Sampled(context, block.Surface(surface)) == all[surface], "past the ceiling, surface " + std::to_string(surface) + " left in place of the least recently used held texture");
    all.clear();
    Sampled(context, block.Surface(capacity + 2u));
    Require(TextureCacheUsage().sampledEntries == capacity && TextureCacheUsage().sampledBytes <= Budget, "released textures were not evicted down to the budget");
}

void releasedTests(Context context) {
    PlaceholderImages placeholders(context);
    context.placeholderImages = &placeholders;
    TextureCache textures(context);
    context.textureCache = &textures;
    ClearCachedTextures(context.device);
    auto& cache = SharedResourceCache();
    cache.Clear();
    try {
        Block block(context);
        auto pinned = ImageResources(context, block, 0, 1, true);
        Require(pinned->SampledTextures().size() == 1 && !pinned->SampledTextures().front()->ViewsStorageImage(), "the pinned descriptor set does not hold a sampled snapshot");
        const std::weak_ptr<Texture> texture = pinned->SampledTextures().front();
        const std::weak_ptr<ShaderResources> pinnedSet = pinned;
        cache.Insert({1}, pinned);
        Require(cache.Find({1}) == pinned, "the resource cache rejected a descriptor set whose texture is cached");
        pinned.reset();
        ClearCachedTextures(context.device);
        Require(!texture.expired() && texture.lock()->Released(), "the sampled texture cache dropped a texture the resource cache holds without marking it released");
        auto live = ImageResources(context, block, 1, 1, true);
        std::vector<std::shared_ptr<ShaderResources>> displaced;
        cache.Insert({2}, live, &displaced);
        Require(cache.Find({1}) == nullptr && cache.Find({2}) == live && cache.Size() == 1, "the resource cache kept an entry holding a released texture");
        Require(displaced.size() == 1 && displaced.front() == pinnedSet.lock(), "the entry holding a released texture was not handed back to the caller");
        displaced.clear();
        Require(pinnedSet.expired() && texture.expired(), "the resource cache kept a released texture alive");
        auto stale = ImageResources(context, block, 2, 1, true);
        ClearCachedTextures(context.device);
        Require(stale->SampledTextures().front()->Released(), "a held texture the sampled cache dropped is not marked released");
        cache.Insert({3}, stale, &displaced);
        Require(cache.Find({3}) == nullptr, "a descriptor set holding a released texture entered the resource cache");
        Require(cache.Size() == 0 && displaced.size() == 1 && displaced.front() == live, "the live entry whose texture was released stayed in the resource cache");
        displaced.clear();
    } catch (...) {
        cache.Clear();
        throw;
    }
    cache.Clear();
    ClearCachedTextures(context.device);
    std::puts("resource cache released texture tests passed");
}

void rescanTests(const Context& context) {
    constexpr std::uint32_t SmallSide = 128;
    constexpr std::size_t SmallBytes = std::size_t{SmallSide} * SmallSide * 4u;
    Block block(context);
    const auto small = [&](std::size_t surface) { return Sampled(context, block.Surface(0) + surface * SmallBytes, SmallSide); };
    std::vector<std::shared_ptr<Texture>> all{small(0)};
    const auto bytes = TextureCacheUsage().sampledBytes;
    const auto capacity = static_cast<std::size_t>(Budget / bytes);
    Require((capacity + 3u) * bytes <= Ceiling && (capacity + 3u) * SmallBytes <= BlockBytes, "a snapshot of the 64 KiB surface counts " + std::to_string(bytes) + " bytes, which the test's ceiling does not fit as planned");
    for (std::size_t surface = 1; surface <= capacity; ++surface) all.push_back(small(surface));
    const std::weak_ptr<Texture> newest = all.back();
    all.pop_back();
    all.push_back(small(capacity + 1u));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    all.push_back(small(capacity + 2u));
    Require(newest.expired(), "a released texture behind held ones was still cached a second later");
    Require(TextureCacheUsage().sampledEntries == capacity + 2u, "the rescan left " + std::to_string(TextureCacheUsage().sampledEntries) + " entries, not the " + std::to_string(capacity + 2u) + " textures still held");
}

void viewEvictionTests(const Context& context) {
    if (context.hostImportAlignment == 0) return;
    Block snapshots(context);
    Block imported(context);
    if (HostImportFor(context, imported.Surface(0), BlockBytes) == nullptr) return;
    const std::weak_ptr<Texture> view = Sampled(context, imported.Surface(0));
    Require(!view.expired() && TextureCacheUsage().sampledBytes == 0, "the view of a cached storage image did not enter the sampled texture cache at 0 bytes");
    Sampled(context, snapshots.Surface(0));
    const auto snapshotBytes = TextureCacheUsage().sampledBytes;
    Require(snapshotBytes != 0, "the snapshot fixture unexpectedly became a view of a cached storage image");
    const auto capacity = static_cast<std::size_t>(Budget / snapshotBytes);
    for (std::size_t surface = 1; surface <= capacity; ++surface) Sampled(context, snapshots.Surface(surface));
    Require(!view.expired(), "the sampled texture cache evicted a view of a cached storage image, which frees no memory");
}

void frameTests(const Context& context) {
    Block block(context);
    ClearCachedTextures(context.device);
    Recorder::CountPresent();
    const auto bytes = Sampled(context, block.Surface(0))->AllocationBytes();
    const auto capacity = static_cast<std::size_t>(Budget / bytes);
    const auto frame = capacity + 2u;
    Require(capacity >= 2u && 2u * frame <= Surfaces && TextureCacheUsage().sampledEntries == 1u, "a snapshot of the 256 KiB surface holds " + std::to_string(bytes) + " bytes, which the test's 1 MiB budget does not fit as planned");
    std::vector<std::weak_ptr<Texture>> made(frame);
    for (std::size_t surface = 0; surface < frame; ++surface) made[surface] = Sampled(context, block.Surface(surface));
    Require(TextureCacheUsage().sampledEntries == capacity, "a frame of " + std::to_string(frame) + " textures left " + std::to_string(TextureCacheUsage().sampledEntries) + " cached, not the " + std::to_string(capacity) + " the budget holds");
    for (std::size_t surface = capacity; surface < frame; ++surface) Require(made[surface].expired(), "surface " + std::to_string(surface) + ", used once in a frame whose textures fill the cache, was cached in place of an earlier one");
    Recorder::CountPresent();
    std::size_t hits = 0;
    for (std::size_t surface = 0; surface < frame; ++surface) {
        const auto again = Sampled(context, block.Surface(surface));
        hits += again == made[surface].lock();
    }
    Require(hits == capacity, "the next frame over the same " + std::to_string(frame) + " textures found " + std::to_string(hits) + " of them cached, not the " + std::to_string(capacity) + " the cache kept");
    auto admitted = Sampled(context, block.Surface(frame - 1u));
    Require(Sampled(context, block.Surface(frame - 1u)) == admitted, "a texture used a second time in the frame was not cached");
    Require(TextureCacheUsage().sampledEntries == capacity && TextureCacheUsage().sampledBytes <= Budget, "the texture admitted on its second use left the cache over its budget");
    admitted.reset();
    Recorder::CountPresent();
    std::vector<std::weak_ptr<Texture>> moved(frame);
    for (std::size_t surface = 0; surface < frame; ++surface) moved[surface] = Sampled(context, block.Surface(frame + surface));
    for (std::size_t surface = 0; surface < frame; ++surface) Require(moved[surface].expired(), "surface " + std::to_string(frame + surface) + ", first used in the frame after the cached textures were, was cached in place of one of them");
    Require(TextureCacheUsage().sampledEntries == capacity, "a frame over other textures left " + std::to_string(TextureCacheUsage().sampledEntries) + " of the previous frame's textures cached");
    Recorder::CountPresent();
    for (std::size_t surface = 0; surface < frame; ++surface) moved[surface] = Sampled(context, block.Surface(frame + surface));
    for (std::size_t surface = 0; surface < capacity; ++surface) Require(!moved[surface].expired(), "surface " + std::to_string(frame + surface) + " of a second frame over other textures did not replace the textures last used two frames before");
    Require(TextureCacheUsage().sampledEntries == capacity, "a second frame over other textures left " + std::to_string(TextureCacheUsage().sampledEntries) + " entries cached");
}

void earlyMissTests(const Context& context) {
    Block block(context);
    ClearCachedTextures(context.device);
    Recorder::CountPresent();
    const auto bytes = Sampled(context, block.Surface(0))->AllocationBytes();
    const auto capacity = static_cast<std::size_t>(Budget / bytes);
    Require(capacity >= 2u && capacity < Surfaces && TextureCacheUsage().sampledEntries == 1u, "a snapshot of the 256 KiB surface holds " + std::to_string(bytes) + " bytes, which the test's 1 MiB budget does not fit as planned");
    std::vector<std::weak_ptr<Texture>> cached(capacity);
    for (std::size_t surface = 0; surface < capacity; ++surface) cached[surface] = Sampled(context, block.Surface(surface));
    Recorder::CountPresent();
    for (std::size_t surface = 0; surface < capacity; ++surface) {
        const auto again = Sampled(context, block.Surface(surface));
        Require(again == cached[surface].lock(), "surface " + std::to_string(surface) + " missed in a frame over the textures the cache holds");
    }
    Recorder::CountPresent();
    Sampled(context, block.Surface(capacity));
    std::size_t hits = 0;
    for (std::size_t surface = 0; surface < capacity; ++surface) {
        const auto again = Sampled(context, block.Surface(surface));
        hits += again == cached[surface].lock();
    }
    Require(hits == capacity, "a frame that first uses a new texture before its " + std::to_string(capacity) + " cached ones found " + std::to_string(hits) + " of them cached");
}

constexpr std::uint64_t KeyHashBasis = 14695981039346656037ull;
constexpr std::uint64_t KeyHashPrime = 1099511628211ull;

std::array<std::array<std::uint32_t, 8>, 2> CollidingDescriptors(VkDevice device, std::uint64_t address) {
    const auto words = Descriptor(address);
    auto prefix = KeyHashBasis ^ reinterpret_cast<std::uintptr_t>(device);
    for (std::size_t word = 0; word < 6; ++word) prefix = (prefix ^ words[word]) * KeyHashPrime;
    std::unordered_map<std::uint32_t, std::uint32_t> word6ByHighHalf;
    for (std::uint32_t candidate = 0; candidate < 0x200u; ++candidate) {
        const auto word6 = ((candidate >> 1u) << 24u) | (candidate & 1u);
        const auto hash = (prefix ^ word6) * KeyHashPrime;
        const auto [match, added] = word6ByHighHalf.emplace(static_cast<std::uint32_t>(hash >> 32u), word6);
        if (added) continue;
        auto first = words;
        auto second = words;
        first[6] = match->second;
        second[6] = word6;
        second[7] = static_cast<std::uint32_t>(hash ^ ((prefix ^ match->second) * KeyHashPrime));
        return {first, second};
    }
    throw std::runtime_error("no two descriptors of the surface that differ only in word 6's statistics counter and the unused meta address collide in TextureKeyHash");
}

void collidingKeyTests(const Context& context) {
    Block block(context);
    ClearCachedTextures(context.device);
    Recorder::CountPresent();
    const auto bytes = Sampled(context, block.Surface(0))->AllocationBytes();
    const auto capacity = static_cast<std::size_t>(Budget / bytes);
    Require(capacity >= 2u && capacity < Surfaces && TextureCacheUsage().sampledEntries == 1u, "a snapshot of the 256 KiB surface holds " + std::to_string(bytes) + " bytes, which the test's 1 MiB budget does not fit as planned");
    std::vector<std::weak_ptr<Texture>> cached(capacity);
    for (std::size_t surface = 0; surface < capacity; ++surface) cached[surface] = Sampled(context, block.Surface(surface));
    Recorder::CountPresent();
    const auto [first, second] = CollidingDescriptors(context.device, block.Surface(capacity));
    CachedSampledTexture(context, first);
    CachedSampledTexture(context, second);
    std::size_t kept = 0;
    for (const auto& texture : cached) kept += !texture.expired();
    Require(kept == capacity && TextureCacheUsage().sampledEntries == capacity, "the first use of a descriptor whose cache key hash equals that of another descriptor first used in the frame left " + std::to_string(kept) + " of the " + std::to_string(capacity) + " textures the last frame used cached");
}

constexpr std::uint64_t ReportedBudget = 64ull << 30u;
constexpr std::uint64_t ReportedUsage = 12ull << 30u;

void VKAPI_PTR ReportBudget(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2* properties) {
    auto* reported = static_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(properties->pNext);
    for (std::uint32_t heap = 0; heap < VK_MAX_MEMORY_HEAPS; ++heap) {
        reported->heapBudget[heap] = ReportedBudget;
        reported->heapUsage[heap] = ReportedUsage;
    }
}

std::uint64_t ExpectedBudget(const Context& context) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT reported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &reported};
    ReportBudget(context.physical, &properties);
    const auto sampled = SampledTextureMemory();
    const auto cached = TextureCacheUsage().sampledBytes;
    return SampledTextureBudget(context.memory, &reported, sampled, TextureCacheUsage().storageBytes, sampled > cached ? sampled - cached : 0u);
}

std::uint64_t ReadBudget(const Context& context) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    return SampledTextureCacheBudget(context);
}

class MirrorSampleProgram {
public:
    MirrorSampleProgram(const Context& context, Recorder& recorder, std::span<const std::uint32_t> code = SAMPLE_LOD_SPV) : context(context), recorder(recorder), result(context, sizeof(float) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;
        Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &layoutInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 2};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &push;
        Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout");
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = code.size_bytes();
        moduleInfo.pCode = code.data();
        Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
        pipelineInfo.layout = pipelineLayout;
        Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines");
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        Check(context.Function<PFN_vkCreateSampler>("vkCreateSampler")(context.device, &samplerInfo, nullptr, &sampler), "vkCreateSampler");
        const VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    }
    ~MirrorSampleProgram() {
        context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
        context.Function<PFN_vkDestroySampler>("vkDestroySampler")(context.device, sampler, nullptr);
        context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
        context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, pipelineLayout, nullptr);
        context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, setLayout, nullptr);
    }
    MirrorSampleProgram(const MirrorSampleProgram&) = delete;
    MirrorSampleProgram& operator=(const MirrorSampleProgram&) = delete;

    float Red(VkImageView view, VkImageLayout layout, float lod, float layer = 0) {
        VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateInfo.descriptorPool = pool;
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts = &setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocateInfo, &set), "vkAllocateDescriptorSets");
        const VkDescriptorImageInfo image{sampler, view, layout};
        const VkDescriptorBufferInfo buffer{result.Handle(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &image;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &buffer;
        context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, 2, writes, 0, nullptr);
        const auto commands = recorder.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        const float parameters[]{lod, layer};
        context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), parameters);
        context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, 1, 1, 1);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Submit();
        recorder.Sync();
        Check(context.Function<PFN_vkFreeDescriptorSets>("vkFreeDescriptorSets")(context.device, pool, 1, &set), "vkFreeDescriptorSets");
        float texel[4];
        std::memcpy(texel, result.Bytes().data(), sizeof(texel));
        return texel[0];
    }

private:
    const Context& context;
    Recorder& recorder;
    Buffer result;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
};

void expectRed(float got, float want, const char* what) {
    if (std::abs(got - want) > 1.5f / 255.0f) throw std::runtime_error(std::string(what) + ": read " + std::to_string(got) + ", expected " + std::to_string(want));
}


#ifdef _WIN32
class MirrorBlock {
public:
    static constexpr std::size_t Bytes = 4u * 65536u;
    MirrorBlock(const Context& context, Recorder& recorder) : context(context), recorder(recorder) {
        memory = GuestArena::GuestArenaAllocate_nid_postfix(Bytes, 65536);
        Require(memory != nullptr, "cannot reserve mirrored texture memory");
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, Bytes, nullptr);
        Require(section != nullptr, "cannot create mirrored texture section");
        GuestArena::GuestArenaMap_nid_postfix(memory, Bytes, section, 0, PAGE_READWRITE);
        std::memset(memory, 0, Bytes);
        GuestAllocations::Mutation().Add(memory, Bytes, true, true, true);
    }
    ~MirrorBlock() {
        recorder.Sync();
        ClearCachedTextures(context.device);
        recorder.FlushDeferredReleases();
        ClearImageMirrors(context.device);
        ClearHostImports(context.device);
        GuestAllocations::Mutation().Remove(memory);
        GuestArena::GuestArenaReset_nid_postfix(memory, Bytes);
        GuestArena::GuestArenaRelease_nid_postfix(memory, Bytes);
        CloseHandle(section);
    }
    std::uint64_t Address(std::size_t offset = 0) const { return reinterpret_cast<std::uint64_t>(memory) + offset; }
    void MakeReadOnly() const {
        GuestAllocations::Mutation().Protect(memory, Bytes, true, false, true, [&] {
            GuestArena::GuestArenaCommit_nid_postfix(memory, Bytes, PAGE_READONLY, Bytes);
        });
        std::uint32_t protection = 0;
        Require(GuestArena::GuestArenaProtection_nid_postfix(Address(), &protection) && protection == PAGE_READONLY,
                "readonly texture fixture kept writable guest protection");
    }
    void Seed() const {
        auto seedContext = context;
        seedContext.hostImportAlignment = 0;
        seedContext.dmaBufImport = false;
        GuestBufferMemory seed(seedContext);
        seed.SetAddressWrites(false);
        seed.AcquireRegistered();
        seed.Upload(true);
        seed.WriteBack();
        recorder.Sync();
        Require(HostImportFor(context, Address(), Bytes) == nullptr, "mirrored texture range acquired a competing host import");
    }
private:
    const Context& context;
    Recorder& recorder;
    void* memory = nullptr;
    HANDLE section = nullptr;
};

std::array<std::uint32_t, 8> MirrorDescriptor(std::uint64_t address, std::uint32_t format) {
    constexpr std::uint32_t side = 64;
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((side - 1u) & 3u) << 30u),
        ((side - 1u) >> 2u) | ((side - 1u) << 14u),
        0xfacu | (Type2D << 28u), 0u, 0u, 0u, 0u,
    };
}

void FillMirrorTexture(const std::array<std::uint32_t, 8>& words, std::uint8_t red, bool compressed) {
    const auto resource = DecodeTextureResource(words);
    const auto bytes = static_cast<std::size_t>(DescribeSurface(resource).guestBytes);
    auto* memory = reinterpret_cast<std::uint8_t*>(resource.baseAddress);
    if (!compressed) {
        std::memset(memory, red, bytes);
    } else {
        const auto color = static_cast<std::uint16_t>((red == 0 ? 0u : 31u) << 11u);
        const std::array<std::uint32_t, 2> block{static_cast<std::uint32_t>(color), 0u};
        for (std::size_t offset = 0; offset < bytes; offset += sizeof(block)) std::memcpy(memory + offset, block.data(), sizeof(block));
    }
    AgcDriver::GuestMemory::BumpCollectEpoch();
}

void RecordMirrorFill(const Context& context, Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint32_t value) {
    auto memory = std::make_shared<GuestBufferMemory>(context);
    memory->AddWritable(address, bytes);
    memory->AddReadable(address, bytes);
    memory->Upload(false);
    std::uint32_t adjustment = 0;
    const auto view = memory->Descriptor(address, bytes, adjustment);
    Require(adjustment == 0, "mirrored texture writer unexpectedly needs an address adjustment");
    memory->BeginGpuWrites(recorder);
    const auto commands = recorder.Commands();
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, view.buffer, view.offset, bytes, value);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    memory->RecordCopyBacks(recorder);
    recorder.Keep(memory);
    recorder.NotePendingWrites(memory->Writes());
    recorder.OnComplete([memory] { memory->WriteBack(); });
    memory->ReadyForSubmit(recorder);
}

void RecordStorageClear(const Context& context, Recorder& recorder, const std::shared_ptr<StorageTexture>& texture, float red) {
    Require(StorageFormatForGuest(context, texture->Descriptor().format) == VK_FORMAT_R8G8B8A8_UNORM,
            "pending mirror texture fixture has an unexpected storage format");
    const auto commands = recorder.Commands();
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue color{{red, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, texture->Descriptor().mipCount, 0, texture->ImageLayers()};
    context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, texture->Image(), VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    recorder.Keep(texture);
    texture->MarkDirty();
}

void pendingMirrorStorageTests(const Context& context, Recorder& recorder, MirrorSampleProgram& sampler, const std::array<std::uint32_t, 8>& words) {
    ClearCachedTextures(context.device);
    const auto resource = DecodeTextureResource(words);
    const auto bytes = DescribeSurface(resource).guestBytes;
    auto image = CachedStorageSurface(context, resource);
    RecordStorageClear(context, recorder, image, 64.0f / 255.0f);
    Require(StorageTexture::FindPending(resource.baseAddress, bytes) == image, "storage image did not retain its pending GPU results");
    const auto submissions = recorder.Submissions();
    const auto uploads = StorageTexture::MirrorUploads();
    auto sampled = CachedSampledTexture(context, words);
    Require(StorageTexture::FindPending(resource.baseAddress, bytes) == image, "sampling flushed the image's own pending GPU results");
    Require(recorder.Submissions() == submissions, "sampling the image's own pending results submitted the open batch");
    Require(StorageTexture::MirrorUploads() == uploads, "sampling the image's own pending results re-uploaded the mirror");
    expectRed(sampler.Red(sampled->View(), sampled->Layout(), 0), 64.0f / 255.0f, "sampling did not preserve the image's pending GPU results");
    RecordStorageClear(context, recorder, image, 192.0f / 255.0f);
    auto replacement = StorageTexture::Create(context, *context.detiler, resource, 0);
    Require(replacement != image, "direct storage creation reused the pending image");
    Require(StorageTexture::FindPending(resource.baseAddress, bytes) == nullptr, "direct storage creation bypassed another image's pending results");
    Texture view(context, replacement, resource, {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY});
    expectRed(sampler.Red(view.View(), view.Layout(), 0), 192.0f / 255.0f, "direct storage creation read stale mirror bytes over a pending image");
    image.reset();
    sampled.reset();
    replacement.reset();
    recorder.Sync();
    ClearCachedTextures(context.device);
}

void readOnlyMirrorTextureTests(const Context& context) {
    Recorder recorder(context);
    recorder.Activate();
    MirrorBlock block(context, recorder);
    const auto words = MirrorDescriptor(block.Address(), 56u);
    const auto bytes = static_cast<std::size_t>(DescribeSurface(DecodeTextureResource(words)).guestBytes);
    FillMirrorTexture(words, 0x80, false);
    block.MakeReadOnly();
    block.Seed();
    MirrorSampleProgram sampler(context, recorder);
    const auto before = TextureCacheUsage().snapshotsMade;
    auto source = MirrorSourceFor(context, block.Address(), bytes);
    if (VramTextures()) Require(source.has_value(), "readonly mirrored surface has no GPU texture source");
    const auto identity = source ? source->proof.identity : 0;
    auto first = CachedSampledTexture(context, words);
    expectRed(sampler.Red(first->View(), first->Layout(), 0), 128.0f / 255.0f, "readonly mirrored texture sampled the wrong bytes");
    if (source) {
        const auto unchanged = MirrorSourceFor(context, block.Address(), bytes);
        Require(unchanged && unchanged->proof == source->proof, "unchanged readonly mirror lost its source proof");
    }
    first.reset();
    source.reset();
    recorder.FlushDeferredReleases();
    ClearImageMirrors(context.device);
    block.Seed();
    auto replaced = CachedSampledTexture(context, words);
    expectRed(sampler.Red(replaced->View(), replaced->Layout(), 0), 128.0f / 255.0f, "readonly mirror replacement sampled the wrong bytes");
    if (VramTextures()) {
        const auto replacement = MirrorSourceFor(context, block.Address(), bytes);
        Require(replacement && replacement->proof.identity != identity, "readonly mirror replacement reused its source identity");
        Require(TextureCacheUsage().snapshotsMade == before, "readonly mirror texture took a CPU snapshot");
    } else Require(TextureCacheUsage().snapshotsMade > before, "readonly switch-off texture skipped the CPU snapshot path");
    replaced.reset();
    recorder.Sync();
}

void mirrorTextureTests(const Context& context, bool compressed) {
    Recorder recorder(context);
    recorder.Activate();
    MirrorBlock block(context, recorder);
    const auto words = MirrorDescriptor(block.Address(), compressed ? 169u : 56u);
    const auto bytes = static_cast<std::size_t>(DescribeSurface(DecodeTextureResource(words)).guestBytes);
    FillMirrorTexture(words, compressed ? 0 : 0x20, compressed);
    block.Seed();
    MirrorSampleProgram sampler(context, recorder);
    const auto before = TextureCacheUsage();
    const auto beforeUploads = StorageTexture::MirrorUploads();
    auto first = CachedSampledTexture(context, words);
    Require(first->ViewsStorageImage() == (!compressed && VramTextures()), "mirrored texture selected the wrong storage/snapshot path");
    expectRed(sampler.Red(first->View(), first->Layout(), 0), compressed ? 0.0f : 32.0f / 255.0f, "initial mirrored texture");
    auto initial = VramTextures() ? MirrorSourceFor(context, block.Address(), bytes) : std::nullopt;
    if (VramTextures()) Require(initial.has_value(), "mirrored texture has no coherent buffer source");
    FillMirrorTexture(words, compressed ? 255 : 0x60, compressed);
    auto cpu = CachedSampledTexture(context, words);
    expectRed(sampler.Red(cpu->View(), cpu->Layout(), 0), compressed ? 1.0f : 96.0f / 255.0f, "CPU write did not refresh mirrored texture");
    if (VramTextures()) {
        const auto refreshed = MirrorSourceFor(context, block.Address(), bytes);
        Require(refreshed && refreshed->proof != initial->proof, "CPU write left the mirror texture source proof unchanged");
        initial = refreshed;
    }
    const auto submissions = recorder.Submissions();
    RecordMirrorFill(context, recorder, block.Address(), bytes, 0u);
    if (!VramTextures()) {
        recorder.Sync();
        FlushHeapMirrorWrites(block.Address(), bytes);
        AgcDriver::GuestMemory::BumpCollectEpoch();
    }
    auto gpu = CachedSampledTexture(context, words);
    if (VramTextures()) {
        Require(recorder.Submissions() == submissions, "writer-to-texture refresh submitted the open batch before GPU sampling");
        const auto written = MirrorSourceFor(context, block.Address(), bytes);
        Require(written && written->proof.revision != initial->proof.revision, "unsubmitted GPU writer did not invalidate the texture source revision");
    }
    expectRed(sampler.Red(gpu->View(), gpu->Layout(), 0), 0.0f, "GPU write did not refresh mirrored texture in command order");
    const auto after = TextureCacheUsage();
    if (VramTextures()) {
        Require(after.snapshotsMade == before.snapshotsMade, "mirrored texture made a transient CPU snapshot");
        Require(after.snapshotBytes == before.snapshotBytes, "mirrored texture retained CPU snapshot bytes");
        if (compressed) Require(after.mirrorSnapshotsMade >= before.mirrorSnapshotsMade + 3, "compressed mirror refreshes did not use GPU snapshot copies");
        else Require(StorageTexture::MirrorUploads() >= beforeUploads + 3, "storage texture refreshes did not upload from the mirror");
    } else {
        Require(after.snapshotsMade > before.snapshotsMade, "texture switch-off path did not retain CPU snapshot behavior");
    }
    initial.reset();
    first.reset();
    cpu.reset();
    gpu.reset();
    recorder.FlushDeferredReleases();
    auto oldSource = VramTextures() ? MirrorSourceFor(context, block.Address(), bytes) : std::nullopt;
    const auto oldIdentity = oldSource ? oldSource->proof.identity : 0;
    oldSource.reset();
    ClearImageMirrors(context.device);
    FillMirrorTexture(words, compressed ? 255 : 0xa0, compressed);
    block.Seed();
    auto replaced = CachedSampledTexture(context, words);
    expectRed(sampler.Red(replaced->View(), replaced->Layout(), 0), compressed ? 1.0f : 160.0f / 255.0f, "mirror replacement reused stale texture contents");
    if (VramTextures()) {
        const auto replacement = MirrorSourceFor(context, block.Address(), bytes);
        Require(replacement && replacement->proof.identity != oldIdentity, "mirror replacement reused its source identity");
    }
    replaced.reset();
    recorder.Sync();
    if (!compressed) {
        pendingMirrorStorageTests(context, recorder, sampler, words);
        ClearCachedTextures(context.device);
        auto clearWords = words;
        const auto metadata = block.Address(2u * 65536u);
        clearWords[6] = (1u << 21u) | (static_cast<std::uint32_t>((metadata >> 8u) & 0xffu) << 24u);
        clearWords[7] = static_cast<std::uint32_t>(metadata >> 16u);
        std::memset(reinterpret_cast<void*>(metadata), 0, 256);
        AgcDriver::GuestMemory::BumpCollectEpoch();
        Require(DecodeTextureResource(clearWords).dccAddress == metadata, "mirrored texture fallback has no DCC metadata");
        const auto beforeClear = TextureCacheUsage().snapshotsMade;
        auto cleared = CachedSampledTexture(context, clearWords);
        expectRed(sampler.Red(cleared->View(), cleared->Layout(), 0), 0.0f, "DCC clear sampled physical mirror bytes instead of the synthetic clear");
        Require(TextureCacheUsage().snapshotsMade > beforeClear, "synthetic DCC clear did not use the CPU fallback snapshot");
        cleared.reset();
    }
    recorder.Sync();
}

class WatchedBlock {
public:
    WatchedBlock(const Context& context, Recorder& recorder, std::size_t bytes) : context(context), recorder(recorder), bytes(bytes) {
        memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536));
        Require(memory != nullptr, "cannot reserve import detile texture memory");
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, static_cast<DWORD>(static_cast<std::uint64_t>(bytes) >> 32u), static_cast<DWORD>(bytes), nullptr);
        Require(section != nullptr, "cannot create the import detile guest section");
        GuestArena::GuestArenaMap_nid_postfix(memory, bytes, section, 0, PAGE_READWRITE);
        GuestAllocations::Mutation().Add(memory, bytes, true, true, true);
    }
    ~WatchedBlock() {
        recorder.Sync();
        ClearCachedTextures(context.device);
        recorder.FlushDeferredReleases();
        ClearHostImports(context.device);
        GuestAllocations::Mutation().Remove(memory);
        GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
        CloseHandle(section);
    }
    WatchedBlock(const WatchedBlock&) = delete;
    WatchedBlock& operator=(const WatchedBlock&) = delete;
    std::uint8_t* Data(std::size_t offset = 0) const { return memory + offset; }
    std::uint64_t Address(std::size_t offset = 0) const { return reinterpret_cast<std::uint64_t>(memory) + offset; }
    std::size_t Bytes() const { return bytes; }
    void Fill(std::size_t offset, std::size_t count, std::uint32_t seed) const {
        std::uint32_t state = seed;
        for (std::size_t index = 0; index < count; ++index) {
            state = state * 1664525u + 1013904223u;
            memory[offset + index] = static_cast<std::uint8_t>(state >> 24u);
        }
        AgcDriver::GuestMemory::BumpCollectEpoch();
    }

private:
    const Context& context;
    Recorder& recorder;
    std::size_t bytes;
    std::uint8_t* memory = nullptr;
    HANDLE section = nullptr;
};

std::array<std::uint32_t, 8> CompressedWords(std::uint64_t address, std::uint32_t side) {
    constexpr std::uint32_t lastLevel = 2;
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (169u << 20u) | (((side - 1u) & 3u) << 30u),
        ((side - 1u) >> 2u) | ((side - 1u) << 14u),
        0xfacu | (lastLevel << 16u) | (TileR64KBX << 20u) | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

bool importDetileTests(const Context& context) {
    constexpr std::uint32_t side = 512;
    constexpr std::uint32_t levels = 3;
    constexpr VkComponentMapping identity{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    const bool on = Texture::ImportDetileEnabled();
    const auto guestBytes = static_cast<std::size_t>(DescribeSurface(DecodeTextureResource(CompressedWords(65536, side))).guestBytes);
    const auto stride = (guestBytes + 65535) / 65536 * 65536;
    Recorder recorder(context);
    recorder.Activate();
    WatchedBlock block(context, recorder, 4 * stride);
    if (context.hostImportAlignment == 0 || HostImportFor(context, block.Address(), block.Bytes()) == nullptr) {
        std::puts("skipped, import detile tests require host imports");
        return false;
    }
    if (AgcDriver::GuestMemory::CollectWrites(block.Address(), block.Bytes()) == 0) {
        std::puts("skipped, import detile tests require write-watched guest memory");
        return false;
    }
    MirrorSampleProgram sampler(context, recorder);
    const auto samples = [&](const Texture& texture) {
        std::array<float, levels> result{};
        for (std::uint32_t level = 0; level < levels; ++level) result[level] = sampler.Red(texture.View(), texture.Layout(), static_cast<float>(level));
        return result;
    };
    for (std::size_t surface = 0; surface < 4; ++surface) block.Fill(surface * stride, guestBytes, 0x1000u + static_cast<std::uint32_t>(surface));
    ClearCachedTextures(context.device);

    for (std::size_t surface = 1; surface < 3; ++surface) {
        const auto words = CompressedWords(block.Address(surface * stride), side);
        const auto resource = DecodeTextureResource(words);
        Require(DescribeSurface(resource).guestBytes == guestBytes, "the import detile fixture changed size");
        std::vector<std::byte> snapshot(guestBytes);
        std::memcpy(snapshot.data(), block.Data(surface * stride), guestBytes);
        const Texture reference(context, *context.detiler, resource, identity, snapshot);
        const Texture direct(context, *context.detiler, resource, identity, std::span<const std::byte>{}, false, nullptr, true);
        Require(!reference.DetiledFromImport() && direct.DetiledFromImport(), "a texture over imported guest pages was not detiled from the import");
        const auto expected = samples(reference);
        Require(samples(direct) == expected, "the texture detiled from the import differs from the one detiled from a snapshot (surface " + std::to_string(surface) + ")");
        Require(std::any_of(expected.begin(), expected.end(), [](float value) { return value != 0.0f; }), "the import detile fixture samples black at every level");
    }

    const auto words = CompressedWords(block.Address(stride), side);
    const auto resource = DecodeTextureResource(words);
    std::vector<std::byte> snapshot(guestBytes);
    std::memcpy(snapshot.data(), block.Data(stride), guestBytes);
    const auto firstExpected = samples(Texture(context, *context.detiler, resource, identity, snapshot));
    const auto before = TextureCacheUsage().snapshotsMade;
    auto cached = CachedSampledTexture(context, words);
    Require(!cached->ViewsStorageImage(), "a compressed texture viewed a storage image");
    Require(cached->DetiledFromImport() == on, on ? "the lookup of a texture over imported guest pages kept the snapshot path" : "the lookup detiled from the import with the switch off");
    Require((TextureCacheUsage().snapshotsMade == before) == on, on ? "the lookup took a CPU snapshot although it detiles from the import" : "the lookup took no CPU snapshot with the switch off");
    Require(samples(*cached) == firstExpected, "the cached texture sampled other texels than a snapshot of the same guest pages");
    Require(CachedSampledTexture(context, words) == cached, "the second lookup of an unchanged texture did not hit the cache");

    block.Fill(stride, guestBytes, 0x2000u);
    std::memcpy(snapshot.data(), block.Data(stride), guestBytes);
    const auto secondExpected = samples(Texture(context, *context.detiler, resource, identity, snapshot));
    Require(secondExpected != firstExpected, "the import detile fixture did not change its texels");
    auto written = CachedSampledTexture(context, words);
    Require(written != cached, "a CPU write over the texture kept the cached texture");
    Require(samples(*written) == secondExpected, "the texture looked up after a CPU write sampled the old texels");

    RecordMirrorFill(context, recorder, resource.baseAddress, guestBytes, 0u);
    if (on) {
        const Texture filled(context, *context.detiler, resource, identity, std::span<const std::byte>{}, false, nullptr, true);
        Require(filled.DetiledFromImport(), "the texture made after a recorded GPU write was not detiled from the import");
        for (const auto value : samples(filled)) Require(value == 0.0f, "the texture detiled from the import ran before the recorded GPU write over its pages");
    }
    cached.reset();
    written.reset();
    recorder.Sync();
    return true;
}
#endif

void VKAPI_PTR ReportPressure(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2* properties) {
    auto* reported = static_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(properties->pNext);
    for (std::uint32_t heap = 0; heap < VK_MAX_MEMORY_HEAPS; ++heap) {
        reported->heapBudget[heap] = 4ull << 30u;
        reported->heapUsage[heap] = 4ull << 30u;
    }
}

void ageTextureTests(const Context& context) {
    if (!TextureAgeEviction()) {
        frameTests(context);
        earlyMissTests(context);
        return;
    }
    Block block(context);
    ClearCachedTextures(context.device);
    Recorder::CountPresent();
    std::vector<std::weak_ptr<Texture>> recent;
    for (std::size_t surface = 0; surface < 32; ++surface) recent.push_back(Sampled(context, block.Surface(0) + surface * 65536, 128));
    Require(TextureCacheUsage().sampledBytes > Budget, "age policy did not overcommit for the current present's working set");
    for (const auto& texture : recent) Require(!texture.expired(), "age policy evicted the current present's texture");
    for (std::uint32_t age = 1; age <= 3; ++age) {
        Recorder::CountPresent();
        Sampled(context, block.Surface(0), 16);
        for (const auto& texture : recent) Require(!texture.expired(), "age policy evicted a texture from the previous three presents");
    }
    Recorder::CountPresent();
    Sampled(context, block.Surface(0), 32);
    const auto victims = std::count_if(recent.begin(), recent.end(), [](const auto& texture) { return texture.expired(); });
    Require(victims != 0 && victims <= 16, "age policy did not evict a bounded group of old textures under pressure");
    const auto second = Sampled(context, block.Surface(1), 32);
    Require(std::count_if(recent.begin(), recent.end(), [](const auto& texture) { return texture.expired(); }) == victims,
            "age policy ran another eviction pass in the same present");
    Require(Sampled(context, block.Surface(1), 32) == second, "age policy failed to cache a current-present admission");
    ClearCachedTextures(context.device);
    Recorder::CountPresent();
    std::vector<std::weak_ptr<Texture>> idle;
    for (std::size_t surface = 0; surface < 32; ++surface) idle.push_back(Sampled(context, block.Surface(0) + surface * 65536, 128));
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    Recorder::CountPresent();
    Sampled(context, block.Surface(1), 32);
    Require(std::any_of(idle.begin(), idle.end(), [](const auto& texture) { return texture.expired(); }),
            "age policy kept textures idle for two seconds because only one present passed");
    ClearCachedTextures(context.device);
    Context pressured = context;
    pressured.memoryProperties2 = &ReportPressure;
    pressured.bufferPool = std::make_shared<BufferPool>(pressured);
    Recorder::CountPresent();
    for (std::size_t surface = 0; surface < 32; ++surface) Sampled(pressured, block.Surface(0) + surface * 65536, 128);
    Require(TextureCacheUsage().sampledBytes <= Ceiling, "age policy overcommitted the sampled cache under VRAM pressure");
    ClearCachedTextures(context.device);
}

void memoryTests(Context context) {
    context.memoryProperties2 = &ReportBudget;
    const auto before = SampledTextureMemory();
    Block block(context);
    auto held = Sampled(context, block.Surface(0));
    Require(!held->ViewsStorageImage() && held->AllocationBytes() != 0, "the test texture is not a snapshot with an image of its own");
    Require(SampledTextureMemory() == before + held->AllocationBytes(), "the sampled texture memory grew by " + std::to_string(SampledTextureMemory() - before) + " bytes, not by the snapshot's allocation of " + std::to_string(held->AllocationBytes()));
    const auto cachedBudget = ReadBudget(context);
    Require(cachedBudget == ExpectedBudget(context), "the sampled texture cache budget does not count its cached texture as the cache's own memory");
    ClearCachedTextures(context.device);
    Require(TextureCacheUsage().sampledEntries == 0 && SampledTextureMemory() == before + held->AllocationBytes(), "a texture its caller still holds stopped counting as sampled texture memory when it left the cache");
    const auto budget = ReadBudget(context);
    Require(budget == ExpectedBudget(context), "the sampled texture cache budget does not keep a texture its caller holds outside the cache out of the budget");
    Require(budget == 2048ull << 20u || budget + held->AllocationBytes() <= cachedBudget + (1ull << 20u), "a texture held outside the cache did not lower the sampled texture cache budget by its allocation");
    std::printf("reported heap budget %llu MiB with %llu MiB in use: sampled texture cache budget %llu MiB with a %llu KiB texture held outside the cache\n", static_cast<unsigned long long>(ReportedBudget >> 20u), static_cast<unsigned long long>(ReportedUsage >> 20u), static_cast<unsigned long long>(budget >> 20u), static_cast<unsigned long long>(held->AllocationBytes() >> 10u));
    held.reset();
    Require(SampledTextureMemory() == before, "a released texture still counts as sampled texture memory");
}

std::array<std::uint32_t, 8> RgbaWords(std::uint64_t address, std::uint32_t side) {
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (56u << 20u) | (((side - 1u) & 3u) << 30u),
        ((side - 1u) >> 2u) | ((side - 1u) << 14u),
        0xfacu | (TileR64KBX << 20u) | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

void scratchPoolTests(const Context& context) {
    constexpr std::uint32_t side = 128;
    const bool on = BufferPool::ScratchEnabled();
    Recorder recorder(context);
    recorder.Activate();
    Block block(context);
    ClearCachedTextures(context.device);
    for (std::size_t surface = 0; surface < 3; ++surface) std::memset(reinterpret_cast<void*>(block.Surface(surface)), static_cast<int>(0x30 * (surface + 1)), SurfaceBytes);
    MirrorSampleProgram sampler(context, recorder);
    const auto lookup = [&](std::size_t surface) { return CachedSampledTexture(context, RgbaWords(block.Surface(surface), side)); };
    const auto pool = GetBufferPool(context);
    auto first = lookup(0);
    auto second = lookup(1);
    Require(!first->ViewsStorageImage() && !second->ViewsStorageImage(), "the scratch pool fixture sampled a storage image");
    Require(pool->ScratchCounters(true).hits == 0 && pool->ScratchCounters(false).hits == 0, "a scratch buffer of an unfinished batch was handed out again");
    expectRed(sampler.Red(first->View(), first->Layout(), 0), 48.0f / 255.0f, "the first texture of a batch");
    recorder.FlushDeferredReleases();
    auto third = lookup(2);
    const auto device = pool->ScratchCounters(true);
    const auto host = pool->ScratchCounters(false);
    if (on) {
        Require(device.misses == 4 && device.hits == 2 && host.misses == 2 && host.hits == 1, "the second upload after the batch completed did not reuse the first uploads' scratch buffers (device " + std::to_string(device.hits) + " hits, " + std::to_string(device.misses) + " misses; host " + std::to_string(host.hits) + " hits, " + std::to_string(host.misses) + " misses)");
        Require(device.retained <= device.limit && host.retained <= host.limit, "the scratch pool kept more than its limits");
    } else {
        Require(device.hits == 0 && device.misses == 0 && host.hits == 0 && host.misses == 0 && device.fallbacks == 0, "the scratch pool counted uploads with the switch off");
    }
    expectRed(sampler.Red(second->View(), second->Layout(), 0), 96.0f / 255.0f, "the second texture of a batch");
    expectRed(sampler.Red(third->View(), third->Layout(), 0), 144.0f / 255.0f, "the texture uploaded through reused scratch buffers");
    first.reset();
    second.reset();
    third.reset();
    recorder.Sync();
}

constexpr std::uint64_t Mib(std::uint64_t count) {
    return count << 20u;
}

TextureCopy CopyAt(std::uint64_t address, TextureBacking backing = TextureBacking::Snapshot, std::uint64_t bytes = Mib(1)) {
    TextureCopy copy;
    copy.address = address;
    copy.guestBytes = bytes;
    copy.gpuBytes = bytes;
    copy.counted = bytes;
    copy.used = 1;
    copy.width = 512;
    copy.height = 512;
    copy.mips = 1;
    copy.dimension = 1;
    copy.tile = 14;
    copy.format = 20;
    copy.elementBytes = 4;
    copy.blockWidth = 1;
    copy.components = {3, 4, 5, 6};
    copy.backing = backing;
    return copy;
}

const DuplicateTotals& KindTotals(const TextureDuplicateSummary& summary, DuplicateKind kind) {
    return summary.kinds[static_cast<std::size_t>(kind)];
}

void duplicateSummaryTests() {
    constexpr std::uint64_t base = 0x100000000ull;
    {
        const std::vector<TextureCopy> copies{CopyAt(base), CopyAt(base + Mib(4))};
        const auto summary = SummarizeTextureDuplicates(copies);
        Require(summary.entries == 2 && summary.images == 2 && summary.ranges == 2 && summary.duplicatedRanges == 0 && summary.extraBytes == 0 && summary.top.empty(), "textures at different addresses were counted as duplicates");
        Require(summary.heldBytes == Mib(2) && summary.overlapping.copies == 0 && summary.nested.copies == 0, "disjoint textures were counted as overlapping");
    }
    {
        auto swizzled = CopyAt(base);
        swizzled.components = {4, 3, 5, 6};
        auto levelled = CopyAt(base);
        levelled.baseLevel = 1;
        const std::vector<TextureCopy> copies{CopyAt(base), swizzled, levelled};
        const auto summary = SummarizeTextureDuplicates(copies);
        Require(summary.duplicatedRanges == 1 && summary.extraBytes == Mib(2) && summary.heldBytes == Mib(3), "two view variants of a texture were not counted as its extra copies");
        Require(KindTotals(summary, DuplicateKind::ViewVariant).copies == 2 && KindTotals(summary, DuplicateKind::ViewVariant).bytes == Mib(2), "view variants were not classified as such");
        Require(summary.viewFields[0].copies == 1 && summary.viewFields[1].copies == 1 && summary.viewFields[2].copies == 0 && summary.viewFields[3].copies == 0 && summary.viewFields[4].copies == 0, "view variants were not attributed to the fields they differ in");
        Require(summary.top.size() == 1 && summary.top.front().address == base && summary.top.front().copies == 3 && summary.top.front().heldBytes == Mib(3) && summary.top.front().extraBytes == Mib(2), "the range of view variants was not reported");
        Require(DescribeDuplicateKinds(summary.top.front()) == "view x2", "the kinds of a range of view variants were described as '" + DescribeDuplicateKinds(summary.top.front()) + "'");
    }
    {
        const std::vector<TextureCopy> copies{CopyAt(base), CopyAt(base)};
        const auto summary = SummarizeTextureDuplicates(copies);
        Require(KindTotals(summary, DuplicateKind::SameDescriptor).copies == 1 && KindTotals(summary, DuplicateKind::ViewVariant).copies == 0, "two copies with the same shape and view parameters were not classified as the same descriptor");
    }
    {
        auto image = CopyAt(base, TextureBacking::Storage, Mib(2));
        image.guestBytes = Mib(1);
        image.used = ~std::uint64_t{0};
        const std::vector<TextureCopy> copies{CopyAt(base), image};
        const auto summary = SummarizeTextureDuplicates(copies);
        Require(KindTotals(summary, DuplicateKind::SampledStorage).copies == 1 && KindTotals(summary, DuplicateKind::SampledStorage).bytes == Mib(1) && summary.extraBytes == Mib(1), "a snapshot beside a storage image of its surface was not counted as the extra copy");
        auto compressed = CopyAt(base);
        compressed.compressed = true;
        compressed.format = 61;
        compressed.blockWidth = 4;
        const std::vector<TextureCopy> blocked{compressed, image};
        const auto blockedSummary = SummarizeTextureDuplicates(blocked);
        Require(KindTotals(blockedSummary, DuplicateKind::SampledStorage).copies == 0 && KindTotals(blockedSummary, DuplicateKind::FormatVariant).copies == 1, "a block-compressed snapshot, which cannot view a storage image, was classified as sampled+storage");
    }
    {
        auto stale = CopyAt(base);
        stale.stale = true;
        stale.used = 9;
        const std::vector<TextureCopy> copies{stale, CopyAt(base)};
        const auto summary = SummarizeTextureDuplicates(copies);
        Require(KindTotals(summary, DuplicateKind::Superseded).copies == 1 && KindTotals(summary, DuplicateKind::Superseded).bytes == Mib(1) && summary.stale.copies == 1, "a stale snapshot beside a current one was not classified as superseded");
    }
    {
        auto format = CopyAt(base);
        format.format = 21;
        auto shape = CopyAt(base);
        shape.width = 256;
        const std::vector<TextureCopy> copies{CopyAt(base), format, shape};
        const auto summary = SummarizeTextureDuplicates(copies);
        Require(KindTotals(summary, DuplicateKind::FormatVariant).copies == 1 && KindTotals(summary, DuplicateKind::ShapeVariant).copies == 1 && summary.extraBytes == Mib(2), "format and extent variants were not classified apart");
    }
    {
        auto view = CopyAt(base, TextureBacking::StorageView, 0);
        const std::vector<TextureCopy> copies{CopyAt(base), view};
        const auto summary = SummarizeTextureDuplicates(copies);
        Require(summary.entries == 2 && summary.images == 1 && summary.duplicatedRanges == 0 && summary.extraBytes == 0, "a view of a storage image, which owns no memory, was counted as a copy");
    }
    {
        auto inner = CopyAt(base + Mib(1));
        auto straddling = CopyAt(base + Mib(3), TextureBacking::Snapshot, Mib(2));
        const std::vector<TextureCopy> copies{CopyAt(base, TextureBacking::Snapshot, Mib(4)), CopyAt(base, TextureBacking::Snapshot, Mib(4)), inner, straddling};
        const auto summary = SummarizeTextureDuplicates(copies);
        Require(summary.overlapping.copies == 2 && summary.overlapping.bytes == Mib(3) && summary.nested.copies == 1 && summary.nested.bytes == Mib(1), "overlapping textures at other base addresses were not counted (" + std::to_string(summary.overlapping.copies) + " overlapping, " + std::to_string(summary.nested.copies) + " nested)");
    }
    {
        auto depth = CopyAt(base, TextureBacking::DepthCopy, Mib(8));
        depth.counted = 0;
        depth.depthCompare = true;
        const std::vector<TextureCopy> copies{depth, CopyAt(base + Mib(16))};
        const auto summary = SummarizeTextureDuplicates(copies);
        Require(summary.depthCopies.copies == 1 && summary.depthCopies.bytes == Mib(8) && summary.uncounted.copies == 1 && summary.uncounted.bytes == Mib(8), "a depth copy outside the cache budget was not counted");
    }
    {
        std::vector<TextureCopy> copies;
        for (std::uint64_t range = 0; range < 12; ++range) {
            auto swizzled = CopyAt(base + range * Mib(16), TextureBacking::Snapshot, Mib(1 + range));
            swizzled.components = {4, 3, 5, 6};
            copies.push_back(CopyAt(base + range * Mib(16), TextureBacking::Snapshot, Mib(1 + range)));
            copies.push_back(swizzled);
        }
        const auto summary = SummarizeTextureDuplicates(copies);
        Require(summary.duplicatedRanges == 12 && summary.top.size() == 10 && summary.top.front().address == base + 11 * Mib(16) && summary.top.front().extraBytes == Mib(12), "the top ranges were not the ten with the most extra bytes");
        for (std::size_t rank = 1; rank < summary.top.size(); ++rank) Require(summary.top[rank - 1].extraBytes > summary.top[rank].extraBytes, "the top ranges are not in descending order");
        Require(SummarizeTextureDuplicates(copies, 3).top.size() == 3, "the number of reported ranges does not follow the limit");
    }
}

using Words = std::array<std::uint32_t, 8>;

Words MippedDescriptor(std::uint64_t address) {
    auto words = Descriptor(address);
    words[3] |= 3u << 16u;
    words[5] |= 3u << 4u;
    return words;
}

Words WithBit(Words words, std::size_t word, unsigned bit) {
    words[word] ^= 1u << bit;
    return words;
}

Words WithMinLod(Words words, std::uint32_t raw) {
    words[1] = (words[1] & ~(0xfffu << 8u)) | (raw << 8u);
    return words;
}

bool SameTextureContent(const GuestTextureResource& first, const GuestTextureResource& second) {
    return first.baseAddress == second.baseAddress && first.pipeBankXor == second.pipeBankXor && first.width == second.width && first.height == second.height && first.depthOrLastArray == second.depthOrLastArray && first.baseArray == second.baseArray && first.mipCount == second.mipCount && first.allocatedMipCount == second.allocatedMipCount && first.baseLevel == second.baseLevel && first.lastLevel == second.lastLevel && first.tileMode == second.tileMode && first.dimension == second.dimension && first.format == second.format && first.bcSwizzle == second.bcSwizzle && first.dccAddress == second.dccAddress && EffectiveMinLod(first) == EffectiveMinLod(second);
}

void dedupeKeyTests() {
    const bool dedupe = std::getenv("APS5_TEXTURE_DEDUPE") != nullptr;
    const Words plain = MippedDescriptor(0x10000000ull);
    const auto where = [](std::size_t word, unsigned bit) { return "word " + std::to_string(word) + " bit " + std::to_string(bit); };
    const auto shares = [](const Words& first, const Words& second) { return SampledTexturesShareEntry(first, second); };
    Require(shares(plain, plain), "a descriptor does not share an entry with itself");
    std::vector<std::pair<std::size_t, unsigned>> masked;
    for (unsigned bit = 8; bit < 20; ++bit) masked.emplace_back(5, bit);
    masked.emplace_back(5, 25);
    for (unsigned bit = 0; bit < 8; ++bit) masked.emplace_back(6, bit);
    for (const auto& [word, bit] : masked) Require(shares(plain, WithBit(plain, word, bit)), where(word, bit) + " is part of the key, as the statistics bits were not");
    std::vector<std::pair<std::size_t, unsigned>> ignored{{1, 29}, {2, 30}, {2, 31}};
    for (const unsigned bit : {13u, 14u, 15u, 29u, 30u, 31u}) ignored.emplace_back(4, bit);
    for (const unsigned bit : {20u, 21u, 22u, 24u, 27u, 28u, 29u, 30u, 31u}) ignored.emplace_back(5, bit);
    for (unsigned bit = 8; bit <= 9; ++bit) ignored.emplace_back(6, bit);
    for (unsigned bit = 11; bit <= 20; ++bit) ignored.emplace_back(6, bit);
    for (unsigned bit = 22; bit <= 31; ++bit) ignored.emplace_back(6, bit);
    for (unsigned bit = 0; bit < 32; ++bit) ignored.emplace_back(7, bit);
    const auto reference = DecodeTextureResource(plain);
    for (const auto& [word, bit] : ignored) {
        const auto changed = WithBit(plain, word, bit);
        Require(SameTextureContent(reference, DecodeTextureResource(changed)), where(word, bit) + " changes what the descriptor decodes to, so the dedupe key may not ignore it");
        Require(shares(plain, changed) == dedupe, where(word, bit) + (dedupe ? " is still part of the texture key" : " is not part of the key without APS5_TEXTURE_DEDUPE"));
    }
    auto reselected = plain;
    reselected[3] = (reselected[3] & ~0xfffu) | 5u | (4u << 3u) | (6u << 6u) | (7u << 9u);
    const auto reselectedResource = DecodeTextureResource(reselected);
    Require(reselectedResource.dstSelX != reference.dstSelX && SameTextureContent(reference, reselectedResource), "the reselected descriptor differs in more than its channel selectors");
    Require(shares(plain, reselected) == dedupe, "descriptors that differ in their channel selectors alone " + std::string(dedupe ? "do not share a key although the swizzle is part of it separately" : "share a key without APS5_TEXTURE_DEDUPE"));
    const std::vector<std::pair<std::size_t, unsigned>> distinct{{0, 8}, {1, 20}, {1, 30}, {2, 0}, {2, 14}, {3, 12}, {3, 16}, {3, 20}, {3, 25}, {3, 28}, {4, 0}, {4, 16}, {5, 4}, {6, 21}};
    for (const auto& [word, bit] : distinct) Require(!shares(plain, WithBit(plain, word, bit)), where(word, bit) + " does not distinguish textures");
    auto metadata = plain;
    metadata[6] |= 1u << 21u;
    for (const auto& [word, bit] : std::vector<std::pair<std::size_t, unsigned>>{{6, 19}, {6, 22}, {6, 23}, {6, 24}, {6, 31}, {7, 0}, {7, 31}}) Require(!shares(metadata, WithBit(metadata, word, bit)), where(word, bit) + " of a surface with DCC metadata does not distinguish textures");
    auto levelled = plain;
    levelled[3] = (levelled[3] & ~(0xfu << 12u)) | (1u << 12u);
    Require(shares(WithMinLod(levelled, 0), WithMinLod(levelled, 0x100)) == dedupe, "minimum LOD values below the base level, which clamp nothing, " + std::string(dedupe ? "do not share a key" : "share a key without APS5_TEXTURE_DEDUPE"));
    Require(shares(WithMinLod(plain, 0x300), WithMinLod(plain, 0xa00)) == dedupe, "minimum LOD values past the last level, which clamp to it, " + std::string(dedupe ? "do not share a key" : "share a key without APS5_TEXTURE_DEDUPE"));
    Require(!shares(WithMinLod(plain, 0x100), WithMinLod(plain, 0x180)) && !shares(WithMinLod(plain, 0), WithMinLod(plain, 0x100)), "minimum LOD values that clamp differently share a key");
    constexpr std::array<std::uint32_t, 17> raws{0, 1, 100, 255, 256, 257, 300, 511, 512, 513, 700, 767, 768, 769, 1000, 2048, 4095};
    for (const auto [baseLevel, lastLevel] : {std::pair{0u, 3u}, std::pair{1u, 3u}, std::pair{2u, 2u}, std::pair{3u, 3u}}) {
        auto descriptor = plain;
        descriptor[3] = (descriptor[3] & ~((0xfu << 12u) | (0xfu << 16u))) | (baseLevel << 12u) | (lastLevel << 16u);
        for (const auto first : raws) {
            for (const auto second : raws) {
                auto left = WithMinLod(descriptor, first);
                auto right = WithMinLod(descriptor, second);
                const bool sameClamp = EffectiveMinLod(DecodeTextureResource(left)) == EffectiveMinLod(DecodeTextureResource(right));
                CanonicalizeTextureWords(left);
                CanonicalizeTextureWords(right);
                Require((left[1] == right[1]) == sameClamp, "the canonical minimum LOD of " + std::to_string(first) + " and " + std::to_string(second) + " at levels " + std::to_string(baseLevel) + ".." + std::to_string(lastLevel) + " disagrees with the clamp the view applies");
            }
        }
    }
    auto once = plain;
    CanonicalizeTextureWords(once);
    auto twice = once;
    CanonicalizeTextureWords(twice);
    Require(once == twice, "canonicalizing a descriptor twice changed it again");
}


std::uint64_t epochReadBytes() {
    std::uint64_t bytes = 0;
    for (const auto& site : AgcDriver::PageTracker::Get()->Counts().reads) for (const auto& kind : site) bytes += kind.total;
    return bytes;
}

std::uint64_t epochCount(const char* name) {
    const auto line = TextureEpoch::ProfileLine();
    const auto start = line.find(std::string(name) + "=");
    Require(start != std::string::npos, "epoch texture counter is missing");
    return std::stoull(line.substr(start + std::strlen(name) + 1));
}

void epochMetadataTests() {
    using namespace AgcDriver::PageTracker;
    const auto exact = [](const char* name) { const auto* value = std::getenv(name); return value != nullptr && std::strcmp(value, "1") == 0; };
    Require(TextureEpoch::Enabled() == (exact("APS5_EPOCH_TEXTURES") && exact("APS5_PAGE_TRACKER")), "epoch texture switch requires exact opt-in to both switches");
    const Range range{0x300000000ull, 4 * RegionBytes};
    auto* tracker = Get();
    if (tracker != nullptr) tracker->RegisterRange(range, Kind::Section, Bit(Reason::ProtectedSection));
    TextureEpoch proof;
    proof.Capture(range);
    if (!TextureEpoch::Enabled()) {
        Require(!proof.token.covered && proof.Check(range).action == TextureEpoch::Action::Fallback, "disabled texture epochs produced a proof");
        const auto before = TextureEpoch::ProfileLine();
        TextureEpoch::Count(TextureEpoch::Action::Full, 123);
        Require(before == TextureEpoch::ProfileLine(), "disabled texture epochs changed counters");
        return;
    }
    Require(proof.Check(range).action == TextureEpoch::Action::Reuse, "unchanged texture did not reuse its epoch");
    tracker->MarkCpuWritten({range.address + RegionBytes + PageBytes, 1}, Source::DriverStore);
    auto update = proof.Check(range);
    Require(update.action == TextureEpoch::Action::Partial && update.runs == std::vector<std::pair<std::uint64_t, std::uint64_t>>{{RegionBytes + PageBytes, RegionBytes + 2 * PageBytes}}, "CPU write did not select its dirty page");
    tracker->MarkCpuWritten({range.address + 2 * RegionBytes, 2 * RegionBytes - PageBytes}, Source::DriverStore);
    Require(proof.Check(range).action == TextureEpoch::Action::Partial, "half-dirty texture did not allow a partial upload");
    tracker->MarkCpuWritten({range.address, RegionBytes}, Source::DriverStore);
    Require(proof.Check(range).action == TextureEpoch::Action::Partial, "majority-dirty texture widened its upload to clean pages");
    tracker->MarkCpuWritten(range, Source::DriverStore);
    Require(proof.Check(range).action == TextureEpoch::Action::Full, "fully dirty texture did not require a full upload");
    proof.Capture(range);
    tracker->MarkGpuWritten({range.address, PageBytes}, Source::GpuDeclaration);
    Require(proof.Check(range).action == TextureEpoch::Action::Fallback, "GPU-pending texture reused a proof");
    tracker->RegisterRange(range, Kind::Section, Bit(Reason::ProtectedSection));
    proof.Capture(range);
    tracker->InvalidateRange({range.address, PageBytes}, true);
    Require(proof.Check(range).action == TextureEpoch::Action::Fallback, "unknown texture coverage reused a proof");
    tracker->RegisterRange(range, Kind::Section, Bit(Reason::ProtectedSection));
    Require(proof.Check(range).action == TextureEpoch::Action::Full, "remapping resurrected a texture proof");
    proof.Capture(range);
    tracker->MarkCpuWritten({range.address, PageBytes}, Source::SectionFault);
    Require(proof.Check(range).action == TextureEpoch::Action::Fallback, "an open CPU writer reused a texture proof");
}

#ifdef _WIN32
PFN_vkGetDeviceProcAddr epochDeviceProc = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL epochCreateImage(VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator, VkImage* image) {
    auto readable = *info;
    readable.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return reinterpret_cast<PFN_vkCreateImage>(epochDeviceProc(device, "vkCreateImage"))(device, &readable, allocator, image);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL epochGetDeviceProc(VkDevice device, const char* name) {
    if (std::strcmp(name, "vkCreateImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&epochCreateImage);
    return epochDeviceProc(device, name);
}

class EpochTextureMemory {
public:
    static constexpr std::size_t Bytes = 4 * AgcDriver::PageTracker::RegionBytes;
    EpochTextureMemory() {
        allocation = VirtualAlloc(nullptr, Bytes + AgcDriver::PageTracker::RegionBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        Require(allocation != nullptr, "cannot allocate epoch texture fixture");
        const auto aligned = (reinterpret_cast<std::uintptr_t>(allocation) + AgcDriver::PageTracker::RegionBytes - 1) & ~(AgcDriver::PageTracker::RegionBytes - 1);
        memory = reinterpret_cast<std::byte*>(aligned);
        std::memset(memory, 64, Bytes);
        if (auto* tracker = AgcDriver::PageTracker::Get()) tracker->RegisterRange(Range(), AgcDriver::PageTracker::Kind::Section, AgcDriver::PageTracker::Bit(AgcDriver::PageTracker::Reason::ProtectedSection));
    }
    ~EpochTextureMemory() {
        if (registered) GuestAllocations::Mutation().Remove(allocation);
        if (auto* tracker = AgcDriver::PageTracker::Get()) tracker->InvalidateRange(Range(), true);
        VirtualFree(allocation, 0, MEM_RELEASE);
    }
    void Register() {
        if (registered) return;
        GuestAllocations::Mutation().Add(allocation, Bytes + AgcDriver::PageTracker::RegionBytes, true, true, true);
        registered = true;
    }
    AgcDriver::PageTracker::Range Range() const { return {reinterpret_cast<std::uintptr_t>(memory), Bytes}; }
    void Protect(std::size_t offset, std::size_t bytes, DWORD protection) {
        DWORD previous = 0;
        Require(VirtualProtect(memory + offset, bytes, protection, &previous), "cannot protect epoch texture fixture");
    }
    void Write(std::size_t offset, std::size_t bytes, int value) {
        std::memset(memory + offset, value, bytes);
        if (auto* tracker = AgcDriver::PageTracker::Get()) tracker->MarkCpuWritten({Range().address + offset, bytes}, AgcDriver::PageTracker::Source::DriverStore);
    }
private:
    void* allocation = nullptr;
    std::byte* memory = nullptr;
    bool registered = false;
};

std::vector<std::byte> epochImageBytes(const Context& context, Recorder& recorder, const StorageTexture& texture, std::uint32_t side) {
    Buffer readback(context, static_cast<std::size_t>(side) * side * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto commands = recorder.Commands();
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {side, side, 1};
    CopyImageToBuffer(context, commands, texture.Image(), VK_IMAGE_LAYOUT_GENERAL, readback.Handle(), 1, &copy);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    recorder.Submit();
    recorder.Sync();
    const auto bytes = readback.ReadBytes(0, readback.Size());
    return {bytes.begin(), bytes.end()};
}

std::vector<std::byte> epochSurfaceBytes(const Context& context, Recorder& recorder, VkImage image, VkImageLayout layout, const GuestTextureResource& descriptor) {
    const auto geometry = DescribeSurface(descriptor);
    std::vector<VkBufferImageCopy> regions;
    std::size_t size = 0;
    for (std::uint32_t level = 0; level < descriptor.mipCount; ++level) {
        VkBufferImageCopy region{};
        region.bufferOffset = size;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, geometry.imageLayers};
        region.imageExtent = {std::max(descriptor.width >> level, 1u), std::max(descriptor.height >> level, 1u), std::max(geometry.imageDepth >> level, 1u)};
        regions.push_back(region);
        size += static_cast<std::size_t>((region.imageExtent.width + BlockWidth(descriptor.format) - 1u) / BlockWidth(descriptor.format)) * ((region.imageExtent.height + BlockHeight(descriptor.format) - 1u) / BlockHeight(descriptor.format)) * region.imageExtent.depth * geometry.imageLayers * BytesPerElement(descriptor.format);
    }
    Buffer readback(context, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto commands = recorder.Commands();
    VkImageMemoryBarrier source{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    source.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    source.oldLayout = layout;
    source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source.image = image;
    source.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, descriptor.mipCount, 0, geometry.imageLayers};
    const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &source);
    CopyImageToBuffer(context, commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.Handle(), static_cast<std::uint32_t>(regions.size()), regions.data());
    auto done = source;
    done.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    done.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    done.oldLayout = source.newLayout;
    done.newLayout = layout;
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &done);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    recorder.Submit();
    recorder.Sync();
    const auto bytes = readback.ReadBytes(0, size);
    return {bytes.begin(), bytes.end()};
}

void epochLayoutTextureTests(const Context& context, std::uint32_t format, std::uint32_t tileMode, bool volume) {
    using namespace AgcDriver::PageTracker;
    const bool enabled = TextureEpoch::Enabled();
    const auto counters = TextureEpoch::ProfileLine();
    EpochTextureMemory memory;
    auto words = RgbaWords(memory.Range().address, IsBlockCompressed(format) ? 259 : 129);
    const auto lastLevel = volume ? 5u : 3u;
    words[1] = (words[1] & ~(0x1ffu << 20u)) | (format << 20u);
    words[3] = (words[3] & 0xffffu) | (lastLevel << 16u) | (tileMode << 20u) | ((volume ? 10u : 13u) << 28u);
    words[4] = volume ? 18 : 1;
    words[5] = lastLevel << 4u;
    if (tileMode == TileR64KBX || tileMode == 0x19u || tileMode == 0x1au) words[0] |= 0xcfu;
    const auto descriptor = DecodeTextureResource(words);
    const auto geometry = DescribeSurface(descriptor);
    Require(geometry.guestBytes > 2 * PageBytes && geometry.guestBytes < EpochTextureMemory::Bytes, "extended epoch fixture size is invalid");
    ClearCachedTextures(context.device);
    Recorder recorder(context);
    recorder.Activate();
    std::shared_ptr<StorageTexture> storage;
    if (!IsBlockCompressed(format)) storage = StorageTexture::Create(context, *context.detiler, descriptor, 0);
    auto sampled = CachedSampledTexture(context, words);
    Require(!sampled->ViewsStorageImage(), "extended epoch fixture needs its own sampled image");
    auto initial = epochSurfaceBytes(context, recorder, sampled->Image(), sampled->Layout(), descriptor);
    unsigned value = 64;
    const auto* path = IsBlockCompressed(format) ? "partial-bc" : geometry.thick ? "partial-thick" : tileMode == 0 ? "partial-linear" : "partial-tiled";
    bool pixelsChanged = false;
    std::vector<std::uint64_t> pages{0, PageBytes, (geometry.guestBytes / 2) & ~(PageBytes - 1), (geometry.guestBytes - 1) & ~(PageBytes - 1)};
    const auto mode = geometry.thick ? (tileMode == 0x05u || tileMode == 0x09u ? tileMode : XorSwizzleMode(descriptor.tileMode)) | 0x100u : EquationSwizzleMode(descriptor.tileMode);
    const auto* equation = FindTextureSwizzleEquation(mode, BytesPerElement(format));
    for (const auto& mip : geometry.mips) {
        if (!mip.tail) continue;
        if (equation != nullptr) {
            auto offset = descriptor.pipeBankXor;
            const auto coordinate = mip.tailX | (mip.tailY << 12u);
            for (unsigned bit = 0; bit < 16; ++bit) offset ^= (std::popcount(equation->bits[bit] & coordinate) & 1u) << bit;
            pages.push_back((mip.tiledOffset + offset) & ~(PageBytes - 1));
        } else {
            for (std::uint64_t offset = 0; offset < mip.tiledSize; offset += PageBytes) pages.push_back(mip.tiledOffset + offset);
            break;
        }
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    for (const auto page : pages) {
        const auto bytes = std::min(PageBytes, geometry.guestBytes - page);
        value = value >= 224 ? 96 : value + 32;
        memory.Write(page, bytes, value);
        const auto partials = epochCount("partial");
        const auto uploaded = epochCount("bytes-uploaded");
        const auto layoutPartials = enabled ? epochCount(path) : 0;
        if (enabled) {
            memory.Protect(0, EpochTextureMemory::Bytes, PAGE_NOACCESS);
            memory.Protect(page, PageBytes, PAGE_READWRITE);
        }
        const auto changed = CachedSampledTexture(context, words);
        if (enabled) {
            Require(changed == sampled && epochCount("partial") == partials + 1 && epochCount("bytes-uploaded") - uploaded <= PageBytes, "extended sampled update did not stay within its dirty page");
            Require(epochCount(path) == layoutPartials + 1, "extended sampled update used the wrong path counter");
        } else Require(changed != sampled, "disabled extended epoch update changed sampled rebuilds");
        sampled = changed;
        if (storage != nullptr) {
            const auto storagePartials = enabled ? epochCount("storage-partial") : 0;
            Require(!storage->Refresh(), "extended storage CPU write did not refresh");
            if (enabled) Require(epochCount("storage-partial") == storagePartials + 1, "extended storage update missed its partial path");
        }
        if (enabled) memory.Protect(0, EpochTextureMemory::Bytes, PAGE_READWRITE);
        const auto actual = epochSurfaceBytes(context, recorder, sampled->Image(), sampled->Layout(), descriptor);
        const auto snapshot = std::span(reinterpret_cast<const std::byte*>(descriptor.baseAddress), static_cast<std::size_t>(geometry.guestBytes));
        Texture reference(context, *context.detiler, descriptor, {}, snapshot);
        const auto expected = epochSurfaceBytes(context, recorder, reference.Image(), reference.Layout(), descriptor);
        Require(actual == expected, "extended partial image differs from a complete detile, including mip edges and layers");
        pixelsChanged = pixelsChanged || actual != initial;
        if (storage != nullptr) Require(epochSurfaceBytes(context, recorder, storage->Image(), VK_IMAGE_LAYOUT_GENERAL, descriptor) == expected, "extended storage partial image differs from a complete detile");
        if (enabled) {
            memory.Protect(0, EpochTextureMemory::Bytes, PAGE_NOACCESS);
            Require(CachedSampledTexture(context, words) == sampled && (storage == nullptr || storage->Refresh()), "extended refresh did not advance its epoch");
            memory.Protect(0, EpochTextureMemory::Bytes, PAGE_READWRITE);
        }
    }
    Require(pixelsChanged, "extended epoch fixture never changed image data");
    if (storage != nullptr) {
        const auto commands = recorder.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue clear{};
        for (auto& channel : clear.float32) channel = 128.f / 255.f;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, descriptor.mipCount, 0, geometry.imageLayers};
        context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, storage->Image(), VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        storage->MarkDirty();
        const auto fallbacks = enabled ? epochCount("storage-fallback") : 0;
        Require(storage->Refresh(), "GPU-written extended storage image was overwritten by a CPU epoch upload");
        if (enabled) Require(epochCount("storage-fallback") == fallbacks + 1, "pending GPU storage refresh did not count its fallback");
        const auto gpu = epochSurfaceBytes(context, recorder, storage->Image(), VK_IMAGE_LAYOUT_GENERAL, descriptor);
        Require(std::all_of(gpu.begin(), gpu.end(), [](std::byte byte) { return byte == std::byte{128}; }), "GPU clear did not reach every storage mip and depth slice");
        memory.Register();
        storage->WriteBack();
        recorder.Sync();
        std::vector<std::byte> snapshot(static_cast<std::size_t>(geometry.guestBytes));
        AgcDriver::GuestMemory::ReadCommitted(descriptor.baseAddress, snapshot);
        Texture reference(context, *context.detiler, descriptor, {}, snapshot);
        Require(epochSurfaceBytes(context, recorder, reference.Image(), reference.Layout(), descriptor) == gpu, "partial-refresh baseline lost GPU writes during storage write-back");
        storage->Refresh();
        Require(epochSurfaceBytes(context, recorder, storage->Image(), VK_IMAGE_LAYOUT_GENERAL, descriptor) == gpu, "storage refresh rolled back its published GPU writes");
    }
    recorder.Sync();
    ClearCachedTextures(context.device);
    if (!enabled) Require(TextureEpoch::ProfileLine() == counters, "disabled extended epochs changed profiling output");
}

void epochFallbackTextureTests(const Context& context) {
    const bool enabled = TextureEpoch::Enabled();
    const auto counters = TextureEpoch::ProfileLine();
    EpochTextureMemory memory;
    auto words = RgbaWords(memory.Range().address + 256, 256);
    words[3] = (words[3] & ~(0x1fu << 20u)) | (0x05u << 20u);
    const auto descriptor = DecodeTextureResource(words);
    Require(descriptor.baseAddress % 65536 != 0, "alignment fallback fixture became aligned");
    Recorder recorder(context);
    recorder.Activate();
    auto storage = StorageTexture::Create(context, *context.detiler, descriptor, 0);
    auto sampled = CachedSampledTexture(context, words);
    memory.Write(AgcDriver::PageTracker::PageBytes, AgcDriver::PageTracker::PageBytes, 128);
    const auto alignment = enabled ? epochCount("fallback-tiled-base-alignment") : 0;
    Require(!storage->Refresh(), "unaligned storage update did not refresh");
    if (enabled) Require(epochCount("fallback-tiled-base-alignment") == alignment + 1, "storage alignment fallback reason is missing");
    const auto changed = CachedSampledTexture(context, words);
    Require(changed != sampled, "unaligned sampled update did not rebuild");
    if (enabled) Require(epochCount("fallback-tiled-base-alignment") == alignment + 2, "sampled alignment fallback was not counted exactly once");
    const auto expected = epochSurfaceBytes(context, recorder, changed->Image(), changed->Layout(), descriptor);
    Require(epochSurfaceBytes(context, recorder, storage->Image(), VK_IMAGE_LAYOUT_GENERAL, descriptor) == expected, "alignment fallback pixels differ between storage and sampled images");
    if (auto* tracker = AgcDriver::PageTracker::Get()) tracker->InvalidateRange(memory.Range());
    const auto unavailable = enabled ? epochCount("fallback-unavailable") + epochCount("fallback-tiled-base-alignment") : 0;
    Require(storage->Refresh(), "unknown coverage changed clean storage content");
    if (enabled) Require(epochCount("fallback-unavailable") + epochCount("fallback-tiled-base-alignment") == unavailable + 1, "unknown coverage fallback reason is missing");
    recorder.Sync();
    ClearCachedTextures(context.device);
    if (!enabled) Require(TextureEpoch::ProfileLine() == counters, "disabled fallback checks changed profiling output");
}

void epochDccFallbackTests(const Context& context) {
    const bool enabled = TextureEpoch::Enabled();
    const auto counters = TextureEpoch::ProfileLine();
    EpochTextureMemory memory;
    const auto keys = memory.Range().address + EpochTextureMemory::Bytes / 2;
    auto words = RgbaWords(memory.Range().address, 256);
    words[6] = (1u << 21u) | (static_cast<std::uint32_t>(keys >> 8u) << 24u);
    words[7] = static_cast<std::uint32_t>(keys >> 16u);
    const auto descriptor = DecodeTextureResource(words);
    const auto geometry = DescribeSurface(descriptor);
    Require(descriptor.dccAddress == keys, "DCC fixture metadata address changed");
    memory.Write(EpochTextureMemory::Bytes / 2, 65536, 255);
    Recorder recorder(context);
    recorder.Activate();
    auto storage = StorageTexture::Create(context, *context.detiler, descriptor, 0);
    auto sampled = CachedSampledTexture(context, words);
    for (const auto key : {0u, 0xffu, 0xc0u}) {
        memory.Write(EpochTextureMemory::Bytes / 2, 65536, key);
        memory.Write(AgcDriver::PageTracker::PageBytes, AgcDriver::PageTracker::PageBytes, 96);
        const auto reasons = enabled ? epochCount("fallback-dcc-metadata") : 0;
        const auto partials = epochCount("partial");
        Require(!storage->Refresh(), "DCC metadata change did not refresh storage");
        if (enabled) Require(epochCount("fallback-dcc-metadata") == reasons + 1, "DCC storage fallback did not report its metadata reason exactly once");
        const auto sampledReasons = enabled ? epochCount("fallback-dcc-metadata") : 0;
        sampled = CachedSampledTexture(context, words);
        if (enabled) Require(epochCount("fallback-dcc-metadata") == sampledReasons + 1, "DCC sampled fallback did not report its metadata reason exactly once");
        const auto actual = epochSurfaceBytes(context, recorder, sampled->Image(), sampled->Layout(), descriptor);
        std::vector<std::byte> snapshot(static_cast<std::size_t>(geometry.guestBytes));
        ReadTextureSurface(descriptor, TextureClearKeys(descriptor, geometry.guestBytes), snapshot);
        Texture reference(context, *context.detiler, descriptor, {}, snapshot);
        const auto expected = epochSurfaceBytes(context, recorder, reference.Image(), reference.Layout(), descriptor);
        Require(actual == expected && epochSurfaceBytes(context, recorder, storage->Image(), VK_IMAGE_LAYOUT_GENERAL, descriptor) == expected, "DCC fallback pixels differ from a complete metadata-aware upload");
        if (enabled) Require(epochCount("partial") == partials, "DCC fallback used an unsafe partial upload");
    }
    recorder.Sync();
    ClearCachedTextures(context.device);
    if (!enabled) Require(TextureEpoch::ProfileLine() == counters, "disabled DCC fallback changed profiling output");
}

void epochPageTextureTests(const Context& context, bool linear, std::uint32_t tileMode = TileR64KBX) {
    using namespace AgcDriver::PageTracker;
    const bool enabled = TextureEpoch::Enabled();
    const auto before = TextureEpoch::ProfileLine();
    EpochTextureMemory memory;
    auto words = RgbaWords(memory.Range().address, 256);
    if (linear) words[3] &= ~(0x1fu << 20u);
    else {
        words[3] = (words[3] & ~(0x1fu << 20u)) | (tileMode << 20u);
        if (tileMode == TileR64KBX) words[0] |= 0xcfu;
    }
    const auto descriptor = DecodeTextureResource(words);
    const auto geometry = DescribeSurface(descriptor);
    Require(geometry.guestBytes >= 2 * PageBytes, "page fixture is too small");
    Recorder recorder(context);
    recorder.Activate();
    auto storage = StorageTexture::Create(context, *context.detiler, descriptor, 0);
    const auto initial = epochImageBytes(context, recorder, *storage, 256);
    ClearCachedTextures(context.device);
    auto sampled = CachedSampledTexture(context, words);
    MirrorSampleProgram sampler(context, recorder);
    recorder.Sync();
    auto centerOffset = std::uint64_t{128} * geometry.mips[0].pitchBytes + 128 * 4;
    if (!linear) {
        centerOffset = descriptor.pipeBankXor;
        const auto* equation = FindTextureSwizzleEquation(EquationSwizzleMode(descriptor.tileMode), 4);
        if (equation != nullptr) for (unsigned bit = 0; bit < 16; ++bit) centerOffset ^= (std::popcount(equation->bits[bit] & (128u | (128u << 12u))) & 1u) << bit;
        const auto block = ThinBlockLayout(descriptor.tileMode, 4);
        centerOffset += (128 / block[2] * geometry.mips[0].blocksPerRow + 128 / block[1]) * block[0];
    }
    const auto centerPage = centerOffset & ~(PageBytes - 1);
    int value = 224;
    for (const auto page : {PageBytes, centerPage, geometry.guestBytes - PageBytes, centerPage}) {
        value -= 32;
        memory.Write(page, PageBytes, value);
        const auto uploaded = epochCount("bytes-uploaded");
        const auto partials = epochCount("partial");
        const auto read = enabled ? epochReadBytes() : 0;
        if (enabled) {
            memory.Protect(0, EpochTextureMemory::Bytes, PAGE_NOACCESS);
            memory.Protect(page, PageBytes, PAGE_READWRITE);
        }
        const auto changed = CachedSampledTexture(context, words);
        if (enabled) {
            Require(changed == sampled && epochCount("partial") == partials + 1 && epochCount("bytes-uploaded") == uploaded + PageBytes, "sampled single-page write uploaded more than its page");
            Require(epochReadBytes() == read + PageBytes, "sampled single-page update read outside its page");
        } else Require(changed != sampled, "disabled page update changed legacy sampled rebuild");
        sampled = changed;
        const auto storageUploaded = epochCount("bytes-uploaded");
        Require(!storage->Refresh(), "single-page storage write did not refresh");
        if (enabled) {
            Require(epochCount("bytes-uploaded") == storageUploaded + PageBytes, "storage single-page write uploaded more than its page");
            memory.Protect(0, EpochTextureMemory::Bytes, PAGE_READWRITE);
        }
        const auto actual = epochImageBytes(context, recorder, *storage, 256);
        auto reference = StorageTexture::Create(context, *context.detiler, descriptor, 0);
        const auto expected = epochImageBytes(context, recorder, *reference, 256);
        Require(actual == expected && actual != initial, "single-page update pixels differ from a complete detile");
        expectRed(sampler.Red(sampled->View(), sampled->Layout(), 0), std::to_integer<unsigned>(actual[(128 * 256 + 128) * 4]) / 255.f, "single-page sampled pixel");
        if (enabled) {
            memory.Protect(0, EpochTextureMemory::Bytes, PAGE_NOACCESS);
            Require(storage->Refresh() && CachedSampledTexture(context, words) == sampled, "page refresh did not advance its token");
            memory.Protect(0, EpochTextureMemory::Bytes, PAGE_READWRITE);
        }
    }
    recorder.Sync();
    ClearCachedTextures(context.device);
    if (!enabled) Require(TextureEpoch::ProfileLine() == before, "disabled page updates changed counters");
}

void epochTextureTests(const Context& context) {
    using namespace AgcDriver::PageTracker;
    const bool enabled = TextureEpoch::Enabled();
    const auto countersBefore = TextureEpoch::ProfileLine();
    EpochTextureMemory memory;
    auto words = RgbaWords(memory.Range().address, 1024);
    words[3] = (words[3] & 0x0fffffffu) | (13u << 28u);
    words[4] = 3;
    const auto descriptor = DecodeTextureResource(words);
    Require(DescribeSurface(descriptor).guestBytes == EpochTextureMemory::Bytes, "epoch texture fixture geometry changed");
    ClearCachedTextures(context.device);
    Recorder recorder(context);
    MirrorSampleProgram sampler(context, recorder, SAMPLE_ARRAY_SPV);
    auto texture = CachedSampledTexture(context, words);
    Require(!texture->ViewsStorageImage(), "epoch sampled fixture unexpectedly became a storage view");
    expectRed(sampler.Red(texture->View(), texture->Layout(), 0, 0), 64.f / 255, "initial epoch sampled pixel");
    if (enabled) {
        Require(TextureCacheUsage().snapshotBytes == 0 && TextureCacheUsage().omittedSnapshotBytes >= EpochTextureMemory::Bytes, "covered sampled texture retained its snapshot");
        const auto counts = epochReadBytes();
        memory.Protect(0, EpochTextureMemory::Bytes, PAGE_NOACCESS);
        Require(CachedSampledTexture(context, words) == texture, "unchanged no-access texture did not reuse its image");
        Require(epochReadBytes() == counts, "epoch sampled reuse issued a guest read");
        MEMORY_BASIC_INFORMATION state{};
        VirtualQuery(reinterpret_cast<void*>(memory.Range().address), &state, sizeof(state));
        Require(state.Protect == PAGE_NOACCESS, "epoch sampled reuse changed guest protection");
        memory.Protect(0, EpochTextureMemory::Bytes, PAGE_READWRITE);
    } else Require(CachedSampledTexture(context, words) == texture, "disabled epoch textures changed normal cache reuse");
    memory.Write(0, RegionBytes, 192);
    const auto partials = epochCount("partial");
    const auto uploaded = epochCount("bytes-uploaded");
    if (enabled) memory.Protect(RegionBytes, EpochTextureMemory::Bytes - RegionBytes, PAGE_NOACCESS);
    const auto changed = CachedSampledTexture(context, words);
    if (enabled) {
        Require(changed == texture && epochCount("partial") == partials + 1 && epochCount("bytes-uploaded") == uploaded + RegionBytes, "sampled CPU write did not upload just its dirty region");
        memory.Protect(RegionBytes, EpochTextureMemory::Bytes - RegionBytes, PAGE_READWRITE);
    } else Require(changed != texture, "disabled epoch textures changed snapshot rebuilds");
    texture = changed;
    if (enabled) {
        memory.Protect(0, EpochTextureMemory::Bytes, PAGE_NOACCESS);
        Require(CachedSampledTexture(context, words) == texture, "partial sampled refresh did not advance its epoch");
        memory.Protect(0, EpochTextureMemory::Bytes, PAGE_READWRITE);
    }
    expectRed(sampler.Red(texture->View(), texture->Layout(), 0, 0), 192.f / 255, "partially uploaded sampled pixel");
    expectRed(sampler.Red(texture->View(), texture->Layout(), 0, 1), 64.f / 255, "unchanged sampled pixel");
    if (auto* tracker = Get()) tracker->MarkGpuWritten({memory.Range().address, PageBytes}, Source::GpuDeclaration);
    memory.Write(0, RegionBytes, 128);
    const auto fallbacks = epochCount("fallbacks");
    const auto afterGpu = CachedSampledTexture(context, words);
    Require(afterGpu != texture, "GPU-invalidated sampled texture retained its stale image");
    if (enabled) Require(epochCount("fallbacks") > fallbacks, "GPU-pending sampled texture did not count a fallback");
    expectRed(sampler.Red(afterGpu->View(), afterGpu->Layout(), 0, 0), 128.f / 255, "GPU-invalidated sampled pixel");
    if (auto* tracker = Get()) tracker->InvalidateRange(memory.Range());
    memory.Write(RegionBytes, RegionBytes, 160);
    const auto unknown = CachedSampledTexture(context, words);
    Require(unknown != afterGpu, "unknown sampled texture did not use the legacy comparison");
    expectRed(sampler.Red(unknown->View(), unknown->Layout(), 0, 1), 160.f / 255, "unknown coverage sampled pixel");
    ClearCachedTextures(context.device);
    if (auto* tracker = Get()) tracker->RegisterRange(memory.Range(), Kind::Section, Bit(Reason::ProtectedSection));
    auto storage = StorageTexture::Create(context, *context.detiler, descriptor, 0);
    if (enabled) {
        const auto counts = epochReadBytes();
        memory.Protect(0, EpochTextureMemory::Bytes, PAGE_NOACCESS);
        Require(storage->Refresh(), "unchanged no-access storage texture was uploaded");
        Require(epochReadBytes() == counts, "epoch storage reuse issued a guest read");
        memory.Protect(0, EpochTextureMemory::Bytes, PAGE_READWRITE);
    } else Require(storage->Refresh(), "disabled epoch textures changed storage reuse");
    memory.Write(2 * RegionBytes, RegionBytes, 224);
    const auto storagePartials = epochCount("partial");
    if (enabled) {
        memory.Protect(0, 2 * RegionBytes, PAGE_NOACCESS);
        memory.Protect(3 * RegionBytes, RegionBytes, PAGE_NOACCESS);
    }
    Require(!storage->Refresh(), "storage CPU write was not uploaded");
    if (enabled) {
        Require(epochCount("partial") == storagePartials + 1, "storage CPU write did not use a partial upload");
        memory.Protect(0, EpochTextureMemory::Bytes, PAGE_READWRITE);
    }
    expectRed(sampler.Red(storage->View(), VK_IMAGE_LAYOUT_GENERAL, 0, 2), 224.f / 255, "partially uploaded storage pixel");
    expectRed(sampler.Red(storage->View(), VK_IMAGE_LAYOUT_GENERAL, 0, 1), 160.f / 255, "unchanged storage pixel");
    if (enabled) {
        memory.Protect(0, EpochTextureMemory::Bytes, PAGE_NOACCESS);
        Require(storage->Refresh(), "partial storage refresh did not advance its epoch");
        memory.Protect(0, EpochTextureMemory::Bytes, PAGE_READWRITE);
    }
    {
        CommandBatch batch(context);
        const auto commands = batch.Handle();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue clear{};
        for (auto& channel : clear.float32) channel = 0.5f;
        const VkImageSubresourceRange subresources{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 4};
        context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, storage->Image(), VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &subresources);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        batch.SubmitAndWait();
    }
    storage->MarkDirty();
    if (auto* tracker = Get()) tracker->MarkGpuWritten(memory.Range(), Source::GpuDeclaration);
    const auto gpuFallbacks = epochCount("fallbacks");
    Require(storage->Refresh(), "GPU-written storage image lost its pending results");
    if (enabled) Require(epochCount("fallbacks") > gpuFallbacks, "GPU-written storage image reused an epoch proof");
    expectRed(sampler.Red(storage->View(), VK_IMAGE_LAYOUT_GENERAL, 0, 0), 0.5f, "GPU-written storage pixel");
    recorder.Sync();
    ClearCachedTextures(context.device);
    if (!enabled) Require(TextureEpoch::ProfileLine() == countersBefore, "disabled epoch textures changed profiling output");
}
#endif

}

int main(int argc, char** argv) {
    try {
        const bool memory = argc > 1 && std::string(argv[1]) == "memory";
        const std::string mode = argc > 1 ? argv[1] : "";
        const bool epochPages = mode == "epoch-pages-linear" || mode == "epoch-pages-tiled";
        const bool epochExtended = mode == "epoch-bc" || mode == "epoch-thick" || mode == "epoch-storage" || mode == "epoch-dcc";
        const bool epochTextures = mode == "epoch-textures" || epochPages || epochExtended;
        if (mode == "epoch-metadata") {
            epochMetadataTests();
            std::puts("epoch texture metadata tests passed");
            return 0;
        }
        const bool resources = mode == "resource-admission" || mode == "resource-oversized" || mode == "resource-clear";
        const bool mirrors = mode == "mirror-storage" || mode == "mirror-snapshot" || mode == "mirror-readonly";
        const bool importDetile = mode == "import-detile";
        if (mode == "duplicates") {
            duplicateSummaryTests();
            std::puts("texture duplicate summary tests passed");
            return 0;
        }
        if (mode == "dedupe-keys") {
            dedupeKeyTests();
            std::puts("texture dedupe key tests passed");
            return 0;
        }
#ifdef _WIN32
        if (!memory && !epochTextures) _putenv_s("APS5_TEXTURE_CACHE_MIB", "1");
#else
        if (!memory && !epochTextures) setenv("APS5_TEXTURE_CACHE_MIB", "1", 1);
#endif
        std::unique_ptr<Device> device;
        try {
            device = std::make_unique<Device>(resources, mirrors || importDetile || mode == "epoch-bc");
        } catch (const std::exception& error) {
            if (std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr) throw;
            std::printf("skipped, no usable Vulkan device: %s\n", error.what());
            return VulkanTestSkipped;
        }
        auto context = device->GetContext();
#ifdef _WIN32
        if (epochExtended) {
            epochDeviceProc = context.deviceProc;
            context.deviceProc = &epochGetDeviceProc;
        }
#endif
        TextureDetiler detiler(context);
        context.detiler = &detiler;
        if (epochTextures) {
#ifdef _WIN32
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            if (mode == "epoch-bc") {
                for (const auto format : {169u, 170u, 171u, 172u, 173u, 174u, 175u, 176u, 177u, 178u, 179u, 180u, 181u, 182u})
                    for (const auto tileMode : {0u, 0x05u, 0x09u, TileR64KBX}) epochLayoutTextureTests(context, format, tileMode, false);
            } else if (mode == "epoch-thick") {
                for (const auto tileMode : {0x05u, 0x09u, 0x19u, 0x1au}) epochLayoutTextureTests(context, 56, tileMode, true);
            } else if (mode == "epoch-storage") {
                for (const auto tileMode : {0u, 0x05u, TileR64KBX}) epochLayoutTextureTests(context, 56, tileMode, false);
                epochFallbackTextureTests(context);
            } else if (mode == "epoch-dcc") epochDccFallbackTests(context);
            else if (mode == "epoch-pages-linear") epochPageTextureTests(context, true);
            else if (epochPages) for (const auto tileMode : {0x05u, 0x09u, TileR64KBX}) epochPageTextureTests(context, false, tileMode);
            else epochTextureTests(context);
            std::puts("epoch texture upload tests passed");
            return 0;
#else
            return VulkanTestSkipped;
#endif
        }
        if (mirrors) {
#ifdef _WIN32
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            if (mode == "mirror-readonly") readOnlyMirrorTextureTests(context);
            else mirrorTextureTests(context, mode == "mirror-snapshot");
            std::puts("mirror texture tests passed");
            return 0;
#else
            return VulkanTestSkipped;
#endif
        }
        if (importDetile) {
#ifdef _WIN32
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            if (!importDetileTests(context)) return VulkanTestSkipped;
            std::puts("texture import detile tests passed");
            return 0;
#else
            return VulkanTestSkipped;
#endif
        }
        if (mode == "scratch-pool") {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            scratchPoolTests(context);
            ClearCachedTextures(context.device);
            std::puts("texture scratch pool tests passed");
            return 0;
        }
        if (mode == "age") {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            ageTextureTests(context);
            std::puts("texture age tests passed");
            return 0;
        }
        if (resources) {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            if (!resourceCacheTests(context, mode)) return VulkanTestSkipped;
            std::puts("resource cache tests passed");
            return 0;
        }
        if (memory) {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            memoryTests(context);
            ClearCachedTextures(context.device);
            std::puts("sampled texture memory budget tests passed");
            return 0;
        }
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            reportedBudgetTests(context);
            snapshotTests(context);
            heldTests(context);
            releasedTests(context);
            rescanTests(context);
            viewEvictionTests(context);
            viewTests(context);
            frameTests(context);
            earlyMissTests(context);
            collidingKeyTests(context);
            ClearCachedTextures(context.device);
        }
        std::puts("sampled texture cache budget tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
