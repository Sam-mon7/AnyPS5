#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "ShaderDiskCache.hpp"
#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver::DriverDetail {

struct DriverTestAccess {
    struct Lookup {
        std::uint64_t key = 0;
        bool hit = false;
        std::shared_ptr<DrawEntry> entry;
        std::vector<std::shared_ptr<DispatchVariant>> matched{1};
        std::vector<std::vector<ShaderRecompiler::MemoryRegion>> regions{1};
    };

    static Lookup Find(const std::shared_ptr<VulkanDevice>& device, const DrawDecode& decode, const ShaderRecompiler::ShaderVertexStageInfo& vertexInfo, const Pm4::DrawParameters& packet, std::uint64_t key = 0, bool allowShaderData = false) {
        Lookup lookup;
        lookup.key = key;
        Submission submission{};
        std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>> infos{vertexInfo};
        DrawDataCandidates candidates(1);
        std::vector<StageDataHit> data(1);
        std::array<double, DrawDriverPhaseCount> ms{};
        std::chrono::steady_clock::time_point lap{};
        DrawPhaseTiming timing{false, ms, lap};
        bool verify = false;
        if (key != 0) {
            const auto found = Driver::Get().drawCache.find(key);
            if (found != Driver::Get().drawCache.end()) lookup.entry = found->second;
        }
        std::uint64_t captures = 0;
        Driver::Get().lookupDraw(submission, device, decode.state, decode.pixel, decode.programs, decode.roles, infos, true, key != 0, false, lookup.key, lookup.entry, lookup.matched, lookup.regions, lookup.hit, verify, timing, ms, nullptr, candidates, data, packet, false, allowShaderData, captures);
        Graphics::Require(!verify, "cache regression must use the ordinary hit path");
        return lookup;
    }

    static void Insert(const Lookup& lookup, const std::shared_ptr<const DrawDecode>& decode, const Pm4::DrawParameters& packet, const StageCapture& capture, const ShaderRecompiler::ShaderVertexStageInfo& vertexInfo, std::span<const Graphics::DecodeRead> decodeReads, bool allowShaderData = false) {
        std::vector<StageCapture> captures{capture};
        std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>> infos{vertexInfo};
        std::vector<std::vector<Graphics::DecodeRead>> reads(1);
        reads[0].assign(decodeReads.begin(), decodeReads.end());
        std::vector<StageDataHit> data(1);
        std::vector<std::shared_ptr<DispatchVariant>> fresh(1);
        std::array<double, DrawDriverPhaseCount> ms{};
        std::chrono::steady_clock::time_point lap{};
        DrawPhaseTiming timing{false, ms, lap};
        Driver::Get().cacheDrawStages(true, lookup.hit, packet, Graphics::IndirectDrawPath::NotFolded, decode->programs, captures, infos, reads, false, lookup.matched, data, fresh, lookup.key, false, decode, timing, std::vector<bool>(1, false), lookup.entry, 0, false, allowShaderData);
    }
};

}

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;
using Path = AgcDriver::Graphics::IndirectDrawPath;
using Rule = AgcDriver::Pm4::DrawParameters::IndirectDraw::Rule;

constexpr std::uint32_t BaseVertexSgpr = 16;
constexpr std::uint32_t StartInstanceSgpr = 17;
constexpr std::uint32_t Vertices = 3;
constexpr std::uint32_t Instances = 2;
constexpr std::uint32_t Slots = Vertices * Instances;
constexpr std::uint32_t Unwritten = 0xdeadbeefu;
constexpr std::uint32_t NoLocation = 0x280u;
constexpr std::uint32_t MergedHiddenWords = 8;

