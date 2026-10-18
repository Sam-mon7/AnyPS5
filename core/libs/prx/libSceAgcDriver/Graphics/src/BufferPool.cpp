#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RebarBudget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramOptions.hpp"
#include <limits>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace AgcDriver::Graphics {

bool RebarUploads() {
    static const bool enabled = [] { const auto* value = std::getenv("APS5_REBAR_UPLOADS"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    return enabled;
}

bool RebarMirrors() {
    static const bool enabled = [] { const auto* value = std::getenv("APS5_REBAR_MIRRORS"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    return enabled;
}

RebarBudget& RebarMemory() {
    static RebarBudget budget([] {
        const auto* value = std::getenv("APS5_REBAR_MIB");
        const auto mib = value != nullptr ? std::strtoull(value, nullptr, 10) : 1024ull;
        return std::min<std::uint64_t>(mib, UINT64_MAX >> 20u) << 20u;
    }());
    return budget;
}

std::uint64_t RebarLimit(const Context& context) {
    auto& rebar = RebarMemory();
    if (!VramCleanup()) return rebar.limit.load();
    const auto pool = GetBufferPool(context);
    const auto report = pool->Vram().Current();
    static std::mutex mutex;
    static std::weak_ptr<BufferPool> sampledPool;
    static std::uint64_t sampleId = 0;
    std::lock_guard lock(mutex);
    if (sampledPool.lock() == pool && sampleId >= report.sampleId) return rebar.limit.load();
    static const auto ceiling = [] {
        const auto* value = std::getenv("APS5_REBAR_MIB");
        return value != nullptr ? std::min<std::uint64_t>(std::strtoull(value, nullptr, 10), UINT64_MAX >> 20u) << 20u : UINT64_MAX;
    }();
    rebar.limit.store(report.heapBytes == 0 ? std::min<std::uint64_t>(ceiling, 1024ull << 20u) : VramBudget::RebarAllowance(report, rebar.live.load(), ceiling));
    sampledPool = pool;
    sampleId = report.sampleId;
    return rebar.limit.load();
}

void RebarBudget::Report() {
    AgcDriver::ProfilePrint_nid_no_patch("[rebar] live %.1f MiB, peak %.1f MiB, allocations %llu, fallbacks ineligible %llu / over budget %llu\n", live.load() / 1048576.0, peak.load() / 1048576.0, static_cast<unsigned long long>(allocations.load()), static_cast<unsigned long long>(ineligible.load()), static_cast<unsigned long long>(overBudget.load()));
}

namespace {

// APS5_BUFFER_POOL_SHARED=1: one tier for every size, as before the split.
bool sharedTiers() {
    static const bool shared = std::getenv("APS5_BUFFER_POOL_SHARED") != nullptr;
    return shared;
}

bool exactSizes() {
    static const bool exact = std::getenv("APS5_NO_BUFFER_CLASSES") != nullptr;
    return exact;
}

constexpr VkBufferUsageFlags deviceUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

}

BufferPool::BufferPool(const Context& context) : device(context.device), unmap(context.Function<PFN_vkUnmapMemory>("vkUnmapMemory")), destroyBuffer(context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")), freeMemory(context.Function<PFN_vkFreeMemory>("vkFreeMemory")), allocateMemory(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")), mapMemory(context.Function<PFN_vkMapMemory>("vkMapMemory")), maxSlots(std::max<std::size_t>(1, std::min<std::size_t>(MaxSlots(), context.limits.maxMemoryAllocationCount / 6u))), vram(context) {
    smallTier.budget = VramCleanup() ? 16ull << 20u : smallBudget;
    largeTier.budget = VramCleanup() ? 128ull << 20u : budget;
    deviceTier.budget = DeviceTierEnabled() ? DeviceBudget(context.memory) : 0;
    imagePolicy = ImagePolicyFromEnvironment();
}

BufferPool::~BufferPool() {
    for (auto* tier : {&smallTier, &largeTier, &deviceTier}) {
        for (const auto& [key, slots] : tier->free) {
            for (const auto& slot : slots) destroy(slot.allocation);
        }
    }
    for (auto& [memory, block] : slabBlocks) freeBlock(*block);
    for (const auto& [memory, block] : imageBlocks) freeMemory(device, memory, nullptr);
}

VkDeviceMemory BufferPool::AllocateMemory(const VkMemoryAllocateInfo& allocation, const char* operation) {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    auto result = allocateMemory(device, &allocation, nullptr, &memory);
    const auto initialResult = result;
    unsigned attempts = 1;
    VkDeviceSize reclaimed = 0;
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY) {
        reclaimed = trim();
        if (reclaimed != 0) {
            memory = VK_NULL_HANDLE;
            result = allocateMemory(device, &allocation, nullptr, &memory);
            ++attempts;
        }
    }
    if (result == VK_SUCCESS) {
        if (attempts == 2) std::fprintf(stderr, "[bufferpool] %s recovered: allocation=%llu reclaimed=%llu initialResult=%d attempts=%u\n", operation, static_cast<unsigned long long>(allocation.allocationSize), static_cast<unsigned long long>(reclaimed), static_cast<int>(initialResult), attempts);
        return memory;
    }
    char details[512];
    std::snprintf(details, sizeof(details), "%s: allocation=%llu memoryType=%u reclaimed=%llu initialResult=%d attempts=%u", operation, static_cast<unsigned long long>(allocation.allocationSize), allocation.memoryTypeIndex, static_cast<unsigned long long>(reclaimed), static_cast<int>(initialResult), attempts);
    Check(result, details);
    return VK_NULL_HANDLE;
}

bool BufferPool::SlabEligible(std::size_t capacity, VkDeviceSize alignment, VkDeviceSize size, VkDeviceSize atom) {
    if (capacity >= classLimit || size > capacity || !std::has_single_bit(capacity)) return false;
    const auto alignmentOk = alignment == 0 || (std::has_single_bit(alignment) && alignment <= capacity);
    const auto atomOk = atom <= 1 || (std::has_single_bit(atom) && atom <= capacity);
    return alignmentOk && atomOk;
}

VkDeviceSize BufferPool::SlabBlockBytes(std::size_t capacity) {
    return std::max<VkDeviceSize>(VkDeviceSize{2} << 20u, static_cast<VkDeviceSize>(capacity) * 8u);
}

std::uint64_t BufferPool::SlabKey(std::uint32_t memoryType, std::size_t capacity, bool addressable) {
    return (static_cast<std::uint64_t>(memoryType) << 40u) | (static_cast<std::uint64_t>(addressable ? 1u : 0u) << 39u) | static_cast<std::uint64_t>(capacity);
}

SlabSlot BufferPool::TakeSlot(const Context& context, std::uint32_t memoryType, std::size_t capacity, bool addressable, bool rebar) {
    if (rebar) RebarLimit(context);
    const auto key = SlabKey(memoryType, capacity, addressable);
    const auto takeFrom = [&](SlabBlock& block, Slab& slab) {
        const auto index = block.free.back();
        block.free.pop_back();
        if (block.used++ == 0) --slab.emptyBlocks;
        if (block.free.empty()) slab.available.erase(std::find(slab.available.begin(), slab.available.end(), &block));
        const auto offset = static_cast<VkDeviceSize>(index) * block.slotBytes;
        return SlabSlot{block.memory, offset, block.mapping != nullptr ? block.mapping + offset : nullptr};
    };
    {
        std::lock_guard lock(slabMutex);
        auto& slab = slabs[key];
        if (!slab.available.empty()) return takeFrom(*slab.available.back(), slab);
    }
    const auto blockBytes = SlabBlockBytes(capacity);
    auto block = std::make_unique<SlabBlock>();
    block->slotBytes = capacity;
    block->slab = key;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    if (addressable) allocation.pNext = &flags;
    allocation.allocationSize = blockBytes;
    allocation.memoryTypeIndex = memoryType;
    if (rebar && !RebarMemory().Reserve(blockBytes)) return {};
    block->rebarBytes = rebar ? blockBytes : 0;
    try {
        block->memory = AllocateMemory(allocation, "vkAllocateMemory buffer slab");
    } catch (...) {
        if (block->rebarBytes != 0) RebarMemory().Release(block->rebarBytes);
        throw;
    }
    if ((context.memory.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        void* mapping = nullptr;
        const auto mapped = mapMemory(device, block->memory, 0, VK_WHOLE_SIZE, 0, &mapping);
        if (mapped != VK_SUCCESS) {
            freeMemory(device, block->memory, nullptr);
            if (block->rebarBytes != 0) RebarMemory().Release(block->rebarBytes);
        }
        Check(mapped, "vkMapMemory buffer slab");
        block->mapping = static_cast<std::byte*>(mapping);
    }
    const auto slots = static_cast<std::uint32_t>(blockBytes / capacity);
    block->free.reserve(slots);
    for (std::uint32_t index = slots; index-- != 0;) block->free.push_back(index);
    std::lock_guard lock(slabMutex);
    auto& slab = slabs[key];
    auto& inserted = *block;
    slabBlocks.emplace(inserted.memory, std::move(block));
    slab.available.push_back(&inserted);
    ++slab.emptyBlocks;
    return takeFrom(*slab.available.back(), slab);
}

void BufferPool::freeBlock(SlabBlock& block) noexcept {
    if (block.mapping != nullptr) unmap(device, block.memory);
    freeMemory(device, block.memory, nullptr);
    if (block.rebarBytes != 0) RebarMemory().Release(block.rebarBytes);
}

VkDeviceSize BufferPool::PutSlot(VkDeviceMemory memory, VkDeviceSize offset) noexcept {
    std::unique_ptr<SlabBlock> released;
    {
        std::lock_guard lock(slabMutex);
        const auto found = slabBlocks.find(memory);
        if (found == slabBlocks.end()) return 0;
        auto& block = *found->second;
        auto& slab = slabs[block.slab];
        if (block.free.empty()) slab.available.push_back(&block);
        block.free.push_back(static_cast<std::uint32_t>(offset / block.slotBytes));
        if (--block.used != 0) return 0;
        if (!VramCleanup() && slab.emptyBlocks == 0) {
            ++slab.emptyBlocks;
            return 0;
        }
        slab.available.erase(std::find(slab.available.begin(), slab.available.end(), &block));
        released = std::move(found->second);
        slabBlocks.erase(found);
    }
    freeBlock(*released);
    return SlabBlockBytes(released->slotBytes);
}

std::size_t BufferPool::SlabBlocks() {
    std::lock_guard lock(slabMutex);
    return slabBlocks.size();
}

BufferPoolStats BufferPool::Stats() {
    BufferPoolStats result;
    {
        std::lock_guard lock(mutex);
        result = {smallTier.retainedBytes, smallTier.budget, largeTier.retainedBytes, largeTier.budget, deviceTier.retainedBytes, deviceTier.budget};
        result.smallPhysicalRetained = smallTier.physicalRetained;
        result.largePhysicalRetained = largeTier.physicalRetained;
        result.devicePhysicalRetained = deviceTier.physicalRetained;
    }
    {
        std::lock_guard lock(slabMutex);
        for (const auto& [memory, block] : slabBlocks) result.slabBytes += SlabBlockBytes(block->slotBytes);
    }
    return result;
}

bool BufferPool::DeviceTierEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_STAGING_POOL_MIB");
        return value == nullptr || std::strtoull(value, nullptr, 10) != 0;
    }();
    return enabled;
}

VkDeviceSize BufferPool::DeviceBudget(const VkPhysicalDeviceMemoryProperties& memory) {
    static const char* value = std::getenv("APS5_STAGING_POOL_MIB");
    const auto cap = VramCleanup() ? 256ull << 20u : std::numeric_limits<VkDeviceSize>::max();
    if (value != nullptr) return std::min<VkDeviceSize>(static_cast<VkDeviceSize>(std::strtoull(value, nullptr, 10)) << 20u, cap);
    VkDeviceSize largest = 0;
    for (std::uint32_t heap = 0; heap < memory.memoryHeapCount && heap < VK_MAX_MEMORY_HEAPS; ++heap) {
        if ((memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) largest = std::max(largest, memory.memoryHeaps[heap].size);
    }
    return std::min<VkDeviceSize>(std::max<VkDeviceSize>(VkDeviceSize{512} << 20u, largest / 8u), cap);
}

VkDeviceSize BufferPool::destroy(const BufferAllocation& allocation) noexcept {
    if (allocation.slab) {
        destroyBuffer(device, allocation.buffer, nullptr);
        return PutSlot(allocation.memory, allocation.offset);
    }
    // Device-local allocations (see DeviceBuffer) are never mapped.
    if (allocation.mapping != nullptr) unmap(device, allocation.memory);
    destroyBuffer(device, allocation.buffer, nullptr);
    freeMemory(device, allocation.memory, nullptr);
    if (allocation.rebarBytes != 0) RebarMemory().Release(allocation.rebarBytes);
    return allocation.allocationBytes;
}

bool BufferPool::DeviceTiered(VkMemoryPropertyFlags properties) {
    return (properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 && (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 && (DeviceTierEnabled() || VramCleanup());
}

std::size_t BufferPool::Capacity(std::size_t bytes, VkMemoryPropertyFlags properties) {
    constexpr std::size_t smallest = 256;
    if (exactSizes()) return bytes;
    if (bytes < classLimit) return std::max(smallest, std::bit_ceil(bytes));
    if (!DeviceTiered(properties)) return bytes;
    const auto step = std::bit_floor(bytes) >> deviceClassBits;
    return (bytes + step - 1) / step * step;
}

std::size_t BufferPool::NextCapacity(std::size_t capacity) {
    return capacity < classLimit ? capacity * 2 : capacity + (std::bit_floor(capacity) >> deviceClassBits);
}

VkBufferUsageFlags BufferPool::Usage(VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    return DeviceTiered(properties) ? usage | deviceUsage : usage;
}

BufferPool::Tier& BufferPool::tierFor(std::size_t capacity, VkMemoryPropertyFlags properties) {
    if (DeviceTiered(properties)) return deviceTier;
    return !sharedTiers() && capacity < classLimit ? smallTier : largeTier;
}

std::optional<BufferAllocation> BufferPool::Take(std::size_t bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, std::uint32_t memoryType) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto capacity = Capacity(bytes, properties);
    std::lock_guard lock(mutex);
    ++clock;
    if (profile) {
        static auto lastReport = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::seconds(10)) {
            lastReport = now;
            if (RebarMemory().allocations.load() != 0 || RebarMemory().ineligible.load() != 0 || RebarMemory().overBudget.load() != 0) RebarMemory().Report();
            AgcDriver::ProfilePrint_nid_no_patch("[bufferpool] small: %llu hits, %llu misses, %llu evictions, %zu retained (%.0f MiB); large: %llu hits, %llu misses, %llu evictions, %zu retained (%.0f MiB)%s; device: %llu hits, %llu misses, %llu evictions, %zu retained (%.0f MiB of %llu)\n", static_cast<unsigned long long>(smallTier.hits), static_cast<unsigned long long>(smallTier.misses), static_cast<unsigned long long>(smallTier.evictions), smallTier.slots, smallTier.retainedBytes / 1048576.0, static_cast<unsigned long long>(largeTier.hits), static_cast<unsigned long long>(largeTier.misses), static_cast<unsigned long long>(largeTier.evictions), largeTier.slots, largeTier.retainedBytes / 1048576.0, sharedTiers() ? " (shared)" : "", static_cast<unsigned long long>(deviceTier.hits), static_cast<unsigned long long>(deviceTier.misses), static_cast<unsigned long long>(deviceTier.evictions), deviceTier.slots, deviceTier.retainedBytes / 1048576.0, static_cast<unsigned long long>(deviceTier.budget >> 20u));
            if (vram.Enabled()) {
                const auto report = vram.Report();
                AgcDriver::ProfilePrint_nid_no_patch("[vram] heap %u: budget %llu MiB, usage %llu MiB, target %llu MiB; %llu released device buffers (%.0f MiB) not pooled over the target\n", report.heap, static_cast<unsigned long long>(report.budget >> 20u), static_cast<unsigned long long>(report.usage >> 20u), static_cast<unsigned long long>(report.target >> 20u), static_cast<unsigned long long>(vramDropped), vramDroppedBytes / 1048576.0);
            }
        }
    }
    auto& tier = tierFor(capacity, properties);
    const auto largest = &tier == &deviceTier && !exactSizes() && bytes <= std::numeric_limits<std::size_t>::max() / 2 ? std::max(capacity, bytes * 2) : capacity;
    auto found = tier.free.end();
    for (auto size = capacity; size <= largest && found == tier.free.end(); size = NextCapacity(size)) found = tier.free.find({size, usage, properties, memoryType});
    if (found == tier.free.end() || found->second.empty()) {
        ++tier.misses;
        return std::nullopt;
    }
    auto result = found->second.back().allocation;
    found->second.pop_back();
    --tier.slots;
    tier.retainedBytes -= result.allocationBytes;
    removeRetained(tier, result);
    ++tier.hits;
    return result;
}

void BufferPool::evictOldest(Tier& tier, std::vector<BufferAllocation>& evicted) {
    const auto lastUse = [](const auto& entry) { return entry.second.empty() ? std::numeric_limits<std::uint64_t>::max() : entry.second.front().lastUse; };
    const auto oldest = std::min_element(tier.free.begin(), tier.free.end(), [&](const auto& left, const auto& right) { return lastUse(left) < lastUse(right); });
    // First: a throw here leaves the slot retained and counted.
    evicted.push_back(oldest->second.front().allocation);
    tier.retainedBytes -= oldest->second.front().allocation.allocationBytes;
    removeRetained(tier, oldest->second.front().allocation);
    oldest->second.pop_front();
    --tier.slots;
    ++tier.evictions;
}

VkDeviceSize BufferPool::retentionCost(const Tier& tier, const BufferAllocation& allocation) {
    if (!VramCleanup() || !allocation.slab) return allocation.allocationBytes;
    const auto found = tier.retainedSlabs.find(allocation.memory);
    return found == tier.retainedSlabs.end() || found->second == 0 ? SlabBlockBytes(allocation.bytes) : 0;
}

void BufferPool::removeRetained(Tier& tier, const BufferAllocation& allocation) {
    auto charge = allocation.allocationBytes;
    if (VramCleanup() && allocation.slab) {
        const auto found = tier.retainedSlabs.find(allocation.memory);
        charge = --found->second == 0 ? SlabBlockBytes(allocation.bytes) : 0;
        if (found->second == 0) tier.retainedSlabs.erase(found);
    }
    tier.physicalRetained -= charge;
}

std::size_t BufferPool::MaxSlots() {
    // APS5_BUFFER_POOL_SLOTS=<n> bounds the retained allocations; 64 is the capacity the pool had
    // before least-recently-used retention, for A/B runs.
    static const std::size_t slots = [] {
        const char* value = std::getenv("APS5_BUFFER_POOL_SLOTS");
        const auto parsed = value != nullptr ? std::strtoull(value, nullptr, 10) : 0;
        return parsed != 0 ? static_cast<std::size_t>(parsed) : defaultSlots;
    }();
    return slots;
}

VkDeviceSize BufferPool::trim() {
    Tier retiredSmall;
    Tier retiredLarge;
    Tier retiredDevice;
    {
        std::lock_guard lock(mutex);
        for (const auto& [tier, retired] : {std::pair{&smallTier, &retiredSmall}, std::pair{&largeTier, &retiredLarge}, std::pair{&deviceTier, &retiredDevice}}) {
            tier->free.swap(retired->free);
            tier->evictions += tier->slots;
            tier->retainedBytes = 0;
            tier->physicalRetained = 0;
            tier->retainedSlabs.clear();
            tier->slots = 0;
        }
    }
    VkDeviceSize bytes = 0;
    for (const auto* tier : {&retiredSmall, &retiredLarge, &retiredDevice}) {
        for (const auto& [key, slots] : tier->free) {
            for (const auto& slot : slots) bytes += destroy(slot.allocation);
        }
    }
    for (;;) {
        std::unique_ptr<SlabBlock> released;
        {
            std::lock_guard lock(slabMutex);
            const auto found = std::find_if(slabBlocks.begin(), slabBlocks.end(), [](const auto& entry) { return entry.second->used == 0; });
            if (found == slabBlocks.end()) break;
            auto& slab = slabs.at(found->second->slab);
            slab.available.erase(std::find(slab.available.begin(), slab.available.end(), found->second.get()));
            --slab.emptyBlocks;
            released = std::move(found->second);
            slabBlocks.erase(found);
        }
        freeBlock(*released);
        bytes += SlabBlockBytes(released->slotBytes);
    }
    return bytes + releaseEmptyImageBlocks();
}

void BufferPool::Put(const BufferAllocation& allocation) noexcept {
    if (VramCleanup() && allocation.residentMirror) {
        const auto freed = destroy(allocation);
        if (allocation.deviceLocal) vram.Released(freed);
        return;
    }
    // Evicted allocations are destroyed after the mutex is released (see evictOldest). The vector
    // may throw on growth; a Put that cannot retain simply destroys, as noexcept requires.
    std::vector<BufferAllocation> evicted;
    VkDeviceSize excess = 0;
    try {
        if (DeviceTiered(allocation.properties) || (VramCleanup() && allocation.deviceLocal)) excess = vram.Excess();
        std::lock_guard lock(mutex);
        auto& tier = tierFor(allocation.bytes, allocation.properties);
        if (excess != 0 && (&tier == &deviceTier || (VramCleanup() && allocation.deviceLocal))) {
            auto dropped = allocation.allocationBytes;
            while (tier.slots != 0 && dropped < excess) {
                evictOldest(tier, evicted);
                dropped += evicted.back().allocationBytes;
            }
            evicted.push_back(allocation);
            vramDropped += evicted.size();
            vramDroppedBytes += dropped;
        } else if (retentionCost(tier, allocation) > tier.budget) {
            evicted.push_back(allocation);
        } else {
            while (tier.slots != 0 && (tier.physicalRetained + retentionCost(tier, allocation) > tier.budget || tier.slots >= maxSlots)) evictOldest(tier, evicted);
            const auto charge = retentionCost(tier, allocation);
            std::size_t* slabCount = VramCleanup() && allocation.slab ? &tier.retainedSlabs.try_emplace(allocation.memory, 0).first->second : nullptr;
            try {
                tier.free[{allocation.bytes, allocation.usage, allocation.properties, allocation.memoryType}].push_back({allocation, ++clock});
            } catch (...) {
                if (slabCount != nullptr && *slabCount == 0) tier.retainedSlabs.erase(allocation.memory);
                throw;
            }
            if (slabCount != nullptr) ++*slabCount;
            ++tier.slots;
            tier.retainedBytes += allocation.allocationBytes;
            tier.physicalRetained += charge;
        }
    } catch (...) {
        destroy(allocation);
    }
    VkDeviceSize freed = 0;
    for (const auto& gone : evicted) {
        const auto bytes = destroy(gone);
        if (!VramCleanup() || gone.deviceLocal || (gone.properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) freed += bytes;
    }
    if (excess != 0 || VramCleanup()) vram.Released(freed);
}

ImageBlockPolicy BufferPool::ImagePolicyFromEnvironment() {
    static const ImageBlockPolicy policy = [] {
        ImageBlockPolicy result;
        const auto megabytes = [](const char* name, VkDeviceSize fallback) {
            const auto* value = std::getenv(name);
            const auto parsed = value != nullptr ? std::strtoull(value, nullptr, 10) : 0;
            return parsed != 0 ? static_cast<VkDeviceSize>(std::min<unsigned long long>(parsed, UINT64_MAX >> 20u)) << 20u : fallback;
        };
        result.blockBytes = megabytes("APS5_IMAGE_BLOCK_MIB", result.blockBytes);
        result.dedicatedBytes = megabytes("APS5_IMAGE_DEDICATED_MIB", result.dedicatedBytes);
        result.ignorePreference = ImageSuballocationIgnoresPreference();
        return result;
    }();
    return policy;
}

void BufferPool::SetImagePolicy(const ImageBlockPolicy& policy) {
    std::lock_guard lock(imageMutex);
    imagePolicy = policy;
}

namespace {

struct DedicatedHint {
    bool required = false;
    bool preferred = false;
};

DedicatedHint DedicatedHintFor(const Context& context, VkImage image) {
    if (context.deviceProc == nullptr) return {};
    const auto query = reinterpret_cast<PFN_vkGetImageMemoryRequirements2>(context.deviceProc(context.device, "vkGetImageMemoryRequirements2"));
    if (query == nullptr) return {};
    VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated};
    const VkImageMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, nullptr, image};
    query(context.device, &info, &requirements);
    return {dedicated.requiresDedicatedAllocation != VK_FALSE, dedicated.prefersDedicatedAllocation != VK_FALSE};
}

void CheckImage(VkResult result, const char* operation, const char* what) {
    if (result != VK_SUCCESS) Check(result, (std::string(operation) + " " + what).c_str());
}

}

ImageMemoryBinding BufferPool::BindOptimalImage(const Context& context, const std::shared_ptr<BufferPool>& self, VkImage image, const VkMemoryRequirements& requirements, const char* what, VkDeviceMemory& dedicated, ImageRange& range) {
    ImageBlockPolicy policy;
    {
        std::lock_guard lock(imageMutex);
        policy = imagePolicy;
    }
    const auto hint = DedicatedHintFor(context, image);
    auto reason = policy.Dedicated(requirements.size, hint.required, hint.preferred);
    const auto bind = context.Function<PFN_vkBindImageMemory>("vkBindImageMemory");
    if (!reason) {
        const auto type = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (const auto grant = takeImageRange(context, type, requirements)) {
            range = ImageRange(self, grant->memory, grant->offset, requirements.size);
            CheckImage(bind(context.device, image, grant->memory, grant->offset), "vkBindImageMemory", what);
            return {type, true};
        }
        reason = ImageDedicatedReason::NoBlock;
    }
    {
        std::lock_guard lock(imageMutex);
        ++imageTotals.dedicated[static_cast<std::size_t>(*reason)];
        reportImageBlocks();
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = context.DeviceMemoryType(requirements.memoryTypeBits, requirements.size, what);
    VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicatedInfo.image = image;
    if (hint.required || hint.preferred) allocation.pNext = &dedicatedInfo;
    CheckImage(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &dedicated), "vkAllocateMemory", what);
    CheckImage(bind(context.device, image, dedicated, 0), "vkBindImageMemory", what);
    return {allocation.memoryTypeIndex, false};
}

bool BufferPool::deviceHeapHasRoom(const Context& context, std::uint32_t type, VkDeviceSize bytes) const {
    if (context.memoryProperties2 == nullptr || context.physical == VK_NULL_HANDLE) return true;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT reported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    properties.pNext = &reported;
    context.memoryProperties2(context.physical, &properties);
    const auto heap = context.memory.memoryTypes[type].heapIndex;
    const auto budget = reported.heapBudget[heap];
    const auto usage = reported.heapUsage[heap];
    return budget == 0 || (usage < budget && bytes <= budget - usage);
}

std::size_t BufferPool::emptyImageBlocks(std::uint32_t type) const {
    const auto found = imageTypes.find(type);
    if (found == imageTypes.end()) return 0;
    return static_cast<std::size_t>(std::count_if(found->second.begin(), found->second.end(), [](const ImageBlock* block) { return block->ranges.Empty(); }));
}

std::optional<BufferPool::ImageGrant> BufferPool::grantFromImageBlocks(std::uint32_t type, const VkMemoryRequirements& requirements) {
    const auto found = imageTypes.find(type);
    if (found == imageTypes.end()) return std::nullopt;
    for (const bool empty : {false, true}) {
        for (auto* block : found->second) {
            if (block->ranges.Empty() != empty) continue;
            const auto before = block->ranges.Used();
            const auto offset = block->ranges.Allocate(requirements.size, requirements.alignment);
            if (!offset) continue;
            imageUsed.fetch_add(block->ranges.Used() - before, std::memory_order_relaxed);
            ++imageTotals.allocations;
            return ImageGrant{block->memory, *offset};
        }
    }
    return std::nullopt;
}

std::optional<BufferPool::ImageGrant> BufferPool::takeImageRange(const Context& context, std::uint32_t type, const VkMemoryRequirements& requirements) {
    VkDeviceSize blockBytes = 0;
    {
        std::lock_guard lock(imageMutex);
        reportImageBlocks();
        if (const auto grant = grantFromImageBlocks(type, requirements)) return grant;
        if (std::chrono::steady_clock::now() < imageRetryAt) return std::nullopt;
        blockBytes = imagePolicy.blockBytes;
    }
    if (!deviceHeapHasRoom(context, type, blockBytes)) return std::nullopt;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = blockBytes;
    allocation.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    try {
        memory = AllocateMemory(allocation, "vkAllocateMemory image block");
    } catch (const std::runtime_error&) {
        std::lock_guard lock(imageMutex);
        imageRetryAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        return std::nullopt;
    }
    std::lock_guard lock(imageMutex);
    ImageBlock* inserted = nullptr;
    try {
        auto block = std::make_unique<ImageBlock>(memory, type, blockBytes);
        inserted = block.get();
        imageBlocks.emplace(memory, std::move(block));
        try {
            imageTypes[type].push_back(inserted);
        } catch (...) {
            imageBlocks.erase(memory);
            throw;
        }
    } catch (...) {
        freeMemory(device, memory, nullptr);
        throw;
    }
    ++imageTotals.blocksMade;
    imageReserved.fetch_add(blockBytes, std::memory_order_relaxed);
    const auto offset = inserted->ranges.Allocate(requirements.size, requirements.alignment);
    if (!offset) return std::nullopt;
    imageUsed.fetch_add(inserted->ranges.Used(), std::memory_order_relaxed);
    ++imageTotals.allocations;
    return ImageGrant{memory, *offset};
}

void BufferPool::PutImageRange(VkDeviceMemory block, VkDeviceSize offset, VkDeviceSize size) noexcept {
    std::unique_ptr<ImageBlock> released;
    {
        std::lock_guard lock(imageMutex);
        const auto found = imageBlocks.find(block);
        if (found == imageBlocks.end()) return;
        auto& entry = *found->second;
        const auto before = entry.ranges.Used();
        if (!entry.ranges.Free(offset, size)) return;
        imageUsed.fetch_sub(before - entry.ranges.Used(), std::memory_order_relaxed);
        ++imageTotals.frees;
        if (entry.ranges.Empty() && emptyImageBlocks(entry.type) > 1) {
            auto& list = imageTypes[entry.type];
            list.erase(std::find(list.begin(), list.end(), &entry));
            imageReserved.fetch_sub(entry.ranges.Capacity(), std::memory_order_relaxed);
            ++imageTotals.blocksReleased;
            released = std::move(found->second);
            imageBlocks.erase(found);
        }
    }
    if (!released) return;
    freeMemory(device, released->memory, nullptr);
    vram.Released(released->ranges.Capacity());
}

VkDeviceSize BufferPool::releaseEmptyImageBlocks() noexcept {
    VkDeviceSize bytes = 0;
    std::lock_guard lock(imageMutex);
    for (auto it = imageBlocks.begin(); it != imageBlocks.end();) {
        auto& block = *it->second;
        if (!block.ranges.Empty()) {
            ++it;
            continue;
        }
        auto& list = imageTypes[block.type];
        list.erase(std::find(list.begin(), list.end(), &block));
        freeMemory(device, block.memory, nullptr);
        bytes += block.ranges.Capacity();
        imageReserved.fetch_sub(block.ranges.Capacity(), std::memory_order_relaxed);
        ++imageTotals.blocksReleased;
        it = imageBlocks.erase(it);
    }
    return bytes;
}

ImageBlockStats BufferPool::ImageStats() {
    std::lock_guard lock(imageMutex);
    auto result = imageTotals;
    result.blocks = imageBlocks.size();
    for (const auto& [memory, block] : imageBlocks) {
        result.reserved += block->ranges.Capacity();
        result.used += block->ranges.Used();
        result.largestFree = std::max<VkDeviceSize>(result.largestFree, block->ranges.LargestFree());
        if (block->ranges.Empty()) ++result.emptyBlocks;
    }
    return result;
}

VkDeviceSize BufferPool::ImageSlack() const {
    const auto reserved = imageReserved.load(std::memory_order_relaxed);
    const auto used = imageUsed.load(std::memory_order_relaxed);
    return reserved - std::min(reserved, used);
}

void BufferPool::reportImageBlocks() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - imageReportedAt < std::chrono::seconds(10)) return;
    imageReportedAt = now;
    VkDeviceSize reserved = 0, used = 0, largest = 0;
    std::size_t empty = 0;
    for (const auto& [memory, block] : imageBlocks) {
        reserved += block->ranges.Capacity();
        used += block->ranges.Used();
        largest = std::max<VkDeviceSize>(largest, block->ranges.LargestFree());
        if (block->ranges.Empty()) ++empty;
    }
    const auto delta = [&](std::uint64_t ImageBlockStats::* field) { return static_cast<unsigned long long>(imageTotals.*field - imageReported.*field); };
    const auto dedicated = [&](ImageDedicatedReason reason) {
        const auto index = static_cast<std::size_t>(reason);
        return static_cast<unsigned long long>(imageTotals.dedicated[index] - imageReported.dedicated[index]);
    };
    const auto fallbacks = dedicated(ImageDedicatedReason::Large) + dedicated(ImageDedicatedReason::Required) + dedicated(ImageDedicatedReason::Preferred) + dedicated(ImageDedicatedReason::NoBlock);
    AgcDriver::ProfilePrint_nid_no_patch("[image-blocks] %zu blocks (%zu empty), %.0f MiB reserved, %.0f MiB used, largest free range %.1f MiB; 10 s: %llu allocations, %llu frees, %llu blocks made, %llu released, %llu dedicated fallbacks (large %llu, required %llu, preferred %llu, no block %llu)\n", imageBlocks.size(), empty, reserved / 1048576.0, used / 1048576.0, largest / 1048576.0, delta(&ImageBlockStats::allocations), delta(&ImageBlockStats::frees), delta(&ImageBlockStats::blocksMade), delta(&ImageBlockStats::blocksReleased), fallbacks, dedicated(ImageDedicatedReason::Large), dedicated(ImageDedicatedReason::Required), dedicated(ImageDedicatedReason::Preferred), dedicated(ImageDedicatedReason::NoBlock));
    imageReported = imageTotals;
}

void ImageRange::Release() noexcept {
    if (block == VK_NULL_HANDLE) return;
    const auto memory = std::exchange(block, VK_NULL_HANDLE);
    const auto owner = std::move(pool);
    if (owner) owner->PutImageRange(memory, offset, size);
}

ImageMemoryBinding BindOptimalImageMemory(const Context& context, VkImage image, const VkMemoryRequirements& requirements, const char* what, VkDeviceMemory& dedicated, ImageRange& range) {
    const auto pool = GetBufferPool(context);
    return pool->BindOptimalImage(context, pool, image, requirements, what, dedicated, range);
}

VkDeviceSize ImageBlockSlack(const Context& context) {
    return context.bufferPool != nullptr ? context.bufferPool->ImageSlack() : 0;
}

std::shared_ptr<BufferPool> GetBufferPool(const Context& context) {
    if (!context.bufferPool) context.bufferPool = std::make_shared<BufferPool>(context);
    return context.bufferPool;
}

}
