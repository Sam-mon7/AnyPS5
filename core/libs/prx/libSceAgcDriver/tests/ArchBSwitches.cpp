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
        setEnvironment("APS5_ARCHB_GATE_DELAY_US", "invalid");
        check(run("delay", "0").first == 0, "disabled gate honours delay hook");
        std::filesystem::remove(output);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
