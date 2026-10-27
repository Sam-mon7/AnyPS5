#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTFLATSLOTCLASSES_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTFLATSLOTCLASSES_HPP

#include "IntermediateRepresentation/IrProgram.hpp"

#include <cstdint>
#include <vector>

namespace ShaderRecompiler::Detail {

enum FlatSlotReason : std::uint16_t {
    IncompletePlan = 1u,
    SpecializationMemory = 2u,
    TableColumnSource = 4u,
    InvalidUniformFill = 8u,
    MaterializedDescriptor = 16u,
    UnusedDescriptor = 32u,
    BranchCondition = 64u,
    UniformFillValue = 128u,
    ReadAddress = 256u,
    // A branch condition whose controlled blocks activate no descriptor source, image or flat slot.
    ItemFreeBranch = 512u
};

// One byte per controlFlow block: 1 when its condition can change which descriptor sources, images or
// flat slots the walk activates, i.e. a block reachable from its successors before they rejoin at its
// immediate post-dominator activates one. Malformed or non-terminating regions count as guarding.
std::vector<std::uint8_t> ResourceGuardingConditions(const IrResourcePlan& plan);
// Unless APS5_NO_SRT_BRANCH_PURE_SLOTS is set, ExtractPlan drops conditions that guard no resources, so
// the walk takes both successors there and the words they read can be pure data, and classifies slots on
// the owned plan values, whose conditions keep only the operands the evaluator reads.
bool DropItemFreeConditions();
std::vector<std::uint16_t> ComputeFlatSlotReasons(const IrResourcePlan& plan);
std::vector<std::uint8_t> ComputePureFlatSlots(const IrResourcePlan& plan);
std::vector<std::uint32_t> ComputeGuardedFlatSlots(const IrResourcePlan& plan);

}

#endif
