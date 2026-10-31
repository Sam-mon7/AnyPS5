#include "prx/libSceAgcDriver/Execution/include/Driver/DeviceAccess.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include <string_view>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void MutexCounts(int threads, int iterations, int holdSpins) {
    AgcDriver::Mutex mutex;
    std::uint64_t counter = 0;
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < iterations; ++i) {
                std::lock_guard lock(mutex);
                const auto seen = counter;
                for (volatile int spin = 0; spin < holdSpins; spin = spin + 1) {
                }
                counter = seen + 1;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    Check(counter == static_cast<std::uint64_t>(threads) * static_cast<std::uint64_t>(iterations), "every increment under the mutex counted");
}

void MutexTryLock() {
    AgcDriver::Mutex mutex;
    Check(mutex.try_lock(), "try_lock takes a free mutex");
    bool taken = true;
    std::thread other([&] { taken = mutex.try_lock(); });
    other.join();
    Check(!taken, "try_lock fails while another thread holds the mutex");
    mutex.unlock();
    std::thread again([&] {
        taken = mutex.try_lock();
        if (taken) mutex.unlock();
    });
    again.join();
    Check(taken, "try_lock takes the mutex once released");
}

void MutexWakesSleeper() {
    AgcDriver::Mutex mutex;
    mutex.lock();
    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        std::lock_guard lock(mutex);
        acquired = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Check(!acquired, "lock() waits while the mutex is held");
    mutex.unlock();
    waiter.join();
    Check(acquired, "the waiting thread took the mutex after the release");
}

void DeviceGate() {
    AgcDriver::DriverDetail::DeviceUseGate gate;
    std::atomic<int> users{0};
    std::atomic<int> replacements{0};
    std::atomic<bool> overlap{false};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> uses{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                std::shared_lock use(gate);
                users.fetch_add(1);
                if (replacements.load() != 0) overlap = true;
                users.fetch_sub(1);
                uses.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int r = 0; r < 2; ++r) {
        threads.emplace_back([&] {
            for (int i = 0; i < 300; ++i) {
                std::unique_lock replacing(gate);
                if (replacements.fetch_add(1) != 0 || users.load() != 0) overlap = true;
                std::this_thread::yield();
                if (users.load() != 0) overlap = true;
                replacements.fetch_sub(1);
            }
        });
    }
    threads[6].join();
    threads[7].join();
    stop = true;
    for (int t = 0; t < 6; ++t) threads[static_cast<std::size_t>(t)].join();
    Check(!overlap, "no user inside the gate during a replacement, one replacement at a time");
    Check(uses.load() != 0, "users got through between replacements");
}

void GpuMutexRecursion() {
    auto& mutex = AgcDriver::GuestMemory::GpuMutex();
    Check(!mutex.HeldByThisThread(), "the GPU mutex starts free");
    mutex.lock();
    mutex.lock();
    Check(mutex.HeldByThisThread() && mutex.DepthOnThisThread() == 2, "a nested lock counts the depth");
    bool taken = true;
    std::thread other([&] {
        taken = mutex.try_lock();
        if (taken) mutex.unlock();
    });
    other.join();
    Check(!taken, "another thread cannot take the held GPU mutex");
    Check(mutex.try_lock() && mutex.DepthOnThisThread() == 3, "the holder's try_lock nests");
    mutex.unlock();
    mutex.unlock();
    other = std::thread([&] {
        taken = mutex.try_lock();
        if (taken) mutex.unlock();
    });
    other.join();
    Check(!taken, "the GPU mutex stays held until the outermost unlock");
    mutex.unlock();
    Check(!mutex.HeldByThisThread() && mutex.DepthOnThisThread() == 0, "the outermost unlock releases the GPU mutex");
    std::uint64_t counter = 0;
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < 20000; ++i) {
                std::lock_guard outer(mutex);
                std::lock_guard inner(mutex);
                ++counter;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    Check(counter == 80000, "every nested increment under the GPU mutex counted");
}

template<typename TTag>
void SlotIsolation(const char* what) {
    using Slot = HostThreadSlot<std::uint64_t, TTag>;
    bool ok = Slot::Get() == 0;
    Slot::Set(0x1234567890abcdefull);
    std::uint64_t seen = 1;
    std::thread other([&] {
        seen = Slot::Get();
        Slot::Set(99);
    });
    other.join();
    ok = ok && seen == 0 && Slot::Get() == 0x1234567890abcdefull;
    Slot::Set(0);
    Check(ok, what);
}

struct DirectSlot {};
struct ExpansionSlot {};
struct PointerSlot {};

