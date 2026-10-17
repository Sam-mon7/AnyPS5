#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_MEMORYLOCKPROBE_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_MEMORYLOCKPROBE_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string_view>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <pthread.h>
#endif

namespace MemoryLockProbe {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<const char*>::is_always_lock_free);

inline bool Enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("APS5_COMPLETION_PROBE");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return enabled;
}

inline std::uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline std::uint64_t Thread() {
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__linux__)
    return static_cast<std::uint64_t>(syscall(SYS_gettid));
#else
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
#endif
}

enum class MapStep { None, Lock, Reset, Setup, Split, MapView, QueryCommit, Protect, Bookkeeping };
inline constexpr std::array<const char*, 9> MapStepNames{"none", "lock", "reset", "setup", "split", "mapview", "query_commit", "protect", "bookkeeping"};

struct MapTimes {
    std::uint64_t thread = 0, bytes = 0, pages = 0, sinceNs = 0, endNs = 0, cpuNs = 0;
    bool cpuValid = false, failed = false;
    std::array<std::uint64_t, 8> steps{};
};

struct MapDetails {
    bool stable = false;
    std::uint64_t calls = 0, cpuFailures = 0, stepSinceNs = 0, completedPages = 0;
    MapStep step = MapStep::None;
    MapTimes total, last, slowest;
};

class MapProbe {
    struct AtomicTimes {
        std::atomic<std::uint64_t> thread{0}, bytes{0}, pages{0}, sinceNs{0}, endNs{0}, cpuNs{0};
        std::atomic<bool> cpuValid{false}, failed{false};
        std::array<std::atomic<std::uint64_t>, 8> steps{};

        MapTimes Read() const {
            MapTimes result{thread.load(), bytes.load(), pages.load(), sinceNs.load(), endNs.load(), cpuNs.load(), cpuValid.load(), failed.load()};
            for (std::size_t i = 0; i < steps.size(); ++i) result.steps[i] = steps[i].load();
            return result;
        }
        void Store(const MapTimes& value) {
            thread.store(value.thread); bytes.store(value.bytes); pages.store(value.pages);
            sinceNs.store(value.sinceNs); endNs.store(value.endNs); cpuNs.store(value.cpuNs);
            cpuValid.store(value.cpuValid); failed.store(value.failed);
            for (std::size_t i = 0; i < steps.size(); ++i) steps[i].store(value.steps[i]);
        }
    };

