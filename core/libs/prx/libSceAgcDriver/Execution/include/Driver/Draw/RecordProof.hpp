#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_RECORDPROOF_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_RECORDPROOF_HPP

#include "prx/libSceAgcDriver/Execution/include/PageTracker.hpp"
#include <span>

namespace AgcDriver::DriverDetail {

using RecordProofRuns = std::span<const std::pair<std::uint64_t, std::uint64_t>>;
enum class RecordProofResult : std::uint8_t { Accepted, Missing, Coverage, Mapping, Cpu, Gpu, Pending, Label, Forget, Publish, Ranges, Count };

struct RecordProofCounts {
    std::array<std::uint64_t, static_cast<std::size_t>(RecordProofResult::Count)> results{};
    std::uint64_t recorded = 0, unstable = 0;
    auto operator<=>(const RecordProofCounts&) const = default;
};

struct RecordProof {
    std::vector<PageTracker::Epoch> epochs;
    std::uint64_t forgetSerial = 0;

    static bool Enabled();
    static PageTracker::Observation Observe(RecordProofRuns runs);
    static std::shared_ptr<const RecordProof> Capture(const PageTracker::Tracker& tracker, RecordProofRuns runs, std::uint64_t forget, const PageTracker::Observation* observation = nullptr);
    static RecordProofResult Check(const PageTracker::Tracker& tracker, const RecordProof* proof, RecordProofRuns runs, std::uint64_t forget, const PageTracker::Observation* observation = nullptr);
    static void Account(RecordProofResult result);
    static void Recorded(bool stable);
    static RecordProofCounts Counts();
    static std::string ProfileLine();
    static void ReportIfDue();
};

}
#endif
