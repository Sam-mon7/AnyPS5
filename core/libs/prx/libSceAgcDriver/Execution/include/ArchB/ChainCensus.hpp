#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_CHAINCENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_CHAINCENSUS_HPP

// Arch C label chain 2 census (APS5_ARCHB_C_CHAIN_CENSUS=<directory>, env only, diagnostic): a compact event log of
// the label round trips between queue 0, the committer and the compute queues, and of queue 0's drains and inline
// dispatches. Every event is 24 bytes {steady ns, address, kind, thread, x, y}; each thread pushes into its own
// single-producer ring and a writer thread appends them to <directory>/chain-<pid>.bin every 20 ms (the game is
// killed at the end of a record, so nothing waits for exit). `s3/labels2/chain.py` rebuilds the round trips.
// APS5_ARCHB_C_CHAIN_CENSUS_MB caps the file (default 600 MB; later events are counted as lost).

#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace AgcDriver::ArchB::ChainCensus {

enum class Kind : std::uint8_t {
    Frame = 1,          // queue 0 reached a flip packet
    LabelQueued,        // queue 0 enqueued a label item for the committer (a = address)
    LabelDecoded,       // a queue deferred a decoded label (a = address)
    LabelRecorded,      // WriteLabelOnGpu made the label visible (a = address, x = reason)
    WaitBlock,          // a WAIT_REG_MEM was not satisfied at entry (a = awaited, x = landing)
    WaitEnd,            // that wait returned (a = awaited)
    DrainBegin,         // queue 0 drains the pipeline (x = cause, y = opcode, a = items outstanding)
    DrainEnd,
    PacketBegin,        // a dispatch packet (x = opcode)
    PacketEnd,
    Q0Dispatch,         // queue 0's pipeline decision for a dispatch (x = outcome)
    IdleBegin,          // the committer found the pipeline empty
    IdleEnd,            // the committer took its next item (x = item kind)
    LockBegin,          // a queue worker blocks on the GPU mutex (outermost)
    LockAcquired,       // a queue worker holds the GPU mutex (outermost)
    LockRelease,        // a queue worker gave it up
    WaitWake,           // C_LABEL_WAKE signalled a sleeping waiter's slot (a = label address)
    Count
};

struct Event {
    std::uint64_t time;
    std::uint64_t address;
    std::uint8_t kind;
    std::uint8_t thread;
    std::uint16_t x;
    std::uint32_t y;
};
static_assert(sizeof(Event) == 24);

inline const char* Directory() {
    static const char* directory = [] {
        const char* value = std::getenv("APS5_ARCHB_C_CHAIN_CENSUS");
        return value != nullptr && value[0] != '\0' ? value : nullptr;
    }();
    return directory;
}

inline bool On() {
    static const bool on = Directory() != nullptr;
    return on;
}

inline std::uint64_t Now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// 0 queue 0, 1 committer, 2-8 compute 0x20-0x50, 9 other.
inline std::uint8_t ThreadCode() {
    if (GuestMemory::IsCommitterThread()) return 1;
    const auto tag = GuestMemory::GpuLockThreadTag();
    if (tag == 0) return 0;
    if (tag >= 0x20 && tag <= 0x50 && (tag & 7u) == 0) return static_cast<std::uint8_t>(2 + (tag - 0x20) / 8);
    return 9;
}

class Ring {
public:
    static constexpr std::size_t Capacity = 1u << 16;
    Ring() : events(std::make_unique<Event[]>(Capacity)) {}
    void Push(const Event& event) {
        const auto next = written.load(std::memory_order_relaxed);
        if (next - consumed.load(std::memory_order_acquire) == Capacity) {
            lost.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        events[next % Capacity] = event;
        written.store(next + 1, std::memory_order_release);
    }
    // Writer thread only.
    std::size_t Drain(std::FILE* file) {
        auto first = consumed.load(std::memory_order_relaxed);
        const auto last = written.load(std::memory_order_acquire);
        std::size_t bytes = 0;
        while (first != last) {
            const auto count = std::min(last - first, Capacity - first % Capacity);
            if (file != nullptr) std::fwrite(events.get() + first % Capacity, sizeof(Event), count, file);
            bytes += count * sizeof(Event);
            first += count;
            consumed.store(first, std::memory_order_release);
        }
        return bytes;
    }
    std::atomic<std::uint64_t> lost{0};

private:
    std::unique_ptr<Event[]> events;
    alignas(64) std::atomic<std::size_t> written{0};
    alignas(64) std::atomic<std::size_t> consumed{0};
};

class Writer {
public:
    Writer() {
        const char* cap = std::getenv("APS5_ARCHB_C_CHAIN_CENSUS_MB");
        limit = static_cast<std::uint64_t>(cap != nullptr ? std::atoll(cap) : 600) << 20u;
#ifdef _WIN32
        const auto pid = static_cast<unsigned long>(_getpid());
#else
        const auto pid = static_cast<unsigned long>(getpid());
#endif
        const std::string path = std::string(Directory()) + "/chain-" + std::to_string(pid) + ".bin";
        file = std::fopen(path.c_str(), "wb");
        if (file == nullptr) std::fprintf(stderr, "[chain-census] cannot open %s\n", path.c_str());
        else std::fprintf(stderr, "[chain-census] writing %s\n", path.c_str());
        worker = std::thread([this] { run(); });
        worker.detach();
    }
    Ring* Register() {
        std::lock_guard lock(mutex);
        rings.push_back(std::make_unique<Ring>());
        return rings.back().get();
    }

private:
    void run() {
        auto lastFlush = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::lock_guard lock(mutex);
            for (auto& ring : rings) {
                const bool full = written >= limit;
                written += ring->Drain(full ? nullptr : file);
            }
            const auto now = std::chrono::steady_clock::now();
            if (file != nullptr && now - lastFlush >= std::chrono::seconds(1)) {
                std::fflush(file);
                lastFlush = now;
            }
        }
    }
    std::mutex mutex;
    std::vector<std::unique_ptr<Ring>> rings;
    std::FILE* file = nullptr;
    std::uint64_t written = 0;
    std::uint64_t limit = 0;
    std::thread worker;
};

inline Writer& Output() {
    static auto* writer = new Writer;
    return *writer;
}

// The calling thread's ring (owned by the writer, never freed: the writer may still drain it after the thread ends).
inline Ring& Local() {
    struct Slot {};
    auto* ring = HostThreadSlot<Ring*, Slot>::Get();
    if (ring == nullptr) {
        ring = Output().Register();
        HostThreadSlot<Ring*, Slot>::Set(ring);
    }
    return *ring;
}

inline void Emit(Kind kind, std::uint64_t address = 0, std::uint32_t x = 0, std::uint32_t y = 0) {
    if (!On()) return;
    Local().Push(Event{Now(), address, static_cast<std::uint8_t>(kind), ThreadCode(), static_cast<std::uint16_t>(x), y});
}

}

#endif
