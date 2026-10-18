#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_RECORDPROOF_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_RECORDPROOF_HPP

#include "prx/libSceAgcDriver/Execution/include/PageTracker.hpp"
#include <span>

namespace AgcDriver::DriverDetail {

class PinnedRecordProof;

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
    static std::shared_ptr<const PinnedRecordProof> Pin(const PageTracker::Tracker& tracker, std::shared_ptr<const RecordProof> contents, RecordProofRuns runs, std::uint64_t forget, std::shared_ptr<const void> resource, std::uint64_t generation, std::uint64_t writerChunk = 0);
    static void Account(RecordProofResult result);
    static void Recorded(bool stable);
    static RecordProofCounts Counts();
    static std::string ProfileLine();
    static void ReportIfDue();
};

class PinnedRecordProof {
public:
    RecordProofResult Check(const PageTracker::Tracker& tracker, std::uint64_t forget) const;
    const std::shared_ptr<const void>& Resource() const { return resource; }
    std::uint64_t Generation() const { return generation; }
    std::uint64_t WriterChunk() const { return writerChunk; }
    RecordProofRuns Runs() const { return runs; }

private:
    friend struct RecordProof;
    PinnedRecordProof() = default;
    const PageTracker::Tracker* tracker = nullptr;
    std::shared_ptr<const RecordProof> contents, parsed;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
    std::shared_ptr<const void> resource;
    std::uint64_t generation = 0, writerChunk = 0;
};

}
#endif
