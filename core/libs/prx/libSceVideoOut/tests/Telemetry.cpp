#include "prx/libSceVideoOut/include/DisplayWindow.hpp"
#include "prx/libSceVideoOut/include/Telemetry.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <string>
#include <vector>

#undef main

namespace {

void check(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::vector<std::string> readLines(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) lines.push_back(line);
    return lines;
}

long long field(const std::string& line, const std::string& name) {
    const auto position = line.find("\"" + name + "\":");
    check(position != std::string::npos, "missing field " + name + " in " + line);
    return std::stoll(line.substr(position + name.size() + 3));
}

std::filesystem::path freshDirectory(const char* name) {
    auto directory = std::filesystem::temp_directory_path() / (std::string("aps5-videoout-telemetry-") + name + "-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

void padChangeIgnoresSensors() {
    VideoOutTelemetry::PadChangeFilter filter;
    PadInputState state;
    check(!filter.Changed(state), "idle state must not be a change");
    state.hasMotion = true;
    state.accel = {1.0f, 2.0f, 3.0f};
    state.gyro = {0.5f, 0.5f, 0.5f};
    state.touch[0] = {true, 100, 200, 1};
    state.touchLeft = true;
    check(!filter.Changed(state), "motion and touch must not be a change");
    state.buttons = 0x4000;
    check(filter.Changed(state), "a button must be a change");
    check(filter.Changed(state), "an uncommitted change must stay a change");
    filter.Commit(state);
    check(!filter.Changed(state), "a repeated state must not be a change");
    state.sticks[2] = 200;
    check(filter.Changed(state), "a stick must be a change");
    filter.Commit(state);
    state.analogButtonsR2 = 10;
    check(filter.Changed(state), "a trigger must be a change");
}

void padChangeSurvivesFullQueue() {
    const auto directory = freshDirectory("fullqueue");
    {
        VideoOutTelemetry::Writer writer(directory.string(), 1, "test");
        VideoOutTelemetry::PadChangeFilter filter;
        PadInputState state;
        state.buttons = 0x4000;
        check(!VideoOutTelemetry::EmitPadIfChanged(writer, filter, 5, state), "a full queue must refuse the pad line");
        check(filter.Changed(state), "a refused pad change must stay pending");
        check(writer.Dropped() == 1, "the refused pad line must be counted");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        check(VideoOutTelemetry::EmitPadIfChanged(writer, filter, 6, state), "the retry must succeed once the queue drains");
        check(!filter.Changed(state), "a written pad change must be committed");
    }
    const auto lines = readLines(directory / "videoout.jsonl");
    bool found = false;
    for (const auto& line : lines) found = found || (line.find("\"ev\":\"pad\"") != std::string::npos && field(line, "n") == 6 && field(line, "buttons") == 0x4000);
    check(found, "the retried pad line must be in the file");
    bool dropsLine = false;
    for (const auto& line : lines) dropsLine = dropsLine || (line.find("\"ev\":\"drops\"") != std::string::npos && field(line, "dropped") == 1);
    check(dropsLine, "the writer must report the refused line in a drops line");
    std::filesystem::remove_all(directory);
}

void noEnvironmentOpensNothing() {
    const auto directory = freshDirectory("noenv");
    check(VideoOutTelemetry::OpenFromEnvironment(nullptr, "test") == nullptr, "unset variable must not open a writer");
    check(VideoOutTelemetry::OpenFromEnvironment("", "test") == nullptr, "empty variable must not open a writer");
    check(std::filesystem::is_empty(directory), "no file may exist without the variable");
    std::filesystem::remove_all(directory);
}

void queueOverflowIsCounted() {
    VideoOutTelemetry::BoundedLineQueue queue(2);
    check(queue.TryPush("a"), "first line must fit");
    check(queue.TryPush("b"), "second line must fit");
    check(!queue.TryPush("c"), "third line must overflow");
    check(!queue.TryPush("d"), "fourth line must overflow");
    check(queue.Dropped() == 2, "two overflows must be counted");
    check(queue.Drain().size() == 2, "the stored lines must survive");
    check(queue.TryPush("e"), "a drained queue must accept lines");
}

void contractPadAndPresentLines() {
    const auto directory = freshDirectory("contract");
    {
        auto writer = VideoOutTelemetry::OpenFromEnvironment(directory.string().c_str(), "abc123");
        check(writer != nullptr, "variable must open a writer");
        PadInputState state;
        state.buttons = 0x4000;
        writer->EmitPad(7, state);
        writer->EmitPresent(30, 59.94);
    }
    const auto lines = readLines(directory / "videoout.jsonl");
    check(lines.size() == 3, "expected contract, pad and present, got " + std::to_string(lines.size()));
    check(lines[0].find("\"ev\":\"contract\"") != std::string::npos, "contract must be first");
    check(lines[0].find("\"contract\":\"v1-pad\"") != std::string::npos, "contract must name the level");
    check(lines[0].find("\"commit\":\"abc123\"") != std::string::npos, "contract must carry the commit");
    check(lines[0].find("\"prx\":\"libSceVideoOut\"") != std::string::npos, "contract must name the prx");
    check(field(lines[0], "pid") > 0, "contract must carry the pid");
    check(lines[1].find("\"ev\":\"pad\"") != std::string::npos && field(lines[1], "n") == 7 && field(lines[1], "buttons") == 0x4000, "pad line");
    check(lines[1].find("\"sticks\":[128,128,128,128]") != std::string::npos, "pad sticks");
    check(lines[2].find("\"ev\":\"present\"") != std::string::npos && field(lines[2], "n") == 30, "present line");
    std::filesystem::remove_all(directory);
}

void gatePresents(const std::string& mode) {
    const auto directory = freshDirectory(mode.c_str());
    const bool enabled = VideoOutTelemetry::GateRunEnabled();
    AgcDriver::PresentCounters counters{};
    AgcDriverPresentCounters_nid_postfix(&counters);
    check(enabled == (mode != "GateRunOffKeepsCadence"), "gate environment mismatch");
    {
        VideoOutTelemetry::Writer writer(directory.string(), 4096, "test");
        for (std::uint64_t frame = 1; frame <= 65; ++frame) {
            VideoOutTelemetry::ObservePresent(&writer, frame, 7.0, 1000000000 + frame * 140000000);
        }
        check(writer.Dropped() == 0, "gate lost present events");
    }
    const auto lines = readLines(directory / "videoout.jsonl");
    std::uint64_t presents = 0;
    long long last = 0;
    for (const auto& line : lines) {
        if (line.find("\"ev\":\"present\"") == std::string::npos) continue;
        ++presents;
        const auto n = field(line, "n");
        const auto timestamp = field(line, "t_ns");
        check(timestamp > last, "present timestamps must increase");
        if (enabled) check(timestamp == 1000000000 + n * 140000000, "gate timestamp must be the observation timestamp");
        if (enabled) {
            check(field(line, "attempts") == static_cast<long long>(counters.attempts), "driver attempts were not exported");
            check(field(line, "presented") == static_cast<long long>(counters.presented), "driver presents were not exported");
            check(field(line, "acquire_dropped") == static_cast<long long>(counters.acquireDropped), "driver acquire drops were not exported");
            check(field(line, "out_of_date") == static_cast<long long>(counters.outOfDate), "driver out-of-date drops were not exported");
        } else {
            check(line.find("\"presented\"") == std::string::npos && line.find("\"attempts\"") == std::string::npos &&
                line.find("\"acquire_dropped\"") == std::string::npos && line.find("\"out_of_date\"") == std::string::npos, "gate-off fields changed");
        }
        check(line.find("\"fps\":7.00") != std::string::npos, "present fields changed");
        if (enabled) check(n == static_cast<long long>(presents), "missing full-rate frame");
        else check(n == (presents == 1 ? 1 : (presents - 1) * 30), "old cadence changed");
        last = timestamp;
    }
    check(presents == (enabled ? 65 : 3), "incorrect present cadence");
    if (mode == "GateRunEmitsPresentCounters") {
        const auto counterDirectory = directory / "counters";
        std::filesystem::create_directory(counterDirectory);
        {
            VideoOutTelemetry::Writer writer(counterDirectory.string(), 4096, "test");
            writer.EmitPresent(9, 7.0, 123456789, AgcDriver::PresentCounters{100, 97, 2, 1});
            writer.EmitPresent(10, 7.0, 223456789, AgcDriver::PresentCounters{101, 97, 3, 1});
        }
        const auto counterLines = readLines(counterDirectory / "videoout.jsonl");
        check(counterLines.size() == 3, "missing synthetic counter events");
        for (std::size_t i = 1; i < counterLines.size(); ++i) {
            check(field(counterLines[i], "attempts") == 99 + static_cast<long long>(i) && field(counterLines[i], "presented") == 97 &&
                field(counterLines[i], "acquire_dropped") == 1 + static_cast<long long>(i) && field(counterLines[i], "out_of_date") == 1,
                "cumulative counter fields differ from the snapshot");
        }
    }
    std::filesystem::remove_all(directory);
}

void runtimeDelayShared() {
    using AgcDriver::ArchB::Switches;
    const auto directory = freshDirectory("runtime-delay");
    const auto path = directory / "control.txt";
#ifdef _WIN32
    _putenv_s("APS5_ARCHB_CONTROL_FILE", path.string().c_str());
#else
    setenv("APS5_ARCHB_CONTROL_FILE", path.c_str(), 1);
#endif
    Switches::Initialize();
    std::ofstream(path) << "1 GATE_DELAY_US=7000\n";
    Switches::PollControl();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) VideoOutTelemetry::ObservePresent(nullptr, i + 1, 0, 1);
    check(std::chrono::steady_clock::now() - start >= std::chrono::microseconds(21000), "VideoOut did not discover the driver's shared delay");
    std::ofstream(path) << "2 GATE_DELAY_US=0\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    Switches::PollControl();
    const auto offStart = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) VideoOutTelemetry::ObservePresent(nullptr, i + 4, 0, 1);
    check(std::chrono::steady_clock::now() - offStart < std::chrono::milliseconds(500), "VideoOut cached the toggled delay");
    std::filesystem::remove_all(directory);
}

int frameNumberMatchesCaption() {
    const auto directory = freshDirectory("caption");
    std::filesystem::create_directories(directory / "app0" / "sce_sys");
    std::ofstream(directory / "app0" / "sce_sys" / "param.json") << R"({"titleId":"PPSA00000","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Telemetry Test"}}})";
    std::filesystem::current_path(directory);
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        std::printf("skipped, no display or Vulkan device: SDL video driver: %s\n", SDL_GetError());
        return 77;
    }
    DisplayWindow window;
    try {
        window.Ensure(640, 360);
    } catch (const std::exception& error) {
        std::printf("skipped, no display or Vulkan device: %s\n", error.what());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return 77;
    }
    check(window.FrameNumber() == 0, "no frame before the first title update");
    for (std::uint64_t expected = 1; expected <= 65; ++expected) {
        window.UpdateTitle();
        check(window.FrameNumber() == expected, "FrameNumber must count title updates");
        const std::string title = SDL_GetWindowTitle(window.Handle());
        const auto open = title.rfind('(');
        check(open != std::string::npos && std::stoull(title.substr(open + 1)) == expected, "caption counter differs from FrameNumber: " + title);
    }
    window.Destroy();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    std::filesystem::current_path(directory.parent_path());
    std::filesystem::remove_all(directory);
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "caption") {
            const int result = frameNumberMatchesCaption();
            if (result == 0) std::printf("VideoOut caption counter test passed\n");
            return result;
        }
        if (argc > 1) {
            if (std::string(argv[1]) == "RuntimeDelayShared") runtimeDelayShared();
            else gatePresents(argv[1]);
            std::printf("%s passed\n", argv[1]);
            return 0;
        }
        padChangeIgnoresSensors();
        padChangeSurvivesFullQueue();
        noEnvironmentOpensNothing();
        queueOverflowIsCounted();
        contractPadAndPresentLines();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    std::printf("VideoOut telemetry tests passed\n");
    return 0;
}