alignas(256) constexpr std::array<std::uint32_t, 30> VertexCode{
    0x7e020305, 0x7e040308,
    0xd70f6a05, 0x00020a10,
    0xd70f6a08, 0x00021011,
    0xf4080506, 0xfa000000,
    0xf4080606, 0xfa000010,
    0x02060b08,
    0xbf8cc07f,
    0xe0002000, 0x80050a03,
    0xe0002000, 0x80060b03,
    0xbf8c3f70,
    0x4a08170a, 0x34080884,
    0x7e180301, 0x7e1a0302, 0x7e1c0305, 0x7e1e0308,
    0xe0781000, 0x80020c04,
    0x7e200280, 0x7e2202f2,
    0xf80008cf, 0x11101010,
    0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 30> PostAddCode{
    0x7e020280, 0x7e040280,
    0xd70f7d05, 0x00020a10,
    0xd70f7d08, 0x00021011,
    0xf4080506, 0xfa000000,
    0xf4080606, 0xfa000010,
    0x02060b08,
    0xbf8cc07f,
    0xe0002000, 0x80050a03,
    0xe0002000, 0x80060b03,
    0xbf8c3f70,
    0x4a08170a, 0x34080884,
    0x7e180301, 0x7e1a0302, 0x7e1c0305, 0x7e1e0308,
    0xe0781000, 0x80020c04,
    0x7e200280, 0x7e2202f2,
    0xf80008cf, 0x11101010,
    0xbf810000,
};

alignas(256) constexpr auto NoCarryCode = [] {
    auto code = PostAddCode;
    code[2] = 0x4a0a0a10u;
    code[3] = 0xbf800000u;
    code[4] = 0x4a101011u;
    code[5] = 0xbf800000u;
    return code;
}();

alignas(256) constexpr auto UnreadOffsetsCode = [] {
    auto code = PostAddCode;
    for (std::size_t word = 2; word < 6; ++word) code[word] = 0xbf800000u;
    return code;
}();

alignas(256) constexpr std::array<std::uint32_t, 30> CarryCode{
    0x7e020280, 0x7e040280,
    0xd70f7d05, 0x00020a10,
    0xd70f6a08, 0x00021011,
    0xf4080506, 0xfa000000,
    0xf4080606, 0xfa000010,
    0x02060b08,
    0xbf8cc07f,
    0xe0002000, 0x80050a03,
    0xe0002000, 0x80060b03,
    0xbf8c3f70,
    0x4a08170a, 0x34080884,
    0x7e180301, 0x7e1a0302, 0x7e1c0305, 0x7e1e0308,
    0xe0781000, 0x80020c04,
    0x7e200280, 0x7e2202f2,
    0xf80008cf, 0x11101010,
    0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 30> ClampCode{
    0x7e020280, 0x7e040280,
    0xd55d8005, 0x04150010,
    0xd70f7d08, 0x00021011,
    0xf4080506, 0xfa000000,
    0xf4080606, 0xfa000010,
    0x02060b08,
    0xbf8cc07f,
    0xe0002000, 0x80050a03,
    0xe0002000, 0x80060b03,
    0xbf8c3f70,
    0x4a08170a, 0x34080884,
    0x7e180301, 0x7e1a0302, 0x7e1c0305, 0x7e1e0308,
    0xe0781000, 0x80020c04,
    0x7e200280, 0x7e2202f2,
    0xf80008cf, 0x11101010,
    0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 32> TwiceCode{
    0x7e020280, 0x7e040280,
    0xd70f7d05, 0x00020a10,
    0xd70f7d05, 0x00020a10,
    0xd70f7d08, 0x00021011,
    0xf4080506, 0xfa000000,
    0xf4080606, 0xfa000010,
    0x02060b08,
    0xbf8cc07f,
    0xe0002000, 0x80050a03,
    0xe0002000, 0x80060b03,
    0xbf8c3f70,
    0x4a08170a, 0x34080884,
    0x7e180301, 0x7e1a0302, 0x7e1c0305, 0x7e1e0308,
    0xe0781000, 0x80020c04,
    0x7e200280, 0x7e2202f2,
    0xf80008cf, 0x11101010,
    0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 30> ExecCode{
    0xbefe03c1, 0x7e040280,
    0xd70f7d05, 0x00020a10,
    0xd70f7d08, 0x00021011,
    0xf4080506, 0xfa000000,
    0xf4080606, 0xfa000010,
    0x02060b08,
    0xbf8cc07f,
    0xe0002000, 0x80050a03,
    0xe0002000, 0x80060b03,
    0xbf8c3f70,
    0x4a08170a, 0x34080884,
    0x7e180301, 0x7e1a0302, 0x7e1c0305, 0x7e1e0308,
    0xe0781000, 0x80020c04,
    0x7e200280, 0x7e2202f2,
    0xf80008cf, 0x11101010,
    0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 30> CarryWriteCode{
    0xd70f1102, 0x00020100,
    0xd70f7d05, 0x00020a10,
    0xd70f7d08, 0x00021011,
    0xf4080506, 0xfa000000,
    0xf4080606, 0xfa000010,
    0x02060b08,
    0xbf8cc07f,
    0xe0002000, 0x80050a03,
    0xe0002000, 0x80060b03,
    0xbf8c3f70,
    0x4a08170a, 0x34080884,
    0x7e180301, 0x7e1a0302, 0x7e1c0305, 0x7e1e0308,
    0xe0781000, 0x80020c04,
    0x7e200280, 0x7e2202f2,
    0xf80008cf, 0x11101010,
    0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 30> OtherVgprCode{
    0x7e020280, 0x7e040280,
    0xd70f7d00, 0x00020010,
    0xd70f7d08, 0x00021011,
    0xf4080506, 0xfa000000,
    0xf4080606, 0xfa000010,
    0x02060b08,
    0xbf8cc07f,
    0xe0002000, 0x80050a03,
    0xe0002000, 0x80060b03,
    0xbf8c3f70,
    0x4a08170a, 0x34080884,
    0x7e180301, 0x7e1a0302, 0x7e1c0305, 0x7e1e0308,
    0xe0781000, 0x80020c04,
    0x7e200280, 0x7e2202f2,
    0xf80008cf, 0x11101010,
    0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 30> SadCode{
    0x7e020280, 0x7e040280,
    0xd55d0005, 0x04150010,
    0xd70f7d08, 0x00021011,
    0xf4080506, 0xfa000000,
    0xf4080606, 0xfa000010,
    0x02060b08,
    0xbf8cc07f,
    0xe0002000, 0x80050a03,
    0xe0002000, 0x80060b03,
    0xbf8c3f70,
    0x4a08170a, 0x34080884,
    0x7e180301, 0x7e1a0302, 0x7e1c0305, 0x7e1e0308,
    0xe0781000, 0x80020c04,
    0x7e200280, 0x7e2202f2,
    0xf80008cf, 0x11101010,
    0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 30> ExecAllCode{
    0xbefe04c1, 0x7e040280,
    0xd70f7d05, 0x00020a10,
    0xd70f7d08, 0x00021011,
    0xf4080506, 0xfa000000,
    0xf4080606, 0xfa000010,
    0x02060b08,
    0xbf8cc07f,
    0xe0002000, 0x80050a03,
    0xe0002000, 0x80060b03,
    0xbf8c3f70,
    0x4a08170a, 0x34080884,
    0x7e180301, 0x7e1a0302, 0x7e1c0305, 0x7e1e0308,
    0xe0781000, 0x80020c04,
    0x7e200280, 0x7e2202f2,
    0xf80008cf, 0x11101010,
    0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 34> ReloadCode{
    0x7e020280, 0x7e040280,
    0xd70f7d05, 0x00020a10,
    0xd70f7d08, 0x00021011,
    0xf4080506, 0xfa000000,
    0xf4080606, 0xfa000010,
    0x02060b08,
    0xbf8cc07f,
    0xe0002000, 0x80050a03,
    0xe0002000, 0x80060b03,
    0xbf8c3f70,
    0x4a08170a, 0x34080884,
    0x7e180301, 0x7e1a0302, 0x7e1c0305, 0x7e1e0308,
    0xe0781000, 0x80020c04,
    0x7e200280, 0x7e2202f2,
    0xf80008cf, 0x11101010,
    0xf4040407, 0xfa000000,
    0xbf8cc07f,
    0x80121110,
    0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 33> SkippedReloadCode{
    0x7e020280, 0x7e040280,
    0xd70f7d05, 0x00020a10,
    0xd70f7d08, 0x00021011,
    0xf4080506, 0xfa000000,
    0xf4080606, 0xfa000010,
    0x02060b08,
    0xbf8cc07f,
    0xe0002000, 0x80050a03,
    0xe0002000, 0x80060b03,
    0xbf8c3f70,
    0x4a08170a, 0x34080884,
    0x7e180301, 0x7e1a0302, 0x7e1c0305, 0x7e1e0308,
    0xe0781000, 0x80020c04,
    0x7e200280, 0x7e2202f2,
    0xf80008cf, 0x11101010,
    0xbf840001,
    0xbe900380,
    0x80121110,
    0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 5> PixelCode{0x7e0002f2u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u};

alignas(256) constexpr std::array<std::uint32_t, 12> BufferBaseCode{
    0xf4080508u, 0xfa000000u, 0xbf8cc07fu, 0x7e060280u,
    0xe0002000u, 0x80050a03u, 0xbf8c3f70u,
    0x7e200210u, 0x7e2202f2u, 0xf80008cfu, 0x1110100au, 0xbf810000u,
};

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 32;
alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};
alignas(256) std::array<std::uint32_t, 4 * Slots> Output{};
alignas(256) std::array<std::uint32_t, 2048> VertexSlots{};
alignas(256) std::array<std::uint32_t, 60000> InstanceSlots{};
alignas(256) std::array<std::uint32_t, 8> BufferTable{};
alignas(256) constexpr std::array<std::uint16_t, 4> Indices{2, 0, 1, 0};

std::array<std::uint32_t, 4> SlotDescriptor(const void* data, std::uint32_t records) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), records, 0x01014facu};
}

std::array<std::uint32_t, 4> OutputDescriptor() {
    const auto address = reinterpret_cast<std::uintptr_t>(Output.data());
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), static_cast<std::uint32_t>(sizeof(Output)), 0x31016facu};
}

struct Expected {
    std::uint32_t vertex;
    std::uint32_t instance;
    std::uint32_t vertexSum;
    std::uint32_t instanceSum;
};

class Fixture {
public:
    explicit Fixture(std::shared_ptr<AgcDriver::VulkanDevice> device) : device(std::move(device)) {
        const auto vertex = SlotDescriptor(VertexSlots.data(), static_cast<std::uint32_t>(VertexSlots.size()));
        const auto instance = SlotDescriptor(InstanceSlots.data(), static_cast<std::uint32_t>(InstanceSlots.size()));
        std::copy(vertex.begin(), vertex.end(), BufferTable.begin());
        std::copy(instance.begin(), instance.end(), BufferTable.begin() + 4);
        records = static_cast<std::uint32_t*>(::operator new(RecordBytes, std::align_val_t{65536}));
        GuestAllocations::Mutation mutation;
        mutation.Add(records, RecordBytes, true, true, true);
    }

    std::vector<std::uint32_t> UserData(std::uint32_t baseVertex, std::uint32_t startInstance) const {
        std::vector<std::uint32_t> userData(10, 0u);
        const auto output = OutputDescriptor();
        std::copy(output.begin(), output.end(), userData.begin());
        const auto table = reinterpret_cast<std::uintptr_t>(BufferTable.data());
        userData[4] = static_cast<std::uint32_t>(table);
        userData[5] = static_cast<std::uint32_t>(table >> 32u);
        userData[BaseVertexSgpr - 8u] = baseVertex;
        userData[StartInstanceSgpr - 8u] = startInstance;
        return userData;
    }

    static ShaderRecompiler::ShaderVertexStageInfo VertexInfo() {
        ShaderRecompiler::ShaderVertexStageInfo info{};
        info.resourcesNum = 2;
        info.resources[0].fields = {BufferTable[0], BufferTable[1], BufferTable[2], BufferTable[3]};
        info.resources[1].fields = {BufferTable[4], BufferTable[5], BufferTable[6], BufferTable[7]};
        info.resourcesDst[0] = {10, 1, 0, 0};
        info.resourcesDst[1] = {11, 1, 1, 1};
        info.fetchAttribReg = 6;
        info.fetchBufferReg = 4;
        info.fetchEmbedded = true;
        return info;
    }

    ShaderRecompiler::RecompileResult Vertex(std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, std::int32_t vertexSgpr = BaseVertexSgpr, std::int32_t instanceSgpr = StartInstanceSgpr, bool fromBase = false) const {
        const auto info = VertexInfo();
        const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{
            {reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)},
            {reinterpret_cast<std::uintptr_t>(BufferTable.data()), std::as_bytes(std::span(BufferTable))}
        }};
        ShaderRecompiler::RecompileRequest request{
            {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
            {32, 8, userData, std::nullopt, std::nullopt, info, memory},
            device->Target(),
            {0, 0, 0, 64}
        };
        request.useCache = false;
        if (fromBase) {
            request.graphics = ShaderRecompiler::GraphicsCompileContext{};
            request.graphics->firstUserSgpr = 8;
            request.graphics->instanceFromBase = true;
            request.graphics->baseInstanceSgpr = StartInstanceSgpr;
            request.graphics->vertexFromBase = true;
            request.graphics->baseVertexSgpr = BaseVertexSgpr;
        }
        auto result = ShaderRecompiler::Recompile(request);
        Require(result.vertexOffsetSgpr == vertexSgpr && result.instanceOffsetSgpr == instanceSgpr, "the program folded vertex " + std::to_string(result.vertexOffsetSgpr) + ", instance " + std::to_string(result.instanceOffsetSgpr) + " into the draw, expected " + std::to_string(vertexSgpr) + ", " + std::to_string(instanceSgpr));
        return result;
    }

    static ShaderRecompiler::ShaderVertexStageInfo MergedVertexInfo() {
        auto info = VertexInfo();
        info.fetchAttribReg += MergedHiddenWords;
        info.fetchBufferReg += MergedHiddenWords;
        return info;
    }

    ShaderRecompiler::RecompileResult MergedVertex(std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData) const {
        const auto info = MergedVertexInfo();
        const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{
            {reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)},
            {reinterpret_cast<std::uintptr_t>(BufferTable.data()), std::as_bytes(std::span(BufferTable))}
        }};
        ShaderRecompiler::RecompileRequest request{
            {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
            {32, 0, userData, std::nullopt, std::nullopt, info, memory},
            device->Target(),
            {0, 0, 0, 64}
        };
        request.useCache = false;
        return ShaderRecompiler::Recompile(request);
    }

    AgcDriver::Graphics::State State(bool rectList = false) const {
        AgcDriver::Graphics::State state{};
        state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
        state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
        state.colors = {state.color};
        state.hasColorTarget = true;
        state.renderExtent = {Width, Height};
        state.topology = rectList ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        state.rectList = rectList;
        state.viewport = {0.0f, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0.0f, 1.0f};
        state.scissor = {{0, 0}, {Width, Height}};
        state.cullMode = VK_CULL_MODE_NONE;
        state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        state.blend.colorWriteMask = 15u;
        state.blends = {state.blend};
        return state;
    }

    void Draw(const ShaderRecompiler::RecompileResult& vertex, const AgcDriver::Pm4::DrawParameters& draw) {
        const auto vertexPush = static_cast<std::uint32_t>(vertex.pushConstants.size());
        ShaderRecompiler::ShaderPixelStageInfo pixelInfo{};
        pixelInfo.wave32 = true;
        pixelInfo.targetOutputMode[0] = 9u;
        pixelInfo.targetExportMapping.fill(0xe4u);
        ShaderRecompiler::RecompileRequest pixelRequest{{ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}}, {32u, 0u, {}, std::nullopt, pixelInfo, std::nullopt, {}}, device->Target(), PixelPushLayout(vertexPush, device->Target())};
        pixelRequest.useCache = false;
        const auto pixel = ShaderRecompiler::Recompile(pixelRequest);
        const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderStage::Vertex, &vertex, 0u}, {ShaderStage::Fragment, &pixel, PixelPushOffset(vertexPush, device->Target())}}};
        Output.fill(Unwritten);
        device->Draw(State(), draw, shaders);
        device->WaitIdle();
    }

    void Place(std::uint32_t firstVertexFetch, std::span<const std::uint32_t> fetchedVertices, std::uint32_t firstInstanceFetch) {
        VertexSlots.fill(Unwritten);
        InstanceSlots.fill(Unwritten);
        for (std::uint32_t vertex = 0; vertex < fetchedVertices.size(); ++vertex) VertexSlots[firstVertexFetch + fetchedVertices[vertex]] = vertex;
        for (std::uint32_t instance = 0; instance < Instances; ++instance) InstanceSlots[firstInstanceFetch + instance] = instance * Vertices;
    }

    std::string Check(const std::string& what, std::span<const std::uint32_t> rawVertices, std::uint32_t baseVertex, std::uint32_t startInstance, bool checkRaw) const {
        std::string failures;
        for (std::uint32_t instance = 0; instance < Instances; ++instance) {
            for (std::uint32_t vertex = 0; vertex < Vertices; ++vertex) {
                const auto slot = instance * Vertices + vertex;
                const Expected expected{rawVertices[vertex], instance, rawVertices[vertex] + baseVertex, instance + startInstance};
                const auto* actual = Output.data() + slot * 4u;
                const bool same = actual[2] == expected.vertexSum && actual[3] == expected.instanceSum && (!checkRaw || (actual[0] == expected.vertex && actual[1] == expected.instance));
                char line[320];
                if (checkRaw) std::snprintf(line, sizeof(line), "%s, vertex %u instance %u: v5 %u v8 %u, after the fetch adds %u %u; expected %u %u, %u %u\n", what.c_str(), vertex, instance, actual[0], actual[1], actual[2], actual[3], expected.vertex, expected.instance, expected.vertexSum, expected.instanceSum);
                else std::snprintf(line, sizeof(line), "%s, vertex %u instance %u: after the fetch adds %u %u; expected %u %u\n", what.c_str(), vertex, instance, actual[2], actual[3], expected.vertexSum, expected.instanceSum);
                std::cout << line;
                if (!same) failures += line;
            }
        }
        return failures;
    }

    std::uint32_t* Records() const {
        return records;
    }

    std::shared_ptr<AgcDriver::VulkanDevice> device;

