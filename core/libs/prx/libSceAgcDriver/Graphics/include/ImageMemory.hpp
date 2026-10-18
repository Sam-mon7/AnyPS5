#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_IMAGEMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_IMAGEMEMORY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>

namespace AgcDriver::Graphics {

class BufferPool;

inline std::uint32_t ImageSuballocationLevel() {
    static const std::uint32_t level = [] {
        const auto* value = std::getenv("APS5_IMAGE_SUBALLOCATION");
        if (value == nullptr) return 0u;
        if (std::strcmp(value, "1") == 0) return 1u;
        if (std::strcmp(value, "2") == 0) return 2u;
        return 0u;
    }();
    return level;
}

inline bool ImageSuballocation() {
    return ImageSuballocationLevel() != 0;
}

inline bool ImageSuballocationIgnoresPreference() {
    return ImageSuballocationLevel() == 2;
}

class RangeAllocator {
public:
    explicit RangeAllocator(std::uint64_t capacity) : capacity(capacity) {
        if (capacity != 0) insert(0, capacity);
    }

    std::optional<std::uint64_t> Allocate(std::uint64_t size, std::uint64_t alignment) {
        if (size == 0) size = 1;
        if (alignment == 0) alignment = 1;
        for (auto candidate = bySize.lower_bound({size, 0}); candidate != bySize.end(); ++candidate) {
            const auto length = candidate->first;
            const auto begin = candidate->second;
            const auto end = begin + length;
            const auto remainder = begin % alignment;
            const auto padding = remainder == 0 ? 0 : alignment - remainder;
            if (padding > length || length - padding < size) continue;
            const auto aligned = begin + padding;
            erase(begin);
            if (padding != 0) insert(begin, padding);
            if (aligned + size < end) insert(aligned + size, end - aligned - size);
            used += size;
            return aligned;
        }
        return std::nullopt;
    }

    bool Free(std::uint64_t offset, std::uint64_t size) {
        if (size == 0) size = 1;
        if (offset > capacity || size > capacity - offset) return false;
        auto next = byOffset.lower_bound(offset);
        if (next != byOffset.end() && next->first < offset + size) return false;
        if (next != byOffset.begin()) {
            const auto previous = std::prev(next);
            if (previous->first + previous->second > offset) return false;
        }
        auto begin = offset;
        auto length = size;
        if (next != byOffset.end() && next->first == offset + size) {
            length += next->second;
            const auto nextBegin = next->first;
            erase(nextBegin);
        }
        next = byOffset.lower_bound(begin);
        if (next != byOffset.begin()) {
            const auto previous = std::prev(next);
            if (previous->first + previous->second == begin) {
                begin = previous->first;
                length += previous->second;
                const auto previousBegin = previous->first;
                erase(previousBegin);
            }
        }
        insert(begin, length);
        used -= size;
        return true;
    }

    std::uint64_t Capacity() const { return capacity; }
    std::uint64_t Used() const { return used; }
    std::uint64_t Available() const { return capacity - used; }
    std::uint64_t LargestFree() const { return bySize.empty() ? 0 : bySize.rbegin()->first; }
    std::size_t FreeRanges() const { return byOffset.size(); }
    bool Empty() const { return used == 0; }

private:
    void insert(std::uint64_t begin, std::uint64_t length) {
        byOffset.emplace(begin, length);
        bySize.emplace(length, begin);
    }

    void erase(std::uint64_t begin) {
        const auto found = byOffset.find(begin);
        bySize.erase({found->second, begin});
        byOffset.erase(found);
    }

    std::uint64_t capacity;
    std::uint64_t used = 0;
    std::map<std::uint64_t, std::uint64_t> byOffset;
    std::set<std::pair<std::uint64_t, std::uint64_t>> bySize;
};

enum class ImageDedicatedReason : std::uint8_t { Large, Required, Preferred, NoBlock, Count };

struct ImageBlockPolicy {
    VkDeviceSize blockBytes = VkDeviceSize{256} << 20u;
    VkDeviceSize dedicatedBytes = VkDeviceSize{64} << 20u;
    bool ignorePreference = false;

    bool Large(VkDeviceSize size) const { return size > dedicatedBytes || size > blockBytes; }

    std::optional<ImageDedicatedReason> Dedicated(VkDeviceSize size, bool required, bool preferred) const {
        if (Large(size)) return ImageDedicatedReason::Large;
        if (required) return ImageDedicatedReason::Required;
        if (preferred && !ignorePreference) return ImageDedicatedReason::Preferred;
        return std::nullopt;
    }
};

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
    std::uint64_t dedicated[static_cast<std::size_t>(ImageDedicatedReason::Count)]{};
};

class ImageRange {
public:
    ImageRange() = default;
    ImageRange(std::shared_ptr<BufferPool> pool, VkDeviceMemory block, VkDeviceSize offset, VkDeviceSize size) noexcept : pool(std::move(pool)), block(block), offset(offset), size(size) {}
    ImageRange(ImageRange&& other) noexcept : pool(std::move(other.pool)), block(std::exchange(other.block, VK_NULL_HANDLE)), offset(other.offset), size(other.size) {}
    ImageRange& operator=(ImageRange&& other) noexcept {
        if (this == &other) return *this;
        Release();
        pool = std::move(other.pool);
        block = std::exchange(other.block, VK_NULL_HANDLE);
        offset = other.offset;
        size = other.size;
        return *this;
    }
    ImageRange(const ImageRange&) = delete;
    ImageRange& operator=(const ImageRange&) = delete;
    ~ImageRange() { Release(); }

    void Release() noexcept;
    bool Valid() const { return block != VK_NULL_HANDLE; }
    VkDeviceMemory Memory() const { return block; }
    VkDeviceSize Offset() const { return offset; }
    VkDeviceSize Size() const { return size; }

private:
    std::shared_ptr<BufferPool> pool;
    VkDeviceMemory block = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

struct ImageMemoryBinding {
    std::uint32_t memoryType = 0;
    bool suballocated = false;
};

ImageMemoryBinding BindOptimalImageMemory(const Context& context, VkImage image, const VkMemoryRequirements& requirements, const char* what, VkDeviceMemory& dedicated, ImageRange& range);

VkDeviceSize ImageBlockSlack(const Context& context);

}

#endif
