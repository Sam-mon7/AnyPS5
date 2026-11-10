#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_CHEAPCLOCK_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_CHEAPCLOCK_HPP

#include "prx/libc/include/HostThreadSlot.hpp"
#include <chrono>
#include <cstdint>

#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#endif

namespace AgcDriver::ArchB::CheapClock {

using Clock = std::chrono::steady_clock;

struct Calibration {
    std::uint64_t ticks = 0;
    std::uint64_t nanos = 0;
    std::uint64_t frequency = 0;
    std::uint64_t scale = 0;
    bool rdtsc = false;
};

}

extern "C" const AgcDriver::ArchB::CheapClock::Calibration* AgcDriverCheapClockCalibration_nid_no_patch() noexcept;

namespace AgcDriver::ArchB::CheapClock {

inline const Calibration& GetCalibration() noexcept {
    return *AgcDriverCheapClockCalibration_nid_no_patch();
}

inline Clock::time_point Now(bool cheap) noexcept {
    if (!cheap) return Clock::now();
#ifdef _WIN32
    const auto& calibration = GetCalibration();
    std::uint64_t ticks;
    if (calibration.rdtsc) ticks = __rdtsc();
    else {
        LARGE_INTEGER value;
        QueryPerformanceCounter(&value);
        ticks = static_cast<std::uint64_t>(value.QuadPart);
    }
    const auto elapsed = ticks - calibration.ticks;
    const auto nanos = calibration.nanos + static_cast<std::uint64_t>((static_cast<unsigned __int128>(elapsed) * calibration.scale) >> 32u);
    return Clock::time_point(std::chrono::nanoseconds(nanos));
#else
    return Clock::now();
#endif
}

struct ActiveSlot {};

inline bool Active() noexcept {
    return HostThreadSlot<bool, ActiveSlot>::Get();
}

class Scope {
public:
    explicit Scope(bool active) noexcept : previous(Active()), changed(previous != active) { if (changed) HostThreadSlot<bool, ActiveSlot>::Set(active); }
    ~Scope() { if (changed) HostThreadSlot<bool, ActiveSlot>::Set(previous); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    bool previous;
    bool changed;
};

}

#endif
