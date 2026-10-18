#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BUFFERPOOL_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BUFFERPOOL_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ImageMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramBudget.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Graphics {

struct BufferAllocation {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapping;
    VkDeviceAddress address;
    VkDeviceSize allocationBytes;
    std::size_t bytes;
    VkBufferUsageFlags usage;
    VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkDeviceSize offset = 0;
    bool slab = false;
    std::size_t bufferBytes = 0;
    std::uint32_t memoryType = UINT32_MAX;
    VkDeviceSize rebarBytes = 0;
    bool residentMirror = false;
    bool deviceLocal = false;
    bool scratch = false;
};

struct BufferPoolStats {
    VkDeviceSize smallRetained = 0, smallLimit = 0;
    VkDeviceSize largeRetained = 0, largeLimit = 0;
    VkDeviceSize deviceRetained = 0, deviceLimit = 0;
    VkDeviceSize slabBytes = 0;
    VkDeviceSize smallPhysicalRetained = 0, largePhysicalRetained = 0, devicePhysicalRetained = 0;
    VkDeviceSize scratchDeviceRetained = 0, scratchDeviceLimit = 0, scratchHostRetained = 0, scratchHostLimit = 0;
};

struct ScratchStats {
    std::uint64_t hits = 0, misses = 0, evictions = 0, hitBytes = 0, missBytes = 0;
    std::uint64_t fallbacks = 0, fallbackBytes = 0;
    std::uint64_t slots = 0;
    VkDeviceSize retained = 0, limit = 0;
};

struct SlabSlot {
    VkDeviceMemory memory;
    VkDeviceSize offset;
    void* mapping;
};

