#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "DepthFastClearHarness.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
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
#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Format16Unorm = 7;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Type2DArray = 13;
constexpr std::uint32_t LessEqual = 3;
constexpr std::uint32_t ClampEdge = 2;
constexpr std::size_t BlockBytes = 2u << 20u;

struct Shape {
    std::uint32_t format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t layers;
    std::uint32_t mips;
    bool array;
    std::uint32_t tile = 0;
};

alignas(256) constexpr std::array<std::uint32_t, 9> StoreHalfCode{
    0x7e280d00, 0x102828ff, 0x3d000000, 0x7e3c0300, 0x7e3e0280, 0xf0201108, 0x0001141e, 0xbf810000, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 9> StoreQuarterCode{
    0x7e280d00, 0x102828ff, 0x3c800000, 0x7e3c0300, 0x7e3e0280, 0xf0201108, 0x0001141e, 0xbf810000, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 10> StoreLayerMipCode{
    0x7e280d00, 0x1028280e, 0x7e3c0300, 0x7e3e0280, 0x7e40020c, 0x7e42020d, 0xf0241128, 0x0001141e, 0xbf810000, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 13> SampleCode{
    0x1614008c, 0xe03c1000, 0x8000010a, 0xbf8c3f70, 0xf0bc0108, 0x00820401, 0xbf8c3f70,
    0x34160082, 0xe0701000, 0x8001040b, 0xbf810000, 0xbf810000, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 15> SampleLayerMipCode{
    0x1614008c, 0xe03c1000, 0x8000010a, 0x7e080214, 0x7e0a0215, 0xbf8c3f70, 0xf0b00128, 0x00820601, 0xbf8c3f70,
    0x34160082, 0xe0701000, 0x8001060b, 0xbf810000, 0xbf810000, 0xbf810000,
};

std::span<float> Input;
std::span<float> Output;

class GuestBlock {
public:
    explicit GuestBlock(std::size_t bytes = BlockBytes) : bytes(bytes) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
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
        Require(block != nullptr, "depth compare copy: cannot allocate the guest block");
        Require(AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uintptr_t>(block), bytes), "depth compare copy: the guest block is not write-watched");
        std::memset(block, 0, bytes);
        GuestAllocations::Mutation().Add(block, bytes, true, true, true);
        AgcDriver::GuestMemory::CollectWritesUncached(reinterpret_cast<std::uintptr_t>(block), bytes);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
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

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
    std::size_t bytes;
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::array<std::uint32_t, 8> TextureDescriptor(const void* data, const Shape& shape) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (shape.format << 20u) | (((shape.width - 1u) & 3u) << 30u),
        ((shape.width - 1u) >> 2u) | ((shape.height - 1u) << 14u),
        0xfacu | ((shape.mips - 1u) << 16u) | (shape.tile << 20u) | ((shape.array ? Type2DArray : Type2D) << 28u),
        shape.array ? shape.layers - 1u : 0u,
        (shape.mips - 1u) << 4u,
        0u, 0u,
    };
}

std::array<std::uint32_t, 4> SamplerDescriptor(bool mips) {
    return {ClampEdge | (ClampEdge << 3u) | (ClampEdge << 6u) | (LessEqual << 12u), mips ? 0xf00u << 12u : 0u, mips ? 1u << 26u : 0u, 0u};
}

std::uint32_t Bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

void Store(AgcDriver::VulkanDevice& device, std::uint8_t* texels, const Shape& shape, std::span<const std::uint32_t> code, std::uint32_t layer = 0, std::uint32_t mip = 0, float scale = 0.0f) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto texture = TextureDescriptor(texels, shape);
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    userData[12] = layer;
    userData[13] = mip;
    userData[14] = Bits(scale);
    const auto result = Compile(device, code, userData);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

ShaderRecompiler::RecompileResult SampleProgram(AgcDriver::VulkanDevice& device, std::uint8_t* texels, const Shape& shape, std::uint32_t layer = 0, std::uint32_t mip = 0) {
    std::vector<std::uint32_t> userData(24, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size_bytes()));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size_bytes()));
    const auto texture = TextureDescriptor(texels, shape);
    const auto sampler = SamplerDescriptor(shape.mips > 1u);
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    std::copy(texture.begin(), texture.end(), userData.begin() + 8);
    std::copy(sampler.begin(), sampler.end(), userData.begin() + 16);
    userData[20] = Bits(static_cast<float>(layer));
    userData[21] = Bits(static_cast<float>(mip));
    return Compile(device, shape.array ? std::span<const std::uint32_t>(SampleLayerMipCode) : std::span<const std::uint32_t>(SampleCode), userData);
}

