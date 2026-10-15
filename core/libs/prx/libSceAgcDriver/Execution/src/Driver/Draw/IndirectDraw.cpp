#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "Translation/EmbeddedVertexFetch.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <cstdlib>
#include <cstring>

namespace AgcDriver::DriverDetail {
namespace {
std::uint64_t UnusedSgprs(std::span<const std::uint32_t> code) {
    if (code.empty() || code.size_bytes() > 1024u * 1024u) return 0;
    struct Entry {
        std::vector<std::uint32_t> code;
        std::uint64_t unused;
    };
    struct CacheStorage {};
    auto& cache = HostThreadLocal<std::vector<Entry>, CacheStorage>();
    const auto found = std::find_if(cache.begin(), cache.end(), [&](const auto& entry) { return std::ranges::equal(entry.code, code); });
    if (found != cache.end()) return found->unused;
    const auto unused = ShaderRecompiler::EmbeddedVertexFetchAnalyzer::UnusedUserSgprs(ShaderRecompiler::RdnaInstructionDecoder{}.Decode(code));
    if (cache.size() == 4) cache.erase(cache.begin());
    cache.push_back({std::vector<std::uint32_t>(code.begin(), code.end()), unused});
    return unused;
}
}

std::uint32_t Driver::drawUserWord(const DrawProgram& program, std::int32_t sgpr) {
    require(sgpr >= 0 && static_cast<std::uint32_t>(sgpr) >= program.firstUserSgpr, "invalid draw offset SGPR");
    const auto index = static_cast<std::uint32_t>(sgpr) - program.firstUserSgpr;
    require(index < program.userData.size(), "draw offset SGPR exceeds user data");
    return program.userData[index];
}

void Driver::FoldDrawOffsets(const ShaderRecompiler::RecompileResult& result, const DrawProgram& program, Pm4::DrawParameters& parameters) {
    if (result.vertexOffsetSgpr >= 0) {
        const auto offset = drawUserWord(program, result.vertexOffsetSgpr);
        require(offset <= std::numeric_limits<std::uint32_t>::max() - parameters.firstVertex, "draw vertex offset overflow");
        parameters.firstVertex += offset;
    }
    if (result.instanceOffsetSgpr >= 0) parameters.firstInstance = drawUserWord(program, result.instanceOffsetSgpr);
}

std::optional<Graphics::IndirectDrawPath> Driver::ClassifyIndirectDraw(const ShaderRecompiler::RecompileResult& result, const Graphics::State& graphics, const DrawProgram& frontProgram, const std::shared_ptr<VulkanDevice>& localDevice, Pm4::DrawParameters& drawParameters, bool traceIndirect) {
    std::optional<Graphics::IndirectDrawPath> indirectCpu;
    auto& indirect = *drawParameters.indirect;
    using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
    using Path = Graphics::IndirectDrawPath;
    const auto support = localDevice->DrawIndirectSupport();
    const auto fail = [&](Path reason) { if (!indirectCpu) indirectCpu = reason; };
    constexpr std::uint32_t NoLocation = 0x280u;
    const bool meshPath = graphics.stages.mesh.has_value() && !graphics.stages.tessellation && !graphics.rectList;
    static const bool indirectFold = [] { const auto* value = std::getenv("APS5_INDIRECT_SGPR_FOLD"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    const auto unused = indirectFold && graphics.stages.path == Graphics::ShaderPath::Vertex && !meshPath && !graphics.stages.tessellation ? UnusedSgprs(frontProgram.binary.code) : 0;
    const bool meshGpuSide = meshPath && drawParameters.indexed && indirect.count == 1 && !indirect.countIndirect && indirect.baseVertexLocation == NoLocation && indirect.startInstanceLocation == NoLocation && (!indirect.drawIndexEnabled || indirect.drawIndexLocation == NoLocation);
    if ((graphics.stages.path != Graphics::ShaderPath::Vertex || graphics.rectList) && !meshGpuSide) fail(Path::NonVertexPath);
    if (indirect.drawIndexEnabled && indirect.drawIndexSgpr >= 0) fail(Path::DrawIndex);
    if (indirect.countIndirect && !support.count) fail(Path::FeatureGap);
    if (indirect.countIndirect && indirect.count > 1 && !support.multi) fail(Path::FeatureGap);
    const auto dimension = [&](std::int32_t k, std::int32_t p, bool shared, bool conflict, std::uint32_t x, Rule& rule, std::uint32_t& constant) {
        if (p < 0 || (p < 64 && (unused & (std::uint64_t{1} << p)) != 0)) {
            rule = Rule::Constant;
            const auto offset = k >= 0 ? drawUserWord(frontProgram, k) : 0u;
            require(offset <= std::numeric_limits<std::uint32_t>::max() - x, "draw vertex offset overflow");
            constant = x + offset;
        } else if (conflict) fail(Path::FetchUnknown);
        else if (k != p || shared) fail(Path::NotFolded);
        else if (x != 0) fail(Path::IndxOffset);
        else rule = Rule::InPlace;
    };
    dimension(result.vertexOffsetSgpr, indirect.baseVertexSgpr, result.vertexOffsetShared, result.vertexOffsetConflict, drawParameters.indexed ? 0u : indirect.indxOffset, indirect.vertexRule, indirect.vertexConstant);
    dimension(result.instanceOffsetSgpr, indirect.startInstanceSgpr, result.instanceOffsetShared, result.instanceOffsetConflict, 0u, indirect.instanceRule, indirect.instanceConstant);
    if (!support.firstInstance && (indirect.instanceRule == Rule::InPlace || indirect.instanceConstant != 0)) fail(Path::FeatureGap);
    if ((indirect.vertexRule == Rule::Constant || indirect.instanceRule == Rule::Constant) && indirect.count > 256) fail(Path::FeatureGap);

    static const std::uint64_t vertexCap = [] { const char* text = std::getenv("APS5_INDIRECT_VERTEX_MIB"); return (text ? std::strtoull(text, nullptr, 10) : 64ull) << 20u; }();
    for (const auto& attribute : result.vertexAttributes) {
        const auto stride = (attribute.resource.fields[1] >> 16u) & 0x3fffu;
        const auto extent = stride == 0 ? static_cast<std::uint64_t>(attribute.resource.fields[2]) : static_cast<std::uint64_t>(attribute.resource.fields[2]) * stride;
        if (extent > vertexCap) fail(Path::VertexRange);
    }
    if (traceIndirect) {
        const auto location = [](std::uint32_t value, std::int32_t sgpr) { char text[24]; if (value == 0x280u) std::snprintf(text, sizeof(text), "none"); else std::snprintf(text, sizeof(text), "0x%x(s%d)", value, sgpr); return std::string(text); };
        const auto rule = [](Rule value, std::uint32_t constant) { char text[24]; if (value == Rule::InPlace) std::snprintf(text, sizeof(text), "in-place"); else std::snprintf(text, sizeof(text), "const %u", constant); return std::string(text); };
        std::string decision = indirectCpu ? "cpu (" + std::string(Graphics::IndirectDrawPathName(*indirectCpu)) + ")" : "gpu vertex=" + rule(indirect.vertexRule, indirect.vertexConstant) + " instance=" + rule(indirect.instanceRule, indirect.instanceConstant);
        std::fprintf(stderr, "[draw] indirect 0x%x args 0x%llx stride %u count %u%s locs base=%s inst=%s idx=%s%s analyzer v=%d i=%d shared=%d/%d conflict=%d/%d indx=%u -> %s\n", indirect.opcode, static_cast<unsigned long long>(indirect.arguments), indirect.stride, indirect.count, indirect.countIndirect ? " (indirect)" : "", location(indirect.baseVertexLocation, indirect.baseVertexSgpr).c_str(), location(indirect.startInstanceLocation, indirect.startInstanceSgpr).c_str(), location(indirect.drawIndexLocation, indirect.drawIndexSgpr).c_str(), indirect.drawIndexEnabled ? " (enabled)" : "", result.vertexOffsetSgpr, result.instanceOffsetSgpr, result.vertexOffsetShared ? 1 : 0, result.instanceOffsetShared ? 1 : 0, result.vertexOffsetConflict ? 1 : 0, result.instanceOffsetConflict ? 1 : 0, indirect.indxOffset, decision.c_str());
    }
    return indirectCpu;
}

}
