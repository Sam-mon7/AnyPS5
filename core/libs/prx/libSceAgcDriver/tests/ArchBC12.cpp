#include "prx/libSceAgcDriver/Execution/include/FrameCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/ProofEvents.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace AgcDriver;
using namespace AgcDriver::ArchB;
constexpr std::size_t Block = 65536;
std::array<ProofEvent, 1024> events{};
std::atomic<std::size_t> eventCount{0};

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void observe(const ProofEvent& event) noexcept {
    const auto index = eventCount.fetch_add(1, std::memory_order_relaxed);
    if (index < events.size()) events[index] = event;
}

std::uint64_t address(const void* pointer) {
    return reinterpret_cast<std::uintptr_t>(pointer);
}

ProofEpoch epoch() {
    return ProofEvents::Acquire(true).epoch;
}

void expect(ProofSource source, std::uint64_t begin, std::size_t bytes, std::size_t count) {
    std::size_t found = 0;
    require(eventCount.load() <= events.size(), "event fixture overflow");
    for (std::size_t i = 0; i < eventCount; ++i) {
        if (events[i].source != source) continue;
        require(events[i].rangeKnown && events[i].address == begin && events[i].bytes == bytes, "event range differs from its mutation");
        ++found;
    }
    require(found == count, "event count differs from fixture mutations");
}

struct Region {
    void* pointer = GuestArena::GuestArenaAllocate_nid_postfix(Block, Block);
    bool registered = false;
    Region() {
        GuestArena::GuestArenaCommit_nid_postfix(pointer, Block, 4, Block);
        GuestAllocations::Mutation mutation;
        mutation.Add(pointer, Block, true, true, false);
        registered = true;
#ifndef _WIN32
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(pointer, Block);
#endif
        std::memset(pointer, 0, Block);
        GuestMemory::CheckRange(pointer, Block, 1, true);
    }
    ~Region() {
        if (registered) {
            GuestAllocations::Mutation mutation;
            mutation.Remove(pointer);
        }
#ifndef _WIN32
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(pointer, Block);
#endif
        GuestArena::GuestArenaReset_nid_postfix(pointer, Block);
        GuestArena::GuestArenaRelease_nid_postfix(pointer, Block);
    }
};

void cpuEvents() {
    std::array<std::byte, 16> memory{};
    const std::array<std::byte, 16> replacement{std::byte{0x5a}};
    GuestMemory::CheckRange(memory.data(), memory.size(), 1, true);
    const auto before = epoch();
    eventCount = 0;
    GuestMemory::Write(address(memory.data()), replacement);
    require(memory == replacement, "controlled CPU write changed output");
    require(epoch().memory > before.memory, "FAIL proof_cpu_events: hooks disabled");
    expect(ProofSource::ExplicitBreak, address(memory.data()), memory.size(), 1);
    expect(ProofSource::TrackerWrite, address(memory.data()), memory.size(), 1);
    require(eventCount == 2, "controlled write emitted unexpected events");
    const auto recorderBefore = Recorder::Collect(Recorder::Clock::now()).counters;
    ProofEvents::FlushCounters();
    const auto counted = Recorder::Collect(Recorder::Clock::now()).counters;
    require(counted.at("proof_cpu_events") == 2, "recorder CPU total differs from fixture mutations");
    require(!recorderBefore.contains("proof_cpu_events"), "mutation path flushed diagnostic counters");

    Region region;
    const auto base = address(region.pointer);
    eventCount = 0;
    auto saved = epoch();
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(base, Block);
    require(epoch().memory > saved.memory, "forget did not invalidate memory evidence");
    expect(ProofSource::TrackerForget, base, Block, 1);
    eventCount = 0;
    saved = epoch();
    {
        GuestArena::HostWrite write(region.pointer, Block);
        require(epoch().memory > saved.memory, "host write did not announce before access");
        std::memset(region.pointer, 0x22, Block);
        saved = epoch();
    }
    require(epoch().memory > saved.memory, "host write completion did not release-publish");
    expect(ProofSource::HostWriteBegin, base, Block, 1);
    expect(ProofSource::HostWriteEnd, base, Block, 1);
    require(eventCount == 2, "host write emitted unexpected events");

    const bool watched = GuestMemory::Watched(base, Block);
#ifdef _WIN32
    require(watched, "Windows native collect fixture is unavailable");
#endif
    GuestMemory::CollectWritesUncached(base, Block);
    eventCount = 0;
    saved = epoch();
    *static_cast<volatile std::uint32_t*>(region.pointer) = 0x44332211;
    GuestMemory::CollectWritesUncached(base, Block);
    if (watched) {
        require(epoch().memory > saved.memory, "collected native write did not publish");
        bool collected = false;
        for (std::size_t i = 0; i < eventCount; ++i) if (events[i].source == ProofSource::TrackerCollect) {
            require(events[i].rangeKnown && events[i].address <= base && events[i].address + events[i].bytes >= base + 4, "collect lost dirty page range");
            collected = true;
        }
        require(collected, "tracker collect event missing");
    } else {
        std::puts("native collect unavailable: conservative coverage asserted; controlled writes remain tested");
        require(ProofEvents::Acquire(true).Conservative(), "untracked native writes admitted");
    }
    eventCount = 0;
    saved = epoch();
    GuestMemory::Unwatch(base, Block);
    require(epoch().memory > saved.memory, "unwatch did not invalidate memory evidence");
    expect(ProofSource::TrackerUnwatch, base, Block, 1);
    eventCount = 0;
    saved = epoch();
    GuestMemory::BreakProofs();
    require(epoch().memory == saved.memory + 1 && eventCount == 1 && !events[0].rangeKnown, "unclassified break was not global");
    require(ProofEvents::Acquire(true).Conservative(), "incomplete event coverage admitted reuse");
}

