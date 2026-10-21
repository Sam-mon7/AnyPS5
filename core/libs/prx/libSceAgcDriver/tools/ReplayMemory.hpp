#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TOOLS_REPLAYMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TOOLS_REPLAYMEMORY_HPP

#include "prx/libSceAgcDriver/Execution/include/FrameCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/MemoryUsage.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace AgcDriver::ReplayMemory {

struct ProcessUsage {
    std::optional<std::uint64_t> resident;
    std::optional<std::uint64_t> peakResident;
    std::optional<std::uint64_t> privateCommit;
    std::optional<std::uint64_t> peakPrivateCommit;
    std::optional<std::uint64_t> virtualBytes;
    std::optional<std::uint64_t> swap;
    std::optional<std::uint64_t> guestArenaPrivate;
    std::optional<std::uint64_t> hostPrivate;
    std::optional<std::uint64_t> mappedCommitted;
};

inline ProcessUsage ReadProcessUsage() {
    ProcessUsage result;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) throw std::runtime_error("memory report: GetProcessMemoryInfo failed");
    result.resident = counters.WorkingSetSize;
    result.peakResident = counters.PeakWorkingSetSize;
    result.privateCommit = counters.PrivateUsage;
    result.peakPrivateCommit = counters.PeakPagefileUsage;
    std::uintptr_t arenaBase = 0;
    std::size_t arenaBytes = 0;
    GuestArena::GuestArenaRange_nid_postfix(&arenaBase, &arenaBytes);
    result.guestArenaPrivate = result.hostPrivate = result.mappedCommitted = 0;
    MEMORY_BASIC_INFORMATION region{};
    for (std::uintptr_t address = 0; VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) != 0;) {
        const auto begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const auto end = begin + region.RegionSize;
        if (end <= address) break;
        if (region.State == MEM_COMMIT && region.Type == MEM_PRIVATE) {
            const auto overlapBegin = std::max(begin, arenaBase);
            const auto overlapEnd = std::min(end, arenaBase + arenaBytes);
            const auto guest = overlapEnd > overlapBegin ? overlapEnd - overlapBegin : 0;
            *result.guestArenaPrivate += guest;
            *result.hostPrivate += region.RegionSize - guest;
        } else if (region.State == MEM_COMMIT && region.Type == MEM_MAPPED) *result.mappedCommitted += region.RegionSize;
        address = end;
    }
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    if (!status) throw std::runtime_error("memory report: cannot read /proc/self/status");
    for (std::string line; std::getline(status, line);) {
        std::istringstream fields(line);
        std::string key, unit;
        std::uint64_t kib = 0;
        if (!(fields >> key >> kib >> unit) || unit != "kB") continue;
        if (key == "VmRSS:") result.resident = kib * 1024;
        else if (key == "VmHWM:") result.peakResident = kib * 1024;
        else if (key == "VmSize:") result.virtualBytes = kib * 1024;
        else if (key == "VmSwap:") result.swap = kib * 1024;
    }
#endif
    return result;
}

inline void Number(std::ostream& out, std::optional<std::uint64_t> value) {
    if (value) out << *value;
    else out << "null";
}

inline std::uint64_t RangeBytes(std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges) {
    std::sort(ranges.begin(), ranges.end());
    std::uint64_t bytes = 0, end = 0;
    for (const auto& [begin, limit] : ranges) {
        if (limit > end) bytes += limit - std::max(begin, end);
        end = std::max(end, limit);
    }
    return bytes;
}

inline void Capture(std::ostream& out, const FrameCaptureReader& reader) {
    std::array<std::vector<std::pair<std::uint64_t, std::uint64_t>>, 6> kinds;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::uint64_t snapshots = 0, deltas = 0, references = 0, stored = 0;
    std::uint64_t indexBytes = reader.Calls().capacity() * sizeof(FrameCall) + reader.Chunks().capacity() * sizeof(FrameMemoryChunk) + reader.Header().capacity();
    for (const auto& call : reader.Calls()) indexBytes += call.values.capacity() * sizeof(std::uint64_t) + (call.context.capacity() + call.primitive.capacity()) * sizeof(ShaderRegister);
    for (const auto& chunk : reader.Chunks()) {
        stored += chunk.compressedBytes;
        if (chunk.kind == FrameMemoryKind::Range) {
            const auto kind = static_cast<std::size_t>(chunk.rangeKind);
            if (kind == 0 || kind > kinds.size() || chunk.bytes > std::numeric_limits<std::uint64_t>::max() - chunk.address) throw std::runtime_error("memory report: invalid captured range");
            const auto range = std::pair{chunk.address, chunk.address + chunk.bytes};
            kinds[kind - 1].push_back(range);
            ranges.push_back(range);
        } else if (chunk.kind == FrameMemoryKind::Snapshot) snapshots += chunk.bytes;
        else if (chunk.kind == FrameMemoryKind::Delta) deltas += chunk.bytes;
        else if (chunk.kind == FrameMemoryKind::Reference) references += chunk.bytes;
    }
    out << "{\"version\":1,\"type\":\"capture\",\"truncated\":" << (reader.Truncated() ? "true" : "false")
        << ",\"range_address_bytes\":" << RangeBytes(std::move(ranges)) << ",\"range_kinds\":{";
    constexpr std::array names{"watched", "unwatched", "section", "alias", "file", "host"};
    for (std::size_t i = 0; i < kinds.size(); ++i) out << (i == 0 ? "" : ",") << '\"' << names[i] << "\":" << RangeBytes(std::move(kinds[i]));
    out << "},\"snapshot_payload_bytes\":" << snapshots << ",\"delta_payload_bytes\":" << deltas
        << ",\"reference_payload_bytes\":" << references << ",\"stored_payload_bytes\":" << stored << ",\"index_capacity_bytes\":" << indexBytes
        << ",\"semantics\":\"Range bytes count virtual addresses, not unique physical backing. Payload bytes describe disk records, not retained RAM. Index capacity excludes allocator overhead. Process, heap, pool, import and kept counters overlap and must not be summed.\"}\n";
}

