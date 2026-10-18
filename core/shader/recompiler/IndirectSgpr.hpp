#ifndef CORE_SHADER_RECOMPILER_INDIRECTSGPR_HPP
#define CORE_SHADER_RECOMPILER_INDIRECTSGPR_HPP

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace ShaderRecompiler {

inline bool IndirectSgprRuntime() {
    static const bool enabled = [] { const auto* value = std::getenv("APS5_INDIRECT_SGPR_RUNTIME"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    return enabled;
}

struct IndirectSgprPatch {
    std::int32_t baseVertex = -1;
    std::int32_t firstInstance = -1;
    std::int32_t drawIndex = -1;
    std::uint32_t indxOffset = 0;
    bool indexed = false;
    bool operator==(const IndirectSgprPatch&) const = default;
};

enum class IndirectSgprFailure : std::uint8_t { None, Feature, Stage, OriginalArgument, ResourceDependency, Fetch };

class IndirectSgprUnsupported : public std::runtime_error {
public:
    explicit IndirectSgprUnsupported(IndirectSgprFailure reason) : std::runtime_error("runtime indirect SGPRs require CPU expansion"), reason(reason) {}
    IndirectSgprFailure reason;
};

constexpr std::uint64_t IndirectSgprVersion(std::uint64_t legacy) {
    return (legacy ^ 0x53475052554e0001ull) * 0x100000001b3ull;
}

}

#endif
