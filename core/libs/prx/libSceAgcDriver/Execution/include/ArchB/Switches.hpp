#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_SWITCHES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_SWITCHES_HPP

#include <cstdint>
#include <span>
#include <string>

namespace AgcDriver::ArchB {

enum class Feature : std::uint8_t {
    GateRun,
    GateNoop,
    Recorder,
#ifdef APS5_ARCHB_SWITCHES_TEST
    TestDependent,
    TestRuntimeDependent,
#endif
    UbTrustProofs,
    UbNoLockInvalidation,
    UbPointerFreeDrawKeys,
    UbSkipCpuIndirect,
    UbSkipDispatchProofWalk,
    FixTsharpFeedbackMask,
    FixPipelineStateReuse,
    FixVqMemo,
    FixImageTableSnapshotReuse,
    FixImageTableBitScanKeys,
    FixBufferBaseRefresh,
    FixAllocChurn,
    FixComputeSyncUnlocked,
    FixWriteTrackingBookkeeping,
    GenTable,
    GenShadow,
    GenLockfree,
    GenDrawValidation,
    PipeQ0Committer,
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
    static std::uint64_t Sequence();
    static std::int64_t GateDelayMicros();
#ifdef APS5_ARCHB_SWITCHES_TEST
    static std::uint64_t ControlPolls();
#endif
    static std::string Describe();
    static std::span<const SwitchInfo> Table();
    static void CountUpperBoundHit(Feature feature);
};

// FIX_ALLOC_CHURN is sampled once per draw or dispatch into the calling thread (AllocChurnScope);
// the allocation sites below it read AllocChurnActive(), so a runtime toggle changes their path only
// between draws. It is off on a thread outside any scope. Every site keeps its results identical
// either way (only where scratch memory comes from differs), so nested or mixed scopes are safe.
bool AllocChurnActive() noexcept;
class AllocChurnScope {
public:
    explicit AllocChurnScope(bool active) noexcept;
    ~AllocChurnScope();
    AllocChurnScope(const AllocChurnScope&) = delete;
    AllocChurnScope& operator=(const AllocChurnScope&) = delete;

private:
    bool previous;
};

}

extern "C" std::int64_t AgcDriverGateDelayMicros_nid_no_patch();

#endif