private:
    static constexpr std::size_t RecordBytes = 65536;
    std::uint32_t* records = nullptr;
};

AgcDriver::DriverDetail::DrawProgram Program(const std::vector<std::uint32_t>& userData) {
    AgcDriver::DriverDetail::DrawProgram program{};
    program.firstUserSgpr = 8;
    program.userData = userData;
    return program;
}

AgcDriver::Pm4::DrawParameters FoldedDraw(const ShaderRecompiler::RecompileResult& vertex, const std::vector<std::uint32_t>& userData, AgcDriver::Pm4::DrawParameters draw) {
    AgcDriver::DriverDetail::Driver::FoldDrawOffsets(vertex, Program(userData), draw);
    return draw;
}

std::string PathName(const std::optional<Path>& path) {
    return path ? "the CPU path (" + std::string(AgcDriver::Graphics::IndirectDrawPathName(*path)) + ")" : std::string("the GPU");
}

std::string Classified(Fixture& fixture, const std::string& what, const ShaderRecompiler::RecompileResult& vertex, const std::vector<std::uint32_t>& userData, AgcDriver::Pm4::DrawParameters& draw, std::optional<Path> expectedPath, Rule expectedRule, bool rectList = false) {
    const auto path = AgcDriver::DriverDetail::Driver::ClassifyIndirectDraw(vertex, fixture.State(rectList), Program(userData), Fixture::VertexInfo(), fixture.device, draw, false);
    const auto& indirect = *draw.indirect;
    const bool rules = expectedPath || (indirect.vertexRule == expectedRule && indirect.instanceRule == expectedRule);
    std::cout << what << ": classified to " << PathName(path) << '\n';
    if (path == expectedPath && rules) return {};
    return what + ": classified to " + PathName(path) + ", expected " + PathName(expectedPath) + (expectedPath ? "" : expectedRule == Rule::InPlace ? " with both dimensions in place" : " with both dimensions constant") + "\n";
}

std::string Analysis(Fixture& fixture) {
    struct Case {
        const char* what;
        std::span<const std::uint32_t> code;
        bool vertex;
        bool instance;
    };
    const std::array<Case, 12> cases{{
        {"a program that copies v5 and v8 before its fetch adds", VertexCode, true, true},
        {"a program whose fetch adds are the only readers of v5, v8, s16 and s17", PostAddCode, false, false},
        {"a program that reads the instance add's carry", CarryCode, false, true},
        {"a program whose vertex add is a clamped v_sad_u32", ClampCode, true, false},
        {"a program that adds s16 twice", TwiceCode, true, false},
        {"a program that writes EXEC before its fetch adds", ExecCode, true, true},
        {"a program that writes another add's carry to s17 before its fetch adds", CarryWriteCode, false, true},
        {"a program whose s16 add writes v0, not v5", OtherVgprCode, true, false},
        {"a program whose vertex add is an unclamped v_sad_u32", SadCode, false, false},
        {"a program that sets every EXEC bit with s_mov_b64 before its fetch adds", ExecAllCode, false, false},
        {"a program that reloads s16 and s17 with s_load_dwordx2 from s[14:15] after its fetch adds and reads them", ReloadCode, false, false},
        {"a program that reads s16 and s17 after a branch that can skip the write of s16", SkippedReloadCode, true, true},
    }};
    const auto userData = fixture.UserData(0x51u, 0x62u);
    std::string failures;
    for (const auto& entry : cases) {
        const auto result = fixture.Vertex(entry.code, userData);
        char line[256];
        std::snprintf(line, sizeof(line), "%s: folded value observable outside the fetch add: vertex %d instance %d; expected %d %d\n", entry.what, result.vertexOffsetShared ? 1 : 0, result.instanceOffsetShared ? 1 : 0, entry.vertex ? 1 : 0, entry.instance ? 1 : 0);
        std::cout << line;
        if (result.vertexOffsetShared != entry.vertex || result.instanceOffsetShared != entry.instance) failures += line;
    }
    return failures;
}

void SameResult(const ShaderRecompiler::RecompileResult& patched, const ShaderRecompiler::RecompileResult& full) {
    std::vector<std::byte> patchedBytes, fullBytes;
    ShaderRecompiler::ShaderDiskCache::EncodeResult(patched, patchedBytes);
    ShaderRecompiler::ShaderDiskCache::EncodeResult(full, fullBytes);
    Require(patchedBytes == fullBytes && patched.variantId == full.variantId && patched.specializationId == full.specializationId && patched.cacheHit == full.cacheHit, "indirect stage differs from full materialization");
}

