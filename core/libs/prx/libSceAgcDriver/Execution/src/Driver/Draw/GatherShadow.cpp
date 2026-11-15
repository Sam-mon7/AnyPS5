#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/GatherShadow.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/WriteEvidence.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
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
        case GatherWord::ValueRecord: return GatherDiffer::RecordCount;
        default: return GatherDiffer::UserStructure;
    }
}

}

const char* GatherWordName(GatherWord word) {
    constexpr std::array<const char*, static_cast<std::size_t>(GatherWord::Count)> names{"structure", "hidden", "callee", "walked_value", "table_base", "value_word", "push_word", "flat_data", "buffer_base", "nested_pointer", "feedback", "value_record"};
    return word < GatherWord::Count ? names[static_cast<std::size_t>(word)] : "unknown";
}

const char* GatherDifferName(GatherDiffer differ) {
    constexpr std::array<const char*, static_cast<std::size_t>(GatherDiffer::Count)> names{"none", "layout", "user_structure", "user_hidden", "user_callee", "user_walked", "table_base_alignment", "run_structure", "nested_pointer", "pointer_unreadable", "unreadable", "pending", "record_count"};
    return differ < GatherDiffer::Count ? names[static_cast<std::size_t>(differ)] : "unknown";
}

const char* GatherElementName(GatherElement element) {
    constexpr std::array<const char*, static_cast<std::size_t>(GatherElement::Count)> names{"vsharp", "flat", "shaderdata", "push", "indirect"};
    return element < GatherElement::Count ? names[static_cast<std::size_t>(element)] : "unknown";
}

GatherExtent GatherBufferExtent(const std::array<std::uint32_t, 4>& words, const GatherBufferLimits& limits) {
    const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1] & ~0x40000000u, words[2], words[3]}};
    const auto address = descriptor.Base48();
    auto bytes = descriptor.GetSize();
    const auto readableEnd = [&](std::uint64_t at) { return limits.readableEnd != nullptr ? limits.readableEnd(at) : Graphics::RegisteredReadableEnd(at); };
    if (limits.maxRange != 0 && bytes > limits.maxRange) {
        const auto mappedEnd = readableEnd(address);
        if (mappedEnd <= address) bytes = 0;
        else if (mappedEnd - address < limits.maxRange) bytes = mappedEnd - address;
    }
    if (bytes == 0 || address == 0) return {};
    if (const std::uint64_t stride = descriptor.Stride(); stride != 0 && (limits.maxRange == 0 || bytes <= limits.maxRange)) {
        constexpr std::uint64_t GuestPageBytes = 0x4000;
        const auto end = address + bytes;
        const auto overhang = end % GuestPageBytes;
        if (overhang != 0 && overhang < stride) {
            const auto allocationEnd = readableEnd(address);
            if (allocationEnd > address && allocationEnd < end && end - allocationEnd < stride) bytes = allocationEnd - address;
        }
    }
    if (const std::uint64_t stride = descriptor.Stride(); stride != 0) {
        const auto target = limits.targetAddress;
        if (target != 0 && address < target && address + bytes > target && address + bytes - target < stride) bytes = target - address;
    }
    return {bytes, static_cast<std::uint32_t>(address % std::max<std::uint32_t>(limits.alignment, 4u))};
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
    const auto valueBaseMask = [&](std::uint32_t index) {
        std::uint32_t mask = 0;
        for (const auto& [user, binding, word] : variant.valueSlots) {
            if (user != index) continue;
            if (word % 4u == 0) mask |= 0xffffffffu;
            else if (word % 4u == 1) mask |= 0xffffu;
        }
        return mask;
    };
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
            mask = valueBaseMask(index);
            kind = mask != 0 ? GatherWord::ValueWord : GatherWord::ValueRecord;
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
        if (kind == GatherWord::TableBase || (kind == GatherWord::ValueWord && shape->userMask[index] == 0xffffffffu) || kind == GatherWord::PushWord) shape->targets.push_back({true, true, index, 0, slot, 0xffffffffu});
    }
    for (const auto& [index, binding, word] : variant.valueSlots) {
        if (index < userData.size() && shape->userClass[index] == GatherWord::ValueWord && word % 4u < 2u) shape->targets.push_back({true, false, index, binding, word, 0xffffffffu});
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
        if ((changed & ~shape.userMask[index]) != 0 && first == GatherDiffer::None) {
            first = UserDiffer(shape.userClass[index]);
            outcome.differIndex = static_cast<std::uint32_t>(index);
            outcome.differOld = shape.user[index];
            outcome.differNew = live;
            outcome.differMask = shape.userMask[index];
        }
        if ((changed & shape.userMask[index]) != 0 && shape.userClass[index] == GatherWord::Feedback) outcome.feedbackChanged = true;
    }
    for (std::size_t word = 0; word < liveWords.size(); ++word) {
        const auto live = liveWords[word];
        hash = (hash ^ (live & ~shape.wordMask[word])) * Prime;
        const auto changed = live ^ shape.words[word];
        if (changed == 0) continue;
        outcome.wordsChanged = true;
        if ((changed & ~shape.wordMask[word]) != 0 && first == GatherDiffer::None) {
            first = shape.wordClass[word] == GatherWord::NestedPointer ? GatherDiffer::NestedPointer : GatherDiffer::RunStructure;
            outcome.differIndex = static_cast<std::uint32_t>(word);
            outcome.differOld = shape.words[word];
            outcome.differNew = live;
            outcome.differMask = shape.wordMask[word];
        }
        if ((changed & shape.wordMask[word]) != 0 && shape.wordClass[word] == GatherWord::Feedback) outcome.feedbackChanged = true;
    }
    outcome.signature = hash;
    outcome.matched = hash == shape.signature && first == GatherDiffer::None;
    if (!outcome.matched || diagnose) outcome.differ = first;
    if (hash == shape.signature && first != GatherDiffer::None) ArchB::Recorder::Count("gather_shadow_signature_collisions");
    return outcome;
}

