#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/GatherShadow.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GpuAuthorityCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/ThreadScratch.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Execution/include/Stage4Diag.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetTransfer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GpuColorTransfer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ImageEpochs.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MemoryAccounting.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"
#include "prx/libc/include/RunReport.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ParameterArena.hpp"
#include "prx/libSceAgcDriver/Graphics/include/StagedMerge.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/StructCensus.hpp"
#include "prx/libSceAgcDriver/Graphics/include/SampleOrder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "ThreadOwned.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "Optimization/include/Optimization/ShaderStageInputInfo.hpp"
#include "SpirvBackend/SpirvAnalysis.hpp"
#include "ThreadOwned.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <algorithm>
#include <bit>
#include <exception>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace AgcDriver::GuestMemory {
void PrepareDriverRead_nid_no_patch(std::uint64_t address, std::size_t bytes);
}

namespace AgcDriver::Graphics {

void WriteCheckpointMarker(const Context& context, VkCommandBuffer commands, std::uint64_t serial, std::uint32_t queue);
void WriteNvCheckpointToken(const Context& context, VkCommandBuffer commands, std::uint64_t token, std::uint32_t queue);

namespace {
enum class VkDrawKind : std::size_t { Direct, Indexed, Indirect, Mesh, Count };
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(VkDrawKind::Count)> vkDrawCounts{};
void CountVkDraw(VkDrawKind kind) {
    vkDrawCounts[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
}


bool DrawFaultTokens() {
    static const bool enabled = std::getenv("APS5_GPU_CHECKPOINTS") != nullptr;
    return enabled;
}

void noteDrawFaultToken(const Context& context, Recorder& recorder, VkCommandBuffer commands, std::span<const CompiledShader> shaders, const Pm4::DrawParameters& draw, std::uint32_t stage, const Pm4::DrawParameters::IndirectDraw* args) {
    try {
        if (!DrawFaultTokens()) return;
        const auto queue = GuestMemory::GpuLockThreadTag();
        if (queue != 0) return;
        const auto faultToken = AgcDriver::FaultTokenRing::Instance().Claim();
        if (faultToken == 0) return;
        if (auto* entry = AgcDriver::FaultTokenRing::Instance().Mutable(faultToken)) {
            entry->submitSerial = recorder.OpenSerial();
            entry->programAddress = 0;
            std::uint64_t variant = 0;
            for (const auto& shader : shaders) {
                if (shader.program != nullptr && (variant == 0 || shader.stage == ShaderRecompiler::ShaderStage::Fragment)) variant = shader.program->variantId;
            }
            entry->variantId = variant;
            entry->stage = stage;
            entry->argBuffer = 0;
            entry->argOffset = 0;
            entry->argStride = 0;
            entry->argCount = 0;
            for (auto& word : entry->argWords) word = 0;
            if (args != nullptr) {
                entry->argBuffer = args->arguments;
                entry->argOffset = 0;
                entry->argStride = args->stride;
                try {
                    constexpr std::uint32_t argWordLimit = 4;
                    const auto first = Pm4::ReadDrawArgumentsUnsynced(*args, 0);
                    entry->argWords[0] = first.count;
                    entry->argWords[1] = first.instances;
                    entry->argWords[2] = first.firstVertexOrIndex;
                    entry->argWords[3] = draw.indexed ? first.vertexOffset : first.firstInstance;
                    entry->argCount = argWordLimit;
                } catch (...) {
                    entry->argCount = 0;
                }
            }
            std::snprintf(entry->desc, sizeof(entry->desc), "draw%s%s c=%u i=%u", draw.indexed ? " indexed" : "", args != nullptr ? " indirect" : "", draw.indexCount, draw.instanceCount);
            recorder.NoteFaultToken(faultToken);
            std::atomic_ref<AgcDriver::FaultToken>(entry->token).store(faultToken, std::memory_order_release);
        }
        WriteCheckpointMarker(context, commands, faultToken, queue);
        WriteNvCheckpointToken(context, commands, faultToken, queue);
    } catch (...) {
    }
}

struct PixelFunctionLdsPool {
    Context context{};
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetLayout empty = VK_NULL_HANDLE;
    VkDescriptorSetLayout bufferLayout = VK_NULL_HANDLE;
    std::uint32_t* header = nullptr;
    std::uint32_t subgroupSize = 0;
    std::uint32_t slots = 0;
    bool announced = false;
};

std::mutex& PixelLdsMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<VkDevice, PixelFunctionLdsPool>& PixelLdsPools() {
    static std::unordered_map<VkDevice, PixelFunctionLdsPool> pools;
    return pools;
}

struct MeshWindowPool {
    Context context{};
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
};

std::mutex& MeshWindowMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<VkDevice, MeshWindowPool>& MeshWindowPools() {
    static std::unordered_map<VkDevice, MeshWindowPool> pools;
    return pools;
}

void DestroyMeshWindow(MeshWindowPool& pool) {
    if (pool.descriptorPool != VK_NULL_HANDLE) pool.context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(pool.context.device, pool.descriptorPool, nullptr);
    if (pool.buffer != VK_NULL_HANDLE) pool.context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(pool.context.device, pool.buffer, nullptr);
    if (pool.memory != VK_NULL_HANDLE) MemoryAccounting::Free(pool.context, pool.memory);
    if (pool.layout != VK_NULL_HANDLE) pool.context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(pool.context.device, pool.layout, nullptr);
    pool = MeshWindowPool{};
}

void DestroyPixelFunctionLds(PixelFunctionLdsPool& pool) {
    if (pool.header != nullptr) pool.context.Function<PFN_vkUnmapMemory>("vkUnmapMemory")(pool.context.device, pool.memory);
    if (pool.descriptorPool != VK_NULL_HANDLE) pool.context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(pool.context.device, pool.descriptorPool, nullptr);
    if (pool.buffer != VK_NULL_HANDLE) pool.context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(pool.context.device, pool.buffer, nullptr);
    if (pool.memory != VK_NULL_HANDLE) MemoryAccounting::Free(pool.context, pool.memory);
    if (pool.bufferLayout != VK_NULL_HANDLE) pool.context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(pool.context.device, pool.bufferLayout, nullptr);
    if (pool.empty != VK_NULL_HANDLE) pool.context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(pool.context.device, pool.empty, nullptr);
    pool.header = nullptr;
    pool.descriptorPool = VK_NULL_HANDLE;
    pool.set = VK_NULL_HANDLE;
    pool.buffer = VK_NULL_HANDLE;
    pool.memory = VK_NULL_HANDLE;
    pool.bufferLayout = VK_NULL_HANDLE;
    pool.empty = VK_NULL_HANDLE;
}

}

PixelFunctionLdsLayouts PixelFunctionLdsSetLayouts(const Context& context) {
    std::lock_guard lock(PixelLdsMutex());
    auto& pool = PixelLdsPools()[context.device];
    if (pool.bufferLayout == VK_NULL_HANDLE) {
        pool.context = context;
        VkDescriptorSetLayoutCreateInfo emptyInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &emptyInfo, nullptr, &pool.empty), "vkCreateDescriptorSetLayout pixel function LDS empty");
        const VkDescriptorSetLayoutBinding binding{0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 1u;
        info.pBindings = &binding;
        Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &info, nullptr, &pool.bufferLayout), "vkCreateDescriptorSetLayout pixel function LDS");
    }
    return {pool.empty, pool.bufferLayout};
}

void PreparePixelFunctionLds(const Context& context, VkCommandBuffer commands, std::uint32_t subgroupSize) {
    const auto operations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT;
    Require((context.subgroup.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0 && (context.subgroup.supportedOperations & operations) == operations, "pixel function LDS needs fragment subgroup elect, ballot and broadcast");
    Require(subgroupSize != 0u && subgroupSize == context.subgroup.subgroupSize, "pixel function LDS subgroup size does not match the device");
    std::lock_guard lock(PixelLdsMutex());
    auto& pool = PixelLdsPools()[context.device];
    Require(pool.bufferLayout != VK_NULL_HANDLE, "pixel function LDS descriptor set layout was not created");
    if (pool.buffer == VK_NULL_HANDLE) {
        const auto stride = static_cast<std::uint64_t>(subgroupSize) * ShaderRecompiler::FunctionLdsDwordLimit * 4ull;
        const auto budget = std::min<std::uint64_t>(context.limits.maxStorageBufferRange, 2ull << 30);
        Require(budget > 16ull + stride, "pixel function LDS does not fit in the device storage buffer range");
        pool.slots = static_cast<std::uint32_t>((budget - 16ull) / stride);
        pool.subgroupSize = subgroupSize;
        const auto bytes = 16ull + stride * pool.slots;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &bufferInfo, nullptr, &pool.buffer), "vkCreateBuffer pixel function LDS");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, pool.buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        constexpr auto hostCoherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        constexpr auto deviceLocalHost = hostCoherent | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        std::optional<std::uint32_t> deviceLocalType;
        std::optional<std::uint32_t> hostOnlyType;
        for (std::uint32_t i = 0; i < context.memory.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) == 0) continue;
            const auto flags = context.memory.memoryTypes[i].propertyFlags;
            const auto heap = context.memory.memoryTypes[i].heapIndex;
            if (!deviceLocalType && (flags & deviceLocalHost) == deviceLocalHost && context.memory.memoryHeaps[heap].size >= requirements.size) {
                deviceLocalType = i;
            }
            if (!hostOnlyType && (flags & hostCoherent) == hostCoherent && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) {
                hostOnlyType = i;
            }
        }
        const auto hostType = hostOnlyType.value_or(context.MemoryType(requirements.memoryTypeBits, hostCoherent));
        allocation.memoryTypeIndex = deviceLocalType.value_or(hostType);
        auto allocated = MemoryAccounting::Allocate(context, allocation, "pixel function LDS", &pool.memory);
        if (allocated != VK_SUCCESS && deviceLocalType && allocation.memoryTypeIndex == *deviceLocalType && hostType != *deviceLocalType) {
            allocation.memoryTypeIndex = hostType;
            allocated = MemoryAccounting::Allocate(context, allocation, "pixel function LDS", &pool.memory);
        }
        if (allocated != VK_SUCCESS) {
            context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, pool.buffer, nullptr);
            pool.buffer = VK_NULL_HANDLE;
            pool.memory = VK_NULL_HANDLE;
            Check(allocated, "vkAllocateMemory pixel function LDS");
        }
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, pool.buffer, pool.memory, 0), "vkBindBufferMemory pixel function LDS");
        Check(context.Function<PFN_vkMapMemory>("vkMapMemory")(context.device, pool.memory, 0, 16, 0, reinterpret_cast<void**>(&pool.header)), "vkMapMemory pixel function LDS");
        pool.header[0] = 0u;
        pool.header[1] = 0u;
        pool.header[2] = pool.slots;
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1u;
        poolInfo.poolSizeCount = 1u;
        poolInfo.pPoolSizes = &size;
        Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool.descriptorPool), "vkCreateDescriptorPool pixel function LDS");
        VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sets.descriptorPool = pool.descriptorPool;
        sets.descriptorSetCount = 1u;
        sets.pSetLayouts = &pool.bufferLayout;
        Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &sets, &pool.set), "vkAllocateDescriptorSets pixel function LDS");
        const VkDescriptorBufferInfo buffer{pool.buffer, 0, bytes};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = pool.set;
        write.dstBinding = 0u;
        write.descriptorCount = 1u;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &buffer;
        context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, 1u, &write, 0u, nullptr);
        std::fprintf(stderr, "[gpu] pixel function LDS pool: %u subgroups, %llu MiB, stride %u lanes\n", pool.slots, static_cast<unsigned long long>(bytes >> 20u), subgroupSize);
        pool.announced = true;
    }
    Require(pool.subgroupSize == subgroupSize, "pixel function LDS subgroup size does not match the pool");
    const VkMemoryBarrier beforeFill{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &beforeFill, 0, nullptr, 0, nullptr);
    context.Resolved(&DeviceFunctions::cmdFillBuffer, "vkCmdFillBuffer")(commands, pool.buffer, 0, 4, 0u);
    const VkMemoryBarrier afterFill{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &afterFill, 0, nullptr, 0, nullptr);
}

bool UsesMeshWindow(std::span<const CompiledShader> shaders) {
    return std::ranges::any_of(shaders, [](const CompiledShader& shader) { return shader.program != nullptr && shader.program->meshWindowBuffer; });
}

VkDescriptorSetLayout MeshWindowSetLayout(const Context& context) {
    std::lock_guard lock(MeshWindowMutex());
    auto& pool = MeshWindowPools()[context.device];
    if (pool.layout == VK_NULL_HANDLE) {
        pool.context = context;
        const VkDescriptorSetLayoutBinding binding{0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr};
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 1u;
        info.pBindings = &binding;
        Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &info, nullptr, &pool.layout), "vkCreateDescriptorSetLayout mesh window");
    }
    return pool.layout;
}

void PrepareMeshWindow(const Context& context) {
    std::lock_guard lock(MeshWindowMutex());
    auto& pool = MeshWindowPools()[context.device];
    Require(pool.layout != VK_NULL_HANDLE, "mesh window descriptor set layout was not created");
    if (pool.set != VK_NULL_HANDLE) return;
    try {
        const auto bytes = (static_cast<std::uint64_t>(ShaderRecompiler::MeshWindowHeaderDwords) + static_cast<std::uint64_t>(ShaderRecompiler::MeshWindowSlots) * ShaderRecompiler::MeshWindowSlotDwords) * 4ull;
        Require(bytes <= context.limits.maxStorageBufferRange, "mesh window pool does not fit in the device storage buffer range");
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &bufferInfo, nullptr, &pool.buffer), "vkCreateBuffer mesh window");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, pool.buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(MemoryAccounting::Allocate(context, allocation, "mesh window", &pool.memory), "vkAllocateMemory mesh window");
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, pool.buffer, pool.memory, 0), "vkBindBufferMemory mesh window");
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1u;
        poolInfo.poolSizeCount = 1u;
        poolInfo.pPoolSizes = &size;
        Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool.descriptorPool), "vkCreateDescriptorPool mesh window");
        VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sets.descriptorPool = pool.descriptorPool;
        sets.descriptorSetCount = 1u;
        sets.pSetLayouts = &pool.layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &sets, &set), "vkAllocateDescriptorSets mesh window");
        const VkDescriptorBufferInfo buffer{pool.buffer, 0, bytes};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = 0u;
        write.descriptorCount = 1u;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &buffer;
        context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, 1u, &write, 0u, nullptr);
        CommandBatch batch(context);
        const auto commands = batch.Handle();
        context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, pool.buffer, 0, ShaderRecompiler::MeshWindowHeaderDwords * 4u, 0u);
        const VkMemoryBarrier afterFill{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &afterFill, 0, nullptr, 0, nullptr);
        batch.SubmitAndWait();
        pool.set = set;
        std::fprintf(stderr, "[gpu] mesh window pool: %u workgroup slots, %llu MiB\n", ShaderRecompiler::MeshWindowSlots, static_cast<unsigned long long>(bytes >> 20u));
    } catch (...) {
        const auto layout = pool.layout;
        pool.layout = VK_NULL_HANDLE;
        DestroyMeshWindow(pool);
        pool.context = context;
        pool.layout = layout;
        throw;
    }
}

void BindMeshWindow(const Context& context, VkCommandBuffer commands, VkPipelineLayout layout) {
    std::lock_guard lock(MeshWindowMutex());
    const auto& pool = MeshWindowPools().at(context.device);
    Require(pool.set != VK_NULL_HANDLE, "mesh window buffer was not prepared");
    context.Resolved(&DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, ShaderRecompiler::MeshWindowDescriptorSet, 1, &pool.set, 0, nullptr);
}

void ReleaseMeshWindow(VkDevice device) {
    std::lock_guard lock(MeshWindowMutex());
    const auto found = MeshWindowPools().find(device);
    if (found == MeshWindowPools().end()) return;
    DestroyMeshWindow(found->second);
    MeshWindowPools().erase(found);
}

std::uint32_t PixelFunctionLdsSubgroup(std::span<const CompiledShader> shaders) {
    std::uint32_t subgroup = 0u;
    for (const auto& shader : shaders) {
        if (shader.program != nullptr && shader.program->functionLdsBuffer) subgroup = shader.program->hostSubgroupSize;
    }
    return subgroup;
}

void BindPixelFunctionLds(const Context& context, VkCommandBuffer commands, VkPipelineLayout layout) {
    std::lock_guard lock(PixelLdsMutex());
    const auto& pool = PixelLdsPools().at(context.device);
    Require(pool.set != VK_NULL_HANDLE, "pixel function LDS buffer was not prepared");
    context.Resolved(&DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, ShaderRecompiler::PixelFunctionLdsDescriptorSet, 1, &pool.set, 0, nullptr);
}

void CheckPixelFunctionLds(const Context& context) {
    std::lock_guard lock(PixelLdsMutex());
    const auto found = PixelLdsPools().find(context.device);
    if (found == PixelLdsPools().end() || found->second.header == nullptr) return;
    const auto exhausted = found->second.header[1] != 0u;
    found->second.header[1] = 0u;
    if (exhausted) throw std::runtime_error("AGC graphics: pixel function LDS subgroup slots exhausted");
}

void ReleasePixelFunctionLds(VkDevice device) {
    std::lock_guard lock(PixelLdsMutex());
    const auto found = PixelLdsPools().find(device);
    if (found == PixelLdsPools().end()) return;
    DestroyPixelFunctionLds(found->second);
    PixelLdsPools().erase(found);
}

std::uint64_t DrawRenderPassKey(const Context& context, const State& state, std::span<const VkImageView> targetViews) {
    std::uint64_t key = 14695981039346656037ull;
    const auto mix = [&](std::uint64_t value) {
        key ^= value;
        key *= 1099511628211ull;
    };
    for (const auto view : targetViews) mix(reinterpret_cast<std::uint64_t>(view));
    if (state.blends.size() != state.colors.size()) {
        mix(state.blends.size());
        for (const auto& color : state.colors) mix(color.exportIndex);
    }
    for (const auto& color : state.colors) {
        if (color.partialWriteMask != 0u) mix(0x6d65726765000000ull | color.exportIndex);
    }
    mix(state.renderExtent.width);
    mix(state.renderExtent.height);
    mix(state.samples);
    mix(state.mergedCoverage);
    mix(state.truncatedCoverage);
    mix(state.customSampleLocations);
    if (state.customSampleLocations) {
        mix(state.sampleLocationGrid);
        for (const auto location : state.sampleLocations) mix(location);
    }
    if (!context.provokingVertexModePerPipeline) mix(state.provokingVertexMode);
    if (state.rasterizerDiscard) mix(0x6469736361726400ull);
    return key;
}

namespace {

// APS5_PROFILE_GPU: the [gputime-passes] key of a draw pass, by its targets' guest addresses,
// formats and extents (stable across frames, unlike the views DrawRenderPassKey mixes), described
// on first sight: "pass WxH s<samples> L<layers> c<format>@<address> WxH ... d<format>@<address>".
std::uint64_t timingPassKey(const State& state) {
    std::uint64_t key = 14695981039346656037ull;
    const auto mix = [&](std::uint64_t value) {
        key ^= value;
        key *= 1099511628211ull;
    };
    const auto extent = [](VkExtent2D size) { return static_cast<std::uint64_t>(size.width) | (static_cast<std::uint64_t>(size.height) << 32u); };
    for (const auto& color : state.colors) {
        mix(color.address);
        mix(static_cast<std::uint64_t>(color.format));
        mix(extent(color.extent));
        mix(color.arraySlice | (static_cast<std::uint64_t>(color.mip) << 32u));
    }
    if (state.depth.has_value()) {
        mix(0xde97ull);
        mix(state.depth->address);
        mix(static_cast<std::uint64_t>(state.depth->format));
        mix(extent(state.depth->extent));
        mix(state.depth->firstSlice);
    }
    mix(extent(state.renderExtent));
    mix(state.samples);
    mix(state.renderLayers);
    const auto timingKey = Recorder::PassTimingKey(key);
    if (Recorder::NoteTimingKey(timingKey)) {
        char text[192];
        std::snprintf(text, sizeof(text), "pass %ux%u s%u L%u", state.renderExtent.width, state.renderExtent.height, static_cast<unsigned>(state.samples), state.renderLayers);
        std::string description = text;
        for (const auto& color : state.colors) {
            std::snprintf(text, sizeof(text), " c%d@0x%llx %ux%u", static_cast<int>(color.format), static_cast<unsigned long long>(color.address), color.extent.width, color.extent.height);
            description += text;
        }
        if (state.depth.has_value()) {
            std::snprintf(text, sizeof(text), " d%d@0x%llx %ux%u%s%s", static_cast<int>(state.depth->format), static_cast<unsigned long long>(state.depth->address), state.depth->extent.width, state.depth->extent.height, state.depthTest ? " test" : "", state.depthWrite ? " write" : "");
            description += text;
        }
        Recorder::DescribeTimingKey(timingKey, std::move(description));
    }
    return timingKey;
}

const char* timingStageName(ShaderRecompiler::ShaderStage stage) {
    switch (stage) {
        case ShaderRecompiler::ShaderStage::Compute: return "cs";
        case ShaderRecompiler::ShaderStage::Vertex: return "vs";
        case ShaderRecompiler::ShaderStage::TessellationControl: return "hs";
        case ShaderRecompiler::ShaderStage::TessellationEvaluation: return "ds";
        case ShaderRecompiler::ShaderStage::Geometry: return "gs";
        case ShaderRecompiler::ShaderStage::Fragment: return "ps";
        case ShaderRecompiler::ShaderStage::Local: return "ls";
        case ShaderRecompiler::ShaderStage::Mesh: return "ms";
    }
    return "xs";
}

// APS5_PROFILE_GPU_DRAWS: a draw's [gputime-top] key, by its stages' pipeline variants, described
// (stage variants, SPIR-V words, the pass) and its SPIR-V dumped on first sight.
std::uint64_t timingDrawKey(std::span<const CompiledShader> shaders, std::uint64_t passKey) {
    std::uint64_t key = 14695981039346656037ull;
    for (const auto& shader : shaders) {
        if (shader.program == nullptr) continue;
        key ^= shader.program->PipelineVariantId() + static_cast<std::uint64_t>(shader.stage);
        key *= 1099511628211ull;
    }
    const auto timingKey = Recorder::PipelineTimingKey(key);
    if (Recorder::NoteTimingKey(timingKey)) {
        std::string description = "draw";
        char text[128];
        for (const auto& shader : shaders) {
            if (shader.program == nullptr) continue;
            std::snprintf(text, sizeof(text), " %s:%llx/%zuw", timingStageName(shader.stage), static_cast<unsigned long long>(shader.program->PipelineVariantId()), shader.program->spirv.size());
            description += text;
            Recorder::DumpTimingSpirv(timingKey, timingStageName(shader.stage), shader.program->spirv.Words());
        }
        std::snprintf(text, sizeof(text), " in 0x%llx", static_cast<unsigned long long>(passKey));
        description += text;
        Recorder::DescribeTimingKey(timingKey, std::move(description));
    }
    return timingKey;
}

}

namespace {

bool writesStencilTarget(const State& state) {
    const auto keeps = [](const VkStencilOpState& face) {
        return face.writeMask == 0u || (face.failOp == VK_STENCIL_OP_KEEP && face.passOp == VK_STENCIL_OP_KEEP && face.depthFailOp == VK_STENCIL_OP_KEEP);
    };
    return state.stencilTest && (!keeps(state.stencilFront) || !keeps(state.stencilBack));
}

bool writesDepthTarget(const State& state) {
    return state.depthWrite || writesStencilTarget(state);
}

std::size_t colorKeyCount(const ColorTarget& color, std::size_t bytes) {
    if (color.mipCount != 1 || color.depth != 1 || !color.dccPipeAligned) return DccKeyBytes(bytes);
    return DccKeyCount(ColorTextureTileMode(color.tileMode), color.elementBytes, color.extent.width, color.extent.height, bytes);
}

std::array<std::byte, 16> clearTexel(const ColorTarget& color, DccKeys keys) {
    std::array<std::byte, 16> texel{};
    const auto elementBytes = static_cast<std::size_t>(color.elementBytes);
    Require(elementBytes != 0 && elementBytes <= texel.size(), "unexpected color element size");
    if (keys == DccKeys::ClearRegister) {
        Require(elementBytes <= sizeof(color.clearWords), "the DCC register clear of a texel over 64 bits is not modeled");
        std::memcpy(texel.data(), color.clearWords.data(), elementBytes);
    } else {
        Require(FillDccClear(color.format, keys, color.dccAlphaOnMsb, std::span(texel.data(), elementBytes)), "the target's format has no encoding of its DCC clear code");
    }
    return texel;
}

bool clearToTexel(StorageTexture& image, const std::array<std::byte, 16>& texel, std::uint32_t elementBytes, const char*& refusal, bool allowRegisterClear = false) {
    std::array<std::byte, 16> repeated{};
    for (std::size_t offset = 0; offset + elementBytes <= repeated.size(); offset += elementBytes) std::memcpy(repeated.data() + offset, texel.data(), elementBytes);
    std::array<std::uint32_t, 4> pattern{};
    std::memcpy(pattern.data(), repeated.data(), repeated.size());
    return image.FillClear(std::span<const std::uint32_t, 4>(pattern), StorageTexture::WholeImage, refusal, allowRegisterClear);
}

void writeTexels(const ColorTarget& color, const std::array<std::byte, 16>& texel) {
    const auto elementBytes = static_cast<std::size_t>(color.elementBytes);
    std::vector<std::byte> texels(color.bytes);
    for (std::size_t offset = 0; offset + elementBytes <= texels.size(); offset += elementBytes) std::memcpy(texels.data() + offset, texel.data(), elementBytes);
    GuestMemory::Write(color.address, texels);
}

void storeClearTexels(const Context& context, const ColorTarget& color, const std::array<std::byte, 16>& texel) {
    StorageTexture::FlushPending(color.address, color.bytes, nullptr, "fast-clear materialization");
    const auto keys = ReadDccKeys(color.dccAddress, color.bytes);
    if (!IsDccClear(keys)) return;
    writeTexels(color, keys == DccKeys::ClearRegister ? texel : clearTexel(color, keys));
    MarkDccUncompressed(context, color.dccAddress, color.bytes, colorKeyCount(color, color.bytes));
}

void storeSingleTexels(const Context& context, const ColorTarget& color) {
    Require(!ColorTileModeIsLinear(color.tileMode), "comp-to-single DCC keys of a linear color target");
    const ColorTargetLayout surface(color.extent.width, color.extent.height, color.tileMode, color.elementBytes, color.pipeBankXor);
    Require(surface.Bytes() == color.bytes, "comp-to-single DCC keys over a color target whose layout differs from its surface");
    StorageTexture::FlushPending(color.address, color.bytes, nullptr, "comp-to-single expansion");
    std::vector<std::byte> texels(color.bytes);
    GuestMemory::Read(color.address, texels, surface.Alignment());
    constexpr std::size_t BlockBytes = 256;
    for (std::uint32_t y = 0; y < color.extent.height; ++y) {
        for (std::uint32_t x = 0; x < color.extent.width; ++x) {
            const auto offset = surface.Offset(x, y);
            const auto first = offset / BlockBytes * BlockBytes;
            if (offset != first) std::memcpy(texels.data() + offset, texels.data() + first, color.elementBytes);
        }
    }
    GuestMemory::Write(color.address, texels, surface.Alignment());
    MarkDccUncompressed(context, color.dccAddress, color.bytes, colorKeyCount(color, color.bytes));
}

void materializeRegisterClear(const Context& context, const ColorTarget& color, StorageTexture& resident) {
    if (color.dccAddress == 0 || resident.Descriptor().dccAddress != color.dccAddress) return;
    if (ProvedCurrentDccKeys(color.dccAddress, color.bytes, resident.TargetKeyProof()) != DccKeys::ClearRegister) return;
    const auto texel = clearTexel(color, DccKeys::ClearRegister);
    const char* refusal = nullptr;
    const bool pendingKeysKnown = KnownPendingDccKeys(color.dccAddress, color.bytes) == DccKeys::ClearRegister;
    bool cleared = clearToTexel(resident, texel, color.elementBytes, refusal, pendingKeysKnown);
    const auto keyBytes = static_cast<std::size_t>(color.bytes / 256);
    if (!cleared && keyBytes != 0) {
        if (auto* recorder = Recorder::Active(); recorder != nullptr && recorder->PendingWriteOverlaps(color.dccAddress, keyBytes)) {
            Recorder::CountSync(2);
            recorder->SyncThrough(color.dccAddress, keyBytes);
            if (CurrentDccKeys(color.dccAddress, color.bytes) != DccKeys::ClearRegister) return;
            cleared = clearToTexel(resident, texel, color.elementBytes, refusal);
        }
    }
    if (cleared) {
        MarkDccUncompressed(context, color.dccAddress, color.bytes, colorKeyCount(color, color.bytes));
        return;
    }
    storeClearTexels(context, color, texel);
    resident.Refresh();
}

enum class CmaskState { Expanded, Cleared, Partial };

struct CmaskProof {
    std::size_t bytes = 0;
    std::uint64_t generation = 0;
};

AgcDriver::Mutex& cmaskProofMutex() {
    static AgcDriver::Mutex mutex;
    return mutex;
}

std::unordered_map<std::uint64_t, CmaskProof>& cmaskProofs() {
    static std::unordered_map<std::uint64_t, CmaskProof> proofs;
    return proofs;
}

bool cmaskProved(std::uint64_t address, std::size_t bytes) {
    if (GuestMemory::CollectWrites(address, bytes) == 0) return false;
    std::lock_guard lock(cmaskProofMutex());
    const auto found = cmaskProofs().find(address);
    return found != cmaskProofs().end() && found->second.bytes == bytes && !GuestMemory::StoredOver(address, bytes, found->second.generation);
}

void proveCmask(std::uint64_t address, std::size_t bytes, std::uint64_t generation) {
    std::lock_guard lock(cmaskProofMutex());
    if (generation == 0) cmaskProofs().erase(address);
    else cmaskProofs()[address] = {bytes, generation};
}

void requireCmaskCodes(std::uint32_t codes) {
    Require((codes & 1u) == 0, "a CMASK fast clear of a DCC color target, whose clear belongs in its DCC keys, was recorded");
}

CmaskState readCmask(const ColorTarget& color, const CmaskLayout& layout, std::vector<std::uint32_t>& cleared) {
    const auto address = color.cmaskAddress;
    const auto bytes = layout.Bytes();
    const bool locked = GuestMemory::GpuMutex().HeldByThisThread();
    auto* recorder = Recorder::Active();
    if (locked && recorder != nullptr && recorder->PendingWriteOverlaps(address, bytes)) {
        Recorder::CountSync(2);
        recorder->SyncThrough(address, bytes);
    }
    if (AnyShadowedOverlaps(address, bytes)) {
        Require(locked, "a CMASK under a unit shadow must be read under the device lock");
        PublishShadow(address, bytes, PublishScope::Whole, PublishReason::Keys);
        if (recorder != nullptr) {
            Recorder::CountSync(2);
            recorder->Sync();
        }
    }
    std::vector<std::byte> mask(bytes);
    GuestMemory::Read(address, mask, CmaskLayout::Alignment);
    const auto uniform = [&](std::byte value) { return std::all_of(mask.begin(), mask.end(), [&](std::byte entry) { return entry == value; }); };
    if (color.coverageSamples > color.samples) {
        for (std::uint32_t tileY = 0; tileY < layout.TilesY(); ++tileY) {
            for (std::uint32_t tileX = 0; tileX < layout.TilesX(); ++tileX) {
                const auto nibble = layout.Nibble(tileX, tileY);
                const auto code = (std::to_integer<std::uint32_t>(mask[nibble / 2u]) >> ((nibble % 2u) * 4u)) & 0xfu;
                if (code != 0u && (code >> 2u) != 3u) {
                    char text[128];
                    std::snprintf(text, sizeof(text), "AGC graphics: CMASK code 0x%x of a one-fragment color target is not implemented", code);
                    throw std::runtime_error(text);
                }
            }
        }
    }
    if (uniform(std::byte{0xff})) return CmaskState::Expanded;
    if (uniform(std::byte{0})) return CmaskState::Cleared;
    for (std::uint32_t tileY = 0; tileY < layout.TilesY(); ++tileY) {
        for (std::uint32_t tileX = 0; tileX < layout.TilesX(); ++tileX) {
            const auto nibble = layout.Nibble(tileX, tileY);
            const auto code = (std::to_integer<std::uint32_t>(mask[nibble / 2u]) >> ((nibble % 2u) * 4u)) & 0xfu;
            if (code == 0u) cleared.push_back(tileY * layout.TilesX() + tileX);
        }
    }
    if (cleared.empty()) return CmaskState::Expanded;
    if (cleared.size() == static_cast<std::size_t>(layout.TilesX()) * layout.TilesY()) return CmaskState::Cleared;
    return CmaskState::Partial;
}

void storeCmaskClearTexels(const ColorTarget& color, const CmaskLayout& layout, const std::vector<std::uint32_t>* tiles, const std::array<std::byte, 16>& texel) {
    StorageTexture::FlushPending(color.address, color.bytes, nullptr, "fast-clear materialization");
    const ColorTargetLayout surface(color.extent.width, color.extent.height, color.tileMode, color.elementBytes);
    Require(surface.Bytes() == color.bytes, "CMASK fast clear over a color target whose layout differs from its surface");
    std::vector<std::byte> texels(surface.Bytes());
    GuestMemory::Read(color.address, texels, surface.Alignment());
    const auto fill = [&](std::uint32_t tile) {
        const auto x0 = (tile % layout.TilesX()) * 8u;
        const auto y0 = (tile / layout.TilesX()) * 8u;
        for (auto y = y0; y < std::min(y0 + 8u, color.extent.height); ++y) {
            for (auto x = x0; x < std::min(x0 + 8u, color.extent.width); ++x) std::memcpy(texels.data() + surface.Offset(x, y), texel.data(), color.elementBytes);
        }
    };
    if (tiles != nullptr) {
        for (const auto tile : *tiles) fill(tile);
    } else {
        for (std::uint32_t tile = 0; tile < layout.TilesX() * layout.TilesY(); ++tile) fill(tile);
    }
    GuestMemory::Write(color.address, texels, surface.Alignment());
}

void recordCmaskClear(const Context& context, Recorder& recorder, const HostImport& import, const ColorTarget& color, const CmaskLayout& layout, const std::shared_ptr<StorageTexture>& resident, VkImageView view) {
    const auto address = color.cmaskAddress;
    const auto bytes = layout.Bytes();
    StorageTexture::FlushPending(address, bytes, nullptr, "fast-clear materialization");
    if (AnyShadowedOverlaps(address, bytes)) PublishShadow(address, bytes, PublishScope::Whole, PublishReason::Keys);
    recorder.FlushKeyStoresOverlapping(address, bytes);
    const auto commands = recorder.Commands();
    if (resident != nullptr && !resident->Cached()) recorder.KeepBytes(resident.get(), static_cast<std::size_t>(resident->BudgetBytes()));
    recorder.Keep(resident);
    context.detiler->DispatchCmaskClear(commands, import.buffer, HostImportOffset(import, address), bytes, view, color.extent.width, color.extent.height, color.elementBytes, color.clearWords, color.dccAddress == 0);
    recorder.NotePendingWrite(address, bytes);
    proveCmask(address, bytes, GuestMemory::MarkWritten(address, bytes));
}

bool materializeCmaskClear(const Context& context, const ColorTarget& color, const std::shared_ptr<StorageTexture>& resident) {
    if (color.cmaskAddress == 0) return false;
    if (context.detiler != nullptr) requireCmaskCodes(context.detiler->CmaskErrors());
    const CmaskLayout layout(color.extent.width, color.extent.height);
    const auto bytes = layout.Bytes();
    Require(color.cmaskBytes == bytes, "CMASK size differs from the color target's CMASK layout");
    if (cmaskProved(color.cmaskAddress, bytes)) return false;
    if (color.coverageSamples > color.samples) {
        std::vector<std::uint32_t> codes;
        static_cast<void>(readCmask(color, layout, codes));
    }
    auto* recorder = Recorder::Active();
    if (resident != nullptr && context.detiler != nullptr && recorder != nullptr && GuestMemory::GpuMutex().HeldByThisThread()) {
        const auto* import = HostImportFor(context, color.cmaskAddress, bytes);
        const auto view = import != nullptr ? resident->ElementView() : VK_NULL_HANDLE;
        if (view != VK_NULL_HANDLE) {
            recordCmaskClear(context, *recorder, *import, color, layout, resident, view);
            return true;
        }
    }
    const auto collected = GuestMemory::CollectWrites(color.cmaskAddress, bytes);
    std::vector<std::uint32_t> cleared;
    const auto state = readCmask(color, layout, cleared);
    if (state == CmaskState::Expanded) {
        proveCmask(color.cmaskAddress, bytes, collected);
        return false;
    }
    Require(color.dccAddress == 0, "CMASK fast clear of a DCC color target, whose clear belongs in its DCC keys");
    const auto texel = clearTexel(color, DccKeys::ClearRegister);
    const char* refusal = nullptr;
    const bool filled = state == CmaskState::Cleared && color.mipCount == 1 && resident != nullptr && clearToTexel(*resident, texel, color.elementBytes, refusal);
    if (!filled) {
        storeCmaskClearTexels(color, layout, state == CmaskState::Cleared ? nullptr : &cleared, texel);
        if (resident != nullptr) resident->Refresh();
    }
    const std::vector<std::byte> expanded(bytes, std::byte{0xff});
    GuestMemory::Write(color.cmaskAddress, expanded, CmaskLayout::Alignment);
    proveCmask(color.cmaskAddress, bytes, GuestMemory::CollectWrites(color.cmaskAddress, bytes));
    return filled;
}

void imageBarrier(const Context& context, VkCommandBuffer commands, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess, VkImageSubresourceRange subresources = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = subresources;
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void memoryBarrier(const Context& context, VkCommandBuffer commands, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) {
    const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, sourceAccess, destinationAccess};
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, sourceStage, destinationStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

// The color surface as one untailed SW_64KB_R_X mip for the GPU detiler, with tightly packed linear rows.
TileMipLayout ColorTargetMip(const ColorTarget& color, const ColorTargetLayout& layout) {
    TileMipLayout mip{};
    mip.width = color.extent.width;
    mip.height = color.extent.height;
    mip.blocksPerRow = layout.BlocksPerRow();
    mip.pitchBytes = color.extent.width * color.elementBytes;
    mip.tiledSize = layout.Bytes();
    mip.linearSize = layout.LinearBytes();
    return mip;
}

// APS5_PROFILE_DRAW: per-draw phase timers in microseconds (a recorded draw's phases are far below
// the millisecond the old print rounded to), totalled over 10 s in the [draws] line.
enum DrawPhase : std::size_t { PhaseValidate, PhaseVertex, PhaseSetup, PhaseReadTarget, PhasePrepare, PhaseLookup, PhaseResources, PhasePipeline, PhaseRecord, PhaseKeep, PhaseSync, PhaseWriteBack, PhaseDescribe, PhaseCount };
constexpr std::array<const char*, PhaseCount> DrawPhaseNames{"validate", "vertex", "setup", "readTarget", "prepare", "lookup", "resources", "pipeline", "record", "keep", "sync", "writeBack", "describe"};

enum RecordPhase : std::size_t { RecordAccess, RecordBindings, RecordPass, RecordBegin, RecordCommands, RecordLeave, RecordKeep, RecordPhaseCount };
constexpr std::array<const char*, RecordPhaseCount> RecordPhaseNames{"access", "bindings/snapshots", "pass/hazards", "begin/barriers", "bind/push/draw", "leave", "keep"};
enum LookupPhase : std::size_t { LookupKeyFind, LookupProof, LookupMoveAliases, LookupPhaseCount };
constexpr std::array<const char*, LookupPhaseCount> LookupPhaseNames{"key/find/misc", "proof", "move/aliases"};
enum RecipeBuild : std::size_t { RecipeNotRequested, RecipeDisabled, RecipeUncacheable, RecipeWaited, RecipeIndirect, RecipeNotReusable, RecipeMovedDisabled, RecipeMultisampled, RecipeReordered, RecipeBuilt, RecipeBuildCount };
constexpr std::array<const char*, RecipeBuildCount> RecipeBuildNames{"not requested/shape", "disabled", "uncacheable", "waited", "indirect", "not reusable", "moved disabled", "multisampled", "sample order", "built"};

int& archcLastRecipeBuild() {
    struct State { int value = -1; };
    struct LastStorage {};
    return AgcDriver::ThreadScratch<State, LastStorage>().value;
}

bool DrawMovedRecipes() {
    static const bool enabled = std::getenv("APS5_NO_DRAW_MOVED_RECIPES") == nullptr;
    return enabled;
}

// Why a draw did not go into the recorder without a wait (counted in the [draws] line): its targets
// are not all resident, no recorder is active, a switch (APS5_SYNC_DRAWS, APS5_DUMP_TARGETS,
// APS5_SYNC_COMPLETION_DRAWS) forced it, or its completion work writes copied guest buffers or
// releases an address-based build's lease, which the CPU must not outrun (see `recorded` in Draw).
enum SyncReason : std::size_t { SyncNone, SyncNotResident, SyncNoRecorder, SyncDisabled, SyncCopiedWrites, SyncLease, SyncCount };
constexpr std::array<const char*, SyncCount> SyncReasonNames{"none", "non-resident target", "no recorder", "disabled", "copied writes", "lease"};
constexpr std::size_t IndirectPathCount = static_cast<std::size_t>(IndirectDrawPath::Count);
constexpr std::array<const char*, IndirectPathCount> IndirectDrawPathNames{"gpu-side", "patched SGPR not folded", "fetch offset unknown", "non-vertex path", "GE_INDX_OFFSET", "draw index", "vertex range too large", "feature gap", "pending image results", "pending label or copied write", "not imported", "disabled", "index range not fully readable", "index limit unknown"};

// What became of one draw, for the totals.
struct DrawOutcome {
    explicit DrawOutcome(bool profile) {
        if (!profile) return;
        recordUs.fill(0);
        lookupSplitUs.fill(0);
        recipeBuild = RecipeNotRequested;
        movedBuffers = 0;
        movedRecipeBuilt = false;
    }
    std::array<double, RecordPhaseCount> recordUs;
    std::array<double, LookupPhaseCount> lookupSplitUs;
    RecipeBuild recipeBuild;
    std::uint64_t movedBuffers;
    bool movedRecipeBuilt;
    // In the recorder; `waited` when the recorder was synced right after (copied writes or a lease).
    bool recorded = false;
    bool waited = false;
    bool completion = false;
    // A recorded draw began a render pass of its own, or continued the previous draw's.
    bool passBegun = false;
    bool passContinued = false;
    PassBreak passBreak = PassBreak::Continued;
    bool keyStoresFlushed = false;
    bool storesFlushed = false;
    bool addressDraw = false;
    SyncReason reason = SyncNone;
    // Shader validation memo (see CachedFragmentOutputs).
    bool validateMemoized = false;
    bool validateHit = false;
    // A build that acquired the allocation registry lease (BDA), whose cost sits outside the
    // build's own sub-phases.
    bool addressBased = false;
    // Resident target lookups, and those that took a millisecond or more. Whether a slow one
    // re-uploaded the image or just walked its pages under contention is not visible from here
    // (StorageTexture::Version advances on every MarkDirty as well as on an upload, and Generation
    // is stamped afresh by every write-watch walk): the [texture] line's "reused" and "direct
    // uploads" counters and APS5_TRACE_UPLOAD name the actual uploads.
    std::uint64_t targetLookups = 0;
    std::uint64_t slowLookups = 0;
    double slowLookupUs = 0;
    // The lookups by whether the draw's scissor covers the whole target (an area-scoped proof
    // would change nothing for those), the pages their write-watch collects walked and their time.
    std::uint64_t fullScissorLookups = 0;
    std::uint64_t partialLookups = 0;
    std::uint64_t pagesWalked = 0;
    double lookupUs = 0;
    // How the draw's resources came about (DrawKind), for the per-kind averages.
    std::size_t kind = 0;
    // The flush hook's fence waits made inside this draw (index and vertex reads, the target
    // lookups' flushes, the build's texture lookups): nested in the draw's GpuMutex hold, so they
    // are the part of the hold that is waiting rather than working. The draw's own syncs (a
    // recorded-then-waited draw's recorder Sync, a synchronous draw's SubmitAndWait) are kept apart
    // in ownSyncUs: those wait by design and would otherwise count as nested hook waits.
    double hookWaitUs = 0;
    double ownSyncUs = 0;
};

// The resources of a draw: a recipe hit (reserved for the draw recipe step), a resource-cache
// template hit, a build, or an address-based (BDA) build.
enum DrawKind : std::size_t { KindRecipeHit, KindTemplateHit, KindBuild, KindBda, KindCount };
constexpr std::array<const char*, KindCount> DrawKindNames{"recipe hit", "template hit", "build", "BDA"};

struct TargetLookupProfile {
    ColorTarget color{};
    GuestTextureResource storage{};
    std::uint64_t storageBytes = 0;
    std::uint32_t storageLayers = 0;
    std::uint64_t lookups = 0;
    std::uint64_t slow = 0;
    std::uint64_t pages = 0;
    std::uint64_t clean = 0;
    double us = 0;
    double slowUs = 0;
    std::array<double, 9> steps{};
    std::array<double, 9> slowSteps{};
    std::array<std::uint64_t, LookupOutcomes::WorkCount> work{};
    double flushGpuMs = 0;
    double flushDepthMs = 0;
};

struct DrawProfile {
    AgcDriver::Mutex mutex;
    std::array<double, RecordPhaseCount> recordUs{};
    std::array<double, LookupPhaseCount> lookupSplitUs{};
    std::array<std::uint64_t, RecipeBuildCount> recipeBuilds{};
    std::uint64_t movedRecipeBuilds = 0, movedRecipeHits = 0, movedRecipeBuffers = 0, movedRecipeRefused = 0;
    std::uint64_t movedDraws = 0;
    double movedRecipeUs = 0, movedDrawUs = 0;
    std::map<std::array<std::uint64_t, 13>, TargetLookupProfile> targets;
    std::array<double, KindCount> kindUs{};
    std::array<std::uint64_t, KindCount> kindCounts{};
    std::uint64_t fullScissorLookups = 0;
    std::uint64_t partialLookups = 0;
    std::uint64_t pagesWalked = 0;
    double lookupUs = 0;
    std::array<double, PhaseCount> totalsUs{};
    // The longest single draw's time per phase, and the longest draw: the [lock] line's 'draw'
    // hold max (tens of ms against an average well under a millisecond) needs a phase name.
    std::array<double, PhaseCount> maxUs{};
    double maxDrawUs = 0;
    double hookWaitUs = 0;
    double maxHookWaitUs = 0;
    double ownSyncUs = 0;
    // The resources build's sub-phases (ShaderResources::Timing) of draws only; dispatches report
    // theirs in the [resources] line. `other` is the rest of the build (prepareAddressBindings:
    // the registry lease and snapshots of an address-based build, the BDA table, the layout
    // lookup), and how much of it address-based builds account for.
    double bindingsUs = 0;
    double uploadUs = 0;
    double descriptorsUs = 0;
    double otherUs = 0;
    double addressOtherUs = 0;
    std::uint64_t addressBuilds = 0;
    std::uint64_t draws = 0;
    std::uint64_t recorded = 0;
    std::uint64_t waited = 0;
    // Recorded draws whose write-back (BDA fault check) runs as a completion action instead of
    // making the draw synchronous (see APS5_SYNC_COMPLETION_DRAWS).
    std::uint64_t completion = 0;
    // Render passes recorded draws began, and draws that continued the previous draw's pass.
    std::uint64_t passesBegun = 0;
    std::uint64_t passesContinued = 0;
    std::array<DrawPassCounts, 2> passTotals{};
    std::array<std::uint64_t, PassBreakCount> passBreaks{};
    std::array<std::uint64_t, PassBreakCount> reportedPassBreaks{};
    std::array<std::uint64_t, SyncCount> reasons{};
    // The CPU inside Graphics::Draw, split by outcome: a synchronous draw costs ten times a recorded
    // one, so one average would only show the synchronous population.
    double recordedUs = 0;
    double waitedUs = 0;
    double synchronousUs = 0;
    std::uint64_t targetLookups = 0;
    std::uint64_t slowLookups = 0;
    double slowLookupUs = 0;
    // Resource cache outcomes: hits, misses (built and inserted when reusable), entries that failed
    // Revalidate, and draws that could not use the cache (synchronous, no variant id, or disabled).
    // Which key words the misses differ in is the cache's own "[rescache] miss churn" line
    // (ResourceCache::noteMiss compares each miss with the last key of the same variants).
    std::uint64_t cacheHits = 0;
    std::uint64_t cacheMisses = 0;
    std::uint64_t cacheInvalidated = 0;
    std::uint64_t uncacheable = 0;
    // Shader validation memo (see CachedFragmentOutputs).
    std::uint64_t validateHits = 0;
    std::uint64_t validateMisses = 0;
    // Draw recipe outcomes (DrawWithRecipe): hits, and misses by DrawRecipeMiss.
    std::uint64_t recipeHits = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrawRecipeMiss::Count)> recipeMisses{};
    // Indirect draws by path (IndirectDrawPath), the GPU-side ones whose records were rewritten
    // with a constant, and the time of the CPU record reads (their syncs).
    std::array<std::uint64_t, IndirectPathCount> indirect{};
    std::uint64_t indirectRewritten = 0;
    std::uint64_t indirectEmpty = 0;
    double indirectReadUs = 0;
    // Draw packets that drew nothing, by DrawSkip, and their time.
    std::array<std::uint64_t, static_cast<std::size_t>(DrawSkip::Count)> skips{};
    std::array<double, static_cast<std::size_t>(DrawSkip::Count)> skipUs{};
    AddressDrawReuseCounts addressSeen{};
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};
constexpr std::array<const char*, static_cast<std::size_t>(DrawSkip::Count)> DrawSkipNames{"nothing to draw", "prechecked", "thrown"};
constexpr std::array<const char*, static_cast<std::size_t>(AddressDrawReuse::Count)> AddressDrawReuseNames{"reused", "built", "not read-only", "not space-only", "snapshot outside the space (copied)", "partial overlap", "snapshot changed", "moved GuestBuffers element", "patched data word", "space or epoch change", "in use", "revalidation"};

std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(AddressDrawReuse::Count)>& addressDrawReuses() {
    static std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(AddressDrawReuse::Count)> counts{};
    return counts;
}

void countAddressDrawReuse(AddressDrawReuse outcome) {
    addressDrawReuses()[static_cast<std::size_t>(outcome)].fetch_add(1, std::memory_order_relaxed);
}

std::atomic<std::uint64_t>& addressDrawReuseVerifications() {
    static std::atomic<std::uint64_t> verifications{0};
    return verifications;
}

void completeSnapshotWrites(std::span<const GuestMemorySnapshot> snapshots) {
    auto* recorder = Recorder::Active();
    if (recorder == nullptr || Recorder::InCompletion()) return;
    for (const auto& snapshot : snapshots) {
        if (snapshot.bytes.empty() || !recorder->PendingWriteOverlaps(snapshot.address, snapshot.bytes.size())) continue;
        Recorder::CountSync(4);
        recorder->SyncThrough(snapshot.address, snapshot.bytes.size());
    }
}

DrawProfile& Profile() {
    static DrawProfile profile;
    return profile;
}

// Adds one draw's phases to the totals and prints the [draws] and [rescache] lines every 10 s.
void reportDraw(const std::array<double, PhaseCount>& us, const ShaderResources::BuildTiming* built, const DrawOutcome& outcome) {
    auto& profile = Profile();
    std::lock_guard lock(profile.mutex);
    double drawUs = 0;
    for (std::size_t i = 0; i < PhaseCount; ++i) {
        profile.totalsUs[i] += us[i];
        profile.maxUs[i] = std::max(profile.maxUs[i], us[i]);
        drawUs += us[i];
    }
    profile.maxDrawUs = std::max(profile.maxDrawUs, drawUs);
    profile.hookWaitUs += outcome.hookWaitUs;
    profile.maxHookWaitUs = std::max(profile.maxHookWaitUs, outcome.hookWaitUs);
    profile.ownSyncUs += outcome.ownSyncUs;
    if (built != nullptr) {
        profile.bindingsUs += built->bindingsMs * 1000.0;
        profile.uploadUs += built->uploadMs * 1000.0;
        profile.descriptorsUs += built->descriptorsMs * 1000.0;
        const auto other = std::max(0.0, us[PhaseResources] - (built->bindingsMs + built->uploadMs + built->descriptorsMs) * 1000.0);
        profile.otherUs += other;
        if (outcome.addressBased) {
            profile.addressOtherUs += other;
            ++profile.addressBuilds;
        }
    }
    ++profile.draws;
    for (std::size_t i = 0; i < RecordPhaseCount; ++i) profile.recordUs[i] += outcome.recordUs[i];
    for (std::size_t i = 0; i < LookupPhaseCount; ++i) profile.lookupSplitUs[i] += outcome.lookupSplitUs[i];
    if (outcome.kind != KindRecipeHit) ++profile.recipeBuilds[outcome.recipeBuild];
    if (outcome.movedRecipeBuilt) ++profile.movedRecipeBuilds;
    if (outcome.kind == KindRecipeHit && outcome.movedBuffers != 0) {
        ++profile.movedRecipeHits;
        profile.movedRecipeBuffers += outcome.movedBuffers;
        profile.movedRecipeUs += drawUs;
    } else if (outcome.movedBuffers != 0) {
        ++profile.movedDraws;
        profile.movedDrawUs += drawUs;
    }
    if (outcome.recorded) ++(outcome.waited ? profile.waited : profile.recorded);
    if (outcome.completion) ++profile.completion;
    if (outcome.passBegun) ++profile.passesBegun;
    if (outcome.passBegun) ++profile.passBreaks[static_cast<std::size_t>(outcome.passBreak)];
    if (outcome.passContinued) ++profile.passesContinued;
    if (outcome.recorded) {
        auto& totals = profile.passTotals[outcome.addressDraw ? 1 : 0];
        ++totals.draws;
        if (outcome.passBegun) ++totals.passesBegun;
        if (outcome.passContinued) ++totals.passesContinued;
        if (outcome.keyStoresFlushed) ++totals.keyStoreFlushes;
        if (outcome.storesFlushed) ++totals.storeFlushes;
    }
    ++profile.reasons[outcome.reason];
    (outcome.recorded ? (outcome.waited ? profile.waitedUs : profile.recordedUs) : profile.synchronousUs) += drawUs;
    profile.targetLookups += outcome.targetLookups;
    profile.slowLookups += outcome.slowLookups;
    profile.slowLookupUs += outcome.slowLookupUs;
    profile.fullScissorLookups += outcome.fullScissorLookups;
    profile.partialLookups += outcome.partialLookups;
    profile.pagesWalked += outcome.pagesWalked;
    profile.lookupUs += outcome.lookupUs;
    if (outcome.kind < KindCount) {
        ++profile.kindCounts[outcome.kind];
        profile.kindUs[outcome.kind] += drawUs;
    }
    if (outcome.validateMemoized) ++(outcome.validateHit ? profile.validateHits : profile.validateMisses);
    const auto now = std::chrono::steady_clock::now();
    if (now - profile.lastReport < std::chrono::seconds(10)) return;
    profile.lastReport = now;
    const auto synchronous = profile.draws - profile.recorded - profile.waited;
    const auto average = [](double total, std::uint64_t count) { return count != 0 ? total / static_cast<double>(count) : 0.0; };
    char line[2048];
    int n = std::snprintf(line, sizeof(line), "[draws] %llu draws over 10 s (%llu recorded avg %.0f us, of them %llu with completion; %llu recorded then waited avg %.0f us; %llu synchronous avg %.0f us; render passes %llu begun", static_cast<unsigned long long>(profile.draws), static_cast<unsigned long long>(profile.recorded), average(profile.recordedUs, profile.recorded), static_cast<unsigned long long>(profile.completion), static_cast<unsigned long long>(profile.waited), average(profile.waitedUs, profile.waited), static_cast<unsigned long long>(synchronous), average(profile.synchronousUs, synchronous), static_cast<unsigned long long>(profile.passesBegun));
    const auto room = [&] { return n > 0 && static_cast<std::size_t>(n) < sizeof(line); };
    bool listed = false;
    for (std::size_t i = 1; i < PassBreakCount && room(); ++i) {
        const auto count = profile.passBreaks[i] - profile.reportedPassBreaks[i];
        if (count == 0) continue;
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "%s%s %llu", listed ? ", " : " (", PassBreakNames[i], static_cast<unsigned long long>(count));
        listed = true;
    }
    if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "%s, %llu draws continued one; waited or synchronous because:", listed ? ")" : "", static_cast<unsigned long long>(profile.passesContinued));
    for (std::size_t i = SyncNone + 1; i < SyncCount && room(); ++i) {
        if (profile.reasons[i] != 0) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %llu", SyncReasonNames[i], static_cast<unsigned long long>(profile.reasons[i]));
    }
    if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "):");
    for (std::size_t i = 0; i < PhaseCount && room(); ++i) {
        if (profile.totalsUs[i] <= 0) continue;
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s=%.1fms", DrawPhaseNames[i], profile.totalsUs[i] / 1000.0);
        if (i == PhaseResources && room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " (bindings %.1f, upload %.1f, descriptors %.1f, other %.1f of which %.1f in %llu address-based builds)", profile.bindingsUs / 1000.0, profile.uploadUs / 1000.0, profile.descriptorsUs / 1000.0, profile.otherUs / 1000.0, profile.addressOtherUs / 1000.0, static_cast<unsigned long long>(profile.addressBuilds));
        if (i == PhaseReadTarget && room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " (%llu resident lookups, %llu of them >= 1 ms = %.1f; target lookups: full-scissor %llu / partial %llu / pages walked %llu / %.0f us)", static_cast<unsigned long long>(profile.targetLookups), static_cast<unsigned long long>(profile.slowLookups), profile.slowLookupUs / 1000.0, static_cast<unsigned long long>(profile.fullScissorLookups), static_cast<unsigned long long>(profile.partialLookups), static_cast<unsigned long long>(profile.pagesWalked), profile.lookupUs);
    }
    if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; per kind (avg us, count):");
    for (std::size_t kind = 0; kind < KindCount && room(); ++kind) {
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %.0f (%llu)", DrawKindNames[kind], average(profile.kindUs[kind], profile.kindCounts[kind]), static_cast<unsigned long long>(profile.kindCounts[kind]));
    }
    // The longest draw and the longest single phase of any draw (which phase a long 'draw' hold
    // was), plus the hook waits nested inside the draws.
    if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; longest draw %.1f ms, longest phase of any draw:", profile.maxDrawUs / 1000.0);
    for (std::size_t i = 0; i < PhaseCount && room(); ++i) {
        if (profile.maxUs[i] < 500.0) continue;
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %.1f", DrawPhaseNames[i], profile.maxUs[i] / 1000.0);
    }
    if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; hook waits inside draws %.1f ms (max %.1f; the draws' own syncs, %.1f ms, not counted)", profile.hookWaitUs / 1000.0, profile.maxHookWaitUs / 1000.0, profile.ownSyncUs / 1000.0);
    std::uint64_t indirectCpu = 0;
    for (std::size_t i = 1; i < IndirectPathCount; ++i) indirectCpu += profile.indirect[i];
    if ((profile.indirect[0] != 0 || indirectCpu != 0 || profile.indirectEmpty != 0) && room()) {
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; indirect: gpu-side %llu (%llu rewritten), cpu-side %llu (", static_cast<unsigned long long>(profile.indirect[0]), static_cast<unsigned long long>(profile.indirectRewritten), static_cast<unsigned long long>(indirectCpu));
        for (std::size_t i = 1; i < IndirectPathCount && room(); ++i) {
            if (profile.indirect[i] != 0) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %llu", IndirectDrawPathNames[i], static_cast<unsigned long long>(profile.indirect[i]));
        }
        if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "), empty skipped before their stages %llu, argument reads %.1f ms", static_cast<unsigned long long>(profile.indirectEmpty), profile.indirectReadUs / 1000.0);
    }
    if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; skipped packets:");
    for (std::size_t i = 0; i < profile.skips.size() && room(); ++i) {
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %llu in %.1f ms", DrawSkipNames[i], static_cast<unsigned long long>(profile.skips[i]), profile.skipUs[i] / 1000.0);
    }
    if (room()) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "; recipe hits %llu, misses by reason:", static_cast<unsigned long long>(profile.recipeHits));
    for (std::size_t i = 1; i < profile.recipeMisses.size() && room(); ++i) {
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %llu", DrawRecipeMissName(static_cast<DrawRecipeMiss>(i)), static_cast<unsigned long long>(profile.recipeMisses[i]));
    }
    AgcDriver::ProfilePrint_nid_no_patch("%s\n", line);
    std::string recordText;
    for (std::size_t i = 0; i < RecordPhaseCount; ++i) {
        char text[96];
        std::snprintf(text, sizeof(text), " %s %.3f", RecordPhaseNames[i], profile.recordUs[i] / 1000.0);
        recordText += text;
    }
    AgcDriver::ProfilePrint_nid_no_patch("[draw-record] (10 s, nested ms):%s\n", recordText.c_str());
    std::string lookupText;
    for (std::size_t i = 0; i < LookupPhaseCount; ++i) {
        char text[96];
        std::snprintf(text, sizeof(text), " %s %.3f", LookupPhaseNames[i], profile.lookupSplitUs[i] / 1000.0);
        lookupText += text;
    }
    AgcDriver::ProfilePrint_nid_no_patch("[draw-lookup] ordinary path (10 s, nested ms):%s\n", lookupText.c_str());
    std::string buildText;
    for (std::size_t i = 0; i < RecipeBuildCount; ++i) {
        char text[96];
        std::snprintf(text, sizeof(text), " %s %llu", RecipeBuildNames[i], static_cast<unsigned long long>(profile.recipeBuilds[i]));
        buildText += text;
    }
    AgcDriver::ProfilePrint_nid_no_patch("[draw-recipe-build] (10 s):%s; moved builds %llu, hits %llu (%llu buffers/data blocks), refused %llu, hit CPU %.3f ms; ordinary moved draws %llu CPU %.3f ms; each moved hit avoids resource key/find and pipeline lookup\n", buildText.c_str(), static_cast<unsigned long long>(profile.movedRecipeBuilds), static_cast<unsigned long long>(profile.movedRecipeHits), static_cast<unsigned long long>(profile.movedRecipeBuffers), static_cast<unsigned long long>(profile.movedRecipeRefused), profile.movedRecipeUs / 1000.0, static_cast<unsigned long long>(profile.movedDraws), profile.movedDrawUs / 1000.0);
    std::vector<const TargetLookupProfile*> targets;
    for (const auto& [key, target] : profile.targets) targets.push_back(&target);
    std::sort(targets.begin(), targets.end(), [](const auto* left, const auto* right) { return left->us > right->us; });
    for (std::size_t i = 0; i < std::min<std::size_t>(targets.size(), 8); ++i) {
        const auto& target = *targets[i];
        const auto& color = target.color;
        AgcDriver::ProfilePrint_nid_no_patch("[resident-target] rank %zu/%zu 0x%llx+0x%zx %ux%u format %u tile %u xor %u DCC 0x%llx layers %u slice %u depth %u mip %u samples %u: %llu lookups %.3f ms, >=1ms %llu %.3f ms, pages %llu, clean proofs %llu; ms wait %.3f proof %.3f collect %.3f DCC %.3f flush %.3f validate %.3f other %.3f; nested write-watch query %.3f map/walk %.3f\n", i + 1, targets.size(), static_cast<unsigned long long>(color.address), color.bytes, color.extent.width, color.extent.height, static_cast<unsigned>(color.format), static_cast<unsigned>(color.tileMode), color.pipeBankXor, static_cast<unsigned long long>(color.dccAddress), color.layers, color.arraySlice, color.depth, color.mip, color.samples, static_cast<unsigned long long>(target.lookups), target.us / 1000, static_cast<unsigned long long>(target.slow), target.slowUs / 1000, static_cast<unsigned long long>(target.pages), static_cast<unsigned long long>(target.clean), target.steps[0], target.steps[1], target.steps[2], target.steps[3], target.steps[4], target.steps[5], target.steps[8], target.steps[6], target.steps[7]);
        if (target.storageBytes != 0) AgcDriver::ProfilePrint_nid_no_patch("[resident-target] storage for 0x%llx: 0x%llx+0x%llx %ux%ux%u guest format %u tile %u DCC 0x%llx mips %u\n", static_cast<unsigned long long>(color.address), static_cast<unsigned long long>(target.storage.baseAddress), static_cast<unsigned long long>(target.storageBytes), target.storage.width, target.storage.height, target.storageLayers, target.storage.format, static_cast<unsigned>(target.storage.tileMode), static_cast<unsigned long long>(target.storage.dccAddress), target.storage.mipCount);
        static constexpr std::array<const char*, LookupOutcomes::WorkCount> workNames{"calls", "images", "storage/depth", "draw", "fill", "copy", "shadow-only", "depth-ranges", "waits", "writeback-bytes", "shadow-bytes", "import-bytes", "CPU-bytes", "retile-windows", "image-buffer-regions", "layout-transitions", "alias-copies", "alias-bytes", "pending-alias-units", "pending-alias-refused", "alias-writer-refused", "alias-foreign-refused", "foreign-clear-fill", "foreign-full-overwrite", "foreign-partial-unknown", "foreign-read-preserve", "foreign-full-scissor-unproven"};
        std::string work;
        char field[96];
        for (std::size_t j = 0; j < workNames.size(); ++j) {
            std::snprintf(field, sizeof(field), " %s=%llu", workNames[j], static_cast<unsigned long long>(target.work[j]));
            work += field;
        }
        AgcDriver::ProfilePrint_nid_no_patch("[resident-target] reconciliation 0x%llx:%s; nested pre-store GPU wait %.3f ms, depth %.3f ms\n", static_cast<unsigned long long>(color.address), work.c_str(), target.flushGpuMs, target.flushDepthMs);
        if (target.slow != 0) AgcDriver::ProfilePrint_nid_no_patch("[resident-target] slow 0x%llx: ms wait %.3f proof %.3f collect %.3f DCC %.3f flush %.3f validate %.3f other %.3f; nested write-watch query %.3f map/walk %.3f\n", static_cast<unsigned long long>(color.address), target.slowSteps[0], target.slowSteps[1], target.slowSteps[2], target.slowSteps[3], target.slowSteps[4], target.slowSteps[5], target.slowSteps[8], target.slowSteps[6], target.slowSteps[7]);
    }
    profile.targets.clear();
    AgcDriver::ProfilePrint_nid_no_patch("[rescache] draws: %llu hits, %llu misses, %llu invalidated, %llu uncacheable; validation memo %llu hits / %llu misses (which key words the misses differ in: the miss churn line)\n", static_cast<unsigned long long>(profile.cacheHits), static_cast<unsigned long long>(profile.cacheMisses), static_cast<unsigned long long>(profile.cacheInvalidated), static_cast<unsigned long long>(profile.uncacheable), static_cast<unsigned long long>(profile.validateHits), static_cast<unsigned long long>(profile.validateMisses));
    const auto address = AddressDrawReuseCounters();
    if (address != profile.addressSeen) {
        char reuse[512];
        const auto seen = [&](AddressDrawReuse outcome) { return static_cast<unsigned long long>(address[static_cast<std::size_t>(outcome)] - profile.addressSeen[static_cast<std::size_t>(outcome)]); };
        int at = std::snprintf(reuse, sizeof(reuse), "[rescache] address-based draws (10 s): reused %llu, built %llu; refused:", seen(AddressDrawReuse::Reused), seen(AddressDrawReuse::Built));
        for (auto outcome = static_cast<std::size_t>(AddressDrawReuse::NotReadOnly); outcome < address.size() && at > 0 && static_cast<std::size_t>(at) < sizeof(reuse); ++outcome) {
            at += std::snprintf(reuse + at, sizeof(reuse) - static_cast<std::size_t>(at), "%s %s %llu", outcome == static_cast<std::size_t>(AddressDrawReuse::NotReadOnly) ? "" : ",", AddressDrawReuseNames[outcome], seen(static_cast<AddressDrawReuse>(outcome)));
        }
        AgcDriver::ProfilePrint_nid_no_patch("%s\n", reuse);
        profile.addressSeen = address;
    }
    profile.totalsUs.fill(0);
    profile.recordUs.fill(0);
    profile.lookupSplitUs.fill(0);
    profile.recipeBuilds.fill(0);
    profile.movedRecipeBuilds = profile.movedRecipeHits = profile.movedRecipeBuffers = profile.movedRecipeRefused = 0;
    profile.movedDraws = 0;
    profile.movedRecipeUs = profile.movedDrawUs = 0;
    profile.maxUs.fill(0);
    profile.maxDrawUs = profile.hookWaitUs = profile.maxHookWaitUs = profile.ownSyncUs = 0;
    profile.bindingsUs = profile.uploadUs = profile.descriptorsUs = profile.otherUs = profile.addressOtherUs = 0;
    profile.addressBuilds = 0;
    profile.draws = profile.recorded = profile.waited = profile.completion = 0;
    profile.passesBegun = profile.passesContinued = 0;
    profile.reportedPassBreaks = profile.passBreaks;
    profile.reasons.fill(0);
    profile.recordedUs = profile.waitedUs = profile.synchronousUs = 0;
    profile.targetLookups = profile.slowLookups = 0;
    profile.slowLookupUs = 0;
    profile.fullScissorLookups = profile.partialLookups = profile.pagesWalked = 0;
    profile.lookupUs = 0;
    profile.kindUs.fill(0);
    profile.kindCounts.fill(0);
    profile.cacheHits = profile.cacheMisses = profile.cacheInvalidated = profile.uncacheable = 0;
    profile.validateHits = profile.validateMisses = 0;
    profile.recipeHits = 0;
    profile.recipeMisses.fill(0);
    profile.indirect.fill(0);
    profile.indirectRewritten = 0;
    profile.indirectReadUs = 0;
    profile.skips.fill(0);
    profile.skipUs.fill(0);
}

void countCache(std::uint64_t DrawProfile::*counter) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    auto& stats = Profile();
    std::lock_guard lock(stats.mutex);
    ++(stats.*counter);
}

}

// ValidateShaders decodes every SPIR-V instruction of every stage on every draw. Its outcome depends
// only on the stages' compiled variants (a variant id names identical SPIR-V and binding layout),
// their push constant placement and vertex attribute shapes, and the state fields it checks, so the
// fragment output set is remembered by exactly those. Nothing here is keyed by pointer: the results
// a draw hands in live in a per-draw vector. A stage without a variant id is validated as before,
// except the rect-list control and evaluation stages, which are generated from the vertex and
// fragment results the key already names (the pipeline key treats them the same way). `memoized`
// says whether the memo applied, `hit` whether it answered. APS5_NO_VALIDATE_CACHE=1 validates
// every draw.
bool ValidationKey(const Context& context, std::span<const CompiledShader> shaders, const State& state, std::vector<std::uint64_t>& key) {
    using Stage = ShaderRecompiler::ShaderStage;
    static const bool disabled = std::getenv("APS5_NO_VALIDATE_CACHE") != nullptr;
    const auto add = [&](auto value) { key.push_back(static_cast<std::uint64_t>(value)); };
    bool keyed = !disabled;
    if (keyed) {
        key.reserve(40 + shaders.size() * 12);
        add(shaders.size());
        const bool patches = BarycentricPatches(state, shaders);
        add(patches ? static_cast<std::uint64_t>(state.provokingVertexMode) + 1u : 0u);
        for (const auto& shader : shaders) {
            Require(shader.program != nullptr, "missing compiled shader");
            const auto& program = *shader.program;
            const bool generated = ((state.rectList || patches) && (shader.stage == Stage::TessellationControl || shader.stage == Stage::TessellationEvaluation)) || shader.stage == Stage::Geometry;
            if (!generated && program.PipelineVariantId() == 0) {
                keyed = false;
                break;
            }
            add(shader.stage);
            add(generated ? std::uint64_t{0} : program.PipelineVariantId());
            add(shader.pushConstantOffset);
            add(program.pushConstants.size());
            add(program.bdaAbiVersion);
            add(program.vertexAttributes.size());
            for (const auto& attribute : program.vertexAttributes) {
                add(attribute.location);
                add(attribute.components);
                add(attribute.fetchIndex);
                add(attribute.formatComponents);
                // The data format bits of the V# decide the attribute's signature.
                add((attribute.resource.fields[3] >> 12u) & 0x7fu);
            }
        }
    }
    if (keyed) {
        add(state.stages.path);
        add(state.rectList);
        add(state.topology);
        add(state.cullMode);
        add(state.polygonMode);
        add(state.lineWidth);
        add(state.blends.size());
        add(context.subgroup.subgroupSize);
        add(context.subgroup.supportedStages);
        add(context.subgroup.supportedOperations);
        add(context.fragmentShaderBarycentric);
        add(context.shaderClipDistance);
        add(context.shaderCullDistance);
        add(context.viewportIndexLayer);
        add(context.shaderTerminateInvocation);
        add(state.paClVsOutCntl);
        add(state.vertexPoints);
        add(state.stages.mesh.has_value());
        if (state.stages.mesh) {
            const auto& mesh = *state.stages.mesh;
            add(mesh.inputPrimitive);
            add(mesh.primitivesPerGroup);
            add(mesh.verticesPerGroup);
            add(mesh.maxVertices);
            add(mesh.maxPrimitives);
            add(mesh.threadsPerGroup);
            add(mesh.ldsSizeDwords);
            add(mesh.provokingVertex);
            add(mesh.outputPrimitiveSize);
        }
        add(state.stages.tessellation.has_value());
        if (state.stages.tessellation) {
            const auto& tessellation = *state.stages.tessellation;
            add(tessellation.inputControlPoints);
            add(tessellation.outputControlPoints);
            add(tessellation.domain);
            add(tessellation.partitioning);
            add(tessellation.outputTopology);
            add(tessellation.patchesPerGroup);
            add(tessellation.primitiveGroupSize);
            add(tessellation.breakAtInstance ? 1u : 0u);
        }
    }
    return keyed;
}

namespace {

AgcDriver::Mutex& validationMutex() {
    static AgcDriver::Mutex mutex;
    return mutex;
}

std::map<std::vector<std::uint64_t>, std::string>& validationFailures() {
    static std::map<std::vector<std::uint64_t>, std::string> failures;
    return failures;
}

std::vector<std::uint64_t>& validationKeyScratch() {
    struct KeySlotStorage {};
    auto& key = AgcDriver::ThreadScratch<std::vector<std::uint64_t>, KeySlotStorage>();
    key.clear();
    return key;
}

std::uint32_t CachedFragmentOutputs(const Context& context, std::span<const CompiledShader> shaders, const State& state, bool& memoized, bool& hit) {
    memoized = false;
    hit = false;
    auto& key = validationKeyScratch();
    const bool keyed = ValidationKey(context, shaders, state, key);
    static std::map<std::vector<std::uint64_t>, std::uint32_t> memo;
    struct LastKeyStorage {};
    auto& lastKey = HostThreadLocal<std::vector<std::uint64_t>, LastKeyStorage>();
    struct LastOutputsThreadStorage {};
    auto& lastOutputs = AgcDriver::ThreadScratch<std::uint32_t, LastOutputsThreadStorage>();
    struct LastValidThreadStorage {};
    auto& lastValid = AgcDriver::ThreadScratch<bool, LastValidThreadStorage>();
    const bool trims = keyed && CommitTrims();
    if (trims && lastValid && key == lastKey) {
        memoized = true;
        hit = true;
        return lastOutputs;
    }
    if (keyed) {
        memoized = true;
        std::lock_guard lock(validationMutex());
        if (const auto found = memo.find(key); found != memo.end()) {
            hit = true;
            if (trims) {
                lastKey = key;
                lastOutputs = found->second;
                lastValid = true;
            }
            return found->second;
        }
    }
    std::uint32_t outputs = 0;
    try {
        for (const auto location : ValidateShaders(shaders, state, context.subgroup, context.fragmentShaderBarycentric, context.descriptorIndexing, context.imageInt64Atomics, context.geometryShader, context.sampleRateShading, context.bufferInt64Atomics, context.shaderClipDistance, context.shaderCullDistance, context.viewportIndexLayer, context.runtimeDescriptorArray, context.demoteToHelperInvocation, context.shaderTerminateInvocation, context.meshShader, context.shaderResourceMinLod, context.fmaFloat16)) {
            Require(location < 32, "fragment output location exceeds the output mask");
            outputs |= 1u << location;
        }
    } catch (const std::exception& error) {
        if (keyed) {
            std::lock_guard lock(validationMutex());
            auto& failures = validationFailures();
            if (failures.size() >= 1024) failures.clear();
            failures.emplace(key, error.what());
        }
        throw;
    }
    if (keyed) {
        std::lock_guard lock(validationMutex());
        // A handful of configurations recur; a runaway key space is dropped wholesale.
        if (memo.size() >= 1024) memo.clear();
        memo.emplace(key, outputs);
    }
    return outputs;
}

// The resource cache key of a recorded draw: a marker no compute key starts with (those begin with
// the stage), the device handle (the cache is process-wide and the driver replaces the headless
// device with the windowed one while workers may still use the old one: an entry's descriptor set
// and pooled buffers belong to the device that built it) and every stage's content key
// (ResourceCache::noteMiss walks this layout to attribute misses, so it changes together with it).
// With `ranges`, also the render target and the index buffer the build's alias checks compared the
// guest buffers against; without it the checks are repeated for a hit (CheckBufferAliases), since
// neither range is part of the descriptor set (the target is attached, the index buffer copied per
// draw), so a target or index ring that moves per frame does not miss on every draw. That guards a
// workload with such rings; in the profiled menu stage every draw was DRAW_INDEX_AUTO (index range
// 0) onto one fixed target, so its misses come from the stages' descriptor words themselves.
bool MovableBuffers() {
    static const bool enabled = std::getenv("APS5_NO_MOVED_BUFFER_TEMPLATES") == nullptr;
    return enabled;
}

ResourceCache::Key DrawResourceKey(const Context& context, std::span<const CompiledShader> shaders, const ColorTarget& target, std::uint32_t colorAttachments, std::uint64_t indexAddress, std::uint64_t indexBytes, bool ranges) {
    ResourceCache::Key key;
    key.reserve(256);
    key.push_back(0xffffffffu);
    const auto append64 = [&](std::uint64_t value) {
        key.push_back(static_cast<std::uint32_t>(value));
        key.push_back(static_cast<std::uint32_t>(value >> 32u));
    };
    append64(reinterpret_cast<std::uint64_t>(context.device));
    key.push_back(static_cast<std::uint32_t>(shaders.size()));
    for (const auto& shader : shaders) {
        const auto sizeAt = key.size();
        key.push_back(0u);
        auto& memo = shader.program->drawKeyMemo.words;
        if (auto words = memo.load(std::memory_order_acquire); words != nullptr) {
            key.insert(key.end(), words->begin(), words->end());
        } else {
            ShaderResources::AppendContentKey(shader, true, MovableBuffers(), key);
            memo.store(std::make_shared<const std::vector<std::uint32_t>>(key.begin() + static_cast<std::ptrdiff_t>(sizeAt + 1u), key.end()), std::memory_order_release);
        }
        key[sizeAt] = static_cast<std::uint32_t>(key.size() - sizeAt - 1u);
    }
    key.push_back(colorAttachments);
    if (ranges) {
        append64(target.address | target.pipeBankXor);
        append64(target.bytes);
        append64(indexAddress);
        append64(indexBytes);
    }
    return key;
}

bool DrawResourcesCacheable(std::span<const CompiledShader> shaders) {
    static const bool noDrawResourceCache = std::getenv("APS5_NO_DRAW_RESOURCE_CACHE") != nullptr;
    static const bool noTextureCache = std::getenv("APS5_NO_TEXTURE_CACHE") != nullptr;
    return !noDrawResourceCache && !noTextureCache && std::all_of(shaders.begin(), shaders.end(), [](const CompiledShader& shader) { return shader.program != nullptr && shader.program->PipelineVariantId() != 0; }) && !ShaderResources::NeverReusable(shaders);
}

// The alias checks the build makes for every guest buffer descriptor (ShaderResources::
// addGuestBuffer), repeated for a cached build whose own checks compared the buffers against
// another draw's render target and index buffer. The same conditions and messages, so a draw that
// would have failed its build fails its hit.
void CheckBufferAliases(std::span<const CompiledShader> shaders, const ColorTarget& target, std::uint64_t indexAddress, std::uint64_t indexBytes) {
    const auto overlap = [](std::uint64_t first, std::uint64_t firstSize, std::uint64_t second, std::uint64_t secondSize) { return first < second + secondSize && second < first + firstSize; };
    const auto targetAddress = target.MemoryAddress();
    for (const auto& shader : shaders) {
        for (const auto& binding : shader.program->bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
            const auto& words = binding.guestDescriptor;
            for (std::size_t offset = 0; offset + 4 <= words.size(); offset += 4) {
                const ShaderRecompiler::ShaderBufferResource descriptor{{words[offset], words[offset + 1], words[offset + 2], words[offset + 3]}};
                const auto address = descriptor.Base48();
                auto size = descriptor.GetSize();
                if (size == 0 || address == 0) continue;
                const auto element = offset / 4;
                const bool written = element >= binding.Flags().bufferWritten.size() || binding.Flags().bufferWritten[element];
                if (!written) size = ShaderResources::BytesUpToTarget(address, size, descriptor.Stride(), &target);
                Require(!overlap(address, size, targetAddress, target.bytes), "shader buffer aliases the render target");
                Require(!written || !overlap(address, size, indexAddress, indexBytes), "writable shader buffer aliases the index buffer");
            }
        }
    }
}

}

bool DrawPrelockEnabled() {
    static const bool enabled = std::getenv("APS5_NO_DRAW_PRELOCK") == nullptr;
    return enabled;
}

void RecheckDrawPreparation(const Context& context, DrawPreparation& preparation) {
    if (preparation.device == VK_NULL_HANDLE) return;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (profile && preparation.device != context.device && !preparation.resourceKey.empty()) preparation.keyInvalidated = true;
    if (preparation.device != context.device) {
        preparation.resourceKey.clear();
        preparation.resolvedTemplate.reset();
        preparation.speculatedPipeline.reset();
        preparation.resolvedFind = false;
        preparation.resolvedOutputsKnown = false;
        preparation.resolvedVertexInput.reset();
    }
    if (profile) preparation.collectInvalidated |= preparation.device != context.device || preparation.epoch != GuestMemory::ThreadCollectEpoch() || preparation.mapping != GuestAllocations::GuestAllocationsGeneration_nid_postfix() || preparation.unwatch != GuestMemory::UnwatchSerial();
}

std::array<std::uint32_t, 4> MeshIndexBufferDescriptor(const Pm4::DrawParameters& draw) {
    constexpr std::uint32_t RawWord3 = 0x31016facu;
    if (!draw.indexed) return {0u, 0u, 0u, RawWord3};
    const auto address = draw.indexAddress;
    const auto bytes = (static_cast<std::uint64_t>(draw.indexCount) * draw.indexSize + 3u) & ~std::uint64_t{3};
    Require(address != 0 && bytes != 0 && bytes <= 0xffffffffu && (address >> 48u) == 0, "invalid mesh index buffer range");
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) & 0xffffu, static_cast<std::uint32_t>(bytes), RawWord3};
}

MeshArgumentRules MeshArgumentRulesFor(const Context& context, const ShaderRecompiler::MeshConfiguration& mesh, std::uint32_t indexCount) {
    const auto inputSize = mesh.inputPrimitive == 1 ? 1u : mesh.inputPrimitive == 2 ? 2u : 3u;
    const auto step = mesh.inputPrimitive == 5 || mesh.inputPrimitive == 6 ? 1u : inputSize;
    Require(mesh.primitivesPerGroup != 0, "mesh pipeline groups no primitive (zero primitives per group)");
    return {indexCount, inputSize, step, mesh.primitivesPerGroup, context.meshLimits.maxMeshWorkGroupCount[0], context.meshLimits.maxMeshWorkGroupCount[1], context.meshLimits.maxMeshWorkGroupTotalCount};
}

std::optional<MeshGrid> MeshGridFor(const Context& context, std::uint32_t groups, std::uint32_t instances) {
    const auto& limits = context.meshLimits;
    if (groups == 0 || instances > limits.maxMeshWorkGroupCount[1] || limits.maxMeshWorkGroupCount[0] == 0) return std::nullopt;
    const auto across = groups <= limits.maxMeshWorkGroupCount[0] ? groups : std::min(limits.maxMeshWorkGroupCount[0], ShaderRecompiler::MeshLayerStrideLimit);
    const auto layers = (groups - 1u) / across + 1u;
    const auto columns = (groups - 1u) / layers + 1u;
    if (layers > limits.maxMeshWorkGroupCount[2] || (instances != 0 && columns > limits.maxMeshWorkGroupTotalCount / instances / layers)) return std::nullopt;
    return MeshGrid{columns, instances, layers, layers > 1u ? columns : 0u};
}

MeshArguments ResolveMeshArguments(const Pm4::DrawArguments& record, const MeshArgumentRules& rules) {
    const auto first = record.firstVertexOrIndex;
    const auto effective = first < rules.indexCount ? std::min(record.count, rules.indexCount - first) : 0u;
    std::uint32_t groups = 0;
    if (effective >= rules.inputSize && record.instances != 0) {
        groups = (effective - rules.inputSize) / rules.step / rules.primitivesPerGroup + 1u;
        if (groups > rules.maxGroups || record.instances > rules.maxInstances || groups > rules.maxTotal / record.instances) groups = 0;
    }
    return {groups, groups != 0 ? record.instances : 0u, groups != 0 ? 1u : 0u, effective, first};
}

std::shared_ptr<std::vector<std::shared_ptr<ShaderResources>>> DrawCopiedWriters() {
    // Never destroyed: a completion action of a batch still in flight at static teardown may erase from it.
    static auto* const writers = new std::shared_ptr<std::vector<std::shared_ptr<ShaderResources>>>(std::make_shared<std::vector<std::shared_ptr<ShaderResources>>>());
    return *writers;
}

const char* IndirectDrawPathName(IndirectDrawPath path) {
    return IndirectDrawPathNames[static_cast<std::size_t>(path)];
}

void CountIndirectDraw(IndirectDrawPath path, double readMs, bool rewritten) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    auto& stats = Profile();
    std::lock_guard lock(stats.mutex);
    ++stats.indirect[static_cast<std::size_t>(path)];
    if (rewritten) ++stats.indirectRewritten;
    stats.indirectReadUs += readMs * 1000.0;
}

std::array<std::uint64_t, IndirectPathCount> IndirectDrawCounts() {
    auto& stats = Profile();
    std::lock_guard lock(stats.mutex);
    return stats.indirect;
}

void CountEmptyIndirectDraw(double readMs) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    auto& stats = Profile();
    std::lock_guard lock(stats.mutex);
    ++stats.indirectEmpty;
    stats.indirectReadUs += readMs * 1000.0;
}

std::uint64_t EmptyIndirectDrawCount() {
    auto& stats = Profile();
    std::lock_guard lock(stats.mutex);
    return stats.indirectEmpty;
}

std::array<DrawPassCounts, 2> DrawPassTotals() {
    auto& stats = Profile();
    std::lock_guard lock(stats.mutex);
    return stats.passTotals;
}

std::array<std::uint64_t, PassBreakCount> DrawPassBreaks() {
    auto& stats = Profile();
    std::lock_guard lock(stats.mutex);
    return stats.passBreaks;
}

void CountDrawSkip(DrawSkip kind, double us) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    auto& stats = Profile();
    std::lock_guard lock(stats.mutex);
    ++stats.skips[static_cast<std::size_t>(kind)];
    stats.skipUs[static_cast<std::size_t>(kind)] += us;
}


bool DrawRecipes() {
    static const bool noDrawRecipe = std::getenv("APS5_NO_DRAW_RECIPE") != nullptr;
    return !noDrawRecipe;
}

const char* DrawRecipeMissName(DrawRecipeMiss miss) {
    constexpr std::array<const char*, static_cast<std::size_t>(DrawRecipeMiss::Count)> names{"none", "not recordable", "target gone", "template gone", "objects gone", "proof"};
    return names[static_cast<std::size_t>(miss)];
}

namespace {

// APS5_PROFILE_DRAW: the per-phase timers of one draw (DrawPhase), shared by Draw's parts and
// DrawWithRecipe.
// APS5_ARCHB_DRAW_REUSE_CENSUS (defined with noteDrawCensus): every DrawTimer phase also ends a
// census stage (markPhase maps DrawPhase to StructCensus::ReuseStage).
namespace reuseCensus {
void markPhase(std::size_t drawPhase);
void mark(StructCensus::ReuseStage stage);
}

// Stage 4 M4 (APS5_RESOLVE_CENSUS): the census bucket each DrawPhase's time goes to. Resolve-class:
// validate, vertex (inputs/index), setup, lookup (template lookup/proof), resources, pipeline;
// apply-class: readTarget and prepare (target bookkeeping), record, keep, sync, writeBack, describe.
constexpr std::array<Stage4::Bucket, PhaseCount> Stage4PhaseBuckets{
    Stage4::Bucket::Validate, Stage4::Bucket::Vertex, Stage4::Bucket::Setup, Stage4::Bucket::ReadTarget, Stage4::Bucket::Prepare, Stage4::Bucket::Lookup,
    Stage4::Bucket::Resources, Stage4::Bucket::Pipeline, Stage4::Bucket::Record, Stage4::Bucket::Keep, Stage4::Bucket::Sync, Stage4::Bucket::WriteBack, Stage4::Bucket::Describe};

// Arch C P3c census (APS5_ARCHB_C_NOSTAGE_CENSUS=1): a direct draw the driver commits with no recipe
// stages (Driver::draw's `why`, armed by ArchcNostageArm on the committer before its Draw) is timed
// per DrawTimer phase inside Graphics::Draw, with its template lookup, resource kind, validation memo,
// completion and recording outcome and the fence waits nested in it (archc_nostage_*).
namespace archcNostage {

constexpr std::size_t WhyCount = 6;
constexpr std::array<const char*, WhyCount> WhyNames{"no_key", "patched", "recipes_off", "null_hit", "null_miss", "other"};
enum LookupState : std::size_t { LookupNone, LookupUncacheable, LookupAbsent, LookupInvalid, LookupMovedRefused, LookupHit, LookupStateCount };
constexpr std::array<const char*, LookupStateCount> LookupNames{"none", "uncacheable", "absent", "invalid", "moved_refused", "hit"};
enum Validate : std::size_t { ValidateOff, ValidateHit, ValidateMiss, ValidateCount };
constexpr std::array<const char*, ValidateCount> ValidateNames{"off", "hit", "miss"};
enum Record : std::size_t { RecordRecorded, RecordWaited, RecordListed, RecordSynchronous, RecordNothing, RecordCount };
constexpr std::array<const char*, RecordCount> RecordNames{"recorded", "waited", "listed", "synchronous", "nothing"};
constexpr std::uint64_t TrimNs = 1000000;

struct Counters {
    std::array<std::atomic<std::uint64_t>, WhyCount> draws{}, ns{};
    std::array<std::atomic<std::uint64_t>, PhaseCount + 1> phaseNs{};
    std::array<std::atomic<std::uint64_t>, KindCount> kind{}, kindNs{};
    std::array<std::atomic<std::uint64_t>, LookupStateCount> lookup{}, lookupNs{};
    std::array<std::atomic<std::uint64_t>, ValidateCount> validate{};
    std::array<std::atomic<std::uint64_t>, RecordCount> record{}, recordNs{};
    std::array<std::atomic<std::uint64_t>, SyncCount> reason{};
    std::atomic<std::uint64_t> completion{0}, passBegun{0}, passContinued{0}, waitNs{0}, slow{0}, slowNs{0}, moved{0}, cacheable{0}, inserted{0};
    std::array<std::array<std::atomic<std::uint64_t>, PhaseCount + 1>, KindCount> kindPhaseNs{};
    std::array<std::atomic<std::uint64_t>, PhaseCount + 1> slowPhaseNs{};
    static constexpr std::size_t WrapperCount = 7;
    static constexpr std::array<const char*, WrapperCount> WrapperNames{"prelock", "lock_wait", "labels", "writers", "draw", "after", "submit"};
    std::array<std::atomic<std::uint64_t>, WrapperCount> wrapperNs{};
    std::atomic<std::uint64_t> wrapped{0}, submits{0}, submitSlowNs{0};
    Counters() {
        using AgcDriver::ArchB::Recorder;
        const auto add = [](const std::string& name, std::atomic<std::uint64_t>& value) { Recorder::RegisterCounter((new std::string("archc_nostage_" + name))->c_str(), value); };
        for (std::size_t i = 0; i < WhyCount; ++i) {
            add(std::string("draws_") + WhyNames[i], draws[i]);
            add(std::string("ns_") + WhyNames[i], ns[i]);
        }
        for (std::size_t i = 0; i < PhaseCount; ++i) add(std::string("phase_ns_") + DrawPhaseNames[i], phaseNs[i]);
        add("phase_ns_tail", phaseNs[PhaseCount]);
        for (std::size_t i = 0; i < KindCount; ++i) {
            add("kind_" + std::to_string(i), kind[i]);
            add("kind_ns_" + std::to_string(i), kindNs[i]);
        }
        for (std::size_t i = 0; i < LookupStateCount; ++i) {
            add(std::string("lookup_") + LookupNames[i], lookup[i]);
            add(std::string("lookup_ns_") + LookupNames[i], lookupNs[i]);
        }
        for (std::size_t i = 0; i < ValidateCount; ++i) add(std::string("validate_") + ValidateNames[i], validate[i]);
        for (std::size_t i = 0; i < RecordCount; ++i) {
            add(std::string("record_") + RecordNames[i], record[i]);
            add(std::string("record_ns_") + RecordNames[i], recordNs[i]);
        }
        for (std::size_t i = 0; i < SyncCount; ++i) add("reason_" + std::to_string(i), reason[i]);
        add("completion", completion);
        add("pass_begun", passBegun);
        add("pass_continued", passContinued);
        add("wait_ns", waitNs);
        add("slow", slow);
        add("slow_ns", slowNs);
        add("moved", moved);
        add("cacheable", cacheable);
        add("inserted", inserted);
        for (std::size_t k = 0; k < KindCount; ++k) {
            for (std::size_t i = 0; i <= PhaseCount; ++i) add("kind_phase_ns_" + std::to_string(k) + "_" + (i < PhaseCount ? DrawPhaseNames[i] : "tail"), kindPhaseNs[k][i]);
        }
        for (std::size_t i = 0; i <= PhaseCount; ++i) add(std::string("slow_phase_ns_") + (i < PhaseCount ? DrawPhaseNames[i] : "tail"), slowPhaseNs[i]);
        for (std::size_t i = 0; i < WrapperCount; ++i) add(std::string("wrapper_ns_") + WrapperNames[i], wrapperNs[i]);
        add("wrapped", wrapped);
        add("submits", submits);
        add("submit_slow_ns", submitSlowNs);
    }
};

Counters& counters() {
    static Counters made;
    return made;
}

bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_ARCHB_C_NOSTAGE_CENSUS");
        return value != nullptr && value[0] == '1';
    }();
    return on;
}

struct ThreadState {
    int armed = -1;
    bool active = false;
    std::size_t why = 0;
    std::uint64_t start = 0;
    std::uint64_t last = 0;
    std::array<std::uint64_t, PhaseCount + 1> phase{};
    LookupState lookup = LookupNone;
    double waitedBefore = 0;
};

ThreadState& state() {
    struct SlotStorage {};
    return AgcDriver::ThreadScratch<ThreadState, SlotStorage>();
}

std::uint64_t now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void phase(std::size_t which) {
    auto& made = state();
    if (!made.active) return;
    const auto at = now();
    made.phase[which] += at - made.last;
    made.last = at;
}

void noteLookup(LookupState lookup) {
    if (!enabled()) return;
    auto& made = state();
    if (made.active) made.lookup = lookup;
}

}

struct DrawTimer {
    bool profile;
    std::chrono::steady_clock::time_point phaseStart;
    std::array<double, PhaseCount> us{};
    std::array<std::uint64_t, PhaseCount> heldNs;
    std::uint64_t heldStart;
    std::uint64_t phaseHeldStart;
    explicit DrawTimer(bool profile) : profile(profile), phaseStart(profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {
        // The item's time before this draw's first phase (lock wait excluded by its own scope).
        Stage4::Mark(Stage4::Bucket::Pre);
        if (!profile) return;
        heldNs.fill(0);
        heldStart = phaseHeldStart = GuestMemory::ThreadGpuHeldNs();
    }
    void phase(DrawPhase which) {
        reuseCensus::markPhase(which);
        Stage4::Mark(Stage4PhaseBuckets[which]);
        if (archcNostage::enabled()) archcNostage::phase(which);
        if (!profile) return;
        const auto now = std::chrono::steady_clock::now();
        const auto held = GuestMemory::ThreadGpuHeldNs();
        heldNs[which] += held - phaseHeldStart;
        phaseHeldStart = held;
        us[which] += std::chrono::duration<double, std::micro>(now - phaseStart).count();
        phaseStart = now;
    }
    void charge(DrawPhase which, std::chrono::steady_clock::time_point start) {
        if (!profile) return;
        const auto now = std::chrono::steady_clock::now();
        us[which] += std::chrono::duration<double, std::micro>(now - start).count();
        phaseStart += now - start;
    }
    ~DrawTimer() {
        if (!profile) return;
        struct Totals {
            std::uint64_t scopes = 0;
            std::uint64_t held = 0;
            std::uint64_t tail = 0;
            std::array<std::uint64_t, PhaseCount> phases{};
            std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
        };
        struct TotalsThreadStorage {};
        auto& totals = AgcDriver::ThreadScratch<Totals, TotalsThreadStorage>();
        const auto held = GuestMemory::ThreadGpuHeldNs();
        ++totals.scopes;
        totals.held += held - heldStart;
        totals.tail += held - phaseHeldStart;
        for (std::size_t i = 0; i < PhaseCount; ++i) totals.phases[i] += heldNs[i];
        const auto now = std::chrono::steady_clock::now();
        if (now - totals.reported < std::chrono::seconds(10)) return;
        char line[1536];
        int n = std::snprintf(line, sizeof(line), "[draw-hold] queue 0x%x target-proof %s (10 s): %llu Graphics scopes (recipe attempts included), held %.3f ms / %.3f us per scope; held phase ms:", GuestMemory::GpuLockThreadTag(), StorageTexture::DrawTargetPrelockEnabled() ? "on" : "off", static_cast<unsigned long long>(totals.scopes), static_cast<double>(totals.held) / 1e6, static_cast<double>(totals.held) / (1000.0 * totals.scopes));
        for (std::size_t i = 0; i < PhaseCount && n > 0 && static_cast<std::size_t>(n) < sizeof(line); ++i) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s=%.3f", DrawPhaseNames[i], static_cast<double>(totals.phases[i]) / 1e6);
        if (n > 0 && static_cast<std::size_t>(n) < sizeof(line)) std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " tail=%.3f\n", static_cast<double>(totals.tail) / 1e6);
        AgcDriver::ProfilePrint_nid_no_patch("%s", line);
        totals = {};
    }
};

// One line per draw with the phases that took time (APS5_TRACE_DRAWS), and the 10 s totals.
// `waitedBefore` is the thread's GPU waits at the draw's start and `ownWaitedMs` the draw's own
// syncs (see DrawOutcome::hookWaitUs).
struct LastDrawPhases {
    bool set = false;
    std::array<double, PhaseCount> us{};
};

LastDrawPhases& lastDrawPhases() {
    struct LastStorage {};
    return AgcDriver::ThreadScratch<LastDrawPhases, LastStorage>();
}

void reportDrawEnd(const State& state, const DrawTimer& timer, const ShaderResources::BuildTiming* built, DrawOutcome& outcome, double waitedBefore, double ownWaitedMs, const char* suffix) {
    if (!timer.profile) return;
    lastDrawPhases() = {true, timer.us};
    static const bool traceDraws = std::getenv("APS5_TRACE_DRAWS") != nullptr;
    outcome.hookWaitUs = std::max(0.0, Recorder::ThreadWaitedMs() - waitedBefore - ownWaitedMs) * 1000.0;
    outcome.ownSyncUs = ownWaitedMs * 1000.0;
    if (traceDraws) {
        char line[512];
        int n = std::snprintf(line, sizeof(line), "[draw] %ux%u %zu targets%s:", state.renderExtent.width, state.renderExtent.height, state.colors.size(), suffix);
        for (std::size_t i = 0; i < PhaseCount && n > 0 && static_cast<std::size_t>(n) < sizeof(line); ++i) {
            if (timer.us[i] <= 0) continue;
            n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s=%.0fus", DrawPhaseNames[i], timer.us[i]);
        }
        std::fprintf(stderr, "%s\n", line);
    }
    reportDraw(timer.us, built, outcome);
}

}

DrawInputCopy CopyDrawInput(const Context& context, Recorder* recorder, std::uint64_t address, std::size_t bytes, std::size_t alignment, Recorder::SnapshotUse use) {
    const GuestMemory::Gen::ConsumerScope genConsumer(GuestMemory::Gen::Consumer::DrawInput);
    Require(use != Recorder::SnapshotUse::Storage, "a draw input is a vertex or index buffer");
    DrawInputCopy copy;
    if (recorder != nullptr && bytes != 0) {
        GuestMemory::FlushGpuWrites(address, bytes);
#ifdef _WIN32
        const bool owned = !RuntimeSwitches::Set(RuntimeSwitch::NoOwnedDrawInputs);
#else
        constexpr bool owned = false;
#endif
        if (owned) copy.buffer = recorder->ReusableOwnedDrawInput(address, bytes, use, &copy.derived);
        if (copy.buffer != nullptr) {
            copy.reused = true;
            return copy;
        }
        copy.registryGeneration = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        if (Recorder::ArmedSnapshotsActive()) {
            std::uint32_t derived = 0;
            if (auto armed = recorder->ArmedDrawSnapshot(address, bytes, use, &derived); armed != nullptr) {
                if (Recorder::ArmedSnapshotsVerify()) {
                    std::uint32_t slowDerived = 0;
                    const auto slow = GuestMemory::CollectWrites(address, bytes) != 0 ? recorder->ReusableDrawSnapshot(address, bytes, use, &slowDerived) : nullptr;
                    Recorder::NoteArmedSnapshotVerify(slow == armed && slowDerived == derived);
                }
                copy.buffer = std::move(armed);
                copy.derived = derived;
                copy.reused = true;
                return copy;
            }
        }
        const bool indexRefresh = use >= Recorder::SnapshotUse::Index16 && use <= Recorder::SnapshotUse::Index32Restart && AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixDerivedIndexCache);
        if (indexRefresh) {
            GuestMemory::PrepareDriverRead_nid_no_patch(address, bytes);
            copy.registryGeneration = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        }
        copy.generation = GuestMemory::CollectWrites(address, bytes);
        if (copy.generation != 0) copy.buffer = recorder->ReusableDrawSnapshot(address, bytes, use, &copy.derived, Recorder::ArmedSnapshotsActive() || indexRefresh ? copy.generation : 0);
        else if (owned) copy.owned = recorder->ArmOwnedDrawInput(address, bytes);
        if (copy.buffer != nullptr) {
            copy.reused = true;
            return copy;
        }
    }
    copy.buffer = std::make_shared<Buffer>(context, bytes, use == Recorder::SnapshotUse::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    GuestMemory::Read(address, copy.buffer->Bytes(), alignment);
    return copy;
}

DrawInputCopy CopyZeroPaddedDrawInput(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t validBytes, std::size_t stride) {
    Require(validBytes <= bytes, "the valid bytes of a vertex fetch exceed the fetch");
    auto readable = validBytes;
    if (stride != 0 && !GuestMemory::Accessible(reinterpret_cast<const void*>(address), validBytes)) {
        const auto ranges = GuestMemory::CommittedRanges(address, validBytes);
        const std::size_t prefix = !ranges.empty() && ranges.front().first == address ? static_cast<std::size_t>(ranges.front().second - address) : 0;
        if (validBytes - prefix < stride) readable = prefix;
    }
    if (readable != 0) GuestMemory::FlushGpuWrites(address, readable);
    DrawInputCopy copy;
    auto* recorder = context.recorder;
    const bool keep = recorder != nullptr && readable != 0 && readable <= std::numeric_limits<std::uint32_t>::max();
    if (keep) {
        copy.registryGeneration = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        copy.generation = GuestMemory::CollectWrites(address, readable);
        std::uint32_t keptReadable = 0;
        if (copy.generation != 0) copy.buffer = recorder->ReusableDrawSnapshot(address, bytes, Recorder::SnapshotUse::PaddedVertex, &keptReadable);
        if (copy.buffer != nullptr && keptReadable == readable) {
            copy.reused = true;
            return copy;
        }
        copy.buffer = nullptr;
    }
    copy.buffer = std::make_shared<Buffer>(context, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    const auto span = copy.buffer->Bytes();
    GuestMemory::Read(address, span.subspan(0, readable), 1);
    std::fill(span.begin() + static_cast<std::ptrdiff_t>(readable), span.end(), std::byte{0});
    if (keep) recorder->KeepDrawSnapshot(address, bytes, copy.generation, copy.registryGeneration, copy.buffer, Recorder::SnapshotUse::PaddedVertex, static_cast<std::uint32_t>(readable));
    return copy;
}

void KeepDrawInput(Recorder* recorder, std::uint64_t address, const DrawInputCopy& copy, Recorder::SnapshotUse use, std::uint32_t derived) {
    if (recorder == nullptr || copy.reused || copy.buffer == nullptr) return;
    if (copy.generation == 0) {
        if (!copy.owned.words.empty() && !RuntimeSwitches::Set(RuntimeSwitch::NoOwnedDrawInputs)) recorder->KeepOwnedDrawInput(address, copy.buffer->Bytes().size(), copy.owned, copy.buffer, use, derived);
        return;
    }
    recorder->KeepDrawSnapshot(address, copy.buffer->Bytes().size(), copy.generation, copy.registryGeneration, copy.buffer, use, derived);
}

const HostImport* InPlaceDrawInput(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t alignment, bool* behindGpuWrite) {
    auto* recorder = context.recorder;
    // APS5_ARCHB_ZERO_COPY_INPUTS binds them in place where the device copies them by default.
    if (recorder == nullptr || bytes == 0 || (!context.drawInputsInPlace && !Recorder::ZeroCopyInputsActive())) return nullptr;
    bool behind = false;
    if (behindGpuWrite == nullptr) {
        GuestMemory::FlushGpuWrites(address, bytes);
        if (recorder->PendingWriteOverlaps(address, bytes)) return nullptr;
    } else {
        StorageTexture::FlushPending(address, bytes, nullptr, "in-place vertex input");
        behind = recorder->PendingWriteOverlaps(address, bytes);
    }
    if (recorder->PendingLabelIn(address, bytes) || recorder->QueuedStoreOverlaps(address, bytes)) return nullptr;
    const auto overlaps = [&](const auto& writer) { return writer->WritesOverlap(address, bytes); };
    if (context.copiedWriters != nullptr && std::any_of(context.copiedWriters->begin(), context.copiedWriters->end(), overlaps)) return nullptr;
    const auto drawWriters = DrawCopiedWriters();
    if (std::any_of(drawWriters->begin(), drawWriters->end(), overlaps)) return nullptr;
    const auto* import = HostImportFor(context, address, bytes);
    if (import == nullptr || HostImportOffset(*import, address) % alignment != 0) return nullptr;
    recorder->NotePendingRead(address, bytes, Recorder::ReadKind::DrawInput);
    if (behindGpuWrite != nullptr && behind) *behindGpuWrite = true;
    if (!context.drawInputsInPlace) Recorder::CountZeroCopyInput(bytes);
    return import;
}

namespace {
// Zero-copy census (Recorder::ZeroCopyChecks): whether InPlaceDrawInput could have bound a copied
// range in place (an import at a usable offset, no queued label or store, no copied writer), without
// its side effects (no flush, no read note).
bool zeroCopyInputImportable(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t alignment) {
    auto* recorder = context.recorder;
    if (recorder == nullptr || bytes == 0) return false;
    if (recorder->PendingLabelIn(address, bytes) || recorder->QueuedStoreOverlaps(address, bytes)) return false;
    const auto overlaps = [&](const auto& writer) { return writer->WritesOverlap(address, bytes); };
    if (context.copiedWriters != nullptr && std::any_of(context.copiedWriters->begin(), context.copiedWriters->end(), overlaps)) return false;
    const auto drawWriters = DrawCopiedWriters();
    if (std::any_of(drawWriters->begin(), drawWriters->end(), overlaps)) return false;
    const auto* import = ExistingHostImportFor(context, address, bytes);
    return import != nullptr && HostImportOffset(*import, address) % alignment == 0;
}
}

InPlaceIndices InPlaceIndexRange(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint32_t indexSize, bool skipRestart) {
    const GuestMemory::Gen::ConsumerScope genConsumer(GuestMemory::Gen::Consumer::DrawInput);
    const std::span<const std::byte> range(reinterpret_cast<const std::byte*>(address), bytes);
    InPlaceIndices indices;
    if (bytes < Recorder::KeptIndexRangeIndices * indexSize) {
        indices.highest = HighestDrawIndex(range, indexSize, skipRestart);
        return indices;
    }
    const auto use = indexSize == 2 ? (skipRestart ? Recorder::SnapshotUse::Index16Restart : Recorder::SnapshotUse::Index16) : (skipRestart ? Recorder::SnapshotUse::Index32Restart : Recorder::SnapshotUse::Index32);
    const auto registryGeneration = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    const auto generation = GuestMemory::CollectWrites(address, bytes);
    if (generation != 0 && recorder.KnownHighestIndex(address, bytes, use, indices.highest)) {
        indices.reused = true;
        return indices;
    }
    indices.highest = HighestDrawIndex(range, indexSize, skipRestart);
    recorder.KeepHighestIndex(address, bytes, use, generation, registryGeneration, indices.highest);
    return indices;
}

namespace {

// The draw's inputs before its resources (prepareDrawInputs): the validated parameters, the index
// buffer copy with its highest index, the vertex buffer copies and their layout, the fragment
// outputs and the pipeline stages.
struct DrawInputs {
    // An empty auto draw: nothing to record.
    bool nothing = false;
    std::uint64_t indexBytes = 0;
    std::shared_ptr<Buffer> indices;
    VkBuffer indexHandle = VK_NULL_HANDLE;
    VkDeviceSize indexOffset = 0;
    std::uint32_t maxIndex = 0;
    std::optional<std::uint32_t> indexCount;
    std::uint32_t listRestartPrimitive = 0;
    VertexInputLayout vertexInput;
    std::shared_ptr<const VertexInputLayout> retainedVertexInput;
    std::vector<std::shared_ptr<Buffer>> vertexBuffers;
    std::vector<VkBuffer> vertexHandles;
    std::vector<VkDeviceSize> vertexOffsets;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> inPlaceReads;
    std::vector<std::size_t> inPlaceAlignments;
    std::vector<VkBuffer> inPlaceBuffers;
    std::vector<VkDeviceSize> inPlaceOffsets;
    bool vertexInputBehindGpuWrite = false;
    std::uint32_t fragmentOutputs = 0;
    VkPipelineStageFlags shaderStages = 0;
    std::uint32_t meshGroups = 0;
    std::uint32_t meshLayers = 1;
    std::uint32_t meshLayerStride = 0;
    std::vector<std::uint32_t> restartTable;
    std::uint32_t expandedIndexCount = 0;
    const VertexInputLayout& VertexLayout() const { return retainedVertexInput != nullptr ? *retainedVertexInput : vertexInput; }
};

bool StripPatches(const State& state, std::span<const CompiledShader> shaders) {
    return state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP && BarycentricPatches(state, shaders);
}

std::shared_ptr<Buffer> zeroVertexBuffer(const Context& context, std::size_t bytes) {
    auto zero = std::make_shared<Buffer>(context, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    std::fill(zero->Bytes().begin(), zero->Bytes().end(), std::byte{0});
    return zero;
}

bool resolveHandoff() {
    return AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::PipeResolveHandoff);
}

// Stage 4.2b: handed-over resolve answers taken by apply (fragment outputs, vertex layouts, finds).
struct HandoffCounters {
    std::atomic<std::uint64_t> outputs{0};
    std::atomic<std::uint64_t> vertexInputs{0};
    std::atomic<std::uint64_t> finds{0};
    std::atomic<std::uint64_t> findsStale{0};
};

HandoffCounters& handoffCounters() {
    static auto* made = [] {
        auto* counters = new HandoffCounters();
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_handoff_outputs", counters->outputs);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_handoff_vertex_inputs", counters->vertexInputs);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_handoff_finds", counters->finds);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_handoff_finds_stale", counters->findsStale);
        return counters;
    }();
    return *made;
}

// APS5_ARCHB_PRELOCK_FIND_HANDOFF (census 2): finds the locked draw took from its unlocked preparation,
// handed finds refused because the cache index changed, and in _VERIFY mode the finds repeated and
// those that answered another object.
struct PrelockFindCounters {
    std::atomic<std::uint64_t> handoffs{0};
    std::atomic<std::uint64_t> stale{0};
    std::atomic<std::uint64_t> verified{0};
    std::atomic<std::uint64_t> mismatches{0};
};

PrelockFindCounters& prelockFindCounters() {
    static auto* made = [] {
        auto* counters = new PrelockFindCounters();
        AgcDriver::ArchB::Recorder::RegisterCounter("prelock_find_handoffs", counters->handoffs);
        AgcDriver::ArchB::Recorder::RegisterCounter("prelock_find_stale", counters->stale);
        AgcDriver::ArchB::Recorder::RegisterCounter("prelock_find_verified", counters->verified);
        AgcDriver::ArchB::Recorder::RegisterCounter("prelock_find_mismatches", counters->mismatches);
        return counters;
    }();
    return *made;
}

bool prelockFindVerify() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_PRELOCK_FIND_HANDOFF_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// Arch C P3b (C_REPLAY_SLOT_INPUTS): a slot replay's validated fragment outputs and pipeline stages. The State
// and stages are those of the capture (the same decode, the same stage owners; a patched stage by its variant and
// words), so they replace the validation memo and the state's pure validation checks.
struct ReplayInputsHint {
    std::uint32_t fragmentOutputs = 0;
    VkPipelineStageFlags shaderStages = 0;
};

struct DerivedIndexKey {
    VkDevice device = VK_NULL_HANDLE;
    std::uint64_t address = 0;
    std::uint32_t count = 0;
    std::uint32_t indexSize = 0;
    bool quadList = false;
    bool lastProvoking = false;
    bool clamp = false;
    bool skipRestart = false;
    std::uint32_t limit = 0;
    bool operator==(const DerivedIndexKey&) const = default;
};

struct DerivedIndexKeyHash {
    std::size_t operator()(const DerivedIndexKey& key) const noexcept {
        std::uint64_t hash = 1469598103934665603ull;
        for (const std::uint64_t word : {static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(key.device)), key.address, static_cast<std::uint64_t>(key.count), static_cast<std::uint64_t>(key.indexSize) | (static_cast<std::uint64_t>(key.quadList) << 8u) | (static_cast<std::uint64_t>(key.lastProvoking) << 9u) | (static_cast<std::uint64_t>(key.clamp) << 10u) | (static_cast<std::uint64_t>(key.skipRestart) << 11u) | (static_cast<std::uint64_t>(key.limit) << 32u)}) hash = (hash ^ word) * 1099511628211ull;
        return static_cast<std::size_t>(hash);
    }
};

struct DerivedIndexEntry {
    std::shared_ptr<Buffer> buffer;
    std::uint32_t maxIndex = 0;
    std::uint64_t generation = 0;
    std::size_t size = 0;
};

struct DerivedIndexCache {
    VkDevice device = VK_NULL_HANDLE;
    std::unordered_map<DerivedIndexKey, DerivedIndexEntry, DerivedIndexKeyHash> entries;
    std::size_t bytes = 0;
    std::vector<std::byte> source;
};

constexpr std::size_t DerivedIndexCacheBytes = 256ull << 20u;

DerivedIndexCache& derivedIndexCache(VkDevice device) {
    struct CacheThreadStorage {};
    auto& cache = AgcDriver::ThreadScratch<DerivedIndexCache*, CacheThreadStorage>();
    if (cache == nullptr) cache = new DerivedIndexCache();
    if (cache->device != device) {
        if (!cache->entries.empty()) {
            auto* abandoned = new DerivedIndexCache();
            abandoned->entries.swap(cache->entries);
        }
        cache->bytes = 0;
        cache->device = device;
    }
    return *cache;
}

bool derivedIndexVerify() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_FIX_DERIVED_INDEX_CACHE_VERIFY");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return enabled;
}

// Draw's validation, index and vertex phases. With `recipe` the fragment outputs, the pipeline
// stages and the vertex input layout are the recipe's (derived from the same compiled stages)
// instead of computed.
struct DrawPlan {
    bool nothing = false;
    std::uint64_t indexBytes = 0;
    std::uint32_t fragmentOutputs = 0;
    VkPipelineStageFlags shaderStages = 0;
    std::uint32_t meshGroups = 0;
    std::uint32_t meshLayers = 1;
    std::uint32_t meshLayerStride = 0;
    VertexInputLayout vertexInput;
    std::shared_ptr<const VertexInputLayout> retainedVertexInput;
    bool candidate = false;
    bool keyed = false;
    ResourceCache::Key contentKey;
    bool preparedKey = false;
    const VertexInputLayout& VertexLayout() const { return retainedVertexInput != nullptr ? *retainedVertexInput : vertexInput; }
};

DrawPlan planDraw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, const DrawRecipe* recipe, DrawOutcome& outcome, DrawTimer& timer, DrawPreparation* preparation = nullptr, const ReplayInputsHint* hint = nullptr, bool keyResources = true) {
    const auto* handoff = recipe == nullptr && preparation != nullptr && preparation->device == context.device && preparation->resolved && resolveHandoff() ? preparation : nullptr;
    DrawPlan plan;
    APS5_LOG_OUT_DEBUG("Draw indices=%u instances=%u indexSize=%u flags=%u indexAddress=0x%llx", draw.indexCount, draw.instanceCount, draw.indexSize, draw.flags, static_cast<unsigned long long>(draw.indexAddress));
    APS5_LOG_OUT_DEBUG("State colorTarget=%u render=%ux%u colorAddress=0x%llx colorBytes=%llu colorExtent=%ux%u", state.hasColorTarget ? 1u : 0u, state.renderExtent.width, state.renderExtent.height, static_cast<unsigned long long>(state.color.address), static_cast<unsigned long long>(state.color.bytes), state.color.extent.width, state.color.extent.height);
    APS5_LOG_OUT_DEBUG("Viewport x=%f y=%f w=%f h=%f minDepth=%f maxDepth=%f", state.viewport.x, state.viewport.y, state.viewport.width, state.viewport.height, state.viewport.minDepth, state.viewport.maxDepth);
    APS5_LOG_OUT_DEBUG("Scissor x=%d y=%d w=%u h=%u topology=%u cullMode=0x%x frontFace=%u", state.scissor.offset.x, state.scissor.offset.y, state.scissor.extent.width, state.scissor.extent.height, static_cast<unsigned>(state.topology), static_cast<unsigned>(state.cullMode), static_cast<unsigned>(state.frontFace));
    const auto* args = draw.indirect ? &*draw.indirect : nullptr;
    Require(draw.indexed ? draw.flags == 0 : (draw.flags & ~0x20u) == 0, "draw modifiers are unsupported");
    if (draw.indexed) {
        Require(draw.indexSize == 2 || draw.indexSize == 4, "only uint16 and uint32 index buffers are supported");
    } else {
        Require(draw.indexAddress == 0 && draw.indexSize == 0, "auto draw must not reference an index buffer");
        if (args == nullptr) {
            if (draw.indexCount == 0 || draw.instanceCount == 0) {
                plan.nothing = true;
                return plan;
            }
            Require(draw.firstVertex <= std::numeric_limits<std::uint32_t>::max() - (draw.indexCount - 1u), "auto draw vertex range overflow");
            Require(draw.firstInstance <= std::numeric_limits<std::uint32_t>::max() - (draw.instanceCount - 1u), "auto draw instance range overflow");
        }
    }
    if (args == nullptr) {
        if (draw.indexCount == 0 || draw.instanceCount == 0) {
            plan.nothing = true;
            return plan;
        }
    } else Require((!state.stages.mesh || draw.indexed) && !state.stages.tessellation && !state.quadList, "indirect draw on a non-vertex path must be resolved by the driver");
    plan.indexBytes = static_cast<std::uint64_t>(draw.indexCount) * draw.indexSize;
    const auto indexBytes = plan.indexBytes;
    APS5_LOG_OUT_DEBUG("Index buffer bytes=%llu", static_cast<unsigned long long>(indexBytes));
    Require(indexBytes <= std::numeric_limits<std::size_t>::max(), "index buffer size overflow");
    Require(!draw.indexed || !state.hasColorTarget || draw.indexAddress + indexBytes <= state.color.MemoryAddress() || state.color.MemoryAddress() + state.color.bytes <= draw.indexAddress, "index buffer aliases the render target");
    APS5_LOG_CHARS_OUT_DEBUG("ValidateShaders");
    if (recipe != nullptr) {
        plan.fragmentOutputs = recipe->fragmentOutputs;
        plan.shaderStages = recipe->shaderStages;
    } else if (hint != nullptr) {
        plan.fragmentOutputs = hint->fragmentOutputs;
        plan.shaderStages = hint->shaderStages;
    } else if (handoff != nullptr && handoff->resolvedOutputsKnown) {
        plan.fragmentOutputs = handoff->resolvedOutputs;
        outcome.validateMemoized = true;
        outcome.validateHit = true;
        plan.shaderStages = PipelineStages(shaders);
        handoffCounters().outputs.fetch_add(1, std::memory_order_relaxed);
    } else {
        plan.fragmentOutputs = CachedFragmentOutputs(context, shaders, state, outcome.validateMemoized, outcome.validateHit);
        plan.shaderStages = PipelineStages(shaders);
    }
    APS5_LOG_CHARS_OUT_DEBUG("ValidateShaders OK");
    APS5_LOG_OUT_DEBUG("PipelineStages=0x%x", static_cast<unsigned>(plan.shaderStages));
    if (state.stages.mesh) {
        APS5_LOG_CHARS_OUT_DEBUG("Mesh path");
        Require(context.meshShader, "device does not support mesh shaders");
        const auto& mesh = *state.stages.mesh;
        const auto inputSize = mesh.inputPrimitive == 1 ? 1u : mesh.inputPrimitive == 2 ? 2u : 3u;
        Require(mesh.primitivesPerGroup != 0, "mesh pipeline groups no primitive (zero primitives per group)");
        Require(((state.stages.registerValue >> 19u) & 3u) == 0 || !draw.indexed, "indexed fast-launch draws are unsupported");
        if (args == nullptr && draw.indexCount < inputSize) {
            plan.nothing = true;
            return plan;
        }
        if (args == nullptr) {
            const auto step = mesh.inputPrimitive == 5 || mesh.inputPrimitive == 6 ? 1u : inputSize;
            const auto primitives = (draw.indexCount - inputSize) / step + 1u;
            const auto groups = (primitives - 1u) / mesh.primitivesPerGroup + 1u;
            const auto grid = MeshGridFor(context, groups, draw.instanceCount);
            Require(grid.has_value(), "mesh draw exceeds workgroup count limits");
            APS5_LOG_OUT_DEBUG("Mesh primitives=%u groups=%u as %u x %u", primitives, groups, grid->groups, grid->layers);
            plan.meshGroups = grid->groups;
            plan.meshLayers = grid->layers;
            plan.meshLayerStride = grid->layerStride;
        }
    }
    if (state.stages.tessellation) Require(draw.indexCount % state.stages.tessellation->inputControlPoints == 0, "incomplete tessellation patch");
    if (hint == nullptr) {
        ValidateViewport(context, state.viewport);
        ValidateDepthBounds(context, state);
        ValidateSampleMask(context, state, shaders);
    }
    timer.phase(PhaseValidate);
    const auto layoutStart = timer.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto& attributes = shaders.front().program->vertexAttributes;
    if (recipe != nullptr) {
        plan.vertexInput = recipe->vertexInput;
    } else if (!attributes.empty() && handoff != nullptr && handoff->resolvedVertexInput != nullptr) {
        plan.retainedVertexInput = handoff->resolvedVertexInput;
        handoffCounters().vertexInputs.fetch_add(1, std::memory_order_relaxed);
    } else if (!attributes.empty()) {
        if (context.vertexInputs != nullptr) plan.retainedVertexInput = context.vertexInputs->TryGet(shaders.front().program->variantId, attributes);
        if (plan.retainedVertexInput == nullptr) plan.vertexInput = BuildVertexInputLayout(context, attributes);
    }
    timer.charge(PhaseVertex, layoutStart);
    static const bool noDrawResourceCache = std::getenv("APS5_NO_DRAW_RESOURCE_CACHE") != nullptr;
    static const bool noTextureCache = std::getenv("APS5_NO_TEXTURE_CACHE") != nullptr;
    static const bool trimKey = std::getenv("APS5_NO_DRAW_KEY_TRIM") == nullptr;
    plan.candidate = recipe == nullptr && !noDrawResourceCache && !noTextureCache && std::all_of(shaders.begin(), shaders.end(), [](const CompiledShader& shader) { return shader.program != nullptr && shader.program->PipelineVariantId() != 0; });
    plan.keyed = keyResources && plan.candidate && !ShaderResources::NeverReusable(shaders);
    if (plan.keyed) {
        const auto keyStart = timer.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (preparation != nullptr && preparation->device == context.device && !preparation->resourceKey.empty()) {
            plan.preparedKey = true;
            plan.contentKey = std::exchange(preparation->resourceKey, {});
        } else plan.contentKey = DrawResourceKey(context, shaders, state.color, static_cast<std::uint32_t>(state.colors.size()), draw.indexAddress, indexBytes, !trimKey);
        timer.charge(PhaseLookup, keyStart);
    }
    return plan;
}

bool VerifyDrawPlan() {
    static const bool verify = std::getenv("APS5_VERIFY_DRAW_PLAN") != nullptr;
    return verify;
}

void requireDrawPlanRepeats(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, const DrawPlan& plan) {
    DrawOutcome scratch(false);
    DrawTimer scratchTimer(false);
    const auto again = planDraw(context, state, draw, shaders, nullptr, scratch, scratchTimer);
    const auto sameBinding = [](const VkVertexInputBindingDescription& a, const VkVertexInputBindingDescription& b) { return a.binding == b.binding && a.stride == b.stride && a.inputRate == b.inputRate; };
    const auto sameAttribute = [](const VkVertexInputAttributeDescription& a, const VkVertexInputAttributeDescription& b) { return a.location == b.location && a.binding == b.binding && a.format == b.format && a.offset == b.offset; };
    const auto& layout = plan.VertexLayout();
    const auto& repeated = again.VertexLayout();
    Require(again.nothing == plan.nothing && again.indexBytes == plan.indexBytes && again.fragmentOutputs == plan.fragmentOutputs && again.shaderStages == plan.shaderStages && again.meshGroups == plan.meshGroups && again.meshLayers == plan.meshLayers && again.meshLayerStride == plan.meshLayerStride && again.candidate == plan.candidate && again.keyed == plan.keyed && again.contentKey == plan.contentKey && std::ranges::equal(repeated.bindings, layout.bindings, sameBinding) && std::ranges::equal(repeated.attributes, layout.attributes, sameAttribute), "APS5_VERIFY_DRAW_PLAN: the plan built after the draw's record-time work differs from the plan the draw used");
}

DrawInputs prepareDrawInputs(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, const DrawPlan& drawPlan, DrawTimer& timer, PerformanceTimer* timing) {
    DrawInputs inputs;
    inputs.indexBytes = drawPlan.indexBytes;
    inputs.fragmentOutputs = drawPlan.fragmentOutputs;
    inputs.shaderStages = drawPlan.shaderStages;
    inputs.meshGroups = drawPlan.meshGroups;
    inputs.meshLayers = drawPlan.meshLayers;
    inputs.meshLayerStride = drawPlan.meshLayerStride;
    inputs.vertexInput = drawPlan.vertexInput;
    inputs.retainedVertexInput = drawPlan.retainedVertexInput;
    const auto* args = draw.indirect ? &*draw.indirect : nullptr;
    const auto indexBytes = inputs.indexBytes;
    if (draw.indexed) GuestMemory::CheckRange(reinterpret_cast<const void*>(draw.indexAddress), static_cast<std::size_t>(indexBytes), draw.indexSize);
    APS5_LOG_CHARS_OUT_DEBUG("Index buffer range OK");
    if (draw.indexed) RequireNoMultisampleTarget(draw.indexAddress, static_cast<std::size_t>(indexBytes), "an index buffer read");
    if (state.pixelKill && !state.pixelEarlyZ && std::ranges::any_of(state.colors, [](const ColorTarget& color) { return color.partialWriteMask != 0; })) {
        const auto* recorder = Recorder::Active();
        Require(recorder == nullptr || !recorder->CountingSamples(), "a partial write of a packed color target merges inside the pixel shader after early depth and stencil tests, which a shader that kills pixels under late tests cannot use while occlusion queries count samples: the early tests would count the pixels it kills");
    }
    timer.phase(PhaseValidate);
    if (timing != nullptr) timing->Mark("validate");
    ArchC::ProbeAt(ArchC::Probe::InValidate);
    inputs.maxIndex = draw.indexed ? 0u : draw.firstVertex + draw.indexCount - 1u;
    if (state.quadList || StripPatches(state, shaders)) {
        Require(!draw.indirect, state.quadList ? "indirect quad-list draw is unsupported" : "an indirect triangle strip through emulated barycentric patches is unsupported");
        Require(state.quadList || !draw.indexed || !state.primitiveRestart, "an indexed triangle strip with primitive restart through emulated barycentric patches is unsupported");
        const std::uint32_t primitives = state.quadList ? draw.indexCount / 4u : draw.indexCount >= 3u ? draw.indexCount - 2u : 0u;
        if (primitives == 0u || draw.instanceCount == 0u) {
            inputs.nothing = true;
            return inputs;
        }
        const std::uint32_t corners = state.quadList ? 6u : 3u;
        Require(primitives <= std::numeric_limits<std::uint32_t>::max() / corners, "expanded index count overflow");
        const std::uint32_t outputIndexCount = primitives * corners;
        const std::size_t expandedIndexBytes = static_cast<std::size_t>(outputIndexCount) * sizeof(std::uint32_t);
        std::uint32_t maxIdx = 0;
        const bool lastProvoking = state.provokingVertexMode == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
        const bool expandedCache = AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixDerivedIndexCache);
        const DerivedIndexKey expandedKey{context.device, draw.indexed ? draw.indexAddress : 0u, draw.indexCount, draw.indexed ? static_cast<std::uint32_t>(draw.indexSize) : 0u, state.quadList, lastProvoking};
        std::uint64_t expandedGeneration = 0;
        std::shared_ptr<Buffer> cachedIndices;
        if (expandedCache) {
            auto& cache = derivedIndexCache(context.device);
            if (draw.indexed) {
                GuestMemory::FlushGpuWrites(draw.indexAddress, static_cast<std::size_t>(indexBytes));
                expandedGeneration = GuestMemory::CollectWrites(draw.indexAddress, static_cast<std::size_t>(indexBytes));
            }
            if (draw.indexed && expandedGeneration == 0) {
                AgcDriver::ArchB::Recorder::Count("fix_dic_refused_untracked");
            } else if (const auto found = cache.entries.find(expandedKey); found != cache.entries.end()) {
                if (!draw.indexed || GuestMemory::UnchangedSince(draw.indexAddress, static_cast<std::size_t>(indexBytes), found->second.generation)) {
                    cachedIndices = found->second.buffer;
                    maxIdx = found->second.maxIndex;
                    AgcDriver::ArchB::Recorder::Count("fix_dic_hits");
                } else {
                    AgcDriver::ArchB::Recorder::Count("fix_dic_changed");
                }
            } else {
                AgcDriver::ArchB::Recorder::Count("fix_dic_absent");
            }
        }
        const bool verifyHit = cachedIndices != nullptr && derivedIndexVerify();
        auto indexCopy = cachedIndices != nullptr && !verifyHit ? cachedIndices : std::make_shared<Buffer>(context, expandedIndexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        auto* outIndices = reinterpret_cast<std::uint32_t*>(indexCopy->Bytes().data());
        const auto expand = [&](const auto& source) {
            for (std::uint32_t primitive = 0; primitive < primitives; ++primitive) {
                auto* out = outIndices + static_cast<std::size_t>(primitive) * corners;
                if (state.quadList) {
                    const auto i0 = source(primitive * 4u + 0u);
                    const auto i1 = source(primitive * 4u + 1u);
                    const auto i2 = source(primitive * 4u + 2u);
                    const auto i3 = source(primitive * 4u + 3u);
                    if (lastProvoking) {
                        out[0] = i0;
                        out[1] = i1;
                        out[2] = i3;
                        out[3] = i1;
                        out[4] = i2;
                        out[5] = i3;
                    } else {
                        out[0] = i0;
                        out[1] = i1;
                        out[2] = i2;
                        out[3] = i0;
                        out[4] = i2;
                        out[5] = i3;
                    }
                } else {
                    const std::uint32_t odd = primitive & 1u;
                    if (lastProvoking) {
                        out[0] = source(primitive + odd);
                        out[1] = source(primitive + 1u - odd);
                        out[2] = source(primitive + 2u);
                    } else {
                        out[0] = source(primitive);
                        out[1] = source(primitive + 1u + odd);
                        out[2] = source(primitive + 2u - odd);
                    }
                }
                for (std::uint32_t corner = 0; corner < corners; ++corner) maxIdx = std::max(maxIdx, out[corner]);
            }
        };
        const std::uint32_t cachedMax = maxIdx;
        if (verifyHit) maxIdx = 0;
        if (cachedIndices != nullptr && !verifyHit) {
            if (draw.indexed) GuestMemory::CheckRange(reinterpret_cast<const void*>(draw.indexAddress), static_cast<std::size_t>(indexBytes), draw.indexSize);
            Require(draw.firstVertex <= std::numeric_limits<std::uint32_t>::max() - maxIdx, "expanded draw vertex range overflow");
        } else if (draw.indexed && expandedCache) {
            GuestMemory::CheckRange(reinterpret_cast<const void*>(draw.indexAddress), static_cast<std::size_t>(indexBytes), draw.indexSize);
            auto& source = derivedIndexCache(context.device).source;
            source.resize(static_cast<std::size_t>(indexBytes));
            GuestMemory::Read(draw.indexAddress, std::span(source), draw.indexSize);
            if (draw.indexSize == 2) {
                expand([&](std::uint32_t idx) -> std::uint32_t {
                    std::uint16_t v = 0;
                    std::memcpy(&v, source.data() + static_cast<std::size_t>(idx) * 2u, 2);
                    return v;
                });
            } else {
                expand([&](std::uint32_t idx) -> std::uint32_t {
                    std::uint32_t v = 0;
                    std::memcpy(&v, source.data() + static_cast<std::size_t>(idx) * 4u, 4);
                    return v;
                });
            }
            Require(draw.firstVertex <= std::numeric_limits<std::uint32_t>::max() - maxIdx, "indexed draw vertex range overflow");
        } else if (draw.indexed) {
            GuestMemory::FlushGpuWrites(draw.indexAddress, static_cast<std::size_t>(indexBytes));
            GuestMemory::CheckRange(reinterpret_cast<const void*>(draw.indexAddress), static_cast<std::size_t>(indexBytes), draw.indexSize);
            const auto readIndex = [&](std::uint32_t idx) -> std::uint32_t {
                if (draw.indexSize == 2) {
                    std::uint16_t v = 0;
                    GuestMemory::Read(draw.indexAddress + static_cast<std::uint64_t>(idx) * 2u, std::as_writable_bytes(std::span(&v, 1)), 2);
                    return v;
                } else {
                    std::uint32_t v = 0;
                    GuestMemory::Read(draw.indexAddress + static_cast<std::uint64_t>(idx) * 4u, std::as_writable_bytes(std::span(&v, 1)), 4);
                    return v;
                }
            };
            expand(readIndex);
            Require(draw.firstVertex <= std::numeric_limits<std::uint32_t>::max() - maxIdx, "indexed draw vertex range overflow");
        } else {
            const std::uint32_t lastVertex = state.quadList ? primitives * 4u - 1u : primitives + 1u;
            Require(draw.firstVertex <= std::numeric_limits<std::uint32_t>::max() - lastVertex, "auto draw vertex range overflow");
            expand([](std::uint32_t idx) { return idx; });
        }
        if (verifyHit) {
            const bool same = maxIdx == cachedMax && std::memcmp(cachedIndices->Bytes().data(), indexCopy->Bytes().data(), expandedIndexBytes) == 0;
            AgcDriver::ArchB::Recorder::Count("fix_dic_verified");
            if (!same) AgcDriver::ArchB::Recorder::Count("fix_dic_verify_mismatches");
        }
        if (expandedCache && cachedIndices == nullptr && (!draw.indexed || expandedGeneration != 0)) {
            auto& cache = derivedIndexCache(context.device);
            if (cache.bytes + expandedIndexBytes > DerivedIndexCacheBytes) {
                cache.entries.clear();
                cache.bytes = 0;
                AgcDriver::ArchB::Recorder::Count("fix_dic_flushes");
            }
            auto [slot, inserted] = cache.entries.try_emplace(expandedKey);
            if (!inserted) cache.bytes -= slot->second.size;
            slot->second = DerivedIndexEntry{indexCopy, maxIdx, expandedGeneration, expandedIndexBytes};
            cache.bytes += expandedIndexBytes;
            AgcDriver::ArchB::Recorder::Count("fix_dic_stored");
        }
        inputs.maxIndex = maxIdx + draw.firstVertex;
        Require(inputs.maxIndex <= context.limits.maxDrawIndexedIndexValue, "index exceeds the device's indexed draw limit");
        inputs.indexHandle = indexCopy->Handle();
        inputs.indexOffset = 0;
        inputs.expandedIndexCount = outputIndexCount;
        inputs.indices = std::move(indexCopy);
    } else if (draw.indexed && state.primitiveRestart && !context.primitiveListRestart && (state.topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST || state.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)) {
        Require(!state.stages.mesh, "AGC graphics: primitive restart in a mesh list draw needs VK_EXT_primitive_topology_list_restart");
        if (indexBytes == 0) {
            inputs.nothing = true;
            return inputs;
        }
        GuestMemory::FlushGpuWrites(draw.indexAddress, static_cast<std::size_t>(indexBytes));
        auto compacted = std::make_shared<Buffer>(context, static_cast<std::size_t>(indexBytes), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        const auto bytes = compacted->Bytes().first(static_cast<std::size_t>(indexBytes));
        GuestMemory::Read(draw.indexAddress, bytes, draw.indexSize);
        const auto primitiveSize = state.topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ? 1u : state.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ? 2u : 3u;
        const auto count = draw.indirect ? draw.indexCount : CompactListRestart(bytes, draw.indexSize, primitiveSize);
        const auto kept = bytes.first(static_cast<std::size_t>(count) * draw.indexSize);
        const bool restartLeft = draw.indirect.has_value();
        auto highest = HighestDrawIndex(kept, draw.indexSize, restartLeft);
        constexpr std::uint32_t ZeroFetchSlack = 0x10000u;
        if (const auto zeroFrom = highest && !shaders.front().program->vertexAttributes.empty() ? ZeroFetchVertex(shaders.front().program->vertexAttributes) : std::nullopt) {
            const auto limit = *zeroFrom > draw.firstVertex ? *zeroFrom - draw.firstVertex : 0u;
            if (*highest >= limit && *highest - limit >= ZeroFetchSlack) highest = ClampDrawIndices(kept, draw.indexSize, restartLeft, limit);
        }
        if (!highest) {
            inputs.nothing = true;
            return inputs;
        }
        Require(*highest <= context.limits.maxDrawIndexedIndexValue, "index exceeds the device's indexed draw limit");
        inputs.maxIndex = *highest;
        inputs.indexCount = count;
        inputs.listRestartPrimitive = primitiveSize;
        inputs.indices = std::move(compacted);
        inputs.indexHandle = inputs.indices->Handle();
        inputs.indexOffset = 0;
    } else if (draw.indexed) {
        const bool listTopology = state.topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST || state.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        const bool skipRestart = state.primitiveRestart && (!listTopology || context.primitiveListRestart);
        const auto use = draw.indexSize == 2 ? (skipRestart ? Recorder::SnapshotUse::Index16Restart : Recorder::SnapshotUse::Index16) : (skipRestart ? Recorder::SnapshotUse::Index32Restart : Recorder::SnapshotUse::Index32);
        constexpr std::uint32_t ZeroFetchSlack = 0x10000u;
        std::shared_ptr<Buffer> zeroIndices;
        if (draw.indexSize == 4 && !skipRestart && !state.stages.mesh && draw.indexCount != 0 && !shaders.front().program->vertexAttributes.empty() && AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixDerivedIndexCache)) {
            if (const auto zeroFrom = ZeroFetchVertex(shaders.front().program->vertexAttributes); zeroFrom && *zeroFrom <= draw.firstVertex) {
                GuestMemory::FlushGpuWrites(draw.indexAddress, static_cast<std::size_t>(indexBytes));
                GuestMemory::CheckRange(reinterpret_cast<const void*>(draw.indexAddress), static_cast<std::size_t>(indexBytes), draw.indexSize);
                auto& cache = derivedIndexCache(context.device);
                constexpr std::size_t ProbeBytes = 64u << 10u;
                cache.source.resize(ProbeBytes);
                bool wide = false;
                for (std::size_t offset = 0; offset < static_cast<std::size_t>(indexBytes) && !wide; offset += ProbeBytes) {
                    const auto chunk = std::span(cache.source).first(std::min(ProbeBytes, static_cast<std::size_t>(indexBytes) - offset));
                    GuestMemory::Read(draw.indexAddress + offset, chunk, 4);
                    for (std::size_t i = 0; i + 4 <= chunk.size(); i += 4) {
                        std::uint32_t value;
                        std::memcpy(&value, chunk.data() + i, 4);
                        if (value >= ZeroFetchSlack) {
                            wide = true;
                            break;
                        }
                    }
                }
                if (wide) {
                    DerivedIndexKey zeroKey{context.device, 0u, draw.indexCount, 4u};
                    zeroKey.clamp = true;
                    if (const auto found = cache.entries.find(zeroKey); found != cache.entries.end()) {
                        zeroIndices = found->second.buffer;
                        AgcDriver::ArchB::Recorder::Count("fix_dic_zero_hits");
                    } else {
                        zeroIndices = std::make_shared<Buffer>(context, static_cast<std::size_t>(indexBytes), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
                        std::memset(zeroIndices->Bytes().data(), 0, static_cast<std::size_t>(indexBytes));
                        if (cache.bytes + static_cast<std::size_t>(indexBytes) > DerivedIndexCacheBytes) {
                            cache.entries.clear();
                            cache.bytes = 0;
                            AgcDriver::ArchB::Recorder::Count("fix_dic_flushes");
                        }
                        cache.entries[zeroKey] = DerivedIndexEntry{zeroIndices, 0u, 0u, static_cast<std::size_t>(indexBytes)};
                        cache.bytes += static_cast<std::size_t>(indexBytes);
                        AgcDriver::ArchB::Recorder::Count("fix_dic_zero_made");
                    }
                    if (derivedIndexVerify()) {
                        std::vector<std::byte> full(static_cast<std::size_t>(indexBytes));
                        GuestMemory::Read(draw.indexAddress, full, 4);
                        const auto clampedHighest = ClampDrawIndices(full, 4, false, 0u);
                        AgcDriver::ArchB::Recorder::Count("fix_dic_verified");
                        if (clampedHighest != std::optional<std::uint32_t>(0u) || std::memcmp(full.data(), zeroIndices->Bytes().data(), full.size()) != 0) AgcDriver::ArchB::Recorder::Count("fix_dic_verify_mismatches");
                    }
                } else {
                    AgcDriver::ArchB::Recorder::Count("fix_dic_zero_narrow");
                }
            }
        }
        const auto* import = zeroIndices != nullptr ? nullptr : InPlaceDrawInput(context, draw.indexAddress, static_cast<std::size_t>(indexBytes), draw.indexSize);
        DrawInputCopy copy;
        bool zeroCopyCopied = false;
        if (zeroIndices != nullptr) {
            copy.buffer = zeroIndices;
        } else if (import == nullptr) {
            copy = CopyDrawInput(context, context.recorder, draw.indexAddress, static_cast<std::size_t>(indexBytes), draw.indexSize, use);
            zeroCopyCopied = true;
        }
        else GuestMemory::CheckRange(reinterpret_cast<const void*>(draw.indexAddress), static_cast<std::size_t>(indexBytes), draw.indexSize);
        std::optional<std::uint32_t> highest;
        if (zeroIndices != nullptr) {
            highest = 0u;
        } else if (import != nullptr) {
            const bool unused = !skipRestart && draw.firstVertex == 0 && !state.stages.mesh && shaders.front().program->vertexAttributes.empty() && context.limits.maxDrawIndexedIndexValue >= (draw.indexSize == 2 ? 0xffffu : 0xffffffffu) && AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixDerivedIndexCache);
            if (unused) {
                highest = 0u;
                AgcDriver::ArchB::Recorder::Count("fix_dic_highest_unused");
            } else {
                highest = InPlaceIndexRange(*context.recorder, draw.indexAddress, static_cast<std::size_t>(indexBytes), draw.indexSize, skipRestart).highest;
            }
        } else if (!copy.reused) {
            highest = HighestDrawIndex(copy.buffer->Bytes().first(static_cast<std::size_t>(indexBytes)), draw.indexSize, skipRestart);
            KeepDrawInput(context.recorder, draw.indexAddress, copy, use, skipRestart ? (highest ? *highest + 1u : 0u) : highest.value_or(0u));
        } else if (!skipRestart) {
            highest = copy.derived;
        } else if (copy.derived != 0) {
            highest = copy.derived - 1u;
        }
        if (!highest) {
            inputs.nothing = true;
            return inputs;
        }
        if (const auto zeroFrom = state.stages.mesh || zeroIndices != nullptr || shaders.front().program->vertexAttributes.empty() ? std::nullopt : ZeroFetchVertex(shaders.front().program->vertexAttributes)) {
            const auto limit = *zeroFrom > draw.firstVertex ? *zeroFrom - draw.firstVertex : 0u;
            if (*highest >= limit && *highest - limit >= ZeroFetchSlack) {
                GuestMemory::FlushGpuWrites(draw.indexAddress, static_cast<std::size_t>(indexBytes));
                const bool clampCache = AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixDerivedIndexCache);
                DerivedIndexKey clampKey{context.device, draw.indexAddress, draw.indexCount, static_cast<std::uint32_t>(draw.indexSize)};
                clampKey.clamp = true;
                clampKey.skipRestart = skipRestart;
                clampKey.limit = limit;
                std::uint64_t clampGeneration = 0;
                std::shared_ptr<Buffer> cachedClamp;
                std::optional<std::uint32_t> cachedHighest;
                if (clampCache) {
                    auto& cache = derivedIndexCache(context.device);
                    clampGeneration = GuestMemory::CollectWrites(draw.indexAddress, static_cast<std::size_t>(indexBytes));
                    if (clampGeneration == 0) {
                        AgcDriver::ArchB::Recorder::Count("fix_dic_clamp_refused_untracked");
                    } else if (const auto found = cache.entries.find(clampKey); found != cache.entries.end()) {
                        if (GuestMemory::UnchangedSince(draw.indexAddress, static_cast<std::size_t>(indexBytes), found->second.generation)) {
                            cachedClamp = found->second.buffer;
                            cachedHighest = found->second.maxIndex;
                            AgcDriver::ArchB::Recorder::Count("fix_dic_clamp_hits");
                        } else {
                            AgcDriver::ArchB::Recorder::Count("fix_dic_clamp_changed");
                        }
                    } else {
                        AgcDriver::ArchB::Recorder::Count("fix_dic_clamp_absent");
                    }
                }
                const bool clampVerify = cachedClamp != nullptr && derivedIndexVerify();
                auto clamped = cachedClamp != nullptr && !clampVerify ? cachedClamp : std::make_shared<Buffer>(context, static_cast<std::size_t>(indexBytes), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
                if (cachedClamp != nullptr && !clampVerify) {
                    GuestMemory::CheckRange(reinterpret_cast<const void*>(draw.indexAddress), static_cast<std::size_t>(indexBytes), draw.indexSize);
                    highest = cachedHighest;
                } else {
                    const auto bytes = clamped->Bytes().first(static_cast<std::size_t>(indexBytes));
                    GuestMemory::Read(draw.indexAddress, bytes, draw.indexSize);
                    highest = ClampDrawIndices(bytes, draw.indexSize, skipRestart, limit);
                    if (clampVerify) {
                        AgcDriver::ArchB::Recorder::Count("fix_dic_verified");
                        if (highest != cachedHighest || std::memcmp(cachedClamp->Bytes().data(), bytes.data(), bytes.size()) != 0) AgcDriver::ArchB::Recorder::Count("fix_dic_verify_mismatches");
                    } else if (clampCache && clampGeneration != 0 && highest) {
                        auto& cache = derivedIndexCache(context.device);
                        if (cache.bytes + bytes.size() > DerivedIndexCacheBytes) {
                            cache.entries.clear();
                            cache.bytes = 0;
                            AgcDriver::ArchB::Recorder::Count("fix_dic_flushes");
                        }
                        auto [slot, inserted] = cache.entries.try_emplace(clampKey);
                        if (!inserted) cache.bytes -= slot->second.size;
                        slot->second = DerivedIndexEntry{clamped, *highest, clampGeneration, bytes.size()};
                        cache.bytes += bytes.size();
                        AgcDriver::ArchB::Recorder::Count("fix_dic_clamp_stored");
                    }
                }
                import = nullptr;
                zeroCopyCopied = false;
                copy = DrawInputCopy{};
                copy.buffer = std::move(clamped);
                if (!highest) {
                    inputs.nothing = true;
                    return inputs;
                }
            }
        }
        Require(*highest <= context.limits.maxDrawIndexedIndexValue, "index exceeds the device's indexed draw limit");
        inputs.maxIndex = *highest;
        if (skipRestart && state.stages.mesh && (state.stages.mesh->inputPrimitive == 5 || state.stages.mesh->inputPrimitive == 6)) {
            const auto indexSpan = import != nullptr ? std::span<const std::byte>(reinterpret_cast<const std::byte*>(draw.indexAddress), static_cast<std::size_t>(indexBytes)) : copy.buffer->Bytes().first(static_cast<std::size_t>(indexBytes));
            inputs.restartTable = MeshRestartTable(indexSpan, draw.indexSize);
        }
        if (import != nullptr) {
            inputs.indexHandle = import->buffer;
            inputs.indexOffset = HostImportOffset(*import, draw.indexAddress);
            inputs.inPlaceReads.emplace_back(draw.indexAddress, draw.indexAddress + indexBytes);
            inputs.inPlaceAlignments.push_back(draw.indexSize);
            inputs.inPlaceBuffers.push_back(inputs.indexHandle);
            inputs.inPlaceOffsets.push_back(inputs.indexOffset);
        } else {
            if (zeroCopyCopied && Recorder::ZeroCopyChecks() && context.recorder != nullptr) {
                Recorder::NoteZeroCopyImportable(Recorder::ZeroCopyKind::Index, static_cast<std::size_t>(indexBytes), zeroCopyInputImportable(context, draw.indexAddress, static_cast<std::size_t>(indexBytes), draw.indexSize));
                context.recorder->NoteZeroCopyCheck(draw.indexAddress, static_cast<std::size_t>(indexBytes), copy.buffer, 0, Recorder::ZeroCopyKind::Index);
            }
            inputs.indices = std::move(copy.buffer);
            inputs.indexHandle = inputs.indices->Handle();
        }
    }
    APS5_LOG_CHARS_OUT_DEBUG("Index validation OK");
    if (timing != nullptr) timing->Mark("index_upload");
    ArchC::ProbeAt(ArchC::Probe::InIndex);
    const auto& attributes = shaders.front().program->vertexAttributes;
    inputs.vertexOffsets.assign(attributes.size(), 0);
    inputs.vertexHandles.reserve(attributes.size());
    // An indexed draw's vertex offset moves every fetch: the copy must reach the last one.
    if (draw.indexed) {
        Require(draw.firstVertex <= std::numeric_limits<std::uint32_t>::max() - inputs.maxIndex, "indexed draw vertex range overflow");
        inputs.maxIndex += draw.firstVertex;
    }
    std::vector<VertexFetch> fetches;
    std::vector<std::size_t> fetchValid;
    fetches.reserve(attributes.size());
    fetchValid.reserve(attributes.size());
    std::vector<std::size_t> fetchOf(attributes.size(), 0);
    std::vector<std::shared_ptr<Buffer>> zeroed(attributes.size());
    const auto& layout = inputs.VertexLayout();
    for (std::size_t i = 0; i < attributes.size(); ++i) {
        const auto& attribute = attributes[i];
        const auto described = std::find_if(layout.attributes.begin(), layout.attributes.end(), [&](const auto& entry) { return entry.location == attribute.location; });
        Require(described != layout.attributes.end(), "vertex attribute missing from its input layout");
        const auto narrow = DecodeVertexFormat(attribute);
        const std::size_t widened = described->format == narrow.format ? 0u : VertexReadBytes(attribute, described->format) - narrow.bytes;
        if (NullVertexDescriptor(attribute) || VertexFetchOutOfRange(attribute)) {
            zeroed[i] = zeroVertexBuffer(context, narrow.bytes + widened);
            continue;
        }
        // An indirect draw's counts are unknown here: the descriptor's whole range is copied.
        const auto bytes = (args != nullptr ? VertexBufferExtent(attribute) : VertexBufferReadSize(attribute, inputs.maxIndex, draw.instanceCount, draw.firstInstance)) + widened;
        const auto& fields = attribute.resource.fields;
        const auto address = fields[0] | (static_cast<std::uint64_t>(fields[1] & 0xffffu) << 32u);
        const auto firstByte = args == nullptr ? VertexBufferReadOffset(attribute, draw.firstVertex, draw.firstInstance) : 0;
        Require(!state.hasColorTarget || address + bytes <= state.color.MemoryAddress() || state.color.MemoryAddress() + state.color.bytes <= address + firstByte, "vertex buffer aliases the render target");
        const auto stride = (fields[1] >> 16u) & 0x3fffu;
        fetchOf[i] = fetches.size();
        fetches.push_back({address, address + bytes, stride, attribute.fetchIndex, DecodeVertexFormat(attribute).alignment});
        const auto recordBytes = static_cast<std::uint64_t>(fields[2]) * stride;
        fetchValid.push_back(args == nullptr && stride != 0 ? static_cast<std::size_t>(std::min(recordBytes, static_cast<std::uint64_t>(bytes))) : bytes);
    }
    const auto soloFetches = SoloZeroPaddedFetchIndices(fetches, fetchValid);
    std::vector<bool> isSolo(fetches.size(), false);
    for (const auto f : soloFetches) isSolo[f] = true;
    std::vector<VertexFetch> plannedFetches;
    plannedFetches.reserve(fetches.size());
    std::vector<std::size_t> plannedOf(fetches.size(), 0);
    for (std::size_t f = 0; f < fetches.size(); ++f) {
        if (isSolo[f]) continue;
        plannedOf[f] = plannedFetches.size();
        plannedFetches.push_back(fetches[f]);
    }
    const auto plan = PlanVertexCopies(plannedFetches);
    std::vector<VkBuffer> rangeHandles;
    std::vector<VkDeviceSize> rangeOffsets;
    rangeHandles.reserve(plan.copies.size());
    rangeOffsets.reserve(plan.copies.size());
    for (std::size_t range = 0; range < plan.copies.size(); ++range) {
        const auto [begin, end] = plan.copies[range];
        const auto bytes = static_cast<std::size_t>(end - begin);
        GuestMemory::CheckRange(reinterpret_cast<const void*>(begin), bytes, 1);
        RequireNoMultisampleTarget(begin, bytes, "a vertex buffer read");
        if (const auto* import = InPlaceDrawInput(context, begin, bytes, plan.alignments[range], &inputs.vertexInputBehindGpuWrite)) {
            rangeHandles.push_back(import->buffer);
            rangeOffsets.push_back(HostImportOffset(*import, begin));
            inputs.inPlaceReads.emplace_back(begin, end);
            inputs.inPlaceAlignments.push_back(plan.alignments[range]);
            inputs.inPlaceBuffers.push_back(import->buffer);
            inputs.inPlaceOffsets.push_back(HostImportOffset(*import, begin));
            continue;
        }
        auto copy = CopyDrawInput(context, context.recorder, begin, bytes, 1, Recorder::SnapshotUse::Vertex);
        KeepDrawInput(context.recorder, begin, copy, Recorder::SnapshotUse::Vertex, 0);
        if (Recorder::ZeroCopyChecks() && context.recorder != nullptr) {
            Recorder::NoteZeroCopyImportable(Recorder::ZeroCopyKind::Vertex, bytes, zeroCopyInputImportable(context, begin, bytes, plan.alignments[range]));
            context.recorder->NoteZeroCopyCheck(begin, bytes, copy.buffer, 0, Recorder::ZeroCopyKind::Vertex);
        }
        rangeHandles.push_back(copy.buffer->Handle());
        rangeOffsets.push_back(0);
        inputs.vertexBuffers.push_back(std::move(copy.buffer));
    }
    std::vector<VkBuffer> soloHandles(fetches.size(), VK_NULL_HANDLE);
    for (const auto f : soloFetches) {
        const auto begin = fetches[f].begin;
        const auto bytes = static_cast<std::size_t>(fetches[f].end - begin);
        auto copy = CopyZeroPaddedDrawInput(context, begin, bytes, fetchValid[f], fetches[f].stride);
        soloHandles[f] = copy.buffer->Handle();
        inputs.vertexBuffers.push_back(std::move(copy.buffer));
    }
    for (std::size_t i = 0; i < attributes.size(); ++i) {
        if (zeroed[i] != nullptr) {
            inputs.vertexHandles.push_back(zeroed[i]->Handle());
            inputs.vertexOffsets[i] = 0;
            inputs.vertexBuffers.push_back(std::move(zeroed[i]));
            continue;
        }
        const auto f = fetchOf[i];
        if (isSolo[f]) {
            inputs.vertexHandles.push_back(soloHandles[f]);
            inputs.vertexOffsets[i] = 0;
            continue;
        }
        const auto j = plannedOf[f];
        inputs.vertexHandles.push_back(rangeHandles[plan.copyOf[j]]);
        inputs.vertexOffsets[i] = rangeOffsets[plan.copyOf[j]] + plan.offsets[j];
    }
    timer.phase(PhaseVertex);
    if (timing != nullptr) timing->Mark("vertex_upload");
    return inputs;
}

// A non-owning reference to a target lookup callable that outlives the call. Under
// APS5_ARCHB_FIX_ALLOC_CHURN it refers to the draw's lambda itself; otherwise to a std::function made
// from it, as before (its captures need a heap block once per color target).
class TargetLookup {
public:
    template<typename TLookup>
    explicit TargetLookup(const TLookup& lookup) : object(&lookup), call([](const void* target, StorageTexture::RefreshPreparation* preparation) { return std::shared_ptr<StorageTexture>((*static_cast<const TLookup*>(target))(preparation)); }) {}
    std::shared_ptr<StorageTexture> operator()(StorageTexture::RefreshPreparation* preparation) const { return call(object, preparation); }

private:
    const void* object;
    std::shared_ptr<StorageTexture> (*call)(const void*, StorageTexture::RefreshPreparation*);
};

template<typename TLookup, typename TRefresh>
std::shared_ptr<StorageTexture> withTargetLookup(const TLookup& lookup, const TRefresh& refresh) {
    if (AgcDriver::ArchB::AllocChurnActive()) return refresh(TargetLookup(lookup));
    const std::function<std::shared_ptr<StorageTexture>(StorageTexture::RefreshPreparation*)> owned = lookup;
    return refresh(TargetLookup(owned));
}

// The resident-target proof of one attachment (StorageTexture::Refresh: FlushPending, CollectWrites
// over the target's pages, the DCC key scan of TextureClearKeys, then UnchangedSince), with the
// [draws] target-lookup accounting. `lookup` makes (or finds) the image; DrawWithRecipe refreshes
// the stored object instead.
std::shared_ptr<StorageTexture> refreshResidentTarget(const Context& context, const State& state, const ColorTarget& color, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, DrawOutcome& outcome, bool profile, const TargetLookup& lookup, StorageTexture::RefreshPreparation* preparation) {
    const auto lookupStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto walkedBefore = profile ? GuestMemory::ThreadCollectedBytes() : 0;
    std::optional<LookupOutcomes> before;
    if (profile) before = ThreadLookupOutcomes();
#ifdef _WIN32
    const auto watchBefore = profile ? GuestArena::GuestArenaWriteWatchTiming_nid_no_patch() : GuestArena::WriteWatchTiming{};
#endif
    std::shared_ptr<StorageTexture> resident;
    try {
        if (profile) {
            StorageTexture::PendingDrawAccess access;
            access.fullScissor = state.scissor.offset.x <= 0 && state.scissor.offset.y <= 0 && std::int64_t{state.scissor.offset.x} + state.scissor.extent.width >= color.extent.width && std::int64_t{state.scissor.offset.y} + state.scissor.extent.height >= color.extent.height;
            const auto& blend = state.blends.at(color.exportIndex);
            access.blend = blend.blendEnable != 0;
            access.writeMask = blend.colorWriteMask;
            access.partialWriteMask = color.partialWriteMask;
            access.rop = state.colorRop;
            access.masked = blend.colorWriteMask != 0xfu || color.partialWriteMask != 0 || PartialSampleMask(state) || state.alphaToCoverage;
            access.depthStencil = state.depthTest || state.depthBoundsTest || state.stencilTest;
            access.kill = state.pixelKill;
            access.dccAddress = color.dccAddress;
            access.cmaskAddress = color.cmaskAddress;
            access.cmaskFastClear = color.cmaskFastClear;
            access.vertices = draw.indexCount;
            access.topology = static_cast<std::uint32_t>(state.topology);
            access.scissor = state.scissor;
            for (const auto& shader : shaders) {
                if (shader.program == nullptr) continue;
                if (shader.stage == ShaderRecompiler::ShaderStage::Vertex) access.vertexVariant = shader.program->PipelineVariantId();
                if (shader.stage == ShaderRecompiler::ShaderStage::Fragment) access.fragmentVariant = shader.program->PipelineVariantId();
            }
            const bool reads = access.blend || state.colorRop != CopyRop || color.partialWriteMask != 0;
            const auto surface = color.arrayLastSlice != 0 ? ArraySurfaceForTarget(color) : SurfaceForTarget(color);
            StorageTexture::ProfilePendingAccess(surface, SurfaceGuestBytes(surface), reads ? StorageTexture::PendingAccess::Read : StorageTexture::PendingAccess::Partial, &access);
        }
        resident = lookup(preparation);
        if (resident != nullptr) materializeRegisterClear(context, color, *resident);
    } catch (const std::exception& error) {
        static AgcDriver::Mutex reportMutex;
        static std::set<std::uint64_t> reported;
        std::lock_guard lock(reportMutex);
        if (reported.insert(color.address).second) std::fprintf(stderr, "[gpu] color target 0x%llx stays non-resident: %s\n", static_cast<unsigned long long>(color.address), error.what());
    }
    if (resident != nullptr) materializeCmaskClear(context, color, resident);
    if (profile) {
        const auto lookupUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - lookupStart).count();
        ++outcome.targetLookups;
        if (lookupUs >= 1000.0) {
            ++outcome.slowLookups;
            outcome.slowLookupUs += lookupUs;
        }
        outcome.lookupUs += lookupUs;
        const auto pages = (GuestMemory::ThreadCollectedBytes() - walkedBefore) / 4096;
        outcome.pagesWalked += pages;
        const auto& after = ThreadLookupOutcomes();
        constexpr std::array<LookupOutcomes::Kind, 6> kinds{LookupOutcomes::RefreshWait, LookupOutcomes::RefreshProofCheck, LookupOutcomes::RefreshCollect, LookupOutcomes::DccScan, LookupOutcomes::RefreshFlush, LookupOutcomes::RefreshValidate};
        std::array<double, 9> steps{};
        steps[8] = lookupUs / 1000;
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            steps[i] = after.ms[kinds[i]] - before->ms[kinds[i]];
            steps[8] -= steps[i];
        }
#ifdef _WIN32
        const auto watchAfter = GuestArena::GuestArenaWriteWatchTiming_nid_no_patch();
        steps[6] = (watchAfter.queryUs - watchBefore.queryUs) / 1000;
        steps[7] = (watchAfter.walkUs - watchBefore.walkUs) / 1000;
#endif
        auto& stats = Profile();
        {
            const std::lock_guard lock(stats.mutex);
            auto& target = stats.targets[{color.address, color.bytes, static_cast<std::uint64_t>(color.format), static_cast<std::uint64_t>(color.tileMode), color.dccAddress, color.layers, color.arraySlice, color.depth, color.mip, color.samples, color.extent.width, color.extent.height, color.pipeBankXor}];
            target.color = color;
            if (resident != nullptr) {
                target.storage = resident->Descriptor();
                target.storageBytes = resident->GuestBytes();
                target.storageLayers = resident->ImageLayers();
            }
            ++target.lookups;
            target.us += lookupUs;
            target.pages += pages;
            target.clean += after.counts[LookupOutcomes::CleanProof] - before->counts[LookupOutcomes::CleanProof];
            for (std::size_t i = 0; i < target.work.size(); ++i) target.work[i] += after.work[i] - before->work[i];
            target.flushGpuMs += after.ms[LookupOutcomes::FlushGpuWait] - before->ms[LookupOutcomes::FlushGpuWait];
            target.flushDepthMs += after.ms[LookupOutcomes::FlushDepth] - before->ms[LookupOutcomes::FlushDepth];
            for (std::size_t i = 0; i < steps.size(); ++i) target.steps[i] += steps[i];
            if (lookupUs >= 1000) {
                ++target.slow;
                target.slowUs += lookupUs;
                for (std::size_t i = 0; i < steps.size(); ++i) target.slowSteps[i] += steps[i];
            }
        }
        const bool fullScissor = state.scissor.offset.x <= 0 && state.scissor.offset.y <= 0 && state.scissor.extent.width >= color.extent.width && state.scissor.extent.height >= color.extent.height;
        ++(fullScissor ? outcome.fullScissorLookups : outcome.partialLookups);
    }
    return resident;
}

namespace archcTargets {

using TargetKey = std::array<std::uint64_t, 16>;

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

TargetKey keyOf(const Context& context, const ColorTarget& color) {
    TargetKey key{};
    key[0] = color.address;
    key[1] = (std::uint64_t{color.extent.width} << 32) | color.extent.height;
    key[2] = (static_cast<std::uint64_t>(color.format) << 32) | (static_cast<std::uint64_t>(color.tileMode) << 16) | color.elementBytes;
    key[3] = color.bytes;
    key[4] = color.dccAddress;
    key[5] = color.chainDccAddress ^ (std::uint64_t{color.dccAlphaOnMsb} << 62) ^ (std::uint64_t{color.dccPipeAligned} << 63);
    key[6] = color.cmaskAddress;
    key[7] = (static_cast<std::uint64_t>(color.cmaskBytes) << 1) | (color.cmaskFastClear ? 1u : 0u);
    key[8] = color.surfaceAddress;
    key[9] = (std::uint64_t{color.surfaceExtent.width} << 32) | color.surfaceExtent.height;
    key[10] = (std::uint64_t{color.mipCount} << 40) | (std::uint64_t{color.mip} << 8) | (color.mipTail ? 1u : 0u);
    key[11] = (std::uint64_t{color.clearWords[0]} << 32) | color.clearWords[1];
    key[12] = (std::uint64_t{color.depth} << 32) | color.depthSlice;
    key[13] = color.arrayAddress ^ (std::uint64_t{color.arrayLastSlice} << 48) ^ (std::uint64_t{color.arraySlice} << 32) ^ (std::uint64_t{color.layers} << 16);
    key[14] = (std::uint64_t{color.pipeBankXor} << 40) | (std::uint64_t{color.samples} << 24) | (std::uint64_t{color.coverageSamples} << 16) | (color.dccControl & 0xffffu);
    key[15] = color.fmaskAddress ^ reinterpret_cast<std::uintptr_t>(context.device);
    return key;
}

struct Token {
    TargetKey key{};
    bool used = false;
    std::weak_ptr<StorageTexture> image;
    const StorageTexture* raw = nullptr;
    std::uint64_t epoch = 0;
    std::uint64_t loss = 0;
    std::uint64_t surfaceSerial = 0;
    bool valid = false;
    std::uint64_t clock = 0;
    std::uint64_t generation = 0;
    std::uint64_t pendingSerial = 0;
    ArchC::EpochCensus census{};
    std::array<ArchC::EpochRange, 3> ranges{};
    std::size_t rangeCount = 0;
    bool registered = false;
    bool cmaskRegistered = false;
    std::uint64_t lastUse = 0;
    std::uint64_t touched = ~std::uint64_t{0};
};

constexpr std::size_t TableSets = 128;
constexpr std::size_t TableWays = 4;

struct TokenTable {
    std::array<Token, TableSets * TableWays> slots{};
    std::uint64_t uses = 0;
};

TokenTable& table() {
    struct TableStorage {};
    return HostThreadLocal<TokenTable, TableStorage>();
}

Token& slotFor(const TargetKey& key) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto word : key) {
        hash ^= word;
        hash *= 0x100000001b3ull;
        hash ^= hash >> 29;
    }
    auto& made = table();
    auto* set = &made.slots[static_cast<std::size_t>(hash % TableSets) * TableWays];
    Token* victim = set;
    for (std::size_t way = 0; way < TableWays; ++way) {
        auto& slot = set[way];
        if (slot.used && slot.key == key) {
            slot.lastUse = ++made.uses;
            return slot;
        }
        if (!slot.used) victim = victim->used ? &slot : victim;
        else if (victim->used && slot.lastUse < victim->lastUse) victim = &slot;
    }
    return *victim;
}

std::size_t rangesOf(const StorageTexture& image, const ColorTarget& color, std::array<ArchC::EpochRange, 3>& ranges) {
    const auto& descriptor = image.Descriptor();
    std::size_t used = 0;
    ranges[used++] = {descriptor.baseAddress, descriptor.baseAddress + image.GuestBytes()};
    if (descriptor.dccAddress != 0) ranges[used++] = {descriptor.dccAddress, descriptor.dccAddress + DccKeyBytes(image.GuestBytes())};
    if (color.cmaskAddress != 0 && color.cmaskBytes != 0) ranges[used++] = {color.cmaskAddress, color.cmaskAddress + color.cmaskBytes};
    return used;
}

bool rangesArmed(std::span<const ArchC::EpochRange> ranges, std::uint64_t clock) {
    for (const auto& range : ranges) {
        if (GuestArena::GuestWtFaultChangedSince_nid_no_patch(static_cast<std::uintptr_t>(range.begin), static_cast<std::size_t>(range.end - range.begin), clock) != GuestArena::WtFaultAnswer::Unchanged) return false;
    }
    return true;
}

enum Refusal : std::size_t { RefusedNew, RefusedImage, RefusedCached, RefusedKeys, RefusedCpu, RefusedStamp, RefusedPending, RefusedDepth, RefusedGpuNote, RefusedCmask, RefusedFmask, RefusedArray, RefusedMsaa, RefusalCount };
constexpr std::array<const char*, RefusalCount> RefusalNames{"new", "image", "cached", "keys", "cpu", "stamp", "pending", "depth", "gpunote", "cmask", "fmask", "array", "msaa"};

struct CensusCounters {
    std::atomic<std::uint64_t> targetNs{0}, prelockNs{0}, depthNs{0}, lookups{0}, wouldhit{0}, wouldhitNs{0}, wouldhitPush{0}, wouldhitPushNs{0}, wouldhitPushCmask{0}, checkNs{0};
    std::atomic<std::uint64_t> selfStamps{0}, otherStamps{0}, selfNotes{0}, otherNotes{0}, pushPending{0}, pushDepth{0}, recipeLookups{0};
    std::array<std::atomic<std::uint64_t>, RefusalCount> refused{};
    CensusCounters() {
        using AgcDriver::ArchB::Recorder;
        Recorder::RegisterCounter("archc_census_target_ns", targetNs);
        Recorder::RegisterCounter("archc_census_prelock_target_ns", prelockNs);
        Recorder::RegisterCounter("archc_census_target_depth_ns", depthNs);
        Recorder::RegisterCounter("archc_census_target_lookups", lookups);
        Recorder::RegisterCounter("archc_census_target_recipe_lookups", recipeLookups);
        Recorder::RegisterCounter("archc_census_target_wouldhit", wouldhit);
        Recorder::RegisterCounter("archc_census_target_wouldhit_ns", wouldhitNs);
        Recorder::RegisterCounter("archc_census_target_wouldhit_push", wouldhitPush);
        Recorder::RegisterCounter("archc_census_target_wouldhit_push_ns", wouldhitPushNs);
        Recorder::RegisterCounter("archc_census_target_wouldhit_push_cmask", wouldhitPushCmask);
        Recorder::RegisterCounter("archc_census_check_ns", checkNs);
        Recorder::RegisterCounter("archc_census_self_stamps", selfStamps);
        Recorder::RegisterCounter("archc_census_other_stamps", otherStamps);
        Recorder::RegisterCounter("archc_census_self_notes", selfNotes);
        Recorder::RegisterCounter("archc_census_other_notes", otherNotes);
        Recorder::RegisterCounter("archc_census_push_pending", pushPending);
        Recorder::RegisterCounter("archc_census_push_depth", pushDepth);
        for (std::size_t i = 0; i < RefusalCount; ++i) Recorder::RegisterCounter((new std::string(std::string("archc_census_refused_") + RefusalNames[i]))->c_str(), refused[i]);
    }
};

CensusCounters& censusCounters() {
    static CensusCounters made;
    return made;
}

bool censusActive() {
    return ArchC::ImageEpochs::CensusEnabled() && GuestMemory::IsCommitterThread();
}

std::array<std::uint64_t, 8>& prelockNs() {
    struct MadeStorage {};
    return AgcDriver::ThreadScratch<std::array<std::uint64_t, 8>, MadeStorage>();
}

class CensusProbe {
public:
    CensusProbe(const Context& context, const ColorTarget& color, bool recipe) : context(context), color(color), recipe(recipe) {
        if (!censusActive()) return;
        active = true;
        const auto start = nowNs();
        key = keyOf(context, color);
        slot = &slotFor(key);
        refusal = classify();
        checkNs += nowNs() - start;
    }

    void After(const std::shared_ptr<StorageTexture>& resident, std::uint64_t blockNs, std::uint64_t extraNs) {
        if (!active) return;
        const auto start = nowNs();
        auto& counters = censusCounters();
        if (resident != nullptr && (refusal == RefusalCount || pushHit) && resident.get() != current.get()) {
            refusal = RefusedImage;
            pushHit = false;
        }
        counters.lookups.fetch_add(1, std::memory_order_relaxed);
        if (recipe) counters.recipeLookups.fetch_add(1, std::memory_order_relaxed);
        counters.targetNs.fetch_add(blockNs, std::memory_order_relaxed);
        if (refusal == RefusalCount) {
            counters.wouldhit.fetch_add(1, std::memory_order_relaxed);
            counters.wouldhitNs.fetch_add(blockNs + extraNs, std::memory_order_relaxed);
        } else {
            counters.refused[refusal].fetch_add(1, std::memory_order_relaxed);
        }
        if (pushHit) {
            counters.wouldhitPush.fetch_add(1, std::memory_order_relaxed);
            counters.wouldhitPushNs.fetch_add(blockNs + extraNs, std::memory_order_relaxed);
            if (color.cmaskAddress != 0) counters.wouldhitPushCmask.fetch_add(1, std::memory_order_relaxed);
        }
        if (resident == nullptr) {
            *slot = Token{};
        } else {
            if (!ArchC::ImageEpochs::Get().Registered(*resident) || color.cmaskAddress != 0) ArchC::ImageEpochs::RegisterImage(*resident, color.cmaskAddress, color.cmaskBytes);
            auto& token = *slot;
            token = Token{};
            token.key = key;
            token.used = true;
            token.lastUse = ++table().uses;
            token.image = resident;
            token.raw = resident.get();
            token.epoch = resident->archcEpoch.load(std::memory_order_acquire);
            token.clock = GuestArena::GuestWtFaultClock_nid_no_patch();
            token.generation = GuestMemory::TrackerGeneration();
            token.pendingSerial = StorageTexture::PendingSerial();
            token.census = ArchC::ImageEpochs::Get().Census(*resident);
            token.rangeCount = rangesOf(*resident, color, token.ranges);
        }
        counters.checkNs.fetch_add(checkNs + nowNs() - start, std::memory_order_relaxed);
    }

    bool Active() const { return active; }

private:
    Refusal classify() {
        auto& counters = censusCounters();
        if (color.samples > 1) return RefusedMsaa;
        if (color.arrayLastSlice != 0) return RefusedArray;
        if (color.fmaskAddress != 0) return RefusedFmask;
        if (!slot->used || slot->key != key) return RefusedNew;
        current = slot->image.lock();
        if (current == nullptr || current.get() != slot->raw) return RefusedImage;
        if (!current->Cached()) return RefusedCached;
        if (!StorageImageServesKeys(*current, SurfaceForTarget(color).dccAddress)) return RefusedKeys;
        const auto ranges = std::span<const ArchC::EpochRange>(slot->ranges.data(), slot->rangeCount);
        const bool armed = rangesArmed(ranges, slot->clock);
        const auto census = ArchC::ImageEpochs::Get().Census(*current);
        if (census.otherStamps != slot->census.otherStamps) counters.otherStamps.fetch_add(1, std::memory_order_relaxed);
        else if (census.selfStamps != slot->census.selfStamps) counters.selfStamps.fetch_add(1, std::memory_order_relaxed);
        if (census.otherNotes != slot->census.otherNotes) counters.otherNotes.fetch_add(1, std::memory_order_relaxed);
        else if (census.selfNotes != slot->census.selfNotes) counters.selfNotes.fetch_add(1, std::memory_order_relaxed);
        if (census.pending != slot->census.pending) counters.pushPending.fetch_add(1, std::memory_order_relaxed);
        if (census.depth != slot->census.depth) counters.pushDepth.fetch_add(1, std::memory_order_relaxed);
        pushHit = armed && current->archcEpoch.load(std::memory_order_acquire) == slot->epoch;
        if (!armed) return RefusedCpu;
        std::array<GuestMemory::UnchangedQuery, 3> queries{};
        for (std::size_t i = 0; i < ranges.size(); ++i) queries[i] = {ranges[i].begin, static_cast<std::size_t>(ranges[i].end - ranges[i].begin), slot->generation};
        if (!GuestMemory::UnchangedSinceAll(std::span<const GuestMemory::UnchangedQuery>(queries.data(), ranges.size()))) return RefusedStamp;
        if (StorageTexture::PendingSerial() != slot->pendingSerial) {
            std::array<StorageTexture::PendingQuery, 1> pending{{{ranges[0].begin, ranges[0].end, current.get(), nullptr, false}}};
            StorageTexture::ScanPending(pending);
            if (pending[0].overlaps) return RefusedPending;
        }
        if (census.depth != slot->census.depth) return RefusedDepth;
        if (auto* recorder = Recorder::Active(); recorder != nullptr) {
            for (const auto& range : ranges) {
                if (recorder->PendingWriteOverlaps(range.begin, static_cast<std::size_t>(range.end - range.begin))) return RefusedGpuNote;
            }
        }
        if (color.cmaskAddress != 0) return RefusedCmask;
        return RefusalCount;
    }

    const Context& context;
    const ColorTarget& color;
    bool recipe = false;
    bool active = false;
    bool pushHit = false;
    TargetKey key{};
    Token* slot = nullptr;
    std::shared_ptr<StorageTexture> current;
    Refusal refusal = RefusalCount;
    std::uint64_t checkNs = 0;
};

enum FastRefusal : std::size_t { FastNone, FastImage, FastCached, FastKeys, FastEpoch, FastLoss, FastCmask, FastFmask, FastArray, FastMsaa, FastSurface, FastDrain, FastLinear, FastRefusalCount };
constexpr std::array<const char*, FastRefusalCount> FastRefusalNames{"none", "image", "cached", "keys", "epoch", "loss", "cmask", "fmask", "array", "msaa", "surface", "drain", "linear"};
enum TokenRefusal : std::size_t { TokenImage, TokenEpoch, TokenOpen, TokenLoss, TokenSurface, TokenDrain, TokenPending, TokenDepth, TokenRefusalCount };
constexpr std::array<const char*, TokenRefusalCount> TokenRefusalNames{"image", "epoch", "open", "loss", "surface", "drain", "pending", "depth"};

struct FastCounters {
    std::atomic<std::uint64_t> lookups{0}, hits{0}, tokens{0}, verifyChecks{0}, verifyMismatches{0}, prelockSkips{0}, legacyPasses{0};
    std::array<std::atomic<std::uint64_t>, FastRefusalCount> refused{};
    std::array<std::atomic<std::uint64_t>, TokenRefusalCount> tokenRefused{};
    FastCounters() {
        using AgcDriver::ArchB::Recorder;
        Recorder::RegisterCounter("archc_target_fast_lookups", lookups);
        Recorder::RegisterCounter("archc_target_fast_hits", hits);
        Recorder::RegisterCounter("archc_target_tokens", tokens);
        Recorder::RegisterCounter("archc_target_legacy_passes", legacyPasses);
        Recorder::RegisterCounter("archc_target_verify_checks", verifyChecks);
        Recorder::RegisterCounter("archc_target_verify_mismatches", verifyMismatches);
        Recorder::RegisterCounter("archc_target_prelock_skips", prelockSkips);
        for (std::size_t i = 0; i < FastRefusalCount; ++i) Recorder::RegisterCounter((new std::string(std::string("archc_target_fast_refused_") + FastRefusalNames[i]))->c_str(), refused[i]);
        for (std::size_t i = 0; i < TokenRefusalCount; ++i) Recorder::RegisterCounter((new std::string(std::string("archc_target_token_refused_") + TokenRefusalNames[i]))->c_str(), tokenRefused[i]);
    }
};

FastCounters& fastCounters() {
    static FastCounters made;
    return made;
}

bool fastActive() {
    return AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::ArchcTargetFastpath) && AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::ArchcImageEpochs) && GuestMemory::IsCommitterThread();
}

std::uint64_t verifyPeriod() {
    static const std::uint64_t period = [] {
        const char* value = std::getenv("APS5_ARCHB_C_TARGET_FASTPATH_VERIFY");
        if (value == nullptr) return std::uint64_t{0};
        const auto parsed = std::strtoull(value, nullptr, 10);
        return static_cast<std::uint64_t>(parsed);
    }();
    return period;
}

bool fastVerify() {
    return verifyPeriod() == 1;
}

bool verifyHitNow() {
    const auto period = verifyPeriod();
    if (period == 0) return false;
    struct HitsThreadStorage {};
    auto& hits = AgcDriver::ThreadScratch<std::uint64_t, HitsThreadStorage>();
    return ++hits % period == 0;
}

bool admitCmask() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_C_TARGET_FASTPATH_CMASK");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

std::uint64_t dccOf(const ColorTarget& color) {
    return color.dccAddress != 0 ? color.dccAddress : color.chainDccAddress;
}

FastRefusal shapeRefusal(const ColorTarget& color) {
    if (color.samples > 1) return FastMsaa;
    if (color.arrayLastSlice != 0) return FastArray;
    if (color.fmaskAddress != 0) return FastFmask;
    if (color.cmaskAddress != 0 && !admitCmask()) return FastCmask;
    if (color.tileMode == ColorTileMode::Linear) return FastLinear;
    return FastRefusalCount;
}

Token* holdingToken(const Context& context, const ColorTarget& color, FastRefusal& refusal, std::shared_ptr<StorageTexture>& image) {
    refusal = shapeRefusal(color);
    if (refusal != FastRefusalCount) return nullptr;
    if (!ArchC::ImageEpochs::Get().DrainIfNew()) {
        refusal = FastDrain;
        return nullptr;
    }
    const auto key = keyOf(context, color);
    auto& slot = slotFor(key);
    if (!slot.used || !slot.valid || slot.key != key) {
        refusal = FastNone;
        return nullptr;
    }
    image = slot.image.lock();
    if (image == nullptr || image.get() != slot.raw) {
        slot.valid = false;
        refusal = FastImage;
        return nullptr;
    }
    if (image->archcEpoch.load(std::memory_order_acquire) != slot.epoch) {
        slot.valid = false;
        refusal = FastEpoch;
        return nullptr;
    }
    if (ArchC::ImageEpochs::Get().Loss() != slot.loss) {
        slot.valid = false;
        refusal = FastLoss;
        return nullptr;
    }
    if (StorageSurfaceSerial() != slot.surfaceSerial) {
        slot.valid = false;
        refusal = FastSurface;
        return nullptr;
    }
    return &slot;
}

std::shared_ptr<StorageTexture> tryTarget(const Context& context, const ColorTarget& color, const StorageTexture* stored) {
    auto& counters = fastCounters();
    counters.lookups.fetch_add(1, std::memory_order_relaxed);
    FastRefusal refusal = FastRefusalCount;
    std::shared_ptr<StorageTexture> image;
    auto* slot = holdingToken(context, color, refusal, image);
    if (slot != nullptr && stored != nullptr && stored != image.get()) {
        slot = nullptr;
        refusal = FastImage;
    }
    if (slot != nullptr && !StorageImageServesKeys(*image, dccOf(color))) {
        slot = nullptr;
        refusal = FastKeys;
    }
    if (slot != nullptr && stored == nullptr) {
        const auto present = Recorder::Presents();
        const bool cached = slot->touched == present ? image->Cached() : StorageImageCached(context, image.get());
        if (!cached) {
            slot->valid = false;
            slot = nullptr;
            refusal = FastCached;
        } else {
            slot->touched = present;
        }
    }
    if (slot == nullptr) {
        counters.refused[refusal].fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    image->NoteProved();
    counters.hits.fetch_add(1, std::memory_order_relaxed);
    return image;
}

bool tokenHolds(const Context& context, const ColorTarget& color) {
    FastRefusal refusal = FastRefusalCount;
    std::shared_ptr<StorageTexture> image;
    return holdingToken(context, color, refusal, image) != nullptr && StorageImageServesKeys(*image, dccOf(color));
}

class LegacyPass {
public:
    LegacyPass(const Context& context, const ColorTarget& color) : context(context), color(color) {
        if (!fastActive() || shapeRefusal(color) != FastRefusalCount) return;
        active = true;
        fastCounters().legacyPasses.fetch_add(1, std::memory_order_relaxed);
        key = keyOf(context, color);
        slot = &slotFor(key);
        if (slot->used && slot->key == key) before = slot->image.lock();
        if (before != nullptr && before.get() == slot->raw && (!slot->registered || (color.cmaskAddress != 0 && !slot->cmaskRegistered))) {
            ArchC::ImageEpochs::RegisterImage(*before, color.cmaskAddress, color.cmaskBytes);
            slot->registered = true;
            slot->cmaskRegistered = color.cmaskAddress != 0;
        }
        drained = ArchC::ImageEpochs::Get().DrainIfNew();
        if (before != nullptr) epochBefore = before->archcEpoch.load(std::memory_order_acquire);
        lossBefore = ArchC::ImageEpochs::Get().Loss();
        serialBefore = StorageSurfaceSerial();
    }

    void Finish(const std::shared_ptr<StorageTexture>& resident) {
        if (!active) return;
        auto& counters = fastCounters();
        auto& token = *slot;
        if (resident == nullptr) {
            token = Token{};
            return;
        }
        if (!token.used || token.key != key || token.raw != resident.get()) {
            token = Token{};
            token.key = key;
            token.used = true;
            token.lastUse = ++table().uses;
            token.image = resident;
            token.raw = resident.get();
        }
        token.valid = false;
        const auto refuse = [&](TokenRefusal refusal) { counters.tokenRefused[refusal].fetch_add(1, std::memory_order_relaxed); };
        if (before != resident) {
            if (!ArchC::ImageEpochs::Get().Registered(*resident) || color.cmaskAddress != 0) ArchC::ImageEpochs::RegisterImage(*resident, color.cmaskAddress, color.cmaskBytes);
            token.registered = true;
            token.cmaskRegistered = color.cmaskAddress != 0;
            return refuse(TokenImage);
        }
        const bool drainedAfter = ArchC::ImageEpochs::Get().DrainIfNew();
        if (!drained || !drainedAfter) return refuse(TokenDrain);
        if (resident->archcEpoch.load(std::memory_order_acquire) != epochBefore) return refuse(TokenEpoch);
        if (ArchC::ImageEpochs::Get().Loss() != lossBefore) return refuse(TokenLoss);
        if (StorageSurfaceSerial() != serialBefore) return refuse(TokenSurface);
        std::array<ArchC::EpochRange, 3> ranges{};
        const auto used = rangesOf(*resident, color, ranges);
        if (!rangesArmed(std::span<const ArchC::EpochRange>(ranges.data(), used), GuestArena::GuestWtFaultClock_nid_no_patch())) return refuse(TokenOpen);
        std::array<StorageTexture::PendingQuery, 1> pending{{{ranges[0].begin, ranges[0].end, resident.get(), nullptr, false}}};
        StorageTexture::ScanPending(pending);
        if (pending[0].overlaps) return refuse(TokenPending);
        if (DepthSurfaceOverlaps(context.device, ranges[0].begin, ranges[0].end - ranges[0].begin)) return refuse(TokenDepth);
        token.valid = true;
        token.epoch = epochBefore;
        token.loss = lossBefore;
        token.surfaceSerial = serialBefore;
        counters.tokens.fetch_add(1, std::memory_order_relaxed);
    }

private:
    const Context& context;
    const ColorTarget& color;
    bool active = false;
    bool drained = false;
    TargetKey key{};
    Token* slot = nullptr;
    std::shared_ptr<StorageTexture> before;
    std::uint64_t epochBefore = 0;
    std::uint64_t lossBefore = 0;
    std::uint64_t serialBefore = 0;
};

class VerifyHit {
public:
    explicit VerifyHit(const std::shared_ptr<StorageTexture>& fast) : fast(fast) {
        if (fast == nullptr) return;
        version = fast->Version();
        epoch = fast->archcEpoch.load(std::memory_order_acquire);
    }

    void Check(const std::shared_ptr<StorageTexture>& resident) const {
        if (fast == nullptr) return;
        auto& counters = fastCounters();
        counters.verifyChecks.fetch_add(1, std::memory_order_relaxed);
        if (resident != fast || fast->Version() != version || fast->archcEpoch.load(std::memory_order_acquire) != epoch) {
            const auto previous = counters.verifyMismatches.fetch_add(1, std::memory_order_relaxed);
            if (previous < 8) std::fprintf(stderr, "[archc] target fast-path VERIFY mismatch: image 0x%llx (%s)\n", static_cast<unsigned long long>(fast->Descriptor().baseAddress), resident != fast ? "another image" : fast->Version() != version ? "refreshed" : "invalidated during the legacy pass");
        }
    }

private:
    std::shared_ptr<StorageTexture> fast;
    std::uint64_t version = 0;
    std::uint64_t epoch = 0;
};

}

// APS5_DRAW_BINDINGS_DIAG (diagnostic): the committer's lookup work of full-path draws by step
// (template Find, moved read-only buffers, alias checks, the indirect path decision, CachedPipeline,
// AcquireFramebuffer, the rest of the pipeline phase); proofs are left out. DrawLookupDiagText.
namespace lookupDiag {
enum Step : std::size_t { StepFind, StepMoved, StepAliases, StepDecide, StepCompletion, StepMasked, StepDepthFeedback, StepPipeline, StepFramebuffer, StepRest, StepTail, Steps };
constexpr const char* StepNames[Steps] = {"find", "moved", "aliases", "decide", "completion", "masked", "depth_feedback", "pipeline", "framebuffer", "rest", "tail"};
struct Counters {
    std::array<std::atomic<std::uint64_t>, Steps> ns{};
    std::atomic<std::uint64_t> draws{0};
    std::atomic<std::uint64_t> hits{0};
};
Counters& counters() {
    static auto* made = new Counters();
    return *made;
}
bool& activeThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<bool, Slot>();
}
std::chrono::steady_clock::time_point& lapThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<std::chrono::steady_clock::time_point, Slot>();
}
void start() {
    activeThreadState() = Recorder::SnapshotDiagEnabled() && GuestMemory::IsCommitterThread();
    if (activeThreadState()) lapThreadState() = std::chrono::steady_clock::now();
}
void restart() {
    if (activeThreadState()) lapThreadState() = std::chrono::steady_clock::now();
}
void mark(Step step) {
    if (!activeThreadState()) return;
    const auto now = std::chrono::steady_clock::now();
    counters().ns[step].fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - lapThreadState()).count()), std::memory_order_relaxed);
    lapThreadState() = now;
}
void finish(bool hit) {
    if (!activeThreadState()) return;
    activeThreadState() = false;
    counters().draws.fetch_add(1, std::memory_order_relaxed);
    if (hit) counters().hits.fetch_add(1, std::memory_order_relaxed);
}
}

// APS5_ARCHB_PIPE_DRAW_LOOKUP_MEMO (Stage 3 B2): the queue-0 committer's memo of its template
// lookups (ResourceCacheMemo); the template's proof, moved-buffer and alias checks run as before.
namespace lookupMemo {
ResourceCacheMemo::Counters& counters() {
    static auto* made = [] {
        auto* counts = new ResourceCacheMemo::Counters();
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_lookup_memo_hits", counts->hits);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_lookup_memo_misses", counts->misses);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_lookup_memo_stale", counts->stale);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_lookup_memo_touches", counts->touches);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_lookup_memo_verified", counts->verified);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_lookup_memo_mismatches", counts->mismatches);
        return counts;
    }();
    return *made;
}
bool verify() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_PIPE_DRAW_LOOKUP_MEMO_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
std::shared_ptr<ShaderResources> find(const ResourceCache::Key& key) {
    if (!AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::PipeDrawLookupMemo) || !GuestMemory::IsCommitterThread()) return SharedResourceCache().Find(key);
    struct MemoSlotStorage {};
    return AgcDriver::ThreadScratch<ResourceCacheMemo, MemoSlotStorage>().Find(SharedResourceCache(), key, verify(), counters());
}
}

// A draw's resources (resolveDrawResources): the resource-cache template a recordable draw's
// stages repeat, or a fresh build.
struct ResolvedResources {
    std::shared_ptr<ShaderResources> resources;
    std::vector<ShaderResources::MovedBuffer> moved;
    ResourceCache::Key contentKey;
    bool cacheable = false;
    const ShaderResources::BuildTiming* built = nullptr;
};

enum class AddressStages { None, ReadOnly, Storing };

AddressStages addressStages(std::span<const CompiledShader> shaders) {
    auto stages = AddressStages::None;
    for (const auto& shader : shaders) {
        if (shader.program == nullptr || std::none_of(shader.program->bindings.begin(), shader.program->bindings.end(), [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable; })) continue;
        if (ShaderStoresThroughAddresses(shader.program->spirv)) return AddressStages::Storing;
        stages = AddressStages::ReadOnly;
    }
    return stages;
}

bool verifyAddressDrawReuse() {
    static const bool verify = std::getenv("APS5_VERIFY_ADDRESS_DRAW_REUSE") != nullptr;
    return verify;
}

bool snapshotsInTable(std::span<const std::byte> table, std::span<const GuestMemorySnapshot> snapshots) {
    ShaderRecompiler::BdaAbi::Header header{};
    if (table.size() < sizeof(header)) return snapshots.empty();
    std::memcpy(&header, table.data(), sizeof(header));
    if (header.entryBytes != sizeof(ShaderRecompiler::BdaAbi::Range) || (table.size() - sizeof(header)) / sizeof(ShaderRecompiler::BdaAbi::Range) < header.count) return false;
    std::vector<ShaderRecompiler::BdaAbi::Range> ranges(header.count);
    if (!ranges.empty()) std::memcpy(ranges.data(), table.data() + sizeof(header), ranges.size() * sizeof(ranges.front()));
    return std::ranges::all_of(snapshots, [&](const GuestMemorySnapshot& snapshot) {
        auto at = snapshot.address;
        const auto end = snapshot.address + snapshot.bytes.size();
        for (const auto& range : ranges) {
            if (at >= end || range.begin > at) break;
            if (range.end > at && (range.permissions & ShaderRecompiler::BdaAbi::Read) != 0u) at = range.end;
        }
        return at >= end;
    });
}

bool snapshotsEqualMemory(std::span<const GuestMemorySnapshot> snapshots) {
    return std::ranges::all_of(snapshots, [](const GuestMemorySnapshot& snapshot) { return snapshot.bytes.empty() || std::memcmp(reinterpret_cast<const void*>(snapshot.address), snapshot.bytes.data(), snapshot.bytes.size()) == 0; });
}

void verifyServedSpace(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, std::uint64_t indexBytes, const ShaderResources& served) {
    const ShaderResources fresh(context, shaders, state.color, static_cast<std::uint32_t>(state.colors.size()), draw.indexAddress, static_cast<std::size_t>(indexBytes), snapshots);
    const auto servedTable = served.AddressTableBytes();
    const auto freshTable = fresh.AddressTableBytes();
    const char* what = !fresh.SpaceReuse() ? "space reuse" : !std::ranges::equal(servedTable, freshTable) ? "BDA table" : !snapshotsInTable(servedTable, snapshots) ? "snapshot ranges" : !snapshotsEqualMemory(snapshots) ? "snapshot bytes" : served.SampledTextures() != fresh.SampledTextures() ? "sampled textures" : served.StorageImages() != fresh.StorageImages() ? "storage images" : served.LayoutKey() != fresh.LayoutKey() || served.FragmentLayoutKey() != fresh.FragmentLayoutKey() ? "set layouts" : nullptr;
    addressDrawReuseVerifications().fetch_add(1, std::memory_order_relaxed);
    if (what == nullptr) return;
    char message[320];
    std::snprintf(message, sizeof(message), "APS5_VERIFY_ADDRESS_DRAW_REUSE: a served address-based draw hit disagrees with a fresh build or guest memory in its %s (BDA table %zu bytes served, %zu fresh; %zu / %zu sampled textures; %zu snapshots)", what, servedTable.size(), freshTable.size(), served.SampledTextures().size(), fresh.SampledTextures().size(), snapshots.size());
    throw std::runtime_error(message);
}

bool drawStagingEnabled() {
    static const bool disabled = std::getenv("APS5_NO_DRAW_STAGING") != nullptr;
    return !disabled;
}

ResolvedResources resolveDrawResources(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, std::uint64_t indexBytes, bool recordable, const DrawPlan& plan, DrawOutcome& outcome, DrawTimer& timer, DrawPreparation* preparation) {
    ResolvedResources resolved;
    lookupDiag::activeThreadState() = false;
    auto lookupLap = timer.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto lookupPhase = [&](LookupPhase phase) {
        if (!timer.profile) return;
        const auto now = std::chrono::steady_clock::now();
        outcome.lookupSplitUs[phase] += std::chrono::duration<double, std::micro>(now - lookupLap).count();
        lookupLap = now;
    };
    APS5_LOG_CHARS_OUT_DEBUG("Creating ShaderResources");
    // A recordable draw whose stages' compiled content repeats an earlier one binds that build's
    // descriptor set when it is still valid (see ResourceCache; the dispatch path does the same).
    // Only recordable draws take part: a synchronous draw writes its resources back, which a shared
    // object must never do. Revalidate proves a hit by texture identity, which needs the texture
    // caches. Push constants still come from this draw's stages. APS5_NO_DRAW_RESOURCE_CACHE=1
    // builds every draw's resources as before.
    // The render target and index ranges stay out of the key (see DrawResourceKey); a hit repeats
    // the alias checks instead. Debug aid: APS5_NO_DRAW_KEY_TRIM=1 keys them as before.
    static const bool trimKey = std::getenv("APS5_NO_DRAW_KEY_TRIM") == nullptr;
    const bool candidate = recordable && plan.candidate;
    resolved.cacheable = recordable && plan.keyed;
    const auto address = candidate && !ShaderResources::SpaceReuseDisabled() ? addressStages(shaders) : AddressStages::None;
    std::optional<AddressDrawReuse> reuse;
    if (address == AddressStages::Storing) reuse = AddressDrawReuse::NotReadOnly;
    else if (address == AddressStages::ReadOnly && !resolved.cacheable) reuse = AddressDrawReuse::NotSpaceOnly;
    if (resolved.cacheable) {
        const bool keyFromPreparation = plan.preparedKey && preparation != nullptr;
        resolved.contentKey = plan.contentKey;
        if (timer.profile && keyFromPreparation) preparation->keyUsed = true;
        lookupDiag::start();
        std::shared_ptr<ShaderResources> cached;
        const bool handedFind = keyFromPreparation && preparation->resolvedFind && resolveHandoff();
        const bool prelockFind = !handedFind && keyFromPreparation && preparation->resolvedFind && !preparation->resolved && ArchB::Switches::Enabled(ArchB::Feature::PrelockFindHandoff);
        if ((handedFind || prelockFind) && SharedResourceCache().Mutations() == preparation->resolvedCacheSerial) {
            cached = preparation->resolvedTemplate;
            if (handedFind) handoffCounters().finds.fetch_add(1, std::memory_order_relaxed);
            else {
                auto& counters = prelockFindCounters();
                counters.handoffs.fetch_add(1, std::memory_order_relaxed);
                if (prelockFindVerify()) {
                    counters.verified.fetch_add(1, std::memory_order_relaxed);
                    if (lookupMemo::find(resolved.contentKey) != cached) {
                        const auto count = counters.mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
                        if ((count & (count - 1)) == 0) std::fprintf(stderr, "[draw] PRELOCK_FIND_HANDOFF_VERIFY: a handed find differs from the locked find (%llu so far)\n", static_cast<unsigned long long>(count));
                    }
                }
            }
        } else {
            if (handedFind) handoffCounters().findsStale.fetch_add(1, std::memory_order_relaxed);
            if (prelockFind) prelockFindCounters().stale.fetch_add(1, std::memory_order_relaxed);
            cached = lookupMemo::find(resolved.contentKey);
        }
        lookupDiag::mark(lookupDiag::StepFind);
        if (timer.profile && preparation != nullptr && preparation->device != VK_NULL_HANDLE) preparation->cacheChanged |= cached.owner_before(preparation->cached) || preparation->cached.owner_before(cached);
        if (cached != nullptr) {
            lookupPhase(LookupKeyFind);
            reuseCensus::mark(StructCensus::ReuseLookup);
            const auto releaseSpace = [](ShaderResources* resources) { static_cast<void>(resources->ReleaseSpace()); };
            std::unique_ptr<ShaderResources, decltype(releaseSpace)> heldSpace(nullptr, releaseSpace);
            const bool counted = address == AddressStages::ReadOnly && cached->SpaceReuse();
            bool spaceHeld = !cached->SpaceReuse();
            bool inUse = false;
            if (!spaceHeld) {
                if (auto* active = Recorder::Active(); active != nullptr) {
                    GuestBufferMemory::ReleaseRetainedSpaces(active->Id(), active->CompletedThrough());
                    static_cast<void>(cached->ReleaseCompletedSpace(active->Id(), active->CompletedThrough()));
                }
                if (cached->SpaceHeld()) {
                    inUse = true;
                    reuse = AddressDrawReuse::InUse;
                } else if (!cached->RetakeSpace()) {
                    reuse = AddressDrawReuse::SpaceChanged;
                } else {
                    heldSpace.reset(cached.get());
                    if (!cached->AddressTableGuardCurrent()) {
                        reuse = AddressDrawReuse::SpaceChanged;
                        static_cast<void>(cached->ReleaseSpace());
                    } else {
                        completeSnapshotWrites(snapshots);
                        const auto service = cached->SnapshotsServedInPlace(snapshots);
                        spaceHeld = service == SnapshotService::Served;
                        if (!spaceHeld) {
                            reuse = service == SnapshotService::Partial ? AddressDrawReuse::PartialOverlap : service == SnapshotService::Changed ? AddressDrawReuse::SnapshotChanged : AddressDrawReuse::SnapshotOutside;
                            static_cast<void>(cached->ReleaseSpace());
                        }
                    }
                }
            }
            const bool valid = spaceHeld && cached->Revalidate(shaders);
            if (counted && spaceHeld && !valid) reuse = AddressDrawReuse::Revalidation;
            lookupPhase(LookupProof);
            reuseCensus::mark(StructCensus::ReuseProof);
            lookupDiag::restart();
            auto* recorder = Recorder::Active();
            std::optional<std::vector<ShaderResources::MovedBuffer>> moved;
            bool patched = false;
            if (valid && recorder != nullptr) moved = cached->MovedReadOnlyBuffers(shaders, *recorder, &state.color, &patched);
            lookupDiag::mark(lookupDiag::StepMoved);
            if (moved.has_value()) {
                if (trimKey) CheckBufferAliases(shaders, state.color, draw.indexAddress, indexBytes);
                lookupDiag::mark(lookupDiag::StepAliases);
                if (counted && verifyAddressDrawReuse()) verifyServedSpace(context, state, draw, shaders, snapshots, indexBytes, *cached);
                static_cast<void>(heldSpace.release());
                resolved.resources = std::move(cached);
                resolved.moved = std::move(*moved);
                if (timer.profile) outcome.movedBuffers = resolved.moved.size();
                outcome.kind = KindTemplateHit;
                countCache(&DrawProfile::cacheHits);
                if (counted) reuse = AddressDrawReuse::Reused;
            } else if (valid || inUse) {
                if (valid && cached->SpaceReuse()) static_cast<void>(cached->ReleaseSpace());
                if (valid && counted) reuse = patched ? AddressDrawReuse::PatchedData : AddressDrawReuse::MovedBuffer;
                countCache(&DrawProfile::cacheMisses);
            } else {
                if (cached->SpaceReuse()) static_cast<void>(cached->ReleaseSpace());
                SharedResourceCache().Remove(resolved.contentKey);
                countCache(&DrawProfile::cacheInvalidated);
            }
            archcNostage::noteLookup(moved.has_value() ? archcNostage::LookupHit : valid ? archcNostage::LookupMovedRefused : archcNostage::LookupInvalid);
            lookupPhase(LookupMoveAliases);
        } else {
            archcNostage::noteLookup(archcNostage::LookupAbsent);
        }
    } else {
        archcNostage::noteLookup(archcNostage::LookupUncacheable);
        countCache(&DrawProfile::uncacheable);
    }
    lookupPhase(LookupKeyFind);
    timer.phase(PhaseLookup);
    if (resolved.resources == nullptr) {
        resolved.resources = std::make_shared<ShaderResources>(context, shaders, state.color, static_cast<std::uint32_t>(state.colors.size()), draw.indexAddress, static_cast<std::size_t>(indexBytes), snapshots, recordable && drawStagingEnabled());
        resolved.built = &resolved.resources->Timing();
        outcome.addressBased = resolved.resources->HoldsLease();
        outcome.kind = outcome.addressBased ? KindBda : KindBuild;
        if (resolved.cacheable) countCache(&DrawProfile::cacheMisses);
        if (address == AddressStages::ReadOnly && !reuse.has_value()) reuse = resolved.resources->SpaceReuse() ? AddressDrawReuse::Built : AddressDrawReuse::NotSpaceOnly;
    }
    if (reuse.has_value()) countAddressDrawReuse(*reuse);
    timer.phase(PhaseResources);
    APS5_LOG_CHARS_OUT_DEBUG("ShaderResources created");
    return resolved;
}

// A GPU-side indirect draw's records (Draw's recorded path; a recipe draw has none) as recordDraw
// takes them: the arguments, how they are read, their host imports and, on the CPU path, the
// records read.
struct IndirectRecord {
    const Pm4::DrawParameters::IndirectDraw* args = nullptr;
    IndirectDrawPath path = IndirectDrawPath::Gpu;
    const HostImport* argumentImport = nullptr;
    const HostImport* countImport = nullptr;
    std::span<const Pm4::DrawArguments> records;
    double readMs = 0;
};

void CompactIndirectListRestart(const Context& context, const Pm4::DrawParameters& draw, DrawInputs& inputs, std::vector<Pm4::DrawArguments>& records) {
    const auto bound = inputs.indexCount.value_or(draw.indexCount);
    const auto raw = inputs.indices->Bytes();
    std::uint64_t total = 0;
    for (const auto& record : records) {
        if (record.count != 0 && record.instances != 0 && record.firstVertexOrIndex < bound) total += std::min(record.count, bound - record.firstVertexOrIndex);
    }
    Require(total <= std::numeric_limits<std::uint32_t>::max(), "indirect list restart index count overflow");
    auto compacted = std::make_shared<Buffer>(context, static_cast<std::size_t>(std::max<std::uint64_t>(total, 1u) * draw.indexSize), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    const auto out = compacted->Bytes();
    std::uint32_t cursor = 0;
    for (auto& record : records) {
        if (record.count == 0 || record.instances == 0 || record.firstVertexOrIndex >= bound) {
            record.count = 0;
            continue;
        }
        const auto count = std::min(record.count, bound - record.firstVertexOrIndex);
        const auto at = out.subspan(static_cast<std::size_t>(cursor) * draw.indexSize, static_cast<std::size_t>(count) * draw.indexSize);
        std::memcpy(at.data(), raw.data() + static_cast<std::size_t>(record.firstVertexOrIndex) * draw.indexSize, at.size());
        record.count = CompactListRestart(at, draw.indexSize, inputs.listRestartPrimitive);
        record.firstVertexOrIndex = cursor;
        cursor += record.count;
    }
    inputs.indexCount = cursor;
    inputs.indices = std::move(compacted);
    inputs.indexHandle = inputs.indices->Handle();
    inputs.indexOffset = 0;
}

// The draw commands of one draw: the vertex and index buffer binds, then the direct draw, the
// GPU-side indirect draw from `argumentBuffer` or the CPU-read records with the driver's rules.
bool recordDrawCommandsImpl(const Context& context, VkCommandBuffer commands, const State& state, const Pm4::DrawParameters& draw, const DrawInputs& inputs, const IndirectRecord* indirect, VkBuffer argumentBuffer, VkDeviceSize argumentOffset, const DrawTraceContext* trace, std::uint32_t& subdraw, std::optional<std::uint32_t> pieceFirstIndex = std::nullopt) {
    bool emitted = false;
    if (context.recorder != nullptr) context.recorder->NoteSampledDraw(commands);
    const auto recorded = [&](std::uint32_t index) {
        if (trace != nullptr && context.recorder != nullptr) {
            context.recorder->NoteDrawTraceToken(trace->token);
            CaptureTrace::Log("draw-recorded token=%llu batch=%llu submission=%llu queue=%u offset=%zu target=%llx subdraw=%u", static_cast<unsigned long long>(trace->token), static_cast<unsigned long long>(context.recorder->OpenBatchSerial()), static_cast<unsigned long long>(trace->submission), trace->queue, trace->offset, static_cast<unsigned long long>(trace->target), index);
        }
    };
    const auto* args = indirect != nullptr ? indirect->args : nullptr;
    if (state.stages.mesh && (args != nullptr || state.tessellationCapture != nullptr)) {
        CountVkDraw(VkDrawKind::Mesh);
        context.Function<PFN_vkCmdDrawMeshTasksIndirectEXT>("vkCmdDrawMeshTasksIndirectEXT")(commands, argumentBuffer, argumentOffset, 1, sizeof(VkDrawMeshTasksIndirectCommandEXT));
        recorded(subdraw++);
        emitted = true;
        return true;
    }
    if (state.stages.mesh) {
        APS5_LOG_OUT_DEBUG("vkCmdDrawMeshTasksEXT groups=%u instances=%u layers=%u", inputs.meshGroups, draw.instanceCount, inputs.meshLayers);
        CountVkDraw(VkDrawKind::Mesh);
        context.Function<PFN_vkCmdDrawMeshTasksEXT>("vkCmdDrawMeshTasksEXT")(commands, inputs.meshGroups, draw.instanceCount, inputs.meshLayers);
        recorded(subdraw++);
        emitted = true;
        return true;
    }
    if (!inputs.vertexHandles.empty()) context.Resolved(&DeviceFunctions::cmdBindVertexBuffers, "vkCmdBindVertexBuffers")(commands, 0, static_cast<std::uint32_t>(inputs.vertexHandles.size()), inputs.vertexHandles.data(), inputs.vertexOffsets.data());
    if (inputs.expandedIndexCount != 0u) {
        context.Resolved(&DeviceFunctions::cmdBindIndexBuffer, "vkCmdBindIndexBuffer")(commands, inputs.indexHandle, inputs.indexOffset, VK_INDEX_TYPE_UINT32);
        CountVkDraw(VkDrawKind::Indexed);
        context.Resolved(&DeviceFunctions::cmdDrawIndexed, "vkCmdDrawIndexed")(commands, inputs.expandedIndexCount, draw.instanceCount, 0, static_cast<std::int32_t>(draw.firstVertex), draw.firstInstance);
        emitted = true;
        return true;
    }
    if (draw.indexed) context.Resolved(&DeviceFunctions::cmdBindIndexBuffer, "vkCmdBindIndexBuffer")(commands, inputs.indexHandle, inputs.indexOffset, draw.indexSize == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    if (args == nullptr) {
        if (draw.indexed) {
            CountVkDraw(VkDrawKind::Indexed);
            context.Resolved(&DeviceFunctions::cmdDrawIndexed, "vkCmdDrawIndexed")(commands, pieceFirstIndex ? draw.indexCount : inputs.indexCount.value_or(draw.indexCount), draw.instanceCount, pieceFirstIndex.value_or(0u), static_cast<std::int32_t>(draw.firstVertex), draw.firstInstance);
            recorded(subdraw++);
            emitted = true;
        } else {
            CountVkDraw(VkDrawKind::Direct);
            context.Resolved(&DeviceFunctions::cmdDraw, "vkCmdDraw")(commands, draw.indexCount, draw.instanceCount, draw.firstVertex, draw.firstInstance);
            recorded(subdraw++);
            emitted = true;
        }
    } else if (indirect->path == IndirectDrawPath::Gpu) {
        emitted = true;
        if (args->countIndirect) {
            CountVkDraw(VkDrawKind::Indirect);
            if (draw.indexed) context.Function<PFN_vkCmdDrawIndexedIndirectCountKHR>("vkCmdDrawIndexedIndirectCountKHR")(commands, argumentBuffer, argumentOffset, indirect->countImport->buffer, HostImportOffset(*indirect->countImport, args->countAddress), args->count, args->stride);
            else context.Function<PFN_vkCmdDrawIndirectCountKHR>("vkCmdDrawIndirectCountKHR")(commands, argumentBuffer, argumentOffset, indirect->countImport->buffer, HostImportOffset(*indirect->countImport, args->countAddress), args->count, args->stride);
        } else if (args->count <= 1 || context.multiDrawIndirect) {
            CountVkDraw(VkDrawKind::Indirect);
            if (draw.indexed) context.Resolved(&DeviceFunctions::cmdDrawIndexedIndirect, "vkCmdDrawIndexedIndirect")(commands, argumentBuffer, argumentOffset, args->count, args->stride);
            else context.Resolved(&DeviceFunctions::cmdDrawIndirect, "vkCmdDrawIndirect")(commands, argumentBuffer, argumentOffset, args->count, args->stride);
        } else {
            for (std::uint32_t record = 0; record < args->count; ++record) {
                const auto offset = argumentOffset + static_cast<VkDeviceSize>(record) * args->stride;
                CountVkDraw(VkDrawKind::Indirect);
                if (draw.indexed) context.Resolved(&DeviceFunctions::cmdDrawIndexedIndirect, "vkCmdDrawIndexedIndirect")(commands, argumentBuffer, offset, 1, args->stride);
                else context.Resolved(&DeviceFunctions::cmdDrawIndirect, "vkCmdDrawIndirect")(commands, argumentBuffer, offset, 1, args->stride);
            }
        }
    } else {
        // The CPU-read records with the driver's rules: an InPlace dimension takes the record's
        // value, a Constant one (the CP writes nowhere) the constant.
        using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
        for (const auto& record : indirect->records) {
            if (record.count == 0 || record.instances == 0) continue;
            const auto firstInstance = args->instanceRule == Rule::InPlace ? record.firstInstance : args->instanceConstant;
            if (draw.indexed) {
                // The CP clamps the index range to INDEX_BUFFER_SIZE (the bound copy).
                const auto boundCount = inputs.indexCount.value_or(draw.indexCount);
                if (record.firstVertexOrIndex >= boundCount) continue;
                const auto vertexOffset = args->vertexRule == Rule::InPlace ? record.vertexOffset : args->vertexConstant;
                emitted = true;
                CountVkDraw(VkDrawKind::Indexed);
                context.Resolved(&DeviceFunctions::cmdDrawIndexed, "vkCmdDrawIndexed")(commands, std::min(record.count, boundCount - record.firstVertexOrIndex), record.instances, record.firstVertexOrIndex, static_cast<std::int32_t>(vertexOffset), firstInstance);
            } else {
                const auto firstVertex = args->vertexRule == Rule::InPlace ? record.firstVertexOrIndex : args->vertexConstant;
                emitted = true;
                CountVkDraw(VkDrawKind::Direct);
                context.Resolved(&DeviceFunctions::cmdDraw, "vkCmdDraw")(commands, record.count, record.instances, firstVertex, firstInstance);
            }
        }
    }
    return emitted;
}


void NoteGuestWorkIn(const Context& context, VkCommandBuffer commands) {
    static const bool reporting = LibcRunReportEnabled_nid_no_patch();
    if (!reporting) return;
    if (context.recorder != nullptr && context.recorder->NoteGuestWork(commands)) return;
    NoteGuestWorkOnCommandBuffer(commands);
}

void recordDrawCommands(const Context& context, VkCommandBuffer commands, const State& state, const Pm4::DrawParameters& draw, const DrawInputs& inputs, const IndirectRecord* indirect, VkBuffer argumentBuffer, VkDeviceSize argumentOffset, const DrawTraceContext* trace, std::uint32_t& subdraw, std::optional<std::uint32_t> pieceFirstIndex = std::nullopt) {
    if (recordDrawCommandsImpl(context, commands, state, draw, inputs, indirect, argumentBuffer, argumentOffset, trace, subdraw, pieceFirstIndex)) NoteGuestWorkIn(context, commands);
}

// The GPU-side records of an indirect draw, recorded outside the render pass: the stores that
// produced them (a dispatch in place, a fill, the host) precede the indirect read, as for
// DISPATCH_INDIRECT; a Constant dimension is rewritten in a scratch copy (vkCmdDrawIndirect reads
// the record's dword as the first vertex / instance, and the constant is what the fixed-function
// fetch must start at). The scratch is kept until the batch completed. Returns whether a copy was
// rewritten; `argumentBuffer`/`argumentOffset` receive where the draw reads the records.
bool recordIndirectArguments(const Context& context, VkCommandBuffer commands, Recorder* recorder, bool recorded, const IndirectRecord& indirect, std::unique_ptr<DeviceBuffer>& scratch, VkBuffer& argumentBuffer, VkDeviceSize& argumentOffset, const std::function<void(std::uint32_t)>& countBarrier) {
    using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
    const auto* args = indirect.args;
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    countBarrier(1);
    if (recorded) {
        // Read in place from the import when the batch runs (a synchronous draw waits for its own).
        recorder->NotePendingRead(args->arguments, static_cast<std::size_t>(args->RangeBytes()), Recorder::ReadKind::Indirect);
        if (args->countIndirect) recorder->NotePendingRead(args->countAddress, 4, Recorder::ReadKind::Indirect);
    }
    argumentBuffer = indirect.argumentImport->buffer;
    argumentOffset = HostImportOffset(*indirect.argumentImport, args->arguments);
    const bool rewritten = args->vertexRule == Rule::Constant || args->instanceRule == Rule::Constant;
    if (!rewritten) return false;
    const auto bytes = static_cast<std::size_t>(args->RangeBytes());
    scratch = std::make_unique<DeviceBuffer>(context, bytes, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    CopyBuffer(context, commands, argumentBuffer, argumentOffset, scratch->Handle(), 0, bytes);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    countBarrier(1);
    const auto update = context.Resolved(&DeviceFunctions::cmdUpdateBuffer, "vkCmdUpdateBuffer");
    for (std::uint32_t record = 0; record < args->count; ++record) {
        const VkDeviceSize base = static_cast<VkDeviceSize>(record) * args->stride;
        if (args->vertexRule == Rule::Constant) update(commands, scratch->Handle(), base + args->VertexDwordOffset(), 4, &args->vertexConstant);
        if (args->instanceRule == Rule::Constant) update(commands, scratch->Handle(), base + args->InstanceDwordOffset(), 4, &args->instanceConstant);
    }
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    countBarrier(1);
    argumentBuffer = scratch->Handle();
    argumentOffset = 0;
    return true;
}

void recordCaptureBarrier(const Context& context, VkCommandBuffer commands) {
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
}

std::shared_ptr<Buffer> meshArgumentBuffer(const Context& context, std::span<const std::uint32_t> restartTable) {
    const auto tableBytes = restartTable.empty() ? 0u : sizeof(std::uint32_t) * (restartTable.size() + 1u);
    auto arguments = std::make_shared<Buffer>(context, ShaderRecompiler::MeshArgumentBytes + tableBytes, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    if (!restartTable.empty()) {
        Require(restartTable.size() <= std::numeric_limits<std::uint32_t>::max(), "mesh restart table is too large");
        const auto length = static_cast<std::uint32_t>(restartTable.size());
        auto* bytes = arguments->Bytes().data() + ShaderRecompiler::MeshArgumentBytes;
        std::memcpy(bytes, &length, sizeof(length));
        std::memcpy(bytes + sizeof(length), restartTable.data(), restartTable.size_bytes());
    }
    return arguments;
}

std::shared_ptr<Buffer> directMeshArguments(const Context& context, const Pm4::DrawParameters& draw, std::span<const std::uint32_t> restartTable) {
    auto arguments = meshArgumentBuffer(context, restartTable);
    const MeshArguments resolved{0u, 0u, 0u, draw.indexCount, 0u};
    std::memcpy(arguments->Bytes().data(), &resolved, sizeof(resolved));
    return arguments;
}

std::shared_ptr<Buffer> recordMeshArguments(const Context& context, VkCommandBuffer commands, Recorder* recorder, bool recorded, const State& state, const Pm4::DrawParameters& draw, const IndirectRecord& indirect, const std::function<void(std::uint32_t)>& countBarrier, std::span<const std::uint32_t> restartTable) {
    const auto* args = indirect.args;
    const auto rules = MeshArgumentRulesFor(context, *state.stages.mesh, draw.indexCount);
    auto arguments = meshArgumentBuffer(context, restartTable);
    if (indirect.path == IndirectDrawPath::Gpu && recorder != nullptr && indirect.argumentImport->address != 0) {
        const std::array<std::uint32_t, 7> words{rules.indexCount, rules.inputSize, rules.step, rules.primitivesPerGroup, rules.maxGroups, rules.maxInstances, rules.maxTotal};
        if (recorder->RecordMeshArguments(commands, indirect.argumentImport->address + (args->arguments - indirect.argumentImport->base), arguments->DeviceAddress(), words)) {
            countBarrier(2);
            if (recorded) recorder->NotePendingRead(args->arguments, static_cast<std::size_t>(args->RangeBytes()), Recorder::ReadKind::Indirect);
            return arguments;
        }
    }
    const auto record = !indirect.records.empty() ? indirect.records.front() : Pm4::ReadDrawArguments(*args, 0);
    const auto resolved = ResolveMeshArguments(record, rules);
    std::memcpy(arguments->Bytes().data(), &resolved, sizeof(resolved));
    return arguments;
}

// Debug aid: APS5_CHECK_INDIRECT_ARGS=1 reads the records of a GPU-side draw back on the CPU once
// its batch completed (the GPU has finished writing them by then) and prints the first 16, counting
// records whose Constant dimension holds a value the hardware would have ignored. Empty otherwise.
std::function<void()> indirectRecordCheck(const IndirectRecord* indirect) {
    static const bool checkArguments = std::getenv("APS5_CHECK_INDIRECT_ARGS") != nullptr;
    if (indirect == nullptr || indirect->args == nullptr || indirect->path != IndirectDrawPath::Gpu || !checkArguments) return {};
    return [args = *indirect->args] {
        using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
        static std::atomic<std::uint64_t> printed{0};
        static std::atomic<std::uint64_t> ignoredValues{0};
        static std::atomic<std::uint64_t> checked{0};
        try {
            for (std::uint32_t record = 0; record < args.count; ++record) {
                const auto arguments = Pm4::ReadDrawArguments(args, record);
                const bool ignoredVertex = args.vertexRule == Rule::Constant && (args.recordBytes == 20 ? arguments.vertexOffset : arguments.firstVertexOrIndex) != 0;
                const bool ignoredInstance = args.instanceRule == Rule::Constant && arguments.firstInstance != 0;
                ++checked;
                if (ignoredVertex || ignoredInstance) ++ignoredValues;
                if (printed.fetch_add(1) < 16) std::fprintf(stderr, "[draw] indirect record 0x%llx: count %u instances %u first %u vertexOffset %u startInstance %u (vertex %s, instance %s)\n", static_cast<unsigned long long>(args.arguments + static_cast<std::uint64_t>(record) * args.stride), arguments.count, arguments.instances, arguments.firstVertexOrIndex, arguments.vertexOffset, arguments.firstInstance, args.vertexRule == Rule::Constant ? "const: record value ignored" : "in-place", args.instanceRule == Rule::Constant ? "const: record value ignored" : "in-place");
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[draw] indirect record read-back failed: %s\n", error.what());
        }
        if (checked % 64 == 0) std::fprintf(stderr, "[draw] indirect records checked %llu, with a value in a Constant dimension %llu\n", static_cast<unsigned long long>(checked.load()), static_cast<unsigned long long>(ignoredValues.load()));
    };
}

std::atomic<std::uint32_t> spaceKeepFailures{0};
std::atomic<int> stopAfterResolve{0};

std::shared_ptr<std::shared_ptr<const void>> reserveSpaceKeep(Recorder& recorder, const ShaderResources& resources) {
    if (!resources.SpaceReuse()) return nullptr;
    if (spaceKeepFailures.load(std::memory_order_relaxed) != 0) {
        spaceKeepFailures.fetch_sub(1, std::memory_order_relaxed);
        throw std::bad_alloc();
    }
    auto slot = std::make_shared<std::shared_ptr<const void>>();
    recorder.Keep(slot);
    return slot;
}

// Everything a recorded draw uses lives until its batch completes.
struct Kept {
    std::shared_ptr<ShaderResources> resources;
    std::shared_ptr<Pipeline> pipeline;
    std::shared_ptr<Framebuffer> framebuffer;
    std::shared_ptr<Buffer> indices;
    std::vector<std::shared_ptr<Buffer>> vertexBuffers;
    std::vector<std::shared_ptr<StorageTexture>> targets;
    std::unique_ptr<DeviceBuffer> scratch;
};

// The completion side of a recorded draw: the kept objects, the record check, the lease outcome,
// the GPU write notes, the copied-buffer write-back (listed in DrawCopiedWriters or run as a
// completion action) and the targets marked dirty.
void keepRecordedDraw(Recorder& recorder, const std::shared_ptr<ShaderResources>& resources, std::shared_ptr<Pipeline> pipeline, std::shared_ptr<Framebuffer> framebuffer, DrawInputs& inputs, std::vector<std::shared_ptr<StorageTexture>> targets, std::vector<std::pair<std::uint32_t, std::uint32_t>> slices, VkRect2D scissor, std::unique_ptr<DeviceBuffer> scratch, std::function<void()> checkRecords, bool listed, bool completion, const DrawOutcome& outcome, std::shared_ptr<ShaderResources::DrawUse> use, std::shared_ptr<const void>* spaceKeep) {
    auto kept = std::make_shared<Kept>();
    kept->resources = resources;
    kept->pipeline = std::move(pipeline);
    kept->framebuffer = std::move(framebuffer);
    kept->indices = std::move(inputs.indices);
    kept->vertexBuffers = std::move(inputs.vertexBuffers);
    kept->scratch = std::move(scratch);
    kept->targets = std::move(targets);
    const auto& residents = kept->targets;
    ArchC::ProbeAt(ArchC::Probe::KpKept);
    resources->KeepStorageImageBytes(recorder);
    for (const auto& target : residents) {
        if (target != nullptr && !target->Cached()) recorder.KeepBytes(target.get(), static_cast<std::size_t>(target->BudgetBytes()));
    }
    recorder.Keep(kept);
    if (resources != nullptr) recorder.KeepBytes(resources.get(), resources->CopiedBytes());
    if (checkRecords) recorder.OnComplete(std::move(checkRecords));
    // Counted for the [address-sync] line: a lease released by the completion (the pin waiter
    // finishes the recorder up to the open batch, which holds the kept resources and gets the
    // next serial), or waited for by the caller (any wait releases it at once).
    if (resources->HoldsLease()) CountLeaseOutcome(outcome.waited, outcome.waited ? 0 : recorder.Submissions() + 1);
    ArchC::ProbeAt(ArchC::Probe::KpStorage);
    // Every use of a (possibly shared) resources object registers its GPU writes anew.
    resources->MarkGpuWrites(recorder);
    if (spaceKeep != nullptr) *spaceKeep = resources->ReleaseSpace();
    ArchC::ProbeAt(ArchC::Probe::KpMarkGpu);
    // The write-back (fault check, copied buffers) runs when the batch completed; a fault is
    // reported by the recorder ("deferred write-back failed") instead of thrown out of the draw.
    if (listed && resources->HasCopiedWrites()) {
        // As VulkanDevice::dispatch lists its copied writers: delisted before the write-back
        // (one that fails must not keep indirect dispatches on the CPU), listed after the
        // registration (a throw there leaves nothing behind).
        auto writers = DrawCopiedWriters();
        recorder.OnComplete([resources, writers] {
            writers->erase(std::remove(writers->begin(), writers->end(), resources), writers->end());
            resources->WriteBackBuffers();
        }, false, resources.get());
        writers->push_back(resources);
        GuestMemory::BreakProofs();
    } else if (completion) {
        if (use != nullptr) recorder.OnComplete([resources, use] { resources->CompleteDrawUse(*use); }, true);
        else if (resources->Reusable()) recorder.OnComplete([resources] { resources->CheckFaults(); }, true);
        else recorder.OnComplete([resources] { resources->WriteBackBuffers(); }, false, resources.get());
    }
    ArchC::ProbeAt(ArchC::Probe::KpComplete);
    std::vector<bool> storeNow(residents.size(), false);
    for (std::size_t index = 0; index < residents.size(); ++index) {
        if (residents[index] == nullptr) continue;
        if (residents[index]->Descriptor().tileMode == TextureTileMode::kLinear) storeNow[index] = !residents[index]->MarkDrawn(index < slices.size() ? slices[index].first : 0u, index < slices.size() ? slices[index].second : 0u, scissor) || !residents[index]->Cached();
        else if (index < slices.size()) residents[index]->MarkSliceDirty(slices[index].first, slices[index].second);
        else residents[index]->MarkDirty();
    }
    for (std::size_t index = 0; index < residents.size(); ++index) {
        if (storeNow[index]) StorageTexture::FlushPending(residents[index]->Descriptor().baseAddress, static_cast<std::size_t>(residents[index]->GuestBytes()), nullptr, "linear target");
    }
    ArchC::ProbeAt(ArchC::Probe::KpDirty);
}

struct SlotPassCapture {
    bool filled = false;
    std::uint64_t passKey = 0;
    bool forced = false;
    bool fenced = false;
    bool inPassIndirect = false;
    bool bindingsReplayable = false;
    bool unknownMemory = false;
    bool unknownWrites = false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> passReads;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> linearReads;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> passWrites;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> writeOnly;
    std::vector<std::pair<VkImage, bool>> images;
    std::vector<std::pair<VkImage, bool>> attachments;
};

// What recordDraw records from: the recorder, the resources, the pipeline and framebuffer, the
// resident targets with their views, the indirect records (null for a direct draw), how the
// completion work is handled (`listed`, `completion`, `waited`: see Draw) and, from a recipe, the
// push constant block (else assembled from the stages).
struct StagedWrite {
    std::shared_ptr<StagedTemporary> temporary;
    VkImageView target = VK_NULL_HANDLE;
    std::uint16_t mask = 0;
    std::uint32_t samples = 1;
    VkRect2D area{};
};

struct RecordedDraw {
    Recorder* recorder = nullptr;
    std::shared_ptr<ShaderResources> resources;
    std::shared_ptr<Pipeline> pipeline;
    std::shared_ptr<Framebuffer> framebuffer;
    std::span<const VkImageView> targetViews;
    std::vector<std::shared_ptr<StorageTexture>> targets;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> targetSlices;
    std::vector<std::shared_ptr<void>> multisampleTargets;
    std::vector<std::shared_ptr<StorageTexture>> proxies;
    std::vector<VkImage> multisampleImages;
    std::vector<StagedWrite> staged;
    const IndirectRecord* indirect = nullptr;
    std::span<const ShaderResources::MovedBuffer> moved;
    bool listed = false;
    bool completion = false;
    bool waited = false;
    const std::array<std::byte, PipelinePushConstantBytes>* pushBytes = nullptr;
    VkShaderStageFlags pushStages = 0;
    std::vector<std::pair<std::uint32_t, std::shared_ptr<Pipeline>>> stencilReplay;
    std::vector<SampleOrderTarget> reordered;
    SampleOrderPass reorderPass;
    std::optional<DepthSampleLocations> depthLocations;
    SlotPassCapture* slotCapture = nullptr;
    std::shared_ptr<std::shared_ptr<const void>>* spaceKeep = nullptr;
};

// A remapped viewport depth range reads its scale and offset from the draw's push block (the vertex shader's
// RemapPositionDepth), so draws that differ only in them share one pipeline; the draw's own state supplies them.
void patchDepthRemapPush(std::array<std::byte, PipelinePushConstantBytes>& block, const State& state) {
    if (state.depthRemap.mode == 0u) return;
    static_assert(ShaderRecompiler::DepthRemapPushOffsetBytes + ShaderRecompiler::DepthRemapPushBytes <= PipelinePushSlotBytes);
    const std::array<float, ShaderRecompiler::DepthRemapPushBytes / 4> words{state.depthRemap.scale, state.depthRemap.offset};
    std::memcpy(block.data() + ShaderRecompiler::DepthRemapPushOffsetBytes, words.data(), sizeof(words));
}

void recordStencilReplay(const Context& context, VkCommandBuffer commands, const State& state, const Pm4::DrawParameters& draw, const DrawInputs& inputs, std::span<const std::pair<std::uint32_t, std::shared_ptr<Pipeline>>> replay, std::span<const CompiledShader> shaders, const ShaderResources& resources, const std::array<std::byte, PipelinePushConstantBytes>* bytes = nullptr, VkShaderStageFlags stages = 0) {
    State selected = state;
    auto block = bytes != nullptr ? *bytes : AssemblePushConstants(shaders);
    if (bytes == nullptr) {
        resources.PatchPushConstants(block);
        stages = PushConstantStages(shaders);
    }
    patchDepthRemapPush(block, state);
    const auto fragment = std::ranges::find_if(shaders, [](const CompiledShader& shader) { return shader.stage == ShaderRecompiler::ShaderStage::Fragment && !shader.program->stencilReferences.empty(); });
    Require(fragment != shaders.end(), "stencil replay fragment stage is missing");
    const auto referenceOffset = fragment->pushConstantOffset + fragment->program->stencilReferenceDword * sizeof(std::uint32_t);
    Require(referenceOffset <= block.size() - sizeof(std::uint32_t), "stencil replay reference exceeds the push block");
    const auto run = [&](const Pm4::DrawParameters& piece, std::optional<std::uint32_t> firstIndex) {
        std::uint32_t unusedSubdraw = 0;
        for (const auto& [reference, pipeline] : replay) {
            selected.stencilReplayReference = reference;
            selected.stencilFront.reference = reference;
            selected.stencilBack.reference = reference;
            context.Resolved(&DeviceFunctions::cmdSetStencilReference, "vkCmdSetStencilReference")(commands, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
            std::memcpy(block.data() + referenceOffset, &reference, sizeof(reference));
            pipeline->PushConstants(commands, stages, block);
            recordDrawCommands(context, commands, selected, piece, inputs, nullptr, VK_NULL_HANDLE, 0, nullptr, unusedSubdraw, firstIndex);
        }
    };
    if (replay.size() == 1u) {
        run(draw, std::nullopt);
        return;
    }
    const auto indexCount = draw.indexed ? inputs.indexCount.value_or(draw.indexCount) : draw.indexCount;
    for (std::uint32_t instance = 0; instance < draw.instanceCount; ++instance) {
        for (std::uint32_t first = 0; first < indexCount - indexCount % 3u; first += 3u) {
            auto piece = draw;
            piece.instanceCount = 1u;
            piece.firstInstance = draw.firstInstance + instance;
            piece.indexCount = 3u;
            if (!draw.indexed) piece.firstVertex += first;
            run(piece, draw.indexed ? first : 0u);
        }
    }
}

void pushDrawConstants(const Pipeline& pipeline, VkCommandBuffer commands, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, const ShaderResources& resources, const std::array<std::byte, PipelinePushConstantBytes>* bytes, VkShaderStageFlags stages, VkDeviceAddress meshArguments = 0, bool restartTable = false, std::uint32_t meshLayerStride = 0, PipelineStateReuse* reuse = nullptr) {
    auto block = bytes != nullptr ? *bytes : AssemblePushConstants(shaders);
    if (bytes == nullptr) {
        resources.PatchPushConstants(block);
        stages = PushConstantStages(shaders);
    }
    patchDepthRemapPush(block, state);
    if (state.stages.tessellation) {
        for (const auto& shader : shaders) Require(shader.program->pushConstants.empty() || shader.pushConstantOffset >= PipelinePushSlotBytes || shader.pushConstantOffset + shader.program->pushConstants.size() <= ShaderRecompiler::TessellationLevelPushOffsetBytes, "a stage's push constants overlap the tessellation levels");
        const std::array<float, ShaderRecompiler::TessellationLevelPushBytes / 4> levels{state.stages.minTessellationLevel, state.stages.maxTessellationLevel};
        static_assert(ShaderRecompiler::TessellationLevelPushOffsetBytes + ShaderRecompiler::TessellationLevelPushBytes == PipelinePushSlotBytes);
        std::memcpy(block.data() + ShaderRecompiler::TessellationLevelPushOffsetBytes, levels.data(), sizeof(levels));
    }
    const auto capturedItems = state.tessellationCapture != nullptr ? state.tessellationCapture->items->DeviceAddress() : 0;
    if (!state.stages.mesh) {
        if (state.tessellationCapture != nullptr) {
            for (const auto& shader : shaders) Require(shader.program->pushConstants.empty() || shader.pushConstantOffset >= PipelinePushSlotBytes || shader.pushConstantOffset + shader.program->pushConstants.size() <= ShaderRecompiler::MeshDrawPushOffsetBytes, "a stage's push constants overlap the tessellation capture addresses");
            const auto header = state.tessellationCapture->header->DeviceAddress();
            const std::array<std::uint32_t, 4> words{static_cast<std::uint32_t>(capturedItems), static_cast<std::uint32_t>(capturedItems >> 32u), static_cast<std::uint32_t>(header), static_cast<std::uint32_t>(header >> 32u)};
            static_assert(ShaderRecompiler::MeshDrawPushOffsetBytes + sizeof(std::uint32_t) * 4 <= ShaderRecompiler::TessellationLevelPushOffsetBytes);
            std::memcpy(block.data() + ShaderRecompiler::MeshDrawPushOffsetBytes, words.data(), sizeof(words));
            stages |= VK_SHADER_STAGE_GEOMETRY_BIT;
        }
        pipeline.PushConstants(commands, stages, block, reuse);
        return;
    }
    const auto firstVertex = draw.indirect ? draw.indirect->vertexConstant : draw.firstVertex;
    const auto firstInstance = draw.indirect ? draw.indirect->instanceConstant : draw.firstInstance;
    std::array<std::uint32_t, ShaderRecompiler::MeshDrawPushBytes / 4> words{draw.indexCount, firstVertex, firstInstance, (draw.indexed ? draw.indexSize | (restartTable ? ShaderRecompiler::MeshIndexRestartTable : 0u) : 0u) | meshLayerStride << ShaderRecompiler::MeshLayerStrideShift, static_cast<std::uint32_t>(meshArguments), static_cast<std::uint32_t>(meshArguments >> 32u)};
    if (state.tessellationCapture != nullptr) words = {static_cast<std::uint32_t>(capturedItems), static_cast<std::uint32_t>(capturedItems >> 32u), 0u, 0u, static_cast<std::uint32_t>(meshArguments), static_cast<std::uint32_t>(meshArguments >> 32u)};
    static_assert(ShaderRecompiler::MeshDrawPushOffsetBytes + ShaderRecompiler::MeshDrawPushBytes == PipelinePushSlotBytes);
    std::memcpy(block.data() + ShaderRecompiler::MeshDrawPushOffsetBytes, words.data(), sizeof(words));
    pipeline.PushConstants(commands, stages | VK_SHADER_STAGE_MESH_BIT_EXT, block, reuse);
}

// The record of a draw whose targets are all resident (`lean`: recorded into the device's open
// batch, rendering in the general layout): one memory barrier before the pass (earlier writes to
// the attachments and the draw's inputs visible to it) and one after it, recorded by the recorder
// when the pass ends (Recorder::LeaveRenderPassOpen), so consecutive draws into the same
// attachments with nothing recorded between them share one pass; the host-write and host-read
// barriers are implicit in the submission and the fence. Then the kept objects, the completion
// work and, for a waited-for draw, the recorder sync.
bool CaptureInputsEnabled() {
    static const bool enabled = std::getenv("APS5_CAPTURE_INPUTS") != nullptr;
    Require(!enabled || CaptureTrace::Enabled(), "APS5_CAPTURE_INPUTS requires APS5_CAPTURE_TRACE");
    return enabled;
}

void captureInputs(const Context& context, Recorder& recorder, VkCommandBuffer commands, const ShaderResources& resources, const std::shared_ptr<ShaderResources::DrawBindings>& bindings, std::uint64_t target) {
    struct Sample {
        std::uint64_t address;
        std::size_t offset;
        std::vector<std::byte> expected;
        VkBuffer source;
        VkDeviceSize sourceOffset;
    };
    static unsigned long long nextDraw = 0;
    const auto draw = ++nextDraw;
    const auto batch = static_cast<unsigned long long>(recorder.Submissions() + 1);
    std::vector<Sample> samples;
    std::size_t total = 0;
    const auto addSample = [&](std::uint64_t address, std::size_t bytes, VkBuffer source, VkDeviceSize offset, const std::byte* expected) {
        if (bytes == 0 || bytes > 512) return;
        Require(offset % 4 == 0 && bytes % 4 == 0, "capture input is not aligned for a Vulkan buffer copy");
        Require(total + bytes <= 65536, "capture inputs exceed 64 KiB per draw");
        Sample sample{address, total, std::vector<std::byte>(bytes), source, offset};
        std::memcpy(sample.expected.data(), expected, bytes);
        total += bytes;
        samples.push_back(std::move(sample));
    };
    if (bindings != nullptr) {
        for (const auto& snapshot : bindings->snapshots) {
            const auto bytes = snapshot.Bytes();
            addSample(snapshot.address, bytes.size(), snapshot.buffer->Handle(), snapshot.offset, bytes.data());
        }
    }
    // APS5_ARCHB_FIX_ALLOC_CHURN: the in-place reads in a per-thread scratch that keeps its capacity;
    // off, a vector of this call (as the by-value InPlaceReads made).
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ownedReads;
    struct ReadsSlotThreadStorage {};
    auto& readsSlot = AgcDriver::ThreadScratch<std::vector<std::pair<std::uint64_t, std::uint64_t>>*, ReadsSlotThreadStorage>();
    auto& inPlaceReads = AgcDriver::ArchB::AllocChurnActive() ? ShaderRecompiler::ThreadOwned(readsSlot) : ownedReads;
    resources.InPlaceReads(inPlaceReads);
    for (const auto& [begin, end] : inPlaceReads) {
        Require(end >= begin, "invalid capture input range");
        const auto bytes = static_cast<std::size_t>(end - begin);
        if (bytes == 0 || bytes > 512) continue;
        if (bindings != nullptr && std::any_of(bindings->snapshots.begin(), bindings->snapshots.end(), [&](const auto& snapshot) { return snapshot.address < end && begin < snapshot.address + snapshot.bytes; })) continue;
        if (resources.WritesOverlap(begin, bytes) || recorder.PendingWriteOverlaps(begin, bytes)) {
            CaptureTrace::Log("input-skip draw=%llu batch=%llu address=%llx bytes=%zu reason=gpu-writer", draw, batch, static_cast<unsigned long long>(begin), bytes);
            continue;
        }
        const auto* imported = HostImportFor(context, begin, bytes);
        Require(imported != nullptr, "capture input has no host import");
        addSample(begin, bytes, imported->buffer, HostImportOffset(*imported, begin), reinterpret_cast<const std::byte*>(begin));
    }
    CaptureTrace::Log("input-capture draw=%llu batch=%llu target=%llx ranges=%zu bytes=%zu", draw, batch, static_cast<unsigned long long>(target), samples.size(), total);
    if (samples.empty()) return;
    auto readback = std::make_shared<Buffer>(context, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const auto copy = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    for (const auto& sample : samples) {
        const VkBufferCopy region{sample.sourceOffset, sample.offset, sample.expected.size()};
        copy(commands, sample.source, readback->Handle(), 1, &region);
    }
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    recorder.OnComplete([draw, batch, target, readback, samples = std::move(samples)] {
        const auto actual = readback->Bytes();
        std::size_t differences = 0;
        for (const auto& sample : samples) {
            std::size_t changed = 0;
            for (std::size_t index = 0; index < sample.expected.size(); ++index) {
                const auto gpu = actual[sample.offset + index];
                if (sample.expected[index] == gpu) continue;
                if (changed < 16) CaptureTrace::Log("input-byte draw=%llu batch=%llu address=%llx offset=%zu cpu=%02x gpu=%02x", draw, batch, static_cast<unsigned long long>(sample.address), index, std::to_integer<unsigned>(sample.expected[index]), std::to_integer<unsigned>(gpu));
                ++changed;
            }
            if (changed != 0) {
                ++differences;
                CaptureTrace::Log("input-mismatch draw=%llu batch=%llu target=%llx address=%llx bytes=%zu changed=%zu", draw, batch, static_cast<unsigned long long>(target), static_cast<unsigned long long>(sample.address), sample.expected.size(), changed);
            }
        }
        CaptureTrace::Log("input-result draw=%llu batch=%llu ranges=%zu mismatched=%zu", draw, batch, samples.size(), differences);
    });
}

bool WriteOnlyPasses() {
    static const bool enabled = std::getenv("APS5_NO_WRITE_ONLY_PASSES") == nullptr;
    return enabled;
}

// APS5_ARCHB_STRUCT_REPEAT_CENSUS: the recorded draw's structural state by component (StructCensus.hpp).
// The snapshot contents are hashed only when `withData` (the repeat census; the reuse census keys
// the structure level, which leaves addresses and data out).
StructCensus::Commit drawCensusCommit(const State& state, const Pm4::DrawParameters& draw, const RecordedDraw& record, std::uint64_t passKey, const ShaderResources::DrawBindings* bindings, bool continued, bool withData) {
    using namespace StructCensus;
    Commit commit;
    commit.kind = Kind::Draw;
    commit.passBegin = !continued;
    auto shape = Mix(reinterpret_cast<std::uintptr_t>(record.pipeline.get()), passKey);
    shape = Mix(shape, reinterpret_cast<std::uintptr_t>(record.framebuffer.get()));
    shape = Mix(shape, record.pushStages);
    shape = Mix(shape, (draw.indexed ? 1u : 0u) | (draw.indirect.has_value() ? 2u : 0u) | (static_cast<std::uint64_t>(draw.indexSize) << 8u));
    if (bindings != nullptr) {
        shape = Mix(shape, reinterpret_cast<std::uintptr_t>(bindings->layout));
        shape = Mix(shape, reinterpret_cast<std::uintptr_t>(bindings->fragmentLayout));
    }
    commit.shape = shape;
    const auto bytesOf = [](const auto& value) { return std::as_bytes(std::span(&value, 1)); };
    auto dynamic = HashBytes(1, bytesOf(state.viewport));
    dynamic = HashBytes(dynamic, bytesOf(state.scissor));
    dynamic = HashBytes(dynamic, bytesOf(state.blendConstants));
    dynamic = Mix(dynamic, (static_cast<std::uint64_t>(state.stencilFront.reference) << 32u) | state.stencilBack.reference);
    dynamic = Mix(dynamic, (static_cast<std::uint64_t>(state.stencilFront.compareMask) << 32u) | state.stencilFront.writeMask);
    float bias[3] = {state.depthBiasConstant, state.depthBiasSlope, state.depthBiasClamp};
    dynamic = HashBytes(dynamic, std::as_bytes(std::span(bias)));
    commit.components[ComponentDynamic] = dynamic;
    auto params = Mix(2, (static_cast<std::uint64_t>(draw.indexCount) << 32u) | draw.instanceCount);
    params = Mix(params, (static_cast<std::uint64_t>(draw.firstVertex) << 32u) | draw.firstInstance);
    params = Mix(params, draw.flags);
    if (draw.indirect.has_value()) params = Mix(params, (static_cast<std::uint64_t>(draw.indirect->count) << 32u) | draw.indirect->stride);
    commit.components[ComponentParams] = params;
    commit.components[ComponentPush] = record.pushBytes != nullptr ? HashBytes(3, std::as_bytes(std::span(*record.pushBytes))) : 3;
    commit.components[ComponentBindings] = Mix(4, reinterpret_cast<std::uintptr_t>(record.resources.get()));
    auto addresses = Mix(5, draw.indexed ? draw.indexAddress : 0);
    if (draw.indirect.has_value()) addresses = Mix(addresses, draw.indirect->arguments);
    auto data = std::uint64_t{6};
    if (bindings != nullptr && withData) {
        for (const auto& snapshot : bindings->snapshots) {
            addresses = Mix(addresses, snapshot.address);
            addresses = Mix(addresses, snapshot.bytes);
            if (snapshot.buffer != nullptr) data = HashBytes(data, snapshot.Bytes());
        }
    }
    commit.components[ComponentAddresses] = addresses;
    commit.components[ComponentData] = data;
    return commit;
}

void noteDrawCensus(const State& state, const Pm4::DrawParameters& draw, const RecordedDraw& record, std::uint64_t passKey, const ShaderResources::DrawBindings* bindings, bool continued) {
    StructCensus::NoteCommit(drawCensusCommit(state, draw, record, passKey, bindings, continued, true));
}

// APS5_ARCHB_DRAW_REUSE_CENSUS (B0, StructCensus.hpp): the committer's current draw, its stage
// times since begin() (a recipe hit in DrawWithRecipe, a full draw in Draw) and its structure hash.
// A draw that leaves before recordDraw (a recipe miss, an unrecorded draw) is dropped by the next
// begin().
namespace reuseCensus {
struct Current {
    bool active = false;
    bool recipe = false;
    std::uint64_t start = 0;
    std::uint64_t last = 0;
    std::uint64_t structure = 0;
    std::array<std::uint64_t, StructCensus::ReuseStages> ns{};
};
Current& currentThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<Current, Slot>();
}

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void begin(bool recipe) {
    static const bool enabled = StructCensus::ReuseCensusEnabled();
    if (!enabled || !GuestMemory::IsCommitterThread()) {
        currentThreadState().active = false;
        return;
    }
    currentThreadState() = {};
    currentThreadState().active = true;
    currentThreadState().recipe = recipe;
    currentThreadState().start = currentThreadState().last = nowNs();
}

void mark(StructCensus::ReuseStage stage) {
    if (!currentThreadState().active) return;
    const auto now = nowNs();
    currentThreadState().ns[stage] += now - currentThreadState().last;
    currentThreadState().last = now;
}

// The structure level of the draw's census commit; its hashing time is left out of the stages.
void structure(const State& state, const Pm4::DrawParameters& draw, const RecordedDraw& record, std::uint64_t passKey, const ShaderResources::DrawBindings* bindings, bool continued) {
    if (!currentThreadState().active) return;
    mark(StructCensus::ReuseRecord);
    currentThreadState().structure = StructCensus::Tracker::LevelHashes(drawCensusCommit(state, draw, record, passKey, bindings, continued, false))[StructCensus::LevelStructure];
    currentThreadState().last = nowNs();
}

void finish() {
    if (!currentThreadState().active) return;
    currentThreadState().active = false;
    StructCensus::NoteReuseDraw(currentThreadState().structure, currentThreadState().recipe, currentThreadState().ns, currentThreadState().last - currentThreadState().start, GuestMemory::CensusFlips());
}

void markPhase(std::size_t drawPhase) {
    if (!currentThreadState().active) return;
    using namespace StructCensus;
    switch (drawPhase) {
        case PhaseReadTarget: mark(ReuseTargets); break;
        case PhaseLookup:
        case PhasePipeline: mark(ReuseLookup); break;
        case PhaseRecord: mark(ReuseRecord); break;
        case PhaseKeep: mark(ReuseKeep); break;
        default: mark(ReuseOther); break;
    }
}
}

void recordDraw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, DrawInputs& inputs, RecordedDraw& record, DrawOutcome& outcome, DrawTimer& timer, double& ownWaitedMs, const DrawTraceContext* trace = nullptr, bool pipelineStateReuse = false) {
    using R = AgcDriver::ArchB::Recorder;
    // B0: a full draw's whole preparation (target lookups, template key and find or build,
    // pipeline) is "other" (reported apart by the recipe counters), a recipe hit's tail "other".
    reuseCensus::mark(StructCensus::ReuseOther);
    auto recordLap = timer.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto recordPhase = [&](RecordPhase phase) {
        if (!timer.profile) return;
        const auto now = std::chrono::steady_clock::now();
        outcome.recordUs[phase] += std::chrono::duration<double, std::micro>(now - recordLap).count();
        recordLap = now;
    };
    std::uint32_t subdraw = 0;
    auto* recorder = record.recorder;
    struct ReuseGuard {
        Recorder* recorder;
        int exceptions = std::uncaught_exceptions();
        ~ReuseGuard() {
            if (std::uncaught_exceptions() != exceptions) recorder->InvalidatePipelineState("fix_pso_reuse_invalidations_exception");
        }
    } reuseGuard{recorder};
    if (!pipelineStateReuse) recorder->ReusePipelineState(false);
    const bool tracked = record.stencilReplay.empty() && !state.stencilReplayReference && !record.pipeline->SplitsFaces() && !record.pipeline->SplitsSecondChannel();
    if (pipelineStateReuse && !tracked) {
        R::Count("fix_pso_reuse_refused_helper_draw");
        recorder->InvalidatePipelineState("fix_pso_reuse_invalidations_helper");
    }
    auto* reuse = pipelineStateReuse && tracked ? recorder->ReusePipelineState(true) : nullptr;
    auto& resources = *record.resources;
    const auto* args = record.indirect != nullptr ? record.indirect->args : nullptr;
    const bool gpuIndirect = args != nullptr && record.indirect->path == IndirectDrawPath::Gpu;
    using IndirectRule = Pm4::DrawParameters::IndirectDraw::Rule;
    static const bool indirectInPass = std::getenv("APS5_NO_INDIRECT_IN_PASS") == nullptr;
    const bool inPassIndirect = indirectInPass && gpuIndirect && !state.stages.mesh && !args->countIndirect && args->vertexRule == IndirectRule::InPlace && args->instanceRule == IndirectRule::InPlace;
    using CommandClass = Recorder::CommandClass;
    const auto countBarrier = [&](std::uint32_t count) { Recorder::CountBarriers(CommandClass::Draw, count); };
    // The pass is named by its attachment views (stable while the kept targets live, so unique
    // within the open batch) and the extent. The previous draw's pass is continued only when this
    // draw neither reads its attachments (a barrier would be owed, which no pass allows) nor
    // records anything outside a pass (an indirect draw's argument barrier and scratch copies).
    const auto passKey = DrawRenderPassKey(context, state, record.targetViews);
    const bool readsTarget = std::any_of(record.targets.begin(), record.targets.end(), [&](const std::shared_ptr<StorageTexture>& target) { return resources.ReadsImage(target.get()); });
    // A queued DCC key store over memory the draw writes or reads in place (unknown for an
    // address-based build), or over its GPU-side records, lands before it, as before a dispatch
    // (VulkanDevice::dispatch); the flush ends an open pass, so it comes before the decision.
    const auto touches = [&](std::uint64_t begin, std::uint64_t end) {
        const auto bytes = static_cast<std::size_t>(end - begin);
        if (resources.WritesOverlap(begin, bytes) || resources.ReadsOverlap(begin, bytes)) return true;
        if (!gpuIndirect) return false;
        return (begin < args->arguments + args->RangeBytes() && args->arguments < end) || (args->countIndirect && begin < args->countAddress + 4 && args->countAddress < end);
    };
    if (recorder->HasQueuedKeyStores() && (resources.HoldsLease() || recorder->AnyQueuedKeyStore(touches))) {
        recorder->FlushKeyStores();
        outcome.keyStoresFlushed = true;
    }
    // A queued label store over such memory likewise (Recorder::RecordStore).
    if (recorder->HasQueuedStores() && (resources.HoldsLease() || recorder->AnyQueuedStore(touches))) {
        recorder->FlushStores();
        outcome.storesFlushed = true;
    }
    const bool capture = CaptureInputsEnabled();
    const bool meshIndirect = state.stages.mesh && args != nullptr;
    const bool captured = state.stages.mesh && state.tessellationCapture != nullptr;
    const bool addressBased = resources.HoldsLease();
    struct PassReadsSlotStorage {};
    auto& passReads = AgcDriver::ThreadScratch<std::vector<std::pair<std::uint64_t, std::uint64_t>>, PassReadsSlotStorage>();
    passReads.clear();
    if (!addressBased) resources.DeviceReads(passReads);
    const auto deviceReads = passReads.size();
    if (gpuIndirect) {
        passReads.emplace_back(args->arguments, args->arguments + args->RangeBytes());
        if (args->countIndirect) passReads.emplace_back(args->countAddress, args->countAddress + 4);
    }
    passReads.insert(passReads.end(), inputs.inPlaceReads.begin(), inputs.inPlaceReads.end());
    const auto passImages = resources.StorageImages();
    std::vector<std::pair<VkImage, bool>> passAttachments;
    passAttachments.reserve(record.targets.size());
    for (const auto& target : record.targets) {
        if (target != nullptr) passAttachments.emplace_back(target->Image(), true);
    }
    for (const auto image : record.multisampleImages) passAttachments.emplace_back(image, true);
    const bool unknownMemory = addressBased || resources.UsesAddressTable() || resources.HasCopiedWrites();
    static_cast<void>(StorageTexture::StoreDrawnLinearTouching("draw access", [&](std::uint64_t begin, std::uint64_t end) { return std::any_of(passReads.begin(), passReads.end(), [&](const auto& read) { return read.first < end && begin < read.second; }) || resources.WritesOverlap(begin, static_cast<std::size_t>(end - begin)); }, unknownMemory));
    if (record.slotCapture != nullptr) record.slotCapture->linearReads.assign(passReads.begin(), passReads.end());
    recordPhase(RecordAccess);
    reuseCensus::mark(StructCensus::ReuseAccess);
    // Stage 4 M4: PrepareDrawBindings is resolve-class work inside the record phase.
    Stage4::Mark(Stage4::Bucket::Record);
    const auto drawUse = resources.BeginDrawUse(*recorder);
    const auto drawBindings = resources.PrepareDrawBindings(*recorder, record.moved, false, drawUse != nullptr ? drawUse->record.get() : nullptr);
    static const bool zcNoPassReads = std::getenv("APS5_ZC_MOVED_NO_PASSREADS") != nullptr;
    const std::size_t bindingReads = !zcNoPassReads && drawBindings != nullptr ? drawBindings->inPlaceReads.size() : 0;
    if (bindingReads != 0) passReads.insert(passReads.end(), drawBindings->inPlaceReads.begin(), drawBindings->inPlaceReads.end());
    Stage4::Mark(Stage4::Bucket::Pdb);
    recordPhase(RecordBindings);
    reuseCensus::mark(StructCensus::ReuseBindings);
    static const bool addressReadPasses = std::getenv("APS5_NO_ADDRESS_READ_PASSES") == nullptr;
    const bool readOnlyBda = addressReadPasses && resources.BdaReadOnly();
    outcome.addressDraw = addressBased;
    const bool unknownWrites = ((addressBased || resources.UsesAddressTable()) && !readOnlyBda) || resources.HasCopiedWrites();
    GuestRangeList passWrites = resources.GpuWrites();
    GuestRangeList passWriteOnly{};
    struct HazardWritesSlotThreadStorage {};
    auto& hazardWritesSlot = AgcDriver::ThreadScratch<std::vector<std::pair<std::uint64_t, std::uint64_t>>*, HazardWritesSlotThreadStorage>();
    if (const auto& writeOnly = resources.WriteOnlyRanges(); !writeOnly.empty() && !unknownMemory && WriteOnlyPasses()) {
        const auto listed = [&](const std::pair<std::uint64_t, std::uint64_t>& range, bool aligned) {
            return std::any_of(writeOnly.begin(), writeOnly.end(), [&](const auto& only) {
                if (!aligned) return only == range;
                const auto begin = only.first & ~std::uint64_t{3};
                const auto end = begin == only.first ? only.second : (only.second + 3) & ~std::uint64_t{3};
                return range.first == begin && range.second == end;
            });
        };
        const auto ownReads = passReads.begin() + static_cast<std::ptrdiff_t>(deviceReads);
        passReads.erase(std::remove_if(passReads.begin(), ownReads, [&](const auto& range) { return listed(range, true); }), ownReads);
        auto& hazardWrites = ShaderRecompiler::ThreadOwned(hazardWritesSlot);
        hazardWrites.clear();
        for (const auto& range : resources.GpuWrites()) {
            if (!listed(range, false)) hazardWrites.push_back(range);
        }
        passWrites = hazardWrites;
        passWriteOnly = writeOnly;
    }
    const PassAccess passAccess{passReads, passWrites, passImages, passAttachments, unknownMemory, unknownWrites, passWriteOnly};
    const bool fenced = state.depth.has_value() || !record.proxies.empty();
    resources.RequireGuestBuffers();
    if (UsesMeshWindow(shaders)) PrepareMeshWindow(context);
    const bool forcedPass = capture || readsTarget || (gpuIndirect && !inPassIndirect) || meshIndirect || captured || !record.staged.empty() || PixelFunctionLdsSubgroup(shaders) != 0u;
    if (auto* slot = record.slotCapture; slot != nullptr) {
        slot->filled = true;
        slot->passKey = passKey;
        slot->forced = forcedPass;
        slot->fenced = fenced;
        slot->inPassIndirect = inPassIndirect;
        slot->bindingsReplayable = drawBindings == nullptr || (drawBindings->cachedSet != nullptr && drawBindings->snapshots.empty());
        slot->unknownMemory = unknownMemory;
        slot->unknownWrites = unknownWrites;
        slot->passReads.assign(passReads.begin(), passReads.end() - static_cast<std::ptrdiff_t>(bindingReads));
        slot->passWrites.assign(passWrites.begin(), passWrites.end());
        slot->writeOnly.assign(passWriteOnly.begin(), passWriteOnly.end());
        slot->images = passImages;
        slot->attachments = passAttachments;
    }
    const auto start = recorder->StartDrawPass(passKey, forcedPass, fenced, passAccess);
    if (record.spaceKeep != nullptr) *record.spaceKeep = reserveSpaceKeep(*recorder, resources);
    recordPhase(RecordPass);
    reuseCensus::mark(StructCensus::ReusePass);
    for (const auto& target : record.multisampleTargets) KeepMultisampleTarget_nid_no_patch(target);
    const bool continued = start.continued;
    outcome.passContinued = continued;
    outcome.passBegun = !continued;
    outcome.passBreak = start.reason;
    const auto commands = start.commands;
    if (!continued) {
        recorder->PrepareSampleSlot();
        PerformanceTrace::Count(PerformanceTrace::RenderPassScope);
    }
    if (capture) captureInputs(context, *recorder, commands, resources, drawBindings, record.targets.empty() || record.targets.front() == nullptr ? 0 : record.targets.front()->Descriptor().baseAddress);
    // The draw's [gputime] class range: from its first barrier to the pass's trailing barrier (a
    // continued draw lies inside its pass's range).
    const auto drawTiming = !continued ? recorder->BeginGpuTiming(CommandClass::Draw) : Recorder::NoTiming;
    const auto passTimingKey = Recorder::GpuTimingEnabled() ? timingPassKey(state) : 0;
    if (drawTiming != Recorder::NoTiming) recorder->TagGpuTiming(drawTiming, passTimingKey);
    if (!continued && Recorder::BarrierValidate()) {
        // APS5_ARCHB_FIX_ALLOC_CHURN: a per-thread scratch (NoteAccess reads it before returning).
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ownedReads;
        struct ReadsSlotThreadStorage {};
        auto& readsSlot = AgcDriver::ThreadScratch<std::vector<std::pair<std::uint64_t, std::uint64_t>>*, ReadsSlotThreadStorage>();
        auto& reads = AgcDriver::ArchB::AllocChurnActive() ? ShaderRecompiler::ThreadOwned(readsSlot) : ownedReads;
        resources.InPlaceReads(reads);
        reads.insert(reads.end(), inputs.inPlaceReads.begin(), inputs.inPlaceReads.end());
        if (gpuIndirect) {
            reads.emplace_back(args->arguments, args->arguments + args->RangeBytes());
            if (args->countIndirect) reads.emplace_back(args->countAddress, args->countAddress + 4);
        }
        auto images = resources.StorageImages();
        for (const auto& target : record.targets) {
            if (target != nullptr) images.emplace_back(target->Image(), true);
        }
        recorder->NoteAccess(CommandClass::Draw, Recorder::Access{reads, resources.GpuWrites(), images, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | inputs.shaderStages, resources.HoldsLease()});
    }
    std::unique_ptr<DeviceBuffer> scratch;
    std::shared_ptr<Buffer> meshArguments;
    VkBuffer argumentBuffer = VK_NULL_HANDLE;
    VkDeviceSize argumentOffset = 0;
    bool rewritten = false;
    if (!meshIndirect && !inputs.restartTable.empty()) meshArguments = directMeshArguments(context, draw, inputs.restartTable);
    if (continued) {
        record.pipeline->Continue(commands, state, reuse);
        if (inPassIndirect) {
            recorder->NotePendingRead(args->arguments, static_cast<std::size_t>(args->RangeBytes()), Recorder::ReadKind::Indirect);
            argumentBuffer = record.indirect->argumentImport->buffer;
            argumentOffset = HostImportOffset(*record.indirect->argumentImport, args->arguments);
        }
    } else {
        for (const auto& proxy : record.proxies) {
            proxy->RecordAttachmentProxyLoad(commands, VK_IMAGE_LAYOUT_GENERAL);
            countBarrier(2);
        }
        if (start.barrier) {
            const VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
            context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
            countBarrier(1);
        }
        APS5_LOG_CHARS_OUT_DEBUG("Upload barrier recorded");
        if (inputs.vertexInputBehindGpuWrite) {
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
            countBarrier(1);
        }
        const auto pixelLdsSubgroup = PixelFunctionLdsSubgroup(shaders);
        if (pixelLdsSubgroup != 0u) PreparePixelFunctionLds(context, commands, pixelLdsSubgroup);
        if (captured) {
            recordCaptureBarrier(context, commands);
            countBarrier(1);
            meshArguments = state.tessellationCapture->header;
            argumentBuffer = meshArguments->Handle();
        } else if (meshIndirect) {
            meshArguments = recordMeshArguments(context, commands, recorder, true, state, draw, *record.indirect, countBarrier, inputs.restartTable);
            argumentBuffer = meshArguments->Handle();
        } else if (gpuIndirect) {
            rewritten = recordIndirectArguments(context, commands, recorder, true, *record.indirect, scratch, argumentBuffer, argumentOffset, countBarrier);
        }
        // Earlier recorded work (dispatches, the previous draw) wrote the images in the general
        // layout, which a lean draw renders in: no transitions.
        if (!record.reordered.empty()) {
            std::vector<std::shared_ptr<void>> kept;
            RecordSampleOrder(context, commands, record.reorderPass, record.reordered, true, kept);
            for (auto& object : kept) recorder->Keep(std::move(object));
        }
        for (const auto& write : record.staged) RecordStagedClear(context, commands, *write.temporary);
        APS5_LOG_OUT_DEBUG("Beginning pipeline renderExtent=%ux%u", state.renderExtent.width, state.renderExtent.height);
        record.pipeline->Begin(commands, *record.framebuffer, state.renderExtent, state, reuse);
    }
    APS5_LOG_CHARS_OUT_DEBUG("Pipeline Begin OK");
    recordPhase(RecordBegin);
    if (drawBindings != nullptr) {
        const auto set = drawBindings->allocation.set;
        const auto fragmentSet = drawBindings->fragmentAllocation.set;
        if (set != VK_NULL_HANDLE) context.Resolved(&DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, record.pipeline->Layout(), 0, 1, &set, 0, nullptr);
        if (fragmentSet != VK_NULL_HANDLE) context.Resolved(&DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, record.pipeline->Layout(), ShaderRecompiler::RuntimeAbi::FragmentDescriptorSet, 1, &fragmentSet, 0, nullptr);
    } else {
        resources.BindDrawUse(commands, record.pipeline->Layout(), drawUse.get());
    }
    if (std::ranges::any_of(shaders, [](const CompiledShader& shader) { return shader.program != nullptr && shader.program->functionLdsBuffer; })) BindPixelFunctionLds(context, commands, record.pipeline->Layout());
    if (UsesMeshWindow(shaders)) BindMeshWindow(context, commands, record.pipeline->Layout());
    APS5_LOG_CHARS_OUT_DEBUG("Resources bound");
    pushDrawConstants(*record.pipeline, commands, state, draw, shaders, resources, record.pushBytes, record.pushStages, meshArguments != nullptr ? meshArguments->DeviceAddress() : 0, !inputs.restartTable.empty(), inputs.meshLayerStride, reuse);
    APS5_LOG_CHARS_OUT_DEBUG("Push constants recorded");
    static const bool timeEachDraw = std::getenv("APS5_PROFILE_GPU_DRAWS") != nullptr;
    auto eachDrawTiming = Recorder::NoTiming;
    if (timeEachDraw) {
        std::uint64_t key = 0;
        for (const auto& shader : shaders) {
            if (shader.program != nullptr && (key == 0 || shader.stage == ShaderRecompiler::ShaderStage::Fragment)) key = shader.program->variantId;
        }
        eachDrawTiming = recorder->BeginInPassGpuTiming(Recorder::DrawTimingKey(key));
    }
    // APS5_PROFILE_GPU_DRAWS: the draw's own range inside its pass (not in a multiview pass, whose
    // timestamps would need one query per view).
    const auto ownTiming = Recorder::GpuDrawTimingEnabled() && state.renderLayers <= 1u ? recorder->BeginGpuTimingInPass(timingDrawKey(shaders, passTimingKey)) : Recorder::NoTiming;
    noteDrawFaultToken(context, *recorder, commands, shaders, draw, static_cast<std::uint32_t>(state.stages.mesh.has_value() ? AgcDriver::FaultStage::mesh : AgcDriver::FaultStage::graphics), record.indirect != nullptr ? record.indirect->args : nullptr);
    if (record.stencilReplay.empty()) recordDrawCommands(context, commands, state, draw, inputs, record.indirect, argumentBuffer, argumentOffset, trace, subdraw);
    else {
        recordStencilReplay(context, commands, state, draw, inputs, record.stencilReplay, shaders, resources, record.pushBytes, record.pushStages);
        for (const auto& [reference, pipeline] : record.stencilReplay) recorder->Keep(pipeline);
    }
    if (record.pipeline->SplitsFaces()) {
        record.pipeline->ContinueBackFaces(commands, state);
        recordDrawCommands(context, commands, state, draw, inputs, record.indirect, argumentBuffer, argumentOffset, trace, subdraw);
    }
    if (record.pipeline->SplitsSecondChannel()) {
        record.pipeline->ContinueSecondChannel(commands);
        recordDrawCommands(context, commands, state, draw, inputs, record.indirect, argumentBuffer, argumentOffset, trace, subdraw);
    }
    recorder->EndGpuTiming(ownTiming);
    recorder->EndGpuTiming(eachDrawTiming);
    if (meshArguments != nullptr) recorder->Keep(meshArguments);
    if (state.tessellationCapture != nullptr) {
        recorder->Keep(state.tessellationCapture->header);
        recorder->Keep(state.tessellationCapture->items);
    }
    if (args != nullptr) CountIndirectDraw(record.indirect->path, record.indirect->readMs, rewritten);
    auto checkRecords = indirectRecordCheck(record.indirect);
    APS5_LOG_CHARS_OUT_DEBUG("Draw recorded");
    if (StructCensus::Enabled()) noteDrawCensus(state, draw, record, passKey, drawBindings.get(), continued);
    reuseCensus::structure(state, draw, record, passKey, drawBindings.get(), continued);
    recordPhase(RecordCommands);
    std::function<void(VkCommandBuffer)> storeProxies;
    if (!continued && (!record.proxies.empty() || !record.reordered.empty() || record.depthLocations)) {
        storeProxies = [proxies = record.proxies, reordered = record.reordered, reorderPass = record.reorderPass, depthLocations = record.depthLocations, context, recorder](VkCommandBuffer passCommands) {
            for (const auto& proxy : proxies) {
                proxy->RecordAttachmentProxyStore(passCommands, VK_IMAGE_LAYOUT_GENERAL);
                Recorder::CountBarriers(Recorder::CommandClass::Draw, 2);
            }
            if (!reordered.empty()) {
                std::vector<std::shared_ptr<void>> kept;
                RecordSampleOrder(context, passCommands, reorderPass, reordered, false, kept);
                for (auto& object : kept) recorder->Keep(std::move(object));
            }
            if (depthLocations) {
                RecordDepthSampleLocations(context, passCommands, *depthLocations);
                Recorder::CountBarriers(Recorder::CommandClass::Draw, 2);
                recorder->Keep(depthLocations->keep);
            }
        };
    }
    reuseCensus::mark(StructCensus::ReuseRecord);
    recorder->LeaveRenderPassOpen(passKey, drawTiming, fenced, passAccess, std::move(storeProxies));
    recordPhase(RecordLeave);
    reuseCensus::mark(StructCensus::ReusePass);
    timer.phase(PhaseRecord);
    keepRecordedDraw(*recorder, record.resources, std::move(record.pipeline), std::move(record.framebuffer), inputs, std::move(record.targets), std::move(record.targetSlices), state.scissor, std::move(scratch), std::move(checkRecords), record.listed, record.completion, outcome, drawUse, record.spaceKeep != nullptr && *record.spaceKeep != nullptr ? record.spaceKeep->get() : nullptr);
    recordPhase(RecordKeep);
    reuseCensus::mark(StructCensus::ReuseKeep);
    reuseCensus::finish();
    timer.phase(PhaseKeep);
    if (!record.staged.empty()) {
        const auto merged = recorder->Commands();
        for (auto& write : record.staged) {
            RecordStagedMerge(context, merged, *write.temporary, write.target, write.mask, write.samples, write.area);
            recorder->Keep(write.temporary);
        }
    }
    if (record.waited) {
        // The wait the dispatch path makes for such work (source 3, "address-based"): the
        // write-back runs before the next packet, so the CPU never reads copied results or
        // touches a leased allocation before they landed.
        Recorder::CountSync(3);
        const auto ownBefore = timer.profile ? Recorder::ThreadWaitedMs() : 0.0;
        recorder->Sync();
        if (timer.profile) ownWaitedMs += Recorder::ThreadWaitedMs() - ownBefore;
        timer.phase(PhaseSync);
    }
}

// The state with the outputs the pixel shader lacks masked: Vulkan leaves attachments a pixel
// shader has no output for undefined, so their writes are masked (shaders that only store to
// images or buffers keep their targets as they were). Empty when no mask must change.
std::optional<State> maskedState(const State& state, std::uint32_t fragmentOutputs) {
    std::optional<State> masked;
    for (std::size_t index = 0; index < state.blends.size(); ++index) {
        if ((index < 32 && ((fragmentOutputs >> index) & 1u) != 0) || state.blends[index].colorWriteMask == 0) continue;
        if (!masked.has_value()) masked = state;
        masked->blends[index].colorWriteMask = 0;
    }
    return masked;
}

std::optional<DepthSampleLocations> passDepthLocations(const Context& context, const State& state, const Pipeline& pipeline) {
    if (!state.depth || state.depth->samples <= 1u || !state.customSampleLocations || !pipeline.SampleLocationsEnabled()) return std::nullopt;
    auto locations = DepthSurfaceSampleLocations(context, *state.depth);
    locations.grid = pipeline.HostSampleLocationGrid();
    locations.locations = SampleLocations(state);
    return locations;
}

// Debug aid: APS5_DUMP_TARGETS=<n> saves the first n renders of every color target as a raw file
// (u32 width, u32 height, u32 VkFormat, then tightly packed rows).
int DumpTargetLimit() {
    static const int dumpLimit = [] { const char* text = std::getenv("APS5_DUMP_TARGETS"); return text ? std::atoi(text) : 0; }();
    return dumpLimit;
}

// A draw whose targets are all resident is recorded into the device's open batch like a dispatch:
// the CPU does not wait for it, and the drain before it goes away (queue order and the barriers
// keep it behind earlier recorded work); see `recorded` in Draw for the buffers that still need a
// synchronous draw. Other draws keep their own synchronous batch. Debug aid: APS5_SYNC_DRAWS=1
// makes every draw synchronous.
bool RecordDraws() {
    static const bool recordDraws = std::getenv("APS5_SYNC_DRAWS") == nullptr;
    return recordDraws;
}

}

std::optional<std::string> KnownValidationFailure(const Context& context, std::span<const CompiledShader> shaders, const State& state) {
    auto& key = validationKeyScratch();
    if (!ValidationKey(context, shaders, state, key)) return std::nullopt;
    std::lock_guard lock(validationMutex());
    const auto& failures = validationFailures();
    if (const auto found = failures.find(key); found != failures.end()) return found->second;
    return std::nullopt;
}

namespace {

struct TargetBinding {
    ColorTarget color;
    bool gpuTiling = false;
    std::shared_ptr<StorageTexture> resident;
    bool proxied = false;
    std::unique_ptr<Buffer> transfer;
    bool banded = false;
    VkRect2D band{};
    std::size_t bandOffset = 0;
    std::uint32_t bandPitch = 0;
    std::unique_ptr<Buffer> tiled;
    std::unique_ptr<DeviceBuffer> tiledDevice;
    std::unique_ptr<DeviceBuffer> linearDevice;
    std::vector<std::byte> original;
    TileMipLayout mip{};
    std::unique_ptr<RenderTarget> target;
    bool multisampled = false;
    VkImage multisampleImage = VK_NULL_HANDLE;
    std::shared_ptr<void> multisampleOwner;
    std::shared_ptr<StagedTemporary> stagedTemporary;
    VkImageView stagedTarget = VK_NULL_HANDLE;
    std::unique_ptr<Buffer> dump;
};

struct PreparedDraw {
    DrawPreparation* preparation = nullptr;
    explicit PreparedDraw(bool profile) : timer(profile), outcome(profile) {}
    PreparedDraw(const PreparedDraw&) = delete;
    PreparedDraw& operator=(const PreparedDraw&) = delete;
    std::shared_ptr<const DrawRecipe>* recipeOut = nullptr;
    const DrawTraceContext* trace = nullptr;
    std::uint64_t shape = 0;
    bool pipelineStateReuse = false;
    std::vector<std::uint32_t> stencilReferences;
    std::optional<PerformanceTimer> timing;
    DrawTimer timer;
    DrawOutcome outcome;
    const ShaderResources::BuildTiming* built = nullptr;
    double waitedBefore = 0;
    double ownWaitedMs = 0;
    DrawInputs inputs;
    const Pm4::DrawParameters::IndirectDraw* args = nullptr;
    int dumpLimit = 0;
    std::vector<TargetBinding> targets;
    std::vector<VkImageView> targetViews;
    Recorder* recorder = nullptr;
    ResolvedResources resolved;
    IndirectRecord indirect;
    std::vector<Pm4::DrawArguments> records;
    bool completion = false;
    bool listed = false;
    bool recorded = false;
    bool lean = false;
    bool multisampled = false;
    std::optional<State> masked;
    std::vector<std::pair<std::uint32_t, std::shared_ptr<Pipeline>>> stencilReplay;
    std::shared_ptr<Pipeline> pipeline;
    std::vector<std::shared_ptr<StorageTexture>> owners;
    std::vector<std::shared_ptr<void>> framebufferOwners;
    std::shared_ptr<Framebuffer> framebuffer;
    std::vector<SampleOrderTarget> reordered;
    SampleOrderPass reorderPass;
    std::optional<DepthSampleLocations> depthLocations;
    std::shared_ptr<std::shared_ptr<const void>> spaceKeep;
    std::optional<VkImageView> takenDepthView;
    const DrawSlotIdentity* identity = nullptr;
    bool slotVerify = false;
    bool slotVerifyPatch = false;
    bool slotWordless = false;
    bool slotCapture = false;
    bool slotLearnOnly = false;
    std::optional<DrawInputs> slotDirectInputs;
};

void keepHeldSpace(PreparedDraw& prepared) {
    const auto& resources = prepared.resolved.resources;
    if (prepared.spaceKeep != nullptr || prepared.recorder == nullptr || resources == nullptr || !resources->SpaceReuse() || !resources->SpaceHeld()) return;
    prepared.spaceKeep = reserveSpaceKeep(*prepared.recorder, *resources);
}

bool ShapeMemoEnabled() {
    static const bool enabled = std::getenv("APS5_NO_SHAPE_MEMO") == nullptr;
    return enabled;
}

bool VerifyShapeMemo() {
    static const bool verify = std::getenv("APS5_VERIFY_SHAPE_MEMO") != nullptr;
    return verify;
}

void noteShapeMemoVerified(bool pipeline) {
    static std::atomic<std::uint64_t> targets{0};
    static std::atomic<std::uint64_t> pipelines{0};
    static std::atomic<std::int64_t> last{0};
    (pipeline ? pipelines : targets).fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto previous = last.load(std::memory_order_relaxed);
    if (now - previous < 10 || !last.compare_exchange_strong(previous, now)) return;
    std::fprintf(stderr, "[shape-memo] verified %llu targets and %llu pipelines\n", static_cast<unsigned long long>(targets.load()), static_cast<unsigned long long>(pipelines.load()));
}

struct ShapeChoice {
    VkImageLayout attachmentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t fragmentOutputs = 0;
    std::vector<std::uint64_t> stages;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<std::uint32_t> mainKey;
    std::vector<std::uint32_t> fragmentKey;
    std::uint64_t hash = 0;
    std::weak_ptr<Pipeline> pipeline;
};

struct ShapeEntry {
    VkDevice device = VK_NULL_HANDLE;
    std::vector<std::weak_ptr<StorageTexture>> targets;
    std::vector<ShapeChoice> choices;
};

struct ShapeMemoStorage {};
constexpr std::size_t ShapeMemoEntries = 4096;
constexpr std::size_t ShapeMemoChoices = 4;

std::unordered_map<std::uint64_t, ShapeEntry>& shapeMemo() {
    return HostThreadLocal<std::unordered_map<std::uint64_t, ShapeEntry>, ShapeMemoStorage>();
}

const ShapeEntry* findShape(const Context& context, std::uint64_t shape) {
    if (shape == 0 || !ShapeMemoEnabled()) return nullptr;
    const auto& entries = shapeMemo();
    const auto found = entries.find(shape);
    return found == entries.end() || found->second.device != context.device ? nullptr : &found->second;
}

std::shared_ptr<StorageTexture> shapeTarget(const Context& context, std::uint64_t shape, std::size_t index) {
    const auto* entry = findShape(context, shape);
    return entry == nullptr || index >= entry->targets.size() ? nullptr : entry->targets[index].lock();
}

bool sameStages(std::span<const std::uint64_t> kept, std::span<const CompiledShader> shaders) {
    if (kept.size() != shaders.size() * 4u + 1u) return false;
    std::size_t at = 0;
    for (const auto& shader : shaders) {
        const auto identity = shader.program->Identity();
        if (identity.Empty() || kept[at] != static_cast<std::uint64_t>(shader.stage) || kept[at + 1] != identity.low || kept[at + 2] != identity.high || kept[at + 3] != shader.pushConstantOffset) return false;
        at += 4;
    }
    return kept[at] == PushConstantStages(shaders);
}

bool sameChoice(const ShapeChoice& choice, const VertexInputLayout& input, const PipelineSetLayouts& layouts, std::span<const CompiledShader> shaders, VkImageLayout attachmentLayout, std::uint32_t fragmentOutputs) {
    const auto sameBinding = [](const VkVertexInputBindingDescription& a, const VkVertexInputBindingDescription& b) { return a.binding == b.binding && a.stride == b.stride && a.inputRate == b.inputRate; };
    const auto sameAttribute = [](const VkVertexInputAttributeDescription& a, const VkVertexInputAttributeDescription& b) { return a.location == b.location && a.binding == b.binding && a.format == b.format && a.offset == b.offset; };
    return choice.attachmentLayout == attachmentLayout && choice.fragmentOutputs == fragmentOutputs && std::ranges::equal(choice.bindings, input.bindings, sameBinding) && std::ranges::equal(choice.attributes, input.attributes, sameAttribute) && std::ranges::equal(choice.mainKey, layouts.mainKey) && std::ranges::equal(choice.fragmentKey, layouts.fragmentKey) && sameStages(choice.stages, shaders);
}

std::shared_ptr<Pipeline> shapePipeline(const Context& context, std::uint64_t shape, const VertexInputLayout& input, const PipelineSetLayouts& layouts, std::span<const CompiledShader> shaders, VkImageLayout attachmentLayout, std::uint32_t fragmentOutputs) {
    const auto* entry = findShape(context, shape);
    if (entry == nullptr) return nullptr;
    for (const auto& choice : entry->choices) {
        if (!sameChoice(choice, input, layouts, shaders, attachmentLayout, fragmentOutputs)) continue;
        const auto hash = choice.hash;
        const auto expected = choice.pipeline;
        return TouchCachedPipeline(context, hash, expected);
    }
    return nullptr;
}

void rememberShape(const Context& context, std::uint64_t shape, std::span<const TargetBinding> targets, const std::shared_ptr<Pipeline>* pipeline, std::uint64_t hash, const VertexInputLayout& input, const PipelineSetLayouts& layouts, std::span<const CompiledShader> shaders, VkImageLayout attachmentLayout, std::uint32_t fragmentOutputs) {
    if (shape == 0 || !ShapeMemoEnabled()) return;
    auto& entries = shapeMemo();
    if (entries.size() >= ShapeMemoEntries && !entries.contains(shape)) entries.clear();
    auto& entry = entries[shape];
    if (entry.device != context.device) {
        entry = {};
        entry.device = context.device;
    }
    entry.targets.resize(targets.size());
    for (std::size_t index = 0; index < targets.size(); ++index) {
        const auto& resident = targets[index].resident;
        if (resident == nullptr || targets[index].color.arrayLastSlice != 0) entry.targets[index].reset();
        else if (entry.targets[index].owner_before(resident) || resident.owner_before(entry.targets[index])) entry.targets[index] = resident;
    }
    if (pipeline == nullptr || hash == 0) return;
    auto& choices = entry.choices;
    const auto kept = std::ranges::find_if(choices, [&](const ShapeChoice& choice) { return sameChoice(choice, input, layouts, shaders, attachmentLayout, fragmentOutputs); });
    if (kept != choices.end()) {
        kept->hash = hash;
        kept->pipeline = *pipeline;
        std::rotate(choices.begin(), kept, std::next(kept));
        return;
    }
    ShapeChoice choice;
    choice.attachmentLayout = attachmentLayout;
    choice.fragmentOutputs = fragmentOutputs;
    for (const auto& shader : shaders) {
        const auto identity = shader.program->Identity();
        if (identity.Empty()) return;
        choice.stages.insert(choice.stages.end(), {static_cast<std::uint64_t>(shader.stage), identity.low, identity.high, shader.pushConstantOffset});
    }
    choice.stages.push_back(PushConstantStages(shaders));
    choice.bindings = input.bindings;
    choice.attributes = input.attributes;
    choice.mainKey.assign(layouts.mainKey.begin(), layouts.mainKey.end());
    choice.fragmentKey.assign(layouts.fragmentKey.begin(), layouts.fragmentKey.end());
    choice.hash = hash;
    choice.pipeline = *pipeline;
    if (choices.size() >= ShapeMemoChoices) choices.pop_back();
    choices.insert(choices.begin(), std::move(choice));
}

// Stage 4.1/4.2: Draw's use of a speculated pipeline (ResolveDraw): taken, refused (the draw's
// template, outputs, proxied attachments or layout differ), and under
// APS5_ARCHB_PIPE_RESOLVE_SPLIT_VERIFY=1 the lookups repeated and the ones that gave another pipeline.
struct ResolvePipelineCounters {
    std::atomic<std::uint64_t> speculated{0};
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
    std::atomic<std::uint64_t> verified{0};
    std::atomic<std::uint64_t> mismatches{0};
    std::atomic<std::uint64_t> precollectNs{0};
    std::atomic<std::uint64_t> pipelineNs{0};
};

ResolvePipelineCounters& resolvePipelineCounters() {
    static auto* counters = [] {
        auto* made = new ResolvePipelineCounters();
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_resolve_pipeline_speculated", made->speculated);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_resolve_pipeline_hits", made->hits);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_resolve_pipeline_misses", made->misses);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_resolve_pipeline_verified", made->verified);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_resolve_pipeline_mismatches", made->mismatches);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_resolve_precollect_ns", made->precollectNs);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_resolve_pipeline_ns", made->pipelineNs);
        return made;
    }();
    return *counters;
}

bool resolvePipelineVerify() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_PIPE_RESOLVE_SPLIT_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

std::uint64_t proxiedMask(const std::vector<TargetBinding>& targets) {
    std::uint64_t mask = 0;
    for (std::size_t index = 0; index < targets.size() && index < 64; ++index) {
        if (targets[index].proxied) mask |= std::uint64_t{1} << index;
    }
    return mask;
}

// The pipeline state Draw builds for these fragment outputs and proxied attachments (prepareDraw).
const State& pipelineStateFor(const Context& context, const State& state, std::uint32_t fragmentOutputs, std::uint64_t proxied, std::optional<State>& masked) {
    masked = maskedState(state, fragmentOutputs);
    for (std::size_t index = 0; index < state.colors.size() && index < 64; ++index) {
        if ((proxied >> index & 1u) == 0) continue;
        if (!masked.has_value()) masked = state;
        masked->colors[index].format = AttachmentProxyFormat(context, state.colors[index].format);
        masked->blends.at(state.colors[index].exportIndex).colorWriteMask &= VK_COLOR_COMPONENT_R_BIT;
    }
    return masked.has_value() ? *masked : state;
}

namespace archcSlots {

constexpr std::size_t MaxStages = 8;
constexpr std::size_t TableLimit = 8192;

enum Refusal : std::size_t { RefusedNew, RefusedSwitches, RefusedRecorder, RefusedInputs, RefusedTargets, RefusedDepth, RefusedTemplate, RefusedProof, RefusedMoved, RefusedArgs, RefusedObjects, RefusedTemplateGone, RefusedTemplateMembership, RefusalCount };
constexpr std::array<const char*, RefusalCount> RefusalNames{"new", "switches", "recorder", "inputs", "targets", "depth", "template", "proof", "moved", "args", "objects", "template_gone", "template_membership"};
enum Ineligible : std::size_t { IneligiblePath, IneligibleRecord, IneligibleShape, IneligibleTargets, IneligibleInputs, IneligibleTemplate, IneligibleIdentity, IneligibleUnrecorded, IneligibleWaited, IneligibleListed, IneligibleCompletion, IneligibleMultisampled, IneligiblePass, IneligibleCopied, IneligibleLease, IneligibleAddress, IneligibleNotHit, IneligibleNotReusable, IneligibleMembership, IneligibleFaultChecks, IneligibleCount };
constexpr std::array<const char*, IneligibleCount> IneligibleNames{"path", "record", "shape", "targets", "inputs", "template", "identity", "unrecorded", "waited", "listed", "completion", "multisampled", "pass", "copied", "lease", "address", "not_hit", "not_reusable", "membership", "fault_checks"};
enum Step : std::size_t { StepChecks, StepInputs, StepTargets, StepDepth, StepProof, StepMoved, StepArgs, StepBindings, StepRecord, StepKeep, StepCount };
constexpr std::array<const char*, StepCount> StepNames{"checks", "inputs", "targets", "depth", "proof", "moved", "args", "bindings", "record", "keep"};
enum Component : std::size_t { ComponentTargets, ComponentTemplate, ComponentObjects, ComponentInputs, ComponentArgs, ComponentPass, ComponentPush, ComponentCount };
constexpr std::array<const char*, ComponentCount + 1> ComponentNames{"targets", "template", "objects", "inputs", "args", "pass", "push", "ineligible"};
enum MissKind : std::size_t { MissState, MissPrograms, MissArguments, MissDraw, MissOther, MissNewState, MissNewProgram, MissNewArguments, MissNewDraw, MissCount };
constexpr std::array<const char*, MissCount> MissNames{"state", "programs", "arguments", "draw", "other", "new_state", "new_program", "new_arguments", "new_draw"};
constexpr std::array<const char*, static_cast<std::size_t>(ArchC::Probe::Count)> ProbeNames{"rv_prelude", "rv_complete", "rv_stamp", "rv_scan", "rv_imports", "rv_staging", "rv_finish", "dv_htile", "dv_settle", "dv_find", "dv_planes", "dv_sync", "dv_written", "dv_stale", "dv_note", "in_validate", "in_index", "bd_reuse", "bd_flush", "bd_linear", "bd_prepare", "rc_start", "rc_timing", "rc_begin", "rc_begin_new", "rc_bind", "rc_push", "rc_draw", "rc_leave", "kp_kept", "kp_storage", "kp_markgpu", "kp_complete", "kp_dirty"};
constexpr std::uint64_t TrimNs = 40000;
enum OutcomeKind : int { OutcomeReplayed = 0, OutcomeVerified = 1, OutcomeRefused = 2, OutcomeNew = 3 };

struct Counters {
    struct Kind {
        std::atomic<std::uint64_t> hits{0}, captures{0}, verifyChecks{0}, verifyMismatches{0};
        std::array<std::atomic<std::uint64_t>, RefusalCount> refused{};
        std::array<std::atomic<std::uint64_t>, IneligibleCount> ineligible{};
    };
    std::array<Kind, 2> kinds;
    std::atomic<std::uint64_t> hits{0}, captures{0}, legacy{0}, verifyChecks{0}, verifyMismatches{0}, prelockSkips{0}, replayNs{0}, evictions{0};
    std::array<std::atomic<std::uint64_t>, RefusalCount> refused{};
    std::array<std::atomic<std::uint64_t>, IneligibleCount> ineligible{};
    std::array<std::atomic<std::uint64_t>, StepCount> stepNs{};
    // Trimmed per-step time (STEPS=1): samples under 40 us, their count, and the samples over.
    std::array<std::atomic<std::uint64_t>, StepCount> stepTrimNs{}, stepTrimN{}, stepOut{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(ArchC::Probe::Count)> probeNs{}, probeN{}, probeOut{};
    std::array<std::atomic<std::uint64_t>, ComponentCount + 1> mismatched{};
    std::array<std::atomic<std::uint64_t>, MissCount> misses{};
    Counters() {
        using AgcDriver::ArchB::Recorder;
        for (std::size_t kind = 0; kind < kinds.size(); ++kind) {
            auto& entry = kinds[kind];
            const auto suffix = kind == 0 ? "_direct" : "_indirect";
            const auto name = [&](const std::string& prefix) { return prefix + suffix; };
            Recorder::RegisterCounter(name("archc_replay_hits").c_str(), entry.hits);
            Recorder::RegisterCounter(name("archc_replay_captures").c_str(), entry.captures);
            Recorder::RegisterCounter(name("archc_replay_verify_checks").c_str(), entry.verifyChecks);
            Recorder::RegisterCounter(name("archc_replay_verify_mismatches").c_str(), entry.verifyMismatches);
            for (std::size_t i = 0; i < RefusalCount; ++i) Recorder::RegisterCounter(name(std::string("archc_replay_refused_") + RefusalNames[i]).c_str(), entry.refused[i]);
            for (std::size_t i = 0; i < IneligibleCount; ++i) Recorder::RegisterCounter(name(std::string("archc_replay_ineligible_") + IneligibleNames[i]).c_str(), entry.ineligible[i]);
        }
        Recorder::RegisterCounter("archc_replay_hits", hits);
        Recorder::RegisterCounter("archc_replay_captures", captures);
        Recorder::RegisterCounter("archc_replay_legacy", legacy);
        Recorder::RegisterCounter("archc_replay_verify_checks", verifyChecks);
        Recorder::RegisterCounter("archc_replay_verify_mismatches", verifyMismatches);
        Recorder::RegisterCounter("archc_replay_prelock_skips", prelockSkips);
        Recorder::RegisterCounter("archc_replay_ns", replayNs);
        Recorder::RegisterCounter("archc_replay_evictions", evictions);
        for (std::size_t i = 0; i < RefusalCount; ++i) Recorder::RegisterCounter((new std::string(std::string("archc_replay_refused_") + RefusalNames[i]))->c_str(), refused[i]);
        for (std::size_t i = 0; i < IneligibleCount; ++i) Recorder::RegisterCounter((new std::string(std::string("archc_replay_ineligible_") + IneligibleNames[i]))->c_str(), ineligible[i]);
        for (std::size_t i = 0; i < StepCount; ++i) Recorder::RegisterCounter((new std::string(std::string("archc_replay_step_ns_") + StepNames[i]))->c_str(), stepNs[i]);
        for (std::size_t i = 0; i < StepCount; ++i) {
            Recorder::RegisterCounter((new std::string(std::string("archc_replay_step_trim_ns_") + StepNames[i]))->c_str(), stepTrimNs[i]);
            Recorder::RegisterCounter((new std::string(std::string("archc_replay_step_trim_n_") + StepNames[i]))->c_str(), stepTrimN[i]);
            Recorder::RegisterCounter((new std::string(std::string("archc_replay_step_out_") + StepNames[i]))->c_str(), stepOut[i]);
        }
        for (std::size_t i = 0; i < ProbeNames.size(); ++i) {
            Recorder::RegisterCounter((new std::string(std::string("archc_replay_probe_ns_") + ProbeNames[i]))->c_str(), probeNs[i]);
            Recorder::RegisterCounter((new std::string(std::string("archc_replay_probe_n_") + ProbeNames[i]))->c_str(), probeN[i]);
            Recorder::RegisterCounter((new std::string(std::string("archc_replay_probe_out_") + ProbeNames[i]))->c_str(), probeOut[i]);
        }
        for (std::size_t i = 0; i < MissCount; ++i) Recorder::RegisterCounter((new std::string(std::string("archc_replay_miss_") + MissNames[i]))->c_str(), misses[i]);
        for (std::size_t i = 0; i <= ComponentCount; ++i) Recorder::RegisterCounter((new std::string(std::string("archc_replay_verify_mismatch_") + ComponentNames[i]))->c_str(), mismatched[i]);
    }
};

Counters& counters() {
    static Counters made;
    return made;
}

// Arch C P3b cuts (C_REPLAY_*): uses, refusals (the step ran live), VERIFY checks and mismatches.
// (C_REPLAY_QUIET_TEMPLATE counts in ShaderResources.cpp: archc_replay_p3b_quiet_template_*.)
// (C_REPLAY_DEPTH_EPOCH counts in DepthSurface.cpp: archc_replay_p3b_depth_epoch_*.)
enum Cut : std::size_t { CutSlotMoved, CutSlotBindings, CutSlotInputs, CutPatchSlots, CutContinue, CutCount };
constexpr std::array<const char*, CutCount> CutNames{"slot_moved", "slot_bindings", "slot_inputs", "patch_slots", "continue"};
constexpr std::array<const char*, CutCount> CutVerifyVariables{"APS5_ARCHB_C_REPLAY_SLOT_BINDINGS_VERIFY", "APS5_ARCHB_C_REPLAY_SLOT_BINDINGS_VERIFY", "APS5_ARCHB_C_REPLAY_SLOT_INPUTS_VERIFY", "APS5_ARCHB_C_REPLAY_PATCH_SLOTS_VERIFY", "APS5_ARCHB_C_REPLAY_CONTINUE_VERIFY"};

struct CutCounters {
    std::array<std::atomic<std::uint64_t>, CutCount> hits{}, live{}, verifyChecks{}, verifyMismatches{};
    CutCounters() {
        using AgcDriver::ArchB::Recorder;
        for (std::size_t i = 0; i < CutCount; ++i) {
            Recorder::RegisterCounter((new std::string(std::string("archc_replay_p3b_") + CutNames[i] + "_hits"))->c_str(), hits[i]);
            Recorder::RegisterCounter((new std::string(std::string("archc_replay_p3b_") + CutNames[i] + "_live"))->c_str(), live[i]);
            Recorder::RegisterCounter((new std::string(std::string("archc_replay_p3b_") + CutNames[i] + "_verify_checks"))->c_str(), verifyChecks[i]);
            Recorder::RegisterCounter((new std::string(std::string("archc_replay_p3b_") + CutNames[i] + "_verify_mismatches"))->c_str(), verifyMismatches[i]);
        }
    }
};

CutCounters& cutCounters() {
    static CutCounters made;
    return made;
}

void cutHit(Cut cut) {
    cutCounters().hits[cut].fetch_add(1, std::memory_order_relaxed);
}

void cutLive(Cut cut) {
    cutCounters().live[cut].fetch_add(1, std::memory_order_relaxed);
}

// One use in N (the cut's _VERIFY=N) also runs the live step and compares; counts the check.
bool cutVerifyNow(Cut cut) {
    static const std::array<std::uint64_t, CutCount> periods = [] {
        std::array<std::uint64_t, CutCount> made{};
        for (std::size_t i = 0; i < CutCount; ++i) {
            const char* value = std::getenv(CutVerifyVariables[i]);
            made[i] = value == nullptr ? 0 : static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10));
        }
        return made;
    }();
    if (periods[cut] == 0) return false;
    static std::array<std::uint64_t, CutCount> uses{};
    if (++uses[cut] % periods[cut] != 0) return false;
    cutCounters().verifyChecks[cut].fetch_add(1, std::memory_order_relaxed);
    return true;
}

void cutMismatch(Cut cut, const char* what) {
    const auto previous = cutCounters().verifyMismatches[cut].fetch_add(1, std::memory_order_relaxed);
    if (previous < 8) std::fprintf(stderr, "[archc] replay cut %s VERIFY mismatch: %s\n", CutNames[cut], what);
}

bool cutEnabled(AgcDriver::ArchB::Feature feature) {
    return AgcDriver::ArchB::Switches::Enabled(feature);
}

bool sameMoved(std::span<const ShaderResources::MovedBuffer> a, std::span<const ShaderResources::MovedBuffer> b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].allocation != b[i].allocation || a[i].address != b[i].address || a[i].size != b[i].size || !std::ranges::equal(a[i].words, b[i].words)) return false;
    }
    return true;
}

int& lastOutcome() {
    struct State { int value = -1; };
    struct MadeStorage {};
    return AgcDriver::ThreadScratch<State, MadeStorage>().value;
}

bool active() {
    using AgcDriver::ArchB::Feature;
    return AgcDriver::ArchB::Switches::Enabled(Feature::ArchcDrawReplay) && AgcDriver::ArchB::Switches::Enabled(Feature::ArchcTargetFastpath) && AgcDriver::ArchB::Switches::Enabled(Feature::ArchcImageEpochs) && GuestMemory::IsCommitterThread();
}

bool diagnosticsBlock() {
    static const bool blocked = std::getenv("APS5_CHECK_INDIRECT_ARGS") != nullptr || std::getenv("APS5_DRAW_TRANSITIONS") != nullptr || std::getenv("APS5_NO_RESIDENT_TARGETS") != nullptr || std::getenv("APS5_NO_GPU_INDIRECT_DRAW") != nullptr || std::getenv("APS5_TRACE_DRAWS") != nullptr || DumpTargetLimit() != 0 || !RecordDraws() || CaptureInputsEnabled() || StructCensus::Enabled() || StructCensus::ReuseCensusEnabled();
    return blocked || Recorder::BarrierValidate();
}

std::uint64_t verifyPeriod() {
    static const std::uint64_t period = [] {
        const char* value = std::getenv("APS5_ARCHB_C_DRAW_REPLAY_VERIFY");
        return value == nullptr ? std::uint64_t{0} : static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10));
    }();
    return period;
}

bool verifyNow() {
    const auto period = verifyPeriod();
    if (period == 0) return false;
    struct ChecksThreadStorage {};
    auto& checks = AgcDriver::ThreadScratch<std::uint64_t, ChecksThreadStorage>();
    return ++checks % period == 0;
}

bool stepTiming() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_C_DRAW_REPLAY_STEPS");
        return value != nullptr && (value[0] == '1' || value[0] == '2');
    }();
    return enabled;
}

// STEPS=2 also times sub-steps inside the shared functions a replay calls (ArchC::ProbeAt).
bool probeTiming() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_C_DRAW_REPLAY_STEPS");
        return value != nullptr && value[0] == '2';
    }();
    return enabled;
}

std::uint64_t probeNow() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The committer's last probe or step mark (only the committer probes).
std::uint64_t probeLast = 0;

void addTrimmed(std::atomic<std::uint64_t>& ns, std::atomic<std::uint64_t>& n, std::atomic<std::uint64_t>& out, std::uint64_t sample) {
    if (sample < TrimNs) {
        ns.fetch_add(sample, std::memory_order_relaxed);
        n.fetch_add(1, std::memory_order_relaxed);
    } else {
        out.fetch_add(1, std::memory_order_relaxed);
    }
}

std::uint32_t currentSwitches() {
    using AgcDriver::ArchB::Feature;
    using AgcDriver::ArchB::Switches;
    std::uint32_t bits = 0;
    bits |= Recorder::ZeroCopyStorageActive() ? 1u : 0u;
    bits |= Recorder::ZeroCopyInputsActive() ? 2u : 0u;
    bits |= Switches::Enabled(Feature::ZeroCopyMoved) ? 4u : 0u;
    bits |= Switches::Enabled(Feature::FixDrawSetPool) ? 8u : 0u;
    bits |= Switches::Enabled(Feature::PipeDrawReuse) ? 16u : 0u;
    return bits;
}

struct SlotTemplateOwnership {
    std::weak_ptr<ShaderResources> resources;
    std::vector<std::weak_ptr<ShaderResources::TemplateSets>> sets;
    ~SlotTemplateOwnership() {
        std::unique_lock lock(GuestMemory::GpuMutex(), std::defer_lock);
        if (!GuestMemory::GpuMutex().HeldByThisThread()) lock.lock();
        const auto owner = resources.lock();
        if (owner == nullptr) return;
        for (const auto& weak : sets) if (const auto set = weak.lock()) owner->DropTemplateSets(set.get());
    }
};

struct Slot {
    std::uint64_t lastUse = 0;
    // C_REPLAY_PATCH_SLOTS: keyed without the patch words of its patched stages (no static moved list, no
    // kept moved list or cached moved set: the replay takes both live).
    bool wordless = false;
    const DriverDetail::GatherCommitTemplate* gatherTemplate = nullptr;
    std::weak_ptr<const DriverDetail::GatherCommitTemplate> gatherHeld;
    VkDevice device = VK_NULL_HANDLE;
    const void* stateOwner = nullptr;
    const State* state = nullptr;
    std::weak_ptr<const void> stateHeld;
    std::size_t stageCount = 0;
    std::array<CompiledShader, MaxStages> stages{};
    std::array<std::weak_ptr<const void>, MaxStages> owners{};
    std::array<const void*, MaxStages> ownerRaw{};
    std::array<std::weak_ptr<const void>, MaxStages> variants{};
    std::array<const void*, MaxStages> variantRaw{};
    std::array<std::vector<std::uint32_t>, MaxStages> words{};
    std::array<std::uint8_t, MaxStages> patched{};
    Pm4::DrawParameters draw{};
    bool pipelineStateReuse = false;
    std::uint32_t switches = 0;
    bool replayedLast = false;
    bool completion = false;
    std::vector<std::weak_ptr<StorageTexture>> targets;
    std::vector<const StorageTexture*> targetRaw;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> slices;
    std::vector<VkImageView> views;
    std::uint64_t depthReleases = 0;
    std::weak_ptr<ShaderResources> resources;
    const ShaderResources* resourcesRaw = nullptr;
    std::uint64_t membership = 0;
    ResourceCache::Key contentKey;
    std::uint64_t touched = ~std::uint64_t{0};
    std::weak_ptr<Pipeline> pipeline;
    const Pipeline* pipelineRaw = nullptr;
    std::weak_ptr<Framebuffer> framebuffer;
    const Framebuffer* framebufferRaw = nullptr;
    std::uint32_t fragmentOutputs = 0;
    VkPipelineStageFlags shaderStages = 0;
    bool movedStatic = false;
    std::vector<ShaderResources::MovedBuffer> moved;
    std::vector<std::vector<std::uint32_t>> movedWords;
    std::size_t argumentRead = 0;
    std::size_t argumentLinearRead = 0;
    SlotPassCapture pass;
    std::array<std::byte, PipelinePushConstantBytes> push{};
    VkShaderStageFlags pushStages = 0;
    // C_REPLAY_SLOT_BINDINGS: the last live moved list of a slot whose list is not static (words copied into
    // liveWords), and the cached moved set its last bindings used (weakly: the template's cache owns it).
    bool liveKept = false;
    std::vector<ShaderResources::MovedBuffer> liveMoved;
    std::vector<std::vector<std::uint32_t>> liveWords;
    std::weak_ptr<void> bindSet;
    VkDescriptorSet bindVertex = VK_NULL_HANDLE;
    VkDescriptorSet bindFragment = VK_NULL_HANDLE;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> bindReads;
    HostImportsProof bindProof{};
    std::shared_ptr<SlotTemplateOwnership> templateOwnership;
    std::array<std::weak_ptr<ShaderResources::TemplateSets>, 4> templates{};
    std::array<const ShaderResources::TemplateSets*, 4> templateRaw{};
    std::array<std::uint64_t, 4> templateUse{};
};

void keepLiveMoved(Slot& slot, std::span<const ShaderResources::MovedBuffer> moved) {
    slot.liveWords.clear();
    slot.liveMoved.clear();
    slot.liveWords.reserve(moved.size());
    slot.liveMoved.reserve(moved.size());
    for (const auto& entry : moved) slot.liveWords.emplace_back(entry.words.begin(), entry.words.end());
    for (std::size_t i = 0; i < moved.size(); ++i) {
        auto entry = moved[i];
        entry.words = slot.liveWords[i];
        slot.liveMoved.push_back(entry);
    }
    slot.liveKept = true;
    slot.bindSet.reset();
}

struct Table {
    std::unordered_map<std::uint64_t, Slot> slots;
    std::uint64_t uses = 0;
};

Table& table() {
    struct SlotStorage {};
    return AgcDriver::ThreadScratch<Table, SlotStorage>();
}

std::uint64_t mix(std::uint64_t hash, std::uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    return hash;
}

bool sameIndirect(const Pm4::DrawParameters::IndirectDraw& a, const Pm4::DrawParameters::IndirectDraw& b) {
    return a.opcode == b.opcode && a.recordBytes == b.recordBytes && a.stride == b.stride && a.count == b.count && a.countIndirect == b.countIndirect && a.countAddress == b.countAddress && a.baseVertexLocation == b.baseVertexLocation && a.startInstanceLocation == b.startInstanceLocation && a.drawIndexLocation == b.drawIndexLocation && a.drawIndexEnabled == b.drawIndexEnabled && a.indxOffset == b.indxOffset && a.indexLimitKnown == b.indexLimitKnown && a.vertexRule == b.vertexRule && a.instanceRule == b.instanceRule && a.vertexConstant == b.vertexConstant && a.instanceConstant == b.instanceConstant && a.baseVertexSgpr == b.baseVertexSgpr && a.startInstanceSgpr == b.startInstanceSgpr && a.drawIndexSgpr == b.drawIndexSgpr && a.instanceFromBase == b.instanceFromBase && a.vertexFromBase == b.vertexFromBase;
}

bool sameDraw(const Pm4::DrawParameters& a, const Pm4::DrawParameters& b) {
    if (a.indexAddress != b.indexAddress || a.indexCount != b.indexCount || a.indexSize != b.indexSize || a.instanceCount != b.instanceCount || a.flags != b.flags || a.indexed != b.indexed || a.firstVertex != b.firstVertex || a.firstInstance != b.firstInstance || a.indirect.has_value() != b.indirect.has_value()) return false;
    return !a.indirect.has_value() || sameIndirect(*a.indirect, *b.indirect);
}

// C_REPLAY_PATCH_SLOTS: an identity with a data-word patched stage whose words reach only its FlattenedSrt binding
// (no buffer-base words), which the draw content key leaves out (movable buffers) and the moved list carries.
bool wordlessIdentity(const DrawSlotIdentity& identity) {
    if (!cutEnabled(AgcDriver::ArchB::Feature::ArchcReplayPatchSlots) || !MovableBuffers()) return false;
    return std::ranges::any_of(identity.stages, [](const DrawSlotStage& stage) { return stage.patched && !stage.bufferBases; });
}

std::uint64_t keyOf(VkDevice device, const DrawSlotIdentity& identity, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, bool wordless = false) {
    std::uint64_t hash = mix(0x534c4f54u, reinterpret_cast<std::uintptr_t>(device));
    const bool live = identity.gather != nullptr;
    if (live) hash = mix(hash, reinterpret_cast<std::uintptr_t>(identity.gatherTemplate.get()));
    hash = mix(hash, reinterpret_cast<std::uintptr_t>(identity.state.get()));
    for (std::size_t i = 0; i < shaders.size(); ++i) {
        const auto& stage = identity.stages[i];
        hash = mix(hash, reinterpret_cast<std::uintptr_t>(stage.owner.get()));
        hash = mix(hash, reinterpret_cast<std::uintptr_t>(stage.variant.get()));
        hash = mix(hash, (static_cast<std::uint64_t>(shaders[i].stage) << 32u) | shaders[i].pushConstantOffset);
        if (live) continue;
        hash = mix(hash, (stage.patched ? 1u : 0u) | (stage.bufferBases ? 2u : 0u));
        if (wordless && stage.patched && !stage.bufferBases) continue;
        for (const auto word : stage.words) hash = mix(hash, word);
    }
    if (!live) hash = mix(hash, draw.indexAddress);
    hash = mix(hash, (static_cast<std::uint64_t>(draw.indexCount) << 32u) | draw.indexSize);
    if (draw.indirect) hash = mix(hash, (static_cast<std::uint64_t>(draw.indirect->count) << 32u) | draw.indirect->stride);
    return hash;
}

bool usableIdentity(const DrawSlotIdentity& identity, const State& state, std::span<const CompiledShader> shaders) {
    if (identity.state == nullptr || shaders.size() > MaxStages || identity.stages.size() != shaders.size()) return false;
    static_cast<void>(state);
    for (std::size_t i = 0; i < shaders.size(); ++i) {
        const auto& stage = identity.stages[i];
        if (shaders[i].program == nullptr || stage.owner == nullptr) return false;
        if (stage.patched ? stage.variant == nullptr : stage.owner.get() != static_cast<const void*>(shaders[i].program)) return false;
    }
    return true;
}

bool sameIdentity(const Slot& slot, VkDevice device, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, const DrawSlotIdentity& identity, bool pipelineStateReuse, bool wordless) {
    static_cast<void>(state);
    if (slot.gatherTemplate != (identity.gather != nullptr ? identity.gatherTemplate.get() : nullptr)) return false;
    if (identity.gather != nullptr && slot.gatherHeld.lock() != identity.gatherTemplate) return false;
    if (slot.device != device || slot.stateOwner != identity.state.get() || slot.stageCount != shaders.size() || slot.pipelineStateReuse != pipelineStateReuse || slot.stateHeld.expired() || slot.wordless != wordless) return false;
    for (std::size_t i = 0; i < shaders.size(); ++i) {
        const auto& stage = identity.stages[i];
        const bool words = identity.gather == nullptr && !(wordless && stage.patched && !stage.bufferBases);
        if (slot.ownerRaw[i] != stage.owner.get() || slot.variantRaw[i] != stage.variant.get() || slot.patched[i] != ((stage.patched ? 1u : 0u) | (stage.bufferBases ? 2u : 0u)) || slot.stages[i].stage != shaders[i].stage || slot.stages[i].pushConstantOffset != shaders[i].pushConstantOffset || (words && slot.words[i] != stage.words) || slot.owners[i].expired() || (stage.patched && slot.variants[i].expired())) return false;
    }
    if (identity.gather != nullptr) {
        auto kept = slot.draw;
        kept.indexAddress = draw.indexAddress;
        if (kept.indirect && draw.indirect) kept.indirect->countAddress = draw.indirect->countAddress;
        return sameDraw(kept, draw);
    }
    return sameDraw(slot.draw, draw);
}

struct Seen {
    const void* state = nullptr;
    std::array<const void*, MaxStages> programs{};
    Pm4::DrawParameters draw{};
};

void classifyMiss(const DrawSlotIdentity& identity, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders) {
    struct ByShapeSlotStorage {};
    auto& byShape = AgcDriver::ThreadScratch<std::unordered_map<std::uint64_t, Seen>, ByShapeSlotStorage>();
    std::uint64_t shape = 0x5348u;
    for (const auto& shader : shaders) shape = mix(shape, (static_cast<std::uint64_t>(shader.stage) << 32u) | shader.pushConstantOffset);
    shape = mix(shape, shaders.empty() || shaders.front().program == nullptr ? 0 : shaders.front().program->variantId);
    shape = mix(shape, shaders.size() < 2 || shaders[1].program == nullptr ? 0 : shaders[1].program->variantId);
    shape = mix(shape, draw.indexed ? 1u : 0u);
    auto& seen = byShape[shape];
    Seen now;
    now.state = identity.state.get();
    for (std::size_t i = 0; i < shaders.size() && i < MaxStages; ++i) now.programs[i] = shaders[i].program;
    now.draw = draw;
    MissKind kind = MissOther;
    if (seen.state == nullptr) kind = MissOther;
    else if (seen.state != now.state) kind = MissState;
    else if (seen.programs != now.programs) kind = MissPrograms;
    else if (draw.indirect && seen.draw.indirect && draw.indirect->arguments != seen.draw.indirect->arguments) kind = MissArguments;
    else if (!sameDraw(seen.draw, draw)) kind = MissDraw;
    counters().misses[kind].fetch_add(1, std::memory_order_relaxed);
    seen = now;
    struct SeenSets {
        std::unordered_set<std::uint64_t> states;
        std::unordered_set<std::uint64_t> programs;
        std::unordered_set<std::uint64_t> arguments;
        std::unordered_set<std::uint64_t> draws;
    };
    struct SetsSlotStorage {};
    auto& sets = AgcDriver::ThreadScratch<SeenSets, SetsSlotStorage>();
    auto& states = sets.states;
    auto& programs = sets.programs;
    auto& arguments = sets.arguments;
    auto& draws = sets.draws;
    if (states.insert(reinterpret_cast<std::uintptr_t>(identity.state.get())).second) counters().misses[MissNewState].fetch_add(1, std::memory_order_relaxed);
    bool newProgram = false;
    for (const auto& shader : shaders) newProgram |= programs.insert(reinterpret_cast<std::uintptr_t>(shader.program)).second;
    if (newProgram) counters().misses[MissNewProgram].fetch_add(1, std::memory_order_relaxed);
    if (draw.indirect && arguments.insert(draw.indirect->arguments).second) counters().misses[MissNewArguments].fetch_add(1, std::memory_order_relaxed);
    std::uint64_t drawHash = mix(mix(mix(draw.indexAddress, draw.indexCount), draw.indirect ? draw.indirect->count : 0u), draw.indirect ? draw.indirect->stride : 0u);
    drawHash = mix(mix(drawHash, draw.firstVertex), draw.indirect ? (static_cast<std::uint64_t>(draw.indirect->vertexRule) << 8u) | static_cast<std::uint64_t>(draw.indirect->instanceRule) | (static_cast<std::uint64_t>(draw.indirect->indxOffset) << 16u) : 0u);
    if (draws.insert(drawHash).second) counters().misses[MissNewDraw].fetch_add(1, std::memory_order_relaxed);
    if (states.size() > 1000000) states.clear();
    if (programs.size() > 1000000) programs.clear();
    if (arguments.size() > 1000000) arguments.clear();
    if (draws.size() > 1000000) draws.clear();
    if (byShape.size() > 65536) byShape.clear();
}

bool sightedBefore(std::uint64_t key) {
    constexpr std::size_t Sightings = 16384;
    struct SeenThreadStorage {};
    auto& seen = AgcDriver::ThreadScratch<std::array<std::uint64_t, Sightings>, SeenThreadStorage>();
    auto& entry = seen[static_cast<std::size_t>((key ^ (key >> 29u)) % Sightings)];
    if (entry == key) return true;
    entry = key;
    return false;
}

// P3c census (APS5_ARCHB_C_NOSTAGE_CENSUS=1): a slot miss whose stages are data-word patched, and whether its
// key without the patch words (patched stages without buffer-base words) was seen before.
struct PatchCensus {
    std::atomic<std::uint64_t> misses{0}, patched{0}, bufferBases{0}, wordlessSeen{0}, wordlessSlot{0};
    PatchCensus() {
        using AgcDriver::ArchB::Recorder;
        Recorder::RegisterCounter("archc_patch_census_misses", misses);
        Recorder::RegisterCounter("archc_patch_census_patched", patched);
        Recorder::RegisterCounter("archc_patch_census_buffer_bases", bufferBases);
        Recorder::RegisterCounter("archc_patch_census_wordless_seen", wordlessSeen);
        Recorder::RegisterCounter("archc_patch_census_wordless_slot", wordlessSlot);
    }
};

void patchCensus(VkDevice device, const DrawSlotIdentity& identity, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, bool slotAtWordless) {
    static PatchCensus made;
    made.misses.fetch_add(1, std::memory_order_relaxed);
    const bool patched = std::ranges::any_of(identity.stages, [](const DrawSlotStage& stage) { return stage.patched; });
    if (!patched) return;
    made.patched.fetch_add(1, std::memory_order_relaxed);
    if (std::ranges::any_of(identity.stages, [](const DrawSlotStage& stage) { return stage.patched && stage.bufferBases; })) made.bufferBases.fetch_add(1, std::memory_order_relaxed);
    if (slotAtWordless) made.wordlessSlot.fetch_add(1, std::memory_order_relaxed);
    struct Seen {
        std::array<std::uint64_t, 16384> keys{};
    };
    struct SeenSlotStorage {};
    auto& seen = AgcDriver::ThreadScratch<Seen, SeenSlotStorage>();
    const auto key = keyOf(device, identity, draw, shaders, true);
    auto& entry = seen.keys[static_cast<std::size_t>((key ^ (key >> 29u)) % seen.keys.size())];
    if (entry == key) made.wordlessSeen.fetch_add(1, std::memory_order_relaxed);
    entry = key;
}

Slot* lookupKeyed(VkDevice device, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, const DrawSlotIdentity& identity, bool pipelineStateReuse, bool wordless) {
    auto& made = table();
    const auto found = made.slots.find(keyOf(device, identity, draw, shaders, wordless));
    if (found == made.slots.end() || !sameIdentity(found->second, device, state, draw, shaders, identity, pipelineStateReuse, wordless)) return nullptr;
    return &found->second;
}

void publishGatherSlot(const Context& context, const State& state, const Pm4::DrawParameters& draw, const DrawSlotIdentity& identity, const Slot& source, const ShaderResources& resources, std::span<const ShaderResources::MovedBuffer> moved) {
    const auto& link = identity.gatherTemplate;
    const auto* plan = identity.learning;
    if (link == nullptr || plan == nullptr || !plan->matched || (plan->incomplete & ~DriverDetail::GatherUnknownBindings) != 0 || !link->valid.load(std::memory_order_acquire)) return;
    if (link->stages.size() != source.stageCount || !resources.GatherBindings(*link, *plan, moved)) return;
    auto slot = source;
    slot.gatherTemplate = link.get();
    slot.gatherHeld = link;
    slot.wordless = false;
    slot.movedStatic = false;
    slot.liveKept = false;
    slot.bindSet.reset();
    slot.state = link->state.get();
    slot.stateHeld = link->state;
    slot.stateOwner = link->state.get();
    DrawSlotIdentity structure;
    structure.state = link->state;
    structure.gatherTemplate = link;
    structure.gather = plan;
    std::vector<CompiledShader> stages;
    for (std::size_t index = 0; index < link->stages.size(); ++index) {
        const auto& stage = link->stages[index];
        structure.stages.push_back({stage.compiled, stage.variant});
        stages.push_back({stage.stage, stage.compiled.get(), stage.pushOffset, stage.registeredViews.get()});
        slot.stages[index] = stages.back();
        slot.owners[index] = stage.compiled;
        slot.ownerRaw[index] = stage.compiled.get();
        slot.variants[index] = stage.variant;
        slot.variantRaw[index] = stage.variant.get();
        slot.words[index].clear();
        slot.patched[index] = 0;
    }
    auto& slots = table().slots;
    const auto key = keyOf(context.device, structure, draw, stages);
    if (slots.size() >= TableLimit && !slots.contains(key)) return;
    slots.insert_or_assign(key, std::move(slot));
    link->ready.store(true, std::memory_order_release);
    AgcDriver::ArchB::Recorder::Count("draw_templates_live_templates_ready");
    AgcDriver::ArchB::Recorder::Count(draw.indirect ? "draw_templates_live_templates_ready_indirect" : "draw_templates_live_templates_ready_direct");
    static_cast<void>(state);
}

// The slot keyed with the patch words first; under C_REPLAY_PATCH_SLOTS the one keyed without them next, so a
// draw whose words repeat keeps its static moved list and cached moved set.
Slot* lookup(VkDevice device, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, const DrawSlotIdentity& identity, bool pipelineStateReuse) {
    if (auto* slot = lookupKeyed(device, state, draw, shaders, identity, pipelineStateReuse, false)) return slot;
    return wordlessIdentity(identity) ? lookupKeyed(device, state, draw, shaders, identity, pipelineStateReuse, true) : nullptr;
}

std::uint64_t hashRanges(std::uint64_t hash, std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
    hash = mix(hash, ranges.size());
    for (const auto& [begin, end] : ranges) hash = mix(mix(hash, begin), end);
    return hash;
}

std::uint64_t hashImages(std::uint64_t hash, std::span<const std::pair<VkImage, bool>> images) {
    hash = mix(hash, images.size());
    for (const auto& [image, written] : images) hash = mix(mix(hash, reinterpret_cast<std::uintptr_t>(image)), written ? 1u : 0u);
    return hash;
}

std::array<std::uint64_t, ComponentCount> fingerprint(const Slot& slot) {
    std::array<std::uint64_t, ComponentCount> parts{};
    auto& targets = parts[ComponentTargets];
    for (const auto* raw : slot.targetRaw) targets = mix(targets, reinterpret_cast<std::uintptr_t>(raw));
    for (const auto view : slot.views) targets = mix(targets, reinterpret_cast<std::uintptr_t>(view));
    for (const auto& [first, count] : slot.slices) targets = mix(targets, (static_cast<std::uint64_t>(first) << 32u) | count);
    parts[ComponentTemplate] = mix(mix(mix(reinterpret_cast<std::uintptr_t>(slot.resourcesRaw), slot.membership), slot.contentKey.size()), slot.completion ? 1u : 0u);
    parts[ComponentTemplate] = mix(parts[ComponentTemplate], slot.movedStatic ? 1u : 0u);
    for (const auto& entry : slot.moved) {
        parts[ComponentTemplate] = mix(mix(mix(parts[ComponentTemplate], entry.allocation), entry.address), entry.size);
        for (const auto word : entry.words) parts[ComponentTemplate] = mix(parts[ComponentTemplate], word);
    }
    parts[ComponentObjects] = mix(reinterpret_cast<std::uintptr_t>(slot.pipelineRaw), reinterpret_cast<std::uintptr_t>(slot.framebufferRaw));
    auto& inputs = parts[ComponentInputs];
    inputs = mix(mix(inputs, slot.fragmentOutputs), slot.shaderStages);
    parts[ComponentArgs] = mix(slot.argumentRead, slot.argumentLinearRead);
    if (!slot.draw.indirect) {
        parts[ComponentArgs] = mix(mix(parts[ComponentArgs], slot.draw.indexAddress), slot.draw.indexCount);
        parts[ComponentArgs] = mix(mix(parts[ComponentArgs], slot.draw.indexSize), slot.draw.instanceCount);
        parts[ComponentArgs] = mix(mix(parts[ComponentArgs], slot.draw.flags), slot.draw.indexed);
        parts[ComponentArgs] = mix(mix(parts[ComponentArgs], slot.draw.firstVertex), slot.draw.firstInstance);
    }
    auto& pass = parts[ComponentPass];
    pass = mix(pass, slot.pass.passKey);
    pass = mix(pass, (slot.pass.forced ? 1u : 0u) | (slot.pass.fenced ? 2u : 0u) | (slot.pass.inPassIndirect ? 4u : 0u) | (slot.pass.unknownMemory ? 8u : 0u) | (slot.pass.unknownWrites ? 16u : 0u));
    pass = hashRanges(pass, slot.pass.passReads);
    pass = hashRanges(pass, slot.pass.linearReads);
    pass = hashRanges(pass, slot.pass.passWrites);
    pass = hashRanges(pass, slot.pass.writeOnly);
    pass = hashImages(pass, slot.pass.images);
    pass = hashImages(pass, slot.pass.attachments);
    auto& push = parts[ComponentPush];
    push = mix(push, slot.pushStages);
    for (std::size_t offset = 0; offset < slot.push.size(); offset += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, slot.push.data() + offset, std::min<std::size_t>(8, slot.push.size() - offset));
        push = mix(push, word);
    }
    return parts;
}

class StepClock {
public:
    StepClock() : on(stepTiming()) {
        if (on) last = start = std::chrono::steady_clock::now();
    }
    void Mark(Step step) {
        if (!on) return;
        const auto now = std::chrono::steady_clock::now();
        const auto sample = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count());
        auto& made = counters();
        made.stepNs[step].fetch_add(sample, std::memory_order_relaxed);
        addTrimmed(made.stepTrimNs[step], made.stepTrimN[step], made.stepOut[step], sample);
        last = now;
        if (probeTiming()) probeLast = probeNow();
    }
    void Finish() {
        if (!on) return;
        counters().replayNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
    }

private:
    bool on = false;
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point last;
};

void noteIneligible(Ineligible why) {
    counters().ineligible[why].fetch_add(1, std::memory_order_relaxed);
}

bool sameDirectInputs(const DrawInputs& a, const DrawInputs& b) {
    const auto copied = [](const DrawInputs& inputs, VkBuffer handle) -> Buffer* {
        if (inputs.indices != nullptr && inputs.indices->Handle() == handle) return inputs.indices.get();
        for (const auto& buffer : inputs.vertexBuffers) if (buffer->Handle() == handle) return buffer.get();
        return nullptr;
    };
    const auto binding = [&](VkBuffer first, VkDeviceSize firstOffset, VkBuffer second, VkDeviceSize secondOffset) {
        if (firstOffset != secondOffset) return false;
        if (first == second) return true;
        auto* firstCopy = copied(a, first);
        auto* secondCopy = copied(b, second);
        return firstCopy != nullptr && secondCopy != nullptr && std::ranges::equal(firstCopy->Bytes(), secondCopy->Bytes());
    };
    if (a.nothing != b.nothing || a.indexBytes != b.indexBytes || a.maxIndex != b.maxIndex || a.indexCount != b.indexCount || a.listRestartPrimitive != b.listRestartPrimitive || a.expandedIndexCount != b.expandedIndexCount || a.restartTable != b.restartTable || a.fragmentOutputs != b.fragmentOutputs || a.shaderStages != b.shaderStages || a.meshGroups != b.meshGroups || a.meshLayers != b.meshLayers || a.meshLayerStride != b.meshLayerStride || a.vertexInputBehindGpuWrite != b.vertexInputBehindGpuWrite || a.inPlaceReads != b.inPlaceReads || a.inPlaceAlignments != b.inPlaceAlignments || a.inPlaceBuffers != b.inPlaceBuffers || a.inPlaceOffsets != b.inPlaceOffsets || a.vertexHandles.size() != b.vertexHandles.size() || a.vertexOffsets.size() != b.vertexOffsets.size()) return false;
    if (!binding(a.indexHandle, a.indexOffset, b.indexHandle, b.indexOffset)) return false;
    for (std::size_t index = 0; index < a.vertexHandles.size(); ++index) if (!binding(a.vertexHandles[index], a.vertexOffsets[index], b.vertexHandles[index], b.vertexOffsets[index])) return false;
    const auto sameBinding = [](const VkVertexInputBindingDescription& first, const VkVertexInputBindingDescription& second) { return first.binding == second.binding && first.stride == second.stride && first.inputRate == second.inputRate; };
    const auto sameAttribute = [](const VkVertexInputAttributeDescription& first, const VkVertexInputAttributeDescription& second) { return first.location == second.location && first.binding == second.binding && first.format == second.format && first.offset == second.offset; };
    return std::ranges::equal(a.VertexLayout().bindings, b.VertexLayout().bindings, sameBinding) && std::ranges::equal(a.VertexLayout().attributes, b.VertexLayout().attributes, sameAttribute);
}

void verifyDirectInputs(const PreparedDraw& prepared, const DrawInputs& inputs) {
    if (!prepared.slotDirectInputs) return;
    AgcDriver::ArchB::Recorder::Count("archc_replay_direct_inputs_verified");
    if (sameDirectInputs(*prepared.slotDirectInputs, inputs)) return;
    auto& counts = counters();
    counts.mismatched[ComponentInputs].fetch_add(1, std::memory_order_relaxed);
    counts.kinds[0].verifyMismatches.fetch_add(1, std::memory_order_relaxed);
    if (counts.verifyMismatches.fetch_add(1, std::memory_order_relaxed) < 8) std::fprintf(stderr, "[archc] draw replay VERIFY mismatch: direct input bindings or effective draw range\n");
}

bool wantsCapture(const PreparedDraw& prepared) {
    return prepared.identity != nullptr && prepared.slotCapture && (prepared.args != nullptr || AgcDriver::ArchB::DirectDrawEnabled()) && active() && !diagnosticsBlock();
}

void capture(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, PreparedDraw& prepared, SlotPassCapture& pass, bool inputsInPlace, const std::optional<std::array<std::uint64_t, ComponentCount>>& predicted) {
    auto& made = table();
    const auto& identity = *prepared.identity;
    const bool wordless = prepared.slotWordless && wordlessIdentity(identity);
    const auto key = keyOf(context.device, identity, draw, shaders, wordless);
    const auto mismatch = [&](std::size_t component) {
        auto& counts = counters();
        counts.mismatched[component].fetch_add(1, std::memory_order_relaxed);
        counts.kinds[draw.indirect.has_value()].verifyMismatches.fetch_add(1, std::memory_order_relaxed);
        if (prepared.slotVerifyPatch) cutMismatch(CutPatchSlots, component < ComponentCount ? ComponentNames[component] : "ineligible");
        if (counts.verifyMismatches.fetch_add(1, std::memory_order_relaxed) < 8) std::fprintf(stderr, "[archc] draw replay VERIFY mismatch: %s (indirect 0x%llx)\n", component < ComponentCount ? ComponentNames[component] : "ineligible", static_cast<unsigned long long>(draw.indirect ? draw.indirect->arguments : 0));
    };
    const auto refuse = [&](Ineligible why) {
        noteIneligible(why);
        counters().kinds[draw.indirect.has_value()].ineligible[why].fetch_add(1, std::memory_order_relaxed);
        made.slots.erase(key);
        if (predicted.has_value()) mismatch(ComponentCount);
    };
    if (!usableIdentity(identity, state, shaders)) return refuse(IneligibleIdentity);
    const auto* args = prepared.args;
    if (args != nullptr ? prepared.indirect.path != IndirectDrawPath::Gpu || args->countIndirect || args->RangeBytes() == 0 || prepared.indirect.argumentImport == nullptr : draw.indirect.has_value() || !AgcDriver::ArchB::DirectDrawEnabled()) return refuse(IneligiblePath);
    if (!prepared.recorded || !prepared.lean) return refuse(IneligibleUnrecorded);
    if (prepared.outcome.waited) return refuse(IneligibleWaited);
    if (prepared.listed) return refuse(IneligibleListed);
    static const bool syncCompletionDraws = std::getenv("APS5_SYNC_COMPLETION_DRAWS") != nullptr;
    if (prepared.completion && (syncCompletionDraws || prepared.resolved.resources == nullptr || !prepared.resolved.resources->ChecksFaults() || prepared.resolved.resources->HasCopiedWrites())) return refuse(IneligibleCompletion);
    if (prepared.multisampled) return refuse(IneligibleMultisampled);
    if (!pass.filled) return refuse(IneligiblePass);
    const auto& pipeline = prepared.pipeline;
    const bool lds = PixelFunctionLdsSubgroup(shaders) != 0u || std::ranges::any_of(shaders, [](const CompiledShader& shader) { return shader.program != nullptr && shader.program->functionLdsBuffer; });
    if (!prepared.stencilReferences.empty() || !prepared.stencilReplay.empty() || state.stencilReplayReference || pipeline == nullptr || (pipeline->SplitsFaces() && !AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::CReplaySplitFaces)) || pipeline->SplitsSecondChannel() || pipeline->ReordersSamples() || prepared.depthLocations.has_value() || !prepared.reordered.empty() || state.stages.mesh || state.stages.tessellation || state.tessellationCapture != nullptr || UsesMeshWindow(shaders) || lds || prepared.framebuffer == nullptr) return refuse(IneligibleShape);
    for (const auto& binding : prepared.targets) {
        if (binding.resident == nullptr || binding.proxied || binding.multisampled || binding.color.partialWriteMask != 0 || archcTargets::shapeRefusal(binding.color) != archcTargets::FastRefusalCount) return refuse(IneligibleTargets);
    }
    const auto& inputs = prepared.inputs;
    static_cast<void>(inputsInPlace);
    if (!inputs.restartTable.empty() || inputs.expandedIndexCount != 0 || pass.passReads.size() < inputs.inPlaceReads.size() || pass.linearReads.size() < inputs.inPlaceReads.size()) return refuse(IneligibleInputs);
    const auto& resources = prepared.resolved.resources;
    if (resources == nullptr) return refuse(IneligibleTemplate);
    if ((prepared.outcome.kind != KindTemplateHit && prepared.outcome.kind != KindRecipeHit) || !prepared.resolved.cacheable) return refuse(IneligibleNotHit);
    if (!resources->Reusable()) return refuse(IneligibleNotReusable);
    if (resources->HoldsLease()) return refuse(IneligibleLease);
    if (resources->UsesAddressTable()) return refuse(IneligibleAddress);
    if (resources->HasCopiedWrites()) return refuse(IneligibleCopied);
    if (resources->CacheMembership() == 0) return refuse(IneligibleMembership);
    Slot slot;
    slot.lastUse = ++made.uses;
    slot.wordless = wordless;
    slot.device = context.device;
    slot.stateOwner = identity.state.get();
    slot.state = &state;
    slot.stateHeld = identity.state;
    slot.stageCount = shaders.size();
    for (std::size_t i = 0; i < shaders.size(); ++i) {
        const auto& stage = identity.stages[i];
        slot.stages[i] = shaders[i];
        slot.owners[i] = stage.owner;
        slot.ownerRaw[i] = stage.owner.get();
        slot.variants[i] = stage.variant;
        slot.variantRaw[i] = stage.variant.get();
        slot.words[i] = stage.words;
        slot.patched[i] = static_cast<std::uint8_t>((stage.patched ? 1u : 0u) | (stage.bufferBases ? 2u : 0u));
    }
    slot.draw = draw;
    slot.pipelineStateReuse = prepared.pipelineStateReuse;
    slot.switches = currentSwitches();
    slot.completion = prepared.completion;
    for (const auto& binding : prepared.targets) {
        slot.targets.push_back(binding.resident);
        slot.targetRaw.push_back(binding.resident.get());
        slot.slices.emplace_back(binding.color.arraySlice, binding.color.arrayLastSlice != 0 ? binding.color.layers : 1u);
    }
    slot.views = prepared.targetViews;
    slot.depthReleases = DepthSurfacesReleased();
    slot.resources = resources;
    slot.resourcesRaw = resources.get();
    slot.membership = resources->CacheMembership();
    slot.contentKey = prepared.resolved.contentKey;
    slot.touched = Recorder::Presents();
    slot.pipeline = pipeline;
    slot.pipelineRaw = pipeline.get();
    slot.framebuffer = prepared.framebuffer;
    slot.framebufferRaw = prepared.framebuffer.get();
    if (!std::equal(inputs.inPlaceReads.begin(), inputs.inPlaceReads.end(), pass.passReads.end() - static_cast<std::ptrdiff_t>(inputs.inPlaceReads.size())) || !std::equal(inputs.inPlaceReads.begin(), inputs.inPlaceReads.end(), pass.linearReads.end() - static_cast<std::ptrdiff_t>(inputs.inPlaceReads.size()))) return refuse(IneligibleInputs);
    pass.passReads.resize(pass.passReads.size() - inputs.inPlaceReads.size());
    pass.linearReads.resize(pass.linearReads.size() - inputs.inPlaceReads.size());
    slot.fragmentOutputs = inputs.fragmentOutputs;
    slot.movedStatic = !slot.wordless && std::ranges::all_of(prepared.resolved.moved, [](const ShaderResources::MovedBuffer& entry) { return !entry.words.empty(); });
    if (slot.movedStatic) {
        slot.movedWords.reserve(prepared.resolved.moved.size());
        for (const auto& entry : prepared.resolved.moved) slot.movedWords.emplace_back(entry.words.begin(), entry.words.end());
        for (std::size_t i = 0; i < prepared.resolved.moved.size(); ++i) {
            auto entry = prepared.resolved.moved[i];
            entry.words = slot.movedWords[i];
            slot.moved.push_back(entry);
        }
    }
    slot.shaderStages = inputs.shaderStages;
    if (args != nullptr) {
        const std::pair<std::uint64_t, std::uint64_t> argumentRange{args->arguments, args->arguments + args->RangeBytes()};
        const auto readAt = std::find(pass.passReads.begin(), pass.passReads.end(), argumentRange);
        const auto linearAt = std::find(pass.linearReads.begin(), pass.linearReads.end(), argumentRange);
        if (readAt == pass.passReads.end() || linearAt == pass.linearReads.end() || std::count(pass.passReads.begin(), pass.passReads.end(), argumentRange) != 1 || std::count(pass.linearReads.begin(), pass.linearReads.end(), argumentRange) != 1) return refuse(IneligiblePath);
        slot.argumentRead = static_cast<std::size_t>(readAt - pass.passReads.begin());
        slot.argumentLinearRead = static_cast<std::size_t>(linearAt - pass.linearReads.begin());
        pass.passReads.erase(readAt);
        pass.linearReads.erase(linearAt);
    }
    slot.pass = std::move(pass);
    slot.pushStages = PushConstantStages(shaders);
    if (slot.pushStages != 0) {
        slot.push = AssemblePushConstants(shaders);
        resources->PatchPushConstants(slot.push);
    }
    if (predicted.has_value()) {
        const auto now = fingerprint(slot);
        for (std::size_t component = 0; component < ComponentCount; ++component) {
            if (now[component] != (*predicted)[component]) {
                mismatch(component);
                break;
            }
        }
    }
    if (prepared.slotLearnOnly) {
        publishGatherSlot(context, state, draw, identity, slot, *resources, prepared.resolved.moved);
        AgcDriver::ArchB::Recorder::Count("draw_templates_live_first_sight_captures");
        AgcDriver::ArchB::Recorder::Count(draw.indirect ? "draw_templates_live_first_sight_captures_indirect" : "draw_templates_live_first_sight_captures_direct");
        counters().kinds[draw.indirect.has_value()].captures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (made.slots.size() >= TableLimit && !made.slots.contains(key)) {
        counters().evictions.fetch_add(made.slots.size(), std::memory_order_relaxed);
        made.slots.clear();
    }
    auto [captured, inserted] = made.slots.insert_or_assign(key, std::move(slot));
    publishGatherSlot(context, state, draw, identity, captured->second, *resources, prepared.resolved.moved);
    counters().captures.fetch_add(1, std::memory_order_relaxed);
    counters().kinds[draw.indirect.has_value()].captures.fetch_add(1, std::memory_order_relaxed);
}

static_assert(ParameterArena::Flights == ShaderResources::TemplateFlights);

enum TemplateRefusal : std::size_t { TemplateLarge, TemplateWritten, TemplatePending, TemplateMovedOther, TemplateFlightBusy, TemplateFaultRecord, TemplateNoZeroCopy, TemplateSetsRefused, TemplateBudget, TemplateInPlaceImport, TemplateInPlaceAlignment, TemplateInPlaceStore, TemplateInPlacePending, TemplateRefusalCount };
constexpr std::array<const char*, TemplateRefusalCount> TemplateRefusalNames{"large", "gpu_written", "pending_write", "moved_other", "flight_busy", "fault_record", "no_zero_copy", "sets_refused", "budget", "in_place_import", "in_place_alignment", "in_place_store", "in_place_pending"};
enum TemplateBuild : std::size_t { BuildNew, BuildShape, BuildImports, BuildCount };
constexpr std::array<const char*, BuildCount> TemplateBuildNames{"new", "shape", "imports"};
enum TemplateMismatch : std::size_t { MismatchElementBytes, MismatchElementRange, MismatchStatic, MismatchElementSet, MismatchPush, MismatchCount };
constexpr std::array<const char*, MismatchCount> TemplateMismatchNames{"element_bytes", "element_range", "static_binding", "element_set", "push"};

struct TemplateCounters {
    struct Kind {
        std::atomic<std::uint64_t> replays{0}, verifyChecks{0};
        std::array<std::atomic<std::uint64_t>, TemplateRefusalCount> refused{};
        std::array<std::atomic<std::uint64_t>, MismatchCount> mismatches{};
    };
    std::array<Kind, 2> kinds;
    std::atomic<std::uint64_t> replays{0}, bytes{0}, candidates{0}, nothingMoved{0}, sets{0}, writes{0}, verifyChecks{0}, largeBytes{0}, groups{0};
    std::atomic<std::uint64_t> inPlaceDraws{0}, inPlaceElements{0}, inPlaceWrites{0}, inPlaceBytes{0}, batchMoved{0}, liveSets{0}, liveBytes{0}, keptBytes{0};
    std::array<std::atomic<std::uint64_t>, TemplateRefusalCount> refused{};
    std::array<std::atomic<std::uint64_t>, BuildCount> built{};
    std::array<std::atomic<std::uint64_t>, MismatchCount> mismatches{};
    TemplateCounters() {
        using AgcDriver::ArchB::Recorder;
        for (std::size_t kind = 0; kind < kinds.size(); ++kind) {
            auto& entry = kinds[kind];
            const auto suffix = kind == 0 ? "_direct" : "_indirect";
            const auto name = [&](const std::string& prefix) { return prefix + suffix; };
            Recorder::RegisterCounter(name("draw_templates_replays").c_str(), entry.replays);
            Recorder::RegisterCounter(name("draw_templates_verify_checks").c_str(), entry.verifyChecks);
            for (std::size_t i = 0; i < TemplateRefusalCount; ++i) Recorder::RegisterCounter(name(std::string("draw_templates_refused_") + TemplateRefusalNames[i]).c_str(), entry.refused[i]);
            for (std::size_t i = 0; i < MismatchCount; ++i) Recorder::RegisterCounter(name(std::string("draw_templates_verify_mismatch_") + TemplateMismatchNames[i]).c_str(), entry.mismatches[i]);
        }
        Recorder::RegisterCounter("draw_templates_replays", replays);
        Recorder::RegisterCounter("draw_templates_bytes", bytes);
        Recorder::RegisterCounter("draw_templates_candidates", candidates);
        Recorder::RegisterCounter("draw_templates_nothing_moved", nothingMoved);
        Recorder::RegisterCounter("draw_templates_sets_allocated", sets);
        Recorder::RegisterCounter("draw_templates_descriptor_writes", writes);
        Recorder::RegisterCounter("draw_templates_verify_checks", verifyChecks);
        Recorder::RegisterCounter("draw_templates_large_bytes", largeBytes);
        Recorder::RegisterCounter("draw_templates_flight_groups", groups);
        Recorder::RegisterCounter("draw_templates_in_place_draws", inPlaceDraws);
        Recorder::RegisterCounter("draw_templates_in_place_elements", inPlaceElements);
        Recorder::RegisterCounter("draw_templates_in_place_writes", inPlaceWrites);
        Recorder::RegisterCounter("draw_templates_in_place_bytes", inPlaceBytes);
        Recorder::RegisterCounter("draw_templates_batch_moved", batchMoved);
        Recorder::RegisterCounter("draw_templates_kept_bytes", keptBytes);
        for (std::size_t i = 0; i < TemplateRefusalCount; ++i) Recorder::RegisterCounter((new std::string(std::string("draw_templates_refused_") + TemplateRefusalNames[i]))->c_str(), refused[i]);
        for (std::size_t i = 0; i < BuildCount; ++i) Recorder::RegisterCounter((new std::string(std::string("draw_templates_built_") + TemplateBuildNames[i]))->c_str(), built[i]);
        for (std::size_t i = 0; i < MismatchCount; ++i) Recorder::RegisterCounter((new std::string(std::string("draw_templates_verify_mismatch_") + TemplateMismatchNames[i]))->c_str(), mismatches[i]);
    }
};

TemplateCounters& templateCounters() {
    static TemplateCounters made;
    return made;
}

void countMoveRefusal(ShaderResources::MoveRefusal refusal, const Pm4::DrawParameters& draw) {
    using Why = ShaderResources::MoveRefusal;
    const auto kind = refusal == Why::Written ? TemplateWritten : refusal == Why::Overlap ? TemplatePending : refusal == Why::Limit ? TemplateLarge : TemplateMovedOther;
    templateCounters().refused[kind].fetch_add(1, std::memory_order_relaxed);
    templateCounters().kinds[draw.indirect.has_value()].refused[kind].fetch_add(1, std::memory_order_relaxed);
}

bool templateVerifyNow() {
    static const std::uint64_t period = [] {
        const char* value = std::getenv("APS5_ARCHB_DRAW_TEMPLATES_VERIFY");
        return value == nullptr ? std::uint64_t{0} : static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10));
    }();
    if (period == 0) return false;
    static std::uint64_t uses = 0;
    return ++uses % period == 0;
}

std::uint64_t templateBudget(const char* name, std::uint64_t fallback) {
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10));
}

std::uint64_t templateSetBudget() {
    static const std::uint64_t budget = templateBudget("APS5_ARCHB_DRAW_TEMPLATES_BUDGET_SETS", 16384);
    return budget;
}

std::uint64_t templateByteBudget() {
    static const std::uint64_t budget = templateBudget("APS5_ARCHB_DRAW_TEMPLATES_BUDGET_BYTES", std::uint64_t{64} << 20u);
    return budget;
}

struct TemplateAccounting {
    std::uint64_t sets = 0;
    std::uint64_t bytes = 0;
    void Add(std::uint64_t moreSets, std::uint64_t moreBytes) {
        sets += moreSets;
        bytes += moreBytes;
        templateCounters().liveSets.fetch_add(moreSets, std::memory_order_relaxed);
        templateCounters().liveBytes.fetch_add(moreBytes, std::memory_order_relaxed);
    }
    ~TemplateAccounting() {
        templateCounters().liveSets.fetch_sub(sets, std::memory_order_relaxed);
        templateCounters().liveBytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
};

bool templateBudgetHolds(std::uint64_t moreSets, std::uint64_t moreBytes) {
    const auto& counts = templateCounters();
    return counts.liveSets.load(std::memory_order_relaxed) + moreSets <= templateSetBudget() && counts.liveBytes.load(std::memory_order_relaxed) + moreBytes <= templateByteBudget();
}

std::shared_ptr<ParameterArena> parameterArena(const Context& context, Recorder& recorder) {
    auto& holder = recorder.ParameterArenaHolder();
    if (holder == nullptr) holder = std::make_shared<ParameterArena>(context);
    return std::static_pointer_cast<ParameterArena>(holder);
}

constexpr std::size_t TemplateGroupLimit = 16;

std::array<std::pair<VkBuffer, VkDeviceSize>, ParameterArena::Flights> groupFlights(const ParameterArena& arena, const ParameterLease& lease) {
    std::array<std::pair<VkBuffer, VkDeviceSize>, ParameterArena::Flights> flights;
    for (std::size_t index = 0; index < flights.size(); ++index) flights[index] = {arena.Handle(index, lease.slice.chunk), static_cast<VkDeviceSize>(lease.slice.offset)};
    return flights;
}

bool sameShape(std::span<const ShaderResources::TemplateElement> a, std::span<const ShaderResources::TemplateElement> b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].allocation != b[i].allocation || a[i].bytes != b[i].bytes || a[i].offset != b[i].offset || a[i].words != b[i].words || a[i].inPlace != b[i].inPlace) return false;
    }
    return true;
}

enum LiveMismatch : std::size_t { LiveElementBytes, LiveElementRange, LiveStatic, LiveElementSet, LivePush, LivePipeline, LiveVertex, LiveIndex, LiveIndirect, LiveArguments, LiveMismatchCount };
constexpr std::array<const char*, LiveMismatchCount> LiveMismatchNames{"element_bytes", "element_range", "static_binding", "element_set", "push", "pipeline", "vertex", "index", "indirect", "arguments"};

void liveMismatch(LiveMismatch kind, const char* what) {
    static const auto names = [] {
        std::array<std::string, LiveMismatchCount> values;
        for (std::size_t i = 0; i < values.size(); ++i) values[i] = std::string("draw_templates_live_verify_mismatch_") + LiveMismatchNames[i];
        return values;
    }();
    static std::array<std::atomic<std::uint64_t>, LiveMismatchCount> counts{};
    ArchB::Recorder::Count(names[kind].c_str());
    if (counts[kind].fetch_add(1, std::memory_order_relaxed) < 8) std::fprintf(stderr, "[draw-templates-live] VERIFY mismatch: %s: %s\n", LiveMismatchNames[kind], what);
}

struct TemplatePlan {
    std::shared_ptr<ShaderResources::TemplateSets> sets;
    std::vector<std::pair<std::size_t, VkDescriptorBufferInfo>> inPlace;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
    std::size_t flight = 0;
    std::uint64_t serial = 0;
    bool committed = false;
    bool verify = false;
    bool liveVerify = false;
    struct Reference {
        VkDescriptorBufferInfo descriptor{};
        std::vector<std::byte> bytes;
        bool valid = false;
    };
    std::vector<Reference> reference;
    std::vector<const DrawReferenceMasks::Words*> ignored;
};

TemplatePlan& templatePlan() {
    struct PlanStorage {};
    return AgcDriver::ThreadScratch<TemplatePlan, PlanStorage>();
}

void verifyTemplate(const Context& context, const ShaderResources& resources, const ShaderResources::TemplateSets& sets, std::size_t flight, std::span<const ShaderResources::MovedBuffer> moved, const Slot& slot) {
    auto& counts = templateCounters();
    if (templatePlan().liveVerify) ArchB::Recorder::Count("draw_templates_live_verify_elements");
    else counts.verifyChecks.fetch_add(1, std::memory_order_relaxed);
    counts.kinds[slot.draw.indirect.has_value()].verifyChecks.fetch_add(1, std::memory_order_relaxed);
    const auto mismatch = [&](TemplateMismatch kind, const char* what) {
        counts.kinds[slot.draw.indirect.has_value()].mismatches[kind].fetch_add(1, std::memory_order_relaxed);
        if (templatePlan().liveVerify) { liveMismatch(static_cast<LiveMismatch>(kind), what); return; }
        if (counts.mismatches[kind].fetch_add(1, std::memory_order_relaxed) < 8) std::fprintf(stderr, "[draw-templates] VERIFY mismatch: %s\n", what);
    };
    if (sets.elements.size() != moved.size() || sets.streamed.size() != moved.size()) {
        mismatch(MismatchElementSet, "the template streams another number of elements than the moved list");
        return;
    }
    const auto alignment = ParameterArena::Alignment(context);
    const auto group = flight / ParameterArena::Flights;
    const auto chunk = sets.leases[group]->slice.chunk;
    const auto& reference = templatePlan().reference;
    for (std::size_t i = 0; i < moved.size(); ++i) {
        const auto& element = sets.elements[i];
        if (element.allocation != moved[i].allocation || element.words != !moved[i].words.empty()) mismatch(MismatchElementSet, "a streamed element is not the moved element at its position");
        const auto& info = sets.buffers[flight][sets.streamed[i]];
        if (i >= reference.size() || !reference[i].valid) {
            mismatch(MismatchElementBytes, "the old binding path did not expose the bound bytes");
            continue;
        }
        const auto& old = reference[i];
        if (info.range != old.descriptor.range || info.offset % (element.inPlace ? std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 4) : alignment) != 0) mismatch(MismatchElementRange, "a descriptor's range or alignment differs from the old binding path");
        if (element.inPlace) {
            const auto [begin, bytes] = resources.MovedView(moved[i]);
            const auto expected = std::find_if(templatePlan().inPlace.begin(), templatePlan().inPlace.end(), [&](const auto& item) { return item.first == i; });
            auto proof = sets.proof;
            proof.inserted = HostImportsIdentity(context, false).inserted;
            const bool current = HostImportsReconciledUnchanged(context, proof);
            if (!current || expected == templatePlan().inPlace.end() || info.buffer != expected->second.buffer || info.offset != expected->second.offset || info.range != expected->second.range) {
                static unsigned reported = 0;
                if (templatePlan().liveVerify && reported++ < 8) {
                    const auto now = HostImportsIdentity(context, false);
                    std::fprintf(stderr, "[draw-templates-live] detail import element %zu current %u epoch %llu/%llu generation %llu/%llu inserted %llu/%llu selected %u\n", i, current, static_cast<unsigned long long>(proof.epoch), static_cast<unsigned long long>(now.epoch), static_cast<unsigned long long>(proof.refreshedGeneration), static_cast<unsigned long long>(now.refreshedGeneration), static_cast<unsigned long long>(proof.inserted), static_cast<unsigned long long>(now.inserted), expected != templatePlan().inPlace.end() && info.buffer == expected->second.buffer && info.offset == expected->second.offset && info.range == expected->second.range);
                }
                mismatch(MismatchElementRange, "an in-place descriptor lost its validated import or differs from the selected live binding");
            }
            if (old.bytes.size() != info.range || std::memcmp(reinterpret_cast<const void*>(begin), old.bytes.data(), old.bytes.size()) != 0) mismatch(MismatchElementBytes, "the in-place bytes differ from what the old binding path binds");
        } else {
            if (info.buffer != sets.arena->Handle(flight % ParameterArena::Flights, chunk)) mismatch(MismatchElementRange, "a streamed descriptor points outside the flight");
            if (old.bytes.size() != info.range || !DrawReferenceMasks::SameBytes(sets.arena->Bytes(flight % ParameterArena::Flights, chunk) + info.offset, old.bytes.data(), old.bytes.size(), i < templatePlan().ignored.size() ? templatePlan().ignored[i] : nullptr)) {
                static unsigned reported = 0;
                if (templatePlan().liveVerify && reported++ < 12) {
                    const auto* bytes = sets.arena->Bytes(flight % ParameterArena::Flights, chunk) + info.offset;
                    std::size_t first = 0;
                    while (first < std::min<std::size_t>(info.range, old.bytes.size()) && bytes[first] == old.bytes[first]) ++first;
                    std::fprintf(stderr, "[draw-templates-live] detail slice element %zu allocation %zu words %u address %llx size %zu range %llu/%zu offset %zu live %02x old %02x\n", i, moved[i].allocation, !moved[i].words.empty(), static_cast<unsigned long long>(moved[i].address), moved[i].size, static_cast<unsigned long long>(info.range), old.bytes.size(), first, first < info.range ? std::to_integer<unsigned>(bytes[first]) : 0, first < old.bytes.size() ? std::to_integer<unsigned>(old.bytes[first]) : 0);
                }
                mismatch(MismatchElementBytes, "the slice bytes differ from what the old binding path binds");
            }
        }
    }
    if (resources.TemplateStaticMismatches(sets, flight) != 0) mismatch(MismatchStatic, "a static binding of the template set differs from the template's own");
    if (!templatePlan().liveVerify && sets.push != slot.push) mismatch(MismatchPush, "the slot's push block changed under its template");
}

std::optional<std::size_t> freeFlight(const ShaderResources::TemplateSets& sets, const Recorder& recorder) {
    for (std::size_t group = 0; group < sets.leases.size(); ++group) {
        auto slice = sets.leases[group]->slice;
        if (const auto flight = ParameterSlices::Pick(slice, recorder.OpenSerial(), recorder.CompletedThrough())) return group * ParameterArena::Flights + *flight;
    }
    return std::nullopt;
}

void keepTemplate(Recorder& recorder, const std::shared_ptr<ShaderResources::TemplateSets>& sets, std::uint64_t bytes) {
    recorder.Keep(sets, static_cast<std::size_t>(bytes));
    if (bytes != 0) templateCounters().keptBytes.fetch_add(bytes, std::memory_order_relaxed);
}

bool prepareTemplate(const Context& context, Recorder& recorder, Slot& slot, const ShaderResources& resources, std::span<const ShaderResources::MovedBuffer> moved, bool drawUse, const DrawSlotIdentity* live = nullptr) {
    auto& counts = templateCounters();
    auto& plan = templatePlan();
    plan.sets.reset();
    plan.inPlace.clear();
    plan.reads.clear();
    plan.committed = false;
    plan.liveVerify = live != nullptr && !live->referenceStages.empty();
    plan.verify = plan.liveVerify || templateVerifyNow();
    plan.reference.clear();
    plan.ignored.clear();
    const auto refuse = [&](TemplateRefusal why) {
        counts.refused[why].fetch_add(1, std::memory_order_relaxed);
        counts.kinds[slot.draw.indirect.has_value()].refused[why].fetch_add(1, std::memory_order_relaxed);
        if (live != nullptr) {
            static const auto names = [] {
                std::array<std::string, TemplateRefusalCount> values;
                for (std::size_t i = 0; i < values.size(); ++i) values[i] = std::string("draw_templates_live_commit_refused_template_") + TemplateRefusalNames[i];
                return values;
            }();
            ArchB::Recorder::Count(names[why].c_str());
            static const auto typedNames = [] {
                std::array<std::array<std::string, TemplateRefusalCount>, 2> values;
                for (std::size_t kind = 0; kind < values.size(); ++kind) for (std::size_t i = 0; i < values[kind].size(); ++i) values[kind][i] = std::string("draw_templates_live_commit_refused_template_") + TemplateRefusalNames[i] + (kind == 0 ? "_direct" : "_indirect");
                return values;
            }();
            ArchB::Recorder::Count(typedNames[slot.draw.indirect.has_value()][why].c_str());
        }
        plan.sets.reset();
        plan.inPlace.clear();
        plan.reads.clear();
        return false;
    };
    if (drawUse) return refuse(TemplateFaultRecord);
    if (!Recorder::ZeroCopyStorageActive()) return refuse(TemplateNoZeroCopy);
    static const std::size_t storeStreamLimit = [] {
        const char* value = std::getenv("APS5_DRAW_TEMPLATES_STORE_STREAM_MAX");
        return value != nullptr ? static_cast<std::size_t>(std::strtoull(value, nullptr, 10)) : std::size_t{65536};
    }();
    const auto limit = recorder.OpenGpuStores() != 0 ? std::max(ShaderResources::DrawUploadLimit(), storeStreamLimit) : ShaderResources::DrawUploadLimit();
    const auto alignment = static_cast<VkDeviceSize>(ParameterArena::Alignment(context));
    const auto storageAlignment = std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 4);
    struct ShapeStorage {};
    auto& shape = AgcDriver::ThreadScratch<std::vector<ShaderResources::TemplateElement>, ShapeStorage>();
    shape.clear();
    VkDeviceSize total = 0;
    for (std::size_t index = 0; index < moved.size(); ++index) {
        const auto& entry = moved[index];
        const bool words = !entry.words.empty();
        const auto bytes = resources.StreamBytes(entry);
        if (bytes == 0) return refuse(TemplateLarge);
        if (!words && bytes > limit) {
            counts.largeBytes.fetch_add(bytes, std::memory_order_relaxed);
            shape.push_back({entry.allocation, bytes, 0, false, true});
            continue;
        }
        total = (total + alignment - 1) / alignment * alignment;
        shape.push_back({entry.allocation, bytes, total, words, false});
        total += bytes;
    }
    resources.RequireGuestBuffers(moved);
    for (std::size_t index = 0; index < moved.size(); ++index) {
        if (!shape[index].inPlace) continue;
        if (recorder.OpenGpuStores() != 0) return refuse(TemplateInPlaceStore);
        const auto [begin, bytes] = resources.MovedView(moved[index]);
        if (recorder.PendingWriteOverlaps(begin, bytes)) return refuse(TemplateInPlacePending);
        const auto* import = ExistingHostImportFor(context, begin, bytes);
        if (import == nullptr) return refuse(TemplateInPlaceImport);
        const auto offset = HostImportOffset(*import, begin);
        if (offset % storageAlignment != 0) return refuse(TemplateInPlaceAlignment);
        plan.inPlace.emplace_back(index, resources.TemplateDescriptor({import->buffer, offset, static_cast<VkDeviceSize>(bytes)}));
        plan.reads.emplace_back(begin, begin + bytes);
    }
    if (plan.verify) {
        std::optional<std::vector<ShaderResources::MovedBuffer>> oldMoved;
        if (plan.liveVerify) oldMoved = resources.MovedReadOnlyBuffers(live->referenceStages, recorder, &live->gatherTemplate->state->color);
        if (plan.liveVerify && !oldMoved) liveMismatch(LiveElementSet, "the old path refused the gathered binding shape");
        if (plan.liveVerify) plan.ignored = resources.MovedIgnoredWords(live->referenceStages, live->referenceMasks, moved);
        auto bound = resources.PrepareDrawBindings(recorder, plan.liveVerify && oldMoved ? std::span<const ShaderResources::MovedBuffer>(*oldMoved) : moved, false, nullptr, true);
        plan.reference.resize(moved.size());
        for (std::size_t i = 0; i < moved.size(); ++i) {
            auto& reference = plan.reference[i];
            reference.valid = resources.BoundElementBytes(moved[i], bound.get(), reference.descriptor, reference.bytes);
        }
        for (auto& [index, descriptor] : plan.inPlace) {
            if (recorder.OpenGpuStores() != 0) return refuse(TemplateInPlaceStore);
            const auto [begin, bytes] = resources.MovedView(moved[index]);
            if (recorder.PendingWriteOverlaps(begin, bytes)) return refuse(TemplateInPlacePending);
            const auto* import = ExistingHostImportFor(context, begin, bytes);
            if (import == nullptr) return refuse(TemplateInPlaceImport);
            const auto offset = HostImportOffset(*import, begin);
            if (offset % storageAlignment != 0) return refuse(TemplateInPlaceAlignment);
            descriptor = resources.TemplateDescriptor({import->buffer, offset, static_cast<VkDeviceSize>(bytes)});
        }
    }
    std::shared_ptr<ShaderResources::TemplateSets> sets;
    std::size_t held = slot.templates.size();
    bool other = false;
    for (std::size_t index = 0; index < slot.templates.size(); ++index) {
        auto candidate = slot.templates[index].lock();
        if (candidate == nullptr || candidate.get() != slot.templateRaw[index]) {
            slot.templates[index].reset();
            slot.templateRaw[index] = nullptr;
            continue;
        }
        if (candidate->recorder == &recorder && sameShape(candidate->elements, shape)) {
            sets = std::move(candidate);
            held = index;
            break;
        }
        other = true;
    }
    std::optional<TemplateBuild> build;
    if (sets == nullptr) build = other ? BuildShape : BuildNew;
    else if (!HostImportsUnchanged(context, sets->proof)) build = BuildImports;
    const auto groupBytes = static_cast<std::uint64_t>(ParameterSlices::ClassBytes(static_cast<std::size_t>(std::max<VkDeviceSize>(total, 1)), static_cast<std::size_t>(alignment))) * ParameterArena::Flights;
    const auto groupSets = static_cast<std::uint64_t>(ParameterArena::Flights) * 2u;
    if (build.has_value()) {
        if (sets != nullptr) {
            resources.DropTemplateSets(sets.get());
            slot.templates[held].reset();
            slot.templateRaw[held] = nullptr;
            sets.reset();
        }
        if (!templateBudgetHolds(groupSets, groupBytes)) return refuse(TemplateBudget);
        auto arena = parameterArena(context, recorder);
        auto lease = ParameterArena::Allocate(arena, static_cast<std::size_t>(std::max<VkDeviceSize>(total, 1)), static_cast<std::size_t>(templateByteBudget()));
        if (lease == nullptr) return refuse(total > ParameterArena::ChunkBytes ? TemplateLarge : TemplateBudget);
        const auto flights = groupFlights(*arena, *lease);
        sets = resources.MakeTemplateSets(shape, flights);
        if (sets == nullptr) return refuse(TemplateSetsRefused);
        auto accounting = std::make_shared<TemplateAccounting>();
        accounting->Add(sets->sets, groupBytes);
        sets->accounting = accounting;
        sets->recorder = &recorder;
        sets->arena = std::move(arena);
        sets->leases.push_back(std::move(lease));
        sets->push = slot.push;
        counts.groups.fetch_add(1, std::memory_order_relaxed);
        resources.KeepTemplateSets(sets);
        if (slot.templateOwnership == nullptr || slot.templateOwnership.use_count() != 1) {
            slot.templateOwnership = std::make_shared<SlotTemplateOwnership>();
            slot.templateOwnership->resources = slot.resources;
        }
        auto& owned = slot.templateOwnership->sets;
        std::erase_if(owned, [](const auto& weak) { return weak.expired(); });
        owned.push_back(sets);
        std::size_t place = 0;
        for (std::size_t index = 0; index < slot.templates.size(); ++index) {
            if (slot.templateRaw[index] == nullptr) {
                place = index;
                break;
            }
            if (slot.templateUse[index] < slot.templateUse[place]) place = index;
        }
        if (slot.templateRaw[place] != nullptr) {
            if (auto evicted = slot.templates[place].lock()) resources.DropTemplateSets(evicted.get());
        }
        slot.templates[place] = sets;
        slot.templateRaw[place] = sets.get();
        held = place;
        counts.built[*build].fetch_add(1, std::memory_order_relaxed);
        counts.sets.fetch_add(sets->sets, std::memory_order_relaxed);
        counts.writes.fetch_add(sets->writes, std::memory_order_relaxed);
        keepTemplate(recorder, sets, groupBytes);
    }
    slot.templateUse[held] = table().uses;
    if (!freeFlight(*sets, recorder).has_value()) {
        if (sets->leases.size() >= TemplateGroupLimit) return refuse(TemplateFlightBusy);
        if (!templateBudgetHolds(groupSets, groupBytes)) return refuse(TemplateBudget);
        auto lease = ParameterArena::Allocate(sets->arena, static_cast<std::size_t>(std::max<VkDeviceSize>(total, 1)), static_cast<std::size_t>(templateByteBudget()));
        if (lease == nullptr) return refuse(total > ParameterArena::ChunkBytes ? TemplateLarge : TemplateBudget);
        const auto before = std::pair{sets->sets, sets->writes};
        resources.AddTemplateFlights(*sets, groupFlights(*sets->arena, *lease));
        sets->leases.push_back(std::move(lease));
        std::static_pointer_cast<TemplateAccounting>(sets->accounting)->Add(sets->sets - before.first, groupBytes);
        counts.groups.fetch_add(1, std::memory_order_relaxed);
        counts.sets.fetch_add(sets->sets - before.first, std::memory_order_relaxed);
        counts.writes.fetch_add(sets->writes - before.second, std::memory_order_relaxed);
        keepTemplate(recorder, sets, groupBytes);
    }
    plan.sets = std::move(sets);
    return true;
}

std::size_t commitTemplate(const Context& context, Recorder& recorder, const ShaderResources& resources, std::span<const ShaderResources::MovedBuffer> moved, const Slot& slot) {
    auto& counts = templateCounters();
    auto& plan = templatePlan();
    auto& sets = *plan.sets;
    std::optional<std::size_t> picked;
    std::size_t group = 0;
    for (; group < sets.leases.size() && !picked.has_value(); ++group) picked = ParameterSlices::Pick(sets.leases[group]->slice, recorder.OpenSerial(), recorder.CompletedThrough());
    Require(picked.has_value(), "a template lost the free flight its preparation found");
    const auto& slice = sets.leases[group - 1]->slice;
    const auto flight = (group - 1) * ParameterArena::Flights + *picked;
    const auto [placed, faults] = resources.UpdateTemplateFlight(sets, flight, plan.inPlace);
    if (placed + faults != 0) counts.writes.fetch_add(placed + faults, std::memory_order_relaxed);
    if (placed != 0) counts.inPlaceWrites.fetch_add(placed, std::memory_order_relaxed);
    auto* base = sets.arena->Bytes(*picked, slice.chunk) + slice.offset;
    std::size_t copied = 0;
    for (std::size_t index = 0; index < moved.size(); ++index) {
        if (sets.elements[index].inPlace) continue;
        resources.StreamElement(moved[index], base + sets.elements[index].offset);
        copied += sets.elements[index].bytes;
    }
    static const bool noNotes = std::getenv("APS5_ZC_MOVED_NO_READNOTES") != nullptr;
    if (!noNotes)
        for (const auto& [begin, end] : plan.reads) recorder.NotePendingRead(begin, static_cast<std::size_t>(end - begin), Recorder::ReadKind::DispatchElement);
    if (!plan.inPlace.empty()) {
        counts.inPlaceDraws.fetch_add(1, std::memory_order_relaxed);
        counts.inPlaceElements.fetch_add(plan.inPlace.size(), std::memory_order_relaxed);
        for (const auto& [begin, end] : plan.reads) counts.inPlaceBytes.fetch_add(end - begin, std::memory_order_relaxed);
    }
    counts.bytes.fetch_add(copied, std::memory_order_relaxed);
    counts.replays.fetch_add(1, std::memory_order_relaxed);
    counts.kinds[slot.draw.indirect.has_value()].replays.fetch_add(1, std::memory_order_relaxed);
    if (plan.verify) verifyTemplate(context, resources, sets, flight, moved, slot);
    plan.flight = flight;
    plan.serial = recorder.OpenSerial();
    plan.committed = true;
    return flight;
}

void finishTemplate(Recorder& recorder) {
    auto& plan = templatePlan();
    if (!plan.committed || plan.sets == nullptr) return;
    const auto open = recorder.OpenSerial();
    if (open != plan.serial) {
        templateCounters().batchMoved.fetch_add(1, std::memory_order_relaxed);
        for (const auto& [begin, end] : plan.reads) recorder.NotePendingRead(begin, static_cast<std::size_t>(end - begin), Recorder::ReadKind::DispatchElement);
    }
    auto& serial = plan.sets->leases[plan.flight / ParameterArena::Flights]->slice.serials[plan.flight % ParameterArena::Flights];
    serial = std::max(serial, open);
    keepTemplate(recorder, plan.sets, 0);
    plan.sets.reset();
    plan.committed = false;
}

struct ReplayScratch {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> passReads;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> linearReads;
    std::vector<std::shared_ptr<StorageTexture>> targets;
    DrawInputs inputs;
    DrawOutcome outcome{false};
    DrawTimer timer{false};
};

ReplayScratch& scratch() {
    struct SlotStorage {};
    return AgcDriver::ThreadScratch<ReplayScratch, SlotStorage>();
}

DrawOutcome& replayOutcome() {
    return scratch().outcome;
}

DrawTimer& replayTimer() {
    return scratch().timer;
}

bool tryReplay(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, const DrawSlotIdentity& identity, const DrawTraceContext* trace, bool pipelineStateReuse, PreparedDraw& prepared) {
    auto& counts = counters();
    struct Probing {
        bool on = probeTiming();
        Probing() {
            if (on) ArchC::probing.store(true, std::memory_order_relaxed);
        }
        ~Probing() {
            if (on) ArchC::probing.store(false, std::memory_order_relaxed);
        }
    } probing;
    StepClock clock;
    const auto refuse = [&](Refusal why) {
        counts.refused[why].fetch_add(1, std::memory_order_relaxed);
        counts.kinds[draw.indirect.has_value()].refused[why].fetch_add(1, std::memory_order_relaxed);
        if (identity.gather != nullptr) {
            static const auto names = [] {
                std::array<std::string, RefusalCount> values;
                for (std::size_t i = 0; i < values.size(); ++i) values[i] = std::string("draw_templates_live_commit_refused_") + RefusalNames[i];
                return values;
            }();
            ArchB::Recorder::Count(names[why].c_str());
            static const auto typedNames = [] {
                std::array<std::array<std::string, RefusalCount>, 2> values;
                for (std::size_t kind = 0; kind < values.size(); ++kind) for (std::size_t i = 0; i < values[kind].size(); ++i) values[kind][i] = std::string("draw_templates_live_commit_refused_") + RefusalNames[i] + (kind == 0 ? "_direct" : "_indirect");
                return values;
            }();
            ArchB::Recorder::Count(typedNames[draw.indirect.has_value()][why].c_str());
            if (identity.gatherTemplate != nullptr && (why == RefusedNew || why == RefusedTargets || why == RefusedDepth || why == RefusedObjects || why == RefusedTemplateGone || why == RefusedTemplateMembership || why == RefusedProof)) identity.gatherTemplate->valid.store(false, std::memory_order_release);
        }
        lastOutcome() = why == RefusedNew ? OutcomeNew : OutcomeRefused;
        scratch().inputs = DrawInputs{};
        scratch().targets.clear();
        return false;
    };
    if (!draw.indirect && !AgcDriver::ArchB::DirectDrawEnabled()) return refuse(RefusedSwitches);
    if (!usableIdentity(identity, state, shaders) || state.colors.size() > 64) return refuse(RefusedNew);
    if (identity.gather != nullptr && (identity.gatherTemplate == nullptr || !identity.gatherTemplate->valid.load(std::memory_order_acquire) || !identity.gatherTemplate->ready.load(std::memory_order_acquire))) return refuse(RefusedTemplate);
    auto* slot = lookup(context.device, state, draw, shaders, identity, pipelineStateReuse);
    if (slot == nullptr) {
        if (stepTiming()) classifyMiss(identity, draw, shaders);
        if (archcNostage::enabled()) patchCensus(context.device, identity, draw, shaders, table().slots.contains(keyOf(context.device, identity, draw, shaders, true)));
        prepared.slotCapture = sightedBefore(keyOf(context.device, identity, draw, shaders));
        prepared.slotWordless = false;
        if (!prepared.slotCapture && wordlessIdentity(identity)) prepared.slotWordless = prepared.slotCapture = sightedBefore(keyOf(context.device, identity, draw, shaders, true));
        static const bool learnFirst = std::getenv("APS5_DRAW_TEMPLATES_LIVE_NO_FIRST_SIGHT") == nullptr;
        prepared.slotLearnOnly = false;
        if (learnFirst && !prepared.slotCapture && identity.gather == nullptr && identity.learning != nullptr && identity.gatherTemplate != nullptr && identity.gatherTemplate->valid.load(std::memory_order_acquire) && !identity.gatherTemplate->ready.load(std::memory_order_acquire)) {
            prepared.slotCapture = true;
            prepared.slotLearnOnly = true;
        }
        return refuse(RefusedNew);
    }
    prepared.slotCapture = true;
    prepared.slotLearnOnly = false;
    prepared.slotWordless = slot->wordless;
    slot->replayedLast = false;
    slot->lastUse = ++table().uses;
    if (slot->switches != currentSwitches() || diagnosticsBlock()) return refuse(RefusedSwitches);
    auto* recorder = Recorder::Active();
    if (recorder == nullptr || context.recorder != recorder) return refuse(RefusedRecorder);
    auto resources = slot->resources.lock();
    if (resources == nullptr || resources.get() != slot->resourcesRaw) return refuse(RefusedTemplateGone);
    if (resources->CacheMembership() != slot->membership) return refuse(RefusedTemplateMembership);
    auto pipeline = slot->pipeline.lock();
    auto framebuffer = slot->framebuffer.lock();
    if (pipeline == nullptr || framebuffer == nullptr || pipeline.get() != slot->pipelineRaw || framebuffer.get() != slot->framebufferRaw || (state.depth && DepthSurfacesReleased() != slot->depthReleases) || slot->targets.size() != state.colors.size()) return refuse(RefusedObjects);
    const auto* args = draw.indirect ? &*draw.indirect : nullptr;
    const auto rangeBytes = args != nullptr ? static_cast<std::size_t>(args->RangeBytes()) : 0;
    if ((context.copiedWriters != nullptr && !context.copiedWriters->empty()) || !DrawCopiedWriters()->empty()) return refuse(RefusedInputs);
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> argumentRange{{{args != nullptr ? args->arguments : 0, args != nullptr ? args->arguments + rangeBytes : 0}}};
    if (args != nullptr && (recorder->PendingLabelIn(args->arguments, rangeBytes) || StorageTexture::AnyPendingOverlaps(argumentRange))) return refuse(RefusedArgs);
    if (archcTargets::verifyPeriod() != 0) return refuse(RefusedTargets);
    clock.Mark(StepChecks);
    using AgcDriver::ArchB::Feature;
    context.recorder->BoundKeptBytes();
    auto& work = scratch();
    const bool slotInputs = cutEnabled(Feature::ArchcReplaySlotInputs);
    const bool verifyInputs = slotInputs && cutVerifyNow(CutSlotInputs);
    const ReplayInputsHint inputsHint{slot->fragmentOutputs, slot->shaderStages};
    if (slotInputs && !verifyInputs) cutHit(CutSlotInputs);
    const auto plan = planDraw(context, state, draw, shaders, nullptr, replayOutcome(), replayTimer(), nullptr, slotInputs && !verifyInputs ? &inputsHint : nullptr, false);
    if (plan.nothing) work.inputs.nothing = true;
    else work.inputs = prepareDrawInputs(context, state, draw, shaders, plan, replayTimer(), nullptr);
    auto& inputs = work.inputs;
    if (verifyInputs && !inputs.nothing && (inputs.fragmentOutputs != slot->fragmentOutputs || inputs.shaderStages != slot->shaderStages)) cutMismatch(CutSlotInputs, "the slot's fragment outputs or stages differ from the validation's");
    if (inputs.nothing) {
        work.inputs = DrawInputs{};
        clock.Finish();
        counts.hits.fetch_add(1, std::memory_order_relaxed);
        counts.kinds[draw.indirect.has_value()].hits.fetch_add(1, std::memory_order_relaxed);
        lastOutcome() = OutcomeReplayed;
        return true;
    }
    if (!inputs.restartTable.empty() || inputs.expandedIndexCount != 0 || inputs.fragmentOutputs != slot->fragmentOutputs || inputs.shaderStages != slot->shaderStages) return refuse(RefusedInputs);
    const bool behind = inputs.vertexInputBehindGpuWrite;
    clock.Mark(StepInputs);
    work.targets.clear();
    for (std::size_t index = 0; index < state.colors.size(); ++index) {
        auto image = archcTargets::tryTarget(context, state.colors[index], nullptr);
        if (image == nullptr || image.get() != slot->targetRaw[index]) return refuse(RefusedTargets);
        work.targets.push_back(std::move(image));
    }
    clock.Mark(StepTargets);
    if (state.depth) {
        const auto view = DepthSurfaceViewWithStencilWriteIntent(context, *state.depth, state.depthWrite, state.stencilTest, writesStencilTarget(state));
        ArchC::ProbeAt(ArchC::Probe::DvNote);
        prepared.takenDepthView = view;
        if (slot->views.size() != state.colors.size() + 1 || view != slot->views.back() || DepthSurfacesReleased() != slot->depthReleases) return refuse(RefusedDepth);
    }
    clock.Mark(StepDepth);
    if (resources->CacheMembership() != slot->membership) return refuse(RefusedTemplate);
    if (!resources->Revalidate(shaders)) {
        SharedResourceCache().Remove(slot->contentKey);
        countCache(&DrawProfile::cacheInvalidated);
        return refuse(RefusedProof);
    }
    clock.Mark(StepProof);
    // C_REPLAY_SLOT_BINDINGS: a non-static list is the slot's last live list while its live conditions hold.
    const bool slotBindings = cutEnabled(Feature::ArchcReplaySlotBindings);
    std::optional<std::vector<ShaderResources::MovedBuffer>> liveMoved;
    bool movedFromSlot = false;
    if (identity.gather != nullptr) {
        auto moveRefusal = ShaderResources::MoveRefusal::None;
        liveMoved = resources->GatherBuffers(*identity.gather, *recorder, moveRefusal);
        if (!liveMoved.has_value()) {
            countMoveRefusal(moveRefusal, draw);
            return refuse(RefusedMoved);
        }
    } else if (!slot->movedStatic) {
        const bool verifyMoved = slotBindings && !slot->wordless && slot->liveKept && cutVerifyNow(CutSlotMoved);
        const bool holds = slotBindings && !slot->wordless && slot->liveKept && resources->MovedStillReadOnly(slot->liveMoved, *recorder);
        if (holds && !verifyMoved) {
            movedFromSlot = true;
            cutHit(CutSlotMoved);
        } else {
            if (slotBindings) cutLive(CutSlotMoved);
            auto moveRefusal = ShaderResources::MoveRefusal::None;
            liveMoved = resources->MovedReadOnlyBuffers(shaders, *recorder, &state.color, nullptr, &moveRefusal);
            if (verifyMoved && holds != (liveMoved.has_value() && sameMoved(*liveMoved, slot->liveMoved))) cutMismatch(CutSlotMoved, holds ? "the kept moved list held where the live list differs" : "the kept moved list was refused where the live list equals it");
            if (!liveMoved.has_value()) {
                countMoveRefusal(moveRefusal, draw);
                return refuse(RefusedMoved);
            }
            if (slotBindings && !slot->wordless && (!slot->liveKept || !sameMoved(*liveMoved, slot->liveMoved))) keepLiveMoved(*slot, *liveMoved);
        }
    }
    const std::span<const ShaderResources::MovedBuffer> moved = slot->movedStatic ? std::span<const ShaderResources::MovedBuffer>(slot->moved) : movedFromSlot ? std::span<const ShaderResources::MovedBuffer>(slot->liveMoved) : std::span<const ShaderResources::MovedBuffer>(*liveMoved);
    if (const auto present = Recorder::Presents(); slot->touched != present) {
        SharedResourceCache().Touch(slot->contentKey);
        slot->touched = present;
    }
    clock.Mark(StepMoved);
    const auto* argumentImport = args != nullptr ? HostImportFor(context, args->arguments, rangeBytes) : nullptr;
    if (args != nullptr && (argumentImport == nullptr || recorder->PendingLabelIn(args->arguments, rangeBytes) || StorageTexture::AnyPendingOverlaps(argumentRange))) return refuse(RefusedArgs);
    if (resources->NeedsCompletion() != slot->completion || resources->HasCopiedWrites() || resources->HoldsLease() || resources->UsesAddressTable() || !resources->Reusable()) return refuse(RefusedTemplate);
    if (state.depth) RequireNoNativeDepthFeedback_nid_no_patch(context, *state.depth, resources->SampledTextures());
    clock.Mark(StepArgs);
    const bool patchVerify = identity.gather == nullptr && slot->wordless && cutVerifyNow(CutPatchSlots);
    if (patchVerify || (identity.gather == nullptr && verifyNow())) {
        counts.verifyChecks.fetch_add(1, std::memory_order_relaxed);
        counts.kinds[draw.indirect.has_value()].verifyChecks.fetch_add(1, std::memory_order_relaxed);
        if (args == nullptr) prepared.slotDirectInputs.emplace(std::move(work.inputs));
        prepared.slotVerify = true;
        prepared.slotVerifyPatch = patchVerify;
        lastOutcome() = OutcomeVerified;
        work.inputs = DrawInputs{};
        work.targets.clear();
        return false;
    }
    if (identity.gather != nullptr && !identity.referenceStages.empty() && identity.referenceDraw != nullptr) {
        ArchB::Recorder::Count("draw_templates_live_verify_checks");
        ArchB::Recorder::Count(draw.indirect ? "draw_templates_live_verify_checks_indirect" : "draw_templates_live_verify_checks_direct");
        const auto& oldDraw = *identity.referenceDraw;
        const auto oldPlan = planDraw(context, state, oldDraw, identity.referenceStages, nullptr, replayOutcome(), replayTimer(), nullptr, nullptr, false);
        auto oldInputs = prepareDrawInputs(context, state, oldDraw, identity.referenceStages, oldPlan, replayTimer(), nullptr);
        std::optional<State> masked;
        std::uint64_t proxies = 0;
        for (std::size_t index = 0; index < state.colors.size() && index < 64; ++index) if (AttachmentProxyFormat(context, state.colors[index].format) != VK_FORMAT_UNDEFINED) proxies |= std::uint64_t{1} << index;
        const auto& pipelineState = pipelineStateFor(context, state, oldInputs.fragmentOutputs, proxies, masked);
        const auto oldPipeline = CachedPipeline(context, pipelineState, oldInputs.VertexLayout(), *resources, identity.referenceStages, VK_IMAGE_LAYOUT_GENERAL);
        if (oldPipeline != pipeline) liveMismatch(LivePipeline, "the old pipeline selection differs");
        if (args == nullptr ? !sameDirectInputs(oldInputs, inputs) : oldInputs.vertexHandles != inputs.vertexHandles || oldInputs.vertexOffsets != inputs.vertexOffsets || oldInputs.vertexInputBehindGpuWrite != inputs.vertexInputBehindGpuWrite) liveMismatch(LiveVertex, "the old vertex bindings or direct read bounds differ");
        if ((args != nullptr && (oldInputs.indexHandle != inputs.indexHandle || oldInputs.indexOffset != inputs.indexOffset)) || oldInputs.indexBytes != inputs.indexBytes || oldInputs.indexCount != inputs.indexCount || oldDraw.indexSize != draw.indexSize) liveMismatch(LiveIndex, "the old index binding or effective count differs");
        if (oldDraw.indexed != draw.indexed || oldDraw.indexCount != draw.indexCount || oldDraw.instanceCount != draw.instanceCount || oldDraw.flags != draw.flags || oldDraw.firstVertex != draw.firstVertex || oldDraw.firstInstance != draw.firstInstance || oldDraw.indexAddress != draw.indexAddress) liveMismatch(LiveArguments, "the old draw packet values differ");
        if (oldDraw.indirect.has_value() != draw.indirect.has_value()) liveMismatch(LiveIndirect, "the old direct or indirect command kind differs");
        else if (args != nullptr && (!sameIndirect(*oldDraw.indirect, *args) || oldDraw.indirect->arguments != args->arguments || oldDraw.indirect->countAddress != args->countAddress)) liveMismatch(LiveIndirect, "the old indirect binding or rewrite rules differ");
        else if (args != nullptr) {
            const auto* oldImport = HostImportFor(context, oldDraw.indirect->arguments, static_cast<std::size_t>(oldDraw.indirect->RangeBytes()));
            if (oldImport == nullptr || oldImport->buffer != argumentImport->buffer || HostImportOffset(*oldImport, oldDraw.indirect->arguments) != HostImportOffset(*argumentImport, args->arguments)) liveMismatch(LiveIndirect, "the old argument import differs");
        }
    }
    if (state.depth && writesDepthTarget(state)) NoteDepthSurfaceWrite(context, *state.depth);
    PerformanceTimer timing("Graphics.DrawReplay", AgcDriver::ArchB::Recorder::Phase::Record);
    struct ReuseGuard {
        Recorder* recorder;
        int exceptions = std::uncaught_exceptions();
        ~ReuseGuard() {
            if (std::uncaught_exceptions() != exceptions) recorder->InvalidatePipelineState("fix_pso_reuse_invalidations_exception");
        }
    } reuseGuard{recorder};
    if (!pipelineStateReuse) recorder->ReusePipelineState(false);
    auto* reuse = pipelineStateReuse ? recorder->ReusePipelineState(true) : nullptr;
    ArchC::ProbeAt(ArchC::Probe::BdReuse);
    const auto touches = [&](std::uint64_t begin, std::uint64_t end) {
        const auto bytes = static_cast<std::size_t>(end - begin);
        if (resources->WritesOverlap(begin, bytes) || resources->ReadsOverlap(begin, bytes)) return true;
        return args != nullptr && begin < args->arguments + args->RangeBytes() && args->arguments < end;
    };
    if (recorder->HasQueuedKeyStores() && recorder->AnyQueuedKeyStore(touches)) recorder->FlushKeyStores();
    if (recorder->HasQueuedStores() && recorder->AnyQueuedStore(touches)) recorder->FlushStores();
    ArchC::ProbeAt(ArchC::Probe::BdFlush);
    auto& linearReads = work.linearReads;
    linearReads.assign(slot->pass.linearReads.begin(), slot->pass.linearReads.end());
    if (args != nullptr) linearReads.insert(linearReads.begin() + static_cast<std::ptrdiff_t>(slot->argumentLinearRead), argumentRange.front());
    linearReads.insert(linearReads.end(), inputs.inPlaceReads.begin(), inputs.inPlaceReads.end());
    static_cast<void>(StorageTexture::StoreDrawnLinearTouching("draw access", [&](std::uint64_t begin, std::uint64_t end) { return std::any_of(linearReads.begin(), linearReads.end(), [&](const auto& read) { return read.first < end && begin < read.second; }) || resources->WritesOverlap(begin, static_cast<std::size_t>(end - begin)); }, slot->pass.unknownMemory));
    ArchC::ProbeAt(ArchC::Probe::BdLinear);
    // C_REPLAY_SLOT_BINDINGS: PrepareDrawBindings' answer without the call where it depends only on the slot:
    // with zero-copy storage and nothing moved it binds the template's set (no bindings); with a moved list it
    // hands back the cached moved set while the batch holds no GPU store, no moved range has a pending write
    // and the imports are those the set was built under. The set's read notes and its keep still run.
    const auto drawUse = resources->BeginDrawUse(*recorder);
    std::shared_ptr<ShaderResources::DrawBindings> drawBindings;
    std::shared_ptr<void> slotSet;
    bool slotBound = false;
    struct PlanReset {
        ~PlanReset() {
            auto& plan = templatePlan();
            plan.sets.reset();
            plan.committed = false;
        }
    } planReset;
    std::shared_ptr<ShaderResources::TemplateSets> templateSets;
    if (moved.empty()) {
        templateCounters().nothingMoved.fetch_add(1, std::memory_order_relaxed);
    } else {
        templateCounters().candidates.fetch_add(1, std::memory_order_relaxed);
        if (cutEnabled(Feature::DrawTemplates) && prepareTemplate(context, *recorder, *slot, *resources, moved, drawUse != nullptr, identity.gather != nullptr ? &identity : nullptr)) templateSets = templatePlan().sets;
    }
    if (identity.gather != nullptr && templateSets == nullptr && !moved.empty()) return refuse(RefusedMoved);
    if (args == nullptr && identity.gather == nullptr && templateSets != nullptr && templatePlan().verify) {
        const auto oldPlan = planDraw(context, state, draw, shaders, nullptr, replayOutcome(), replayTimer(), nullptr, nullptr, false);
        const auto oldInputs = prepareDrawInputs(context, state, draw, shaders, oldPlan, replayTimer(), nullptr);
        std::optional<State> masked;
        const auto& oldState = pipelineStateFor(context, state, oldInputs.fragmentOutputs, 0, masked);
        const auto oldPipeline = CachedPipeline(context, oldState, oldInputs.VertexLayout(), *resources, shaders, VK_IMAGE_LAYOUT_GENERAL);
        AgcDriver::ArchB::Recorder::Count("draw_templates_direct_inputs_verified");
        if (!sameDraw(slot->draw, draw) || !sameDirectInputs(oldInputs, inputs) || oldPipeline != pipeline) {
            AgcDriver::ArchB::Recorder::Count("draw_templates_direct_inputs_mismatches");
            static unsigned reported = 0;
            if (reported++ < 8) std::fprintf(stderr, "[draw-templates] VERIFY mismatch: direct command, input bindings, read bounds or pipeline\n");
        }
    }
    if (templateSets == nullptr && slotBindings) {
        if (moved.empty() && Recorder::ZeroCopyStorageActive()) {
            slotBound = true;
        } else if (!slot->wordless && !moved.empty() && Recorder::ZeroCopyStorageActive() && cutEnabled(Feature::ZeroCopyMoved) && recorder->OpenGpuStores() == 0 && (slotSet = slot->bindSet.lock()) != nullptr) {
            slotBound = std::none_of(slot->bindReads.begin(), slot->bindReads.end(), [&](const auto& read) { return recorder->PendingWriteOverlaps(read.first, static_cast<std::size_t>(read.second - read.first)); }) && HostImportsUnchanged(context, slot->bindProof);
            if (!slotBound) slotSet.reset();
        }
    }
    const bool verifyBindings = templateSets == nullptr && slotBound && cutVerifyNow(CutSlotBindings);
    if (templateSets == nullptr && slotBound && !verifyBindings) {
        cutHit(CutSlotBindings);
        if (slotSet != nullptr) {
            static const bool noNotes = std::getenv("APS5_ZC_MOVED_NO_READNOTES") != nullptr;
            if (!noNotes)
                for (const auto& [begin, end] : slot->bindReads) recorder->NotePendingRead(begin, static_cast<std::size_t>(end - begin), Recorder::ReadKind::DispatchElement);
            recorder->Keep(slotSet);
        }
    } else if (templateSets == nullptr) {
        if (slotBindings) cutLive(CutSlotBindings);
        drawBindings = resources->PrepareDrawBindings(*recorder, moved, false, drawUse != nullptr ? drawUse->record.get() : nullptr);
        if (verifyBindings) {
            const bool same = slotSet == nullptr ? drawBindings == nullptr : drawBindings != nullptr && drawBindings->cachedSet == slotSet && drawBindings->allocation.set == slot->bindVertex && drawBindings->fragmentAllocation.set == slot->bindFragment && drawBindings->inPlaceReads == slot->bindReads;
            if (!same) cutMismatch(CutSlotBindings, slotSet == nullptr ? "the slot answered no bindings" : "the slot's cached moved set differs from PrepareDrawBindings'");
        }
        if (slotBindings && !slot->wordless && !moved.empty() && drawBindings != nullptr && drawBindings->cachedSet != nullptr) {
            slot->bindSet = drawBindings->cachedSet;
            slot->bindVertex = drawBindings->allocation.set;
            slot->bindFragment = drawBindings->fragmentAllocation.set;
            slot->bindReads = drawBindings->inPlaceReads;
            slot->bindProof = drawBindings->movedProof;
        }
        slotSet.reset();
    }
    const VkDescriptorSet boundSet = slotSet != nullptr ? slot->bindVertex : drawBindings != nullptr ? drawBindings->allocation.set : VK_NULL_HANDLE;
    const VkDescriptorSet boundFragmentSet = slotSet != nullptr ? slot->bindFragment : drawBindings != nullptr ? drawBindings->fragmentAllocation.set : VK_NULL_HANDLE;
    const bool setBound = templateSets != nullptr || slotSet != nullptr || drawBindings != nullptr;
    const auto* boundReads = templateSets != nullptr ? &templatePlan().reads : slotSet != nullptr ? &slot->bindReads : drawBindings != nullptr ? &drawBindings->inPlaceReads : nullptr;
    ArchC::ProbeAt(ArchC::Probe::BdPrepare);
    static const bool zcNoPassReads = std::getenv("APS5_ZC_MOVED_NO_PASSREADS") != nullptr;
    auto& passReads = work.passReads;
    passReads.assign(slot->pass.passReads.begin(), slot->pass.passReads.end());
    if (args != nullptr) passReads.insert(passReads.begin() + static_cast<std::ptrdiff_t>(slot->argumentRead), argumentRange.front());
    passReads.insert(passReads.end(), inputs.inPlaceReads.begin(), inputs.inPlaceReads.end());
    if (!zcNoPassReads && boundReads != nullptr && !boundReads->empty()) passReads.insert(passReads.end(), boundReads->begin(), boundReads->end());
    clock.Mark(StepBindings);
    const PassAccess passAccess{passReads, slot->pass.passWrites, slot->pass.images, slot->pass.attachments, slot->pass.unknownMemory, slot->pass.unknownWrites, slot->pass.writeOnly};
    using CommandClass = Recorder::CommandClass;
    const auto countBarrier = [&](std::uint32_t count) { Recorder::CountBarriers(CommandClass::Draw, count); };
    resources->RequireGuestBuffers();
    const auto start = recorder->StartDrawPass(slot->pass.passKey, slot->pass.forced, slot->pass.fenced, passAccess);
    ArchC::ProbeAt(ArchC::Probe::RcStart);
    const bool continued = start.continued;
    const auto commands = start.commands;
    if (!continued) {
        recorder->PrepareSampleSlot();
        PerformanceTrace::Count(PerformanceTrace::RenderPassScope);
    }
    const auto drawTiming = !continued ? recorder->BeginGpuTiming(CommandClass::Draw) : Recorder::NoTiming;
    const auto passTimingKey = Recorder::GpuTimingEnabled() ? timingPassKey(state) : 0;
    if (drawTiming != Recorder::NoTiming) recorder->TagGpuTiming(drawTiming, passTimingKey);
    ArchC::ProbeAt(ArchC::Probe::RcTiming);
    std::unique_ptr<DeviceBuffer> argumentScratch;
    VkBuffer argumentBuffer = VK_NULL_HANDLE;
    VkDeviceSize argumentOffset = 0;
    bool rewritten = false;
    IndirectRecord indirect;
    indirect.args = args;
    indirect.path = IndirectDrawPath::Gpu;
    indirect.argumentImport = argumentImport;
    if (continued) {
        // C_REPLAY_CONTINUE: Continue records nothing when the reuse state holds this pipeline's selected
        // handle and layout and its input memo holds these inputs (Bind skips, SameInputs hits); only the
        // deferred relink's Schedule remains. VERIFY runs Continue and requires the reuse state unchanged.
        bool held = false;
        if (reuse != nullptr && !state.stencilReplayReference && cutEnabled(Feature::ArchcReplayContinue)) {
            pipeline->ScheduleOptimized();
            std::array<std::byte, 256> continueInputs;
            std::size_t continueSize = 0;
            pipeline->ContinueInputs(state, continueInputs, continueSize);
            held = reuse->HoldsContinue(pipeline->Selected(), pipeline->Layout(), pipeline.get(), {continueInputs.data(), continueSize});
            if (held && cutVerifyNow(CutContinue)) {
                const auto before = reuse->Changes();
                pipeline->Continue(commands, state, reuse);
                if (reuse->Changes() != before) cutMismatch(CutContinue, "Continue changed the reuse state the memo held");
            } else if (held) {
                cutHit(CutContinue);
            } else {
                cutLive(CutContinue);
            }
        }
        if (!held) pipeline->Continue(commands, state, reuse);
        if (args != nullptr && slot->pass.inPassIndirect) {
            recorder->NotePendingRead(args->arguments, rangeBytes, Recorder::ReadKind::Indirect);
            argumentBuffer = argumentImport->buffer;
            argumentOffset = HostImportOffset(*argumentImport, args->arguments);
        }
    } else {
        if (start.barrier) {
            const VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
            context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
            countBarrier(1);
        }
        if (behind) {
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
            countBarrier(1);
        }
        if (args != nullptr) rewritten = recordIndirectArguments(context, commands, recorder, true, indirect, argumentScratch, argumentBuffer, argumentOffset, countBarrier);
        pipeline->Begin(commands, *framebuffer, state.renderExtent, state, reuse);
    }
    ArchC::ProbeAt(continued ? ArchC::Probe::RcBegin : ArchC::Probe::RcBeginNew);
    if (setBound) {
        const auto templateFlight = templateSets != nullptr ? commitTemplate(context, *recorder, *resources, moved, *slot) : 0;
        const auto set = templateSets != nullptr ? templateSets->allocations[templateFlight].set : boundSet;
        const auto fragmentSet = templateSets != nullptr ? templateSets->fragmentAllocations[templateFlight].set : boundFragmentSet;
        if (set != VK_NULL_HANDLE) context.Resolved(&DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Layout(), 0, 1, &set, 0, nullptr);
        if (fragmentSet != VK_NULL_HANDLE) context.Resolved(&DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Layout(), ShaderRecompiler::RuntimeAbi::FragmentDescriptorSet, 1, &fragmentSet, 0, nullptr);
    } else {
        resources->BindDrawUse(commands, pipeline->Layout(), drawUse.get());
    }
    ArchC::ProbeAt(ArchC::Probe::RcBind);
    auto livePush = slot->push;
    if (identity.gather != nullptr) {
        livePush = {};
        for (std::size_t stage = 0; stage < identity.gather->stages.size(); ++stage) {
            const auto& push = identity.gather->stages[stage].push;
            const auto offset = identity.gatherTemplate->stages[stage].pushOffset;
            Require(offset <= livePush.size() && push.size() <= livePush.size() - offset, "live gather push exceeds its stage slot");
            std::copy(push.begin(), push.end(), livePush.begin() + offset);
        }
        resources->PatchPushConstants(livePush);
    }
    if (identity.gather != nullptr && !identity.referenceStages.empty()) {
        auto oldPush = AssemblePushConstants(identity.referenceStages);
        resources->PatchPushConstants(oldPush);
        bool samePush = PushConstantStages(identity.referenceStages) == PushConstantStages(shaders);
        if (samePush && oldPush != livePush) {
            auto ignored = oldPush;
            ignored.fill(std::byte{0});
            for (std::size_t stage = 0; stage < identity.referenceStages.size() && stage < identity.referenceMasks.size(); ++stage) {
                const auto& reference = identity.referenceStages[stage];
                if (reference.program == nullptr) continue;
                for (const auto& [word, bits] : identity.referenceMasks[stage].push) {
                    for (std::size_t byte = 0; byte < 4; ++byte) {
                        const auto offset = static_cast<std::size_t>(word) * 4u + byte;
                        if (offset < reference.program->pushConstants.size() && reference.pushConstantOffset + offset < ignored.size()) ignored[reference.pushConstantOffset + offset] |= static_cast<std::byte>((bits >> (byte * 8u)) & 0xffu);
                    }
                }
            }
            for (std::size_t offset = 0; offset < oldPush.size() && samePush; ++offset) samePush = ((oldPush[offset] ^ livePush[offset]) & ~ignored[offset]) == std::byte{0};
        }
        if (!samePush) liveMismatch(LivePush, "the old push block differs");
    }
    pushDrawConstants(*pipeline, commands, state, draw, shaders, *resources, &livePush, identity.gather != nullptr ? PushConstantStages(shaders) : slot->pushStages, 0, false, inputs.meshLayerStride, reuse);
    ArchC::ProbeAt(ArchC::Probe::RcPush);
    static const bool timeEachDraw = std::getenv("APS5_PROFILE_GPU_DRAWS") != nullptr;
    auto eachDrawTiming = Recorder::NoTiming;
    if (timeEachDraw) {
        std::uint64_t key = 0;
        for (const auto& shader : shaders) {
            if (shader.program != nullptr && (key == 0 || shader.stage == ShaderRecompiler::ShaderStage::Fragment)) key = shader.program->variantId;
        }
        eachDrawTiming = recorder->BeginInPassGpuTiming(Recorder::DrawTimingKey(key));
    }
    const auto ownTiming = Recorder::GpuDrawTimingEnabled() && state.renderLayers <= 1u ? recorder->BeginGpuTimingInPass(timingDrawKey(shaders, passTimingKey)) : Recorder::NoTiming;
    noteDrawFaultToken(context, *recorder, commands, shaders, draw, static_cast<std::uint32_t>(AgcDriver::FaultStage::graphics), args);
    std::uint32_t subdraw = 0;
    recordDrawCommands(context, commands, state, draw, inputs, args != nullptr ? &indirect : nullptr, argumentBuffer, argumentOffset, trace, subdraw);
    if (pipeline->SplitsFaces()) {
        pipeline->ContinueBackFaces(commands, state);
        recordDrawCommands(context, commands, state, draw, inputs, args != nullptr ? &indirect : nullptr, argumentBuffer, argumentOffset, trace, subdraw);
    }
    recorder->EndGpuTiming(ownTiming);
    recorder->EndGpuTiming(eachDrawTiming);
    if (args != nullptr) CountIndirectDraw(IndirectDrawPath::Gpu, 0, rewritten);
    ArchC::ProbeAt(ArchC::Probe::RcDraw);
    recorder->LeaveRenderPassOpen(slot->pass.passKey, drawTiming, slot->pass.fenced, passAccess, {});
    ArchC::ProbeAt(ArchC::Probe::RcLeave);
    if (templateSets != nullptr) finishTemplate(*recorder);
    if (identity.gather == nullptr) publishGatherSlot(context, state, draw, identity, *slot, *resources, moved);
    else {
        AgcDriver::ArchB::Recorder::Count("draw_templates_live_commits");
        AgcDriver::ArchB::Recorder::Count(draw.indirect ? "draw_templates_live_commits_indirect" : "draw_templates_live_commits_direct");
    }
    clock.Mark(StepRecord);
    keepRecordedDraw(*recorder, resources, std::move(pipeline), std::move(framebuffer), inputs, std::move(work.targets), slot->slices, state.scissor, std::move(argumentScratch), {}, false, slot->completion, replayOutcome(), drawUse, nullptr);
    work.targets.clear();
    work.inputs = DrawInputs{};
    clock.Mark(StepKeep);
    clock.Finish();
    slot->replayedLast = true;
    if (slot->wordless) cutHit(CutPatchSlots);
    counts.hits.fetch_add(1, std::memory_order_relaxed);
    counts.kinds[draw.indirect.has_value()].hits.fetch_add(1, std::memory_order_relaxed);
    lastOutcome() = OutcomeReplayed;
    return true;
}

}

bool storesIntoResident(const ShaderResources& resources, const StorageTexture& resident) {
    return std::ranges::any_of(resources.StorageImages(), [&](const std::pair<VkImage, bool>& image) { return image.first == resident.Image() && image.second; });
}

bool prepareDraw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, PreparedDraw& prepared) {
    auto*& recipeOut = prepared.recipeOut;
    auto& stencilReferences = prepared.stencilReferences;
    for (const auto& shader : shaders) {
        if (shader.program != nullptr && !shader.program->stencilReferences.empty()) {
            Require(shader.stage == ShaderRecompiler::ShaderStage::Fragment && stencilReferences.empty(), "stencil replay requires exactly one fragment stage");
            stencilReferences = shader.program->stencilReferences;
        }
    }
    if (!stencilReferences.empty()) {
        Require(!draw.indirect && !state.primitiveRestart && !state.rectList && !state.quadList && !state.stages.mesh && !state.stages.tessellation && state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, "stencil replay supports direct triangle lists without restart, mesh or tessellation");
        Require(!state.depthBiasPerFace && !ShadersWriteMemory(shaders), "stencil replay refuses face splits and shader memory writes");
        Require(std::ranges::all_of(shaders, [](const CompiledShader& shader) { return shader.program != nullptr && shader.program->stencilReplaySafe && (shader.stage == ShaderRecompiler::ShaderStage::Vertex || shader.stage == ShaderRecompiler::ShaderStage::Fragment); }), "stencil replay refuses stages with side effects, clocks, subgroup dependencies or primitive ID inputs");
        if (stencilReferences.size() == 256u) {
            static std::once_flag unbounded;
            std::call_once(unbounded, [] { APS5_LOG_CHARS_OUT("Stencil replay: a draw replays all 256 stencil reference values because its pixel shader's export is not bounded, recording 256 passes per triangle and instance"); });
        }
        for (const auto& shader : shaders) {
            if (shader.stage != ShaderRecompiler::ShaderStage::Vertex) continue;
            Require((shader.program->hostSubgroupSize == 1u || shader.program->hostSubgroupSize == context.subgroup.subgroupSize) && shader.program->spirv.size() >= 5u && shader.program->spirv[1] >= 0x00010000u && shader.program->spirv[1] < 0x00010600u, "stencil replay vertex subgroup contract differs from the physical device");
        }
        Require(static_cast<std::uint64_t>(draw.firstInstance) + draw.instanceCount <= 0x100000000ull && (draw.indexed || static_cast<std::uint64_t>(draw.firstVertex) + draw.indexCount <= 0x100000000ull), "stencil replay draw indices overflow");
        Require(state.samples == 1u && (!state.depth || HasStencil(state.depth->format)), "stencil replay requires single-sample rendering and an eight-bit stencil format");
        recipeOut = nullptr;
    }
    if (state.quadList || StripPatches(state, shaders)) recipeOut = nullptr;
    if (context.recorder != nullptr) context.recorder->BoundKeptBytes();
    auto& timing = prepared.timing.emplace("Graphics.Draw", AgcDriver::ArchB::Recorder::Phase::Prepare);
    // APS5_PROFILE_DRAW prints the time of each phase of the draw (microseconds) and the [draws] totals.
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& timer = prepared.timer;
    timer = DrawTimer(profile);
    // APS5_TRACE_DRAWS (together with APS5_PROFILE_DRAW; alone it only enables Driver.cpp's own
    // [draw] lines) additionally prints one line per draw with its phases and, for synchronous
    // draws, their inputs: per-draw string building at ~1400 draws per 10 s is itself a cost the
    // totals must not carry.
    static const bool traceDraws = std::getenv("APS5_TRACE_DRAWS") != nullptr;
    auto& outcome = prepared.outcome;
    auto& built = prepared.built;
    // The thread's GPU waits so far: the difference at the report, less the draw's own syncs
    // (`ownWaitedMs`, sampled around them), is what this draw waited for through the flush hook
    // (nested in the draw's hold, see DrawOutcome::hookWaitUs).
    const auto waitedBefore = prepared.waitedBefore = profile ? Recorder::ThreadWaitedMs() : 0.0;
    auto& ownWaitedMs = prepared.ownWaitedMs;
    const auto report = [&](const char* suffix) { reportDrawEnd(state, timer, built, outcome, waitedBefore, ownWaitedMs, suffix); };
    auto& inputs = prepared.inputs;
    const auto plan = planDraw(context, state, draw, shaders, nullptr, outcome, timer, prepared.preparation);
    if (plan.nothing) return false;
    inputs = prepareDrawInputs(context, state, draw, shaders, plan, timer, &timing);
    if (inputs.nothing) {
        if (VerifyDrawPlan()) requireDrawPlanRepeats(context, state, draw, shaders, plan);
        return false;
    }
    const auto* args = prepared.args = draw.indirect ? &*draw.indirect : nullptr;
    const auto indexBytes = inputs.indexBytes;
    const int dumpLimit = prepared.dumpLimit = DumpTargetLimit();
    auto& targets = prepared.targets;
    targets.resize(state.colors.size());
    auto& targetViews = prepared.targetViews;
    for (std::size_t index = 0; index < state.colors.size(); ++index) {
        auto& binding = targets[index];
        binding.color = state.colors[index];
        const auto& color = binding.color;
        binding.gpuTiling = color.tileMode == ColorTileMode::RenderTarget && context.detiler != nullptr;
        if (color.samples > 1) {
            binding.multisampled = true;
            timer.phase(PhaseSetup);
            if (color.staged) {
                const auto packed = StagedMergeTarget(color);
                binding.stagedTemporary = AcquireStagedTemporary(context, color.extent, color.samples);
                PrepareMultisampleTargetForDraw_nid_no_patch(context, packed);
                static_cast<void>(MultisampleTargetView(context, packed, binding.multisampleOwner, &binding.multisampleImage));
                binding.stagedTarget = MultisampleTargetStorageView(context, TargetMergeSurface(packed));
                Require(MultisampleTargetStorable(context, packed), "a partial write of a multisampled packed color target needs a multisample image of this form to allow storage");
                targetViews.push_back(binding.stagedTemporary->View());
                timer.phase(PhaseReadTarget);
                continue;
            }
            PrepareMultisampleTargetForDraw_nid_no_patch(context, color);
            targetViews.push_back(MultisampleTargetView(context, color, binding.multisampleOwner, &binding.multisampleImage));
            timer.phase(PhaseReadTarget);
            continue;
        }
        APS5_LOG_OUT_DEBUG("Creating color target %zu address=0x%llx bytes=%llu extent=%ux%u", index, static_cast<unsigned long long>(color.address), static_cast<unsigned long long>(color.bytes), color.extent.width, color.extent.height);
        const ColorTargetLayout colorLayout(color.extent.width, color.extent.height, color.tileMode, color.elementBytes);
        // Debug aid: APS5_NO_RESIDENT_TARGETS=1 copies every target in and out again.
        static const bool residentTargets = std::getenv("APS5_NO_RESIDENT_TARGETS") == nullptr;
        static const bool residentLinear = std::getenv("APS5_NO_RESIDENT_LINEAR") == nullptr;
        auto fastResident = residentTargets && context.detiler != nullptr && archcTargets::fastActive() ? archcTargets::tryTarget(context, color, nullptr) : nullptr;
        const bool fastHit = fastResident != nullptr && !archcTargets::verifyHitNow();
        const archcTargets::VerifyHit verifyHit(fastHit ? nullptr : fastResident);
        const auto censusDepthStart = archcTargets::censusActive() ? archcTargets::nowNs() : 0;
        if (!fastHit) PrepareColorTargetDepth(context.device, color.address, colorLayout.Bytes());
        const auto censusDepthNs = censusDepthStart != 0 ? archcTargets::nowNs() - censusDepthStart : 0;
        ClaimMultisampleTargetMemory(color.address, colorLayout.Bytes());
        if (color.fmaskAddress != 0) GuardOneFragmentFmask(context, color);
        constexpr VkBufferUsageFlags copies = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        timer.phase(PhaseSetup);
        const bool linearResident = !fastHit && residentLinear && residentTargets && context.detiler != nullptr && color.tileMode == ColorTileMode::Linear && (ExistingHostImportFor(context, color.address, colorLayout.Bytes()) != nullptr || (GuestMemory::Watched(color.address, colorLayout.Bytes()) && HostImportFor(context, color.address, colorLayout.Bytes(), ImportRequester::ColorTarget) != nullptr));
        if (fastHit) {
            binding.resident = std::move(fastResident);
        } else if ((binding.gpuTiling || ((!ColorTileModeIsLinear(color.tileMode) || linearResident) && context.detiler != nullptr)) && residentTargets) {
            // The lookup refreshes the image on every draw (StorageTexture::Refresh: FlushPending,
            // CollectWrites over the target's pages, the DCC key scan of TextureClearKeys, then
            // UnchangedSince). The page walk is skipped while the worker's collect epoch lasts
            // (GuestMemory::BumpCollectEpoch: ordering points of the queue, or every packet under
            // APS5_PACKET_EPOCH=1); the key scan runs on every lookup regardless. Lookups of a
            // millisecond or more are counted apart; the [texture] line says how many uploaded.
            auto* targetPreparation = prepared.preparation != nullptr && prepared.preparation->device == context.device && index < prepared.preparation->targets.size() ? &prepared.preparation->targets[index] : nullptr;
            const auto lookup = [&](StorageTexture::RefreshPreparation* targetProof) {
                if (color.arrayLastSlice != 0) {
                    Require(color.dccAddress == 0 && color.cmaskAddress == 0 && AttachmentProxyFormat(context, color.format) == VK_FORMAT_UNDEFINED, "a slice of a 2D array color target with DCC, CMASK or an attachment proxy has no resident array image");
                    const auto array = ArraySurfaceForTarget(color);
                    // DecodeColorBuffer offsets slice s by s times this size; every slice the image stores must agree.
                    const auto sliceBytes = ComputeSurfaceSize(ComputeElementMipLayout(ColorTextureTileMode(color.tileMode), color.elementBytes, color.surfaceExtent.width, color.surfaceExtent.height, color.mipCount), 1);
                    const auto geometry = DescribeSurface(array);
                    for (std::uint32_t slice = 0; slice <= color.arrayLastSlice; ++slice) Require(geometry.GuestLayerOffset(slice) == slice * sliceBytes, "the array image's layer pitch differs from the color view's slice offset");
                    Require(color.surfaceAddress == color.arrayAddress + color.arraySlice * sliceBytes, "the array image's layer pitch differs from the color view's slice offset");
                    auto resident = CachedStorageLayers(context, array, color.arraySlice, color.layers);
                    Require(resident->Attachable(), "storage format cannot be a color attachment");
                    return resident;
                }
                const auto surface = SurfaceForTarget(color);
                auto resident = RepeatStorageSurface(context, surface, shapeTarget(context, prepared.shape, index));
                if (resident != nullptr && VerifyShapeMemo()) {
                    Require(CachedStorageSurface(context, surface, targetProof) == resident, "APS5_VERIFY_SHAPE_MEMO: the shape memo's resident target differs from the storage cache's");
                    noteShapeMemoVerified(false);
                }
                if (resident == nullptr) resident = CachedStorageSurface(context, surface, targetProof);
                Require(resident->Attachable(), "storage format cannot be a color attachment");
                Require(color.volume || color.mipCount > 1 || color.depth > 1 || resident->GuestBytes() == colorLayout.Bytes(), "resident image layout differs from the color layout");
                return resident;
            };
            archcTargets::CensusProbe censusProbe(context, color, false);
            const auto censusStart = censusProbe.Active() ? archcTargets::nowNs() : 0;
            archcTargets::LegacyPass legacyPass(context, color);
            binding.resident = withTargetLookup(lookup, [&](const TargetLookup& target) { return refreshResidentTarget(context, state, color, draw, shaders, outcome, profile, target, targetPreparation); });
            verifyHit.Check(binding.resident);
            legacyPass.Finish(binding.resident);
            if (censusProbe.Active()) {
                const auto blockNs = archcTargets::nowNs() - censusStart;
                const auto prelock = index < 8 ? std::exchange(archcTargets::prelockNs()[index], 0) : 0;
                archcTargets::censusCounters().depthNs.fetch_add(censusDepthNs, std::memory_order_relaxed);
                archcTargets::censusCounters().prelockNs.fetch_add(prelock, std::memory_order_relaxed);
                censusProbe.After(binding.resident, blockNs, censusDepthNs + prelock);
            }
        }
        if (binding.resident != nullptr) {
            timer.phase(PhaseReadTarget);
            binding.proxied = AttachmentProxyFormat(context, color.format) != VK_FORMAT_UNDEFINED;
            targetViews.push_back(binding.proxied ? binding.resident->AttachmentProxyView() : binding.resident->AttachmentView(color.format, color.mip, color.depthSlice, color.layers, color.arraySlice));
            continue;
        }
        materializeCmaskClear(context, color, nullptr);
        Require(!color.mipTail || binding.gpuTiling, "rendering into a packed mip tail requires the GPU tiled fallback");
        Require(color.depth == 1 && (!color.volume || (color.mipCount == 1 && color.tileMode == ColorTileMode::Linear)), "rendering into a 3D color target needs the resident image of its surface");
        Require(color.layers == 1, "layered rendering into a 2D array color target needs the resident image of its array");
        Require(color.arraySlice == 0 || !ColorTileModeIsXor(color.tileMode), "rendering into a slice of a 2D array color target with an XOR tile mode needs the resident image of its array");
        if (binding.gpuTiling) {
            binding.mip = color.mipTail ? ComputeColorTargetMipLayout(color.tileMode, color.elementBytes, color.surfaceExtent.width, color.surfaceExtent.height, color.mipCount, color.mip) : ColorTargetMip(color, colorLayout);
            binding.tiled = std::make_unique<Buffer>(context, colorLayout.Bytes(), copies);
            binding.tiledDevice = std::make_unique<DeviceBuffer>(context, colorLayout.Bytes(), copies | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            binding.linearDevice = std::make_unique<DeviceBuffer>(context, colorLayout.LinearBytes(), copies | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            binding.original.resize(colorLayout.Bytes());
            GuestMemory::Read(color.address, binding.original, colorLayout.Alignment());
            std::memcpy(binding.tiled->Bytes().data(), binding.original.data(), binding.original.size());
        } else {
            const auto x0 = static_cast<std::uint32_t>(std::max(state.scissor.offset.x, 0));
            const auto y0 = static_cast<std::uint32_t>(std::max(state.scissor.offset.y, 0));
            const auto x1 = std::min<std::uint32_t>(x0 + state.scissor.extent.width, color.extent.width);
            const auto y1 = std::min<std::uint32_t>(y0 + state.scissor.extent.height, color.extent.height);
            binding.banded = color.tileMode == ColorTileMode::Linear && color.dccAddress == 0 && x0 < x1 && y0 < y1 && (x1 - x0 < color.extent.width || y1 - y0 < color.extent.height);
            if (binding.banded) {
                binding.band = {{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0)}, {x1 - x0, y1 - y0}};
                binding.bandOffset = colorLayout.Offset(x0, y0);
                binding.bandPitch = static_cast<std::uint32_t>(colorLayout.Bytes() / (std::size_t{color.extent.height} * color.elementBytes));
                const auto bandBytes = colorLayout.Offset(x1 - 1, y1 - 1) + color.elementBytes - binding.bandOffset;
                binding.transfer = std::make_unique<Buffer>(context, bandBytes, copies);
                binding.original.resize(bandBytes);
                GuestMemory::Read(color.address + binding.bandOffset, binding.original, color.elementBytes);
                std::memcpy(binding.transfer->Bytes().data(), binding.original.data(), bandBytes);
            } else {
                binding.transfer = std::make_unique<Buffer>(context, colorLayout.LinearBytes(), copies);
                ReadColorTarget(color, binding.transfer->Bytes());
            }
        }
        // A fast-cleared DCC target holds its clear value wherever the draw does not write.
        if (color.dccAddress != 0) {
            const auto keys = CurrentDccKeys(color.dccAddress, colorLayout.Bytes());
            if (IsDccClear(keys)) {
                const auto pixels = binding.gpuTiling ? binding.tiled->Bytes() : binding.transfer->Bytes();
                if (keys == DccKeys::ClearRegister) {
                    const auto texel = clearTexel(color, keys);
                    for (std::size_t offset = 0; offset + color.elementBytes <= pixels.size(); offset += color.elementBytes) std::memcpy(pixels.data() + offset, texel.data(), color.elementBytes);
                } else if (!FillDccClear(color.format, keys, color.dccAlphaOnMsb, pixels)) {
                    static std::set<std::pair<std::uint64_t, int>> reported;
                    if (reported.size() < 32 && reported.insert({color.address, static_cast<int>(keys)}).second) std::fprintf(stderr, "[gpu] color target 0x%llx (VkFormat %d) has %s DCC keys; its stored texels are used\n", static_cast<unsigned long long>(color.address), static_cast<int>(color.format), DccKeysName(keys));
                    if (binding.gpuTiling) std::memcpy(pixels.data(), binding.original.data(), binding.original.size());
                    else ReadColorTarget(color, pixels);
                }
            }
        }
        timer.phase(PhaseReadTarget);
        binding.target = std::make_unique<RenderTarget>(context, color, state.blends.at(color.exportIndex).blendEnable != 0);
        targetViews.push_back(binding.target->View());
    }
    if (state.depth) {
        if (prepared.takenDepthView.has_value()) {
            targetViews.push_back(*prepared.takenDepthView);
        } else {
            targetViews.push_back(DepthSurfaceViewWithStencilWriteIntent(context, *state.depth, state.depthWrite, state.stencilTest, writesStencilTarget(state)));
        }
    }
    timer.phase(PhasePrepare);
    const bool recordDraws = RecordDraws();
    auto* recorder = prepared.recorder = Recorder::Active();
    bool recordable = recordDraws && recorder != nullptr && dumpLimit == 0 && std::all_of(targets.begin(), targets.end(), [](const TargetBinding& binding) { return binding.resident != nullptr || binding.multisampled; });
    auto& resolved = prepared.resolved;
    resolved = resolveDrawResources(context, state, draw, shaders, snapshots, indexBytes, recordable, plan, outcome, timer, prepared.preparation);
    if (stopAfterResolve.load(std::memory_order_relaxed) != 0) {
        const auto stop = stopAfterResolve.exchange(0, std::memory_order_relaxed);
        if (stop == 1) return false;
        if (stop == 2) throw std::runtime_error("injected failure after the draw's resources were resolved");
    }
    if (state.depth && writesDepthTarget(state)) NoteDepthSurfaceWrite(context, *state.depth);
    auto& resources = resolved.resources;
    for (std::size_t index = 0; index < targets.size(); ++index) {
        auto& binding = targets[index];
        if (binding.color.partialWriteMask == 0 || binding.multisampled || binding.resident == nullptr || binding.proxied || storesIntoResident(*resources, *binding.resident)) continue;
        const auto& color = binding.color;
        const auto lookup = [&](StorageTexture::RefreshPreparation* targetProof) { return color.arrayLastSlice != 0 ? CachedStorageLayers(context, ArraySurfaceForTarget(color), color.arraySlice, color.layers) : CachedStorageSurface(context, SurfaceForTarget(color), targetProof); };
        binding.resident = withTargetLookup(lookup, [&](const TargetLookup& target) { return refreshResidentTarget(context, state, color, draw, shaders, outcome, profile, target, nullptr); });
        if (binding.resident != nullptr) targetViews[index] = binding.resident->AttachmentView(color.format, color.mip, color.depthSlice, color.layers, color.arraySlice);
    }
    if (!stencilReferences.empty() && resources->ChecksFaults()) recordable = false;
    if (!stencilReferences.empty()) {
        const auto readImages = resources->StorageImages();
        const auto inputOverlaps = [&](std::uint64_t address, std::uint64_t bytes) {
            return std::ranges::any_of(inputs.inPlaceReads, [&](const auto& range) { return address < range.second && range.first < address + bytes; });
        };
        for (const auto& target : targets) {
            const bool imageRead = target.resident != nullptr && std::ranges::any_of(readImages, [&](const auto& image) { return image.first == target.resident->Image(); });
            Require(!imageRead && !resources->ReadsImage(target.resident.get()) && !resources->ReadsOverlap(target.color.MemoryAddress(), static_cast<std::size_t>(target.color.bytes)) && !inputOverlaps(target.color.MemoryAddress(), target.color.bytes), "stencil replay refuses attachment feedback reads");
        }
        if (state.depth) {
            const auto bytes = static_cast<std::size_t>(state.depth->extent.width) * state.depth->extent.height * 4u;
            Require(!resources->ReadsOverlap(state.depth->address, bytes) && !resources->ReadsOverlap(state.depth->stencilAddress, bytes) && !inputOverlaps(state.depth->address, bytes) && !inputOverlaps(state.depth->stencilAddress, bytes), "stencil replay refuses depth or stencil feedback reads");
            for (const auto& shader : shaders) {
                for (const auto& binding : shader.program->bindings) {
                    if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages && binding.role != ShaderRecompiler::DescriptorRole::ImageTable) continue;
                    for (std::size_t offset = 0; offset + 8u <= binding.guestDescriptor.size(); offset += 8u) {
                        const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(offset, 8u);
                        if (words[0] == 0u && (words[1] & 0xffu) == 0u) continue;
                        const auto texture = DecodeTextureResource(words);
                        Require(!DepthSurfaceAt(context, words, texture), "stencil replay refuses sampled depth-surface feedback");
                    }
                }
            }
        }
    }
    const auto& contentKey = resolved.contentKey;
    const bool cacheable = resolved.cacheable;
    built = resolved.built;
    // The memory-state decision of an indirect draw, made here after the resource build (which may
    // retire imports and wait for recorded work) and before anything is recorded, exactly as
    // VulkanDevice::dispatch decides for DISPATCH_INDIRECT: the GPU reads the records in place from
    // the host import unless storage-image results are pending over them (flushed, then synced), a
    // label or a copied buffer's write-back still has to land on them, or they are not imported; in
    // those cases (and with APS5_NO_GPU_INDIRECT_DRAW=1) the CPU reads them through the flush hook
    // and records plain draws with the same per-dimension rules. The import pointer stays valid:
    // refreshImports runs only under GuestMemory::GpuMutex, held here, and a retired import's
    // buffer is kept while the recorder is busy.
    auto& indirect = prepared.indirect;
    indirect.args = args;
    auto& records = prepared.records;
    lookupDiag::restart();
    if (args != nullptr) {
        static const bool gpuIndirectDraws = std::getenv("APS5_NO_GPU_INDIRECT_DRAW") == nullptr;
        const auto decide = [&](std::uint64_t address, std::size_t bytes, const HostImport*& import) {
            if (StorageTexture::FlushPending(address, bytes, nullptr, "indirect draw arguments")) {
                if (recorder != nullptr) {
                    Recorder::CountSync(2);
                    recorder->Sync();
                }
                return IndirectDrawPath::PendingImage;
            }
            if (recorder != nullptr && recorder->PendingLabelIn(address, bytes)) return IndirectDrawPath::PendingLabelOrCopy;
            const auto overlaps = [&](const auto& writer) { return writer->CopiedWritesOverlap(address, bytes); };
            if (context.copiedWriters != nullptr && std::any_of(context.copiedWriters->begin(), context.copiedWriters->end(), overlaps)) return IndirectDrawPath::PendingLabelOrCopy;
            if (std::any_of(DrawCopiedWriters()->begin(), DrawCopiedWriters()->end(), overlaps)) return IndirectDrawPath::PendingLabelOrCopy;
            if ((import = HostImportFor(context, address, bytes)) == nullptr) return IndirectDrawPath::NotImported;
            return IndirectDrawPath::Gpu;
        };
        const auto rangeBytes = args->RangeBytes();
        if (rangeBytes == 0) {
            report(" indirect draw without records");
            return false;
        }
        Require(rangeBytes <= std::numeric_limits<std::size_t>::max(), "indirect draw record range overflow");
        if (!gpuIndirectDraws) indirect.path = IndirectDrawPath::Disabled;
        else if (inputs.listRestartPrimitive != 0) indirect.path = IndirectDrawPath::FeatureGap;
        else indirect.path = decide(args->arguments, static_cast<std::size_t>(rangeBytes), indirect.argumentImport);
        if (indirect.path == IndirectDrawPath::Gpu && args->countIndirect) {
            Require(context.drawIndirectCount, "indirect draw count without VK_KHR_draw_indirect_count must be resolved by the driver");
            indirect.path = decide(args->countAddress, 4, indirect.countImport);
        }
        if (indirect.path != IndirectDrawPath::Gpu) {
            // The reads wait for the producing batch through the flush hook, as the driver's
            // resolve did for every indirect dispatch before its GPU path.
            const auto readStart = std::chrono::steady_clock::now();
            const auto count = std::min(args->countIndirect ? Pm4::ReadDrawCount(*args) : args->count, args->count);
            records.reserve(count);
            for (std::uint32_t record = 0; record < count; ++record) records.push_back(Pm4::ReadDrawArguments(*args, record));
            indirect.readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count();
            if (inputs.listRestartPrimitive != 0) CompactIndirectListRestart(context, draw, inputs, records);
            indirect.records = records;
            if (std::none_of(records.begin(), records.end(), [](const Pm4::DrawArguments& record) { return record.count != 0 && record.instances != 0; })) {
                CountIndirectDraw(indirect.path, indirect.readMs);
                report(" indirect draw with empty records");
                return false;
            }
        }
    }
    lookupDiag::mark(lookupDiag::StepDecide);
    // A draw whose only completion work is a BDA fault check (the rect-list stages carry a fault
    // buffer, so every rect-list draw needed one) is recorded too, with the check as a completion
    // action like a dispatch's write-back: its own batch would first wait for everything recorded
    // before it. A draw that writes a copied buffer (at the menu stage: 0x48-byte constant buffers
    // inside an import but not at its storage offset alignment, so GuestBufferMemory copies them
    // and every guest buffer counts as writable) is recorded and then waited for at once: the
    // write-back runs before the next packet, without the separate command batch and fence a
    // synchronous draw takes. The copied write-back is listed in DrawCopiedWriters, which lets such
    // draws run without the wait; both consumers of the device's list in VulkanDevice.cpp
    // (DispatchIndirect's argument check and the fill HLE's range check, see Draw.hpp) consult this
    // list too. A draw holding an address-based build's lease (which pins guest allocations until
    // the write-back releases it) runs free like the dispatch path's: the completion releases the
    // lease, and a guest thread that needs a leased allocation syncs the recorder through the
    // registry's pin waiter (see SyncLeaseWork); APS5_SYNC_LEASE_DISPATCH=1 waits for such draws
    // at once as before. Debug aids:
    // APS5_SYNC_COMPLETION_DRAWS=1 keeps every draw with completion work synchronous;
    // APS5_NO_RECORDER_SYNC_DRAWS=1 gives the waited-for draws their own batch as before;
    // APS5_NO_RECORD_COPIED_DRAWS=1 waits for copied-write draws at once instead of listing them.
    static const bool syncCompletionDraws = std::getenv("APS5_SYNC_COMPLETION_DRAWS") != nullptr;
    static const bool recorderSyncDraws = std::getenv("APS5_NO_RECORDER_SYNC_DRAWS") == nullptr;
    static const bool recordCopiedDraws = std::getenv("APS5_NO_RECORD_COPIED_DRAWS") == nullptr;
    const bool completion = prepared.completion = resources->NeedsCompletion();
    const bool copiedWrites = completion && resources->HasCopiedWrites();
    // A lease only forces the wait when deferred release is off.
    const bool lease = resources->HoldsLease() && SyncLeaseWork();
    outcome.completion = completion;
    outcome.recorded = recordable;
    // Whether the copied write-back is listed in DrawCopiedWriters instead of waited for.
    auto& listed = prepared.listed;
    if (!recordable) {
        outcome.reason = !recordDraws || dumpLimit != 0 ? SyncDisabled : recorder == nullptr ? SyncNoRecorder : SyncNotResident;
    } else if (completion && (syncCompletionDraws || copiedWrites || lease)) {
        outcome.reason = syncCompletionDraws ? SyncDisabled : lease ? SyncLease : SyncCopiedWrites;
        if (syncCompletionDraws) outcome.recorded = false;
        else if (copiedWrites && !lease && recordCopiedDraws) listed = true;
        else if (recorderSyncDraws) outcome.waited = true;
        else outcome.recorded = false;
    }
    const bool recorded = prepared.recorded = outcome.recorded;
    // A build this recorded draw can share with later identical ones goes into the cache (a cache
    // hit is reusable by construction, so `recorded` holds for it; one with completion work is
    // never reusable).
    if (cacheable && built != nullptr && recorded && resources->Reusable()) SharedResourceCache().Insert(contentKey, resources);
    lookupDiag::mark(lookupDiag::StepCompletion);
    // A recorded draw renders into its resident targets in the general layout (recordDraw). Debug
    // aid: APS5_DRAW_TRANSITIONS=1 keeps the per-draw upload and download barriers, the layout
    // transitions of every target and one pass per draw, as before.
    static const bool drawTransitions = std::getenv("APS5_DRAW_TRANSITIONS") != nullptr;
    const bool lean = prepared.lean = recorded && !drawTransitions;
    const bool multisampled = prepared.multisampled = std::any_of(targets.begin(), targets.end(), [](const TargetBinding& binding) { return binding.multisampled; });
    Require(lean || !multisampled, "multisampled draws are only rendered as recorded draws");
    for (const auto& binding : targets) {
        if (!binding.multisampled) continue;
        const auto& sources = resources->MultisampleSources();
        const auto reads = std::count(sources.begin(), sources.end(), binding.color.surfaceAddress);
        Require(reads == 0, "sampling a multisampled color target the draw renders to is not implemented");
    }
    for (const auto& binding : targets) {
        if (binding.color.partialWriteMask == 0 || binding.multisampled) continue;
        Require(lean && binding.resident != nullptr && !binding.proxied, "a partial write of a packed color target is merged into its resident image by a recorded draw only");
        Require(storesIntoResident(*resources, *binding.resident), "the partial write of a packed color target does not store into the target's resident image");
    }
    if (!lean && (!resolved.moved.empty() || resolved.resources->HasStagedCopies())) {
        if (resolved.resources->SpaceReuse()) static_cast<void>(resolved.resources->ReleaseSpace());
        resolved.resources = std::make_shared<ShaderResources>(context, shaders, state.color, static_cast<std::uint32_t>(state.colors.size()), draw.indexAddress, static_cast<std::size_t>(indexBytes), snapshots);
        resolved.moved.clear();
    }
    lookupDiag::mark(lookupDiag::StepRest);
    if (state.depth) RequireNoNativeDepthFeedback_nid_no_patch(context, *state.depth, resources->SampledTextures());
    lookupDiag::mark(lookupDiag::StepDepthFeedback);
    APS5_LOG_CHARS_OUT_DEBUG("Creating Pipeline");
    // The state is copied only when a mask must change.
    auto& masked = prepared.masked;
    if (std::any_of(targets.begin(), targets.end(), [](const TargetBinding& binding) { return binding.proxied; })) recipeOut = nullptr;
    const State& pipelineState = pipelineStateFor(context, state, inputs.fragmentOutputs, proxiedMask(targets), masked);
    lookupDiag::mark(lookupDiag::StepMasked);
    static const bool verifySetLayouts = std::getenv("APS5_VERIFY_SET_LAYOUTS") != nullptr;
    if (verifySetLayouts && context.descriptorCache != nullptr) {
        const auto described = DescribeSetLayouts(context, shaders);
        const auto built = resources->SetLayouts();
        Require(described.layouts.main == built.main && described.layouts.fragment == built.fragment && described.layouts.split == built.split && std::ranges::equal(described.mainKey, built.mainKey) && std::ranges::equal(described.fragmentKey, built.fragmentKey), "set layouts described from the shaders differ from the draw's");
    }
    auto& stencilReplay = prepared.stencilReplay;
    for (const auto reference : stencilReferences) {
        auto selected = pipelineState;
        selected.stencilReplayReference = reference;
        selected.stencilFront.reference = reference;
        selected.stencilBack.reference = reference;
        auto selectedPipeline = stencilReplay.empty() ? CachedPipeline(context, selected, inputs.VertexLayout(), *resources, shaders, lean ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) : stencilReplay.front().second;
        Require(!selectedPipeline->SplitsFaces() && !selectedPipeline->SplitsSecondChannel(), "stencil replay refuses pipeline face or channel splits");
        stencilReplay.emplace_back(reference, std::move(selectedPipeline));
    }
    auto& pipeline = prepared.pipeline;
    lookupDiag::mark(lookupDiag::StepRest);
    const auto* speculation = prepared.preparation != nullptr && prepared.preparation->device == context.device && prepared.preparation->speculatedPipeline != nullptr ? prepared.preparation : nullptr;
    if (speculation != nullptr && stencilReplay.empty() && lean && speculation->resolvedTemplate == resources && speculation->speculatedOutputs == inputs.fragmentOutputs && speculation->speculatedProxied == proxiedMask(targets)) {
        pipeline = speculation->speculatedPipeline;
        resolvePipelineCounters().hits.fetch_add(1, std::memory_order_relaxed);
        if (resolvePipelineVerify()) {
            resolvePipelineCounters().verified.fetch_add(1, std::memory_order_relaxed);
            if (CachedPipeline(context, pipelineState, inputs.VertexLayout(), *resources, shaders, VK_IMAGE_LAYOUT_GENERAL) != pipeline) resolvePipelineCounters().mismatches.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        if (speculation != nullptr) resolvePipelineCounters().misses.fetch_add(1, std::memory_order_relaxed);
        if (!stencilReplay.empty()) {
            pipeline = stencilReplay.front().second;
        } else {
            const auto attachmentLayout = lean ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            const bool memoized = stencilReferences.empty() && std::none_of(targets.begin(), targets.end(), [](const TargetBinding& binding) { return binding.proxied || binding.multisampled; });
            const auto layouts = resources->SetLayouts();
            std::uint64_t storeHash = 0;
            if (memoized) pipeline = shapePipeline(context, prepared.shape, inputs.VertexLayout(), layouts, shaders, attachmentLayout, inputs.fragmentOutputs);
            if (pipeline != nullptr && VerifyShapeMemo()) {
                Require(CachedPipeline(context, pipelineState, inputs.VertexLayout(), layouts, shaders, attachmentLayout) == pipeline, "APS5_VERIFY_SHAPE_MEMO: the shape memo's pipeline differs from the pipeline store's");
                noteShapeMemoVerified(true);
            }
            const bool repeated = pipeline != nullptr;
            if (!repeated) pipeline = CachedPipeline(context, pipelineState, inputs.VertexLayout(), layouts, shaders, attachmentLayout, nullptr, &storeHash);
            if (memoized) rememberShape(context, prepared.shape, targets, repeated ? nullptr : &pipeline, storeHash, inputs.VertexLayout(), layouts, shaders, attachmentLayout, inputs.fragmentOutputs);
        }
    }
    lookupDiag::mark(lookupDiag::StepPipeline);
    // Resident targets' views are stable while their storage image lives, so the framebuffer is
    // reused with the pipeline; a per-draw RenderTarget gets a framebuffer of its own.
    auto& owners = prepared.owners;
    owners.reserve(targets.size());
    auto& framebufferOwners = prepared.framebufferOwners;
    framebufferOwners.reserve(targets.size());
    for (const auto& binding : targets) {
        owners.push_back(binding.resident);
        framebufferOwners.push_back(binding.multisampled ? binding.multisampleOwner : binding.resident);
    }
    Require(!pipeline->ReordersSamples() || std::ranges::none_of(state.colors, [](const ColorTarget& color) { return color.staged; }), "a partial write of a multisampled packed color target on a device that puts the guest's samples in order is not implemented");
    auto& framebuffer = prepared.framebuffer;
    framebuffer = pipeline->AcquireFramebuffer(targetViews, framebufferOwners, state.renderExtent, state.renderLayers);
    lookupDiag::mark(lookupDiag::StepFramebuffer);
    auto& reordered = prepared.reordered;
    auto& reorderPass = prepared.reorderPass;
    if (pipeline->ReordersSamples()) {
        reorderPass = SampleOrderPassFor(context, pipelineState, pipeline->HostSampleOrder());
        for (const auto& step : PlanSampleOrder(context, state, pipeline->HostSampleOrder())) {
            if (step.attachment < targets.size()) reordered.push_back(MultisampleTargetOrderTarget(context, targets[step.attachment].color));
            else reordered.push_back(DepthSurfaceOrderTarget(context, *state.depth));
            Require(reordered.back().method == step.method, "a reordered attachment's surface disagrees with the planned way of putting its samples in order");
        }
    }
    prepared.depthLocations = passDepthLocations(context, pipelineState, *pipeline);
    lookupDiag::mark(lookupDiag::StepTail);
    lookupDiag::finish(outcome.kind == KindTemplateHit);
    timer.phase(PhasePipeline);
    if (Stage4::ResolveCensus()) Stage4::NoteTemplate(resolved.resources.get(), outcome.kind == KindBuild || outcome.kind == KindBda);
    APS5_LOG_CHARS_OUT_DEBUG("Pipeline created");
    if (VerifyDrawPlan()) requireDrawPlanRepeats(context, state, draw, shaders, plan);
    return true;
}

void recordPreparedDraw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, PreparedDraw& prepared) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    static const bool traceDraws = std::getenv("APS5_TRACE_DRAWS") != nullptr;
    static AgcDriver::Mutex dumpMutex;
    static std::map<std::uint64_t, int> dumped;
    auto& timing = *prepared.timing;
    timing.Mark("prepare_complete");
    timing.RecorderPhase(AgcDriver::ArchB::Recorder::Phase::Record);
    auto& timer = prepared.timer;
    auto& outcome = prepared.outcome;
    const auto* built = prepared.built;
    const auto waitedBefore = prepared.waitedBefore;
    auto& ownWaitedMs = prepared.ownWaitedMs;
    const auto report = [&](const char* suffix) { reportDrawEnd(state, timer, built, outcome, waitedBefore, ownWaitedMs, suffix); };
    auto* recipeOut = prepared.recipeOut;
    const auto& stencilReferences = prepared.stencilReferences;
    auto& inputs = prepared.inputs;
    const auto* args = prepared.args;
    const int dumpLimit = prepared.dumpLimit;
    auto& targets = prepared.targets;
    const auto& targetViews = prepared.targetViews;
    auto* recorder = prepared.recorder;
    auto& resolved = prepared.resolved;
    auto& resources = resolved.resources;
    const auto& contentKey = resolved.contentKey;
    const bool cacheable = resolved.cacheable;
    auto& indirect = prepared.indirect;
    const bool completion = prepared.completion;
    const bool listed = prepared.listed;
    const bool recorded = prepared.recorded;
    const bool lean = prepared.lean;
    const bool multisampled = prepared.multisampled;
    const auto& masked = prepared.masked;
    const auto& stencilReplay = prepared.stencilReplay;
    const auto& pipeline = prepared.pipeline;
    const auto& owners = prepared.owners;
    const auto& framebuffer = prepared.framebuffer;
    const auto& reordered = prepared.reordered;
    const auto& reorderPass = prepared.reorderPass;
    const auto& depthLocations = prepared.depthLocations;
    if (lean) {
        RecordedDraw record;
        record.recorder = recorder;
        record.resources = resources;
        record.moved = resolved.moved;
        record.pipeline = pipeline;
        record.framebuffer = framebuffer;
        record.targetViews = targetViews;
        record.targets = owners;
        for (const auto& binding : targets) record.targetSlices.emplace_back(binding.color.arraySlice, binding.color.arrayLastSlice != 0 ? binding.color.layers : 1u);
        for (const auto& binding : targets) {
            if (binding.proxied) record.proxies.push_back(binding.resident);
            if (binding.multisampled) {
                record.multisampleImages.push_back(binding.multisampleImage);
                record.multisampleTargets.push_back(binding.multisampleOwner);
            }
            if (binding.stagedTemporary != nullptr) {
                record.multisampleImages.push_back(binding.stagedTemporary->Image());
                const auto& color = binding.color;
                const auto bounds = VkRect2D{{0, 0}, color.extent};
                auto area = state.scissor;
                area.offset.x = std::max(area.offset.x, bounds.offset.x);
                area.offset.y = std::max(area.offset.y, bounds.offset.y);
                const auto right = std::min<std::int64_t>(static_cast<std::int64_t>(state.scissor.offset.x) + state.scissor.extent.width, color.extent.width);
                const auto bottom = std::min<std::int64_t>(static_cast<std::int64_t>(state.scissor.offset.y) + state.scissor.extent.height, color.extent.height);
                area.extent = {static_cast<std::uint32_t>(std::max<std::int64_t>(right - area.offset.x, 0)), static_cast<std::uint32_t>(std::max<std::int64_t>(bottom - area.offset.y, 0))};
                record.staged.push_back({binding.stagedTemporary, binding.stagedTarget, color.stagedMask, color.samples, area});
            }
        }
        record.indirect = args != nullptr ? &indirect : nullptr;
        record.listed = listed;
        record.completion = completion;
        record.waited = outcome.waited;
        record.stencilReplay = stencilReplay;
        record.reordered = reordered;
        record.reorderPass = reorderPass;
        record.depthLocations = depthLocations;
        record.spaceKeep = &prepared.spaceKeep;
        const bool slotCapture = archcSlots::wantsCapture(prepared);
        SlotPassCapture passCapture;
        const bool inputsInPlace = inputs.vertexBuffers.empty() && inputs.indices == nullptr;
        std::optional<std::array<std::uint64_t, archcSlots::ComponentCount>> predicted;
        if (slotCapture && prepared.slotVerify) {
            if (auto* slot = archcSlots::lookup(context.device, state, draw, shaders, *prepared.identity, prepared.pipelineStateReuse)) predicted = archcSlots::fingerprint(*slot);
        }
        record.slotCapture = slotCapture ? &passCapture : nullptr;
        archcSlots::verifyDirectInputs(prepared, inputs);
        recordDraw(context, state, draw, shaders, inputs, record, outcome, timer, ownWaitedMs, prepared.trace, prepared.pipelineStateReuse);
        if (slotCapture) archcSlots::capture(context, state, draw, shaders, prepared, passCapture, inputsInPlace, predicted);
        // The recipe for the caller's draw-cache entry (design_cpu_final M8, rule R3 for draws):
        // only a template the resource cache serves under this content key (reusable: no lease,
        // no copied writes, every direct region import- or mirror-served) of a direct draw, so a
        // hit's proof is the template's ProveCurrent and nothing needs completion work.
        const auto recipeBuild = recipeOut == nullptr ? RecipeNotRequested : !DrawRecipes() ? RecipeDisabled : !cacheable ? RecipeUncacheable : outcome.waited ? RecipeWaited : args != nullptr ? RecipeIndirect : !resources->Reusable() || resources->SpaceReuse() ? RecipeNotReusable : !resolved.moved.empty() && !DrawMovedRecipes() ? RecipeMovedDisabled : multisampled ? RecipeMultisampled : pipeline->ReordersSamples() ? RecipeReordered : RecipeBuilt;
        if (profile) outcome.recipeBuild = recipeBuild;
        archcLastRecipeBuild() = static_cast<int>(recipeBuild);
        if (recipeBuild == RecipeBuilt) {
            auto recipe = std::make_shared<DrawRecipe>();
            recipe->device = context.device;
            recipe->templateRef = resources;
            recipe->key = contentKey;
            recipe->pipeline = pipeline;
            recipe->framebuffer = framebuffer;
            for (const auto& owner : owners) recipe->targets.emplace_back(owner);
            recipe->targetViews = targetViews;
            recipe->passKey = DrawRenderPassKey(context, state, targetViews);
            recipe->vertexInput = inputs.VertexLayout();
            recipe->pushStages = PushConstantStages(shaders);
            if (recipe->pushStages != 0) {
                recipe->pushBytes = AssemblePushConstants(shaders);
                resources->PatchPushConstants(recipe->pushBytes);
            }
            recipe->masked = masked;
            recipe->fragmentOutputs = inputs.fragmentOutputs;
            recipe->shaderStages = inputs.shaderStages;
            recipe->hasDepth = state.depth.has_value();
            *recipeOut = std::move(recipe);
            if (profile) outcome.movedRecipeBuilt = !resolved.moved.empty();
        }
        timing.Mark("draw_and_resource_release");
        report(outcome.waited ? (outcome.reason == SyncLease ? " recorded then waited (lease)" : " recorded then waited (copied writes)") : completion ? " recorded with completion" : " recorded");
        return;
    }
    const bool gpuIndirect = args != nullptr && indirect.path == IndirectDrawPath::Gpu;
    const auto touches = [&](std::uint64_t begin, std::uint64_t end) {
        const auto bytes = static_cast<std::size_t>(end - begin);
        if (resources->WritesOverlap(begin, bytes) || resources->ReadsOverlap(begin, bytes)) return true;
        if (!gpuIndirect) return false;
        return (begin < args->arguments + args->RangeBytes() && args->arguments < end) || (args->countIndirect && begin < args->countAddress + 4 && args->countAddress < end);
    };
    static_cast<void>(StorageTexture::StoreDrawnLinearTouching("draw access", [&](std::uint64_t begin, std::uint64_t end) { return touches(begin, end) || std::any_of(inputs.inPlaceReads.begin(), inputs.inPlaceReads.end(), [&](const auto& read) { return read.first < end && begin < read.second; }); }, resources->HoldsLease() || resources->UsesAddressTable() || resources->HasCopiedWrites()));
    if (UsesMeshWindow(shaders)) PrepareMeshWindow(context);
    APS5_LOG_CHARS_OUT_DEBUG("Creating CommandBatch");
    std::optional<CommandBatch> batch;
    if (!recorded) {
        // The color-target detiles below take their descriptor sets from a fresh batch.
        if (context.detiler != nullptr) context.detiler->BeginBatch();
        batch.emplace(context);
    }
    using CommandClass = Recorder::CommandClass;
    const auto countBarrier = [&](std::uint32_t count = 1) { if (recorded) Recorder::CountBarriers(CommandClass::Draw, count); };
    // A queued DCC key store over memory the draw writes or reads in place (unknown for an
    // address-based build), or over its GPU-side records, lands before it, as before a dispatch
    // (VulkanDevice::dispatch).
    if (recorded && recorder->HasQueuedKeyStores() && (resources->HoldsLease() || recorder->AnyQueuedKeyStore(touches))) recorder->FlushKeyStores();
    // A queued label store over such memory likewise (Recorder::RecordStore).
    if (recorded && recorder->HasQueuedStores() && (resources->HoldsLease() || recorder->AnyQueuedStore(touches))) recorder->FlushStores();
    if (prepared.pipelineStateReuse) AgcDriver::ArchB::Recorder::Count("fix_pso_reuse_refused_untracked_draw");
    if (context.recorder != nullptr) context.recorder->InvalidatePipelineState("fix_pso_reuse_invalidations_untracked_writer");
    const auto commands = recorded ? recorder->Commands() : batch->Handle();
    if (recorded) prepared.spaceKeep = reserveSpaceKeep(*recorder, *resources);
    if (recorded) recorder->PrepareSampleSlot();
    APS5_LOG_CHARS_OUT_DEBUG("CommandBatch created");
    // The draw's [gputime] class range: from its first barrier to the download barrier.
    const auto drawTiming = recorded ? recorder->BeginGpuTiming(CommandClass::Draw) : Recorder::NoTiming;
    if (recorded && Recorder::BarrierValidate()) {
        // APS5_ARCHB_FIX_ALLOC_CHURN: a per-thread scratch (NoteAccess reads it before returning).
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ownedReads;
        struct ReadsSlotThreadStorage {};
        auto& readsSlot = AgcDriver::ThreadScratch<std::vector<std::pair<std::uint64_t, std::uint64_t>>*, ReadsSlotThreadStorage>();
        auto& reads = AgcDriver::ArchB::AllocChurnActive() ? ShaderRecompiler::ThreadOwned(readsSlot) : ownedReads;
        resources->InPlaceReads(reads);
        reads.insert(reads.end(), inputs.inPlaceReads.begin(), inputs.inPlaceReads.end());
        if (gpuIndirect) {
            reads.emplace_back(args->arguments, args->arguments + args->RangeBytes());
            if (args->countIndirect) reads.emplace_back(args->countAddress, args->countAddress + 4);
        }
        auto images = resources->StorageImages();
        for (const auto& binding : targets) {
            if (binding.resident != nullptr) images.emplace_back(binding.resident->Image(), true);
        }
        recorder->NoteAccess(CommandClass::Draw, Recorder::Access{reads, resources->GpuWrites(), images, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | inputs.shaderStages, resources->HoldsLease()});
    }
    std::unique_ptr<DeviceBuffer> scratch;
    VkBuffer argumentBuffer = VK_NULL_HANDLE;
    VkDeviceSize argumentOffset = 0;
    bool rewritten = false;
    VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    upload.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    upload.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | inputs.shaderStages, 0, 1, &upload, 0, nullptr, 0, nullptr);
    countBarrier();
    if (state.depth) {
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        countBarrier();
    }
    APS5_LOG_CHARS_OUT_DEBUG("Upload barrier recorded");
    if (inputs.vertexInputBehindGpuWrite) {
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
        countBarrier();
    }
    const auto pixelLdsSubgroup = PixelFunctionLdsSubgroup(shaders);
    if (pixelLdsSubgroup != 0u) PreparePixelFunctionLds(context, commands, pixelLdsSubgroup);
    std::shared_ptr<Buffer> meshArguments;
    if (state.stages.mesh && state.tessellationCapture != nullptr) {
        recordCaptureBarrier(context, commands);
        countBarrier();
        meshArguments = state.tessellationCapture->header;
        argumentBuffer = meshArguments->Handle();
    } else if (state.stages.mesh && args != nullptr) {
        meshArguments = recordMeshArguments(context, commands, recorder, recorded, state, draw, indirect, countBarrier, inputs.restartTable);
        argumentBuffer = meshArguments->Handle();
    } else if (!inputs.restartTable.empty()) {
        meshArguments = directMeshArguments(context, draw, inputs.restartTable);
    } else if (gpuIndirect) {
        rewritten = recordIndirectArguments(context, commands, recorder, recorded, indirect, scratch, argumentBuffer, argumentOffset, countBarrier);
    }
    for (auto& binding : targets) {
        if (binding.proxied) {
            binding.resident->RecordAttachmentProxyLoad(commands, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            countBarrier(2);
            continue;
        }
        if (binding.resident != nullptr) {
            imageBarrier(context, commands, binding.resident->Image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, {VK_IMAGE_ASPECT_COLOR_BIT, binding.color.mip, 1, binding.resident->ImageDepth() == 1 ? binding.color.arraySlice : 0, binding.resident->ImageDepth() == 1 ? binding.color.layers : 1});
            countBarrier();
            continue;
        }
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {binding.color.extent.width, binding.color.extent.height, 1};
        if (binding.banded) {
            copy.bufferRowLength = binding.bandPitch;
            copy.imageOffset = {binding.band.offset.x, binding.band.offset.y, 0};
            copy.imageExtent = {binding.band.extent.width, binding.band.extent.height, 1};
        }
        const VkBuffer linear = binding.gpuTiling ? binding.linearDevice->Handle() : binding.transfer->Handle();
        countBarrier(binding.gpuTiling ? 4 : 2);
        if (binding.gpuTiling) {
            CopyBuffer(context, commands, binding.tiled->Handle(), 0, binding.tiledDevice->Handle(), 0, binding.original.size());
            memoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            context.detiler->Dispatch(commands, TextureTileMode::kR64KBX, binding.color.elementBytes, binding.tiledDevice->Handle(), 0, binding.linearDevice->Handle(), 0, binding.mip, false, 0, false, {.pipeBankXor = binding.color.pipeBankXor});
            memoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        }
        imageBarrier(context, commands, binding.target->Image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        context.Resolved(&DeviceFunctions::cmdCopyBufferToImage, "vkCmdCopyBufferToImage")(commands, linear, binding.target->Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        imageBarrier(context, commands, binding.target->Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    std::vector<std::shared_ptr<void>> reorderKept;
    RecordSampleOrder(context, commands, reorderPass, reordered, true, reorderKept);
    APS5_LOG_OUT_DEBUG("Beginning pipeline renderExtent=%ux%u", state.renderExtent.width, state.renderExtent.height);
    pipeline->Begin(commands, *framebuffer, state.renderExtent, state);
    APS5_LOG_CHARS_OUT_DEBUG("Pipeline Begin OK");
    const auto drawUse = recorded ? resources->BeginDrawUse(*recorder) : nullptr;
    resources->BindDrawUse(commands, pipeline->Layout(), drawUse.get());
    if (pixelLdsSubgroup != 0u) BindPixelFunctionLds(context, commands, pipeline->Layout());
    if (UsesMeshWindow(shaders)) BindMeshWindow(context, commands, pipeline->Layout());
    APS5_LOG_CHARS_OUT_DEBUG("Resources bound");
    pushDrawConstants(*pipeline, commands, state, draw, shaders, *resources, nullptr, 0, meshArguments != nullptr ? meshArguments->DeviceAddress() : 0, !inputs.restartTable.empty(), inputs.meshLayerStride);
    APS5_LOG_CHARS_OUT_DEBUG("Push constants recorded");
    std::uint32_t subdraw = 0;
    if (recorded) noteDrawFaultToken(context, *recorder, commands, shaders, draw, static_cast<std::uint32_t>(state.stages.mesh.has_value() ? AgcDriver::FaultStage::mesh : AgcDriver::FaultStage::graphics), args);
    if (stencilReplay.empty()) recordDrawCommands(context, commands, state, draw, inputs, args != nullptr ? &indirect : nullptr, argumentBuffer, argumentOffset, prepared.trace, subdraw);
    else {
        recordStencilReplay(context, commands, state, draw, inputs, stencilReplay, shaders, *resources);
        if (recorded) for (const auto& [reference, selectedPipeline] : stencilReplay) recorder->Keep(selectedPipeline);
    }
    if (pipeline->SplitsFaces()) {
        pipeline->ContinueBackFaces(commands, state);
        recordDrawCommands(context, commands, state, draw, inputs, args != nullptr ? &indirect : nullptr, argumentBuffer, argumentOffset, prepared.trace, subdraw);
    }
    if (pipeline->SplitsSecondChannel()) {
        pipeline->ContinueSecondChannel(commands);
        recordDrawCommands(context, commands, state, draw, inputs, args != nullptr ? &indirect : nullptr, argumentBuffer, argumentOffset, prepared.trace, subdraw);
    }
    if (meshArguments != nullptr && recorded) recorder->Keep(meshArguments);
    if (state.tessellationCapture != nullptr && recorded) {
        recorder->Keep(state.tessellationCapture->header);
        recorder->Keep(state.tessellationCapture->items);
    }
    if (args != nullptr) CountIndirectDraw(indirect.path, indirect.readMs, rewritten);
    auto checkRecords = indirectRecordCheck(args != nullptr ? &indirect : nullptr);
    APS5_LOG_CHARS_OUT_DEBUG("Draw recorded");
    if (recorded) recorder->EndPassSamples();
    EndRenderPass(context, commands);
    RecordSampleOrder(context, commands, reorderPass, reordered, false, reorderKept);
    if (depthLocations) {
        RecordDepthSampleLocations(context, commands, *depthLocations);
        countBarrier(2);
        reorderKept.push_back(depthLocations->keep);
    }
    if (recorded) {
        for (auto& object : reorderKept) recorder->Keep(std::move(object));
    }
    APS5_LOG_CHARS_OUT_DEBUG("Render pass ended");
    for (auto& binding : targets) {
        if (binding.proxied) {
            binding.resident->RecordAttachmentProxyStore(commands, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            countBarrier(2);
            continue;
        }
        if (binding.resident != nullptr) {
            imageBarrier(context, commands, binding.resident->Image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, {VK_IMAGE_ASPECT_COLOR_BIT, binding.color.mip, 1, binding.resident->ImageDepth() == 1 ? binding.color.arraySlice : 0, binding.resident->ImageDepth() == 1 ? binding.color.layers : 1});
            countBarrier();
            continue;
        }
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {binding.color.extent.width, binding.color.extent.height, 1};
        if (binding.banded) {
            copy.bufferRowLength = binding.bandPitch;
            copy.imageOffset = {binding.band.offset.x, binding.band.offset.y, 0};
            copy.imageExtent = {binding.band.extent.width, binding.band.extent.height, 1};
        }
        countBarrier(binding.gpuTiling ? 4 : 2);
        imageBarrier(context, commands, binding.target->Image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBuffer linear = binding.gpuTiling ? binding.linearDevice->Handle() : binding.transfer->Handle();
        VkBufferMemoryBarrier reuse{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        reuse.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        reuse.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        reuse.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        reuse.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        reuse.buffer = linear;
        reuse.size = VK_WHOLE_SIZE;
        context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &reuse, 0, nullptr);
        context.Resolved(&DeviceFunctions::cmdCopyImageToBuffer, "vkCmdCopyImageToBuffer")(commands, binding.target->Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, linear, 1, &copy);
        if (binding.gpuTiling) {
            memoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
            if (dumpLimit > 0) {
                std::lock_guard lock(dumpMutex);
                if (dumped[binding.color.address] < dumpLimit) {
                    binding.dump = std::make_unique<Buffer>(context, binding.linearDevice->Size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                    CopyBuffer(context, commands, binding.linearDevice->Handle(), 0, binding.dump->Handle(), 0, binding.linearDevice->Size());
                }
            }
            context.detiler->Dispatch(commands, TextureTileMode::kR64KBX, binding.color.elementBytes, binding.linearDevice->Handle(), 0, binding.tiledDevice->Handle(), 0, binding.mip, true, 0, false, {.pipeBankXor = binding.color.pipeBankXor});
            memoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            CopyBuffer(context, commands, binding.tiledDevice->Handle(), 0, binding.tiled->Handle(), 0, binding.original.size());
        }
    }
    VkMemoryBarrier download{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    download.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    download.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT | inputs.shaderStages, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &download, 0, nullptr, 0, nullptr);
    countBarrier();
    if (recorded) recorder->EndGpuTiming(drawTiming);
    APS5_LOG_CHARS_OUT_DEBUG("Download barrier recorded");
    APS5_LOG_CHARS_OUT_DEBUG("SubmitAndWait");
    timer.phase(PhaseRecord);
    if (recorded) {
        keepRecordedDraw(*recorder, resources, pipeline, framebuffer, inputs, owners, [&] { std::vector<std::pair<std::uint32_t, std::uint32_t>> slices; for (const auto& binding : targets) slices.emplace_back(binding.color.arraySlice, binding.color.arrayLastSlice != 0 ? binding.color.layers : 1u); return slices; }(), state.scissor, std::move(scratch), std::move(checkRecords), listed, completion, outcome, drawUse, prepared.spaceKeep != nullptr ? prepared.spaceKeep.get() : nullptr);
        timer.phase(PhaseKeep);
        if (outcome.waited) {
            // The wait the dispatch path makes for such work (source 3, "address-based"): the
            // write-back runs before the next packet, so the CPU never reads copied results or
            // touches a leased allocation before they landed.
            Recorder::CountSync(3);
            const auto ownBefore = profile ? Recorder::ThreadWaitedMs() : 0.0;
            recorder->Sync();
            if (profile) ownWaitedMs += Recorder::ThreadWaitedMs() - ownBefore;
            timer.phase(PhaseSync);
        }
        report(outcome.waited ? (outcome.reason == SyncLease ? " recorded then waited (lease)" : " recorded then waited (copied writes)") : completion ? " recorded with completion" : " recorded");
        return;
    }
    {
        // The recorder sync SubmitAndWait makes is the draw's own wait, not a nested hook wait.
        const auto ownBefore = profile ? Recorder::ThreadWaitedMs() : 0.0;
        batch->SubmitAndWait();
        if (!stencilReferences.empty()) resources->CheckFaults();
        if (profile) ownWaitedMs += Recorder::ThreadWaitedMs() - ownBefore;
    }
    if (checkRecords) checkRecords();
    timer.phase(PhaseSync);
    APS5_LOG_CHARS_OUT_DEBUG("SubmitAndWait OK");
    for (const auto& binding : targets) GuestMemory::CheckRange(reinterpret_cast<const void*>(binding.color.MemoryAddress()), binding.color.bytes, 256, true);
    for (auto& binding : targets) {
        if (!binding.dump) continue;
        std::lock_guard lock(dumpMutex);
        char name[64];
        std::snprintf(name, sizeof(name), "target_%llx_%d.raw", static_cast<unsigned long long>(binding.color.address), dumped[binding.color.address]++);
        if (std::FILE* file = std::fopen(name, "wb")) {
            const std::uint32_t header[3] = {binding.color.extent.width, binding.color.extent.height, static_cast<std::uint32_t>(binding.color.format)};
            std::fwrite(header, sizeof(header), 1, file);
            std::fwrite(binding.dump->Bytes().data(), 1, binding.dump->Bytes().size(), file);
            std::fclose(file);
        }
    }
    APS5_LOG_CHARS_OUT_DEBUG("Shader resources WriteBack");
    resources->WriteBack();
    APS5_LOG_CHARS_OUT_DEBUG("Shader resources WriteBack OK");
    static const bool skipTargetWrite = std::getenv("APS5_NO_TARGET_WRITEBACK") != nullptr;
    for (const auto& binding : targets) {
        if (skipTargetWrite) break;
        if (binding.resident != nullptr) {
            // The results stay on the GPU until something reads the target's memory.
            binding.resident->MarkSliceDirty(binding.color.arraySlice, binding.color.arrayLastSlice != 0 ? binding.color.layers : 1u);
            continue;
        }
        if (binding.gpuTiling) GuestMemory::WriteChanged(binding.color.address, binding.tiled->Bytes(), binding.original);
        else if (binding.banded) GuestMemory::WriteChanged(binding.color.address + binding.bandOffset, binding.transfer->Bytes(), binding.original);
        else WriteColorTarget(binding.color, binding.transfer->Bytes());
        // The stored texels are the whole target now, so later reads must see them rather than a fast clear.
        const auto targetBytes = ColorTargetLayout(binding.color.extent.width, binding.color.extent.height, binding.color.tileMode, binding.color.elementBytes).Bytes();
        MarkDccUncompressed(binding.color.dccAddress, targetBytes, colorKeyCount(binding.color, targetBytes));
    }
    timer.phase(PhaseWriteBack);
    if (profile && traceDraws) {
        // A synchronous draw's inputs and target sample are described as a rendering debug aid;
        // the ~0.5 ms that takes (Describe samples every bound range and reads its DCC keys) is
        // shown as its own phase and kept out of the plain profile.
        std::size_t nonzero = 0;
        std::size_t sampled = 0;
        if (!targets.empty() && targets.front().resident == nullptr) {
            const auto bytes = targets.front().gpuTiling ? targets.front().tiled->Bytes() : targets.front().transfer->Bytes();
            sampled = bytes.size() / 64;
            for (std::size_t i = 0; i < bytes.size(); i += 64) nonzero += bytes[i] != std::byte{0};
        }
        std::fprintf(stderr, "[draw]   inputs:%s\n", resources->Describe().c_str());
        if (targets.size() > 1) {
            std::string list;
            for (const auto& binding : targets) {
                char entry[64];
                std::snprintf(entry, sizeof(entry), " 0x%llx(%d,%ux%u)", static_cast<unsigned long long>(binding.color.address), static_cast<int>(binding.color.format), binding.color.extent.width, binding.color.extent.height);
                list += entry;
            }
            std::fprintf(stderr, "[draw]   targets:%s\n", list.c_str());
        }
        timer.phase(PhaseDescribe);
        char suffix[160];
        std::snprintf(suffix, sizeof(suffix), " synchronous (%s), first 0x%llx format %d (%zu of %zu sampled bytes nonzero)", SyncReasonNames[outcome.reason], static_cast<unsigned long long>(state.color.address), static_cast<int>(state.color.format), nonzero, sampled);
        report(suffix);
    } else {
        report(" synchronous");
    }
    APS5_LOG_CHARS_OUT_DEBUG("Draw finished");
}

}

namespace {
// APS5_ARCHB_PRELOCK_STAMP_SKIP (census 2, 2026-11-04): template surface precollects made, skipped,
// and in _VERIFY mode (skipped ones collected anyway) the time and walks the skip would have removed.
struct PrelockStampSkipCounters {
    std::atomic<std::uint64_t> precollects{0};
    std::atomic<std::uint64_t> skips{0};
    std::atomic<std::uint64_t> verified{0};
    std::atomic<std::uint64_t> verifyWalks{0};
    std::atomic<std::uint64_t> verifyNs{0};
};

PrelockStampSkipCounters& prelockStampSkipCounters() {
    static PrelockStampSkipCounters* const made = [] {
        auto* counters = new PrelockStampSkipCounters;
        AgcDriver::ArchB::Recorder::RegisterCounter("prelock_precollects", counters->precollects);
        AgcDriver::ArchB::Recorder::RegisterCounter("prelock_stamp_skips", counters->skips);
        AgcDriver::ArchB::Recorder::RegisterCounter("prelock_stamp_skip_verified", counters->verified);
        AgcDriver::ArchB::Recorder::RegisterCounter("prelock_stamp_skip_verify_walks", counters->verifyWalks);
        AgcDriver::ArchB::Recorder::RegisterCounter("prelock_stamp_skip_verify_ns", counters->verifyNs);
        return counters;
    }();
    return *made;
}

bool prelockStampSkipVerify() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_PRELOCK_STAMP_SKIP_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

void prelockPrecollect(const ShaderResources& cached, bool memoVirtualQuery) {
    auto& counters = prelockStampSkipCounters();
    if (!cached.StampProofArmed() || !ArchB::Switches::Enabled(ArchB::Feature::PrelockStampSkip)) {
        counters.precollects.fetch_add(1, std::memory_order_relaxed);
        cached.PrecollectSurfaces(memoVirtualQuery);
        return;
    }
    counters.skips.fetch_add(1, std::memory_order_relaxed);
    if (!prelockStampSkipVerify()) return;
    const auto walks = GuestMemory::CommitterCollects().walks.load(std::memory_order_relaxed);
    const auto start = std::chrono::steady_clock::now();
    cached.PrecollectSurfaces(memoVirtualQuery);
    counters.verifyNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
    counters.verifyWalks.fetch_add(GuestMemory::CommitterCollects().walks.load(std::memory_order_relaxed) - walks, std::memory_order_relaxed);
    counters.verified.fetch_add(1, std::memory_order_relaxed);
}
}

DrawPreparation PrepareDraw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, const DrawRecipe* recipe, bool memoVirtualQuery, DrawPreparation* resolved) {
    // Stage 4 M4: the committer's unlocked preparation (precollects, template find, target proofs).
    Stage4::Mark(Stage4::Bucket::Pre);
    struct Stage4Prelock {
        ~Stage4Prelock() { Stage4::Mark(Stage4::Bucket::Prelock); }
    } stage4Prelock;
    const GuestMemory::Gen::ConsumerScope genConsumer(GuestMemory::Gen::Consumer::Prelock);
    DrawPreparation prepared;
    if (!DrawPrelockEnabled() || GuestMemory::GpuMutex().HeldByThisThread()) return prepared;
    const bool ahead = resolved != nullptr && resolved->resolved && resolved->device == context.device;
    if (ahead) prepared = std::move(*resolved);
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    prepared.device = context.device;
    const auto collectedBefore = profile ? GuestMemory::ThreadCollectedBytes() : 0;
    if (profile) {
        prepared.epoch = GuestMemory::ThreadCollectEpoch();
        prepared.mapping = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        prepared.unwatch = GuestMemory::UnwatchSerial();
    }
    const bool collect = GuestMemory::ThreadCollectEpoch() != 0;
    static const bool residentTargets = std::getenv("APS5_NO_RESIDENT_TARGETS") == nullptr;
    static const bool residentLinear = std::getenv("APS5_NO_RESIDENT_LINEAR") == nullptr;
    const bool census = archcTargets::censusActive();
    if (census) archcTargets::prelockNs().fill(0);
    const bool fastTargets = archcTargets::fastActive() && !archcTargets::fastVerify();
    std::uint64_t fastSkipped = 0;
    if (fastTargets && collect && residentTargets && context.detiler != nullptr) {
        for (std::size_t colorIndex = 0; colorIndex < state.colors.size() && colorIndex < 64; ++colorIndex) {
            if (archcTargets::tokenHolds(context, state.colors[colorIndex])) fastSkipped |= std::uint64_t{1} << colorIndex;
        }
        if (fastSkipped != 0) archcTargets::fastCounters().prelockSkips.fetch_add(static_cast<std::uint64_t>(std::popcount(fastSkipped)), std::memory_order_relaxed);
    }
    if (collect && residentTargets && context.detiler != nullptr) {
        for (std::size_t colorIndex = 0; colorIndex < state.colors.size(); ++colorIndex) {
            const auto& color = state.colors[colorIndex];
            if (color.samples != 1 || (color.tileMode == ColorTileMode::Linear && !residentLinear)) continue;
            if (colorIndex < 64 && ((fastSkipped >> colorIndex) & 1) != 0) continue;
            const auto censusStart = census ? archcTargets::nowNs() : 0;
            struct CensusSpan {
                bool census;
                std::size_t index;
                std::uint64_t start;
                ~CensusSpan() {
                    if (census && index < 8) archcTargets::prelockNs()[index] += archcTargets::nowNs() - start;
                }
            } censusSpan{census, colorIndex, censusStart};
            std::optional<std::pair<GuestTextureResource, std::uint64_t>> described;
            try {
                const auto requested = color.arrayLastSlice != 0 ? ArraySurfaceForTarget(color) : SurfaceForTarget(color);
                const auto surface = ArrayLayerWindow(requested, context.limits.maxImageArrayLayers);
                described.emplace(surface, SurfaceGuestBytes(surface));
            } catch (const std::runtime_error&) {
                continue;
            }
            const auto& [surface, bytes] = *described;
            const auto precollect = [&](std::uint64_t address, std::uint64_t count) {
                const auto generation = GuestMemory::CollectWrites(address, static_cast<std::size_t>(count));
                if (profile) {
                    ++prepared.ranges;
                    prepared.unavailable += generation == 0;
                }
            };
            precollect(surface.baseAddress, bytes);
            if (surface.dccAddress != 0) precollect(surface.dccAddress, DccKeyBytes(bytes));
        }
    }
    std::shared_ptr<ShaderResources> cached;
    if (ahead) {
        cached = prepared.resolvedTemplate;
    } else if (recipe != nullptr && recipe->device == context.device) {
        cached = recipe->templateRef.lock();
    } else if (RecordDraws() && DumpTargetLimit() == 0 && DrawResourcesCacheable(shaders)) {
        static const bool trimKey = std::getenv("APS5_NO_DRAW_KEY_TRIM") == nullptr;
        prepared.resourceKey = DrawResourceKey(context, shaders, state.color, static_cast<std::uint32_t>(state.colors.size()), draw.indexAddress, static_cast<std::uint64_t>(draw.indexCount) * draw.indexSize, !trimKey);
        const bool handoff = ArchB::Switches::Enabled(ArchB::Feature::PrelockFindHandoff);
        if (handoff) prepared.resolvedCacheSerial = SharedResourceCache().Mutations();
        cached = SharedResourceCache().Find(prepared.resourceKey);
        if (handoff) {
            prepared.resolvedFind = true;
            prepared.resolvedTemplate = cached;
        }
    }
    if (cached != nullptr && collect) prelockPrecollect(*cached, memoVirtualQuery);
    if (collect && residentTargets && context.detiler != nullptr && StorageTexture::DrawTargetPrelockEnabled()) {
        const auto proofStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        prepared.targets.resize(state.colors.size());
        for (std::size_t index = 0; index < state.colors.size(); ++index) {
            const auto& color = state.colors[index];
            if (color.samples != 1 || (color.tileMode == ColorTileMode::Linear && !residentLinear)) continue;
            if (index < 64 && ((fastSkipped >> index) & 1) != 0) continue;
            const auto censusStart = census ? archcTargets::nowNs() : 0;
            try {
                const auto surface = color.arrayLastSlice != 0 ? ArraySurfaceForTarget(color) : SurfaceForTarget(color);
                prepared.targets[index] = PrepareStorageSurface(context, surface);
            } catch (const std::runtime_error&) {
                if (census && index < 8) archcTargets::prelockNs()[index] += archcTargets::nowNs() - censusStart;
                continue;
            }
            if (census && index < 8) archcTargets::prelockNs()[index] += archcTargets::nowNs() - censusStart;
        }
        if (profile) prepared.targetProofUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - proofStart).count();
    }
    if (profile) {
        prepared.cached = cached;
        prepared.collectedBytes = GuestMemory::ThreadCollectedBytes() - collectedBefore;
    }
    return prepared;
}

namespace {
bool resolvePrecollects() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_PIPE_RESOLVE_PRECOLLECT");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool resolvePipelines() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_PIPE_RESOLVE_PIPELINE");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

std::uint64_t mixPlan(std::uint64_t hash, std::uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    return hash;
}
}

std::uint64_t ResolveDraw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, const DrawRecipe* recipe, bool memoVirtualQuery, DrawPreparation& out, std::vector<std::pair<std::uint64_t, std::uint64_t>>& reads) {
    out = {};
    reads.clear();
    if (!DrawPrelockEnabled() || GuestMemory::GpuMutex().HeldByThisThread()) return 0;
    std::uint64_t plan = 0x5245534fu;
    try {
        std::shared_ptr<ShaderResources> cached;
        if (recipe != nullptr && recipe->device == context.device) {
            cached = recipe->templateRef.lock();
            plan = mixPlan(plan, 1);
        } else if (RecordDraws() && DumpTargetLimit() == 0 && DrawResourcesCacheable(shaders)) {
            static const bool trimKey = std::getenv("APS5_NO_DRAW_KEY_TRIM") == nullptr;
            out.resourceKey = DrawResourceKey(context, shaders, state.color, static_cast<std::uint32_t>(state.colors.size()), draw.indexAddress, static_cast<std::uint64_t>(draw.indexCount) * draw.indexSize, !trimKey);
            for (const auto word : out.resourceKey) plan = mixPlan(plan, word);
            out.resolvedCacheSerial = SharedResourceCache().Mutations();
            cached = SharedResourceCache().Find(out.resourceKey);
            out.resolvedFind = true;
        }
        out.device = context.device;
        out.resolved = true;
        out.resolvedTemplate = cached;
        if (cached == nullptr) return plan;
        cached->DescribedRanges(reads);
        if (resolvePrecollects() && !GuestMemory::IsCommitterThread() && GuestMemory::ThreadCollectEpoch() != 0) {
            const auto start = std::chrono::steady_clock::now();
            cached->PrecollectSurfaces(memoVirtualQuery);
            resolvePipelineCounters().precollectNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
        }
        if (recipe != nullptr || !resolvePipelines() || shaders.empty() || shaders.front().program == nullptr || state.colors.size() > 64) return plan;
        for (const auto& shader : shaders) {
            if (shader.program != nullptr && !shader.program->stencilReferences.empty()) return plan;
        }
        const auto pipelineStart = std::chrono::steady_clock::now();
        bool memoized = false;
        bool hit = false;
        const auto outputs = CachedFragmentOutputs(context, shaders, state, memoized, hit);
        std::uint64_t proxied = 0;
        for (std::size_t index = 0; index < state.colors.size(); ++index) {
            if (state.colors[index].samples == 1 && AttachmentProxyFormat(context, state.colors[index].format) != VK_FORMAT_UNDEFINED) proxied |= std::uint64_t{1} << index;
        }
        const auto& attributes = shaders.front().program->vertexAttributes;
        std::shared_ptr<const VertexInputLayout> retained;
        VertexInputLayout built;
        if (!attributes.empty()) {
            if (context.vertexInputs != nullptr) retained = context.vertexInputs->TryGet(shaders.front().program->variantId, attributes);
            if (retained == nullptr) built = BuildVertexInputLayout(context, attributes);
        }
        std::optional<State> masked;
        const State& pipelineState = pipelineStateFor(context, state, outputs, proxied, masked);
        out.resolvedOutputsKnown = true;
        out.resolvedOutputs = outputs;
        if (!attributes.empty()) out.resolvedVertexInput = retained != nullptr ? retained : std::make_shared<const VertexInputLayout>(built);
        out.speculatedPipeline = CachedPipeline(context, pipelineState, retained != nullptr ? *retained : built, *cached, shaders, VK_IMAGE_LAYOUT_GENERAL);
        out.speculatedOutputs = outputs;
        out.speculatedProxied = proxied;
        resolvePipelineCounters().speculated.fetch_add(1, std::memory_order_relaxed);
        resolvePipelineCounters().pipelineNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pipelineStart).count()), std::memory_order_relaxed);
    } catch (...) {
        out = {};
        reads.clear();
        return 0;
    }
    return plan;
}

int ArchcTakeRecipeBuild() {
    return std::exchange(archcLastRecipeBuild(), -1);
}

namespace ArchC {

std::atomic<bool> probing{false};

void ProbeMark(Probe probe) {
    if (!GuestMemory::IsCommitterThread() || probe >= Probe::Count) return;
    const auto now = archcSlots::probeNow();
    auto& made = archcSlots::counters();
    const auto index = static_cast<std::size_t>(probe);
    archcSlots::addTrimmed(made.probeNs[index], made.probeN[index], made.probeOut[index], now - archcSlots::probeLast);
    archcSlots::probeLast = now;
}

}

int ArchcTakeReplayOutcome() {
    return std::exchange(archcSlots::lastOutcome(), -1);
}

bool ArchcReplayLikely(VkDevice device, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, const DrawSlotIdentity* identity, bool pipelineStateReuse) {
    if (identity == nullptr || (!draw.indirect && !AgcDriver::ArchB::DirectDrawEnabled()) || !archcSlots::active() || !archcSlots::usableIdentity(*identity, state, shaders)) return false;
    const auto* slot = archcSlots::lookup(device, state, draw, shaders, *identity, pipelineStateReuse);
    if (slot == nullptr || !slot->replayedLast) return false;
    archcSlots::counters().prelockSkips.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool ArchcReplayActive() {
    return AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::ArchcDrawReplay);
}

ArchcReplayTotals ArchcReplayCounts() {
    auto& counters = archcSlots::counters();
    return {counters.hits.load(), counters.captures.load(), counters.legacy.load(), counters.verifyChecks.load(), counters.verifyMismatches.load()};
}

ArchcTargetTotals ArchcTargetCounts() {
    auto& counters = archcTargets::fastCounters();
    return {counters.lookups.load(), counters.hits.load(), counters.tokens.load(), counters.legacyPasses.load(), counters.verifyChecks.load(), counters.verifyMismatches.load()};
}

std::string ResolvePipelineText() {
    auto& counters = resolvePipelineCounters();
    const auto get = [](const std::atomic<std::uint64_t>& value) { return std::to_string(value.load(std::memory_order_relaxed)); };
    return "[pipe-resolve-pipeline] cumulative: speculated " + get(counters.speculated) + " hits " + get(counters.hits) + " misses " + get(counters.misses) + " verified " + get(counters.verified) + " mismatches " + get(counters.mismatches);
}

namespace {

struct NostageTargets {
    std::array<std::atomic<std::uint64_t>, archcTargets::FastRefusalCount> refused{};
    std::array<std::atomic<std::uint64_t>, archcTargets::TokenRefusalCount> tokenRefused{};
    std::atomic<std::uint64_t> hits{0}, tokens{0}, targets{0}, slowLogged{0}, sampled{0};
    NostageTargets() {
        using AgcDriver::ArchB::Recorder;
        for (std::size_t i = 0; i < archcTargets::FastRefusalCount; ++i) Recorder::RegisterCounter((new std::string(std::string("archc_nostage_target_refused_") + archcTargets::FastRefusalNames[i]))->c_str(), refused[i]);
        for (std::size_t i = 0; i < archcTargets::TokenRefusalCount; ++i) Recorder::RegisterCounter((new std::string(std::string("archc_nostage_target_token_refused_") + archcTargets::TokenRefusalNames[i]))->c_str(), tokenRefused[i]);
        Recorder::RegisterCounter("archc_nostage_target_hits", hits);
        Recorder::RegisterCounter("archc_nostage_target_tokens", tokens);
        Recorder::RegisterCounter("archc_nostage_targets", targets);
    }
};

NostageTargets& nostageTargets() {
    static NostageTargets made;
    return made;
}

}

constexpr std::uint64_t MaxCapturedTriangles = 1ull << 20u;

void drawTessellationGeometry(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, const DrawTraceContext* trace) {
    using Stage = ShaderRecompiler::ShaderStage;
    Require(context.meshShader && context.geometryShader, "tessellation followed by a geometry stage needs mesh and geometry shaders");
    Require(!draw.indirect, "an indirect draw with tessellation and a geometry stage must be resolved by the driver");
    Require(draw.instanceCount <= 1u, "tessellation followed by a geometry stage is drawn for one instance only");
    if (draw.indexCount == 0 || draw.instanceCount == 0) return;
    const auto& tessellation = *state.stages.tessellation;
    const auto& geometry = *state.stages.tessellationGeometry;
    std::vector<CompiledShader> first;
    std::vector<CompiledShader> second;
    for (const auto& shader : shaders) {
        if (shader.stage != Stage::Mesh && shader.stage != Stage::Fragment) first.push_back(shader);
        if (shader.stage == Stage::Mesh || shader.stage == Stage::Fragment) second.push_back(shader);
    }
    Require(second.size() == 2u && second.front().stage == Stage::Mesh && std::ranges::count_if(first, [](const CompiledShader& shader) { return shader.stage == Stage::Geometry; }) == 1, "tessellation followed by a geometry stage needs its capture and mesh stages");
    const auto patches = static_cast<std::uint64_t>(draw.indexCount / tessellation.inputControlPoints);
    const auto level = static_cast<std::uint64_t>(std::ceil(std::clamp(state.stages.maxTessellationLevel, 1.0f, 64.0f))) + 1u;
    const auto perPatch = 2u * level * level;
    const auto capacity = static_cast<std::uint32_t>(std::min(perPatch * patches, MaxCapturedTriangles));
    auto capture = std::make_shared<TessellationCapture>();
    capture->header = std::make_shared<Buffer>(context, ShaderRecompiler::MeshCapturedHeaderBytes, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    std::array<std::uint32_t, ShaderRecompiler::MeshCapturedHeaderBytes / 4> header{};
    header[1] = 1u;
    header[2] = 1u;
    header[ShaderRecompiler::MeshCapturedCapacityDword] = capacity;
    std::memcpy(capture->header->Bytes().data(), header.data(), sizeof(header));
    capture->items = std::make_shared<Buffer>(context, static_cast<std::size_t>(capacity) * 3u * geometry.esgsItemSize * sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    auto tessellated = state;
    tessellated.stages.path = ShaderPath::Tessellation;
    tessellated.stages.tessellationGeometry.reset();
    tessellated.rasterizerDiscard = true;
    tessellated.tessellationCapture = capture;
    Draw(context, tessellated, draw, first, snapshots, nullptr, trace);
    auto meshed = state;
    meshed.stages.path = ShaderPath::Geometry;
    meshed.stages.mesh = geometry;
    meshed.stages.tessellation.reset();
    meshed.stages.tessellationGeometry.reset();
    meshed.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    meshed.tessellationCapture = capture;
    const Pm4::DrawParameters captured{0, 3u, 0, 1u, 0, false, 0, 0};
    Draw(context, meshed, captured, second, snapshots, nullptr, trace);
}

void Draw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, std::shared_ptr<const DrawRecipe>* recipeOut, const DrawTraceContext* trace, std::uint64_t shape, DrawPreparation* preparation, bool pipelineStateReuse, const DrawSlotIdentity* identity) {
    const GpuAuthorityCensus::ProducerScope censusProducer(2, GpuAuthorityCensus::Enabled() ? timingDrawKey(shaders, 0) : 0);
    if (!pipelineStateReuse && context.recorder != nullptr) context.recorder->InvalidatePipelineState("fix_pso_reuse_invalidations_off");
    if (preparation != nullptr) RecheckDrawPreparation(context, *preparation);
    const DepthCopySourceScope depthCopySource(state.depthCopySource);
    if (state.stages.tessellationGeometry) {
        drawTessellationGeometry(context, state, draw, shaders, snapshots, trace);
        return;
    }
    if (state.stages.mesh && state.layerExports != 0u && state.renderLayers > context.meshLimits.maxMeshOutputLayers && (draw.indirect || (draw.indexCount != 0u && draw.instanceCount != 0u))) {
        const auto limit = context.meshLimits.maxMeshOutputLayers;
        Require(limit != 0u && !state.depth, "mesh layer windows require a nonzero output limit and color-only layered rendering");
        Require(std::ranges::all_of(shaders, [](const CompiledShader& shader) { return shader.program != nullptr && shader.program->layerReplaySafe; }), "mesh layer windows refuse shaders with guest memory writes, clocks or unproven side effects");
        if (recipeOut != nullptr) recipeOut->reset();
        for (std::uint32_t base = 0; base < state.renderLayers;) {
            auto window = state;
            window.renderLayers = std::min(limit, state.renderLayers - base);
            window.meshLayerBase = base;
            window.meshLayerCount = window.renderLayers;
            for (auto& color : window.colors) {
                Require(color.layers == state.renderLayers, "mesh layer window targets have different layer counts");
                if (color.depth > 1u) color.depthSlice += base;
                else color.arraySlice += base;
                color.layers = window.renderLayers;
            }
            if (!window.colors.empty()) window.color = window.colors.front();
            Draw(context, window, draw, shaders, snapshots, nullptr, trace);
            base += window.renderLayers;
        }
        return;
    }
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    PreparedDraw prepared(profile);
    archcSlots::lastOutcome() = -1;
    if (identity != nullptr && (draw.indirect || AgcDriver::ArchB::DirectDrawEnabled()) && archcSlots::active()) {
        prepared.identity = identity;
        if (archcSlots::tryReplay(context, state, draw, shaders, *identity, trace, pipelineStateReuse, prepared)) return;
        archcSlots::counters().legacy.fetch_add(1, std::memory_order_relaxed);
    }
    if (identity != nullptr && identity->gather != nullptr) {
        AgcDriver::ArchB::Recorder::Count("draw_templates_live_commit_fallback");
        AgcDriver::ArchB::Recorder::Count(draw.indirect ? "draw_templates_live_commit_fallback_indirect" : "draw_templates_live_commit_fallback_direct");
        auto programs = DriverDetail::MaterializeGather(*identity->gather);
        if (!identity->referenceStages.empty()) {
            AgcDriver::ArchB::Recorder::Count("draw_templates_live_verify_fallback_checks");
            if (programs.size() != identity->referenceStages.size()) archcSlots::liveMismatch(archcSlots::LiveElementSet, "the fallback stage count differs");
            for (std::size_t i = 0; i < programs.size() && i < identity->referenceStages.size(); ++i) {
                const auto& live = *programs[i];
                const auto& old = *identity->referenceStages[i].program;
                if (live.bindings.size() != old.bindings.size()) archcSlots::liveMismatch(archcSlots::LiveElementSet, "the fallback binding count differs");
                for (std::size_t binding = 0; binding < live.bindings.size() && binding < old.bindings.size(); ++binding) {
                    const auto& a = live.bindings[binding];
                    const auto& b = old.bindings[binding];
                    if (a.kind != b.kind || a.role != b.role || a.binding != b.binding || a.count != b.count || a.descriptorSet != b.descriptorSet) archcSlots::liveMismatch(archcSlots::LiveElementSet, "the fallback binding layout differs");
                    const auto* ignoredWords = i < identity->referenceMasks.size() && binding < identity->referenceMasks[i].bindings.size() ? &identity->referenceMasks[i].bindings[binding] : nullptr;
                    if (!DrawReferenceMasks::SameWords(a.guestDescriptor, b.guestDescriptor, ignoredWords)) {
                        static unsigned reported = 0;
                        if (reported++ < 12) {
                            const auto common = std::min(a.guestDescriptor.size(), b.guestDescriptor.size());
                            std::size_t word = 0;
                            while (word < common && a.guestDescriptor[word] == b.guestDescriptor[word]) ++word;
                            std::fprintf(stderr, "[draw-templates-live] detail fallback stage %zu binding %zu role %u word %zu sizes %zu/%zu live %08x old %08x\n", i, binding, static_cast<unsigned>(a.role), word, a.guestDescriptor.size(), b.guestDescriptor.size(), word < a.guestDescriptor.size() ? a.guestDescriptor[word] : 0, word < b.guestDescriptor.size() ? b.guestDescriptor[word] : 0);
                        }
                        archcSlots::liveMismatch(archcSlots::LiveElementBytes, "the fallback descriptor words differ");
                    }
                }
                if (live.pushConstants.size() != old.pushConstants.size() || !DrawReferenceMasks::SameBytes(live.pushConstants.data(), old.pushConstants.data(), old.pushConstants.size(), i < identity->referenceMasks.size() ? &identity->referenceMasks[i].push : nullptr)) archcSlots::liveMismatch(archcSlots::LivePush, "the fallback push input differs");
                if (!std::ranges::equal(live.spirv.Words(), old.spirv.Words())) archcSlots::liveMismatch(archcSlots::LivePipeline, "the fallback shader differs");
            }
            if (identity->referenceDraw == nullptr || !archcSlots::sameDraw(draw, *identity->referenceDraw) || (draw.indirect && draw.indirect->arguments != identity->referenceDraw->indirect->arguments)) archcSlots::liveMismatch(archcSlots::LiveArguments, "the fallback packet differs");
        }
        auto patched = std::vector<CompiledShader>(shaders.begin(), shaders.end());
        auto fallback = *identity;
        fallback.gather = nullptr;
        fallback.learning = identity->gather;
        fallback.referenceStages = {};
        fallback.referenceDraw = nullptr;
        for (std::size_t stage = 0; stage < patched.size(); ++stage) {
            patched[stage].program = programs[stage].get();
            fallback.stages[stage].owner = programs[stage];
            fallback.stages[stage].variant.reset();
        }
        Draw(context, state, draw, patched, snapshots, recipeOut, trace, shape, preparation, pipelineStateReuse, &fallback);
        return;
    }
    reuseCensus::begin(false);
    prepared.preparation = preparation;
    prepared.recipeOut = recipeOut;
    prepared.trace = trace;
    prepared.shape = shape;
    prepared.pipelineStateReuse = pipelineStateReuse;
    const struct HeldSpace {
        PreparedDraw& prepared;
        ~HeldSpace() {
            const auto& resources = prepared.resolved.resources;
            if (resources == nullptr || !resources->SpaceReuse() || !resources->SpaceHeld()) return;
            if (prepared.spaceKeep != nullptr) *prepared.spaceKeep = resources->ReleaseSpace();
            else if (prepared.recorder != nullptr) resources->HoldSpaceThrough(prepared.recorder->Id(), prepared.recorder->OpenSerial());
            else static_cast<void>(resources->ReleaseSpace());
        }
    } heldSpace{prepared};
    struct NostageScope {
        PreparedDraw& prepared;
        bool on = false;
        bool reached = false;
        explicit NostageScope(PreparedDraw& owner, bool direct, const State& drawn) : prepared(owner), state(drawn) {
            if (!archcNostage::enabled()) return;
            auto& made = archcNostage::state();
            if (made.armed < 0 || !direct) return;
            on = made.active = true;
            made.why = std::min<std::size_t>(static_cast<std::size_t>(made.armed), archcNostage::WhyCount - 1);
            made.armed = -1;
            made.phase.fill(0);
            made.lookup = archcNostage::LookupNone;
            made.waitedBefore = Recorder::ThreadWaitedMs();
            made.start = made.last = archcNostage::now();
        }
        ~NostageScope() {
            if (!on) return;
            auto& made = archcNostage::state();
            made.active = false;
            const auto end = archcNostage::now();
            made.phase[PhaseCount] += end - made.last;
            const auto total = end - made.start;
            auto& counts = archcNostage::counters();
            counts.draws[made.why].fetch_add(1, std::memory_order_relaxed);
            counts.ns[made.why].fetch_add(total, std::memory_order_relaxed);
            for (std::size_t i = 0; i <= PhaseCount; ++i) counts.phaseNs[i].fetch_add(made.phase[i], std::memory_order_relaxed);
            counts.lookup[made.lookup].fetch_add(1, std::memory_order_relaxed);
            counts.lookupNs[made.lookup].fetch_add(total, std::memory_order_relaxed);
            const auto& outcome = prepared.outcome;
            const auto record = !reached ? archcNostage::RecordNothing : !outcome.recorded ? archcNostage::RecordSynchronous : outcome.waited ? archcNostage::RecordWaited : prepared.listed ? archcNostage::RecordListed : archcNostage::RecordRecorded;
            counts.record[record].fetch_add(1, std::memory_order_relaxed);
            counts.recordNs[record].fetch_add(total, std::memory_order_relaxed);
            if (reached) {
                const auto kind = std::min<std::size_t>(outcome.kind, KindCount - 1);
                counts.kind[kind].fetch_add(1, std::memory_order_relaxed);
                counts.kindNs[kind].fetch_add(total, std::memory_order_relaxed);
                counts.reason[outcome.reason].fetch_add(1, std::memory_order_relaxed);
                if (outcome.completion) counts.completion.fetch_add(1, std::memory_order_relaxed);
                if (outcome.passBegun) counts.passBegun.fetch_add(1, std::memory_order_relaxed);
                if (outcome.passContinued) counts.passContinued.fetch_add(1, std::memory_order_relaxed);
                if (!prepared.resolved.moved.empty()) counts.moved.fetch_add(1, std::memory_order_relaxed);
                if (prepared.resolved.cacheable) counts.cacheable.fetch_add(1, std::memory_order_relaxed);
                if (prepared.resolved.cacheable && prepared.resolved.built != nullptr && outcome.recorded) counts.inserted.fetch_add(1, std::memory_order_relaxed);
            }
            counts.validate[!outcome.validateMemoized ? archcNostage::ValidateOff : outcome.validateHit ? archcNostage::ValidateHit : archcNostage::ValidateMiss].fetch_add(1, std::memory_order_relaxed);
            const auto waited = Recorder::ThreadWaitedMs() - made.waitedBefore;
            if (waited > 0) counts.waitNs.fetch_add(static_cast<std::uint64_t>(waited * 1e6), std::memory_order_relaxed);
            const auto kind = reached ? std::min<std::size_t>(outcome.kind, KindCount - 1) : std::size_t{0};
            for (std::size_t i = 0; i <= PhaseCount; ++i) counts.kindPhaseNs[kind][i].fetch_add(made.phase[i], std::memory_order_relaxed);
            if (total >= archcNostage::TrimNs) {
                counts.slow.fetch_add(1, std::memory_order_relaxed);
                counts.slowNs.fetch_add(total, std::memory_order_relaxed);
                for (std::size_t i = 0; i <= PhaseCount; ++i) counts.slowPhaseNs[i].fetch_add(made.phase[i], std::memory_order_relaxed);
            }
            auto& targets = nostageTargets();
            auto& fast = archcTargets::fastCounters();
            for (std::size_t i = 0; i < archcTargets::FastRefusalCount; ++i) targets.refused[i].fetch_add(fast.refused[i].load(std::memory_order_relaxed) - refusedBefore[i], std::memory_order_relaxed);
            for (std::size_t i = 0; i < archcTargets::TokenRefusalCount; ++i) targets.tokenRefused[i].fetch_add(fast.tokenRefused[i].load(std::memory_order_relaxed) - tokenBefore[i], std::memory_order_relaxed);
            targets.hits.fetch_add(fast.hits.load(std::memory_order_relaxed) - hitsBefore, std::memory_order_relaxed);
            targets.tokens.fetch_add(fast.tokens.load(std::memory_order_relaxed) - tokensBefore, std::memory_order_relaxed);
            targets.targets.fetch_add(prepared.targets.size(), std::memory_order_relaxed);
            const bool slowDraw = total >= 300000;
            const auto logged = slowDraw ? targets.slowLogged.fetch_add(1, std::memory_order_relaxed) : targets.sampled.fetch_add(1, std::memory_order_relaxed);
            if ((slowDraw && logged < 48) || (!slowDraw && logged % 4096 == 0 && logged < 4096 * 24)) {
                char line[1536];
                int n = std::snprintf(line, sizeof(line), "[archc-nostage] %s total %.1f us why %s kind %zu lookup %s record %d waited %.1f us targets %zu depth %d:", slowDraw ? "slow" : "sample", total / 1000.0, archcNostage::WhyNames[made.why], kind, archcNostage::LookupNames[made.lookup], static_cast<int>(record), waited * 1000.0, prepared.targets.size(), state.depth.has_value() ? 1 : 0);
                for (std::size_t i = 0; i <= PhaseCount && n > 0 && static_cast<std::size_t>(n) < sizeof(line); ++i) {
                    if (made.phase[i] >= 1000) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s=%.1f", i < PhaseCount ? DrawPhaseNames[i] : "tail", made.phase[i] / 1000.0);
                }
                for (const auto& binding : prepared.targets) {
                    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(line)) break;
                    const auto& color = binding.color;
                    n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " | target 0x%llx+0x%zx %ux%u fmt %u tile %u dcc 0x%llx cmask 0x%llx fmask 0x%llx slice %u layers %u mips %u samples %u resident %d", static_cast<unsigned long long>(color.address), color.bytes, color.extent.width, color.extent.height, static_cast<unsigned>(color.format), static_cast<unsigned>(color.tileMode), static_cast<unsigned long long>(color.dccAddress), static_cast<unsigned long long>(color.cmaskAddress), static_cast<unsigned long long>(color.fmaskAddress), color.arraySlice, color.layers, color.mipCount, color.samples, binding.resident != nullptr ? 1 : 0);
                }
                if (n > 0 && static_cast<std::size_t>(n) < sizeof(line)) {
                    n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " | refused");
                    for (std::size_t i = 0; i < archcTargets::FastRefusalCount && n > 0 && static_cast<std::size_t>(n) < sizeof(line); ++i) {
                        const auto delta = fast.refused[i].load(std::memory_order_relaxed) - refusedBefore[i];
                        if (delta != 0) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s=%llu", archcTargets::FastRefusalNames[i], static_cast<unsigned long long>(delta));
                    }
                    for (std::size_t i = 0; i < archcTargets::TokenRefusalCount && n > 0 && static_cast<std::size_t>(n) < sizeof(line); ++i) {
                        const auto delta = fast.tokenRefused[i].load(std::memory_order_relaxed) - tokenBefore[i];
                        if (delta != 0) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " token_%s=%llu", archcTargets::TokenRefusalNames[i], static_cast<unsigned long long>(delta));
                    }
                }
                std::fprintf(stderr, "%s\n", line);
            }
        }
        std::array<std::uint64_t, archcTargets::FastRefusalCount> refusedBefore{};
        std::array<std::uint64_t, archcTargets::TokenRefusalCount> tokenBefore{};
        std::uint64_t hitsBefore = 0;
        std::uint64_t tokensBefore = 0;
        const State& state;
        void Snapshot() {
            if (!on) return;
            auto& fast = archcTargets::fastCounters();
            for (std::size_t i = 0; i < archcTargets::FastRefusalCount; ++i) refusedBefore[i] = fast.refused[i].load(std::memory_order_relaxed);
            for (std::size_t i = 0; i < archcTargets::TokenRefusalCount; ++i) tokenBefore[i] = fast.tokenRefused[i].load(std::memory_order_relaxed);
            hitsBefore = fast.hits.load(std::memory_order_relaxed);
            tokensBefore = fast.tokens.load(std::memory_order_relaxed);
        }
    } nostage(prepared, !draw.indirect.has_value(), state);
    nostage.Snapshot();
    try {
        if (prepareDraw(context, state, draw, shaders, snapshots, prepared)) {
            nostage.reached = true;
            recordPreparedDraw(context, state, draw, shaders, prepared);
        }
    } catch (...) {
        keepHeldSpace(prepared);
        throw;
    }
    keepHeldSpace(prepared);
}

bool ArchcNostageCensus() {
    return archcNostage::enabled();
}

bool ArchcNostageArmed() {
    return archcNostage::enabled() && archcNostage::state().armed >= 0;
}

void ArchcNostageWrapper(std::uint64_t prelockNs, std::uint64_t lockWaitNs, std::uint64_t labelsNs, std::uint64_t writersNs, std::uint64_t drawNs, std::uint64_t afterNs, std::uint64_t submitNs) {
    auto& counts = archcNostage::counters();
    const std::array<std::uint64_t, archcNostage::Counters::WrapperCount> parts{prelockNs, lockWaitNs, labelsNs, writersNs, drawNs, afterNs, submitNs};
    for (std::size_t i = 0; i < parts.size(); ++i) counts.wrapperNs[i].fetch_add(parts[i], std::memory_order_relaxed);
    counts.wrapped.fetch_add(1, std::memory_order_relaxed);
    if (submitNs >= 5000) {
        counts.submits.fetch_add(1, std::memory_order_relaxed);
        counts.submitSlowNs.fetch_add(submitNs, std::memory_order_relaxed);
    }
}

void ArchcNostageArm(int why) {
    if (archcNostage::enabled()) archcNostage::state().armed = why;
}

DrawRecipeOutcome DrawWithRecipe(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, const DrawRecipe& recipe, const DrawTraceContext* trace, DrawPreparation* preparation, bool pipelineStateReuse, const DrawSlotIdentity* identity) {
    const GpuAuthorityCensus::ProducerScope censusProducer(2, GpuAuthorityCensus::Enabled() ? timingDrawKey(shaders, 0) : 0);
    if (!pipelineStateReuse && context.recorder != nullptr) context.recorder->InvalidatePipelineState("fix_pso_reuse_invalidations_off");
    const DepthCopySourceScope depthCopySource(state.depthCopySource);
    if (context.recorder != nullptr) context.recorder->BoundKeptBytes();
    static_cast<void>(snapshots);
    PerformanceTimer timing("Graphics.DrawWithRecipe", AgcDriver::ArchB::Recorder::Phase::Record);
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    DrawTimer timer(profile);
    DrawRecipeOutcome result;
    DrawOutcome outcome(profile);
    outcome.kind = KindRecipeHit;
    const auto waitedBefore = profile ? Recorder::ThreadWaitedMs() : 0.0;
    double ownWaitedMs = 0;
    const auto miss = [&](DrawRecipeMiss reason) {
        result.miss = reason;
        if (profile) {
            auto& stats = Profile();
            std::lock_guard lock(stats.mutex);
            ++stats.recipeMisses[static_cast<std::size_t>(reason)];
        }
        return result;
    };
    Require(!draw.indirect, "a draw recipe covers direct draws only");
    reuseCensus::begin(true);
    auto* recorder = Recorder::Active();
    if (state.quadList || StripPatches(state, shaders) || !RecordDraws() || recorder == nullptr || DumpTargetLimit() != 0) return miss(DrawRecipeMiss::NotRecordable);
    PreparedDraw prepared(profile);
    prepared.pipelineStateReuse = pipelineStateReuse;
    if (identity != nullptr && !draw.indirect && AgcDriver::ArchB::DirectDrawEnabled() && archcSlots::active()) {
        prepared.identity = identity;
        if (archcSlots::tryReplay(context, state, draw, shaders, *identity, trace, pipelineStateReuse, prepared)) {
            result.recorded = true;
            return result;
        }
        archcSlots::counters().legacy.fetch_add(1, std::memory_order_relaxed);
    }
    if (recipe.hasDepth != state.depth.has_value()) return miss(DrawRecipeMiss::TargetGone);
    const std::size_t expectedViews = state.colors.size() + (recipe.hasDepth ? 1u : 0u);
    Require(recipe.targets.size() == state.colors.size() && recipe.targetViews.size() == expectedViews, "draw recipe targets do not match the state");
    const auto plan = planDraw(context, state, draw, shaders, &recipe, outcome, timer, preparation);
    DrawInputs inputs;
    if (plan.nothing) inputs.nothing = true;
    else inputs = prepareDrawInputs(context, state, draw, shaders, plan, timer, &timing);
    if (inputs.nothing) {
        result.recorded = true;
        return result;
    }
    reuseCensus::mark(StructCensus::ReuseOther);
    // The resident-target proof on the stored images (design_cpu_final 3.7): while an image is
    // still the storage cache's for its surface (StorageImageCached: the cache entry, removed under
    // the cache mutex before an eviction's flush, and the LRU touch a lookup would make), it is
    // what the lookup would return, and Refresh brings it up to date exactly as the lookup's does
    // (FlushPending, the whole-surface collect and UnchangedSince, the key scan). An image gone
    // from the cache is a miss: the ordinary path looks up anew.
    std::vector<std::shared_ptr<StorageTexture>> targets;
    targets.reserve(recipe.targets.size());
    for (std::size_t index = 0; index < recipe.targets.size(); ++index) {
        timer.phase(PhaseSetup);
        if (state.colors[index].fmaskAddress != 0) GuardOneFragmentFmask(context, state.colors[index]);
        auto stored = recipe.targets[index].lock();
        if (stored == nullptr || !StorageImageCached(context, stored.get()) || !StorageImageServesKeys(*stored, SurfaceForTarget(state.colors[index]).dccAddress)) return miss(DrawRecipeMiss::TargetGone);
        auto fastStored = archcTargets::fastActive() ? archcTargets::tryTarget(context, state.colors[index], stored.get()) : nullptr;
        if (fastStored != nullptr && !archcTargets::verifyHitNow()) {
            timer.phase(PhaseReadTarget);
            targets.push_back(std::move(fastStored));
            continue;
        }
        const archcTargets::VerifyHit verifyHit(fastStored);
        auto* targetPreparation = preparation != nullptr && preparation->device == context.device && index < preparation->targets.size() ? &preparation->targets[index] : nullptr;
        const auto lookup = [&](StorageTexture::RefreshPreparation* targetProof) {
            if (state.colors[index].arrayLastSlice != 0) stored->RefreshLayers(state.colors[index].arraySlice, 1);
            else stored->Refresh(targetProof);
            return stored;
        };
        archcTargets::CensusProbe censusProbe(context, state.colors[index], true);
        const auto censusStart = censusProbe.Active() ? archcTargets::nowNs() : 0;
        archcTargets::LegacyPass legacyPass(context, state.colors[index]);
        auto resident = withTargetLookup(lookup, [&](const TargetLookup& target) { return refreshResidentTarget(context, state, state.colors[index], draw, shaders, outcome, profile, target, targetPreparation); });
        verifyHit.Check(resident);
        legacyPass.Finish(resident);
        if (censusProbe.Active()) {
            const auto blockNs = archcTargets::nowNs() - censusStart;
            const auto prelock = index < 8 ? std::exchange(archcTargets::prelockNs()[index], 0) : 0;
            archcTargets::censusCounters().prelockNs.fetch_add(prelock, std::memory_order_relaxed);
            censusProbe.After(resident, blockNs, prelock);
        }
        timer.phase(PhaseReadTarget);
        if (resident == nullptr) return miss(DrawRecipeMiss::TargetGone);
        targets.push_back(std::move(resident));
    }
    if (recipe.hasDepth) {
        timer.phase(PhaseSetup);
        const auto depthView = DepthSurfaceViewWithStencilWriteIntent(context, *state.depth, state.depthWrite, state.stencilTest, writesStencilTarget(state));
        timer.phase(PhaseReadTarget);
        if (recipe.targetViews.back() != depthView) return miss(DrawRecipeMiss::TargetGone);
    }
    timer.phase(PhasePrepare);
    reuseCensus::mark(StructCensus::ReuseTargets);
    // The template's proof (rules R6/R7): ProveCurrent, T1 included, the alias checks the trimmed
    // key leaves to a hit repeated; a failure removes the template from the cache (the batch keeps
    // it) and the caller rebuilds.
    auto resources = recipe.templateRef.lock();
    if (resources == nullptr) return miss(DrawRecipeMiss::TemplateGone);
    Require(resources->Reusable(), "draw recipe over a non-reusable template");
    reuseCensus::mark(StructCensus::ReuseLookup);
    const auto proofStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const bool proved = resources->ProveCurrent(shaders, &result.proof);
    reuseCensus::mark(StructCensus::ReuseProof);
    if (profile) result.proofUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - proofStart).count();
    if (!proved) {
        SharedResourceCache().Remove(recipe.key, resources.get());
        countCache(&DrawProfile::cacheInvalidated);
        resources->KeepStorageImageBytes(*recorder);
        recorder->Keep(std::move(resources));
        timer.phase(PhaseLookup);
        return miss(DrawRecipeMiss::Proof);
    }
    CheckBufferAliases(shaders, state.color, draw.indexAddress, inputs.indexBytes);
    std::vector<ShaderResources::MovedBuffer> moved;
    if (DrawMovedRecipes()) {
        auto current = resources->MovedReadOnlyBuffers(shaders, *recorder, &state.color);
        if (!current.has_value()) {
            if (profile) countCache(&DrawProfile::movedRecipeRefused);
            result.proof.failure = ShaderResources::ProofFailure::Other;
            return miss(DrawRecipeMiss::Proof);
        }
        moved = std::move(*current);
        if (profile) outcome.movedBuffers = moved.size();
    }
    if (recipe.hasDepth && writesDepthTarget(state)) NoteDepthSurfaceWrite(context, *state.depth);
    SharedResourceCache().Touch(recipe.key);
    countCache(&DrawProfile::cacheHits);
    timer.phase(PhaseLookup);
    if (Stage4::ResolveCensus()) Stage4::NoteTemplate(resources.get(), false);
    outcome.completion = resources->ChecksFaults();
    outcome.recorded = true;
    // The pipeline is the recipe's while the pipeline store still holds it (its device's teardown
    // or its eviction bound drops it: a miss). The framebuffer is the recipe's while the pipeline's
    // list holds it: the views are the stored targets' (stable while they live, and they are the
    // same objects), so AcquireFramebuffer only runs when the list evicted it.
    auto pipeline = recipe.pipeline.lock();
    if (pipeline == nullptr) return miss(DrawRecipeMiss::ObjectsGone);
    auto framebuffer = recipe.framebuffer.lock();
    if (framebuffer == nullptr) {
        const std::vector<std::shared_ptr<void>> framebufferOwners(targets.begin(), targets.end());
        framebuffer = pipeline->AcquireFramebuffer(recipe.targetViews, framebufferOwners, state.renderExtent, state.renderLayers);
    }
    timer.phase(PhasePipeline);
    reuseCensus::mark(StructCensus::ReuseLookup);
    const auto recordStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    RecordedDraw record;
    record.recorder = recorder;
    record.completion = outcome.completion;
    record.resources = std::move(resources);
    record.moved = std::move(moved);
    record.pipeline = std::move(pipeline);
    record.framebuffer = std::move(framebuffer);
    record.targetViews = recipe.targetViews;
    record.targets = std::move(targets);
    for (const auto& color : state.colors) record.targetSlices.emplace_back(color.arraySlice, color.arrayLastSlice != 0 ? color.layers : 1u);
    auto pushBytes = recipe.pushBytes;
    if (recipe.pushStages != 0) {
        pushBytes = AssemblePushConstants(shaders);
        record.resources->PatchPushConstants(pushBytes);
    }
    record.pushBytes = &pushBytes;
    record.pushStages = recipe.pushStages;
    record.depthLocations = passDepthLocations(context, state, *record.pipeline);
    std::shared_ptr<std::shared_ptr<const void>> spaceKeep;
    record.spaceKeep = &spaceKeep;
    const bool slotCapture = archcSlots::wantsCapture(prepared);
    SlotPassCapture passCapture;
    std::optional<std::array<std::uint64_t, archcSlots::ComponentCount>> predicted;
    if (slotCapture) {
        prepared.recorder = recorder;
        prepared.recorded = true;
        prepared.lean = true;
        prepared.completion = record.completion;
        prepared.resolved.resources = record.resources;
        prepared.resolved.moved.assign(record.moved.begin(), record.moved.end());
        prepared.resolved.contentKey = recipe.key;
        prepared.resolved.cacheable = true;
        prepared.pipeline = record.pipeline;
        prepared.framebuffer = record.framebuffer;
        prepared.targetViews = recipe.targetViews;
        prepared.depthLocations = record.depthLocations;
        prepared.inputs = inputs;
        prepared.targets.resize(record.targets.size());
        for (std::size_t index = 0; index < record.targets.size(); ++index) {
            prepared.targets[index].color = state.colors[index];
            prepared.targets[index].resident = record.targets[index];
            prepared.targets[index].proxied = AttachmentProxyFormat(context, state.colors[index].format) != VK_FORMAT_UNDEFINED;
            prepared.targets[index].multisampled = state.colors[index].samples > 1;
        }
        if (prepared.slotVerify) {
            if (auto* slot = archcSlots::lookup(context.device, state, draw, shaders, *identity, pipelineStateReuse)) predicted = archcSlots::fingerprint(*slot);
        }
        record.slotCapture = &passCapture;
    }
    archcSlots::verifyDirectInputs(prepared, inputs);
    recordDraw(context, state, draw, shaders, inputs, record, outcome, timer, ownWaitedMs, trace, pipelineStateReuse);
    if (slotCapture) {
        prepared.outcome = outcome;
        archcSlots::capture(context, state, draw, shaders, prepared, passCapture, prepared.inputs.vertexBuffers.empty() && prepared.inputs.indices == nullptr, predicted);
    }
    if (profile) {
        result.recordUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - recordStart).count();
        auto& stats = Profile();
        std::lock_guard lock(stats.mutex);
        ++stats.recipeHits;
    }
    timing.Mark("draw_and_resource_release");
    reportDrawEnd(state, timer, nullptr, outcome, waitedBefore, ownWaitedMs, " recorded from recipe");
    result.recorded = true;
    return result;
}

namespace {

std::shared_ptr<StorageTexture> metadataPassResident(const Context& context, const ColorTarget& color) {
    if (ColorTileModeIsLinear(color.tileMode) || context.detiler == nullptr) return nullptr;
    const auto resident = CachedStorageSurface(context, SurfaceForTarget(color));
    return resident != nullptr && resident->GuestBytes() == color.bytes ? resident : nullptr;
}

}

std::vector<std::pair<std::uint64_t, std::uint64_t>> PipelinedColorMetadataPassWrites(const ColorMetadataPass& pass) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> writes;
    if (pass.mode == ColorMetadataPass::Mode::Resolve) {
        if (!pass.source.has_value() || pass.targets.size() != 1) return writes;
        const auto& destination = pass.targets.front();
        if (destination.samples != 1u || destination.bytes == 0 || destination.cmaskAddress != 0) return writes;
        writes.emplace_back(destination.MemoryAddress(), destination.MemoryAddress() + destination.bytes);
        if (destination.dccAddress != 0) writes.emplace_back(destination.dccAddress, destination.dccAddress + std::max<std::size_t>(colorKeyCount(destination, destination.bytes), 1u));
        return writes;
    }
    if (pass.mode != ColorMetadataPass::Mode::EliminateFastClear && pass.mode != ColorMetadataPass::Mode::DccDecompress) return writes;
    for (const auto& color : pass.targets) {
        if (color.samples != 1u || color.bytes == 0) return {};
        writes.emplace_back(color.MemoryAddress(), color.MemoryAddress() + color.bytes);
        if (color.cmaskAddress != 0) writes.emplace_back(color.cmaskAddress, color.cmaskAddress + std::max<std::size_t>(color.cmaskBytes, 1u));
        if (color.dccAddress != 0) writes.emplace_back(color.dccAddress, color.dccAddress + std::max<std::size_t>(colorKeyCount(color, color.bytes), 1u));
    }
    return writes;
}

void RunColorMetadataPass(const Context& context, const ColorMetadataPass& pass) {
    const GpuAuthorityCensus::ProducerScope censusProducer(7);
    if (pass.mode == ColorMetadataPass::Mode::Resolve) {
        Require(pass.source.has_value() && pass.targets.size() == 1, "CB resolve without its source and destination");
        const auto& destination = pass.targets.front();
        Require(!ColorTileModeIsLinear(destination.tileMode) && context.detiler != nullptr, "CB resolve into a target without a resident image is not implemented");
        const auto resident = CachedStorageSurface(context, SurfaceForTarget(destination));
        Require(resident->Attachable(), "storage format cannot be a color attachment");
        ResolveMultisampleTarget(context, *pass.source, *resident, resident->AttachmentView(destination.format, destination.mip));
        return;
    }
    if (pass.mode == ColorMetadataPass::Mode::FmaskDecompress) {
        for (const auto& color : pass.targets) MaterializeMultisampleTargetClear(context, color);
        return;
    }
    for (const auto& color : pass.targets) {
        if (color.cmaskAddress != 0) {
            const auto resident = metadataPassResident(context, color);
            if (materializeCmaskClear(context, color, resident) && resident != nullptr) resident->MarkDirty();
        }
        if (color.dccAddress == 0) continue;
        auto keys = CurrentDccKeys(color.dccAddress, color.bytes);
        if (keys == DccKeys::Uncompressed) continue;
        if (keys == DccKeys::ClearSingle) {
            Require(pass.mode == ColorMetadataPass::Mode::DccDecompress, "CB fast-clear eliminate over comp-to-single DCC keys (whether it expands them is not modeled)");
            storeSingleTexels(context, color);
            if (const auto resident = metadataPassResident(context, color)) resident->Refresh();
            continue;
        }
        Require(IsDccClear(keys), std::string("CB metadata pass over DCC keys that are ") + DccKeysName(keys) + " (per-block metadata is not modeled)");
        const auto texel = clearTexel(color, keys);
        const auto resident = metadataPassResident(context, color);
        if (resident != nullptr) {
            const char* refusal = nullptr;
            const bool current = keys == DccKeys::ClearRegister ? clearToTexel(*resident, texel, color.elementBytes, refusal, true) : StorageTexture::FindPending(color.address, color.bytes) == resident || resident->UploadedKeys() == keys;
            if (current) {
                resident->MarkDirty();
                MarkDccUncompressed(context, color.dccAddress, color.bytes, colorKeyCount(color, color.bytes));
                continue;
            }
        }
        storeClearTexels(context, color, texel);
    }
}

AddressDrawReuseCounts AddressDrawReuseCounters() {
    AddressDrawReuseCounts counts{};
    const auto& counters = addressDrawReuses();
    for (std::size_t index = 0; index < counts.size(); ++index) counts[index] = counters[index].load(std::memory_order_relaxed);
    return counts;
}

const char* AddressDrawReuseName(AddressDrawReuse outcome) {
    return static_cast<std::size_t>(outcome) < AddressDrawReuseNames.size() ? AddressDrawReuseNames[static_cast<std::size_t>(outcome)] : "?";
}

void SetSpaceKeepFailuresForTest(std::uint32_t count) {
    spaceKeepFailures.store(count, std::memory_order_relaxed);
}

void SetDrawStopAfterResolveForTest(DrawStopForTest stop) {
    stopAfterResolve.store(static_cast<int>(stop), std::memory_order_relaxed);
}

std::uint64_t AddressDrawReuseVerifications() {
    return addressDrawReuseVerifications().load(std::memory_order_relaxed);
}

bool DrawPipelineEnabled() {
    static const bool enabled = std::getenv("APS5_DRAW_PIPELINE") != nullptr;
    return enabled;
}

bool CommitTrims() {
    return DrawPipelineEnabled();
}

VkDrawCounts TakeVkDrawCounts() {
    VkDrawCounts counts;
    counts.direct = vkDrawCounts[static_cast<std::size_t>(VkDrawKind::Direct)].exchange(0, std::memory_order_relaxed);
    counts.indexed = vkDrawCounts[static_cast<std::size_t>(VkDrawKind::Indexed)].exchange(0, std::memory_order_relaxed);
    counts.indirect = vkDrawCounts[static_cast<std::size_t>(VkDrawKind::Indirect)].exchange(0, std::memory_order_relaxed);
    counts.mesh = vkDrawCounts[static_cast<std::size_t>(VkDrawKind::Mesh)].exchange(0, std::memory_order_relaxed);
    return counts;
}


static_assert(PhaseCount == DrawGraphicsPhaseCount);

const char* DrawGraphicsPhaseName(std::size_t phase) {
    return phase < PhaseCount ? DrawPhaseNames[phase] : "unknown";
}

bool TakeLastDrawPhases(std::array<double, DrawGraphicsPhaseCount>& us) {
    auto& last = lastDrawPhases();
    if (!last.set) return false;
    us = last.us;
    last.set = false;
    return true;
}

// APS5_DRAW_BINDINGS_DIAG: a "[draw-lookup-diag]" line (cumulative ns per step); empty when off.
std::string DrawLookupDiagText() {
    if (!Recorder::SnapshotDiagEnabled()) return {};
    auto& counters = lookupDiag::counters();
    std::string text = "[draw-lookup-diag] cumulative: draws " + std::to_string(counters.draws.load(std::memory_order_relaxed)) + " template_hits " + std::to_string(counters.hits.load(std::memory_order_relaxed));
    for (std::size_t step = 0; step < lookupDiag::Steps; ++step) text += std::string(" ") + lookupDiag::StepNames[step] + "_ns " + std::to_string(counters.ns[step].load(std::memory_order_relaxed));
    return text;
}

// APS5_ARCHB_PIPE_DRAW_LOOKUP_MEMO: a "[draw-lookup-memo]" cumulative line.
std::string DrawLookupMemoText() {
    auto& counters = lookupMemo::counters();
    const auto get = [](const std::atomic<std::uint64_t>& value) { return std::to_string(value.load(std::memory_order_relaxed)); };
    return "[draw-lookup-memo] cumulative: hits " + get(counters.hits) + " misses " + get(counters.misses) + " stale " + get(counters.stale) + " touches " + get(counters.touches) + " verified " + get(counters.verified) + " mismatches " + get(counters.mismatches);
}
}
