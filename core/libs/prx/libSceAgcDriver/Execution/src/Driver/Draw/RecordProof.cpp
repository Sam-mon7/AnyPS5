#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/RecordProof.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string_view>

namespace AgcDriver::DriverDetail {
namespace {
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(RecordProofResult::Count)> results{};
std::atomic<std::uint64_t> recorded{0}, unstable{0};
}

bool RecordProof::Enabled() {
    static const bool enabled = [] { const auto* value = std::getenv("APS5_EPOCH_PROOFS"); return value != nullptr && std::string_view(value) == "1" && PageTracker::Enabled(); }();
    return enabled;
}

std::shared_ptr<const RecordProof> RecordProof::Capture(const PageTracker::Tracker& tracker, RecordProofRuns runs, std::uint64_t forget) {
    if (!Enabled()) return nullptr;
    auto proof = std::make_shared<RecordProof>();
    proof->forgetSerial = forget;
    proof->epochs.reserve(runs.size());
    for (const auto& [begin, end] : runs) proof->epochs.push_back(tracker.CaptureEpoch({begin, end > begin ? end - begin : 0}));
    return proof;
}

RecordProofResult RecordProof::Check(const PageTracker::Tracker& tracker, const RecordProof* proof, RecordProofRuns runs, std::uint64_t forget) {
    if ((forget & 1) != 0 || (proof != nullptr && proof->forgetSerial != forget)) return RecordProofResult::Forget;
    if (proof == nullptr) {
        for (const auto& [begin, end] : runs) {
            if (end <= begin || !tracker.CaptureEpoch({begin, end - begin}).covered) return RecordProofResult::Coverage;
        }
        return RecordProofResult::Missing;
    }
    if (proof->epochs.size() != runs.size()) return RecordProofResult::Ranges;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const auto& [begin, end] = runs[i];
        const PageTracker::Range range{begin, end > begin ? end - begin : 0};
        const auto& epoch = proof->epochs[i];
        if (epoch.range != range) return RecordProofResult::Ranges;
        if (!tracker.DirtySince(range, epoch)) continue;
        const auto current = tracker.CaptureEpoch(range);
        if (current.regions.size() != epoch.regions.size()) return RecordProofResult::Coverage;
        for (std::size_t r = 0; r < epoch.regions.size(); ++r) {
            const auto& before = epoch.regions[r];
            const auto& after = current.regions[r];
            if (before.regionId != after.regionId || before.mapEpoch != after.mapEpoch) return RecordProofResult::Mapping;
            if (before.gpuEpoch != after.gpuEpoch) return RecordProofResult::Gpu;
            if (before.cpuEpoch != after.cpuEpoch) return RecordProofResult::Cpu;
        }
        return RecordProofResult::Coverage;
    }
    return RecordProofResult::Accepted;
}

void RecordProof::Account(RecordProofResult result) {
    if (!Enabled()) return;
    results[static_cast<std::size_t>(result)].fetch_add(1, std::memory_order_relaxed);
    ReportIfDue();
}

void RecordProof::Recorded(bool stable) {
    if (!Enabled()) return;
    (stable ? recorded : unstable).fetch_add(1, std::memory_order_relaxed);
}

RecordProofCounts RecordProof::Counts() {
    RecordProofCounts counts;
    for (std::size_t i = 0; i < results.size(); ++i) counts.results[i] = results[i].load(std::memory_order_relaxed);
    counts.recorded = recorded.load(std::memory_order_relaxed);
    counts.unstable = unstable.load(std::memory_order_relaxed);
    return counts;
}

std::string RecordProof::ProfileLine() {
    if (!Enabled()) return {};
    const auto counts = Counts();
    constexpr std::array names{"accepted", "missing", "coverage", "mapping", "cpu", "gpu", "pending", "label", "forget", "publish", "ranges"};
    static_assert(names.size() == static_cast<std::size_t>(RecordProofResult::Count));
    std::ostringstream line;
    line << "[record-proof] cumulative accepted=" << counts.results[0];
    std::uint64_t fallbacks = 0;
    for (std::size_t i = 1; i < names.size(); ++i) {
        line << " rejected-" << names[i] << '=' << counts.results[i];
        fallbacks += counts.results[i];
    }
    line << " fallbacks=" << fallbacks << " recorded=" << counts.recorded << " unstable=" << counts.unstable << '\n';
    return line.str();
}

void RecordProof::ReportIfDue() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!Enabled() || !profile) return;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    static std::atomic<std::int64_t> last{now};
    auto previous = last.load(std::memory_order_relaxed);
    if (now - previous < 10000 || !last.compare_exchange_strong(previous, now)) return;
    const auto line = ProfileLine();
    std::fputs(line.c_str(), stderr);
}

}
