#include "prx/libSceAgcDriver/Graphics/include/GpuAuthorityCensus.hpp"
#include "prx/libSceAgcDriver/Graphics/include/LazyCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace AgcDriver::Graphics::GpuAuthorityCensus {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(Kind::Count)> Names{"copyback", "block_publish", "heap_readback", "storage", "depth", "shadow", "copy", "label", "fill", "direct_shader", "cpu_writeback", "dcc", "storage_shadow"};
struct ProducerSlot {};
Producer& producer() { return HostThreadLocal<Producer, ProducerSlot>(); }

struct Counters {
    std::atomic<std::uint64_t> frames{0}, checks{0}, mismatches{0}, expired{0}, ioErrors{0};
    std::array<std::atomic<std::uint64_t>, Names.size()> publications{}, bytes{}, cpu{}, gpuOnly{}, untouched{};
    Counters() {
        const auto add = [](std::string name, const std::atomic<std::uint64_t>& value) { AgcDriver::ArchB::Recorder::RegisterCounter((new std::string(std::move(name)))->c_str(), value); };
        add("n5c_frames", frames);
        add("n5c_verify_checks", checks);
        add("n5c_verify_mismatches", mismatches);
        add("n5c_expired", expired);
        add("n5c_io_errors", ioErrors);
        for (std::size_t i = 0; i < Names.size(); ++i) {
            const auto prefix = std::string("n5c_") + Names[i];
            add(prefix + "_publications", publications[i]);
            add(prefix + "_bytes", bytes[i]);
            add(prefix + "_cpu_bytes", cpu[i]);
            add(prefix + "_gpu_only_bytes", gpuOnly[i]);
            add(prefix + "_untouched_bytes", untouched[i]);
        }
    }
};
Counters& counters() { static Counters value; return value; }

struct Event {
    std::uint64_t id, frame, begin, end;
    Kind kind;
    Producer source;
    const void* recorder;
    std::uint64_t serial;
    std::uint32_t timing;
    std::uint64_t cpu = 0, gpu = 0, frameCpu = 0, frameGpu = 0;
    bool reported = false;
    bool watched = false;
    std::uint64_t ticks = 0;
    std::uint64_t lastRead = 0;
};
struct Segment { std::uint64_t end; std::shared_ptr<Event> event; };
struct Origin { std::uint64_t end; Producer source; };
struct State {
    std::mutex mutex;
    std::uint64_t frame = 0, nextId = 0, queries = 0, readSequence = 0;
    std::map<std::uint64_t, Segment> live;
    std::map<std::uint64_t, Origin> origins;
    std::deque<std::shared_ptr<Event>> events;
    std::array<std::uint64_t, 64> readerCalls{}, readerHits{}, readerBytes{};
    std::FILE* output = nullptr;
    bool opened = false;
    bool verify = [] { const auto* v = std::getenv("APS5_ARCHB_GPU_AUTHORITY_CENSUS_VERIFY"); return v && v[0] == '1'; }();
    void Open() {
        if (opened) return;
        opened = true;
        const auto* path = std::getenv("APS5_GPU_AUTHORITY_CENSUS_FILE");
        output = std::fopen(path ? path : "gpu-authority-census.csv", "w");
        if (!output) { counters().ioErrors.fetch_add(1); return; }
        std::setvbuf(output, nullptr, _IOFBF, 1024 * 1024);
        std::fprintf(output, "schema,1\n");
    }
};
State& state() { static auto* value = new State; return *value; }
std::atomic<bool> running{false};

template<class T> auto firstOverlap(T& ranges, std::uint64_t begin) {
    auto it = ranges.upper_bound(begin);
    if (it != ranges.begin() && std::prev(it)->second.end > begin) --it;
    return it;
}

template<class T> void eraseRange(T& ranges, std::uint64_t begin, std::uint64_t end) {
    for (auto it = firstOverlap(ranges, begin); it != ranges.end() && it->first < end;) {
        const auto from = it->first;
        auto entry = it->second;
        it = ranges.erase(it);
        if (from < begin) { auto left = entry; left.end = begin; ranges.emplace(from, std::move(left)); }
        if (entry.end > end) { ranges.emplace(end, std::move(entry)); break; }
    }
}