void Sample(AgcDriver::VulkanDevice& device, const ShaderRecompiler::RecompileResult& program, const Shape& shape, float scale, const std::string& what, std::uint32_t layer = 0, std::uint32_t mip = 0) {
    const auto width = std::max(shape.width >> mip, 1u);
    const auto height = std::max(shape.height >> mip, 1u);
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const float value = static_cast<float>(tid) * scale;
        Input[tid * 3u + 0u] = (tid & 1u) != 0u ? value - scale / 4.0f : value + scale / 4.0f;
        Input[tid * 3u + 1u] = (static_cast<float>(tid) + 0.5f) / static_cast<float>(width);
        Input[tid * 3u + 2u] = 0.5f / static_cast<float>(height);
    }
    std::fill(Output.begin(), Output.end(), -1.0f);
    const auto* code = shape.array ? SampleLayerMipCode.data() : SampleCode.data();
    device.Dispatch(program, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code));
    device.WaitIdle();
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const float expected = (tid & 1u) != 0u ? 1.0f : 0.0f;
        Require(Output[tid] == expected, what + ": thread " + std::to_string(tid) + " compared to " + std::to_string(Output[tid]) + ", expected " + std::to_string(expected));
    }
}

std::uint64_t SurfaceBytes(std::uint8_t* texels, const Shape& shape) {
    const auto surface = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(TextureDescriptor(texels, shape)));
    Require(surface.guestBytes >= (1u << 20u) && surface.guestBytes <= BlockBytes, "depth compare copy: the surface does not fit the guest block or is below the depth copy floor");
    return surface.guestBytes;
}

std::uint64_t PendingVersion(std::uint8_t* texels, std::uint64_t bytes, const std::string& what) {
    const auto source = AgcDriver::Graphics::StorageTexture::FindPending(reinterpret_cast<std::uintptr_t>(texels), bytes);
    Require(source != nullptr, what + ": no storage image holds the surface's results");
    return source->Version();
}

void Settle(AgcDriver::VulkanDevice& device, std::uint8_t* texels, std::uint64_t bytes) {
    AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(texels), bytes, nullptr, "test");
    device.WaitIdle();
    AgcDriver::GuestMemory::CollectWritesUncached(reinterpret_cast<std::uintptr_t>(texels), bytes);
}

void RequireCounts(std::uint64_t textures, std::uint64_t refreshes, const std::string& what) {
    const auto counts = AgcDriver::Graphics::Texture::TakeDepthCopyCounts();
    Require(counts.textures == textures && counts.refreshes == refreshes, what + ": " + std::to_string(counts.textures) + " depth copy textures and " + std::to_string(counts.refreshes) + " refreshes, expected " + std::to_string(textures) + " and " + std::to_string(refreshes));
}

void RequireNewerVersion(std::uint64_t before, std::uint64_t after, const std::string& what) {
    Require(after > before, what + ": the storage image version stayed at " + std::to_string(before));
}

float Texel(const std::uint8_t* texels, std::uint32_t x) {
    float value = 0.0f;
    std::memcpy(&value, texels + static_cast<std::size_t>(x) * sizeof(float), sizeof(float));
    return value;
}

void RequireGuestTexels(const std::uint8_t* texels, float scale, const std::string& what) {
    for (std::uint32_t x = 0; x < Threads; ++x) {
        const float expected = static_cast<float>(x) * scale;
        Require(Texel(texels, x) == expected, what + ": texel " + std::to_string(x) + " is " + std::to_string(Texel(texels, x)) + ", expected " + std::to_string(expected));
    }
}

void SingleLevelCase(AgcDriver::VulkanDevice& device, const Shape& shape, const std::string& name) {
    GuestBlock block;
    auto* texels = block.Data();
    const auto bytes = SurfaceBytes(texels, shape);
    Store(device, texels, shape, StoreQuarterCode);
    Settle(device, texels, bytes);
    AgcDriver::Graphics::Texture::TakeDepthCopyCounts();
    Store(device, texels, shape, StoreHalfCode);
    const auto program = SampleProgram(device, texels, shape);
    Sample(device, program, shape, 1.0f / 32.0f, name + ": comparison sample of pending results");
    RequireCounts(1, 1, name + ": first comparison sample");
    const auto before = PendingVersion(texels, bytes, name);
    Store(device, texels, shape, StoreQuarterCode);
    RequireNewerVersion(before, PendingVersion(texels, bytes, name), name + ": second store");
    Sample(device, program, shape, 1.0f / 64.0f, name + ": comparison sample of the same descriptor set after a second store");
    RequireCounts(0, 1, name + ": comparison sample after a second store");
    AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(texels), bytes, nullptr, "test");
    device.WaitIdle();
    if (shape.format == Format32Float) RequireGuestTexels(texels, 1.0f / 64.0f, name + ": flushed results");
}

