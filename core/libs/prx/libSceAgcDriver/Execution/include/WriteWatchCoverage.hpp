#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_WRITEWATCHCOVERAGE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_WRITEWATCHCOVERAGE_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <limits>
#include <unordered_map>

namespace AgcDriver::GuestMemory {

// Pages of the write-watched arena excluded from write watching. Every mutation (Initialize,
// Exclude, Restore) runs under the write tracker's mutex; Covers and ExcludedSpan may also run
// without it (slice S2, GEN_LOCKFREE): the masks and chunk pointers are atomics, chunks are only
// freed by the next Initialize (made once, before any lock-free reader), and a reader racing a
// mutation sees each block's mask before or after it.
class WriteWatchCoverage {
public:
    void Initialize(std::uint64_t address, std::size_t bytes) {
        const auto blocks = bytes / BlockBytes + (bytes % BlockBytes != 0);
        Chunks replacement(blocks / ChunkBlocks + (blocks % ChunkBlocks != 0));
        excluded.Swap(replacement);
        base = address;
        size = bytes;
        excludedGenerations.clear();
    }

    bool Covers(std::uint64_t address, std::size_t bytes) const {
        if (bytes == 0 || bytes > size || address < base || address - base > size - bytes) return false;
        const auto first = (address - base) / BlockBytes;
        const auto last = (address - base + bytes - 1) / BlockBytes;
        const auto firstPage = (address - base) % BlockBytes / PageBytes;
        const auto lastPage = (address - base + bytes - 1) % BlockBytes / PageBytes;
        if (first == last) return (excludedMask(first) & pageMask(firstPage, lastPage + 1)) == 0;
        if ((excludedMask(first) & pageMask(firstPage, PagesPerBlock)) != 0 || (excludedMask(last) & pageMask(0, lastPage + 1)) != 0) return false;
        for (auto block = first + 1; block < last;) {
            const auto index = block / ChunkBlocks;
            const auto stop = std::min<std::size_t>(last, (index + 1) * ChunkBlocks);
            const auto* chunk = excluded.Get(index);
            if (chunk != nullptr && std::any_of(chunk->begin() + block % ChunkBlocks, chunk->begin() + (stop - index * ChunkBlocks), [](const auto& mask) { return mask.load(std::memory_order_acquire) != 0; })) return false;
            block = stop;
        }
        return true;
    }

    bool ExcludedSpan(std::uint64_t address, std::size_t bytes, std::uint64_t* begin, std::uint64_t* end) const {
        std::size_t first = 0;
        std::size_t last = 0;
        if (!excludedBlocks(address, bytes, first, last)) return false;
        *begin = base + first * BlockBytes;
        *end = std::min<std::uint64_t>(base + (last + 1) * BlockBytes, base + size);
        return true;
    }
    bool Exclude(std::uint64_t address, std::size_t bytes, std::uint64_t generation = 1) {
        std::size_t first = 0;
        std::size_t last = 0;
        if (!excludedBlocks(address, bytes, first, last)) return false;
        for (auto index = first / ChunkBlocks; index <= last / ChunkBlocks; ++index) excluded.Ensure(index);
        for (auto block = first; block <= last; ++block) excludedGenerations.try_emplace(block, 0);
        bool changed = false;
        for (auto block = first; block <= last; ++block) {
            const auto pages = (std::min<std::uint64_t>(BlockBytes, size - block * BlockBytes) + PageBytes - 1) / PageBytes;
            const auto mask = pageMask(0, pages);
            auto& value = (*excluded.Get(block / ChunkBlocks))[block % ChunkBlocks];
            changed = changed || value.load(std::memory_order_relaxed) != mask;
            auto& stamp = excludedGenerations.at(block);
            stamp = std::max(stamp, std::max<std::uint64_t>(generation, 1));
            value.store(mask, std::memory_order_release);
        }
        return changed;
    }

