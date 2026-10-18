#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ImageMemory.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

constexpr std::uint64_t KiB = std::uint64_t{1} << 10u;
constexpr std::uint64_t MiB = std::uint64_t{1} << 20u;

struct Random {
    std::uint64_t state;
    std::uint64_t Next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return state >> 33u;
    }
};

void Allocation() {
    RangeAllocator ranges(1024);
    const auto first = ranges.Allocate(100, 1);
    const auto second = ranges.Allocate(200, 1);
    const auto third = ranges.Allocate(24, 1);
    Expect(first && *first == 0 && second && *second == 100 && third && *third == 300, "sequential allocations did not pack from the start");
    Expect(ranges.Used() == 324 && ranges.Available() == 700 && ranges.Capacity() == 1024 && !ranges.Empty(), "usage counters are wrong after three allocations");
    Expect(ranges.LargestFree() == 700 && ranges.FreeRanges() == 1, "the free tail is not one range");
    Expect(!ranges.Allocate(701, 1), "an allocation larger than the free range succeeded");
    Expect(ranges.Allocate(700, 1).value_or(0) == 324, "the whole free tail could not be allocated");
    Expect(!ranges.Allocate(1, 1) && ranges.LargestFree() == 0 && ranges.FreeRanges() == 0, "a full block served another allocation");
    Expect(ranges.Allocate(0, 0).has_value() == false, "an allocation from a full block succeeded");
    RangeAllocator none(0);
    Expect(!none.Allocate(1, 1) && none.Empty() && none.LargestFree() == 0, "a zero-capacity allocator served an allocation");
}

void Alignment() {
    RangeAllocator ranges(1 << 20);
    Expect(ranges.Allocate(1, 1).value_or(99) == 0, "the first allocation is not at the start");
    const auto aligned = ranges.Allocate(100, 4096);
    Expect(aligned && *aligned == 4096, "an aligned allocation did not start at the next boundary");
    Expect(ranges.FreeRanges() == 2 && ranges.Used() == 101, "the padding before an aligned allocation was lost");
    const auto filler = ranges.Allocate(4000, 1);
    Expect(filler && *filler == 1, "the padding before an aligned allocation was not reusable");
    const auto odd = ranges.Allocate(10, 3000);
    Expect(odd && *odd % 3000 == 0 && *odd >= 4196, "a non-power-of-two alignment was not honoured");
    Expect(ranges.Allocate(8, 0).has_value(), "a zero alignment was refused");
    RangeAllocator small(4096);
    Expect(small.Allocate(4000, 1).value_or(99) == 0, "setup allocation failed");
    Expect(!small.Allocate(10, 4096), "an allocation whose aligned start lies past the free range succeeded");
    Expect(!small.Allocate(96, 128), "an allocation that only fits unaligned succeeded");
    RangeAllocator huge(4096);
    Expect(huge.Allocate(16, 1 << 20).value_or(99) == 0, "an alignment larger than the block refused offset zero");
    Expect(!huge.Allocate(16, 1 << 20), "a second allocation with an alignment larger than the block succeeded");
}

void Coalescing() {
    RangeAllocator ranges(1000);
    const auto a = *ranges.Allocate(100, 1);
    const auto b = *ranges.Allocate(100, 1);
    const auto c = *ranges.Allocate(100, 1);
    const auto d = *ranges.Allocate(100, 1);
    Expect(ranges.Free(b, 100) && ranges.FreeRanges() == 2, "freeing a middle range did not leave a separate free range");
    Expect(ranges.Free(d, 100) && ranges.FreeRanges() == 2 && ranges.LargestFree() == 700, "freeing before the tail did not merge into it");
    Expect(ranges.Free(c, 100) && ranges.FreeRanges() == 1 && ranges.LargestFree() == 900, "freeing between two free ranges did not merge both");
    Expect(ranges.Free(a, 100) && ranges.FreeRanges() == 1 && ranges.LargestFree() == 1000 && ranges.Empty() && ranges.Used() == 0, "freeing everything did not leave one whole range");
    Expect(ranges.Allocate(1000, 1).value_or(99) == 0, "the coalesced range could not be allocated whole");
}

