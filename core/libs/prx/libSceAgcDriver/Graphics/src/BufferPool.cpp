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
}

BufferPool::~BufferPool() {
    for (auto* tier : {&smallTier, &largeTier, &deviceTier}) {
        for (const auto& [key, slots] : tier->free) {
            for (const auto& slot : slots) destroy(slot.allocation);
        }
    }
    for (auto& [memory, block] : slabBlocks) freeBlock(*block);
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
    return bytes;
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

std::shared_ptr<BufferPool> GetBufferPool(const Context& context) {
    if (!context.bufferPool) context.bufferPool = std::make_shared<BufferPool>(context);
    return context.bufferPool;
}

}
