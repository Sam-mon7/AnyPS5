#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "RuntimeAbi.hpp"
#include "ThreadOwned.hpp"
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <set>

namespace AgcDriver::DriverDetail {

std::uint32_t WordMask(std::span<const std::pair<std::uint32_t, std::uint32_t>> masks, std::size_t position) {
    const auto found = std::lower_bound(masks.begin(), masks.end(), position, [](const auto& mask, auto word) { return mask.first < word; });
    return found != masks.end() && found->first == position ? found->second : 0u;
}

bool SameCapturedRuns(std::span<const std::pair<std::uint64_t, std::uint64_t>> before, std::span<const std::pair<std::uint64_t, std::uint64_t>> after) {
    const auto next = [](auto runs, std::size_t& index) {
        auto range = runs[index++];
        while (index < runs.size() && runs[index].first == range.second && runs[index].first < runs[index].second) range.second = runs[index++].second;
        return range;
    };
    std::size_t a = 0, b = 0;
    while (a < before.size() && b < after.size()) {
        if (before[a].first >= before[a].second || after[b].first >= after[b].second || next(before, a) != next(after, b)) return false;
    }
    return a == before.size() && b == after.size();
}

bool FeedbackWordsEqual(std::span<const std::uint32_t> before, std::span<const std::uint32_t> after, std::span<const std::pair<std::uint32_t, std::uint32_t>> masks, bool* used) {
    if (before.size() != after.size()) return false;
    for (std::size_t word = 0; word < before.size(); ++word) {
        const auto difference = before[word] ^ after[word];
        if (difference == 0u) continue;
        if ((difference & ~WordMask(masks, word)) != 0u) return false;
        if (used != nullptr) *used = true;
    }
    return true;
}

namespace {

enum class Refusal { Plan, Table, Role, Data, Alias, Unmapped, Value };
using Refusals = std::array<std::uint32_t, 7>;
constexpr std::array RefusalCounters{
    "fix_tsharp_mask_refused_plan", "fix_tsharp_mask_refused_table", "fix_tsharp_mask_refused_role",
    "fix_tsharp_mask_refused_data", "fix_tsharp_mask_refused_alias", "fix_tsharp_mask_refused_unmapped", "fix_tsharp_mask_refused_value"
};

void CountRefusals(const Refusals& refusals) {
    for (std::size_t index = 0; index < refusals.size(); ++index) if (refusals[index] != 0u) ArchB::Recorder::Count(RefusalCounters[index], refusals[index]);
}

enum class BuildOutcome { Plan, Table, NoImage, NoCandidate, Refused, Accepted, Fallback };

struct MaskCandidate {
    const ShaderRecompiler::IrValue* value;
    std::uint32_t source, word, bits;
    std::optional<std::uint32_t> userWord;
    std::vector<std::uint32_t> flatOffsets;
};

struct MaskPlan {
    std::weak_ptr<const ShaderRecompiler::IrResourcePlan> owner;
    BuildOutcome outcome = BuildOutcome::Refused;
    Refusals refusals{};
    std::vector<MaskCandidate> candidates;
    std::vector<std::pair<const ShaderRecompiler::IrValue*, const ShaderRecompiler::IrValue*>> traceSources;
};

void BuildUncached(DispatchVariant& variant, const ShaderRecompiler::ResourceCapture& capture, Refusals& refusals) {
    const auto refuse = [&](Refusal reason) { ++refusals[static_cast<std::size_t>(reason)]; };
    variant.feedback = {};
    variant.userData = capture.snapshot.userData;
    if (capture.plan == nullptr || !capture.plan->srtPlanComplete || !capture.plan->resourceTrackingComplete || capture.plan->requiresSpecializationMemory || !capture.snapshot.srtPoison.empty() || capture.snapshot.nullRootReads != 0u) {
        refuse(Refusal::Plan);
        return;
    }
    const auto& plan = *capture.plan;
    if (plan.shaderFlatSlots.size() != plan.srtReads.size()) {
        refuse(Refusal::Plan);
        return;
    }
    if (!capture.snapshot.tables.tables.empty() || std::ranges::any_of(plan.info.images, [](const auto& image) { return image.table != ShaderRecompiler::NoTable; })) {
        refuse(Refusal::Table);
        return;
    }
    const auto sourceValue = [&](const ShaderRecompiler::IrValue* value) {
        std::set<const ShaderRecompiler::IrValue*> visited;
        while (value != nullptr) {
            value = value->Resolve();
            if (!visited.insert(value).second) return static_cast<const ShaderRecompiler::IrValue*>(nullptr);
            if (value->Opcode() == ShaderRecompiler::IrOpcode::ReadConst && value->ArgumentCount() == 2u) {
                if (value->Argument(0)->Resolve()->Opcode() != ShaderRecompiler::IrOpcode::GetSrtResource) return static_cast<const ShaderRecompiler::IrValue*>(nullptr);
                const auto* slot = value->Argument(1)->Resolve();
                if (!slot->HasImmediate() || slot->Type() != ShaderRecompiler::IrType::U32 || slot->ImmediateU32() >= plan.srtReads.size()) return static_cast<const ShaderRecompiler::IrValue*>(nullptr);
                value = plan.srtReads[slot->ImmediateU32()].value;
            } else if ((value->Opcode() == ShaderRecompiler::IrOpcode::BitCastU32F32 || value->Opcode() == ShaderRecompiler::IrOpcode::BitCastF32U32) && value->ArgumentCount() == 1u) value = value->Argument(0);
            else return value;
        }
        return value;
    };
    std::set<const ShaderRecompiler::IrValue*> forbidden, data;
    const auto mark = [&](auto&& self, const ShaderRecompiler::IrValue* value, std::set<const ShaderRecompiler::IrValue*>& set) -> void {
        value = sourceValue(value);
        if (value == nullptr || !set.insert(value).second) return;
        for (const auto* argument : value->Arguments()) self(self, argument, set);
    };
    std::set<std::uint32_t> images, otherSources;
    for (const auto& image : plan.info.images) if (image.table == ShaderRecompiler::NoTable) images.insert(image.source);
    for (const auto& buffer : plan.info.buffers) otherSources.insert(buffer.source);
    for (const auto& sampler : plan.info.samplers) otherSources.insert(sampler.source);
    struct Candidate { std::uint32_t source, word, bits; };
    std::map<const ShaderRecompiler::IrValue*, std::vector<Candidate>> candidates;
    for (std::uint32_t index = 0; index < plan.descriptorSources.size(); ++index) {
        const auto& source = plan.descriptorSources[index];
        const bool image = images.contains(index) && !otherSources.contains(index) && source.dwordCount == 8u && !source.tableColumn && std::ranges::find(plan.materializationSources, index) != plan.materializationSources.end();
        for (std::uint32_t word = 0; word < std::min<std::size_t>(source.dwordCount, source.dwords.size()); ++word) {
            const auto* value = sourceValue(source.dwords[word]);
            const bool direct = value != nullptr && (value->Opcode() == ShaderRecompiler::IrOpcode::LoadAddressU32 || value->Opcode() == ShaderRecompiler::IrOpcode::ReadConstBuffer || value->Opcode() == ShaderRecompiler::IrOpcode::GetUserData);
            if (image && (word == 5u || word == 6u) && direct) candidates[value].push_back({index, word, word == 5u ? 0x020fff00u : 0xffu});
            else {
                mark(mark, source.dwords[word], forbidden);
                if (image && (word == 5u || word == 6u) && (value == nullptr || !value->HasImmediate())) refuse(Refusal::Role);
            }
        }
    }
    for (std::size_t slot = 0; slot < plan.srtReads.size(); ++slot) {
        const auto* value = sourceValue(plan.srtReads[slot].value);
        if (plan.shaderFlatSlots[slot] != 0u) mark(mark, value, data);
        if (value != nullptr) for (const auto* argument : value->Arguments()) mark(mark, argument, forbidden);
    }
    for (const auto& block : plan.controlFlow) mark(mark, block.condition, forbidden);
    if (plan.uniformFill.fill.kind != ShaderRecompiler::UniformFillKind::None) for (const auto* value : plan.uniformFill.values) mark(mark, value, data);
    std::map<const ShaderRecompiler::IrValue*, std::set<std::uint64_t>> addresses;
    std::map<std::uint64_t, std::set<const ShaderRecompiler::IrValue*>> readers;
    for (const auto& [raw, address] : capture.readTrace.slotReads) {
        const auto* value = sourceValue(raw);
        addresses[value].insert(address);
        readers[address].insert(value);
    }
    WordBitMasks flatMasks;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> accepted;
    for (const auto& [value, uses] : candidates) {
        if (data.contains(value)) { refuse(Refusal::Data); continue; }
        if (uses.size() != 1u) { refuse(Refusal::Alias); continue; }
        if (forbidden.contains(value)) { refuse(Refusal::Role); continue; }
        const auto& candidate = uses.front();
        std::uint32_t stored = 0u;
        WordBitMasks* masks = &variant.feedback.memory;
        std::size_t position = variant.words.size();
        if (value->Opcode() == ShaderRecompiler::IrOpcode::GetUserData) {
            if (value->ArgumentCount() != 1u || value->Argument(0)->Register().index < plan.userDataBase) { refuse(Refusal::Role); continue; }
            const auto sameRegister = [&](const ShaderRecompiler::IrValue* other) {
                return other != nullptr && other->Opcode() == ShaderRecompiler::IrOpcode::GetUserData && other->ArgumentCount() == 1u && other->Argument(0)->Register().index == value->Argument(0)->Register().index;
            };
            if (std::ranges::any_of(forbidden, sameRegister) || std::ranges::any_of(candidates, [&](const auto& other) { return other.first != value && sameRegister(other.first); })) { refuse(Refusal::Alias); continue; }
            position = value->Argument(0)->Register().index - plan.userDataBase;
            if (position >= capture.snapshot.userData.size() || position >= 64u || ((capture.userDataUse.asBase | plan.shaderUserWords) & (std::uint64_t{1} << position)) != 0u) { refuse(Refusal::Data); continue; }
            masks = &variant.feedback.userData;
            stored = capture.snapshot.userData[position];
        } else {
            const auto found = addresses.find(value);
            if (found == addresses.end() || found->second.size() != 1u) { refuse(Refusal::Unmapped); continue; }
            const auto address = *found->second.begin();
            if (address % 4u != 0u || address > std::numeric_limits<std::uint64_t>::max() - 4u) { refuse(Refusal::Unmapped); continue; }
            if (readers[address].size() != 1u || std::ranges::any_of(capture.readTrace.leaves, [&](const auto& leaf) { return leaf.second == address; })) { refuse(Refusal::Alias); continue; }
            std::size_t first = 0, matches = 0;
            for (const auto& [begin, end] : variant.runs) {
                if (begin <= address && address + 4u <= end && (address - begin) % 4u == 0u) { position = first + (address - begin) / 4u; ++matches; }
                first += (end - begin) / 4u;
            }
            if (matches != 1u || position >= variant.words.size()) { refuse(matches > 1u ? Refusal::Alias : Refusal::Unmapped); continue; }
            if (std::binary_search(variant.dataPositions.begin(), variant.dataPositions.end(), position)) { refuse(Refusal::Data); continue; }
            stored = variant.words[position];
        }
        bool matches = false;
        for (std::size_t index = 0; index < plan.info.images.size() && index < capture.snapshot.images.size(); ++index) {
            if (plan.info.images[index].source != candidate.source) continue;
            const auto& descriptor = capture.snapshot.images[index];
            if (descriptor.dwordCount != 8u || descriptor.dwords[candidate.word] != stored) { matches = false; break; }
            matches = true;
        }
        if (!matches) { refuse(Refusal::Value); continue; }
        if (position > std::numeric_limits<std::uint32_t>::max()) { refuse(Refusal::Unmapped); continue; }
        masks->emplace_back(static_cast<std::uint32_t>(position), candidate.bits);
        accepted[{candidate.source, candidate.word}] = candidate.bits;
        for (const auto& read : plan.srtReads) if (sourceValue(read.value) == value) flatMasks.emplace_back(read.flatOffset, candidate.bits);
    }
    std::sort(variant.feedback.memory.begin(), variant.feedback.memory.end());
    std::sort(variant.feedback.userData.begin(), variant.feedback.userData.end());
    std::sort(flatMasks.begin(), flatMasks.end());
    WordBitMasks shaderDataMasks;
    for (std::uint32_t slot = 0; slot < variant.compiled->shaderDataUserWords.size(); ++slot) {
        const auto bits = WordMask(variant.feedback.userData, variant.compiled->shaderDataUserWords[slot]);
        if (bits != 0u) shaderDataMasks.emplace_back(slot, bits);
    }
    for (std::size_t image = 0; image < plan.info.images.size(); ++image) {
        for (const auto word : {5u, 6u}) {
            const auto bits = accepted.find({plan.info.images[image].source, word});
            if (bits == accepted.end() || !variant.compiled->registrationModule || image >= variant.compiled->runtimeImageCount) continue;
            shaderDataMasks.emplace_back(variant.compiled->imageMetadataDword + static_cast<std::uint32_t>(image) * (sizeof(ShaderRecompiler::RuntimeAbi::ResourceMetadata) / 4u) + offsetof(ShaderRecompiler::RuntimeAbi::ResourceMetadata, descriptor) / 4u + word, bits->second);
        }
    }
    variant.feedback.pushConstants = shaderDataMasks;
    variant.feedback.bindings.resize(variant.compiled->bindings.size());
    for (std::size_t binding = 0; binding < variant.compiled->bindings.size(); ++binding) {
        const auto& descriptor = variant.compiled->bindings[binding];
        auto& masks = variant.feedback.bindings[binding];
        if (descriptor.role == ShaderRecompiler::DescriptorRole::FlattenedSrt) masks = flatMasks;
        else if (descriptor.role == ShaderRecompiler::DescriptorRole::ShaderData) masks = shaderDataMasks;
        else if (descriptor.role == ShaderRecompiler::DescriptorRole::GuestImages) {
            for (std::size_t offset = 0; offset + 8u <= descriptor.guestDescriptor.size(); offset += 8u) {
                for (const auto word : {5u, 6u}) {
                    std::uint32_t bits = word == 5u ? 0x020fff00u : 0xffu;
                    bool found = false;
                    for (std::size_t image = 0; image < capture.snapshot.images.size() && image < plan.info.images.size(); ++image) {
                        const auto& words = capture.snapshot.images[image].dwords;
                        bool same = true;
                        for (std::size_t w = 0; w < 8u; ++w) if (((words[w] ^ descriptor.guestDescriptor[offset + w]) & (w == 3u ? ~0xfffu : ~0u)) != 0u) same = false;
                        if (!same) continue;
                        found = true;
                        const auto approved = accepted.find({plan.info.images[image].source, word});
                        bits &= approved != accepted.end() ? approved->second : 0u;
                    }
                    if (found && bits != 0u) masks.emplace_back(static_cast<std::uint32_t>(offset + word), bits);
                }
            }
        }
    }
}
MaskPlan BuildPlan(const std::shared_ptr<const ShaderRecompiler::IrResourcePlan>& owner) {
    PerformanceTimer timing("Fix.TsharpMask.Plan");
    timing.RecorderCounter("fix_tsharp_build_plan_ms");
    ArchB::Recorder::Count("fix_tsharp_build_plan_calls");
    MaskPlan result;
    result.owner = owner;
    const auto& plan = *owner;
    const auto refuse = [&](Refusal reason) { ++result.refusals[static_cast<std::size_t>(reason)]; };
    if (!plan.srtPlanComplete || !plan.resourceTrackingComplete || plan.requiresSpecializationMemory || plan.shaderFlatSlots.size() != plan.srtReads.size()) {
        result.outcome = BuildOutcome::Plan;
        return result;
    }
    if (std::ranges::any_of(plan.info.images, [](const auto& image) { return image.table != ShaderRecompiler::NoTable; })) {
        result.outcome = BuildOutcome::Table;
        return result;
    }
    if (std::ranges::none_of(plan.info.images, [&](const auto& image) { return std::ranges::find(plan.materializationSources, image.source) != plan.materializationSources.end(); })) {
        result.outcome = BuildOutcome::NoImage;
        return result;
    }
    const auto resolveSource = [&](const ShaderRecompiler::IrValue* value) {
        std::set<const ShaderRecompiler::IrValue*> visited;
        while (value != nullptr) {
            value = value->Resolve();
            if (!visited.insert(value).second) return static_cast<const ShaderRecompiler::IrValue*>(nullptr);
            if (value->Opcode() == ShaderRecompiler::IrOpcode::ReadConst && value->ArgumentCount() == 2u) {
                if (value->Argument(0)->Resolve()->Opcode() != ShaderRecompiler::IrOpcode::GetSrtResource) return static_cast<const ShaderRecompiler::IrValue*>(nullptr);
                const auto* slot = value->Argument(1)->Resolve();
                if (!slot->HasImmediate() || slot->Type() != ShaderRecompiler::IrType::U32 || slot->ImmediateU32() >= plan.srtReads.size()) return static_cast<const ShaderRecompiler::IrValue*>(nullptr);
                value = plan.srtReads[slot->ImmediateU32()].value;
            } else if ((value->Opcode() == ShaderRecompiler::IrOpcode::BitCastU32F32 || value->Opcode() == ShaderRecompiler::IrOpcode::BitCastF32U32) && value->ArgumentCount() == 1u) value = value->Argument(0);
            else return value;
        }
        return value;
    };
    std::map<const ShaderRecompiler::IrValue*, const ShaderRecompiler::IrValue*> resolvedSources;
    const auto sourceValue = [&](const ShaderRecompiler::IrValue* value) {
        const auto found = resolvedSources.find(value);
        if (found != resolvedSources.end()) return found->second;
        return resolvedSources.emplace(value, resolveSource(value)).first->second;
    };
    std::set<const ShaderRecompiler::IrValue*> forbidden, data;
    const auto mark = [&](auto&& self, const ShaderRecompiler::IrValue* value, std::set<const ShaderRecompiler::IrValue*>& set) -> void {
        value = sourceValue(value);
        if (value == nullptr || !set.insert(value).second) return;
        for (const auto* argument : value->Arguments()) self(self, argument, set);
    };
    std::set<std::uint32_t> images, otherSources;
    for (const auto& image : plan.info.images) if (image.table == ShaderRecompiler::NoTable) images.insert(image.source);
    for (const auto& buffer : plan.info.buffers) otherSources.insert(buffer.source);
    for (const auto& sampler : plan.info.samplers) otherSources.insert(sampler.source);
    struct Candidate { std::uint32_t source, word, bits; };
    std::map<const ShaderRecompiler::IrValue*, std::vector<Candidate>> candidates;
    for (std::uint32_t index = 0; index < plan.descriptorSources.size(); ++index) {
        const auto& source = plan.descriptorSources[index];
        const bool image = images.contains(index) && !otherSources.contains(index) && source.dwordCount == 8u && !source.tableColumn && std::ranges::find(plan.materializationSources, index) != plan.materializationSources.end();
        for (std::uint32_t word = 0; word < std::min<std::size_t>(source.dwordCount, source.dwords.size()); ++word) {
            const auto* value = sourceValue(source.dwords[word]);
            const bool direct = value != nullptr && (value->Opcode() == ShaderRecompiler::IrOpcode::LoadAddressU32 || value->Opcode() == ShaderRecompiler::IrOpcode::ReadConstBuffer || value->Opcode() == ShaderRecompiler::IrOpcode::GetUserData);
            if (image && (word == 5u || word == 6u) && direct) candidates[value].push_back({index, word, word == 5u ? 0x020fff00u : 0xffu});
            else {
                mark(mark, source.dwords[word], forbidden);
                if (image && (word == 5u || word == 6u) && (value == nullptr || !value->HasImmediate())) refuse(Refusal::Role);
            }
        }
    }
    for (std::size_t slot = 0; slot < plan.srtReads.size(); ++slot) {
        const auto* value = sourceValue(plan.srtReads[slot].value);
        if (plan.shaderFlatSlots[slot] != 0u) mark(mark, value, data);
        if (value != nullptr) for (const auto* argument : value->Arguments()) mark(mark, argument, forbidden);
    }
    for (const auto& block : plan.controlFlow) mark(mark, block.condition, forbidden);
    if (plan.uniformFill.fill.kind != ShaderRecompiler::UniformFillKind::None) for (const auto* value : plan.uniformFill.values) mark(mark, value, data);
    ArchB::Recorder::Count("fix_tsharp_build_srt_reads", plan.srtReads.size());
    ArchB::Recorder::Count("fix_tsharp_build_role_nodes", forbidden.size() + data.size());
    ArchB::Recorder::Count("fix_tsharp_build_candidates", candidates.size());
    if (candidates.empty()) result.outcome = BuildOutcome::NoCandidate;
    for (const auto& [value, uses] : candidates) {
        if (data.contains(value)) { refuse(Refusal::Data); continue; }
        if (uses.size() != 1u) { refuse(Refusal::Alias); continue; }
        if (forbidden.contains(value)) { refuse(Refusal::Role); continue; }
        const auto& use = uses.front();
        MaskCandidate candidate{value, use.source, use.word, use.bits, {}, {}};
        if (value->Opcode() == ShaderRecompiler::IrOpcode::GetUserData) {
            if (value->ArgumentCount() != 1u || value->Argument(0)->Register().index < plan.userDataBase) { refuse(Refusal::Role); continue; }
            const auto sameRegister = [&](const ShaderRecompiler::IrValue* other) {
                return other != nullptr && other->Opcode() == ShaderRecompiler::IrOpcode::GetUserData && other->ArgumentCount() == 1u && other->Argument(0)->Register().index == value->Argument(0)->Register().index;
            };
            if (std::ranges::any_of(forbidden, sameRegister) || std::ranges::any_of(candidates, [&](const auto& other) { return other.first != value && sameRegister(other.first); })) { refuse(Refusal::Alias); continue; }
            candidate.userWord = value->Argument(0)->Register().index - plan.userDataBase;
        }
        for (const auto& read : plan.srtReads) if (sourceValue(read.value) == value) candidate.flatOffsets.push_back(read.flatOffset);
        result.candidates.push_back(std::move(candidate));
    }
    if (!result.candidates.empty()) {
        for (const auto& read : plan.srtReads) if (read.value != nullptr) sourceValue(read.value->Resolve());
        result.traceSources.assign(resolvedSources.begin(), resolvedSources.end());
    }
    return result;
}

const MaskPlan& CachedPlan(const std::shared_ptr<const ShaderRecompiler::IrResourcePlan>& plan) {
    thread_local std::unordered_map<const ShaderRecompiler::IrResourcePlan*, MaskPlan>* cacheSlot = nullptr;
    auto& cache = ShaderRecompiler::ThreadOwned(cacheSlot);
    const auto found = cache.find(plan.get());
    if (found != cache.end() && found->second.owner.lock() == plan) {
        ArchB::Recorder::Count("fix_tsharp_build_plan_hits");
        return found->second;
    }
    if (cache.size() >= 1024u) {
        std::erase_if(cache, [](const auto& item) { return item.second.owner.expired(); });
        if (cache.size() >= 1024u) cache.clear();
        ArchB::Recorder::Count("fix_tsharp_build_plan_prunes");
    }
    return cache.insert_or_assign(plan.get(), BuildPlan(plan)).first->second;
}

BuildOutcome BuildCached(DispatchVariant& variant, const ShaderRecompiler::ResourceCapture& capture, Refusals& refusals) {
    const auto refuse = [&](Refusal reason) { ++refusals[static_cast<std::size_t>(reason)]; };
    variant.feedback = {};
    variant.userData = capture.snapshot.userData;
    if (capture.plan == nullptr || !capture.snapshot.srtPoison.empty() || capture.snapshot.nullRootReads != 0u) {
        refuse(Refusal::Plan);
        return BuildOutcome::Plan;
    }
    const auto& proof = CachedPlan(capture.plan);
    if (proof.outcome == BuildOutcome::Plan) {
        refuse(Refusal::Plan);
        return BuildOutcome::Plan;
    }
    if (proof.outcome == BuildOutcome::Table || !capture.snapshot.tables.tables.empty()) {
        refuse(Refusal::Table);
        return BuildOutcome::Table;
    }
    refusals = proof.refusals;
    if (proof.candidates.empty()) {
        variant.feedback.bindings.resize(variant.compiled->bindings.size());
        ArchB::Recorder::Count("fix_tsharp_build_skipped");
        return proof.outcome;
    }
    thread_local std::vector<std::pair<const ShaderRecompiler::IrValue*, std::uint64_t>>* traceSlot = nullptr;
    auto& trace = ShaderRecompiler::ThreadOwned(traceSlot);
    trace.clear();
    for (const auto& [raw, address] : capture.readTrace.slotReads) {
        const auto source = std::lower_bound(proof.traceSources.begin(), proof.traceSources.end(), raw, [](const auto& entry, const auto* key) { return std::less<const ShaderRecompiler::IrValue*>{}(entry.first, key); });
        if (source == proof.traceSources.end() || source->first != raw) return BuildOutcome::Fallback;
        trace.emplace_back(source->second, address);
    }
    ArchB::Recorder::Count("fix_tsharp_build_resolve_calls");
    ArchB::Recorder::Count("fix_tsharp_build_trace_reads", trace.size());
    const auto& plan = *capture.plan;
    WordBitMasks flatMasks;
    std::vector<const MaskCandidate*> accepted;
    const auto acceptedBits = [&](std::uint32_t source, std::uint32_t word) {
        const auto found = std::ranges::find_if(accepted, [&](const auto* entry) { return entry->source == source && entry->word == word; });
        return found == accepted.end() ? 0u : (*found)->bits;
    };
    for (const auto& candidate : proof.candidates) {
        const auto* value = candidate.value;
        std::uint32_t stored = 0u;
        WordBitMasks* masks = &variant.feedback.memory;
        std::size_t position = variant.words.size();
        if (candidate.userWord) {
            position = *candidate.userWord;
            if (position >= capture.snapshot.userData.size() || position >= 64u || ((capture.userDataUse.asBase | plan.shaderUserWords) & (std::uint64_t{1} << position)) != 0u) { refuse(Refusal::Data); continue; }
            masks = &variant.feedback.userData;
            stored = capture.snapshot.userData[position];
        } else {
            std::optional<std::uint64_t> address;
            bool multiple = false;
            for (const auto& read : trace) {
                if (read.first != value) continue;
                if (address && *address != read.second) multiple = true;
                address = read.second;
            }
            if (!address || multiple || *address % 4u != 0u || *address > std::numeric_limits<std::uint64_t>::max() - 4u) { refuse(Refusal::Unmapped); continue; }
            if (std::ranges::any_of(trace, [&](const auto& read) { return read.second == *address && read.first != value; }) || std::ranges::any_of(capture.readTrace.leaves, [&](const auto& leaf) { return leaf.second == *address; })) { refuse(Refusal::Alias); continue; }
            std::size_t first = 0, matches = 0;
            for (const auto& [begin, end] : variant.runs) {
                if (begin <= *address && *address + 4u <= end && (*address - begin) % 4u == 0u) { position = first + (*address - begin) / 4u; ++matches; }
                first += (end - begin) / 4u;
            }
            if (matches != 1u || position >= variant.words.size()) { refuse(matches > 1u ? Refusal::Alias : Refusal::Unmapped); continue; }
            if (std::binary_search(variant.dataPositions.begin(), variant.dataPositions.end(), position)) { refuse(Refusal::Data); continue; }
            stored = variant.words[position];
        }
        bool matches = false;
        for (std::size_t index = 0; index < plan.info.images.size() && index < capture.snapshot.images.size(); ++index) {
            if (plan.info.images[index].source != candidate.source) continue;
            const auto& descriptor = capture.snapshot.images[index];
            if (descriptor.dwordCount != 8u || descriptor.dwords[candidate.word] != stored) { matches = false; break; }
            matches = true;
        }
        if (!matches) { refuse(Refusal::Value); continue; }
        if (position > std::numeric_limits<std::uint32_t>::max()) { refuse(Refusal::Unmapped); continue; }
        masks->emplace_back(static_cast<std::uint32_t>(position), candidate.bits);
        accepted.push_back(&candidate);
        for (const auto offset : candidate.flatOffsets) flatMasks.emplace_back(offset, candidate.bits);
    }
    std::sort(variant.feedback.memory.begin(), variant.feedback.memory.end());
    std::sort(variant.feedback.userData.begin(), variant.feedback.userData.end());
    std::sort(flatMasks.begin(), flatMasks.end());
    WordBitMasks shaderDataMasks;
    for (std::uint32_t slot = 0; slot < variant.compiled->shaderDataUserWords.size(); ++slot) {
        const auto bits = WordMask(variant.feedback.userData, variant.compiled->shaderDataUserWords[slot]);
        if (bits != 0u) shaderDataMasks.emplace_back(slot, bits);
    }
    for (std::size_t image = 0; image < plan.info.images.size(); ++image) {
        for (const auto word : {5u, 6u}) {
            const auto bits = acceptedBits(plan.info.images[image].source, word);
            if (bits == 0u || !variant.compiled->registrationModule || image >= variant.compiled->runtimeImageCount) continue;
            shaderDataMasks.emplace_back(variant.compiled->imageMetadataDword + static_cast<std::uint32_t>(image) * (sizeof(ShaderRecompiler::RuntimeAbi::ResourceMetadata) / 4u) + offsetof(ShaderRecompiler::RuntimeAbi::ResourceMetadata, descriptor) / 4u + word, bits);
        }
    }
    variant.feedback.pushConstants = shaderDataMasks;
    variant.feedback.bindings.resize(variant.compiled->bindings.size());
    for (std::size_t binding = 0; binding < variant.compiled->bindings.size(); ++binding) {
        const auto& descriptor = variant.compiled->bindings[binding];
        auto& masks = variant.feedback.bindings[binding];
        if (descriptor.role == ShaderRecompiler::DescriptorRole::FlattenedSrt) masks = flatMasks;
        else if (descriptor.role == ShaderRecompiler::DescriptorRole::ShaderData) masks = shaderDataMasks;
        else if (descriptor.role == ShaderRecompiler::DescriptorRole::GuestImages) {
            for (std::size_t offset = 0; offset + 8u <= descriptor.guestDescriptor.size(); offset += 8u) {
                for (const auto word : {5u, 6u}) {
                    std::uint32_t bits = word == 5u ? 0x020fff00u : 0xffu;
                    bool found = false;
                    for (std::size_t image = 0; image < capture.snapshot.images.size() && image < plan.info.images.size(); ++image) {
                        const auto& words = capture.snapshot.images[image].dwords;
                        bool same = true;
                        for (std::size_t w = 0; w < 8u; ++w) if (((words[w] ^ descriptor.guestDescriptor[offset + w]) & (w == 3u ? ~0xfffu : ~0u)) != 0u) same = false;
                        if (!same) continue;
                        found = true;
                        const auto approved = acceptedBits(plan.info.images[image].source, word);
                        bits &= approved;
                    }
                    if (found && bits != 0u) masks.emplace_back(static_cast<std::uint32_t>(offset + word), bits);
                }
            }
        }
    }
    return !variant.feedback.memory.empty() || !variant.feedback.userData.empty() ? BuildOutcome::Accepted : BuildOutcome::Refused;
}

}

void RecordUncachedTsharpFeedbackMasks(DispatchVariant& variant, const ShaderRecompiler::ResourceCapture& capture) {
    Refusals refusals{};
    BuildUncached(variant, capture, refusals);
    CountRefusals(refusals);
}

void RecordTsharpFeedbackMasks(DispatchVariant& variant, const ShaderRecompiler::ResourceCapture& capture) {
    Refusals refusals{};
    {
        PerformanceTimer timing("Fix.TsharpMask.Build");
        ArchB::Recorder::Count("fix_tsharp_build_calls");
        const auto outcome = BuildCached(variant, capture, refusals);
        if (outcome == BuildOutcome::Fallback) {
            refusals = {};
            BuildUncached(variant, capture, refusals);
        }
        constexpr std::array calls{
            "fix_tsharp_build_calls_plan", "fix_tsharp_build_calls_table", "fix_tsharp_build_calls_no_image",
            "fix_tsharp_build_calls_no_candidate", "fix_tsharp_build_calls_refused", "fix_tsharp_build_calls_accepted", "fix_tsharp_build_calls_fallback"
        };
        constexpr std::array times{
            "fix_tsharp_build_ms_plan", "fix_tsharp_build_ms_table", "fix_tsharp_build_ms_no_image",
            "fix_tsharp_build_ms_no_candidate", "fix_tsharp_build_ms_refused", "fix_tsharp_build_ms_accepted", "fix_tsharp_build_ms_fallback"
        };
        ArchB::Recorder::Count(calls[static_cast<std::size_t>(outcome)]);
        timing.RecorderCounter(times[static_cast<std::size_t>(outcome)]);
    }
    static const bool verify = [] { const auto* value = std::getenv("APS5_VERIFY_TSHARP_MASK"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    if (verify) {
        PerformanceTimer timing("Fix.TsharpMask.Verify");
        timing.RecorderCounter("fix_tsharp_build_verify_ms");
        auto feedback = std::move(variant.feedback);
        Refusals expected{};
        BuildUncached(variant, capture, expected);
        ArchB::Recorder::Count("fix_tsharp_build_verified");
        if (variant.feedback != feedback || refusals != expected) {
            ArchB::Recorder::Count("fix_tsharp_build_verify_mismatches");
            refusals = expected;
        } else variant.feedback = std::move(feedback);
    }
    CountRefusals(refusals);
}

bool FeedbackBindingsEqual(const DispatchVariant& variant, const ShaderRecompiler::RecompileResult& before, const ShaderRecompiler::RecompileResult& after, bool enabled) {
    if (before.bindings.size() != after.bindings.size() || before.pushConstants.size() != after.pushConstants.size() || before.specialization != after.specialization || before.spirv.ContentHash() != after.spirv.ContentHash()) return false;
    const auto bytesEqual = [&](const auto& a, const auto& b) {
        for (std::size_t offset = 0; offset < a.size(); ++offset) {
            const auto bits = enabled ? (WordMask(variant.feedback.pushConstants, offset / 4u) >> (offset % 4u * 8u)) & 0xffu : 0u;
            if ((std::to_integer<unsigned>(a[offset] ^ b[offset]) & ~bits) != 0u) return false;
        }
        return true;
    };
    if (!bytesEqual(before.pushConstants, after.pushConstants)) return false;
    for (std::size_t index = 0; index < before.bindings.size(); ++index) {
        const auto& a = before.bindings[index];
        const auto& b = after.bindings[index];
        if (a.kind != b.kind || a.role != b.role || a.descriptorSet != b.descriptorSet || a.binding != b.binding || a.count != b.count || a.Flags() != b.Flags() || a.imageShape != b.imageShape || a.imageShapes != b.imageShapes || a.imageSamplers != b.imageSamplers) return false;
        const std::span<const std::pair<std::uint32_t, std::uint32_t>> masks = enabled && index < variant.feedback.bindings.size() ? std::span<const std::pair<std::uint32_t, std::uint32_t>>(variant.feedback.bindings[index]) : std::span<const std::pair<std::uint32_t, std::uint32_t>>{};
        if (!FeedbackWordsEqual(a.guestDescriptor, b.guestDescriptor, masks)) return false;
    }
    return true;
}

}
