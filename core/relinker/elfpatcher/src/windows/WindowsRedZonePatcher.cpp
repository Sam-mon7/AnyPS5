// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from shadPS4 / Kyty SuperMedo src/loader/redZonePatcher.cpp for AnyPS5.

#include <elfpatcher/windows/WindowsRedZonePatcher.hpp>
#include <codegen/x86/StubBodyBuilder.hpp>
#include <codegen/x86/X64InstructionDecoder.hpp>
#include <codegen/CodegenException.hpp>
#include <io/BufferUtils.hpp>
#include <algorithm>
#include <bitset>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace Elfpatcher::Windows {

namespace {

using Bytes = std::vector<std::uint8_t>;
using Mask = std::bitset<128>;
using Flow = Codegen::ControlFlowKind;

PeSection* sectionAt(std::vector<PeSection>& sections, const std::uint32_t address, const std::size_t size = 1) {
    for (auto& section : sections)
        if (address >= section.Rva && address - section.Rva < section.Data.size() && size <= section.Data.size() - (address - section.Rva)) return &section;
    return nullptr;
}

std::uint64_t readUnsigned(const std::span<const std::uint8_t> bytes, std::size_t& position, const std::size_t size) {
    if (position > bytes.size() || size > bytes.size() - position) throw Domain::RelinkerException("Red-zone unwind table is truncated");
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < size; ++index) value |= static_cast<std::uint64_t>(bytes[position++]) << (8 * index);
    return value;
}

std::uint64_t ehPointer(const std::span<const std::uint8_t> bytes, std::size_t& position, const std::uint8_t encoding, const std::uint64_t base) {
    if (position > std::numeric_limits<std::uint64_t>::max() - base) throw Domain::RelinkerException("Red-zone unwind field address overflowed");
    const auto field = base + position;
    const auto format = encoding & 15;
    const std::size_t size = format == 0 || format == 4 || format == 12 ? 8 : format == 2 || format == 10 ? 2 : format == 3 || format == 11 ? 4 : 0;
    if (size == 0 || (encoding & 0x80) != 0) throw Domain::RelinkerException("Red-zone unwind pointer encoding is unsupported");
    auto value = readUnsigned(bytes, position, size);
    if (format >= 10 && size < 8 && (value & (std::uint64_t{1} << (size * 8 - 1))) != 0) value |= ~std::uint64_t{0} << (size * 8);
    std::uint64_t relativeBase = 0;
    switch (encoding & 0x70) {
        case 0: return value;
        case 0x10: relativeBase = field; break;
        case 0x30: relativeBase = base; break;
        default: throw Domain::RelinkerException("Red-zone unwind pointer base is unsupported");
    }
    if (format >= 10 && (value >> 63) != 0) {
        const auto magnitude = ~value + 1;
        if (relativeBase < magnitude) throw Domain::RelinkerException("Red-zone unwind pointer underflowed");
        return relativeBase - magnitude;
    }
    if (value > std::numeric_limits<std::uint64_t>::max() - relativeBase) throw Domain::RelinkerException("Red-zone unwind pointer overflowed");
    return relativeBase + value;
}

std::vector<std::uint64_t> functionStarts(const Bytes& source, const std::vector<Domain::ProgramHeader>& headers) {
    std::vector<std::uint64_t> starts;
    for (const auto& header : headers) {
        if (header.Type != 0x6474e550) continue;
        if (header.Offset > source.size() || header.FileSize > source.size() - header.Offset || header.FileSize < 4)
            throw Domain::RelinkerException("Red-zone unwind header exceeds the file", header.Offset);
        const std::span bytes(source.data() + header.Offset, static_cast<std::size_t>(header.FileSize));
        if (bytes[0] != 1) throw Domain::RelinkerException("Red-zone unwind header version is unsupported", header.Offset);
        std::size_t position = 4;
        ehPointer(bytes, position, bytes[1], header.MappedAddress);
        if (bytes[2] == 0xff || bytes[3] == 0xff) continue;
        const auto count = ehPointer(bytes, position, bytes[2], header.MappedAddress);
        if (count > bytes.size() / 2) throw Domain::RelinkerException("Red-zone unwind function count is invalid", header.Offset);
        for (std::uint64_t index = 0; index < count; ++index) {
            starts.push_back(ehPointer(bytes, position, bytes[3], header.MappedAddress));
            ehPointer(bytes, position, bytes[3], header.MappedAddress);
        }
    }
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    return starts;
}

struct Instruction {
    std::uint32_t Address;
    Bytes BytesValue;
    Codegen::DecodedInstructionInfo Info;
    Mask Use;
    Mask Def;
    Mask Live;
    std::optional<std::int64_t> StackDelta;
    std::optional<Codegen::MemoryOperand> Memory;
    std::size_t ModRmOffset = 0;
    bool AccessesMemory = false;
    bool UsesStack = false;
    bool ReplacesStack = false;
    bool RedZone = false;
    bool Conservative = false;
    bool Protect = false;
    bool ProtectedCall = false;
};

bool call(const Instruction& instruction) {
    return instruction.Info.FlowKind == Flow::Call || instruction.Info.FlowKind == Flow::IndirectCall;
}

bool terminator(const Instruction& instruction) {
    const auto flow = instruction.Info.FlowKind;
    return flow == Flow::UnconditionalJump || flow == Flow::IndirectJump || flow == Flow::Return || flow == Flow::Trap;
}

