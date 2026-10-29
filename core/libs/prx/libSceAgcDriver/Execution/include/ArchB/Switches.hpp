#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_SWITCHES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_SWITCHES_HPP

#include <cstdint>
#include <span>
#include <string>

namespace AgcDriver::ArchB {

enum class Feature : std::uint8_t {
    GateRun,
    GateNoop,
#ifdef APS5_ARCHB_SWITCHES_TEST
    TestDependent,
    TestRuntimeDependent,
#endif
    UbTrustProofs,
    UbNoLockInvalidation,
    UbPointerFreeDrawKeys,
    UbSkipCpuIndirect,
    UbSkipDispatchProofWalk,
    Count
};

struct SwitchInfo {
    Feature feature;
    const char* name;
    bool defaultOn;
    bool runtime;
    std::span<const Feature> prerequisites;
};

class Switches {
public:
    static bool Enabled(Feature feature);
    static void Initialize();
    static void PollControl();
    static std::int64_t GateDelayMicros();
#ifdef APS5_ARCHB_SWITCHES_TEST
    static std::uint64_t ControlPolls();
#endif
    static std::string Describe();
    static std::span<const SwitchInfo> Table();
    static void CountUpperBoundHit(Feature feature);
};

}

extern "C" std::int64_t AgcDriverGateDelayMicros_nid_no_patch();

#endif