std::string IndirectPushPatches(Fixture& fixture, bool bufferBase) {
    using Driver = AgcDriver::DriverDetail::Driver;
    const bool enabled = std::getenv("APS5_NO_INDIRECT_PUSH_PATCH") == nullptr;
    const bool verify = std::getenv("APS5_VERIFY_INDIRECT_PUSH_PATCH") != nullptr;
    auto program = Program(fixture.UserData(0x51u, 0x62u));
    const std::span<const std::uint32_t> code = bufferBase ? std::span<const std::uint32_t>(BufferBaseCode) : std::span<const std::uint32_t>(VertexCode);
    program.binary = {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}};
    alignas(256) std::array<std::uint32_t, 8> otherTable = BufferTable;
    std::copy_n(BufferTable.begin() + 4, 4, otherTable.begin());
    const auto tableAddress = reinterpret_cast<std::uintptr_t>(BufferTable.data());
    if (bufferBase) {
        program.userData[8] = static_cast<std::uint32_t>(tableAddress);
        program.userData[9] = static_cast<std::uint32_t>(tableAddress >> 32u);
    }
    const std::array<ShaderRecompiler::MemoryRegion, 3> regions{{
        {program.binary.codeAddress, std::as_bytes(code)},
        {tableAddress, std::as_bytes(std::span(BufferTable))},
        {reinterpret_cast<std::uintptr_t>(otherTable.data()), std::as_bytes(std::span(otherTable))}
    }};
    const std::optional<ShaderRecompiler::ShaderVertexStageInfo> vertexInfo = bufferBase ? ShaderRecompiler::ShaderVertexStageInfo{} : Fixture::VertexInfo();
    ShaderRecompiler::RecompileRequest request{
        program.binary, {32, 8, program.userData, std::nullopt, std::nullopt, vertexInfo, regions}, fixture.device->Target(), {0, 0, 0, 64}
    };
    const auto handle = ShaderRecompiler::PrepareShader(request);
    Require(handle != nullptr, "indirect patch test has no prepared shader");
    AgcDriver::ShaderMemory memory(regions);
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;
    const auto materialize = [&] {
        const auto invocation = ShaderRecompiler::PreparedShaderInvocation::TryCreate(request, handle);
        Require(invocation.has_value(), "indirect patch test lost its prepared invocation");
        capture = memory.Capture(*invocation);
        return invocation->Materialize(*capture);
    };
    auto result = *materialize();
    const auto original = result;
    const auto initialCapture = capture;
    std::uint32_t materializations = 0;
    const auto refresh = [&] {
        return Driver::RefreshIndirectStage(result, capture.get(), program, vertexInfo, [&] {
            ++materializations;
            return materialize();
        });
    };
    if (bufferBase) {
        Require((capture->userDataUse.asBase & (std::uint64_t{1} << 8u)) != 0, "buffer-base test did not capture s16 as a base");
        const std::array<std::size_t, 1> patchedWords{8};
        Require(Driver::IndirectStageCacheRefusal(result, capture.get(), program, vertexInfo, patchedWords) == AgcDriver::DriverDetail::IndirectCacheRefusal::CaptureRead, "CPU indirect cache admitted a patched SRT base before its value changed");
        Require(std::count(result.shaderDataUserWords.begin(), result.shaderDataUserWords.end(), 8u) == 1, "buffer-base test did not also forward s16");
        const auto otherAddress = reinterpret_cast<std::uintptr_t>(otherTable.data());
        program.userData[8] = static_cast<std::uint32_t>(otherAddress);
        program.userData[9] = static_cast<std::uint32_t>(otherAddress >> 32u);
        Require(!refresh() && materializations == 1, "indirect buffer base did not fall back to materialization");
        SameResult(result, *materialize());
        const auto buffers = std::find_if(result.bindings.begin(), result.bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers; });
        Require(buffers != result.bindings.end() && buffers->guestDescriptor.size() >= 4 && std::equal(otherTable.begin(), otherTable.begin() + 4, buffers->guestDescriptor.begin()), "buffer-base fallback retained the old descriptor");
        std::cout << "indirect buffer-base fallback checked\n";
        return {};
    }
    Require(std::count(result.shaderDataUserWords.begin(), result.shaderDataUserWords.end(), 8u) == 1 && std::count(result.shaderDataUserWords.begin(), result.shaderDataUserWords.end(), 9u) == 1, "indirect patch test did not forward both offset registers");
    const std::array<std::array<std::uint32_t, 5>, 3> records{{
        {Vertices, Instances, 0, 700, 59455},
        {Vertices, Instances, 0, 19, 37},
        {Vertices, Instances, 0, 0x51u, 0x62u}
    }};
    for (std::size_t record = 0; record < records.size(); ++record) std::copy(records[record].begin(), records[record].end(), fixture.Records() + record * 5u);
    AgcDriver::Pm4::DrawParameters::IndirectDraw indirect{reinterpret_cast<std::uintptr_t>(fixture.Records()), 0x38u, 20u, 20u, static_cast<std::uint32_t>(records.size()), false, 0u, 0x10u, 0x11u, NoLocation, false, 0u};
    indirect.baseVertexSgpr = BaseVertexSgpr;
    indirect.startInstanceSgpr = StartInstanceSgpr;
    const auto indexAddress = reinterpret_cast<std::uintptr_t>(Indices.data());
    AgcDriver::Pm4::DrawParameters packet{indexAddress, static_cast<std::uint32_t>(Indices.size()), 2, 0, 0, true, 0, 0};
    packet.indirect = indirect;
    std::string failures = Classified(fixture, "multi-record indirect push patch", result, program.userData, packet, Path::NotFolded, Rule::Constant);
    const std::array<std::uint32_t, Vertices> raw{Indices[0], Indices[1], Indices[2]};
    for (std::uint32_t record = 0; record < indirect.count; ++record) {
        const auto arguments = AgcDriver::Pm4::ReadDrawArguments(indirect, record);
        program.userData[8] = arguments.vertexOffset;
        program.userData[9] = arguments.firstInstance;
        if (record == 0 && enabled) {
            const auto invocation = ShaderRecompiler::PreparedShaderInvocation::TryCreate(request, handle);
            Require(invocation.has_value(), "indirect eligibility test lost its prepared invocation");
            const auto full = invocation->Materialize(*memory.Capture(*invocation));
            for (std::uint32_t refusal = 0; refusal < 6; ++refusal) {
                auto candidate = result;
                auto captured = *capture;
                auto info = vertexInfo;
                if (refusal == 0) candidate.shaderDataUserWords.back() = candidate.shaderDataUserWords.front();
                if (refusal == 1) candidate.shaderDataUserWords.front() = 64;
                if (refusal == 2) candidate.pushConstants.clear();
                if (refusal == 3) captured.userDataUse.asValue |= std::uint64_t{1} << 8u;
                if (refusal == 4) captured.userDataUse.asBase |= std::uint64_t{1} << 8u;
                if (refusal == 5) info->fetchBufferReg = 8;
                std::uint32_t calls = 0;
                Require(!Driver::RefreshIndirectStage(candidate, &captured, program, info, [&] { ++calls; return full; }) && calls == 1, "unsafe indirect push mapping did not fall back");
                SameResult(candidate, *full);
            }
        }
        Require(refresh() == enabled, "forwarded indirect offsets did not select the expected patch path");
        const auto invocation = ShaderRecompiler::PreparedShaderInvocation::TryCreate(request, handle);
        Require(invocation.has_value(), "indirect comparison lost its prepared invocation");
        SameResult(result, *invocation->Materialize(*memory.Capture(*invocation)));
        const auto draw = FoldedDraw(result, program.userData, {indexAddress, arguments.count, 2, arguments.instances, 0, true, 0, 0});
        fixture.Place(arguments.vertexOffset, raw, arguments.firstInstance);
        fixture.Draw(result, draw);
        failures += fixture.Check("indirect push patch record " + std::to_string(record), raw, arguments.vertexOffset, arguments.firstInstance, true);
    }
    Require(materializations == (!enabled || verify ? records.size() : 0u), "indirect patch or verification switch used the wrong number of materializations");
    SameResult(original, *ShaderRecompiler::PreparedShaderInvocation::TryCreate(request, handle)->Materialize(*initialCapture));
    std::cout << "indirect push patch records checked: " << records.size() << ", materializations " << materializations << '\n';
    return failures;
}