void FillGatherPlan(const GatherTemplate& shape, std::span<const std::uint32_t> liveUser, std::span<const std::uint32_t> liveWords, GatherStagePlan& plan, const GatherBufferLimits& limits, const GatherCommitStage* bound) {
    plan.words.clear();
    plan.vsharps.clear();
    plan.slots.clear();
    plan.reads.clear();
    plan.knownReads.clear();
    plan.push.clear();
    plan.elements = {};
    plan.bytes = {};
    if (shape.compiled == nullptr) return;
    const auto& compiled = *shape.compiled;
    using Role = ShaderRecompiler::DescriptorRole;
    plan.buffers.resize(std::count_if(compiled.bindings.begin(), compiled.bindings.end(), [](const auto& binding) { return binding.role == Role::FlattenedSrt || binding.role == Role::ShaderData; }));
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
        const auto extent = GatherBufferExtent(vsharp.words, limits);
        vsharp.extent = extent.bytes;
        vsharp.adjustment = extent.adjustment;
        vsharp.viewBytes = Graphics::GuestBufferMemory::ViewBytes(vsharp.extent, vsharp.adjustment);
        if (bound != nullptr) {
            const auto binding = std::find_if(bound->bindings.begin(), bound->bindings.end(), [&](const auto& binding) { return binding.binding == vsharp.binding && binding.element == vsharp.element; });
            if (binding != bound->bindings.end()) {
                vsharp.extent = binding->extent;
                vsharp.adjustment = binding->adjustment;
                vsharp.viewBytes = binding->viewBytes;
            }
        }
        ++plan.elements[at(GatherElement::Vsharp)];
        plan.bytes[at(GatherElement::Vsharp)] += vsharp.extent;
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
    plan.push = compiled.pushConstants;
    for (const auto& [slot, value] : plan.slots) if ((static_cast<std::size_t>(slot) + 1) * sizeof(value) <= plan.push.size()) std::memcpy(plan.push.data() + slot * sizeof(value), &value, sizeof(value));
    std::size_t bufferIndex = 0;
    for (std::uint32_t index = 0; index < compiled.bindings.size(); ++index) {
        const auto& binding = compiled.bindings[index];
        if (binding.role != Role::FlattenedSrt && binding.role != Role::ShaderData) continue;
        auto& buffer = plan.buffers[bufferIndex++];
        buffer.binding = index;
        buffer.words = binding.guestDescriptor;
        for (const auto& word : plan.words) if (word.binding == index && word.word < buffer.words.size()) buffer.words[word.word] = word.value;
        if (binding.role == Role::ShaderData) for (const auto& [slot, value] : plan.slots) if (slot < buffer.words.size()) buffer.words[slot] = value;
    }
}

