#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "DepthFastClearHarness.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <atomic>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string_view>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using DepthFastClearHarness::BufferDescriptor;
using DepthFastClearHarness::PixelCode;
using DepthFastClearHarness::VertexCode;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Format16Uint = 11;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t TypeCube = 11;
constexpr std::uint32_t Type2DArray = 13;
constexpr std::uint32_t Swizzle64KbS = 9;
constexpr std::uint32_t Swizzle64KbZX = 24;
constexpr std::uint64_t SliceBytes = 65536;
constexpr std::size_t BlockBytes = 2u << 20u;
constexpr std::size_t OutputOffset = 0;
constexpr std::size_t ArrayOffset = 0x10000;
constexpr std::size_t CubeOffset = 0x40000;
constexpr std::size_t IntegerOffset = 0xa0000;
constexpr std::size_t DepthBitsOffset = 0xb0000;
constexpr std::size_t OneSliceOffset = 0xd0000;
constexpr std::size_t FirstSliceOffset = 0xe0000;
constexpr std::size_t PaddedCubeOffset = 0x120000;
constexpr float DrawnDepth = 0.25f;
constexpr float DrawnD16Depth = 0.2f;
constexpr std::uint32_t DrawnD16Bits = 13107;

// image_load of row 0, one texel per thread (x = thread id), slice / face from user data, stored as a dword per
// thread into the buffer at s[0:3]. The T# is s[4:11], the slice s12.
constexpr std::array<std::uint32_t, 3> LoadDims{1u, 3u, 5u};

std::array<std::uint32_t, 11> LoadCode(std::uint32_t dim) {
    return {
        0x7e020280,                        // v_mov_b32 v1, 0 (y)
        0x7e04020c,                        // v_mov_b32 v2, s12 (slice or face)
        0xf0001100u | (dim << 3u),         // image_load v3, v[0:2], s[4:11] dmask:0x1 unorm, dim 2D / CUBE / 2D_ARRAY
        0x00010300,
        0xbf8c3f70,                        // s_waitcnt vmcnt(0)
        0x34080082,                        // v_lshlrev_b32 v4, 2, v0
        0xe0701000,                        // buffer_store_dword v3, v4, s[0:3], 0 offen
        0x80000304,
        0xbf810000,                        // s_endpgm
        0xbf810000,
        0xbf810000,
    };
}

alignas(256) std::array<std::uint32_t, 11> Load2D = LoadCode(LoadDims[0]);
alignas(256) std::array<std::uint32_t, 11> LoadCube = LoadCode(LoadDims[1]);
alignas(256) std::array<std::uint32_t, 11> LoadArray = LoadCode(LoadDims[2]);

alignas(256) std::array<std::byte, 1024 * 1024 * 4> Pixels{};

class GuestBlock {
public:
    explicit GuestBlock(std::size_t bytes = BlockBytes, std::size_t granule = 0) : bytes(bytes), granule(granule == 0 ? bytes : granule) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, this->granule);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + bytes != begin + bytes + alignment) munmap(reinterpret_cast<void*>(aligned + bytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
        }
#endif
        Require(block != nullptr, "depth plane sampling: cannot allocate the guest block");
        std::memset(block, 0, bytes);
        {
            GuestAllocations::Mutation mutation;
            for (std::size_t offset = 0; offset < bytes; offset += this->granule) mutation.Add(block + offset, std::min(this->granule, bytes - offset), true, true, true);
        }
        AgcDriver::GuestMemory::CollectWritesUncached(reinterpret_cast<std::uintptr_t>(block), bytes);
    }

    ~GuestBlock() {
        {
            GuestAllocations::Mutation mutation;
            for (std::size_t offset = 0; offset < bytes; offset += granule) mutation.Remove(block + offset);
        }
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
#else
        munmap(block, bytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, bytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* At(std::size_t offset) { return block + offset; }

private:
    const std::size_t bytes;
    const std::size_t granule;
    std::uint8_t* block = nullptr;
};

struct Shape {
    std::uint32_t format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t type;
    std::uint32_t lastArray;
    std::uint32_t swizzle;
    std::uint32_t baseArray = 0;
};

std::array<std::uint32_t, 8> TextureDescriptor(const void* data, const Shape& shape) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (shape.format << 20u) | (((shape.width - 1u) & 3u) << 30u),
        ((shape.width - 1u) >> 2u) | ((shape.height - 1u) << 14u),
        0xfacu | (shape.swizzle << 20u) | (shape.type << 28u),
        shape.lastArray | (shape.baseArray << 16u),
        0u, 0u, 0u,
    };
}

