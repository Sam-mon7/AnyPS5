#ifndef ANYPS5_KYTY_ADAPTERS_COMMON_DEBUGCOUNTERS_H
#define ANYPS5_KYTY_ADAPTERS_COMMON_DEBUGCOUNTERS_H

#include <cstdint>

namespace Common::DebugCounters {

enum class Counter { BufferUploadBytes, BufferDownloadBytes, BufferEvictions, Count };

void Add(Counter counter, std::uint64_t value = 1);
std::uint64_t Take(Counter counter);

}

#endif
