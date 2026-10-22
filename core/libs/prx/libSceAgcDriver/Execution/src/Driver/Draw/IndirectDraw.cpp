#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "ShaderDiskCache.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::DriverDetail {

namespace {

bool PatchIndirectPushConstants(ShaderRecompiler::RecompileResult& result, const ShaderRecompiler::ResourceCapture* capture, const DrawProgram& program, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertexInfo) {
    if (capture == nullptr || capture->plan == nullptr || capture->plan->userDataBase != program.firstUserSgpr || capture->snapshot.userData.size() != program.userData.size()) return false;
    if (!capture->plan->resourceTrackingComplete || !capture->plan->srtPlanComplete || capture->plan->requiresSpecializationMemory) return false;
    if (program.binary.stage != ShaderRecompiler::ShaderStage::Vertex && program.binary.stage != ShaderRecompiler::ShaderStage::Fragment) return false;
    const auto& mapping = result.shaderDataUserWords;
    if (mapping.empty() || mapping.size() != result.memoryOffsetDword || mapping.size() > result.shaderDataDwords || result.shaderDataDwords > result.pushConstants.size() / sizeof(std::uint32_t)) return false;
    if (std::any_of(result.bindings.begin(), result.bindings.end(), [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; })) return false;
    std::uint64_t forwarded = 0;
    for (const auto word : mapping) {
        if (word >= ShaderRecompiler::SrtUserDataUse::TrackedWords || word >= program.userData.size() || (forwarded & (std::uint64_t{1} << word)) != 0) return false;
        forwarded |= std::uint64_t{1} << word;
    }
    if (forwarded != result.userDataRegisterReads) return false;
    const auto consumed = capture->userDataUse.asBase | capture->userDataUse.asValue;
    for (std::size_t word = 0; word < program.userData.size(); ++word) {
        if (program.userData[word] == capture->snapshot.userData[word]) continue;
        if (word >= ShaderRecompiler::SrtUserDataUse::TrackedWords || ((forwarded & ~consumed) & (std::uint64_t{1} << word)) == 0) return false;
        if (word < program.hiddenUserWords) return false;
        if (vertexInfo && vertexInfo->resourcesNum != 0) {
            for (const auto reg : {vertexInfo->fetchAttribReg, vertexInfo->fetchBufferReg}) {
                for (const auto hidden : {0u, program.hiddenUserWords}) {
                    const auto first = static_cast<std::uint64_t>(reg) + hidden;
                    if (word == first || word == first + 1u) return false;
                }
            }
        }
    }
    for (std::size_t slot = 0; slot < mapping.size(); ++slot) {
        std::memcpy(result.pushConstants.data() + slot * sizeof(std::uint32_t), &program.userData[mapping[slot]], sizeof(std::uint32_t));
    }
    return true;
}

void CountIndirectStage(bool patched, bool verified) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    static std::atomic<std::uint64_t> patches{0}, materializations{0}, verifications{0};
    (patched ? patches : materializations).fetch_add(1, std::memory_order_relaxed);
    if (verified) verifications.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<std::chrono::steady_clock::rep> nextReport{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto due = nextReport.load(std::memory_order_relaxed);
    if (now < due || !nextReport.compare_exchange_strong(due, now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(10)).count(), std::memory_order_relaxed)) return;
    ProfilePrint_nid_no_patch("[indirect-push] stages (cumulative): %llu patched, %llu re-materialized, %llu verified\n", static_cast<unsigned long long>(patches.load(std::memory_order_relaxed)), static_cast<unsigned long long>(materializations.load(std::memory_order_relaxed)), static_cast<unsigned long long>(verifications.load(std::memory_order_relaxed)));
}

}

