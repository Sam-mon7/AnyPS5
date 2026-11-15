#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchGatherGraph.hpp"
#include "Optimization/DescriptorBindingBuilder.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace AgcDriver::DriverDetail {

namespace {


constexpr std::size_t MaxSources = 4096;
constexpr std::size_t MaxFlatWords = 65536;
constexpr std::size_t MaxNodes = 262144;
constexpr std::uint64_t MaxBufferBytes = 1ull << 30u;
constexpr std::uint8_t ScalarOnly = 1u;
constexpr std::uint8_t DisabledTypeZero = 2u;
constexpr std::uint8_t DisabledMeasured = 4u;

bool sameShape(const ShaderRecompiler::DescriptorBinding& before, const ShaderRecompiler::DescriptorBinding& after) {
    return before.kind == after.kind && before.role == after.role && before.descriptorSet == after.descriptorSet && before.binding == after.binding && before.count == after.count && before.readOnly == after.readOnly && before.bufferExtent == after.bufferExtent && before.Flags() == after.Flags();
}

ShaderRecompiler::ShaderBufferResource bufferResource(const ShaderRecompiler::DescriptorValue& value) {
    ShaderRecompiler::ShaderBufferResource resource;
    std::copy_n(value.dwords.begin(), 4u, resource.fields.begin());
    return resource;
}

bool sourceLayoutValid(const ShaderRecompiler::IrResourcePlan& plan) {
    if (plan.materializationSources.size() != plan.info.buffers.size()) return false;
    for (std::size_t index = 0; index < plan.info.buffers.size(); ++index) {
        const auto source = plan.materializationSources[index];
        if (source >= plan.descriptorSources.size() || source != plan.info.buffers[index].source || plan.descriptorSources[source].dwordCount != 4u || plan.info.buffers[index].imageAlias != ShaderRecompiler::BufferResource::NoImageAlias) return false;
    }
    for (const auto& source : plan.descriptorSources) {
        if (source.tableColumn) return false;
        for (const auto block : source.requiredBlocks) if (block >= plan.controlFlow.size()) return false;
    }
    for (const auto& block : plan.controlFlow) {
        if ((block.condition == nullptr && block.successors.size() > 1u) || (block.condition != nullptr && block.successors.size() != 2u)) return false;
        for (const auto successor : block.successors) if (successor >= plan.controlFlow.size()) return false;
    }
    std::vector<std::uint32_t> incoming(plan.controlFlow.size());
    std::vector<std::uint32_t> ready;
    for (const auto& block : plan.controlFlow) for (const auto successor : block.successors) ++incoming[successor];
    for (std::uint32_t block = 0; block < incoming.size(); ++block) if (incoming[block] == 0u) ready.push_back(block);
    std::size_t blocks = 0;
    while (!ready.empty()) {
        const auto block = ready.back();
        ready.pop_back();
        ++blocks;
        for (const auto successor : plan.controlFlow[block].successors) if (--incoming[successor] == 0u) ready.push_back(successor);
    }
    if (blocks != plan.controlFlow.size()) return false;
    std::vector<std::uint8_t> slots(plan.srtReads.size());
    for (const auto& read : plan.srtReads) {
        if (read.flatOffset >= slots.size() || slots[read.flatOffset] != 0u) return false;
        slots[read.flatOffset] = 1u;
    }
    return true;
}

}

