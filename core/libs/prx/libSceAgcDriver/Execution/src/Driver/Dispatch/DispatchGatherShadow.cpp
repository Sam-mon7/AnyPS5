#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchGatherShadow.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/WriteEvidence.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace AgcDriver::DriverDetail::DispatchShadow {
namespace {
constexpr std::size_t CandidateLimit = 256;
constexpr std::size_t UseLimit = 64;
constexpr unsigned DifferReportLimit = 32;
struct Statistics {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> aheadRefused{0};
    std::atomic<std::uint64_t> aheadRefusedOther{0};
    std::atomic<std::uint64_t> aheadCandidates{0};
    std::atomic<std::uint64_t> aheadAdopted{0};
    std::atomic<std::uint64_t> aheadFallback{0};
    std::atomic<std::uint64_t> faultAbiCalls{0};
    std::atomic<std::uint64_t> addressTableCalls{0};
    std::atomic<std::uint64_t> callsRefused{0};
    std::atomic<std::uint64_t> indexFull{0};
    std::atomic<std::uint64_t> templatesBuilt{0};
    std::atomic<std::uint64_t> learnedOnMiss{0};
    std::atomic<std::uint64_t> variantSearches{0};
    std::atomic<std::uint64_t> variantHits{0};
    std::atomic<std::uint64_t> variantsLearned{0};
    std::atomic<std::uint64_t> variantFull{0};
    std::atomic<std::uint64_t> variantMissingCapture{0};
    std::atomic<std::uint64_t> matchedLostBeforeMaterializedSerial{0};
    std::atomic<std::uint64_t> matchedLostBeforeMaterializedViews{0};
    std::atomic<std::uint64_t> materializedLostSerial{0};
    std::atomic<std::uint64_t> materializedLostViews{0};
    std::atomic<std::uint64_t> variantStaleViews{0};
    std::array<std::atomic<std::uint64_t>, VariantLimit + 1> variantBucketUses{};
    std::array<std::atomic<std::uint64_t>, VariantLimit> variantSelected{};
    std::array<std::atomic<std::uint64_t>, VariantLimit> variantLearnedSlots{};
    std::atomic<std::uint64_t> indexMiss{0};
    std::atomic<std::uint64_t> matched{0};
    std::atomic<std::uint64_t> gathered{0};
    std::atomic<std::uint64_t> evaluated{0};
    std::atomic<std::uint64_t> commitNanoseconds{0};
    std::atomic<std::uint64_t> queuedWithoutDrain{0};
    std::atomic<std::uint64_t> committedWithoutRebuild{0};
    std::atomic<std::uint64_t> queuedCommittedWithoutRebuild{0};
    std::atomic<std::uint64_t> falseMatch{0};
    std::atomic<std::uint64_t> falseDiffer{0};
    std::atomic<std::uint64_t> absentMatched{0};
    std::atomic<std::uint64_t> differingMatched{0};
    std::atomic<std::uint64_t> guardedReads{0};
    std::atomic<std::uint64_t> fallbackReads{0};
    std::atomic<std::uint64_t> unreadable{0};
    std::atomic<std::uint64_t> pending{0};
    std::atomic<std::uint64_t> queued{0};
    std::atomic<std::uint64_t> shadowed{0};
    std::atomic<std::uint64_t> labels{0};
    std::atomic<std::uint64_t> serialMoved{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(SerialPhase::Count)> serialPhases{};
    std::array<std::atomic<std::uint64_t>, 8> serialComponents{};
    std::array<std::array<std::atomic<std::uint64_t>, 8>, static_cast<std::size_t>(SerialPhase::Count)> serialChanges{};
    std::atomic<std::uint64_t> poolBusy{0};
    std::atomic<std::uint64_t> verifiedResolutions{0};
    std::atomic<std::uint64_t> payloadReady{0};
    std::atomic<std::uint64_t> materializedChecks{0};
    std::atomic<std::uint64_t> materializedMismatch{0};
    std::atomic<std::uint64_t> commitChecks{0};
    std::atomic<std::uint64_t> writerMultiUnitChecks{0};
    std::atomic<std::uint64_t> commitMismatch{0};
    std::atomic<std::uint64_t> commitRefused{0};
    std::array<std::atomic<std::uint64_t>, 18> commitRefusals{};
    std::atomic<std::uint64_t> nanoseconds{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DispatchGatherRefusal::Count)> graphRefusals{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DispatchGatherRefusal::Count)> aheadGraphRefusals{};
    std::array<std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(PopulationStage::Count)>, static_cast<std::size_t>(Population::Count)> populations{};
    std::array<std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DispatchGatherRefusal::Count)>, static_cast<std::size_t>(Population::Count)> populationRefusals{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DispatchGatherDifference::Count)> differences{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DispatchGatherDifference::Count)> falseDifferences{};
};
Statistics stats;
std::atomic<std::uint64_t> nextCandidateId{1};
constexpr std::array<std::string_view, 18> commitRefusalNames{"unknown", "no_payload", "overlay", "private_commands", "thread_dimensions", "indirect_payload", "split_groups", "unobserved_upload", "address_table", "fault_buffer", "binding_role", "resource_state", "writer_mode", "writer_shape", "writer_alias", "writer_depth", "writer_unobserved", "writer_epoch_suppressed"};
std::atomic<void (*)(std::uint64_t, std::size_t, bool)> readHook{nullptr};
using Serials = std::array<std::uint64_t, 8>;
Serials serials() {
    return {GuestMemory::ForgetSerial(), GuestMemory::ProofSerial(), GuestMemory::ResourceProofSerial(), GuestMemory::WriteGeneration(), Graphics::Recorder::PublishGeneration(), Graphics::Recorder::WriteGeneration(), Graphics::StorageTexture::PendingSerial(), DrawPipeline::EpochToken().load(std::memory_order_acquire)};
}
bool noteSerialMovement(const Serials& before, const Serials& after, SerialPhase phase, bool counted = true, bool requireEven = false) {
    const bool odd = requireEven && (before[0] & 1u) != 0u;
    if (before == after && !odd) return false;
    if (counted) ++stats.serialMoved;
    const auto p = static_cast<std::size_t>(phase);
    ++stats.serialPhases[p];
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (before[i] == after[i] && !(i == 0u && odd)) continue;
        ++stats.serialComponents[i];
        ++stats.serialChanges[p][i];
    }
    return true;
}
void notePopulation(std::uint8_t mask, PopulationStage stage, std::uint64_t amount = 1u) {
    for (std::size_t population = 0; population < stats.populations.size(); ++population) if ((mask & (1u << population)) != 0u) stats.populations[population][static_cast<std::size_t>(stage)].fetch_add(amount, std::memory_order_relaxed);
}
struct Timer {
    const std::uint8_t* populationMask = nullptr;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~Timer() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        stats.nanoseconds.fetch_add(elapsed, std::memory_order_relaxed);
        if (populationMask != nullptr) notePopulation(*populationMask, PopulationStage::Nanoseconds, elapsed);
    }
};
void registerCounters() {
    static const bool registered = [] {
        ArchB::Recorder::RegisterCounter("dispatch_gather_calls", stats.calls);
        ArchB::Recorder::RegisterCounter("dispatch_gather_ahead_refused", stats.aheadRefused);
        ArchB::Recorder::RegisterCounter("dispatch_gather_ahead_refused_other", stats.aheadRefusedOther);
        ArchB::Recorder::RegisterCounter("dispatch_gather_ahead_candidates", stats.aheadCandidates);
        ArchB::Recorder::RegisterCounter("dispatch_gather_ahead_adopted", stats.aheadAdopted);
        ArchB::Recorder::RegisterCounter("dispatch_gather_ahead_fallback", stats.aheadFallback);
        ArchB::Recorder::RegisterCounter("dispatch_gather_fault_abi_calls", stats.faultAbiCalls);
        ArchB::Recorder::RegisterCounter("dispatch_gather_address_table_calls", stats.addressTableCalls);
        ArchB::Recorder::RegisterCounter("dispatch_gather_calls_refused", stats.callsRefused);
        ArchB::Recorder::RegisterCounter("dispatch_gather_index_full", stats.indexFull);
        ArchB::Recorder::RegisterCounter("dispatch_gather_templates_built", stats.templatesBuilt);
        ArchB::Recorder::RegisterCounter("dispatch_gather_learned_on_miss", stats.learnedOnMiss);
        ArchB::Recorder::RegisterCounter("dispatch_gather_variant_searches", stats.variantSearches);
        ArchB::Recorder::RegisterCounter("dispatch_gather_variant_hits", stats.variantHits);
        ArchB::Recorder::RegisterCounter("dispatch_gather_variants_learned", stats.variantsLearned);
        ArchB::Recorder::RegisterCounter("dispatch_gather_variant_full", stats.variantFull);
        ArchB::Recorder::RegisterCounter("dispatch_gather_variant_missing_capture", stats.variantMissingCapture);
        ArchB::Recorder::RegisterCounter("dispatch_gather_matched_lost_before_materialized_serial", stats.matchedLostBeforeMaterializedSerial);
        ArchB::Recorder::RegisterCounter("dispatch_gather_matched_lost_before_materialized_views", stats.matchedLostBeforeMaterializedViews);
        ArchB::Recorder::RegisterCounter("dispatch_gather_materialized_lost_serial", stats.materializedLostSerial);
        ArchB::Recorder::RegisterCounter("dispatch_gather_materialized_lost_views", stats.materializedLostViews);
        ArchB::Recorder::RegisterCounter("dispatch_gather_variant_stale_views", stats.variantStaleViews);
        static std::array<std::string, VariantLimit + 1> variantBucketNames;
        static std::array<std::string, VariantLimit> variantSelectedNames;
        static std::array<std::string, VariantLimit> variantLearnedNames;
        for (std::size_t i = 0; i <= VariantLimit; ++i) {
            variantBucketNames[i] = "dispatch_gather_variant_bucket_uses_" + std::to_string(i);
            ArchB::Recorder::RegisterCounter(variantBucketNames[i].c_str(), stats.variantBucketUses[i]);
            if (i == VariantLimit) continue;
            variantSelectedNames[i] = "dispatch_gather_variant_selected_" + std::to_string(i + 1u);
            variantLearnedNames[i] = "dispatch_gather_variant_learned_slot_" + std::to_string(i + 1u);
            ArchB::Recorder::RegisterCounter(variantSelectedNames[i].c_str(), stats.variantSelected[i]);
            ArchB::Recorder::RegisterCounter(variantLearnedNames[i].c_str(), stats.variantLearnedSlots[i]);
        }
        ArchB::Recorder::RegisterCounter("dispatch_gather_index_miss", stats.indexMiss);
        ArchB::Recorder::RegisterCounter("dispatch_gather_matched", stats.matched);
        ArchB::Recorder::RegisterCounter("dispatch_gather_gathered", stats.gathered);
        ArchB::Recorder::RegisterCounter("dispatch_gather_evaluated", stats.evaluated);
        ArchB::Recorder::RegisterCounter("dispatch_gather_commit_nanoseconds", stats.commitNanoseconds);
        ArchB::Recorder::RegisterCounter("dispatch_gather_queued_without_drain", stats.queuedWithoutDrain);
        ArchB::Recorder::RegisterCounter("dispatch_gather_committed_without_rebuild", stats.committedWithoutRebuild);
        ArchB::Recorder::RegisterCounter("dispatch_gather_queued_committed_without_rebuild", stats.queuedCommittedWithoutRebuild);
        ArchB::Recorder::RegisterCounter("dispatch_gather_false_match", stats.falseMatch);
        ArchB::Recorder::RegisterCounter("dispatch_gather_false_differ", stats.falseDiffer);
        ArchB::Recorder::RegisterCounter("dispatch_gather_absent_matched", stats.absentMatched);
        ArchB::Recorder::RegisterCounter("dispatch_gather_differing_matched", stats.differingMatched);
        ArchB::Recorder::RegisterCounter("dispatch_gather_guarded_reads", stats.guardedReads);
        ArchB::Recorder::RegisterCounter("dispatch_gather_fallback_reads", stats.fallbackReads);
        ArchB::Recorder::RegisterCounter("dispatch_gather_unreadable", stats.unreadable);
        ArchB::Recorder::RegisterCounter("dispatch_gather_pending", stats.pending);
        ArchB::Recorder::RegisterCounter("dispatch_gather_queued", stats.queued);
        ArchB::Recorder::RegisterCounter("dispatch_gather_shadowed", stats.shadowed);
        ArchB::Recorder::RegisterCounter("dispatch_gather_labels", stats.labels);
        ArchB::Recorder::RegisterCounter("dispatch_gather_serial_moved", stats.serialMoved);
        constexpr std::array<std::string_view, static_cast<std::size_t>(SerialPhase::Count)> serialPhaseNames{"read_pre", "read_post", "evaluate_begin", "evaluate_end", "resolved_entry", "learn_issued", "learn_compare", "learn_end", "false_differ", "materialized_compare"};
        constexpr std::array<std::string_view, 8> serialComponentNames{"forget", "proof", "resource_proof", "guest_write", "recorder_publish", "recorder_write", "storage_pending", "draw_epoch"};
        static std::array<std::string, static_cast<std::size_t>(SerialPhase::Count)> serialPhaseCounters;
        static std::array<std::string, 8> serialComponentCounters;
        static std::array<std::array<std::string, 8>, static_cast<std::size_t>(SerialPhase::Count)> serialChangeCounters;
        for (std::size_t i = 0; i < serialComponentNames.size(); ++i) {
            serialComponentCounters[i] = "dispatch_gather_serial_component_" + std::string(serialComponentNames[i]);
            ArchB::Recorder::RegisterCounter(serialComponentCounters[i].c_str(), stats.serialComponents[i]);
        }
        for (std::size_t p = 0; p < serialPhaseNames.size(); ++p) {
            serialPhaseCounters[p] = "dispatch_gather_serial_phase_" + std::string(serialPhaseNames[p]);
            ArchB::Recorder::RegisterCounter(serialPhaseCounters[p].c_str(), stats.serialPhases[p]);
            for (std::size_t i = 0; i < serialComponentNames.size(); ++i) {
                serialChangeCounters[p][i] = "dispatch_gather_serial_" + std::string(serialPhaseNames[p]) + "_" + std::string(serialComponentNames[i]);
                ArchB::Recorder::RegisterCounter(serialChangeCounters[p][i].c_str(), stats.serialChanges[p][i]);
            }
        }
        ArchB::Recorder::RegisterCounter("dispatch_gather_pool_busy", stats.poolBusy);
        ArchB::Recorder::RegisterCounter("dispatch_gather_verified_resolutions", stats.verifiedResolutions);
        ArchB::Recorder::RegisterCounter("dispatch_gather_payload_ready", stats.payloadReady);
        ArchB::Recorder::RegisterCounter("dispatch_gather_materialized_checks", stats.materializedChecks);
        ArchB::Recorder::RegisterCounter("dispatch_gather_materialized_mismatch", stats.materializedMismatch);
        ArchB::Recorder::RegisterCounter("dispatch_gather_commit_checks", stats.commitChecks);
        ArchB::Recorder::RegisterCounter("dispatch_gather_writer_multi_unit_checks", stats.writerMultiUnitChecks);
        ArchB::Recorder::RegisterCounter("dispatch_gather_commit_mismatch", stats.commitMismatch);
        ArchB::Recorder::RegisterCounter("dispatch_gather_commit_refused", stats.commitRefused);
        ArchB::Recorder::RegisterCounter("dispatch_gather_nanoseconds", stats.nanoseconds);
        static std::array<std::string, 18> commitRefusals;
        for (std::size_t i = 0; i < commitRefusals.size(); ++i) {
            commitRefusals[i] = std::string("dispatch_gather_commit_refused_") + std::string(commitRefusalNames[i]);
            ArchB::Recorder::RegisterCounter(commitRefusals[i].c_str(), stats.commitRefusals[i]);
        }
        static std::array<std::string, static_cast<std::size_t>(DispatchGatherRefusal::Count)> refusals;
        static std::array<std::string, static_cast<std::size_t>(DispatchGatherRefusal::Count)> aheadRefusals;
        static std::array<std::array<std::string, static_cast<std::size_t>(PopulationStage::Count)>, static_cast<std::size_t>(Population::Count)> populationNames;
        static std::array<std::array<std::string, static_cast<std::size_t>(DispatchGatherRefusal::Count)>, static_cast<std::size_t>(Population::Count)> populationRefusalNames;
        constexpr std::array<std::string_view, static_cast<std::size_t>(Population::Count)> classes{"image_read", "guarded", "image_write", "merged_control"};
        constexpr std::array<std::string_view, static_cast<std::size_t>(PopulationStage::Count)> stages{"calls", "evaluated", "matched", "gathered", "materialized_checks", "queued_without_drain", "commit_checks", "committed_without_rebuild", "queued_committed_without_rebuild", "false_match", "false_differ", "materialized_mismatch", "commit_mismatch", "guarded_reads", "fallback_reads", "nanoseconds", "writer_effect_checks", "writer_effect_mismatch"};
        for (std::size_t population = 0; population < classes.size(); ++population) {
            for (std::size_t stage = 0; stage < stages.size(); ++stage) {
                auto& name = populationNames[population][stage];
                name = "dispatch_gather_" + std::string(classes[population]) + "_" + std::string(stages[stage]);
                ArchB::Recorder::RegisterCounter(name.c_str(), stats.populations[population][stage]);
            }
            for (std::size_t refusal = 1; refusal < stats.graphRefusals.size(); ++refusal) {
                auto& name = populationRefusalNames[population][refusal];
                name = "dispatch_gather_" + std::string(classes[population]) + "_refused_" + DispatchGatherRefusalName(static_cast<DispatchGatherRefusal>(refusal));
                ArchB::Recorder::RegisterCounter(name.c_str(), stats.populationRefusals[population][refusal]);
            }
        }
        static std::array<std::string, static_cast<std::size_t>(DispatchGatherDifference::Count)> differences;
        for (std::size_t i = 1; i < refusals.size(); ++i) {
            refusals[i] = std::string("dispatch_gather_refused_") + DispatchGatherRefusalName(static_cast<DispatchGatherRefusal>(i));
            ArchB::Recorder::RegisterCounter(refusals[i].c_str(), stats.graphRefusals[i]);
            aheadRefusals[i] = std::string("dispatch_gather_ahead_refused_") + DispatchGatherRefusalName(static_cast<DispatchGatherRefusal>(i));
            ArchB::Recorder::RegisterCounter(aheadRefusals[i].c_str(), stats.aheadGraphRefusals[i]);
        }
        static std::array<std::string, static_cast<std::size_t>(DispatchGatherDifference::Count)> falseDifferences;
        for (std::size_t i = 1; i < differences.size(); ++i) {
            falseDifferences[i] = std::string("dispatch_gather_false_differ_") + DispatchGatherDifferenceName(static_cast<DispatchGatherDifference>(i));
            ArchB::Recorder::RegisterCounter(falseDifferences[i].c_str(), stats.falseDifferences[i]);
            differences[i] = std::string("dispatch_gather_differ_") + DispatchGatherDifferenceName(static_cast<DispatchGatherDifference>(i));
            ArchB::Recorder::RegisterCounter(differences[i].c_str(), stats.differences[i]);
        }
        return true;
    }();
    (void)registered;
}
void mismatch(const char* reason) {
    std::fprintf(stderr, "[dispatch-gather-shadow] mismatch: %s\n", reason);
    if (Strict()) throw std::runtime_error(std::string("DISPATCH_GATHER_SHADOW: ") + reason);
}
struct Candidate;
}
struct Use {
    Candidate* owner = nullptr;
    std::shared_ptr<const DispatchGatherGraph> graph;
    DispatchGatherUse values;
    std::array<DispatchGatherUse, VariantLimit> variantValues;
    std::size_t valueSlot = VariantLimit;
    std::vector<std::uint32_t> userData;
    std::uint64_t program = 0;
    std::uint64_t arguments = 0;
    std::uint64_t drainRequests = 0;
    std::array<std::uint32_t, 3> indirect{};
    std::uint32_t queue = 0;
    std::thread::id issuer;
    std::atomic<bool> inlineCommit{false};
    std::atomic<unsigned> stages{0};
    Serials before{};
    Serials issued{};
    PendingView pendingView;
    DispatchGatherDifference difference = DispatchGatherDifference::None;
    std::uint8_t populationMask = 0;
    DispatchGatherRefusal refusal = DispatchGatherRefusal::None;
    bool populationCounted = false;
    bool censusCounted = false;
    bool aheadCandidate = false;
    bool readFailed = false;
    bool attempted = false;
    bool evaluationCounted = false;
    bool matched = false;
    bool verified = false;
    bool payload = false;
    bool rebuilt = false;
    CommitInput commit;
};
namespace {
struct Candidate {
    std::vector<std::uint64_t> key;
    std::vector<std::uint32_t> code;
    std::shared_ptr<const ShaderSnapshot> shader;
    std::array<std::shared_ptr<const DispatchGatherGraph>, VariantLimit> variants;
    std::size_t variantCount = 0;
    std::size_t lastMatch = VariantLimit - 1u;
    std::uint64_t id = 0;
    bool fullReported = false;
    unsigned differReports = 0;
    unsigned readFailureReports = 0;
    std::array<std::shared_ptr<Use>, UseLimit> uses;
    std::uint64_t program = 0;
    DispatchGatherRefusal refusal = DispatchGatherRefusal::None;
    const char* refusalDetail = nullptr;
    const char* writerCensus = nullptr;
    bool mergedCensus = false;
    std::uint8_t populationMask = 0;
    bool calls = false;
};
struct Index {
    std::array<std::unique_ptr<Candidate>, CandidateLimit> candidates;
    std::vector<std::uint64_t> key;
    std::size_t count = 0;
};
struct IndexStorage {};
struct ActiveStorage {};
using ActiveSlot = HostThreadSlot<Use*, ActiveStorage>;
Index& threadIndex() { return HostThreadLocal<Index, IndexStorage>(); }
std::uint64_t drainRequests() {
    std::uint64_t sum = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(DrawPipeline::DrainReason::Count); ++i) sum += DrawPipeline::DrainRequestsByReason(static_cast<DrawPipeline::DrainReason>(i));
    return sum;
}
void noteRefusalDetail(const char* detail) {
    if (detail == nullptr) return;
    ArchB::Recorder::Count(detail);
    if (std::string_view(detail).starts_with("dispatch_gather_census_layout_merged_control_")) ArchB::Recorder::Count("dispatch_gather_census_layout_merged_control");
}
void noteRefusal(Use& use, DispatchGatherRefusal refusal) {
    use.refusal = refusal;
    const auto index = static_cast<std::size_t>(refusal);
    ++stats.graphRefusals[index];
    for (std::size_t population = 0; population < stats.populationRefusals.size(); ++population) if ((use.populationMask & (1u << population)) != 0u) ++stats.populationRefusals[population][index];
}
void classify(Use& use, const ShaderRecompiler::IrResourcePlan* plan = nullptr) {
    if (plan != nullptr) {
        if (!plan->info.images.empty() && std::ranges::none_of(plan->info.images, [](const auto& image) { return image.written || image.atomic || image.atomic64 || image.targetMerge; })) use.populationMask |= 1u << static_cast<unsigned>(Population::ImageRead);
        if (!plan->guardedSrtSlots.empty()) use.populationMask |= 1u << static_cast<unsigned>(Population::Guarded);
        if (std::ranges::any_of(plan->info.images, [](const auto& image) { return image.written || image.atomic || image.atomic64 || image.targetMerge; })) use.populationMask |= 1u << static_cast<unsigned>(Population::ImageWrite);
        if (std::ranges::any_of(plan->controlFlow, [](const auto& block) { return block.condition == nullptr && block.successors.size() > 1u; })) use.populationMask |= 1u << static_cast<unsigned>(Population::MergedControl);
        const auto written = std::ranges::count_if(plan->info.images, [](const auto& image) { return image.written; });
        if (written == 1 && std::ranges::none_of(plan->info.images, [](const auto& image) { return image.atomic || image.atomic64 || image.targetMerge || image.table != ShaderRecompiler::NoTable; })) use.owner->writerCensus = plan->info.images.size() == 1u ? "dispatch_gather_cohort_writer_one_image_no_readers" : "dispatch_gather_cohort_writer_one_image_with_readers";
        use.owner->mergedCensus = (use.populationMask & (1u << static_cast<unsigned>(Population::MergedControl))) != 0u;
        use.owner->populationMask = use.populationMask;
    }
    if (!use.censusCounted && (plan != nullptr || use.owner->writerCensus != nullptr || use.owner->mergedCensus)) {
        if (use.owner->writerCensus != nullptr) ArchB::Recorder::Count(use.owner->writerCensus);
        if (use.owner->mergedCensus) ArchB::Recorder::Count("dispatch_gather_cohort_layout_merged_control");
        use.censusCounted = true;
    }
    if (!use.populationCounted && (plan != nullptr || use.populationMask != 0u)) {
        notePopulation(use.populationMask, PopulationStage::Calls);
        use.populationCounted = true;
    }
}
void selectValues(Use& use, std::size_t index) {
    if (use.valueSlot == index) return;
    if (use.valueSlot != VariantLimit) std::swap(use.values, use.variantValues[use.valueSlot]);
    std::swap(use.values, use.variantValues[index]);
    use.valueSlot = index;
}
bool registeredViewsCurrent(const Use& use) {
    if (!use.graph) return true;
    for (const auto& view : use.graph->RegisteredViews()) {
        const auto address = static_cast<std::uintptr_t>(view.address);
        std::shared_ptr<const GuestAllocations::Range> range;
        GuestAllocations::GuestAllocationsLookupEach_nid_postfix(&address, 1u, &range);
        if (view.hasRange != (range != nullptr)) return false;
        if (range != nullptr && (range->address != view.rangeAddress || range->bytes != view.rangeBytes || range->allocationAddress != view.allocationAddress || range->allocationBytes != view.allocationBytes || range->backingSerial != view.backingSerial || range->readable != view.readable || range->writable != view.writable || range->gpu != view.gpu)) return false;
    }
    return true;
}
void reportReadFailure(Use& use, std::uint64_t address, const char* reason, std::uint32_t bytes = 4u) {
    if (use.owner->readFailureReports >= 8u) return;
    ++use.owner->readFailureReports;
    std::fprintf(stderr, "[dispatch-gather-read-failed] candidate=%llu program=0x%llx queue=0x%x address=0x%llx bytes=%u reason=%s\n", static_cast<unsigned long long>(use.owner->id), static_cast<unsigned long long>(use.program), use.queue, static_cast<unsigned long long>(address), bytes, reason);
}
bool readWord(void* context, std::uint64_t address, std::uint32_t* value) {
    auto& use = *static_cast<Use*>(context);
    if (use.readFailed) return false;
    const auto refuse = [&](std::atomic<std::uint64_t>& counter, const char* reason) { ++counter; use.readFailed = true; reportReadFailure(use, address, reason); return false; };
    if (const auto hook = readHook.load(std::memory_order_acquire)) hook(address, sizeof(*value), false);
    if (address == 0 || address > std::numeric_limits<std::uint64_t>::max() - sizeof(*value)) return refuse(stats.unreadable, "null_or_overflow");
    if (noteSerialMovement(use.before, serials(), SerialPhase::ReadPre, true, true)) { use.readFailed = true; reportReadFailure(use, address, "serial_pre"); return false; }
    if (DrawPipeline::Queue0().Overlaps(address, sizeof(*value)) || DrawFinisher::Get().PendingOverlaps(address, address + sizeof(*value))) return refuse(stats.queued, "queued");
    use.pendingView.Load();
    if (use.pendingView.Overlaps(address, sizeof(*value))) return refuse(stats.pending, "recorder_pending");
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> ranges{{{address, address + sizeof(*value)}}};
    if (Graphics::StorageTexture::AnyPendingOverlaps(ranges)) return refuse(stats.pending, "image_pending");
    if (Graphics::AnyShadowedOverlaps(address, sizeof(*value))) return refuse(stats.shadowed, "unit_shadow");
    for (const auto& label : deferredLabels().labels) if (label.address < address + sizeof(*value) && address < label.address + label.size) return refuse(stats.labels, "deferred_label");
    bool copied = false;
#ifdef _WIN32
    copied = GuestArena::GuestArenaCopyOwned_nid_no_patch(address, value, sizeof(*value));
#endif
    if (copied) { ++stats.guardedReads; notePopulation(use.populationMask, PopulationStage::GuardedReads); }
    else {
        copied = GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span{value, std::size_t{1}}), true) == GuestMemory::Compare::Equal;
        if (copied) { ++stats.fallbackReads; notePopulation(use.populationMask, PopulationStage::FallbackReads); }
    }
    if (!copied) return refuse(stats.unreadable, "copy_failed");
    if (const auto hook = readHook.load(std::memory_order_acquire)) hook(address, sizeof(*value), true);
    if (noteSerialMovement(use.before, serials(), SerialPhase::ReadPost)) { use.readFailed = true; reportReadFailure(use, address, "serial_post"); return false; }
    return true;
}
bool evaluate(Use& use) {
    use.attempted = true;
    use.values.gathered = false;
    use.before = serials();
    use.pendingView = {};
    use.readFailed = false;
    if (noteSerialMovement(use.before, use.before, SerialPhase::EvaluateBegin, true, true)) { use.readFailed = true; reportReadFailure(use, 0u, "gather_begin_transition", 0u); return false; }
    if (!registeredViewsCurrent(use)) { noteRefusal(use, DispatchGatherRefusal::RegisteredViews); return false; }
    auto refusal = use.graph->Evaluate(use.userData, use.program, readWord, &use, use.values);
    if (refusal == DispatchGatherRefusal::None && use.arguments != 0) {
        for (std::size_t i = 0; i < use.indirect.size(); ++i) if (!readWord(&use, use.arguments + i * sizeof(std::uint32_t), &use.indirect[i])) { refusal = DispatchGatherRefusal::ReadFailed; break; }
    }
    if (noteSerialMovement(use.before, serials(), SerialPhase::EvaluateEnd, !use.readFailed)) {
        if (!use.readFailed) reportReadFailure(use, 0u, "gather_end_serial", 0u);
        use.readFailed = true;
        refusal = DispatchGatherRefusal::ReadFailed;
    }
    if (refusal == DispatchGatherRefusal::None && !registeredViewsCurrent(use)) refusal = DispatchGatherRefusal::RegisteredViews;
    if (refusal != DispatchGatherRefusal::None) {
        noteRefusal(use, refusal);
        return false;
    }
    if (!use.evaluationCounted) {
        ++stats.evaluated;
        notePopulation(use.populationMask, PopulationStage::Evaluated);
        use.evaluationCounted = true;
    }
    return true;
}
void matchVariants(Use& use) {
    auto& candidate = *use.owner;
    if (candidate.variantCount == 0u) return;
    std::shared_ptr<const DispatchGatherGraph> evaluatedGraph;
    for (std::size_t trial = 0; trial < candidate.variantCount; ++trial) {
        const auto index = (candidate.lastMatch + 1u + trial) % candidate.variantCount;
        use.graph = candidate.variants[index];
        if (!registeredViewsCurrent(use)) { ++stats.variantStaleViews; continue; }
        ++stats.variantSearches;
        selectValues(use, index);
        if (!evaluate(use)) return;
        evaluatedGraph = use.graph;
        use.difference = use.graph->Matches(use.values);
        if (use.difference != DispatchGatherDifference::None) {
            if (candidate.differReports < DifferReportLimit && (candidate.variantCount == VariantLimit || candidate.variantCount == 1u)) {
                ++candidate.differReports;
                std::fprintf(stderr, "[dispatch-gather-differ] candidate=%llu program=0x%llx queue=0x%x variants=%zu variant=%zu kind=%s %s\n", static_cast<unsigned long long>(candidate.id), static_cast<unsigned long long>(use.program), use.queue, candidate.variantCount, index, DispatchGatherDifferenceName(use.difference), use.graph->DescribeDifference(use.values).c_str());
            }
            continue;
        }
        candidate.lastMatch = index;
        use.matched = true;
        ++stats.variantHits;
        ++stats.variantSelected[index];
        ++stats.matched;
        ++stats.gathered;
        notePopulation(use.populationMask, PopulationStage::Matched);
        notePopulation(use.populationMask, PopulationStage::Gathered);
        return;
    }
    if (!evaluatedGraph) { noteRefusal(use, DispatchGatherRefusal::RegisteredViews); return; }
    use.graph = std::move(evaluatedGraph);
    ++stats.differences[static_cast<std::size_t>(use.difference)];
}
}