void LayersAndMipsCase(AgcDriver::VulkanDevice& device) {
    const Shape shape{Format32Float, 256, 256, 4, 3, true};
    const std::string name = "R32F 256x256, 4 layers, 3 mips";
    GuestBlock block;
    auto* texels = block.Data();
    const auto bytes = SurfaceBytes(texels, shape);
    Store(device, texels, shape, StoreLayerMipCode, 0, 0, 1.0f / 512.0f);
    Settle(device, texels, bytes);
    AgcDriver::Graphics::Texture::TakeDepthCopyCounts();
    Store(device, texels, shape, StoreLayerMipCode, 0, 0, 1.0f / 32.0f);
    Store(device, texels, shape, StoreLayerMipCode, 2, 1, 1.0f / 64.0f);
    Store(device, texels, shape, StoreLayerMipCode, 3, 2, 1.0f / 128.0f);
    const auto first = SampleProgram(device, texels, shape, 0, 0);
    const auto second = SampleProgram(device, texels, shape, 2, 1);
    const auto third = SampleProgram(device, texels, shape, 3, 2);
    Sample(device, first, shape, 1.0f / 32.0f, name + ": layer 0 mip 0", 0, 0);
    Sample(device, second, shape, 1.0f / 64.0f, name + ": layer 2 mip 1", 2, 1);
    Sample(device, third, shape, 1.0f / 128.0f, name + ": layer 3 mip 2", 3, 2);
    RequireCounts(1, 1, name + ": first comparison samples");
    const auto before = PendingVersion(texels, bytes, name);
    Store(device, texels, shape, StoreLayerMipCode, 2, 1, 1.0f / 256.0f);
    RequireNewerVersion(before, PendingVersion(texels, bytes, name), name + ": second store");
    Sample(device, second, shape, 1.0f / 256.0f, name + ": layer 2 mip 1 after a second store", 2, 1);
    Sample(device, first, shape, 1.0f / 32.0f, name + ": layer 0 mip 0 after a second store", 0, 0);
    RequireCounts(0, 1, name + ": comparison samples after a second store");
}

void DrawArrayDepth(AgcDriver::VulkanDevice& device, std::uint64_t address, const Shape& shape, std::uint32_t slice, float depth, std::span<std::byte> pixels, std::span<const std::array<float, 4>> vertices, std::uint64_t htile) {
    const auto compiled = DepthFastClearHarness::Compile(device, vertices.data());
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &compiled.vertex, 0},
        {ShaderStage::Fragment, &compiled.pixel, PixelPushOffset(compiled.vertexPush, device.Target())}
    }};
    const VkExtent2D extent{shape.width, shape.height};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(pixels.data()), extent, VK_FORMAT_R8G8B8A8_UNORM, pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.depth = AgcDriver::Graphics::DepthTarget{address, 0, extent, VK_FORMAT_D32_SFLOAT, 1.0f, 0};
    state.depth->htileAddress = htile;
    state.depth->firstSlice = slice;
    state.depth->lastSlice = slice;
    state.depth->depthSwizzle = shape.tile;
    state.depthTest = true;
    state.depthWrite = true;
    state.depthCompare = VK_COMPARE_OP_ALWAYS;
    state.renderExtent = extent;
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(extent.height), static_cast<float>(extent.width), -static_cast<float>(extent.height), depth, depth};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, extent};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(vertices.size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
}