void Reuse() {
    RangeAllocator ranges(10000);
    const auto a = *ranges.Allocate(1000, 1);
    const auto b = *ranges.Allocate(500, 1);
    const auto c = *ranges.Allocate(1000, 1);
    ranges.Free(b, 500);
    Expect(ranges.Allocate(500, 1).value_or(99) == b, "a freed range was not reused for an allocation of its size");
    ranges.Free(b, 500);
    Expect(ranges.Allocate(300, 1).value_or(99) == b, "a smaller allocation did not take the smallest free range that fits");
    Expect(ranges.Allocate(200, 1).value_or(99) == b + 300, "the rest of the reused range was lost");
    ranges.Free(a, 1000);
    ranges.Free(c, 1000);
    Expect(ranges.Allocate(900, 1).value_or(99) == a, "best fit did not take the smallest free range that fits");
}

void RejectedFrees() {
    RangeAllocator ranges(1000);
    const auto a = *ranges.Allocate(100, 1);
    *ranges.Allocate(100, 1);
    Expect(ranges.Free(a, 100), "freeing a live range failed");
    Expect(!ranges.Free(a, 100), "freeing a range twice succeeded");
    Expect(!ranges.Free(a + 50, 20), "freeing inside a free range succeeded");
    Expect(!ranges.Free(50, 100), "freeing a range that overlaps a free range succeeded");
    Expect(!ranges.Free(900, 200), "freeing past the capacity succeeded");
    Expect(!ranges.Free(1001, 1), "freeing from beyond the capacity succeeded");
    Expect(ranges.Used() == 100 && ranges.FreeRanges() == 2, "rejected frees changed the allocator");
}

void Stress() {
    constexpr std::uint64_t Capacity = 1 << 22;
    RangeAllocator ranges(Capacity);
    Random random{12345};
    std::map<std::uint64_t, std::uint64_t> live;
    std::uint64_t used = 0;
    for (int step = 0; step < 20000; ++step) {
        if (live.empty() || random.Next() % 100 < 55) {
            const std::uint64_t size = 1 + random.Next() % 20000;
            const std::uint64_t alignment = std::uint64_t{1} << (random.Next() % 13);
            const auto offset = ranges.Allocate(size, alignment);
            if (!offset) continue;
            Expect(*offset % alignment == 0 && *offset + size <= Capacity, "a stress allocation broke its alignment or the capacity");
            const auto next = live.lower_bound(*offset);
            Expect(next == live.end() || next->first >= *offset + size, "a stress allocation overlaps the next live range");
            Expect(next == live.begin() || std::prev(next)->first + std::prev(next)->second <= *offset, "a stress allocation overlaps the previous live range");
            live[*offset] = size;
            used += size;
        } else {
            auto victim = live.begin();
            std::advance(victim, static_cast<std::ptrdiff_t>(random.Next() % live.size()));
            Expect(ranges.Free(victim->first, victim->second), "a stress free of a live range failed");
            used -= victim->second;
            live.erase(victim);
        }
        Expect(ranges.Used() == used && ranges.Available() == Capacity - used, "the stress usage counter drifted");
    }
    for (const auto& [offset, size] : live) Expect(ranges.Free(offset, size), "the final stress free failed");
    Expect(ranges.Empty() && ranges.FreeRanges() == 1 && ranges.LargestFree() == Capacity, "freeing every stress range did not coalesce the block");
}