std::shared_ptr<const DispatchGatherGraph> DispatchGatherGraph::Create(const ShaderRecompiler::ResourceCapture& capture, const ShaderRecompiler::RecompileResult& compiled, DispatchGatherRefusal& refusal) {
    const auto reject = [&](DispatchGatherRefusal reason) -> std::shared_ptr<const DispatchGatherGraph> {
        refusal = reason;
        return nullptr;
    };
    if (!capture.plan) return reject(DispatchGatherRefusal::MissingPlan);
    const auto& plan = *capture.plan;
    const auto& snapshot = capture.snapshot;
    if (!plan.resourceTrackingComplete || !plan.srtPlanComplete) return reject(DispatchGatherRefusal::IncompletePlan);
    if (plan.stage != ShaderRecompiler::IrShaderStage::Compute || ShaderRecompiler::GcnGuestShaders()) return reject(DispatchGatherRefusal::NonCompute);
    if (compiled.bdaAbiVersion != 0u || compiled.bdaWrites || plan.info.bdaWrites) return reject(DispatchGatherRefusal::Bda);
    if (!plan.info.images.empty() || !plan.info.samplers.empty() || !snapshot.tables.tables.empty() || !snapshot.tables.nested.empty() || !snapshot.registeredImages.empty()) return reject(DispatchGatherRefusal::ImageResources);
    if (plan.uniformFill.fill.kind != ShaderRecompiler::UniformFillKind::None) return reject(DispatchGatherRefusal::UniformFill);
    if (!plan.guardedSrtSlots.empty()) {
        if (plan.srtGuardOffset != plan.srtReads.size() || !std::is_sorted(plan.guardedSrtSlots.begin(), plan.guardedSrtSlots.end()) || std::adjacent_find(plan.guardedSrtSlots.begin(), plan.guardedSrtSlots.end()) != plan.guardedSrtSlots.end()) return reject(DispatchGatherRefusal::GuardedSlots);
        for (const auto slot : plan.guardedSrtSlots) if (slot >= plan.srtReads.size()) return reject(DispatchGatherRefusal::GuardedSlots);
    }
    if (snapshot.nullRootReads != 0u || !snapshot.srtPoison.empty() || compiled.poisonedSrtReads != 0u || compiled.imageTableFaults != 0u || compiled.descriptorFaults != 0u) return reject(DispatchGatherRefusal::Poison);
    if (plan.descriptorSources.size() > MaxSources || plan.srtReads.size() > MaxFlatWords || plan.userDataCount > 64u || plan.controlFlow.size() > MaxSources || plan.valueStorage.Size() > MaxNodes) return reject(DispatchGatherRefusal::Unbounded);
    if (!sourceLayoutValid(plan) || snapshot.buffers.size() != plan.info.buffers.size() || snapshot.flattenedSrt.size() != plan.srtReads.size() + plan.guardedSrtSlots.size()) return reject(DispatchGatherRefusal::SourceLayout);
    if (plan.info.dispatchThreadLimit || compiled.runtimeImageCount != 0u || compiled.functionLdsBuffer || compiled.meshWindowBuffer || compiled.scratchPoolSlotDwords != 0u) return reject(DispatchGatherRefusal::BindingLayout);
    for (const auto& descriptor : snapshot.buffers) {
        if (descriptor.dwordCount != 4u || descriptor.availableDwordMask != 0u) return reject(DispatchGatherRefusal::SourceLayout);
        if (!ShaderRecompiler::BufferDescriptorPresent(descriptor)) return reject(DispatchGatherRefusal::AbsentBuffer);
        if (bufferResource(descriptor).GetSize() > MaxBufferBytes) return reject(DispatchGatherRefusal::Unbounded);
    }
    auto graph = std::make_shared<DispatchGatherGraph>();
    graph->plan = capture.plan;
    graph->tape = ShaderRecompiler::Detail::SrtTape::CompileStrict(plan);
    if (!graph->tape || graph->tape->Nodes() > MaxNodes) return reject(DispatchGatherRefusal::UnsupportedGraph);
    graph->structuralDescriptors = snapshot.buffers;
    graph->bufferNormalization.resize(plan.info.buffers.size());
    for (std::uint32_t index = 0; index < plan.info.buffers.size(); ++index) {
        const auto& buffer = plan.info.buffers[index];
        bool accessed = false;
        bool untyped = true;
        bool measured = true;
        for (const auto& memory : plan.memoryInfo) {
            if (memory.kind != ShaderRecompiler::ResourceKind::Buffer || memory.resource != index) continue;
            accessed = true;
            untyped = untyped && !memory.typed && !memory.gpuDescriptor;
            measured = measured && memory.dataDwords == 1u && memory.componentCount == 1u;
        }
        auto& normalization = graph->bufferNormalization[index];
        if (buffer.scalar && !buffer.written && !buffer.atomic && !buffer.formatted && buffer.imageAlias == ShaderRecompiler::BufferResource::NoImageAlias && !accessed) normalization |= ScalarOnly;
        if (!buffer.atomic && !buffer.scalar && buffer.imageAlias == ShaderRecompiler::BufferResource::NoImageAlias && accessed && untyped) {
            normalization |= DisabledTypeZero;
            if (measured && !buffer.read) normalization |= DisabledMeasured;
        }
    }
    graph->structuralFlat = snapshot.flattenedSrt;
    if (snapshot.userData.size() != plan.userDataCount) return reject(DispatchGatherRefusal::UserData);
    graph->structuralUserData = snapshot.userData;
    graph->dynamicUserDataMask = graph->tape->UserDataMask();
    graph->bindings = compiled.bindings;
    graph->bindingResources.resize(compiled.bindings.size());
    graph->pushConstants = compiled.pushConstants;
    graph->shaderDataUserWords = compiled.shaderDataUserWords;
    graph->pipeline = compiled.Identity();
    std::size_t bufferBindings = 0;
    bool shaderData = false;
    for (std::size_t index = 0; index < compiled.bindings.size(); ++index) {
        const auto& binding = compiled.bindings[index];
        switch (binding.role) {
        case ShaderRecompiler::DescriptorRole::GuestBuffers: {
            if (++bufferBindings != 1u || binding.count != snapshot.buffers.size() || binding.guestDescriptor.size() != snapshot.buffers.size() * 4u || binding.bufferExtent.size() != snapshot.buffers.size()) return reject(DispatchGatherRefusal::BindingLayout);
            auto& resources = graph->bindingResources[index];
            for (std::uint32_t resource = 0; resource < snapshot.buffers.size(); ++resource) {
                if (!std::equal(snapshot.buffers[resource].dwords.begin(), snapshot.buffers[resource].dwords.begin() + 4u, binding.guestDescriptor.begin() + resource * 4u)) return reject(DispatchGatherRefusal::BindingLayout);
                resources.push_back(resource);
                const auto buffer = bufferResource(snapshot.buffers[resource]);
                const auto bytes = buffer.GetSize();
                graph->structuralBuffers.push_back({binding.binding, resource, buffer.Base48(), bytes, bytes});
            }
            break;
        }
        case ShaderRecompiler::DescriptorRole::FlattenedSrt:
            if (binding.guestDescriptor != snapshot.flattenedSrt) return reject(DispatchGatherRefusal::BindingLayout);
            break;
        case ShaderRecompiler::DescriptorRole::ShaderData:
            if (shaderData || binding.guestDescriptor.size() * sizeof(std::uint32_t) != compiled.shaderDataDwords * sizeof(std::uint32_t) || binding.guestDescriptor.size() < compiled.shaderDataUserWords.size()) return reject(DispatchGatherRefusal::BindingLayout);
            shaderData = true;
            break;
        case ShaderRecompiler::DescriptorRole::FaultBuffer:
            if (!binding.guestDescriptor.empty()) return reject(DispatchGatherRefusal::BindingLayout);
            break;
        default: return reject(DispatchGatherRefusal::BindingLayout);
        }
    }
    if (!compiled.pushConstants.empty() && (compiled.pushConstants.size() != static_cast<std::size_t>(compiled.shaderDataDwords) * sizeof(std::uint32_t) || compiled.pushConstants.size() < compiled.shaderDataUserWords.size() * sizeof(std::uint32_t))) return reject(DispatchGatherRefusal::BindingLayout);
    for (const auto word : graph->shaderDataUserWords) {
        if (word >= plan.userDataCount || word >= snapshot.userData.size()) return reject(DispatchGatherRefusal::UserData);
        graph->dynamicUserDataMask |= std::uint64_t{1} << word;
    }
    refusal = DispatchGatherRefusal::None;
    return graph;
}

