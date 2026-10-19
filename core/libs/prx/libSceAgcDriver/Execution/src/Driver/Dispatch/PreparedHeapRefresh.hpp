#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

struct Context;
struct PreparedHeapRefresh;
class Recorder;

bool DispatchLockDietEnabled();
using DispatchHeapRanges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
std::shared_ptr<PreparedHeapRefresh> CaptureDispatchHeapRefresh(const Context& context, std::function<bool(std::uint64_t, std::size_t)> pending, DispatchHeapRanges* writers = nullptr);
void StageDispatchHeapRefresh(PreparedHeapRefresh& prepared, const std::function<void(std::uint64_t, std::size_t)>& duringCopy = {});
std::shared_ptr<PreparedHeapRefresh> PrepareDispatchHeapRefresh(const Context& context, std::function<bool(std::uint64_t, std::size_t)> pending, const std::function<bool(std::span<const std::pair<std::uint64_t, std::uint64_t>>)>& synchronize);

class DispatchHeapRefreshScope {
public:
    explicit DispatchHeapRefreshScope(PreparedHeapRefresh* prepared);
    ~DispatchHeapRefreshScope();
    DispatchHeapRefreshScope(const DispatchHeapRefreshScope&) = delete;
    DispatchHeapRefreshScope& operator=(const DispatchHeapRefreshScope&) = delete;

private:
    PreparedHeapRefresh* previous;
};

struct DispatchHeapRefreshStats {
    std::uint64_t captured;
    std::uint64_t unavailable;
    std::uint64_t stagedBytes;
    std::uint64_t stageUs;
    std::uint64_t applied;
    std::uint64_t fallback;
    std::uint64_t presyncs;
    std::uint64_t presyncFallbacks;
    std::array<std::uint64_t, 7> unavailableBy;
};

DispatchHeapRefreshStats DispatchHeapRefreshCounters();

bool HeapGpuRefreshEnabled();
bool MirrorGpuWriteBackEnabled();
void CountCompletionWait(int kind);

struct HeapGpuRefreshStats {
    std::uint64_t runs;
    std::uint64_t copies;
    std::uint64_t bytes;
    std::uint64_t notInPlace;
    std::uint64_t pendingResults;
    std::uint64_t unimported;
    std::uint64_t unmirrored;
    std::uint64_t mirrored;
};

HeapGpuRefreshStats HeapGpuRefreshCounters();
std::string HeapGpuRefreshWriters();
std::uint64_t StoreLabelInHeapMirror(const Context& context, Recorder& recorder, std::uint64_t address, std::span<const std::byte> bytes);

}
