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
    ReadAddress = 256u
};

std::vector<std::uint16_t> ComputeFlatSlotReasons(const IrResourcePlan& plan);
std::vector<std::uint8_t> ComputePureFlatSlots(const IrResourcePlan& plan);
std::vector<std::uint32_t> ComputeGuardedFlatSlots(const IrResourcePlan& plan);

}

#endif
