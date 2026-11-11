#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/GatherShadow.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/WriteEvidence.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::uint64_t Prime = 0x100000001b3ull;
constexpr std::uint64_t PointerBits = 0x0000fffffffffffcull;

std::uint64_t AlignedWord(std::uint32_t word) {
    return word & ~std::uint32_t{3};
}

std::uint64_t SignatureSeed(std::uint32_t pushOffset, std::size_t users, std::size_t runs) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    hash = (hash ^ pushOffset) * Prime;
    hash = (hash ^ users) * Prime;
    return (hash ^ runs) * Prime;
}

bool Strict() {
    static const bool strict = [] {
        const char* value = std::getenv("APS5_DRAW_GATHER_SHADOW_STRICT");
        return value != nullptr && value[0] == '1';
    }();
    return strict;
}

struct StrictTotals {
    std::atomic<std::uint64_t> hits{0}, matches{0}, pairs{0}, falseDiffers{0}, plans{0}, values{0};
    void Report(std::uint64_t count) const {
        if ((count & (count - 1u)) != 0) return;
        std::fprintf(stderr, "[gather-shadow] strict totals: %llu hits, %llu matched, %llu validations compared, %llu false differs, %llu plans checked on commit, %llu values checked\n", static_cast<unsigned long long>(hits.load()), static_cast<unsigned long long>(matches.load()), static_cast<unsigned long long>(pairs.load()), static_cast<unsigned long long>(falseDiffers.load()), static_cast<unsigned long long>(plans.load()), static_cast<unsigned long long>(values.load()));
    }
};

StrictTotals& strictTotals() {
    static StrictTotals totals;
    return totals;
}

GatherDiffer UserDiffer(GatherWord word) {
    switch (word) {
        case GatherWord::Hidden: return GatherDiffer::UserHidden;
        case GatherWord::Callee: return GatherDiffer::UserCallee;
        case GatherWord::WalkedValue: return GatherDiffer::UserWalked;
        case GatherWord::TableBase: return GatherDiffer::TableBaseAlignment;
        default: return GatherDiffer::UserStructure;
    }
}

}

const char* GatherWordName(GatherWord word) {
    constexpr std::array<const char*, static_cast<std::size_t>(GatherWord::Count)> names{"structure", "hidden", "callee", "walked_value", "table_base", "value_word", "push_word", "flat_data", "buffer_base", "nested_pointer", "feedback"};
    return word < GatherWord::Count ? names[static_cast<std::size_t>(word)] : "unknown";
}

const char* GatherDifferName(GatherDiffer differ) {
    constexpr std::array<const char*, static_cast<std::size_t>(GatherDiffer::Count)> names{"none", "layout", "user_structure", "user_hidden", "user_callee", "user_walked", "table_base_alignment", "run_structure", "nested_pointer", "pointer_unreadable", "unreadable", "pending"};
    return differ < GatherDiffer::Count ? names[static_cast<std::size_t>(differ)] : "unknown";
}

const char* GatherElementName(GatherElement element) {
    constexpr std::array<const char*, static_cast<std::size_t>(GatherElement::Count)> names{"vsharp", "flat", "shaderdata", "push", "indirect"};
    return element < GatherElement::Count ? names[static_cast<std::size_t>(element)] : "unknown";
}

