#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BUFFERCACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BUFFERCACHE_HPP

#include "Resources.hpp"
#include <array>
#include <optional>

namespace AgcDriver::Graphics::BufferCache {

constexpr std::uint64_t BlockBytes = 4 * 1024 * 1024, BudgetBytes = 64 * 1024 * 1024;
enum class Fallback : std::size_t { Coverage, Size, Alias, Written, Addressable, Mapping, Alignment, Budget, Allocation, Changed, Resident, Count };
struct Statistics {
    std::uint64_t uploaded = 0, copiedBack = 0, hits = 0, misses = 0, wraps = 0, uploads = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(Fallback::Count)> fallbacks{};
};

bool Enabled();
void PermitBindings(const void* owner, bool permitted);
bool BindingsPermitted(const void* owner);
bool BindReadOnly(const Context& context, const void* owner, std::uint64_t address, std::size_t bytes);
std::optional<VkDescriptorBufferInfo> Descriptor(const void* owner, std::uint64_t address, std::size_t bytes, std::uint32_t& adjustment);
bool HasBindings(const void* owner);
void Release(const void* owner);
void Clear(VkDevice device);
void CountFallback(Fallback reason);
Statistics Counters();

}
#endif
