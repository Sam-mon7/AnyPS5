#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_TIMELINE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_TIMELINE_HPP

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace AgcDriver::Timeline {

enum class Kind : std::uint32_t {
    SubmissionBegin = 1, SubmissionEnd, PacketBegin, PacketEnd, WaitBegin, WaitEnd,
    WaitSource, LabelRecord, LabelHistory, LabelDeferred, LockBegin, LockAcquired,
    LockRelease, LockFailed, BatchOpen, SubmitBegin, SubmitEnd, Complete, Retire,
    Lost, Flip, GpuTime, Clock, LabelVisible, BatchDiscard, GpuWaitBegin, GpuWaitEnd
};

struct Event {
    std::uint64_t time, id, a, b, c, d;
    std::uint32_t queue, kind, opcode, flags;
    std::uint64_t packet, batch;
};

static_assert(sizeof(Event) == 80);
static_assert(std::endian::native == std::endian::little);

inline const char* const Directory = std::getenv("APS5_TIMELINE");
inline const bool Enabled = Directory != nullptr && *Directory != '\0';

inline std::uint64_t Now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

class Buffer {
public:
    static constexpr std::size_t Capacity = 65536;

    explicit Buffer(std::uint32_t thread) : thread(thread), events(std::make_unique<Event[]>(Capacity)) {}

    void Push(Event event) {
        const auto next = written.load(std::memory_order_relaxed);
        if (next - consumed.load(std::memory_order_acquire) == Capacity) {
            lost.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        events[next % Capacity] = event;
        written.store(next + 1, std::memory_order_release);
    }

    bool Drain(std::ostream& output) {
        auto first = consumed.load(std::memory_order_relaxed);
        const auto last = written.load(std::memory_order_acquire);
        while (first != last) {
            const auto count = std::min(last - first, Capacity - first % Capacity);
            output.write(reinterpret_cast<const char*>(events.get() + first % Capacity), static_cast<std::streamsize>(count * sizeof(Event)));
            if (!output) return false;
            first += count;
            consumed.store(first, std::memory_order_release);
        }
        const auto dropped = lost.exchange(0, std::memory_order_relaxed);
        if (dropped != 0) {
            const Event event{Now(), 0, dropped, 0, 0, 0, 0xffffffffu, static_cast<std::uint32_t>(Kind::Lost), 0, 0, 0, 0};
            output.write(reinterpret_cast<const char*>(&event), sizeof(event));
        }
        return static_cast<bool>(output);
    }

    std::uint32_t Thread() const { return thread; }

private:
    const std::uint32_t thread;
    std::unique_ptr<Event[]> events;
    alignas(64) std::atomic<std::size_t> written{0};
    alignas(64) std::atomic<std::size_t> consumed{0};
    std::atomic<std::uint64_t> lost{0};
};

class Writer {
public:
    Writer() {
        const auto runId = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(Directory);
        for (std::uint64_t attempt = 0;; ++attempt) {
            directory = std::filesystem::path(Directory) / ("run-" + runId + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(directory)) break;
        }
        worker = std::thread([this] { run(); });
    }

    Buffer* Register() {
        std::lock_guard lock(mutex);
        const auto id = static_cast<std::uint32_t>(buffers.size() + 1);
        auto buffer = std::make_unique<Buffer>(id);
        auto file = std::make_unique<std::ofstream>(directory / ("thread-" + std::to_string(id) + ".aps5tl"), std::ios::binary);
        const std::array<std::uint32_t, 2> header{sizeof(Event), id};
        file->write("APS5TL1\0", 8);
        file->write(reinterpret_cast<const char*>(header.data()), sizeof(header));
        if (!*file) throw std::runtime_error("cannot open APS5_TIMELINE output");
        auto* result = buffer.get();
        buffers.push_back({std::move(buffer), std::move(file)});
        return result;
    }

    void Stop() {
        stopping.store(true, std::memory_order_release);
        changed.notify_one();
        if (worker.joinable()) worker.join();
    }

private:
    void run() {
        for (;;) {
            std::unique_lock lock(mutex);
            changed.wait_for(lock, std::chrono::seconds(1), [&] { return stopping.load(std::memory_order_acquire); });
            for (auto& [buffer, file] : buffers) {
                if (!buffer->Drain(*file)) std::fprintf(stderr, "[timeline] output failed on thread %u\n", buffer->Thread());
                file->flush();
            }
            if (stopping.load(std::memory_order_acquire)) return;
        }
    }

    std::filesystem::path directory;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<std::unique_ptr<Buffer>, std::unique_ptr<std::ofstream>>> buffers;
    std::atomic<bool> stopping{false};
    std::thread worker;
};

inline Writer& Output() {
    static auto* writer = new Writer;
    static const bool registered = [] { std::atexit([] { Output().Stop(); }); return true; }();
    static_cast<void>(registered);
    return *writer;
}

inline thread_local Buffer* Local = nullptr;
inline thread_local std::uint32_t Queue = 0xffffffffu;
inline thread_local std::uint32_t Opcode = 0;
inline thread_local std::uint64_t Packet = 0;
inline thread_local std::uint64_t Serial = 0;
inline thread_local std::array<std::uint64_t, 2> LabelStamps{};
inline thread_local std::array<std::uint32_t, 2> LabelQueues{};
struct Origin {
    std::uint64_t packet;
    std::uint32_t opcode;
};
inline std::vector<Origin>& DeferredOrigins() {
    thread_local std::vector<Origin>* slot = nullptr;
    if (slot == nullptr) slot = new std::vector<Origin>();
    return *slot;
}

inline void Bind(std::uint32_t queue) {
    if (!Enabled) return;
    Queue = queue;
    if (Local == nullptr) Local = Output().Register();
}

inline std::uint64_t Emit(Kind kind, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0, std::uint64_t d = 0,
                          std::uint64_t batch = 0, std::uint32_t flags = 0) {
    if (!Enabled) return 0;
    if (Local == nullptr) Bind(Queue);
    const auto id = (static_cast<std::uint64_t>(Local->Thread()) << 48u) | ++Serial;
    Local->Push({Now(), id, a, b, c, d, Queue, static_cast<std::uint32_t>(kind), Opcode, flags, Packet, batch});
    return id;
}

inline void Store(Kind kind, std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint64_t batch = 0, std::uint32_t flags = 0) {
    if (!Enabled) return;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 4) {
        std::uint64_t value = 0;
        const auto count = std::min<std::size_t>(4, bytes.size() - offset);
        std::memcpy(&value, bytes.data() + offset, count);
        Emit(kind, address + offset, value, stamp, count, batch, flags);
    }
}

class PacketScope {
public:
    PacketScope(std::uint32_t opcode, std::uint64_t submission, std::uint64_t offset) {
        if (!Enabled) return;
        previous = Packet;
        previousOpcode = Opcode;
        Opcode = opcode;
        Packet = Emit(Kind::PacketBegin, submission, offset);
    }
    ~PacketScope() {
        if (!Enabled) return;
        Emit(Kind::PacketEnd, Packet);
        Packet = previous;
        Opcode = previousOpcode;
    }
private:
    std::uint64_t previous = 0;
    std::uint32_t previousOpcode = 0;
};

class ContextScope {
public:
    ContextScope(std::uint64_t packet, std::uint32_t opcode) {
        if (Enabled) { previous = Packet; previousOpcode = Opcode; Packet = packet; Opcode = opcode; }
    }
    ~ContextScope() { if (Enabled) { Packet = previous; Opcode = previousOpcode; } }
private:
    std::uint64_t previous;
    std::uint32_t previousOpcode;
};

class DeferredScope {
public:
    DeferredScope() { if (Enabled) origins.swap(DeferredOrigins()); }
    ~DeferredScope() {
        if (Enabled && DeferredOrigins().empty()) {
            origins.clear();
            origins.swap(DeferredOrigins());
        }
    }
    ContextScope At(std::size_t index) const {
        if (Enabled && index < origins.size()) return ContextScope(origins[index].packet, origins[index].opcode);
        return ContextScope(0, 0);
    }
private:
    std::vector<Origin> origins;
};

class GpuWaitScope {
public:
    GpuWaitScope(std::uint64_t serial, std::uint64_t recorder, std::uint64_t batch = 0) {
        if (Enabled) id = Emit(Kind::GpuWaitBegin, serial, recorder, 0, 0, batch);
    }
    ~GpuWaitScope() { if (Enabled) Emit(Kind::GpuWaitEnd, id, 0, 0, 0, 0, result); }
    std::uint32_t result = 0xffffffffu;
private:
    std::uint64_t id = 0;
};

class WaitScope {
public:
    WaitScope(std::span<const std::uint32_t> packet, std::uint64_t address, std::uint64_t bytes) {
        if (!Enabled) return;
        const bool wide = ((packet[0] >> 8u) & 0xffu) == 0x93u;
        const auto reference = wide ? packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u) : packet[4];
        const auto mask = wide ? packet[6] | (static_cast<std::uint64_t>(packet[7]) << 32u) : packet[5];
        id = Emit(Kind::WaitBegin, address, reference, mask, bytes, 0, packet[1] & 7u);
    }
    ~WaitScope() { if (Enabled) Emit(Kind::WaitEnd, id, 0, 0, 0, 0, end); }
    std::uint32_t end = 6;
private:
    std::uint64_t id = 0;
};

}

#endif