void read(State& s, std::uint64_t begin, std::uint64_t end, std::uint32_t reader, bool cpu, std::uint64_t ticks = 0) {
    const bool check = s.verify && (s.live.size() <= 16 || s.queries++ % 1024 == 0);
    const auto sequence = ++s.readSequence;
    std::uint64_t hits = 0;
    std::vector<std::uint64_t> indexed;
    std::uint64_t bytes = 0;
    for (auto it = firstOverlap(s.live, begin); it != s.live.end() && it->first < end; ++it) {
        if (it->second.end <= begin) continue;
        auto& event = *it->second.event;
        if (ticks != 0 && event.ticks > ticks) continue;
        const auto bit = std::uint64_t{1} << std::min(reader, 63u);
        (cpu ? event.cpu : event.gpu) |= bit;
        if (event.frame == s.frame) (cpu ? event.frameCpu : event.frameGpu) |= bit;
        if (event.lastRead != sequence) {
            event.lastRead = sequence;
            ++hits;
            if (check) indexed.push_back(event.id);
        }
        bytes += std::min(end, it->second.end) - std::max(begin, it->first);
    }
    if (cpu && hits != 0) {
        const auto index = std::min(reader, 63u);
        ++s.readerCalls[index];
        s.readerHits[index] += hits;
        s.readerBytes[index] += bytes;
    }
    if (check) {
        std::vector<std::uint64_t> reference;
        for (const auto& [from, segment] : s.live) if (from < end && begin < segment.end && (ticks == 0 || segment.event->ticks <= ticks)) reference.push_back(segment.event->id);
        std::sort(reference.begin(), reference.end());
        reference.erase(std::unique(reference.begin(), reference.end()), reference.end());
        std::sort(indexed.begin(), indexed.end());
        counters().checks.fetch_add(1, std::memory_order_relaxed);
        if (reference != indexed) counters().mismatches.fetch_add(1, std::memory_order_relaxed);
    }
}

void row(State& s, const Event& e, char type) {
    if (!s.output) return;
    std::fprintf(s.output, "%c,%llu,%llu,%llu,%llu,%s,%u,%u,%llu,%llu,%llu,%llu,%u,%llu,%llu,%llu,%llu,%u\n", type,
        static_cast<unsigned long long>(e.id), static_cast<unsigned long long>(e.frame), static_cast<unsigned long long>(e.begin), static_cast<unsigned long long>(e.end), Names[static_cast<std::size_t>(e.kind)],
        e.source.queue, e.source.type, static_cast<unsigned long long>(e.source.program), static_cast<unsigned long long>(e.source.target), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(e.recorder)), static_cast<unsigned long long>(e.serial), e.timing,
        static_cast<unsigned long long>(e.frameCpu), static_cast<unsigned long long>(e.frameGpu), static_cast<unsigned long long>(e.cpu), static_cast<unsigned long long>(e.gpu), e.watched ? 1u : 0u);
}

}

bool Enabled() { return AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::GpuAuthorityCensus); }

Producer CurrentProducer() { return Enabled() ? producer() : Producer{}; }
ProducerScope::ProducerScope(std::uint32_t type, std::uint64_t program) : active(Enabled()) {
    if (active) { previous = producer(); producer() = {GuestMemory::CurrentPacket().queue, type, program, 0}; }
}
ProducerScope::~ProducerScope() { if (active) producer() = previous; }

ProducerScope::ProducerScope(Producer source) : active(Enabled()) {
    if (active) { previous = producer(); producer() = source; }
}

Producer ProducerFor(std::uint64_t begin, std::uint64_t end) {
    if (!Enabled() || end <= begin) return {};
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto it = firstOverlap(s.origins, begin);
    return it != s.origins.end() && it->first <= begin && end <= it->second.end ? it->second.source : Producer{};
}

void Produced(std::uint64_t begin, std::uint64_t end, std::uint64_t target) {
    if (!Enabled() || end <= begin) return;
    auto source = producer();
    source.target = target;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    eraseRange(s.live, begin, end);
    eraseRange(s.origins, begin, end);
    s.origins.emplace(begin, Origin{end, source});
}

void Published(std::uint64_t begin, std::uint64_t end, Kind kind, const void* recorder, std::uint64_t serial, std::uint32_t timing) {
    if (!Enabled() || end <= begin || kind >= Kind::Count) return;
    running.store(true, std::memory_order_relaxed);
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.Open();
    eraseRange(s.live, begin, end);
    for (auto from = begin; from < end;) {
        auto source = producer();
        auto to = end;
        const auto origin = firstOverlap(s.origins, from);
        if (origin != s.origins.end() && origin->first < end) {
            if (origin->first <= from) { source = origin->second.source; to = std::min(end, origin->second.end); }
            else to = origin->first;
        }
        auto event = std::make_shared<Event>(Event{++s.nextId, s.frame, from, to, kind, source, recorder, serial, timing});
        event->ticks = GuestArena::GuestWtFaultTicks_nid_no_patch(nullptr);
        s.live.emplace(from, Segment{to, event});
        s.events.push_back(std::move(event));
        const auto k = static_cast<std::size_t>(kind);
        counters().publications[k].fetch_add(1, std::memory_order_relaxed);
        counters().bytes[k].fetch_add(to - from, std::memory_order_relaxed);
        from = to;
    }
}

void CpuRead(std::uint64_t begin, std::size_t bytes, std::uint32_t reader, std::uint64_t ticks) {
    if (!Enabled() || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - begin) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    read(s, begin, begin + bytes, reader, true, ticks);
}

void StoragePublished(std::uint64_t begin, std::uint64_t end, const char* reason, const void* recorder, std::uint64_t serial, std::uint32_t timing) {
    if (!Enabled()) return;
    Published(begin, end, reason && (std::strstr(reason, "depth plane") || std::strstr(reason, "stencil plane")) ? Kind::DepthPlane : Kind::StorageWriteBack, recorder, serial, timing);
}

