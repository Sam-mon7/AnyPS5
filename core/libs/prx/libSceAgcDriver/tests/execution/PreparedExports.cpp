#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace ShaderRecompiler;
using AgcDriver::Graphics::Require;

constexpr std::uint32_t Extent = 16;
alignas(256) std::array<std::uint32_t, Extent * Extent * 4> pixels{};
alignas(256) std::array<std::uint32_t, Extent * Extent * 4> otherPixels{};
constexpr std::array<std::array<float, 4>, 3> positions{{{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}}};
constexpr std::array<std::uint32_t, 6> vertexCode{0xe0382000u, 0x80000005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};

std::shared_ptr<const RecompileResult> Run(AgcDriver::VulkanDevice& device, bool integer, std::uint8_t rawMode = 0u, bool signedOutput = false, AgcDriver::DriverDetail::ShaderSnapshot* registered = nullptr, bool mixed = false) {
    std::cerr << "prepared exports: type=" << (!integer ? "float" : signedOutput ? "sint" : "uint") << " mode=" << static_cast<unsigned>(rawMode != 0u ? rawMode : integer ? 7u : 9u) << " shared-registration=" << (registered != nullptr) << " mixed=" << mixed << std::endl;
    const auto address = reinterpret_cast<std::uintptr_t>(positions.data());
    const std::array<std::uint32_t, 4> users{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) | (16u << 16u), 3, 0x01016facu};
    RecompileRequest vertexRequest{{ShaderStage::Vertex, 0x10000, vertexCode, 0, {}}, {32, 0, users, {}, {}, ShaderVertexStageInfo{}, {}}, device.Target(), {0, 0, 0, 128}};
    const auto vertex = Recompile(vertexRequest);
    const std::array<std::uint32_t, 4> values = registered != nullptr ? std::array<std::uint32_t, 4>{0x3f800001u, 0x40000001u, 0x40400001u, 0x40800001u} : rawMode != 0u ? std::array<std::uint32_t, 4>{0x80000001u, 0xffffffffu, 0x7fc12345u, 0xdeadbeefu} : integer ? std::array<std::uint32_t, 4>{11, 22, 33, 44} : std::array<std::uint32_t, 4>{0, 0x3e800000u, 0x3f000000u, 0x3f800000u};
    const std::array<std::uint32_t, 13> code{0x7e0002ffu, values[0], 0x7e0202ffu, values[1], 0x7e0402ffu, values[2], 0x7e0602ffu, values[3], 0xf800100fu, 0x03020100u, 0xf800183fu, 0x03020100u, 0xbf810000u};
    AgcDriver::Registers context{{0x1b6, 0x8000}, {0x1b3, 0}, {0x1b4, 0}, {0x203, 0}, {0x1c5, integer ? 0x7007u : 0x9009u}};
    if (rawMode != 0u) context[0x1c5] = rawMode | (rawMode << 12u);
    RecompileRequest request{{ShaderStage::Fragment, 0x20000, code, 0, {}}, {32, 0, {}, {}, AgcDriver::Graphics::DecodePixelStageInfo(context, {}), {}, {}}, device.Target(), PixelPushLayout(0u, device.Target())};
    const auto outputType = !integer ? ColorOutputType::Float : signedOutput ? ColorOutputType::Sint : ColorOutputType::Uint;
    if (rawMode != 0u) request.context.pixel->targetOutputType[0] = request.context.pixel->targetOutputType[3] = outputType;
    if (mixed) request.context.pixel->targetOutputType[3] = ColorOutputType::Sint;
    if (integer && rawMode == 9u && registered == nullptr && !mixed) {
        for (const auto instruction : {8u, 10u}) {
            auto compressed = code;
            compressed[instruction] |= 1u << 10u;
            auto unsupported = request;
            unsupported.shader.code = compressed;
            bool rejected = false;
            try {
                static_cast<void>(PrepareShader(unsupported));
            } catch (const std::exception& error) {
                rejected = std::string_view(error.what()).find("compressed raw32 integer fragment exports are unsupported") != std::string_view::npos;
            }
            Require(rejected, "a compressed raw32 integer export was accepted or failed for an unrelated reason");
        }
    }
    if (integer && rawMode == 0u) {
        auto compressed = code;
        compressed[8] |= 1u << 10u;
        compressed[10] |= 1u << 10u;
        auto supported = request;
        supported.shader.code = compressed;
        static_cast<void>(PrepareShader(supported));
    }
    const auto handle = PrepareShader(request);
    const auto& artifact = GetPreparedArtifact(*handle);
    std::uint32_t scalarType = 0;
    std::uint32_t uintType = 0, sintType = 0;
    unsigned extracts = 0;
    unsigned unsignedExtracts = 0, signedExtracts = 0;
    const auto& words = artifact.spirv.Words();
    for (std::size_t offset = 5; offset < words.size(); offset += words[offset] >> 16u) {
        const auto opcode = words[offset] & 0xffffu;
        if (opcode == spv::OpTypeInt && words[offset + 2] == 32) {
            if (words[offset + 3] == 0) uintType = words[offset + 1];
            else sintType = words[offset + 1];
        }
        if (integer && opcode == spv::OpTypeInt && words[offset + 2] == 32 && words[offset + 3] == (signedOutput ? 1u : 0u)) scalarType = words[offset + 1];
        if (!integer && opcode == spv::OpTypeFloat && words[offset + 2] == 32) scalarType = words[offset + 1];
        if (opcode == spv::OpVectorExtractDynamic) {
            Require(mixed ? (uintType != 0 && sintType != 0 && (words[offset + 1] == uintType || words[offset + 1] == sintType)) : (scalarType != 0 && words[offset + 1] == scalarType), "runtime export mapping uses the wrong scalar type");
            unsignedExtracts += words[offset + 1] == uintType;
            signedExtracts += words[offset + 1] == sintType;
            ++extracts;
        }
    }
    Require(extracts == 8, "fragment did not map both MRT outputs at runtime");
    Require(!mixed || (unsignedExtracts == 4 && signedExtracts == 4), "mixed MRT did not keep separate unsigned and signed mapping operations");
    AgcDriver::DriverDetail::ShaderSnapshot localSnapshot{0x20000, 0, 1, {code.begin(), code.end()}, {}};
    auto& snapshot = registered != nullptr ? *registered : localSnapshot;
    if (snapshot.code.empty()) snapshot.code.assign(code.begin(), code.end());
    Require(std::ranges::equal(snapshot.code, code), "numeric transition changed the registered shader code");
    if (snapshot.prepared->entries.empty()) snapshot.prepared->entries.push_back({0, handle});
    request.shader.code = snapshot.code;
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0, 32, 32, {}, {}};
    state.color = {reinterpret_cast<std::uintptr_t>(pixels.data()), {Extent, Extent}, integer ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(pixels), 0};
    state.color.elementBytes = 16;
    if (signedOutput) {
        state.color.format = VK_FORMAT_R32_SINT;
        state.color.elementBytes = 4;
    }
    const AgcDriver::Graphics::ColorTargetLayout firstLayout(Extent, Extent, state.color.tileMode, state.color.elementBytes);
    state.color.bytes = firstLayout.Bytes();
    Require(state.color.bytes <= sizeof(pixels), "first attachment exceeds its fixture allocation");
    auto other = state.color;
    other.address = reinterpret_cast<std::uintptr_t>(otherPixels.data());
    other.slot = 3;
    other.exportIndex = 3;
    if (mixed) {
        other.format = VK_FORMAT_R32_SINT;
        other.elementBytes = 4;
    }
    const AgcDriver::Graphics::ColorTargetLayout secondLayout(Extent, Extent, other.tileMode, other.elementBytes);
    other.bytes = secondLayout.Bytes();
    Require(other.bytes <= sizeof(otherPixels), "second attachment exceeds its fixture allocation");
    state.colors = {state.color, other};
    state.hasColorTarget = true;
    state.renderExtent = {Extent, Extent};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Extent), static_cast<float>(Extent), -static_cast<float>(Extent), 0, 1};
    state.scissor = {{0, 0}, {Extent, Extent}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends.resize(4);
    state.blends[0] = state.blends[3] = state.blend;
    const AgcDriver::Pm4::DrawParameters draw{0, 3, 0, 1, 0, false};
    const auto definedChannels = rawMode == 1u ? 1u : rawMode == 2u ? 3u : rawMode == 3u ? 9u : 15u;
    const auto writeMask = [&](std::uint32_t mapping) {
        VkColorComponentFlags mask = 0;
        for (unsigned channel = 0; channel < 4u; ++channel) {
            if ((definedChannels & (1u << ((mapping >> (channel * 2u)) & 3u))) != 0) mask |= 1u << channel;
        }
        return mask;
    };
    std::shared_ptr<const RecompileResult> first;
    for (const auto mapping : {0xe4u, 0xe5u, 0xe6u, 0xe7u, 0x1bu, 0u, 0xe4u}) {
        state.colors[0].componentMapping = mapping;
        state.colors[1].componentMapping = mapping ^ 0xffu;
        state.blends[0].colorWriteMask = writeMask(mapping) & (signedOutput ? 1u : 15u);
        state.blends[3].colorWriteMask = writeMask(mapping ^ 0xffu) & (signedOutput || mixed ? 1u : 15u);
        state.blend = state.blends[0];
        request.context.pixel = AgcDriver::Graphics::DecodePixelStageInfo(context, AgcDriver::Graphics::ExportMappings(state));
        if (rawMode != 0u) request.context.pixel->targetOutputType = AgcDriver::Graphics::ExportOutputTypes(state);
        const auto invocation = AgcDriver::DriverDetail::InvocationFor(snapshot, 0, request);
        SrtRuntime runtime{};
        const auto capture = invocation.Capture(runtime);
        const auto pixel = invocation.Materialize(*capture);
        Require(pixel->variantId == artifact.variantId && pixel->specialization.empty(), "export mapping did not materialize a prepared fragment module");
        if (!first) first = pixel;
        else if (mapping == 0xe4u) Require(pixel == first, "export mapping did not reuse the materialized result");
        else Require(pixel != first, "export mapping reused stale materialized data");
        for (const bool cached : {true, false}) {
            request.useCache = cached;
            const auto materialized = MaterializeShader(request, *capture, *handle);
            Require(materialized->spirv.data() == pixel->spirv.data() && materialized->PipelineVariantId() == pixel->PipelineVariantId(), "export mapping did not reuse its specialized module");
            if (device.Target().registrationModules) {
                Require(materialized->PipelineVariantId() == first->PipelineVariantId(), "registration modules: the export mapping specialized the fragment module");
                continue;
            }
            for (std::size_t cursor = 5; cursor < materialized->spirv.size();) {
                const auto count = materialized->spirv[cursor] >> 16u;
                Require(count != 0u && count <= materialized->spirv.size() - cursor, "invalid specialized fragment instruction");
                Require((materialized->spirv[cursor] & 0xffffu) != static_cast<std::uint32_t>(spv::OpVectorExtractDynamic), "fragment export retained a dynamic channel selection");
                cursor += count;
            }
        }
        request.useCache = true;
        const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderStage::Vertex, &vertex, 0}, {ShaderStage::Fragment, pixel.get(), PixelPushOffset(0u, device.Target())}}};
        pixels.fill(0xdeadbeef);
        otherPixels.fill(0xdeadbeef);
        device.Draw(state, draw, shaders);
        device.WaitIdle();
        auto expected = values;
        if (rawMode == 3u) expected[3] = values[1];
        const auto count = signedOutput ? Extent * Extent : pixels.size();
        for (std::size_t i = 0; i < count; ++i) {
            const auto shift = signedOutput ? 0u : (i % 4) * 2;
            const auto selected = (mapping >> shift) & 3u;
            if ((definedChannels & (1u << selected)) == 0) continue;
            const auto texel = signedOutput ? i : i / 4u;
            const auto component = signedOutput ? 0u : i % 4u;
            const auto word = firstLayout.Offset(texel % Extent, texel / Extent) / sizeof(std::uint32_t) + component;
            Require(pixels[word] == expected[selected], "fragment exported the wrong channel or changed raw integer bits at mapping " + std::to_string(mapping));
        }
        const auto otherCount = signedOutput || mixed ? Extent * Extent : otherPixels.size();
        for (std::size_t i = 0; i < otherCount; ++i) {
            const auto shift = signedOutput || mixed ? 0u : (i % 4) * 2;
            const auto selected = ((mapping ^ 0xffu) >> shift) & 3u;
            if ((definedChannels & (1u << selected)) == 0) continue;
            const auto texel = signedOutput || mixed ? i : i / 4u;
            const auto component = signedOutput || mixed ? 0u : i % 4u;
            const auto word = secondLayout.Offset(texel % Extent, texel / Extent) / sizeof(std::uint32_t) + component;
            Require(otherPixels[word] == expected[selected], "fragment used the wrong MRT mapping " + std::to_string(mapping));
        }
    }
    return first;
}

}

