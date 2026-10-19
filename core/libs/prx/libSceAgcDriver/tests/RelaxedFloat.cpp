#include "BdaAbi.hpp"
#include "Recompiler.hpp"
#include "RelaxedFloat.hpp"
#include "ShaderDiskCache.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <span>
#include <spirv/unified1/spirv.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

ShaderRecompiler::SpirvTarget Target() {
    static constexpr std::array<std::uint32_t, 5> capabilities{
        spv::CapabilityInt64, spv::CapabilityFloat64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess, spv::CapabilityClipDistance,
    };
    static constexpr std::array<std::string_view, 4> extensions{
        "SPV_EXT_descriptor_indexing", "SPV_KHR_8bit_storage", "SPV_KHR_float_controls", "SPV_KHR_physical_storage_buffer",
    };
    return {0x00401000u, 0x00010300u, 32, ShaderRecompiler::BdaAbi::Version, capabilities, extensions, false,
            {1024, 1024, 64}, 1024, 32768, {}, {}};
}

struct Counts {
    std::size_t selects = 0;
    std::size_t multiplies = 0;
};

Counts Count(std::span<const std::uint32_t> words) {
    Check(words.size() >= 5 && words[0] == spv::MagicNumber, "relaxed float: missing SPIR-V module");
    Counts counts;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> spv::WordCountShift;
        Check(count != 0 && count <= words.size() - offset, "relaxed float: malformed SPIR-V instruction");
        const auto opcode = words[offset] & spv::OpCodeMask;
        if (opcode == spv::OpSelect) ++counts.selects;
        if (opcode == spv::OpFMul) ++counts.multiplies;
        offset += count;
    }
    return counts;
}

Counts Compile(std::uint32_t multiplies) {
    std::vector<std::uint32_t> code;
    for (std::uint32_t load = 0; load < 2u; ++load) code.insert(code.end(), {0xe0300000u | (load * 4u), 0x80010000u | ((1u + load) << 8u)});
    code.push_back(0xbf8c3f70u);
    std::uint32_t result = 1u;
    for (std::uint32_t multiply = 0; multiply < multiplies; ++multiply) {
        code.push_back((0x08u << 25u) | (10u << 17u) | (2u << 9u) | (256u + result));
        result = 10u;
    }
    code.insert(code.end(), {0xe0700000u, 0x80010000u | (result << 8u)});
    code.push_back(0xbf810000u);
    const std::span<const std::uint32_t> words(code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(words)}}};
    const std::array<std::uint32_t, 8> userData{0u, 0u, 0u, 0u, 0x10000000u, 0u, 0x1000u, 0x31016facu};
    ShaderRecompiler::RecompileRequest request{};
    request.shader = {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), words, 0, {}};
    request.target = Target();
    request.layout = {0, 0, 0, 128};
    request.context.waveSize = 32;
    request.context.userDataBaseRegister = 0;
    request.context.userData = userData;
    request.context.compute = ShaderRecompiler::ShaderComputeStageInfo{{32u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    request.context.memory = memory;
    request.useCache = false;
    return Count(ShaderRecompiler::Recompile(request).spirv.Words());
}

}

int main() {
    try {
        const auto* value = std::getenv("APS5_RELAXED_FLOAT32");
        const bool expected = value != nullptr && std::strcmp(value, "1") == 0;
        Check(ShaderRecompiler::RelaxedFloat32() == expected, "relaxed float: APS5_RELAXED_FLOAT32 did not follow its exact value");
        Check((ShaderRecompiler::ShaderDiskCache::OutputVersion() != 0x29e5f633bdc9df6dull) == expected, "relaxed float: the shader output version did not follow the switch");
        const auto none = Compile(0);
        const auto eight = Compile(8);
        Check(eight.multiplies >= none.multiplies + 8u, "relaxed float: eight v_mul_f32 did not emit eight OpFMul");
        const auto added = eight.selects - none.selects;
        if (expected) Check(added == 0u, "relaxed float: eight relaxed v_mul_f32 still added " + std::to_string(added) + " selects for denormal or NaN emulation");
        else Check(added >= 8u, "relaxed float: eight exact v_mul_f32 added only " + std::to_string(added) + " selects, expected the denormal and NaN emulation");
        std::printf("relaxed float tests passed (%s: %zu selects added by eight v_mul_f32)\n", expected ? "on" : "off", added);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