void DepthArrayCase(AgcDriver::VulkanDevice& device, std::string_view mode) {
    namespace Graphics = AgcDriver::Graphics;
    namespace Memory = AgcDriver::GuestMemory;
    using Counters = AgcDriver::ArchB::Recorder;
    Require(mode == "on" || mode == "off" || mode == "verify" || mode == "cpu" || mode == "pending" || mode == "cold" || mode == "htile" || mode == "changed-htile" || mode == "queued" || mode == "toggle" || mode == "partial" || mode == "stale-cpu", "unknown depth array mode");
    const Shape shape{Format32Float, 512, 512, 8, 1, true, 0x18};
    GuestBlock block(16u << 20u);
    GuestBlock drawBuffers(4u << 20u);
    GuestBlock metadataBuffers(4u << 20u);
    auto* vertices = reinterpret_cast<std::array<float, 4>*>(drawBuffers.Data());
    vertices[0] = {-1.0f, -1.0f, 0.5f, 1.0f};
    vertices[1] = {3.0f, -1.0f, 0.5f, 1.0f};
    vertices[2] = {-1.0f, 3.0f, 0.5f, 1.0f};
    const bool htile = mode == "htile" || mode == "changed-htile";
    if (htile) {
        for (const auto offset : {1u << 20u, 2u << 20u}) std::fill_n(reinterpret_cast<std::uint32_t*>(metadataBuffers.Data() + offset), 32768u / sizeof(std::uint32_t), 0xffffffffu);
    }
    const auto address = reinterpret_cast<std::uintptr_t>(block.Data());
    const auto resource = Graphics::DecodeTextureResource(TextureDescriptor(block.Data(), shape));
    const auto geometry = Graphics::DescribeSurface(resource);
    Require(geometry.layerBytes == (1u << 20u) && geometry.guestBytes == (8u << 20u), "depth array geometry is invalid");
    std::fill_n(reinterpret_cast<float*>(block.Data()), geometry.guestBytes / sizeof(float), 0.25f);
    Graphics::SetHostImportWindow(1u << 20u);
    Graphics::SetHostImportRangeWindow(1u << 20u);
    const AgcDriver::ArchB::PartialSnapshotScope partial(true);
    std::unique_lock gpu(Memory::GpuMutex());
    const auto context = device.graphicsContext();
    Graphics::PrepareImportWatch(context);
    auto* recorder = Graphics::Recorder::Active();
    Require(recorder != nullptr && recorder->Device() == device.Device(), "depth array recorder is unavailable");
    std::array<ShaderRecompiler::RecompileResult, 4> programs;
    for (std::uint32_t layer = 0; layer < programs.size(); ++layer) programs[layer] = SampleProgram(device, block.Data(), shape, layer);
    const auto sample = [&](std::uint32_t layer, float depth) {
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            Input[tid * 3u] = (tid & 1u) != 0u ? depth - 0.0625f : depth + 0.0625f;
            Input[tid * 3u + 1u] = (static_cast<float>(tid) + 0.5f) / shape.width;
            Input[tid * 3u + 2u] = 0.5f / shape.height;
        }
        std::fill(Output.begin(), Output.end(), -1.0f);
        Memory::BumpCollectEpoch();
        gpu.unlock();
        const auto prepared = device.PrepareDispatch(programs.at(layer), {}, Threads, reinterpret_cast<std::uintptr_t>(SampleLayerMipCode.data()));
        gpu.lock();
        device.Dispatch(programs.at(layer), 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(SampleLayerMipCode.data()), prepared);
        device.WaitIdle();
        for (std::uint32_t tid = 0; tid < Threads; ++tid) Require(Output[tid] == ((tid & 1u) != 0u ? 1.0f : 0.0f), "depth array comparison differs at layer " + std::to_string(layer) + " thread " + std::to_string(tid));
    };
    if (mode != "cold") sample(0, 0.25f);
    Counters::BindThread("depth array test");
    Counters::Boundary(true, 1, Counters::Clock::now());
    Counters::Collect(Counters::Clock::now());
    const auto storesBefore = Graphics::DepthPlaneStoreCounts();
    const auto presyncBefore = AgcDriver::VulkanDevice::PresyncCounts();
    const auto bridgeBefore = Graphics::DepthArrayGpuCounts();
    const auto draw = [&](std::uint32_t layer, float depth, std::size_t pixelOffset) {
        Memory::BumpCollectEpoch();
        if (htile) {
            const auto metadata = reinterpret_cast<std::uintptr_t>(metadataBuffers.Data() + (layer == 1u ? 1u << 20u : 2u << 20u));
            Require(Memory::Watched(metadata, 32768u) && Memory::CollectWritesUncached(metadata, 32768u) != 0u, "depth array metadata is not tracked before draw");
        }
        auto drawShape = shape;
        if (mode == "partial" && layer == 1) drawShape.width /= 2;
        DrawArrayDepth(device, address + geometry.GuestLayerOffset(layer), drawShape, layer, depth, {reinterpret_cast<std::byte*>(drawBuffers.Data() + pixelOffset), static_cast<std::size_t>(geometry.layerBytes)}, {vertices, 3}, htile ? reinterpret_cast<std::uintptr_t>(metadataBuffers.Data() + (layer == 1u ? 1u << 20u : 2u << 20u)) : 0);
    };
    draw(1, 0.75f, 1u << 20u);
    draw(3, 0.125f, 2u << 20u);
    Require(Graphics::DepthStalePlaneOverlaps(address, address + geometry.guestBytes), "depth array draws did not leave GPU-authoritative planes");
    if (mode == "changed-htile") {
        *reinterpret_cast<std::uint32_t*>(metadataBuffers.Data() + (1u << 20u)) = 0xfffeffffu;
        Memory::CollectWritesUncached(reinterpret_cast<std::uintptr_t>(metadataBuffers.Data() + (1u << 20u)), 32768u);
    }
    bool landed = false;
    if (mode == "pending") {
        const auto destination = address + geometry.GuestLayerOffset(2);
        recorder->NotePendingWrite(destination, static_cast<std::size_t>(geometry.layerBytes));
        recorder->OnComplete([destination, bytes = geometry.layerBytes, &landed] {
            Memory::StoreOwnBytes(destination, static_cast<std::size_t>(bytes), [&] { std::fill_n(reinterpret_cast<float*>(destination), bytes / sizeof(float), 0.375f); });
            landed = true;
        });
    }
    if (mode == "queued") {
        const auto destination = address + geometry.GuestLayerOffset(2);
        const auto* import = Graphics::HostImportFor(context, destination, static_cast<std::size_t>(geometry.layerBytes));
        Require(import != nullptr, "depth array queued store has no import");
        const std::vector<std::uint32_t> values(65536u / sizeof(std::uint32_t), Bits(0.375f));
        Require(device.WriteLabelOnGpu(destination, std::as_bytes(std::span(values)), 1, 0, false) == 0, "depth array queued GPU store was refused");
        Require(recorder->QueuedStoreOverlaps(destination, static_cast<std::size_t>(geometry.layerBytes)), "depth array did not queue its external store");
    }
    sample(1, 0.75f);
    sample(3, 0.125f);
    sample(2, mode == "pending" || mode == "queued" ? 0.375f : 0.25f);
    Require(mode != "pending" || landed, "depth array pending CPU writer was not completed");
    if (mode == "toggle") {
        Require(Graphics::DepthPlaneStoreCounts().stores == storesBefore.stores, "depth array stored before runtime disable");
        Require(Graphics::DepthArrayGpuCounts().layers - bridgeBefore.layers == 2, "depth array did not refresh before runtime disable");
        const auto* control = std::getenv("APS5_ARCHB_CONTROL_FILE");
        Require(control != nullptr, "depth array runtime control file is missing");
        { std::ofstream file(control); file << "2 DEPTH_ARRAY_GPU=0\n"; }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        AgcDriver::ArchB::Switches::PollControl();
        Require(!AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::DepthArrayGpu), "depth array runtime disable was not applied");
    }
    if (mode == "on" || mode == "verify" || mode == "cpu" || mode == "htile" || mode == "toggle" || mode == "stale-cpu") {
        draw(1, 0.625f, 1u << 20u);
        draw(3, 0.375f, 2u << 20u);
        sample(1, 0.625f);
        sample(3, 0.375f);
    }
    const auto counts = Counters::Collect(Counters::Clock::now());
    for (const auto& [name, value] : counts.counters) if (name.starts_with("depth_array") || name.starts_with("fix_sampled_depth") || name.starts_with("presync") || name.starts_with("fix_compute_snapshot_prewait")) std::printf("%s %.0f\n", name.c_str(), value);
    const auto presyncAfter = AgcDriver::VulkanDevice::PresyncCounts();
    std::printf("depth array presync checks %llu waits %llu\n", static_cast<unsigned long long>(presyncAfter.checked - presyncBefore.checked), static_cast<unsigned long long>(presyncAfter.presyncs - presyncBefore.presyncs));
    const auto bridgeAfter = Graphics::DepthArrayGpuCounts();
    const auto layers = bridgeAfter.layers - bridgeBefore.layers;
    const auto taken = bridgeAfter.taken - bridgeBefore.taken;
    const auto verified = bridgeAfter.verified - bridgeBefore.verified;
    std::printf("depth array %s: taken %llu layers %llu verified %llu stores %llu\n", std::string(mode).c_str(), static_cast<unsigned long long>(taken), static_cast<unsigned long long>(layers), static_cast<unsigned long long>(verified), static_cast<unsigned long long>(Graphics::DepthPlaneStoreCounts().stores - storesBefore.stores));
    const bool expected = mode == "on" || mode == "verify" || mode == "cpu" || mode == "htile" || mode == "toggle" || mode == "stale-cpu";
    Require((taken != 0) == expected, "depth array did not take its selected path");
    Require(!expected || layers == (mode == "toggle" ? 2u : 4u), "depth array did not refresh exactly two changed layers on each round");
    Require(bridgeAfter.mismatches == bridgeBefore.mismatches, "depth array texels differ from old store-back");
    Require(mode != "verify" || verified != 0, "depth array did not verify the old path");
    if (mode == "on" || mode == "cpu" || mode == "htile" || mode == "stale-cpu") {
        Require(AgcDriver::VulkanDevice::PresyncCounts().presyncs == presyncBefore.presyncs, "depth array waited in producer pre-sync");
        Require(Graphics::DepthPlaneStoreCounts().stores == storesBefore.stores, "depth array sample stored a GPU-authoritative plane on the CPU");
        Require(Graphics::DepthStalePlaneOverlaps(address, address + geometry.guestBytes), "depth array sample lost deferred CPU store-back");
    }
    if (mode == "stale-cpu") {
        const auto destination = address + geometry.GuestLayerOffset(1);
        Require(Graphics::DepthStalePlaneOverlaps(destination, destination + geometry.layerBytes), "depth array CPU overwrite did not start over a stale plane");
        std::fill_n(reinterpret_cast<float*>(destination), geometry.layerBytes / sizeof(float), 0.875f);
        Memory::CollectWritesUncached(destination, geometry.layerBytes);
        sample(1, 0.875f);
        sample(3, 0.375f);
        Require(Graphics::DepthArrayGpuCounts().layers == bridgeAfter.layers, "depth array copied a stale GPU plane over a CPU overwrite");
        Require(Graphics::DepthArrayGpuCounts().mismatches == bridgeAfter.mismatches, "depth array CPU overwrite introduced a verification mismatch");
    }
    Graphics::FlushDepthPlanes(address, static_cast<std::size_t>(geometry.guestBytes));
    device.WaitIdle();
    for (const auto layer : {1u, 3u}) {
        const auto* values = reinterpret_cast<const float*>(block.Data() + geometry.GuestLayerOffset(layer));
        for (std::size_t index = 0; index < geometry.layerBytes / sizeof(float); ++index) {
            const auto value = mode == "stale-cpu" && layer == 1u ? 0.875f : mode == "partial" && layer == 1u && index >= geometry.layerBytes / (2u * sizeof(float)) ? 0.25f : expected ? (layer == 1u ? 0.625f : 0.375f) : (layer == 1u ? 0.75f : 0.125f);
            Require(values[index] == value, "depth array deferred CPU store-back differs");
        }
    }
    if (mode == "cpu") {
        std::fill_n(reinterpret_cast<float*>(block.Data() + geometry.GuestLayerOffset(1)), geometry.layerBytes / sizeof(float), 0.375f);
        Memory::CollectWritesUncached(address, geometry.guestBytes);
        sample(1, 0.375f);
        sample(3, 0.375f);
    }
    Counters::Boundary(false, 2, Counters::Clock::now());
}

