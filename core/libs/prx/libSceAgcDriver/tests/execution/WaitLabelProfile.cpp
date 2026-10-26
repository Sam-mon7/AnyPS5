#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

using Recorder = AgcDriver::Graphics::Recorder;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}

int main() {
    try {
        constexpr std::uint64_t base = 0x1000;
        Recorder::ProfileStore(base, 16, 0x20, 0x49, Recorder::WaitStage::Deferred);
        if (!std::getenv("APS5_PROFILE_DRAW")) {
            Require(!Recorder::ProfileWriter(base, 4).writer, "disabled profiling kept writer metadata");
            Recorder::ReportWaitLabels(true);
            std::puts("disabled label profiler test passed");
            return 0;
        }
        const auto deferred = Recorder::ProfileWriter(base, 8);
        Require(deferred.writer && deferred.writer->queue == 0x20 && deferred.writer->opcode == 0x49 && deferred.stage == Recorder::WaitStage::Deferred, "deferred writer attribution is wrong");
        Recorder::ProfileStore(base + 4, 4, 0x38, 0x37, Recorder::WaitStage::Cpu);
        const auto left = Recorder::ProfileWriter(base, 4);
        const auto middle = Recorder::ProfileWriter(base + 4, 4);
        const auto right = Recorder::ProfileWriter(base + 8, 8);
        Require(left.writer == deferred.writer && right.writer == deferred.writer, "partial overwrite lost the untouched writer ranges");
        Require(middle.writer && middle.writer->queue == 0x38 && middle.writer->cpu && middle.stage == Recorder::WaitStage::Cpu, "CPU writer attribution is wrong");
        Require(!Recorder::ProfileWriter(base, 8).writer, "a wide wait with different dword writers was attributed to one writer");
        Require(!Recorder::ProfileWriter(base - 4, 4).writer && !Recorder::ProfileWriter(base + 16, 4).writer, "a write range covered a gap");
        Recorder::ProfileStore(std::numeric_limits<std::uint64_t>::max() - 1, 4, 0, 0x37, Recorder::WaitStage::Cpu);
        Require(!Recorder::ProfileWriter(std::numeric_limits<std::uint64_t>::max() - 1, 4).writer, "overflowing writer range was accepted");
        {
            Recorder::WaitProfile sample(0, base + 4, 4);
            sample.started -= std::chrono::milliseconds(2);
            sample.end = Recorder::WaitEnd::Recorder;
            Require(sample.entry.writer == middle.writer && sample.entry.stage == Recorder::WaitStage::Cpu, "wait entry did not preserve its writer snapshot");
        }
        {
            Recorder::WaitProfile sample(0x20, base, 4);
            sample.end = Recorder::WaitEnd::Entry;
        }
        Recorder::ReportWaitLabels(true);
        AgcDriver::ProfileOutput_nid_no_patch().Flush();
        std::puts("label profiler tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