void Policy() {
    ImageBlockPolicy policy;
    Expect(policy.blockBytes == 256 * MiB && policy.dedicatedBytes == 64 * MiB, "the default block and dedicated sizes changed");
    Expect(!policy.Dedicated(64 * MiB, false, false), "an image of exactly the dedicated threshold was not suballocated");
    Expect(policy.Dedicated(64 * MiB + 1, false, false) == ImageDedicatedReason::Large, "an image over the threshold was not dedicated");
    Expect(!policy.Dedicated(1, false, false), "a tiny image was not suballocated");
    Expect(policy.Dedicated(4 * KiB, true, false) == ImageDedicatedReason::Required, "a required dedicated allocation was ignored");
    Expect(policy.Dedicated(4 * KiB, false, true) == ImageDedicatedReason::Preferred, "a preferred dedicated allocation was ignored");
    Expect(policy.Dedicated(64 * MiB + 1, true, true) == ImageDedicatedReason::Large, "the size did not take precedence over the driver's hint");
    policy.ignorePreference = true;
    Expect(!policy.Dedicated(4 * KiB, false, true), "an ignored preference still forced a dedicated allocation");
    Expect(policy.Dedicated(4 * KiB, true, true) == ImageDedicatedReason::Required, "a requirement was ignored along with the preference");
    policy.dedicatedBytes = 512 * MiB;
    Expect(policy.Dedicated(300 * MiB, false, false) == ImageDedicatedReason::Large, "an image larger than the block was not dedicated");
    const bool expected = [] { const auto* value = std::getenv("APS5_IMAGE_SUBALLOCATION"); return value != nullptr && (std::strcmp(value, "1") == 0 || std::strcmp(value, "2") == 0); }();
    Expect(ImageSuballocation() == expected, "the switch does not follow APS5_IMAGE_SUBALLOCATION");
    const auto* value = std::getenv("APS5_IMAGE_SUBALLOCATION");
    Expect(ImageSuballocationIgnoresPreference() == (value != nullptr && std::strcmp(value, "2") == 0), "level 2 does not ignore the driver's preference");
}

constexpr VkMemoryPropertyFlags HostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

struct MockDevice {
    std::uint64_t next = 1;
    std::map<VkDeviceMemory, VkDeviceSize> memoryBytes;
    std::map<VkDeviceMemory, std::uint32_t> memoryTypes;
    std::map<VkDeviceMemory, VkImage> dedicatedImages;
    std::map<VkImage, std::pair<VkDeviceMemory, VkDeviceSize>> bound;
    std::map<VkImage, std::pair<bool, bool>> hints;
    std::uint64_t allocations = 0;
    std::uint64_t frees = 0;
    std::uint64_t attempts = 0;
    VkDeviceSize liveBytes = 0;
    VkDeviceSize limit = 0;
    VkDeviceSize heapBudget = 0;
    VkDeviceSize otherUsage = 0;
};