class Report {
public:
    explicit Report(const std::filesystem::path& path) {
        if (path.empty()) return;
        file.open(path);
        if (!file) throw std::runtime_error("memory report: cannot open " + path.string());
    }

    void Capture(const FrameCaptureReader& reader) {
        if (!file.is_open()) return;
        ReplayMemory::Capture(file, reader);
        Flush();
    }

    void Checkpoint(const char* stage, std::size_t pass = 0, std::uint64_t skipped = 0, bool queryDriver = true) {
        if (!file.is_open()) return;
        const auto driver = queryDriver ? AgcDriverMemoryUsage_nid_no_patch() : DriverMemoryUsage{};
        const auto process = ReadProcessUsage();
        file << "{\"version\":1,\"type\":\"checkpoint\",\"stage\":\"" << stage << "\",\"pass\":" << pass
             << ",\"elapsed_s\":" << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << ",\"skipped_range_bytes\":" << skipped
             << ",\"process\":{\"resident_bytes\":";
        Number(file, process.resident);
        file << ",\"peak_resident_bytes\":"; Number(file, process.peakResident);
        file << ",\"private_commit_bytes\":"; Number(file, process.privateCommit);
        file << ",\"peak_private_commit_bytes\":"; Number(file, process.peakPrivateCommit);
        file << ",\"virtual_bytes\":"; Number(file, process.virtualBytes);
        file << ",\"swap_bytes\":"; Number(file, process.swap);
        file << ",\"guest_arena_private_bytes\":"; Number(file, process.guestArenaPrivate);
        file << ",\"host_private_region_bytes\":"; Number(file, process.hostPrivate);
        file << ",\"mapped_committed_region_bytes\":"; Number(file, process.mappedCommitted);
        file << "},\"driver\":{\"queried\":" << (queryDriver ? "true" : "false") << ",\"device\":" << (driver.device ? "true" : "false")
             << ",\"retained_buffer_bytes\":" << driver.pool.retainedBuffers << ",\"retained_image_bytes\":" << driver.pool.retainedImages
             << ",\"slab_allocated_bytes\":" << driver.pool.slabAllocated << ",\"slab_unused_bytes\":" << driver.pool.slabUnused
             << ",\"imported_guest_bytes\":" << driver.importedGuest << ",\"open_kept_bytes\":" << driver.openKept << ",\"in_flight_kept_bytes\":" << driver.inFlightKept
             << ",\"heaps\":[";
        for (std::size_t i = 0; i < driver.heaps.size(); ++i) {
            const auto& heap = driver.heaps[i];
            file << (i == 0 ? "" : ",") << "{\"index\":" << i << ",\"device_local\":" << (heap.deviceLocal ? "true" : "false") << ",\"size_bytes\":" << heap.size << ",\"budget_bytes\":";
            Number(file, driver.budgetAvailable ? std::optional(heap.budget) : std::nullopt);
            file << ",\"usage_estimate_bytes\":";
            Number(file, driver.budgetAvailable ? std::optional(heap.usage) : std::nullopt);
            file << '}';
        }
        file << "]}}\n";
        Flush();
        if (process.resident) std::fprintf(stderr, "[memory] %s pass %zu: resident %.1f MiB, peak %.1f MiB, reusable buffers %.1f MiB, images %.1f MiB\n", stage, pass,
            *process.resident / 1048576.0, process.peakResident.value_or(0) / 1048576.0, driver.pool.retainedBuffers / 1048576.0, driver.pool.retainedImages / 1048576.0);
    }

private:
    void Flush() {
        file.flush();
        if (!file) throw std::runtime_error("memory report: write failed");
    }

    std::ofstream file;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

}

#endif