std::string CachedIndirectPatches(Fixture& fixture, bool storageData = false, const std::filesystem::path& control = {}) {
    using namespace AgcDriver::DriverDetail;
    const bool cacheEnabled = std::getenv("APS5_NO_CPU_INDIRECT_DRAW_CACHE") == nullptr;
    const bool pushEnabled = std::getenv("APS5_NO_INDIRECT_PUSH_PATCH") == nullptr;
    const bool verify = std::getenv("APS5_VERIFY_INDIRECT_PUSH_PATCH") != nullptr;
    auto decode = std::make_shared<DrawDecode>();
    decode->state = fixture.State();
    decode->roles = {ShaderRecompiler::ProgramRole::Main};
    auto program = Program(fixture.UserData(0x51u, 0x62u));
    program.userDataBase = 0x8cu;
    program.binary = {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}};
    auto shader = std::make_shared<ShaderSnapshot>();
    shader->codeAddress = program.binary.codeAddress;
    shader->code.assign(VertexCode.begin(), VertexCode.end());
    program.snapshot = shader;
    decode->programs = {program};
    alignas(256) constexpr std::array<std::uint16_t, 6> indices{2, 0, 1, 1, 2, 0};
    alignas(256) std::array<std::uint32_t, VertexSlots.size()> otherVertices{};
    const auto originalTable = BufferTable;
    const auto originalInfo = Fixture::VertexInfo();
    const auto tableAddress = reinterpret_cast<std::uintptr_t>(BufferTable.data());
    AgcDriver::Pm4::DrawParameters packet{reinterpret_cast<std::uintptr_t>(indices.data()), static_cast<std::uint32_t>(indices.size()), 2, 0, 0, true, 0, 0};
    packet.indirect = AgcDriver::Pm4::DrawParameters::IndirectDraw{reinterpret_cast<std::uintptr_t>(fixture.Records()), 0x25u, 20u, 20u, 1u, false, 0u, 0x94u, 0x95u, NoLocation, false, 0u};
    packet.indirect->baseVertexSgpr = BaseVertexSgpr;
    packet.indirect->startInstanceSgpr = StartInstanceSgpr;
    std::uint64_t key = 0;
    std::uint32_t hits = 0;
    std::string failures;
    for (std::uint32_t draw = 0; draw < 7; ++draw) {
        using namespace AgcDriver::ArchB;
        if (!control.empty()) {
            std::ofstream(control, std::ios::trunc) << draw + 1u << " FIX_INDIRECT_CPU_DRAW=" << (draw != 3u ? 1 : 0) << '\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            Switches::PollControl();
        }
        const bool allowShaderData = storageData && Switches::Enabled(Feature::FixIndirectCpuDraw);
        const bool enabled = cacheEnabled && (!storageData || allowShaderData);
        const bool movedResource = draw >= 5;
        if (movedResource) {
            const auto descriptor = SlotDescriptor(otherVertices.data(), static_cast<std::uint32_t>(otherVertices.size()));
            std::copy(descriptor.begin(), descriptor.end(), BufferTable.begin());
        }
        const auto info = Fixture::VertexInfo();
        std::array<AgcDriver::Graphics::DecodeRead, 2> decodeReads{};
        for (std::size_t resource = 0; resource < decodeReads.size(); ++resource) {
            auto& read = decodeReads[resource];
            read.address = tableAddress + resource * 16u;
            read.size = 16;
            std::memcpy(read.data.data(), BufferTable.data() + resource * 4u, read.size);
        }
        auto lookup = DriverTestAccess::Find(fixture.device, *decode, originalInfo, packet, key, allowShaderData);
        key = lookup.key;
        if (storageData && lookup.hit) Require(!DriverTestAccess::Find(fixture.device, *decode, originalInfo, packet, key).hit, "switch-off lookup consumed a storage-data proof");
        Require(lookup.hit == (enabled && draw >= 2 && draw != 5), "CPU indirect cache selected the wrong hit/miss for draw " + std::to_string(draw));
        if (enabled && draw == 2) {
            auto changedPacket = packet;
            changedPacket.indirect->baseVertexLocation = 0x90u;
            Require(!DriverTestAccess::Find(fixture.device, *decode, originalInfo, changedPacket, key, allowShaderData).hit, "CPU indirect cache reused a proof for different packet SGPR locations");
        }
        hits += lookup.hit;
        const std::array<ShaderRecompiler::MemoryRegion, 2> regions{{
            {program.binary.codeAddress, std::as_bytes(std::span(VertexCode))},
            {tableAddress, std::as_bytes(std::span(BufferTable))}
        }};
        AgcDriver::ShaderMemory memory(regions);
        ShaderRecompiler::RecompileRequest request{program.binary, {32, 8, program.userData, std::nullopt, std::nullopt, info, regions}, fixture.device->Target(), {0, 0, 0, 64}};
        if (storageData) request.layout.pushConstantSizeBytes = 0;
        const auto handle = ShaderRecompiler::PrepareShader(request);
        Require(handle != nullptr, "cached indirect test has no prepared shader");
        StageCapture capture;
        const auto materialize = [&] {
            const auto invocation = ShaderRecompiler::PreparedShaderInvocation::TryCreate(request, handle);
            Require(invocation.has_value(), "cached indirect test lost its invocation");
            capture.capture = memory.Capture(*invocation);
            capture.compiled = invocation->Materialize(*capture.capture);
            return capture.compiled;
        };
        if (lookup.hit) {
            capture.compiled = lookup.matched[0]->compiled;
            capture.capture = lookup.matched[0]->indirectCapture;
            Require(capture.capture != nullptr, "CPU indirect cache lost its patch proof");
            for (const auto& read : decodeReads) {
                Require(std::any_of(lookup.matched[0]->runs.begin(), lookup.matched[0]->runs.end(), [&](const auto& run) { return run.first <= read.address && read.address + read.size <= run.second; }), "CPU indirect cache did not retain a fetch descriptor read");
            }
        } else {
            materialize();
            capture.regions = memory.TakeRecentRegions();
            capture.userDataUse = capture.capture->userDataUse;
            capture.forgetSerial = AgcDriver::GuestMemory::ForgetSerial();
            const std::array<std::size_t, 2> words{8, 9};
            Require(Driver::IndirectStageCacheRefusal(*capture.compiled, capture.capture.get(), program, info, words, allowShaderData) == (storageData && !allowShaderData ? IndirectCacheRefusal::PushMapping : IndirectCacheRefusal::None), "cached indirect offsets selected the wrong proof");
            if (storageData) {
                Require(Driver::IndirectStageCacheRefusal(*capture.compiled, capture.capture.get(), program, info, words) == IndirectCacheRefusal::PushMapping, "storage-data test did not reproduce the old refusal");
                auto malformed = *capture.compiled;
                auto binding = std::find_if(malformed.bindings.begin(), malformed.bindings.end(), [](const auto& value) { return value.role == ShaderRecompiler::DescriptorRole::ShaderData; });
                Require(binding != malformed.bindings.end(), "test shader data did not spill to storage");
                binding->guestDescriptor.pop_back();
                Require(Driver::IndirectStageCacheRefusal(malformed, capture.capture.get(), program, info, words, true) == IndirectCacheRefusal::PushMapping, "storage-data proof admitted a truncated buffer");
                malformed = *capture.compiled;
                binding = std::find_if(malformed.bindings.begin(), malformed.bindings.end(), [](const auto& value) { return value.role == ShaderRecompiler::DescriptorRole::ShaderData; });
                const auto duplicate = *binding;
                malformed.bindings.push_back(duplicate);
                Require(Driver::IndirectStageCacheRefusal(malformed, capture.capture.get(), program, info, words, true) == IndirectCacheRefusal::PushMapping, "storage-data proof admitted duplicate buffers");
            }
            if (draw == 0) {
                auto unsafe = capture;
                auto unsafeProof = std::make_shared<ShaderRecompiler::ResourceCapture>(*capture.capture);
                unsafeProof->userDataUse.asValue |= std::uint64_t{1} << 8u;
                unsafe.capture = unsafeProof;
                Require(Driver::IndirectStageCacheRefusal(*capture.compiled, unsafeProof.get(), program, info, words, storageData) == IndirectCacheRefusal::CaptureRead, "storage-data proof admitted a captured patched value");
                DriverTestAccess::Insert(lookup, decode, packet, unsafe, info, decodeReads, allowShaderData);
                DriverTestAccess::Insert(lookup, decode, packet, unsafe, info, decodeReads, allowShaderData);
                Require(!DriverTestAccess::Find(fixture.device, *decode, originalInfo, packet, key, allowShaderData).hit, "CPU indirect cache inserted a capture that reads a patched word");
                auto fetchInfo = info;
                fetchInfo.fetchBufferReg = 8;
                Require(Driver::IndirectStageCacheRefusal(*capture.compiled, capture.capture.get(), program, fetchInfo, words, storageData) == IndirectCacheRefusal::VertexDecode, "CPU indirect cache admitted a patched fetch pointer");
            }
        }
        DriverTestAccess::Insert(lookup, decode, packet, capture, info, decodeReads, allowShaderData);
        const auto kept = capture.compiled;
        auto result = *kept;
        const auto record = std::array<std::uint32_t, 5>{Vertices, Instances, draw % 2u * 3u, 19u + draw * 11u, 37u + draw * 7u};
        std::copy(record.begin(), record.end(), fixture.Records());
        const auto arguments = AgcDriver::Pm4::ReadDrawArguments(*packet.indirect, 0);
        program.userData[8] = arguments.vertexOffset;
        program.userData[9] = arguments.firstInstance;
        if (storageData) result.drawKeyMemo.words.store(std::make_shared<const std::vector<std::uint32_t>>(std::initializer_list<std::uint32_t>{0x51u, 0x62u}), std::memory_order_relaxed);
        std::uint32_t materializations = 0;
        Driver::RefreshIndirectStage(result, capture.capture.get(), program, info, [&] { ++materializations; return materialize(); }, allowShaderData);
        if (storageData) Require(result.drawKeyMemo.words.load(std::memory_order_relaxed) == nullptr, "indirect data refresh retained the previous record's resource key");
        Require(materializations == (!pushEnabled || verify || (storageData && !allowShaderData) ? 1u : 0u), "cached indirect draw unexpectedly captured its resources");
        const std::array<std::uint32_t, Vertices> raw{indices[arguments.firstVertexOrIndex], indices[arguments.firstVertexOrIndex + 1], indices[arguments.firstVertexOrIndex + 2]};
        fixture.Place(arguments.vertexOffset, raw, arguments.firstInstance);
        if (movedResource) {
            otherVertices.fill(Unwritten);
            for (std::uint32_t vertex = 0; vertex < raw.size(); ++vertex) otherVertices[arguments.vertexOffset + raw[vertex]] = vertex;
            VertexSlots.fill(Unwritten);
        }
        auto direct = FoldedDraw(result, program.userData, {packet.indexAddress + arguments.firstVertexOrIndex * packet.indexSize, arguments.count, packet.indexSize, arguments.instances, 0, true, 0, 0});
        if (storageData) {
            const auto invocation = ShaderRecompiler::PreparedShaderInvocation::TryCreate(request, handle);
            Require(invocation.has_value(), "storage-data comparison lost its invocation");
            SameResult(result, *invocation->Materialize(*memory.Capture(*invocation)));
        }
        fixture.Draw(result, direct);
        failures += fixture.Check("cached CPU indirect draw " + std::to_string(draw), raw, arguments.vertexOffset, arguments.firstInstance, true);
        if (!storageData) Require(kept->pushConstants != result.pushConstants, "cached draw retained another record's push constants");
        program.userData = decode->programs[0].userData;
    }
    BufferTable = originalTable;
    const bool enabled = cacheEnabled && (!storageData || AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixIndirectCpuDraw));
    Require(hits == (enabled ? control.empty() ? 4u : 3u : 0u), "CPU indirect cache did not exercise its expected hits");
    if (storageData) {
        const auto counters = AgcDriver::ArchB::Recorder::Collect(AgcDriver::ArchB::Recorder::Clock::now()).counters;
        for (const auto& [name, count] : counters) {
            if (name.starts_with("fix_indirect_cpu_draw_")) std::cout << name << '=' << count << '\n';
        }
        if (enabled && pushEnabled) Require(counters.contains("fix_indirect_cpu_draw_hits") && counters.at("fix_indirect_cpu_draw_hits") > 0, "storage-data refresh counter was not recorded");
        if (enabled) Require(counters.contains("fix_indirect_cpu_draw_cache_hits") && counters.at("fix_indirect_cpu_draw_cache_hits") == hits, "storage-data cache counter disagrees with reused draws");
    }
    return failures;
}