int main(int argc, char** argv) {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (argc == 2 && std::string_view(argv[1]) == "--raw32-integer") {
            Run(*device, true, 9u, false);
            std::cout << "raw32 integer fragment export tests passed\n";
            return 0;
        }
        Run(*device, false);
        Run(*device, true);
        for (const std::uint8_t mode : {1u, 2u, 3u, 9u}) {
            Run(*device, true, mode, false);
            Run(*device, true, mode, true);
        }
        AgcDriver::DriverDetail::ShaderSnapshot registered{0x20000, 0, 1, {}, {}};
        const auto floating = Run(*device, false, 9u, false, &registered);
        const auto unsignedResult = Run(*device, true, 9u, false, &registered);
        const auto signedResult = Run(*device, true, 9u, true, &registered);
        const auto floatingAgain = Run(*device, false, 9u, false, &registered);
        Require(floating->PipelineVariantId() != unsignedResult->PipelineVariantId() && floating->PipelineVariantId() != signedResult->PipelineVariantId() && unsignedResult->PipelineVariantId() != signedResult->PipelineVariantId(), "one registered shader reused an incompatible numeric interface");
        Require(floatingAgain == floating && registered.prepared->entries.size() == 3u, "returning to float did not reuse its original registered artifact and result");
        Run(*device, true, 9u, false, nullptr, true);
        std::cout << "prepared fragment export mapping tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
