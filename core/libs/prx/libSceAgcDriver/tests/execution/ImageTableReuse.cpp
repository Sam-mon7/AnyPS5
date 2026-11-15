#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramOptions.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
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
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 4;
constexpr std::uint32_t TextureBytes = Width * Height * 4;
constexpr std::uint32_t Stride = 48;
constexpr std::uint32_t Records = 48;
constexpr std::uint32_t Moved = 64;
constexpr std::uint32_t Lanes = 64;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t TexturesOffset = 65536;
constexpr std::size_t BlockBytes = TexturesOffset + 2 * 65536;
constexpr std::size_t UnimportableBytes = BlockBytes - 256;
#ifdef _WIN32
constexpr std::uint64_t CpuWriteResolutions = 2;
#else
constexpr std::uint64_t CpuWriteResolutions = 1;
#endif

alignas(256) std::array<std::uint32_t, Records> Keys{};
alignas(256) std::array<std::uint32_t, Lanes * 4> Output{};
std::uint32_t* OutputData = Output.data();
bool LastTableMirrored = false;
alignas(256) std::array<std::uint32_t, 32> Srt{};

alignas(256) constexpr std::array<std::uint32_t, 28> TableCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0x8f108202u, 0xf4200444u,
    0x20000000u, 0x9312b011u, 0xf42c0802u, 0x24000000u, 0x360200bfu, 0x7e020d01u, 0x060202f0u, 0x100202ffu,
    0x3c800000u, 0x7e0402ffu, 0x3e000000u, 0xf09c8f08u, 0x01090401u, 0x7e120202u, 0x34101286u, 0x4a101100u,
    0x34101084u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 7> StoreCode{
    0x7e020d00u, 0x082802f7u, 0x7e3c0300u, 0x7e3e0280u, 0xf0201108u, 0x0001141eu, 0xbf810000u,
};

constexpr std::array<std::uint32_t, 4> PointClamp{0x92u, (4u * 256u) << 12u, 0u, 0u};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

std::array<std::uint32_t, 4> Image(const void* texels, std::uint32_t width = Width, std::uint32_t height = Height) {
    const auto address = AddressOf(texels);
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32Float << 20u) | (((width - 1u) & 3u) << 30u), ((width - 1u) >> 2u) | ((height - 1u) << 14u) | (1u << 31u), 0xfacu | (Type2D << 28u)};
}

std::array<std::uint32_t, 4> Buffer(const void* base, std::uint32_t bytes) {
    const auto address = AddressOf(base);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

class GuestBlock {
public:
    explicit GuestBlock(std::size_t registered = BlockBytes, std::size_t allocationBytes = BlockBytes) : bytes(allocationBytes) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536));
        if (block != nullptr) {
            if (AgcDriver::Graphics::VramWritable()) {
                section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, static_cast<DWORD>(bytes), nullptr);
                Require(section != nullptr, "cannot create shared image-table memory");
                GuestArena::GuestArenaMap_nid_postfix(block, bytes, section, 0, PAGE_READWRITE);
            } else GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
        }
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
        Require(block != nullptr, "image table reuse: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, registered, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
        if (section != nullptr) CloseHandle(section);
#else
        munmap(block, bytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, bytes);
#endif
    }

    std::uint32_t* Table() const { return reinterpret_cast<std::uint32_t*>(block); }
    float* Texels(std::uint32_t texture, std::size_t textureBytes = TextureBytes) const { return reinterpret_cast<float*>(block + TexturesOffset + static_cast<std::size_t>(texture) * textureBytes); }

private:
    std::uint8_t* block = nullptr;
    std::size_t bytes;
#ifdef _WIN32
    HANDLE section = nullptr;
#endif
};

void SetRecord(const GuestBlock& block, std::uint32_t record, const float* texels) {
    auto* words = block.Table() + record * (Stride / 4u);
    std::fill(words, words + Stride / 4u, 0u);
    std::copy(PointClamp.begin(), PointClamp.end(), words);
    const auto image = Image(texels);
    std::copy(image.begin(), image.end(), words + 4);
}

void SetRecord(const GuestBlock& block, std::uint32_t record, std::uint32_t texture) {
    SetRecord(block, record, block.Texels(texture));
}