void mapEvents() {
    Region region;
    std::memset(region.pointer, 0x6d, Block);
    const auto base = address(region.pointer);
    auto saved = epoch();
    eventCount = 0;
    {
        GuestAllocations::Mutation mutation;
        mutation.Protect(region.pointer, Block, true, false, false, [&] {
            require(epoch().mapping > saved.mapping, "FAIL proof_mapping_events: hooks disabled");
            GuestArena::GuestArenaSetProtection_nid_postfix(base, Block, 2);
        });
        require(epoch().mapping > saved.mapping, "protection not visible before registry unlock");
    }
    expect(ProofSource::Protection, base, Block, 4);
    saved = epoch();
    {
        GuestAllocations::Mutation mutation;
        mutation.Unmap(region.pointer, Block, [&](const void* pointer, std::size_t bytes, const void*, bool) {
            require(epoch().mapping > saved.mapping, "unmap was not announced before OS mutation");
            GuestArena::GuestArenaReset_nid_postfix(const_cast<void*>(pointer), bytes);
        });
        region.registered = false;
    }
    require(epoch().mapping > saved.mapping, "retirement did not change mapping epoch");
    const auto retired = epoch();
    GuestArena::GuestArenaCommit_nid_postfix(region.pointer, Block, 4, Block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(region.pointer, Block, true, true, false);
        region.registered = true;
    }
    require(epoch().mapping > retired.mapping && epoch().mapping > saved.mapping, "same-address mapping ABA preserved old epoch");
    require(*static_cast<std::uint32_t*>(region.pointer) == 0, "remapped storage retained old content");
    for (std::size_t i = 0; i < eventCount; ++i) require(events[i].rangeKnown && events[i].address == base && events[i].bytes == Block, "mapping mutation lost guest range");
}

void oldInvalidation() {
    const auto serial = GuestMemory::ProofSerial();
    const auto hold = GuestMemory::GpuHoldSerial();
    const auto saved = epoch();
    {
        std::lock_guard lock(GuestMemory::GpuMutex());
        require(GuestMemory::ProofSerial() == serial + 1, "read-only lock stopped invalidating proofs");
        require(GuestMemory::GpuHoldSerial() == hold + 1, "GPU hold serial changed semantics");
    }
    require(epoch() == saved, "read-only lock was misclassified as mutation");
    const auto resource = GuestMemory::ResourceProofSerial();
    GuestMemory::BreakProofs();
    require(GuestMemory::ProofSerial() == serial + 2, "explicit legacy invalidation changed");
    require(GuestMemory::ResourceProofSerial() == resource + (GuestMemory::ResourceEpochProofsEnabled() ? 1 : 0), "legacy resource proof mode changed");
    std::thread proofThread([] {
        GuestMemory::MarkProofThread();
        const auto before = GuestMemory::ProofSerial();
        {
            std::lock_guard lock(GuestMemory::GpuMutex());
            require(GuestMemory::ProofSerial() == before, "proof-thread lock semantics changed");
            GuestMemory::BreakProofs();
            require(GuestMemory::ProofSerial() == before + 1, "proof thread hid explicit mutation");
        }
    });
    proofThread.join();
    const bool enabled = ProofEvents::Enabled();
    const auto scoped = epoch();
    {
        ProofEvents::Work item(false);
        GuestMemory::BreakProofs();
        require(epoch() == scoped, "off item emitted events after handoff");
        {
            ProofEvents::Work nested(enabled);
            GuestMemory::BreakProofs();
        }
        require(!ProofEvents::Enabled(), "nested item lost enclosing switch decision");
    }
    require(ProofEvents::Enabled() == enabled && epoch().memory == scoped.memory + (enabled ? 1 : 0), "item switch decision did not survive nesting");
    ProofEvents::FlushCounters();
    Recorder::Collect(Recorder::Clock::now());
    Recorder::Boundary(false, 1, Recorder::Clock::now());
    const auto unrecorded = epoch();
    GuestMemory::BreakProofs();
    require(epoch().memory == unrecorded.memory + (enabled ? 1 : 0), "correctness epoch depended on recorder");
    Recorder::Boundary(true, 2, Recorder::Clock::now());
    ProofEvents::FlushCounters();
    const auto counters = Recorder::Collect(Recorder::Clock::now()).counters;
    require(!counters.contains("proof_cpu_events") || counters.at("proof_cpu_events") == 0, "recorder-off mutation incremented counters");
}

