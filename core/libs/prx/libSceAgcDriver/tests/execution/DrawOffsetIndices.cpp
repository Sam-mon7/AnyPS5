#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramOptions.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "Recompiler.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "Translation/EmbeddedVertexFetch.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

alignas(256) constexpr std::array<std::uint32_t, 28> NoCarryCode = [] {
    std::array<std::uint32_t, 28> code{};
    code[0] = PostAddCode[0];
    code[1] = PostAddCode[1];
    code[2] = 0x4a0a0a10;
    code[3] = 0x4a101011;
    std::copy(PostAddCode.begin() + 6, PostAddCode.end(), code.begin() + 4);
    return code;
}();

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

alignas(256) constexpr std::array<std::uint32_t, 5> PixelCode{0x7e0002f2u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u};

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

std::array<std::uint32_t, 4> OutputDescriptor(const std::uint32_t* output) {
    const auto address = reinterpret_cast<std::uintptr_t>(output);
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
    explicit Fixture(std::shared_ptr<AgcDriver::VulkanDevice> device, bool writableSync = false) : device(std::move(device)), writableSync(writableSync) {
        const auto vertex = SlotDescriptor(VertexSlots.data(), static_cast<std::uint32_t>(VertexSlots.size()));
        const auto instance = SlotDescriptor(InstanceSlots.data(), static_cast<std::uint32_t>(InstanceSlots.size()));
        std::copy(vertex.begin(), vertex.end(), BufferTable.begin());
        std::copy(instance.begin(), instance.end(), BufferTable.begin() + 4);
        records = static_cast<std::uint32_t*>(::operator new(RecordBytes, std::align_val_t{65536}));
        GuestAllocations::Mutation mutation;
        mutation.Add(records, RecordBytes, true, true, true);
#ifdef _WIN32
        if (writableSync) {
            sharedOutput = GuestArena::GuestArenaAllocate_nid_postfix(RecordBytes, RecordBytes);
            Require(sharedOutput != nullptr, "cannot allocate synchronous draw output");
            outputSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, RecordBytes, nullptr);
            Require(outputSection != nullptr, "cannot create synchronous draw output section");
            GuestArena::GuestArenaMap_nid_postfix(sharedOutput, RecordBytes, outputSection, 0, PAGE_READWRITE);
            std::memset(sharedOutput, 0, RecordBytes);
            mutation.Add(sharedOutput, RecordBytes, true, true, true);
            output = {static_cast<std::uint32_t*>(sharedOutput), Output.size()};
        }
#endif
    }

    ~Fixture() {
#ifdef _WIN32
        if (sharedOutput != nullptr) {
            device->WaitIdle();
            GuestAllocations::Mutation().Remove(sharedOutput);
            GuestArena::GuestArenaReset_nid_postfix(sharedOutput, RecordBytes);
            GuestArena::GuestArenaRelease_nid_postfix(sharedOutput, RecordBytes);
            CloseHandle(outputSection);
        }
#endif
    }

    std::vector<std::uint32_t> UserData(std::uint32_t baseVertex, std::uint32_t startInstance) const {
        std::vector<std::uint32_t> userData(10, 0u);
        const auto descriptor = OutputDescriptor(output.data());
        std::copy(descriptor.begin(), descriptor.end(), userData.begin());
        const auto table = reinterpret_cast<std::uintptr_t>(BufferTable.data());
        userData[4] = static_cast<std::uint32_t>(table);
        userData[5] = static_cast<std::uint32_t>(table >> 32u);
        userData[BaseVertexSgpr - 8u] = baseVertex;
        userData[StartInstanceSgpr - 8u] = startInstance;
        return userData;
    }

    ShaderRecompiler::RecompileResult Vertex(std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, bool expectOffsets = true) const {
        ShaderRecompiler::ShaderVertexStageInfo info{};
        info.resourcesNum = 2;
        info.resources[0].fields = {BufferTable[0], BufferTable[1], BufferTable[2], BufferTable[3]};
        info.resources[1].fields = {BufferTable[4], BufferTable[5], BufferTable[6], BufferTable[7]};
        info.resourcesDst[0] = {10, 1, 0, 0};
        info.resourcesDst[1] = {11, 1, 1, 1};
        info.fetchAttribReg = 6;
        info.fetchBufferReg = 4;
        info.fetchEmbedded = true;
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
        auto result = ShaderRecompiler::Recompile(request);
        Require(result.vertexOffsetSgpr == (expectOffsets ? static_cast<std::int32_t>(BaseVertexSgpr) : -1) && result.instanceOffsetSgpr == (expectOffsets ? static_cast<std::int32_t>(StartInstanceSgpr) : -1), "the fetch adds of s16 and s17 did not follow the fold switch (vertex " + std::to_string(result.vertexOffsetSgpr) + ", instance " + std::to_string(result.instanceOffsetSgpr) + ")");
        return result;
    }

    AgcDriver::Graphics::State State() const {
        AgcDriver::Graphics::State state{};
        state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
        state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
        state.colors = {state.color};
        state.hasColorTarget = true;
        state.renderExtent = {Width, Height};
        state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
        std::fill(output.begin(), output.end(), Unwritten);
        device->Draw(State(), draw, shaders);
        if (!writableSync) device->WaitIdle();
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
                const auto* actual = output.data() + slot * 4u;
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

    std::uint64_t OutputAddress() const { return reinterpret_cast<std::uintptr_t>(output.data()); }

    std::shared_ptr<AgcDriver::VulkanDevice> device;

private:
    static constexpr std::size_t RecordBytes = 65536;
    std::uint32_t* records = nullptr;
    bool writableSync;
    std::span<std::uint32_t> output{Output};
#ifdef _WIN32
    void* sharedOutput = nullptr;
    HANDLE outputSection = nullptr;
#endif
};

