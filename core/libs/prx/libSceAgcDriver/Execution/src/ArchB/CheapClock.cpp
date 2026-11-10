#include "prx/libSceAgcDriver/Execution/include/ArchB/CheapClock.hpp"
#include <cstdio>

#if defined(_WIN32) && defined(__GNUC__)
#include <cpuid.h>
#endif

namespace AgcDriver::ArchB::CheapClock {

static const Calibration& LoadCalibration() noexcept {
    static const Calibration calibration = [] {
        Calibration result;
#ifdef _WIN32
        LARGE_INTEGER qpcFrequency;
        QueryPerformanceFrequency(&qpcFrequency);
        result.frequency = static_cast<std::uint64_t>(qpcFrequency.QuadPart);
        unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
#if defined(__GNUC__)
        const bool invariant = __get_cpuid_max(0x80000000u, nullptr) >= 0x80000007u && __get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx) != 0 && (edx & (1u << 8u)) != 0;
#else
        int registers[4]{};
        __cpuid(registers, static_cast<int>(0x80000000u));
        if (static_cast<unsigned>(registers[0]) >= 0x80000007u) __cpuid(registers, static_cast<int>(0x80000007u));
        const bool invariant = (static_cast<unsigned>(registers[3]) & (1u << 8u)) != 0;
#endif
        if (invariant) {
            LARGE_INTEGER qpcBegin;
            QueryPerformanceCounter(&qpcBegin);
            const auto tscBegin = __rdtsc();
            Sleep(20);
            LARGE_INTEGER qpcEnd;
            QueryPerformanceCounter(&qpcEnd);
            const auto tscEnd = __rdtsc();
            const auto qpcElapsed = static_cast<std::uint64_t>(qpcEnd.QuadPart - qpcBegin.QuadPart);
            if (qpcElapsed != 0 && tscEnd > tscBegin) {
                result.frequency = static_cast<std::uint64_t>((static_cast<unsigned __int128>(tscEnd - tscBegin) * static_cast<std::uint64_t>(qpcFrequency.QuadPart)) / qpcElapsed);
                result.ticks = tscEnd;
                result.nanos = static_cast<std::uint64_t>((static_cast<unsigned __int128>(static_cast<std::uint64_t>(qpcEnd.QuadPart)) * 1000000000u) / static_cast<std::uint64_t>(qpcFrequency.QuadPart));
                result.rdtsc = true;
            }
        }
        if (!result.rdtsc) {
            LARGE_INTEGER qpc;
            QueryPerformanceCounter(&qpc);
            result.ticks = static_cast<std::uint64_t>(qpc.QuadPart);
            result.nanos = static_cast<std::uint64_t>((static_cast<unsigned __int128>(result.ticks) * 1000000000u) / result.frequency);
        }
        result.scale = static_cast<std::uint64_t>((static_cast<unsigned __int128>(1000000000u) << 32u) / result.frequency);
#else
        result.frequency = 1000000000u;
        result.scale = std::uint64_t{1} << 32u;
#endif
        std::fprintf(stderr, "[archb] cheap clock rdtsc=%u frequency=%llu\n", result.rdtsc ? 1u : 0u, static_cast<unsigned long long>(result.frequency));
        return result;
    }();
    return calibration;
}

}

extern "C" const AgcDriver::ArchB::CheapClock::Calibration* AgcDriverCheapClockCalibration_nid_no_patch() noexcept {
    return &AgcDriver::ArchB::CheapClock::LoadCalibration();
}
