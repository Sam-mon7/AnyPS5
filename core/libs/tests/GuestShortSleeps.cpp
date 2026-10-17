#include "SceTypes.hpp"
#include "prx/libkernel/Time/include/TimedWait.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <windows.h>

extern "C" {
int APS5_VABI sceKernelUsleep_nid_postfix(KernelUseconds microseconds);
int APS5_VABI sceKernelNanosleep(const KernelTimespec* request, KernelTimespec* remaining);
int APS5_VABI nanosleep_nid_postfix(const KernelTimespec* request, KernelTimespec* remaining);
void APS5_VABI scePthreadYield();
int APS5_VABI sched_yield_nid_postfix();
void APS5_VABI pthread_yield_nid_postfix();
}

namespace {

using Clock = std::chrono::steady_clock;

void Require(bool value, const char* message) {
    if (value) return;
    std::fflush(stdout);
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
}

std::uint64_t CpuNanos() {
    FILETIME created{}, exited{}, kernel{}, user{};
    Require(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) != 0, "cannot read thread CPU time");
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) * 100u;
}

template<class TSleep>
void Durations(const char* name, bool enabled, TSleep sleep) {
    for (const std::uint64_t micros : {1u, 30u, 300u, 2000u}) {
        std::uint64_t shortest = UINT64_MAX;
        for (unsigned iteration = 0; iteration != 16; ++iteration) {
            const auto begin = Clock::now();
            sleep(micros);
            const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
            if (enabled) Require(elapsed >= micros * 1000u, "positive sleep returned before its requested duration");
            if (elapsed < shortest) shortest = elapsed;
        }
        std::printf("%s request=%llu us shortest=%.3f us enabled=%u\n", name, static_cast<unsigned long long>(micros), shortest / 1000.0, enabled ? 1u : 0u);
    }
}

void CpuUse(bool enabled) {
    Require(sceKernelUsleep_nid_postfix(1000) == 0, "timer warmup failed");
    std::uint64_t totalCpu = 0;
    std::uint64_t totalWall = 0;
    for (unsigned trial = 0; trial != 5; ++trial) {
        const auto cpuBegin = CpuNanos();
        const auto begin = Clock::now();
        for (unsigned iteration = 0; iteration != 1000; ++iteration) Require(sceKernelUsleep_nid_postfix(30) == 0, "short sleep failed");
        const auto wall = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
        const auto cpu = CpuNanos() - cpuBegin;
        totalCpu += cpu;
        totalWall += wall;
        std::printf("1000 x 30 us trial=%u cpu=%.3f ms wall=%.3f ms cpu-share=%.3f%% enabled=%u\n", trial + 1u, cpu / 1e6, wall / 1e6, wall != 0 ? 100.0 * cpu / wall : 0.0, enabled ? 1u : 0u);
    }
    Require(totalWall != 0, "sleep CPU measurement has no wall time");
    std::printf("1000 x 30 us average cpu=%.3f ms wall=%.3f ms cpu-share=%.3f%% enabled=%u\n", totalCpu / 5e6, totalWall / 5e6, 100.0 * totalCpu / totalWall, enabled ? 1u : 0u);
    if (enabled) Require(totalCpu <= totalWall / 10u, "short sleeps consumed more than ten percent of one CPU");
}

}

int main(int argc, char** argv) {
    Require(argc == 2, "expected enabled or disabled test mode");
    const bool enabled = std::strcmp(argv[1], "on") == 0;
    Require(enabled || std::strcmp(argv[1], "off") == 0, "unknown test mode");
    Require(TimedWait::RealShortSleeps() == enabled, "short-sleep switch does not match the test mode");
    Require(sceKernelUsleep_nid_postfix(0) == 0, "zero microsecond sleep failed");
    KernelTimespec zero{};
    KernelTimespec remaining{7, 9};
    Require(sceKernelNanosleep(&zero, &remaining) == 0 && remaining.tv_sec == 7 && remaining.tv_nsec == 9, "zero kernel nanosleep changed its result");
    Require(nanosleep_nid_postfix(&zero, &remaining) == 0 && remaining.tv_sec == 7 && remaining.tv_nsec == 9, "zero POSIX nanosleep changed its result");
    Durations("usleep", enabled, [](std::uint64_t micros) {
        Require(sceKernelUsleep_nid_postfix(static_cast<KernelUseconds>(micros)) == 0, "microsecond sleep failed");
    });
    Durations("kernel nanosleep", enabled, [](std::uint64_t micros) {
        const KernelTimespec request{0, static_cast<std::int64_t>(micros * 1000u)};
        Require(sceKernelNanosleep(&request, nullptr) == 0, "kernel nanosleep failed");
    });
    Durations("POSIX nanosleep", enabled, [](std::uint64_t micros) {
        const KernelTimespec request{0, static_cast<std::int64_t>(micros * 1000u)};
        Require(nanosleep_nid_postfix(&request, nullptr) == 0, "POSIX nanosleep failed");
    });
    Durations("sleep until", enabled, [](std::uint64_t micros) {
        TimedWait::SleepUntil(TimedWait::DeadlineNanos(micros));
    });
    CpuUse(enabled);
    scePthreadYield();
    Require(sched_yield_nid_postfix() == 0, "sched_yield failed");
    pthread_yield_nid_postfix();
}
