#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUCHECKPOINTS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUCHECKPOINTS_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include "GpuAddressBindings.hpp"
#include <memory>
#include <utility>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

class GpuCheckpoints {
public:
    static constexpr std::size_t Capacity = 16384;

    GpuCheckpoints(PFN_vkCmdSetCheckpointNV record, PFN_vkGetQueueCheckpointDataNV query, VkQueue queue, VkDevice device = VK_NULL_HANDLE, PFN_vkGetDeviceFaultInfoEXT queryFault = nullptr, std::shared_ptr<GpuAddressBindings> bindings = {}) : record(record), query(query), queue(queue), device(device), queryFault(queryFault), bindings(std::move(bindings)) {}

    std::shared_ptr<GpuAddressBindings> AddressBindings() const { return bindings; }

    void Record(VkCommandBuffer commands, const char* operation, std::span<const std::uint64_t> shaders) {
        if (record == nullptr) return;
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
        reportFault(operation, output);
        if (query == nullptr) {
            std::fflush(output);
            return;
        }
        std::uint32_t count = 0;
        query(queue, &count, nullptr);
        std::array<VkCheckpointDataNV, 32> checkpoints{};
        for (auto& checkpoint : checkpoints) checkpoint.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
        const auto available = count;
        count = std::min(count, static_cast<std::uint32_t>(checkpoints.size()));
        if (count != 0) query(queue, &count, checkpoints.data());
        std::fprintf(output, "[gpu-checkpoint] device lost at %s; %u/%u stage markers, newest recorded=%llu\n", operation, count, available, static_cast<unsigned long long>(next));
        std::uintptr_t latest = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto serial = reinterpret_cast<std::uintptr_t>(checkpoints[i].pCheckpointMarker);
            const auto& entry = history[serial % Capacity];
            std::fprintf(output, "[gpu-checkpoint] stage=0x%x serial=%llu", static_cast<unsigned>(checkpoints[i].stage), static_cast<unsigned long long>(serial));
            if (serial != 0 && entry.serial == serial) {
                latest = std::max(latest, serial);
                std::fprintf(output, " %s", entry.operation);
                for (std::size_t j = 0; j < entry.count; ++j) std::fprintf(output, " 0x%llx", static_cast<unsigned long long>(entry.shaders[j]));
            } else {
                std::fprintf(output, " history expired or unavailable");
            }
            std::fputc('\n', output);
        }
        if (latest < next) {
            const auto begin = latest != 0 ? latest : next - std::min<std::uint64_t>(next, 32);
            const auto end = begin + std::min<std::uint64_t>(next - begin, 32);
            std::fprintf(output, latest != 0 ? "[gpu-checkpoint] following recorded markers (CPU order, GPU completion unknown): %llu of %llu\n" : "[gpu-checkpoint] recent recorded markers (CPU order, no GPU progress available): %llu of %llu\n", static_cast<unsigned long long>(end - begin), static_cast<unsigned long long>(next - latest));
            for (auto serial = begin + 1; serial <= end; ++serial) {
                const auto& entry = history[serial % Capacity];
                std::fprintf(output, "[gpu-checkpoint] recorded serial=%llu", static_cast<unsigned long long>(serial));
                if (entry.serial == serial) {
                    std::fprintf(output, " %s", entry.operation);
                    for (std::size_t j = 0; j < entry.count; ++j) std::fprintf(output, " 0x%llx", static_cast<unsigned long long>(entry.shaders[j]));
                } else {
                    std::fprintf(output, " history expired or unavailable");
                }
                std::fputc('\n', output);
            }
        }
        std::fflush(output);
    }

private:
    void reportFault(const char* operation, FILE* output) noexcept {
        if (queryFault == nullptr || faultReported) return;
        faultReported = true;
        try {
            VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
            auto result = queryFault(device, &counts, nullptr);
            if (result != VK_SUCCESS) {
                std::fprintf(output, "[gpu-fault] device lost at %s; count query failed: Vulkan result %d\n", operation, static_cast<int>(result));
                return;
            }
            const auto availableAddresses = counts.addressInfoCount;
            const auto availableVendors = counts.vendorInfoCount;
            std::vector<VkDeviceFaultAddressInfoEXT> addresses(std::min(availableAddresses, 256u));
            std::vector<VkDeviceFaultVendorInfoEXT> vendors(std::min(availableVendors, 256u));
            counts.addressInfoCount = static_cast<std::uint32_t>(addresses.size());
            counts.vendorInfoCount = static_cast<std::uint32_t>(vendors.size());
            counts.vendorBinarySize = 0;
            VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
            info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
            info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
            result = queryFault(device, &counts, &info);
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                std::fprintf(output, "[gpu-fault] device lost at %s; data query failed: Vulkan result %d\n", operation, static_cast<int>(result));
                return;
            }
            const auto addressCount = std::min(counts.addressInfoCount, static_cast<std::uint32_t>(addresses.size()));
            const auto vendorCount = std::min(counts.vendorInfoCount, static_cast<std::uint32_t>(vendors.size()));
            std::fprintf(output, "[gpu-fault] device lost at %s; %.*s; addresses %u/%u, vendor entries %u/%u; Vulkan result %d%s\n", operation, static_cast<int>(VK_MAX_DESCRIPTION_SIZE), info.description, addressCount, availableAddresses, vendorCount, availableVendors, static_cast<int>(result), result == VK_INCOMPLETE || addressCount < availableAddresses || vendorCount < availableVendors ? " (partial)" : "");
            for (std::uint32_t i = 0; i < addressCount; ++i) {
                const auto& address = addresses[i];
                std::fprintf(output, "[gpu-fault] address type=%d reported=0x%llx precision=0x%llx\n", static_cast<int>(address.addressType), static_cast<unsigned long long>(address.reportedAddress), static_cast<unsigned long long>(address.addressPrecision));
                if (bindings) bindings->Report(address, output);
            }
            for (std::uint32_t i = 0; i < vendorCount; ++i) {
                const auto& vendor = vendors[i];
                std::fprintf(output, "[gpu-fault] vendor %.*s code=0x%llx data=0x%llx\n", static_cast<int>(VK_MAX_DESCRIPTION_SIZE), vendor.description, static_cast<unsigned long long>(vendor.vendorFaultCode), static_cast<unsigned long long>(vendor.vendorFaultData));
            }
        } catch (...) {
            std::fprintf(output, "[gpu-fault] device lost at %s; diagnostic collection failed\n", operation);
        }
    }

    struct Entry {
        std::uintptr_t serial = 0;
        const char* operation = nullptr;
        std::array<std::uint64_t, 16> shaders{};
        std::size_t count = 0;
    };
    PFN_vkCmdSetCheckpointNV record;
    PFN_vkGetQueueCheckpointDataNV query;
    VkQueue queue;
    VkDevice device;
    PFN_vkGetDeviceFaultInfoEXT queryFault;
    std::shared_ptr<GpuAddressBindings> bindings;
    bool faultReported = false;
    std::mutex mutex;
    std::uintptr_t next = 0;
    std::array<Entry, Capacity> history{};
};

}

#endif