DispatchGatherRefusal DispatchGatherGraph::Evaluate(std::span<const std::uint32_t> userData, std::uint64_t shaderBase, ShaderRecompiler::SrtMemoryReader guardedRead, void* context, DispatchGatherUse& use) const {
    use.gathered = false;
    if (userData.size() < plan->userDataCount) return DispatchGatherRefusal::UserData;
    if (guardedRead == nullptr) return DispatchGatherRefusal::ReadFailed;
    ShaderRecompiler::SrtRuntime runtime;
    runtime.userData = userData;
    runtime.shaderBase = shaderBase;
    runtime.readMemory = guardedRead;
    runtime.readSpecializationMemory = guardedRead;
    runtime.userContext = context;
    if (!tape->RunStrict(*plan, runtime, use.descriptors, use.flat, use.activeSources, &use.structuralTrace)) return DispatchGatherRefusal::ReadFailed;
    if (use.descriptors.size() != bufferNormalization.size()) return DispatchGatherRefusal::SourceLayout;
    for (std::size_t index = 0; index < use.descriptors.size(); ++index) {
        auto& descriptor = use.descriptors[index];
        const auto normalization = bufferNormalization[index];
        const auto raw = bufferResource(descriptor);
        const bool disabled = ((normalization & DisabledTypeZero) != 0u && raw.Type() == 0u && raw.RawFormat() == 0u)
            || ((normalization & DisabledMeasured) != 0u && (raw.Type() == 2u || raw.Type() == 3u) && raw.RawFormat() <= static_cast<std::uint32_t>(ShaderRecompiler::IrBufferFormat::Format32_32_32_32Float));
        if (disabled && std::any_of(descriptor.dwords.begin(), descriptor.dwords.begin() + 4u, [](std::uint32_t word) { return word != 0u; })) return DispatchGatherRefusal::DescriptorNormalization;
        if ((normalization & ScalarOnly) != 0u) {
            descriptor.dwords[1] &= ~0x40000000u;
            descriptor.dwords[3] = 0u;
        }
    }
    use.userData.assign(userData.begin(), userData.begin() + plan->userDataCount);
    use.flat.resize(plan->srtReads.size() + plan->guardedSrtSlots.size(), 0u);
    use.bindings = bindings;
    use.pushConstants.assign(pushConstants.begin(), pushConstants.end());
    use.buffers.clear();
    use.buffers.reserve(plan->info.buffers.size());
    for (std::size_t index = 0; index < use.bindings.size(); ++index) {
        auto& binding = use.bindings[index];
        if (binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
            const auto& resources = bindingResources[index];
            for (std::size_t element = 0; element < resources.size(); ++element) {
                const auto& descriptor = use.descriptors[resources[element]];
                const auto resource = bufferResource(descriptor);
                const auto size = resource.GetSize();
                const auto bounded = size;
                if (bounded > MaxBufferBytes) return DispatchGatherRefusal::Unbounded;
                std::copy_n(descriptor.dwords.begin(), 4u, binding.guestDescriptor.begin() + element * 4u);
                use.buffers.push_back({binding.binding, static_cast<std::uint32_t>(element), resource.Base48(), size, bounded});
            }
        } else if (binding.role == ShaderRecompiler::DescriptorRole::FlattenedSrt) {
            binding.guestDescriptor.assign(use.flat.begin(), use.flat.end());
        } else if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData) {
            for (std::size_t word = 0; word < shaderDataUserWords.size(); ++word) binding.guestDescriptor[word] = userData[shaderDataUserWords[word]];
        }
    }
    for (std::size_t word = 0; word < shaderDataUserWords.size() && !use.pushConstants.empty(); ++word) {
        const auto value = userData[shaderDataUserWords[word]];
        std::memcpy(use.pushConstants.data() + word * sizeof(value), &value, sizeof(value));
    }
    use.pipeline = pipeline;
    use.gathered = true;
    return DispatchGatherRefusal::None;
}

