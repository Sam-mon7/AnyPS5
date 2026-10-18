#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BUFFERCACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BUFFERCACHE_HPP

#include "Resources.hpp"
#include "BdaAbi.hpp"
#include <array>
#include <optional>

namespace AgcDriver::Graphics::BufferCache {

constexpr std::uint64_t BlockBytes = 4 * 1024 * 1024, BudgetBytes = 64 * 1024 * 1024;
enum class Fallback : std::size_t { Coverage, Size, Alias, Written, Addressable, Mapping, Alignment, Budget, Allocation, Changed, Resident, Count };
struct Statistics {
    std::uint64_t uploaded = 0, copiedBack = 0, hits = 0, misses = 0, wraps = 0, uploads = 0;
    std::uint64_t retired = 0, spills = 0, pendingUploads = 0, gpuWritten = 0, completedWrites = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(Fallback::Count)> fallbacks{};
};

bool Enabled();
bool WritesEnabled();
void PermitBindings(const void* owner, bool permitted);
bool BindingsPermitted(const void* owner);
bool BindReadOnly(const Context& context, const void* owner, std::uint64_t address, std::size_t bytes);
bool BindWritable(const Context& context, const void* owner, std::uint64_t address, std::size_t bytes);
std::optional<VkDescriptorBufferInfo> Descriptor(const void* owner, std::uint64_t address, std::size_t bytes, std::uint32_t& adjustment);
bool HasBindings(const void* owner);
bool Contains(const void* owner, std::uint64_t address, std::size_t bytes);
bool Reusable(const void* owner);
void BeginWrites(const void* owner, Recorder& recorder, std::span<const std::pair<std::uint64_t, std::uint64_t>> writes);
void Flush(std::uint64_t address = 0, std::size_t bytes = SIZE_MAX);
bool FlushForMapping(std::uint64_t address, std::size_t bytes);
void AddressRanges(const void* owner, std::vector<ShaderRecompiler::BdaAbi::Range>& ranges);
void Release(const void* owner);
void Clear(VkDevice device);
void CountFallback(Fallback reason);
Statistics Counters();

}
#endif
