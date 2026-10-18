#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace AgcDriver::Graphics {

struct Context;
struct PreparedHeapRefresh;

bool DispatchLockDietEnabled();
std::shared_ptr<PreparedHeapRefresh> CaptureDispatchHeapRefresh(const Context& context, std::function<bool(std::uint64_t, std::size_t)> pending);
void StageDispatchHeapRefresh(PreparedHeapRefresh& prepared);

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
};

DispatchHeapRefreshStats DispatchHeapRefreshCounters();

}