std::shared_ptr<const GatherTemplate> BuildGatherTemplate(const DispatchVariant& variant, std::span<const std::uint32_t> userData, std::uint32_t hiddenUserWords, const GatherOptions& options) {
    auto shape = std::make_shared<GatherTemplate>();
    shape->compiled = variant.compiled;
    shape->pushOffset = variant.pushOffset;
    if (variant.compiled == nullptr) {
        shape->refusal = "no_program";
        return shape;
    }
    const auto& compiled = *variant.compiled;
    if (std::any_of(compiled.bindings.begin(), compiled.bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable; })) {
        shape->refusal = "bda";
        return shape;
    }
    std::size_t total = 0;
    for (const auto& [begin, end] : variant.runs) total += static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
    if (total != variant.words.size() || (!variant.runBases.empty() && variant.runBases.size() != variant.runs.size())) {
        shape->refusal = "run_layout";
        return shape;
    }
    shape->user.assign(userData.begin(), userData.end());
    shape->userMask.assign(userData.size(), 0);
    shape->userClass.assign(userData.size(), GatherWord::Structure);
    const auto& forwarded = compiled.shaderDataUserWords;
    for (std::uint32_t index = 0; index < userData.size(); ++index) {
        const auto bit = index < 64 ? std::uint64_t{1} << index : 0;
        auto& kind = shape->userClass[index];
        auto& mask = shape->userMask[index];
        const bool isForwarded = std::find(forwarded.begin(), forwarded.end(), index) != forwarded.end();
        if (index < hiddenUserWords) kind = GatherWord::Hidden;
        else if (bit == 0) kind = GatherWord::Structure;
        else if ((variant.relocatableWords & bit) != 0) {
            kind = GatherWord::TableBase;
            mask = 0xfffffffcu;
        } else if ((variant.valueWords & bit) != 0) {
            kind = GatherWord::ValueWord;
            mask = 0xffffffffu;
        } else if ((variant.calleeWords & bit) != 0) kind = GatherWord::Callee;
        else if (isForwarded && (variant.userWordsWalked & bit) == 0) {
            kind = GatherWord::PushWord;
            mask = 0xffffffffu;
        } else if ((variant.userWordsWalked & bit) != 0) kind = GatherWord::WalkedValue;
        if (options.feedback) {
            const auto bits = WordMask(variant.feedback.userData, index);
            if ((bits & ~mask) != 0 && (kind == GatherWord::Structure || kind == GatherWord::WalkedValue)) kind = GatherWord::Feedback;
            mask |= bits;
        }
    }
    for (std::uint32_t slot = 0; slot < forwarded.size(); ++slot) {
        const auto index = forwarded[slot];
        if (index >= userData.size()) continue;
        const auto kind = shape->userClass[index];
        if (kind == GatherWord::TableBase || kind == GatherWord::ValueWord || kind == GatherWord::PushWord) shape->targets.push_back({true, true, index, 0, slot, 0xffffffffu});
    }
    for (const auto& [index, binding, word] : variant.valueSlots) {
        if (index < userData.size() && shape->userClass[index] == GatherWord::ValueWord) shape->targets.push_back({true, false, index, binding, word, 0xffffffffu});
    }
    shape->nested = options.nested && variant.nestedPointerProof && variant.runParents.size() == variant.runs.size() && std::any_of(variant.runParents.begin(), variant.runParents.end(), [](const auto& parent) { return parent.run != DispatchVariant::NoFlatBinding; });
    std::uint32_t offset = 0;
    shape->runs.reserve(variant.runs.size());
    for (std::size_t run = 0; run < variant.runs.size(); ++run) {
        GatherRun item;
        item.address = variant.runs[run].first;
        item.first = offset;
        item.count = static_cast<std::uint32_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t));
        if (run < variant.runBases.size() && variant.runBases[run] >= 0 && static_cast<std::size_t>(variant.runBases[run]) < userData.size() && shape->userClass[static_cast<std::size_t>(variant.runBases[run])] == GatherWord::TableBase) item.base = variant.runBases[run];
        if (shape->nested && variant.runParents[run].run != DispatchVariant::NoFlatBinding) {
            item.parentRun = variant.runParents[run].run;
            item.parentWord = variant.runParents[run].word;
        }
        offset += item.count;
        shape->runs.push_back(item);
    }
    shape->words = variant.words;
    shape->wordMask.assign(variant.words.size(), 0);
    shape->wordClass.assign(variant.words.size(), GatherWord::Structure);
    if (options.dataWords && variant.flatBinding != DispatchVariant::NoFlatBinding && variant.dataSlots.size() == variant.dataPositions.size()) {
        for (std::size_t k = 0; k < variant.dataPositions.size(); ++k) {
            const auto position = variant.dataPositions[k];
            if (position >= shape->words.size()) continue;
            shape->wordMask[position] = 0xffffffffu;
            shape->wordClass[position] = GatherWord::FlatData;
            shape->targets.push_back({false, false, position, variant.flatBinding, variant.dataSlots[k], 0xffffffffu});
        }
    }
    if (options.bufferBases) {
        for (const auto& [position, mask] : variant.bufferBaseMasks) {
            if (position >= shape->words.size()) continue;
            shape->wordMask[position] |= mask;
            shape->wordClass[position] = GatherWord::BufferBase;
        }
        for (const auto& patch : variant.bufferBasePatches) {
            if (patch.position >= shape->words.size() || WordMask(variant.bufferBaseMasks, patch.position) == 0) continue;
            shape->targets.push_back({false, false, patch.position, patch.binding, patch.word, 0xffffffffu});
        }
    }
    if (shape->nested) {
        for (const auto& run : shape->runs) {
            if (run.parentRun == DispatchVariant::NoFlatBinding) continue;
            if (run.parentRun >= shape->runs.size()) {
                shape->refusal = "nested_layout";
                return shape;
            }
            const auto position = shape->runs[run.parentRun].first + run.parentWord;
            if (position + 1 >= shape->words.size()) {
                shape->refusal = "nested_layout";
                return shape;
            }
            shape->wordMask[position] |= 0xfffffffcu;
            shape->wordMask[position + 1] = 0xffffffffu;
            shape->wordClass[position] = GatherWord::NestedPointer;
            shape->wordClass[position + 1] = GatherWord::NestedPointer;
        }
        for (const auto& [position, binding, word, mask] : variant.nestedPointerPatches) {
            if (position < shape->words.size()) shape->targets.push_back({false, false, position, binding, word, mask});
        }
    }
    if (options.feedback) {
        for (const auto& [position, bits] : variant.feedback.memory) {
            if (position >= shape->words.size()) continue;
            if ((bits & ~shape->wordMask[position]) != 0 && shape->wordClass[position] == GatherWord::Structure) shape->wordClass[position] = GatherWord::Feedback;
            shape->wordMask[position] |= bits;
        }
    }
    auto hash = SignatureSeed(shape->pushOffset, shape->user.size(), shape->runs.size());
    for (std::size_t index = 0; index < shape->user.size(); ++index) hash = (hash ^ (shape->user[index] & ~shape->userMask[index])) * Prime;
    for (std::size_t word = 0; word < shape->words.size(); ++word) hash = (hash ^ (shape->words[word] & ~shape->wordMask[word])) * Prime;
    shape->signature = hash;
    return shape;
}