std::optional<std::uint32_t> target(const Instruction& instruction) {
    if (!instruction.Info.HasBranchTarget || instruction.Info.HasRipRelativeDisp) return std::nullopt;
    const auto address = static_cast<std::int64_t>(instruction.Address) + static_cast<std::int64_t>(instruction.Info.Length) + instruction.Info.BranchDisp;
    if (address < 0 || address > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return static_cast<std::uint32_t>(address);
}

struct VectorFields {
    bool Reg = false;
    bool Rm = false;
    bool Vvvv = false;
};

VectorFields vectorFields(const std::uint8_t* data, const Codegen::DecodedInstructionInfo& info) {
    const auto at = info.OpcodeOffset;
    if (!WindowsRedZonePatcher::VectorRegisters() || at + 1 >= info.Length) return {};
    const auto first = data[at];
    std::uint8_t map = 0;
    std::size_t opcodeAt = 0;
    if (first == 0xc5) { map = 1; opcodeAt = at + 2; }
    else if (first == 0xc4) { map = data[at + 1] & 31; opcodeAt = at + 3; }
    else if (first == 0x0f) { map = data[at + 1] == 0x38 ? 2 : data[at + 1] == 0x3a ? 3 : 1; opcodeAt = at + (map == 1 ? 1 : 2); }
    if (map < 1 || map > 3 || opcodeAt >= info.Length) return {};
    const auto opcode = data[opcodeAt];
    if (map != 1) return {opcode < 0xf0, opcode < 0xf0 && (map == 2 || ((opcode & 0xfc) != 0x14 && opcode != 0x20 && opcode != 0x22)), opcode < 0xf0 || map == 3};
    const bool gprRm = opcode == 0x2a || opcode == 0x6e || opcode == 0x7e || opcode == 0xc4;
    const bool gprReg = opcode == 0x2c || opcode == 0x2d || opcode == 0x50 || opcode == 0xc5 || opcode == 0xd7;
    if (first != 0x0f) return {!gprReg, !gprRm, true};
    const bool sse = (opcode >= 0x10 && opcode <= 0x17) || (opcode >= 0x28 && opcode <= 0x2f) || (opcode >= 0x50 && opcode <= 0x7f && opcode != 0x78 && opcode != 0x79) || opcode == 0xc2 || opcode == 0xc4 || opcode == 0xc5 || opcode == 0xc6 || (opcode >= 0xd0 && opcode <= 0xfe);
    return {sse && !gprReg, sse && !gprRm, false};
}

Instruction decode(std::vector<PeSection>& sections, const std::uint32_t address, const std::uint32_t end) {
    const auto* section = sectionAt(sections, address);
    if (section == nullptr || address >= end) throw Codegen::CodegenException("Red-zone instruction is outside executable code", address);
    const auto offset = address - section->Rva;
    const auto available = std::min<std::size_t>(end - address, section->Data.size() - offset);
    const auto* data = section->Data.data() + offset;
    Instruction result{address, {}, Codegen::X64InstructionDecoder{}.DecodeInstruction(data, available)};
    const auto& info = result.Info;
    result.BytesValue.assign(data, data + info.Length);
    auto position = info.OpcodeOffset;
    const auto first = data[position++];
    const bool vector = first == 0xc4 || first == 0xc5 || first == 0x62 || (first == 0x8f && position < info.Length && (data[position] & 31) >= 8);
    std::uint8_t rex = info.RexPrefix;
    if (vector) {
        if (first == 0xc5) { rex = static_cast<std::uint8_t>((~data[position] >> 5) & 4); position += 2; }
        else { rex = static_cast<std::uint8_t>((~data[position] >> 5) & 7); position += first == 0x62 ? 4 : 3; }
    } else if (first == 0x0f) {
        const auto second = data[position++];
        if (second == 0x38 || second == 0x3a) ++position;
    }
    result.ModRmOffset = position;
    const bool scalar = !vector && first != 0x0f;
    const bool wide = (rex & 8) != 0;
    const bool word = std::find(data, data + info.OpcodeOffset, 0x66) != data + info.OpcodeOffset;
    const auto opcode = info.Opcode;
    if ((scalar && (opcode == 0xcc || opcode == 0xcd || opcode == 0xce || opcode == 0xcf || opcode == 0xf1 || opcode == 0xf4)) || (first == 0x0f && (opcode == 0x05 || opcode == 0x07 || opcode == 0x34 || opcode == 0x35))) result.Info.FlowKind = Flow::Trap;
    const bool stackImplicit = scalar && ((opcode >= 0x50 && opcode <= 0x5f) || opcode == 0x68 || opcode == 0x6a || opcode == 0x8f || opcode == 0x9c || opcode == 0x9d || opcode == 0xc8 || opcode == 0xc9 || (opcode == 0xff && info.ModRmRegField == 6));
    result.UsesStack = stackImplicit;
    if (stackImplicit) {
        result.AccessesMemory = true;
        if ((opcode >= 0x50 && opcode <= 0x57) || opcode == 0x68 || opcode == 0x6a || opcode == 0x9c || opcode == 0xff) result.StackDelta = word ? -2 : -8;
        else if ((opcode >= 0x58 && opcode <= 0x5f) || opcode == 0x8f || opcode == 0x9d) result.StackDelta = word ? 2 : 8;
        else result.Conservative = true;
        if (opcode == 0x5c && (rex & 1) == 0) { result.StackDelta.reset(); result.Conservative = true; }
    }
    if (scalar && ((opcode >= 0xa0 && opcode <= 0xa7) || (opcode >= 0xaa && opcode <= 0xaf) || (opcode >= 0x6c && opcode <= 0x6f) || opcode == 0xd7)) result.AccessesMemory = true;
    if (scalar && ((opcode == 0xbc && (rex & 1) == 0) || (opcode == 0x94 && (rex & 1) == 0))) result.Conservative = true;
    if (first == 0x0f && opcode == 0xcc && (rex & 1) == 0) result.Conservative = true;
    if (!info.HasModRm) return result;
    const auto mod = info.ModRmByte >> 6;
    const auto rm = static_cast<std::uint8_t>((info.ModRmByte & 7) | ((rex & 1) << 3));
    const auto reg = static_cast<std::uint8_t>(info.ModRmRegField | ((rex & 4) << 1));
    const bool extension = scalar && (opcode == 0x80 || opcode == 0x81 || opcode == 0x82 || opcode == 0x83 || opcode == 0x8f || opcode == 0xc0 || opcode == 0xc1 || opcode == 0xc6 || opcode == 0xc7 || (opcode >= 0xd0 && opcode <= 0xd3) || opcode == 0xf6 || opcode == 0xf7 || opcode == 0xfe || opcode == 0xff);
    const auto fields = vectorFields(data, info);
    result.UsesStack |= (!extension && !fields.Reg && reg == 4) || (mod == 3 && !fields.Rm && rm == 4);
    if (vector) {
        const auto vvvvOffset = info.OpcodeOffset + (first == 0xc5 ? 1 : 2);
        result.UsesStack |= !fields.Vvvv && ((~data[vvvvOffset] >> 3) & 15) == 4;
    }
    if (mod != 3) {
        result.Memory = Codegen::DecodeMemoryOperand(data, info.Length, position, rex, {});
        const auto& memory = *result.Memory;
        const bool nop = first == 0x0f && opcode == 0x1f;
        const bool lea = scalar && opcode == 0x8d;
        result.AccessesMemory |= !nop && !lea && !memory.StackBase;
        result.UsesStack |= memory.StackBase;
        if (memory.StackBase && memory.Displacement < 0 && !nop) {
            result.RedZone = true;
            if (lea && reg == 4 && memory.Sib == 0x24 && !info.AddressSize32 && wide) {
                result.RedZone = false;
                result.StackDelta = memory.Displacement;
            } else if (lea || memory.Sib != 0x24 || info.AddressSize32) {
                result.Conservative = true;
            } else {
                std::size_t width = wide ? 8 : word ? 2 : 4;
                bool reads = true;
                bool writes = false;
                bool known = scalar;
                if (opcode == 0x88 || opcode == 0x8a || opcode == 0xc6) width = 1;
                if (opcode == 0x63) width = 4;
                if (opcode == 0x88 || opcode == 0x89 || opcode == 0xc6 || opcode == 0xc7) { reads = false; writes = true; }
                else if (opcode != 0x8a && opcode != 0x8b && opcode != 0x63) known = false;
                if (!known) result.Conservative = true;
                else for (auto displacement = std::max<std::int64_t>(memory.Displacement, -128); displacement < std::min<std::int64_t>(memory.Displacement + static_cast<std::int64_t>(width), 0); ++displacement) {
                    const auto bit = static_cast<std::size_t>(displacement + 128);
                    if (reads) result.Use.set(bit);
                    if (writes) result.Def.set(bit);
                }
            }
        }
        if (scalar && opcode == 0x8b && reg == 4 && (wide || !word)) result.ReplacesStack = true;
        if (lea && reg == 4 && memory.StackBase && memory.Sib == 0x24 && !info.AddressSize32 && wide) result.StackDelta = memory.Displacement;
    }
    if (scalar && mod == 3 && rm == 4 && wide && (opcode == 0x81 || opcode == 0x83) && (info.ModRmRegField == 0 || info.ModRmRegField == 5)) {
        std::int64_t immediate = opcode == 0x83 ? static_cast<std::int8_t>(data[info.Length - 1]) : static_cast<std::int32_t>(Io::ReadU32(result.BytesValue, info.Length - 4));
        result.StackDelta = info.ModRmRegField == 0 ? immediate : -immediate;
    } else if (result.UsesStack && !result.StackDelta && mod == 3 && !call(result)) {
        result.Conservative = true;
    }
    if (scalar && opcode == 0x8f && mod == 3 && rm == 4) { result.StackDelta.reset(); result.Conservative = true; }
    if (!extension && reg == 4 && !result.StackDelta) result.Conservative = true;
    if (result.UsesStack && !result.StackDelta && (!result.Memory || !result.Memory->StackBase)) result.Conservative = true;
    if (result.ReplacesStack || (result.UsesStack && !result.StackDelta && scalar && opcode != 0x89 && opcode != 0x8b && opcode != 0x8d && !call(result))) result.Conservative = true;
    return result;
}

struct Function {
    std::map<std::uint32_t, Instruction> Instructions;
    std::set<std::uint32_t> Targets;
    std::map<std::uint32_t, std::vector<std::uint32_t>> IndirectTargets;
    bool RedZone = false;
    bool Conservative = false;
    bool Indirect = false;
    bool Refused = false;
    std::uint64_t Swept = 0;
};

bool opcode(const Instruction& instruction, const std::uint8_t value) {
    return !instruction.Info.IsTwoByteOpcode && instruction.BytesValue[instruction.Info.OpcodeOffset] == value;
}

unsigned regField(const Instruction& instruction) {
    return instruction.Info.ModRmRegField | ((instruction.Info.RexPrefix & 4u) << 1);
}

unsigned rmField(const Instruction& instruction) {
    return (instruction.Info.ModRmByte & 7u) | ((instruction.Info.RexPrefix & 1u) << 3);
}

bool writesRegister(const Instruction& instruction, const unsigned reg) {
    const auto& info = instruction.Info;
    if (call(instruction)) return true;
    if (info.HasBranchTarget) return !info.IsTwoByteOpcode && info.Opcode >= 0xe0 && info.Opcode <= 0xe2 && reg == 1;
    if (terminator(instruction)) return false;
    const auto first = instruction.BytesValue[info.OpcodeOffset];
    if (first == 0xc4 || first == 0xc5 || first == 0x62) {
        if (first == 0x62) return true;
        const auto map = first == 0xc5 ? 1 : instruction.BytesValue[info.OpcodeOffset + 1] & (first == 0x62 ? 7 : 31);
        const auto op = instruction.BytesValue[instruction.ModRmOffset - 1];
        if (map < 1 || map > 3) return true;
        return (map == 2 && op >= 0xf2 && op <= 0xf7) || (map == 3 && (op == 0xf0 || (op >= 0x14 && op <= 0x17 && (info.ModRmByte >> 6) == 3))) || (map == 1 && (op == 0x2c || op == 0x2d || op == 0x50 || op == 0x7e || op == 0xc5 || op == 0xd7 || (op >= 0x90 && op <= 0x93)));
    }
    if (first == 0x8f && (instruction.BytesValue[info.OpcodeOffset + 1] & 31) >= 8) return true;
    if (!info.HasModRm) {
        if (first >= 0xb0 && first <= 0xbf) return ((first & 7u) | ((info.RexPrefix & 1u) << 3)) == reg || ((first & 7u) >= 4 && (first & 7u) - 4 == reg);
        if (first == 0x90) return (info.RexPrefix & 1) != 0 && (reg == 0 || reg == 8);
        if (first >= 0x50 && first <= 0x57) return reg == 4;
        if (first >= 0x58 && first <= 0x5f) return rmField(instruction) == reg || reg == 4 || ((first & 7u) | ((info.RexPrefix & 1u) << 3)) == reg;
        return true;
    }
    const bool registerRm = (info.ModRmByte >> 6) == 3;
    const auto writesRm = [&] { return registerRm && rmField(instruction) == reg; };
    if (info.IsTwoByteOpcode) {
        const auto op = info.Opcode;
        if (op == 0x1f) return false;
        if (op == 0x2c || op == 0x2d || op == 0x50 || op == 0xc5 || op == 0xd7) return regField(instruction) == reg;
        if (op == 0x7e) return writesRm();
        if ((op >= 0x10 && op <= 0x17) || (op >= 0x28 && op <= 0x2f) || (op >= 0x51 && op <= 0x7f) || (op >= 0xc2 && op <= 0xc6) || op >= 0xd0) return false;
        return true;
    }
    const auto op = info.Opcode;
    if (op == 0x88 || op == 0x89 || op == 0xc6 || op == 0xc7) return writesRm();
    if (op == 0x8a || op == 0x8b || op == 0x8d || op == 0x63 || op == 0x69 || op == 0x6b) return regField(instruction) == reg;
    if (op <= 0x3b && (op & 7) <= 3) return op < 0x38 && ((op & 2) != 0 ? regField(instruction) == reg : writesRm());
    if (op == 0x84 || op == 0x85) return false;
    if (op >= 0x80 && op <= 0x83) return info.ModRmRegField != 7 && writesRm();
    return true;
}

bool registerAdd(const Instruction& instruction, const unsigned destination, unsigned& source) {
    if ((instruction.Info.ModRmByte >> 6) != 3 || (instruction.Info.RexPrefix & 8) == 0) return false;
    if (opcode(instruction, 0x01) && rmField(instruction) == destination) { source = regField(instruction); return true; }
    if (opcode(instruction, 0x03) && regField(instruction) == destination) { source = rmField(instruction); return true; }
    return false;
}

bool registerCopy(const Instruction& instruction, unsigned& source) {
    if ((instruction.Info.ModRmByte >> 6) != 3 || (instruction.Info.RexPrefix & 8) == 0) return false;
    if (opcode(instruction, 0x89)) { source = regField(instruction); return true; }
    if (opcode(instruction, 0x8b)) { source = rmField(instruction); return true; }
    return false;
}

std::optional<std::uint32_t> tableAddress(const Instruction& instruction, const unsigned reg) {
    if (!opcode(instruction, 0x8d) || (instruction.Info.RexPrefix & 8) == 0 || regField(instruction) != reg || !instruction.Info.HasRipRelativeDisp || instruction.Info.AddressSize32) return std::nullopt;
    const auto displacement = static_cast<std::int32_t>(Io::ReadU32(instruction.BytesValue, instruction.Info.RipRelativeDispOffset));
    const auto address = static_cast<std::int64_t>(instruction.Address) + static_cast<std::int64_t>(instruction.Info.Length) + displacement;
    if (address < 0 || address > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return static_cast<std::uint32_t>(address);
}

bool tableLoad(const Instruction& instruction, const unsigned destination, unsigned& base, unsigned& index) {
    if (!opcode(instruction, 0x63) || (instruction.Info.RexPrefix & 8) == 0 || regField(instruction) != destination || !instruction.Memory || instruction.Memory->Displacement != 0 || instruction.Info.AddressSize32 || instruction.Info.SegmentPrefix != 0) return false;
    const auto& memory = *instruction.Memory;
    if (memory.Rm != 4 || (memory.Sib >> 6) != 2 || (memory.Mod == 0 && (memory.Sib & 7) == 5)) return false;
    base = (memory.Sib & 7u) | ((memory.RexIndexBase & 1u) << 3);
    index = ((memory.Sib >> 3) & 7u) | ((memory.RexIndexBase & 2u) << 2);
    return index != 4;
}

std::optional<std::uint32_t> tableEntry(std::vector<PeSection>& sections, const std::uint32_t table, const std::size_t index, const std::uint32_t begin, const std::uint32_t end) {
    const auto address = static_cast<std::uint64_t>(table) + index * 4;
    if (address > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    const auto* section = sectionAt(sections, static_cast<std::uint32_t>(address), 4);
    if (section == nullptr || !section->Name.starts_with(".elf")) return std::nullopt;
    const auto displacement = static_cast<std::int32_t>(Io::ReadU32(section->Data, address - section->Rva));
    const auto destination = static_cast<std::int64_t>(table) + displacement;
    if (destination < begin || destination >= end) return std::nullopt;
    return static_cast<std::uint32_t>(destination);
}

std::optional<std::vector<std::uint32_t>> boundedTable(const Function& function, std::vector<PeSection>& sections, const std::uint32_t branchAddress, const std::uint32_t begin, const std::uint32_t end) {
    const auto branch = function.Instructions.find(branchAddress);
    if (branch == function.Instructions.end() || !opcode(branch->second, 0xff) || branch->second.Info.ModRmRegField != 4 || (branch->second.Info.ModRmByte >> 6) != 3) return std::nullopt;
    const auto destination = rmField(branch->second);
    const auto previous = [&](auto cursor) {
        if (cursor == function.Instructions.begin()) return function.Instructions.end();
        const auto result = std::prev(cursor);
        return result->first + result->second.Info.Length == cursor->first ? result : function.Instructions.end();
    };
    auto add = branch;
    unsigned base = 0;
    for (unsigned count = 0; count <= 4; ++count) {
        add = previous(add);
        if (add == function.Instructions.end()) return std::nullopt;
        if (registerAdd(add->second, destination, base)) break;
        if (writesRegister(add->second, destination) || terminator(add->second) || count == 4) return std::nullopt;
    }
    const auto load = previous(add);
    unsigned loadBase = 0;
    unsigned index = 0;
    if (load == function.Instructions.end() || !tableLoad(load->second, destination, loadBase, index) || loadBase != base) return std::nullopt;
    std::optional<std::size_t> count;
    std::optional<std::uint32_t> guarded;
    auto cursor = load;
    for (unsigned step = 0; step < 64; ++step) {
        cursor = previous(cursor);
        if (cursor == function.Instructions.end()) break;
        const auto& instruction = cursor->second;
        const bool accumulatorCompare = opcode(instruction, 0x3d) && index == 0;
        const bool groupCompare = (opcode(instruction, 0x81) || opcode(instruction, 0x83)) && instruction.Info.ModRmRegField == 7 && (instruction.Info.ModRmByte >> 6) == 3 && rmField(instruction) == index;
        if (accumulatorCompare || groupCompare) {
            const auto prefixEnd = instruction.BytesValue.begin() + static_cast<std::ptrdiff_t>(instruction.Info.OpcodeOffset);
            if ((instruction.Info.RexPrefix & 8) == 0 && std::find(instruction.BytesValue.begin(), prefixEnd, 0x66) != prefixEnd) return std::nullopt;
            const auto immediate = opcode(instruction, 0x83) ? static_cast<std::int64_t>(static_cast<std::int8_t>(instruction.BytesValue.back())) : static_cast<std::int64_t>(static_cast<std::int32_t>(Io::ReadU32(instruction.BytesValue, instruction.Info.Length - 4)));
            const auto guard = function.Instructions.find(CheckedRva(instruction.Address + instruction.Info.Length));
            if (immediate < 0 || guard == function.Instructions.end()) return std::nullopt;
            const auto& info = guard->second.Info;
            const bool above = info.Opcode == (info.IsTwoByteOpcode ? 0x87 : 0x77);
            const bool aboveEqual = info.Opcode == (info.IsTwoByteOpcode ? 0x83 : 0x73);
            if (!above && !aboveEqual) return std::nullopt;
            count = static_cast<std::size_t>(immediate) + above;
            guarded = CheckedRva(guard->first + guard->second.Info.Length);
            break;
        }
        if (writesRegister(instruction, index) || terminator(instruction)) return std::nullopt;
    }
    if (!count || *count == 0 || *count > 4096 || std::any_of(function.Targets.begin(), function.Targets.end(), [&](auto address) { return address >= *guarded && address <= branchAddress; })) return std::nullopt;
    std::set<std::uint32_t> tables;
    cursor = load;
    for (unsigned step = 0; step < 64; ++step) {
        cursor = previous(cursor);
        if (cursor == function.Instructions.end()) break;
        if (!writesRegister(cursor->second, base)) continue;
        if (const auto table = tableAddress(cursor->second, base)) tables.insert(*table);
        break;
    }
    if (tables.empty()) for (const auto& [address, instruction] : function.Instructions) {
        if (address >= branchAddress) break;
        if (const auto table = tableAddress(instruction, base)) tables.insert(*table);
    }
    std::optional<std::vector<std::uint32_t>> result;
    for (const auto table : tables) {
        std::vector<std::uint32_t> targets;
        for (std::size_t entry = 0; entry < *count; ++entry) {
            const auto address = tableEntry(sections, table, entry, begin, end);
            if (!address) break;
            targets.push_back(*address);
        }
        if (targets.size() != *count) continue;
        if (result) return std::nullopt;
        result = std::move(targets);
    }
    return result;
}

std::set<std::uint32_t> linearBoundaries(const Function& function, std::vector<PeSection>& sections, const std::uint32_t begin, const std::uint32_t end) {
    std::set<std::uint32_t> result;
    for (auto address = begin; address < end;) {
        result.insert(address);
        if (const auto known = function.Instructions.find(address); known != function.Instructions.end()) { address = CheckedRva(address + known->second.Info.Length); continue; }
        try {
            const auto instruction = decode(sections, address, end);
            const auto next = function.Instructions.lower_bound(address);
            if (next != function.Instructions.end() && next->first < address + instruction.Info.Length) return {};
            address = CheckedRva(address + instruction.Info.Length);
        } catch (const Codegen::CodegenException&) { return {}; }
    }
    return result;
}

std::optional<std::vector<std::uint32_t>> scannedTable(const Function& function, std::vector<PeSection>& sections, const std::uint32_t branchAddress, const std::uint32_t begin, const std::uint32_t end, const std::set<std::uint32_t>& boundaries) {
    using Cursor = decltype(function.Instructions.begin());
    const auto branch = function.Instructions.find(branchAddress);
    if (branch == function.Instructions.end() || !opcode(branch->second, 0xff) || branch->second.Info.ModRmRegField != 4 || (branch->second.Info.ModRmByte >> 6) != 3) return std::nullopt;
    std::size_t steps = 0;
    std::function<bool(Cursor, unsigned, unsigned, std::vector<Cursor>&)> definitions;
    const auto after = [&](Cursor last, unsigned reg, unsigned depth, std::vector<Cursor>& result) {
        if (call(last->second)) return false;
        if (writesRegister(last->second, reg)) { result.push_back(last); return true; }
        return definitions(last, reg, depth, result);
    };
    definitions = [&](Cursor at, unsigned reg, unsigned depth, std::vector<Cursor>& result) {
        if (++steps > 8192 || depth > 6 || at == function.Instructions.begin()) return false;
        const auto previous = std::prev(at);
        const bool fallsThrough = previous->first + previous->second.Info.Length == at->first && !terminator(previous->second);
        if (function.Targets.contains(at->first)) {
            bool any = false;
            for (auto it = function.Instructions.begin(); it != function.Instructions.end(); ++it) {
                const auto flow = it->second.Info.FlowKind;
                if ((flow == Flow::ConditionalBranch || flow == Flow::UnconditionalJump) && target(it->second) == at->first) {
                    if (!definitions(it, reg, depth + 1, result)) return false;
                    any = true;
                }
            }
            if (!fallsThrough) return any;
        } else if (!fallsThrough) return false;
        return after(previous, reg, depth, result);
    };
    std::function<bool(Cursor, unsigned, unsigned, std::set<std::uint32_t>&)> tablesAt;
    tablesAt = [&](Cursor at, unsigned reg, unsigned depth, std::set<std::uint32_t>& tables) {
        std::vector<Cursor> defs;
        if (depth > 3 || !definitions(at, reg, 0, defs)) return false;
        for (const auto def : defs) {
            if (const auto address = tableAddress(def->second, reg)) tables.insert(*address);
            else {
                unsigned source = 0;
                if (!registerCopy(def->second, source) || !tablesAt(def, source, depth + 1, tables)) return false;
            }
        }
        return !tables.empty();
    };
    const auto destination = rmField(branch->second);
    std::vector<Cursor> adds;
    if (!definitions(branch, destination, 0, adds)) return std::nullopt;
    std::set<std::uint32_t> loadTables;
    std::set<std::uint32_t> addTables;
    for (const auto add : adds) {
        unsigned source = 0;
        if (!registerAdd(add->second, destination, source) || !tablesAt(add, source, 0, addTables)) return std::nullopt;
        std::vector<Cursor> loads;
        if (!definitions(add, destination, 0, loads)) return std::nullopt;
        for (const auto load : loads) {
            unsigned base = 0;
            unsigned index = 0;
            if (!tableLoad(load->second, destination, base, index) || !tablesAt(load, base, 0, loadTables)) return std::nullopt;
        }
    }
    if (loadTables.empty() || loadTables != addTables) return std::nullopt;
    std::vector<std::uint32_t> result;
    for (const auto table : loadTables) {
        if (table >= begin && table < end) return std::nullopt;
        std::size_t entries = 0;
        for (std::size_t index = 0; index < 2048; ++index) {
            const auto entry = tableEntry(sections, table, index, begin, end);
            if (!entry || !boundaries.contains(*entry)) break;
            result.push_back(*entry);
            ++entries;
            if (index == 2047) return std::nullopt;
        }
        if (entries < 2) return std::nullopt;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

Function decodeFunction(std::vector<PeSection>& sections, const std::uint32_t begin, const std::uint32_t end) {
    Function function;
    const auto allowed = [&](const std::uint32_t address) {
        if (address >= begin && address < end) return true;
        const auto* section = sectionAt(sections, address);
        return section != nullptr && (section->Name == ".gtcode" || section->Name == ".amdstub");
    };
    std::vector<std::uint32_t> blocks{begin};
    std::set<std::uint32_t> indirect;
    std::set<std::uint32_t> resolved;
    std::optional<std::set<std::uint32_t>> linearStarts;
    for (;;) {
        while (!blocks.empty()) {
            auto address = blocks.back();
            blocks.pop_back();
            while (allowed(address) && !function.Instructions.contains(address)) {
                const auto* section = sectionAt(sections, address);
                if (section == nullptr) { function.Refused = true; break; }
                const auto limit = address >= begin && address < end ? end : CheckedRva(section->Rva + section->Data.size());
                Instruction instruction;
                try { instruction = decode(sections, address, limit); }
                catch (const Codegen::CodegenException&) { function.Refused = true; break; }
                const auto next = CheckedRva(address + instruction.Info.Length);
                const auto branch = target(instruction);
                function.RedZone |= instruction.RedZone;
                function.Conservative |= instruction.Conservative;
                function.Conservative |= instruction.StackDelta && *instruction.StackDelta != 0;
                const bool jump = instruction.Info.FlowKind == Flow::UnconditionalJump || instruction.Info.FlowKind == Flow::ConditionalBranch;
                if (branch && allowed(*branch)) {
                    function.Targets.insert(*branch);
                    if (jump) blocks.push_back(*branch);
                }
                if (instruction.Info.FlowKind == Flow::IndirectJump || (instruction.Info.FlowKind == Flow::UnconditionalJump && !branch)) indirect.insert(address);
                const bool stop = terminator(instruction);
                function.Instructions.emplace(address, std::move(instruction));
                if (stop) break;
                address = next;
            }
        }
        for (const auto address : indirect) {
            if (resolved.contains(address)) continue;
            auto targets = boundedTable(function, sections, address, begin, end);
            if (!targets) {
                if (!linearStarts) linearStarts = linearBoundaries(function, sections, begin, end);
                if (!linearStarts->empty()) targets = scannedTable(function, sections, address, begin, end, *linearStarts);
            }
            if (!targets) continue;
            resolved.insert(address);
            function.IndirectTargets.emplace(address, *targets);
            for (const auto entry : *targets) {
                function.Targets.insert(entry);
                if (!function.Instructions.contains(entry)) blocks.push_back(entry);
            }
        }
        if (blocks.empty()) break;
    }
    function.Indirect = indirect.size() != resolved.size();
    for (const auto& [address, instruction] : function.Instructions) {
        const auto next = function.Instructions.upper_bound(address);
        if (next != function.Instructions.end() && next->first < address + instruction.Info.Length) function.Refused = true;
    }
    if (!function.Indirect || function.Refused) return function;
    function.Conservative = true;
    std::vector<Instruction> swept;
    for (auto address = begin; address < end;) {
        if (const auto known = function.Instructions.find(address); known != function.Instructions.end()) { address = CheckedRva(address + known->second.Info.Length); continue; }
        try {
            auto instruction = decode(sections, address, end);
            const auto next = function.Instructions.lower_bound(address);
            if (next != function.Instructions.end() && next->first < address + instruction.Info.Length) { function.Refused = true; return function; }
            address = CheckedRva(address + instruction.Info.Length);
            swept.push_back(std::move(instruction));
        } catch (const Codegen::CodegenException&) { function.Refused = true; return function; }
    }
    std::set<std::uint32_t> boundaries;
    for (const auto& [address, instruction] : function.Instructions) boundaries.insert(address);
    for (const auto& instruction : swept) boundaries.insert(instruction.Address);
    const auto inlineData = [&](const Instruction& instruction) {
        if (!instruction.Info.HasRipRelativeDisp) return false;
        const auto displacement = static_cast<std::int32_t>(Io::ReadU32(instruction.BytesValue, instruction.Info.RipRelativeDispOffset));
        const auto pointer = static_cast<std::int64_t>(instruction.Address) + static_cast<std::int64_t>(instruction.Info.Length) + displacement;
        return pointer >= begin && pointer < end && !boundaries.contains(static_cast<std::uint32_t>(pointer));
    };
    for (const auto& [address, instruction] : function.Instructions) if (inlineData(instruction)) function.Refused = true;
    for (auto& instruction : swept) {
        if (inlineData(instruction)) function.Refused = true;
        function.RedZone |= instruction.RedZone;
        if (const auto branch = target(instruction); branch && allowed(*branch)) function.Targets.insert(*branch);
        function.Targets.insert(instruction.Address);
        function.Instructions.emplace(instruction.Address, std::move(instruction));
    }
    function.Swept = swept.size();
    return function;
}

Mask translate(const Mask& mask, const std::int64_t delta) {
    Mask translated;
    for (std::size_t bit = 0; bit < 128; ++bit) {
        if (!mask[bit] || delta < -127 || delta > 127) continue;
        const auto offset = static_cast<std::int64_t>(bit) + delta;
        if (offset >= 0 && offset < 128) translated.set(static_cast<std::size_t>(offset));
    }
    return translated;
}

void analyze(Function& function) {
    if (!function.RedZone) return;
    if (function.Conservative || function.Indirect || std::any_of(function.Targets.begin(), function.Targets.end(), [&](auto address) { return !function.Instructions.contains(address); })) {
        for (auto& [address, instruction] : function.Instructions) instruction.Live.set();
        return;
    }
    bool changed;
    do {
        changed = false;
        for (auto it = function.Instructions.rbegin(); it != function.Instructions.rend(); ++it) {
            auto& instruction = it->second;
            Mask live;
            const auto successor = [&](std::uint32_t address) {
                const auto found = function.Instructions.find(address);
                if (found != function.Instructions.end()) live |= found->second.Live;
            };
            if (!terminator(instruction)) successor(CheckedRva(instruction.Address + instruction.Info.Length));
            if (instruction.Info.FlowKind == Flow::UnconditionalJump || instruction.Info.FlowKind == Flow::ConditionalBranch)
                if (const auto branch = target(instruction)) successor(*branch);
            if (const auto targets = function.IndirectTargets.find(instruction.Address); targets != function.IndirectTargets.end())
                for (const auto address : targets->second) successor(address);
            if (instruction.StackDelta) live = translate(live, *instruction.StackDelta);
            const auto next = instruction.Use | (live & ~instruction.Def);
            if (next != instruction.Live) { instruction.Live = next; changed = true; }
        }
    } while (changed);
}

void lowerStack(Codegen::StubBodyBuilder& body) { body.Raw(Bytes{0x48, 0x8d, 0x64, 0x24, 0x80}); }
void restoreStack(Codegen::StubBodyBuilder& body) { body.Raw(Bytes{0x48, 0x8d, 0xa4, 0x24, 0x80, 0, 0, 0}); }

void emitInstruction(Codegen::StubBodyBuilder& body, const Instruction& instruction) {
    const auto& info = instruction.Info;
    if (instruction.Protect) lowerStack(body);
    if (instruction.ProtectedCall) {
        const auto& memory = *instruction.Memory;
        body.Raw(Bytes{0x41, 0x53});
        Bytes load;
        if (info.SegmentPrefix != 0) load.push_back(info.SegmentPrefix);
        if (info.AddressSize32) load.push_back(0x67);
        load.insert(load.end(), {static_cast<std::uint8_t>(0x4c | memory.RexIndexBase), 0x8b, static_cast<std::uint8_t>((info.ModRmByte & 0xc7) | 0x18)});
        load.insert(load.end(), instruction.BytesValue.begin() + static_cast<std::ptrdiff_t>(instruction.ModRmOffset + 1), instruction.BytesValue.end());
        if (memory.RipRelative) {
            const auto displacementOffset = load.size() - 4;
            body.RipRelative(load, displacementOffset, static_cast<std::int64_t>(info.Length) + memory.Displacement);
        } else body.Raw(load);
        body.Raw(Bytes{0x4c, 0x87, 0x1c, 0x24, 0x48, 0x8d, 0xa4, 0x24, 0x88, 0, 0, 0, 0xff, 0x94, 0x24, 0x78, 0xff, 0xff, 0xff});
        body.Advance(info.Length);
        return;
    }
    if (call(instruction)) {
        if (!info.HasBranchTarget) throw Codegen::CodegenException("Red-zone register call cannot be relocated", instruction.Address);
        body.RipRelative(instruction.BytesValue, info.Length - 4, static_cast<std::int64_t>(info.Length) + info.BranchDisp);
        body.Advance(info.Length);
    } else body.MoveControlFlow(instruction.BytesValue, info);
    if (instruction.Protect && !instruction.ReplacesStack) restoreStack(body);
}

void report(const RedZonePatchResult& result, const std::string_view module) {
    std::clog << "Red-zone patching " << module << ": functions=" << result.FunctionCount << " instructions=" << result.InstructionCount
              << " red_zone=" << result.RedZoneFunctionCount << " memory=" << result.MemoryInstructionCount << " short=" << result.ShortMemoryInstructionCount
              << " patched=" << result.PatchedMemoryInstructionCount << " stack=" << result.StackDependentMemoryInstructionCount << " control=" << result.ControlFlowMemoryInstructionCount
              << " unrelocatable=" << result.UnrelocatableMemoryInstructionCount << " indirect=" << result.IndirectRedZoneFunctionCount << " swept=" << result.SweptInstructionCount
              << " calls=" << result.LiveCallCount << " refused_functions=" << result.RefusedFunctionCount << " missing_eh_frame=" << result.MissingEhFrameCount << '\n';
}

void refused(const std::string_view module, const std::uint32_t address, const char* reason) {
    std::clog << "Red-zone unprotected " << module << " rva=0x" << std::hex << address << std::dec << " reason=" << reason << '\n';
}

}

bool WindowsRedZonePatcher::Enabled() {
    static const bool enabled = [] { const auto* value = std::getenv("ANYPS5_RELINK_REDZONE"); return value != nullptr && std::strcmp(value, "1") == 0; }();
    return enabled;
}

bool WindowsRedZonePatcher::VectorRegisters() {
    static const bool enabled = [] { const auto* value = std::getenv("ANYPS5_RELINK_REDZONE_VECTOR_REGISTERS"); return Enabled() && value != nullptr && std::strcmp(value, "1") == 0; }();
    return enabled;
}

RedZonePatchResult WindowsRedZonePatcher::Build(const Bytes& source, const std::vector<Domain::ProgramHeader>& headers, const WindowsLoadImage& image, std::vector<PeSection>& sections, std::uint32_t& nextRva, const std::span<const std::uint32_t> baseRelocations, const std::string_view module) const {
    RedZonePatchResult result;
    if (!Enabled()) return result;
    const auto starts = functionStarts(source, headers);
    for (const auto start : starts)
        if (std::none_of(headers.begin(), headers.end(), [&](const auto& header) { return header.Type == 1 && (header.Flags & 1) != 0 && start >= header.MappedAddress && start - header.MappedAddress < header.FileSize; }))
            throw Domain::RelinkerException("Red-zone unwind function is outside file-backed executable code", start);
    std::vector<std::uint32_t> fixedSlots(baseRelocations.begin(), baseRelocations.end());
    std::sort(fixedSlots.begin(), fixedSlots.end());
    result.MissingEhFrameCount = starts.empty();
    const auto stubRva = nextRva;
    Bytes stubs{'A', 'P', 'S', '5', 'R', 'Z', 1, 0};
    for (const auto& header : headers) {
        if (header.Type != 1 || (header.Flags & 1) == 0 || header.FileSize == 0) continue;
        const auto begin = image.GetRva(header.MappedAddress, header.FileSize);
        const auto end = CheckedRva(begin + header.FileSize);
        std::vector<std::uint32_t> functions;
        for (const auto start : starts)
            if (start >= header.MappedAddress && start - header.MappedAddress < header.FileSize) functions.push_back(image.GetRva(start));
        for (std::size_t index = 0; index < functions.size(); ++index) {
            ++result.FunctionCount;
            auto function = decodeFunction(sections, functions[index], index + 1 < functions.size() ? functions[index + 1] : end);
            result.InstructionCount += function.Instructions.size();
            result.SweptInstructionCount += function.Swept;
            result.RedZoneFunctionCount += function.RedZone;
            result.IndirectRedZoneFunctionCount += function.RedZone && function.Indirect;
            for (const auto address : function.Targets) if (!function.Instructions.contains(address)) function.Refused = true;
            if (function.Refused) { ++result.RefusedFunctionCount; refused(module, functions[index], "function-decode-or-inline-data"); continue; }
            analyze(function);
            for (auto& [address, instruction] : function.Instructions) {
                if (!function.RedZone || instruction.Live.none()) continue;
                if (call(instruction) && instruction.Info.HasBranchTarget && !instruction.Info.HasRipRelativeDisp) {
                    ++result.LiveCallCount;
                    if (function.Conservative) { ++result.UnrelocatableMemoryInstructionCount; refused(module, address, "unproven-call-liveness"); continue; }
                    instruction.Protect = true;
                    continue;
                }
                if (!instruction.AccessesMemory) continue;
                ++result.MemoryInstructionCount;
                result.ShortMemoryInstructionCount += instruction.Info.Length < 5;
                const auto prefixEnd = instruction.BytesValue.begin() + static_cast<std::ptrdiff_t>(instruction.Info.OpcodeOffset);
                if (call(instruction) && instruction.Memory && !instruction.UsesStack && instruction.Info.ModRmRegField == 2 && std::find(instruction.BytesValue.begin(), prefixEnd, 0x66) == prefixEnd) {
                    instruction.Protect = true;
                    instruction.ProtectedCall = true;
                } else if (instruction.UsesStack && !instruction.ReplacesStack) {
                    ++result.StackDependentMemoryInstructionCount;
                    if (VectorRegisters()) refused(module, address, "stack-dependent");
                }
                else if (call(instruction) || terminator(instruction)) ++result.ControlFlowMemoryInstructionCount;
                else instruction.Protect = true;
            }
            using Span = std::vector<Instruction*>;
            std::set<std::uint32_t> patched;
            std::vector<std::uint32_t> relays;
            std::vector<std::uint32_t> shortRelays;
            const auto jumpFits = [](const std::uint32_t from, const std::uint32_t to, const bool shortJump) {
                const auto displacement = static_cast<std::int64_t>(to) - (static_cast<std::int64_t>(from) + (shortJump ? 2 : 5));
                return shortJump ? displacement >= -128 && displacement <= 127 : displacement >= std::numeric_limits<std::int32_t>::min() && displacement <= std::numeric_limits<std::int32_t>::max();
            };
            const auto writeJump = [&](const std::uint32_t from, const std::uint32_t to, const bool shortJump) {
                auto* section = sectionAt(sections, from, shortJump ? 2 : 5);
                if (section == nullptr || !jumpFits(from, to, shortJump)) return false;
                const auto offset = from - section->Rva;
                const auto displacement = static_cast<std::int64_t>(to) - (static_cast<std::int64_t>(from) + (shortJump ? 2 : 5));
                section->Data[offset] = shortJump ? 0xeb : 0xe9;
                if (shortJump) section->Data[offset + 1] = static_cast<std::uint8_t>(displacement);
                else Io::WriteU32(section->Data, offset + 1, static_cast<std::uint32_t>(displacement));
                return true;
            };
            const auto publish = [&](const Span& span, const std::uint32_t address, const bool shortJump = false, const std::size_t reserved = 5) {
                const auto start = span.front()->Address;
                const auto size = span.back()->Address + span.back()->Info.Length - start;
                auto* section = sectionAt(sections, start, size);
                if (section == nullptr || size < (shortJump ? 2u : 5u) || !jumpFits(start, address, shortJump)) return false;
                std::fill_n(section->Data.begin() + (start - section->Rva), size, 0x90);
                writeJump(start, address, shortJump);
                for (const auto* instruction : span)
                    if (patched.insert(instruction->Address).second) result.PatchedMemoryInstructionCount += instruction->Protect;
                if (size >= reserved + 5) relays.push_back(CheckedRva(start + reserved));
                if (size >= reserved + 2) shortRelays.push_back(CheckedRva(start + reserved));
                return true;
            };
            const auto emitBody = [&](const Span& span) -> std::optional<std::uint32_t> {
                const auto patchStart = span.front()->Address;
                const auto continuation = CheckedRva(span.back()->Address + span.back()->Info.Length);
                const auto patchSize = continuation - patchStart;
                auto* section = sectionAt(sections, patchStart, patchSize);
                if (section == nullptr) return std::nullopt;
                for (const auto* instruction : span) {
                    const auto offset = instruction->Address - section->Rva;
                    if (patched.contains(instruction->Address) || !std::equal(instruction->BytesValue.begin(), instruction->BytesValue.end(), section->Data.begin() + offset)) return std::nullopt;
                }
                const auto relocation = std::lower_bound(fixedSlots.begin(), fixedSlots.end(), patchStart > 7 ? patchStart - 7 : 0);
                if (relocation != fixedSlots.end() && *relocation < continuation) return std::nullopt;
                const auto aligned = Io::AlignUp(stubs.size(), std::size_t{16});
                const auto address = CheckedRva(stubRva + aligned);
                try {
                    Codegen::StubBodyBuilder builder;
                    for (const auto* instruction : span) {
                        builder.Seek(static_cast<std::int64_t>(instruction->Address) - patchStart);
                        emitInstruction(builder, *instruction);
                    }
                    auto body = builder.Finish();
                    const auto returnDisplacement = static_cast<std::int64_t>(continuation) - (static_cast<std::int64_t>(address) + static_cast<std::int64_t>(body.ReturnBranchOffset) + 5);
                    if (returnDisplacement < std::numeric_limits<std::int32_t>::min() || returnDisplacement > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
                    Io::WriteU32(body.Bytes, body.ReturnBranchOffset + 1, static_cast<std::uint32_t>(returnDisplacement));
                    Codegen::ApplyStubRelocations(body.Bytes, body.Relocations, patchStart, address, patchStart);
                    const auto displacement = static_cast<std::int64_t>(address) - (static_cast<std::int64_t>(patchStart) + 5);
                    if (displacement < std::numeric_limits<std::int32_t>::min() || displacement > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
                    stubs.resize(aligned, 0xcc);
                    stubs.insert(stubs.end(), body.Bytes.begin(), body.Bytes.end());
                    return address;
                } catch (const Codegen::CodegenException&) { return std::nullopt; }
            };
            const auto emitSpan = [&](const Span& span) {
                const auto offset = stubs.size();
                const auto body = emitBody(span);
                if (body && publish(span, *body)) return true;
                stubs.resize(offset);
                return false;
            };
            for (auto site = function.Instructions.begin(); site != function.Instructions.end(); ++site) {
                if (!site->second.Protect || patched.contains(site->first)) continue;
                std::vector<Instruction*> span;
                std::size_t size = 0;
                for (auto it = site; it != function.Instructions.end();) {
                    if (patched.contains(it->first) || (it != site && function.Targets.contains(it->first))) break;
                    span.push_back(&it->second);
                    size += it->second.Info.Length;
                    if (size >= 5 || terminator(it->second) || function.Indirect) break;
                    const auto next = it->first + it->second.Info.Length;
                    if (++it == function.Instructions.end() || it->first != next) break;
                }
                if (size >= 5 && emitSpan(span)) continue;
                span = {&site->second};
                size = site->second.Info.Length;
                auto it = site;
                while (size < 5 && it != function.Instructions.begin() && !function.Indirect && !function.Targets.contains(it->first)) {
                    const auto next = it->first;
                    --it;
                    if (patched.contains(it->first) || terminator(it->second) || it->first + it->second.Info.Length != next) break;
                    span.insert(span.begin(), &it->second);
                    size += it->second.Info.Length;
                }
                if (size >= 5 && emitSpan(span)) continue;
            }
            const auto consumeRelay = [&](const std::uint32_t address) {
                std::erase(relays, address);
                std::erase(shortRelays, address);
            };
            for (auto& [site, instruction] : function.Instructions) {
                if (!instruction.Protect || patched.contains(site) || function.Indirect || instruction.Info.Length < 2) continue;
                const Span siteSpan{&instruction};
                const auto stubOffset = stubs.size();
                const auto body = emitBody(siteSpan);
                if (!body) continue;
                const auto findHost = [&](const std::uint32_t from) -> std::optional<std::pair<Span, std::uint32_t>> {
                    const auto minimum = std::max<std::int64_t>(static_cast<std::int64_t>(from) - 131, 0);
                    const auto maximum = static_cast<std::int64_t>(from) + 124;
                    for (auto candidate = function.Instructions.lower_bound(static_cast<std::uint32_t>(minimum)); candidate != function.Instructions.end() && candidate->first <= maximum; ++candidate) {
                        Span span;
                        std::size_t size = 0;
                        for (auto cursor = candidate; cursor != function.Instructions.end();) {
                            if (patched.contains(cursor->first) || (cursor != candidate && function.Targets.contains(cursor->first))) break;
                            span.push_back(&cursor->second);
                            size += cursor->second.Info.Length;
                            if (size >= 10 || terminator(cursor->second)) break;
                            const auto next = cursor->first + cursor->second.Info.Length;
                            if (++cursor == function.Instructions.end() || cursor->first != next) break;
                        }
                        if (size < 10 || !(candidate->first + size <= site || candidate->first >= site + instruction.Info.Length) || !jumpFits(from, CheckedRva(candidate->first + 5), true)) continue;
                        if (const auto address = emitBody(span)) return std::pair{std::move(span), *address};
                    }
                    return std::nullopt;
                };
                std::map<std::uint32_t, std::uint32_t> parents{{site, site}};
                std::vector<std::uint32_t> queue{site};
                std::optional<std::uint32_t> relay;
                std::optional<std::pair<Span, std::uint32_t>> host;
                std::uint32_t finalJump = site;
                for (std::size_t index = 0; index < queue.size() && !relay && !host; ++index) {
                    const auto current = queue[index];
                    const auto found = std::find_if(relays.begin(), relays.end(), [&](auto address) { return address != current && jumpFits(current, address, true) && jumpFits(address, *body, false); });
                    if (found != relays.end()) { relay = *found; finalJump = current; break; }
                    host = findHost(current);
                    if (host) { finalJump = current; break; }
                    for (const auto address : shortRelays) {
                        if (!parents.contains(address) && jumpFits(current, address, true)) { parents.emplace(address, current); queue.push_back(address); }
                    }
                }
                if (!relay && !host) { stubs.resize(stubOffset); continue; }
                if (host) {
                    relay = CheckedRva(host->first.front()->Address + 5);
                    if (!jumpFits(*relay, *body, false) || !publish(host->first, host->second, false, 10)) { stubs.resize(stubOffset); continue; }
                }
                writeJump(*relay, *body, false);
                consumeRelay(*relay);
                auto destination = *relay;
                while (finalJump != site) {
                    writeJump(finalJump, destination, true);
                    consumeRelay(finalJump);
                    destination = finalJump;
                    finalJump = parents.at(finalJump);
                }
                publish(siteSpan, destination, true);
            }
            for (const auto& [address, instruction] : function.Instructions) {
                if (!instruction.Protect || patched.contains(address)) continue;
                ++result.UnrelocatableMemoryInstructionCount;
                refused(module, address, "span-or-relocation");
            }
        }
    }
    sections.push_back({".redzone", stubRva, SectionRead | SectionExecute | 0x20u, std::move(stubs)});
    nextRva = AlignRva(stubRva + sections.back().Data.size());
    report(result, module);
    return result;
}

}
