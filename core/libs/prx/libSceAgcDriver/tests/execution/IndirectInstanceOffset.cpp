#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "Translation/EmbeddedVertexFetch.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::RdnaInstruction;
using ShaderRecompiler::RdnaInstructionFamily;
using ShaderRecompiler::RdnaOpcode;
using ShaderRecompiler::RdnaOperandKind;
using ShaderRecompiler::ShaderStage;
using Rule = AgcDriver::Pm4::DrawParameters::IndirectDraw::Rule;

constexpr std::uint32_t FirstInstance = 7;
constexpr std::uint32_t Unwritten = 0xdeadbeef;
alignas(256) constexpr std::array<std::uint32_t, 19> VertexCode{
    0xf4080304, 0xfa000000,
    0xf4080404, 0xfa000010,
    0x4a00100a,
    0xbf8cc07f,
    0xe0302000, 0x80030100,
    0x7e100280,
    0xbe8a0380,
    0xbf8c3f70,
    0x4a040b01,
    0xe0702000, 0x80040002,
    0x7e200280, 0x7e2202f2,
    0xf80008cf, 0x11101010,
    0xbf810000,
};
alignas(256) constexpr std::array<std::uint32_t, 5> PixelCode{0x7e0002f2, 0x7e020280, 0xf800180f, 0x00010100, 0xbf810000};
alignas(256) std::array<std::uint32_t, 256> InstanceSlots{};
alignas(256) std::array<std::uint32_t, 6> Output{};
alignas(256) std::array<std::uint32_t, 8> Table{};
alignas(256) std::array<std::byte, 64 * 32 * 4> Pixels{};

bool Enabled() {
    const auto* value = std::getenv("APS5_INDIRECT_SGPR_FOLD");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

void CheckPlan(const ShaderRecompiler::RdnaProgram& program, bool eligible, const char* what) {
    ShaderRecompiler::EmbeddedFetchPlan plan;
    ShaderRecompiler::EmbeddedVertexFetchAnalyzer::ExtendInstanceOffset(plan, program, 8, 3);
    const bool folded = plan.instanceOffsetSgpr == 10 && plan.instanceOffsetAddPc == 16 && !plan.instanceOffsetShared;
    Require(folded == (Enabled() && eligible), what);
}

void Analysis() {
    const auto program = ShaderRecompiler::RdnaInstructionDecoder{}.Decode(VertexCode);
    Require(program.instructions[2].op == RdnaOpcode::VAddNcU32 && program.instructions[2].destination.reg == 0, "test shader did not decode its different-destination add");
    CheckPlan(program, true, "different-destination instance add was not folded");
    const auto read = [](RdnaOperandKind kind, std::uint32_t reg) {
        RdnaInstruction inst{};
        inst.op = RdnaOpcode::VMovB32;
        inst.family = RdnaInstructionFamily::VOP1;
        inst.destination.kind = RdnaOperandKind::VectorRegister;
        inst.destination.reg = 3;
        inst.source0.kind = kind;
        inst.source0.reg = reg;
        inst.sourceCount = 1;
        return inst;
    };
    for (const auto kind : {RdnaOperandKind::ScalarRegister, RdnaOperandKind::VectorRegister}) {
        const auto inst = read(kind, kind == RdnaOperandKind::ScalarRegister ? 10 : 8);
        auto second = program;
        second.instructions.insert(second.instructions.begin() + 3, inst);
        CheckPlan(second, false, "a second read before overwrite must block the fold");
        auto prior = program;
        prior.instructions.insert(prior.instructions.begin() + 2, inst);
        CheckPlan(prior, false, "a read before the add must block the fold");
        auto overwritten = program;
        overwritten.instructions.insert(overwritten.instructions.begin() + 7, inst);
        CheckPlan(overwritten, true, "a read after overwrite must not block the fold");
    }
    auto masked = program;
    RdnaInstruction exec{};
    exec.op = RdnaOpcode::SMovB64;
    exec.family = RdnaInstructionFamily::SOP1;
    exec.destination.kind = RdnaOperandKind::ExecLo;
    masked.instructions.insert(masked.instructions.begin() + 7, exec);
    CheckPlan(masked, false, "changing EXEC after the add must block the fold");
    auto partial = program;
    partial.instructions[5].destination.explicitSdwaDst = true;
    partial.instructions[5].destination.sdwaSel = 0;
    partial.instructions.insert(partial.instructions.begin() + 7, read(RdnaOperandKind::VectorRegister, 8));
    CheckPlan(partial, false, "a partial overwrite cannot hide a second read");
    auto relative = program;
    relative.instructions[5].op = RdnaOpcode::VMovrelsB32;
    CheckPlan(relative, false, "relative addressing must block the fold");
    auto dpp = program;
    dpp.instructions[8].source0.dpp = true;
    CheckPlan(dpp, false, "a cross-lane read must block the fold");
    auto descriptor = program;
    descriptor.instructions[0].source0.reg = 10;
    CheckPlan(descriptor, false, "an SGPR pointer read must block the fold");
    auto oldValue = program;
    oldValue.instructions[0].destination.reg = 8;
    CheckPlan(oldValue, false, "an overwrite before the add must block the fold");
    auto addressPair = program;
    addressPair.instructions[4].source0.reg = 7;
    addressPair.instructions[4].offen = true;
    CheckPlan(addressPair, false, "a buffer address pair reading v8 must block the fold");
    auto tuple = program;
    auto pairRead = read(RdnaOperandKind::VectorRegister, 7);
    pairRead.is64Bit = true;
    pairRead.op = RdnaOpcode::VCvtF32F64;
    tuple.instructions.insert(tuple.instructions.begin() + 3, pairRead);
    CheckPlan(tuple, false, "a 64-bit source pair reading v8 must block the fold");
    auto scalarTuple = program;
    auto scalarPairRead = read(RdnaOperandKind::ScalarRegister, 9);
    scalarPairRead.op = RdnaOpcode::SMovB64;
    scalarPairRead.family = RdnaInstructionFamily::SOP1;
    scalarPairRead.destination.kind = RdnaOperandKind::ScalarRegister;
    scalarPairRead.destination.reg = 40;
    scalarPairRead.dataDwordCount = 2;
    scalarTuple.instructions.insert(scalarTuple.instructions.begin() + 3, scalarPairRead);
    CheckPlan(scalarTuple, false, "a scalar pair reading s10 must block the fold");
    Require((ShaderRecompiler::EmbeddedVertexFetchAnalyzer::UnusedUserSgprs(scalarTuple) & (std::uint64_t{1} << 10)) == 0, "a scalar pair was incorrectly considered unused");
    auto readModifyWrite = program;
    readModifyWrite.instructions[6].op = RdnaOpcode::SBitset0B64;
    readModifyWrite.instructions[6].dataDwordCount = 2;
    CheckPlan(readModifyWrite, false, "a scalar read-modify-write must block the fold");
}

std::array<std::uint32_t, 4> Descriptor(const void* data, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32) & 0xffff) | (4u << 16), count, 0x01014fac};
}

