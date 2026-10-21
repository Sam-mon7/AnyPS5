#pragma once

#ifdef _WIN32
#include "WindowsThreadLocal.hpp"
#include "SceTypes.hpp"
#include <array>
#include <cstdint>
#include <cstring>

namespace DemonsSoulsIdle {

inline constexpr std::uintptr_t CallRva = 0x84b59b;
inline constexpr std::uintptr_t PollRva = 0x84b880;
inline constexpr std::array<std::uint8_t, 24> CallBytes{0xe8, 0xe0, 0x02, 0x00, 0x00, 0x84, 0xc0, 0x74, 0xd7, 0x4c, 0x8b, 0x7d, 0xc8, 0x49, 0x8b, 0xb7, 0x90, 0x00, 0x00, 0x00, 0x48, 0x85, 0xf6, 0x74};
inline constexpr std::array<std::uint8_t, 24> PollBytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x35, 0x25};

struct Timer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ~Timer() { if (handle) CloseHandle(handle); }
};

static inline void APS5_VABI Wait() {
    const HANDLE timer = WindowsThreadLocal<Timer>::Get().handle;
    LARGE_INTEGER due{};
    due.QuadPart = -500;
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) && WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0) return;
    Sleep(1);
}

static inline const char* Install(std::uint8_t* image, std::uintptr_t imageSize) {
    if (PollRva + PollBytes.size() > imageSize) return "code signature differs";
    for (const auto rva : {CallRva, PollRva}) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(image + rva, &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0 || (memory.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0 || reinterpret_cast<std::uint8_t*>(memory.BaseAddress) + memory.RegionSize < image + rva + 24)
            return "code signature differs";
    }
    auto* call = image + CallRva;
    auto* poll = image + PollRva;
    if (std::memcmp(call, CallBytes.data(), CallBytes.size()) != 0 || std::memcmp(poll, PollBytes.data(), PollBytes.size()) != 0) return "code signature differs";
    const auto word = reinterpret_cast<std::uintptr_t>(call) & ~std::uintptr_t{7};
    if (reinterpret_cast<std::uintptr_t>(call) + 5 > word + 8) return "call site straddles a quadword";
    LONG64 expected = 0;
    std::memcpy(&expected, reinterpret_cast<const void*>(word), sizeof(expected));
    std::uint8_t* cave = nullptr;
    const auto first = (reinterpret_cast<std::uintptr_t>(image) + imageSize + 0xffff) & ~std::uintptr_t{0xffff};
    for (auto at = first; cave == nullptr && at < reinterpret_cast<std::uintptr_t>(call) + 0x70000000; at += 0x10000)
        cave = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (cave == nullptr) return "no code cave within a call's reach";
    std::uint8_t code[] = {
        0x48, 0x83, 0xec, 0x08,
        0xe8, 0, 0, 0, 0,
        0x48, 0x83, 0xc4, 0x08,
        0x84, 0xc0,
        0x0f, 0x85, 0x40, 0x00, 0x00, 0x00,
        0x9c, 0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
        0x48, 0x81, 0xec, 0x08, 0x02, 0x00, 0x00,
        0x48, 0x0f, 0xae, 0x04, 0x24,
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xff, 0xd0,
        0x48, 0x0f, 0xae, 0x0c, 0x24,
        0x48, 0x81, 0xc4, 0x08, 0x02, 0x00, 0x00,
        0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58, 0x9d,
        0xc3,
    };
    static_assert(sizeof(code) == 86);
    const auto pollDisplacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(poll) - reinterpret_cast<std::intptr_t>(cave + 9));
    std::memcpy(code + 5, &pollDisplacement, sizeof(pollDisplacement));
    const auto wait = reinterpret_cast<std::uint64_t>(&Wait);
    std::memcpy(code + 49, &wait, sizeof(wait));
    std::memcpy(cave, code, sizeof(code));
    DWORD previous = 0;
    if (!VirtualProtect(cave, 0x1000, PAGE_EXECUTE_READ, &previous) || !FlushInstructionCache(GetCurrentProcess(), cave, sizeof(code))) {
        VirtualFree(cave, 0, MEM_RELEASE);
        return "cannot make the code cave executable";
    }
    std::uint8_t patch[5] = {0xe8};
    const auto caveDisplacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(cave) - reinterpret_cast<std::intptr_t>(call + 5));
    std::memcpy(patch + 1, &caveDisplacement, sizeof(caveDisplacement));
    if (!VirtualProtect(reinterpret_cast<void*>(word), 8, PAGE_EXECUTE_READWRITE, &previous)) {
        VirtualFree(cave, 0, MEM_RELEASE);
        return "cannot unprotect the call site";
    }
    LONG64 desired = expected;
    std::memcpy(reinterpret_cast<std::uint8_t*>(&desired) + (reinterpret_cast<std::uintptr_t>(call) - word), patch, sizeof(patch));
    const bool swapped = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(word), desired, expected) == expected;
    VirtualProtect(reinterpret_cast<void*>(word), 8, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(word), 8);
    if (!swapped) VirtualFree(cave, 0, MEM_RELEASE);
    return swapped ? nullptr : "the call site changed while it was patched";
}

} // namespace DemonsSoulsIdle
#endif
