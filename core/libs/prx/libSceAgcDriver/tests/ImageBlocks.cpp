#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <thread>

using namespace AgcDriver::Graphics;

namespace {

constexpr VkDeviceSize MiB = VkDeviceSize{1} << 20u;
int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

struct MockDevice {
    std::uintptr_t next = 1;
    std::map<VkDeviceMemory, VkDeviceSize> memory;
    std::map<VkDeviceMemory, std::uint32_t> types;
    std::map<VkDeviceMemory, VkImage> dedicated;
    std::map<VkImage, std::pair<bool, bool>> hints;
    std::map<VkImage, std::pair<VkDeviceMemory, VkDeviceSize>> bound;
    VkDeviceSize liveBytes = 0;
    VkDeviceSize limit = 0;
    VkDeviceSize heapBudget = 4096 * MiB;
    VkDeviceSize otherUsage = 0;
    unsigned allocations = 0;
    unsigned frees = 0;
    unsigned attempts = 0;
    unsigned queries = 0;
    VkResult bindResult = VK_SUCCESS;
    VkResult allocationResult = VK_SUCCESS;
    std::map<VkFence, bool> signaled;
    std::uint64_t submits = 0;
    std::uint64_t fenceWaits = 0;
    std::uint64_t allocatedCommands = 0;
    std::uint64_t freedCommands = 0;
    VkResult endResult = VK_SUCCESS;
    VkResult submitResult = VK_SUCCESS;
    bool lostSubmissionPending = false;
    std::uint64_t idleWaits = 0;
    VkResult idleResult = VK_SUCCESS;
};

MockDevice mock;

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateMemory(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* memory) {
    ++mock.attempts;
    if (mock.allocationResult != VK_SUCCESS) return mock.allocationResult;
    if (mock.limit != 0 && mock.liveBytes + info->allocationSize > mock.limit) {
        *memory = reinterpret_cast<VkDeviceMemory>(std::uintptr_t{0xdead});
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *memory = reinterpret_cast<VkDeviceMemory>(mock.next++);
    mock.memory[*memory] = info->allocationSize;
    mock.types[*memory] = info->memoryTypeIndex;
    for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next != nullptr; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) mock.dedicated[*memory] = reinterpret_cast<const VkMemoryDedicatedAllocateInfo*>(next)->image;
    }
    mock.liveBytes += info->allocationSize;
    ++mock.allocations;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    Expect(mock.memory.contains(memory), "unknown or duplicate memory release");
    for (const auto& [image, binding] : mock.bound) Expect(binding.first != memory, "memory freed before its image was destroyed");
    mock.liveBytes -= mock.memory.at(memory);
    mock.memory.erase(memory);
    mock.types.erase(memory);
    mock.dedicated.erase(memory);
    ++mock.frees;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyImage(VkDevice, VkImage image, const VkAllocationCallbacks*) {
    mock.bound.erase(image);
    mock.hints.erase(image);
}

VKAPI_ATTR VkResult VKAPI_CALL mockBindImageMemory(VkDevice, VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
    Expect(mock.memory.contains(memory) && offset < mock.memory.at(memory), "binding outside an allocation");
    if (mock.dedicated.contains(memory)) Expect(mock.dedicated.at(memory) == image && offset == 0, "dedicated allocation reused for another image");
    if (mock.bindResult == VK_SUCCESS) mock.bound[image] = {memory, offset};
    return mock.bindResult;
}

VKAPI_ATTR void VKAPI_CALL mockRequirements(VkDevice, const VkImageMemoryRequirementsInfo2* info, VkMemoryRequirements2* requirements) {
    ++mock.queries;
    auto* hint = static_cast<VkMemoryDedicatedRequirements*>(requirements->pNext);
    hint->requiresDedicatedAllocation = mock.hints.at(info->image).first;
    hint->prefersDedicatedAllocation = mock.hints.at(info->image).second;
}

VKAPI_ATTR void VKAPI_CALL mockMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2* properties) {
    auto* budget = static_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(properties->pNext);
    budget->heapBudget[0] = mock.heapBudget;
    budget->heapUsage[0] = mock.otherUsage + mock.liveBytes;
}