MockDevice mock;
std::mutex mockMutex;

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateMemory(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* memory) {
    std::lock_guard lock(mockMutex);
    ++mock.attempts;
    if (mock.limit != 0 && mock.liveBytes + info->allocationSize > mock.limit) {
        *memory = reinterpret_cast<VkDeviceMemory>(std::uintptr_t{0xdeadbeef});
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *memory = reinterpret_cast<VkDeviceMemory>(static_cast<std::uintptr_t>(mock.next++));
    mock.memoryBytes[*memory] = info->allocationSize;
    mock.memoryTypes[*memory] = info->memoryTypeIndex;
    for (auto* header = static_cast<const VkBaseInStructure*>(info->pNext); header != nullptr; header = header->pNext) {
        if (header->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) mock.dedicatedImages[*memory] = reinterpret_cast<const VkMemoryDedicatedAllocateInfo*>(header)->image;
    }
    mock.liveBytes += info->allocationSize;
    ++mock.allocations;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    std::lock_guard lock(mockMutex);
    if (!mock.memoryBytes.contains(memory)) {
        Expect(false, "an unknown allocation was freed");
        return;
    }
    mock.liveBytes -= mock.memoryBytes.at(memory);
    mock.memoryBytes.erase(memory);
    mock.dedicatedImages.erase(memory);
    ++mock.frees;
}

VKAPI_ATTR VkResult VKAPI_CALL mockBindImageMemory(VkDevice, VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
    std::lock_guard lock(mockMutex);
    if (!mock.memoryBytes.contains(memory) || offset >= mock.memoryBytes.at(memory)) return VK_ERROR_UNKNOWN;
    mock.bound[image] = {memory, offset};
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockImageRequirements2(VkDevice, const VkImageMemoryRequirementsInfo2* info, VkMemoryRequirements2* requirements) {
    std::lock_guard lock(mockMutex);
    for (auto* header = static_cast<VkBaseOutStructure*>(requirements->pNext); header != nullptr; header = header->pNext) {
        if (header->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) continue;
        auto* dedicated = reinterpret_cast<VkMemoryDedicatedRequirements*>(header);
        const auto found = mock.hints.find(info->image);
        dedicated->requiresDedicatedAllocation = found != mock.hints.end() && found->second.first ? VK_TRUE : VK_FALSE;
        dedicated->prefersDedicatedAllocation = found != mock.hints.end() && found->second.second ? VK_TRUE : VK_FALSE;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL mockMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void** data) {
    *data = nullptr;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockUnmapMemory(VkDevice, VkDeviceMemory) {}

VKAPI_ATTR void VKAPI_CALL mockDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {}

VKAPI_ATTR void VKAPI_CALL mockMemoryProperties2(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2* properties) {
    for (auto* next = static_cast<VkBaseOutStructure*>(properties->pNext); next != nullptr; next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) continue;
        auto* reported = reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(next);
        std::lock_guard lock(mockMutex);
        reported->heapBudget[0] = mock.heapBudget;
        reported->heapUsage[0] = mock.otherUsage + mock.liveBytes;
    }
}

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    static const std::map<std::string, PFN_vkVoidFunction> table{
        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(mockAllocateMemory)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(mockFreeMemory)},
        {"vkBindImageMemory", reinterpret_cast<PFN_vkVoidFunction>(mockBindImageMemory)},
        {"vkGetImageMemoryRequirements2", reinterpret_cast<PFN_vkVoidFunction>(mockImageRequirements2)},
        {"vkMapMemory", reinterpret_cast<PFN_vkVoidFunction>(mockMapMemory)},
        {"vkUnmapMemory", reinterpret_cast<PFN_vkVoidFunction>(mockUnmapMemory)},
        {"vkDestroyBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyBuffer)},
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

void ResetMock() {
    std::lock_guard lock(mockMutex);
    mock = MockDevice{};
}

Context MakeContext(bool budget = false) {
    Context context{};
    context.deviceProc = mockProc;
    context.memory.memoryHeapCount = 2;
    context.memory.memoryHeaps[0] = {16384 * MiB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    context.memory.memoryHeaps[1] = {32768 * MiB, 0};
    context.memory.memoryTypeCount = 3;
    context.memory.memoryTypes[0] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    context.memory.memoryTypes[1] = {HostVisible, 1};
    context.memory.memoryTypes[2] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    context.limits.maxMemoryAllocationCount = 1u << 20u;
    if (budget) {
        context.physical = reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{1});
        context.memoryProperties2 = mockMemoryProperties2;
    }
    context.bufferPool = std::make_shared<BufferPool>(context);
    context.bufferPool->SetImagePolicy(ImageBlockPolicy{});
    return context;
}

std::atomic<std::uint64_t> nextImage{1};

struct Bound {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory dedicated = VK_NULL_HANDLE;
    ImageRange range;
    ImageMemoryBinding binding;
    VkDeviceSize size = 0;
};

std::unique_ptr<Bound> Bind(const Context& context, VkDeviceSize size, VkDeviceSize alignment = 64 * KiB, std::uint32_t mask = 1u, std::pair<bool, bool> hint = {false, false}) {
    auto bound = std::make_unique<Bound>();
    bound->image = reinterpret_cast<VkImage>(static_cast<std::uintptr_t>(nextImage.fetch_add(1)));
    bound->size = size;
    if (hint.first || hint.second) {
        std::lock_guard lock(mockMutex);
        mock.hints[bound->image] = hint;
    }
    const VkMemoryRequirements requirements{size, alignment, mask};
    bound->binding = BindOptimalImageMemory(context, bound->image, requirements, "texture", bound->dedicated, bound->range);
    return bound;
}

void Dispose(Bound& bound) {
    bound.range.Release();
    if (bound.dedicated != VK_NULL_HANDLE) mockFreeMemory(VK_NULL_HANDLE, bound.dedicated, nullptr);
    bound.dedicated = VK_NULL_HANDLE;
}

void SharedBlock() {
    ResetMock();
    auto context = MakeContext();
    std::vector<std::unique_ptr<Bound>> images;
    for (int i = 0; i < 8; ++i) images.push_back(Bind(context, 4 * MiB));
    Expect(mock.allocations == 1 && mock.liveBytes == 256 * MiB, "eight 4 MiB images did not share one 256 MiB block (" + std::to_string(mock.allocations) + " allocations)");
    auto stats = context.bufferPool->ImageStats();
    Expect(stats.blocks == 1 && stats.reserved == 256 * MiB && stats.used == 32 * MiB && stats.allocations == 8 && stats.largestFree == 224 * MiB, "the block statistics do not describe eight images");
    Expect(ImageBlockSlack(context) == 224 * MiB, "the slack of the block is not its unused part");
    std::vector<std::pair<std::uint64_t, std::uint64_t>> placed;
    for (const auto& image : images) {
        Expect(image->range.Valid() && image->dedicated == VK_NULL_HANDLE && image->binding.suballocated, "a small image was not suballocated");
        Expect(image->range.Offset() % (64 * KiB) == 0, "an image offset breaks its alignment");
        Expect(mock.bound.at(image->image) == std::make_pair(image->range.Memory(), image->range.Offset()), "the image was bound to another range than the one granted");
        placed.emplace_back(image->range.Offset(), image->range.Offset() + image->range.Size());
    }
    std::sort(placed.begin(), placed.end());
    for (std::size_t i = 1; i < placed.size(); ++i) Expect(placed[i - 1].second <= placed[i].first, "two images overlap in the block");
    ImageRange moved = std::move(images[0]->range);
    Expect(moved.Valid() && !images[0]->range.Valid(), "moving a range left the source valid");
    moved.Release();
    moved.Release();
    stats = context.bufferPool->ImageStats();
    Expect(stats.used == 28 * MiB && stats.frees == 1, "releasing a moved range twice freed it twice");
    images.clear();
    stats = context.bufferPool->ImageStats();
    Expect(stats.used == 0 && stats.blocks == 1 && stats.emptyBlocks == 1 && stats.frees == 8, "freeing every image did not leave one empty spare block");
    Expect(mock.frees == 0 && mock.liveBytes == 256 * MiB, "the empty spare block was returned to Vulkan");
    context.bufferPool.reset();
    Expect(mock.liveBytes == 0 && mock.frees == 1, "destroying the pool did not return its blocks");
}

void Alignments() {
    ResetMock();
    auto context = MakeContext();
    auto first = Bind(context, 100 * KiB);
    auto strict = Bind(context, 64 * KiB, MiB);
    auto filler = Bind(context, 64 * KiB);
    Expect(first->range.Offset() == 0, "the first image is not at the start of its block");
    Expect(strict->range.Offset() == MiB, "an image with a 1 MiB alignment was not placed on the boundary");
    Expect(filler->range.Offset() == 128 * KiB, "the gap before an aligned image was not reused");
    Expect(mock.allocations == 1, "alignment padding made a second block");
}

void ReuseAfterFree() {
    ResetMock();
    auto context = MakeContext();
    std::vector<std::unique_ptr<Bound>> images;
    for (int i = 0; i < 6; ++i) images.push_back(Bind(context, 2 * MiB));
    const auto offset = images[3]->range.Offset();
    const auto memory = images[3]->range.Memory();
    images[3].reset();
    auto again = Bind(context, 2 * MiB);
    Expect(again->range.Memory() == memory && again->range.Offset() == offset, "a freed range was not reused by the next image of its size");
    Expect(mock.allocations == 1, "reusing a freed range allocated device memory");
    images.clear();
    again.reset();
    context.bufferPool.reset();
    Expect(mock.liveBytes == 0, "device memory leaked after the reuse test");
}

void BlocksAndSpare() {
    ResetMock();
    auto context = MakeContext();
    std::vector<std::unique_ptr<Bound>> images;
    for (int i = 0; i < 20; ++i) images.push_back(Bind(context, 32 * MiB));
    auto stats = context.bufferPool->ImageStats();
    Expect(mock.allocations == 3 && stats.blocks == 3 && stats.blocksMade == 3, "twenty 32 MiB images did not fill three 256 MiB blocks (" + std::to_string(mock.allocations) + " allocations)");
    for (std::size_t i = 0; i < 4; ++i) images[i].reset();
    stats = context.bufferPool->ImageStats();
    Expect(stats.blocks == 3 && stats.used == 16 * 32 * MiB, "freeing part of a block released it");
    Bind(context, 32 * MiB);
    stats = context.bufferPool->ImageStats();
    Expect(stats.blocks == 3 && mock.allocations == 3, "a freed range of a used block was not preferred over a new block");
    images.clear();
    stats = context.bufferPool->ImageStats();
    Expect(stats.blocks == 1 && stats.emptyBlocks == 1 && stats.blocksReleased == 2, "emptied blocks were not reduced to one spare");
    Expect(mock.frees == 2 && mock.liveBytes == 256 * MiB, "the emptied blocks were not returned to Vulkan");
    auto fresh = Bind(context, 32 * MiB);
    Expect(mock.allocations == 3 && fresh->range.Valid(), "the spare block was not used before a new one was made");
    fresh.reset();
    context.bufferPool.reset();
    Expect(mock.liveBytes == 0, "device memory leaked after the spare block test");
}

void DedicatedFallbacks() {
    ResetMock();
    auto context = MakeContext();
    auto exact = Bind(context, 64 * MiB);
    Expect(exact->range.Valid() && exact->dedicated == VK_NULL_HANDLE, "an image of exactly the threshold was not suballocated");
    auto large = Bind(context, 64 * MiB + 1);
    Expect(!large->range.Valid() && large->dedicated != VK_NULL_HANDLE && !large->binding.suballocated, "an image over the threshold was not dedicated");
    Expect(mock.memoryBytes.at(large->dedicated) == 64 * MiB + 1 && !mock.dedicatedImages.contains(large->dedicated), "a large dedicated allocation was not exactly sized or named an image without being asked");
    Expect(mock.bound.at(large->image) == std::make_pair(large->dedicated, VkDeviceSize{0}), "a dedicated image was not bound at offset zero");
    auto required = Bind(context, 4 * KiB, 4 * KiB, 1u, {true, false});
    Expect(!required->range.Valid() && required->dedicated != VK_NULL_HANDLE, "a required dedicated allocation was suballocated");
    Expect(mock.dedicatedImages.contains(required->dedicated) && mock.dedicatedImages.at(required->dedicated) == required->image, "the dedicated allocation did not name its image");
    auto preferred = Bind(context, 4 * KiB, 4 * KiB, 1u, {false, true});
    Expect(!preferred->range.Valid() && preferred->dedicated != VK_NULL_HANDLE && mock.dedicatedImages.contains(preferred->dedicated), "a preferred dedicated allocation was suballocated");
    auto stats = context.bufferPool->ImageStats();
    const auto index = [](ImageDedicatedReason reason) { return static_cast<std::size_t>(reason); };
    Expect(stats.dedicated[index(ImageDedicatedReason::Large)] == 1 && stats.dedicated[index(ImageDedicatedReason::Required)] == 1 && stats.dedicated[index(ImageDedicatedReason::Preferred)] == 1 && stats.dedicated[index(ImageDedicatedReason::NoBlock)] == 0, "the dedicated fallbacks were not counted by reason");
    auto policy = BufferPool::ImagePolicyFromEnvironment();
    policy.ignorePreference = true;
    context.bufferPool->SetImagePolicy(policy);
    auto ignored = Bind(context, 4 * KiB, 4 * KiB, 1u, {false, true});
    Expect(ignored->range.Valid() && ignored->dedicated == VK_NULL_HANDLE, "an ignored preference still produced a dedicated allocation");
    auto stillRequired = Bind(context, 4 * KiB, 4 * KiB, 1u, {true, true});
    Expect(!stillRequired->range.Valid() && stillRequired->dedicated != VK_NULL_HANDLE, "ignoring the preference ignored a requirement");
    for (auto* image : {exact.get(), large.get(), required.get(), preferred.get(), ignored.get(), stillRequired.get()}) Dispose(*image);
    context.bufferPool.reset();
    Expect(mock.liveBytes == 0, "device memory leaked after the dedicated test");
}

void SmallBlocks() {
    ResetMock();
    auto context = MakeContext();
    ImageBlockPolicy policy;
    policy.blockBytes = 8 * MiB;
    policy.dedicatedBytes = 2 * MiB;
    context.bufferPool->SetImagePolicy(policy);
    std::vector<std::unique_ptr<Bound>> images;
    for (int i = 0; i < 9; ++i) images.push_back(Bind(context, 2 * MiB));
    Expect(mock.allocations == 3 && mock.liveBytes == 24 * MiB, "2 MiB images did not fill 8 MiB blocks");
    auto over = Bind(context, 2 * MiB + 1);
    Expect(over->dedicated != VK_NULL_HANDLE, "an image over a small threshold was suballocated");
    Dispose(*over);
    images.clear();
    context.bufferPool.reset();
    Expect(mock.liveBytes == 0, "device memory leaked after the small block test");
}

void BudgetEdge() {
    ResetMock();
    auto context = MakeContext(true);
    {
        std::lock_guard lock(mockMutex);
        mock.heapBudget = 1024 * MiB;
        mock.otherUsage = 900 * MiB;
    }
    auto crowded = Bind(context, 4 * MiB);
    Expect(!crowded->range.Valid() && crowded->dedicated != VK_NULL_HANDLE, "a block was made although the heap's budget could not hold it");
    Expect(mock.memoryBytes.at(crowded->dedicated) == 4 * MiB && mock.memoryTypes.at(crowded->dedicated) == 0, "the image fallback is not a dedicated device-local allocation of its size");
    auto stats = context.bufferPool->ImageStats();
    Expect(stats.blocks == 0 && stats.dedicated[static_cast<std::size_t>(ImageDedicatedReason::NoBlock)] == 1, "the missing block was not counted");
    {
        std::lock_guard lock(mockMutex);
        mock.otherUsage = 100 * MiB;
    }
    auto roomy = Bind(context, 4 * MiB);
    Expect(roomy->range.Valid() && context.bufferPool->ImageStats().blocks == 1, "a block was not made once the budget had room");
    Dispose(*crowded);
    roomy.reset();
    context.bufferPool.reset();
    Expect(mock.liveBytes == 0, "device memory leaked after the budget test");
}

void BlockAllocationFails() {
    ResetMock();
    auto context = MakeContext();
    mock.limit = 100 * MiB;
    auto first = Bind(context, 4 * MiB);
    Expect(!first->range.Valid() && first->dedicated != VK_NULL_HANDLE && mock.attempts == 2, "a failed block allocation was not followed by a dedicated allocation");
    auto second = Bind(context, 4 * MiB);
    Expect(!second->range.Valid() && second->dedicated != VK_NULL_HANDLE && mock.attempts == 3, "a block allocation was retried at once after it failed");
    auto stats = context.bufferPool->ImageStats();
    Expect(stats.blocks == 0 && stats.dedicated[static_cast<std::size_t>(ImageDedicatedReason::NoBlock)] == 2, "the failed blocks were not counted");
    Dispose(*first);
    Dispose(*second);
    context.bufferPool.reset();
    Expect(mock.liveBytes == 0, "device memory leaked after the failed block test");
}

void SpareTrimmedForPressure() {
    ResetMock();
    auto context = MakeContext();
    Bind(context, 4 * MiB);
    Expect(mock.liveBytes == 256 * MiB && mock.frees == 0, "the first block was not kept as a spare");
    mock.limit = 300 * MiB;
    auto other = Bind(context, 4 * MiB, 64 * KiB, 1u << 2u);
    Expect(other->range.Valid() && mock.memoryTypes.at(other->range.Memory()) == 2, "the block of the second memory type was not made");
    Expect(mock.frees == 1 && mock.liveBytes == 256 * MiB, "the idle spare block was not returned to Vulkan under memory pressure");
    auto stats = context.bufferPool->ImageStats();
    Expect(stats.blocks == 1 && stats.blocksReleased == 1, "the trimmed spare block is still tracked");
    other.reset();
    context.bufferPool.reset();
    Expect(mock.liveBytes == 0, "device memory leaked after the trim test");
}

void Concurrent() {
    ResetMock();
    auto context = MakeContext();
    std::mutex placedMutex;
    std::map<std::pair<VkDeviceMemory, VkDeviceSize>, VkDeviceSize> placed;
    std::atomic<int> overlaps{0};
    const auto worker = [&](std::uint64_t seed) {
        Random random{seed};
        std::deque<std::unique_ptr<Bound>> live;
        const auto release = [&] {
            auto& bound = *live.front();
            {
                std::lock_guard lock(placedMutex);
                placed.erase({bound.range.Memory(), bound.range.Offset()});
            }
            live.pop_front();
        };
        static constexpr std::uint64_t sizes[] = {64 * KiB, 256 * KiB, MiB, 3 * MiB, 8 * MiB, 20 * MiB};
        for (int i = 0; i < 300; ++i) {
            auto bound = Bind(context, sizes[random.Next() % std::size(sizes)], 64 * KiB << (random.Next() % 3));
            {
                std::lock_guard lock(placedMutex);
                const std::pair<VkDeviceMemory, VkDeviceSize> key{bound->range.Memory(), bound->range.Offset()};
                const auto next = placed.lower_bound(key);
                if (next != placed.end() && next->first.first == key.first && next->first.second < key.second + bound->size) ++overlaps;
                if (next != placed.begin()) {
                    const auto previous = std::prev(next);
                    if (previous->first.first == key.first && previous->first.second + previous->second > key.second) ++overlaps;
                }
                placed[key] = bound->size;
            }
            live.push_back(std::move(bound));
            if (live.size() > 6) release();
        }
        while (!live.empty()) release();
    };
    std::vector<std::thread> threads;
    for (std::uint64_t seed = 1; seed <= 4; ++seed) threads.emplace_back(worker, seed * 7919);
    for (auto& thread : threads) thread.join();
    Expect(overlaps.load() == 0, "concurrent threads were granted overlapping ranges");
    const auto stats = context.bufferPool->ImageStats();
    Expect(stats.used == 0 && stats.allocations == stats.frees && stats.allocations == 1200, "the concurrent run leaked or double freed ranges");
    Expect(stats.blocks <= 1 && stats.emptyBlocks == stats.blocks, "more than one empty block was kept");
    Expect(mock.liveBytes == stats.reserved, "device memory is not exactly the blocks the pool tracks");
    context.bufferPool.reset();
    Expect(mock.liveBytes == 0, "device memory leaked after the concurrent test");
}

}

int main() {
    try {
        Allocation();
        Alignment();
        Coalescing();
        Reuse();
        RejectedFrees();
        Stress();
        Policy();
        SharedBlock();
        Alignments();
        ReuseAfterFree();
        BlocksAndSpare();
        DedicatedFallbacks();
        SmallBlocks();
        BudgetEdge();
        BlockAllocationFails();
        SpareTrimmedForPressure();
        Concurrent();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    if (failures != 0) return 1;
    std::puts("image block tests passed");
    return 0;
}