void SetDccRecord(const GuestBlock& block, std::uint32_t record, std::uint32_t texture) {
    SetRecord(block, record, texture);
    const auto metadata = AddressOf(block.Texels(Records - 1)) + TextureBytes;
    auto* words = block.Table() + record * (Stride / 4u);
    const auto metadataAddress = metadata >> 8u;
    words[10] = (1u << 21u) | (static_cast<std::uint32_t>(metadataAddress & 0xffu) << 24u);
    words[11] = static_cast<std::uint32_t>(metadataAddress >> 8u);
    std::memset(reinterpret_cast<void*>(metadata), 0xff, 256);
    Require(AgcDriver::Graphics::DecodeTextureResource(std::span(words + 4, 8)).dccAddress == metadata, "image table reuse: the mixed-table descriptor has no DCC metadata");
}

void Fill(const GuestBlock& block, std::uint32_t texture, float bias) {
    for (std::uint32_t texel = 0; texel < Width * Height; ++texel) block.Texels(texture)[texel] = bias + static_cast<float>(texture) * 100.0f + static_cast<float>(texel);
}

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    const auto srtAddress = AddressOf(Srt.data());
    const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(srtAddress), static_cast<std::uint32_t>(srtAddress >> 32u)};
    const std::span<const std::uint32_t> code(TableCode);
    const std::span<const std::uint32_t> table(block.Table(), Records * Stride / 4u);
    const std::array<ShaderRecompiler::MemoryRegion, 4> memory{{
        {AddressOf(code.data()), std::as_bytes(code)},
        {srtAddress, std::as_bytes(std::span(Srt))},
        {AddressOf(table.data()), std::as_bytes(table)},
        {AddressOf(Keys.data()), std::as_bytes(std::span(Keys))},
    }};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {true, false, false}, false, 1};
    const ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, AddressOf(code.data()), code, 0, {}},
        {64, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    auto result = ShaderRecompiler::Recompile(request);
    const auto binding = std::find_if(result.bindings.begin(), result.bindings.end(), [](const ShaderRecompiler::DescriptorBinding& candidate) { return candidate.role == ShaderRecompiler::DescriptorRole::ImageTable; });
    Require(binding != result.bindings.end() && binding->guestDescriptor.size() == static_cast<std::size_t>(Records) * 8u, "image table reuse: the shader does not bind the whole table");
    return result;
}

void SetOutput(const void* output) {
    const auto view = Buffer(output, static_cast<std::uint32_t>(sizeof(Output)));
    std::copy(view.begin(), view.end(), Srt.begin() + 8);
}