std::vector<std::uint32_t> UserData(std::uint32_t firstInstance) {
    const auto table = reinterpret_cast<std::uintptr_t>(Table.data());
    return {static_cast<std::uint32_t>(table), static_cast<std::uint32_t>(table >> 32), firstInstance};
}

AgcDriver::DriverDetail::DrawProgram Program(const std::vector<std::uint32_t>& userData) {
    AgcDriver::DriverDetail::DrawProgram program{};
    program.firstUserSgpr = 8;
    program.userData = userData;
    program.binary.code = VertexCode;
    return program;
}

ShaderRecompiler::RecompileResult Vertex(const std::shared_ptr<AgcDriver::VulkanDevice>& device, const std::vector<std::uint32_t>& userData, std::span<const std::uint32_t> code = VertexCode, bool eligible = true) {
    ShaderRecompiler::ShaderVertexStageInfo info{};
    const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{
        {reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)},
        {reinterpret_cast<std::uintptr_t>(Table.data()), std::as_bytes(std::span(Table))}
    }};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64, 8, userData, std::nullopt, std::nullopt, info, memory},
        device->Target(), {0, 0, 0, 64}
    };
    request.useCache = false;
    auto result = ShaderRecompiler::Recompile(request);
    Require(result.instanceOffsetSgpr == (Enabled() && eligible ? 10 : -1) && result.vertexOffsetSgpr == -1 && !result.instanceOffsetShared, "recompiled ordinary vertex shader has incorrect offset metadata");
    return result;
}

AgcDriver::Graphics::State State() {
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0, 64, 32, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {64, 32}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {64, 32};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0.0f, 32.0f, 64.0f, -32.0f, 0.0f, 1.0f};
    state.scissor = {{0, 0}, {64, 32}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    return state;
}