bool Enabled() { return ArchB::Switches::Enabled(ArchB::Feature::DispatchGatherShadow); }
bool Strict() {
    const auto value = std::getenv("APS5_DISPATCH_GATHER_SHADOW_STRICT");
    return value != nullptr && std::string_view(value) == "1";
}
Counters ReadCounters() {
    Counters result;
    result.calls = stats.calls.load(std::memory_order_relaxed);
    result.aheadRefused = stats.aheadRefused.load(std::memory_order_relaxed);
    result.aheadRefusedOther = stats.aheadRefusedOther.load(std::memory_order_relaxed);
    result.aheadCandidates = stats.aheadCandidates.load(std::memory_order_relaxed);
    result.aheadAdopted = stats.aheadAdopted.load(std::memory_order_relaxed);
    result.aheadFallback = stats.aheadFallback.load(std::memory_order_relaxed);
    result.faultAbiCalls = stats.faultAbiCalls.load(std::memory_order_relaxed);
    result.addressTableCalls = stats.addressTableCalls.load(std::memory_order_relaxed);
    result.callsRefused = stats.callsRefused.load(std::memory_order_relaxed);
    result.indexFull = stats.indexFull.load(std::memory_order_relaxed);
    result.templatesBuilt = stats.templatesBuilt.load(std::memory_order_relaxed);
    result.learnedOnMiss = stats.learnedOnMiss.load(std::memory_order_relaxed);
    result.variantSearches = stats.variantSearches.load(std::memory_order_relaxed);
    result.variantHits = stats.variantHits.load(std::memory_order_relaxed);
    result.variantsLearned = stats.variantsLearned.load(std::memory_order_relaxed);
    result.variantFull = stats.variantFull.load(std::memory_order_relaxed);
    result.variantMissingCapture = stats.variantMissingCapture.load(std::memory_order_relaxed);
    result.matchedLostBeforeMaterializedSerial = stats.matchedLostBeforeMaterializedSerial.load(std::memory_order_relaxed);
    result.matchedLostBeforeMaterializedViews = stats.matchedLostBeforeMaterializedViews.load(std::memory_order_relaxed);
    result.materializedLostSerial = stats.materializedLostSerial.load(std::memory_order_relaxed);
    result.materializedLostViews = stats.materializedLostViews.load(std::memory_order_relaxed);
    result.variantStaleViews = stats.variantStaleViews.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i <= VariantLimit; ++i) {
        result.variantBucketUses[i] = stats.variantBucketUses[i].load(std::memory_order_relaxed);
        if (i == VariantLimit) continue;
        result.variantSelected[i] = stats.variantSelected[i].load(std::memory_order_relaxed);
        result.variantLearnedSlots[i] = stats.variantLearnedSlots[i].load(std::memory_order_relaxed);
    }
    result.indexMiss = stats.indexMiss.load(std::memory_order_relaxed);
    result.matched = stats.matched.load(std::memory_order_relaxed);
    result.gathered = stats.gathered.load(std::memory_order_relaxed);
    result.evaluated = stats.evaluated.load(std::memory_order_relaxed);
    result.commitNanoseconds = stats.commitNanoseconds.load(std::memory_order_relaxed);
    result.queuedWithoutDrain = stats.queuedWithoutDrain.load(std::memory_order_relaxed);
    result.committedWithoutRebuild = stats.committedWithoutRebuild.load(std::memory_order_relaxed);
    result.queuedCommittedWithoutRebuild = stats.queuedCommittedWithoutRebuild.load(std::memory_order_relaxed);
    result.falseMatch = stats.falseMatch.load(std::memory_order_relaxed);
    result.falseDiffer = stats.falseDiffer.load(std::memory_order_relaxed);
    result.absentMatched = stats.absentMatched.load(std::memory_order_relaxed);
    result.differingMatched = stats.differingMatched.load(std::memory_order_relaxed);
    result.guardedReads = stats.guardedReads.load(std::memory_order_relaxed);
    result.fallbackReads = stats.fallbackReads.load(std::memory_order_relaxed);
    result.unreadable = stats.unreadable.load(std::memory_order_relaxed);
    result.pending = stats.pending.load(std::memory_order_relaxed);
    result.queued = stats.queued.load(std::memory_order_relaxed);
    result.shadowed = stats.shadowed.load(std::memory_order_relaxed);
    result.labels = stats.labels.load(std::memory_order_relaxed);
    result.serialMoved = stats.serialMoved.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.serialComponents.size(); ++i) result.serialComponents[i] = stats.serialComponents[i].load(std::memory_order_relaxed);
    for (std::size_t p = 0; p < result.serialPhases.size(); ++p) {
        result.serialPhases[p] = stats.serialPhases[p].load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < result.serialChanges[p].size(); ++i) result.serialChanges[p][i] = stats.serialChanges[p][i].load(std::memory_order_relaxed);
    }
    result.poolBusy = stats.poolBusy.load(std::memory_order_relaxed);
    result.verifiedResolutions = stats.verifiedResolutions.load(std::memory_order_relaxed);
    result.payloadReady = stats.payloadReady.load(std::memory_order_relaxed);
    result.materializedChecks = stats.materializedChecks.load(std::memory_order_relaxed);
    result.materializedMismatch = stats.materializedMismatch.load(std::memory_order_relaxed);
    result.commitChecks = stats.commitChecks.load(std::memory_order_relaxed);
    result.writerMultiUnitChecks = stats.writerMultiUnitChecks.load(std::memory_order_relaxed);
    result.commitMismatch = stats.commitMismatch.load(std::memory_order_relaxed);
    result.commitRefused = stats.commitRefused.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.commitRefusals.size(); ++i) result.commitRefusals[i] = stats.commitRefusals[i].load(std::memory_order_relaxed);
    result.nanoseconds = stats.nanoseconds.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.graphRefusals.size(); ++i) result.graphRefusals[i] = stats.graphRefusals[i].load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.differences.size(); ++i) result.differences[i] = stats.differences[i].load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.falseDifferences.size(); ++i) result.falseDifferences[i] = stats.falseDifferences[i].load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.aheadGraphRefusals.size(); ++i) result.aheadGraphRefusals[i] = stats.aheadGraphRefusals[i].load(std::memory_order_relaxed);
    for (std::size_t population = 0; population < result.populations.size(); ++population) {
        for (std::size_t stage = 0; stage < result.populations[population].size(); ++stage) result.populations[population][stage] = stats.populations[population][stage].load(std::memory_order_relaxed);
        for (std::size_t refusal = 0; refusal < result.populationRefusals[population].size(); ++refusal) result.populationRefusals[population][refusal] = stats.populationRefusals[population][refusal].load(std::memory_order_relaxed);
    }
    return result;
}
void NoteVerificationNanoseconds(std::uint64_t nanoseconds) {
    stats.nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
    stats.commitNanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
    if (const auto* active = ActiveSlot::Get()) notePopulation(active->populationMask, PopulationStage::Nanoseconds, nanoseconds);
}
void SetReadHook(void (*hook)(std::uint64_t, std::size_t, bool)) { readHook.store(hook, std::memory_order_release); }
std::shared_ptr<Use> Begin(const ShaderRecompiler::RecompileRequest& request, const std::shared_ptr<const ShaderSnapshot>& shader, std::uint32_t queue, std::uint64_t arguments, bool fromAhead, bool resolving) {
    if (!Enabled() || resolving) return {};
    registerCounters();
    Timer timer;
    ++stats.calls;
    if (fromAhead) ++stats.aheadCandidates;
    const auto refuse = [&]() -> std::shared_ptr<Use> { if (fromAhead) { ++stats.aheadRefused; ++stats.aheadRefusedOther; } return {}; };
    if (arguments > std::numeric_limits<std::uint64_t>::max() - 3 * sizeof(std::uint32_t)) { ++stats.unreadable; return refuse(); }
    auto& index = threadIndex();
    ShaderRecompiler::BuildPreparedShaderKey(request, index.key);
    Candidate* candidate = nullptr;
    for (std::size_t i = 0; i < index.count; ++i) {
        auto& entry = *index.candidates[i];
        if (entry.program == request.shader.codeAddress && entry.shader == shader && entry.key == index.key && std::ranges::equal(entry.code, request.shader.code)) { candidate = &entry; break; }
    }
    if (candidate == nullptr) {
        ++stats.indexMiss;
        if (index.count == CandidateLimit) { ++stats.indexFull; return refuse(); }
        auto entry = std::make_unique<Candidate>();
        entry->key = index.key;
        entry->code.assign(request.shader.code.begin(), request.shader.code.end());
        entry->calls = !request.context.linkedCallFallbacks.empty() || std::ranges::any_of(request.shader.code, [](std::uint32_t word) { return (word & 0xff80ff00u) == 0xbe802100u; });
        entry->shader = shader;
        entry->program = request.shader.codeAddress;
        entry->id = nextCandidateId.fetch_add(1u, std::memory_order_relaxed);
        candidate = entry.get();
        index.candidates[index.count++] = std::move(entry);
    }
    if (candidate->calls) { ++stats.callsRefused; return refuse(); }
    std::shared_ptr<Use> use;
    for (auto& slot : candidate->uses) {
        if (!slot) { slot = std::make_shared<Use>(); slot->owner = candidate; }
        if (slot.use_count() == 1) { use = slot; break; }
    }
    if (!use) { ++stats.poolBusy; return refuse(); }
    use->populationMask = candidate->populationMask;
    use->populationCounted = false;
    use->censusCounted = false;
    use->refusal = DispatchGatherRefusal::None;
    timer.populationMask = &use->populationMask;
    use->aheadCandidate = fromAhead;
    ++stats.variantBucketUses[candidate->variantCount];
    use->graph = candidate->variantCount != 0u ? candidate->variants[(candidate->lastMatch + 1u) % candidate->variantCount] : nullptr;
    use->userData.assign(request.context.userData.begin(), request.context.userData.end());
    use->program = request.shader.codeAddress;
    use->arguments = arguments;
    use->queue = queue;
    use->issuer = std::this_thread::get_id();
    use->inlineCommit.store(false, std::memory_order_relaxed);
    use->stages.store(0, std::memory_order_relaxed);
    use->drainRequests = drainRequests();
    use->issued = serials();
    use->readFailed = use->attempted = use->evaluationCounted = use->matched = use->verified = use->payload = use->rebuilt = false;
    use->values.gathered = false;
    use->difference = DispatchGatherDifference::None;
    use->commit = {};
    classify(*use, use->graph ? &use->graph->Plan() : nullptr);
    if (!fromAhead) matchVariants(*use);
    return use;
}
void AfterAdoption(const std::shared_ptr<Use>& use, bool adopted) {
    if (adopted) ++stats.aheadAdopted;
    else ++stats.aheadFallback;
    if (!use) return;
    Timer timer{&use->populationMask};
    use->issued = serials();
    matchVariants(*use);
}
void Resolved(const std::shared_ptr<Use>& use, const ShaderRecompiler::ResourceCapture* capture, const ShaderRecompiler::RecompileResult& compiled, bool cached, bool validated, bool differing) {
    if (!use) return;
    Timer timer{&use->populationMask};
    classify(*use, capture && capture->plan ? capture->plan.get() : nullptr);
    struct AheadRefusal {
        const Use& use;
        ~AheadRefusal() {
            if (use.aheadCandidate && !use.verified) {
                ++stats.aheadRefused;
                if (use.refusal == DispatchGatherRefusal::None) ++stats.aheadRefusedOther;
                else ++stats.aheadGraphRefusals[static_cast<std::size_t>(use.refusal)];
            }
        }
    } aheadRefusal{*use};
    const bool addressTable = std::ranges::any_of(compiled.bindings, [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable; });
    if (addressTable) ++stats.addressTableCalls;
    else if (compiled.bdaAbiVersion != 0u && std::ranges::any_of(compiled.bindings, [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::FaultBuffer; })) ++stats.faultAbiCalls;
    if (use->graph && use->values.gathered && noteSerialMovement(use->before, serials(), SerialPhase::ResolvedEntry)) {
        if (use->matched) ++stats.matchedLostBeforeMaterializedSerial;
        use->matched = false;
        return;
    }
    if (use->graph && use->values.gathered && !registeredViewsCurrent(*use)) { if (use->matched) ++stats.matchedLostBeforeMaterializedViews; noteRefusal(*use, DispatchGatherRefusal::RegisteredViews); use->matched = false; return; }
    if (!use->matched) {
        if (use->values.gathered && !use->readFailed && use->refusal == DispatchGatherRefusal::None && use->difference != DispatchGatherDifference::None && cached && validated) {
            if (noteSerialMovement(use->before, serials(), SerialPhase::FalseDiffer)) return;
            ++stats.falseDiffer;
            notePopulation(use->populationMask, PopulationStage::FalseDiffer);
            ++stats.falseDifferences[static_cast<std::size_t>(use->difference)];
        }
        if (use->readFailed || use->refusal != DispatchGatherRefusal::None || (use->attempted && !use->values.gathered)) return;
        if (use->owner->variantCount == VariantLimit) {
            ++stats.variantFull;
            if (!use->owner->fullReported) {
                use->owner->fullReported = true;
                std::fprintf(stderr, "[dispatch-gather-variant] candidate=%llu program=0x%llx queue=0x%x no_match_at_limit=%zu\n", static_cast<unsigned long long>(use->owner->id), static_cast<unsigned long long>(use->program), use->queue, VariantLimit);
            }
            return;
        }
        if (use->owner->refusal != DispatchGatherRefusal::None) {
            noteRefusalDetail(use->owner->refusalDetail);
            noteRefusal(*use, use->owner->refusal);
            return;
        }
        if (!capture) { ++stats.variantMissingCapture; noteRefusal(*use, DispatchGatherRefusal::MissingPlan); return; }
        DispatchGatherRefusal refusal;
        use->graph = DispatchGatherGraph::Create(*capture, compiled, refusal);
        if (!use->graph) {
            const auto* detail = DispatchGatherGraph::LastRefusalDetail();
            noteRefusalDetail(detail);
            use->owner->refusalDetail = detail;
            if (use->owner->variantCount == 0u && refusal != DispatchGatherRefusal::AbsentBuffer && refusal != DispatchGatherRefusal::Poison && refusal != DispatchGatherRefusal::Unbounded && refusal != DispatchGatherRefusal::BindingLayout && refusal != DispatchGatherRefusal::UserData && refusal != DispatchGatherRefusal::RegisteredViews && refusal != DispatchGatherRefusal::ImageNormalization) use->owner->refusal = refusal;
            noteRefusal(*use, refusal);
            return;
        }
        selectValues(*use, use->owner->variantCount);
        if (!evaluate(*use)) { use->graph.reset(); return; }
        const auto captured = use->graph->CompareCapture(use->values, *capture);
        const auto materialized = use->graph->CompareMaterialized(use->values, compiled);
        ++stats.materializedChecks;
        notePopulation(use->populationMask, PopulationStage::MaterializedChecks);
        if (captured != DispatchGatherDifference::None || materialized != DispatchGatherDifference::None) {
            const bool issuedMoved = noteSerialMovement(use->issued, use->before, SerialPhase::LearnIssued, false);
            const bool gatheredMoved = noteSerialMovement(use->before, serials(), SerialPhase::LearnCompare, false);
            if (issuedMoved || gatheredMoved) { ++stats.serialMoved; use->graph.reset(); return; }
            if (use->graph->RestoredPredicates()) {
                noteRefusal(*use, DispatchGatherRefusal::PredicateOldPathDisagreement);
                use->graph.reset();
                return;
            }
            ++stats.materializedMismatch; notePopulation(use->populationMask, PopulationStage::MaterializedMismatch);
            std::fprintf(stderr, "[dispatch-gather-shadow] learning program=0x%llx capture=%s materialized=%s descriptors=%zu flat=%zu bindings=%zu push=%zu\n", static_cast<unsigned long long>(use->program), DispatchGatherDifferenceName(captured), DispatchGatherDifferenceName(materialized), use->values.descriptors.size(), use->values.flat.size(), use->values.bindings.size(), use->values.pushConstants.size());
            mismatch("learning capture/materialization differs from complete gather");
            use->graph.reset();
            return;
        }
        if (noteSerialMovement(use->before, serials(), SerialPhase::LearnEnd)) { use->graph.reset(); return; }
        if (!registeredViewsCurrent(*use)) { noteRefusal(*use, DispatchGatherRefusal::RegisteredViews); use->graph.reset(); return; }
        if (!use->graph->SetStructure(use->values)) { use->graph.reset(); return; }
        const auto variant = use->owner->variantCount++;
        use->owner->variants[variant] = use->graph;
        use->owner->lastMatch = variant;
        ++stats.variantsLearned;
        ++stats.variantLearnedSlots[variant];
        std::fprintf(stderr, "[dispatch-gather-variant] candidate=%llu program=0x%llx queue=0x%x learned=%zu limit=%zu\n", static_cast<unsigned long long>(use->owner->id), static_cast<unsigned long long>(use->program), use->queue, variant + 1u, VariantLimit);
        ++stats.templatesBuilt;
        if (!cached) ++stats.learnedOnMiss;
        use->matched = use->verified = true;
        ++stats.verifiedResolutions;
        ++stats.matched;
        ++stats.gathered;
        notePopulation(use->populationMask, PopulationStage::Matched);
        notePopulation(use->populationMask, PopulationStage::Gathered);
        return;
    }
    ++stats.materializedChecks;
    notePopulation(use->populationMask, PopulationStage::MaterializedChecks);
    const auto difference = use->graph->CompareMaterialized(use->values, compiled);
    const auto captured = capture && !cached ? use->graph->CompareCapture(use->values, *capture) : DispatchGatherDifference::None;
    if (noteSerialMovement(use->before, serials(), SerialPhase::MaterializedCompare)) { ++stats.materializedLostSerial; use->matched = false; return; }
    if (difference != DispatchGatherDifference::None || captured != DispatchGatherDifference::None) {
        ++stats.falseMatch;
        notePopulation(use->populationMask, PopulationStage::FalseMatch);
        ++stats.materializedMismatch; notePopulation(use->populationMask, PopulationStage::MaterializedMismatch);
        use->matched = false;
        std::fprintf(stderr, "[dispatch-gather-shadow] verify program=0x%llx capture=%s materialized=%s\n", static_cast<unsigned long long>(use->program), DispatchGatherDifferenceName(captured), DispatchGatherDifferenceName(difference));
        mismatch("matched template differs from unchanged materialization");
        return;
    }
    if (!registeredViewsCurrent(*use)) { ++stats.materializedLostViews; noteRefusal(*use, DispatchGatherRefusal::RegisteredViews); use->matched = false; return; }
    use->verified = true;
    ++stats.verifiedResolutions;
    if (!cached && !differing) ++stats.absentMatched;
    if (differing) ++stats.differingMatched;
}
void SetPayload(const std::shared_ptr<Use>& use, const std::array<std::uint32_t, 3>& groups, std::uint64_t arguments, const std::array<std::uint32_t, 3>& threadGroup) {
    if (!use || !use->verified) return;
    if (threadGroup != std::array<std::uint32_t, 3>{}) { ++stats.commitRefused; ++stats.commitRefusals[4]; return; }
    use->commit = {&use->values, use->arguments != 0 && arguments == 0 ? use->indirect : groups, arguments, threadGroup};
    use->payload = true;
    if (arguments != 0 && arguments != use->arguments) {
        use->payload = false;
        ++stats.materializedMismatch; notePopulation(use->populationMask, PopulationStage::MaterializedMismatch);
        mismatch("indirect source changed between gather and materialization");
    }
    if (use->payload) ++stats.payloadReady;
}
void Queued(const std::shared_ptr<Use>& use, bool readsClear, bool pipelined) {
    if (use && !use->inlineCommit.load(std::memory_order_acquire) && use->verified && use->payload && readsClear && pipelined && drainRequests() == use->drainRequests) {
        ++stats.queuedWithoutDrain;
        notePopulation(use->populationMask, PopulationStage::QueuedWithoutDrain);
        if (use->stages.fetch_or(1u, std::memory_order_acq_rel) == 2u) { ++stats.queuedCommittedWithoutRebuild; notePopulation(use->populationMask, PopulationStage::QueuedCommittedWithoutRebuild); }
    }
}
const CommitInput* ActiveCommit() {
    const auto* active = ActiveSlot::Get();
    return active && active->verified && active->payload ? &active->commit : nullptr;
}
void CommitResult(bool compatible, bool supported, bool rebuilt, const char* reason) {
    if (!ActiveCommit()) return;
    auto* active = ActiveSlot::Get();
    if (!supported) {
        ++stats.commitRefused;
        const auto found = std::ranges::find(commitRefusalNames, std::string_view(reason));
        const auto index = found == commitRefusalNames.end() ? 0u : static_cast<std::size_t>(found - commitRefusalNames.begin());
        ++stats.commitRefusals[index];
        return;
    }
    ++stats.commitChecks;
    notePopulation(active->populationMask, PopulationStage::CommitChecks);
    if (!compatible) {
        ++stats.commitMismatch;
        notePopulation(active->populationMask, PopulationStage::CommitMismatch);
        mismatch(reason);
        return;
    }
    if (!rebuilt && !active->rebuilt) {
        ++stats.committedWithoutRebuild;
        notePopulation(active->populationMask, PopulationStage::CommittedWithoutRebuild);
        if (active->stages.fetch_or(2u, std::memory_order_acq_rel) == 1u) { ++stats.queuedCommittedWithoutRebuild; notePopulation(active->populationMask, PopulationStage::QueuedCommittedWithoutRebuild); }
    }
}
void NoteWriterEffects(bool compatible, bool supported, std::uint32_t units) {
    const auto* active = ActiveSlot::Get();
    if (active == nullptr || !supported) return;
    notePopulation(active->populationMask, PopulationStage::WriterEffectChecks);
    if (units > 1u) ++stats.writerMultiUnitChecks;
    if (!compatible) notePopulation(active->populationMask, PopulationStage::WriterEffectMismatch);
}
void MarkRebuild() { if (auto* active = ActiveSlot::Get()) active->rebuilt = true; }
CommitScope::CommitScope(const std::shared_ptr<Use>& use) : previous(ActiveSlot::Get()) {
    auto* active = use.get();
    ActiveSlot::Set(active);
    if (active && active->issuer == std::this_thread::get_id()) active->inlineCommit.store(true, std::memory_order_release);
}
CommitScope::~CommitScope() { ActiveSlot::Set(previous); }
}