std::string CrossRegisterBuiltinOffsets(Fixture& fixture) {
    using AgcDriver::DriverDetail::Driver;
    constexpr std::array<std::uint32_t, 15> code{
        0x4a00100au, 0x7e020208u, 0x7e040308u, 0x7e060305u,
        0x34081081u, 0x4a080908u, 0x4a080905u, 0x34080884u, 0xe0781000u, 0x80030004u,
        0x7e200280u, 0x7e2202f2u, 0xf80008cfu, 0x11101010u, 0xbf810000u
    };
    std::vector<std::uint32_t> userData(8, 0u);
    userData[0] = 1234u;
    userData[2] = 0x51u;
    const auto output = OutputDescriptor();
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    ShaderRecompiler::ShaderVertexStageInfo info{};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 8, userData, std::nullopt, std::nullopt, info, {}},
        fixture.device->Target(), {0, 0, 0, 64}, ShaderRecompiler::GraphicsCompileContext{}
    };
    request.graphics->firstUserSgpr = 8;
    request.graphics->instanceFromBase = true;
    request.graphics->baseInstanceSgpr = 10;
    request.graphics->vertexFromBase = true;
    Require(Driver::IndirectBuiltinPlanSupported(request), "cross-register instance add has no complete builtin resource plan");
    const auto vertex = ShaderRecompiler::Recompile(request);
    Require(vertex.instanceOffsetSgpr == -1 && (vertex.userDataRegisterReads & 4u) == 0, "cross-register add retained a CPU instance word");
    auto ordinary = request;
    ordinary.graphics.reset();
    const auto unfolded = ShaderRecompiler::Recompile(ordinary);
    Require(unfolded.instanceOffsetSgpr == -1 && (unfolded.userDataRegisterReads & 4u) != 0, "cross-register fixture no longer reproduces the unfurled instance word");
    for (const auto firstInstance : {0u, 37u, 59455u}) {
        std::copy_n(std::array<std::uint32_t, 4>{Vertices, Instances, 29, firstInstance}.begin(), 4, fixture.Records());
        AgcDriver::Pm4::DrawParameters draw{0, Vertices, 0, Instances, 0, false, 0, 0};
        draw.indirect = AgcDriver::Pm4::DrawParameters::IndirectDraw{reinterpret_cast<std::uintptr_t>(fixture.Records()), 0x24u, 16u, 16u, 1u, false, 0u, NoLocation, 0x8eu, NoLocation, false, 0u};
        draw.indirect->startInstanceSgpr = 10;
        draw.indirect->instanceFromBase = true;
        draw.indirect->vertexFromBase = true;
        auto program = Program(userData);
        program.binary = request.shader;
        Require(!Driver::ClassifyIndirectDraw(vertex, fixture.State(), program, info, fixture.device, draw, false), "cross-register instance add did not select GPU indirect");
        fixture.Draw(vertex, draw);
        for (std::uint32_t instance = 0; instance < Instances; ++instance) {
            for (std::uint32_t index = 0; index < Vertices; ++index) {
                const std::array<std::uint32_t, 4> expected{firstInstance + instance, 1234u, instance, index};
                const auto slot = (instance * Vertices + index) * 4u;
                for (std::size_t word = 0; word < expected.size(); ++word) Require(Output[slot + word] == expected[word], "cross-register builtin slot " + std::to_string(slot) + " word " + std::to_string(word) + " read " + std::to_string(Output[slot + word]) + ", expected " + std::to_string(expected[word]));
            }
        }
    }
    std::vector<std::uint32_t> unsupported{0xf4080305u, 0xfa000000u, 0xbf8cc07fu};
    unsupported.insert(unsupported.end(), code.begin(), code.end());
    request.shader.code = unsupported;
    request.shader.codeAddress = reinterpret_cast<std::uintptr_t>(unsupported.data());
    Require(!Driver::IndirectBuiltinPlanSupported(request), "builtin instance used as an SRT pointer was admitted");
    return {};
}

std::string RelocatedImageResources(Fixture& fixture) {
    constexpr std::array<std::uint32_t, 18> code{0xf40c0106u, 0xfa000000u, 0xbf8cc07fu, 0x7e080280u, 0x343c0885u, 0x4a3c3d00u, 0x34063c84u, 0x7e3e0280u, 0x7e140280u, 0x7e160280u, 0x7e180280u, 0x7e1a0280u, 0xf0001f08u, 0x00010a1eu, 0xbf8c3f70u, 0xe0781000u, 0x80000a03u, 0xbf810000u};
    std::vector<std::uint32_t> userData(14, 0u);
    const auto output = OutputDescriptor();
    std::copy(output.begin(), output.end(), userData.begin());
    const auto descriptor = [](std::uint64_t address) {
        return std::array<std::uint32_t, 8>{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (22u << 20u), 0u, 0x90000facu, 0u, 0u, 0u, 0u};
    };
    const auto old = descriptor(reinterpret_cast<std::uintptr_t>(Pixels.data()));
    const auto live = descriptor(reinterpret_cast<std::uintptr_t>(Pixels.data() + 256u));
    alignas(256) static std::array<std::uint32_t, 8> imageTable{};
    imageTable = old;
    const auto imageRoot = reinterpret_cast<std::uintptr_t>(imageTable.data());
    userData[12] = static_cast<std::uint32_t>(imageRoot);
    userData[13] = static_cast<std::uint32_t>(imageRoot >> 32u);
    const std::array imageRegions{ShaderRecompiler::MemoryRegion{imageRoot, std::as_bytes(std::span(imageTable))}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{32, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, imageRegions}, fixture.device->Target(), {0, 0, 0, 128}
    };
    request.target.registrationModules = true;
    request.shader.stage = ShaderStage::Fragment;
    request.context.compute.reset();
    request.context.pixel = ShaderRecompiler::ShaderPixelStageInfo{};
    AgcDriver::ShaderMemory memory(imageRegions);
    AgcDriver::DriverDetail::DispatchVariant original;
    original.compiled = std::make_shared<ShaderRecompiler::RecompileResult>(ShaderRecompiler::Recompile(request));
    original.relocationCapture = memory.Capture(request);
    Require(original.compiled->runtimeImageCount != 0, "image relocation fixture has no runtime metadata");
    for (const bool changedFormat : {false, true}) {
        imageTable = live;
        if (changedFormat) imageTable[1] ^= (22u ^ 56u) << 20u;
        AgcDriver::DriverDetail::DrawProgram program{};
        program.binary = request.shader;
        program.firstUserSgpr = 0;
        program.userData = userData;
        AgcDriver::DriverDetail::DispatchVariant candidate;
        candidate.compiled = original.compiled;
        candidate.relocationCapture = original.relocationCapture;
        std::uint64_t captures = 0;
        const auto reused = AgcDriver::DriverDetail::Driver::Get().RewalkIndirectStage(candidate, program, captures);
        Require(reused == !changedFormat && captures == 1u, "image relocation did not distinguish an address from a format change");
        if (!reused) continue;
        const auto full = ShaderRecompiler::Recompile(request);
        std::vector<std::byte> patchedBytes, fullBytes;
        ShaderRecompiler::ShaderDiskCache::EncodeResult(*candidate.compiled, patchedBytes);
        ShaderRecompiler::ShaderDiskCache::EncodeResult(full, fullBytes);
        Require(patchedBytes == fullBytes, "relocated image binding or runtime metadata differs from fresh materialization");
    }
    constexpr std::array<std::uint32_t, 23> tableCode{0xbf068002u, 0xbf850013u, 0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080300u, 0xfa000020u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x93109010u, 0xf4200406u, 0x20000004u, 0x8f108510u, 0xf42c0502u, 0x20000000u, 0xf09c0f08u, 0x00450000u, 0xe0700000u, 0x80070000u, 0xbf810000u, 0xbf800000u};
    alignas(4096) static std::array<std::array<std::array<std::uint32_t, 8>, 4>, 2> heaps{};
    alignas(4096) static std::array<std::uint32_t, 12> material{};
    alignas(4096) static std::array<std::uint32_t, 16> srt{};
    for (auto& heap : heaps) heap[0] = old;
    heaps[1][0] = live;
    const auto tableBuffer = [](const void* pointer, std::uint32_t stride, std::uint32_t records) {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        return std::array<std::uint32_t, 4>{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) | (stride << 16u), records, 0x01016facu};
    };
    const auto table = tableBuffer(heaps[0].data(), 32u, 4u);
    const auto materials = tableBuffer(material.data(), 16u, 3u);
    std::copy(table.begin(), table.end(), srt.begin());
    std::copy(materials.begin(), materials.end(), srt.begin() + 8u);
    std::copy(output.begin(), output.end(), srt.begin() + 12u);
    const auto root = reinterpret_cast<std::uintptr_t>(srt.data());
    userData = {static_cast<std::uint32_t>(root), static_cast<std::uint32_t>(root >> 32u), 1u};
    request.shader = {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(tableCode.data()), tableCode, 0, {}};
    request.context.compute = compute;
    request.context.pixel.reset();
    request.context.userData = userData;
    const std::array regions{ShaderRecompiler::MemoryRegion{root, std::as_bytes(std::span(srt))}, ShaderRecompiler::MemoryRegion{reinterpret_cast<std::uintptr_t>(heaps.data()), std::as_bytes(std::span(heaps))}, ShaderRecompiler::MemoryRegion{reinterpret_cast<std::uintptr_t>(material.data()), std::as_bytes(std::span(material))}};
    request.context.memory = regions;
    AgcDriver::ShaderMemory tableMemory(regions);
    original.compiled = std::make_shared<ShaderRecompiler::RecompileResult>(ShaderRecompiler::Recompile(request));
    original.relocationCapture = tableMemory.Capture(request);
    Require(original.relocationCapture->plan->requiresSpecializationMemory && !original.relocationCapture->snapshot.tables.tables.empty(), "table relocation fixture has no image table");
    const auto moved = tableBuffer(heaps[1].data(), 32u, 4u);
    std::copy(moved.begin(), moved.end(), srt.begin());
    for (const auto change : {0u, 1u, 2u}) {
        if (change == 1u) {
            heaps[1][0][1] ^= (22u ^ 56u) << 20u;
            heaps[1][1] = old;
        }
        if (change == 2u) srt[2] = 3u;
        AgcDriver::DriverDetail::DrawProgram program{};
        program.binary = request.shader;
        program.firstUserSgpr = 0;
        program.userData = userData;
        AgcDriver::DriverDetail::DispatchVariant candidate;
        candidate.compiled = original.compiled;
        candidate.relocationCapture = original.relocationCapture;
        std::uint64_t captures = 0;
        const auto reused = AgcDriver::DriverDetail::Driver::Get().RewalkIndirectStage(candidate, program, captures);
        Require(reused == (change != 2u) && captures == 1u, "image table relocation accepted a changed extent or refused runtime table data");
        if (!reused) continue;
        const auto full = ShaderRecompiler::Recompile(request);
        std::vector<std::byte> patchedBytes, fullBytes;
        ShaderRecompiler::ShaderDiskCache::EncodeResult(*candidate.compiled, patchedBytes);
        ShaderRecompiler::ShaderDiskCache::EncodeResult(full, fullBytes);
        Require(patchedBytes == fullBytes, "relocated image table or read ranges differ from fresh materialization");
    }
    return {};
}

