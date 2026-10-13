#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/DenormalFlushEliminator.hpp"
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <initializer_list>
#include <utility>

using namespace ShaderRecompiler;

namespace {

struct Builder {
    IrProgram program;
    IrBlock* block = nullptr;

    Builder() {
        block = &newBlock();
        program.SetEntryBlock(*block);
    }

    IrBlock& newBlock() {
        auto& created = program.CreateBlock();
        program.BlockOrder().push_back(&created);
        return created;
    }

    IrValue& constant(std::uint32_t value) {
        auto& created = program.CreateValue(IrOpcode::Void, IrType::U32);
        created.SetImmediateU32(value);
        return created;
    }

    IrValue& emit(IrOpcode opcode, IrType type, std::initializer_list<IrValue*> arguments) {
        auto& created = program.CreateValue(opcode, type);
        for (auto* argument : arguments) created.AddArgument(argument);
        block->AppendInstruction(&created);
        return created;
    }

    IrValue& unknown() {
        return emit(IrOpcode::LaneId, IrType::U32, {});
    }

    IrValue& flush(IrValue& bits) {
        auto& exponent = emit(IrOpcode::BitwiseAnd32, IrType::U32, {&bits, &constant(0x7f800000u)});
        auto& denormal = emit(IrOpcode::IEqual32, IrType::Bool, {&exponent, &constant(0u)});
        auto& sign = emit(IrOpcode::BitwiseAnd32, IrType::U32, {&bits, &constant(0x80000000u)});
        return emit(IrOpcode::SelectU32, IrType::U32, {&denormal, &sign, &bits});
    }

    IrValue& phi(IrBlock& block, std::initializer_list<std::pair<IrBlock*, IrValue*>> incoming) {
        auto& created = program.CreateValue(IrOpcode::Phi, IrType::U32);
        block.AppendInstruction(&created);
        for (const auto& [predecessor, value] : incoming) created.AddPhiOperand(predecessor, value);
        return created;
    }

    IrValue& select(IrOpcode opcode, IrType type, IrValue& condition, IrValue& whenTrue, IrValue& whenFalse, SelectFlags flags = {}) {
        auto& created = emit(opcode, type, {&condition, &whenTrue, &whenFalse});
        created.SetFlags(flags);
        return created;
    }

    IrValue& asF32(IrValue& bits) {
        return emit(IrOpcode::BitCastF32U32, IrType::F32, {&bits});
    }

    IrValue& asU32(IrValue& value) {
        return emit(IrOpcode::BitCastU32F32, IrType::U32, {&value});
    }

    void keep(IrValue& value) {
        (void)emit(IrOpcode::ReferenceU32, IrType::Void, {&value});
    }

    std::uint32_t eliminate() {
        return DenormalFlushEliminator{}.Eliminate(program).removedFlushes;
    }
};

bool expect(const char* name, bool condition) {
    if (!condition) std::fprintf(stderr, "%s\n", name);
    return condition;
}

bool run(const char* name, const std::function<bool()>& test) {
    try {
        return expect(name, test());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: %s\n", name, error.what());
        return false;
    }
}

}

