#include "prx/libSceAgcDriver/Execution/include/ArchB/ProofEvents.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <array>
#include <atomic>
#include <limits>

namespace AgcDriver::ArchB {
namespace {

std::atomic<std::uint64_t> memory{1};
std::atomic<std::uint64_t> mapping{1};
std::atomic<void (*)(const ProofEvent&) noexcept> observer{nullptr};
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(ProofSource::Count)> counters{};
constexpr std::array Names{
    "proof_cpu_explicit_break", "proof_cpu_tracker_write", "proof_cpu_tracker_collect", "proof_cpu_tracker_forget",
    "proof_cpu_tracker_unwatch", "proof_cpu_tracker_rewatch", "proof_cpu_host_write_begin", "proof_cpu_host_write_end",
    "proof_mapping_register", "proof_mapping_protection", "proof_mapping_retire", "proof_cpu_watch_register",
    "proof_cpu_watch_unregister", "proof_cpu_watch_collect"
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(Names.size() == counters.size());

void libcMutation(GuestAllocations::MemoryMutation event, std::uintptr_t address, std::size_t bytes) noexcept {
    using Event = GuestAllocations::MemoryMutation;
    switch (event) {
    case Event::Mapping: ProofEvents::Publish(ProofMutation::Mapping, ProofSource::Mapping, address, bytes); break;
    case Event::Protection: ProofEvents::Publish(ProofMutation::Protection, ProofSource::Protection, address, bytes); break;
    case Event::Retire: ProofEvents::Publish(ProofMutation::Mapping, ProofSource::MappingRetire, address, bytes); break;
    case Event::HostWriteBegin: ProofEvents::Publish(ProofMutation::CpuWrite, ProofSource::HostWriteBegin, address, bytes); break;
    case Event::HostWriteEnd: ProofEvents::Publish(ProofMutation::CpuWrite, ProofSource::HostWriteEnd, address, bytes); break;
    case Event::WatchRegister: ProofEvents::Publish(ProofMutation::CpuWrite, ProofSource::WatchRegister, address, bytes); break;
    case Event::WatchUnregister: ProofEvents::Publish(ProofMutation::CpuWrite, ProofSource::WatchUnregister, address, bytes); break;
    case Event::WatchCollect: ProofEvents::Publish(ProofMutation::CpuWrite, ProofSource::WatchCollect, address, bytes); break;
    }
}

const bool registered = [] {
    Switches::Initialize();
    GuestAllocations::GuestAllocationsSetMutationHook_nid_no_patch(&libcMutation);
    return true;
}();

}

ProofEvents::Work::Work() : Work(Switches::Enabled(Feature::ProofCpuEvents)) {}

ProofEvents::Work::Work(bool on) : previous(work), enabled(on) {
    work = this;
}

ProofEvents::Work::~Work() {
    work = previous;
}

bool ProofEvents::Enabled() {
    return work != nullptr ? work->enabled : Switches::Enabled(Feature::ProofCpuEvents);
}

void ProofEvents::Publish(ProofMutation mutation, ProofSource source, std::uint64_t address, std::uint64_t bytes) {
    if (!Enabled()) return;
    const ProofEvent event{mutation, source, address, bytes, bytes != 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address};
    if (const auto callback = observer.load(std::memory_order_acquire)) callback(event);
    if (mutation == ProofMutation::Mapping || mutation == ProofMutation::Protection) mapping.fetch_add(1, std::memory_order_release);
    else memory.fetch_add(1, std::memory_order_release);
    if (Recorder::Enabled()) counters[static_cast<std::size_t>(source)].fetch_add(1, std::memory_order_relaxed);
}

ProofObservation ProofEvents::Acquire(bool enabled) {
    if (!enabled) return {{}, static_cast<std::uint8_t>(ProofCoverage::Disabled)};
    return {{memory.load(std::memory_order_acquire), mapping.load(std::memory_order_acquire), 0},
            static_cast<std::uint8_t>(ProofCoverage::NativeWrites) | static_cast<std::uint8_t>(ProofCoverage::GpuEvents)};
}

void ProofEvents::SetObserver(void (*callback)(const ProofEvent&) noexcept) {
    observer.store(callback, std::memory_order_release);
}

void ProofEvents::FlushCounters() {
    for (std::size_t i = 0; i < counters.size(); ++i) {
        const auto count = counters[i].exchange(0, std::memory_order_relaxed);
        if (count == 0) continue;
        Recorder::Count(Names[i], static_cast<double>(count));
        const bool mapped = i >= static_cast<std::size_t>(ProofSource::Mapping) && i <= static_cast<std::size_t>(ProofSource::MappingRetire);
        Recorder::Count(mapped ? "proof_mapping_events" : "proof_cpu_events", static_cast<double>(count));
    }
}

}