std::string BuiltinOffsets(Fixture& fixture) {
    const auto stale = fixture.UserData(0x51u, 0x62u);
    const std::array<std::uint32_t, Vertices> raw{2, 0, 1};
    std::string failures;
    for (const auto* code : {&VertexCode, &PostAddCode, &NoCarryCode}) {
        const auto vertex = fixture.Vertex(*code, stale, -1, -1, true);
        Require(!vertex.vertexOffsetShared && !vertex.instanceOffsetShared && !vertex.vertexOffsetConflict && !vertex.instanceOffsetConflict, "builtin offsets retained CPU folding metadata");
        for (const auto& offsets : {std::array<std::uint32_t, 2>{700, 59455}, std::array<std::uint32_t, 2>{31, 19}, std::array<std::uint32_t, 2>{0, 0}}) {
            const std::array<std::uint32_t, 4> first{Vertices, Instances, 0, offsets[0]};
            const std::array<std::uint32_t, 4> last{offsets[1], 0, 0, 0};
            const auto address = reinterpret_cast<std::uintptr_t>(fixture.Records());
            {
                std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
                Require(fixture.device->FillBuffer(address, 16, first) && fixture.device->FillBuffer(address + 16, 16, last), "could not write indirect arguments on the GPU");
                Require(AgcDriver::Graphics::Recorder::SnapshotWriteOverlaps(address, 20), "the indirect record has no pending GPU writer");
            }
            AgcDriver::Pm4::DrawParameters draw{reinterpret_cast<std::uintptr_t>(Indices.data()), Vertices, 2, 0, 0, true, 0, 0};
            draw.indirect = AgcDriver::Pm4::DrawParameters::IndirectDraw{address, 0x25u, 20u, 20u, 1u, false, 0u, 0x94u, 0x95u, NoLocation, false, 0u};
            draw.indirect->baseVertexSgpr = BaseVertexSgpr;
            draw.indirect->startInstanceSgpr = StartInstanceSgpr;
            draw.indirect->vertexFromBase = true;
            draw.indirect->instanceFromBase = true;
            failures += Classified(fixture, "GPU-written builtin offsets", vertex, stale, draw, std::nullopt, Rule::InPlace);
            fixture.Place(offsets[0], raw, offsets[1]);
            const auto before = AgcDriver::Graphics::IndirectDrawCounts()[static_cast<std::size_t>(Path::Gpu)];
            fixture.Draw(vertex, draw);
            Require(AgcDriver::Graphics::IndirectDrawCounts()[static_cast<std::size_t>(Path::Gpu)] == before + 1u, "the GPU-written builtin record fell back to CPU argument reads");
            failures += fixture.Check("GPU-written builtin offsets", raw, offsets[0], offsets[1], code == &VertexCode);
        }
    }
    const auto vertex = fixture.Vertex(VertexCode, stale, -1, -1, true);
    alignas(256) constexpr std::array<std::uint16_t, Vertices> negativeIndices{702, 700, 701};
    const std::array<std::uint32_t, 5> negative{Vertices, Instances, 0, static_cast<std::uint32_t>(-700), 7};
    std::copy(negative.begin(), negative.end(), fixture.Records());
    AgcDriver::Pm4::DrawParameters draw{reinterpret_cast<std::uintptr_t>(negativeIndices.data()), Vertices, 2, 0, 0, true, 0, 0};
    draw.indirect = AgcDriver::Pm4::DrawParameters::IndirectDraw{reinterpret_cast<std::uintptr_t>(fixture.Records()), 0x25u, 20u, 20u, 1u, false, 0u, 0x94u, 0x95u, NoLocation, false, 0u};
    draw.indirect->baseVertexSgpr = BaseVertexSgpr;
    draw.indirect->startInstanceSgpr = StartInstanceSgpr;
    draw.indirect->vertexFromBase = true;
    draw.indirect->instanceFromBase = true;
    failures += Classified(fixture, "negative builtin base vertex", vertex, stale, draw, std::nullopt, Rule::InPlace);
    fixture.Place(0, raw, 7);
    fixture.Draw(vertex, draw);
    const std::array<std::uint32_t, Vertices> negativeRaw{702, 700, 701};
    failures += fixture.Check("negative builtin base vertex", negativeRaw, negative[3], 7, true);
    draw.indirect->indexLimitKnown = false;
    failures += Classified(fixture, "builtin offsets with unknown index limit", vertex, stale, draw, Path::IndexLimitUnknown, Rule::InPlace);
    return failures;
}

