#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

struct Context;
struct PreparedHeapRefresh;

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
};

DispatchHeapRefreshStats DispatchHeapRefreshCounters();

}
