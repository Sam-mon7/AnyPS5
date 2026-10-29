#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::uint8_t Pointer = 1, Descriptor = 2, Constant = 4;

void CountWord(std::uint8_t kind, bool user, bool runtime) {
    using ArchB::Recorder;
    if (user) {
        if (kind & Pointer) Recorder::Count("fix_drawkeys_user_pointer_words");
        if (kind & Descriptor) Recorder::Count("fix_drawkeys_user_descriptor_words");
        if (kind & Constant) Recorder::Count("fix_drawkeys_user_constant_words");
        if (kind == 0) Recorder::Count("fix_drawkeys_user_unknown_words");
        Recorder::Count(runtime ? "fix_drawkeys_user_runtime_words" : "fix_drawkeys_user_structural_words");
    } else {
        if (kind & Pointer) Recorder::Count("fix_drawkeys_fragment_pointer_words");
        if (kind & Descriptor) Recorder::Count("fix_drawkeys_fragment_descriptor_words");
        if (kind & Constant) Recorder::Count("fix_drawkeys_fragment_constant_words");
        if (kind == 0) Recorder::Count("fix_drawkeys_fragment_unknown_words");
        Recorder::Count(runtime ? "fix_drawkeys_fragment_runtime_words" : "fix_drawkeys_fragment_structural_words");
    }
}

}

void RecordDrawKeyProof(DispatchVariant& variant, const DrawProgram& program, const ShaderRecompiler::ResourceCapture& capture, bool eligible) {
    if (!capture.readTrace.profile) return;
    auto proof = std::make_shared<DrawKeyProof>();
    proof->userWords = program.userData;
    proof->userClasses.resize(program.userData.size());
    proof->capturedClasses.resize(variant.words.size());
    const auto& plan = capture.plan;
    if (plan == nullptr) {
        variant.drawKeyProof = std::move(proof);
        return;
    }
    std::map<std::uint64_t, std::uint8_t> classes;
    std::set<std::pair<const ShaderRecompiler::IrValue*, std::uint8_t>> visited;
    const auto visit = [&](auto&& self, const ShaderRecompiler::IrValue* value, std::uint8_t kind) -> void {
        if (value == nullptr) return;
        value = value->Resolve();
        if (value->HasImmediate() || !visited.emplace(value, kind).second) return;
        if (value->Opcode() == ShaderRecompiler::IrOpcode::GetUserData && value->ArgumentCount() == 1) {
            const auto reg = value->Argument(0)->Register().index;
            if (reg >= plan->userDataBase && reg - plan->userDataBase < proof->userClasses.size()) proof->userClasses[reg - plan->userDataBase] |= kind;
            return;
        }
        if (value->Opcode() == ShaderRecompiler::IrOpcode::ReadConst && value->ArgumentCount() == 2) {
            const auto* slot = value->Argument(1)->Resolve();
            if (slot->HasImmediate() && slot->ImmediateU32() < plan->srtReads.size()) self(self, plan->srtReads[slot->ImmediateU32()].value, kind);
            return;
        }
        if (value->Opcode() == ShaderRecompiler::IrOpcode::LoadAddressU32 || value->Opcode() == ShaderRecompiler::IrOpcode::ReadConstBuffer) {
            for (const auto& [read, address] : capture.readTrace.slotReads) {
                if (read == value) classes[address] |= kind;
            }
            kind = Pointer;
        }
        for (const auto* argument : value->Arguments()) self(self, argument, kind);
    };
    for (const auto& source : plan->descriptorSources) {
        for (std::size_t word = 0; word < source.dwordCount && word < source.dwords.size(); ++word) visit(visit, source.dwords[word], Descriptor);
    }
    for (const auto& read : plan->srtReads) visit(visit, read.value, Constant);
    std::size_t position = 0;
    for (const auto& [begin, end] : variant.runs) {
        for (auto address = begin; address < end; address += sizeof(std::uint32_t), ++position) {
            if (const auto found = classes.find(address); found != classes.end()) proof->capturedClasses[position] = found->second;
            if (std::binary_search(variant.dataPositions.begin(), variant.dataPositions.end(), position)) proof->capturedClasses[position] |= Constant;
        }
    }
    for (std::size_t word = 0; word < program.userData.size() && word < 64; ++word) {
        if ((capture.userDataUse.asBase >> word & 1u) != 0) proof->userClasses[word] |= Pointer;
        if ((variant.compiled->userDataRegisterReads >> word & 1u) != 0 && proof->userClasses[word] == 0) proof->userClasses[word] = Constant;
    }
    proof->complete = eligible && plan->resourceTrackingComplete && plan->srtPlanComplete && !plan->requiresSpecializationMemory &&
        plan->userDataBase == program.firstUserSgpr && capture.snapshot.userData == program.userData && program.hiddenUserWords == 0 && !program.merged &&
        (program.binary.stage == ShaderRecompiler::ShaderStage::Vertex || program.binary.stage == ShaderRecompiler::ShaderStage::Fragment);
    if (proof->complete) {
        for (const auto origin : variant.runBases) {
            if (origin >= 0) proof->pointerWords |= variant.relocatableWords & (std::uint64_t{1} << origin);
        }
        const auto& result = *variant.compiled;
        std::uint64_t forwarded = 0;
        bool mapping = result.shaderDataUserWords.size() == result.memoryOffsetDword && result.memoryOffsetDword <= result.shaderDataDwords;
        const auto data = std::find_if(result.bindings.begin(), result.bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; });
        mapping &= data == result.bindings.end() ? result.shaderDataDwords <= result.pushConstants.size() / sizeof(std::uint32_t) : result.shaderDataDwords <= data->guestDescriptor.size();
        for (const auto word : result.shaderDataUserWords) {
            if (word >= 64 || word >= program.userData.size() || (forwarded & (std::uint64_t{1} << word)) != 0) {
                mapping = false;
                break;
            }
            forwarded |= std::uint64_t{1} << word;
        }
        if (mapping && forwarded == result.userDataRegisterReads) proof->runtimeWords = forwarded & ~(capture.userDataUse.asBase | capture.userDataUse.asValue);
        else proof->pointerWords &= ~result.userDataRegisterReads;
        proof->runtimeWords |= proof->pointerWords;
    }
    variant.drawKeyProof = std::move(proof);
}

