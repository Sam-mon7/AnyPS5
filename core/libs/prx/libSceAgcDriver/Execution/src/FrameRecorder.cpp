#include "prx/libSceAgcDriver/Execution/include/FrameRecorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/TraceRevision.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/General.hpp"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

extern "C" int APS5_VABI sceAgcDriverGetTFRing(uintptr_t* base, uint32_t* size);

namespace AgcDriver {
namespace {

std::uint64_t readNumber(const char* text, std::uint64_t fallback) {
    if (text == nullptr || *text == '\0') return fallback;
    std::uint64_t value = 0;
    const std::string_view view(text);
    const auto [last, error] = std::from_chars(view.data(), view.data() + view.size(), value);
    if (error != std::errc{} || last != view.data() + view.size()) throw std::runtime_error("frame capture: '" + std::string(view) + "' is not a number");
    return value;
}

std::string jsonString(std::string_view text) {
    std::string result = "\"";
    for (const char character : text) {
        if (character == '"' || character == '\\') {
            result += '\\';
            result += character;
        } else if (static_cast<unsigned char>(character) < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(character)));
            result += escaped;
        } else {
            result += character;
        }
    }
    return result + "\"";
}

std::vector<std::pair<std::string, std::string>> captureEnvironment() {
#ifdef _WIN32
    char** entries = _environ;
#else
    char** entries = environ;
#endif
    std::vector<std::pair<std::string, std::string>> result;
    for (; entries != nullptr && *entries != nullptr; ++entries) {
        const std::string_view entry = *entries;
        const auto equals = entry.find('=');
        if (!entry.starts_with("APS5_") || equals == std::string_view::npos) continue;
        result.emplace_back(std::string(entry.substr(0, equals)), std::string(entry.substr(equals + 1)));
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::uint64_t threadId() {
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

std::vector<ShaderRegister> copyRegisters(std::span<const ShaderRegister> registers) {
    return {registers.begin(), registers.end()};
}

std::uint64_t pointer(const void* value) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value));
}

std::uint64_t flipValue(std::int64_t value) {
    return static_cast<std::uint64_t>(value);
}

}

FrameRecorder* FrameRecorder::Active() {
    static FrameRecorder* const recorder = []() -> FrameRecorder* {
        const char* directory = std::getenv("APS5_FRAME_CAPTURE");
        if (directory == nullptr || *directory == '\0') return nullptr;
        const char* trigger = std::getenv("APS5_FRAME_CAPTURE_TRIGGER");
        const auto atBoundary = readNumber(std::getenv("APS5_FRAME_CAPTURE_AT_FLIP"), 0);
        const auto frames = readNumber(std::getenv("APS5_FRAME_CAPTURE_FRAMES"), DefaultFrames);
        if (frames == 0) throw std::runtime_error("frame capture: APS5_FRAME_CAPTURE_FRAMES must be at least 1");
        return new FrameRecorder(directory, trigger != nullptr ? trigger : "", atBoundary, frames);
    }();
    return recorder;
}

FrameRecorder::FrameRecorder(std::filesystem::path directory, std::filesystem::path trigger, std::uint64_t atBoundary, std::uint64_t frames)
    : directory(std::move(directory)), trigger(std::move(trigger)), atBoundary(atBoundary), frames(frames), origin(std::chrono::steady_clock::now()) {}

FrameCall FrameRecorder::make(FrameCallKind kind) {
    FrameCall call;
    call.kind = kind;
    call.timeNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - origin).count());
    call.thread = threadId();
    return call;
}

void FrameRecorder::RegisterShader(const Shader* shader) {
    auto call = make(FrameCallKind::RegisterShader);
    call.address = pointer(shader);
    record(std::move(call), true);
}

void FrameRecorder::ResolveShaderAbi(const Shader* shader, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive) {
    auto call = make(FrameCallKind::ResolveShaderAbi);
    call.address = pointer(shader);
    call.context = copyRegisters(context);
    call.primitive = copyRegisters(primitive);
    record(std::move(call), true);
}

void FrameRecorder::ResolveGraphicsStagesAbi(std::span<const Shader* const> stages, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive) {
    auto call = make(FrameCallKind::ResolveGraphicsStagesAbi);
    for (const auto* stage : stages) call.values.push_back(pointer(stage));
    call.context = copyRegisters(context);
    call.primitive = copyRegisters(primitive);
    record(std::move(call), true);
}

