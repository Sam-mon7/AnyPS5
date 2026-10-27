#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
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
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Side = 128;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Type2DArray = 13;
constexpr std::size_t SurfaceBytes = 65536;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Initial = 0x5a5a5a5au;

alignas(256) constexpr std::array<std::uint32_t, 5> StoreCode{
    0x7e020280, 0x7e04020c, 0xf0201108, 0x00010200, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 9> LoadArrayCode{
    0x7e020280, 0x7e040280, 0xf0001128, 0x00010300, 0x34080082, 0xbf8c3f70, 0xe0701000, 0x80000304, 0xbf810000,
};

alignas(256) std::array<std::uint32_t, Threads> Output{};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

class GuestBlock {
public:
    explicit GuestBlock(bool watched, bool resident = false) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr && resident) {
            section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, BlockBytes, nullptr);
            Require(section != nullptr, "one-slice alias: cannot create the resident backing");
            GuestArena::GuestArenaMapDirect_nid_postfix(block, BlockBytes, section, 0, PAGE_READWRITE);
        } else if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
        static_cast<void>(watched);
#else
        static_cast<void>(resident);
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            if (watched) GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "one-slice alias: cannot allocate the guest block");
        Require(!watched || AgcDriver::GuestMemory::Watched(AddressOf(block), BlockBytes), "one-slice alias: the guest block is not write-watched");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
        if (section != nullptr) {
            GuestArena::GuestArenaForgetSection_nid_postfix(section);
            CloseHandle(section);
        }
#else
        munmap(block, BlockBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
#ifdef _WIN32
    HANDLE section = nullptr;
#endif
};

std::array<std::uint32_t, 8> TextureDescriptor(const std::uint8_t* texels, std::uint32_t type, std::uint32_t format = Format32UInt) {
    const auto address = AddressOf(texels);
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (TileR64KBX << 20u) | (type << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = AddressOf(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

void Dispatch(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, bool wait = true) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    if (wait) device.WaitIdle();
}

void Store(AgcDriver::VulkanDevice& device, const std::uint8_t* texels, std::uint32_t value) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto texture = TextureDescriptor(texels, Type2D);
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    userData[12] = value;
    Dispatch(device, StoreCode, userData);
}

void LoadArray(AgcDriver::VulkanDevice& device, const std::uint8_t* texels, std::uint32_t format = Format32UInt) {
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(16, 0u);
    const auto buffer = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    const auto texture = TextureDescriptor(texels, Type2DArray, format);
    std::copy(buffer.begin(), buffer.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    Dispatch(device, LoadArrayCode, userData);
}

void RequireOutput(std::uint32_t expected, const std::string& what) {
    for (std::uint32_t x = 0; x < Threads; ++x) {
        Require(Output[x] == expected, what + ": texel (" + std::to_string(x) + ", 0) reads " + Hex(Output[x]) + ", expected " + Hex(expected));
    }
}

void CheckCpuRefresh(AgcDriver::VulkanDevice& device, std::uint8_t* texels) {
    using namespace AgcDriver::GuestMemory;
    const std::lock_guard gpu(GpuMutex());
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, Initial);
    BumpCollectEpoch();
    const auto resource = AgcDriver::Graphics::DecodeTextureResource(TextureDescriptor(texels, Type2D));
    auto image = AgcDriver::Graphics::CachedStorageSurface(AgcDriver::VulkanDeviceTestAccess::ContextOf(device), resource);
    BumpCollectEpoch();
    Require(image->Refresh(), "a newly uploaded target did not refresh unchanged");
    const auto generation = WriteGeneration();
    for (int epoch = 0; epoch < 3; ++epoch) {
        BumpCollectEpoch();
        Require(image->Refresh(), "a clean target changed across refresh epochs");
    }
#ifdef _WIN32
    if (std::getenv("APS5_TARGET_CLEAN_PROOF") != nullptr) Require(WriteGeneration() == generation, "the clean target still walked its pages on refresh");
#else
    static_cast<void>(generation);
#endif
    const auto version = image->Version();
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, 0x34343434u);
    BumpCollectEpoch();
    Require(!image->Refresh() && image->Version() != version, "a CPU write was missed by the target refresh proof");
    LoadArray(device, texels);
    RequireOutput(0x34343434u, "CPU-written texels after a clean target proof");
    image.reset();
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
}