void ThreadSlots() {
    SlotIsolation<DirectSlot>("a thread slot starts at zero on every thread and keeps its own value");
    int object = 0;
    HostThreadSlot<int*, PointerSlot>::Set(&object);
    Check(HostThreadSlot<int*, PointerSlot>::Get() == &object, "a pointer round-trips through a thread slot");
#ifdef _WIN32
    std::vector<DWORD> held;
    for (DWORD index = TlsAlloc(); index != TLS_OUT_OF_INDEXES; index = TlsAlloc()) {
        held.push_back(index);
        if (index >= 64) break;
    }
    SlotIsolation<ExpansionSlot>("an expansion thread slot starts at zero on every thread and keeps its own value");
    for (const auto index : held) TlsFree(index);
#endif
    AgcDriver::GuestMemory::PacketTag fresh{};
    AgcDriver::GuestMemory::ReadSite freshSite = AgcDriver::GuestMemory::ReadSite::Count;
    std::thread other([&] {
        fresh = AgcDriver::GuestMemory::CurrentPacket();
        freshSite = AgcDriver::GuestMemory::CurrentReadSite();
    });
    other.join();
    Check(fresh.opcode == AgcDriver::GuestMemory::NoPacket && fresh.queue == 0xffffffffu, "a thread that ran no packet reads NoPacket");
    Check(freshSite == AgcDriver::GuestMemory::ReadSite::Unknown, "a thread that named no read site reads Unknown");
    AgcDriver::GuestMemory::SetCurrentPacket(0xffffu, 0x2au);
    const auto packet = AgcDriver::GuestMemory::CurrentPacket();
    Check(packet.opcode == 0xffffu && packet.queue == 0x2au, "the packet tag reads back what was set");
    const auto previous = AgcDriver::GuestMemory::SetReadSite(AgcDriver::GuestMemory::ReadSite::Scanout);
    Check(previous == AgcDriver::GuestMemory::ReadSite::Unknown && AgcDriver::GuestMemory::CurrentReadSite() == AgcDriver::GuestMemory::ReadSite::Scanout, "SetReadSite returns the previous site and sets the new one");
    AgcDriver::GuestMemory::SetReadSite(previous);
}

void MutexCensus() {
    using R = AgcDriver::ArchB::Recorder;
    namespace G = AgcDriver::GuestMemory;
    using namespace std::chrono_literals;
    R::Boundary(true, 0, R::Clock::now());
    R::BindThread("census waiter");
    G::TagGpuLockThread(0);
    std::atomic<bool> held{false}, waiting{false};
    std::thread holder([&] {
        R::BindThread("census holder");
        G::TagGpuLockThread(0x28);
        G::TagGpuLockSite(G::GpuLockSite::Dispatch);
        std::lock_guard lock(G::GpuMutex());
        held.store(true);
        while (!waiting.load()) std::this_thread::yield();
        std::this_thread::sleep_for(25ms);
    });
    while (!held.load()) std::this_thread::yield();
    std::this_thread::sleep_for(40ms);
    G::TagGpuLockSite(G::GpuLockSite::Wait);
    Check(!G::GpuMutex().try_lock(), "census try failure still refuses a held mutex");
    waiting.store(true);
    G::TagGpuLockSite(G::GpuLockSite::Draw);
    {
        std::lock_guard outer(G::GpuMutex());
        G::TagGpuLockSite(G::GpuLockSite::Hook);
        std::lock_guard inner(G::GpuMutex());
    }
    holder.join();
    const auto result = R::Collect(R::Clock::now());
    double behind = 0, hold = 0, wait = 0, recursive = 0, failed = 0;
    for (const auto& [name, value] : result.threadCounters.at("census waiter")) {
        if (name.starts_with("M2.WaitBehind.q0x28.")) behind += value;
        if (name.ends_with(".wait_ms")) wait += value;
        if (name.ends_with(".recursive")) recursive += value;
        if (name.ends_with(".try_failed")) failed += value;
    }
    for (const auto& [name, value] : result.threadCounters.at("census holder")) {
        if (name.ends_with(".hold_ms")) hold += value;
    }
    Check(behind > 0 && behind <= wait && behind < hold, "census clips holder time to the actual wait");
    Check(recursive == 1 && failed == 1, "census separates recursive acquisitions and failed tries");
    Check(result.counters.at("M2.wait_covered_ms") == behind, "census holder attribution reconciles");
    Check(!result.counters.contains("M2.site_overflow") && !result.counters.contains("M2.wait_history_overflow"), "census did not overflow");
    R::Boundary(false, 1, R::Clock::now());
    G::TagGpuLockSite(G::GpuLockSite::Draw);
    { std::lock_guard off(G::GpuMutex()); }
    const auto off = R::Collect(R::Clock::now());
    for (const auto& [name, value] : off.counters) {
        if (name.starts_with("M2.")) Check(value == 0, "disabled recorder published mutex counters");
    }
}