bool DispatchGatherGraph::SetStructure(const DispatchGatherUse& use) const {
    if (!use.gathered) return false;
    std::call_once(structureOnce, [&] {
        structuralTrace = use.structuralTrace;
        structureReady.store(true, std::memory_order_release);
    });
    return structuralTrace == use.structuralTrace;
}

DispatchGatherDifference DispatchGatherGraph::Matches(const DispatchGatherUse& use) const {
    if (!structureReady.load(std::memory_order_acquire) || structuralTrace != use.structuralTrace) return DispatchGatherDifference::Structure;
    if (!use.gathered || use.descriptors.size() != structuralDescriptors.size()) return DispatchGatherDifference::DescriptorShape;
    for (std::size_t index = 0; index < structuralDescriptors.size(); ++index) {
        const auto& before = structuralDescriptors[index];
        const auto& after = use.descriptors[index];
        if (before.dwordCount != after.dwordCount || before.availableDwordMask != after.availableDwordMask) return DispatchGatherDifference::DescriptorShape;
        if ((before.dwords[0] & 3u) != (after.dwords[0] & 3u) || (before.dwords[1] & 0xffff0000u) != (after.dwords[1] & 0xffff0000u) || before.dwords[2] != after.dwords[2] || before.dwords[3] != after.dwords[3] || ShaderRecompiler::BufferDescriptorPresent(before) != ShaderRecompiler::BufferDescriptorPresent(after)) return DispatchGatherDifference::DescriptorWords;
    }
    if (use.userData.size() != structuralUserData.size()) return DispatchGatherDifference::UserData;
    for (std::size_t word = 0; word < structuralUserData.size(); ++word) {
        if ((dynamicUserDataMask & (std::uint64_t{1} << word)) == 0u && use.userData[word] != structuralUserData[word]) return DispatchGatherDifference::UserData;
    }
    if (use.buffers.size() != structuralBuffers.size()) return DispatchGatherDifference::BindingShape;
    for (std::size_t first = 0; first < use.buffers.size(); ++first) {
        for (std::size_t second = first + 1u; second < use.buffers.size(); ++second) {
            const auto& beforeFirst = structuralBuffers[first];
            const auto& beforeSecond = structuralBuffers[second];
            const auto& afterFirst = use.buffers[first];
            const auto& afterSecond = use.buffers[second];
            const auto overlaps = [](const DispatchGatherBuffer& left, const DispatchGatherBuffer& right) {
                return left.address != 0u && right.address != 0u && left.accessBytes != 0u && right.accessBytes != 0u && left.address < right.address + right.accessBytes && right.address < left.address + left.accessBytes;
            };
            const bool beforeAlias = overlaps(beforeFirst, beforeSecond);
            const bool afterAlias = overlaps(afterFirst, afterSecond);
            if (beforeAlias != afterAlias || (beforeAlias && beforeFirst.address - beforeSecond.address != afterFirst.address - afterSecond.address)) return DispatchGatherDifference::Alias;
        }
    }
    if (use.flat.size() != structuralFlat.size()) return DispatchGatherDifference::FlatShape;
    for (std::size_t slot = 0; slot < structuralFlat.size(); ++slot) {
        if (slot < plan->pureFlatSlots.size() && plan->pureFlatSlots[slot] != 0u) continue;
        if (use.flat[slot] != structuralFlat[slot]) return DispatchGatherDifference::FlatStructure;
    }
    return DispatchGatherDifference::None;
}

