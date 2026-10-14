#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUCOPYCHECKPOINTS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUCOPYCHECKPOINTS_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>

namespace AgcDriver::Graphics {

class GpuCopyCheckpoints {
public:
    static constexpr std::size_t Capacity = 16384;
    static constexpr std::uintptr_t Tag = 0xc070000000000000ull;
    static constexpr std::uintptr_t TagMask = 0xffff000000000000ull;
    static constexpr std::uint64_t MaxSerial = 0x7fffffffffffull;
    enum class Kind { Buffer, BufferToImage, ImageToBuffer, Image, Blit, QueryResults };
    struct Entry {
        std::uint64_t serial = 0, commands = 0, source = 0, destination = 0, bindingSerial = 0;
        Kind kind = Kind::Buffer;
        std::uint32_t regionCount = 0, regionWords = 0;
        std::array<std::uint32_t, 20> firstRegion{};
    };

    GpuCopyCheckpoints(VkDevice device, PFN_vkCmdSetCheckpointNV checkpoint) : device(device), checkpoint(checkpoint), entries(std::make_unique<Entry[]>(Capacity)) {
        static_assert(sizeof(std::uintptr_t) == 8);
        if (checkpoint == nullptr) throw std::invalid_argument("copy checkpoints require an enabled checkpoint function");
    }

    static bool IsCopy(std::uintptr_t marker) { return (marker & TagMask) == Tag; }
    static std::uint64_t Serial(std::uintptr_t marker) { return (marker & ~TagMask) >> 1u; }
    static bool IsAfter(std::uintptr_t marker) { return (marker & 1u) != 0; }
    static std::uintptr_t Marker(std::uint64_t serial, bool after) { return Tag | (serial << 1u) | static_cast<std::uintptr_t>(after); }

    template<typename THandle>
    static std::uint64_t Handle(THandle handle) {
        if constexpr (std::is_pointer_v<THandle>) return reinterpret_cast<std::uintptr_t>(handle);
        else return static_cast<std::uint64_t>(handle);
    }

    template<typename TRegion, typename TSource, typename TDestination>
    std::uint64_t Before(VkCommandBuffer commands, Kind kind, TSource source, TDestination destination, std::uint32_t count, const TRegion* regions, std::uint64_t bindingSerial) {
        static_assert(std::is_trivially_copyable_v<TRegion> && sizeof(TRegion) <= sizeof(Entry::firstRegion) && sizeof(TRegion) % sizeof(std::uint32_t) == 0);
        if (count != 0 && regions == nullptr) throw std::invalid_argument("copy checkpoint regions are missing");
        std::uint64_t serial;
        {
            std::lock_guard lock(mutex);
            if (newest == MaxSerial) throw std::overflow_error("copy checkpoint serial exhausted");
            serial = ++newest;
            auto& entry = entries[(serial - 1) % Capacity];
            entry = {serial, Handle(commands), Handle(source), Handle(destination), bindingSerial, kind, count, count != 0 ? static_cast<std::uint32_t>(sizeof(TRegion) / sizeof(std::uint32_t)) : 0};
            if (count != 0) std::memcpy(entry.firstRegion.data(), regions, sizeof(TRegion));
        }
        checkpoint(commands, reinterpret_cast<const void*>(Marker(serial, false)));
        return serial;
    }

    void After(VkCommandBuffer commands, std::uint64_t serial) const {
        checkpoint(commands, reinterpret_cast<const void*>(Marker(serial, true)));
    }

    bool Find(std::uint64_t serial, Entry& result) const {
        std::lock_guard lock(mutex);
        if (serial == 0 || serial > newest || newest - serial >= Capacity) return false;
        result = entries[(serial - 1) % Capacity];
        return result.serial == serial;
    }

    void Report(std::FILE* output) const {
        std::lock_guard lock(mutex);
        const auto count = std::min<std::uint64_t>(newest, Capacity);
        std::fprintf(output, "[gpu-copy] device=0x%llx retained=%llu newest=%llu overwritten=%llu; CPU recording history, not GPU completion; before/after markers are command boundaries at the reported pipeline stage; only the first region is retained\n", static_cast<unsigned long long>(Handle(device)), static_cast<unsigned long long>(count), static_cast<unsigned long long>(newest), static_cast<unsigned long long>(newest - count));
        for (auto serial = newest - count + 1; serial <= newest; ++serial) {
            const auto& entry = entries[(serial - 1) % Capacity];
            char line[1024];
            auto used = std::snprintf(line, sizeof(line), "[gpu-copy] device=0x%llx id=%llu kind=%s commands=0x%llx source=0x%llx destination=0x%llx bindingSerial=%llu regions=%u omitted-regions=%u first-region-u32=", static_cast<unsigned long long>(Handle(device)), static_cast<unsigned long long>(entry.serial), Name(entry.kind), static_cast<unsigned long long>(entry.commands), static_cast<unsigned long long>(entry.source), static_cast<unsigned long long>(entry.destination), static_cast<unsigned long long>(entry.bindingSerial), entry.regionCount, entry.regionCount == 0 ? 0 : entry.regionCount - 1);
            for (std::uint32_t word = 0; word < entry.regionWords; ++word) used += std::snprintf(line + used, sizeof(line) - used, "%s%08x", word != 0 ? "," : "", entry.firstRegion[word]);
            std::fprintf(output, "%s\n", line);
        }
    }

    static const char* Name(Kind kind) {
        switch (kind) {
        case Kind::Buffer: return "VkBufferCopy";
        case Kind::BufferToImage: return "buffer-to-image/VkBufferImageCopy";
        case Kind::ImageToBuffer: return "image-to-buffer/VkBufferImageCopy";
        case Kind::Image: return "VkImageCopy";
        case Kind::Blit: return "VkImageBlit";
        case Kind::QueryResults: return "query-results/u64-first,count,offset,stride,flags";
        }
        throw std::invalid_argument("unknown copy checkpoint kind");
    }

private:
    VkDevice device;
    PFN_vkCmdSetCheckpointNV checkpoint;
    mutable std::mutex mutex;
    std::uint64_t newest = 0;
    // ponytail: retain 16384 commands and their first region; extend only if a fault marker or region is omitted.
    std::unique_ptr<Entry[]> entries;
};

}

#endif
