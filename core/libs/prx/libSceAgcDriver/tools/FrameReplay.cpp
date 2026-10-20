#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/FrameCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "SceTypes.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" int APS5_VABI sceAgcDriverSetTFRing(const volatile void* base, std::uint32_t size);

namespace {

using AgcDriver::FrameCall;
using AgcDriver::FrameCallKind;
using AgcDriver::FrameCaptureReader;
using AgcDriver::FrameMemoryChunk;
using AgcDriver::FrameMemoryKind;
using AgcDriver::FrameRangeAccess;
using Clock = std::chrono::steady_clock;

struct Dump {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    std::filesystem::path path;
};

struct Options {
    std::filesystem::path directory;
    std::vector<Dump> dumps;
    bool pace = true;
    bool strict = false;
    std::chrono::milliseconds paceTimeout{10000};
};

class Frames {
public:
    void MarkReady() {
        std::lock_guard lock(mutex);
        ready.push_back(Clock::now());
    }

    void Failed(std::exception_ptr error) {
        std::lock_guard lock(mutex);
        if (!failure) failure = error;
    }

    std::vector<Clock::time_point> Times() const {
        std::lock_guard lock(mutex);
        return ready;
    }

    std::exception_ptr Failure() const {
        std::lock_guard lock(mutex);
        return failure;
    }

private:
    mutable std::mutex mutex;
    std::vector<Clock::time_point> ready;
    std::exception_ptr failure;
};

class ReplayFlip final : public AgcDriver::IFlipRequest {
public:
    explicit ReplayFlip(Frames& frames) : frames(frames) {}
    void GpuReady(const std::shared_ptr<AgcDriver::FrameTiming>&) override { frames.MarkReady(); }
    void Fail(std::exception_ptr error) noexcept override { frames.Failed(error); }

private:
    Frames& frames;
};

class ReplayRenderingWait final : public AgcDriver::IRenderingWait {
public:
    void Wait() override {}
};

class ReplayOutput final : public AgcDriver::IVideoOutput {
public:
    explicit ReplayOutput(Frames& frames) : frames(frames) {}
    std::shared_ptr<AgcDriver::IFlipRequest> Reserve(const AgcDriver::FlipInfo&) override { return std::make_shared<ReplayFlip>(frames); }
    std::shared_ptr<AgcDriver::IRenderingWait> CaptureRenderingWait(std::uint32_t) override { return std::make_shared<ReplayRenderingWait>(); }
    void Fail(std::exception_ptr error) noexcept override { frames.Failed(error); }

private:
    Frames& frames;
};

std::uint64_t headerNumber(const std::string& header, std::string_view key) {
    const auto quoted = "\"" + std::string(key) + "\":";
    const auto at = header.find(quoted);
    if (at == std::string::npos) throw std::runtime_error("frame replay: the capture header has no " + std::string(key));
    return std::strtoull(header.c_str() + at + quoted.size(), nullptr, 10);
}

void restore(const FrameCaptureReader& reader, const FrameMemoryChunk& chunk) {
    reader.Read(chunk, std::span(reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(chunk.address)), static_cast<std::size_t>(chunk.bytes)));
}

bool committedWritable(std::uint64_t address, std::uint64_t bytes) {
#ifdef _WIN32
    for (auto at = address; at < address + bytes;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(at)), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) return false;
        const auto access = info.Protect & 0xffu;
        if ((info.Protect & PAGE_GUARD) != 0 || (access != PAGE_READWRITE && access != PAGE_EXECUTE_READWRITE)) return false;
        at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    return false;
#endif
}

std::pair<std::uint64_t, std::uint64_t> progress();

void restoreMemory(const FrameCaptureReader& reader) {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    GuestArena::GuestArenaRange_nid_postfix(&base, &size);
    if (!reader.Header().empty() && headerNumber(reader.Header(), "arena_base") != base) throw std::runtime_error("frame replay: the guest arena is at a different address than in the capture");
    progress();
    std::vector<const FrameMemoryChunk*> snapshot;
    {
        GuestAllocations::Mutation mutation;
        for (const auto& chunk : reader.Chunks()) {
            if (chunk.kind == FrameMemoryKind::Snapshot) snapshot.push_back(&chunk);
            if (chunk.kind != FrameMemoryKind::Range || committedWritable(chunk.address, chunk.bytes)) continue;
            auto* const pointer = reinterpret_cast<void*>(static_cast<std::uintptr_t>(chunk.address));
            const auto bytes = static_cast<std::size_t>(chunk.bytes);
            GuestArena::GuestArenaMarkUsed_nid_postfix(pointer, bytes);
#ifdef _WIN32
            GuestArena::GuestArenaCommit_nid_postfix(pointer, bytes, PAGE_READWRITE, bytes);
#else
            GuestArena::GuestArenaCommit_nid_postfix(pointer, bytes, 3, bytes);
#endif
            mutation.Add(pointer, bytes, (chunk.access & FrameRangeAccess::Readable) != 0, (chunk.access & FrameRangeAccess::Writable) != 0, (chunk.access & FrameRangeAccess::Gpu) != 0);
        }
    }
    const auto workers = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    for (std::size_t first = 0; first < snapshot.size(); first += workers) {
        std::vector<std::future<void>> pending;
        for (auto index = first; index < std::min(snapshot.size(), first + workers); ++index) {
            pending.push_back(std::async(std::launch::async, [&reader, chunk = snapshot[index]] { restore(reader, *chunk); }));
        }
        for (auto& future : pending) future.get();
    }
}

