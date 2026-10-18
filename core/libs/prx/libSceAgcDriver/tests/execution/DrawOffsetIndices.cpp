#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
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
#include <mutex>
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

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context Context(const VulkanDevice& device) { return device.graphicsContext(); }
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

void FoldReasons(Fixture& fixture) {
    using namespace AgcDriver::DriverDetail;
    using Reason = IndirectFoldReason;
    const bool enabled = IndirectFoldReasonsEnabled();
    const auto userData = fixture.UserData(0, 0);
    auto program = Program(userData, PostAddCode);
    program.binary.codeHash = 0x100;
    auto vertex = fixture.Vertex(PostAddCode, userData);
    vertex.vertexAttributes.clear();
    vertex.vertexOffsetSgpr = BaseVertexSgpr;
    vertex.instanceOffsetSgpr = -1;
    vertex.vertexOffsetShared = vertex.instanceOffsetShared = false;
    vertex.vertexOffsetConflict = vertex.instanceOffsetConflict = false;
    vertex.shaderDataUserWords = {BaseVertexSgpr - program.firstUserSgpr};
    auto state = fixture.State();
    AgcDriver::Pm4::DrawParameters packet{};
    packet.indexed = true;
    packet.indirect = AgcDriver::Pm4::DrawParameters::IndirectDraw{};
    packet.indirect->opcode = 0x25;
    packet.indirect->count = 1;
    packet.indirect->baseVertexSgpr = BaseVertexSgpr;
    packet.indirect->startInstanceSgpr = -1;
    packet.indirect->baseVertexLocation = 0x10;
    packet.indirect->startInstanceLocation = NoLocation;
    packet.indirect->drawIndexLocation = NoLocation;
    const auto check = [&](const auto& result, const auto& graphics, auto draw, Path expectedPath, Reason first, std::uint8_t mask) {
        auto probe = draw;
        const auto plain = Driver::ClassifyIndirectDraw(result, graphics, program, fixture.device, probe, false);
        const auto before = IndirectFoldReasonCounters().packets;
        IndirectFoldDiagnostic diagnostic{};
        const auto path = Driver::ClassifyIndirectDraw(result, graphics, program, fixture.device, draw, false, enabled ? &diagnostic : nullptr);
        Require(path == expectedPath && path == plain, "fold reasons changed the fallback path");
        Require(draw.indirect->vertexRule == probe.indirect->vertexRule && draw.indirect->instanceRule == probe.indirect->instanceRule && draw.indirect->vertexConstant == probe.indirect->vertexConstant && draw.indirect->instanceConstant == probe.indirect->instanceConstant, "fold reasons changed the offset rules");
        Require(IndirectFoldReasonCounters().packets == before, "classifier probes counted a fallback");
        if (enabled) {
            Require(diagnostic.first == first && diagnostic.notFoldedMask == mask, "fold reason or simultaneous mask differs");
            Require(diagnostic.dimensions[0].k == result.vertexOffsetSgpr && diagnostic.dimensions[1].p == draw.indirect->startInstanceSgpr, "fold diagnostic lost k/p");
            Require(diagnostic.patched[0] == draw.indirect->baseVertexSgpr && diagnostic.userBase == program.firstUserSgpr, "fold diagnostic mixed absolute SGPRs with user words");
        }
        CountIndirectFold(diagnostic);
        const auto after = IndirectFoldReasonCounters();
        Require(after.packets == before + (enabled ? 1 : 0), "fallback packet was not counted exactly once");
        if (enabled) Require(after.reasons[static_cast<std::size_t>(first)].first != 0, "first fold reason was not counted");
        return diagnostic;
    };
    auto missing = vertex;
    missing.vertexOffsetSgpr = -1;
    const auto initial = check(missing, state, packet, Path::NotFolded, Reason::VertexMissing, 1);
    check(missing, state, packet, Path::NotFolded, Reason::VertexMissing, 1);
    for (std::uint64_t hash = 1; hash <= 9; ++hash) {
        program.binary.codeHash = 0x100 + hash;
        check(missing, state, packet, Path::NotFolded, Reason::VertexMissing, 1);
    }
    program.binary.codeHash = 0x100;
    if (enabled) {
        const auto bucket = IndirectFoldReasonCounters().reasons[static_cast<std::size_t>(Reason::VertexMissing)];
        Require(bucket.packets == 11 && bucket.shaders.size() == 10 && bucket.samples.size() == 8, "distinct shaders or eight-sample bound differs");
        Require(bucket.samples.front().dimensions[0].k == -1 && bucket.samples.front().dimensions[0].p == BaseVertexSgpr && bucket.samples.front().patched == initial.patched, "sample lost its rejected offset or patch set");
    }
    auto different = vertex;
    different.vertexOffsetSgpr = StartInstanceSgpr;
    check(different, state, packet, Path::NotFolded, Reason::VertexDifferent, 2);
    auto shared = vertex;
    shared.vertexOffsetShared = true;
    check(shared, state, packet, Path::NotFolded, Reason::VertexShared, 4);
    auto instancePacket = packet;
    instancePacket.indirect->baseVertexSgpr = -1;
    instancePacket.indirect->baseVertexLocation = NoLocation;
    instancePacket.indirect->startInstanceSgpr = StartInstanceSgpr;
    instancePacket.indirect->startInstanceLocation = 0x11;
    check(missing, state, instancePacket, Path::NotFolded, Reason::InstanceMissing, 8);
    auto instance = missing;
    instance.instanceOffsetSgpr = BaseVertexSgpr;
    check(instance, state, instancePacket, Path::NotFolded, Reason::InstanceDifferent, 16);
    instance.instanceOffsetSgpr = StartInstanceSgpr;
    instance.instanceOffsetShared = true;
    check(instance, state, instancePacket, Path::NotFolded, Reason::InstanceShared, 32);
    auto multiple = packet;
    multiple.indirect->startInstanceSgpr = StartInstanceSgpr;
    multiple.indirect->startInstanceLocation = 0x11;
    auto simultaneous = missing;
    simultaneous.vertexOffsetShared = simultaneous.instanceOffsetShared = true;
    check(simultaneous, state, multiple, Path::NotFolded, Reason::VertexMissing, 45);
    auto conflict = vertex;
    conflict.vertexOffsetConflict = true;
    check(conflict, state, packet, Path::FetchUnknown, Reason::VertexConflict, 0);
    conflict = instance;
    conflict.instanceOffsetConflict = true;
    check(conflict, state, instancePacket, Path::FetchUnknown, Reason::InstanceConflict, 0);
    auto geometry = state;
    geometry.stages.path = AgcDriver::Graphics::ShaderPath::Geometry;
    check(vertex, geometry, packet, Path::NonVertexPath, Reason::NonVertex, 0);
    check(simultaneous, geometry, multiple, Path::NonVertexPath, Reason::NonVertex, 45);
    geometry.rectList = true;
    check(vertex, geometry, packet, Path::NonVertexPath, Reason::NonVertex, 0);
    auto rect = state;
    rect.rectList = true;
    check(vertex, rect, packet, Path::NonVertexPath, Reason::RectList, 0);
    auto drawIndex = packet;
    drawIndex.indirect->drawIndexEnabled = true;
    drawIndex.indirect->drawIndexLocation = 0x12;
    drawIndex.indirect->drawIndexSgpr = 18;
    check(vertex, state, drawIndex, Path::DrawIndex, Reason::DrawIndex, 0);
    auto indxOffset = packet;
    indxOffset.indexed = false;
    indxOffset.indirect->indxOffset = 5;
    check(vertex, state, indxOffset, Path::IndxOffset, Reason::IndxOffset, 0);
    auto large = packet;
    large.indirect->count = 257;
    check(vertex, state, large, Path::FeatureGap, Reason::ConstantRecordLimit, 0);
    auto range = vertex;
    range.vertexAttributes.resize(1);
    range.vertexAttributes[0].resource.fields[1] = 0;
    range.vertexAttributes[0].resource.fields[2] = 0xffffffffu;
    check(range, state, packet, Path::VertexRange, Reason::VertexRange, 0);
    const auto support = fixture.device->DrawIndirectSupport();
    if (!support.count) {
        auto count = packet;
        count.indirect->countIndirect = true;
        check(vertex, state, count, Path::FeatureGap, Reason::CountUnsupported, 0);
    }
    if (!support.multi) {
        auto multiCount = packet;
        multiCount.indirect->countIndirect = true;
        multiCount.indirect->count = 2;
        check(vertex, state, multiCount, Path::FeatureGap, support.count ? Reason::MultiCountUnsupported : Reason::CountUnsupported, 0);
        if (enabled) Require(IndirectFoldReasonCounters().reasons[static_cast<std::size_t>(Reason::MultiCountUnsupported)].packets != 0, "later multi-count rejection was not counted");
    }
    if (!support.firstInstance) {
        instance.instanceOffsetShared = false;
        check(instance, state, instancePacket, Path::FeatureGap, Reason::FirstInstanceUnsupported, 0);
    }
    const auto* foldSetting = std::getenv("APS5_INDIRECT_SGPR_FOLD");
    const bool folded = foldSetting != nullptr && std::strcmp(foldSetting, "1") == 0;
    auto unused = packet;
    unused.indirect->baseVertexSgpr = 18;
    unused.indirect->baseVertexLocation = 0x12;
    if (folded) {
        IndirectFoldDiagnostic diagnostic{};
        Require(!Driver::ClassifyIndirectDraw(missing, state, program, fixture.device, unused, false, enabled ? &diagnostic : nullptr), "unused patch became a fallback");
        if (enabled) Require(diagnostic.unusedAvailable && diagnostic.dimensions[0].patchedUnused && diagnostic.notFoldedMask == 0 && !diagnostic.dimensions[0].rejected, "unused patch acquired a fold rejection");
    } else check(missing, state, unused, Path::NotFolded, Reason::VertexMissing, 1);
    if (enabled) {
        auto diagnostic = initial;
        ShaderRecompiler::SrtUserDataUse use;
        use.asBase = std::uint64_t{1} << (BaseVertexSgpr - program.firstUserSgpr);
        use.asValue = use.asBase;
        SetIndirectFoldSrt(diagnostic, 0x1234, &use);
        auto cached = initial;
        SetIndirectFoldSrt(cached, 0x1234, nullptr);
        Require(cached.srtKnown && cached.srtBase == use.asBase && cached.srtValue == use.asValue, "cache hit lost its SRT patch use");
        diagnostic.storageData = true;
        CountIndirectFold(diagnostic);
        const auto counters = IndirectFoldReasonCounters();
        std::uint64_t firstTotal = 0;
        for (const auto& reason : counters.reasons) firstTotal += reason.first;
        Require(firstTotal == counters.packets, "first-failure reasons did not partition fallback packets");
        const auto& bucket = counters.reasons[static_cast<std::size_t>(Reason::VertexMissing)];
        Require(bucket.srtBase == 1 && bucket.srtValue == 1 && bucket.storage == 1 && bucket.push > 0 && bucket.forwarded > 0, "SRT/forwarded/push/storage counters differ");
        Require(counters.notFoldedMasks[45] == 2 && counters.reasons[static_cast<std::size_t>(Reason::MultiplePatched)].packets >= 1 && counters.reasons[static_cast<std::size_t>(Reason::MultiplePatched)].first == 0, "overlapping failures or multiple-patch characteristic differs");
        for (const auto reason : {Reason::VertexMissing, Reason::VertexDifferent, Reason::VertexShared, Reason::InstanceMissing, Reason::InstanceDifferent, Reason::InstanceShared, Reason::NonVertex, Reason::RectList, Reason::DrawIndex, Reason::VertexConflict, Reason::InstanceConflict, Reason::IndxOffset, Reason::ConstantRecordLimit, Reason::VertexRange}) Require(counters.reasons[static_cast<std::size_t>(reason)].packets != 0, "a provoked rejection was not counted");
    }
    const std::unique_ptr<std::FILE, decltype(&std::fclose)> log(std::tmpfile(), &std::fclose);
    Require(log != nullptr, "cannot capture fold reasons output");
    const auto now = std::chrono::steady_clock::now();
    Require(!ReportIndirectFoldReasons(log.get(), now), "fold reasons reported before ten seconds");
    Require(ReportIndirectFoldReasons(log.get(), now + std::chrono::seconds(11)) == enabled, "fold reasons reporting switch differs");
    Require(!ReportIndirectFoldReasons(log.get(), now + std::chrono::seconds(12)), "fold reasons reported twice in ten seconds");
    std::fflush(log.get());
    Require((std::ftell(log.get()) > 0) == enabled, "disabled fold reasons printed output");
    std::rewind(log.get());
    std::string output;
    std::array<char, 4096> buffer{};
    while (const auto size = std::fread(buffer.data(), 1, buffer.size(), log.get())) output.append(buffer.data(), size);
    if (enabled) {
        Require(output.find("[indirect-fold]") != std::string::npos && output.find("reason=instance.missing") != std::string::npos && output.find("hash-or-variant=0x100 k=-1/-1 p=16/-1 patched={16,-1,-1}") != std::string::npos, "fold reason report lost counters or sample details");
        std::fwrite(output.data(), 1, output.size(), stderr);
    }
    Require(IndirectFoldReasonCounters().packets == 0, "reported fold window did not reset");
}

std::string WritableSync(Fixture& fixture) {
#ifdef _WIN32
    using namespace AgcDriver::Graphics;
    Require(std::getenv("APS5_SYNC_DRAWS") != nullptr, "writable synchronous draw test requires APS5_SYNC_DRAWS");
    if (VramWritable()) {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        const auto context = AgcDriver::VulkanDeviceTestAccess::Context(*fixture.device);
        GuestBufferMemory seed(context);
        seed.SetAddressWrites(false);
        seed.AcquireRegistered();
        seed.Upload(true);
        seed.WriteBack();
        fixture.device->WaitIdle();
        Require(HostImportFor(context, fixture.OutputAddress(), sizeof(Output)) == nullptr, "synchronous draw output was imported instead of mirrored");
    }
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
        const bool foldReasons = argc == 2 && std::strcmp(argv[1], "--indirect-fold-reasons") == 0;
        if (foldReasons) FoldReasons(fixture);
        const auto failures = foldReasons ? std::string{} : writableSync ? WritableSync(fixture) : Run(fixture) + ExtendedFold(fixture);
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