std::string Run(Fixture& fixture) {
    std::string failures = Analysis(fixture);
    const std::array<std::uint32_t, Vertices> autoIndices{0, 1, 2};
    const std::array<std::uint32_t, Vertices> bufferIndices{Indices[0], Indices[1], Indices[2]};
    const auto indexAddress = reinterpret_cast<std::uintptr_t>(Indices.data());

    {
        constexpr std::uint32_t indxOffset = 5;
        constexpr std::uint32_t baseVertex = 1000;
        constexpr std::uint32_t startInstance = 40;
        const auto userData = fixture.UserData(baseVertex, startInstance);
        const auto vertex = fixture.Vertex(VertexCode, userData);
        const auto draw = FoldedDraw(vertex, userData, {0, Vertices, 0, Instances, 0, false, indxOffset, 0});
        fixture.Place(indxOffset + baseVertex, autoIndices, startInstance);
        fixture.Draw(vertex, draw);
        const std::array<std::uint32_t, Vertices> raw{indxOffset, indxOffset + 1, indxOffset + 2};
        failures += fixture.Check("direct auto draw, GE_INDX_OFFSET 5, s16 1000, s17 40", raw, baseVertex, startInstance, true);
    }
    {
        constexpr std::uint32_t baseVertex = 1000;
        constexpr std::uint32_t startInstance = 40;
        const auto userData = fixture.UserData(baseVertex, startInstance);
        const auto vertex = fixture.Vertex(VertexCode, userData);
        const auto draw = FoldedDraw(vertex, userData, {indexAddress, Vertices, 2, Instances, 0, true, 0, 0});
        fixture.Place(baseVertex, bufferIndices, startInstance);
        fixture.Draw(vertex, draw);
        failures += fixture.Check("direct indexed draw, s16 1000, s17 40", bufferIndices, baseVertex, startInstance, true);
    }

    const auto staleUserData = fixture.UserData(0x51u, 0x62u);
    const std::array<std::uint32_t, 5> indexedRecord{Vertices, Instances, 0, 700, 59455};
    std::copy(indexedRecord.begin(), indexedRecord.end(), fixture.Records());
    AgcDriver::Pm4::DrawParameters::IndirectDraw indexedIndirect{reinterpret_cast<std::uintptr_t>(fixture.Records()), 0x25u, 20u, 20u, 1u, false, 0u, 0x10u, 0x11u, NoLocation, false, 0u};
    indexedIndirect.baseVertexSgpr = static_cast<std::int32_t>(BaseVertexSgpr);
    indexedIndirect.startInstanceSgpr = static_cast<std::int32_t>(StartInstanceSgpr);
    {
        const std::string what = "indexed indirect record whose program copies v5/v8 before its fetch adds, vertex offset 700, start instance 59455";
        AgcDriver::Pm4::DrawParameters packet{indexAddress, static_cast<std::uint32_t>(Indices.size()), 2, 0, 0, true, 0, 0};
        packet.indirect = indexedIndirect;
        failures += Classified(fixture, what, fixture.Vertex(VertexCode, staleUserData), staleUserData, packet, Path::NotFolded, Rule::Constant);
        const auto arguments = AgcDriver::Pm4::ReadDrawArguments(indexedIndirect, 0);
        const auto userData = fixture.UserData(arguments.vertexOffset, arguments.firstInstance);
        const auto vertex = fixture.Vertex(VertexCode, userData);
        const auto draw = FoldedDraw(vertex, userData, {indexAddress + arguments.firstVertexOrIndex * 2u, arguments.count, 2, arguments.instances, 0, true, 0, 0});
        fixture.Place(arguments.vertexOffset, bufferIndices, arguments.firstInstance);
        fixture.Draw(vertex, draw);
        failures += fixture.Check(what + ", on the CPU path", bufferIndices, arguments.vertexOffset, arguments.firstInstance, true);
    }
    {
        const std::string what = "auto indirect record whose program copies v5/v8 before its fetch adds, start vertex 300, start instance 7";
        const std::array<std::uint32_t, 4> record{Vertices, Instances, 300, 7};
        std::copy(record.begin(), record.end(), fixture.Records() + 16);
        AgcDriver::Pm4::DrawParameters::IndirectDraw indirect{reinterpret_cast<std::uintptr_t>(fixture.Records() + 16), 0x24u, 16u, 16u, 1u, false, 0u, 0x10u, 0x11u, NoLocation, false, 0u};
        indirect.baseVertexSgpr = static_cast<std::int32_t>(BaseVertexSgpr);
        indirect.startInstanceSgpr = static_cast<std::int32_t>(StartInstanceSgpr);
        AgcDriver::Pm4::DrawParameters packet{0, 0, 0, 0, 0, false, 0, 0};
        packet.indirect = indirect;
        failures += Classified(fixture, what, fixture.Vertex(VertexCode, staleUserData), staleUserData, packet, Path::NotFolded, Rule::Constant);
        const auto arguments = AgcDriver::Pm4::ReadDrawArguments(indirect, 0);
        const auto userData = fixture.UserData(arguments.firstVertexOrIndex, arguments.firstInstance);
        const auto vertex = fixture.Vertex(VertexCode, userData);
        const auto draw = FoldedDraw(vertex, userData, {0, arguments.count, 0, arguments.instances, 0, false, indirect.indxOffset, 0});
        fixture.Place(arguments.firstVertexOrIndex, autoIndices, arguments.firstInstance);
        fixture.Draw(vertex, draw);
        failures += fixture.Check(what + ", on the CPU path", autoIndices, arguments.firstVertexOrIndex, arguments.firstInstance, true);
    }
    if (fixture.device->DrawIndirectSupport().firstInstance) {
        for (const auto* code : {&PostAddCode, &NoCarryCode}) {
            const std::string what = std::string(code == &NoCarryCode ? "no-carry " : "carry ") + "indexed indirect record whose fetch adds are the only readers of v5/v8, user data s16 0x51, s17 0x62";
            const auto vertex = fixture.Vertex(*code, staleUserData);
            AgcDriver::Pm4::DrawParameters draw{indexAddress, static_cast<std::uint32_t>(Indices.size()), 2, 0, 0, true, 0, 0};
            draw.indirect = indexedIndirect;
            failures += Classified(fixture, what, vertex, staleUserData, draw, std::nullopt, Rule::InPlace);
            fixture.Place(indexedRecord[3], bufferIndices, indexedRecord[4]);
            fixture.Draw(vertex, draw);
            failures += fixture.Check(what + ", drawn in place on the GPU", bufferIndices, indexedRecord[3], indexedRecord[4], false);
            if (code == &NoCarryCode) std::cout << "no-carry indexed indirect GPU readback checked\n";
        }
        {
            const std::string what = "indexed indirect record whose program never reads the registers its packet writes the offsets to, vertex offset 700, start instance 59455";
            const auto vertex = fixture.Vertex(UnreadOffsetsCode, staleUserData, -1, -1);
            AgcDriver::Pm4::DrawParameters draw{indexAddress, static_cast<std::uint32_t>(Indices.size()), 2, 0, 0, true, 0, 0};
            draw.indirect = indexedIndirect;
            failures += Classified(fixture, what, vertex, staleUserData, draw, std::nullopt, Rule::Constant);
            fixture.Place(0, bufferIndices, 0);
            fixture.Draw(vertex, draw);
            failures += fixture.Check(what + ", drawn on the GPU like the CPU path draws it", bufferIndices, 0, 0, false);
        }
        {
            const std::string what = "indexed indirect record of a merged program (eight hidden user words, user data from s0) that reads s10, its base vertex register";
            std::vector<std::uint32_t> mergedUserData(MergedHiddenWords, 0u);
            mergedUserData.insert(mergedUserData.end(), staleUserData.begin(), staleUserData.end());
            const auto vertex = fixture.MergedVertex(VertexCode, mergedUserData);
            AgcDriver::DriverDetail::DrawProgram program{};
            program.firstUserSgpr = 0;
            program.hiddenUserWords = MergedHiddenWords;
            program.userData = mergedUserData;
            AgcDriver::Pm4::DrawParameters draw{indexAddress, static_cast<std::uint32_t>(Indices.size()), 2, 0, 0, true, 0, 0};
            draw.indirect = indexedIndirect;
            draw.indirect->baseVertexSgpr = 10;
            draw.indirect->startInstanceLocation = NoLocation;
            draw.indirect->startInstanceSgpr = -1;
            const auto path = AgcDriver::DriverDetail::Driver::ClassifyIndirectDraw(vertex, fixture.State(), program, Fixture::MergedVertexInfo(), fixture.device, draw, false);
            std::cout << what << ": classified to " << PathName(path) << ", user register reads 0x" << std::hex << vertex.userDataRegisterReads << std::dec << '\n';
            if (!path) failures += what + ": classified to the GPU, but the program reads the register its packet writes the base vertex to\n";
        }
        {
            const std::string what = "indexed indirect record whose packet writes the base vertex over the program's vertex buffer table pointer s12";
            const auto vertex = fixture.Vertex(UnreadOffsetsCode, staleUserData, -1, -1);
            AgcDriver::Pm4::DrawParameters draw{indexAddress, static_cast<std::uint32_t>(Indices.size()), 2, 0, 0, true, 0, 0};
            draw.indirect = indexedIndirect;
            draw.indirect->baseVertexSgpr = 12;
            failures += Classified(fixture, what, vertex, staleUserData, draw, Path::NotFolded, Rule::Constant);
        }
        {
            const std::string what = "indexed indirect record whose packet names no register for its offsets, user data s16 0x51, s17 0x62";
            const auto vertex = fixture.Vertex(VertexCode, staleUserData);
            AgcDriver::Pm4::DrawParameters draw{indexAddress, static_cast<std::uint32_t>(Indices.size()), 2, 0, 0, true, 0, 0};
            draw.indirect = indexedIndirect;
            draw.indirect->baseVertexLocation = NoLocation;
            draw.indirect->startInstanceLocation = NoLocation;
            draw.indirect->baseVertexSgpr = -1;
            draw.indirect->startInstanceSgpr = -1;
            failures += Classified(fixture, what, vertex, staleUserData, draw, std::nullopt, Rule::Constant);
            fixture.Place(0x51u, bufferIndices, 0x62u);
            fixture.Draw(vertex, draw);
            failures += fixture.Check(what + ", drawn on the GPU", bufferIndices, 0x51u, 0x62u, true);
        }
        {
            const std::string what = "auto indirect rect-list record whose packet names no register for its offsets, user data s16 0x51, s17 0x62";
            const std::array<std::uint32_t, 4> record{Vertices, Instances, 300, 7};
            std::copy(record.begin(), record.end(), fixture.Records() + 32);
            const auto vertex = fixture.Vertex(VertexCode, staleUserData);
            AgcDriver::Pm4::DrawParameters draw{0, 0, 0, 0, 0, false, 0, 0};
            draw.indirect = AgcDriver::Pm4::DrawParameters::IndirectDraw{reinterpret_cast<std::uintptr_t>(fixture.Records() + 32), 0x24u, 16u, 16u, 1u, false, 0u, NoLocation, NoLocation, NoLocation, false, 0u};
            failures += Classified(fixture, what, vertex, staleUserData, draw, std::nullopt, Rule::Constant, true);
        }
    }
    return failures;
}

}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    const bool storageData = mode == "--storage-indirect-data" || mode == "--storage-indirect-toggle";
    const auto control = mode == "--storage-indirect-toggle" ? std::filesystem::temp_directory_path() / ("agc-indirect-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) : std::filesystem::path{};
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { if (!path.empty()) std::filesystem::remove(path); }
    } cleanup{control};
    if (!control.empty()) {
#ifdef _WIN32
        _putenv_s("APS5_ARCHB_CONTROL_FILE", control.string().c_str());
#else
        setenv("APS5_ARCHB_CONTROL_FILE", control.string().c_str(), 1);
#endif
    }
    if (storageData) AgcDriver::ArchB::Recorder::Boundary(true, 0, AgcDriver::ArchB::Recorder::Clock::now());
    if (mode == "--builtin-offsets") {
#ifdef _WIN32
        _putenv_s("APS5_PROFILE_DRAW", "1");
#else
        setenv("APS5_PROFILE_DRAW", "1", 1);
#endif
    }
    try {
        std::shared_ptr<AgcDriver::VulkanDevice> device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (mode == "--builtin-offsets" && (!device->DrawIndirectSupport().drawParameters || !device->DrawIndirectSupport().firstInstance)) return VulkanTestSkipped;
        Fixture fixture(device);
        const auto failures = storageData ? CachedIndirectPatches(fixture, true, control) : mode == "--relocated-image-resources" ? RelocatedImageResources(fixture) : mode == "--cross-register-builtin" ? CrossRegisterBuiltinOffsets(fixture) : mode == "--builtin-offsets" ? BuiltinOffsets(fixture) : mode == "--cpu-indirect-cache" ? CachedIndirectPatches(fixture) : mode == "--indirect-push-patch" ? IndirectPushPatches(fixture, false) : mode == "--indirect-buffer-base" ? IndirectPushPatches(fixture, true) : Run(fixture);
        if (!failures.empty()) {
            std::cerr << "vertex and instance indices differ from the guest's:\n" << failures;
            return 1;
        }
        std::cout << "draw offset index tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
