#include "Optimization/SrtWalker/SrtFlatSlotClasses.hpp"
#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace ShaderRecompiler::Detail {

std::vector<std::uint16_t> ComputeFlatSlotReasons(const IrResourcePlan& plan) {
    std::uint16_t flags = 0u;
    if (!plan.srtPlanComplete) flags |= IncompletePlan;
    if (plan.requiresSpecializationMemory) flags |= SpecializationMemory;
    if (std::any_of(plan.descriptorSources.begin(), plan.descriptorSources.end(), [](const DescriptorSource& source) { return source.tableColumn.has_value(); })) flags |= TableColumnSource;
    if (plan.uniformFill.fill.kind != UniformFillKind::None && plan.uniformFill.fill.words > plan.uniformFill.values.size()) flags |= InvalidUniformFill;
    std::vector<std::uint16_t> reasons(plan.srtReads.size(), flags);
    if (reasons.empty() || flags != 0u) return reasons;
    std::unordered_map<const IrValue*, std::uint32_t> readSlots;
    for (std::uint32_t slot = 0; slot < plan.srtReads.size(); ++slot) {
        if (plan.srtReads[slot].value != nullptr) readSlots.emplace(plan.srtReads[slot].value->Resolve(), slot);
    }
    std::unordered_map<const IrValue*, std::uint16_t> visited;
    std::vector<IrValue*> pending;
    const auto mark = [&](IrValue* root, std::uint16_t reason) {
        if (root == nullptr) return;
        pending.push_back(root);
        while (!pending.empty()) {
            IrValue* value = pending.back()->Resolve();
            pending.pop_back();
            if ((visited[value] & reason) != 0u) continue;
            visited[value] |= reason;
            if (value->Opcode() == IrOpcode::ReadConst && value->ArgumentCount() >= 2 && value->Argument(1) != nullptr) {
                const IrValue* slot = value->Argument(1)->Resolve();
                if (slot->HasImmediate() && slot->Type() == IrType::U32 && slot->ImmediateU32() < reasons.size()) {
                    const auto index = slot->ImmediateU32();
                    reasons[index] |= reason;
                    if (plan.srtReads[index].value != nullptr) pending.push_back(plan.srtReads[index].value);
                }
            }
            if (const auto found = readSlots.find(value); found != readSlots.end()) reasons[found->second] |= reason;
            for (std::size_t i = 0; i < value->ArgumentCount(); ++i) {
                if (value->Argument(i) != nullptr) pending.push_back(value->Argument(i));
            }
        }
    };
    for (std::size_t index = 0; index < plan.descriptorSources.size(); ++index) {
        const auto& source = plan.descriptorSources[index];
        const auto reason = std::find(plan.materializationSources.begin(), plan.materializationSources.end(), index) != plan.materializationSources.end() ? MaterializedDescriptor : UnusedDescriptor;
        for (std::uint32_t i = 0; i < source.dwordCount && i < source.dwords.size(); ++i) mark(source.dwords[i], reason);
    }
    for (const auto& block : plan.controlFlow) mark(block.condition, BranchCondition);
    if (plan.uniformFill.fill.kind != UniformFillKind::None) {
        for (std::uint32_t i = 0; i < plan.uniformFill.fill.words; ++i) mark(plan.uniformFill.values[i], UniformFillValue);
    }
    for (const auto& read : plan.srtReads) {
        if (read.value == nullptr) continue;
        IrValue* raw = read.value->Resolve();
        for (std::size_t i = 0; i < raw->ArgumentCount(); ++i) mark(raw->Argument(i), ReadAddress);
    }
    return reasons;
}

std::vector<std::uint8_t> ComputePureFlatSlots(const IrResourcePlan& plan) {
    static const bool materializedOnly = [] {
        const char* value = std::getenv("APS5_SRT_MATERIALIZED_PURE_SLOTS");
        return value != nullptr && std::string_view(value) == "1";
    }();
    const auto reasons = ComputeFlatSlotReasons(plan);
    std::vector<std::uint8_t> pure;
    pure.reserve(reasons.size());
    for (const auto reason : reasons) pure.push_back(reason == 0u || (materializedOnly && reason == UnusedDescriptor) ? 1u : 0u);
    return pure;
}

std::vector<std::uint32_t> ComputeGuardedFlatSlots(const IrResourcePlan& plan) {
    std::unordered_map<const IrValue*, std::vector<const IrValue*>> users;
    std::unordered_set<const IrValue*> seen;
    std::vector<const IrValue*> pending;
    std::vector<const IrValue*> poisonable;
    for (const auto& read : plan.srtReads) {
        if (read.value != nullptr) pending.push_back(read.value->Resolve());
    }
    while (!pending.empty()) {
        const IrValue* value = pending.back();
        pending.pop_back();
        if (!seen.insert(value).second) continue;
        const IrValue* handle = value->ArgumentCount() != 0u && value->Argument(0) != nullptr ? value->Argument(0)->Resolve() : nullptr;
        if (IsRawRead(plan, *value) && handle != nullptr && LoadedFromMemory(*handle)) poisonable.push_back(value);
        const auto follow = [&](const IrValue* argument) {
            if (argument == nullptr) return;
            argument = argument->Resolve();
            users[argument].push_back(value);
            pending.push_back(argument);
        };
        for (std::size_t i = 0; i < value->ArgumentCount(); ++i) follow(value->Argument(i));
        if (value->Opcode() == IrOpcode::ReadConst && value->ArgumentCount() >= 2 && value->Argument(1) != nullptr) {
            const IrValue* slot = value->Argument(1)->Resolve();
            if (slot->HasImmediate() && slot->Type() == IrType::U32 && slot->ImmediateU32() < plan.srtReads.size()) follow(plan.srtReads[slot->ImmediateU32()].value);
        }
    }
    std::unordered_set<const IrValue*> reaching(poisonable.begin(), poisonable.end());
    while (!poisonable.empty()) {
        const IrValue* value = poisonable.back();
        poisonable.pop_back();
        const auto found = users.find(value);
        if (found == users.end()) continue;
        for (const IrValue* user : found->second) {
            if (reaching.insert(user).second) poisonable.push_back(user);
        }
    }
    std::vector<std::uint32_t> guarded;
    for (std::uint32_t slot = 0; slot < plan.srtReads.size(); ++slot) {
        if (plan.srtReads[slot].value != nullptr && reaching.contains(plan.srtReads[slot].value->Resolve())) guarded.push_back(slot);
    }
    return guarded;
}

}