GatherOutcome EvaluateGather(const GatherTemplate& shape, std::span<const std::uint32_t> liveUser, const GatherReader& read, std::vector<std::uint32_t>& liveWords, std::vector<std::uint64_t>* liveAddresses, bool diagnose) {
    GatherOutcome outcome;
    if (liveUser.size() != shape.user.size()) {
        outcome.differ = GatherDiffer::Layout;
        return outcome;
    }
    liveWords.resize(shape.words.size());
    thread_local std::vector<std::uint8_t> states;
    thread_local std::vector<std::uint64_t> addresses;
    states.assign(shape.runs.size(), 0);
    addresses.assign(shape.runs.size(), 0);
    const auto visit = [&](auto&& self, std::size_t index) -> bool {
        if (states[index] == 2) return true;
        if (states[index] == 1) {
            outcome.differ = GatherDiffer::Layout;
            return false;
        }
        states[index] = 1;
        const auto& run = shape.runs[index];
        auto address = run.address;
        if (run.base >= 0) {
            const auto user = static_cast<std::size_t>(run.base);
            const auto live = liveUser[user];
            if (live == 0 || ((live ^ shape.user[user]) & 3u) != 0) {
                outcome.differ = GatherDiffer::TableBaseAlignment;
                return false;
            }
            address = run.address + (AlignedWord(live) - AlignedWord(shape.user[user]));
        } else if (run.parentRun != DispatchVariant::NoFlatBinding) {
            if (!self(self, run.parentRun)) return false;
            const auto position = shape.runs[run.parentRun].first + run.parentWord;
            std::uint64_t live = 0, old = 0;
            std::memcpy(&live, liveWords.data() + position, sizeof(live));
            std::memcpy(&old, shape.words.data() + position, sizeof(old));
            if (live == 0) {
                outcome.differ = GatherDiffer::PointerUnreadable;
                return false;
            }
            if (((live ^ old) & 3u) != 0) {
                outcome.differ = GatherDiffer::NestedPointer;
                return false;
            }
            address = run.address + ((live & PointerBits) - (old & PointerBits));
            ++outcome.nestedRuns;
        }
        if (run.count != 0 && !read(address, std::span(liveWords).subspan(run.first, run.count))) {
            outcome.differ = run.parentRun != DispatchVariant::NoFlatBinding ? GatherDiffer::PointerUnreadable : GatherDiffer::Unreadable;
            return false;
        }
        addresses[index] = address;
        states[index] = 2;
        return true;
    };
    for (std::size_t index = 0; index < shape.runs.size(); ++index) {
        if (!visit(visit, index)) {
            if (liveAddresses != nullptr) liveAddresses->assign(addresses.begin(), addresses.end());
            return outcome;
        }
    }
    if (liveAddresses != nullptr) liveAddresses->assign(addresses.begin(), addresses.end());
    auto hash = SignatureSeed(shape.pushOffset, shape.user.size(), shape.runs.size());
    GatherDiffer first = GatherDiffer::None;
    for (std::size_t index = 0; index < liveUser.size(); ++index) {
        const auto live = liveUser[index];
        hash = (hash ^ (live & ~shape.userMask[index])) * Prime;
        const auto changed = live ^ shape.user[index];
        if (changed == 0) continue;
        outcome.wordsChanged = true;
        if ((changed & ~shape.userMask[index]) != 0 && first == GatherDiffer::None) first = UserDiffer(shape.userClass[index]);
        if ((changed & shape.userMask[index]) != 0 && shape.userClass[index] == GatherWord::Feedback) outcome.feedbackChanged = true;
    }
    for (std::size_t word = 0; word < liveWords.size(); ++word) {
        const auto live = liveWords[word];
        hash = (hash ^ (live & ~shape.wordMask[word])) * Prime;
        const auto changed = live ^ shape.words[word];
        if (changed == 0) continue;
        outcome.wordsChanged = true;
        if ((changed & ~shape.wordMask[word]) != 0 && first == GatherDiffer::None) first = shape.wordClass[word] == GatherWord::NestedPointer ? GatherDiffer::NestedPointer : GatherDiffer::RunStructure;
        if ((changed & shape.wordMask[word]) != 0 && shape.wordClass[word] == GatherWord::Feedback) outcome.feedbackChanged = true;
    }
    outcome.signature = hash;
    outcome.matched = hash == shape.signature;
    if (!outcome.matched || diagnose) outcome.differ = first;
    if (outcome.matched && first != GatherDiffer::None) ArchB::Recorder::Count("gather_shadow_signature_collisions");
    return outcome;
}

void FillGatherPlan(const GatherTemplate& shape, std::span<const std::uint32_t> liveUser, std::span<const std::uint32_t> liveWords, GatherStagePlan& plan) {
    plan.words.clear();
    plan.vsharps.clear();
    plan.slots.clear();
    plan.elements = {};
    plan.bytes = {};
    if (shape.compiled == nullptr) return;
    const auto& compiled = *shape.compiled;
    using Role = ShaderRecompiler::DescriptorRole;
    bool flat = false, shaderData = false;
    for (const auto& target : shape.targets) {
        const auto source = target.user ? (target.source < liveUser.size() ? liveUser[target.source] : 0u) : (target.source < liveWords.size() ? liveWords[target.source] : 0u);
        const auto value = source & target.mask;
        if (target.slot) {
            plan.slots.emplace_back(target.word, value);
            continue;
        }
        if (target.binding >= compiled.bindings.size()) continue;
        const auto& binding = compiled.bindings[target.binding];
        if (binding.role == Role::GuestBuffers) {
            const auto element = target.word / 4u;
            auto found = std::find_if(plan.vsharps.begin(), plan.vsharps.end(), [&](const GatherVsharp& vsharp) { return vsharp.binding == target.binding && vsharp.element == element; });
            if (found == plan.vsharps.end()) {
                GatherVsharp vsharp;
                vsharp.binding = target.binding;
                vsharp.element = element;
                if (static_cast<std::size_t>(element) * 4u + 4u <= binding.guestDescriptor.size()) std::copy_n(binding.guestDescriptor.begin() + static_cast<std::ptrdiff_t>(element) * 4, 4, vsharp.words.begin());
                plan.vsharps.push_back(vsharp);
                found = plan.vsharps.end() - 1;
            }
            found->words[target.word % 4u] = value;
            continue;
        }
        plan.words.push_back({target.binding, target.word, value, target.user ? (target.source < shape.userClass.size() ? shape.userClass[target.source] : GatherWord::Structure) : (target.source < shape.wordClass.size() ? shape.wordClass[target.source] : GatherWord::Structure)});
        if (binding.role == Role::ShaderData) shaderData = true;
        else flat = true;
    }
    const auto at = [](GatherElement element) { return static_cast<std::size_t>(element); };
    for (auto& vsharp : plan.vsharps) {
        const ShaderRecompiler::ShaderBufferResource resource{vsharp.words};
        vsharp.address = resource.Base48();
        vsharp.size = resource.GetSize();
        ++plan.elements[at(GatherElement::Vsharp)];
        plan.bytes[at(GatherElement::Vsharp)] += vsharp.size;
    }
    bool push = false;
    for (const auto& [slot, value] : plan.slots) {
        if ((static_cast<std::size_t>(slot) + 1u) * sizeof(std::uint32_t) <= compiled.pushConstants.size()) push = true;
        if (std::any_of(compiled.bindings.begin(), compiled.bindings.end(), [&](const auto& binding) { return binding.role == Role::ShaderData && slot < binding.guestDescriptor.size(); })) shaderData = true;
    }
    for (const auto& binding : compiled.bindings) {
        if (binding.role == Role::FlattenedSrt && flat) {
            ++plan.elements[at(GatherElement::Flat)];
            plan.bytes[at(GatherElement::Flat)] += binding.guestDescriptor.size() * sizeof(std::uint32_t);
            flat = false;
        }
        if (binding.role == Role::ShaderData && shaderData) {
            ++plan.elements[at(GatherElement::ShaderData)];
            plan.bytes[at(GatherElement::ShaderData)] += binding.guestDescriptor.size() * sizeof(std::uint32_t);
            shaderData = false;
        }
    }
    if (push) {
        ++plan.elements[at(GatherElement::Push)];
        plan.bytes[at(GatherElement::Push)] += compiled.pushConstants.size();
    }
}