DispatchGatherDifference DispatchGatherGraph::CompareMaterialized(const DispatchGatherUse& use, const ShaderRecompiler::RecompileResult& compiled) const {
    if (!use.gathered || use.bindings.size() != compiled.bindings.size()) return DispatchGatherDifference::BindingShape;
    for (std::size_t index = 0; index < use.bindings.size(); ++index) {
        if (!sameShape(use.bindings[index], compiled.bindings[index])) return DispatchGatherDifference::BindingShape;
        if (use.bindings[index].guestDescriptor != compiled.bindings[index].guestDescriptor) return DispatchGatherDifference::BindingWords;
    }
    if (use.pushConstants != compiled.pushConstants) return DispatchGatherDifference::PushBytes;
    if (use.pipeline != compiled.Identity()) return DispatchGatherDifference::Pipeline;
    return DispatchGatherDifference::None;
}

DispatchGatherDifference DispatchGatherGraph::CompareCapture(const DispatchGatherUse& use, const ShaderRecompiler::ResourceCapture& capture) const {
    if (!use.gathered || use.descriptors != capture.snapshot.buffers) return DispatchGatherDifference::DescriptorWords;
    if (use.flat != capture.snapshot.flattenedSrt) return DispatchGatherDifference::FlatStructure;
    if (use.userData != capture.snapshot.userData) return DispatchGatherDifference::UserData;
    return DispatchGatherDifference::None;
}

const char* DispatchGatherRefusalName(DispatchGatherRefusal refusal) {
    constexpr std::array names{"none", "missing_plan", "incomplete_plan", "non_compute", "bda", "image_resources", "uniform_fill", "guarded_slots", "poison", "unbounded", "unsupported_graph", "binding_layout", "source_layout", "user_data", "read_failed", "descriptor_normalization", "absent_buffer"};
    const auto index = static_cast<std::size_t>(refusal);
    return index < names.size() ? names[index] : "unknown";
}

const char* DispatchGatherDifferenceName(DispatchGatherDifference difference) {
    constexpr std::array names{"none", "descriptor_shape", "descriptor_words", "flat_shape", "flat_structure", "binding_shape", "binding_words", "push_bytes", "pipeline", "structure", "user_data", "alias"};
    const auto index = static_cast<std::size_t>(difference);
    return index < names.size() ? names[index] : "unknown";
}

}