std::pair<std::uint64_t, std::uint64_t> progress() {
    std::uint64_t accepted = 0;
    std::uint64_t completed = 0;
    AgcDriverSerialProgress_nid_postfix(&accepted, &completed);
    return {accepted, completed};
}

struct Replay {
    const Options& options;
    const FrameCaptureReader& reader;
    Frames frames;
    std::vector<std::shared_ptr<ReplayOutput>> outputs;
    std::map<std::uint64_t, std::vector<const FrameMemoryChunk*>> groups;
    std::uint64_t paceTimeouts = 0;
    std::uint64_t staleRegistrations = 0;
    std::uint64_t submits = 0;
    std::uint64_t deltaBytes = 0;
    bool recording = false;
    Clock::time_point startedAt{};
    Clock::time_point endedAt{};

    void pace(const FrameCall& call) {
        if (!options.pace || !recording || call.accepted < call.completed) return;
        const auto inFlight = call.accepted - call.completed;
        const auto deadline = Clock::now() + options.paceTimeout;
        while (true) {
            const auto [accepted, completed] = progress();
            if (accepted - completed <= inFlight) return;
            if (Clock::now() >= deadline) {
                ++paceTimeouts;
                if (options.strict) throw std::runtime_error("frame replay: call " + std::to_string(call.ordinal) + " waited longer than the pacing timeout");
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    void execute(const FrameCall& call) {
        pace(call);
        switch (call.kind) {
        case FrameCallKind::RegisterShader:
            registration([&] { AgcDriverRegisterShader_nid_postfix(shader(call.address)); });
            return;
        case FrameCallKind::ResolveShaderAbi:
            registration([&] { AgcDriverResolveShaderAbi_nid_postfix(shader(call.address), call.context, call.primitive); });
            return;
        case FrameCallKind::ResolveGraphicsAbi:
            registration([&] { AgcDriverResolveGraphicsAbi_nid_postfix(shader(call.values.at(0)), shader(call.values.at(1)), static_cast<std::uint32_t>(call.values.at(2))); });
            return;
        case FrameCallKind::ResolveGraphicsStagesAbi: {
            std::vector<const Shader*> stages;
            for (const auto value : call.values) stages.push_back(shader(value));
            registration([&] { AgcDriverResolveGraphicsStagesAbi_nid_postfix(stages, call.context, call.primitive); });
            return;
        }
        case FrameCallKind::RegisterVideoOutput: {
            auto output = std::make_shared<ReplayOutput>(frames);
            outputs.push_back(output);
            AgcDriverRegisterVideoOutput_nid_postfix(static_cast<std::uint32_t>(call.values.at(0)), output);
            return;
        }
        case FrameCallKind::SetTessellationFactorRing:
            sceAgcDriverSetTFRing(reinterpret_cast<const volatile void*>(static_cast<std::uintptr_t>(call.address)), static_cast<std::uint32_t>(call.size));
            return;
        case FrameCallKind::Submit: {
            const Packet packet{reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(call.address)), static_cast<std::uint32_t>(call.size), static_cast<std::uint8_t>(call.flags), {}};
            if (call.queue == 0) sceAgcDriverSubmitDcb(&packet);
            else sceAgcDriverSubmitAcb(call.queue, &packet);
            ++submits;
            return;
        }
        case FrameCallKind::Flip:
            AgcDriverSubmitFlip_nid_postfix(flipInfo(call));
            return;
        case FrameCallKind::ReserveEopFlip: {
            const auto token = static_cast<std::uint32_t>(call.values.at(4));
            if (AgcDriverReserveEopFlipAt_nid_postfix(flipInfo(call), token) != token) throw std::runtime_error("frame replay: EOP flip token " + std::to_string(token) + " was not reserved");
            return;
        }
        case FrameCallKind::Suspend:
            AgcDriverSuspendPoint_nid_postfix();
            return;
        case FrameCallKind::Memory:
            for (const auto* chunk : groups[call.values.at(0)]) {
                restore(reader, *chunk);
                deltaBytes += chunk->bytes;
            }
            return;
        case FrameCallKind::FrameStart:
            recording = true;
            startedAt = Clock::now();
            return;
        case FrameCallKind::End:
            return;
        }
        throw std::runtime_error("frame replay: unknown call kind");
    }

    template <typename TFunction>
    void registration(TFunction&& function) {
        if (recording) {
            function();
            return;
        }
        try {
            function();
        } catch (const std::exception& error) {
            ++staleRegistrations;
            std::fprintf(stderr, "[frame-replay] prologue registration failed: %s\n", error.what());
        }
    }

    static const Shader* shader(std::uint64_t address) {
        return reinterpret_cast<const Shader*>(static_cast<std::uintptr_t>(address));
    }

    static AgcDriver::FlipInfo flipInfo(const FrameCall& call) {
        return {static_cast<std::uint32_t>(call.values.at(0)), static_cast<std::int32_t>(call.values.at(1)), static_cast<std::uint32_t>(call.values.at(2)), static_cast<std::int64_t>(call.values.at(3))};
    }
};

int usage() {
    std::fprintf(stderr, "usage: agc_frame_replay <capture directory> [--no-pace] [--strict] [--dump <address>:<bytes>:<file>]...\n"
                         "  replays the driver calls and guest memory that APS5_FRAME_CAPTURE=<directory> recorded, through this\n"
                         "  build's AGC driver with no title running, and prints one JSON line with the frame times; --dump writes\n"
                         "  guest memory to a file after the replay (address and size in hexadecimal or decimal)\n");
    return 2;
}

Dump parseDump(std::string_view text) {
    const auto first = text.find(':');
    const auto second = first == std::string_view::npos ? std::string_view::npos : text.find(':', first + 1);
    if (second == std::string_view::npos) throw std::invalid_argument("frame replay: --dump takes <address>:<bytes>:<file>");
    const auto number = [](std::string_view part) {
        const std::string value(part);
        char* end = nullptr;
        const auto parsed = std::strtoull(value.c_str(), &end, 0);
        if (value.empty() || end != value.c_str() + value.size()) throw std::invalid_argument("frame replay: '" + value + "' is not a number");
        return static_cast<std::uint64_t>(parsed);
    };
    return {number(text.substr(0, first)), number(text.substr(first + 1, second - first - 1)), std::filesystem::path(std::string(text.substr(second + 1)))};
}

void writeDump(const Dump& dump) {
    auto* const pointer = reinterpret_cast<const std::byte*>(static_cast<std::uintptr_t>(dump.address));
    GuestArena::GuestArenaPrepareHostRead_nid_no_patch(pointer, static_cast<std::size_t>(dump.bytes));
    std::ofstream file(dump.path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(pointer), static_cast<std::streamsize>(dump.bytes));
    if (!file.good()) throw std::runtime_error("frame replay: cannot write " + dump.path.string());
}

int run(const Options& options) {
    const auto loadStart = Clock::now();
    const FrameCaptureReader reader(options.directory);
    if (reader.Truncated()) std::fprintf(stderr, "[frame-replay] the capture is truncated; replaying the complete records\n");
    restoreMemory(reader);
    const auto loaded = Clock::now();
    Replay replay{options, reader};
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind == FrameMemoryKind::Delta) replay.groups[chunk.group].push_back(&chunk);
    }
    for (const auto& call : reader.Calls()) replay.execute(call);
    AgcDriverWaitIdle_nid_postfix();
    replay.endedAt = Clock::now();
    if (const auto failure = replay.frames.Failure()) std::rethrow_exception(failure);
    for (const auto& dump : options.dumps) writeDump(dump);
    const auto ready = replay.frames.Times();
    std::vector<double> frameMs;
    for (std::size_t index = 1; index < ready.size(); ++index) frameMs.push_back(std::chrono::duration<double, std::milli>(ready[index] - ready[index - 1]).count());
    std::sort(frameMs.begin(), frameMs.end());
    const auto percentile = [&](double fraction) { return frameMs.empty() ? 0.0 : frameMs[std::min(frameMs.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(frameMs.size())))]; };
    const auto seconds = std::chrono::duration<double>(replay.endedAt - replay.startedAt).count();
    std::printf("{\"frames\":%zu,\"replay_s\":%.3f,\"fps\":%.3f,\"frame_ms\":{\"p50\":%.3f,\"p90\":%.3f,\"max\":%.3f},\"submits\":%llu,\"delta_mib\":%.1f,\"pace_timeouts\":%llu,\"stale_registrations\":%llu,\"load_s\":%.3f}\n",
                ready.size(), seconds, seconds > 0 ? static_cast<double>(ready.size()) / seconds : 0.0, percentile(0.5), percentile(0.9), frameMs.empty() ? 0.0 : frameMs.back(),
                static_cast<unsigned long long>(replay.submits), static_cast<double>(replay.deltaBytes) / (1024.0 * 1024.0), static_cast<unsigned long long>(replay.paceTimeouts),
                static_cast<unsigned long long>(replay.staleRegistrations), std::chrono::duration<double>(loaded - loadStart).count());
    std::fflush(stdout);
    return 0;
}

}

int main(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--no-pace") options.pace = false;
        else if (argument == "--strict") options.strict = true;
        else if (argument == "--dump" && index + 1 < argc) options.dumps.push_back(parseDump(argv[++index]));
        else if (!argument.starts_with("-") && options.directory.empty()) options.directory = argument;
        else return usage();
    }
    if (options.directory.empty()) return usage();
#ifdef _WIN32
    _putenv_s("APS5_FRAME_CAPTURE", "");
#else
    unsetenv("APS5_FRAME_CAPTURE");
#endif
    try {
        const auto code = run(options);
        AgcDriverShutdown_nid_postfix();
        return code;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_frame_replay: %s\n", error.what());
        return 2;
    }
}
