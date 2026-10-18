#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "Translation/EmbeddedVertexFetch.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

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

template<bool Diagnose>
std::optional<Graphics::IndirectDrawPath> Driver::classifyIndirectDraw(const ShaderRecompiler::RecompileResult& result, const Graphics::State& graphics, const DrawProgram& frontProgram, const std::shared_ptr<VulkanDevice>& localDevice, Pm4::DrawParameters& drawParameters, bool traceIndirect, IndirectFoldDiagnostic* diagnostic) {
    std::optional<Graphics::IndirectDrawPath> indirectCpu;
    auto& indirect = *drawParameters.indirect;
    using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
    using Path = Graphics::IndirectDrawPath;
    const auto support = localDevice->DrawIndirectSupport();
    using Reason = IndirectFoldReason;
    if constexpr (Diagnose) {
        *diagnostic = {};
        diagnostic->first = Reason::Count;
        diagnostic->shader = frontProgram.binary.codeHash != 0 ? frontProgram.binary.codeHash : result.variantId;
        diagnostic->patched = {indirect.baseVertexSgpr, indirect.startInstanceSgpr, indirect.drawIndexEnabled ? indirect.drawIndexSgpr : -1};
        diagnostic->locations = {indirect.baseVertexLocation, indirect.startInstanceLocation, indirect.drawIndexEnabled ? indirect.drawIndexLocation : 0x280u};
        diagnostic->userBase = frontProgram.firstUserSgpr;
        diagnostic->opcode = indirect.opcode;
        diagnostic->records = indirect.count;
        diagnostic->recordLimit = indirect.count;
        diagnostic->countIndirect = indirect.countIndirect;
        diagnostic->role = ShaderRecompiler::ProgramRole::Main;
        diagnostic->storageData = std::ranges::any_of(result.bindings, [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; });
        for (std::size_t i = 0; i < diagnostic->patched.size(); ++i) {
            const auto p = diagnostic->patched[i];
            if (p >= 0 && static_cast<std::uint32_t>(p) >= frontProgram.firstUserSgpr && std::ranges::find(result.shaderDataUserWords, static_cast<std::uint32_t>(p) - frontProgram.firstUserSgpr) != result.shaderDataUserWords.end()) diagnostic->forwardedMask |= 1u << i;
        }
    }
    const auto fail = [&](Path path, Reason reason) {
        if constexpr (Diagnose) {
            diagnostic->reasons |= std::uint64_t{1} << static_cast<std::size_t>(reason);
            if (!indirectCpu) diagnostic->first = reason;
        }
        if (!indirectCpu) indirectCpu = path;
    };
    constexpr std::uint32_t NoLocation = 0x280u;
    const bool meshPath = graphics.stages.mesh.has_value() && !graphics.stages.tessellation && !graphics.rectList;
    static const bool indirectFold = [] { const auto* value = std::getenv("APS5_INDIRECT_SGPR_FOLD"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    const auto unused = indirectFold && graphics.stages.path == Graphics::ShaderPath::Vertex && !meshPath && !graphics.stages.tessellation ? UnusedSgprs(frontProgram.binary.code) : 0;
    const bool meshGpuSide = meshPath && drawParameters.indexed && indirect.count == 1 && !indirect.countIndirect && indirect.baseVertexLocation == NoLocation && indirect.startInstanceLocation == NoLocation && (!indirect.drawIndexEnabled || indirect.drawIndexLocation == NoLocation);
    if constexpr (Diagnose) {
        diagnostic->foldEnabled = indirectFold;
        diagnostic->unusedMask = unused;
        diagnostic->unusedAvailable = indirectFold && graphics.stages.path == Graphics::ShaderPath::Vertex && !meshPath && !graphics.stages.tessellation && !frontProgram.binary.code.empty() && frontProgram.binary.code.size_bytes() <= 1024u * 1024u;
        std::array<std::uint32_t, 3> unique{};
        std::size_t count = 0;
        for (const auto location : diagnostic->locations) {
            if (location != NoLocation && std::find(unique.begin(), unique.begin() + count, location) == unique.begin() + count) unique[count++] = location;
        }
        if (count > 1) diagnostic->reasons |= std::uint64_t{1} << static_cast<std::size_t>(Reason::MultiplePatched);
    }
    if ((graphics.stages.path != Graphics::ShaderPath::Vertex || graphics.rectList) && !meshGpuSide) {
        fail(Path::NonVertexPath, graphics.stages.path != Graphics::ShaderPath::Vertex ? Reason::NonVertex : Reason::RectList);
        if constexpr (Diagnose) {
            if (graphics.rectList) diagnostic->reasons |= std::uint64_t{1} << static_cast<std::size_t>(Reason::RectList);
        }
    }
    if (indirect.drawIndexEnabled && indirect.drawIndexSgpr >= 0) fail(Path::DrawIndex, Reason::DrawIndex);
    if (indirect.countIndirect && !support.count) fail(Path::FeatureGap, Reason::CountUnsupported);
    if (indirect.countIndirect && indirect.count > 1 && !support.multi) fail(Path::FeatureGap, Reason::MultiCountUnsupported);
    const auto dimension = [&](std::int32_t k, std::int32_t p, bool shared, bool conflict, std::uint32_t x, Rule& rule, std::uint32_t& constant, std::size_t index) {
        if (p < 0 || (p < 64 && (unused & (std::uint64_t{1} << p)) != 0)) {
            rule = Rule::Constant;
            const auto offset = k >= 0 ? drawUserWord(frontProgram, k) : 0u;
            require(offset <= std::numeric_limits<std::uint32_t>::max() - x, "draw vertex offset overflow");
            constant = x + offset;
        } else if (conflict) fail(Path::FetchUnknown, index == 0 ? Reason::VertexConflict : Reason::InstanceConflict);
        else if (k != p || shared) {
            const auto reason = static_cast<Reason>(index * 3 + (k < 0 ? 0 : k != p ? 1 : 2));
            fail(Path::NotFolded, reason);
            if constexpr (Diagnose) {
                if (k != p) diagnostic->notFoldedMask |= 1u << (index * 3 + (k < 0 ? 0 : 1));
                if (shared) {
                    diagnostic->notFoldedMask |= 1u << (index * 3 + 2);
                    diagnostic->reasons |= std::uint64_t{1} << (index * 3 + 2);
                }
            }
        }
        else if (x != 0) fail(Path::IndxOffset, Reason::IndxOffset);
        else rule = Rule::InPlace;
        if constexpr (Diagnose) {
            const bool patchedUnused = p >= 0 && p < 64 && (unused & (std::uint64_t{1} << p)) != 0;
            diagnostic->dimensions[index] = {k, p, shared, conflict, patchedUnused, p >= 0 && !patchedUnused && (conflict || k != p || shared || x != 0), rule};
        }
    };
    dimension(result.vertexOffsetSgpr, indirect.baseVertexSgpr, result.vertexOffsetShared, result.vertexOffsetConflict, drawParameters.indexed ? 0u : indirect.indxOffset, indirect.vertexRule, indirect.vertexConstant, 0);
    dimension(result.instanceOffsetSgpr, indirect.startInstanceSgpr, result.instanceOffsetShared, result.instanceOffsetConflict, 0u, indirect.instanceRule, indirect.instanceConstant, 1);
    if (!support.firstInstance && (indirect.instanceRule == Rule::InPlace || indirect.instanceConstant != 0)) fail(Path::FeatureGap, Reason::FirstInstanceUnsupported);
    if ((indirect.vertexRule == Rule::Constant || indirect.instanceRule == Rule::Constant) && indirect.count > 256) fail(Path::FeatureGap, Reason::ConstantRecordLimit);

    static const std::uint64_t vertexCap = [] { const char* text = std::getenv("APS5_INDIRECT_VERTEX_MIB"); return (text ? std::strtoull(text, nullptr, 10) : 64ull) << 20u; }();
    for (const auto& attribute : result.vertexAttributes) {
        const auto stride = (attribute.resource.fields[1] >> 16u) & 0x3fffu;
        const auto extent = stride == 0 ? static_cast<std::uint64_t>(attribute.resource.fields[2]) : static_cast<std::uint64_t>(attribute.resource.fields[2]) * stride;
        if (extent > vertexCap) fail(Path::VertexRange, Reason::VertexRange);
    }
    if (traceIndirect) {
        const auto location = [](std::uint32_t value, std::int32_t sgpr) { char text[24]; if (value == 0x280u) std::snprintf(text, sizeof(text), "none"); else std::snprintf(text, sizeof(text), "0x%x(s%d)", value, sgpr); return std::string(text); };
        const auto rule = [](Rule value, std::uint32_t constant) { char text[24]; if (value == Rule::InPlace) std::snprintf(text, sizeof(text), "in-place"); else std::snprintf(text, sizeof(text), "const %u", constant); return std::string(text); };
        std::string decision = indirectCpu ? "cpu (" + std::string(Graphics::IndirectDrawPathName(*indirectCpu)) + ")" : "gpu vertex=" + rule(indirect.vertexRule, indirect.vertexConstant) + " instance=" + rule(indirect.instanceRule, indirect.instanceConstant);
        std::fprintf(stderr, "[draw] indirect 0x%x args 0x%llx stride %u count %u%s locs base=%s inst=%s idx=%s%s analyzer v=%d i=%d shared=%d/%d conflict=%d/%d indx=%u -> %s\n", indirect.opcode, static_cast<unsigned long long>(indirect.arguments), indirect.stride, indirect.count, indirect.countIndirect ? " (indirect)" : "", location(indirect.baseVertexLocation, indirect.baseVertexSgpr).c_str(), location(indirect.startInstanceLocation, indirect.startInstanceSgpr).c_str(), location(indirect.drawIndexLocation, indirect.drawIndexSgpr).c_str(), indirect.drawIndexEnabled ? " (enabled)" : "", result.vertexOffsetSgpr, result.instanceOffsetSgpr, result.vertexOffsetShared ? 1 : 0, result.instanceOffsetShared ? 1 : 0, result.vertexOffsetConflict ? 1 : 0, result.instanceOffsetConflict ? 1 : 0, indirect.indxOffset, decision.c_str());
    }
    return indirectCpu;
}

std::optional<Graphics::IndirectDrawPath> Driver::ClassifyIndirectDraw(const ShaderRecompiler::RecompileResult& result, const Graphics::State& graphics, const DrawProgram& frontProgram, const std::shared_ptr<VulkanDevice>& localDevice, Pm4::DrawParameters& drawParameters, bool traceIndirect, IndirectFoldDiagnostic* diagnostic) {
    if (diagnostic != nullptr) return classifyIndirectDraw<true>(result, graphics, frontProgram, localDevice, drawParameters, traceIndirect, diagnostic);
    return classifyIndirectDraw<false>(result, graphics, frontProgram, localDevice, drawParameters, traceIndirect, nullptr);
}

namespace {
struct FoldCounterState {
    std::mutex mutex;
    IndirectFoldCounters counters;
    std::unordered_map<std::uint64_t, std::array<std::uint64_t, 3>> srtUses;
    std::uint64_t srtWindow = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

FoldCounterState& FoldCounters() {
    static FoldCounterState state;
    return state;
}

bool ReportFoldCounters(FoldCounterState& state, std::FILE* output, std::chrono::steady_clock::time_point now) {
    if (now - state.lastReport < std::chrono::seconds(10) || state.counters.packets == 0) return false;
    const auto& counters = state.counters;
    std::fprintf(output, "[indirect-fold] packets=%llu seconds=%.1f mask-bits=vertex.missing,different,shared-or-index-observed,instance.missing,different,shared-or-index-observed masks=", static_cast<unsigned long long>(counters.packets), std::chrono::duration<double>(now - state.lastReport).count());
    for (std::size_t i = 0; i < counters.notFoldedMasks.size(); ++i) {
        if (counters.notFoldedMasks[i] != 0) std::fprintf(output, "%02zx:%llu,", i, static_cast<unsigned long long>(counters.notFoldedMasks[i]));
    }
    std::fputc('\n', output);
    for (std::size_t i = 0; i < counters.reasons.size(); ++i) {
        const auto& bucket = counters.reasons[i];
        if (bucket.packets == 0) continue;
        std::fprintf(output, "[indirect-fold] reason=%s packets=%llu first=%llu shaders=%zu fold-on=%llu unused-available=%llu patched-unused=%llu srt-base=%llu srt-value=%llu srt-unknown=%llu forwarded=%llu push=%llu storage=%llu",
            IndirectFoldReasonName(static_cast<IndirectFoldReason>(i)), static_cast<unsigned long long>(bucket.packets), static_cast<unsigned long long>(bucket.first), bucket.shaders.size(),
            static_cast<unsigned long long>(bucket.foldOn), static_cast<unsigned long long>(bucket.unusedAvailable), static_cast<unsigned long long>(bucket.patchedUnused),
            static_cast<unsigned long long>(bucket.srtBase), static_cast<unsigned long long>(bucket.srtValue), static_cast<unsigned long long>(bucket.srtUnknown), static_cast<unsigned long long>(bucket.forwarded), static_cast<unsigned long long>(bucket.push), static_cast<unsigned long long>(bucket.storage));
        const auto histogram = [&](const char* name, const auto& values) {
            std::fprintf(output, " %s=", name);
            for (const auto& [key, count] : values) std::fprintf(output, "%u:%llu,", key, static_cast<unsigned long long>(count));
        };
        histogram("opcodes", bucket.opcodes);
        histogram("records", bucket.records);
        histogram("front-roles", bucket.roles);
        std::fputc('\n', output);
        for (const auto& sample : bucket.samples) {
            const auto& v = sample.dimensions[0];
            const auto& n = sample.dimensions[1];
            const auto rule = [](const auto& d) { return d.rejected ? "rejected" : d.rule == Pm4::DrawParameters::IndirectDraw::Rule::InPlace ? "in-place" : "constant"; };
            std::fprintf(output, "[indirect-fold] reason=%s hash-or-variant=0x%llx k=%d/%d p=%d/%d patched={%d,%d,%d} locations={0x%x,0x%x,0x%x} user-base=%u shared-or-index-observed=%d/%d conflict=%d/%d unused-available=%d unused-mask=0x%llx patched-unused=%d/%d rules=%s/%s opcode=0x%x records=%u record-limit=%u count-indirect=%d front-role=%u fold=%d srt-known=%d srt-base=0x%llx srt-value=0x%llx forwarded-mask=0x%x data=%s\n",
                IndirectFoldReasonName(static_cast<IndirectFoldReason>(i)), static_cast<unsigned long long>(sample.shader), v.k, n.k, v.p, n.p,
                sample.patched[0], sample.patched[1], sample.patched[2], sample.locations[0], sample.locations[1], sample.locations[2], sample.userBase,
                v.shared, n.shared, v.conflict, n.conflict, sample.unusedAvailable, static_cast<unsigned long long>(sample.unusedMask), v.patchedUnused, n.patchedUnused, rule(v), rule(n),
                sample.opcode, sample.records, sample.recordLimit, sample.countIndirect, static_cast<unsigned>(sample.role), sample.foldEnabled, sample.srtKnown, static_cast<unsigned long long>(sample.srtBase), static_cast<unsigned long long>(sample.srtValue), sample.forwardedMask, sample.storageData ? "storage" : "push");
        }
    }
    state.counters = {};
    std::erase_if(state.srtUses, [&](const auto& entry) { return entry.second[2] != state.srtWindow; });
    ++state.srtWindow;
    state.lastReport = now;
    return true;
}
}

bool IndirectFoldReasonsEnabled() {
    static const bool enabled = [] { const auto* value = std::getenv("APS5_INDIRECT_SGPR_FOLD_REASONS"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    return enabled;
}

const char* IndirectFoldReasonName(IndirectFoldReason reason) {
    static constexpr std::array names{
        "vertex.missing", "vertex.different", "vertex.shared-or-index-observed", "instance.missing", "instance.different", "instance.shared-or-index-observed",
        "non-vertex", "rect-list", "draw-index", "count-unsupported", "multi-count-unsupported", "vertex.conflict", "instance.conflict",
        "indx-offset", "first-instance-unsupported", "constant-record-limit", "vertex-range", "multiple-patched-sgprs"
    };
    return names.at(static_cast<std::size_t>(reason));
}

void SetIndirectFoldSrt(IndirectFoldDiagnostic& diagnostic, std::uint64_t variant, const ShaderRecompiler::SrtUserDataUse* use) {
    if (!IndirectFoldReasonsEnabled()) return;
    auto& state = FoldCounters();
    std::lock_guard lock(state.mutex);
    if (use != nullptr) {
        diagnostic.srtKnown = true;
        diagnostic.srtBase = use->asBase;
        diagnostic.srtValue = use->asValue;
        if (variant != 0) state.srtUses[variant] = {use->asBase, use->asValue, state.srtWindow};
    } else if (variant != 0) {
        if (const auto found = state.srtUses.find(variant); found != state.srtUses.end()) {
            diagnostic.srtKnown = true;
            diagnostic.srtBase = found->second[0];
            diagnostic.srtValue = found->second[1];
            found->second[2] = state.srtWindow;
        }
    }
}

void CountIndirectFold(const IndirectFoldDiagnostic& diagnostic) {
    if (!IndirectFoldReasonsEnabled()) return;
    auto& state = FoldCounters();
    std::lock_guard lock(state.mutex);
    auto& counters = state.counters;
    ++counters.packets;
    ++counters.notFoldedMasks[diagnostic.notFoldedMask];
    std::uint64_t patchedWords = 0;
    for (const auto p : diagnostic.patched) {
        if (p >= 0 && static_cast<std::uint32_t>(p) >= diagnostic.userBase && static_cast<std::uint32_t>(p) - diagnostic.userBase < 64) patchedWords |= std::uint64_t{1} << (static_cast<std::uint32_t>(p) - diagnostic.userBase);
    }
    for (std::size_t i = 0; i < counters.reasons.size(); ++i) {
        if ((diagnostic.reasons & (std::uint64_t{1} << i)) == 0) continue;
        auto& bucket = counters.reasons[i];
        ++bucket.packets;
        bucket.first += i == static_cast<std::size_t>(diagnostic.first);
        bucket.foldOn += diagnostic.foldEnabled;
        bucket.unusedAvailable += diagnostic.unusedAvailable;
        bucket.patchedUnused += diagnostic.dimensions[0].patchedUnused || diagnostic.dimensions[1].patchedUnused;
        bucket.srtBase += diagnostic.srtKnown && (patchedWords & diagnostic.srtBase) != 0;
        bucket.srtValue += diagnostic.srtKnown && (patchedWords & diagnostic.srtValue) != 0;
        bucket.srtUnknown += !diagnostic.srtKnown;
        bucket.forwarded += diagnostic.forwardedMask != 0;
        bucket.push += diagnostic.forwardedMask != 0 && !diagnostic.storageData;
        bucket.storage += diagnostic.forwardedMask != 0 && diagnostic.storageData;
        ++bucket.opcodes[diagnostic.opcode];
        ++bucket.records[diagnostic.records];
        ++bucket.roles[static_cast<std::uint32_t>(diagnostic.role)];
        if (bucket.shaders.insert(diagnostic.shader).second && bucket.samples.size() < 8) bucket.samples.push_back(diagnostic);
    }
    ReportFoldCounters(state, stderr, std::chrono::steady_clock::now());
}

IndirectFoldCounters IndirectFoldReasonCounters() {
    if (!IndirectFoldReasonsEnabled()) return {};
    auto& state = FoldCounters();
    std::lock_guard lock(state.mutex);
    return state.counters;
}

bool ReportIndirectFoldReasons(std::FILE* output, std::chrono::steady_clock::time_point now) {
    if (!IndirectFoldReasonsEnabled()) return false;
    auto& state = FoldCounters();
    std::lock_guard lock(state.mutex);
    return ReportFoldCounters(state, output, now);
}

}