void GpuRead(std::uint64_t begin, std::uint64_t end, std::uint32_t reader) {
    if (!Enabled() || end <= begin) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    read(s, begin, end, reader, false);
}

void Overwritten(std::uint64_t begin, std::uint64_t end, std::uint64_t ticks) {
    if (!Enabled() || end <= begin) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (ticks == 0) eraseRange(s.live, begin, end);
    else {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        for (auto it = firstOverlap(s.live, begin); it != s.live.end() && it->first < end; ++it) if (it->second.event->ticks <= ticks) ranges.emplace_back(std::max(begin, it->first), std::min(end, it->second.end));
        for (const auto& [from, to] : ranges) eraseRange(s.live, from, to);
    }
}

void WatchCoverage(std::uint64_t begin, std::uint64_t end, bool complete) {
    if (!Enabled() || end <= begin) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    for (auto it = firstOverlap(s.live, begin); it != s.live.end() && it->first < end; ++it) {
        auto& event = *it->second.event;
        event.watched = complete && begin <= event.begin && event.end <= end;
    }
}

void GpuTime(const void* recorder, std::uint64_t serial, std::uint32_t index, std::uint64_t key, double ns, std::uint64_t bytes) {
    if (!Enabled()) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.Open();
    if (s.output) std::fprintf(s.output, "T,%llu,%llu,%u,%llu,%.0f,%llu\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(recorder)), static_cast<unsigned long long>(serial), index, static_cast<unsigned long long>(key), ns, static_cast<unsigned long long>(bytes));
}

void Flip() {
    if (!Enabled()) {
        if (running.exchange(false, std::memory_order_relaxed)) {
            auto& s = state();
            std::lock_guard lock(s.mutex);
            for (const auto& event : s.events) row(s, *event, 'X');
            s.live.clear(); s.origins.clear(); s.events.clear();
            s.readerCalls.fill(0); s.readerHits.fill(0); s.readerBytes.fill(0);
            if (s.output) std::fflush(s.output);
            ++s.frame;
        }
        return;
    }
    ArchC::LazyCensus::Drain();
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.Open();
    if (s.output) std::fprintf(s.output, "F,%llu,%lld\n", static_cast<unsigned long long>(s.frame), static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    for (auto& event : s.events) if (!event->reported) {
        row(s, *event, 'P');
        event->reported = true;
        const auto k = static_cast<std::size_t>(event->kind);
        (event->frameCpu ? counters().cpu : event->frameGpu ? counters().gpuOnly : counters().untouched)[k].fetch_add(event->end - event->begin, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < s.readerCalls.size(); ++i) if (s.readerCalls[i]) {
        if (s.output) std::fprintf(s.output, "R,%llu,%zu,%llu,%llu,%llu\n", static_cast<unsigned long long>(s.frame), i, static_cast<unsigned long long>(s.readerCalls[i]), static_cast<unsigned long long>(s.readerHits[i]), static_cast<unsigned long long>(s.readerBytes[i]));
        s.readerCalls[i] = s.readerHits[i] = s.readerBytes[i] = 0;
    }
    for (auto it = s.events.begin(); it != s.events.end();) {
        const bool aged = (*it)->frame + 60 <= s.frame;
        if (it->use_count() == 1 || aged) {
            row(s, **it, aged ? 'X' : 'E');
            if (aged) {
                const auto id = (*it)->id;
                for (auto live = s.live.begin(); live != s.live.end();) { if (live->second.event->id == id) live = s.live.erase(live); else ++live; }
                counters().expired.fetch_add(1, std::memory_order_relaxed);
            }
            it = s.events.erase(it);
        } else ++it;
    }
    if (s.output && std::fflush(s.output) != 0) counters().ioErrors.fetch_add(1, std::memory_order_relaxed);
    ++s.frame;
    counters().frames.fetch_add(1, std::memory_order_relaxed);
}

Totals Snapshot() {
    auto& c = counters();
    Totals result;
    for (std::size_t k = 0; k < Names.size(); ++k) {
        result.publications += c.publications[k].load();
        result.bytes += c.bytes[k].load();
        result.cpuBytes += c.cpu[k].load();
        result.gpuOnlyBytes += c.gpuOnly[k].load();
        result.untouchedBytes += c.untouched[k].load();
    }
    result.verifyChecks = c.checks.load();
    result.verifyMismatches = c.mismatches.load();
    return result;
}

void ResetForTest() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.live.clear(); s.origins.clear(); s.events.clear(); s.frame = s.nextId = s.queries = s.readSequence = 0;
    s.readerCalls.fill(0); s.readerHits.fill(0); s.readerBytes.fill(0);
    auto& c = counters();
    for (std::size_t k = 0; k < Names.size(); ++k) { c.publications[k] = 0; c.bytes[k] = 0; c.cpu[k] = 0; c.gpuOnly[k] = 0; c.untouched[k] = 0; }
    c.frames = c.checks = c.mismatches = 0;
}

}
