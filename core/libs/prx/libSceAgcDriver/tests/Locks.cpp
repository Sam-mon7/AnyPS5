#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/DeviceAccess.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
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
    while (uses.load(std::memory_order_relaxed) == 0) std::this_thread::yield();
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

// APS5_ARCHB_PIPE_COMMITTER_MUTEX_PRIORITY: a queue worker (tagged, nonzero queue) waits before
// its outermost lock() while the committer hook reports work, up to the window; queue 0, untagged
// threads, nested acquisitions and try_lock never wait; the switch turned off stops the waits.
std::atomic<bool> committerBusy{false};

double LockMs(std::uint32_t tag, bool nested = false) {
    double ms = -1;
    std::thread worker([&] {
        if (tag != 0xffffffffu) AgcDriver::GuestMemory::TagGpuLockThread(tag);
        auto& mutex = AgcDriver::GuestMemory::GpuMutex();
        std::unique_lock outer(mutex, std::defer_lock);
        if (nested) {
            const bool previous = committerBusy.exchange(false);
            outer.lock();
            committerBusy.store(previous);
        }
        const auto start = std::chrono::steady_clock::now();
        mutex.lock();
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        mutex.unlock();
    });
    worker.join();
    return ms;
}

void CommitterMutexPriority(const std::filesystem::path& control) {
    using namespace std::chrono_literals;
    const auto window = AgcDriver::GuestMemory::GpuLockPriorityWindow();
    Check(window == std::chrono::nanoseconds(10ms), "APS5_PIPE_MUTEX_PRIORITY_US sets the window (clamped to 10 ms)");
    Check(AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::PipeCommitterMutexPriority), "the switch is on from the environment (with PIPE_Q0_COMMITTER)");
    AgcDriver::GuestMemory::SetGpuLockPriorityHook([] { return committerBusy.load(); });
    committerBusy = false;
    Check(LockMs(0x28) < 5, "a queue worker does not wait while the committer is idle");
    committerBusy = true;
    Check(LockMs(0) < 5, "queue 0 never waits for the committer");
    Check(LockMs(0xffffffffu) < 5, "an untagged thread (committer, presenter) never waits");
    Check(LockMs(0x28, true) < 5, "a nested acquisition never waits");
    const double timedOut = LockMs(0x30);
    Check(timedOut >= 9 && timedOut < 1000, "a queue worker takes the mutex at the end of the window while the committer stays busy");
    // The committer goes idle partway: the worker takes the mutex then, not at the window's end.
    // (Busy-waits here, not sleeps: a sleep may overshoot the 10 ms window on a loaded machine.)
    std::atomic<bool> acquired{false};
    std::atomic<bool> started{false};
    std::chrono::steady_clock::time_point acquiredAt{};
    std::thread worker([&] {
        AgcDriver::GuestMemory::TagGpuLockThread(0x40);
        started = true;
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        acquiredAt = std::chrono::steady_clock::now();
        acquired = true;
    });
    while (!started.load()) std::this_thread::yield();
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - begin < 3ms) std::this_thread::yield();
    Check(!acquired.load(), "a queue worker waits while the committer is busy");
    const auto cleared = std::chrono::steady_clock::now();
    committerBusy = false;
    worker.join();
    Check(acquired.load() && acquiredAt >= cleared && acquiredAt - begin < 9ms, "the waiting worker takes the mutex once the committer is idle, before the window ends");
    {
        std::lock_guard held(AgcDriver::GuestMemory::GpuMutex());
        committerBusy = true;
        bool taken = true;
        std::chrono::steady_clock::duration tryDuration{};
        std::thread trier([&] {
            AgcDriver::GuestMemory::TagGpuLockThread(0x28);
            const auto start = std::chrono::steady_clock::now();
            taken = AgcDriver::GuestMemory::GpuMutex().try_lock();
            tryDuration = std::chrono::steady_clock::now() - start;
            if (taken) AgcDriver::GuestMemory::GpuMutex().unlock();
        });
        trier.join();
        Check(!taken && tryDuration < 5ms, "try_lock fails at once on a held mutex (no wait for the committer)");
    }
    std::ofstream(control, std::ios::binary | std::ios::trunc) << "1 PIPE_COMMITTER_MUTEX_PRIORITY=0\n";
    std::this_thread::sleep_for(60ms);
    AgcDriver::ArchB::Switches::PollControl();
    Check(!AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::PipeCommitterMutexPriority), "the switch turns off at run time");
    Check(LockMs(0x28) < 5, "with the switch off a queue worker does not wait");
    committerBusy = false;
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

}

void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

int main() {
    // Before anything reads the ArchB switches (the first GPU-mutex acquisition does).
    const auto control = std::filesystem::temp_directory_path() / ("agc-locks-control-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    std::ofstream(control, std::ios::binary | std::ios::trunc) << "";
    SetEnvironment("APS5_ARCHB_CONTROL_FILE", control.string().c_str());
    SetEnvironment("APS5_ARCHB_GATE_RUN", "1");  // the control file is read only on a gate run
    SetEnvironment("APS5_ARCHB_PIPE_Q0_COMMITTER", "1");
    SetEnvironment("APS5_ARCHB_PIPE_COMMITTER_MUTEX_PRIORITY", "1");
    SetEnvironment("APS5_PIPE_MUTEX_PRIORITY_US", "50000");
    MutexCounts(8, 100000, 0);
    MutexCounts(4, 2000, 20000);
    MutexTryLock();
    MutexWakesSleeper();
    DeviceGate();
    GpuMutexRecursion();
    ThreadSlots();
    CommitterMutexPriority(control);
    std::error_code ignored;
    std::filesystem::remove(control, ignored);
    if (failures != 0) {
        std::fprintf(stderr, "%d lock check(s) failed\n", failures);
        return 1;
    }
    std::printf("lock tests passed\n");
    return 0;
}
