#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_PROOFEVENTS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_PROOFEVENTS_HPP

#include <cstdint>

namespace AgcDriver::ArchB {

enum class ProofMutation : std::uint8_t {
    CpuWrite = 0, Mapping = 2, Protection = 3
};

enum class ProofSource : std::uint8_t {
    ExplicitBreak, TrackerWrite, TrackerCollect, TrackerForget, TrackerUnwatch, TrackerRewatch,
    HostWriteBegin, HostWriteEnd, Mapping, Protection, MappingRetire, WatchRegister, WatchUnregister, WatchCollect, Count
};

struct ProofEpoch {
    std::uint64_t memory = 0;
    std::uint64_t mapping = 0;
    std::uint64_t resources = 0;
    bool operator==(const ProofEpoch&) const = default;
};

struct ProofEvent {
    ProofMutation mutation;
    ProofSource source;
    std::uint64_t address;
    std::uint64_t bytes;
    bool rangeKnown;
};

enum class ProofCoverage : std::uint8_t { Complete = 0, NativeWrites = 1, GpuEvents = 2, Disabled = 4 };

struct ProofObservation {
    ProofEpoch epoch;
    std::uint8_t missingCoverage = static_cast<std::uint8_t>(ProofCoverage::Disabled);
    bool Conservative() const { return missingCoverage != 0; }
};

class ProofEvents {
public:
    class Work {
    public:
        Work();
        explicit Work(bool enabled);
        ~Work();
        Work(const Work&) = delete;
        Work& operator=(const Work&) = delete;
        bool Enabled() const { return enabled; }
    private:
        friend class ProofEvents;
        Work* previous;
        bool enabled;
    };
    static bool Enabled();
    static void Publish(ProofMutation mutation, ProofSource source, std::uint64_t address = 0, std::uint64_t bytes = 0);
    static ProofObservation Acquire(bool enabled);
    static void SetObserver(void (*observer)(const ProofEvent&) noexcept);
    static void FlushCounters();
private:
    inline static thread_local Work* work = nullptr;
};

}

#endif
