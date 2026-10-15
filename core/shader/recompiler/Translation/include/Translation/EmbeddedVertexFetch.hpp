#ifndef CORE_SHADER_RECOMPILIER_TRANSLATION_INCLUDE_TRANSLATION_EMBEDDEDVERTEXFETCH_HPP
#define CORE_SHADER_RECOMPILIER_TRANSLATION_INCLUDE_TRANSLATION_EMBEDDEDVERTEXFETCH_HPP

#include "RdnaDecoder/RdnaProgram.hpp"
#include <cstdint>
#include <optional>
#include <vector>

namespace ShaderRecompiler {

struct EmbeddedFetchLoad {
    std::uint32_t programCounter = 0;
    std::int32_t attributeId = -1;
    std::uint32_t componentCount = 0;
    std::vector<std::uint32_t> prologLoads;
};

struct EmbeddedFetchPlan {
    std::vector<EmbeddedFetchLoad> loads;
    std::int32_t vertexOffsetSgpr = -1;
    std::int32_t instanceOffsetSgpr = -1;
    // Whether the offset SGPR is read (or written) anywhere besides the accumulator add, and
    // whether two different SGPRs were added to the accumulator (the SGPR then stays -1).
    bool vertexOffsetShared = false;
    bool instanceOffsetShared = false;
    bool vertexOffsetConflict = false;
    bool instanceOffsetConflict = false;
    bool vertexIndexObserved = false;
    bool instanceIndexObserved = false;
    std::optional<std::uint32_t> instanceOffsetAddPc;
};

class EmbeddedVertexFetchAnalyzer {
public:
    static std::uint64_t UnusedUserSgprs(const RdnaProgram& program);
    static void ExtendInstanceOffset(EmbeddedFetchPlan& plan, const RdnaProgram& program, std::uint32_t userDataBaseRegister, std::uint32_t userDataCount);
    [[nodiscard]] EmbeddedFetchPlan Analyze(const RdnaProgram& program, std::uint32_t attributeTableRegister, std::uint32_t bufferTableRegister, std::uint32_t userDataBaseRegister, std::uint32_t userDataCount, std::uint32_t waveSize) const;
};

}

#endif