AgcDriver::DriverDetail::DrawProgram Program(const std::vector<std::uint32_t>& userData, std::span<const std::uint32_t> code = {}) {
    AgcDriver::DriverDetail::DrawProgram program{};
    program.firstUserSgpr = 8;
    program.userData = userData;
    program.binary.code = code;
    return program;
}

AgcDriver::Pm4::DrawParameters FoldedDraw(const ShaderRecompiler::RecompileResult& vertex, const std::vector<std::uint32_t>& userData, AgcDriver::Pm4::DrawParameters draw) {
    AgcDriver::DriverDetail::Driver::FoldDrawOffsets(vertex, Program(userData), draw);
    return draw;
}

std::string PathName(const std::optional<Path>& path) {
    return path ? "the CPU path (" + std::string(AgcDriver::Graphics::IndirectDrawPathName(*path)) + ")" : std::string("the GPU");
}

std::string Classified(Fixture& fixture, const std::string& what, const ShaderRecompiler::RecompileResult& vertex, const std::vector<std::uint32_t>& userData, AgcDriver::Pm4::DrawParameters& draw, std::optional<Path> expectedPath, Rule expectedRule, std::span<const std::uint32_t> code = {}) {
    const auto path = AgcDriver::DriverDetail::Driver::ClassifyIndirectDraw(vertex, fixture.State(), Program(userData, code), fixture.device, draw, false);
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
    const std::array<Case, 8> cases{{
        {"a program that copies v5 and v8 before its fetch adds", VertexCode, true, true},
        {"a program whose fetch adds are the only readers of v5, v8, s16 and s17", PostAddCode, false, false},
        {"a program that reads the instance add's carry", CarryCode, false, true},
        {"a program whose vertex add is a clamped v_sad_u32", ClampCode, true, false},
        {"a program that adds s16 twice", TwiceCode, true, false},
        {"a program that writes EXEC before its fetch adds", ExecCode, true, true},
        {"a program that writes another add's carry to s17 before its fetch adds", CarryWriteCode, false, true},
        {"a program whose s16 add writes v0, not v5", OtherVgprCode, true, false},
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
        {
            const std::string what = "indexed indirect record whose fetch adds are the only readers of v5/v8, user data s16 0x51, s17 0x62";
            const auto vertex = fixture.Vertex(PostAddCode, staleUserData);
            AgcDriver::Pm4::DrawParameters draw{indexAddress, static_cast<std::uint32_t>(Indices.size()), 2, 0, 0, true, 0, 0};
            draw.indirect = indexedIndirect;
            failures += Classified(fixture, what, vertex, staleUserData, draw, std::nullopt, Rule::InPlace);
            fixture.Place(indexedRecord[3], bufferIndices, indexedRecord[4]);
            fixture.Draw(vertex, draw);
            failures += fixture.Check(what + ", drawn in place on the GPU", bufferIndices, indexedRecord[3], indexedRecord[4], false);
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
    }
    return failures;
}

std::string ExtendedFold(Fixture& fixture) {
    const auto* setting = std::getenv("APS5_INDIRECT_SGPR_FOLD");
    const bool enabled = setting != nullptr && std::strcmp(setting, "1") == 0;
    auto decoded = ShaderRecompiler::RdnaInstructionDecoder{}.Decode(PostAddCode);
    const auto unused = ShaderRecompiler::EmbeddedVertexFetchAnalyzer::UnusedUserSgprs(decoded);
    Require((unused & (std::uint64_t{1} << 18)) != 0 && (unused & (std::uint64_t{1} << 13)) == 0, "unused-SGPR proof missed a descriptor-base pair");
    decoded.instructions.front().op = ShaderRecompiler::RdnaOpcode::SMovrelsB32;
    Require(ShaderRecompiler::EmbeddedVertexFetchAnalyzer::UnusedUserSgprs(decoded) == 0, "unused-SGPR proof accepted relative scalar addressing");
    const std::array<std::uint32_t, Vertices> indices{Indices[0], Indices[1], Indices[2]};
    const std::array<std::uint32_t, 5> record{Vertices, Instances, 0, 700, 59455};
    std::copy(record.begin(), record.end(), fixture.Records());
    auto stale = fixture.UserData(0x51u, 0x62u);
    stale.resize(12);
    AgcDriver::Pm4::DrawParameters packet{reinterpret_cast<std::uintptr_t>(Indices.data()), static_cast<std::uint32_t>(Indices.size()), 2, 0, 0, true, 0, 0};
    packet.indirect = AgcDriver::Pm4::DrawParameters::IndirectDraw{reinterpret_cast<std::uintptr_t>(fixture.Records()), 0x25u, 20u, 20u, 1u, false, 0u, 0x10u, 0x11u, NoLocation, false, 0u};
    packet.indirect->baseVertexSgpr = BaseVertexSgpr;
    packet.indirect->startInstanceSgpr = StartInstanceSgpr;
    const auto vertex = fixture.Vertex(NoCarryCode, stale, enabled);
    std::string failures;
    if (fixture.device->DrawIndirectSupport().firstInstance) {
        failures += Classified(fixture, "no-carry integer fetch adds", vertex, stale, packet, enabled ? std::nullopt : std::optional(Path::NotFolded), Rule::InPlace, NoCarryCode);
        if (enabled) {
            fixture.Place(record[3], indices, record[4]);
            fixture.Draw(vertex, packet);
            failures += fixture.Check("no-carry adds on the GPU", indices, record[3], record[4], false);
        }
        if (enabled) {
            const auto patched = fixture.UserData(record[3], record[4]);
            const auto cpuVertex = fixture.Vertex(NoCarryCode, patched);
            fixture.Place(record[3], indices, record[4]);
            fixture.Draw(cpuVertex, FoldedDraw(cpuVertex, patched, {reinterpret_cast<std::uintptr_t>(Indices.data()), Vertices, 2, Instances, 0, true, 0, 0}));
            failures += fixture.Check("no-carry adds on the CPU", indices, record[3], record[4], false);
        }
        const auto staticVertex = fixture.Vertex(PostAddCode, stale);
        packet.indirect->baseVertexSgpr = 18;
        packet.indirect->startInstanceSgpr = 19;
        failures += Classified(fixture, "packet patches unused SGPRs", staticVertex, stale, packet, enabled ? std::nullopt : std::optional(Path::NotFolded), Rule::Constant, PostAddCode);
        if (enabled) {
            fixture.Place(0x51u, indices, 0x62u);
            fixture.Draw(staticVertex, packet);
            failures += fixture.Check("unused patched SGPRs on the GPU", indices, 0x51u, 0x62u, false);
        }
    }
    return failures;
}

std::string WritableSync(Fixture& fixture) {
#ifdef _WIN32
    using namespace AgcDriver::Graphics;
    Require(std::getenv("APS5_SYNC_DRAWS") != nullptr, "writable synchronous draw test requires APS5_SYNC_DRAWS");
    const auto before = VramWritableCounters();
    const auto descriptorsBefore = VramDescriptorCounters();
    constexpr std::uint32_t baseVertex = 1000;
    constexpr std::uint32_t startInstance = 40;
    const std::array<std::uint32_t, Vertices> indices{0, 1, 2};
    const auto userData = fixture.UserData(baseVertex, startInstance);
    const auto vertex = fixture.Vertex(VertexCode, userData);
    const auto draw = FoldedDraw(vertex, userData, {0, Vertices, 0, Instances, 0, false, 0, 0});
    fixture.Place(baseVertex, indices, startInstance);
    fixture.Draw(vertex, draw);
    const auto failures = fixture.Check("synchronous writable draw before WaitIdle", indices, baseVertex, startInstance, true);
    Require(!GuestArena::GuestArenaHasGpuOverlay_nid_no_patch(fixture.OutputAddress(), sizeof(Output)), "synchronous draw returned with protected output");
    const auto after = VramWritableCounters();
    if (VramWritable()) {
        Require(VramDescriptorCounters().mirrorRegions > descriptorsBefore.mirrorRegions, "synchronous draw did not bind its shared output mirror");
        Require(after.pagesWritten > before.pagesWritten && after.readbackBytes > before.readbackBytes, "synchronous draw did not publish its mirrored writes");
        Require(!HasVramMirrorWriters() && after.pendingPages == 0, "synchronous draw left a pending mirror writer");
    } else Require(after.pagesWritten == before.pagesWritten && after.readbackBytes == before.readbackBytes, "disabled writable draw entered mirror publication");
    fixture.device->WaitIdle();
    return failures;
#else
    return {};
#endif
}

}

int main(int argc, char** argv) {
    try {
        const bool writableSync = argc == 2 && std::strcmp(argv[1], "--vram-writable-sync") == 0;
#ifndef _WIN32
        if (writableSync) return VulkanTestSkipped;
#endif
        std::shared_ptr<AgcDriver::VulkanDevice> device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Fixture fixture(device, writableSync);
        const auto failures = writableSync ? WritableSync(fixture) : Run(fixture) + ExtendedFold(fixture);
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
