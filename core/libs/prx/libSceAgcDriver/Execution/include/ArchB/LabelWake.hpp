#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_LABELWAKE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_LABELWAKE_HPP

// Arch C labels, C_LABEL_WAKE (runtime, default off, _VERIFY=N): a WAIT_REG_MEM past its spin sleeps in 200 us timer
// steps (PollSleep) and re-polls; the label census measured ~240 us from a label's record to its waiter's return on
// every waiter. With the switch, a sleeping waiter also waits on an auto-reset event of its queue's slot, and the
// thread that records a label over the awaited dword sets it (WriteLabelOnGpu, reasons 0 and 5: the label is in the
// table). The wake only ends a sleep early: the waiter re-polls exactly as after a timer step, so what satisfies a wait
// is unchanged. A missed or dropped signal falls back to the timer. _VERIFY=N: one signal in N is dropped.

#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace AgcDriver::ArchB::LabelWake {

// One per queue worker: q0 at 0, compute queue q at q - 0x20 + 1.
inline constexpr std::size_t SlotCount = 16;

struct Slot {
    std::atomic<std::uint64_t> address{0};
    std::atomic<std::uint32_t> bytes{0};
    std::atomic<void*> event{nullptr};
};

inline std::array<Slot, SlotCount>& Slots() {
    static std::array<Slot, SlotCount> slots;
    return slots;
}

inline std::atomic<std::uint64_t>& Signals() {
    static std::atomic<std::uint64_t> signals{0};
    return signals;
}

// Signals sent so far, read through the driver module (the inline counter above is per module; tests link their own).
std::uint64_t SignalCount();

inline std::atomic<std::uint64_t>& Sleeping() {
    static std::atomic<std::uint64_t> sleeping{0};
    return sleeping;
}

inline bool On() {
    return Switches::Enabled(Feature::CLabelWake);
}

inline std::size_t SlotOf(std::uint32_t queue) {
    const std::size_t index = queue == 0 ? 0 : queue >= 0x20 ? queue - 0x20 + 1 : SlotCount;
    return index < SlotCount ? index : SlotCount;
}

// The slot's auto-reset event, made on first use (null off Windows or on failure).
inline void* EventOf(std::size_t index) {
#ifdef _WIN32
    auto& slot = Slots()[index];
    if (void* made = slot.event.load(std::memory_order_acquire); made != nullptr) return made;
    void* event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) return nullptr;
    void* expected = nullptr;
    if (!slot.event.compare_exchange_strong(expected, event, std::memory_order_acq_rel)) {
        CloseHandle(event);
        return expected;
    }
    return event;
#else
    static_cast<void>(index);
    return nullptr;
#endif
}

// The waiter publishes the dword it sleeps on (before it sleeps) and clears it after.
inline void Arm(std::size_t index, std::uint64_t address, std::uint32_t bytes) {
    if (index >= SlotCount) return;
    auto& slot = Slots()[index];
    slot.bytes.store(bytes, std::memory_order_relaxed);
    slot.address.store(address, std::memory_order_seq_cst);
    Sleeping().fetch_add(1, std::memory_order_seq_cst);
}

inline void Disarm(std::size_t index) {
    if (index >= SlotCount) return;
    Slots()[index].address.store(0, std::memory_order_release);
    Sleeping().fetch_sub(1, std::memory_order_acq_rel);
}

// After a label over [address, address + bytes) entered the table: wake every sleeping waiter on an overlapping dword.
inline void Recorded(std::uint64_t address, std::size_t bytes) {
    if (Sleeping().load(std::memory_order_seq_cst) == 0 || !On()) return;
#ifdef _WIN32
    static const std::uint64_t verifyPeriod = [] {
        const char* value = std::getenv("APS5_ARCHB_C_LABEL_WAKE_VERIFY");
        return value == nullptr ? std::uint64_t{0} : static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10));
    }();
    static std::atomic<std::uint64_t> wakes{0};
    const auto end = address + bytes;
    for (std::size_t index = 0; index < SlotCount; ++index) {
        auto& slot = Slots()[index];
        const auto awaited = slot.address.load(std::memory_order_acquire);
        if (awaited == 0 || awaited >= end || address >= awaited + slot.bytes.load(std::memory_order_relaxed)) continue;
        void* event = slot.event.load(std::memory_order_acquire);
        if (event == nullptr) continue;
        if (verifyPeriod != 0 && (wakes.fetch_add(1, std::memory_order_relaxed) + 1) % verifyPeriod == 0) {
            Recorder::Count("c_label_wake_dropped");
            continue;
        }
        SetEvent(event);
        Signals().fetch_add(1, std::memory_order_relaxed);
        Recorder::Count("c_label_wake_signals");
    }
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
#endif
}

}

#endif
