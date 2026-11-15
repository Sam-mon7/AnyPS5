#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCH_DISPATCHGATHERGRAPH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCH_DISPATCHGATHERGRAPH_HPP

#include "Optimization/ResourceProgram.hpp"
#include "Optimization/SrtWalker/SrtTape.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace AgcDriver::DriverDetail {

enum class DispatchGatherRefusal : std::uint8_t {
    None, MissingPlan, IncompletePlan, NonCompute, Bda, ImageResources, UniformFill,
    GuardedSlots, Poison, Unbounded, UnsupportedGraph, BindingLayout, SourceLayout,
    UserData, ReadFailed, DescriptorNormalization, AbsentBuffer, ImageWrites, ImageTables,
    ImageNormalization, RegisteredViews, PredicateOldPathDisagreement, Count
};

enum class DispatchGatherDifference : std::uint8_t {
    None, DescriptorShape, DescriptorWords, FlatShape, FlatStructure, BindingShape,
    BindingWords, PushBytes, Pipeline, Structure, UserData, Alias, Count
};

struct DispatchGatherBuffer {
    std::uint32_t binding = 0;
    std::uint32_t element = 0;
    std::uint64_t address = 0;
    std::uint64_t descriptorBytes = 0;
    std::uint64_t accessBytes = 0;
};

struct DispatchGatherUse {
    std::vector<ShaderRecompiler::DescriptorValue> descriptors;
    std::vector<ShaderRecompiler::DescriptorValue> sources;
    std::vector<ShaderRecompiler::DescriptorValue> images;
    std::vector<ShaderRecompiler::DescriptorValue> samplers;
    std::vector<ShaderRecompiler::RegisteredImageView> registeredImages;
    std::vector<std::uint32_t> flat;
    std::vector<std::uint8_t> activeSources;
    std::vector<std::uint32_t> structuralTrace;
    std::vector<std::uint32_t> userData;
    std::vector<ShaderRecompiler::DescriptorBinding> bindings;
    std::vector<std::byte> pushConstants;
    std::vector<DispatchGatherBuffer> buffers;
    ShaderRecompiler::PipelineIdentity pipeline;
    bool gathered = false;
    bool imageWriter = false;
};

class DispatchGatherGraph {
public:
    static std::shared_ptr<const DispatchGatherGraph> Create(const ShaderRecompiler::ResourceCapture& capture, const ShaderRecompiler::RecompileResult& compiled, DispatchGatherRefusal& refusal);
    static const char* LastRefusalDetail();
    DispatchGatherRefusal Evaluate(std::span<const std::uint32_t> userData, std::uint64_t shaderBase, ShaderRecompiler::SrtMemoryReader guardedRead, void* context, DispatchGatherUse& use) const;
    bool SetStructure(const DispatchGatherUse& use) const;
    DispatchGatherDifference Matches(const DispatchGatherUse& use) const;
    DispatchGatherDifference CompareMaterialized(const DispatchGatherUse& use, const ShaderRecompiler::RecompileResult& compiled) const;
    DispatchGatherDifference CompareCapture(const DispatchGatherUse& use, const ShaderRecompiler::ResourceCapture& capture) const;
    std::size_t Nodes() const { return tape->Nodes(); }
    bool RestoredPredicates() const { return restoredPredicates; }
    const ShaderRecompiler::IrResourcePlan& Plan() const { return *plan; }
    std::span<const ShaderRecompiler::RegisteredImageView> RegisteredViews() const { return registeredImages; }

private:
    std::shared_ptr<const ShaderRecompiler::IrResourcePlan> plan;
    std::shared_ptr<const ShaderRecompiler::Detail::SrtTape> tape;
    std::vector<ShaderRecompiler::DescriptorValue> structuralDescriptors;
    std::vector<ShaderRecompiler::DescriptorValue> structuralImages;
    std::vector<ShaderRecompiler::DescriptorValue> structuralSamplers;
    std::vector<ShaderRecompiler::RegisteredImageView> registeredImages;
    std::vector<std::uint32_t> imageViews;
    std::vector<std::uint8_t> bufferNormalization;
    std::vector<std::uint32_t> structuralFlat;
    std::vector<std::uint32_t> structuralUserData;
    std::uint64_t dynamicUserDataMask = 0;
    std::vector<ShaderRecompiler::DescriptorBinding> bindings;
    std::vector<DispatchGatherBuffer> structuralBuffers;
    std::vector<std::vector<std::uint32_t>> bindingResources;
    std::vector<std::vector<std::uint8_t>> bindingTransforms;
    std::vector<std::byte> pushConstants;
    std::vector<std::uint32_t> shaderDataUserWords;
    ShaderRecompiler::PipelineIdentity pipeline;
    mutable std::vector<std::uint32_t> structuralTrace;
    bool restoredPredicates = false;
    mutable std::once_flag structureOnce;
    mutable std::atomic<bool> structureReady{false};
};

const char* DispatchGatherRefusalName(DispatchGatherRefusal refusal);
const char* DispatchGatherDifferenceName(DispatchGatherDifference difference);

}

#endif
