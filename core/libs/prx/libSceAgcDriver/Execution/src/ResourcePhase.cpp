#include "prx/libSceAgcDriver/Execution/include/ResourcePhase.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <cstdlib>
#include <cstring>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif

namespace AgcDriver {

namespace {

std::uint64_t timestamp() {
#ifdef _WIN32
    FILETIME time;
    GetSystemTimePreciseAsFileTime(&time);
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime;
#else
    return 0;
#endif
}

}

ResourcePhase::ResourcePhase(const char* name) : name(name) {
    static const bool enabled = [] {
        const auto* value = std::getenv("APS5_RESOURCE_PHASE_PROFILE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    if (enabled) start = timestamp();
}

ResourcePhase::~ResourcePhase() {
    End();
}

void ResourcePhase::Stop() {
    StopProfileOutput_nid_no_patch();
}

void ResourcePhase::End() {
    if (start == 0) return;
    const auto end = timestamp();
    const auto begin = std::exchange(start, 0);
#ifdef _WIN32
    ProfilePrint_nid_no_patch("[resource-phase] %s tid=%lu start=%llu end=%llu\n", name, GetCurrentThreadId(), static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end));
#else
    static_cast<void>(end);
    static_cast<void>(begin);
#endif
}

}