void CheckGatherStage(const GatherStagePlan& plan, const ShaderRecompiler::RecompileResult& program, GatherCheck& check, const GatherBufferLimits& limits) {
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
        const auto bound = GatherBufferExtent(resource.fields, limits);
        const bool address = resource.Base48() != vsharp.address;
        const bool size = resource.GetSize() != vsharp.size;
        const bool extent = bound.bytes != vsharp.extent || bound.adjustment != vsharp.adjustment;
        check.vsharpAddress += address;
        check.vsharpSize += size;
        check.vsharpExtent += extent;
        if (address || size || extent) ++check.mismatched[at(GatherElement::Vsharp)];
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

GatherCheck CheckGatherDrawPlan(const GatherDrawPlan& plan, std::span<const Graphics::CompiledShader> stages, std::uint64_t targetAddress, const GatherIndirectPlan& committed) {
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherElement::Count)> checkedNames{"gather_shadow_commit_checked_vsharp", "gather_shadow_commit_checked_flat", "gather_shadow_commit_checked_shaderdata", "gather_shadow_commit_checked_push", "gather_shadow_commit_checked_indirect"};
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherElement::Count)> mismatchNames{"gather_shadow_commit_mismatch_vsharp", "gather_shadow_commit_mismatch_flat", "gather_shadow_commit_mismatch_shaderdata", "gather_shadow_commit_mismatch_push", "gather_shadow_commit_mismatch_indirect"};
    GatherCheck check;
    auto limits = plan.limits;
    limits.targetAddress = targetAddress;
    for (const auto& stage : plan.stages) {
        if (stage.program == nullptr) continue;
        const auto found = std::find_if(stages.begin(), stages.end(), [&](const Graphics::CompiledShader& shader) { return shader.program == stage.program; });
        if (found == stages.end()) {
            ++check.stagesMissing;
            continue;
        }
        CheckGatherStage(stage, *found->program, check, limits);
    }
    if (plan.indirect.present || committed.present) {
        const auto kind = static_cast<std::size_t>(GatherElement::Indirect);
        ++check.checked[kind];
        if (!(plan.indirect == committed)) ++check.mismatched[kind];
    }
    ArchB::Recorder::Count("gather_shadow_commit_plans");
    if (plan.complete) ArchB::Recorder::Count("gather_shadow_commit_plans_complete");
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
    if (check.vsharpExtent != 0) ArchB::Recorder::Count("gather_shadow_commit_mismatch_vsharp_extent", static_cast<double>(check.vsharpExtent));
    if (check.stagesMissing != 0) ArchB::Recorder::Count("gather_shadow_commit_stage_missing", static_cast<double>(check.stagesMissing));
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherWord::Count)> fromNames{"gather_shadow_commit_mismatch_from_structure", "gather_shadow_commit_mismatch_from_hidden", "gather_shadow_commit_mismatch_from_callee", "gather_shadow_commit_mismatch_from_walked_value", "gather_shadow_commit_mismatch_from_table_base", "gather_shadow_commit_mismatch_from_value_word", "gather_shadow_commit_mismatch_from_push_word", "gather_shadow_commit_mismatch_from_flat_data", "gather_shadow_commit_mismatch_from_buffer_base", "gather_shadow_commit_mismatch_from_nested_pointer", "gather_shadow_commit_mismatch_from_feedback", "gather_shadow_commit_mismatch_from_value_record"};
    for (std::size_t kind = 0; kind < fromNames.size(); ++kind) {
        if (check.mismatchedFrom[kind] != 0) ArchB::Recorder::Count(fromNames[kind], static_cast<double>(check.mismatchedFrom[kind]));
    }
    if (mismatches != 0 || check.stagesMissing != 0) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 16) std::fprintf(stderr, "[gather-shadow] the committer's draw differs from the gather plan in %llu values (vsharp address %llu, size %llu, extent %llu), %llu stages missing\n", static_cast<unsigned long long>(mismatches), static_cast<unsigned long long>(check.vsharpAddress), static_cast<unsigned long long>(check.vsharpSize), static_cast<unsigned long long>(check.vsharpExtent), static_cast<unsigned long long>(check.stagesMissing));
        if (Strict()) throw std::runtime_error("APS5_DRAW_GATHER_SHADOW_STRICT: the committer's draw differs from the gather plan or lacks one of its stages");
    }
    return check;
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
    std::uint64_t pairs = 0, trueMatch = 0, trueDiffer = 0, falseMatch = 0, falseDiffer = 0, nonstructural = 0, nestedDirect = 0;
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

namespace {

std::shared_ptr<GatherDrawPlan> acquirePlan(std::size_t stages, std::uint64_t& allocations) {
    thread_local std::array<std::shared_ptr<GatherDrawPlan>, 512> ring;
    thread_local std::size_t next = 0;
    auto& slot = ring[next];
    next = (next + 1) % ring.size();
    if (slot == nullptr || slot.use_count() != 1) {
        slot = std::make_shared<GatherDrawPlan>();
        ++allocations;
    }
    auto& plan = *slot;
    plan.stages.resize(stages);
    for (auto& stage : plan.stages) {
        stage.program = nullptr;
        stage.words.clear();
        stage.vsharps.clear();
        stage.slots.clear();
        stage.reads.clear();
        stage.knownReads.clear();
        stage.elements = {};
        stage.bytes = {};
    }
    plan.indirect = {};
    plan.limits = {};
    plan.incomplete = GatherIncompleteNone;
    plan.complete = false;
    plan.matched = false;
    plan.commitTemplate.reset();
    return slot;
}

std::array<std::uint64_t, 4> gatherGenerations() {
    return {Graphics::Recorder::PublishGeneration(), Graphics::StorageTexture::PendingSerial(), GuestMemory::ForgetSerial(), GuestAllocations::GuestAllocationsGeneration_nid_postfix()};
}

}

