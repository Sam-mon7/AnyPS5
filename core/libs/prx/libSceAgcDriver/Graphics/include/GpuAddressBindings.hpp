#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUADDRESSBINDINGS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUADDRESSBINDINGS_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>

namespace AgcDriver::Graphics {

class GpuAddressBindings {
public:
    static constexpr std::size_t Capacity = 1048576;
    static constexpr std::size_t ObjectLimit = 8;
    static constexpr std::size_t MatchLimit = 32;

    struct Entry {
        std::uint64_t serial = 0;
        VkDeviceAddress base = 0;
        VkDeviceSize size = 0;
        std::uint64_t object = 0;
        VkObjectType objectType = VK_OBJECT_TYPE_UNKNOWN;
        VkDeviceAddressBindingTypeEXT bindingType = VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT;
        VkDeviceAddressBindingFlagsEXT flags = 0;
        std::chrono::steady_clock::time_point observedAt{};
    };
    struct Matches {
        std::array<Entry, MatchLimit> entries{};
        std::size_t count = 0;
        std::uint64_t total = 0;
        std::uint64_t oldest = 0;
        std::uint64_t newest = 0;
        std::uint64_t overwritten = 0;
        std::uint64_t omittedObjects = 0;
        std::uint64_t droppedCallbacks = 0;
    };

    explicit GpuAddressBindings(std::uint64_t instanceSerial) : instanceSerial(instanceSerial) {}

    static VKAPI_ATTR VkBool32 VKAPI_CALL Callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) noexcept {
        if (user == nullptr || severity != VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT || (types & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) == 0) return VK_FALSE;
        auto& self = *static_cast<GpuAddressBindings*>(user);
        try {
            const auto* next = data != nullptr ? static_cast<const VkBaseInStructure*>(data->pNext) : nullptr;
            std::size_t walked = 0;
            while (next != nullptr && next->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT && walked++ < 32) next = next->pNext;
            if (next == nullptr || next->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT || data->objectCount == 0 || data->pObjects == nullptr) {
                self.droppedCallbacks.fetch_add(1, std::memory_order_relaxed);
                return VK_FALSE;
            }
            const auto& binding = *reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(next);
            std::lock_guard lock(self.mutex);
            const auto observedAt = std::chrono::steady_clock::now();
            const auto count = std::min<std::size_t>(data->objectCount, ObjectLimit);
            self.omittedObjects += data->objectCount - count;
            for (std::size_t i = 0; i < count; ++i) {
                const auto serial = ++self.nextSerial;
                const auto& object = data->pObjects[i];
                self.history[serial % Capacity] = {serial, binding.baseAddress, binding.size, object.objectHandle, object.objectType, binding.bindingType, binding.flags, observedAt};
            }
        } catch (...) {
            self.droppedCallbacks.fetch_add(1, std::memory_order_relaxed);
        }
        return VK_FALSE;
    }

    static bool Overlaps(const Entry& entry, VkDeviceAddress first, VkDeviceAddress last) {
        return first <= last && entry.size != 0 && entry.base <= last && (entry.base >= first || entry.size > first - entry.base);
    }

    Matches Find(VkDeviceAddress first, VkDeviceAddress last, VkObjectType objectType = VK_OBJECT_TYPE_UNKNOWN, std::uint64_t object = 0) const {
        Matches result;
        std::lock_guard lock(mutex);
        result.newest = nextSerial;
        const auto retained = std::min<std::uint64_t>(nextSerial, Capacity);
        result.oldest = retained != 0 ? nextSerial - retained + 1 : 0;
        result.overwritten = nextSerial - retained;
        result.omittedObjects = omittedObjects;
        result.droppedCallbacks = droppedCallbacks.load(std::memory_order_relaxed);
        // ponytail: scan the fixed ring only on diagnostics; index it only if fault-time cost matters.
        for (std::uint64_t offset = 0; offset < retained; ++offset) {
            const auto& entry = history[(nextSerial - offset) % Capacity];
            if (!Overlaps(entry, first, last)) continue;
            if (objectType != VK_OBJECT_TYPE_UNKNOWN && (entry.objectType != objectType || entry.object != object)) continue;
            ++result.total;
            if (result.count < result.entries.size()) result.entries[result.count++] = entry;
        }
        return result;
    }

    void Report(const VkDeviceFaultAddressInfoEXT& fault, FILE* output) const {
        const auto precision = fault.addressPrecision;
        if (precision == 0 || (precision & (precision - 1)) != 0) {
            std::fprintf(output, "[gpu-binding] instance=%llu invalid precision=0x%llx; overlap report unavailable\n", static_cast<unsigned long long>(instanceSerial), static_cast<unsigned long long>(precision));
            return;
        }
        const auto first = fault.reportedAddress & ~(precision - 1);
        const auto last = fault.reportedAddress | (precision - 1);
        const auto matches = Find(first, last);
        const auto reportedAt = std::chrono::steady_clock::now();
        std::fprintf(output, "[gpu-binding] instance=%llu interval=0x%llx..0x%llx newest overlaps=%zu/%llu retained serials=%llu..%llu overwritten=%llu omitted-objects=%llu dropped-callbacks=%llu (CPU callback order; no GPU progress or fault causality)\n", static_cast<unsigned long long>(instanceSerial), static_cast<unsigned long long>(first), static_cast<unsigned long long>(last), matches.count, static_cast<unsigned long long>(matches.total), static_cast<unsigned long long>(matches.oldest), static_cast<unsigned long long>(matches.newest), static_cast<unsigned long long>(matches.overwritten), static_cast<unsigned long long>(matches.omittedObjects), static_cast<unsigned long long>(matches.droppedCallbacks));
        for (std::size_t i = 0; i < matches.count; ++i) {
            const auto& entry = matches.entries[i];
            const char* operation = entry.bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT ? "bind" : entry.bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT ? "unbind" : "unknown";
            const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(reportedAt - entry.observedAt).count();
            std::fprintf(output, "[gpu-binding] instance=%llu serial=%llu %s base=0x%llx size=0x%llx object-type=%d object=0x%llx flags=0x%x cpu-age-at-report-ms=%llu\n", static_cast<unsigned long long>(instanceSerial), static_cast<unsigned long long>(entry.serial), operation, static_cast<unsigned long long>(entry.base), static_cast<unsigned long long>(entry.size), static_cast<int>(entry.objectType), static_cast<unsigned long long>(entry.object), static_cast<unsigned>(entry.flags), static_cast<unsigned long long>(ageMs));
        }
        std::fputs("[gpu-binding] Historical aliases and unbinds are candidates, not live ownership; old bindings may have rolled out, and sparse or driver-private ranges may be unreported. No match does not prove no allocation. CPU ages measure callback recording to diagnostic collection, not GPU fault time.\n", output);
    }

private:
    const std::uint64_t instanceSerial;
    mutable std::mutex mutex;
    std::unique_ptr<Entry[]> history = std::make_unique<Entry[]>(Capacity);
    std::uint64_t nextSerial = 0;
    std::uint64_t omittedObjects = 0;
    std::atomic<std::uint64_t> droppedCallbacks{0};
};

}

#endif