    static bool cpuTime(std::uint64_t& result) {
#ifdef _WIN32
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return false;
        const auto ticks = [](FILETIME time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
        result = (ticks(kernel) + ticks(user)) * 100;
        return true;
#else
        return false;
#endif
    }

public:
    struct Start {
        bool enabled = Enabled(), cpuValid = false;
        std::uint64_t wallNs = 0, cpuNs = 0;
        Start() {
            if (!enabled) return;
            wallNs = Now();
            cpuValid = cpuTime(cpuNs);
        }
    };

    class Timer {
    public:
        Timer(MapProbe& probe, const Start& start, std::uint64_t bytes) : probe(probe), start(start) {
            if (!start.enabled) return;
            times.thread = Thread();
            times.bytes = bytes;
            times.sinceNs = start.wallNs;
            currentSince = Now();
            times.steps[0] = currentSince - start.wallNs;
            current = MapStep::Reset;
            exceptions = std::uncaught_exceptions();
            publish();
        }
        ~Timer() {
            if (!start.enabled) return;
            times.endNs = Now();
            times.steps[static_cast<std::size_t>(current) - 1] += times.endNs - currentSince;
            std::uint64_t cpu = 0;
            times.cpuValid = start.cpuValid && cpuTime(cpu) && cpu >= start.cpuNs;
            if (times.cpuValid) times.cpuNs = cpu - start.cpuNs;
            times.failed = std::uncaught_exceptions() > exceptions;
            probe.version.fetch_add(1);
            probe.calls.fetch_add(1);
            if (!times.cpuValid) probe.cpuFailures.fetch_add(1);
            auto total = probe.total.Read();
            total.bytes += times.bytes;
            total.pages += times.pages;
            total.endNs += times.endNs - times.sinceNs;
            total.cpuNs += times.cpuNs;
            total.cpuValid = probe.cpuFailures.load() == 0;
            total.failed = total.failed || times.failed;
            for (std::size_t i = 0; i < times.steps.size(); ++i) total.steps[i] += times.steps[i];
            probe.total.Store(total);
            probe.last.Store(times);
            const auto slowest = probe.slowest.Read();
            if (times.endNs - times.sinceNs >= slowest.endNs - slowest.sinceNs) probe.slowest.Store(times);
            probe.step.store(MapStep::None);
            probe.stepSinceNs.store(0);
            probe.completedPages.store(0);
            probe.version.fetch_add(1);
        }
        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;

        void Step(MapStep next) {
            if (!start.enabled) return;
            const auto now = Now();
            times.steps[static_cast<std::size_t>(current) - 1] += now - currentSince;
            current = next;
            currentSince = now;
            publish();
        }
        void PageDone() { if (start.enabled) ++times.pages; }

    private:
        void publish() {
            probe.version.fetch_add(1);
            probe.step.store(current);
            probe.stepSinceNs.store(currentSince);
            probe.completedPages.store(times.pages);
            probe.version.fetch_add(1);
        }
        MapProbe& probe;
        const Start& start;
        MapTimes times;
        MapStep current = MapStep::None;
        std::uint64_t currentSince = 0;
        int exceptions = 0;
    };

    MapDetails Read() const {
        if (!Enabled()) return MapDetails{.stable = true};
        for (int attempt = 0; attempt != 3; ++attempt) {
            const auto before = version.load();
            if ((before & 1) != 0) continue;
            MapDetails result{true, calls.load(), cpuFailures.load(), stepSinceNs.load(), completedPages.load(), step.load(), total.Read(), last.Read(), slowest.Read()};
            if (version.load() == before) return result;
        }
        return {};
    }

private:
    std::atomic<std::uint64_t> version{0}, calls{0}, cpuFailures{0}, stepSinceNs{0}, completedPages{0};
    std::atomic<MapStep> step{MapStep::None};
    AtomicTimes total, last, slowest;
};

struct Snapshot {
    bool stable = false;
    std::uint64_t thread = 0;
    std::uint64_t sinceNs = 0;
    std::uint64_t operationSinceNs = 0;
    const char* operation = "none";
    std::uint64_t bytes = 0;
    std::uint64_t maps = 0;
    std::uint64_t failedMaps = 0;
    std::uint64_t mapBytes = 0;
    std::uint64_t mapNs = 0;
    std::uint64_t maxMapNs = 0;
    std::uint64_t lastMapThread = 0;
    const char* lastMapOperation = "none";
    std::uint64_t lastMapBytes = 0;
    std::uint64_t lastMapSinceNs = 0;
    std::uint64_t lastMapEndNs = 0;
    MapDetails mapDetails;
};

class Lock;

class Mutex {
public:
    Snapshot Read() const {
        if (!Enabled()) return Snapshot{.stable = true};
        for (int attempt = 0; attempt != 3; ++attempt) {
            const auto before = version.load();
            if ((before & 1) != 0) continue;
            Snapshot result{true, thread.load(), sinceNs.load(), operationSinceNs.load(), operation.load(), bytes.load(),
                maps.load(), failedMaps.load(), mapBytes.load(), mapNs.load(), maxMapNs.load(),
                lastMapThread.load(), lastMapOperation.load(), lastMapBytes.load(), lastMapSinceNs.load(), lastMapEndNs.load()};
            if (version.load() == before) return result;
        }
        return {};
    }

private:
    friend class Lock;
    std::mutex mutex;
    std::atomic<std::uint64_t> version{0}, thread{0}, sinceNs{0}, operationSinceNs{0}, bytes{0};
    std::atomic<const char*> operation{"none"};
    std::atomic<std::uint64_t> maps{0}, failedMaps{0}, mapBytes{0}, mapNs{0}, maxMapNs{0};
    std::atomic<std::uint64_t> lastMapThread{0}, lastMapBytes{0}, lastMapSinceNs{0}, lastMapEndNs{0};
    std::atomic<const char*> lastMapOperation{"none"};
};

class Lock {
public:
    explicit Lock(Mutex& probe, const char* operation = "mutation", std::uint64_t bytes = 0)
        : probe(probe), held(probe.mutex), operation(operation), bytes(bytes) {
        if (Enabled()) {
            started = Now();
            exceptions = std::uncaught_exceptions();
            publish();
        }
    }
    Lock(Mutex& probe, std::try_to_lock_t, const char* operation)
        : probe(probe), held(probe.mutex, std::try_to_lock), operation(operation), bytes(0) {
        if (held.owns_lock() && Enabled()) {
            started = Now();
            exceptions = std::uncaught_exceptions();
            publish();
        }
    }
    ~Lock() {
        if (!held.owns_lock() || !Enabled()) return;
        probe.version.fetch_add(1);
        if (std::string_view(operation).starts_with("map")) {
            const auto ended = Now();
            const auto elapsed = ended - started;
            probe.maps.fetch_add(1);
            probe.failedMaps.fetch_add(std::uncaught_exceptions() > exceptions);
            probe.mapBytes.fetch_add(bytes);
            probe.mapNs.fetch_add(elapsed);
            if (elapsed > probe.maxMapNs.load()) probe.maxMapNs.store(elapsed);
            probe.lastMapThread.store(Thread());
            probe.lastMapOperation.store(operation);
            probe.lastMapBytes.store(bytes);
            probe.lastMapSinceNs.store(started);
            probe.lastMapEndNs.store(ended);
        }
        probe.thread.store(0);
        probe.version.fetch_add(1);
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

    void Describe(const char* value, std::uint64_t size) {
        if (!Enabled()) return;
        operation = value;
        bytes = size;
        probe.version.fetch_add(1);
        probe.operation.store(operation);
        probe.bytes.store(bytes);
        probe.version.fetch_add(1);
    }
    bool owns_lock() const { return held.owns_lock(); }
    void unlock() {
        if (Enabled()) {
            probe.version.fetch_add(1);
            probe.thread.store(0);
            probe.version.fetch_add(1);
        }
        held.unlock();
    }
    void lock() {
        held.lock();
        if (Enabled()) publish();
    }

private:
    void publish() {
        probe.version.fetch_add(1);
        probe.thread.store(Thread());
        probe.sinceNs.store(Now());
        probe.operationSinceNs.store(started);
        probe.operation.store(operation);
        probe.bytes.store(bytes);
        probe.version.fetch_add(1);
    }
    Mutex& probe;
    std::unique_lock<std::mutex> held;
    const char* operation;
    std::uint64_t bytes;
    std::uint64_t started = 0;
    int exceptions = 0;
};

extern "C" Snapshot GuestAllocationLockSnapshot_nid_no_patch();
extern "C" Snapshot WindowsMappingsLockSnapshot_nid_no_patch();

}
#endif