void Store(AgcDriver::VulkanDevice& device, const GuestBlock& block, std::uint32_t texture) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto image = Image(block.Texels(texture));
    std::copy(image.begin(), image.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(StoreCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{AddressOf(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, AddressOf(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, AddressOf(code.data()));
    device.WaitIdle();
}

bool MirroredSurface(AgcDriver::VulkanDevice& device, std::uint64_t address, std::size_t bytes) {
    if (!AgcDriver::Graphics::VramTextures()) return false;
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    AgcDriver::Graphics::Context context{};
    context.device = device.Device();
    return AgcDriver::Graphics::MirrorSourceCovers(context, address, bytes);
}

bool MirroredTable(AgcDriver::VulkanDevice& device, const ShaderRecompiler::RecompileResult& result) {
    for (const auto& binding : result.bindings) {
        if (binding.role != ShaderRecompiler::DescriptorRole::ImageTable) continue;
        for (std::size_t offset = 0; offset + 8 <= binding.guestDescriptor.size(); offset += 8) {
            const auto resource = AgcDriver::Graphics::DecodeTextureResource(std::span(binding.guestDescriptor).subspan(offset, 8));
            if (MirroredSurface(device, resource.baseAddress, static_cast<std::size_t>(AgcDriver::Graphics::DescribeSurface(resource).guestBytes))) return true;
        }
    }
    return false;
}

std::size_t MirroredTextures(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    std::size_t count = 0;
    for (std::uint32_t texture = 0; texture < Records; ++texture) count += MirroredSurface(device, AddressOf(block.Texels(texture)), TextureBytes);
    return count;
}

std::vector<std::uint32_t> Dispatch(AgcDriver::VulkanDevice& device, const ShaderRecompiler::RecompileResult& result, std::uint32_t record) {
    Keys[0] = record;
    std::fill_n(OutputData, Output.size(), 0xdeadbeefu);
    device.Dispatch(result, 1, 1, 1, {}, AddressOf(TableCode.data()));
    device.WaitIdle();
    LastTableMirrored = MirroredTable(device, result);
    std::vector<std::uint32_t> red(Lanes);
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) red[lane] = OutputData[lane * 4u];
    return red;
}

std::vector<std::uint32_t> Row(const float* texels) {
    std::vector<std::uint32_t> row(Lanes);
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) row[lane] = std::bit_cast<std::uint32_t>(texels[lane % Width]);
    return row;
}

struct Step {
    std::uint64_t reused;
    std::uint64_t resolved;
};

Step Since(const AgcDriver::Graphics::ImageTableReuseCounts& before) {
    const auto now = AgcDriver::Graphics::ImageTableReuses();
    return {now.reused - before.reused, now.resolved - before.resolved};
}

void Expect(const AgcDriver::Graphics::ImageTableReuseCounts& before, std::uint64_t reused, std::uint64_t resolved, const std::string& what) {
    const auto step = Since(before);
    if (LastTableMirrored) {
        Require(step.reused == 0 && step.resolved != 0, "mirrored image table bypassed its required full lookup: " + what);
        return;
    }
    Require(step.reused == reused && step.resolved == resolved, "image table reuse: " + what + ": " + std::to_string(step.reused) + " reused and " + std::to_string(step.resolved) + " resolved tables, expected " + std::to_string(reused) + " and " + std::to_string(resolved));
}

void RunImageStore(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    for (std::uint32_t texture = 0; texture < Records; ++texture) {
        Fill(block, texture, 0.125f);
        SetRecord(block, texture, texture);
    }
    const auto srt = Buffer(block.Table(), Records * Stride);
    std::copy(srt.begin(), srt.end(), Srt.begin());
    const auto result = Compile(device, block);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto before = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, result, 25) == Row(block.Texels(25)), "image table reuse: the table before an image store sampled the wrong entry");
    Require(Dispatch(device, result, 27) == Row(block.Texels(27)), "image table reuse: the table resolved again before an image store sampled the wrong entry");
    Require(Dispatch(device, result, 23) == Row(block.Texels(23)), "image table reuse: the table reused before an image store sampled the wrong entry");
    Store(device, block, 23);
    std::vector<float> stored(Width);
    for (std::uint32_t x = 0; x < Width; ++x) stored[x] = -4.0f - static_cast<float>(x);
    Require(Dispatch(device, result, 23) == Row(stored.data()), "image table reuse: an entry an image store left results pending over kept its old texels");
    Expect(before, 3, CpuWriteResolutions + 1, "CPU writes to a new table's 64 KiB block, then a pending image store invalidates one entry while the other entries stay reusable");
    AgcDriver::Graphics::StorageTexture::FlushPending(AddressOf(block.Texels(23)), TextureBytes, nullptr, "test");
    device.WaitIdle();
    Require(Row(block.Texels(23)) == Row(stored.data()), "image table reuse: the image store wrote the wrong values");
}

void RunSelectivePendingWrite(AgcDriver::VulkanDevice& device, const GuestBlock& block, const GuestBlock& separate) {
    for (std::uint32_t texture = 0; texture < Records; ++texture) {
        Fill(block, texture, 0.25f);
        SetRecord(block, texture, texture);
    }
    Fill(separate, 0, 0.75f);
    SetRecord(block, 23, separate.Texels(0));
    const auto srt = Buffer(block.Table(), Records * Stride);
    std::copy(srt.begin(), srt.end(), Srt.begin());
    SetOutput(OutputData);
    const auto result = Compile(device, block);
    AgcDriver::GuestMemory::BumpCollectEpoch();

    auto before = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, result, 23) == Row(separate.Texels(0)), "image table reuse: the separate-block entry sampled the wrong texels");
    Require(Dispatch(device, result, 5) == Row(block.Texels(5)), "image table reuse: a sibling entry sampled the wrong texels");
    Expect(before, 1, 2, "a table whose images include a separate tracking block");

    const auto unwatch = AgcDriver::GuestMemory::UnwatchSerial();
    Store(device, separate, 0);
    Require(AgcDriver::GuestMemory::UnwatchSerial() == unwatch, "image table reuse: the selective-write fixture globally unwatched memory");
    if (const auto* across = std::getenv("APS5_TABLE_REUSE_ACROSS_EPOCHS"); across != nullptr && std::strcmp(across, "1") == 0) AgcDriver::GuestMemory::BumpCollectEpoch();
    std::vector<float> stored(Width);
    for (std::uint32_t x = 0; x < Width; ++x) stored[x] = -4.0f - static_cast<float>(x);
    before = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, result, 23) == Row(stored.data()), "image table reuse: a pending write to one tracking block kept that entry's old texels");
    Expect(before, 1, 1, "a pending write invalidating one table element while its siblings remain reusable");
    AgcDriver::Graphics::StorageTexture::FlushPending(AddressOf(separate.Texels(0)), TextureBytes, nullptr, "test");
    device.WaitIdle();
    Require(Row(separate.Texels(0)) == Row(stored.data()), "image table reuse: the separate-block image store wrote the wrong values");
}

