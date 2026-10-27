#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetTransfer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 16;
constexpr std::uint32_t WaveSize = 64;
constexpr std::byte Kept{0x40};
constexpr float SurfaceDepth = 0.5f;
constexpr float Bias = 1048576.0f;
alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};
alignas(256) std::array<float, Width * Height> DepthMemory{};

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 4> PixelCode{
    0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000,
};

constexpr std::array<std::array<float, 2>, 6> Corners{{
    {1.0f, 1.0f}, {1.0f, 15.0f}, {15.0f, 1.0f},
    {17.0f, 1.0f}, {31.0f, 1.0f}, {17.0f, 15.0f},
}};

alignas(256) std::array<std::array<float, 4>, Corners.size()> Vertices{};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

void Draw(AgcDriver::VulkanDevice& device, float frontBias, float backBias) {
    Pixels.fill(Kept);
    for (std::size_t index = 0; index < Corners.size(); ++index) {
        Vertices[index] = {Corners[index][0] * 2.0f / Width - 1.0f, 1.0f - Corners[index][1] * 2.0f / Height, SurfaceDepth, 1.0f};
    }
    const auto target = device.Target();

    std::vector<std::uint32_t> vertexUserData(4, 0u);
    const auto vertexBuffer = BufferDescriptor(Vertices.data(), 16u, static_cast<std::uint32_t>(Vertices.size()));
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {WaveSize, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());

    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixel.posX = true;
    pixel.posY = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const std::vector<std::uint32_t> pixelUserData(8, 0u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {WaveSize, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, target)}
    }};

    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, WaveSize, WaveSize, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.depth = AgcDriver::Graphics::DepthTarget{reinterpret_cast<std::uintptr_t>(DepthMemory.data()), 0, {Width, Height}, VK_FORMAT_D32_SFLOAT, SurfaceDepth, 0};
    state.depthTest = true;
    state.depthWrite = false;
    state.depthCompare = VK_COMPARE_OP_LESS;
    state.depthBias = true;
    state.depthBiasConstant = frontBias;
    state.depthBiasPerFace = frontBias != backBias;
    state.backDepthBiasConstant = backBias;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Vertices.size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
    device.WaitIdle();
}

void Check(bool frontDrawn, bool backDrawn, std::string_view what) {
    for (const auto& [x, y, front] : std::array<std::tuple<std::uint32_t, std::uint32_t, bool>, 2>{{{4, 4, true}, {20, 4, false}}}) {
        const bool drawn = front ? frontDrawn : backDrawn;
        const auto expected = drawn ? std::byte{255} : Kept;
        const auto actual = Pixels[(y * Width + x) * 4u];
        Require(actual == expected, std::string(what) + ": the " + (front ? "front" : "back") + "-facing triangle at (" + std::to_string(x) + ", " + std::to_string(y) + ") is " + std::to_string(static_cast<unsigned>(actual)) + ", expected " + (drawn ? "drawn" : "depth-rejected"));
    }
}

class GuestBlock {
public:
    static constexpr std::size_t BlockBytes = 65536;

    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "cannot allocate a guest block");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() const { return block; }
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    std::uint8_t* block = nullptr;
};

