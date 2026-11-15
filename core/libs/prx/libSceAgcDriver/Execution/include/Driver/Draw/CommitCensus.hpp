#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_COMMITCENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_COMMITCENSUS_HPP

#include <chrono>
#include <cstdint>

namespace AgcDriver::DriverDetail {

class CommitCensus {
public:
    enum class Kind : std::uint8_t { Replayed, Legacy, Recipe, Label, Dispatch, Copy, Fill, Other, Count };
    explicit CommitCensus(Kind kind);
    ~CommitCensus();
    CommitCensus(const CommitCensus&) = delete;
    CommitCensus& operator=(const CommitCensus&) = delete;
    void Finish(bool failed = false);
    static void SetKind(Kind kind);
    static bool LabelActive();
private:
    Kind kind;
    bool enabled = false, previousMutexCensus = false;
    int exceptions = 0;
    CommitCensus* previous = nullptr;
    std::chrono::steady_clock::time_point start{};
    std::uint64_t mutexNs = 0;
    double gpuWaitMs = 0;
};

}

#endif
