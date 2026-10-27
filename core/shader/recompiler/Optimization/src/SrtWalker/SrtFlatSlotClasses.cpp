#include "Optimization/SrtWalker/SrtFlatSlotClasses.hpp"
#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace ShaderRecompiler::Detail {

std::vector<std::uint8_t> ResourceGuardingConditions(const IrResourcePlan& plan) {
    const auto& blocks = plan.controlFlow;
    const std::size_t count = blocks.size();
    std::vector<std::uint8_t> guards(count, 1u);
    for (const auto& block : blocks) {
        for (const auto successor : block.successors) {
            if (successor >= count) return guards;
        }
    }
    // Post-dominator sets as bit rows. Exit blocks start with themselves, the others with every block.
    const std::size_t words = (count + 63u) / 64u;
    std::vector<std::uint64_t> sets(count * words, ~std::uint64_t {0});
    const auto row = [&](std::size_t index) { return sets.data() + index * words; };
    const auto bit = [](std::size_t index) { return std::uint64_t {1} << (index % 64u); };
    for (std::size_t index = 0; index < count; ++index) {
        if (!blocks[index].successors.empty()) continue;
        std::fill_n(row(index), words, std::uint64_t {0});
        row(index)[index / 64u] |= bit(index);
    }
    std::vector<std::uint64_t> next(words);
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t index = count; index-- > 0;) {
            const auto& successors = blocks[index].successors;
            if (successors.empty()) continue;
            std::copy_n(row(successors.front()), words, next.begin());
            for (std::size_t i = 1; i < successors.size(); ++i) {
                const auto* other = row(successors[i]);
                for (std::size_t word = 0; word < words; ++word) next[word] &= other[word];
            }
            next[index / 64u] |= bit(index);
            if (!std::equal(next.begin(), next.end(), row(index))) {
                std::copy(next.begin(), next.end(), row(index));
                changed = true;
            }
        }
    }
    // Blocks that cannot reach an exit keep the full set; a region touching one stays guarding.
    std::vector<std::vector<std::uint32_t>> predecessors(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        for (const auto successor : blocks[index].successors) predecessors[successor].push_back(index);
    }
    std::vector<std::uint8_t> exits(count);
    std::vector<std::uint32_t> pending;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (!blocks[index].successors.empty()) continue;
        exits[index] = 1u;
        pending.push_back(index);
    }
    while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        for (const auto predecessor : predecessors[index]) {
            if (exits[predecessor] != 0u) continue;
            exits[predecessor] = 1u;
            pending.push_back(predecessor);
        }
    }
    const auto members = [&](std::size_t index) {
        std::size_t total = 0;
        const auto* bits = row(index);
        for (std::size_t word = 0; word < words; ++word) total += static_cast<std::size_t>(std::popcount(bits[word]));
        return total;
    };
    std::vector<std::uint8_t> seen(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto& block = blocks[index];
        if (block.condition == nullptr || block.successors.size() != 2u || exits[index] == 0u) continue;
        // The immediate post-dominator is the strict post-dominator with the largest set of its own.
        std::size_t join = count;
        std::size_t joinMembers = 0;
        const auto* bits = row(index);
        for (std::size_t other = 0; other < count; ++other) {
            if (other == index || (bits[other / 64u] & bit(other)) == 0u) continue;
            const auto size = members(other);
            if (join == count || size > joinMembers) {
                join = other;
                joinMembers = size;
            }
        }
        std::fill(seen.begin(), seen.end(), std::uint8_t {0});
        pending.assign(block.successors.begin(), block.successors.end());
        bool guarding = false;
        while (!pending.empty() && !guarding) {
            const auto visit = pending.back();
            pending.pop_back();
            if (visit == join || seen[visit] != 0u) continue;
            seen[visit] = 1u;
            const auto& region = blocks[visit];
            guarding = exits[visit] == 0u || !region.sources.empty() || !region.images.empty() || !region.flatSlots.empty();
            pending.insert(pending.end(), region.successors.begin(), region.successors.end());
        }
        guards[index] = guarding ? 1u : 0u;
    }
    return guards;
}

bool DropItemFreeConditions() {
    static const bool enabled = std::getenv("APS5_NO_SRT_BRANCH_PURE_SLOTS") == nullptr;
    return enabled;
}

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
    const auto guards = ResourceGuardingConditions(plan);
    for (std::size_t index = 0; index < plan.controlFlow.size(); ++index) mark(plan.controlFlow[index].condition, guards[index] != 0u ? BranchCondition : ItemFreeBranch);
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