void outsideLock() {
    std::array<std::byte, 16> memory{};
    const auto prepared = memory;
    GuestMemory::CheckRange(memory.data(), memory.size(), 1, true);
    const auto saved = epoch();
    const auto serial = GuestMemory::ProofSerial();
    const auto hold = GuestMemory::GpuHoldSerial();
    const bool enabled = ProofEvents::Enabled();
    std::atomic<bool> finished{false};
    std::lock_guard lock(GuestMemory::GpuMutex());
    std::thread writer([&] {
        const std::array<std::byte, 16> next{std::byte{0x71}};
        GuestMemory::StoreOwnBytes(address(memory.data()), next);
        finished.store(true, std::memory_order_release);
    });
    if (enabled) while (epoch().memory < saved.memory + 2) std::this_thread::yield();
    else while (!finished.load(std::memory_order_acquire)) std::this_thread::yield();
    require(GuestMemory::CompareMapped(address(memory.data()), prepared) == GuestMemory::Compare::Differs, "stale prepared bytes passed after outside-lock write");
    require(memory[0] == std::byte{0x71}, "acquire did not expose CPU payload");
    writer.join();
    require(GuestMemory::ProofSerial() > serial && GuestMemory::GpuHoldSerial() == hold + 1, "outside writer acquired GPU lock or lost legacy invalidation");
    if (enabled) require(epoch() != saved, "outside writer preserved old prepared tuple");
}

void benchmark(const std::filesystem::path& capture) {
    const FrameCaptureReader input(capture);
    require(!input.Truncated() && !input.Chunks().empty(), "capture is incomplete");
    std::vector<FrameMemoryChunk> mutations;
    std::uint64_t digest = 14695981039346656037ull;
    for (const auto& chunk : input.Chunks()) {
        if (chunk.kind != FrameMemoryKind::Delta && chunk.kind != FrameMemoryKind::Range) continue;
        mutations.push_back(chunk);
        digest = ((digest ^ chunk.address) * 1099511628211ull ^ chunk.bytes) * 1099511628211ull;
    }
    require(!mutations.empty(), "capture has no mutations");
    ProofEvents::SetObserver(nullptr);
    Recorder::Boundary(false, 0, Recorder::Clock::now());
    constexpr std::uint64_t iterations = 2000000;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        const auto& chunk = mutations[i % mutations.size()];
        const bool mapped = chunk.kind == FrameMemoryKind::Range;
        ProofEvents::Publish(mapped ? ProofMutation::Mapping : ProofMutation::CpuWrite, mapped ? ProofSource::Mapping : ProofSource::TrackerWrite, chunk.address, chunk.bytes);
        GuestAllocations::GuestAllocationsNotifyMutation_nid_no_patch(mapped ? GuestAllocations::MemoryMutation::Mapping : GuestAllocations::MemoryMutation::HostWriteEnd, chunk.address, chunk.bytes);
    }
    const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::printf("hook_benchmark enabled=%d events=%llu ns_per_hook=%.3f ranges_fnv=%llx recorder=off capture=%s\n", ProofEvents::Enabled(), static_cast<unsigned long long>(iterations * 2), ns / (iterations * 2), static_cast<unsigned long long>(digest), capture.string().c_str());
}

}

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "case required");
        const std::string test = argv[1];
        if (test == "benchmark") {
            require(argc == 3, "capture directory required");
            benchmark(argv[2]);
            return 0;
        }
        GuestMemory::WriteWatched();
        Recorder::BindThread("fixture");
        Recorder::Boundary(true, 0, Recorder::Clock::now());
        ProofEvents::SetObserver(&observe);
        std::puts("READY");
        std::fflush(stdout);
        std::jthread watchdog([](std::stop_token stop) {
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!stop.stop_requested()) {
                if (std::chrono::steady_clock::now() > limit) {
                    std::fputs("FAIL progress watchdog\n", stderr);
                    std::_Exit(2);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        if (test == "cpu_events") cpuEvents();
        else if (test == "map_events") mapEvents();
        else if (test == "old_invalidation_kept") oldInvalidation();
        else if (test == "outside_lock_write") outsideLock();
        else throw std::runtime_error("unknown case");
        ProofEvents::SetObserver(nullptr);
        ProofEvents::FlushCounters();
        std::printf("source=fixture %s\n", Recorder::Collect(Recorder::Clock::now()).Json().c_str());
        std::printf("PASS %s\n", argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