void RunTests(AgcDriver::VulkanDevice& device, const GuestBlock& block, const GuestBlock& stores) {
    for (std::uint32_t texture = 0; texture < Records; ++texture) {
        Fill(block, texture, 0.25f);
        SetRecord(block, texture, texture);
    }
    Fill(block, Moved, 0.5f);
    for (std::uint32_t key = 0; key < Records; ++key) Keys[key] = key;
    const auto srt = Buffer(block.Table(), Records * Stride);
    const auto keys = Buffer(Keys.data(), static_cast<std::uint32_t>(sizeof(Keys)));
    std::copy(srt.begin(), srt.end(), Srt.begin());
    std::copy(keys.begin(), keys.end(), Srt.begin() + 4);
    SetOutput(OutputData);
    const auto result = Compile(device, block);
    const bool watched = AgcDriver::GuestMemory::WriteWatched();

    std::vector<std::uint32_t> unbumped[2];
    auto before = AgcDriver::Graphics::ImageTableReuses();
    std::thread thread([&] {
        unbumped[0] = Dispatch(device, result, 3);
        unbumped[1] = Dispatch(device, result, 3);
    });
    thread.join();
    Require(unbumped[0] == Row(block.Texels(3)) && unbumped[1] == Row(block.Texels(3)), "image table reuse: a thread without a collect epoch sampled the wrong entry");
    Expect(before, 0, 2, "a thread without a collect epoch");

    AgcDriver::GuestMemory::BumpCollectEpoch();
    before = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, result, 5) == Row(block.Texels(5)), "image table reuse: the first dispatch of the epoch sampled the wrong entry");
    Require(Dispatch(device, result, 9) == Row(block.Texels(9)), "image table reuse: a dispatch reusing the table sampled the wrong entry");
    Require(Dispatch(device, result, 5) == Row(block.Texels(5)), "image table reuse: the third dispatch of the epoch sampled the wrong entry");
    if (!watched) {
        Expect(before, 0, 3, "guest memory without a write watch");
        std::puts("guest memory has no write watch: image table reuse is not tested");
        return;
    }
    Expect(before, 2, 1, "three dispatches of one table in one epoch");

    before = AgcDriver::Graphics::ImageTableReuses();
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    Require(Dispatch(device, result, 7) == Row(block.Texels(7)), "image table reuse: the table after the texture cache was cleared sampled the wrong entry");
    Require(Dispatch(device, result, 9) == Row(block.Texels(9)), "image table reuse: the table resolved after the texture cache was cleared sampled the wrong entry");
    Expect(before, 1, 1, "a kept table whose textures the texture cache dropped");

    before = AgcDriver::Graphics::ImageTableReuses();
    std::vector<float> stored(Width * Height);
    for (std::uint32_t texel = 0; texel < Width * Height; ++texel) stored[texel] = -1.0f - static_cast<float>(texel);
    AgcDriver::GuestMemory::Write(AddressOf(block.Texels(11)), std::as_bytes(std::span(stored)));
    Require(Dispatch(device, result, 11) == Row(stored.data()), "image table reuse: an entry the driver stored over in the epoch kept its old texels");
    Require(Dispatch(device, result, 11) == Row(stored.data()), "image table reuse: the table resolved after a driver store sampled the wrong entry");
    Expect(before, 1, 1, "a driver store over an entry");

    before = AgcDriver::Graphics::ImageTableReuses();
    Fill(block, 17, 0.75f);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(Dispatch(device, result, 17) == Row(block.Texels(17)), "image table reuse: an entry the CPU wrote before the epoch kept its old texels");
    Require(Dispatch(device, result, 3) == Row(block.Texels(3)), "image table reuse: an entry stamped by another entry's collect sampled the wrong texels");
    Require(Dispatch(device, result, 17) == Row(block.Texels(17)), "image table reuse: the table of the new epoch sampled the wrong entry");
    Expect(before, 2, CpuWriteResolutions, "a CPU write collected in a new epoch, then two dispatches reusing unaffected entries");

    before = AgcDriver::Graphics::ImageTableReuses();
    SetRecord(block, 19, Moved);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto moved = Compile(device, block);
    Require(Dispatch(device, moved, 19) == Row(block.Texels(Moved)), "image table reuse: a record pointed at another texture sampled the old one");
    Require(Dispatch(device, result, 19) == Row(block.Texels(19)), "image table reuse: the original table sampled the moved record");
    Require(Dispatch(device, moved, 19) == Row(block.Texels(Moved)), "image table reuse: the moved table sampled the wrong entry when reused");
    Require(Dispatch(device, result, 21) == Row(block.Texels(21)), "image table reuse: the original table sampled the wrong entry when reused");
    Expect(before, 2, 2, "two tables that differ in one record");

    SetDccRecord(block, Records - 1, Records - 1);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto mixed = Compile(device, block);
    before = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, mixed, 5) == Row(block.Texels(5)), "image table reuse: the first dispatch of a mixed table sampled the wrong clean entry");
    Require(Dispatch(device, mixed, Records - 1) == Row(block.Texels(Records - 1)), "image table reuse: the DCC entry in a mixed table sampled the wrong texels");
    Expect(before, 1, 1, "a table with clean entries and one DCC entry that cannot be cached");

    SetOutput(block.Texels(13));
    const auto writer = Compile(device, block);
    SetOutput(OutputData);
    before = AgcDriver::Graphics::ImageTableReuses();
    static_cast<void>(Dispatch(device, writer, 7));
    const auto sampled = Dispatch(device, result, 13);
    const std::vector<float> written(block.Texels(13), block.Texels(13) + Width * Height);
    Require(std::bit_cast<std::uint32_t>(written[0]) == Row(block.Texels(7))[0], "image table reuse: the dispatch writing into an entry's texels stored the wrong values");
    Require(sampled == Row(written.data()), "image table reuse: an entry a dispatch wrote in the epoch kept its old texels");
    Expect(before, 1, 1, "a dispatch writing an entry's texels");
    Require(Dispatch(device, result, 15) == Row(block.Texels(15)), "image table reuse: the table after a dispatch wrote an entry sampled the wrong entry");

    RunImageStore(device, stores);
}