void Run(AgcDriver::VulkanDevice& device, std::uint8_t* texels, bool watched) {
    const auto surface = AddressOf(texels);
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, Initial);
    AgcDriver::GuestMemory::CollectWrites(surface, SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Store(device, texels, 0x11111111u);
    Require(StorageTexture::FindPending(surface, SurfaceBytes) != nullptr, "the 2D store left no pending image");
    LoadArray(device, texels);
    RequireOutput(0x11111111u, "the first load through the one-slice array");
    Store(device, texels, 0x22222222u);
    const auto written = StorageTexture::FindPending(surface, SurfaceBytes);
    Require(written != nullptr && written->Descriptor().dimension == AgcDriver::Graphics::TextureDimension::k2D, "the second 2D store left no pending 2D image");
    LoadArray(device, texels);
    RequireOutput(0x22222222u, "the second load through the one-slice array");
    if (watched && AgcDriver::GuestMemory::Watched(surface, SurfaceBytes)) {
        Require(StorageTexture::FindPending(surface, SurfaceBytes) == written, "the array image's refresh wrote the 2D image's results back instead of taking them on the device");
    } else {
        std::puts("the imported block is compared, not watched: taking the results on the device is not checked");
    }
    StorageTexture::FlushPending(surface, SurfaceBytes, nullptr, "test");
    device.WaitIdle();
    Require(std::count(reinterpret_cast<const std::uint32_t*>(texels), reinterpret_cast<const std::uint32_t*>(texels) + SurfaceBytes / 4u, 0x22222222u) == static_cast<std::ptrdiff_t>(Threads), "guest memory does not hold the 32 texels of the second store after the flush");
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
}

constexpr std::uint32_t DrawnValue = 0x3f400000u;
constexpr std::uint32_t Format32Float = 22;

alignas(256) constexpr std::array<std::uint32_t, 8> StorePairCode{
    0x7e020280, 0x7e040200, 0xf0201108, 0x00010200, 0x7e040201, 0xf0201108, 0x00030200, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 6> TargetVertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 5> TargetPixelCode{
    0x7e0002ff, DrawnValue, 0xf8001801, 0x00000000, 0xbf810000,
};

alignas(256) constexpr std::array<std::array<float, 4>, 3> TargetVertices{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f},
}};