std::shared_ptr<GatherDrawPlan> RunGatherShadow(const GatherShadowDraw& draw) {
    const auto started = std::chrono::steady_clock::now();
    thread_local std::vector<GatherValidation> selectedLog;
    selectedLog.clear();
    if (draw.live) for (std::uint32_t stage = 0; stage < draw.matched.size(); ++stage) if (draw.matched[stage] != nullptr) selectedLog.push_back({stage, draw.matched[stage], EntryOutcome::Equal});
    const auto& log = draw.live ? selectedLog : validationLog().entries;
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
    const auto sourceGeneration = gatherGenerations();
    PendingView pending;
    pending.Load();
    bool pendingWriteOnly = false;
    const auto busy = [&](std::uint64_t address, std::uint64_t bytes) -> bool {
        pendingWriteOnly = false;
        if (bytes == 0) return false;
        if (draw.busy != nullptr) return (*draw.busy)(address, static_cast<std::size_t>(bytes));
        const auto blockedBy = [&](const char* name) {
            if (draw.live) ArchB::Recorder::Count(name);
            return true;
        };
        if (pending.Overlaps(address, static_cast<std::size_t>(bytes))) {
            const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> span{{{address, address + bytes}}};
            bool other = DrawPipeline::Active() && DrawPipeline::Queue0().Busy() && DrawPipeline::Queue0().Overlaps(address, static_cast<std::size_t>(bytes));
            other = other || Graphics::StorageTexture::AnyPendingOverlaps(span) || Graphics::AnyShadowedOverlaps(address, static_cast<std::size_t>(bytes));
            for (const auto& label : deferredLabels().labels) other = other || (label.address < address + bytes && address < label.address + label.size);
            pendingWriteOnly = !other;
            return blockedBy("draw_templates_live_q0_busy_pending_write");
        }
        if (DrawPipeline::Active()) {
            auto& pipeline = DrawPipeline::Queue0();
            if (pipeline.Busy() && pipeline.Overlaps(address, static_cast<std::size_t>(bytes))) return blockedBy("draw_templates_live_q0_busy_queued_writer");
        }
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> range{{{address, address + bytes}}};
        if (Graphics::StorageTexture::AnyPendingOverlaps(range)) return blockedBy("draw_templates_live_q0_busy_storage_image");
        if (Graphics::AnyShadowedOverlaps(address, static_cast<std::size_t>(bytes))) return blockedBy("draw_templates_live_q0_busy_unit_shadow");
        for (const auto& label : deferredLabels().labels) {
            if (label.address < address + bytes && address < label.address + label.size) return blockedBy("draw_templates_live_q0_busy_deferred_label");
        }
        return false;
    };
    std::uint64_t guardedReads = 0, fallbackReads = 0;
    const GatherReader checkedRead = [&](std::uint64_t address, std::span<std::uint32_t> out) {
        bool owned = false;
        const bool copied = GuestMemory::CopyMappedGuarded(address, std::as_writable_bytes(out), owned) == GuestMemory::Compare::Equal;
        ++(owned ? guardedReads : fallbackReads);
        return copied;
    };
    const GatherReader& baseRead = draw.read != nullptr ? *draw.read : checkedRead;
    bool blocked = false;
    std::vector<std::uint64_t> knownServed;
    std::uint64_t reads = 0;
    const GatherReader read = [&](std::uint64_t address, std::span<std::uint32_t> out) {
        ++reads;
        if (busy(address, out.size_bytes())) {
            static const bool knownWrites = std::getenv("APS5_DRAW_TEMPLATES_LIVE_NO_KNOWN_WRITES") == nullptr;
            if (draw.live && pendingWriteOnly && knownWrites) {
                const auto policy = Driver::QueryPendingSpan(address, out.size_bytes(), std::as_writable_bytes(out));
                if (policy == ShaderMemory::PendingWrite::KnownValue) {
                    ArchB::Recorder::Count("draw_templates_live_q0_known_value_reads");
                    knownServed.push_back(address);
                    return true;
                }
                if (policy == ShaderMemory::PendingWrite::Raw) {
                    ArchB::Recorder::Count("draw_templates_live_q0_raw_evidence_reads");
                    return baseRead(address, out);
                }
                ArchB::Recorder::Count("draw_templates_live_q0_pending_sync");
            }
            blocked = true;
            return false;
        }
        return baseRead(address, out);
    };
    thread_local std::vector<std::uint32_t> liveWords;
    thread_local std::vector<std::uint64_t> liveAddresses;
    thread_local std::vector<std::uint8_t> stageSeen;
    ShadowCounts counts;
    std::uint64_t allocations = 0;
    std::shared_ptr<GatherDrawPlan> plan;
    if (draw.hit) {
        plan = acquirePlan(draw.programs.size(), allocations);
        plan->limits = draw.limits;
        plan->commitTemplate = draw.commitTemplate;
    }
    bool allMatched = draw.hit, templatable = true, wordsChanged = false, feedbackChanged = false, large = false;
    std::uint32_t incomplete = GatherIncompleteNone;
    const char* refusal = nullptr;
    std::uint64_t hitNs = 0, generationRefused = 0, pendingRefused = 0;
    std::uint32_t nestedRuns = 0;
    std::array<std::uint64_t, 5> vsharpSizes{};
    stageSeen.assign(draw.programs.size(), 0);
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
                stageSeen[validation.stage] = 1;
                incomplete |= GatherNotTemplatable;
            }
            continue;
        }
        const auto& liveUser = draw.programs[validation.stage].userData;
        GatherOutcome outcome;
        bool refusedPending = validation.pending, refusedGeneration = false;
        for (std::uint32_t attempt = 0; !refusedPending && attempt < 2; ++attempt) {
            const auto before = gatherGenerations();
            if ((before[2] & 1u) != 0) {
                refusedGeneration = true;
                break;
            }
            pending.Load();
            blocked = false;
            knownServed.clear();
            outcome = EvaluateGather(*shape, liveUser, read, liveWords, &liveAddresses, false);
            if (blocked) {
                refusedPending = true;
                break;
            }
            refusedGeneration = gatherGenerations() != before;
            if (!refusedGeneration) break;
        }
        if (refusedPending || refusedGeneration) {
            outcome.matched = false;
            outcome.differ = GatherDiffer::Pending;
        }
        if (selected) {
            stageSeen[validation.stage] = 1;
            auto& stagePlan = plan->stages[validation.stage];
            if (outcome.matched) {
                const auto* binding = draw.commitTemplate != nullptr && draw.commitTemplate->ready.load(std::memory_order_acquire) && validation.stage < draw.commitTemplate->stages.size() ? &draw.commitTemplate->stages[validation.stage] : nullptr;
                FillGatherPlan(*shape, liveUser, liveWords, stagePlan, draw.limits, binding);
                for (std::size_t run = 0; run < liveAddresses.size(); ++run) {
                    stagePlan.reads.emplace_back(liveAddresses[run], liveAddresses[run] + shape->runs[run].count * sizeof(std::uint32_t));
                    if (std::find(knownServed.begin(), knownServed.end(), liveAddresses[run]) == knownServed.end()) continue;
                    const auto first = liveWords.begin() + static_cast<std::ptrdiff_t>(shape->runs[run].first);
                    stagePlan.knownReads.push_back({liveAddresses[run], std::vector<std::uint32_t>(first, first + static_cast<std::ptrdiff_t>(shape->runs[run].count))});
                }
                if (!stagePlan.vsharps.empty() && binding == nullptr) incomplete |= GatherUnknownBindings;
                if (binding != nullptr) for (const auto& vsharp : stagePlan.vsharps) {
                    const auto known = std::find_if(binding->bindings.begin(), binding->bindings.end(), [&](const auto& item) { return item.binding == vsharp.binding && item.element == vsharp.element; });
                    const auto fresh = GatherBufferExtent(vsharp.words, draw.limits);
                    auto viewBytes = Graphics::GuestBufferMemory::ViewBytes(fresh.bytes, vsharp.adjustment);
                    if (draw.limits.wholeDwords && viewBytes >= 4) viewBytes &= ~std::uint64_t{3};
                    if (known == binding->bindings.end() || fresh.bytes != vsharp.extent || vsharp.viewBytes != viewBytes || vsharp.viewBytes == 0 || vsharp.address < vsharp.adjustment) incomplete |= GatherBindingShape;
                }
            }
            hitNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - timed).count());
            allMatched &= outcome.matched;
            if (!outcome.matched && plan->differ == GatherDiffer::None) {
                plan->differ = outcome.differ;
                plan->differStage = validation.stage;
                plan->differIndex = outcome.differIndex;
                plan->differOld = outcome.differOld;
                plan->differNew = outcome.differNew;
                plan->differMask = outcome.differMask;
            }
            wordsChanged |= outcome.wordsChanged;
            feedbackChanged |= outcome.feedbackChanged;
            if (refusedPending) incomplete |= GatherPendingWords;
            if (refusedGeneration) incomplete |= GatherGenerationMoved;
            if (!outcome.matched && !refusedPending && !refusedGeneration) incomplete |= GatherUnmatched;
            nestedRuns += outcome.nestedRuns;
            for (const auto& vsharp : stagePlan.vsharps) {
                const auto bucket = vsharp.words[2] == 0xffffffffu ? 4u : vsharp.extent <= GatherSmallElementBytes ? 0u : vsharp.extent <= 65536u ? 1u : vsharp.extent <= 1048576u ? 2u : 3u;
                ++vsharpSizes[bucket];
                if (vsharp.extent > GatherSmallElementBytes) large = true;
                if (vsharp.extent != 0 && busy(vsharp.address - vsharp.adjustment, vsharp.viewBytes)) incomplete |= GatherPendingVsharp;
            }
        }
        if (draw.live) {
            if (refusedPending) ++pendingRefused;
            if (refusedGeneration) ++generationRefused;
            continue;
        }
        if (refusedPending) {
            ++pendingRefused;
            continue;
        }
        if (refusedGeneration) {
            ++generationRefused;
            continue;
        }
        if (validation.refused) {
            ++counts.nonstructural;
            continue;
        }
        const bool equal = validation.outcome == EntryOutcome::Equal || validation.outcome == EntryOutcome::EqualData;
        if (equal) {
            ++counts.pairs;
            if (outcome.matched) ++counts.trueMatch;
            else {
                ++counts.falseDiffer;
                ++counts.falseDiffers[static_cast<std::size_t>(outcome.differ)];
                static std::atomic<unsigned> reports{0};
                if (reports.fetch_add(1, std::memory_order_relaxed) < 16) std::fprintf(stderr, "[gather-shadow] false differ: stage %u program 0x%llx class %s\n", validation.stage, static_cast<unsigned long long>(draw.programs[validation.stage].binary.codeAddress), GatherDifferName(outcome.differ));
                if (Strict() && outcome.differ != GatherDiffer::RecordCount) throw std::runtime_error("APS5_DRAW_GATHER_SHADOW_STRICT: the structural signature differs on a variant the old path accepted");
            }
        } else if (validation.outcome == EntryOutcome::Differing) {
            ++counts.pairs;
            if (outcome.matched) {
                const auto& kept = *validation.variant;
                const auto diagnosisGeneration = gatherGenerations();
                pending.Load();
                blocked = false;
                bool readable = true, onlyNested = true, sawNested = false;
                std::size_t position = 0, firstPosition = kept.words.size();
                std::uint32_t stored = 0, fresh = 0;
                std::size_t runIndex = 0;
                std::vector<std::uint32_t> words;
                for (std::size_t run = 0; run < kept.runs.size(); ++run) {
                    const auto count = static_cast<std::size_t>((kept.runs[run].second - kept.runs[run].first) / sizeof(std::uint32_t));
                    if (shape->nested && run < liveAddresses.size() && liveAddresses[run] != kept.runs[run].first) {
                        position += count;
                        continue;
                    }
                    words.assign(count, 0);
                    if (count != 0 && !read(kept.runs[run].first, words)) {
                        readable = false;
                        break;
                    }
                    for (std::size_t word = 0; word < count; ++word) {
                        const auto at = position + word;
                        auto mask = draw.options.feedback ? WordMask(kept.feedback.memory, at) : 0u;
                        if (draw.options.bufferBases) mask |= WordMask(kept.bufferBaseMasks, at);
                        if (draw.options.dataWords && std::find(kept.dataPositions.begin(), kept.dataPositions.end(), static_cast<std::uint32_t>(at)) != kept.dataPositions.end()) mask = 0xffffffffu;
                        if (((words[word] ^ kept.words[at]) & ~mask) == 0) continue;
                        const bool nestedWord = at < shape->wordClass.size() && shape->wordClass[at] == GatherWord::NestedPointer;
                        sawNested |= nestedWord;
                        onlyNested &= nestedWord;
                        if (firstPosition == kept.words.size()) {
                            firstPosition = at;
                            stored = kept.words[at];
                            fresh = words[word];
                            runIndex = run;
                        }
                    }
                    position += count;
                }
                if (!readable || blocked || gatherGenerations() != diagnosisGeneration) {
                    if (!readable || blocked) ++pendingRefused;
                    else ++generationRefused;
                    if (selected) incomplete |= !readable || blocked ? GatherPendingWords : GatherGenerationMoved;
                    continue;
                }
                const auto mine = firstPosition < shape->wordClass.size() ? GatherWordName(shape->wordClass[firstPosition]) : "none";
                const bool nestedDirect = kept.donor == nullptr && sawNested && onlyNested;
                if (nestedDirect) {
                    ++counts.nestedDirect;
                    continue;
                }
                ++counts.falseMatch;
                static std::atomic<unsigned> reports{0};
                if (reports.fetch_add(1, std::memory_order_relaxed) < 16) std::fprintf(stderr, "[gather-shadow] false match: stage %u program 0x%llx relocated %u; old-mask difference at word %zu of %zu (run %zu of %zu, parent run %u) stored 0x%x live 0x%x, template class %s, template words %zu runs %zu\n", validation.stage, static_cast<unsigned long long>(draw.programs[validation.stage].binary.codeAddress), kept.donor != nullptr ? 1u : 0u, firstPosition, kept.words.size(), runIndex, kept.runs.size(), runIndex < kept.runParents.size() ? kept.runParents[runIndex].run : 0xffffffffu, stored, fresh, mine, shape->words.size(), shape->runs.size());
                if (Strict()) throw std::runtime_error("APS5_DRAW_GATHER_SHADOW_STRICT: the structural signature matched a variant the old path found different");
            } else {
                ++counts.trueDiffer;
                ++counts.trueDiffers[static_cast<std::size_t>(outcome.differ)];
            }
        } else {
            ++counts.nonstructural;
        }
    }
    if (draw.hit) {
        for (std::size_t stage = 0; stage < draw.programs.size() && stage < draw.matched.size(); ++stage) {
            if (draw.matched[stage] != nullptr && stageSeen[stage] == 0) {
                allMatched = false;
                refusal = "unlogged";
                incomplete |= GatherUnmatched;
            }
        }
        if (draw.indirect.present) {
            plan->indirect = draw.indirect;
            if (busy(draw.indirect.arguments, std::max<std::uint64_t>(draw.indirect.rangeBytes, 4)) || (draw.indirect.countIndirect && busy(draw.indirect.countAddress, 4))) incomplete |= GatherPendingIndirect;
        }
        if (gatherGenerations() != sourceGeneration || (sourceGeneration[2] & 1u) != 0) {
            incomplete |= GatherGenerationMoved;
            ++generationRefused;
        }
        plan->matched = allMatched && templatable;
        plan->incomplete = incomplete;
        plan->complete = allMatched && templatable && incomplete == GatherIncompleteNone;
    }
    const auto totalNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    if (draw.stats != nullptr) {
        auto& stats = *draw.stats;
        stats.built += built;
        stats.trueMatch += counts.trueMatch;
        stats.trueDiffer += counts.trueDiffer;
        stats.falseMatch += counts.falseMatch;
        stats.nestedDirect += counts.nestedDirect;
        stats.falseDiffer += counts.falseDiffer;
        stats.pendingRefused += pendingRefused;
        stats.generationRefused += generationRefused;
        stats.reads += reads;
    }
    using ArchB::Recorder;
    Recorder::Count("gather_shadow_ns", static_cast<double>(totalNs));
    if (built != 0) Recorder::Count("gather_shadow_templates_built", static_cast<double>(built));
    if (allocations != 0) Recorder::Count("gather_shadow_plan_allocations", static_cast<double>(allocations));
    if (pendingRefused != 0) Recorder::Count("gather_shadow_pending_refused", static_cast<double>(pendingRefused));
    if (generationRefused != 0) Recorder::Count("gather_shadow_generation_refused", static_cast<double>(generationRefused));
    if (reads != 0) Recorder::Count("gather_shadow_runs_read", static_cast<double>(reads));
    if (guardedReads != 0) Recorder::Count("gather_shadow_reads_guarded", static_cast<double>(guardedReads));
    if (fallbackReads != 0) Recorder::Count("gather_shadow_reads_fallback", static_cast<double>(fallbackReads));
    static constexpr std::array<const char*, 5> sizeNames{"gather_shadow_vsharp_extent_4k", "gather_shadow_vsharp_extent_64k", "gather_shadow_vsharp_extent_1m", "gather_shadow_vsharp_extent_over_1m", "gather_shadow_vsharp_extent_unbounded"};
    for (std::size_t bucket = 0; bucket < sizeNames.size(); ++bucket) {
        if (vsharpSizes[bucket] != 0) Recorder::Count(sizeNames[bucket], static_cast<double>(vsharpSizes[bucket]));
    }
    if (counts.pairs != 0) Recorder::Count("gather_shadow_pairs", static_cast<double>(counts.pairs));
    if (counts.trueMatch != 0) Recorder::Count("gather_shadow_true_match", static_cast<double>(counts.trueMatch));
    if (counts.trueDiffer != 0) Recorder::Count("gather_shadow_true_differ", static_cast<double>(counts.trueDiffer));
    if (counts.falseMatch != 0) Recorder::Count("gather_shadow_false_match", static_cast<double>(counts.falseMatch));
    if (counts.nestedDirect != 0) Recorder::Count("gather_shadow_nested_direct_match", static_cast<double>(counts.nestedDirect));
    if (counts.falseDiffer != 0) Recorder::Count("gather_shadow_false_differ", static_cast<double>(counts.falseDiffer));
    if (counts.nonstructural != 0) Recorder::Count("gather_shadow_nonstructural", static_cast<double>(counts.nonstructural));
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherDiffer::Count)> falseNames{"gather_shadow_false_differ_none", "gather_shadow_false_differ_layout", "gather_shadow_false_differ_user_structure", "gather_shadow_false_differ_user_hidden", "gather_shadow_false_differ_user_callee", "gather_shadow_false_differ_user_walked", "gather_shadow_false_differ_table_base_alignment", "gather_shadow_false_differ_run_structure", "gather_shadow_false_differ_nested_pointer", "gather_shadow_false_differ_pointer_unreadable", "gather_shadow_false_differ_unreadable", "gather_shadow_false_differ_pending", "gather_shadow_false_differ_record_count"};
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherDiffer::Count)> trueNames{"gather_shadow_true_differ_none", "gather_shadow_true_differ_layout", "gather_shadow_true_differ_user_structure", "gather_shadow_true_differ_user_hidden", "gather_shadow_true_differ_user_callee", "gather_shadow_true_differ_user_walked", "gather_shadow_true_differ_table_base_alignment", "gather_shadow_true_differ_run_structure", "gather_shadow_true_differ_nested_pointer", "gather_shadow_true_differ_pointer_unreadable", "gather_shadow_true_differ_unreadable", "gather_shadow_true_differ_pending", "gather_shadow_true_differ_record_count"};
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
    if (plan->complete) Recorder::Count("gather_shadow_plans_complete");
    if (wordsChanged) {
        Recorder::Count("gather_shadow_words_changed");
        if (allMatched) Recorder::Count("gather_shadow_words_changed_matched");
        if (plan->complete) Recorder::Count("gather_shadow_words_changed_complete");
    }
    if (feedbackChanged) Recorder::Count("gather_shadow_feedback_changed");
    if (nestedRuns != 0) {
        Recorder::Count("gather_shadow_draws_nested");
        Recorder::Count("gather_shadow_nested_reads", nestedRuns);
    }
    if ((incomplete & GatherPendingWords) != 0) Recorder::Count("gather_shadow_draws_pending_words");
    if ((incomplete & GatherPendingVsharp) != 0) Recorder::Count("gather_shadow_draws_pending_vsharp");
    if ((incomplete & GatherPendingIndirect) != 0) Recorder::Count("gather_shadow_draws_pending_indirect");
    if ((incomplete & GatherGenerationMoved) != 0) Recorder::Count("gather_shadow_draws_generation_moved");
    if ((incomplete & (GatherPendingWords | GatherPendingVsharp | GatherPendingIndirect)) != 0) Recorder::Count("gather_shadow_draws_pending");
    if (large) Recorder::Count("gather_shadow_draws_large");
    if (draw.indirect.present) Recorder::Count("gather_shadow_hits_indirect");
    if (plan->complete && !large && !feedbackChanged) Recorder::Count("gather_shadow_served");
    if (!allMatched) return draw.live ? plan : nullptr;
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherElement::Count)> elementNames{"gather_shadow_elements_vsharp", "gather_shadow_elements_flat", "gather_shadow_elements_shaderdata", "gather_shadow_elements_push", "gather_shadow_elements_indirect"};
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherElement::Count)> smallNames{"gather_shadow_bytes_vsharp_small", "gather_shadow_bytes_flat_small", "gather_shadow_bytes_shaderdata_small", "gather_shadow_bytes_push_small", "gather_shadow_bytes_indirect_small"};
    static constexpr std::array<const char*, static_cast<std::size_t>(GatherElement::Count)> largeNames{"gather_shadow_bytes_vsharp_large", "gather_shadow_bytes_flat_large", "gather_shadow_bytes_shaderdata_large", "gather_shadow_bytes_push_large", "gather_shadow_bytes_indirect_large"};
    std::array<std::uint64_t, static_cast<std::size_t>(GatherElement::Count)> elements{}, smallBytes{}, largeBytes{};
    for (const auto& stage : plan->stages) {
        for (std::size_t kind = 0; kind < elements.size(); ++kind) elements[kind] += stage.elements[kind];
        for (const auto& vsharp : stage.vsharps) (vsharp.extent > GatherSmallElementBytes ? largeBytes : smallBytes)[static_cast<std::size_t>(GatherElement::Vsharp)] += vsharp.extent;
        for (std::size_t kind = static_cast<std::size_t>(GatherElement::Flat); kind <= static_cast<std::size_t>(GatherElement::Push); ++kind) (stage.bytes[kind] > GatherSmallElementBytes ? largeBytes : smallBytes)[kind] += stage.bytes[kind];
    }
    if (draw.indirect.present) {
        const auto kind = static_cast<std::size_t>(GatherElement::Indirect);
        ++elements[kind];
        (draw.indirect.rangeBytes > GatherSmallElementBytes ? largeBytes : smallBytes)[kind] += draw.indirect.rangeBytes;
    }
    for (std::size_t kind = 0; kind < elements.size(); ++kind) {
        if (elements[kind] != 0) Recorder::Count(elementNames[kind], static_cast<double>(elements[kind]));
        if (smallBytes[kind] != 0) Recorder::Count(smallNames[kind], static_cast<double>(smallBytes[kind]));
        if (largeBytes[kind] != 0) Recorder::Count(largeNames[kind], static_cast<double>(largeBytes[kind]));
    }
    return plan;
}

std::vector<std::shared_ptr<const ShaderRecompiler::RecompileResult>> MaterializeGather(const GatherDrawPlan& plan) {
    std::vector<std::shared_ptr<const ShaderRecompiler::RecompileResult>> made;
    for (std::size_t index = 0; index < plan.stages.size(); ++index) {
        const auto& stage = plan.stages[index];
        const auto& donor = plan.commitTemplate->stages[index].compiled;
        auto program = std::make_shared<ShaderRecompiler::RecompileResult>(*donor);
        for (const auto& buffer : stage.buffers) program->bindings.at(buffer.binding).guestDescriptor = buffer.words;
        for (const auto& vsharp : stage.vsharps) std::copy(vsharp.words.begin(), vsharp.words.end(), program->bindings.at(vsharp.binding).guestDescriptor.begin() + static_cast<std::ptrdiff_t>(vsharp.element) * 4);
        program->pushConstants = stage.push;
        made.push_back(std::move(program));
    }
    return made;
}

}
