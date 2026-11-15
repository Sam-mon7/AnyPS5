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
    std::atomic<std::uint64_t> poolBusy{0};
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
constexpr std::array<std::string_view, 18> commitRefusalNames{"unknown", "no_payload", "overlay", "private_commands", "thread_dimensions", "indirect_payload", "split_groups", "unobserved_upload", "address_table", "fault_buffer", "binding_role", "resource_state", "writer_mode", "writer_shape", "writer_alias", "writer_depth", "writer_unobserved", "writer_epoch_suppressed"};
std::atomic<void (*)(std::uint64_t, std::size_t, bool)> readHook{nullptr};
using Serials = std::array<std::uint64_t, 8>;
Serials serials() {
    return {GuestMemory::ForgetSerial(), GuestMemory::ProofSerial(), GuestMemory::ResourceProofSerial(), GuestMemory::WriteGeneration(), Graphics::Recorder::PublishGeneration(), Graphics::Recorder::WriteGeneration(), Graphics::StorageTexture::PendingSerial(), DrawPipeline::EpochToken().load(std::memory_order_acquire)};
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
        ArchB::Recorder::RegisterCounter("dispatch_gather_pool_busy", stats.poolBusy);
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
    std::shared_ptr<const DispatchGatherGraph> graph;
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
bool readWord(void* context, std::uint64_t address, std::uint32_t* value) {
    auto& use = *static_cast<Use*>(context);
    if (use.readFailed) return false;
    const auto refuse = [&](std::atomic<std::uint64_t>& counter) { ++counter; use.readFailed = true; return false; };
    if (const auto hook = readHook.load(std::memory_order_acquire)) hook(address, sizeof(*value), false);
    if (address == 0 || address > std::numeric_limits<std::uint64_t>::max() - sizeof(*value)) return refuse(stats.unreadable);
    if (serials() != use.before || (use.before[0] & 1u) != 0u) return refuse(stats.serialMoved);
    if (DrawPipeline::Queue0().Overlaps(address, sizeof(*value)) || DrawFinisher::Get().PendingOverlaps(address, address + sizeof(*value))) return refuse(stats.queued);
    use.pendingView.Load();
    if (use.pendingView.Overlaps(address, sizeof(*value))) return refuse(stats.pending);
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> ranges{{{address, address + sizeof(*value)}}};
    if (Graphics::StorageTexture::AnyPendingOverlaps(ranges)) return refuse(stats.pending);
    if (Graphics::AnyShadowedOverlaps(address, sizeof(*value))) return refuse(stats.shadowed);
    for (const auto& label : deferredLabels().labels) if (label.address < address + sizeof(*value) && address < label.address + label.size) return refuse(stats.labels);
    bool copied = false;
#ifdef _WIN32
    copied = GuestArena::GuestArenaCopyOwned_nid_no_patch(address, value, sizeof(*value));
#endif
    if (copied) { ++stats.guardedReads; notePopulation(use.populationMask, PopulationStage::GuardedReads); }
    else {
        copied = GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span{value, std::size_t{1}}), true) == GuestMemory::Compare::Equal;
        if (copied) { ++stats.fallbackReads; notePopulation(use.populationMask, PopulationStage::FallbackReads); }
    }
    if (!copied) return refuse(stats.unreadable);
    if (const auto hook = readHook.load(std::memory_order_acquire)) hook(address, sizeof(*value), true);
    if (serials() != use.before) return refuse(stats.serialMoved);
    return true;
}
bool evaluate(Use& use) {
    use.attempted = true;
    use.before = serials();
    use.pendingView = {};
    use.readFailed = false;
    if ((use.before[0] & 1u) != 0u) { ++stats.serialMoved; return false; }
    if (!registeredViewsCurrent(use)) { noteRefusal(use, DispatchGatherRefusal::RegisteredViews); return false; }
    auto refusal = use.graph->Evaluate(use.userData, use.program, readWord, &use, use.values);
    if (refusal == DispatchGatherRefusal::None && use.arguments != 0) {
        for (std::size_t i = 0; i < use.indirect.size(); ++i) if (!readWord(&use, use.arguments + i * sizeof(std::uint32_t), &use.indirect[i])) { refusal = DispatchGatherRefusal::ReadFailed; break; }
    }
    if (serials() != use.before) {
        if (!use.readFailed) ++stats.serialMoved;
        use.readFailed = true;
        refusal = DispatchGatherRefusal::ReadFailed;
    }
    if (refusal == DispatchGatherRefusal::None && !registeredViewsCurrent(use)) refusal = DispatchGatherRefusal::RegisteredViews;
    if (refusal != DispatchGatherRefusal::None) {
        noteRefusal(use, refusal);
        return false;
    }
    ++stats.evaluated;
    notePopulation(use.populationMask, PopulationStage::Evaluated);
    return true;
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
    result.poolBusy = stats.poolBusy.load(std::memory_order_relaxed);
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
    use->graph = candidate->graph;
    use->userData.assign(request.context.userData.begin(), request.context.userData.end());
    use->program = request.shader.codeAddress;
    use->arguments = arguments;
    use->queue = queue;
    use->issuer = std::this_thread::get_id();
    use->inlineCommit.store(false, std::memory_order_relaxed);
    use->stages.store(0, std::memory_order_relaxed);
    use->drainRequests = drainRequests();
    use->issued = serials();
    use->readFailed = use->attempted = use->matched = use->verified = use->payload = use->rebuilt = false;
    use->values.gathered = false;
    use->difference = DispatchGatherDifference::None;
    use->commit = {};
    classify(*use, use->graph ? &use->graph->Plan() : nullptr);
    if (!fromAhead && use->graph && evaluate(*use)) {
        use->difference = use->graph->Matches(use->values);
        use->matched = use->difference == DispatchGatherDifference::None;
        if (use->matched) { ++stats.matched; ++stats.gathered; notePopulation(use->populationMask, PopulationStage::Matched); notePopulation(use->populationMask, PopulationStage::Gathered); }
        else ++stats.differences[static_cast<std::size_t>(use->difference)];
    }
    return use;
}
void AfterAdoption(const std::shared_ptr<Use>& use, bool adopted) {
    if (adopted) ++stats.aheadAdopted;
    else ++stats.aheadFallback;
    if (!use) return;
    Timer timer{&use->populationMask};
    use->issued = serials();
    if (use->graph && evaluate(*use)) {
        use->difference = use->graph->Matches(use->values);
        use->matched = use->difference == DispatchGatherDifference::None;
        if (use->matched) { ++stats.matched; ++stats.gathered; notePopulation(use->populationMask, PopulationStage::Matched); notePopulation(use->populationMask, PopulationStage::Gathered); }
        else ++stats.differences[static_cast<std::size_t>(use->difference)];
    }
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
    if (use->graph && use->values.gathered && serials() != use->before) {
        ++stats.serialMoved;
        use->matched = false;
        return;
    }
    if (use->graph && use->values.gathered && !registeredViewsCurrent(*use)) { noteRefusal(*use, DispatchGatherRefusal::RegisteredViews); use->matched = false; return; }
    if (!use->graph) {
        if (use->owner->refusal != DispatchGatherRefusal::None) {
            noteRefusalDetail(use->owner->refusalDetail);
            noteRefusal(*use, use->owner->refusal);
            return;
        }
        if (!capture) { noteRefusal(*use, DispatchGatherRefusal::MissingPlan); return; }
        DispatchGatherRefusal refusal;
        use->graph = DispatchGatherGraph::Create(*capture, compiled, refusal);
        if (!use->graph) {
            const auto* detail = DispatchGatherGraph::LastRefusalDetail();
            noteRefusalDetail(detail);
            use->owner->refusalDetail = detail;
            if (refusal != DispatchGatherRefusal::AbsentBuffer && refusal != DispatchGatherRefusal::Poison && refusal != DispatchGatherRefusal::Unbounded && refusal != DispatchGatherRefusal::BindingLayout && refusal != DispatchGatherRefusal::UserData && refusal != DispatchGatherRefusal::RegisteredViews && refusal != DispatchGatherRefusal::ImageNormalization) use->owner->refusal = refusal;
            noteRefusal(*use, refusal);
            return;
        }
        if (!evaluate(*use)) { use->graph.reset(); return; }
        const auto captured = use->graph->CompareCapture(use->values, *capture);
        const auto materialized = use->graph->CompareMaterialized(use->values, compiled);
        ++stats.materializedChecks;
        notePopulation(use->populationMask, PopulationStage::MaterializedChecks);
        if (captured != DispatchGatherDifference::None || materialized != DispatchGatherDifference::None) {
            if (use->issued != use->before || serials() != use->before) { ++stats.serialMoved; use->graph.reset(); return; }
            ++stats.materializedMismatch; notePopulation(use->populationMask, PopulationStage::MaterializedMismatch);
            std::fprintf(stderr, "[dispatch-gather-shadow] learning program=0x%llx capture=%s materialized=%s descriptors=%zu flat=%zu bindings=%zu push=%zu\n", static_cast<unsigned long long>(use->program), DispatchGatherDifferenceName(captured), DispatchGatherDifferenceName(materialized), use->values.descriptors.size(), use->values.flat.size(), use->values.bindings.size(), use->values.pushConstants.size());
            mismatch("learning capture/materialization differs from complete gather");
            use->graph.reset();
            return;
        }
        if (serials() != use->before) { ++stats.serialMoved; use->graph.reset(); return; }
        if (!registeredViewsCurrent(*use)) { noteRefusal(*use, DispatchGatherRefusal::RegisteredViews); use->graph.reset(); return; }
        if (!use->graph->SetStructure(use->values)) { use->graph.reset(); return; }
        use->owner->graph = use->graph;
        ++stats.templatesBuilt;
        if (!cached) ++stats.learnedOnMiss;
        use->matched = use->verified = true;
        ++stats.matched;
        ++stats.gathered;
        notePopulation(use->populationMask, PopulationStage::Matched);
        notePopulation(use->populationMask, PopulationStage::Gathered);
        return;
    }
    if (!use->matched) {
        if (use->values.gathered && !use->readFailed && use->refusal == DispatchGatherRefusal::None && use->difference != DispatchGatherDifference::None && cached && validated) {
            if (serials() != use->before) { ++stats.serialMoved; return; }
            ++stats.falseDiffer;
            notePopulation(use->populationMask, PopulationStage::FalseDiffer);
            ++stats.falseDifferences[static_cast<std::size_t>(use->difference)];
        }
        return;
    }
    ++stats.materializedChecks;
    notePopulation(use->populationMask, PopulationStage::MaterializedChecks);
    const auto difference = use->graph->CompareMaterialized(use->values, compiled);
    const auto captured = capture && !cached ? use->graph->CompareCapture(use->values, *capture) : DispatchGatherDifference::None;
    if (serials() != use->before) { ++stats.serialMoved; use->matched = false; return; }
    if (difference != DispatchGatherDifference::None || captured != DispatchGatherDifference::None) {
        ++stats.falseMatch;
        notePopulation(use->populationMask, PopulationStage::FalseMatch);
        ++stats.materializedMismatch; notePopulation(use->populationMask, PopulationStage::MaterializedMismatch);
        use->matched = false;
        std::fprintf(stderr, "[dispatch-gather-shadow] verify program=0x%llx capture=%s materialized=%s\n", static_cast<unsigned long long>(use->program), DispatchGatherDifferenceName(captured), DispatchGatherDifferenceName(difference));
        mismatch("matched template differs from unchanged materialization");
        return;
    }
    if (!registeredViewsCurrent(*use)) { noteRefusal(*use, DispatchGatherRefusal::RegisteredViews); use->matched = false; return; }
    use->verified = true;
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