void Draw(const std::shared_ptr<AgcDriver::VulkanDevice>& device, const ShaderRecompiler::RecompileResult& vertex, const AgcDriver::Pm4::DrawParameters& draw) {
    const auto vertexPush = static_cast<std::uint32_t>(vertex.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo info{};
    info.wave32 = true;
    info.targetOutputMode[0] = 9;
    info.targetExportMapping.fill(0xe4);
    ShaderRecompiler::RecompileRequest request{{ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}}, {32, 0, {}, std::nullopt, info, std::nullopt, {}}, device->Target(), PixelPushLayout(vertexPush, device->Target())};
    request.useCache = false;
    const auto pixel = ShaderRecompiler::Recompile(request);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderStage::Vertex, &vertex, 0}, {ShaderStage::Fragment, &pixel, PixelPushOffset(vertexPush, device->Target())}}};
    Output.fill(Unwritten);
    device->Draw(State(), draw, shaders);
    device->WaitIdle();
    for (std::uint32_t i = 0; i < Output.size(); ++i) Require(Output[i] == FirstInstance + i / 3, "the GPU and CPU instance-offset readbacks differ at slot " + std::to_string(i));
}

void Run(const std::shared_ptr<AgcDriver::VulkanDevice>& device) {
    const auto input = Descriptor(InstanceSlots.data(), static_cast<std::uint32_t>(InstanceSlots.size()));
    const auto output = Descriptor(Output.data(), static_cast<std::uint32_t>(Output.size()));
    std::copy(input.begin(), input.end(), Table.begin());
    std::copy(output.begin(), output.end(), Table.begin() + 4);
    InstanceSlots.fill(Unwritten);
    InstanceSlots[FirstInstance] = 0;
    InstanceSlots[FirstInstance + 1] = 3;
    const auto patched = UserData(FirstInstance);
    const auto cpuVertex = Vertex(device, patched);
    AgcDriver::Pm4::DrawParameters direct{0, 3, 0, 2, 0, false, 0, 0};
    AgcDriver::DriverDetail::Driver::FoldDrawOffsets(cpuVertex, Program(patched), direct);
    Draw(device, cpuVertex, direct);
    for (const auto copy : {0x7e06020au, 0x7e060308u}) {
        std::vector<std::uint32_t> code(VertexCode.begin(), VertexCode.end());
        code.insert(code.begin() + 5, copy);
        const auto rejected = Vertex(device, patched, code, false);
        AgcDriver::Pm4::DrawParameters fallback{0, 3, 0, 2, 0, false, 0, 0};
        AgcDriver::DriverDetail::Driver::FoldDrawOffsets(rejected, Program(patched), fallback);
        Draw(device, rejected, fallback);
    }
    auto* records = static_cast<std::uint32_t*>(::operator new(65536, std::align_val_t{65536}));
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(records, 65536, true, true, true);
    }
    const std::array<std::uint32_t, 4> record{3, 2, 0, FirstInstance};
    std::copy(record.begin(), record.end(), records);
    const auto stale = UserData(81);
    const auto vertex = Vertex(device, stale);
    AgcDriver::Pm4::DrawParameters indirect{0, 0, 0, 0, 0, false, 0, 0};
    indirect.indirect = AgcDriver::Pm4::DrawParameters::IndirectDraw{reinterpret_cast<std::uintptr_t>(records), 0x24, 16, 16, 1, false, 0, 0x280, 0x8e, 0x280, false, 0};
    indirect.indirect->baseVertexSgpr = -1;
    indirect.indirect->startInstanceSgpr = 10;
    if (device->DrawIndirectSupport().firstInstance) {
        const auto path = AgcDriver::DriverDetail::Driver::ClassifyIndirectDraw(vertex, State(), Program(stale), device, indirect, false);
        Require(path == (Enabled() ? std::optional<AgcDriver::Graphics::IndirectDrawPath>{} : AgcDriver::Graphics::IndirectDrawPath::NotFolded), "packet patching s10 took the wrong path");
        if (Enabled()) {
            Require(indirect.indirect->vertexRule == Rule::Constant && indirect.indirect->instanceRule == Rule::InPlace, "different-destination offset rules are incorrect");
            Draw(device, vertex, indirect);
        }
    }
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(records);
    }
    ::operator delete(records, std::align_val_t{65536});
}

}

int main() {
    try {
        Analysis();
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(device);
        std::cout << "different-destination instance offset tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