void FrameRecorder::ResolveGraphicsAbi(const Shader* vertex, const Shader* pixel, std::uint32_t primitiveType) {
    auto call = make(FrameCallKind::ResolveGraphicsAbi);
    call.values = {pointer(vertex), pointer(pixel), primitiveType};
    record(std::move(call), true);
}

void FrameRecorder::RegisterVideoOutput(std::uint32_t handle) {
    auto call = make(FrameCallKind::RegisterVideoOutput);
    call.values = {handle};
    record(std::move(call), true);
}

void FrameRecorder::Submit(const Packet& packet, std::uint32_t queue) {
    auto call = make(FrameCallKind::Submit);
    call.queue = queue;
    call.address = pointer(packet.addr);
    call.size = packet.dw_num;
    call.flags = packet.flags;
    record(std::move(call), false);
}

void FrameRecorder::Flip(const FlipInfo& info) {
    auto call = make(FrameCallKind::Flip);
    call.cpuFlip = true;
    call.values = {info.handle, flipValue(info.index), info.mode, flipValue(info.argument)};
    record(std::move(call), false);
}

void FrameRecorder::ReserveEopFlip(const FlipInfo& info, std::uint32_t token) {
    auto call = make(FrameCallKind::ReserveEopFlip);
    call.values = {info.handle, flipValue(info.index), info.mode, flipValue(info.argument), token};
    std::lock_guard lock(mutex);
    if (state == State::Waiting) {
        call.ordinal = ++ordinal;
        reservations.push_back(std::move(call));
        if (reservations.size() > ReservationHistory) reservations.pop_front();
        return;
    }
    if (state != State::Recording) return;
    emitDelta();
    if (state != State::Recording) return;
    call.ordinal = ++ordinal;
    std::tie(call.accepted, call.completed) = DriverDetail::Driver::Get().SerialProgress();
    writer->WriteCall(call);
    ++recordedCalls;
}

void FrameRecorder::Suspend() {
    record(make(FrameCallKind::Suspend), false);
}

void FrameRecorder::record(FrameCall call, bool prologueCall) {
    std::lock_guard lock(mutex);
    if (state == State::Waiting) {
        if (!prologueCall) return;
        call.ordinal = ++ordinal;
        prologue.push_back(std::move(call));
        return;
    }
    if (state != State::Recording) return;
    emitDelta();
    if (state != State::Recording) return;
    call.ordinal = ++ordinal;
    std::tie(call.accepted, call.completed) = DriverDetail::Driver::Get().SerialProgress();
    writer->WriteCall(call);
    ++recordedCalls;
}

void FrameRecorder::FrameBoundary() {
    std::lock_guard lock(mutex);
    ++boundaries;
    if (state == State::Recording) {
        if (++recordedFrames >= frames) finish();
        return;
    }
    if (state == State::Waiting && triggered()) start();
}

bool FrameRecorder::triggered() {
    if (!trigger.empty()) {
        std::error_code error;
        if (!std::filesystem::exists(trigger, error)) return false;
        std::ifstream file(trigger);
        std::uint64_t requested = 0;
        if (file >> requested && requested != 0) frames = requested;
        return true;
    }
    return boundaries >= std::max<std::uint64_t>(atBoundary, 1);
}

void FrameRecorder::emitDelta() {
    if (abandonIfDiskLow()) return;
    const auto mark = GuestMemory::WriteGeneration();
    const auto [accepted, completed] = DriverDetail::Driver::Get().SerialProgress();
    const auto group = deltaGroups + 1;
    bool any = false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> written;
    std::vector<std::byte> bytes;
    for (const auto& range : ranges) {
        if (range.kind != FrameRangeKind::Watched && range.kind != FrameRangeKind::Unwatched) continue;
        GuestMemory::CollectWrittenBlocks(range.address, static_cast<std::size_t>(range.bytes), generation, written);
        for (const auto& [begin, end] : written) {
            bytes.resize(static_cast<std::size_t>(end - begin));
            GuestMemory::ReadCommittedUnsynced(begin, bytes);
            writer->WriteDelta(group, completed, begin, bytes);
            deltaBytes += bytes.size();
            any = true;
        }
    }
    generation = mark;
    if (!any) return;
    deltaGroups = group;
    auto call = make(FrameCallKind::Memory);
    call.ordinal = ++ordinal;
    call.accepted = accepted;
    call.completed = completed;
    call.values = {group};
    writer->WriteCall(call);
}