int main() {
    bool passed = true;

    passed &= run("a flush of a flushed value is removed and the first flush stays", [] {
        Builder b;
        auto& first = b.flush(b.unknown());
        auto& second = b.flush(first);
        b.keep(second);
        return b.eliminate() == 1u && !second.HasUses() && first.HasUses();
    });

    passed &= run("a flush of an unknown value stays", [] {
        Builder b;
        auto& flushed = b.flush(b.unknown());
        b.keep(flushed);
        return b.eliminate() == 0u && flushed.HasUses();
    });

    passed &= run("a sign change or absolute value of a flushed value needs no flush", [] {
        Builder b;
        auto& first = b.flush(b.unknown());
        auto& negated = b.flush(b.emit(IrOpcode::BitwiseXor32, IrType::U32, {&first, &b.constant(0x80000000u)}));
        auto& absolute = b.flush(b.emit(IrOpcode::BitwiseAnd32, IrType::U32, {&b.constant(0x7fffffffu), &first}));
        b.keep(negated);
        b.keep(absolute);
        return b.eliminate() == 2u && !negated.HasUses() && !absolute.HasUses();
    });

    passed &= run("a select of flushed values needs no flush, one with an unknown arm does", [] {
        Builder b;
        auto& condition = b.emit(IrOpcode::ULessThan32, IrType::Bool, {&b.unknown(), &b.constant(16u)});
        auto& both = b.flush(b.emit(IrOpcode::SelectU32, IrType::U32, {&condition, &b.flush(b.unknown()), &b.flush(b.unknown())}));
        auto& mixed = b.flush(b.emit(IrOpcode::SelectU32, IrType::U32, {&condition, &b.flush(b.unknown()), &b.unknown()}));
        b.keep(both);
        b.keep(mixed);
        return b.eliminate() == 1u && !both.HasUses() && mixed.HasUses();
    });

    passed &= run("normal and zero constants need no flush, a denormal constant does", [] {
        Builder b;
        auto& one = b.flush(b.constant(0x3f800000u));
        auto& negativeZero = b.flush(b.constant(0x80000000u));
        auto& denormal = b.flush(b.constant(0x00000001u));
        b.keep(one);
        b.keep(negativeZero);
        b.keep(denormal);
        return b.eliminate() == 2u && !one.HasUses() && !negativeZero.HasUses() && denormal.HasUses();
    });

    passed &= run("an integer or f16 converted to f32 needs no flush", [] {
        Builder b;
        auto& integer = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::ConvertF32U32, IrType::F32, {&b.unknown()})}));
        auto& half = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::ConvertF32F16, IrType::F32, {&b.emit(IrOpcode::BitCastF16U16, IrType::F16, {&b.emit(IrOpcode::ConvertU16U32, IrType::U16, {&b.unknown()})})})}));
        b.keep(integer);
        b.keep(half);
        return b.eliminate() == 2u && !integer.HasUses() && !half.HasUses();
    });

    passed &= run("a clamped flushed value needs no flush, an arithmetic result does", [] {
        Builder b;
        auto& first = b.emit(IrOpcode::BitCastF32U32, IrType::F32, {&b.flush(b.unknown())});
        auto& clamped = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::FPSaturate32, IrType::F32, {&first})}));
        auto& product = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::FPMul32, IrType::F32, {&first, &first})}));
        b.keep(clamped);
        b.keep(product);
        return b.eliminate() == 1u && !clamped.HasUses() && product.HasUses();
    });

    passed &= run("a phi of flushed values needs no flush", [] {
        Builder b;
        IrBlock& head = *b.block;
        IrBlock& body = b.newBlock();
        IrBlock& merge = b.newBlock();
        head.AddBranch(&body);
        head.AddBranch(&merge);
        body.AddBranch(&merge);
        auto& fromHead = b.flush(b.unknown());
        b.block = &body;
        auto& fromBody = b.flush(b.unknown());
        b.block = &merge;
        auto& merged = b.flush(b.phi(merge, {{&head, &fromHead}, {&body, &fromBody}}));
        b.keep(merged);
        return b.eliminate() == 1u && !merged.HasUses();
    });

    passed &= run("a loop phi needs no flush when every value it takes is flushed, and keeps it otherwise", [] {
        const auto loop = [](bool flushedBackEdge) {
            Builder b;
            IrBlock& head = *b.block;
            IrBlock& body = b.newBlock();
            IrBlock& exit = b.newBlock();
            head.AddBranch(&body);
            body.AddBranch(&body);
            body.AddBranch(&exit);
            auto& initial = b.flush(b.unknown());
            b.block = &body;
            auto& carried = b.program.CreateValue(IrOpcode::Phi, IrType::U32);
            body.AppendInstruction(&carried);
            auto& next = b.flush(carried);
            auto& backEdge = flushedBackEdge ? next : b.emit(IrOpcode::BitwiseXor32, IrType::U32, {&b.unknown(), &next});
            carried.AddPhiOperand(&head, &initial);
            carried.AddPhiOperand(&body, &backEdge);
            b.block = &exit;
            b.keep(next);
            b.eliminate();
            return !next.HasUses();
        };
        return loop(true) && !loop(false);
    });

    passed &= run("a product flushed when tiny needs no flush", [] {
        Builder b;
        auto& lhs = b.asF32(b.flush(b.unknown()));
        auto& product = b.emit(IrOpcode::FPMul32, IrType::F32, {&lhs, &lhs});
        auto& tiny = b.emit(IrOpcode::ULessThan32, IrType::Bool, {&b.unknown(), &b.constant(0x207fffffu)});
        auto& flushed = b.flush(b.asU32(b.select(IrOpcode::SelectF32, IrType::F32, tiny, b.asF32(b.constant(0u)), product, SelectFlags{true})));
        auto& unflagged = b.flush(b.asU32(b.select(IrOpcode::SelectF32, IrType::F32, tiny, b.asF32(b.constant(0u)), product)));
        b.keep(flushed);
        b.keep(unflagged);
        return b.eliminate() == 1u && !flushed.HasUses() && unflagged.HasUses();
    });

    passed &= run("a NaN choice of a quieted operand is as denormal-free as its fallback", [] {
        Builder b;
        auto& candidate = b.unknown();
        auto& isNan = b.emit(IrOpcode::FPIsNan32, IrType::Bool, {&b.asF32(candidate)});
        auto& quieted = b.emit(IrOpcode::BitwiseOr32, IrType::U32, {&candidate, &b.constant(0x00400000u)});
        auto& flushedFallback = b.flush(b.select(IrOpcode::SelectU32, IrType::U32, isNan, quieted, b.flush(b.unknown())));
        auto& unknownFallback = b.flush(b.select(IrOpcode::SelectU32, IrType::U32, isNan, quieted, b.unknown()));
        auto& otherValue = b.flush(b.select(IrOpcode::SelectU32, IrType::U32, isNan, b.unknown(), b.flush(b.unknown())));
        b.keep(flushedFallback);
        b.keep(unknownFallback);
        b.keep(otherValue);
        return b.eliminate() == 1u && !flushedFallback.HasUses() && unknownFallback.HasUses() && otherValue.HasUses();
    });

    passed &= run("a flushed value times a constant of at least one needs no flush, times a half it does", [] {
        Builder b;
        auto& value = b.asF32(b.flush(b.unknown()));
        auto& scaled = b.flush(b.asU32(b.emit(IrOpcode::FPMul32, IrType::F32, {&value, &b.constant(0x40000000u)})));
        auto& halved = b.flush(b.asU32(b.emit(IrOpcode::FPMul32, IrType::F32, {&b.constant(0x3f000000u), &value})));
        b.keep(scaled);
        b.keep(halved);
        return b.eliminate() == 1u && !scaled.HasUses() && halved.HasUses();
    });

    return passed ? 0 : 1;
}
