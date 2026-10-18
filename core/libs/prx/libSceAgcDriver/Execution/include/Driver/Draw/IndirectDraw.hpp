#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_INDIRECTDRAW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_INDIRECTDRAW_HPP

#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Recompiler.hpp"
#include "UserDataUse.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <map>
#include <unordered_set>
#include <vector>

namespace AgcDriver::DriverDetail {

enum class IndirectFoldReason : std::size_t {
    VertexMissing, VertexDifferent, VertexShared, InstanceMissing, InstanceDifferent, InstanceShared,
    NonVertex, RectList, DrawIndex, CountUnsupported, MultiCountUnsupported, VertexConflict, InstanceConflict,
    IndxOffset, FirstInstanceUnsupported, ConstantRecordLimit, VertexRange, MultiplePatched, Count
};

struct IndirectFoldDimension {
    std::int32_t k, p;
    bool shared, conflict, patchedUnused, rejected;
    Pm4::DrawParameters::IndirectDraw::Rule rule;
};

struct IndirectFoldDiagnostic {
    IndirectFoldReason first;
    std::uint64_t reasons, shader, unusedMask, srtBase, srtValue;
    std::array<IndirectFoldDimension, 2> dimensions;
    std::array<std::int32_t, 3> patched;
    std::array<std::uint32_t, 3> locations;
    std::uint32_t userBase, opcode, records, recordLimit;
    ShaderRecompiler::ProgramRole role;
    std::uint8_t notFoldedMask, forwardedMask;
    bool foldEnabled, unusedAvailable, countIndirect, srtKnown, storageData;
};

struct IndirectFoldReasonCounter {
    std::uint64_t packets = 0, first = 0;
    std::uint64_t srtBase = 0, srtValue = 0, srtUnknown = 0, forwarded = 0, push = 0, storage = 0, unusedAvailable = 0, patchedUnused = 0, foldOn = 0;
    std::map<std::uint32_t, std::uint64_t> opcodes, records, roles;
    std::unordered_set<std::uint64_t> shaders;
    std::vector<IndirectFoldDiagnostic> samples;
};

struct IndirectFoldCounters {
    std::uint64_t packets = 0;
    std::array<IndirectFoldReasonCounter, static_cast<std::size_t>(IndirectFoldReason::Count)> reasons;
    std::array<std::uint64_t, 64> notFoldedMasks{};
};

bool IndirectFoldReasonsEnabled();
const char* IndirectFoldReasonName(IndirectFoldReason reason);
void SetIndirectFoldSrt(IndirectFoldDiagnostic& diagnostic, std::uint64_t variant, const ShaderRecompiler::SrtUserDataUse* use);
void CountIndirectFold(const IndirectFoldDiagnostic& diagnostic);
IndirectFoldCounters IndirectFoldReasonCounters();
bool ReportIndirectFoldReasons(std::FILE* output = stderr, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

}

#endif
