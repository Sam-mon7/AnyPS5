#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceVideoOut/include/Telemetry.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceAgcDriver/Execution/include/Screenshot.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

extern "C" void AgcDriverPresentCounters_nid_postfix(AgcDriver::PresentCounters* counters) {
    *counters = {};
}

namespace {

thread_local std::size_t allocations = 0;
thread_local bool countAllocations = false;
std::size_t screenshotRequests = 0;

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
    const bool imageTable = name == "ImageTableSnapshot";
    const bool bitScanKeys = name == "ImageTableBitScanKeys";
    const bool bookkeeping = name == "WriteTrackingBookkeeping";
    const bool pipeQ0 = name == "PipeQ0Committer";
    if (name == "VqMemo" || imageTable || bitScanKeys) setEnvironment(bitScanKeys ? "APS5_ARCHB_FIX_IMAGE_TABLE_BITSCAN_KEYS" : imageTable ? "APS5_ARCHB_FIX_IMAGE_TABLE_SNAPSHOT_REUSE" : "APS5_ARCHB_FIX_VQ_MEMO", nullptr);
    if (bookkeeping) setEnvironment("APS5_ARCHB_FIX_WRITE_TRACKING_BOOKKEEPING", nullptr);
    if (pipeQ0) setEnvironment("APS5_ARCHB_PIPE_Q0_COMMITTER", nullptr);
    // A2 cuts that require PIPE_Q0_COMMITTER: the same contract.
    struct PipeCut {
        Feature feature;
        const char* token;
    };
    const auto pipeCut = name == "PipeKeepIndirectRegs" ? std::optional<PipeCut>{{Feature::PipeKeepIndirectRegs, "PIPE_KEEP_INDIRECT_REGS"}} : name == "PipeIndirectGpuArgs" ? std::optional<PipeCut>{{Feature::PipeIndirectGpuArgs, "PIPE_INDIRECT_GPU_ARGS"}} : name == "PipeNarrowDrawWrites" ? std::optional<PipeCut>{{Feature::PipeNarrowDrawWrites, "PIPE_NARROW_DRAW_WRITES"}} : name == "PipeNarrowSlicedDepth" ? std::optional<PipeCut>{{Feature::PipeNarrowSlicedDepth, "PIPE_NARROW_SLICED_DEPTH"}} : name == "PipeNarrowDispatchWrites" ? std::optional<PipeCut>{{Feature::PipeNarrowDispatchWrites, "PIPE_NARROW_DISPATCH_WRITES"}} : name == "PipeKeepOwnLabelWaits" ? std::optional<PipeCut>{{Feature::PipeKeepOwnLabelWaits, "PIPE_KEEP_OWN_LABEL_WAITS"}} : name == "FixDrawSetPool" ? std::optional<PipeCut>{{Feature::FixDrawSetPool, "FIX_DRAW_SET_POOL"}} : name == "PipeDeepQueue" ? std::optional<PipeCut>{{Feature::PipeDeepQueue, "PIPE_DEEP_QUEUE"}} : std::nullopt;
    const bool keepRegisters = pipeCut.has_value();
    if (keepRegisters) {
        setEnvironment("APS5_ARCHB_PIPE_Q0_COMMITTER", nullptr);
        // From the environment without PIPE_Q0_COMMITTER: refused at startup (prerequisite).
        setEnvironment((std::string("APS5_ARCHB_") + pipeCut->token).c_str(), "1");
    }
    if (name == "AllocChurn") setEnvironment("APS5_ARCHB_FIX_ALLOC_CHURN", nullptr);
    const bool genTable = name == "GenTable";
    // Switches that require GEN_TABLE (slice S1 GEN_SHADOW, S2 GEN_LOCKFREE): same contract.
    const struct GenDependent {
        const char* name;
        Feature feature;
        const char* token;
    } genDependents[] = {{"GenShadow", Feature::GenShadow, "GEN_SHADOW"}, {"GenLockfree", Feature::GenLockfree, "GEN_LOCKFREE"}, {"GenDrawValidation", Feature::GenDrawValidation, "GEN_DRAW_VALIDATION"}};
    const GenDependent* genDependent = nullptr;
    for (const auto& dependent : genDependents) if (name == dependent.name) genDependent = &dependent;
    const bool genShadow = genDependent != nullptr;
    if (genTable || genShadow) {
        setEnvironment("APS5_ARCHB_GEN_TABLE", nullptr);
        // The dependent from the environment without GEN_TABLE: refused at startup (prerequisite).
        for (const auto& dependent : genDependents) setEnvironment((std::string("APS5_ARCHB_") + dependent.token).c_str(), genDependent == &dependent ? "1" : nullptr);
    }
    Switches::Initialize();
    const auto write = [&](const std::string& line, bool poll = true) {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << line << '\n';
        if (poll) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            Switches::PollControl();
        }
    };
    if (name == "VqMemo" || imageTable || bitScanKeys || bookkeeping || pipeQ0) {
        const auto feature = pipeQ0 ? Feature::PipeQ0Committer : bookkeeping ? Feature::FixWriteTrackingBookkeeping : bitScanKeys ? Feature::FixImageTableBitScanKeys : imageTable ? Feature::FixImageTableSnapshotReuse : Feature::FixVqMemo;
        const std::string token = pipeQ0 ? "PIPE_Q0_COMMITTER" : bookkeeping ? "FIX_WRITE_TRACKING_BOOKKEEPING" : bitScanKeys ? "FIX_IMAGE_TABLE_BITSCAN_KEYS" : imageTable ? "FIX_IMAGE_TABLE_SNAPSHOT_REUSE" : "FIX_VQ_MEMO";
        bool listed = false;
        for (const auto& entry : Switches::Table()) {
            if (entry.feature != feature) continue;
            listed = true;
            check(token == entry.name && !entry.defaultOn && entry.runtime && entry.prerequisites.empty(), "runtime fix switch contract");
        }
        check(listed, "runtime fix switch is listed");
        check(!Switches::Enabled(feature), "runtime fix must default off");
        write("1 " + token + "=1");
        const auto selected = Switches::Enabled(feature);
        check(selected, "runtime fix did not turn on");
        write("2 " + token + "=0");
        check(selected && !Switches::Enabled(feature), "runtime fix did not preserve the selected path across a toggle");
        write("3 " + token + "=1");
        check(Switches::Enabled(feature), "runtime fix did not turn back on");
    } else if (genTable || genShadow) {
        // Write generations: GEN_TABLE runtime and default off; each dependent (GEN_SHADOW, GEN_LOCKFREE)
        // runtime, default off, and requires GEN_TABLE (named alone it is refused, and it drops when
        // GEN_TABLE does).
        const auto& dependent = genDependent != nullptr ? *genDependent : genDependents[0];
        const std::string token = dependent.token;
        int listed = 0;
        for (const auto& entry : Switches::Table()) {
            if (entry.feature == Feature::GenTable) {
                ++listed;
                check(std::string(entry.name) == "GEN_TABLE" && !entry.defaultOn && entry.runtime && entry.prerequisites.empty(), "GEN_TABLE switch contract");
            }
            for (const auto& other : genDependents) {
                if (entry.feature != other.feature) continue;
                ++listed;
                check(std::string(entry.name) == other.token && !entry.defaultOn && entry.runtime && entry.prerequisites.size() == 1 && entry.prerequisites[0] == Feature::GenTable, std::string(other.token) + " switch contract");
            }
        }
        check(listed == 1 + static_cast<int>(std::size(genDependents)), "write-generation switches are listed");
        check(!Switches::Enabled(Feature::GenTable) && !Switches::Enabled(dependent.feature), "write-generation switches must start off (a dependent without GEN_TABLE refused)");
        write("1 " + token + "=1");
        check(!Switches::Enabled(dependent.feature), token + " turned on without GEN_TABLE");
        write("2 GEN_TABLE=1 " + token + "=1");
        check(Switches::Enabled(Feature::GenTable) && Switches::Enabled(dependent.feature), "GEN_TABLE and " + token + " did not turn on together");
        write("3 GEN_TABLE=0");
        check(!Switches::Enabled(Feature::GenTable) && !Switches::Enabled(dependent.feature), token + " stayed on after GEN_TABLE turned off");
        write("4 GEN_TABLE=1");
        check(Switches::Enabled(Feature::GenTable) && !Switches::Enabled(dependent.feature), "GEN_TABLE alone did not turn on alone");
    } else if (keepRegisters) {
        // A2 cut: runtime, default off, requires PIPE_Q0_COMMITTER (named alone it is refused, and it
        // drops when the committer switch does).
        const std::string token = pipeCut->token;
        const auto feature = pipeCut->feature;
        bool listed = false;
        for (const auto& entry : Switches::Table()) {
            if (entry.feature != feature) continue;
            listed = true;
            check(std::string(entry.name) == token && !entry.defaultOn && entry.runtime && entry.prerequisites.size() == 1 && entry.prerequisites[0] == Feature::PipeQ0Committer, token + " switch contract");
        }
        check(listed, token + " is listed");
        check(!Switches::Enabled(feature), token + " started on without PIPE_Q0_COMMITTER");
        write("1 " + token + "=1");
        check(!Switches::Enabled(feature), token + " turned on without PIPE_Q0_COMMITTER");
        write("2 PIPE_Q0_COMMITTER=1 " + token + "=1");
        check(Switches::Enabled(Feature::PipeQ0Committer) && Switches::Enabled(feature), "the committer and " + token + " did not turn on together");
        write("3 PIPE_Q0_COMMITTER=0");
        check(!Switches::Enabled(Feature::PipeQ0Committer) && !Switches::Enabled(feature), token + " stayed on after the committer switch turned off");
    } else if (name == "AllocChurn") {
        using AgcDriver::ArchB::AllocChurnActive;
        using AgcDriver::ArchB::AllocChurnScope;
        bool listed = false;
        for (const auto& entry : Switches::Table()) {
            if (entry.feature != Feature::FixAllocChurn) continue;
            listed = true;
            check(std::string(entry.name) == "FIX_ALLOC_CHURN" && !entry.defaultOn && entry.runtime && entry.prerequisites.empty(), "allocation churn switch contract");
        }
        check(listed, "allocation churn switch is listed");
        check(!Switches::Enabled(Feature::FixAllocChurn), "allocation churn must default off");
        write("1 FIX_ALLOC_CHURN=1");
        check(Switches::Enabled(Feature::FixAllocChurn), "allocation churn did not turn on");
        // The sites read the value sampled into the thread's scope, not the live switch.
        check(!AllocChurnActive(), "allocation churn is active outside a draw scope");
        {
            const AllocChurnScope draw(Switches::Enabled(Feature::FixAllocChurn));
            check(AllocChurnActive(), "a draw scope did not take the sampled value");
            write("2 FIX_ALLOC_CHURN=0");
            check(!Switches::Enabled(Feature::FixAllocChurn) && AllocChurnActive(), "a runtime toggle changed the path inside a draw");
            bool otherThread = true;
            std::thread([&] { otherThread = AllocChurnActive(); }).join();
            check(!otherThread, "a draw scope leaked to another thread");
            {
                const AllocChurnScope nested(false);
                check(!AllocChurnActive(), "a nested scope did not take its value");
            }
            check(AllocChurnActive(), "a nested scope did not restore the outer value");
        }
        check(!AllocChurnActive(), "a draw scope did not restore the value outside it");
        {
            const AllocChurnScope draw(Switches::Enabled(Feature::FixAllocChurn));
            check(!AllocChurnActive(), "the next draw did not sample the toggled switch");
        }
        write("3 FIX_ALLOC_CHURN=1");
        check(Switches::Enabled(Feature::FixAllocChurn), "allocation churn did not turn back on");
    } else if (name == "SeqMonotonic") {
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
    } else if (name == "ScreenshotToken") {
        write("1 SCREENSHOT=1", false);
        check(screenshotRequests == 0, "screenshot requested away from boundary");
        Switches::PollControl();
        check(screenshotRequests == 1, "standalone screenshot token did not request once");
        Switches::PollControl();
        write("1 SCREENSHOT=1");
        write("0 SCREENSHOT=1");
        check(screenshotRequests == 1, "stale screenshot token requested again");
        for (const auto* invalid : {"0", "2", "-1", "true", "01", ""}) {
            write("2 GATE_NOOP=1 SCREENSHOT=" + std::string(invalid));
        }
        for (const auto* tokens : {"SCREENSHOT=1 UNKNOWN=1", "SCREENSHOT=1 GATE_RUN=0", "SCREENSHOT=1 SCREENSHOT=1",
                                   "SCREENSHOT=1 TEST_RUNTIME_DEPENDENT=1", "SCREENSHOT=1\n3 SCREENSHOT=1"}) {
            write("2 " + std::string(tokens));
        }
        check(screenshotRequests == 1 && !Switches::Enabled(Feature::GateNoop), "refused screenshot line had side effects");
        write("2 GATE_NOOP=1 SCREENSHOT=1");
        check(screenshotRequests == 2 && Switches::Enabled(Feature::GateNoop), "combined screenshot line did not apply once");
        write("3 GATE_NOOP=0");
        check(screenshotRequests == 2, "ordinary toggle requested screenshot");
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

void AgcDriver::Screenshot::Request() {
    ++screenshotRequests;
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
        if (argc == 2 && std::string(argv[1]) == "--ub-default") {
            setEnvironment("APS5_ARCHB_GATE_RUN", nullptr);
            for (const auto& entry : Switches::Table()) {
                if (std::string(entry.name).starts_with("UB_")) setEnvironment(("APS5_ARCHB_" + std::string(entry.name)).c_str(), nullptr);
            }
            std::size_t count = 0;
            for (const auto& entry : Switches::Table()) {
                if (!std::string(entry.name).starts_with("UB_")) continue;
                ++count;
                check(!entry.defaultOn && entry.runtime && entry.prerequisites.empty() && !Switches::Enabled(entry.feature), "UB switch must default off, be runtime and have no prerequisites");
                check(Switches::Describe().find(std::string(entry.name) + "=0(default)(rt)") != std::string::npos, "UB switch missing runtime description");
            }
            check(count == 5, "missing upper-bound switch");
            std::puts("upper-bound switches default off passed");
            return 0;
        }
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