bool AcrossEpochs() {
    const auto* value = std::getenv("APS5_TABLE_REUSE_ACROSS_EPOCHS");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

void SampleInNewEpoch(AgcDriver::VulkanDevice& device, const ShaderRecompiler::RecompileResult& result, std::uint32_t record, const std::vector<std::uint32_t>& expected, const std::string& what) {
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(Dispatch(device, result, record) == expected, "image table reuse across epochs: " + what);
}

void SettleAcrossEpochs(AgcDriver::VulkanDevice& device, const ShaderRecompiler::RecompileResult& result, std::uint32_t record, const std::vector<std::uint32_t>& expected, const std::string& what) {
    for (std::uint32_t attempt = 0; attempt < 4; ++attempt) {
        const auto before = AgcDriver::Graphics::ImageTableReuses();
        SampleInNewEpoch(device, result, record, expected, what);
        if (LastTableMirrored) {
            Require(Since(before).reused == 0 && Since(before).resolved != 0, "mirrored image table skipped its full lookup across epochs");
            return;
        }
        if (Since(before).reused == 1) return;
    }
    Require(false, "image table reuse across epochs: " + what + ": the table was never kept");
}

void RunAcrossEpochs(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    for (std::uint32_t texture = 0; texture < Records; ++texture) {
        Fill(block, texture, 0.375f);
        SetRecord(block, texture, texture);
    }
    for (std::uint32_t key = 0; key < Records; ++key) Keys[key] = key;
    const auto srt = Buffer(block.Table(), Records * Stride);
    const auto keys = Buffer(Keys.data(), static_cast<std::uint32_t>(sizeof(Keys)));
    std::copy(srt.begin(), srt.end(), Srt.begin());
    std::copy(keys.begin(), keys.end(), Srt.begin() + 4);
    SetOutput(OutputData);
    const auto result = Compile(device, block);
    if (!AgcDriver::GuestMemory::WriteWatched()) {
        std::puts("guest memory has no write watch: image table reuse across epochs is not tested");
        return;
    }
    SettleAcrossEpochs(device, result, 1, Row(block.Texels(1)), "a warm-up dispatch sampled the wrong entry");

    auto before = AgcDriver::Graphics::ImageTableReuses();
    for (const std::uint32_t record : {5u, 9u, 5u}) SampleInNewEpoch(device, result, record, Row(block.Texels(record)), "a dispatch of a kept table in a new epoch sampled the wrong entry");
    Expect(before, 3, 0, "three dispatches of one kept table in three new epochs");

    before = AgcDriver::Graphics::ImageTableReuses();
    Fill(block, 17, 0.875f);
    SampleInNewEpoch(device, result, 17, Row(block.Texels(17)), "an entry the CPU wrote before the epoch kept its old texels");
    Require(Since(before).resolved >= 1, "image table reuse across epochs: a CPU write over an entry did not resolve the table again");
    SettleAcrossEpochs(device, result, 17, Row(block.Texels(17)), "the table resolved after a CPU write sampled the wrong entry");

    std::vector<float> stored(Width * Height);
    for (std::uint32_t texel = 0; texel < Width * Height; ++texel) stored[texel] = -2.0f - static_cast<float>(texel);
    before = AgcDriver::Graphics::ImageTableReuses();
    AgcDriver::GuestMemory::Write(AddressOf(block.Texels(11)), std::as_bytes(std::span(stored)));
    Require(Dispatch(device, result, 11) == Row(stored.data()), "image table reuse across epochs: an entry the driver stored over in the epoch kept its old texels");
    Require(Since(before).resolved >= 1, "image table reuse across epochs: a driver store over an entry did not resolve the table again");
    SettleAcrossEpochs(device, result, 11, Row(stored.data()), "the table resolved after a driver store sampled the wrong entry");

    for (std::uint32_t texel = 0; texel < Width * Height; ++texel) stored[texel] = -3.0f - static_cast<float>(texel);
    before = AgcDriver::Graphics::ImageTableReuses();
    AgcDriver::GuestMemory::Write(AddressOf(block.Texels(19)), std::as_bytes(std::span(stored)));
    SampleInNewEpoch(device, result, 19, Row(stored.data()), "an entry the driver stored over before the epoch kept its old texels");
    Require(Since(before).resolved >= 1, "image table reuse across epochs: a driver store before the epoch did not resolve the table again");

    SettleAcrossEpochs(device, result, 21, Row(block.Texels(21)), "the table before the texture cache was cleared sampled the wrong entry");
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    SampleInNewEpoch(device, result, 21, Row(block.Texels(21)), "the table after the texture cache and its held textures were cleared sampled the wrong entry");
    SettleAcrossEpochs(device, result, 23, Row(block.Texels(23)), "the table resolved after the texture cache was cleared sampled the wrong entry");

    SetOutput(block.Texels(13));
    const auto writer = Compile(device, block);
    SetOutput(OutputData);
    SettleAcrossEpochs(device, result, 15, Row(block.Texels(15)), "the table before a dispatch wrote an entry sampled the wrong entry");
    AgcDriver::GuestMemory::BumpCollectEpoch();
    static_cast<void>(Dispatch(device, writer, 7));
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto sampled = Dispatch(device, result, 13);
    const std::vector<float> written(block.Texels(13), block.Texels(13) + Width * Height);
    Require(std::bit_cast<std::uint32_t>(written[0]) == Row(block.Texels(7))[0], "image table reuse across epochs: the dispatch writing into an entry's texels stored the wrong values");
    Require(sampled == Row(written.data()), "image table reuse across epochs: an entry a dispatch wrote in an earlier epoch kept its old texels");
}

void RunPinBudget(AgcDriver::VulkanDevice& device, bool overflow) {
    const std::uint32_t side = overflow ? 128 : 16;
    const std::uint32_t pressureSide = overflow ? 256 : 128;
    const std::size_t bytes = 2 * TexturesOffset + Records * side * side * 4u;
    const std::size_t pressureBytes = 2 * TexturesOffset + Records * pressureSide * pressureSide * 4u;
    GuestBlock block(bytes - 256, bytes);
    GuestBlock pressure(pressureBytes - 256, pressureBytes);
    const auto prepare = [&](const GuestBlock& guest, std::uint32_t extent) {
        for (std::uint32_t record = 0; record < Records; ++record) {
            auto* texels = guest.Texels(record, extent * extent * 4u);
            std::fill_n(texels, extent * extent, 0.25f + static_cast<float>(record));
            auto* words = guest.Table() + record * (Stride / 4u);
            std::fill_n(words, Stride / 4u, 0u);
            std::copy(PointClamp.begin(), PointClamp.end(), words);
            const auto image = Image(texels, extent, extent);
            std::copy(image.begin(), image.end(), words + 4);
            Keys[record] = record;
        }
        const auto srt = Buffer(guest.Table(), Records * Stride);
        const auto keys = Buffer(Keys.data(), static_cast<std::uint32_t>(sizeof(Keys)));
        std::copy(srt.begin(), srt.end(), Srt.begin());
        std::copy(keys.begin(), keys.end(), Srt.begin() + 4);
        SetOutput(OutputData);
        return Compile(device, guest);
    };
    const auto result = prepare(block, side);
    const auto flood = prepare(pressure, pressureSide);
    const std::vector<std::uint32_t> expected(Lanes, std::bit_cast<std::uint32_t>(0.25f));
    SettleAcrossEpochs(device, result, 0, expected, "the table before eviction sampled the wrong entry");
    const auto budget = std::strtoull(std::getenv("APS5_TABLE_PIN_MIB"), nullptr, 10) << 20u;
    auto held = AgcDriver::Graphics::ImageTableReuses();
    Require(held.pinnedBytes == 0, "image table pins: cached textures were charged before eviction");
    Require(held.pinnedTables == (budget == 0 ? 0u : 1u), "image table pins: a zero budget retained a table or a nonzero budget did not pin it");
    SampleInNewEpoch(device, flood, 0, expected, "the eviction pressure table sampled the wrong entry");
    held = AgcDriver::Graphics::ImageTableReuses();
    Require(held.pinnedBytes <= budget, "image table pins: later cache eviction exceeded the pin budget");
    if (budget != 0 && !overflow) Require(held.pinnedBytes != 0, "image table pins: later cache eviction was not charged");
    const auto before = held;
    SampleInNewEpoch(device, result, 0, expected, "the table after eviction sampled the wrong entry");
    if (budget != 0 && !overflow) Expect(before, 1, 0, "a table pinned across cache eviction");
    else Require(Since(before).resolved == 1, "image table pins: a table still held textures past its budget");
    const auto beforeWrite = AgcDriver::Graphics::ImageTableReuses();
    std::fill_n(block.Texels(0), side * side, 0.75f);
    const std::vector<std::uint32_t> changed(Lanes, std::bit_cast<std::uint32_t>(0.75f));
    SampleInNewEpoch(device, result, 0, changed, "a CPU write after cache eviction kept old pinned texels");
    Require(Since(beforeWrite).resolved == 1, "image table pins: a CPU write after eviction did not invalidate the table");
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    held = AgcDriver::Graphics::ImageTableReuses();
    Require(held.pinnedBytes == 0 && held.pinnedTables == 0, "image table pins: device cache clear retained pins or their charge");
}

void RunImageStoreAcrossEpochs(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    for (std::uint32_t texture = 0; texture < Records; ++texture) {
        Fill(block, texture, 0.625f);
        SetRecord(block, texture, texture);
    }
    const auto srt = Buffer(block.Table(), Records * Stride);
    std::copy(srt.begin(), srt.end(), Srt.begin());
    const auto result = Compile(device, block);
    SettleAcrossEpochs(device, result, 25, Row(block.Texels(25)), "the table before an image store sampled the wrong entry");
    SampleInNewEpoch(device, result, 23, Row(block.Texels(23)), "the kept table before an image store sampled the wrong entry");
    Store(device, block, 23);
    std::vector<float> stored(Width);
    for (std::uint32_t x = 0; x < Width; ++x) stored[x] = -4.0f - static_cast<float>(x);
    SampleInNewEpoch(device, result, 23, Row(stored.data()), "an entry an image store left results pending over kept its old texels");
    AgcDriver::Graphics::StorageTexture::FlushPending(AddressOf(block.Texels(23)), TextureBytes, nullptr, "test");
    device.WaitIdle();
    Require(Row(block.Texels(23)) == Row(stored.data()), "image table reuse across epochs: the image store wrote the wrong values");
    SampleInNewEpoch(device, result, 23, Row(stored.data()), "an entry whose image store results were flushed sampled the wrong texels");
}

void RunSnapshotRetention(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    using namespace AgcDriver::Graphics;
    const auto* value = std::getenv("APS5_RELEASE_TEXTURE_SNAPSHOTS");
    const bool released = value != nullptr && std::strcmp(value, "1") == 0;
    for (std::uint32_t texture = 0; texture < Records; ++texture) {
        Fill(block, texture, 0.375f);
        SetRecord(block, texture, texture);
        Keys[texture] = texture;
    }
    const auto srt = Buffer(block.Table(), Records * Stride);
    const auto keys = Buffer(Keys.data(), static_cast<std::uint32_t>(sizeof(Keys)));
    std::copy(srt.begin(), srt.end(), Srt.begin());
    std::copy(keys.begin(), keys.end(), Srt.begin() + 4);
    SetOutput(OutputData);
    const auto result = Compile(device, block);
    SampleInNewEpoch(device, result, 5, Row(block.Texels(5)), "snapshot retention sampled the wrong entry");
    auto use = TextureCacheUsage();
    Require(use.sampledEntries == Records, "snapshot retention did not cache every test texture");
    auto mirrored = MirroredTextures(device, block);
    Require(use.snapshotBytes == (released ? 0u : (Records - mirrored) * TextureBytes), "snapshot retention kept the wrong CPU byte count");
    Require(use.omittedSnapshotBytes == (released ? (Records - mirrored) * TextureBytes : 0u), "snapshot retention omitted-byte accounting is wrong");
    Fill(block, 5, 0.875f);
    SampleInNewEpoch(device, result, 5, Row(block.Texels(5)), "a released snapshot hid a CPU write");

    ClearCachedTextures(device.Device());
    Require(TextureCacheUsage().snapshotBytes == 0 && TextureCacheUsage().omittedSnapshotBytes == 0, "cache clear kept snapshot byte charges");
    SetDccRecord(block, Records - 1, Records - 1);
    auto mixed = Compile(device, block);
    const auto binding = std::find_if(mixed.bindings.begin(), mixed.bindings.end(), [](const ShaderRecompiler::DescriptorBinding& candidate) { return candidate.role == ShaderRecompiler::DescriptorRole::ImageTable; });
    const auto* dccWords = block.Table() + (Records - 1u) * (Stride / 4u) + 4u;
    std::copy_n(dccWords, 8, binding->guestDescriptor.end() - 8);
    Require(DecodeTextureResource(std::span(binding->guestDescriptor).last(8)).dccAddress != 0, "snapshot retention fixture did not supply DCC metadata to the driver");
    SampleInNewEpoch(device, mixed, Records - 1, Row(block.Texels(Records - 1)), "snapshot retention changed DCC texels");
    use = TextureCacheUsage();
    mirrored = MirroredTextures(device, block);
    const auto mirroredDcc = MirroredSurface(device, AddressOf(block.Texels(Records - 1)), TextureBytes);
    Require(use.snapshotBytes == (released ? (mirroredDcc ? 0u : TextureBytes) : (Records - mirrored) * TextureBytes), "snapshot retention DCC CPU bytes: " + std::to_string(use.snapshotBytes) + " retained, " + std::to_string(use.omittedSnapshotBytes) + " omitted");

    AgcDriver::GuestMemory::Unwatch(AddressOf(block.Texels(0)), Records * TextureBytes);
    Require(AgcDriver::GuestMemory::CollectWrites(AddressOf(block.Texels(0)), TextureBytes) == 0, "snapshot retention fixture is still watched");
    Fill(block, 5, 0.625f);
    SampleInNewEpoch(device, mixed, 5, Row(block.Texels(5)), "a released snapshot hid a write after unwatching");
    use = TextureCacheUsage();
    mirrored = MirroredTextures(device, block);
    Require(use.snapshotBytes == (Records - mirrored) * TextureBytes && use.omittedSnapshotBytes == 0, "unprovable snapshots did not retain comparison bytes");
    ClearCachedTextures(device.Device());
    Require(TextureCacheUsage().snapshotBytes == 0 && TextureCacheUsage().omittedSnapshotBytes == 0, "final cache clear kept snapshot byte charges");
}

}

