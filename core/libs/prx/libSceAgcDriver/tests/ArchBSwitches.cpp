#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceVideoOut/include/Telemetry.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

thread_local std::size_t allocations = 0;
thread_local bool countAllocations = false;

void check(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

void setEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value == nullptr ? "" : value);
#else
    if (value == nullptr) unsetenv(name);
    else setenv(name, value, 1);
#endif
}

void writerCost() {
    const auto directory = std::filesystem::temp_directory_path() / ("archb-writer-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    {
        VideoOutTelemetry::Writer writer(directory.string(), 1, "test");
        VideoOutTelemetry::ObservePresent(nullptr, 1, 0, 1);
        allocations = 0;
        countAllocations = true;
        for (int i = 0; i < 1000; ++i) writer.EmitPresent(1, 0, 1000);
        countAllocations = false;
        const auto original = allocations;
        allocations = 0;
        const auto start = std::chrono::steady_clock::now();
        countAllocations = true;
        for (int i = 0; i < 1000; ++i) VideoOutTelemetry::ObservePresent(&writer, 1, 0, 1000);
        countAllocations = false;
        check(allocations <= original + 2, "gate adds allocations beyond the existing writer path");
        check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500), "full telemetry queue blocks presentation");
        check(writer.Dropped() > 0, "full queue must record drops");
    }
    std::filesystem::remove_all(directory);
}

void toggleCase(const std::string& name) {
    using AgcDriver::ArchB::Feature;
    using AgcDriver::ArchB::Switches;
    const auto path = std::filesystem::temp_directory_path() / ("archb-control-" + std::to_string(getpid()) + ".txt");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove(path); }
    } cleanup{path};
    setEnvironment("APS5_ARCHB_GATE_RUN", "1");
    setEnvironment("APS5_ARCHB_GATE_NOOP", "0");
    setEnvironment("APS5_ARCHB_GATE_DELAY_US", "0");
    setEnvironment("APS5_ARCHB_TEST_RUNTIME_DEPENDENT", "0");
    setEnvironment("APS5_ARCHB_CONTROL_FILE", name == "NoFileNoCost" ? nullptr : path.string().c_str());
    Switches::Initialize();
    const auto write = [&](const std::string& line, bool poll = true) {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << line << '\n';
        if (poll) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            Switches::PollControl();
        }
    };
    if (name == "SeqMonotonic") {
        write("2 GATE_NOOP=1 GATE_DELAY_US=7000");
        check(Switches::Enabled(Feature::GateNoop) && Switches::GateDelayMicros() == 7000, "initial toggle");
        const auto start = std::chrono::steady_clock::now();
        VideoOutTelemetry::ObservePresent(nullptr, 1, 0, 1);
        check(std::chrono::steady_clock::now() - start >= std::chrono::microseconds(7000), "VideoOut did not read the shared delay");
        write("2 GATE_NOOP=0 GATE_DELAY_US=0");
        write("1 GATE_NOOP=0 GATE_DELAY_US=0");
        check(Switches::Enabled(Feature::GateNoop) && Switches::GateDelayMicros() == 7000, "old sequence applied");
        write("3 GATE_NOOP=0 GATE_DELAY_US=0");
        check(!Switches::Enabled(Feature::GateNoop) && Switches::GateDelayMicros() == 0, "new sequence ignored");
        const auto polls = Switches::ControlPolls();
        Switches::PollControl();
        check(Switches::ControlPolls() == polls, "poll rate exceeded");
    } else if (name == "RefusesStartupOnly" || name == "RefusesUnknown") {
        write(name == "RefusesStartupOnly" ? "1 GATE_NOOP=1 GATE_RUN=0" : "1 GATE_NOOP=1 UNKNOWN=1");
        check(!Switches::Enabled(Feature::GateNoop) && Switches::Enabled(Feature::GateRun), "refused line partially applied");
        for (const auto* invalid : {"2", "-1", "true", "01", ""}) {
            write("2 GATE_NOOP=" + std::string(invalid) + " GATE_DELAY_US=1");
            check(!Switches::Enabled(Feature::GateNoop) && Switches::GateDelayMicros() == 0, "invalid boolean applied");
        }
        for (const auto* invalid : {"-1", "1.0", "9223372036854775807", "18446744073709551616"}) {
            write("2 GATE_NOOP=1 GATE_DELAY_US=" + std::string(invalid));
            check(!Switches::Enabled(Feature::GateNoop), "invalid delay partially applied");
        }
        write("2 GATE_NOOP=1\n3 GATE_NOOP=0");
        check(!Switches::Enabled(Feature::GateNoop), "multiple lines applied");
        write("2 GATE_NOOP=1 GATE_NOOP=0");
        check(!Switches::Enabled(Feature::GateNoop), "duplicate token applied");
        write("1 GATE_NOOP=1");
        check(Switches::Enabled(Feature::GateNoop), "refused sequence was consumed");
    } else if (name == "PrerequisiteCascades") {
        write("1 TEST_RUNTIME_DEPENDENT=1");
        check(!Switches::Enabled(Feature::TestRuntimeDependent), "prerequisite-disabled switch applied");
        write("1 GATE_NOOP=1 TEST_RUNTIME_DEPENDENT=1");
        check(Switches::Enabled(Feature::TestRuntimeDependent), "runtime dependency enable");
        write("2 GATE_NOOP=0");
        check(!Switches::Enabled(Feature::GateNoop) && !Switches::Enabled(Feature::TestRuntimeDependent), "dependent did not turn off");
    } else if (name == "NoFileNoCost") {
        for (int i = 0; i < 10000; ++i) Switches::PollControl();
        check(Switches::ControlPolls() == 0, "no-file path performed a stat");
        check(!Switches::Enabled(Feature::GateNoop), "no-file path changed switches");
    } else if (name == "AppliesAtBoundaryOnly") {
        write("1 GATE_NOOP=1 GATE_DELAY_US=7000", false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        VideoOutTelemetry::ObservePresent(nullptr, 1, 0, 1);
        check(!Switches::Enabled(Feature::GateNoop) && Switches::GateDelayMicros() == 0, "toggle applied away from driver boundary");
        Switches::PollControl();
        check(Switches::Enabled(Feature::GateNoop) && Switches::GateDelayMicros() == 7000, "boundary did not apply toggle");
    } else {
        throw std::runtime_error("unknown toggle case");
    }
}

}