void DrawPendingTarget(AgcDriver::VulkanDevice& device, std::uint8_t* texels) {
    const auto target = device.Target();
    std::vector<std::uint32_t> vertexData(4);
    auto vertexBuffer = BufferDescriptor(TargetVertices.data(), static_cast<std::uint32_t>(sizeof(TargetVertices)));
    vertexBuffer[1] |= 16u << 16u;
    vertexBuffer[2] = 3;
    vertexBuffer[3] = 0x01016facu;
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(TargetVertexCode.data()), std::as_bytes(std::span(TargetVertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(TargetVertexCode.data()), TargetVertexCode, 0, {}},
        {64, 0, vertexData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.targetOutputMode[0] = 7;
    pixel.targetExportMapping.fill(0xe4u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(TargetPixelCode.data()), std::as_bytes(std::span(TargetPixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(TargetPixelCode.data()), TargetPixelCode, 0, {}},
        {64, 0, {}, std::nullopt, pixel, std::nullopt, pixelMemory},
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
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
    state.color = {AddressOf(texels), {Side, Side}, VK_FORMAT_R32_UINT, SurfaceBytes, 0xe4u};
    state.color.tileMode = AgcDriver::Graphics::ColorTileMode::RenderTarget;
    state.color.uintExport = true;
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Side, Side};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Side), static_cast<float>(Side), -static_cast<float>(Side), 0, 1};
    state.scissor = {{8, 0}, {8, 1}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 1;
    state.blends = {state.blend};
    const AgcDriver::Pm4::DrawParameters draw{0, 3, 0, 1, 0, false};
    device.Draw(state, draw, shaders);
}

void CheckPendingTarget(AgcDriver::VulkanDevice& device, std::uint8_t* texels, bool submit, bool cpuOverwrite = false) {
    using namespace AgcDriver::Graphics;
    using namespace AgcDriver::GuestMemory;
    const auto surface = AddressOf(texels);
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, Initial);
    CollectWritesUncached(surface, SurfaceBytes);
    BumpCollectEpoch();
    std::lock_guard gpu(GpuMutex());
    std::vector<std::uint32_t> data(20);
    data[0] = 0x3e800000u;
    data[1] = 0x3f000000u;
    const auto targetWords = TextureDescriptor(texels, Type2D);
    const auto sourceWords = TextureDescriptor(texels, Type2D, Format32Float);
    std::copy(targetWords.begin(), targetWords.end(), data.begin() + 4);
    std::copy(sourceWords.begin(), sourceWords.end(), data.begin() + 12);
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    auto target = CachedStorageSurface(context, DecodeTextureResource(targetWords));
    auto source = CachedStorageSurface(context, DecodeTextureResource(sourceWords));
    for (int iteration = 0; iteration < 2; ++iteration) {
        data[1] = iteration == 0 ? 0x3f000000u : 0x3f800000u;
        Dispatch(device, StorePairCode, data, false);
        Require(target->Dirty() && source->Dirty(), "the paired image dispatch did not leave both images pending");
        if (submit) {
            const auto serial = device.SubmitAndEpoch();
            device.WaitRecorded(serial);
        }
        const bool profiled = LookupOutcomes::Profiled();
        const auto before = profiled ? ThreadLookupOutcomes().work[LookupOutcomes::PendingAliasUnits] : 0;
        const auto storedBefore = profiled ? ThreadLookupOutcomes().work[LookupOutcomes::WriteBackBytes] : 0;
        if (cpuOverwrite) {
            device.WaitIdle();
            std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, 0x45454545u);
        }
        BumpCollectEpoch();
        DrawPendingTarget(device, texels);
        const bool borrowed = !cpuOverwrite && std::getenv("APS5_PENDING_ALIAS_BORROW") != nullptr && std::getenv("APS5_NO_ALIAS_BORROW") == nullptr && std::getenv("APS5_NO_BLOCK_TRACKING") == nullptr && Watched(surface, SurfaceBytes);
        if (borrowed) {
            Require(source->Dirty() == false && target->Dirty(), "the draw did not take responsibility for the borrowed compute results");
            if (profiled) {
                Require(ThreadLookupOutcomes().work[LookupOutcomes::PendingAliasUnits] > before, "the paired pending-unit device copy did not fire");
                Require(ThreadLookupOutcomes().work[LookupOutcomes::WriteBackBytes] == storedBefore, "the paired pending-unit copy also wrote an image back");
            }
        } else if (profiled) {
            Require(ThreadLookupOutcomes().work[LookupOutcomes::PendingAliasUnits] == before, "pending units were borrowed despite a disabled path or a CPU overwrite");
        }
        const auto computeValue = cpuOverwrite ? 0x45454545u : data[1];
        LoadArray(device, texels, Format32Float);
        for (std::uint32_t x = 0; x < Threads; ++x) {
            const auto expected = x >= 8 && x < 16 ? DrawnValue : computeValue;
            Require(Output[x] == expected, "pending target lost a compute or draw write at x=" + std::to_string(x) + ": " + Hex(Output[x]) + ", expected " + Hex(expected));
        }
    }
    StorageTexture::FlushPending(surface, SurfaceBytes, nullptr, "test");
    device.WaitIdle();
    const ColorTargetLayout layout(Side, Side, ColorTileMode::RenderTarget, 4);
    for (std::uint32_t x = 0; x < Threads; ++x) {
        const auto expected = x >= 8 && x < 16 ? DrawnValue : (cpuOverwrite ? 0x45454545u : data[1]);
        Require(*reinterpret_cast<const std::uint32_t*>(texels + layout.Offset(x, 0)) == expected, "the resident result was not published to guest memory");
    }
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, 0x34343434u);
    BumpCollectEpoch();
    LoadArray(device, texels);
    RequireOutput(0x34343434u, "CPU overwrite after the pending-unit device copy");
    target.reset();
    source.reset();
    ClearCachedTextures(device.Device());
}

}

int main(int argc, char** argv) {
    try {
        const bool watched = AgcDriver::GuestMemory::WriteWatched();
        GuestBlock block(watched);
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (argc == 2 && std::string_view(argv[1]) == "--pending-target") {
            GuestBlock resident(watched, true);
            CheckPendingTarget(*device, resident.Data(), false);
            CheckPendingTarget(*device, resident.Data(), true);
            CheckPendingTarget(*device, resident.Data(), false, true);
            std::puts("pending render target alias tests passed");
            return 0;
        }
        if (!watched) std::puts("guest memory has no write watch: only the texels read through the array are checked");
        Run(*device, block.Data(), watched);
        if (watched) {
            GuestBlock resident(true, true);
            CheckCpuRefresh(*device, resident.Data());
        }
        std::puts("storage one-slice alias tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