void FrameRecorder::snapshot(const Range& range) {
    writer->WriteRange(range.address, range.bytes, range.kind, range.protection);
    if (!range.readable) {
        unreadableBytes += range.bytes;
        return;
    }
    std::vector<std::byte> bytes;
    for (std::uint64_t at = 0; at < range.bytes; at += SnapshotPieceBytes) {
        if (abandonIfDiskLow()) return;
        bytes.resize(static_cast<std::size_t>(std::min<std::uint64_t>(SnapshotPieceBytes, range.bytes - at)));
        GuestMemory::ReadCommitted(range.address + at, bytes);
        snapshotBytes += writer->WriteSnapshot(range.address + at, bytes);
    }
}

bool FrameRecorder::abandonIfDiskLow() {
    std::error_code error;
    const auto space = std::filesystem::space(directory, error);
    if (error || space.available >= MinimumFreeBytes) return false;
    warnings.push_back("stopped: less than 4 GiB free on the capture volume");
    std::fprintf(stderr, "[frame-capture] stopped: less than 4 GiB free on the capture volume\n");
    try {
        writer->Finish(header());
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "[frame-capture] %s\n", failure.what());
    }
    writer.reset();
    state = State::Done;
    return true;
}

void FrameRecorder::start() {
    ++startAttempts;
    auto& driver = DriverDetail::Driver::Get();
    if (!driver.WaitIdleFor(std::chrono::seconds(10))) {
        std::fprintf(stderr, "[frame-capture] the driver did not go idle within 10 s at frame boundary %llu (attempt %llu)\n", static_cast<unsigned long long>(boundaries), static_cast<unsigned long long>(startAttempts));
        if (startAttempts >= 5) {
            std::fprintf(stderr, "[frame-capture] giving up: no idle frame boundary\n");
            state = State::Done;
        }
        return;
    }
    startedAt = std::chrono::steady_clock::now();
    startBoundary = boundaries;
    ranges = committedRanges();
    writer = std::make_unique<FrameCaptureWriter>(directory);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> written;
    for (const auto& range : ranges) {
        if (range.kind == FrameRangeKind::Watched || range.kind == FrameRangeKind::Unwatched) GuestMemory::CollectWrittenBlocks(range.address, static_cast<std::size_t>(range.bytes), std::numeric_limits<std::uint64_t>::max(), written);
    }
    generation = GuestMemory::WriteGeneration();
    for (const auto& range : ranges) {
        snapshot(range);
        if (!writer) return;
    }
    for (const auto& call : prologue) writer->WriteCall(call);
    std::uintptr_t ringBase = 0;
    std::uint32_t ringSize = 0;
    sceAgcDriverGetTFRing(&ringBase, &ringSize);
    auto ring = make(FrameCallKind::SetTessellationFactorRing);
    ring.ordinal = ++ordinal;
    ring.address = ringBase;
    ring.size = ringSize;
    writer->WriteCall(ring);
    const auto pending = driver.PendingEopFlipTokens();
    const std::set<std::uint64_t> open(pending.begin(), pending.end());
    for (const auto& call : reservations) {
        if (open.contains(call.values.back())) writer->WriteCall(call);
    }
    auto begin = make(FrameCallKind::FrameStart);
    begin.ordinal = ++ordinal;
    std::tie(begin.accepted, begin.completed) = driver.SerialProgress();
    begin.values = {boundaries};
    writer->WriteCall(begin);
    state = State::Recording;
    recordedFrames = 0;
    std::fprintf(stderr, "[frame-capture] started at frame boundary %llu: %zu ranges, %.1f MiB snapshot in %.1f s\n", static_cast<unsigned long long>(boundaries), ranges.size(),
                 static_cast<double>(snapshotBytes) / (1024.0 * 1024.0), std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count());
}