// Released buffer allocations kept for reuse, since creating, binding and mapping one costs tens of
// microseconds and a build makes several. Small requests are served by size class (the next power of
// two), so the many differently sized data and copied-region buffers of consecutive builds hit;
// requests of a MiB and more keep their exact size (the multi-MiB registered-range snapshots repeat
// exactly, and rounding them would waste pinned host memory). Retained allocations are evicted least
// recently used under a byte budget and a slot count, so a burst of small buffers between two large
// builds no longer sweeps the large ones out. Free slots are kept per size, usage and memory
// properties, so a lookup does not scan the other slots. Reuse is safe because a Buffer is only released once
// the GPU work using it completed (kept until the batch fence, or after a CommandBatch wait).
// APS5_NO_BUFFER_CLASSES=1 matches exact sizes only, as before; APS5_BUFFER_POOL_SLOTS=<n> sets the
// slot count (the bound applies to each tier below, and is held to a sixth of the device's
// maxMemoryAllocationCount so the three tiers keep half of it free).
//
// The size classes and the exact-size allocations are retained in two tiers with a budget each:
// the multi-MiB texture scratch buffers that come back from a batch (GPU-direct uploads and
// write-backs of storage images) filled the one budget, so every one returned evicted hundreds of
// the 256-byte copied-region buffers to fit, and those then missed on every build (81869 misses
// and 80850 evictions per run, each a Vulkan create or destroy under the pool mutex, 3.5 s per
// run). APS5_BUFFER_POOL_SHARED=1 keeps one tier as before.
class BufferPool {
public:
    explicit BufferPool(const Context& context);
    static VkDeviceSize DeviceBudget(const VkPhysicalDeviceMemoryProperties& memory);
    ~BufferPool();
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    // The size a buffer for `bytes` is created with: its size class, or `bytes` itself when large.
    static std::size_t Capacity(std::size_t bytes, VkMemoryPropertyFlags properties);
    static VkBufferUsageFlags Usage(VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
    VkDeviceMemory AllocateMemory(const VkMemoryAllocateInfo& allocation, const char* operation);
    std::optional<BufferAllocation> Take(std::size_t bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, std::uint32_t memoryType = UINT32_MAX, bool scratch = false);
    void Put(const BufferAllocation& allocation) noexcept;
    static bool ScratchEnabled();
    static std::size_t ScratchCapacity(std::size_t bytes);
    bool ScratchEligible(std::size_t bytes, VkMemoryPropertyFlags properties) const;
    void NoteScratchFallback(std::size_t bytes) noexcept;
    ScratchStats ScratchCounters(bool device);
    static bool SlabEligible(std::size_t capacity, VkDeviceSize alignment, VkDeviceSize size, VkDeviceSize atom);
    static VkDeviceSize SlabBlockBytes(std::size_t capacity);
    SlabSlot TakeSlot(const Context& context, std::uint32_t memoryType, std::size_t capacity, bool addressable, bool rebar = false);
    VkDeviceSize PutSlot(VkDeviceMemory memory, VkDeviceSize offset) noexcept;
    std::size_t SlabBlocks();
    BufferPoolStats Stats();
    VramBudget& Vram() { return vram; }
    static ImageBlockPolicy ImagePolicyFromEnvironment();
    void SetImagePolicy(const ImageBlockPolicy& policy);
    ImageMemoryBinding BindOptimalImage(const Context& context, const std::shared_ptr<BufferPool>& self, VkImage image, const VkMemoryRequirements& requirements, const char* what, VkDeviceMemory& dedicated, ImageRange& range);
    void PutImageRange(VkDeviceMemory block, VkDeviceSize offset, VkDeviceSize size) noexcept;
    ImageBlockStats ImageStats();
    VkDeviceSize ImageSlack() const;

private:
    struct SlabBlock {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::byte* mapping = nullptr;
        VkDeviceSize slotBytes = 0;
        std::vector<std::uint32_t> free;
        std::uint32_t used = 0;
        std::uint64_t slab = 0;
        VkDeviceSize rebarBytes = 0;
    };
    struct Slab {
        std::vector<SlabBlock*> available;
        std::size_t emptyBlocks = 0;
    };
    static std::uint64_t SlabKey(std::uint32_t memoryType, std::size_t capacity, bool addressable);
    void freeBlock(SlabBlock& block) noexcept;
    struct ImageBlock {
        ImageBlock(VkDeviceMemory memory, std::uint32_t type, VkDeviceSize bytes) : memory(memory), type(type), ranges(bytes) {}
        VkDeviceMemory memory;
        std::uint32_t type;
        RangeAllocator ranges;
    };
    struct ImageGrant {
        VkDeviceMemory memory;
        VkDeviceSize offset;
    };
    std::optional<ImageGrant> takeImageRange(const Context& context, std::uint32_t type, const VkMemoryRequirements& requirements);
    std::optional<ImageGrant> grantFromImageBlocks(std::uint32_t type, const VkMemoryRequirements& requirements);
    std::size_t emptyImageBlocks(std::uint32_t type) const;
    bool deviceHeapHasRoom(const Context& context, std::uint32_t type, VkDeviceSize bytes) const;
    VkDeviceSize releaseEmptyImageBlocks() noexcept;
    void reportImageBlocks();
    struct Slot {
        BufferAllocation allocation;
        std::uint64_t lastUse;
    };
    struct SlotKey {
        std::size_t bytes;
        VkBufferUsageFlags usage;
        VkMemoryPropertyFlags properties;
        std::uint32_t memoryType;
        bool operator==(const SlotKey&) const = default;
    };
    struct SlotKeyHash {
        std::size_t operator()(const SlotKey& key) const noexcept {
            return std::hash<std::size_t>{}(key.bytes) ^ (static_cast<std::size_t>(key.usage) << 32u) ^ (static_cast<std::size_t>(key.properties) << 48u) ^ std::hash<std::uint32_t>{}(key.memoryType);
        }
    };
    // One retention tier: its slots by key (each list oldest first), their count and bytes, the
    // byte budget they are evicted under and its counters (APS5_PROFILE_DRAW, reported every 10 s
    // from Take).
    struct Tier {
        std::unordered_map<SlotKey, std::deque<Slot>, SlotKeyHash> free;
        std::size_t slots = 0;
        VkDeviceSize retainedBytes = 0;
        VkDeviceSize physicalRetained = 0;
        std::unordered_map<VkDeviceMemory, std::size_t> retainedSlabs;
        VkDeviceSize budget = 0;
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::uint64_t hitBytes = 0;
        std::uint64_t missBytes = 0;
    };
    // The tier a buffer of `capacity` and `properties` is retained in (the large one for everything
    // when shared; the device tier for device-local memory while it has a budget).
    Tier& tierFor(std::size_t capacity, VkMemoryPropertyFlags properties);
    static bool DeviceTierEnabled();
    static bool DeviceTiered(VkMemoryPropertyFlags properties);
    static std::size_t NextCapacity(std::size_t capacity);
    static VkDeviceSize ScratchBudget(bool device);
    Tier& scratchTier(VkMemoryPropertyFlags properties);
    const Tier& scratchTier(VkMemoryPropertyFlags properties) const;
    void putScratch(const BufferAllocation& allocation) noexcept;
    VkDeviceSize destroy(const BufferAllocation& allocation) noexcept;
    VkDeviceSize trim();
    // Moves the tier's least recently used slot (the oldest front of its lists) to `evicted`; the
    // caller destroys those after releasing the mutex, so builds taking buffers on other threads
    // do not wait behind the Vulkan destroy calls. Nothing changes when the vector cannot grow.
    void evictOldest(Tier& tier, std::vector<BufferAllocation>& evicted);
    // The retained-slot bound of each tier (APS5_BUFFER_POOL_SLOTS, default `defaultSlots`), read once.
    static std::size_t MaxSlots();
    static VkDeviceSize retentionCost(const Tier& tier, const BufferAllocation& allocation);
    static void removeRetained(Tier& tier, const BufferAllocation& allocation);
    std::size_t maxSlots;
    VkDevice device;
    PFN_vkUnmapMemory unmap;
    PFN_vkDestroyBuffer destroyBuffer;
    PFN_vkFreeMemory freeMemory;
    PFN_vkAllocateMemory allocateMemory;
    PFN_vkMapMemory mapMemory;
    std::mutex mutex;
    // Not `small`/`large`: <rpcndr.h> (via <windows.h>) defines `small` as a macro.
    Tier smallTier;
    Tier largeTier;
    Tier deviceTier;
    Tier scratchDeviceTier;
    Tier scratchHostTier;
    std::atomic<std::uint64_t> scratchFallbacks{0};
    std::atomic<std::uint64_t> scratchFallbackBytes{0};
    std::uint64_t clock = 0;
    std::mutex slabMutex;
    std::unordered_map<std::uint64_t, Slab> slabs;
    std::unordered_map<VkDeviceMemory, std::unique_ptr<SlabBlock>> slabBlocks;
    std::mutex imageMutex;
    std::unordered_map<VkDeviceMemory, std::unique_ptr<ImageBlock>> imageBlocks;
    std::map<std::uint32_t, std::vector<ImageBlock*>> imageTypes;
    ImageBlockPolicy imagePolicy;
    ImageBlockStats imageTotals;
    ImageBlockStats imageReported;
    std::chrono::steady_clock::time_point imageReportedAt = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point imageRetryAt{};
    std::atomic<VkDeviceSize> imageReserved{0};
    std::atomic<VkDeviceSize> imageUsed{0};
    VramBudget vram;
    std::uint64_t vramDropped = 0;
    VkDeviceSize vramDroppedBytes = 0;
    static constexpr VkDeviceSize budget = 512ull * 1024 * 1024;
    // The small tier's own budget (slots of at most half a MiB each): pinned host memory the large
    // tier's budget does not count.
    static constexpr VkDeviceSize smallBudget = 64ull * 1024 * 1024;
    // Requests of this size and more keep their exact size and go to the large tier.
    static constexpr std::size_t classLimit = std::size_t{1} << 20u;
    static constexpr unsigned deviceClassBits = 3;
    static constexpr std::size_t scratchMinBytes = std::size_t{64} << 10u;
    static constexpr std::size_t defaultSlots = 4096;
};

std::shared_ptr<BufferPool> GetBufferPool(const Context& context);

}

#endif
