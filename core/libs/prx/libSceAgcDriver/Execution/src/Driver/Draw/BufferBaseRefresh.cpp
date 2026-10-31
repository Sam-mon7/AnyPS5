#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <map>
#include <set>

namespace AgcDriver::DriverDetail {

void RecordBufferBasePatches(DispatchVariant& variant, const ShaderRecompiler::ResourceCapture& capture) {
    variant.bufferBaseMasks.clear();
    variant.bufferBasePatches.clear();
    if (capture.plan == nullptr || variant.compiled == nullptr) return;
    const auto& plan = *capture.plan;
    const auto& compiled = *variant.compiled;
    if (plan.info.buffers.empty()) return;
    if (!plan.srtPlanComplete || !plan.resourceTrackingComplete || plan.requiresSpecializationMemory || !compiled.vertexAttributes.empty() || !capture.snapshot.tables.tables.empty() || !capture.snapshot.srtPoison.empty() || capture.snapshot.nullRootReads != 0 || plan.uniformFill.fill.kind != ShaderRecompiler::UniformFillKind::None) {
        ArchB::Recorder::Count("fix_buffer_base_refused_plan");
        return;
    }
    std::uint32_t buffers = DispatchVariant::NoFlatBinding, flat = DispatchVariant::NoFlatBinding;
    for (std::uint32_t b = 0; b < compiled.bindings.size(); ++b) {
        if (compiled.bindings[b].role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
            if (buffers != DispatchVariant::NoFlatBinding) return;
            buffers = b;
        }
        if (compiled.bindings[b].role == ShaderRecompiler::DescriptorRole::FlattenedSrt) {
            if (flat != DispatchVariant::NoFlatBinding) return;
            flat = b;
        }
    }
    if (buffers == DispatchVariant::NoFlatBinding || flat == DispatchVariant::NoFlatBinding || capture.snapshot.buffers.size() != plan.info.buffers.size() || compiled.bindings[buffers].guestDescriptor.size() != plan.info.buffers.size() * 4u || compiled.bindings[buffers].count != plan.info.buffers.size()) {
        ArchB::Recorder::Count("fix_buffer_base_refused_binding");
        return;
    }
    const auto& bound = compiled.bindings[buffers].guestDescriptor;
    for (std::size_t i = 0; i < capture.snapshot.buffers.size(); ++i) {
        const auto& descriptor = capture.snapshot.buffers[i];
        if (descriptor.dwordCount != 4 || !std::equal(descriptor.dwords.begin(), descriptor.dwords.begin() + 4, bound.begin() + i * 4)) {
            ArchB::Recorder::Count("fix_buffer_base_refused_binding");
            return;
        }
    }
    const auto sourceValue = [&](const ShaderRecompiler::IrValue* value) {
        std::set<const ShaderRecompiler::IrValue*> seen;
        while (value != nullptr) {
            value = value->Resolve();
            if (!seen.insert(value).second) return static_cast<const ShaderRecompiler::IrValue*>(nullptr);
            if (value->Opcode() == ShaderRecompiler::IrOpcode::ReadConst && value->ArgumentCount() == 2 && value->Argument(0) != nullptr && value->Argument(0)->Resolve()->Opcode() == ShaderRecompiler::IrOpcode::GetSrtResource) {
                const auto* slot = value->Argument(1)->Resolve();
                if (!slot->HasImmediate() || slot->Type() != ShaderRecompiler::IrType::U32 || slot->ImmediateU32() >= plan.srtReads.size()) return static_cast<const ShaderRecompiler::IrValue*>(nullptr);
                value = plan.srtReads[slot->ImmediateU32()].value;
            } else if ((value->Opcode() == ShaderRecompiler::IrOpcode::BitCastU32F32 || value->Opcode() == ShaderRecompiler::IrOpcode::BitCastF32U32) && value->ArgumentCount() == 1) value = value->Argument(0);
            else return value;
        }
        return value;
    };
    struct Candidate { const ShaderRecompiler::IrValue* value; std::uint32_t resource, word; };
    std::vector<Candidate> candidates;
    std::set<const ShaderRecompiler::IrValue*> forbidden;
    const auto mark = [&](auto&& self, const ShaderRecompiler::IrValue* value) -> void {
        value = sourceValue(value);
        if (value == nullptr || !forbidden.insert(value).second) return;
        for (const auto* argument : value->Arguments()) self(self, argument);
    };
    for (std::uint32_t s = 0; s < plan.descriptorSources.size(); ++s) {
        const auto& source = plan.descriptorSources[s];
        std::vector<std::uint32_t> users;
        for (std::uint32_t b = 0; b < plan.info.buffers.size(); ++b) if (plan.info.buffers[b].source == s) users.push_back(b);
        bool eligible = users.size() == 1 && source.dwordCount == 4 && !source.tableColumn;
        if (eligible) {
            const auto& buffer = plan.info.buffers[users[0]];
            const auto& descriptor = capture.snapshot.buffers[users[0]].dwords;
            eligible = !buffer.scalar && !buffer.written && !buffer.atomic && buffer.imageAlias == ShaderRecompiler::BufferResource::NoImageAlias && (descriptor[3] >> 30u) == 0 && descriptor[2] != 0 && (descriptor[0] != 0 || (descriptor[1] & 0xffffu) != 0);
        }
        eligible &= std::none_of(plan.info.images.begin(), plan.info.images.end(), [s](const auto& r) { return r.source == s; }) && std::none_of(plan.info.samplers.begin(), plan.info.samplers.end(), [s](const auto& r) { return r.source == s; });
        for (std::uint32_t w = 0; w < std::min<std::size_t>(source.dwordCount, source.dwords.size()); ++w) {
            const auto* value = sourceValue(source.dwords[w]);
            if (eligible && w < 2 && value != nullptr && (value->Opcode() == ShaderRecompiler::IrOpcode::LoadAddressU32 || value->Opcode() == ShaderRecompiler::IrOpcode::ReadConstBuffer)) candidates.push_back({value, users[0], w});
            else {
                if (eligible && w < 2 && value != nullptr && !value->HasImmediate()) ArchB::Recorder::Count("fix_buffer_base_refused_source");
                mark(mark, source.dwords[w]);
            }
        }
    }
    for (const auto& block : plan.controlFlow) mark(mark, block.condition);
    for (const auto& read : plan.srtReads) {
        const auto* value = sourceValue(read.value);
        if (value != nullptr) for (const auto* argument : value->Arguments()) mark(mark, argument);
    }
    std::map<const ShaderRecompiler::IrValue*, std::set<std::uint64_t>> addresses;
    std::map<std::uint64_t, std::set<const ShaderRecompiler::IrValue*>> readers;
    for (const auto& [raw, address] : capture.readTrace.slotReads) {
        const auto* value = sourceValue(raw);
        addresses[value].insert(address);
        readers[address].insert(value);
    }
    for (const auto& candidate : candidates) {
        if (forbidden.contains(candidate.value)) { ArchB::Recorder::Count("fix_buffer_base_refused_dependency"); continue; }
        if (std::count_if(candidates.begin(), candidates.end(), [&](const auto& other) { return other.value == candidate.value; }) != 1) { ArchB::Recorder::Count("fix_buffer_base_refused_alias"); continue; }
        const auto found = addresses.find(candidate.value);
        if (found == addresses.end() || found->second.size() != 1) { ArchB::Recorder::Count("fix_buffer_base_refused_unmapped"); continue; }
        const auto address = *found->second.begin();
        if (address % 4 != 0 || readers[address].size() != 1) { ArchB::Recorder::Count("fix_buffer_base_refused_alias"); continue; }
        std::size_t offset = 0, position = variant.words.size(), matches = 0;
        for (const auto& [begin, end] : variant.runs) {
            if (begin <= address && address < end && end - address >= 4 && (address - begin) % 4 == 0) { position = offset + (address - begin) / 4; ++matches; }
            offset += (end - begin) / 4;
        }
        if (matches != 1 || position >= variant.words.size() || position > std::numeric_limits<std::uint32_t>::max()) { ArchB::Recorder::Count("fix_buffer_base_refused_unmapped"); continue; }
        if (std::binary_search(variant.dataPositions.begin(), variant.dataPositions.end(), position) || WordMask(variant.feedback.memory, position) != 0) { ArchB::Recorder::Count("fix_buffer_base_refused_alias"); continue; }
        const auto word = candidate.resource * 4 + candidate.word;
        if (variant.words[position] != bound[word]) { ArchB::Recorder::Count("fix_buffer_base_refused_value"); continue; }
        std::vector<DispatchVariant::BufferBasePatch> patches;
        bool valid = true;
        for (const auto& read : plan.srtReads) {
            if (sourceValue(read.value) != candidate.value) continue;
            const auto& data = compiled.bindings[flat].guestDescriptor;
            if (read.flatOffset >= data.size() || data[read.flatOffset] != variant.words[position]) { valid = false; break; }
            patches.push_back({static_cast<std::uint32_t>(position), flat, read.flatOffset});
        }
        if (!valid || patches.empty()) { ArchB::Recorder::Count("fix_buffer_base_refused_value"); continue; }
        patches.push_back({static_cast<std::uint32_t>(position), buffers, word});
        variant.bufferBasePatches.insert(variant.bufferBasePatches.end(), patches.begin(), patches.end());
        variant.bufferBaseMasks.emplace_back(static_cast<std::uint32_t>(position), candidate.word == 0 ? 0xfffffffcu : 0xffffu);
    }
    std::sort(variant.bufferBaseMasks.begin(), variant.bufferBaseMasks.end());
    ArchB::Recorder::Count("fix_buffer_base_positions", variant.bufferBaseMasks.size());
}

bool PatchBufferBases(const DispatchVariant& variant, std::span<const std::uint32_t> words, ShaderRecompiler::RecompileResult& result) {
    if (words.size() != variant.words.size()) return false;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (((words[i] ^ variant.words[i]) & ~WordMask(variant.bufferBaseMasks, i)) != 0 && !std::binary_search(variant.dataPositions.begin(), variant.dataPositions.end(), i)) return false;
    }
    for (const auto& [position, mask] : variant.bufferBaseMasks) {
        if (position >= words.size() || ((words[position] ^ variant.words[position]) & ~mask) != 0) return false;
    }
    for (const auto& patch : variant.bufferBasePatches) {
        if (patch.position >= words.size() || patch.binding >= result.bindings.size() || patch.word >= result.bindings[patch.binding].guestDescriptor.size()) return false;
        result.bindings[patch.binding].guestDescriptor[patch.word] = words[patch.position];
    }
    for (const auto& binding : result.bindings) {
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
        for (std::size_t w = 0; w + 4 <= binding.guestDescriptor.size(); w += 4) {
            if (binding.guestDescriptor[w + 2] != 0 && binding.guestDescriptor[w] == 0 && (binding.guestDescriptor[w + 1] & 0xffffu) == 0) return false;
        }
    }
    return true;
}

}