std::uint32_t Bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Draws a full-target triangle at depth `z` into the depth surface at `address`, which makes the driver create the
// surface (the base slice a later plane copy reads from the surface instead of from guest memory).
void DrawDepth(AgcDriver::VulkanDevice& device, std::uint64_t address, VkExtent2D extent, VkFormat format, float z) {
    static std::deque<std::vector<std::array<float, 4>>> vertexData;
    vertexData.push_back({{-1.0f, -1.0f, z, 1.0f}, {3.0f, -1.0f, z, 1.0f}, {-1.0f, 3.0f, z, 1.0f}});
    const auto& vertices = vertexData.back();
    const auto target = device.Target();
    std::vector<std::uint32_t> vertexUserData(4, 0u);
    const auto vertexBuffer = BufferDescriptor(vertices.data(), 16u, static_cast<std::uint32_t>(vertices.size()));
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertexRequest{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {32, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertexRequest.useCache = false;
    const auto vertex = ShaderRecompiler::Recompile(vertexRequest);
    const auto vertexPush = static_cast<std::uint32_t>(vertex.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo pixelInfo{};
    pixelInfo.wave32 = true;
    pixelInfo.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixelInfo.posX = true;
    pixelInfo.posY = true;
    pixelInfo.targetOutputMode[0] = 9;
    pixelInfo.targetExportMapping.fill(0xe4u);
    std::vector<std::uint32_t> pixelUserData(4, 0u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragmentRequest{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {32, 0, pixelUserData, std::nullopt, pixelInfo, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragmentRequest.useCache = false;
    const auto pixel = ShaderRecompiler::Recompile(fragmentRequest);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertex, 0},
        {ShaderStage::Fragment, &pixel, PixelPushOffset(vertexPush, target)}
    }};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), extent, VK_FORMAT_R8G8B8A8_UNORM, static_cast<std::size_t>(extent.width) * extent.height * 4u, 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.depth = AgcDriver::Graphics::DepthTarget{address, 0, extent, format, 1.0f, 0};
    state.depthTest = true;
    state.depthWrite = true;
    state.depthCompare = VK_COMPARE_OP_ALWAYS;
    state.renderExtent = extent;
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(extent.height), static_cast<float>(extent.width), -static_cast<float>(extent.height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, extent};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(vertices.size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
    device.WaitIdle();
}

// Loads row 0 of `slice` of the texture `shape` describes at `texels` into `output` (one dword per thread). With
// `shaderImage2D` the shader declares a 2D image whatever the T#'s type is.
void Load(AgcDriver::VulkanDevice& device, std::uint8_t* texels, const Shape& shape, std::uint32_t slice, std::span<std::uint32_t> output, bool shaderImage2D = false, bool wait = true) {
    const auto& code = shaderImage2D ? Load2D : shape.type == TypeCube ? LoadCube : shape.type == Type2DArray ? LoadArray : Load2D;
    std::vector<std::uint32_t> userData(16, 0u);
    const auto out = std::array<std::uint32_t, 4>{static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(output.data())), static_cast<std::uint32_t>((reinterpret_cast<std::uintptr_t>(output.data()) >> 32u) & 0xffffu), static_cast<std::uint32_t>(output.size_bytes()), 0x31016facu};
    const auto texture = TextureDescriptor(texels, shape);
    std::copy(out.begin(), out.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    userData[12] = slice;
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    std::fill(output.begin(), output.end(), 0xdeadbeefu);
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    if (wait) device.WaitIdle();
}

void Expect(std::span<const std::uint32_t> output, std::uint32_t expected, const std::string& what) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        char text[160];
        std::snprintf(text, sizeof(text), "%s: texel %u read 0x%08x, expected 0x%08x", what.c_str(), tid, output[tid], expected);
        Require(output[tid] == expected, text);
    }
}

void FillSlice(std::uint8_t* slice, std::uint64_t bytes, std::uint32_t value, std::uint32_t elementBytes) {
    for (std::uint64_t offset = 0; offset < bytes; offset += elementBytes) std::memcpy(slice + offset, &value, elementBytes);
}

// A 3-slice D32 array: slice 0 from the live depth surface, slices 1 and 2 from guest memory (detiled).
void ArrayCase(AgcDriver::VulkanDevice& device, GuestBlock& block, std::span<std::uint32_t> output) {
    auto* base = block.At(ArrayOffset);
    DrawDepth(device, reinterpret_cast<std::uintptr_t>(base), {128, 128}, VK_FORMAT_D32_SFLOAT, DrawnDepth);
    FillSlice(base + SliceBytes, SliceBytes, Bits(0.5f), 4u);
    FillSlice(base + 2u * SliceBytes, SliceBytes, Bits(0.75f), 4u);
    const Shape shape{Format32Float, 128, 128, Type2DArray, 2, Swizzle64KbZX};
    const std::array<float, 3> expected{DrawnDepth, 0.5f, 0.75f};
    for (std::uint32_t slice = 0; slice < 3u; ++slice) {
        Load(device, base, shape, slice, output);
        Expect(output, Bits(expected[slice]), "D32 2D array slice " + std::to_string(slice));
    }
}

// A cube over six square D32 slices: face 0 from the live surface, faces 1-5 from guest memory.
void CubeCase(AgcDriver::VulkanDevice& device, GuestBlock& block, std::span<std::uint32_t> output) {
    auto* base = block.At(CubeOffset);
    DrawDepth(device, reinterpret_cast<std::uintptr_t>(base), {128, 128}, VK_FORMAT_D32_SFLOAT, DrawnDepth);
    for (std::uint32_t face = 1; face < 6u; ++face) FillSlice(base + face * SliceBytes, SliceBytes, Bits(static_cast<float>(face) / 8.0f), 4u);
    const Shape shape{Format32Float, 128, 128, TypeCube, 0, Swizzle64KbZX};
    for (std::uint32_t face = 0; face < 6u; ++face) {
        Load(device, base, shape, face, output);
        Expect(output, Bits(face == 0 ? DrawnDepth : static_cast<float>(face) / 8.0f), "D32 cube face " + std::to_string(face));
    }
}

// A D16 plane read through an R16_UINT view (a non-depth swizzle, so not flagged as depth bits): the raw depth value.
void IntegerCase(AgcDriver::VulkanDevice& device, GuestBlock& block, std::span<std::uint32_t> output) {
    auto* base = block.At(IntegerOffset);
    DrawDepth(device, reinterpret_cast<std::uintptr_t>(base), {256, 128}, VK_FORMAT_D16_UNORM, DrawnD16Depth);
    Load(device, base, {Format16Uint, 256, 128, Type2D, 0, Swizzle64KbS}, 0, output);
    Expect(output, DrawnD16Bits, "D16 plane as R16_UINT");
}

// A D16 2D array whose R16_UINT T# has a depth swizzle (flagged as depth bits): both slices' raw values.
void DepthBitsArrayCase(AgcDriver::VulkanDevice& device, GuestBlock& block, std::span<std::uint32_t> output) {
    auto* base = block.At(DepthBitsOffset);
    DrawDepth(device, reinterpret_cast<std::uintptr_t>(base), {256, 128}, VK_FORMAT_D16_UNORM, DrawnD16Depth);
    FillSlice(base + SliceBytes, SliceBytes, 0x1234u, 2u);
    const Shape shape{Format16Uint, 256, 128, Type2DArray, 1, Swizzle64KbZX};
    Load(device, base, shape, 0, output);
    Expect(output, DrawnD16Bits, "D16 depth-bits array slice 0");
    Load(device, base, shape, 1, output);
    Expect(output, 0x1234u, "D16 depth-bits array slice 1");
}

// A 2D shader image over a 2D-array T# reads the array's first slice: a one-slice array, which samples the depth
// image itself, and a 3-slice array, which samples the plane copy.
void DepthBitsBaseSliceCase(AgcDriver::VulkanDevice& device, GuestBlock& block, std::span<std::uint32_t> output) {
    Shape shape{Format16Uint, 256, 128, Type2DArray, 1, Swizzle64KbZX};
    shape.baseArray = 1;
    Load(device, block.At(DepthBitsOffset), shape, 0, output);
    Expect(output, 0x1234u, "D16 depth-bits array from base slice 1");
}

void FirstSliceCase(AgcDriver::VulkanDevice& device, GuestBlock& block, std::span<std::uint32_t> output) {
    auto* one = block.At(OneSliceOffset);
    DrawDepth(device, reinterpret_cast<std::uintptr_t>(one), {128, 128}, VK_FORMAT_D32_SFLOAT, DrawnDepth);
    Load(device, one, {Format32Float, 128, 128, Type2DArray, 0, Swizzle64KbZX}, 0, output, true);
    Expect(output, Bits(DrawnDepth), "one-slice D32 2D array through a 2D shader image");
    auto* base = block.At(FirstSliceOffset);
    DrawDepth(device, reinterpret_cast<std::uintptr_t>(base), {128, 128}, VK_FORMAT_D32_SFLOAT, DrawnDepth);
    FillSlice(base + SliceBytes, SliceBytes, Bits(0.5f), 4u);
    FillSlice(base + 2u * SliceBytes, SliceBytes, Bits(0.75f), 4u);
    Load(device, base, {Format32Float, 128, 128, Type2DArray, 2, Swizzle64KbZX}, 0, output, true);
    Expect(output, Bits(DrawnDepth), "3-slice D32 2D array through a 2D shader image");
}

void BaseSliceCase(AgcDriver::VulkanDevice& device, GuestBlock& block, std::span<std::uint32_t> output) {
    auto* base = block.At(ArrayOffset);
    Shape shape{Format32Float, 128, 128, Type2DArray, 2, Swizzle64KbZX};
    shape.baseArray = 1;
    Load(device, base, shape, 0, output);
    Expect(output, Bits(0.5f), "D32 2D array from base slice 1, slice 0");
    Load(device, base, shape, 1, output);
    Expect(output, Bits(0.75f), "D32 2D array from base slice 1, slice 1");
    Load(device, base, shape, 0, output, true);
    Expect(output, Bits(0.5f), "D32 2D array from base slice 1 through a 2D shader image");
}

void PaddedCubeCase(AgcDriver::VulkanDevice& device, GuestBlock& block, std::span<std::uint32_t> output) {
    auto* base = block.At(PaddedCubeOffset);
    DrawDepth(device, reinterpret_cast<std::uintptr_t>(base), {128, 128}, VK_FORMAT_D16_UNORM, DrawnD16Depth);
    for (std::uint32_t face = 1; face < 6u; ++face) FillSlice(base + face * SliceBytes, SliceBytes, 0x1000u + face, 2u);
    const Shape shape{Format16Uint, 128, 128, TypeCube, 0, Swizzle64KbZX};
    for (std::uint32_t face = 0; face < 6u; ++face) {
        Load(device, base, shape, face, output);
        Expect(output, face == 0 ? DrawnD16Bits : 0x1000u + face, "D16 cube with padded rows, face " + std::to_string(face));
    }
}

void DemonsSoulsArrayCase(AgcDriver::VulkanDevice& device, GuestBlock& block, GuestBlock& outputBlock) {
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    constexpr std::size_t sliceBytes = 1024u * 1024u * 4u;
    auto* base = block.At(65536);
    const std::span<std::uint32_t> output(reinterpret_cast<std::uint32_t*>(outputBlock.At(0)), Threads);
    FillSlice(base, 80u * sliceBytes, Bits(0.375f), 4u);
    DrawDepth(device, reinterpret_cast<std::uintptr_t>(base), {1024, 1024}, VK_FORMAT_D32_SFLOAT, 0.25f);
    DrawDepth(device, reinterpret_cast<std::uintptr_t>(base + 79u * sliceBytes), {1024, 1024}, VK_FORMAT_D32_SFLOAT, 0.625f);
    const Shape shape{Format32Float, 1024, 1024, Type2DArray, 79, Swizzle64KbZX};
    for (const auto slice : {0u, 39u, 79u}) {
        Load(device, base, shape, slice, output);
        Expect(output, Bits(slice == 0 ? 0.25f : slice == 79 ? 0.625f : 0.375f), "Demon's Souls 80-layer depth array slice " + std::to_string(slice));
    }
    DrawDepth(device, reinterpret_cast<std::uintptr_t>(base), {1024, 1024}, VK_FORMAT_D32_SFLOAT, 0.75f);
    Load(device, base, shape, 0, output);
    Expect(output, Bits(0.75f), "Demon's Souls depth array after another GPU write");
    constexpr std::uint32_t queued = 6u;
    const auto queuedOutput = [&](std::uint32_t index) {
        return std::span<std::uint32_t>(reinterpret_cast<std::uint32_t*>(outputBlock.At(256u + index * 256u)), Threads);
    };
    static std::uint64_t countedLayer = 0;
    static std::atomic<std::size_t> layerReads{0};
    countedLayer = reinterpret_cast<std::uintptr_t>(base + 39u * sliceBytes);
    layerReads = 0;
    AgcDriver::GuestMemory::SetReadGuard([](std::uint64_t address, std::size_t bytes) {
        if (address == countedLayer && bytes == sliceBytes) ++layerReads;
    });
    struct ResetReadGuard {
        ~ResetReadGuard() { AgcDriver::GuestMemory::SetReadGuard(nullptr); }
    } resetReadGuard;
    AgcDriver::GuestMemory::Unwatch(countedLayer, sliceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    for (std::uint32_t index = 0; index < queued; ++index) Load(device, base, shape, 39u, queuedOutput(index), false, false);
    const auto expectedReads = std::getenv("APS5_NO_DEPTH_UPLOAD_MEMO") != nullptr ? queued : 1u;
    Require(layerReads == expectedReads, "unchanged depth layer read count " + std::to_string(layerReads.load()) + ", expected " + std::to_string(expectedReads));
    AgcDriver::GuestMemory::StoreOwnBytes(reinterpret_cast<std::uintptr_t>(base + 39u * sliceBytes), sliceBytes, [&] {
        FillSlice(base + 39u * sliceBytes, sliceBytes, Bits(0.875f), 4u);
    });
    Load(device, base, shape, 39u, queuedOutput(queued), false, false);
    device.WaitIdle();
    for (std::uint32_t index = 0; index < queued; ++index) Expect(queuedOutput(index), Bits(0.375f), "queued depth upload retained its old contents");
    Expect(queuedOutput(queued), Bits(0.875f), "changed guest depth layer replaced its upload");
    FillSlice(base + 39u * sliceBytes, sliceBytes, Bits(0.5f), 4u);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Load(device, base, shape, 39u, output);
    Expect(output, Bits(0.5f), "raw guest depth write was detected in the next submission");
    Load(device, base, shape, 40u, output);
    Expect(output, Bits(0.375f), "incremental copies preserved an untouched guest layer");
    AgcDriver::Graphics::ClearDepthSurfaces(device.Device());
    std::puts("Demon's Souls 1024x1024x80 depth array values and refresh passed");
}

#ifdef _WIN32
void InvalidateZeroPage(std::uint8_t* page) {
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(reinterpret_cast<std::uintptr_t>(page), SliceBytes);
}

void ProtectZeroPage(std::uint8_t* page, DWORD protection) {
    DWORD old = 0;
    Require(VirtualProtect(page, SliceBytes, protection, &old) != 0, "zero reads: protection change failed");
    GuestArena::GuestArenaSetProtection_nid_postfix(reinterpret_cast<std::uintptr_t>(page), SliceBytes, protection);
    InvalidateZeroPage(page);
}

void CommitZeroPage(std::uint8_t* page, std::uint32_t value) {
    GuestArena::GuestArenaCommit_nid_postfix(page, SliceBytes, PAGE_READWRITE, SliceBytes);
    FillSlice(page, SliceBytes, value, 4);
    InvalidateZeroPage(page);
}

void MapZeroPage(std::uint8_t* page, std::uint32_t value) {
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, SliceBytes, nullptr);
    Require(section != nullptr, "zero reads: cannot create shared section");
    try {
        GuestArena::GuestArenaMap_nid_postfix(page, SliceBytes, section, 0, PAGE_READWRITE);
    } catch (...) {
        CloseHandle(section);
        throw;
    }
    CloseHandle(section);
    FillSlice(page, SliceBytes, value, 4);
    InvalidateZeroPage(page);
}

void ZeroReadApiCases() {
    using namespace AgcDriver::GuestMemory;
    GuestBlock block(2 * SliceBytes, SliceBytes);
    auto* page = block.At(0);
    const auto address = reinterpret_cast<std::uintptr_t>(page);
    std::vector<std::byte> snapshot(17, std::byte{0x5a});
    const auto unchanged = snapshot;
    const auto* allocation = snapshot.data();
    const auto capacity = snapshot.capacity();
    struct ResetHooks {
        ~ResetHooks() { SetReadGuard(nullptr); SetFlushHook(nullptr); }
    } reset;
    static unsigned guards = 0, flushes = 0;
    guards = flushes = 0;
    SetReadGuard([](std::uint64_t, std::size_t) { ++guards; });
    SetFlushHook([](std::uint64_t, std::size_t) { ++flushes; });
    Require(!ReadCommittedIfAny(std::numeric_limits<std::uint64_t>::max(), 0, snapshot) && guards == 0 && flushes == 0 && snapshot == unchanged, "zero reads: zero bytes performed a read");
    bool overflow = false;
    try { static_cast<void>(ReadCommittedIfAny(std::numeric_limits<std::uint64_t>::max() - 1, 4, snapshot)); }
    catch (const std::runtime_error& error) { overflow = std::string_view(error.what()).find("address range overflow") != std::string_view::npos; }
    Require(overflow && snapshot == unchanged && guards == 1 && flushes == 1, "zero reads: overflowing range was not refused after its hooks");
    GuestArena::GuestArenaReset_nid_postfix(page, SliceBytes);
    InvalidateZeroPage(page);
    for (unsigned i = 0; i < 3; ++i) Require(!ReadCommittedIfAny(address, SliceBytes, snapshot), "zero reads: reserved page was readable");
    Require(snapshot == unchanged && snapshot.data() == allocation && snapshot.capacity() == capacity, "zero reads: empty snapshot materialized or changed its destination");
    SetReadGuard([](std::uint64_t, std::size_t) { ++guards; throw std::runtime_error("zero read guard refusal"); });
    const auto beforeGuard = guards;
    const auto beforeFlush = flushes;
    bool refused = false;
    try { static_cast<void>(ReadCommittedIfAny(address, SliceBytes, snapshot)); }
    catch (const std::runtime_error& error) { refused = std::string_view(error.what()) == "zero read guard refusal"; }
    Require(refused && guards == beforeGuard + 1 && flushes == beforeFlush && snapshot == unchanged, "zero reads: an unreadable span bypassed or repeated the read guard");
    SetReadGuard([](std::uint64_t, std::size_t) { ++guards; });
    SetFlushHook([](std::uint64_t at, std::size_t bytes) {
        ++flushes;
        Require(bytes == SliceBytes, "zero reads: wrong flush extent");
        CommitZeroPage(reinterpret_cast<std::uint8_t*>(at), 0x12345678);
    });
    Require(ReadCommittedIfAny(address, SliceBytes, snapshot), "zero reads: the query preceded materialization by its flush");
    Require(guards == 6 && flushes == 5, "zero reads: hooks ran more than once for one read");
    for (std::size_t offset = 0; offset < snapshot.size(); offset += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, snapshot.data() + offset, 4);
        Require(word == 0x12345678, "zero reads: flush-produced bytes were lost");
    }
    const auto beforeThrow = snapshot;
    const auto guardsBeforeThrow = guards, flushesBeforeThrow = flushes;
    SetFlushHook([](std::uint64_t, std::size_t) { ++flushes; throw std::runtime_error("zero flush refusal"); });
    refused = false;
    try { static_cast<void>(ReadCommittedIfAny(address, SliceBytes, snapshot)); }
    catch (const std::runtime_error& error) { refused = std::string_view(error.what()) == "zero flush refusal"; }
    Require(refused && guards == guardsBeforeThrow + 1 && flushes == flushesBeforeThrow + 1 && snapshot == beforeThrow, "zero reads: a throwing flush changed scratch or ran hooks twice");
    SetFlushHook(nullptr);
    ProtectZeroPage(page, PAGE_NOACCESS);
    Require(!ReadCommittedIfAny(address, SliceBytes, snapshot), "zero reads: NOACCESS bytes were read");
    const auto serial = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    const auto forget = ForgetSerial();
    {
        GuestAllocations::Mutation mutation;
        mutation.Protect(page, SliceBytes, true, true, true, [&] {
            DWORD old = 0;
            Require(VirtualProtect(page, SliceBytes, PAGE_READWRITE, &old) != 0, "zero reads: mutation protection failed");
        });
        Require(GuestAllocations::GuestAllocationsGeneration_nid_postfix() == serial && ForgetSerial() == forget, "zero reads: the adversarial mutation already published its counters");
        Require(ReadCommittedIfAny(address, SliceBytes, snapshot), "zero reads: stable counters hid a readable mutation");
    }
    ProtectZeroPage(page, PAGE_NOACCESS);
    const auto mappingSerial = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    const auto mappingForget = ForgetSerial();
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, SliceBytes, nullptr);
    Require(section != nullptr, "zero reads: cannot create counterexample section");
    try { GuestArena::GuestArenaMap_nid_postfix(page, SliceBytes, section, 0, PAGE_READWRITE); }
    catch (...) { CloseHandle(section); throw; }
    CloseHandle(section);
    FillSlice(page, SliceBytes, 0xabcdef01, 4);
    Require(GuestAllocations::GuestAllocationsGeneration_nid_postfix() == mappingSerial && ForgetSerial() == mappingForget, "zero reads: direct map changed registry counters");
    Require(ReadCommittedIfAny(address, SliceBytes, snapshot) && snapshot.front() == std::byte{1}, "zero reads: direct map was hidden by old coverage");
    MapZeroPage(page, 0x10203040);
    Require(ReadCommittedIfAny(address, SliceBytes, snapshot) && snapshot.front() == std::byte{0x40}, "zero reads: shared remap kept old data");
    GuestArena::GuestArenaReset_nid_postfix(block.At(SliceBytes), SliceBytes);
    InvalidateZeroPage(block.At(SliceBytes));
    Require(ReadCommittedIfAny(address, 2 * SliceBytes, snapshot), "zero reads: partially readable span was treated as empty");
    Require(snapshot[0] == std::byte{0x40} && std::all_of(snapshot.begin() + SliceBytes, snapshot.end(), [](std::byte value) { return value == std::byte{0}; }), "zero reads: partial snapshot holes were not zeroed");
    std::vector<std::byte> reference(2 * SliceBytes);
    ReadCommitted(address, reference);
    Require(snapshot == reference, "zero reads: nonempty snapshot differs from ReadCommitted semantics");
    std::puts("zero-read API transitions and hook ordering passed");
}