void CheckGatherStage(const GatherStagePlan& plan, const ShaderRecompiler::RecompileResult& program, GatherCheck& check) {
    using Role = ShaderRecompiler::DescriptorRole;
    const auto at = [](GatherElement element) { return static_cast<std::size_t>(element); };
    for (const auto& word : plan.words) {
        const bool data = word.binding < program.bindings.size() && program.bindings[word.binding].role == Role::ShaderData;
        const auto kind = at(data ? GatherElement::ShaderData : GatherElement::Flat);
        ++check.checked[kind];
        if (word.binding >= program.bindings.size() || word.word >= program.bindings[word.binding].guestDescriptor.size() || program.bindings[word.binding].guestDescriptor[word.word] != word.value) {
            ++check.mismatched[kind];
            ++check.mismatchedFrom[static_cast<std::size_t>(word.source)];
        }
    }
    for (const auto& vsharp : plan.vsharps) {
        ++check.checked[at(GatherElement::Vsharp)];
        if (vsharp.binding >= program.bindings.size() || static_cast<std::size_t>(vsharp.element) * 4u + 4u > program.bindings[vsharp.binding].guestDescriptor.size()) {
            ++check.mismatched[at(GatherElement::Vsharp)];
            continue;
        }
        ShaderRecompiler::ShaderBufferResource resource;
        std::copy_n(program.bindings[vsharp.binding].guestDescriptor.begin() + static_cast<std::ptrdiff_t>(vsharp.element) * 4, 4, resource.fields.begin());
        const bool address = resource.Base48() != vsharp.address;
        const bool size = resource.GetSize() != vsharp.size;
        check.vsharpAddress += address;
        check.vsharpSize += size;
        if (address || size) ++check.mismatched[at(GatherElement::Vsharp)];
    }
    for (const auto& [slot, value] : plan.slots) {
        const auto offset = static_cast<std::size_t>(slot) * sizeof(std::uint32_t);
        if (offset + sizeof(std::uint32_t) <= program.pushConstants.size()) {
            std::uint32_t pushed = 0;
            std::memcpy(&pushed, program.pushConstants.data() + offset, sizeof(pushed));
            ++check.checked[at(GatherElement::Push)];
            if (pushed != value) ++check.mismatched[at(GatherElement::Push)];
        }
        for (const auto& binding : program.bindings) {
            if (binding.role != Role::ShaderData || slot >= binding.guestDescriptor.size()) continue;
            ++check.checked[at(GatherElement::ShaderData)];
            if (binding.guestDescriptor[slot] != value) ++check.mismatched[at(GatherElement::ShaderData)];
        }
    }
}

void CountGatherPlanUnchecked(const char* reason) {
    ArchB::Recorder::Count("gather_shadow_plan_unchecked");
    if (reason != nullptr) ArchB::Recorder::Count(reason);
}