bool Driver::RefreshIndirectStage(ShaderRecompiler::RecompileResult& result, const ShaderRecompiler::ResourceCapture* capture, const DrawProgram& program, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertexInfo, const std::function<std::shared_ptr<const ShaderRecompiler::RecompileResult>()>& materialize) {
    static const bool enabled = std::getenv("APS5_NO_INDIRECT_PUSH_PATCH") == nullptr;
    static const bool verify = std::getenv("APS5_VERIFY_INDIRECT_PUSH_PATCH") != nullptr;
    const bool patched = enabled && PatchIndirectPushConstants(result, capture, program, vertexInfo);
    if (!patched || verify) {
        const auto full = materialize();
        require(full != nullptr, "indirect stage materialization returned no result");
        if (patched) {
            std::vector<std::byte> patchedBytes, fullBytes;
            ShaderRecompiler::ShaderDiskCache::EncodeResult(result, patchedBytes);
            ShaderRecompiler::ShaderDiskCache::EncodeResult(*full, fullBytes);
            if (patchedBytes != fullBytes || result.variantId != full->variantId || result.specializationId != full->specializationId || result.cacheHit != full->cacheHit || result.meshBarycentrics != full->meshBarycentrics) {
                std::fprintf(stderr, "APS5_VERIFY_INDIRECT_PUSH_PATCH: program 0x%llx stage %u differs from re-materialization\n", static_cast<unsigned long long>(program.binary.codeAddress), static_cast<unsigned>(program.binary.stage));
                std::abort();
            }
        } else {
            result = *full;
        }
    }
    CountIndirectStage(patched, patched && verify);
    return patched;
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

std::optional<Graphics::IndirectDrawPath> Driver::ClassifyIndirectDraw(const ShaderRecompiler::RecompileResult& result, const Graphics::State& graphics, const DrawProgram& frontProgram, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertexInfo, const std::shared_ptr<VulkanDevice>& localDevice, Pm4::DrawParameters& drawParameters, bool traceIndirect) {
    std::optional<Graphics::IndirectDrawPath> indirectCpu;
    auto& indirect = *drawParameters.indirect;
    using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
    using Path = Graphics::IndirectDrawPath;
    const auto support = localDevice->DrawIndirectSupport();
    const auto fail = [&](Path reason) { if (!indirectCpu) indirectCpu = reason; };
    if (drawParameters.indexed && !indirect.indexLimitKnown) fail(Path::IndexLimitUnknown);
    else if (drawParameters.indexed && !GuestMemory::Accessible(reinterpret_cast<const void*>(drawParameters.indexAddress), static_cast<std::size_t>(drawParameters.indexCount) * drawParameters.indexSize)) fail(Path::IndexRange);
    constexpr std::uint32_t NoLocation = 0x280u;
    const bool meshPath = graphics.stages.mesh.has_value() && !graphics.stages.tessellation && !graphics.rectList;
    const bool meshGpuSide = meshPath && drawParameters.indexed && indirect.count == 1 && !indirect.countIndirect && indirect.baseVertexLocation == NoLocation && indirect.startInstanceLocation == NoLocation && (!indirect.drawIndexEnabled || indirect.drawIndexLocation == NoLocation);
    static const bool rectListOnGpu = std::getenv("APS5_NO_RECT_INDIRECT") == nullptr;
    const bool rectListGpuSide = rectListOnGpu && graphics.rectList && !graphics.stages.mesh && !graphics.stages.tessellation;
    if ((graphics.stages.path != Graphics::ShaderPath::Vertex || (graphics.rectList && !rectListGpuSide)) && !meshGpuSide) fail(Path::NonVertexPath);
    if (indirect.drawIndexEnabled && indirect.drawIndexSgpr >= 0) fail(Path::DrawIndex);
    if (indirect.countIndirect && !support.count) fail(Path::FeatureGap);
    if (indirect.countIndirect && indirect.count > 1 && !support.multi) fail(Path::FeatureGap);
    const auto reads = [&](std::int32_t sgpr) {
        const auto word = static_cast<std::int64_t>(sgpr) - static_cast<std::int64_t>(frontProgram.firstUserSgpr);
        if (word < 0 || word >= 64) return true;
        if (vertexInfo && vertexInfo->resourcesNum != 0) {
            for (const auto reg : {static_cast<std::int64_t>(vertexInfo->fetchAttribReg), static_cast<std::int64_t>(vertexInfo->fetchBufferReg)}) {
                if (word == reg || word == reg + 1) return true;
            }
        }
        return ((result.userDataRegisterReads >> word) & 1u) != 0;
    };
    const auto dimension = [&](std::int32_t k, std::int32_t p, bool shared, bool conflict, std::uint32_t x, Rule& rule, std::uint32_t& constant) {
        if (p < 0 || (k < 0 && !conflict && !reads(p))) {
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
        std::fprintf(stderr, "[draw] indirect 0x%x args 0x%llx stride %u count %u%s locs base=%s inst=%s idx=%s%s analyzer v=%d i=%d shared=%d/%d conflict=%d/%d indx=%u user reads 0x%llx (first %u, hidden %u) -> %s\n", indirect.opcode, static_cast<unsigned long long>(indirect.arguments), indirect.stride, indirect.count, indirect.countIndirect ? " (indirect)" : "", location(indirect.baseVertexLocation, indirect.baseVertexSgpr).c_str(), location(indirect.startInstanceLocation, indirect.startInstanceSgpr).c_str(), location(indirect.drawIndexLocation, indirect.drawIndexSgpr).c_str(), indirect.drawIndexEnabled ? " (enabled)" : "", result.vertexOffsetSgpr, result.instanceOffsetSgpr, result.vertexOffsetShared ? 1 : 0, result.instanceOffsetShared ? 1 : 0, result.vertexOffsetConflict ? 1 : 0, result.instanceOffsetConflict ? 1 : 0, indirect.indxOffset, static_cast<unsigned long long>(result.userDataRegisterReads), static_cast<unsigned>(frontProgram.firstUserSgpr), static_cast<unsigned>(frontProgram.hiddenUserWords), decision.c_str());
    }
    return indirectCpu;
}

}