void* operator new(std::size_t size) {
    if (countAllocations) ++allocations;
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main(int argc, char** argv) {
    using AgcDriver::ArchB::Feature;
    using AgcDriver::ArchB::Switches;
    try {
        if (argc == 3 && std::string(argv[1]) == "--toggle") {
            toggleCase(argv[2]);
            std::printf("%s passed\n", argv[2]);
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--probe") {
            const std::string probe(argv[2]);
            if (probe == "video") {
                std::printf("enabled=%d\n", VideoOutTelemetry::GateRunEnabled());
            } else if (probe == "writer") {
                writerCost();
            } else if (probe == "delay") {
                const auto start = std::chrono::steady_clock::now();
                for (int i = 0; i < 3; ++i) VideoOutTelemetry::ObservePresent(nullptr, i + 1, 0, 1);
                const auto elapsed = std::chrono::steady_clock::now() - start;
                if (VideoOutTelemetry::GateRunEnabled()) check(elapsed >= std::chrono::microseconds(21000), "delay hook did not sleep after each present");
            } else {
                Switches::Initialize();
                Switches::Initialize();
                std::printf("enabled=%d\n", Switches::Enabled(Feature::GateRun));
                for (const auto& entry : Switches::Table()) check(Switches::Describe().find(std::string(entry.name) + "=") != std::string::npos, "Describe omits a table entry");
            }
            return 0;
        }
        const auto output = std::filesystem::temp_directory_path() / ("archb-switches-" + std::to_string(getpid()) + ".txt");
        const auto run = [&](const char* parser, const char* value) {
            setEnvironment("APS5_ARCHB_GATE_RUN", value);
            setEnvironment("APS5_ARCHB_TEST_DEPENDENT", nullptr);
            std::string command = "\"" + std::filesystem::absolute(argv[0]).string() + "\" --probe " + parser + " > \"" + output.string() + "\" 2>&1";
#ifdef _WIN32
            command = "\"" + command + "\"";
#endif
            const int result = std::system(command.c_str());
            std::ifstream stream(output);
            std::ostringstream text;
            text << stream.rdbuf();
            return std::pair(result, text.str());
        };
        setEnvironment("APS5_ARCHB_GATE_DELAY_US", nullptr);
        auto result = run("driver", nullptr);
        check(result.first == 0 && result.second.find("GATE_RUN=0(default)") != std::string::npos, "UnsetUsesDefault");
        check(result.second.find("[archb] switches") == result.second.rfind("[archb] switches"), "Initialize must print exactly one switches line");
        std::printf("UnsetUsesDefault passed\n");
        for (const auto* value : {"0", "1"}) {
            result = run("driver", value);
            check(result.first == 0 && result.second.find("GATE_RUN=" + std::string(value) + "(env)") != std::string::npos, "ZeroAndOne");
            const auto video = run("video", value);
            check(video.first == 0 && video.second.find("enabled=" + std::string(value)) != std::string::npos, "VideoOutParserAgrees");
        }
        const auto videoDefault = run("video", nullptr);
        check(videoDefault.first == 0 && videoDefault.second.find("enabled=0") != std::string::npos, "VideoOut default disagrees");
        std::printf("ZeroAndOne passed\nVideoOutParserAgrees passed\n");
        for (const auto* invalid : {"2", "true", "-1", "01", " 1"}) {
            for (const auto* parser : {"driver", "video"}) {
                result = run(parser, invalid);
                check(result.first != 0 && result.second.find("[archb] invalid GATE_RUN=" + std::string(invalid)) != std::string::npos, "InvalidAborts");
            }
        }
        std::printf("InvalidAborts passed\n");
        result = run("driver", "1");
        check(result.first == 0 && result.second.find("TEST_DEPENDENT=1(default)") != std::string::npos, "DescribeListsAll");
        result = run("driver", "0");
        check(result.first == 0 && result.second.find("[archb] TEST_DEPENDENT off: requires GATE_RUN") != std::string::npos && result.second.find("TEST_DEPENDENT=0(default)") != std::string::npos, "PrerequisiteOffDisablesDependent");
        std::printf("DescribeListsAll passed\nPrerequisiteOffDisablesDependent passed\n");
        check(run("writer", "1").first == 0, "full-rate writer cost");
        setEnvironment("APS5_ARCHB_GATE_DELAY_US", "7000");
        check(run("delay", "1").first == 0, "delay sensitivity hook");
        setEnvironment("ARCHB_TEST_LEGACY_DRIVER", "1");
        check(run("delay", "1").first == 0, "C01 parent delay fallback");
        setEnvironment("APS5_ARCHB_GATE_DELAY_US", "invalid");
        check(run("delay", "1").first != 0, "C01 parent fallback accepted an invalid delay");
        setEnvironment("ARCHB_TEST_LEGACY_DRIVER", nullptr);
        setEnvironment("APS5_ARCHB_GATE_DELAY_US", "invalid");
        check(run("delay", "0").first == 0, "disabled gate honours delay hook");
        std::filesystem::remove(output);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
