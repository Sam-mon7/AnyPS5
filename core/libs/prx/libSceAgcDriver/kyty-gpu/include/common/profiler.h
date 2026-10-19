#ifndef ANYPS5_KYTY_GPU_COMMON_PROFILER_H
#define ANYPS5_KYTY_GPU_COMMON_PROFILER_H

#include "common/common.h"
#include <cstdint>

namespace profiler::colors {
inline constexpr std::uint32_t RedA100 = 0xff8a80;
inline constexpr std::uint32_t Blue300 = 0x64b5f6;
inline constexpr std::uint32_t CyanA700 = 0x00b8d4;
inline constexpr std::uint32_t Green100 = 0xc8e6c9;
inline constexpr std::uint32_t Green200 = 0xa5d6a7;
inline constexpr std::uint32_t Green300 = 0x81c784;
inline constexpr std::uint32_t Green400 = 0x66bb6a;
inline constexpr std::uint32_t Amber300 = 0xffd54f;
inline constexpr std::uint32_t DeepOrangeA200 = 0xff6e40;
}

namespace Profiler {
inline void SetThreadName(const char*) {}
inline void Initialize() {}
inline void Shutdown() {}
struct Lifecycle {
    static constexpr const char* name = "Profiler";
    static constexpr auto initialize = Profiler::Initialize;
    static constexpr auto shutdown = Profiler::Shutdown;
    static constexpr auto emergency_shutdown = Profiler::Shutdown;
};
}

#define KYTY_PROFILER_BLOCK(name, ...)
#define KYTY_PROFILER_FUNCTION(...)
#define KYTY_PROFILER_THREAD(name)
#define ZoneScoped
#define ZoneScopedN(name)
#define ZoneScopedC(color)
#define ZoneScopedNC(name, color)
#define FrameMark
#define FrameMarkNamed(name)
#define TracyPlot(name, value)
#define TracyMessage(text, size)
#define TracyMessageL(text)

#endif