    bool Restore(std::uint64_t address, std::size_t bytes, std::uint64_t generation = std::numeric_limits<std::uint64_t>::max()) {
        if (bytes == 0 || size == 0) return false;
        if (address < base) {
            if (base - address >= bytes) return false;
            bytes -= base - address;
            address = base;
        }
        const auto offset = address - base;
        if (offset >= size) return false;
        bytes = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, size - offset));
        const auto firstPage = offset / PageBytes + (offset % PageBytes != 0);
        const auto stop = offset + bytes;
        const auto endPage = stop / PageBytes + (stop == size && stop % PageBytes != 0);
        if (firstPage >= endPage) return false;
        const auto first = firstPage / PagesPerBlock;
        const auto last = (endPage - 1) / PagesPerBlock;
        bool changed = false;
        for (auto block = first; block <= last; ++block) {
            const auto mask = pageMask(block == first ? firstPage % PagesPerBlock : 0, block == last ? (endPage - 1) % PagesPerBlock + 1 : PagesPerBlock);
            if ((excludedMask(block) & mask) != 0 && excludedGenerations.at(block) < generation) {
                changed = true;
                auto& value = (*excluded.Get(block / ChunkBlocks))[block % ChunkBlocks];
                const auto remaining = static_cast<std::uint16_t>(value.load(std::memory_order_relaxed) & ~mask);
                value.store(remaining, std::memory_order_release);
                if (remaining == 0) excludedGenerations.erase(block);
            }
        }
        return changed;
    }

private:
    std::uint16_t excludedMask(std::size_t block) const {
        const auto* chunk = excluded.Get(block / ChunkBlocks);
        return chunk != nullptr ? (*chunk)[block % ChunkBlocks].load(std::memory_order_acquire) : 0;
    }
    static std::uint16_t pageMask(std::size_t first, std::size_t end) {
        return static_cast<std::uint16_t>(((1u << end) - 1u) & ~((1u << first) - 1u));
    }
    bool excludedBlocks(std::uint64_t address, std::size_t bytes, std::size_t& first, std::size_t& last) const {
        if (bytes == 0 || size == 0) return false;
        if (address < base) {
            if (base - address >= bytes) return false;
            bytes -= base - address;
            address = base;
        }
        const auto offset = address - base;
        if (offset >= size) return false;
        bytes = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, size - offset));
        first = offset / BlockBytes;
        last = (offset + bytes - 1) / BlockBytes;
        return true;
    }
    static constexpr std::size_t BlockBytes = 65536;
    static constexpr std::size_t PageBytes = 4096;
    static constexpr std::size_t PagesPerBlock = BlockBytes / PageBytes;
    static constexpr std::size_t ChunkBlocks = 4096;
    using MaskChunk = std::array<std::atomic<std::uint16_t>, ChunkBlocks>;
    // Chunk pointers published with release (allocated on the first exclusion that needs them).
    class Chunks {
    public:
        Chunks() = default;
        explicit Chunks(std::size_t count) : count(count), slots(count != 0 ? new std::atomic<MaskChunk*>[count]() : nullptr) {}
        ~Chunks() {
            for (std::size_t i = 0; i < count; ++i) delete slots[i].load(std::memory_order_relaxed);
        }
        Chunks(const Chunks&) = delete;
        Chunks& operator=(const Chunks&) = delete;
        void Swap(Chunks& other) noexcept {
            std::swap(count, other.count);
            slots.swap(other.slots);
        }
        const MaskChunk* Get(std::size_t index) const { return slots[index].load(std::memory_order_acquire); }
        MaskChunk* Get(std::size_t index) { return slots[index].load(std::memory_order_acquire); }
        void Ensure(std::size_t index) {
            if (slots[index].load(std::memory_order_relaxed) == nullptr) slots[index].store(new MaskChunk(), std::memory_order_release);
        }

    private:
        std::size_t count = 0;
        std::unique_ptr<std::atomic<MaskChunk*>[]> slots;
    };
    std::uint64_t base = 0;
    std::size_t size = 0;
    Chunks excluded;
    std::unordered_map<std::uint64_t, std::uint64_t> excludedGenerations;
};

}

#endif
