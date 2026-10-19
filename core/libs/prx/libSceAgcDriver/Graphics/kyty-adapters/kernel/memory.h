#ifndef ANYPS5_KYTY_ADAPTERS_KERNEL_MEMORY_H
#define ANYPS5_KYTY_ADAPTERS_KERNEL_MEMORY_H

#include <cstdint>

namespace Libs::LibKernel::Memory {

inline constexpr std::uint64_t kExtendedMemoryBase = std::uint64_t{1} << 40u;
inline constexpr std::uint64_t kExtendedMemorySize = 0;

bool CopySparseMemory(std::uint64_t address, void* destination, std::uint64_t size);
bool TryReadSparseBacking(std::uint64_t address, void* destination, std::uint64_t size);
void WriteBacking(std::uint64_t address, const void* source, std::uint64_t size);

}

#endif