void OwnershipContention() {
    using R = AgcDriver::ArchB::Recorder;
    using D2 = AgcDriver::ArchB::OwnershipCensus;
    namespace G = AgcDriver::GuestMemory;
    using namespace std::chrono_literals;
    R::Boundary(true, 0, R::Clock::now());
    R::BindThread("ownership q0");
    G::TagGpuLockThread(0);
    std::array<std::uint32_t, 2> bytes{};
    const auto batch = D2::Open(31);
    for (unsigned kind = 0; kind < 4; ++kind) {
        std::atomic<bool> held{false}, waiting{false};
        const auto address = reinterpret_cast<std::uintptr_t>(bytes.data());
        D2::Command request(0);
        request.Requested()->Add({.begin = address + (kind == 3 ? 4 : 0), .end = address + (kind == 3 ? 8 : 4), .resource = D2::Resource::Buffer,
            .flags = static_cast<std::uint8_t>(kind == 0 ? D2::Read : D2::Write)});
        std::thread holder([&] {
            const auto queue = kind % 2 == 0 ? 0x20u : 0x28u;
            R::BindThread(queue == 0x20 ? "ownership q20" : "ownership q28");
            G::TagGpuLockThread(queue);
            G::TagGpuLockSite(G::GpuLockSite::Dispatch);
            D2::Command command(queue, true);
            std::lock_guard lock(G::GpuMutex());
            D2::Observe(batch, {.begin = address, .end = address + 4, .resource = D2::Resource::Buffer, .flags = static_cast<std::uint8_t>(kind == 1 ? D2::Read : D2::Write)});
            held = true;
            while (!waiting.load()) std::this_thread::yield();
            {
                const auto until = R::Clock::now() + 3ms;
                D2::PhaseScope changing(D2::Phase::Resources);
                while (R::Clock::now() < until) {
                    changing.Mark(D2::Phase::Record);
                    changing.Mark(D2::Phase::Resources);
                }
            }
            D2::Sync sync;
            D2::PhaseScope phase(D2::Phase::Seal, true);
            for (const auto current : {D2::Phase::Seal, D2::Phase::Submit, D2::Phase::GpuWait, D2::Phase::Callbacks, D2::Phase::Publish, D2::Phase::Release}) {
                phase.Mark(current);
                std::this_thread::sleep_for(1ms);
            }
            if (kind != 1) bytes[0] = 8;
            else Check(bytes[0] == 8, "delayed reader saw preceding writer bytes");
        });
        while (!held.load()) std::this_thread::yield();
        waiting = true;
        G::TagGpuLockSite(G::GpuLockSite::Draw);
        {
            std::lock_guard lock(G::GpuMutex());
            if (kind == 0) Check(bytes[0] == 8, "RAW draw read final compute bytes");
            else bytes[kind == 3 ? 1 : 0] = 9;
        }
        holder.join();
    }
    Check(bytes[0] == 8 && bytes[1] == 9, "overlap and disjoint final bytes");
    D2::Transition(batch, D2::Life::Submitted, 4);
    D2::Transition(batch, D2::Life::Finishing);
    D2::Transition(batch, D2::Life::Published);
    D2::Retire(batch);
    const auto result = R::Collect(R::Clock::now());
    double behind = 0, exclusive = 0, phases = 0;
    for (const auto& [name, value] : result.counters) {
        if (name.starts_with("M2.WaitBehind.q0x20.") || name.starts_with("M2.WaitBehind.q0x28.")) behind += value * 1e6;
        if (name.starts_with("d2_conflict_") || name == "d2_candidate_independent_ns" || name == "d2_unknown_ns") exclusive += value;
        if (name.starts_with("d2_wait_phase_ns.")) phases += value;
    }
    Check(std::abs(behind - result.counters.at("d2_q0_wait_compute_ns")) < 1, "D2 exact clipped wait reconciles to M2");
    Check(exclusive == phases && exclusive == result.counters.at("d2_q0_wait_compute_ns"), "D2 exclusive category and phase sums reconcile");
    for (const auto* name : {"d2_conflict_raw_ns", "d2_conflict_war_ns", "d2_conflict_waw_ns", "d2_unknown_ns", "d2_sync_seal_ns", "d2_sync_submit_ns", "d2_sync_gpu_wait_ns", "d2_sync_callbacks_ns", "d2_sync_publish_ns", "d2_sync_release_ns"}) Check(result.counters.at(name) > 0, name);
    for (const auto* name : {"d2_pending_shadow_mismatches", "d2_early_retirement", "d2_overflow", "d2_m2_reconciliation_mismatches", "d2_candidate_independent_ns"}) Check(result.counters.at(name) == 0, name);
    std::printf("%s\n", result.Json().c_str());
}

}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "ownership") {
        OwnershipContention();
        return failures != 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "census") {
        MutexCensus();
        return failures != 0;
    }
    MutexCounts(8, 100000, 0);
    MutexCounts(4, 2000, 20000);
    MutexTryLock();
    MutexWakesSleeper();
    DeviceGate();
    GpuMutexRecursion();
    ThreadSlots();
    if (failures != 0) {
        std::fprintf(stderr, "%d lock check(s) failed\n", failures);
        return 1;
    }
    std::printf("lock tests passed\n");
    return 0;
}
