#include "prx/libc/include/CriticalPathTrace.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"
#include <chrono>
#include <charconv>
#include <limits>
#include <string_view>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <memory>
#include <thread>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {
constexpr std::size_t Capacity = 65536;
constexpr std::size_t MaxThreads = 128;
constexpr std::uint64_t MaxEventBytes = 256 * 1024 * 1024;
constexpr std::uint64_t Stopped = std::uint64_t{1} << 63;
struct Event {
    std::uint64_t id, ns, object, value, peer;
    unsigned kind;
    bool truncated;
    char name[96];
};
struct Thread {
    using Buffer = CriticalPath::Detail::Ring<Event, Capacity>;
    std::atomic<Buffer*> ring{nullptr};
    std::uint64_t sequence = 0;
    std::uint64_t emitted = 0, capDropped = 0;
    std::atomic<std::uint64_t> dropped{0}, truncated{0};
    char name[96]{};
    bool nameTruncated = false;
};
struct State {
    std::array<Thread, MaxThreads> threads;
    std::atomic<std::uint64_t> gate{0};
    std::atomic<unsigned> allocated{0};
    std::atomic<std::uint64_t> rejected{0}, presents{0};
    std::uint64_t captureStart = 800, captureLength = 60;
    std::atomic<unsigned> window{0};
    std::atomic_flag presentBusy = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> startNs{0}, endNs{0};
    std::atomic<bool> capReached{false}, closed{false}, joined{false};
    std::uint64_t eventBytes = 0, capDropped = 0;
    std::thread consumer;
    std::FILE* file = nullptr;
    std::array<char, 1024 * 1024> fileBuffer;
};
struct ThreadTag;
using Slot = HostThreadSlot<std::uintptr_t, ThreadTag>;
State* state = nullptr;
const char* const Kinds[] = {"thread_name", "phase_begin", "phase_end", "phase_mark", "wait_begin", "wait_end", "release", "point", "present", "handoff", "gpu_submit_begin", "gpu_submit_end", "gpu_observed", "label_batch"};
std::uint64_t now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool copyName(char (&destination)[96], const char* name) noexcept {
    std::size_t n = 0;
    if (name) for (; n + 1 < sizeof(destination) && name[n]; ++n) destination[n] = name[n];
    destination[n] = 0;
    return name && name[n] != 0;
}
class Line {
public:
    void Text(std::string_view text) noexcept { std::memcpy(cursor, text.data(), text.size()); cursor += text.size(); }
    void Number(std::uint64_t value) noexcept { cursor = std::to_chars(cursor, bytes.data() + bytes.size(), value).ptr; }
    void Quote(const char* text) noexcept {
        static constexpr char Hex[] = "0123456789abcdef";
        *cursor++ = '"';
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
            if (*p == '"' || *p == '\\') { *cursor++ = '\\'; *cursor++ = static_cast<char>(*p); }
            else if (*p < 32 || *p >= 127) {
                Text("\\u00"); *cursor++ = Hex[*p >> 4]; *cursor++ = Hex[*p & 15];
            } else *cursor++ = static_cast<char>(*p);
        }
        *cursor++ = '"';
    }
    std::size_t Size() const noexcept { return cursor - bytes.data(); }
    void Write(std::FILE* file) const noexcept { std::fwrite(bytes.data(), 1, Size(), file); }
