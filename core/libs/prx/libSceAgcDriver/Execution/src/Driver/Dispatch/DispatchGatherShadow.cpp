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
    std::atomic<std::uint64_t> commitMismatch{0};
    std::atomic<std::uint64_t> commitRefused{0};
    std::array<std::atomic<std::uint64_t>, 12> commitRefusals{};
    std::atomic<std::uint64_t> nanoseconds{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DispatchGatherRefusal::Count)> graphRefusals{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DispatchGatherDifference::Count)> differences{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DispatchGatherDifference::Count)> falseDifferences{};
};
Statistics stats;
constexpr std::array<std::string_view, 12> commitRefusalNames{"unknown", "no_payload", "overlay", "private_commands", "thread_dimensions", "indirect_payload", "split_groups", "unobserved_upload", "address_table", "fault_buffer", "binding_role", "resource_state"};
std::atomic<void (*)(std::uint64_t, std::size_t, bool)> readHook{nullptr};
using Serials = std::array<std::uint64_t, 8>;
Serials serials() {
    return {GuestMemory::ForgetSerial(), GuestMemory::ProofSerial(), GuestMemory::ResourceProofSerial(), GuestMemory::WriteGeneration(), Graphics::Recorder::PublishGeneration(), Graphics::Recorder::WriteGeneration(), Graphics::StorageTexture::PendingSerial(), DrawPipeline::EpochToken().load(std::memory_order_acquire)};
}
struct Timer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~Timer() { stats.nanoseconds.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count(), std::memory_order_relaxed); }
};
void registerCounters() {
    static const bool registered = [] {
        ArchB::Recorder::RegisterCounter("dispatch_gather_calls", stats.calls);
        ArchB::Recorder::RegisterCounter("dispatch_gather_ahead_refused", stats.aheadRefused);
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
        ArchB::Recorder::RegisterCounter("dispatch_gather_commit_mismatch", stats.commitMismatch);
        ArchB::Recorder::RegisterCounter("dispatch_gather_commit_refused", stats.commitRefused);
        ArchB::Recorder::RegisterCounter("dispatch_gather_nanoseconds", stats.nanoseconds);
        static std::array<std::string, 12> commitRefusals;
        for (std::size_t i = 0; i < commitRefusals.size(); ++i) {
            commitRefusals[i] = std::string("dispatch_gather_commit_refused_") + std::string(commitRefusalNames[i]);
            ArchB::Recorder::RegisterCounter(commitRefusals[i].c_str(), stats.commitRefusals[i]);
        }
        static std::array<std::string, static_cast<std::size_t>(DispatchGatherRefusal::Count)> refusals;
        static std::array<std::string, static_cast<std::size_t>(DispatchGatherDifference::Count)> differences;
        for (std::size_t i = 1; i < refusals.size(); ++i) {
            refusals[i] = std::string("dispatch_gather_refused_") + DispatchGatherRefusalName(static_cast<DispatchGatherRefusal>(i));
            ArchB::Recorder::RegisterCounter(refusals[i].c_str(), stats.graphRefusals[i]);
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
    if (copied) ++stats.guardedReads;
    else {
        copied = GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span{value, std::size_t{1}}), true) == GuestMemory::Compare::Equal;
        if (copied) ++stats.fallbackReads;
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
    auto refusal = use.graph->Evaluate(use.userData, use.program, readWord, &use, use.values);
    if (refusal == DispatchGatherRefusal::None && use.arguments != 0) {
        for (std::size_t i = 0; i < use.indirect.size(); ++i) if (!readWord(&use, use.arguments + i * sizeof(std::uint32_t), &use.indirect[i])) { refusal = DispatchGatherRefusal::ReadFailed; break; }
    }
    if (serials() != use.before) {
        if (!use.readFailed) ++stats.serialMoved;
        use.readFailed = true;
        refusal = DispatchGatherRefusal::ReadFailed;
    }
    if (refusal != DispatchGatherRefusal::None) {
        ++stats.graphRefusals[static_cast<std::size_t>(refusal)];
        return false;
    }
    ++stats.evaluated;
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
    result.commitMismatch = stats.commitMismatch.load(std::memory_order_relaxed);
    result.commitRefused = stats.commitRefused.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.commitRefusals.size(); ++i) result.commitRefusals[i] = stats.commitRefusals[i].load(std::memory_order_relaxed);
    result.nanoseconds = stats.nanoseconds.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.graphRefusals.size(); ++i) result.graphRefusals[i] = stats.graphRefusals[i].load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.differences.size(); ++i) result.differences[i] = stats.differences[i].load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.falseDifferences.size(); ++i) result.falseDifferences[i] = stats.falseDifferences[i].load(std::memory_order_relaxed);
    return result;
}
void NoteVerificationNanoseconds(std::uint64_t nanoseconds) {
    stats.nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
    stats.commitNanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
}
void SetReadHook(void (*hook)(std::uint64_t, std::size_t, bool)) { readHook.store(hook, std::memory_order_release); }
std::shared_ptr<Use> Begin(const ShaderRecompiler::RecompileRequest& request, const std::shared_ptr<const ShaderSnapshot>& shader, std::uint32_t queue, std::uint64_t arguments, bool fromAhead, bool resolving) {
    if (!Enabled() || resolving) return {};
    registerCounters();
    Timer timer;
    ++stats.calls;
    if (fromAhead) { ++stats.aheadRefused; return {}; }
    if (arguments > std::numeric_limits<std::uint64_t>::max() - 3 * sizeof(std::uint32_t)) { ++stats.unreadable; return {}; }
    auto& index = threadIndex();
    ShaderRecompiler::BuildPreparedShaderKey(request, index.key);
    Candidate* candidate = nullptr;
    for (std::size_t i = 0; i < index.count; ++i) {
        auto& entry = *index.candidates[i];
        if (entry.program == request.shader.codeAddress && entry.shader == shader && entry.key == index.key && std::ranges::equal(entry.code, request.shader.code)) { candidate = &entry; break; }
    }
    if (candidate == nullptr) {
        ++stats.indexMiss;
        if (index.count == CandidateLimit) { ++stats.indexFull; return {}; }
        auto entry = std::make_unique<Candidate>();
        entry->key = index.key;
        entry->code.assign(request.shader.code.begin(), request.shader.code.end());
        entry->calls = !request.context.linkedCallFallbacks.empty() || std::ranges::any_of(request.shader.code, [](std::uint32_t word) { return (word & 0xff80ff00u) == 0xbe802100u; });
        entry->shader = shader;
        entry->program = request.shader.codeAddress;
        candidate = entry.get();
        index.candidates[index.count++] = std::move(entry);
    }
    if (candidate->calls) { ++stats.callsRefused; return {}; }
    std::shared_ptr<Use> use;
    for (auto& slot : candidate->uses) {
        if (!slot) { slot = std::make_shared<Use>(); slot->owner = candidate; }
        if (slot.use_count() == 1) { use = slot; break; }
    }
    if (!use) { ++stats.poolBusy; return {}; }
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
    if (use->graph && evaluate(*use)) {
        use->difference = use->graph->Matches(use->values);
        use->matched = use->difference == DispatchGatherDifference::None;
        if (use->matched) { ++stats.matched; ++stats.gathered; }
        else ++stats.differences[static_cast<std::size_t>(use->difference)];
    }
    return use;
}
void Resolved(const std::shared_ptr<Use>& use, const ShaderRecompiler::ResourceCapture* capture, const ShaderRecompiler::RecompileResult& compiled, bool cached, bool validated, bool differing) {
    if (!use) return;
    Timer timer;
    if (use->graph && use->matched && serials() != use->before) {
        ++stats.serialMoved;
        use->matched = false;
        return;
    }
    if (!use->graph) {
        if (use->owner->refusal != DispatchGatherRefusal::None) { ++stats.graphRefusals[static_cast<std::size_t>(use->owner->refusal)]; return; }
        if (!capture) { ++stats.graphRefusals[static_cast<std::size_t>(DispatchGatherRefusal::MissingPlan)]; return; }
        DispatchGatherRefusal refusal;
        use->graph = DispatchGatherGraph::Create(*capture, compiled, refusal);
        if (!use->graph) {
            if (refusal != DispatchGatherRefusal::AbsentBuffer && refusal != DispatchGatherRefusal::Poison && refusal != DispatchGatherRefusal::Unbounded && refusal != DispatchGatherRefusal::BindingLayout && refusal != DispatchGatherRefusal::UserData) use->owner->refusal = refusal;
            ++stats.graphRefusals[static_cast<std::size_t>(refusal)];
            return;
        }
        if (!evaluate(*use)) { use->graph.reset(); return; }
        const auto captured = use->graph->CompareCapture(use->values, *capture);
        const auto materialized = use->graph->CompareMaterialized(use->values, compiled);
        ++stats.materializedChecks;
        if (captured != DispatchGatherDifference::None || materialized != DispatchGatherDifference::None) {
            if (use->issued != use->before || serials() != use->before) { ++stats.serialMoved; use->graph.reset(); return; }
            ++stats.materializedMismatch;
            std::fprintf(stderr, "[dispatch-gather-shadow] learning program=0x%llx capture=%s materialized=%s descriptors=%zu flat=%zu bindings=%zu push=%zu\n", static_cast<unsigned long long>(use->program), DispatchGatherDifferenceName(captured), DispatchGatherDifferenceName(materialized), use->values.descriptors.size(), use->values.flat.size(), use->values.bindings.size(), use->values.pushConstants.size());
            mismatch("learning capture/materialization differs from complete gather");
            use->graph.reset();
            return;
        }
        if (serials() != use->before) { ++stats.serialMoved; use->graph.reset(); return; }
        if (!use->graph->SetStructure(use->values)) { use->graph.reset(); return; }
        use->owner->graph = use->graph;
        ++stats.templatesBuilt;
        if (!cached) ++stats.learnedOnMiss;
        use->matched = use->verified = true;
        ++stats.matched;
        ++stats.gathered;
        return;
    }
    if (!use->matched) {
        if (use->values.gathered && !use->readFailed && cached && validated) { ++stats.falseDiffer; ++stats.falseDifferences[static_cast<std::size_t>(use->difference)]; }
        return;
    }
    ++stats.materializedChecks;
    const auto difference = use->graph->CompareMaterialized(use->values, compiled);
    const auto captured = capture && !cached ? use->graph->CompareCapture(use->values, *capture) : DispatchGatherDifference::None;
    if (serials() != use->before) { ++stats.serialMoved; use->matched = false; return; }
    if (difference != DispatchGatherDifference::None || captured != DispatchGatherDifference::None) {
        ++stats.falseMatch;
        ++stats.materializedMismatch;
        use->matched = false;
        std::fprintf(stderr, "[dispatch-gather-shadow] verify program=0x%llx capture=%s materialized=%s\n", static_cast<unsigned long long>(use->program), DispatchGatherDifferenceName(captured), DispatchGatherDifferenceName(difference));
        mismatch("matched template differs from unchanged materialization");
        return;
    }
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
        ++stats.materializedMismatch;
        mismatch("indirect source changed between gather and materialization");
    }
}
void Queued(const std::shared_ptr<Use>& use, bool readsClear, bool pipelined) {
    if (use && !use->inlineCommit.load(std::memory_order_acquire) && use->verified && use->payload && readsClear && pipelined && drainRequests() == use->drainRequests) {
        ++stats.queuedWithoutDrain;
        if (use->stages.fetch_or(1u, std::memory_order_acq_rel) == 2u) ++stats.queuedCommittedWithoutRebuild;
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
    if (!compatible) {
        ++stats.commitMismatch;
        mismatch(reason);
        return;
    }
    if (!rebuilt && !active->rebuilt) {
        ++stats.committedWithoutRebuild;
        if (active->stages.fetch_or(2u, std::memory_order_acq_rel) == 1u) ++stats.queuedCommittedWithoutRebuild;
    }
}
void MarkRebuild() { if (auto* active = ActiveSlot::Get()) active->rebuilt = true; }
CommitScope::CommitScope(const std::shared_ptr<Use>& use) : previous(ActiveSlot::Get()) {
    auto* active = use.get();
    ActiveSlot::Set(active);
    if (active && active->issuer == std::this_thread::get_id()) active->inlineCommit.store(true, std::memory_order_release);
}
CommitScope::~CommitScope() { ActiveSlot::Set(previous); }
}
