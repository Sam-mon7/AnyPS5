#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_MEMORYLOCKPROBE_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_MEMORYLOCKPROBE_HPP

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
