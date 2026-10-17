#include "SceTypes.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <cstring>

extern "C" {
int APS5_VABI sceKernelCreateEventFlag(KernelEventFlag*, const char*, uint32_t, uint64_t, const void*);
int APS5_VABI sceKernelDeleteEventFlag(KernelEventFlag);
int APS5_VABI sceKernelSetEventFlag(KernelEventFlag, uint64_t);
int APS5_VABI sceKernelClearEventFlag(KernelEventFlag, uint64_t);
int APS5_VABI sceKernelCancelEventFlag(KernelEventFlag, uint64_t, int*);
int APS5_VABI sceKernelPollEventFlag(KernelEventFlag, uint64_t, uint32_t, uint64_t*);
int APS5_VABI sceKernelWaitEventFlag(KernelEventFlag, uint64_t, uint32_t, uint64_t*, KernelUseconds*);
}

static void Require(bool value) { if (!value) std::abort(); }

[[gnu::noinline]] static uint64_t WaitAtSelectedSite(KernelEventFlag flag) {
    uint64_t pattern = 0;
    Require(sceKernelWaitEventFlag(flag, 1, 0x11, &pattern, nullptr) == 0);
    return pattern;
}

static void AwaitWaiter(KernelEventFlag flag) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sceKernelPollEventFlag(flag, 1, 1, nullptr) != static_cast<int>(0x80020001u)) {
        Require(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int main(int argc, char** argv) {
    KernelEventFlag flag = nullptr, other = nullptr;
    Require(sceKernelCreateEventFlag(&flag, "trace-target", 0x10, 0, nullptr) == 0);
    Require(sceKernelCreateEventFlag(&other, "trace-other", 0x10, 0, nullptr) == 0);
    char address[32] = {};
    if (argc > 1 && std::strcmp(argv[1], "trace") == 0) std::snprintf(address, sizeof(address), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(flag)));
#ifdef _WIN32
    Require(_putenv_s("APS5_TRACE_EVENT_FLAG", address) == 0);
#else
    Require(setenv("APS5_TRACE_EVENT_FLAG", address, 1) == 0);
#endif
    Require(sceKernelSetEventFlag(other, 1) == 0);
    std::thread otherWaiter([&] { Require(WaitAtSelectedSite(other) == 1); });
    otherWaiter.join();
    Require(sceKernelSetEventFlag(flag, 3) == 0);
    uint64_t pattern = 0;
    Require(WaitAtSelectedSite(flag) == 3);
    Require(sceKernelSetEventFlag(other, 1) == 0);
    Require(sceKernelPollEventFlag(flag, 1, 1, &pattern) == static_cast<int>(0x80020010u) && pattern == 0);
    Require(sceKernelSetEventFlag(flag, 7) == 0);
    Require(sceKernelClearEventFlag(flag, ~uint64_t{2}) == 0);
    Require(sceKernelPollEventFlag(flag, 1, 0x21, &pattern) == 0 && pattern == 5);
    KernelUseconds timeout = 0;
    Require(sceKernelWaitEventFlag(flag, 1, 1, &pattern, &timeout) == static_cast<int>(0x8002003Cu) && pattern == 4);
    Require(sceKernelClearEventFlag(flag, 0) == 0);
    int result = -1;
    std::thread waiter([&] {
        KernelUseconds limit = 2000000;
        result = sceKernelWaitEventFlag(flag, 1, 0x11, &pattern, &limit);
    });
    AwaitWaiter(flag);
    Require(sceKernelSetEventFlag(flag, 1) == 0);
    waiter.join();
    Require(result == 0 && pattern == 1);
    std::thread cancelled([&] {
        KernelUseconds limit = 2000000;
        result = sceKernelWaitEventFlag(flag, 1, 1, &pattern, &limit);
    });
    AwaitWaiter(flag);
    int count = 0;
    Require(sceKernelCancelEventFlag(flag, 4, &count) == 0 && count == 1);
    cancelled.join();
    Require(result == static_cast<int>(0x80020055u) && pattern == 4);
    std::thread deleted([&] {
        KernelUseconds limit = 2000000;
        result = sceKernelWaitEventFlag(flag, 1, 1, &pattern, &limit);
    });
    AwaitWaiter(flag);
    Require(sceKernelDeleteEventFlag(flag) == 0);
    deleted.join();
    Require(result == static_cast<int>(0x8002000Du));
    Require(sceKernelDeleteEventFlag(other) == 0);
}