std::shared_ptr<DispatchVariant> StructuralDrawVariant(const std::shared_ptr<DispatchVariant>& variant, const DrawProgram& program) {
    using ArchB::Recorder;
    const auto& proof = variant->drawKeyProof;
    if (proof == nullptr || proof->userWords.size() != program.userData.size() || !variant->shaderDataPatches.empty()) {
        Recorder::Count("fix_drawkeys_refused_proof");
        return nullptr;
    }
    std::uint64_t changed = 0;
    bool structural = false;
    for (std::size_t word = 0; word < program.userData.size(); ++word) {
        if (proof->userWords[word] == program.userData[word]) continue;
        if (ArchB::Recorder::Enabled() && word < 64) {
            static const auto names = [] {
                std::array<std::array<std::string, 64>, 2> result;
                for (std::size_t stage = 0; stage < result.size(); ++stage) {
                    for (std::size_t index = 0; index < result[stage].size(); ++index) result[stage][index] = std::string(stage == 0 ? "fix_drawkeys_vertex_user_word_" : "fix_drawkeys_fragment_user_word_") + std::to_string(index);
                }
                return result;
            }();
            Recorder::Count(names[program.binary.stage == ShaderRecompiler::ShaderStage::Fragment ? 1 : 0][word].c_str());
        }
        const bool runtime = proof->complete && word < 64 && (proof->runtimeWords >> word & 1u) != 0;
        CountWord(proof->userClasses[word], true, runtime);
        structural |= !runtime;
        if (word < 64) changed |= std::uint64_t{1} << word;
    }
    if (structural) {
        Recorder::Count("fix_drawkeys_refused_structural_word");
        return nullptr;
    }
    if (changed == 0) return variant;
    auto copy = std::make_shared<DispatchVariant>();
    copy->runs = variant->runs;
    copy->words = variant->words;
    copy->forgetSerial = variant->forgetSerial;
    copy->compiled = variant->compiled;
    copy->shader = variant->shader;
    copy->pushOffset = variant->pushOffset;
    copy->vertexInfo = variant->vertexInfo;
    copy->userDataMask = variant->userDataMask;
    copy->dataPositions = variant->dataPositions;
    copy->dataSlots = variant->dataSlots;
    copy->flatBinding = variant->flatBinding;
    copy->drawKeyProof = proof;
    for (std::size_t word = 0; word < program.userData.size() && word < 64; ++word) {
        if ((changed & proof->pointerWords & (std::uint64_t{1} << word)) == 0) continue;
        const auto from = proof->userWords[word];
        const auto to = program.userData[word];
        if (from == 0 || to == 0 || ((from ^ to) & 3u) != 0 || variant->runBases.size() != copy->runs.size()) {
            Recorder::Count("fix_drawkeys_refused_pointer");
            return nullptr;
        }
        const auto delta = static_cast<std::int64_t>(to & ~3u) - static_cast<std::int64_t>(from & ~3u);
        for (std::size_t run = 0; run < copy->runs.size(); ++run) {
            if (variant->runBases[run] != static_cast<std::int8_t>(word)) continue;
            auto& [begin, end] = copy->runs[run];
            if ((delta < 0 && begin < static_cast<std::uint64_t>(-delta)) || (delta > 0 && end > std::numeric_limits<std::uint64_t>::max() - delta)) {
                Recorder::Count("fix_drawkeys_refused_pointer");
                return nullptr;
            }
            begin += delta;
            end += delta;
        }
    }
    auto compiled = std::make_shared<ShaderRecompiler::RecompileResult>(*variant->compiled);
    for (std::size_t slot = 0; slot < compiled->shaderDataUserWords.size(); ++slot) {
        const auto word = compiled->shaderDataUserWords[slot];
        if (word >= 64 || (changed >> word & 1u) == 0) continue;
        bool patched = false;
        for (auto& binding : compiled->bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::ShaderData || slot >= binding.guestDescriptor.size()) continue;
            binding.guestDescriptor[slot] = program.userData[word];
            patched = true;
        }
        if (!patched && (slot + 1) * sizeof(std::uint32_t) <= compiled->pushConstants.size()) {
            std::memcpy(compiled->pushConstants.data() + slot * sizeof(std::uint32_t), &program.userData[word], sizeof(std::uint32_t));
            patched = true;
        }
        if (!patched) {
            Recorder::Count("fix_drawkeys_refused_push_mapping");
            return nullptr;
        }
    }
    copy->compiled = std::move(compiled);
    return copy;
}

void CountDrawFragmentDifferences(const DispatchVariant& variant) {
    if (!ArchB::Recorder::Enabled()) return;
    ArchB::Recorder::Count("fix_drawkeys_fragment_user_words_equal");
    std::vector<std::uint32_t> words;
    std::size_t position = 0;
    for (const auto& [begin, end] : variant.runs) {
        words.resize(static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t)));
        if (GuestMemory::CopyMapped(begin, std::as_writable_bytes(std::span(words))) != GuestMemory::Compare::Equal) {
            ArchB::Recorder::Count("fix_drawkeys_fragment_unreadable");
            position += words.size();
            continue;
        }
        for (const auto word : words) {
            if (word != variant.words[position]) {
                const auto kind = variant.drawKeyProof != nullptr && position < variant.drawKeyProof->capturedClasses.size() ? variant.drawKeyProof->capturedClasses[position] : 0;
                CountWord(kind, false, std::binary_search(variant.dataPositions.begin(), variant.dataPositions.end(), position));
            }
            ++position;
        }
    }
}

}
