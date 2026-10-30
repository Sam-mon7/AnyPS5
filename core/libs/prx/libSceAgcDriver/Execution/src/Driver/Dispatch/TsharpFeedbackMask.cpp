#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "RuntimeAbi.hpp"
#include <cstring>
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

void RecordTsharpFeedbackMasks(DispatchVariant& variant, const ShaderRecompiler::ResourceCapture& capture) {
    const auto refuse = [](const char* reason) { ArchB::Recorder::Count(reason); };
    variant.feedback = {};
    variant.userData = capture.snapshot.userData;
    if (capture.plan == nullptr || !capture.plan->srtPlanComplete || !capture.plan->resourceTrackingComplete || capture.plan->requiresSpecializationMemory || !capture.snapshot.srtPoison.empty() || capture.snapshot.nullRootReads != 0u) {
        refuse("fix_tsharp_mask_refused_plan");
        return;
    }
    const auto& plan = *capture.plan;
    if (plan.shaderFlatSlots.size() != plan.srtReads.size()) {
        refuse("fix_tsharp_mask_refused_plan");
        return;
    }
    if (!capture.snapshot.tables.tables.empty() || std::ranges::any_of(plan.info.images, [](const auto& image) { return image.table != ShaderRecompiler::NoTable; })) {
        refuse("fix_tsharp_mask_refused_table");
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
                if (image && (word == 5u || word == 6u) && (value == nullptr || !value->HasImmediate())) refuse("fix_tsharp_mask_refused_role");
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
        if (data.contains(value)) { refuse("fix_tsharp_mask_refused_data"); continue; }
        if (uses.size() != 1u) { refuse("fix_tsharp_mask_refused_alias"); continue; }
        if (forbidden.contains(value)) { refuse("fix_tsharp_mask_refused_role"); continue; }
        const auto& candidate = uses.front();
        std::uint32_t stored = 0u;
        WordBitMasks* masks = &variant.feedback.memory;
        std::size_t position = variant.words.size();
        if (value->Opcode() == ShaderRecompiler::IrOpcode::GetUserData) {
            if (value->ArgumentCount() != 1u || value->Argument(0)->Register().index < plan.userDataBase) { refuse("fix_tsharp_mask_refused_role"); continue; }
            const auto sameRegister = [&](const ShaderRecompiler::IrValue* other) {
                return other != nullptr && other->Opcode() == ShaderRecompiler::IrOpcode::GetUserData && other->ArgumentCount() == 1u && other->Argument(0)->Register().index == value->Argument(0)->Register().index;
            };
            if (std::ranges::any_of(forbidden, sameRegister) || std::ranges::any_of(candidates, [&](const auto& other) { return other.first != value && sameRegister(other.first); })) { refuse("fix_tsharp_mask_refused_alias"); continue; }
            position = value->Argument(0)->Register().index - plan.userDataBase;
            if (position >= capture.snapshot.userData.size() || position >= 64u || ((capture.userDataUse.asBase | plan.shaderUserWords) & (std::uint64_t{1} << position)) != 0u) { refuse("fix_tsharp_mask_refused_data"); continue; }
            masks = &variant.feedback.userData;
            stored = capture.snapshot.userData[position];
        } else {
            const auto found = addresses.find(value);
            if (found == addresses.end() || found->second.size() != 1u) { refuse("fix_tsharp_mask_refused_unmapped"); continue; }
            const auto address = *found->second.begin();
            if (address % 4u != 0u || address > std::numeric_limits<std::uint64_t>::max() - 4u) { refuse("fix_tsharp_mask_refused_unmapped"); continue; }
            if (readers[address].size() != 1u || std::ranges::any_of(capture.readTrace.leaves, [&](const auto& leaf) { return leaf.second == address; })) { refuse("fix_tsharp_mask_refused_alias"); continue; }
            std::size_t first = 0, matches = 0;
            for (const auto& [begin, end] : variant.runs) {
                if (begin <= address && address + 4u <= end && (address - begin) % 4u == 0u) { position = first + (address - begin) / 4u; ++matches; }
                first += (end - begin) / 4u;
            }
            if (matches != 1u || position >= variant.words.size()) { refuse(matches > 1u ? "fix_tsharp_mask_refused_alias" : "fix_tsharp_mask_refused_unmapped"); continue; }
            if (std::binary_search(variant.dataPositions.begin(), variant.dataPositions.end(), position)) { refuse("fix_tsharp_mask_refused_data"); continue; }
            stored = variant.words[position];
        }
        bool matches = false;
        for (std::size_t index = 0; index < plan.info.images.size() && index < capture.snapshot.images.size(); ++index) {
            if (plan.info.images[index].source != candidate.source) continue;
            const auto& descriptor = capture.snapshot.images[index];
            if (descriptor.dwordCount != 8u || descriptor.dwords[candidate.word] != stored) { matches = false; break; }
            matches = true;
        }
        if (!matches) { refuse("fix_tsharp_mask_refused_value"); continue; }
        if (position > std::numeric_limits<std::uint32_t>::max()) { refuse("fix_tsharp_mask_refused_unmapped"); continue; }
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
