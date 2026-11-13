#ifndef CORE_SHADER_RECOMPILER_STENCILREPLAY_HPP
#define CORE_SHADER_RECOMPILER_STENCILREPLAY_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include <string>
#include <vector>

namespace ShaderRecompiler {

[[nodiscard]] std::vector<std::uint32_t> StencilReferenceValues(const IrProgram& program);
[[nodiscard]] std::vector<std::uint32_t> StencilReferenceValues(const IrProgram& program, std::uint32_t shift);
[[nodiscard]] bool LayerReplaySafe(const IrProgram& program);
[[nodiscard]] bool StencilReplaySafe(const IrProgram& program);
[[nodiscard]] bool StencilReplayAccepted(const IrProgram& program);
[[nodiscard]] std::string StencilReplayUnsafeReason(const IrProgram& program, bool acceptUnprovenMasks = false);
[[nodiscard]] bool StencilReferenceDefinedForKeptInvocations(const IrProgram& program);

}

#endif
