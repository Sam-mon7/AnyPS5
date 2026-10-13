#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUCHECKPOINTS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUCHECKPOINTS_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>

namespace AgcDriver::Graphics {

class GpuCheckpoints {
public:
    static constexpr std::size_t Capacity = 16384;

    GpuCheckpoints(PFN_vkCmdSetCheckpointNV record, PFN_vkGetQueueCheckpointDataNV query, VkQueue queue) : record(record), query(query), queue(queue) {}

    void Record(VkCommandBuffer commands, const char* operation, std::span<const std::uint64_t> shaders) {
        std::lock_guard lock(mutex);
        const auto serial = ++next;
        auto& entry = history[serial % Capacity];
        entry = {serial, operation};
        entry.count = std::min(shaders.size(), entry.shaders.size());
        std::copy_n(shaders.begin(), entry.count, entry.shaders.begin());
        // The driver carries this opaque integer; it never points at reused history storage.
        record(commands, reinterpret_cast<const void*>(serial));
    }

    void Report(VkResult result, const char* operation, FILE* output = stderr) {
        if (result != VK_ERROR_DEVICE_LOST) return;
        std::lock_guard lock(mutex);
        std::uint32_t count = 0;
        query(queue, &count, nullptr);
        std::array<VkCheckpointDataNV, 32> checkpoints{};
        for (auto& checkpoint : checkpoints) checkpoint.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
        const auto available = count;
        count = std::min(count, static_cast<std::uint32_t>(checkpoints.size()));
        if (count != 0) query(queue, &count, checkpoints.data());
        std::fprintf(output, "[gpu-checkpoint] device lost at %s; %u/%u stage markers, newest recorded=%llu\n", operation, count, available, static_cast<unsigned long long>(next));
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto serial = reinterpret_cast<std::uintptr_t>(checkpoints[i].pCheckpointMarker);
            const auto& entry = history[serial % Capacity];
            std::fprintf(output, "[gpu-checkpoint] stage=0x%x serial=%llu", static_cast<unsigned>(checkpoints[i].stage), static_cast<unsigned long long>(serial));
            if (serial != 0 && entry.serial == serial) {
                std::fprintf(output, " %s", entry.operation);
                for (std::size_t j = 0; j < entry.count; ++j) std::fprintf(output, " 0x%llx", static_cast<unsigned long long>(entry.shaders[j]));
            } else {
                std::fprintf(output, " history expired or unavailable");
            }
            std::fputc('\n', output);
        }
        std::fflush(output);
    }

private:
    struct Entry {
        std::uintptr_t serial = 0;
        const char* operation = nullptr;
        std::array<std::uint64_t, 16> shaders{};
        std::size_t count = 0;
    };
    PFN_vkCmdSetCheckpointNV record;
    PFN_vkGetQueueCheckpointDataNV query;
    VkQueue queue;
    std::mutex mutex;
    std::uintptr_t next = 0;
    std::array<Entry, Capacity> history{};
};

}

#endif
