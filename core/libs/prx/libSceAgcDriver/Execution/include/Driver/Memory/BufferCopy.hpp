#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_BUFFERCOPY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_BUFFERCOPY_HPP

#include "Recompiler.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

inline constexpr std::size_t KnownValueMaxBytes = 4096;

extern std::atomic<std::uint64_t> knownValueEntries, knownValueReads, knownValueVerified, knownValueMismatches;

enum CopyPath { CopyCpu = 0, CopyGpu = 1, CopyNotImported = 2, CopyAliased = 3, CopyRecordPending = 4, CopyShape = 5, CopyOverlapping = 6, CopyInaccessible = 7, CopyOverGpuMax = 8, CopyPaths = 9 };

extern std::array<std::atomic<std::uint64_t>, CopyPaths> copyPathCounts;
// The CopyPath the calling thread counted last (diagnostics; -1 after a reset).
int& LastCopyPathOnThread();

struct StreamCopy {
    std::uint64_t source = 0;
    std::uint64_t destination = 0;
    std::size_t bytes = 0;
};

bool MatchesStreamCopyKernel(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const ShaderRecompiler::ShaderComputeStageInfo& compute);
std::optional<StreamCopy> StreamCopyRange(std::span<const std::uint32_t> userData, std::uint32_t groups);
bool MatchesIndexedCopyKernel(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const ShaderRecompiler::ShaderComputeStageInfo& compute);
std::optional<StreamCopy> IndexedCopyRange(std::span<const std::uint32_t> userData, std::uint32_t groups, std::uint32_t count, std::uint32_t sourceOffset, std::uint32_t destinationOffset);

// The three copy kernels the driver copies itself (Driver::copyBuffer): counted (a count record of
// {count, period}), indexed (a record of {count, source offset, destination offset}) and stream.
enum class CopyKernelKind : std::uint8_t { Counted, Indexed, Stream };
using CopyRange = std::pair<std::uint64_t, std::uint64_t>;
// Every byte a copy kernel of `groups` x 64 threads may read or write, bounded by its descriptors'
// record counts before its record is read (stream: exact; record empty for it). Empty ranges for a
// shape copyBuffer refuses outright.
struct CopyKernelBounds {
    CopyRange source{};
    CopyRange destination{};
    CopyRange record{};
};
std::optional<CopyKernelBounds> CopyKernelRanges(CopyKernelKind kind, std::span<const std::uint32_t> userData, std::uint32_t groups);

// APS5_ARCHB_PIPE_COPY_KERNEL_ITEMS: queue-0 copy kernels committed as pipeline items (recorder
// counters pipe_copy_item_<name>): enqueued; copied by the committer; the committer's fallback copy
// (the device refused: an unimported range); refused on queue 0 (they drain and copy inline) because
// of their kind (indexed), shape, count record (a queued or recorded write over it, a pending image
// or label), or because labels were deferred; verify-mode record mismatches (must stay 0).
enum CopyItemCounter : std::size_t { CopyItemEnqueued, CopyItemCopied, CopyItemFallback, CopyItemRefusedKind, CopyItemRefusedShape, CopyItemRefusedRecord, CopyItemRefusedLabels, CopyItemVerified, CopyItemVerifyMismatch, CopyItemCounters };
extern std::array<std::atomic<std::uint64_t>, CopyItemCounters> copyItemCounts;
void NoteCopyItem(CopyItemCounter counter);

}

#endif