VKAPI_ATTR void VKAPI_CALL mockUnmapMemory(VkDevice, VkDeviceMemory) {}
VKAPI_ATTR void VKAPI_CALL mockDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL mockMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void**) { return VK_ERROR_MEMORY_MAP_FAILED; }

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* commands) {
    *commands = reinterpret_cast<VkCommandBuffer>(mock.next++);
    ++mock.allocatedCommands;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockFreeCommandBuffers(VkDevice, VkCommandPool, std::uint32_t count, const VkCommandBuffer*) {
    Expect(!mock.lostSubmissionPending, "command buffers were freed before the lost submission finished");
    mock.freedCommands += count;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence) {
    *fence = reinterpret_cast<VkFence>(mock.next++);
    mock.signaled[*fence] = false;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*) {
    Expect(!mock.lostSubmissionPending, "a fence was destroyed before the lost submission finished");
    mock.signaled.erase(fence);
}

VKAPI_ATTR VkResult VKAPI_CALL mockGetFenceStatus(VkDevice, VkFence fence) {
    return mock.signaled.at(fence) ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL mockWaitForFences(VkDevice, std::uint32_t count, const VkFence* fences, VkBool32, std::uint64_t) {
    for (std::uint32_t i = 0; i < count; ++i) mock.signaled.at(fences[i]) = true;
    ++mock.fenceWaits;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockResetFences(VkDevice, std::uint32_t count, const VkFence* fences) {
    Expect(!mock.lostSubmissionPending, "a fence was reset before the lost submission finished");
    for (std::uint32_t i = 0; i < count; ++i) mock.signaled.at(fences[i]) = false;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockEndCommandBuffer(VkCommandBuffer) {
    return mock.endResult;
}

VKAPI_ATTR VkResult VKAPI_CALL mockQueueSubmit(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence) {
    ++mock.submits;
    if (mock.submitResult == VK_ERROR_DEVICE_LOST) mock.lostSubmissionPending = true;
    return mock.submitResult;
}

VKAPI_ATTR VkResult VKAPI_CALL mockDeviceWaitIdle(VkDevice) {
    ++mock.idleWaits;
    mock.lostSubmissionPending = false;
    return mock.idleResult;
}

VKAPI_ATTR void VKAPI_CALL mockCmdUpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdBeginQuery(VkCommandBuffer, VkQueryPool, std::uint32_t, VkQueryControlFlags) {}
VKAPI_ATTR void VKAPI_CALL mockCmdEndQuery(VkCommandBuffer, VkQueryPool, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdResetQueryPool(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdCopyQueryPoolResults(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t, VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags) {}
VKAPI_ATTR void VKAPI_CALL mockCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL mockCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t, std::uint32_t, const void*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdDispatch(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t) {}

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(mockAllocateMemory)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(mockFreeMemory)},
        {"vkDestroyImage", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyImage)},
        {"vkBindImageMemory", reinterpret_cast<PFN_vkVoidFunction>(mockBindImageMemory)},
        {"vkGetImageMemoryRequirements2", reinterpret_cast<PFN_vkVoidFunction>(mockRequirements)},
        {"vkUnmapMemory", reinterpret_cast<PFN_vkVoidFunction>(mockUnmapMemory)},
        {"vkMapMemory", reinterpret_cast<PFN_vkVoidFunction>(mockMapMemory)},
        {"vkDestroyBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyBuffer)},
        {"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(mockAllocateCommandBuffers)},
        {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(mockFreeCommandBuffers)},
        {"vkCreateFence", reinterpret_cast<PFN_vkVoidFunction>(mockCreateFence)},
        {"vkDestroyFence", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyFence)},
        {"vkGetFenceStatus", reinterpret_cast<PFN_vkVoidFunction>(mockGetFenceStatus)},
        {"vkWaitForFences", reinterpret_cast<PFN_vkVoidFunction>(mockWaitForFences)},
        {"vkResetFences", reinterpret_cast<PFN_vkVoidFunction>(mockResetFences)},
        {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockBeginCommandBuffer)},
        {"vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockEndCommandBuffer)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(mockQueueSubmit)},
        {"vkDeviceWaitIdle", reinterpret_cast<PFN_vkVoidFunction>(mockDeviceWaitIdle)},
        {"vkCmdUpdateBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockCmdUpdateBuffer)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPipelineBarrier)},
        {"vkCmdBeginQuery", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBeginQuery)},
        {"vkCmdEndQuery", reinterpret_cast<PFN_vkVoidFunction>(mockCmdEndQuery)},
        {"vkCmdResetQueryPool", reinterpret_cast<PFN_vkVoidFunction>(mockCmdResetQueryPool)},
        {"vkCmdCopyQueryPoolResults", reinterpret_cast<PFN_vkVoidFunction>(mockCmdCopyQueryPoolResults)},
        {"vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBindPipeline)},
        {"vkCmdPushConstants", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPushConstants)},
        {"vkCmdDispatch", reinterpret_cast<PFN_vkVoidFunction>(mockCmdDispatch)},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}

Context MakeContext() {
    Expect(mock.liveBytes == 0, "previous case leaked device memory");
    mock = {};
    Context context{};
    context.device = reinterpret_cast<VkDevice>(mock.next++);
    context.physical = reinterpret_cast<VkPhysicalDevice>(mock.next++);
    context.queue = reinterpret_cast<VkQueue>(mock.next++);
    context.deviceProc = mockProc;
    context.memoryProperties2 = mockMemoryProperties;
    context.limits.maxMemoryAllocationCount = 4096;
    context.memory.memoryHeapCount = 2;
    context.memory.memoryHeaps[0] = {4096 * MiB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    context.memory.memoryHeaps[1] = {4096 * MiB, 0};
    context.memory.memoryTypeCount = 3;
    context.memory.memoryTypes[0] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    context.memory.memoryTypes[1] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1};
    context.memory.memoryTypes[2] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    GetBufferPool(context);
    return context;
}

std::shared_ptr<OwnedImage> MakeImage(const Context& context, VkDeviceSize size = 4 * MiB, VkDeviceSize alignment = 65536, std::uint32_t mask = 1, bool required = false, bool preferred = false) {
    const auto image = reinterpret_cast<VkImage>(mock.next++);
    mock.hints[image] = {required, preferred};
    auto owner = std::make_shared<OwnedImage>(context, image, VK_NULL_HANDLE);
    const VkMemoryRequirements requirements{size, alignment, mask};
    const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, size, ImageSuballocationLevel() == 0 ? context.DeviceMemoryType(mask, size, "test image") : 0};
    owner->Allocate(context, allocation, requirements, "vkAllocateMemory test image");
    Check(mockBindImageMemory(context.device, image, owner->memory, owner->memoryRange.Offset()), "vkBindImageMemory test image");
    return owner;
}

void Ranges() {
    ImageRangeAllocator ranges(1024);
    Expect(ranges.Allocate(64, 1) == 0, "first allocation offset");
    Expect(ranges.Allocate(128, 256) == 256, "aligned allocation offset");
    Expect(ranges.Allocate(128, 64) == 64, "best fit did not use the alignment gap");
    Expect(ranges.Allocate(64, 1) == 192, "remaining alignment gap was lost");
    Expect(ranges.Free(64, 128) && ranges.Free(192, 64), "adjacent ranges could not be freed");
    Expect(ranges.Free(0, 64) && ranges.Free(256, 128), "neighbour coalescing failed");
    Expect(ranges.Empty() && ranges.LargestFree() == 1024, "freed neighbours did not form one range");
    Expect(ranges.Allocate(1024, 256) == 0 && !ranges.Allocate(1, 1), "coalesced capacity was not reusable");
    Expect(ranges.Free(0, 1024) && !ranges.Free(0, 1024), "double free accepted");
    Expect(!ranges.Allocate(1025, 1) && !ranges.Allocate(0, 1) && !ranges.Allocate(1, 0), "invalid allocation accepted");
    ImageRangeAllocator aligned(64);
    Expect(aligned.Allocate(1, 1) == 0 && aligned.Allocate(32, 32) == 32, "alignment padding was not retained");
    Expect(!aligned.Allocate(31, 32) && aligned.Allocate(31, 1) == 1, "alignment fit was tested without its padding");
}

void SwitchOff() {
    auto context = MakeContext();
    auto first = MakeImage(context);
    const auto memory = first->memory;
    Expect(!first->memoryRange.Valid() && mock.memory.at(memory) == 4 * MiB, "switch off changed allocation size");
    first.reset();
    Expect(context.bufferPool->RetainedImages() == 1 && mock.frees == 0, "switch off changed retention");
    auto second = MakeImage(context);
    Expect(second->memory == memory && mock.allocations == 1, "switch off stopped whole-allocation reuse");
    Expect(mock.queries == 0 && context.bufferPool->ImageStats().blocksMade == 0, "switch off queried dedicated requirements or created blocks");
    Expect(mock.bound.at(second->image).second == 0, "switch off bound at a nonzero offset");
}

void Sharing() {
    auto context = MakeContext();
    auto first = MakeImage(context, MiB, MiB);
    auto second = MakeImage(context, MiB, 4 * MiB);
    auto third = MakeImage(context, 2 * MiB, MiB);
    Expect(first->memory == second->memory && first->memory == third->memory && mock.allocations == 1, "eligible images did not share a block");
    Expect(second->memoryRange.Offset() == 4 * MiB && third->memoryRange.Offset() == MiB, "best fit did not reuse aligned block padding");
    const auto stats = context.bufferPool->ImageStats();
    Expect(stats.used == 4 * MiB && stats.reserved == 256 * MiB, "image bytes were counted more than once");
    Expect(ImageBlockSlack(context, 0) == 252 * MiB && ImageBlockSlack(context, 1) == 0, "slack escaped its heap or counted used bytes");
    first.reset();
    second.reset();
    third.reset();
    Expect(context.bufferPool->RetainedImages() == 0, "a suballocation entered the whole-allocation pool");
    Expect(context.bufferPool->ImageStats().largestFree == 256 * MiB, "returned ranges did not coalesce");
    auto reused = MakeImage(context, 64 * MiB);
    Expect(reused->memoryRange.Offset() == 0 && mock.allocations == 1, "coalesced block was not reused");
}

void Fallbacks() {
    auto context = MakeContext();
    auto exact = MakeImage(context, 64 * MiB);
    auto large = MakeImage(context, 64 * MiB + 1);
    Expect(exact->memoryRange.Valid() && !large->memoryRange.Valid(), "64 MiB eligibility boundary changed");
    const auto largeMemory = large->memory;
    large.reset();
    large = MakeImage(context, 64 * MiB + 1);
    Expect(large->memory == largeMemory, "large fallback bypassed whole-allocation reuse");
    auto required = MakeImage(context, MiB, 65536, 1, true, true);
    Expect(!required->memoryRange.Valid() && required->memoryDedicated && mock.dedicated.at(required->memory) == required->image, "required-dedicated image was not dedicated");
    const auto requiredMemory = required->memory;
    required.reset();
    Expect(!mock.memory.contains(requiredMemory), "required-dedicated memory was retained for another image");
    auto preferred = MakeImage(context, 2 * MiB, 65536, 1, false, true);
    Expect(preferred->memoryRange.Valid() == (ImageSuballocationLevel() == 2), "dedicated preference gate changed");
    if (ImageSuballocationLevel() == 1) {
        const auto preferredMemory = preferred->memory;
        preferred.reset();
        preferred = MakeImage(context, 2 * MiB, 65536, 1, false, true);
        Expect(preferred->memory == preferredMemory, "preferred fallback bypassed whole-allocation reuse");
    }
    const auto stats = context.bufferPool->ImageStats();
    Expect(stats.fallbacks[static_cast<std::size_t>(ImageFallback::Required)] == 1 && stats.fallbacks[static_cast<std::size_t>(ImageFallback::Large)] == 2, "fallback reasons not counted");
}

void BudgetFallback() {
    auto context = MakeContext();
    mock.heapBudget = 1024 * MiB;
    mock.otherUsage = 900 * MiB;
    auto image = MakeImage(context);
    Expect(!image->memoryRange.Valid() && mock.allocations == 1 && mock.memory.at(image->memory) == 4 * MiB, "block exceeded the heap budget");
    const auto memory = image->memory;
    image.reset();
    image = MakeImage(context);
    Expect(image->memory == memory, "budget fallback bypassed whole-allocation reuse");
    mock.otherUsage = 0;
    auto roomy = MakeImage(context);
    Expect(roomy->memoryRange.Valid(), "block allocation did not resume after budget pressure");
    mock.otherUsage = mock.heapBudget;
    auto existing = MakeImage(context);
    Expect(existing->memory == roomy->memory, "budget pressure prevented using an existing block");
}

void AllocationFailure() {
    auto context = MakeContext();
    mock.limit = 100 * MiB;
    auto first = MakeImage(context);
    Expect(!first->memoryRange.Valid() && mock.attempts == 2, "block failure did not fall back to a whole allocation");
    auto second = MakeImage(context);
    Expect(!second->memoryRange.Valid() && mock.attempts == 3, "failed block allocation retried without backoff");
    Expect(context.bufferPool->ImageStats().reserved == 0, "failed block was counted as reserved memory");
}

void HostFallback() {
    auto context = MakeContext();
    mock.otherUsage = mock.heapBudget;
    auto image = MakeImage(context, 4 * MiB, 65536, 3);
    Expect(!image->memoryRange.Valid() && mock.types.at(image->memory) == 1, "heap pressure lost the existing host-memory fallback");
    image.reset();
    Expect(mock.liveBytes == 0 && context.bufferPool->RetainedImages() == 0, "host fallback entered the device-local image pool");
}

void Spares() {
    auto context = MakeContext();
    std::vector<std::shared_ptr<OwnedImage>> images;
    for (int i = 0; i < 9; ++i) images.push_back(MakeImage(context, 64 * MiB));
    Expect(mock.allocations == 3, "images did not fill blocks before growing");
    images.clear();
    Expect(mock.frees == 2 && mock.liveBytes == 256 * MiB && context.bufferPool->ImageStats().emptyBlocks == 1, "more than one spare survived");
    mock.limit = 300 * MiB;
    auto anotherType = MakeImage(context, 4 * MiB, 65536, 4);
    Expect(anotherType->memoryRange.Valid() && mock.types.at(anotherType->memory) == 2 && mock.frees == 3, "allocation pressure did not release the spare");
    anotherType.reset();
    const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, 128 * MiB, 0};
    const auto memory = context.bufferPool->AllocateMemory(allocation, "test pressure");
    Expect(mock.liveBytes == 128 * MiB && context.bufferPool->ImageStats().reserved == 0, "general allocation failure did not trim image blocks");
    mockFreeMemory(context.device, memory, nullptr);
}

void FailedBind() {
    auto context = MakeContext();
    mock.bindResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    bool threw = false;
    try { MakeImage(context); } catch (const std::runtime_error&) { threw = true; }
    Expect(threw && context.bufferPool->ImageStats().used == 0 && context.bufferPool->RetainedImages() == 0, "failed bind leaked or pooled a range");
    mock.bindResult = VK_SUCCESS;
    auto next = MakeImage(context);
    Expect(next->memoryRange.Offset() == 0 && mock.allocations == 1, "failed binding's range was not reusable");
}

void DeviceLost() {
    auto context = MakeContext();
    mock.allocationResult = VK_ERROR_DEVICE_LOST;
    bool threw = false;
    try { MakeImage(context); } catch (const std::runtime_error&) { threw = true; }
    Expect(threw && mock.attempts == 1 && context.bufferPool->ImageStats().blocks == 0, "device loss was treated as an allocation fallback");
}

void ConcurrentRanges() {
    auto context = MakeContext();
    std::mutex mutex;
    std::map<std::pair<VkDeviceMemory, VkDeviceSize>, VkDeviceSize> live;
    const VkMemoryRequirements requirements{MiB, 65536, 1};
    const auto worker = [&] {
        for (unsigned i = 0; i < 200; ++i) {
            auto range = context.bufferPool->TakeImageRange(context.bufferPool, requirements, 0);
            const auto key = std::pair{range.Memory(), range.Offset()};
            {
                std::lock_guard lock(mutex);
                Expect(range.Valid(), "concurrent range allocation failed");
                for (const auto& [other, size] : live) {
                    Expect(other.first != key.first || other.second + size <= key.second || key.second + requirements.size <= other.second, "concurrent ranges overlap");
                }
                live.emplace(key, requirements.size);
            }
            std::this_thread::yield();
            {
                std::lock_guard lock(mutex);
                live.erase(key);
            }
        }
    };
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 4; ++i) threads.emplace_back(worker);
    for (auto& thread : threads) thread.join();
    const auto stats = context.bufferPool->ImageStats();
    Expect(stats.allocations == 800 && stats.frees == 800 && stats.used == 0 && stats.blocks == 1, "concurrent ranges leaked or grew unnecessary blocks");
}

void Completion() {
    auto context = MakeContext();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(context);
    auto first = MakeImage(context);
    const auto memory = first->memory;
    const auto offset = first->memoryRange.Offset();
    std::weak_ptr<OwnedImage> held = first;
    recorder.Keep(first);
    first.reset();
    recorder.Submit();
    Expect(!recorder.Reap() && !held.expired(), "image released before its fence completed");
    auto second = MakeImage(context);
    Expect(second->memory != memory || second->memoryRange.Offset() != offset, "an unfinished image's range was reused");
    for (auto& [fence, signaled] : mock.signaled) signaled = true;
    Expect(recorder.Reap(), "signaled submission did not complete");
    recorder.FlushDeferredReleases();
    Expect(held.expired(), "completed image survived deferred-release drain");
    auto third = MakeImage(context);
    Expect(third->memory == memory && third->memoryRange.Offset() == offset, "completed image's memory was not reused");
}

}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "ranges") {
            Ranges();
        } else if (argc > 1 && std::string_view(argv[1]) == "off") {
            Expect(ImageSuballocationLevel() == 0, "invalid or off switch enabled suballocation");
            SwitchOff();
            Completion();
        } else {
            Expect(ImageSuballocationLevel() == 1 || ImageSuballocationLevel() == 2, "enabled test has no switch");
            Sharing();
            Fallbacks();
            BudgetFallback();
            AllocationFailure();
            HostFallback();
            Spares();
            FailedBind();
            DeviceLost();
            ConcurrentRanges();
            Completion();
        }
        Expect(mock.liveBytes == 0, "device memory leaked after teardown");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
