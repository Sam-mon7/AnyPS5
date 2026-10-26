#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_WRITEWATCHCOVERAGE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_WRITEWATCHCOVERAGE_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <limits>
#include <unordered_map>

namespace AgcDriver::GuestMemory {

class WriteWatchCoverage {
public:
    void Initialize(std::uint64_t address, std::size_t bytes) {
        const auto blocks = bytes / BlockBytes + (bytes % BlockBytes != 0);
        std::vector<std::unique_ptr<MaskChunk>> replacement(blocks / ChunkBlocks + (blocks % ChunkBlocks != 0));
        excluded.swap(replacement);
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
            const auto& chunk = excluded[index];
            if (chunk != nullptr && std::any_of(chunk->begin() + block % ChunkBlocks, chunk->begin() + (stop - index * ChunkBlocks), [](auto mask) { return mask != 0; })) return false;
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
        for (auto index = first / ChunkBlocks; index <= last / ChunkBlocks; ++index) {
            if (excluded[index] == nullptr) excluded[index] = std::make_unique<MaskChunk>();
        }
        for (auto block = first; block <= last; ++block) excludedGenerations.try_emplace(block, 0);
        bool changed = false;
        for (auto block = first; block <= last; ++block) {
            const auto pages = (std::min<std::uint64_t>(BlockBytes, size - block * BlockBytes) + PageBytes - 1) / PageBytes;
            const auto mask = pageMask(0, pages);
            auto& value = (*excluded[block / ChunkBlocks])[block % ChunkBlocks];
            changed = changed || value != mask;
            auto& stamp = excludedGenerations.at(block);
            stamp = std::max(stamp, std::max<std::uint64_t>(generation, 1));
            value = mask;
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
                auto& value = (*excluded[block / ChunkBlocks])[block % ChunkBlocks];
                value &= static_cast<std::uint16_t>(~mask);
                if (value == 0) excludedGenerations.erase(block);
            }
        }
        return changed;
    }

private:
    std::uint16_t excludedMask(std::size_t block) const {
        const auto& chunk = excluded[block / ChunkBlocks];
        return chunk != nullptr ? (*chunk)[block % ChunkBlocks] : 0;
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
    using MaskChunk = std::array<std::uint16_t, ChunkBlocks>;
    std::uint64_t base = 0;
    std::size_t size = 0;
    std::vector<std::unique_ptr<MaskChunk>> excluded;
    std::unordered_map<std::uint64_t, std::uint64_t> excludedGenerations;
};

}

#endif