void CheckGatherDrawPlan(const GatherDrawPlan& plan, std::span<const Graphics::CompiledShader> stages) {
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherElement::Count)> checkedNames{"gather_shadow_commit_checked_vsharp", "gather_shadow_commit_checked_flat", "gather_shadow_commit_checked_shaderdata", "gather_shadow_commit_checked_push", "gather_shadow_commit_checked_indirect"};
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherElement::Count)> mismatchNames{"gather_shadow_commit_mismatch_vsharp", "gather_shadow_commit_mismatch_flat", "gather_shadow_commit_mismatch_shaderdata", "gather_shadow_commit_mismatch_push", "gather_shadow_commit_mismatch_indirect"};
    GatherCheck check;
    for (const auto& stage : plan.stages) {
        if (stage.program == nullptr) continue;
        const auto found = std::find_if(stages.begin(), stages.end(), [&](const Graphics::CompiledShader& shader) { return shader.program == stage.program; });
        if (found == stages.end()) {
            ++check.stagesMissing;
            continue;
        }
        CheckGatherStage(stage, *found->program, check);
    }
    ArchB::Recorder::Count("gather_shadow_commit_plans");
    std::uint64_t mismatches = 0;
    if (Strict()) {
        for (const auto count : check.checked) strictTotals().values.fetch_add(count, std::memory_order_relaxed);
        strictTotals().Report(strictTotals().plans.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    for (std::size_t kind = 0; kind < check.checked.size(); ++kind) {
        if (check.checked[kind] != 0) ArchB::Recorder::Count(checkedNames[kind], static_cast<double>(check.checked[kind]));
        if (check.mismatched[kind] != 0) ArchB::Recorder::Count(mismatchNames[kind], static_cast<double>(check.mismatched[kind]));
        mismatches += check.mismatched[kind];
    }
    if (check.vsharpAddress != 0) ArchB::Recorder::Count("gather_shadow_commit_mismatch_vsharp_address", static_cast<double>(check.vsharpAddress));
    if (check.vsharpSize != 0) ArchB::Recorder::Count("gather_shadow_commit_mismatch_vsharp_size", static_cast<double>(check.vsharpSize));
    if (check.stagesMissing != 0) ArchB::Recorder::Count("gather_shadow_commit_stage_missing", static_cast<double>(check.stagesMissing));
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherWord::Count)> fromNames{"gather_shadow_commit_mismatch_from_structure", "gather_shadow_commit_mismatch_from_hidden", "gather_shadow_commit_mismatch_from_callee", "gather_shadow_commit_mismatch_from_walked_value", "gather_shadow_commit_mismatch_from_table_base", "gather_shadow_commit_mismatch_from_value_word", "gather_shadow_commit_mismatch_from_push_word", "gather_shadow_commit_mismatch_from_flat_data", "gather_shadow_commit_mismatch_from_buffer_base", "gather_shadow_commit_mismatch_from_nested_pointer", "gather_shadow_commit_mismatch_from_feedback"};
    for (std::size_t kind = 0; kind < fromNames.size(); ++kind) {
        if (check.mismatchedFrom[kind] != 0) ArchB::Recorder::Count(fromNames[kind], static_cast<double>(check.mismatchedFrom[kind]));
    }
    if (mismatches != 0) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 16) std::fprintf(stderr, "[gather-shadow] the committer's program differs from the gather plan in %llu values (vsharp address %llu, size %llu)\n", static_cast<unsigned long long>(mismatches), static_cast<unsigned long long>(check.vsharpAddress), static_cast<unsigned long long>(check.vsharpSize));
        if (Strict()) throw std::runtime_error("APS5_DRAW_GATHER_SHADOW_STRICT: the committer's program differs from the gather plan");
    }
}

namespace {

struct ValidationLog {
    bool armed = false;
    std::vector<GatherValidation> entries;
};

ValidationLog& validationLog() {
    thread_local ValidationLog log;
    return log;
}

struct CachedTemplate {
    std::weak_ptr<const DispatchVariant> variant;
    GatherOptions options;
    std::shared_ptr<const GatherTemplate> shape;
};

struct ShadowCounts {
    std::uint64_t pairs = 0, trueMatch = 0, trueDiffer = 0, falseMatch = 0, falseDiffer = 0, nonstructural = 0, falseMatchNestedDirect = 0, falseMatchOther = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(GatherDiffer::Count)> falseDiffers{};
    std::array<std::uint64_t, static_cast<std::size_t>(GatherDiffer::Count)> trueDiffers{};
};

}

std::vector<GatherValidation>* GatherValidationLog() {
    auto& log = validationLog();
    return log.armed ? &log.entries : nullptr;
}

bool GatherRunsPending(const DispatchVariant& variant) {
    PendingView pending;
    pending.Load();
    return std::any_of(variant.runs.begin(), variant.runs.end(), [&](const auto& run) { return run.second > run.first && pending.Overlaps(run.first, static_cast<std::size_t>(run.second - run.first)); });
}

void ArmGatherValidationLog(bool armed) {
    auto& log = validationLog();
    log.armed = armed;
    log.entries.clear();
}

