#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawHitCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/WriteEvidence.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include <algorithm>

namespace AgcDriver::DriverDetail {

bool DrawHitCensusEnabled() {
    return ArchB::Switches::Enabled(ArchB::Feature::DrawHitCensus);
}

DrawHitCensus::DrawHitCensus(const std::shared_ptr<DrawEntry>& candidate, bool registerKey, std::span<const std::uint32_t> words, const Pm4::DrawParameters& parameters, const Graphics::State& graphics, const std::vector<DrawProgram>& currentPrograms) : enabled(DrawHitCensusEnabled()) {
    if (!enabled) return;
    entry = candidate;
    programs = &currentPrograms;
    packet = words;
    indirect = parameters.indirect.has_value();
    arguments = {parameters.indexAddress, parameters.indexCount, parameters.indexSize, parameters.instanceCount, parameters.flags, parameters.indexed, parameters.firstVertex, parameters.firstInstance};
    ArchB::Recorder::Count("j3_lookup_calls");
    ArchB::Recorder::Count(entry->donor == nullptr ? "j3_direct_calls" : "j3_relocated_calls");
    ArchB::Recorder::Count(parameters.indirect ? "j3_indirect_calls" : "j3_nonindirect_calls");
    const char* reason = !registerKey ? "j3_refused_no_register_key" : entry->donor != nullptr ? "j3_refused_relocated" : parameters.indirect ? "j3_refused_indirect" : graphics.stages.path != Graphics::ShaderPath::Vertex || graphics.stages.mesh || graphics.stages.tessellation ? "j3_refused_shader_path" : words.empty() ? "j3_refused_no_packet" : nullptr;
    eligible = reason == nullptr;
    if (eligible) {
        const auto probeStarted = std::chrono::steady_clock::now();
        ArchB::Recorder::Count("j3_eligible_calls");
        const auto previous = entry->hitObservation.load();
        reason = previous == nullptr ? "j3_refused_no_observation" : previous->packet.size() != words.size() || !std::equal(previous->packet.begin(), previous->packet.end(), words.begin()) ? "j3_refused_packet" : previous->arguments != arguments ? "j3_refused_arguments" : refusal(*previous);
        if (reason == nullptr) {
            predicted = previous;
            ArchB::Recorder::Count("j3_predicted_hits");
        }
        const auto probeNs = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - probeStarted).count();
        ArchB::Recorder::Count("j3_probe_ns", probeNs);
        if (predicted != nullptr) ArchB::Recorder::Count("j3_accepted_probe_ns", probeNs);
    }
    if (reason != nullptr) ArchB::Recorder::Count(reason);
    fault = GuestMemory::FaultClock();
    stored = GuestMemory::WriteGeneration();
    forget = GuestMemory::ForgetSerial();
    allocations = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    waitedMs = Graphics::Recorder::ThreadWaitedMs();
    began = std::chrono::steady_clock::now();
}

const char* DrawHitCensus::refusal(const DrawHitObservation& observation) const {
    const auto publish = Graphics::Recorder::PublishGeneration();
    const auto pendingSerial = Graphics::StorageTexture::PendingSerial();
    const auto currentForget = GuestMemory::ForgetSerial();
    const auto currentAllocations = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    if ((currentForget & 1u) != 0 || currentForget != observation.forget) return "j3_refused_forget";
    if (currentAllocations != observation.allocations) return "j3_refused_allocations";
    if (observation.userData.size() != programs->size() || observation.stages.size() != programs->size()) return "j3_refused_stage_count";
    for (std::size_t i = 0; i < programs->size(); ++i) {
        if (observation.userData[i] != (*programs)[i].userData) return "j3_refused_user_words";
        const auto& stage = observation.stages[i];
        if (stage == nullptr || stage->compiled == nullptr || stage->donor != nullptr || stage->nestedRelocated || !stage->shaderDataPatches.empty() || !stage->descriptorPatches.empty()) return "j3_refused_stage_patch";
        if (stage->registeredViews != nullptr && !CaptureRegisteredViewsCurrent(*stage->registeredViews)) return "j3_refused_registered_views";
    }
    if (DrawPipeline::Active() && DrawPipeline::Queue0().Busy() && DrawPipeline::Queue0().OverlapsAny(observation.runs)) return "j3_refused_queued_writes";
    PendingView pending;
    pending.Load();
    for (const auto& [begin, end] : observation.runs) {
        if (pending.Overlaps(begin, static_cast<std::size_t>(end - begin))) return "j3_refused_pending_writes";
        for (const auto& label : deferredLabels().labels) {
            if (label.address < end && begin < label.address + label.size) return "j3_refused_labels";
        }
    }
    if (Graphics::StorageTexture::AnyPendingOverlaps(observation.runs)) return "j3_refused_pending_images";
    if (Graphics::AnyShadowedOverlaps(observation.runs)) return "j3_refused_shadowed_images";
    if (!observation.runs.empty() && (observation.fault == 0 || observation.stored == 0)) return "j3_refused_untracked";
    for (const auto& [begin, end] : observation.runs) {
        if (GuestMemory::FaultUntracked(begin, static_cast<std::size_t>(end - begin), observation.fault)) return "j3_refused_untracked";
        if (!GuestMemory::FaultUnchangedSince(begin, static_cast<std::size_t>(end - begin), observation.fault)) return "j3_refused_cpu_written";
    }
    if (!observation.queries.empty() && !GuestMemory::UnchangedSinceAll(observation.queries)) return "j3_refused_stored";
    if (Graphics::Recorder::PublishGeneration() != publish || Graphics::StorageTexture::PendingSerial() != pendingSerial || GuestMemory::ForgetSerial() != currentForget || GuestAllocations::GuestAllocationsGeneration_nid_postfix() != currentAllocations) return "j3_refused_serial_moved";
    return nullptr;
}