int ZeroReadCases() {
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    ZeroReadApiCases();
    GuestBlock array(3 * SliceBytes, SliceBytes);
    GuestBlock outputBlock(SliceBytes);
    const auto device = OpenVulkanTestDevice();
    if (!device) return VulkanTestSkipped;
    auto* base = array.At(0);
    auto* middle = array.At(SliceBytes);
    auto* tail = array.At(2 * SliceBytes);
    const auto output = [&](std::size_t offset = 0) { return std::span<std::uint32_t>(reinterpret_cast<std::uint32_t*>(outputBlock.At(offset)), Threads); };
    const Shape shape{Format32Float, 128, 128, Type2DArray, 2, Swizzle64KbZX};
    DrawDepth(*device, reinterpret_cast<std::uintptr_t>(base), {128, 128}, VK_FORMAT_D32_SFLOAT, DrawnDepth);
    FillSlice(middle, SliceBytes, Bits(0.375f), 4);
    ProtectZeroPage(middle, PAGE_NOACCESS);
    GuestArena::GuestArenaReset_nid_postfix(tail, SliceBytes);
    InvalidateZeroPage(tail);
    const auto load = [&](std::uint32_t layer, std::uint32_t expected, const char* label) {
        AgcDriver::GuestMemory::BumpCollectEpoch();
        Load(*device, base, shape, layer, output());
        Expect(output(), expected, label);
    };
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Load(*device, base, shape, 2, output(256), false, false);
    Require(device->OpenWriteOverlaps(reinterpret_cast<std::uintptr_t>(output(256).data()), output(256).size_bytes()), "zero reads: first output was not queued before replacement");
    CommitZeroPage(tail, Bits(0.5f));
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Load(*device, base, shape, 2, output(512), false, false);
    Require(device->OpenWriteOverlaps(reinterpret_cast<std::uintptr_t>(output(256).data()), output(256).size_bytes()), "zero reads: first queued output was submitted before replacement");
    Require(device->OpenWriteOverlaps(reinterpret_cast<std::uintptr_t>(output(512).data()), output(512).size_bytes()), "zero reads: replacement output was not queued");
    device->WaitIdle();
    Expect(output(256), 0, "queued old zero upload lifetime");
    Expect(output(512), Bits(0.5f), "reserve to commit depth slice");
    for (unsigned i = 0; i < 3; ++i) load(1, 0, "unchanged unreadable depth upload");
    ProtectZeroPage(middle, PAGE_READWRITE);
    load(1, Bits(0.375f), "NOACCESS to readable depth slice");
    ProtectZeroPage(middle, PAGE_NOACCESS);
    load(1, 0, "nonzero depth upload becoming unreadable");
    MapZeroPage(middle, Bits(0.625f));
    load(1, Bits(0.625f), "shared mapped depth slice");
    MapZeroPage(middle, Bits(0.875f));
    load(1, Bits(0.875f), "remapped depth slice");
    load(2, Bits(0.5f), "unchanged readable neighbor");
    load(0, Bits(DrawnDepth), "unchanged live depth slice");
    const auto tailAddress = reinterpret_cast<std::uintptr_t>(tail);
    const std::array<std::uint32_t, 4> pattern{Bits(0.75f), Bits(0.75f), Bits(0.75f), Bits(0.75f)};
    if (!device->FillBuffer(tailAddress, SliceBytes, pattern)) {
        std::puts("skipped, zero-read GPU materialization requires writable host imports");
        return VulkanTestSkipped;
    }
    Require(device->OpenWriteOverlaps(tailAddress, SliceBytes), "zero reads: GPU materialization was not deferred");
    std::vector<std::byte> snapshot;
    Require(AgcDriver::GuestMemory::ReadCommittedIfAny(tailAddress, SliceBytes, snapshot), "zero reads: GPU destination was unreadable");
    for (std::size_t offset = 0; offset < snapshot.size(); offset += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, snapshot.data() + offset, 4);
        Require(word == Bits(0.75f), "zero reads: pending GPU result was not materialized before reading");
    }
    load(2, Bits(0.75f), "GPU-written depth slice");
    std::puts("zero depth array transitions, retention and GPU materialization passed");
    return 0;
}
#endif

}