void GpuArrayCase(AgcDriver::VulkanDevice& device, std::string_view mode) {
    namespace Graphics = AgcDriver::Graphics;
    namespace Memory = AgcDriver::GuestMemory;
    using Counters = AgcDriver::ArchB::Recorder;
    Require(mode == "on" || mode == "off" || mode == "cpu" || mode == "queued" || mode == "shadow", "unknown GPU sampled array mode");
    const Shape shape{Format32Float, 512, 512, 8, 1, true, 0x18};
    Graphics::SetHostImportWindow(1u << 20u);
    Graphics::SetHostImportRangeWindow(1u << 20u);
    GuestBlock block(16u << 20u);
    const auto address = reinterpret_cast<std::uintptr_t>(block.Data());
    const auto resource = Graphics::DecodeTextureResource(TextureDescriptor(block.Data(), shape));
    const auto geometry = Graphics::DescribeSurface(resource);
    Require(geometry.layerBytes == (1u << 20u) && geometry.guestBytes == (8u << 20u), "GPU sampled array geometry is invalid");
    std::fill_n(reinterpret_cast<float*>(block.Data()), geometry.guestBytes / sizeof(float), 0.25f);
    const AgcDriver::ArchB::PartialSnapshotScope partial(true);
    std::lock_guard gpu(Memory::GpuMutex());
    const auto context = device.graphicsContext();
    const auto watch = Graphics::PrepareImportWatch(context);
    struct RestoreWatch {
        const Graphics::Context& context;
        Graphics::ImportWatch watch;
        bool active;
        ~RestoreWatch() { if (active) Graphics::SetImportWatch(context, watch); }
    } restoreWatch{context, watch, mode == "shadow"};
    if (mode == "shadow") Graphics::SetImportWatch(context, Graphics::ImportWatch::Watch);
    auto* recorder = Graphics::Recorder::Active();
    Require(recorder != nullptr && recorder->Device() == device.Device(), "GPU sampled array recorder is unavailable");
    std::array<ShaderRecompiler::RecompileResult, 4> programs;
    for (std::uint32_t layer = 0; layer < programs.size(); ++layer) programs[layer] = SampleProgram(device, block.Data(), shape, layer);
    const auto sample = [&](std::uint32_t layer, float depth) {
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            Input[tid * 3u] = (tid & 1u) != 0u ? depth - 0.0625f : depth + 0.0625f;
            Input[tid * 3u + 1u] = (static_cast<float>(tid) + 0.5f) / shape.width;
            Input[tid * 3u + 2u] = 0.5f / shape.height;
        }
        std::fill(Output.begin(), Output.end(), -1.0f);
        Memory::BumpCollectEpoch();
        device.Dispatch(programs.at(layer), 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(SampleLayerMipCode.data()));
        device.WaitIdle();
        for (std::uint32_t tid = 0; tid < Threads; ++tid) Require(Output[tid] == ((tid & 1u) != 0u ? 1.0f : 0.0f), "GPU sampled array comparison differs at layer " + std::to_string(layer) + " thread " + std::to_string(tid));
    };
    sample(0, 0.25f);
    Require(!Graphics::HostImportCovers(context, address, static_cast<std::size_t>(geometry.guestBytes)), "GPU sampled array was covered by a whole-array import");
    Counters::BindThread("sampled array test");
    Counters::Boundary(true, 1, Counters::Clock::now());
    Counters::Collect(Counters::Clock::now());
    const auto fill = [&](std::uint32_t layer, float value) {
        if (mode == "shadow") {
            const auto begin = address + geometry.GuestLayerOffset(layer);
            const auto end = begin + geometry.layerBytes;
            const auto* import = Graphics::HostImportFor(context, begin, static_cast<std::size_t>(geometry.layerBytes));
            Require(import != nullptr && Memory::Watched(begin, geometry.layerBytes), "the lazy sampled array layer is not imported and watched");
            Memory::CollectWritesUncached(begin, geometry.layerBytes);
            GuestArena::GuestWtFaultTrack_nid_no_patch(begin, geometry.layerBytes);
            const auto destination = Graphics::ShadowDestinationFor(context, *import, begin, end);
            Require(destination.has_value() && destination->seedUnits.empty(), "the lazy sampled array layer has no slab");
            const auto commands = recorder->Commands();
            recorder->Keep(destination->slab);
            Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, destination->buffer, destination->offset, geometry.layerBytes, Bits(value));
            Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
            const Graphics::ShadowedRange range{begin, end, destination->slab};
            Graphics::MarkShadowed(*import, std::span(&range, 1), Memory::TrackerGeneration());
            Graphics::SettlePlaneStore("depth plane store", begin, end, Graphics::PublishScope::None);
            Require(Graphics::AnyShadowedOverlaps(begin, geometry.layerBytes), "the lazy sampled array published before sampling");
            return;
        }
        const std::array<std::uint32_t, 4> pattern{Bits(value), Bits(value), Bits(value), Bits(value)};
        const auto source = address + geometry.guestBytes + geometry.GuestLayerOffset(layer);
        Require(device.FillBuffer(source, static_cast<std::size_t>(geometry.layerBytes), pattern), "GPU sampled array source fill was refused");
        const auto copied = device.CopyBuffer(address + geometry.GuestLayerOffset(layer), source, static_cast<std::size_t>(geometry.layerBytes), 0, BlockBytes, 0, 0, 0, [](std::span<const std::byte>, std::uint64_t) {});
        Require(copied.path == 1 && !copied.synced, "GPU sampled array depth values were not copied on the GPU");
        Require(Graphics::ExistingHostImportFor(context, address + geometry.GuestLayerOffset(layer), static_cast<std::size_t>(geometry.layerBytes)) != nullptr, "GPU sampled array layer has no GPU source");
    };
    fill(1, 0.75f);
    fill(3, 0.125f);
    bool landed = false;
    if (mode == "cpu") {
        const auto destination = address + geometry.GuestLayerOffset(1);
        recorder->NotePendingWrite(destination, static_cast<std::size_t>(geometry.layerBytes));
        recorder->OnComplete([destination, bytes = geometry.layerBytes, &landed] {
            Memory::StoreOwnBytes(destination, static_cast<std::size_t>(bytes), [&] { std::fill_n(reinterpret_cast<float*>(destination), bytes / sizeof(float), 0.375f); });
            landed = true;
        });
    } else if (mode == "queued") {
        const auto destination = address + geometry.GuestLayerOffset(1);
        const auto* import = Graphics::ExistingHostImportFor(context, destination, static_cast<std::size_t>(geometry.layerBytes));
        const std::vector<std::uint32_t> values(65536u / sizeof(std::uint32_t), Bits(0.375f));
        recorder->RecordStore(import->buffer, Graphics::HostImportOffset(*import, destination), std::as_bytes(std::span(values)), destination);
        Require(recorder->QueuedStoreOverlaps(destination, static_cast<std::size_t>(geometry.layerBytes)), "GPU sampled array did not queue its label store");
    }
    Require(mode == "shadow" ? Graphics::AnyShadowedOverlaps(address, geometry.guestBytes) : recorder->PendingWriteOverlaps(address, static_cast<std::size_t>(geometry.guestBytes)), "GPU sampled array writer completed before the lookup");
    const auto mismatches = Graphics::Texture::PartialSnapshotVerifyMismatches();
    sample(1, mode == "cpu" || mode == "queued" ? 0.375f : 0.75f);
    sample(3, 0.125f);
    sample(2, 0.25f);
    auto counts = Counters::Collect(Counters::Clock::now());
    for (const auto& [name, value] : counts.counters) if (name.starts_with("fix_sampled_array_gpu")) std::printf("%s %.0f\n", name.c_str(), value);
    std::printf("GPU sampled array %s: taken %.0f, refused CPU %.0f, verified %.0f, mismatches %.0f\n", std::string(mode).c_str(), counts.counters["fix_sampled_array_gpu_taken"], counts.counters["fix_sampled_array_gpu_refused_cpu_store"], counts.counters["fix_sampled_array_gpu_verified"], counts.counters["fix_sampled_array_gpu_verify_mismatches"]);
    Require((counts.counters["fix_sampled_array_gpu_taken"] == 1.0) == (mode == "on" || mode == "shadow"), "GPU sampled array did not take its selected path");
    if (mode == "shadow") Require(counts.counters["n5a_force_sampled_array_import_bytes"] == 2.0 * geometry.layerBytes, "the GPU array consumer did not materialize its shadow inputs exactly once");
    if (mode == "cpu") Require(landed && counts.counters["fix_sampled_array_gpu_refused_cpu_store"] >= 1.0, "GPU sampled array accepted a pending CPU completion store");
    if (mode == "queued") Require(counts.counters["fix_sampled_array_gpu_refused_queued_store"] >= 1.0, "GPU sampled array accepted an unrecorded label store");
    Require(Graphics::Texture::PartialSnapshotVerifyMismatches() == mismatches, "GPU sampled array bytes differ from the old snapshot");
    for (const auto layer : {1u, 3u}) {
        const auto* values = reinterpret_cast<const float*>(block.Data() + geometry.GuestLayerOffset(layer));
        for (std::size_t index = 0; index < geometry.layerBytes / sizeof(float); ++index) {
            const auto expected = layer == 3u ? 0.125f : mode == "cpu" || (mode == "queued" && index < 65536u / sizeof(float)) ? 0.375f : 0.75f;
            Require(values[index] == expected, "GPU sampled array changed guest publication");
        }
    }
    Counters::Boundary(false, 2, Counters::Clock::now());
}

}

