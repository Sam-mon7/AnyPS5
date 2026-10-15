#ifndef CORE_SHADER_RECOMPILER_OPTIMIZATION_RESOURCEPROGRAM_HPP
#define CORE_SHADER_RECOMPILER_OPTIMIZATION_RESOURCEPROGRAM_HPP

#include "Recompiler.hpp"
#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"
#include <cstdint>
#include <memory>
#include <functional>

namespace ShaderRecompiler {

struct PreparedControlFlow;
class ShaderPreparationContext {
public:
    [[nodiscard]] std::shared_ptr<const PreparedControlFlow> AcquireFrontend(const RecompileRequest& request);

private:
    std::shared_ptr<const PreparedControlFlow> frontend;
};

[[nodiscard]] IrProgram PrepareResourceProgram(const RecompileRequest& request);
[[nodiscard]] IrProgram PrepareResourceProgram(const RecompileRequest& request, ShaderPreparationContext* preparation);
[[nodiscard]] std::uint64_t FrontEndBuilds();
[[nodiscard]] std::shared_ptr<const IrResourcePlan> GetResourcePlan(const RecompileRequest& request);

struct SourceEntry;
struct ResourceCapture {
    std::shared_ptr<const IrResourcePlan> plan;
    ResourceSnapshot snapshot;
    // The cache entry the plan belongs to; null when the request bypasses the cache.
    std::shared_ptr<SourceEntry> source;
    // The walk's read addresses when the plan has pure flat slots (the leaf of each pure slot,
    // and every other read sorted and deduplicated); empty otherwise.
    SrtReadTrace readTrace;
    SrtUserDataUse userDataUse;
    // APS5_PROFILE_DRAW: what CaptureResources spent resolving the source (the stage inputs, the
    // key over the code, the plan) before the walk.
    std::uint64_t sourceNanoseconds = 0;
};
[[nodiscard]] std::shared_ptr<const ResourceCapture> CaptureResources(const RecompileRequest& request, const SrtRuntime& runtime);

// The resolved source of a request (its cache entry with the plan built), for a driver that
// memoizes it per registered shader: ResolveSource is what CaptureResources does before the walk
// (the stage input validation, the key over the code, the lookup, the first-sight plan build), and
// the overload below captures over a handle without repeating it. A handle stays valid for every
// request with the same code and the same cache key fields (RecompileCacheKey::ContextHash plus
// the target); the vertex stages' input validation is repeated per capture because it reads V#
// fields the key does not cover. Null for a request that bypasses the cache (useCache false).
struct CompiledVariant;
struct SourceHandle {
    std::shared_ptr<SourceEntry> source;
    std::shared_ptr<const CompiledVariant> artifact;
    std::vector<std::uint64_t> staticKey;
};
struct PreparedShaderMemory {
    std::uint64_t code = 0;
    std::uint64_t ir = 0;
    std::uint64_t spirv = 0;
    std::uint64_t reflection = 0;
    std::uint64_t layouts = 0;
    std::uint64_t results = 0;
    std::uint64_t cache = 0;
    std::uint64_t pipelines = 0;
    std::uint64_t modules = 0;
    std::uint64_t driverLayouts = 0;
    std::uint64_t pipelineCaches = 0;
    std::size_t sources = 0;
    [[nodiscard]] std::uint64_t Bytes() const { return code + ir + spirv + reflection + layouts + results + cache + pipelines + modules + driverLayouts + pipelineCaches; }
};
struct PreparedShaderCacheStats {
    PreparedShaderMemory memory;
    std::uint64_t peakBytes = 0;
    std::uint64_t evictions = 0;
    std::uint64_t rePrepares = 0;
    std::size_t pinned = 0;
    std::uint64_t overBudgetBytes = 0;
};
[[nodiscard]] bool PreparedShaderBudgetEnabled();
[[nodiscard]] PreparedShaderCacheStats MaintainPreparedShaderCache();
void TouchPreparedShader(const std::shared_ptr<const SourceHandle>& handle);
void NotePreparedShaderReprepare();
enum class PreparedObjectKind { Host, Pipeline, Module, Layout, Cache };
void CachePreparedObject(const std::shared_ptr<const void>& object, PreparedObjectKind kind, std::function<std::uint64_t()> bytes, std::uintptr_t owner = 0);
void AdjustPreparedObjectMemory(PreparedObjectKind kind, std::uint64_t allocated, std::uint64_t freed);
void ClearPreparedObjects(std::uintptr_t owner);
class PreparedShaderInvocation {
public:
    static std::optional<PreparedShaderInvocation> TryCreate(const RecompileRequest& request, const std::shared_ptr<const SourceHandle>& handle);
    static std::optional<PreparedShaderInvocation> TryCreate(const RecompileRequest& request, const std::shared_ptr<const SourceHandle>& handle, std::span<const std::uint64_t> key);
    const RecompileRequest& Request() const { return request; }
    std::shared_ptr<const ResourceCapture> Capture(const SrtRuntime& runtime) const;
    std::shared_ptr<const RecompileResult> Materialize(const ResourceCapture& capture) const;

private:
    PreparedShaderInvocation(const RecompileRequest& request, const std::shared_ptr<const SourceHandle>& handle);
    RecompileRequest request;
    std::shared_ptr<const SourceHandle> handle;
};
[[nodiscard]] std::span<const std::uint32_t> GetPreparedCode(const SourceHandle& handle);
[[nodiscard]] std::shared_ptr<const SourceHandle> PrepareShader(const RecompileRequest& request);
[[nodiscard]] std::shared_ptr<const SourceHandle> PrepareShader(const RecompileRequest& request, ShaderPreparationContext* preparation);
[[nodiscard]] bool MatchesPreparedShader(const RecompileRequest& request, const SourceHandle& handle);
void BuildPreparedShaderKey(const RecompileRequest& request, std::vector<std::uint64_t>& key);
[[nodiscard]] bool MatchesPreparedShader(const RecompileRequest& request, const SourceHandle& handle, std::span<const std::uint64_t> key);
[[nodiscard]] const CompiledShaderArtifact& GetPreparedArtifact(const SourceHandle& handle);
[[nodiscard]] std::shared_ptr<const RecompileResult> RegistrationResult(const RecompileRequest& request, const SourceHandle& handle);
[[nodiscard]] std::shared_ptr<const RecompileResult> MaterializeShader(const RecompileRequest& request, const ResourceCapture& capture, const SourceHandle& handle);
[[nodiscard]] std::shared_ptr<const SourceHandle> ResolveSource(const RecompileRequest& request);
[[nodiscard]] std::shared_ptr<const ResourceCapture> CaptureResources(const RecompileRequest& request, const SrtRuntime& runtime, const SourceHandle& handle);

}

#endif
