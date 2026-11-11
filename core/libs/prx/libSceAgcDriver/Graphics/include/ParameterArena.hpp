#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PARAMETERARENA_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PARAMETERARENA_HPP

#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace AgcDriver::Graphics {

class ParameterSlices {
public:
    static constexpr std::size_t Flights = 3;
    static constexpr std::size_t MinimumBytes = 256;
    struct Slice {
        std::size_t chunk = 0;
        std::size_t offset = 0;
        std::size_t bytes = 0;
        std::array<std::uint64_t, Flights> serials{};
    };
    ParameterSlices(std::size_t chunkBytes, std::size_t alignment) : chunkBytes(chunkBytes), alignment(std::max<std::size_t>(alignment, 1)) {}
    static std::size_t ClassBytes(std::size_t bytes, std::size_t alignment) {
        return std::bit_ceil(std::max({bytes, MinimumBytes, alignment}));
    }
    std::optional<Slice> Allocate(std::size_t bytes, std::size_t chunkLimit = std::numeric_limits<std::size_t>::max()) {
        if (bytes == 0) return std::nullopt;
        const auto size = ClassBytes(bytes, alignment);
        if (size > chunkBytes) return std::nullopt;
        auto& free = freed[size];
        if (!free.empty()) {
            auto slice = free.back();
            free.pop_back();
            return slice;
        }
        if (chunks == 0 || cursor + size > chunkBytes) {
            if (chunks >= chunkLimit) return std::nullopt;
            ++chunks;
            cursor = 0;
        }
        Slice slice{chunks - 1, cursor, size, {}};
        cursor += size;
        return slice;
    }
    void Release(const Slice& slice) {
        freed[slice.bytes].push_back(slice);
    }
    static std::optional<std::size_t> Pick(Slice& slice, std::uint64_t openSerial, std::uint64_t completedThrough) {
        std::optional<std::size_t> best;
        for (std::size_t flight = 0; flight < Flights; ++flight) {
            if (slice.serials[flight] > completedThrough) continue;
            if (!best.has_value() || slice.serials[flight] < slice.serials[*best]) best = flight;
        }
        if (best.has_value()) slice.serials[*best] = openSerial;
        return best;
    }
    std::size_t Chunks() const { return chunks; }
    std::size_t ChunkBytes() const { return chunkBytes; }
    std::size_t Freed() const {
        std::size_t count = 0;
        for (const auto& [size, slices] : freed) count += slices.size();
        return count;
    }

private:
    std::size_t chunkBytes;
    std::size_t alignment;
    std::size_t chunks = 0;
    std::size_t cursor = 0;
    std::map<std::size_t, std::vector<Slice>> freed;
};

class ParameterArena;

struct ParameterLease {
    ParameterLease() = default;
    ParameterLease(const ParameterLease&) = delete;
    ParameterLease& operator=(const ParameterLease&) = delete;
    ~ParameterLease();
    std::weak_ptr<ParameterArena> arena;
    ParameterSlices::Slice slice;
};

class ParameterArena {
public:
    static constexpr std::size_t Flights = ParameterSlices::Flights;
    static constexpr std::size_t ChunkBytes = std::size_t{1} << 20u;
    explicit ParameterArena(const Context& context, std::size_t chunkBytes = ChunkBytes) : context(context), slices(chunkBytes, Alignment(context)) {}
    ParameterArena(const ParameterArena&) = delete;
    ParameterArena& operator=(const ParameterArena&) = delete;
    static std::size_t Alignment(const Context& context) {
        return static_cast<std::size_t>(std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 16));
    }
    using Lease = ParameterLease;
    static std::shared_ptr<Lease> Allocate(const std::shared_ptr<ParameterArena>& arena, std::size_t bytes, std::size_t byteLimit = std::numeric_limits<std::size_t>::max()) {
        std::lock_guard lock(arena->mutex);
        auto slice = arena->slices.Allocate(bytes, byteLimit / Flights / arena->slices.ChunkBytes());
        if (!slice.has_value()) return {};
        arena->grow(arena->slices.Chunks());
        auto lease = std::make_shared<Lease>();
        lease->arena = arena;
        lease->slice = *slice;
        return lease;
    }
    VkBuffer Handle(std::size_t flight, std::size_t chunk) const {
        std::lock_guard lock(mutex);
        return buffers[chunk][flight]->Handle();
    }
    std::byte* Bytes(std::size_t flight, std::size_t chunk) {
        std::lock_guard lock(mutex);
        return buffers[chunk][flight]->Bytes().data();
    }
    std::size_t Chunks() const {
        std::lock_guard lock(mutex);
        return buffers.size();
    }
    std::size_t Freed() const {
        std::lock_guard lock(mutex);
        return slices.Freed();
    }

    void Release(const ParameterSlices::Slice& slice) {
        std::lock_guard lock(mutex);
        slices.Release(slice);
    }

private:
    void grow(std::size_t chunks) {
        while (buffers.size() < chunks) {
            std::array<std::unique_ptr<Buffer>, Flights> made;
            for (auto& buffer : made) buffer = std::make_unique<Buffer>(context, slices.ChunkBytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            buffers.push_back(std::move(made));
        }
    }
    Context context;
    mutable std::mutex mutex;
    ParameterSlices slices;
    std::vector<std::array<std::unique_ptr<Buffer>, Flights>> buffers;
};

inline ParameterLease::~ParameterLease() {
    if (auto held = arena.lock()) held->Release(slice);
}

}

#endif