int main(int argc, char** argv) {
    try {
        Require(argc == 1 || (argc == 2 && (std::string_view(argv[1]) == "--zero-only" || std::string_view(argv[1]) == "--large-only")), "usage: depth_plane_sampling [--zero-only|--large-only]");
        if (argc == 2 && std::string_view(argv[1]) == "--zero-only") {
#ifdef _WIN32
            return ZeroReadCases();
#else
            std::puts("skipped, zero-read transition cases require Windows guest mappings");
            return VulkanTestSkipped;
#endif
        }
        GuestBlock block;
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const std::span<std::uint32_t> output(reinterpret_cast<std::uint32_t*>(block.At(OutputOffset)), Threads);
        if (argc == 2) {
            GuestBlock largeBlock(65536u + 80u * 1024u * 1024u * 4u);
            DemonsSoulsArrayCase(*device, largeBlock, block);
            return 0;
        }
        ArrayCase(*device, block, output);
        CubeCase(*device, block, output);
        IntegerCase(*device, block, output);
        DepthBitsArrayCase(*device, block, output);
        DepthBitsBaseSliceCase(*device, block, output);
        FirstSliceCase(*device, block, output);
        BaseSliceCase(*device, block, output);
        PaddedCubeCase(*device, block, output);
        std::puts("depth plane sampling tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