void FrameRecorder::finish() {
    emitDelta();
    if (!writer) return;
    auto end = make(FrameCallKind::End);
    end.ordinal = ++ordinal;
    std::tie(end.accepted, end.completed) = DriverDetail::Driver::Get().SerialProgress();
    writer->WriteCall(end);
    writer->Finish(header());
    std::fprintf(stderr, "[frame-capture] %llu frames, %llu calls, %llu memory deltas (%.1f MiB), %.1f MiB raw, %.1f MiB stored in %s\n", static_cast<unsigned long long>(recordedFrames),
                 static_cast<unsigned long long>(recordedCalls), static_cast<unsigned long long>(deltaGroups), static_cast<double>(deltaBytes) / (1024.0 * 1024.0),
                 static_cast<double>(writer->RawBytes()) / (1024.0 * 1024.0), static_cast<double>(writer->StoredBytes()) / (1024.0 * 1024.0), directory.string().c_str());
    writer.reset();
    state = State::Done;
}

std::string FrameRecorder::header() const {
    std::uintptr_t arenaBase = 0;
    std::size_t arenaBytes = 0;
    GuestArena::GuestArenaRange_nid_postfix(&arenaBase, &arenaBytes);
    std::string text = "{\"magic\":\"aps5-frame-capture 1\"";
    text += ",\"commit\":" + jsonString(APS5_TRACE_COMMIT);
    text += std::string(",\"dirty\":") + (APS5_TRACE_DIRTY < 0 ? "null" : APS5_TRACE_DIRTY != 0 ? "true" : "false");
    text += ",\"arena_base\":" + std::to_string(arenaBase) + ",\"arena_bytes\":" + std::to_string(arenaBytes);
    text += ",\"start_boundary\":" + std::to_string(startBoundary) + ",\"frames\":" + std::to_string(recordedFrames);
    text += ",\"calls\":" + std::to_string(recordedCalls) + ",\"prologue_calls\":" + std::to_string(prologue.size());
    text += ",\"ranges\":" + std::to_string(ranges.size()) + ",\"snapshot_bytes\":" + std::to_string(snapshotBytes) + ",\"unreadable_bytes\":" + std::to_string(unreadableBytes);
    text += ",\"memory_groups\":" + std::to_string(deltaGroups) + ",\"delta_bytes\":" + std::to_string(deltaBytes);
    text += ",\"capture_seconds\":" + std::to_string(std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count());
    text += ",\"env\":{";
    bool first = true;
    for (const auto& [name, value] : captureEnvironment()) {
        text += (first ? "" : ",") + jsonString(name) + ":" + jsonString(value);
        first = false;
    }
    text += "},\"warnings\":[";
    first = true;
    for (const auto& warning : warnings) {
        text += (first ? "" : ",") + jsonString(warning);
        first = false;
    }
    return text + "]}\n";
}

std::vector<FrameRecorder::Range> FrameRecorder::committedRanges() {
#ifdef _WIN32
    std::uintptr_t base = 0;
    std::size_t size = 0;
    GuestArena::GuestArenaRange_nid_postfix(&base, &size);
    if (size == 0) throw std::runtime_error("frame capture: the guest arena is not reserved");
    std::vector<Range> result;
    const auto end = base + size;
    for (auto address = base; address < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) throw std::runtime_error("frame capture: VirtualQuery failed inside the guest arena");
        const auto regionEnd = std::min<std::uintptr_t>(end, reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
        if (info.State == MEM_COMMIT) {
            Range range;
            range.address = address;
            range.bytes = regionEnd - address;
            range.protection = info.Protect;
            const auto access = info.Protect & 0xffu;
            range.readable = (info.Protect & PAGE_GUARD) == 0 && (access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY);
            if (info.Type == MEM_PRIVATE) range.kind = GuestMemory::Watched(range.address, static_cast<std::size_t>(range.bytes)) ? FrameRangeKind::Watched : FrameRangeKind::Unwatched;
            else range.kind = FrameRangeKind::Section;
            auto& last = result.empty() ? range : result.back();
            if (!result.empty() && last.address + last.bytes == range.address && last.kind == range.kind && last.protection == range.protection && last.readable == range.readable) last.bytes += range.bytes;
            else result.push_back(range);
        }
        address = regionEnd;
    }
    return result;
#else
    throw std::runtime_error("frame capture: guest memory capture is implemented on Windows only");
#endif
}

}
