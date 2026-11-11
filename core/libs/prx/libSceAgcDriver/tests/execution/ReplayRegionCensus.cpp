#include "prx/libSceAgcDriver/Execution/include/Driver/Replay/RegionCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/CommitCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}

int main() {
    using namespace AgcDriver;
    using namespace AgcDriver::DriverDetail;
    using namespace AgcDriver::DriverDetail::Replay;
    try {
        Require(RegionCensus::Enabled(), "the runtime fixture requires the region census switch");
        const char* timingValue = std::getenv("APS5_REPLAY_REGION_CENSUS_TIMING_ONLY");
        const bool timing = timingValue != nullptr && timingValue[0] == '1';
        ArchB::Recorder::Boundary(true, 0, ArchB::Recorder::Clock::now());
        RegionRecognizer recognizer;
        const std::vector<std::uint32_t> commands{0xc0012d00u, 3u, 2u};
        for (std::uint64_t frame = 0; frame < 5; ++frame) {
            const auto report = recognizer.Observe(0, commands);
            auto region = RegionCensus::Begin(0, commands, report, true);
            recognizer.EndFrame();
            std::shared_ptr<RegionCensus::Packet> queued;
            {
                const RegionCensus::RegionScope regionScope(region);
                const RegionCensus::PacketScope packetScope(0, commands);
                RegionCensus::Note(ResolvedField::State, {10});
                queued = RegionCensus::CapturePipe();
            }
            if (frame == 0) {
                const auto pending = ArchB::Recorder::Collect(ArchB::Recorder::Clock::now());
                Require(!pending.counters.contains("rrc_frames"), "a region was published before its committer item finished");
            }
            {
                const RegionCensus::CommitScope commitScope(queued);
                CommitCensus timer(CommitCensus::Kind::Legacy);
                RegionCensus::Note(ResolvedField::Pipeline, {20});
                RegionCensus::Note(ResolvedField::Buffers, {frame == 4 ? 0x2000u : frame % 2 == 0 ? 0x1000u : 0x2000u, 64});
            }
        }
        const auto snapshot = ArchB::Recorder::Collect(ArchB::Recorder::Clock::now());
        const auto count = [&](const char* name) { const auto found = snapshot.counters.find(name); return found == snapshot.counters.end() ? 0.0 : found->second; };
        Require(count("rrc_frames") == 5, "the completed frames were not each published once");
        Require(count("rrc_n2_regions") == (timing ? 0 : 2), "the two-flight region was not attributed to N-2 or a partial record claimed equality");
        Require(count("rrc_n1_regions") == (timing ? 0 : 1), "the immediate repeat was not attributed to N-1 or a partial record claimed equality");
        Require(count("rrc_first_n1_buffers") == (timing ? 0 : 3), "a moved resolved buffer was not the first differing field");
        Require(count("rrc_verify_checks") == 7 && count("rrc_verify_mismatch") == 0, "the independently compared signatures disagreed or were not verified");
        Require(count("rrc_n2_pipe_items") == (timing ? 0 : 2) && count("rrc_n1_pipe_items") == (timing ? 0 : 1), "pipe work was attributed to another frame");
        if (timing) Require(count("rrc_unclassified_regions") == 4, "a partial equal record was not left unclassified");
        RegionRecognizer waits;
        for (std::uint32_t frame = 0; frame < 4; ++frame) {
            const auto value = timing ? frame + 1 : frame % 2 + 1;
            const std::vector<std::uint32_t> words{0xc0053c00u, 0x13u, frame % 2 == 0 ? 0x1000u : 0x2000u, 0u, value, 0xffffffffu, 4u};
            const auto report = waits.Observe(0, words);
            const auto region = RegionCensus::Begin(0, words, report, true);
            waits.EndFrame();
            const RegionCensus::RegionScope regionScope(region);
            const RegionCensus::PacketScope packetScope(0, words);
            Require(Pm4::WaitComparesValue(words, value), "the original label comparison changed");
        }
        const auto observed = ArchB::Recorder::Collect(ArchB::Recorder::Clock::now());
        const auto observedCount = [&](const char* name) { const auto found = observed.counters.find(name); return found == observed.counters.end() ? 0.0 : found->second; };
        if (timing) {
            Require(observedCount("rrc_different_regions") > 0, "a changing wait did not prove the partial region different");
            Require(observedCount("rrc_equal_n1") == 0 && observedCount("rrc_equal_n2") == 0, "timing-only witnesses claimed full equality");
        } else {
            Require(observedCount("rrc_patch_use_label_wait_address") > 0 && observedCount("rrc_patch_use_label_wait_value") > 0, "wait patch words lost their typed address/value roles");
            Require(observedCount("rrc_patch_use_label_compare_input") > 0 && observedCount("rrc_input_equal_n2") > 0, "the already-resolved label comparison input was not compared at N-2");
        }
        if (!timing) {
            for (const bool executed : {true, false}) {
                RegionRecognizer noops;
                for (std::uint32_t frame = 0; frame < 3; ++frame) {
                    const std::vector<std::uint32_t> words{0xc0021000u, 0u, 0u, frame == 0 ? 0u : 1u, 0xc0012d00u, 3u, 2u};
                    const auto report = noops.Observe(0, words);
                    const auto region = RegionCensus::Begin(0, words, report, true);
                    noops.EndFrame();
                    const RegionCensus::RegionScope regionScope(region);
                    if (executed) { const RegionCensus::PacketScope nopScope(0, std::span(words).first(4)); }
                    const RegionCensus::PacketScope drawScope(4, std::span(words).subspan(4));
                }
            }
            const auto noops = ArchB::Recorder::Collect(ArchB::Recorder::Clock::now());
            Require(noops.counters.contains("rrc_patch_use_nop_payload") && noops.counters.contains("rrc_patch_use_not_executed"), "NOP payloads and words skipped by execution were not distinguished");
            Require(noops.counters.contains("rrc_patch_positions_unchanged_word"), "a known patch position disappeared when its current word stopped changing");
            RegionRecognizer converted;
            const std::vector<std::uint32_t> words{0xc0031500u, 1u, 1u, 1u, 0u};
            for (std::uint32_t frame = 0; frame < 2; ++frame) {
                const auto report = converted.Observe(0, words);
                const auto region = RegionCensus::Begin(0, words, report, true);
                converted.EndFrame();
                const RegionCensus::RegionScope regionScope(region);
                const RegionCensus::PacketScope packetScope(0, words);
                const auto packet = RegionCensus::CapturePipe();
                std::thread commit([packet] {
                    const RegionCensus::CommitScope scope(packet);
                    RegionCensus::SetKind(ResolvedKind::Fill);
                    RegionCensus::Note(ResolvedField::Arguments, {0x1000u, 64u});
                });
                commit.join();
            }
            const auto convertedCounts = ArchB::Recorder::Collect(ArchB::Recorder::Clock::now());
            Require(convertedCounts.counters.contains("rrc_n1_fill"), "a concurrent dispatch-to-fill result was not published as a fill");
            RegionRecognizer missingBindings;
            for (std::uint32_t frame = 0; frame < 3; ++frame) {
                const auto report = missingBindings.Observe(0, commands);
                const auto region = RegionCensus::Begin(0, commands, report, true);
                missingBindings.EndFrame();
                const RegionCensus::RegionScope regionScope(region);
                const RegionCensus::PacketScope packetScope(0, commands);
                RegionCensus::UnobservedBindings();
            }
            const auto missing = ArchB::Recorder::Collect(ArchB::Recorder::Clock::now());
            Require(missing.counters.contains("rrc_incomplete_regions") && missing.counters.contains("rrc_unclassified_regions"), "missing cached bindings claimed a complete repeated region");
        }
        std::cout << "resolved region runtime: deferred completion, alternating frames and moved buffers passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
