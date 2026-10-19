#ifndef CORE_SHADER_RECOMPILER_RELAXEDFLOAT_HPP
#define CORE_SHADER_RECOMPILER_RELAXEDFLOAT_HPP

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace ShaderRecompiler {

inline bool RelaxedFloat32() {
    static const bool enabled = [] { const auto* value = std::getenv("APS5_RELAXED_FLOAT32"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    return enabled;
}

constexpr std::uint64_t RelaxedFloatVersion(std::uint64_t version) {
    return (version ^ 0x52454c4158463332ull) * 0x100000001b3ull;
}

}

#endif