std::shared_ptr<GatherDrawPlan> RunGatherShadow(const GatherShadowDraw& draw) {
    const auto started = std::chrono::steady_clock::now();
    auto& log = validationLog().entries;
    if (!draw.hit && log.empty()) {
        ArchB::Recorder::Count(draw.entry == nullptr ? "gather_shadow_miss_no_entry" : "gather_shadow_miss_unvalidated");
        return nullptr;
    }
    thread_local std::unordered_map<const DispatchVariant*, CachedTemplate> cache;
    if (cache.size() > 16384) cache.clear();
    std::uint64_t built = 0;
    const auto templateFor = [&](const std::shared_ptr<DispatchVariant>& variant, std::uint32_t stage) -> std::shared_ptr<const GatherTemplate> {
        const std::shared_ptr<const DispatchVariant> donor = variant->donor != nullptr ? std::shared_ptr<const DispatchVariant>(variant->donor) : std::shared_ptr<const DispatchVariant>(variant);
        auto& slot = cache[donor.get()];
        if (slot.shape != nullptr && slot.variant.lock() == donor && slot.options == draw.options) return slot.shape;
        std::vector<std::uint32_t> user;
        std::uint32_t hidden = 0;
        if (stage < draw.programs.size()) hidden = draw.programs[stage].hiddenUserWords;
        if (draw.entry != nullptr && draw.entry->decode != nullptr && stage < draw.entry->decode->programs.size()) user = draw.entry->decode->programs[stage].userData;
        else {
            auto refused = std::make_shared<GatherTemplate>();
            refused->refusal = "no_decode";
            return refused;
        }
        if (stage < draw.programs.size() && draw.programs[stage].binary.stage == ShaderRecompiler::ShaderStage::Mesh && user.size() >= ShaderRecompiler::MeshIndexBufferUserWord + donor->meshIndexDescriptor.size()) std::copy(donor->meshIndexDescriptor.begin(), donor->meshIndexDescriptor.end(), user.begin() + ShaderRecompiler::MeshIndexBufferUserWord);
        slot.variant = donor;
        slot.options = draw.options;
        slot.shape = BuildGatherTemplate(*donor, user, hidden, draw.options);
        ++built;
        return slot.shape;
    };
    PendingView pending;
    pending.Load();
    const GatherReader read = [](std::uint64_t address, std::span<std::uint32_t> out) {
        return GuestMemory::CopyMapped(address, std::as_writable_bytes(out)) == GuestMemory::Compare::Equal;
    };
    thread_local std::vector<std::uint32_t> liveWords;
    thread_local std::vector<std::uint64_t> liveAddresses;
    ShadowCounts counts;
    std::shared_ptr<GatherDrawPlan> plan;
    if (draw.hit) {
        plan = std::make_shared<GatherDrawPlan>();
        plan->stages.resize(draw.programs.size());
    }
    bool allMatched = draw.hit, templatable = true, wordsChanged = false, feedbackChanged = false, pendingWords = false, pendingVsharp = false, large = false;
    const char* refusal = nullptr;
    std::uint64_t hitNs = 0;
    std::uint32_t nestedRuns = 0;
    std::uint64_t pendingMasked = 0, pendingMaskedOldDiffering = 0, runsRead = 0;
    std::array<std::uint64_t, 5> vsharpSizes{};
    std::vector<bool> stageSeen(draw.programs.size(), false);
    for (const auto& validation : log) {
        if (validation.variant == nullptr || validation.stage >= draw.programs.size()) continue;
        const bool selected = draw.hit && validation.stage < draw.matched.size() && draw.matched[validation.stage] == validation.variant;
        const auto timed = selected ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto shape = templateFor(validation.variant, validation.stage);
        if (shape->refusal != nullptr) {
            if (selected) {
                templatable = false;
                allMatched = false;
                refusal = shape->refusal;
                stageSeen[validation.stage] = true;
            }
            continue;
        }
        const auto& liveUser = draw.programs[validation.stage].userData;
        auto outcome = EvaluateGather(*shape, liveUser, read, liveWords, &liveAddresses, false);
        bool overPending = false;
        for (std::size_t run = 0; run < liveAddresses.size() && run < shape->runs.size(); ++run) {
            if (liveAddresses[run] != 0 && shape->runs[run].count != 0 && pending.Overlaps(liveAddresses[run], static_cast<std::size_t>(shape->runs[run].count) * sizeof(std::uint32_t))) overPending = true;
        }
        const bool pendingNow = overPending;
        overPending |= validation.pending;
        if (overPending && outcome.matched) {
            ++pendingMasked;
            if (validation.outcome == EntryOutcome::Differing && !validation.refused) ++pendingMaskedOldDiffering;
            outcome.matched = false;
            outcome.differ = GatherDiffer::Pending;
        }
        if (!outcome.matched && overPending && (outcome.differ == GatherDiffer::RunStructure || outcome.differ == GatherDiffer::NestedPointer)) outcome.differ = GatherDiffer::Pending;
        runsRead += shape->runs.size();
        if (selected) {
            stageSeen[validation.stage] = true;
            auto& stagePlan = plan->stages[validation.stage];
            if (outcome.matched) FillGatherPlan(*shape, liveUser, liveWords, stagePlan);
            hitNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - timed).count());
            allMatched &= outcome.matched;
            wordsChanged |= outcome.wordsChanged;
            feedbackChanged |= outcome.feedbackChanged;
            pendingWords |= overPending;
            nestedRuns += outcome.nestedRuns;
            for (const auto& vsharp : stagePlan.vsharps) {
                const auto bucket = vsharp.words[2] == 0xffffffffu ? 4u : vsharp.size <= GatherSmallElementBytes ? 0u : vsharp.size <= 65536u ? 1u : vsharp.size <= 1048576u ? 2u : 3u;
                ++vsharpSizes[bucket];
                if (vsharp.size > GatherSmallElementBytes) large = true;
                if (vsharp.address != 0 && vsharp.size != 0 && pending.Overlaps(vsharp.address, static_cast<std::size_t>(vsharp.size))) pendingVsharp = true;
            }
        }
        if (validation.refused) {
            ++counts.nonstructural;
            continue;
        }
        ++counts.pairs;
        const bool equal = validation.outcome == EntryOutcome::Equal || validation.outcome == EntryOutcome::EqualData;
        if (equal) {
            if (outcome.matched) ++counts.trueMatch;
            else {
                ++counts.falseDiffer;
                ++counts.falseDiffers[static_cast<std::size_t>(outcome.differ)];
            }
        } else if (validation.outcome == EntryOutcome::Differing) {
            if (outcome.matched) {
                ++counts.falseMatch;
                {
                    const auto& kept = *validation.variant;
                    std::size_t position = 0, firstPosition = kept.words.size();
                    std::uint32_t stored = 0, fresh = 0;
                    std::size_t runIndex = 0;
                    for (std::size_t run = 0; run < kept.runs.size() && firstPosition == kept.words.size(); ++run) {
                        const auto count = static_cast<std::size_t>((kept.runs[run].second - kept.runs[run].first) / sizeof(std::uint32_t));
                        std::vector<std::uint32_t> words(count);
                        if (count != 0 && !read(kept.runs[run].first, words)) {
                            firstPosition = position;
                            runIndex = run;
                            break;
                        }
                        for (std::size_t word = 0; word < count; ++word) {
                            const auto at = position + word;
                            auto mask = draw.options.feedback ? WordMask(kept.feedback.memory, at) : 0u;
                            if (draw.options.bufferBases) mask |= WordMask(kept.bufferBaseMasks, at);
                            if (draw.options.dataWords && std::find(kept.dataPositions.begin(), kept.dataPositions.end(), static_cast<std::uint32_t>(at)) != kept.dataPositions.end()) mask = 0xffffffffu;
                            if (((words[word] ^ kept.words[at]) & ~mask) == 0) continue;
                            firstPosition = at;
                            stored = kept.words[at];
                            fresh = words[word];
                            runIndex = run;
                            break;
                        }
                        position += count;
                    }
                    const auto mine = firstPosition < shape->wordClass.size() ? GatherWordName(shape->wordClass[firstPosition]) : "none";
                    const bool nestedDirect = kept.donor == nullptr && firstPosition < shape->wordClass.size() && shape->wordClass[firstPosition] == GatherWord::NestedPointer;
                    ++(nestedDirect ? counts.falseMatchNestedDirect : counts.falseMatchOther);
                    static std::atomic<unsigned> reports{0};
                    if (reports.fetch_add(1, std::memory_order_relaxed) < 16) std::fprintf(stderr, "[gather-shadow] false match: stage %u program 0x%llx relocated %u pending now %u at validation %u; old-mask difference at word %zu of %zu (run %zu of %zu, parent run %u) stored 0x%x live 0x%x, template class %s, template words %zu runs %zu\n", validation.stage, static_cast<unsigned long long>(draw.programs[validation.stage].binary.codeAddress), kept.donor != nullptr ? 1u : 0u, pendingNow ? 1u : 0u, validation.pending ? 1u : 0u, firstPosition, kept.words.size(), runIndex, kept.runs.size(), runIndex < kept.runParents.size() ? kept.runParents[runIndex].run : 0xffffffffu, stored, fresh, mine, shape->words.size(), shape->runs.size());
                }
                if (Strict()) throw std::runtime_error("APS5_DRAW_GATHER_SHADOW_STRICT: the structural signature matched a variant the old path found different");
            } else {
                ++counts.trueDiffer;
                ++counts.trueDiffers[static_cast<std::size_t>(outcome.differ)];
            }
        } else {
            --counts.pairs;
            ++counts.nonstructural;
        }
    }
    if (draw.hit) {
        for (std::size_t stage = 0; stage < draw.programs.size() && stage < draw.matched.size(); ++stage) {
            if (draw.matched[stage] != nullptr && !stageSeen[stage]) {
                allMatched = false;
                refusal = "unlogged";
            }
        }
    }
    bool pendingIndirect = false;
    if (draw.hit && draw.indirectArguments != 0) {
        pendingIndirect = pending.Overlaps(draw.indirectArguments, static_cast<std::size_t>(std::max<std::uint64_t>(draw.indirectBytes, 4))) || (draw.indirectCountAddress != 0 && pending.Overlaps(draw.indirectCountAddress, 4));
    }
    const auto totalNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    using ArchB::Recorder;
    Recorder::Count("gather_shadow_ns", static_cast<double>(totalNs));
    if (built != 0) Recorder::Count("gather_shadow_templates_built", static_cast<double>(built));
    if (pendingMasked != 0) Recorder::Count("gather_shadow_pending_masked_match", static_cast<double>(pendingMasked));
    if (pendingMaskedOldDiffering != 0) Recorder::Count("gather_shadow_pending_masked_match_old_differing", static_cast<double>(pendingMaskedOldDiffering));
    if (runsRead != 0) Recorder::Count("gather_shadow_runs_read", static_cast<double>(runsRead));
    static constexpr std::array<const char*, 5> sizeNames{"gather_shadow_vsharp_size_4k", "gather_shadow_vsharp_size_64k", "gather_shadow_vsharp_size_1m", "gather_shadow_vsharp_size_over_1m", "gather_shadow_vsharp_size_unbounded"};
    for (std::size_t bucket = 0; bucket < sizeNames.size(); ++bucket) {
        if (vsharpSizes[bucket] != 0) Recorder::Count(sizeNames[bucket], static_cast<double>(vsharpSizes[bucket]));
    }
    if (counts.pairs != 0) Recorder::Count("gather_shadow_pairs", static_cast<double>(counts.pairs));
    if (counts.trueMatch != 0) Recorder::Count("gather_shadow_true_match", static_cast<double>(counts.trueMatch));
    if (counts.trueDiffer != 0) Recorder::Count("gather_shadow_true_differ", static_cast<double>(counts.trueDiffer));
    if (counts.falseMatch != 0) Recorder::Count("gather_shadow_false_match", static_cast<double>(counts.falseMatch));
    if (counts.falseMatchNestedDirect != 0) Recorder::Count("gather_shadow_false_match_nested_direct", static_cast<double>(counts.falseMatchNestedDirect));
    if (counts.falseMatchOther != 0) Recorder::Count("gather_shadow_false_match_other", static_cast<double>(counts.falseMatchOther));
    if (counts.falseDiffer != 0) Recorder::Count("gather_shadow_false_differ", static_cast<double>(counts.falseDiffer));
    if (counts.nonstructural != 0) Recorder::Count("gather_shadow_nonstructural", static_cast<double>(counts.nonstructural));
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherDiffer::Count)> falseNames{"gather_shadow_false_differ_none", "gather_shadow_false_differ_layout", "gather_shadow_false_differ_user_structure", "gather_shadow_false_differ_user_hidden", "gather_shadow_false_differ_user_callee", "gather_shadow_false_differ_user_walked", "gather_shadow_false_differ_table_base_alignment", "gather_shadow_false_differ_run_structure", "gather_shadow_false_differ_nested_pointer", "gather_shadow_false_differ_pointer_unreadable", "gather_shadow_false_differ_unreadable", "gather_shadow_false_differ_pending"};
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherDiffer::Count)> trueNames{"gather_shadow_true_differ_none", "gather_shadow_true_differ_layout", "gather_shadow_true_differ_user_structure", "gather_shadow_true_differ_user_hidden", "gather_shadow_true_differ_user_callee", "gather_shadow_true_differ_user_walked", "gather_shadow_true_differ_table_base_alignment", "gather_shadow_true_differ_run_structure", "gather_shadow_true_differ_nested_pointer", "gather_shadow_true_differ_pointer_unreadable", "gather_shadow_true_differ_unreadable", "gather_shadow_true_differ_pending"};
    for (std::size_t kind = 0; kind < falseNames.size(); ++kind) {
        if (counts.falseDiffers[kind] != 0) Recorder::Count(falseNames[kind], static_cast<double>(counts.falseDiffers[kind]));
        if (counts.trueDiffers[kind] != 0) Recorder::Count(trueNames[kind], static_cast<double>(counts.trueDiffers[kind]));
    }
    if (Strict()) {
        strictTotals().pairs.fetch_add(counts.pairs, std::memory_order_relaxed);
        strictTotals().falseDiffers.fetch_add(counts.falseDiffer, std::memory_order_relaxed);
        if (draw.hit && allMatched) strictTotals().matches.fetch_add(1, std::memory_order_relaxed);
        if (draw.hit) strictTotals().Report(strictTotals().hits.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    if (!draw.hit) return nullptr;
    Recorder::Count("gather_shadow_hits");
    Recorder::Count("gather_shadow_hit_ns", static_cast<double>(hitNs));
    if (!templatable) {
        Recorder::Count("gather_shadow_not_templatable");
        static constexpr std::array<std::pair<const char*, const char*>, 5> refusals{{{"bda", "gather_shadow_refused_bda"}, {"no_program", "gather_shadow_refused_no_program"}, {"run_layout", "gather_shadow_refused_run_layout"}, {"nested_layout", "gather_shadow_refused_nested_layout"}, {"no_decode", "gather_shadow_refused_no_decode"}}};
        for (const auto& [reason, name] : refusals) {
            if (refusal != nullptr && std::strcmp(reason, refusal) == 0) Recorder::Count(name);
        }
    } else if (refusal != nullptr) Recorder::Count("gather_shadow_refused_unlogged");
    if (allMatched) Recorder::Count("gather_shadow_matches");
    if (wordsChanged) {
        Recorder::Count("gather_shadow_words_changed");
        if (allMatched) Recorder::Count("gather_shadow_words_changed_matched");
    }
    if (feedbackChanged) Recorder::Count("gather_shadow_feedback_changed");
    if (nestedRuns != 0) {
        Recorder::Count("gather_shadow_draws_nested");
        Recorder::Count("gather_shadow_nested_reads", nestedRuns);
    }
    if (pendingWords) Recorder::Count("gather_shadow_draws_pending_words");
    if (pendingVsharp) Recorder::Count("gather_shadow_draws_pending_vsharp");
    if (pendingIndirect) Recorder::Count("gather_shadow_draws_pending_indirect");
    const bool pendingAny = pendingWords || pendingVsharp || pendingIndirect;
    if (pendingAny) Recorder::Count("gather_shadow_draws_pending");
    if (large) Recorder::Count("gather_shadow_draws_large");
    if (draw.indirectArguments != 0) Recorder::Count("gather_shadow_hits_indirect");
    if (allMatched && templatable && !pendingAny && !large && !feedbackChanged) Recorder::Count("gather_shadow_served");
    if (!allMatched) return nullptr;
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherElement::Count)> elementNames{"gather_shadow_elements_vsharp", "gather_shadow_elements_flat", "gather_shadow_elements_shaderdata", "gather_shadow_elements_push", "gather_shadow_elements_indirect"};
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherElement::Count)> smallNames{"gather_shadow_bytes_vsharp_small", "gather_shadow_bytes_flat_small", "gather_shadow_bytes_shaderdata_small", "gather_shadow_bytes_push_small", "gather_shadow_bytes_indirect_small"};
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherElement::Count)> largeNames{"gather_shadow_bytes_vsharp_large", "gather_shadow_bytes_flat_large", "gather_shadow_bytes_shaderdata_large", "gather_shadow_bytes_push_large", "gather_shadow_bytes_indirect_large"};
    std::array<std::uint64_t, static_cast<std::size_t>(GatherElement::Count)> elements{}, smallBytes{}, largeBytes{};
    for (const auto& stage : plan->stages) {
        for (std::size_t kind = 0; kind < elements.size(); ++kind) elements[kind] += stage.elements[kind];
        for (const auto& vsharp : stage.vsharps) (vsharp.size > GatherSmallElementBytes ? largeBytes : smallBytes)[static_cast<std::size_t>(GatherElement::Vsharp)] += vsharp.size;
        for (std::size_t kind = static_cast<std::size_t>(GatherElement::Flat); kind <= static_cast<std::size_t>(GatherElement::Push); ++kind) (stage.bytes[kind] > GatherSmallElementBytes ? largeBytes : smallBytes)[kind] += stage.bytes[kind];
    }
    if (draw.indirectArguments != 0) {
        const auto kind = static_cast<std::size_t>(GatherElement::Indirect);
        ++elements[kind];
        (draw.indirectBytes > GatherSmallElementBytes ? largeBytes : smallBytes)[kind] += draw.indirectBytes;
    }
    for (std::size_t kind = 0; kind < elements.size(); ++kind) {
        if (elements[kind] != 0) Recorder::Count(elementNames[kind], static_cast<double>(elements[kind]));
        if (smallBytes[kind] != 0) Recorder::Count(smallNames[kind], static_cast<double>(smallBytes[kind]));
        if (largeBytes[kind] != 0) Recorder::Count(largeNames[kind], static_cast<double>(largeBytes[kind]));
    }
    plan->complete = templatable;
    return plan;
}

}