void DrawHitCensus::Finish(bool hit, bool exact, const std::vector<std::shared_ptr<DispatchVariant>>& matched) {
    if (!enabled) return;
    const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - began).count();
    const auto waited = std::max(0.0, Graphics::Recorder::ThreadWaitedMs() - waitedMs) * 1000000.0;
    const auto nanoseconds = std::max(0.0, elapsed - waited);
    ArchB::Recorder::Count(hit ? "j3_hit_lookup_wait_ns" : "j3_miss_lookup_wait_ns", waited);
    ArchB::Recorder::Count(hit ? "j3_hit_lookup_ns" : "j3_miss_lookup_ns", nanoseconds);
    ArchB::Recorder::Count(hit ? "j3_old_hits" : "j3_old_misses");
    if (hit) {
        ArchB::Recorder::Count(entry->donor == nullptr ? "j3_direct_hit_lookup_ns" : "j3_relocated_hit_lookup_ns", nanoseconds);
        ArchB::Recorder::Count(indirect ? "j3_indirect_hits" : "j3_nonindirect_hits");
        ArchB::Recorder::Count(indirect ? "j3_indirect_hit_lookup_ns" : "j3_nonindirect_hit_lookup_ns", nanoseconds);
    }
    if (predicted != nullptr) {
        const bool same = hit && exact && predicted->stages == matched;
        ArchB::Recorder::Count(same ? "j3_prediction_verified" : "j3_prediction_mismatch");
        ArchB::Recorder::Count("j3_predicted_hit_lookup_ns", nanoseconds);
    }
    if (!eligible || !hit || !exact) {
        if (hit && !exact) ArchB::Recorder::Count("j3_hit_not_exact");
        return;
    }
    auto next = std::make_shared<DrawHitObservation>();
    next->fault = fault;
    next->stored = stored;
    next->forget = forget;
    next->allocations = allocations;
    next->packet.assign(packet.begin(), packet.end());
    next->arguments = arguments;
    next->stages = matched;
    for (const auto& program : *programs) next->userData.push_back(program.userData);
    for (const auto& stage : matched) {
        if (stage == nullptr) return;
        next->runs.insert(next->runs.end(), stage->runs.begin(), stage->runs.end());
    }
    std::sort(next->runs.begin(), next->runs.end());
    std::size_t used = 0;
    for (const auto& run : next->runs) {
        if (run.first >= run.second) continue;
        if (used != 0 && run.first <= next->runs[used - 1].second) next->runs[used - 1].second = std::max(next->runs[used - 1].second, run.second);
        else next->runs[used++] = run;
    }
    next->runs.resize(used);
    std::uint64_t bytes = 0;
    for (const auto& [begin, end] : next->runs) {
        next->queries.push_back({begin, static_cast<std::size_t>(end - begin), stored});
        bytes += end - begin;
    }
    ArchB::Recorder::Count("j3_exact_hit_runs", static_cast<double>(used));
    ArchB::Recorder::Count("j3_exact_hit_bytes", static_cast<double>(bytes));
    if (const auto* reason = refusal(*next)) {
        ArchB::Recorder::Count("j3_observation_refused");
        ArchB::Recorder::Count(reason);
        return;
    }
    entry->hitObservation.store(std::move(next));
    ArchB::Recorder::Count("j3_observations");
}

}
