#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADEROBJECTMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADEROBJECTMEMORY_HPP

#include "Optimization/ResourceProgram.hpp"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace AgcDriver::Graphics {

inline const VkAllocationCallbacks* ShaderObjectAllocator(ShaderRecompiler::PreparedObjectKind kind) {
    static const bool enabled = ShaderRecompiler::PreparedShaderBudgetEnabled() || std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!enabled) return nullptr;
    struct Allocation {
        void* base;
        std::size_t size;
        std::size_t reserved;
    };
    struct Allocator {
        ShaderRecompiler::PreparedObjectKind kind;
        VkAllocationCallbacks callbacks{};
        explicit Allocator(ShaderRecompiler::PreparedObjectKind kind) : kind(kind) {
            callbacks.pUserData = this;
            callbacks.pfnAllocation = Allocate;
            callbacks.pfnReallocation = Reallocate;
            callbacks.pfnFree = Free;
        }
        static VKAPI_ATTR void* VKAPI_CALL Allocate(void* user, std::size_t size, std::size_t alignment, VkSystemAllocationScope) {
            alignment = std::max(alignment, alignof(Allocation));
            if ((alignment & (alignment - 1)) != 0 || alignment - 1 > std::numeric_limits<std::size_t>::max() - sizeof(Allocation)
                || size > std::numeric_limits<std::size_t>::max() - sizeof(Allocation) - (alignment - 1)) return nullptr;
            const auto reserved = size + sizeof(Allocation) + alignment - 1;
            auto* base = std::malloc(reserved);
            if (base == nullptr) return nullptr;
            const auto address = (reinterpret_cast<std::uintptr_t>(base) + sizeof(Allocation) + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
            auto* header = reinterpret_cast<Allocation*>(address) - 1;
            *header = {base, size, reserved};
            ShaderRecompiler::AdjustPreparedObjectMemory(static_cast<Allocator*>(user)->kind, reserved, 0);
            return reinterpret_cast<void*>(address);
        }
        static VKAPI_ATTR void VKAPI_CALL Free(void* user, void* memory) {
            if (memory == nullptr) return;
            const auto allocation = *(static_cast<Allocation*>(memory) - 1);
            ShaderRecompiler::AdjustPreparedObjectMemory(static_cast<Allocator*>(user)->kind, 0, allocation.reserved);
            std::free(allocation.base);
        }
        static VKAPI_ATTR void* VKAPI_CALL Reallocate(void* user, void* original, std::size_t size, std::size_t alignment, VkSystemAllocationScope scope) {
            if (size == 0) { Free(user, original); return nullptr; }
            auto* replacement = Allocate(user, size, alignment, scope);
            if (replacement == nullptr) return nullptr;
            if (original != nullptr) {
                std::memcpy(replacement, original, std::min(size, (static_cast<Allocation*>(original) - 1)->size));
                Free(user, original);
            }
            return replacement;
        }
    };
    static std::array<Allocator, 4> allocators{Allocator(ShaderRecompiler::PreparedObjectKind::Pipeline), Allocator(ShaderRecompiler::PreparedObjectKind::Module), Allocator(ShaderRecompiler::PreparedObjectKind::Layout), Allocator(ShaderRecompiler::PreparedObjectKind::Cache)};
    return &allocators.at(static_cast<std::size_t>(kind) - 1).callbacks;
}

template<class T> class PreparedObjectReference {
public:
    PreparedObjectReference() = default;
    PreparedObjectReference(const std::shared_ptr<T>& object, VkDevice device = VK_NULL_HANDLE) : weak(object), device(reinterpret_cast<std::uintptr_t>(device)) {
        ShaderRecompiler::CachePreparedObject(object, ShaderRecompiler::PreparedObjectKind::Host, [] { return 0; }, this->device);
        if (!ShaderRecompiler::PreparedShaderBudgetEnabled()) strong = object;
    }
    std::shared_ptr<T> Pin() const {
        auto object = strong != nullptr ? strong : weak.lock();
        if (object != nullptr) ShaderRecompiler::CachePreparedObject(object, ShaderRecompiler::PreparedObjectKind::Host, [] { return 0; }, device);
        return object;
    }
    bool Expired() const { return strong == nullptr && weak.expired(); }
    long UseCount() const { return strong != nullptr ? strong.use_count() : weak.use_count(); }

private:
    std::shared_ptr<T> strong;
    std::weak_ptr<T> weak;
    std::uintptr_t device = 0;
};

}

#endif