int main(int argc, char** argv) {
    try {
        std::unique_ptr<GuestBlock> writableOutput;
        if (AgcDriver::Graphics::VramWritable()) {
            writableOutput = std::make_unique<GuestBlock>();
            OutputData = writableOutput->Table();
        }
        GuestBlock block;
        GuestBlock stores(UnimportableBytes);
        GuestBlock separate(UnimportableBytes);
        GuestBlock selective(UnimportableBytes);
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (argc == 2 && std::strcmp(argv[1], "--snapshot-retention") == 0) {
            if (!AgcDriver::GuestMemory::WriteWatched()) return VulkanTestSkipped;
            RunSnapshotRetention(*device, selective);
            std::puts("snapshot retention tests passed");
            return 0;
        }
        if (argc == 2 && (std::strcmp(argv[1], "--pin-budget") == 0 || std::strcmp(argv[1], "--pin-overflow") == 0)) {
            RunPinBudget(*device, std::strcmp(argv[1], "--pin-overflow") == 0);
            std::puts("image table pin budget tests passed");
            return 0;
        }
        if (AcrossEpochs()) {
            RunAcrossEpochs(*device, block);
            RunImageStoreAcrossEpochs(*device, stores);
            RunSelectivePendingWrite(*device, selective, separate);
            std::puts("image table reuse across epochs tests passed");
            return 0;
        }
        RunTests(*device, block, stores);
        RunSelectivePendingWrite(*device, selective, separate);
        std::puts("image table reuse tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
