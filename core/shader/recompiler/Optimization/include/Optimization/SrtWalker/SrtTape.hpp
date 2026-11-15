#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTTAPE_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTTAPE_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace ShaderRecompiler::Detail {

class SrtTape {
public:
    static const SrtTape* For(const IrResourcePlan& plan);
    enum class StrictFailure : std::uint8_t { None, Unsupported, NonInvariantPhi, PredicateMissing, PredicateUnsupported, PredicateGpuValue, PredicateNonInvariantPhi };
    static std::shared_ptr<const SrtTape> CompileStrict(const IrResourcePlan& plan, StrictFailure* failure = nullptr, bool restoreUndecidable = false);
    static bool Enabled();
    static bool Verified();
    static bool FastEnabled();

    bool Run(const IrResourcePlan& plan, const SrtRuntime& runtime, std::vector<DescriptorValue>& results, std::vector<std::uint32_t>& flat, std::vector<std::uint8_t>& activeSources) const;

    bool RunStrict(const IrResourcePlan& plan, const SrtRuntime& runtime, std::vector<DescriptorValue>& results, std::vector<std::uint32_t>& flat, std::vector<std::uint8_t>& activeSources, std::vector<std::uint32_t>* structure = nullptr) const;

    std::size_t Nodes() const { return nodes.size(); }
    std::uint64_t UserDataMask() const { return userDataMask; }
    const std::vector<std::uint8_t>& AddressFlatSlots() const { return addressFlatSlots; }

private:
    friend class SrtTapeCompiler;
    static std::shared_ptr<const SrtTape> Compile(const IrResourcePlan& plan);
    enum class Op : std::uint8_t {
        Fail, Const, UserData, ShaderBase, Read, ReadConstBuffer, ExtractHalf, AddCarry, ConstructU64,
        ConvertF32U32, ConvertU32F32, FPMul32, FPTrunc32, FPIsNan32, FPOrdLessThanEqual32, FPOrdGreaterThanEqual32, FPUnordLessThanEqual32, FPUnordGreaterThanEqual32,
        IAdd32, IAdd64, ISub32, ISub64, IMul32, IMul64, SMin32, UMin32, BitwiseAnd32, BitwiseAnd64, BitwiseOr32, BitwiseXor32, BitwiseNot32,
        ShiftLeftLogical32, ShiftLeftLogical64, ShiftRightLogical32, ShiftRightLogical64, ShiftRightArithmetic32, ShiftRightArithmetic64,
        BitFieldUExtract, BitFieldSExtract, BitFieldInsert, Select, IEqual32, INotEqual32, SLessThan32, ULessThan32, UGreaterThan32,
        LogicalAnd, LogicalOr, LogicalXor, LogicalNot
    };
    static constexpr std::uint32_t NoNode = 0xffffffffu;
    struct Node {
        Op op = Op::Fail;
        bool clean = false;
        bool loaded = false;
        std::uint8_t component = 0;
        std::uint8_t argumentCount = 0;
        std::uint8_t literalArguments = 0;
        std::int32_t baseWord = -1;
        std::array<std::uint32_t, 5> arguments{};
        std::uint64_t immediate = 0;
        const IrValue* source = nullptr;
        const IrValue* parentRead = nullptr;
    };
    struct Frame;
    bool run(const IrResourcePlan& plan, const SrtRuntime& runtime, std::vector<DescriptorValue>& results, std::vector<std::uint32_t>& flat, std::vector<std::uint8_t>& activeSources, bool strict, std::vector<std::uint32_t>* structure = nullptr) const;
    bool evaluate(Frame& frame, std::uint32_t index, std::uint64_t& result) const;
    bool compute(Frame& frame, const Node& node, std::uint64_t& result) const;

    std::vector<Node> nodes;
    std::vector<std::array<std::uint32_t, 8>> sourceDwords;
    std::vector<std::uint32_t> flatRoots;
    std::vector<std::uint32_t> conditionRoots;
    std::vector<std::uint8_t> addressFlatSlots;
    std::vector<std::uint8_t> initialActive;
    std::vector<std::uint8_t> initialActiveFlat;
    std::size_t activityEntries = 0;
    bool needsCleanMemory = false;
    bool strictProgram = false;
    std::uint64_t userDataMask = 0;
};

}

#endif
