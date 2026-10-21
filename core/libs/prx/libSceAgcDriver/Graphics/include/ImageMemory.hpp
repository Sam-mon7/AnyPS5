#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_IMAGEMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_IMAGEMEMORY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

class BufferPool;

inline unsigned ImageSuballocationLevel() {
    static const unsigned level = [] {
        const auto* value = std::getenv("APS5_IMAGE_SUBALLOCATION");
        if (value != nullptr && std::strcmp(value, "1") == 0) return 1u;
        if (value != nullptr && std::strcmp(value, "2") == 0) return 2u;
        return 0u;
    }();
    return level;
}

class ImageRangeAllocator {
public:
    struct Range {
        VkDeviceSize offset;
        VkDeviceSize size;
    };

    explicit ImageRangeAllocator(VkDeviceSize capacity) : capacity(capacity) {
        if (capacity != 0) free.push_back({0, capacity});
    }

    std::optional<Range> BestFit(VkDeviceSize size, VkDeviceSize alignment) const {
        if (size == 0 || alignment == 0) return std::nullopt;
        std::optional<Range> best;
        for (const auto& range : free) {
            const auto remainder = range.offset % alignment;
            const auto padding = remainder == 0 ? 0 : alignment - remainder;
            if (padding > range.size || size > range.size - padding) continue;
            if (!best || range.size < best->size) best = Range{range.offset + padding, range.size};
        }
        return best;
    }

    std::optional<VkDeviceSize> Allocate(VkDeviceSize size, VkDeviceSize alignment) {
        const auto best = BestFit(size, alignment);
        if (!best) return std::nullopt;
        const auto needed = free.size() + allocations + 2;
        if (needed > free.capacity()) free.reserve(std::max(needed, free.capacity() * 2));
        auto found = std::find_if(free.begin(), free.end(), [&](const Range& range) { return range.offset <= best->offset && best->offset - range.offset < range.size; });
        const auto end = found->offset + found->size;
        const auto padding = best->offset - found->offset;
        if (padding != 0) {
            found->size = padding;
            ++found;
        } else {
            found = free.erase(found);
        }
        if (best->offset + size < end) free.insert(found, {best->offset + size, end - best->offset - size});
        used += size;
        ++allocations;
        return best->offset;
    }

    bool Free(VkDeviceSize offset, VkDeviceSize size) noexcept {
        if (size == 0 || size > used || offset > capacity || size > capacity - offset || allocations == 0) return false;
        auto next = std::lower_bound(free.begin(), free.end(), offset, [](const Range& range, VkDeviceSize value) { return range.offset < value; });
        if (next != free.end() && next->offset < offset + size) return false;
        if (next != free.begin() && (next - 1)->offset + (next - 1)->size > offset) return false;
        used -= size;
        --allocations;
        if (next != free.end() && next->offset == offset + size) {
            size += next->size;
            next = free.erase(next);
        }
        if (next != free.begin() && (next - 1)->offset + (next - 1)->size == offset) {
            (next - 1)->size += size;
        } else {
            free.insert(next, {offset, size});
        }
        return true;
    }

    VkDeviceSize Capacity() const { return capacity; }
    VkDeviceSize Used() const { return used; }
    bool Empty() const { return used == 0; }
    VkDeviceSize LargestFree() const {
        VkDeviceSize largest = 0;
        for (const auto& range : free) largest = std::max(largest, range.size);
        return largest;
    }

private:
    VkDeviceSize capacity;
    VkDeviceSize used = 0;
    std::size_t allocations = 0;
    std::vector<Range> free;
};

enum class ImageFallback : std::uint8_t { Large, Required, Preferred, NoBlock, Count };

struct ImageBlockStats {
    std::size_t blocks = 0;
    std::size_t emptyBlocks = 0;
    VkDeviceSize reserved = 0;
    VkDeviceSize used = 0;
    VkDeviceSize largestFree = 0;
    std::uint64_t allocations = 0;
    std::uint64_t frees = 0;
    std::uint64_t blocksMade = 0;
    std::uint64_t blocksReleased = 0;
    std::uint64_t fallbacks[static_cast<std::size_t>(ImageFallback::Count)]{};
};

class ImageRange {
public:
    ImageRange() = default;
    ImageRange(std::shared_ptr<BufferPool> pool, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size) noexcept : pool(std::move(pool)), memory(memory), offset(offset), size(size) {}
    ImageRange(ImageRange&& other) noexcept : pool(std::move(other.pool)), memory(std::exchange(other.memory, VK_NULL_HANDLE)), offset(other.offset), size(other.size) {}
    ImageRange& operator=(ImageRange&& other) noexcept {
        if (this == &other) return *this;
        Release();
        pool = std::move(other.pool);
        memory = std::exchange(other.memory, VK_NULL_HANDLE);
        offset = other.offset;
        size = other.size;
        return *this;
    }
    ImageRange(const ImageRange&) = delete;
    ImageRange& operator=(const ImageRange&) = delete;
    ~ImageRange() { Release(); }
    void Release() noexcept;
    bool Valid() const { return memory != VK_NULL_HANDLE; }
    VkDeviceMemory Memory() const { return memory; }
    VkDeviceSize Offset() const { return Valid() ? offset : 0; }

private:
    std::shared_ptr<BufferPool> pool;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

VkDeviceMemory AllocateOptimalImageMemory(const Context& context, const VkMemoryAllocateInfo& allocation, const VkMemoryRequirements& requirements, VkImage image, const char* operation, ImageRange& range, std::uint32_t& memoryType, bool& dedicated);
VkDeviceSize ImageBlockSlack(const Context& context, std::uint32_t heap);

}

#endif
