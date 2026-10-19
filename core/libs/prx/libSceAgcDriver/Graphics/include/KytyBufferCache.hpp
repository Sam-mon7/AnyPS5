#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_KYTYBUFFERCACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_KYTYBUFFERCACHE_HPP

#include "Resources.hpp"
#include "BdaAbi.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace AgcDriver::Graphics::KytyBufferCache {

struct Statistics {
    std::uint64_t descriptors = 0, writtenDescriptors = 0, addressBuilds = 0, addressRanges = 0, unmapped = 0, mapped = 0;
    std::uint64_t invalidations = 0, downloads = 0, faults = 0, uploadBytes = 0, downloadBytes = 0, evictions = 0, buffers = 0, bufferBytes = 0;
};

bool Enabled();
void SetInstance(VkInstance instance, PFN_vkGetInstanceProcAddr getInstanceProcAddr);
std::optional<VkDescriptorBufferInfo> Descriptor(const Context& context, std::uint64_t address, std::size_t bytes, bool written, std::uint32_t& adjustment);
void AddressRanges(const Context& context, std::vector<ShaderRecompiler::BdaAbi::Range>& ranges);
void NoteUnmapped(std::uint64_t address);
void Flush(std::uint64_t address, std::size_t bytes);
void Clear(VkDevice device);
Statistics Counters();

}

#endif