private:
    std::array<char, 2048> bytes;
    char* cursor = bytes.data();
};
void writeEvent(State& s, unsigned tid, const Event& e) noexcept {
    auto& thread = s.threads[tid - 1];
    if (s.capReached.load(std::memory_order_relaxed)) { ++thread.capDropped; ++s.capDropped; return; }
    Line line;
    line.Text("["); line.Number(e.id);
    line.Text(","); line.Number(tid);
    line.Text(","); line.Number(e.ns);
    line.Text(","); line.Quote(Kinds[e.kind]);
    line.Text(","); line.Quote(e.name);
    line.Text(","); line.Number(e.object);
    line.Text(","); line.Number(e.value);
    line.Text(","); line.Number(e.peer);
    line.Text(e.truncated ? ",true]\n" : ",false]\n");
    if (s.eventBytes + line.Size() > MaxEventBytes) {
        s.capReached.store(true, std::memory_order_release);
        s.gate.fetch_or(Stopped, std::memory_order_acq_rel);
        ++thread.capDropped; ++s.capDropped;
        return;
    }
    line.Write(s.file);
    s.eventBytes += line.Size();
    ++thread.emitted;
}
void consume(State* s) noexcept {
    for (;;) {
        const bool quiescent = s->gate.load(std::memory_order_acquire) == Stopped;
        bool found = false;
        const auto count = s->allocated.load(std::memory_order_acquire);
        for (unsigned i = 0; i < count; ++i) {
            auto* ring = s->threads[i].ring.load(std::memory_order_acquire);
            if (!ring) continue;
            Event event;
            for (std::size_t n = 0; n < 4096 && ring->Pop(event); ++n) {
                found = true;
                writeEvent(*s, i + 1, event);
            }
        }
        if (!found && quiescent) break;
        if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const unsigned count = s->allocated.load();
    for (unsigned i = 0; i < count; ++i) {
        auto& t = s->threads[i];
        if (*t.name) {
            Line line;
            line.Text("{\"type\":\"thread_metadata\",\"tid\":"); line.Number(i + 1);
            line.Text(",\"name\":"); line.Quote(t.name);
            line.Text(t.nameTruncated ? ",\"name_truncated\":true}\n" : ",\"name_truncated\":false}\n");
            line.Write(s->file);
        }
        std::fprintf(s->file, "{\"type\":\"thread_stats\",\"tid\":%u,\"emitted\":%llu,\"dropped\":%llu,\"cap_dropped\":%llu,\"truncated\":%llu}\n", i + 1, static_cast<unsigned long long>(t.emitted), static_cast<unsigned long long>(t.dropped.load() + t.capDropped), static_cast<unsigned long long>(t.capDropped), static_cast<unsigned long long>(t.truncated.load()));
    }
    const auto start = s->startNs.load(), end = s->endNs.load();
    std::fprintf(s->file, "{\"type\":\"capture_window\",\"opened\":%s,\"closed\":%s,\"start_ns\":%llu,\"end_ns\":%llu,\"start_present_index\":%llu,\"end_present_index\":%llu,\"boundary_unknown\":true}\n", start ? "true" : "false", end ? "true" : "false", static_cast<unsigned long long>(start), static_cast<unsigned long long>(end), static_cast<unsigned long long>(s->captureStart), static_cast<unsigned long long>(s->captureStart + s->captureLength));
    std::fprintf(s->file, "{\"type\":\"diagnostics\",\"unregistered_events\":%llu,\"cap_reached\":%s,\"cap_dropped\":%llu,\"event_bytes\":%llu}\n", static_cast<unsigned long long>(s->rejected.load()), s->capReached.load() ? "true" : "false", static_cast<unsigned long long>(s->capDropped), static_cast<unsigned long long>(s->eventBytes));
    const bool failed = std::ferror(s->file) != 0;
    if (!failed) std::fputs("{\"type\":\"footer\",\"complete\":true}\n", s->file);
    const bool closeFailed = std::fclose(s->file) != 0;
    if (failed || closeFailed) std::fputs("APS5_CRITICAL_PATH: trace output failed\n", stderr);
    s->closed.store(true, std::memory_order_release);
}
bool initialize() noexcept {
    const char* option = std::getenv("APS5_CRITICAL_PATH");
    if (!option || std::strcmp(option, "0") == 0) return false;
    if (std::strcmp(option, "1") != 0) { std::fputs("APS5_CRITICAL_PATH: expected 0 or 1; tracing disabled\n", stderr); return false; }
    try {
        auto pending = std::make_unique<State>();
        auto* s = pending.get();
        if (const char* frames = std::getenv("APS5_TRACE_FRAMES")) {
            const std::string_view spec(frames);
            const auto colon = spec.find(':');
            const auto number = [](std::string_view text, std::uint64_t& value) {
                const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
                return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
            };
            if (colon == std::string_view::npos || !number(spec.substr(0, colon), s->captureStart) || !number(spec.substr(colon + 1), s->captureLength) || !s->captureLength || s->captureStart >= std::numeric_limits<std::uint64_t>::max() - s->captureLength) {
                std::fputs("APS5_CRITICAL_PATH: APS5_TRACE_FRAMES must be start:length with positive length and nonoverflowing closing boundary; tracing disabled\n", stderr);
                return false;
            }
        }
        const char* directory = std::getenv("APS5_TRACE_DIR");
        auto path = std::filesystem::path(directory && *directory ? directory : ".");
        std::filesystem::create_directories(path);
#ifdef _WIN32
        const auto pid = _getpid();
#else
        const auto pid = getpid();
#endif
        path /= "aps5-critical-path-" + std::to_string(pid) + ".jsonl";
        s->file = std::fopen(path.string().c_str(), "wb");
        if (!s->file) { std::fputs("APS5_CRITICAL_PATH: cannot open trace output\n", stderr); return false; }
        std::setvbuf(s->file, s->fileBuffer.data(), _IOFBF, s->fileBuffer.size());
        std::fprintf(s->file, "{\"type\":\"header\",\"format\":\"aps5-critical-path/1\",\"clock\":\"steady_ns\",\"clock_shared\":true,\"coverage\":[\"partial_instrumentation\",\"gpu_completion_clock_unavailable\",\"arbitrary_guest_waits_uninstrumented\"],\"ring_capacity\":%zu,\"max_threads\":%zu,\"capture_mode\":\"window_only\",\"event_encoding\":\"array-v1\",\"capture_start_index\":%llu,\"capture_length\":%llu,\"phase_marks\":\"omitted\",\"edge_contract\":\"redesign-v1\",\"max_event_bytes\":%llu,\"boundary_unknown\":true}\n", Capacity, MaxThreads, static_cast<unsigned long long>(s->captureStart), static_cast<unsigned long long>(s->captureLength), static_cast<unsigned long long>(MaxEventBytes));
        try { s->consumer = std::thread(consume, s); }
        catch (...) { std::fclose(s->file); throw; }
        state = pending.release();
        std::atexit([] { CriticalPathShutdown_nid_no_patch(); });
        return true;
    } catch (...) { std::fputs("APS5_CRITICAL_PATH: initialization failed\n", stderr); return false; }
}
bool configured() noexcept { static const bool enabled = initialize(); return enabled; }
[[gnu::noinline]] bool enabledImpl() noexcept {
    return configured() && !(state->gate.load(std::memory_order_relaxed) & Stopped);
}
[[gnu::noinline]] bool capturingImpl() noexcept {
    return configured() && state->window.load(std::memory_order_acquire) == 1 && !(state->gate.load(std::memory_order_relaxed) & Stopped);
}
[[gnu::noinline]] std::uint64_t emitImpl(unsigned kind, const char* name, std::uint64_t object, std::uint64_t value, std::uint64_t peer) noexcept {
    if (!configured() || kind >= std::size(Kinds) || kind == static_cast<unsigned>(CriticalPath::Kind::PhaseMark)) return 0;
    State& s = *state;
    const bool isPresent = kind == static_cast<unsigned>(CriticalPath::Kind::Present);
    const bool isName = kind == static_cast<unsigned>(CriticalPath::Kind::ThreadName);
    const auto window = s.window.load(std::memory_order_acquire);
    const bool prepare = !isPresent && !isName && window == 0;
    if (window == 2 && !isPresent && !isName) return 0;
    if (prepare) {
        const auto existing = Slot::Get();
        if (existing > 0 && existing <= MaxThreads && s.threads[existing - 1].ring.load(std::memory_order_acquire)) return 0;
        if (existing == MaxThreads + 1) return 0;
    }
    auto gate = s.gate.load(std::memory_order_relaxed);
    do { if (gate & Stopped) return 0; } while (!s.gate.compare_exchange_weak(gate, gate + 1, std::memory_order_acquire));
    constexpr std::uintptr_t Busy = std::uintptr_t{1} << (sizeof(std::uintptr_t) * 8 - 1);
    auto slot = Slot::Get();
    if (slot & Busy) {
        s.rejected.fetch_add(1, std::memory_order_relaxed);
        s.gate.fetch_sub(1, std::memory_order_release);
        return 0;
    }
    Slot::Set(slot | Busy);
    bool capture = !isName && !prepare;
    bool autoClose = false;
    std::uint64_t timestamp = 0;
    if (isPresent) {
        while (s.presentBusy.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
        const auto index = s.presents.fetch_add(1, std::memory_order_relaxed);
        capture = index >= s.captureStart && index <= s.captureStart + s.captureLength;
        if (capture) timestamp = now();
        if (index == s.captureStart) {
            s.startNs.store(timestamp, std::memory_order_relaxed);
            s.window.store(1, std::memory_order_release);
        }
        if (index == s.captureStart + s.captureLength) {
            s.endNs.store(timestamp, std::memory_order_relaxed);
            s.window.store(2, std::memory_order_release);
            autoClose = true;
        }
    }
    if ((capture || isName || prepare) && !slot) {
        auto index = s.allocated.load(std::memory_order_relaxed);
        while (index < MaxThreads && !s.allocated.compare_exchange_weak(index, index + 1)) {}
        slot = index < MaxThreads ? index + 1 : MaxThreads + 1;
        Slot::Set(slot | Busy);
    }
    std::uint64_t id = 0;
    if ((capture || isName || prepare) && slot > MaxThreads) s.rejected.fetch_add(1, std::memory_order_relaxed);
    else if (isName) s.threads[slot - 1].nameTruncated = copyName(s.threads[slot - 1].name, name);
    else if (prepare) {
        auto& t = s.threads[slot - 1];
        auto* ring = new (std::nothrow) Thread::Buffer;
        if (ring) t.ring.store(ring, std::memory_order_release);
        else slot = MaxThreads + 1;
    }
    else if (capture) {
        auto& t = s.threads[slot - 1];
        auto* ring = t.ring.load(std::memory_order_relaxed);
        if (!ring) {
            ring = new (std::nothrow) Thread::Buffer;
            if (ring) t.ring.store(ring, std::memory_order_release);
        }
        if (!ring || t.sequence == ((std::uint64_t{1} << 48) - 1)) t.dropped.fetch_add(1, std::memory_order_relaxed);
        else {
            id = (std::uint64_t(slot) << 48) | ++t.sequence;
            Event e{};
            e.id = id; e.ns = timestamp ? timestamp : now();
            e.kind = kind; e.object = object; e.value = value; e.peer = peer;
            e.truncated = copyName(e.name, name);
            if (e.truncated) t.truncated.fetch_add(1, std::memory_order_relaxed);
            if (!ring->Push(e, isPresent ? 0 : 128)) t.dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (isPresent) s.presentBusy.clear(std::memory_order_release);
    Slot::Set(slot);
    s.gate.fetch_sub(1, std::memory_order_release);
    if (autoClose) CriticalPathShutdown_nid_no_patch();
    return id;
}
[[gnu::noinline]] void shutdownImpl() noexcept {
    if (!configured()) return;
    State& s = *state;
    s.gate.fetch_or(Stopped, std::memory_order_acq_rel);
    while (!s.closed.load(std::memory_order_acquire)) std::this_thread::yield();
    if (!s.joined.exchange(true, std::memory_order_acq_rel) && s.consumer.joinable()) s.consumer.join();
}
}
extern "C" bool APS5_VABI CriticalPathEnabled_nid_no_patch() noexcept { return enabledImpl(); }
extern "C" bool APS5_VABI CriticalPathCapturing_nid_no_patch() noexcept { return capturingImpl(); }
extern "C" std::uint64_t APS5_VABI CriticalPathEmit_nid_no_patch(unsigned kind, const char* name, std::uint64_t object, std::uint64_t value, std::uint64_t peer) noexcept { return emitImpl(kind, name, object, value, peer); }
extern "C" void APS5_VABI CriticalPathShutdown_nid_no_patch() noexcept { shutdownImpl(); }