void DepthDrawRecipe(AgcDriver::VulkanDevice& device) {
    constexpr std::uint32_t TargetWidth = 128;
    constexpr std::uint32_t TargetHeight = 128;
    const AgcDriver::Graphics::ColorTargetLayout colorLayout(TargetWidth, TargetHeight, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4);

    const GuestBlock guestVertices;
    const GuestBlock guestTarget;
    alignas(256) static std::array<float, TargetWidth * TargetHeight> RecipeDepthMemory{};
    alignas(256) static std::array<float, TargetWidth * TargetHeight> RecipeAlternateDepthMemory{};

    std::memset(guestTarget.Data(), static_cast<int>(Kept), GuestBlock::BlockBytes);
    RecipeDepthMemory.fill(1.0f);
    RecipeAlternateDepthMemory.fill(1.0f);

    auto* targetVertices = reinterpret_cast<std::array<float, 4>*>(guestVertices.Data());
    for (std::size_t index = 0; index < Corners.size(); ++index) {
        targetVertices[index] = {Corners[index][0] * 2.0f / TargetWidth - 1.0f, 1.0f - Corners[index][1] * 2.0f / TargetHeight, SurfaceDepth, 1.0f};
    }
    const auto target = device.Target();

    std::vector<std::uint32_t> vertexUserData(4, 0u);
    const auto vertexBuffer = BufferDescriptor(targetVertices, 16u, static_cast<std::uint32_t>(Corners.size()));
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {WaveSize, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());

    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixel.posX = true;
    pixel.posY = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const std::vector<std::uint32_t> pixelUserData(8, 0u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {WaveSize, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, target)}
    }};

    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, WaveSize, WaveSize, std::nullopt, std::nullopt};
    state.color = {guestTarget.Address(), {TargetWidth, TargetHeight}, VK_FORMAT_R8G8B8A8_UNORM, colorLayout.Bytes(), 0xe4u, AgcDriver::Graphics::ColorTileMode::RenderTarget};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.depth = AgcDriver::Graphics::DepthTarget{reinterpret_cast<std::uintptr_t>(RecipeDepthMemory.data()), 0, {TargetWidth, TargetHeight}, VK_FORMAT_D32_SFLOAT, SurfaceDepth, 0};
    state.depthTest = true;
    state.depthWrite = false;
    state.depthCompare = VK_COMPARE_OP_LESS;
    state.depthBias = true;
    state.depthBiasConstant = -Bias;
    state.depthBiasPerFace = false;
    state.backDepthBiasConstant = -Bias;
    state.renderExtent = {TargetWidth, TargetHeight};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(TargetHeight), static_cast<float>(TargetWidth), -static_cast<float>(TargetHeight), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {TargetWidth, TargetHeight}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Corners.size()), 0, 1, 0, false};

    std::shared_ptr<const AgcDriver::DrawRecipe> recipe;
    device.Draw(state, draw, shaders, {}, &recipe);
    device.WaitIdle();
    Require(recipe != nullptr, "a direct draw with depth target must produce a recipe");
    Require(recipe->hasDepth, "the recipe must record depth target presence");
    Require(recipe->targetViews.size() == 2, "the recipe must record color and depth views");

    const auto outcome = device.DrawFromRecipe(state, draw, shaders, {}, recipe);
    device.WaitIdle();
    Require(outcome == AgcDriver::RecipeOutcome::Recorded, "drawing with depth from recipe must record successfully");

    auto mutatedAddressState = state;
    mutatedAddressState.depth = AgcDriver::Graphics::DepthTarget{reinterpret_cast<std::uintptr_t>(RecipeAlternateDepthMemory.data()), 0, {TargetWidth, TargetHeight}, VK_FORMAT_D32_SFLOAT, SurfaceDepth, 0};
    const auto rejectedAddressOutcome = device.DrawFromRecipe(mutatedAddressState, draw, shaders, {}, recipe);
    Require(rejectedAddressOutcome == AgcDriver::RecipeOutcome::Rebuild, "a recipe with depth must be rejected when depth target address changes");

    auto removedDepthState = state;
    removedDepthState.depth.reset();
    const auto rejectedDepthOutcome = device.DrawFromRecipe(removedDepthState, draw, shaders, {}, recipe);
    Require(rejectedDepthOutcome == AgcDriver::RecipeOutcome::Rebuild, "a recipe with depth must be rejected when depth target is removed");

    const GuestBlock firstSource;
    const GuestBlock secondSource;
    const float firstValue = 0.25f;
    const float secondValue = 0.75f;
    std::memcpy(firstSource.Data(), &firstValue, sizeof(firstValue));
    std::memcpy(secondSource.Data(), &secondValue, sizeof(secondValue));
    alignas(256) static constexpr std::array<std::uint32_t, 8> readPixelCode{0xe0300000u, 0x80000000u, 0xbf8c3f70u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u};
    const auto firstDescriptor = BufferDescriptor(firstSource.Data(), 4u, 1u);
    const std::vector<std::uint32_t> readUserData(firstDescriptor.begin(), firstDescriptor.end());
    const std::array<ShaderRecompiler::MemoryRegion, 1> readMemory{{{reinterpret_cast<std::uintptr_t>(readPixelCode.data()), std::as_bytes(std::span(readPixelCode))}}};
    auto readRequest = fragment;
    readRequest.shader = {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(readPixelCode.data()), readPixelCode, 0, {}};
    readRequest.context.userData = readUserData;
    readRequest.context.memory = readMemory;
    const auto firstPixel = ShaderRecompiler::Recompile(readRequest);
    auto secondPixel = firstPixel;
    const auto secondDescriptor = BufferDescriptor(secondSource.Data(), 4u, 1u);
    const auto replaceBuffer = [&](auto& program, const auto& descriptor) {
        std::size_t replaced = 0;
        for (auto& binding : program.bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
            Require(binding.count == 1u && binding.guestDescriptor.size() == descriptor.size(), "the recipe regression needs one read-only buffer");
            binding.guestDescriptor.assign(descriptor.begin(), descriptor.end());
            ++replaced;
        }
        Require(replaced == 1u, "the recipe regression did not replace its buffer descriptor");
    };
    replaceBuffer(secondPixel, secondDescriptor);
    auto readShaders = shaders;
    readShaders[1].program = &firstPixel;
    auto readState = state;
    readState.depth.reset();
    readState.depthTest = false;
    readState.depthBias = false;
    std::shared_ptr<const AgcDriver::DrawRecipe> firstRecipe;
    device.Draw(readState, draw, readShaders, {}, &firstRecipe);
    device.WaitIdle();
    Require(firstRecipe != nullptr, "the first buffer draw did not create a recipe");
    readShaders[1].program = &secondPixel;
    std::shared_ptr<const AgcDriver::DrawRecipe> movedRecipe;
    device.Draw(readState, draw, readShaders, {}, &movedRecipe);
    device.WaitIdle();
    const bool movedEnabled = std::getenv("APS5_NO_DRAW_MOVED_RECIPES") == nullptr;
    Require((movedRecipe != nullptr) == movedEnabled, "moved draw recipe creation did not follow its switch");
    const auto checkRed = [&](std::uint32_t x, std::uint32_t y, unsigned expected) {
        std::vector<std::byte> texels(TargetWidth * TargetHeight * 4u);
        AgcDriver::Graphics::ReadColorTarget(readState.color, texels);
        const auto actual = static_cast<unsigned>(texels[(y * TargetWidth + x) * 4u]);
        Require(actual + 1u >= expected && actual <= expected + 1u, "a moved recipe used stale target or buffer bytes: " + std::to_string(actual) + ", expected " + std::to_string(expected));
    };
    checkRed(4, 4, 191u);
    if (!movedEnabled) return;
    Require(firstRecipe->templateRef.lock() == movedRecipe->templateRef.lock(), "the moved recipe did not reuse the original resource template");
    const auto replay = [&](const auto& selected) {
        Require(device.DrawFromRecipe(readState, draw, readShaders, {}, selected) == AgcDriver::RecipeOutcome::Recorded, "the moved draw recipe did not record");
        device.WaitIdle();
    };
    replay(movedRecipe);
    checkRed(4, 4, 191u);
    const float changedValue = 0.5f;
    AgcDriver::GuestMemory::Write(secondSource.Address(), std::as_bytes(std::span(&changedValue, 1)));
    const std::vector<std::byte> cleared(GuestBlock::BlockBytes, Kept);
    AgcDriver::GuestMemory::Write(guestTarget.Address(), cleared);
    readState.scissor.extent.width = 16;
    replay(movedRecipe);
    checkRed(4, 4, 128u);
    checkRed(20, 4, static_cast<unsigned>(Kept));
    readShaders[1].program = &firstPixel;
    replay(firstRecipe);
    checkRed(4, 4, 64u);
    readShaders[1].program = &secondPixel;
    auto* recorder = AgcDriver::Graphics::Recorder::Active();
    Require(recorder != nullptr, "the moved recipe test has no recorder");
    static_cast<void>(recorder->Commands());
    recorder->NotePendingWrite(secondSource.Address(), sizeof(float));
    Require(recorder->PendingWriteOverlaps(secondSource.Address(), sizeof(float)), "the moved recipe test did not leave a pending buffer write");
    Require(device.DrawFromRecipe(readState, draw, readShaders, {}, movedRecipe) == AgcDriver::RecipeOutcome::Rebuild, "a recipe snapshotted a moved buffer behind a pending GPU write");
    device.WaitIdle();
    auto emptyDescriptor = secondDescriptor;
    emptyDescriptor[2] = 0;
    replaceBuffer(secondPixel, emptyDescriptor);
    Require(device.DrawFromRecipe(readState, draw, readShaders, {}, movedRecipe) == AgcDriver::RecipeOutcome::Rebuild, "a recipe reused the original resource after its moved buffer was emptied");
}

}

int main(int argc, char** argv) {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (argc == 2 && std::string_view(argv[1]) == "--moved-recipes") {
            DepthDrawRecipe(*device);
            return 0;
        }
        Draw(*device, -Bias, Bias);
        Check(true, false, "front faces pulled closer, back faces pushed away");
        Draw(*device, Bias, -Bias);
        Check(false, true, "front faces pushed away, back faces pulled closer");
        Draw(*device, -Bias, -Bias);
        Check(true, true, "both faces pulled closer");
        Draw(*device, -Bias, 0.0f);
        Check(true, false, "only front faces biased");
        DepthDrawRecipe(*device);
        std::puts("per-face depth bias tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