int main(int argc, char** argv) {
    try {
        struct ControlFile {
            std::filesystem::path path;
            ~ControlFile() { if (!path.empty()) { std::error_code error; std::filesystem::remove(path, error); } }
        } control;
        if (argc == 3 && std::string_view(argv[1]) == "--depth-array" && std::string_view(argv[2]) == "toggle") {
            control.path = std::filesystem::current_path() / ("depth-array-control-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
            { std::ofstream file(control.path); file << "1 DEPTH_ARRAY_GPU=1\n"; }
#ifdef _WIN32
            _putenv_s("APS5_ARCHB_CONTROL_FILE", control.path.string().c_str());
#else
            setenv("APS5_ARCHB_CONTROL_FILE", control.path.string().c_str(), 1);
#endif
        }
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock buffers;
        Input = std::span<float>(reinterpret_cast<float*>(buffers.Data()), Threads * 3u);
        Output = std::span<float>(reinterpret_cast<float*>(buffers.Data() + 4096u), Threads);
        if (argc == 3 && std::string_view(argv[1]) == "--depth-array") {
            if (device->graphicsContext().hostImportAlignment == 0) return VulkanTestSkipped;
            DepthArrayCase(*device, argv[2]);
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--gpu-array") {
            if (device->graphicsContext().hostImportAlignment == 0) return VulkanTestSkipped;
            GpuArrayCase(*device, argv[2]);
            return 0;
        }
        SingleLevelCase(*device, {Format32Float, 512, 512, 1, 1, false}, "R32F 512x512");
        SingleLevelCase(*device, {Format16Unorm, 1024, 512, 1, 1, false}, "R16 unorm 1024x512");
        LayersAndMipsCase(*device);
        std::puts("depth compare copy tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
