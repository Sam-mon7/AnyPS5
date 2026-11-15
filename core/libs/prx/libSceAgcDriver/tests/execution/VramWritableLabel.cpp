#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramOptions.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#endif

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context Context(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

int main() {
#ifndef _WIN32
    return VulkanTestSkipped;
#else
    try {
        using namespace AgcDriver::Graphics;
        namespace Memory = AgcDriver::GuestMemory;
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::unique_lock lock(Memory::GpuMutex());
        const auto context = AgcDriver::VulkanDeviceTestAccess::Context(*device);
        if (context.hostImportAlignment == 0 || context.hostImportAlignment > 65536) {
            Require(std::getenv("ANYPS5_REQUIRE_VULKAN") == nullptr, "VRAM label test requires host imports aligned within 64 KiB");
            return VulkanTestSkipped;
        }
        constexpr std::size_t bytes = 65536;
        auto* labels = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Require(labels != nullptr, "cannot allocate the imported completion label");
        const auto labelAddress = reinterpret_cast<std::uint64_t>(labels);
        GuestAllocations::Mutation().Add(labels, bytes, true, true, true);
        Require(HostImportFor(context, labelAddress, bytes) != nullptr, "completion label was not imported before the mirror build");
        void* raw = GuestArena::GuestArenaAllocate_nid_postfix(bytes, bytes);
        HANDLE section = nullptr;
        if (VramWritable()) {
            section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
            Require(section != nullptr, "cannot create shared writable label-fixture memory");
            GuestArena::GuestArenaMap_nid_postfix(raw, bytes, section, 0, PAGE_READWRITE);
        } else GuestArena::GuestArenaCommit_nid_postfix(raw, bytes, PAGE_READWRITE, bytes);
        std::memset(raw, 0x11, bytes);
        const auto address = reinterpret_cast<std::uint64_t>(raw);
        GuestAllocations::Mutation().Add(raw, bytes, true, true, true);
        alignas(256) constexpr std::array<std::uint32_t, 45> code{
            0x34020082, 0x34040084, 0x340c0081, 0x340e0083, 0x4a060200, 0x7e080201, 0x4a1806ff, 0x00000204,
            0x7e1a0201, 0x4a1c04ff, 0x00001000, 0x4a1000ff, 0xa1b2c300, 0x4a1202ff, 0x51000000, 0x4a1404ff,
            0x00c0ffee, 0x4a160eff, 0x7e570000, 0xdc708000, 0x00000801, 0xdc700100, 0x007d0903, 0xdc708ffc,
            0x007d0a0c, 0xdc608400, 0x00000800, 0xdc688480, 0x00000906, 0xdc708601, 0x00000b07, 0xdc788000,
            0x0000080e, 0xdc748400, 0x0000080e, 0xdc7c8800, 0x0000090e, 0xdc308000, 0x0f000001, 0xdc708500,
            0x00000f01, 0x7da80090, 0xdc708300, 0x00000a01, 0xbf810000,
        };
        const std::array<std::uint32_t, 8> userData{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)};
        const std::array<ShaderRecompiler::MemoryRegion, 1> regions{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
        const ShaderRecompiler::ShaderComputeStageInfo compute{{64, 1, 1}, 0, {false, false, false}, false, 1};
        ShaderRecompiler::RecompileRequest request{
            {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
            {32, 0, userData, compute, std::nullopt, std::nullopt, regions},
            device->Target(),
            {0, 0, 0, 128}
        };
        request.useCache = false;
        const auto shader = ShaderRecompiler::Recompile(request);
        Require(shader.bdaWrites, "label fixture lost BDA write metadata");
        device->Dispatch(shader, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
        Require(HasVramMirrorWriters() == VramWritable(), "label fixture did not establish the selected writer backing");
        if (VramWritable()) Require(HostImportFor(context, address, bytes) == nullptr, "label fixture imported its writable mirror");
        const std::uint32_t label = 0x76543210u;
        const auto payload = std::as_bytes(std::span(&label, 1));
        const auto reason = device->WriteLabelOnGpu(labelAddress, payload, 1, 0, false);
        Require(reason == (VramWritable() ? 2 : 0), "imported disjoint label did not follow the mirror completion gate");
        if (VramWritable()) Require(*static_cast<volatile std::uint32_t*>(labels) == 0, "imported label became visible before mirror publication");
        device->WaitIdle();
        if (reason == 2) Memory::Write(labelAddress, payload, 4);
        Require(*static_cast<volatile std::uint32_t*>(labels) == label, "completion label was not published after the caller drained");
        Require(*static_cast<volatile std::uint32_t*>(raw) == 0xa1b2c300u, "native read after the imported label missed the BDA store");
        Require(!HasVramMirrorWriters(), "the completed label kept an active mirror writer");
        if (device->CanWaitUnlocked()) {
            const std::uint32_t initial = 0x11111111u;
            Memory::Write(address, std::as_bytes(std::span(&initial, 1)));
            device->Dispatch(shader, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
            const auto serial = device->SubmitAndEpoch();
            lock.unlock();
            device->WaitRecorded(serial);
            lock.lock();
            Require(HasVramMirrorWriters() == VramWritable(), "waiting for the GPU fence unexpectedly reaped mirror publication");
            const auto destination = labelAddress + 4096;
            const auto copied = device->CopyBuffer(destination, address, sizeof(std::uint32_t), sizeof(std::uint32_t), sizeof(std::uint32_t), 0, 0, 1, [](std::span<const std::byte>, std::uint64_t) {});
            if (VramWritable()) Require(copied.path != 0 && copied.synced && copied.reason == AgcDriver::VulkanDevice::CopyOutcome::SourceUnsettled, "CPU copy accepted a signaled but unpublished BDA mirror writer");
            else Require(copied.path == 0 && !copied.synced, "imported BDA store lost the signaled CPU-copy fast path");
            if (copied.path == 2) {
                std::array<std::byte, sizeof(std::uint32_t)> value{};
                Memory::Read(address, value);
                Memory::Write(destination, value);
            }
            device->WaitIdle();
            Require(*reinterpret_cast<volatile std::uint32_t*>(destination) == 0xa1b2c300u, "copy after a signaled BDA writer read stale guest bytes");
        }
        GuestAllocations::Mutation().Remove(raw);
        GuestAllocations::Mutation().Remove(labels);
        ClearImageMirrors(context.device);
        ClearHostImports(context.device);
        GuestArena::GuestArenaReset_nid_postfix(raw, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(raw, bytes);
        if (section != nullptr) CloseHandle(section);
        VirtualFree(labels, 0, MEM_RELEASE);
        std::cout << "VRAM imported completion label tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#endif
}
