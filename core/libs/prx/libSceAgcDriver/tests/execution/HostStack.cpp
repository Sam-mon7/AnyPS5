#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/HostStack.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "SceTypes.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using GuestFiberEntry = void (APS5_VABI*)(std::uint64_t, std::uint64_t);
extern "C" {
std::int32_t APS5_VABI _sceFiberInitializeImpl_nid_postfix(FiberObject*, const char*, GuestFiberEntry, std::uint64_t, void*, std::uint64_t, const void*, std::uint32_t);
std::int32_t APS5_VABI sceFiberFinalize(FiberObject*);
std::int32_t APS5_VABI sceFiberRun_nid_postfix(FiberObject*, std::uint64_t, std::uint64_t*);
std::int32_t APS5_VABI sceFiberReturnToThread(std::uint64_t, std::uint64_t*);
}

namespace {

using AgcDriver::Graphics::Require;
constexpr std::uint64_t Fill = 0x5aa5c33c5aa5c33cull;
alignas(4096) std::array<std::uint64_t, 65536 / sizeof(std::uint64_t)> FiberContext;
alignas(16) unsigned char FiberStorage[0x100];
std::exception_ptr failure;
thread_local int threadValue = 0;

[[gnu::noinline]] void TouchStack() {
    volatile unsigned char bytes[512 * 1024];
    for (std::size_t index = 0; index < sizeof(bytes); index += 4096) bytes[index] = 73;
    for (std::size_t index = 0; index < sizeof(bytes); index += 4096) Require(bytes[index] == 73, "host stack contents changed");
}

void APS5_VABI RunOnFiber(std::uint64_t stackBytes, std::uint64_t) {
    try {
        const auto thread = std::this_thread::get_id();
        threadValue = 29;
        Require(!AgcDriver::OnHostStack(), "fiber started on the driver stack");
#ifdef _WIN32
        const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        const auto* base = tib->StackBase;
        const auto* limit = tib->StackLimit;
        const auto* deallocation = *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(tib) + 0x1478);
#endif
        auto call = [&] {
            Require(AgcDriver::OnHostStack(), "driver call did not change stacks");
            Require(std::this_thread::get_id() == thread && threadValue == 29, "driver call changed thread-local state");
            TouchStack();
            AgcDriver::WithHostStack([] { TouchStack(); });
            threadValue = 31;
        };
        AgcDriver::WithHostStack(call);
        Require(threadValue == 31 && !AgcDriver::OnHostStack(), "driver call did not return to the fiber");
        if (stackBytes >= 65536) {
            bool caught = false;
            try {
                AgcDriver::WithHostStack([] { throw std::runtime_error("host stack exception"); });
            } catch (const std::runtime_error& error) {
                caught = std::string(error.what()) == "host stack exception";
            }
            Require(caught && !AgcDriver::OnHostStack(), "driver exception did not return to the fiber");
        }
        alignas(256) static constexpr std::array<std::uint32_t, 8> code{0x7e000208u, 0x7e020280u, 0x7e040280u, 0x7e0602f2u, 0xf80008cfu, 0x03020100u, 0xbf810000u, 0xbf810000u};
        struct Header {
            Shader shader{};
            ShaderUserData users{};
        } header;
        header.shader.file_header = 0x34333231u;
        header.shader.version = 0x18;
        header.shader.header_size = sizeof(header);
        header.shader.shader_size = sizeof(code);
        header.shader.code = code.data();
        header.shader.type = 2;
        header.shader.user_data = &header.users;
        AgcDriverRegisterShader_nid_postfix(&header.shader);
        AgcDriverWaitIdle_nid_postfix();
#ifdef _WIN32
        Require(tib->StackBase == base && tib->StackLimit == limit && *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(tib) + 0x1478) == deallocation, "driver call did not restore Windows stack bounds");
#endif
    } catch (...) {
        failure = std::current_exception();
    }
    sceFiberReturnToThread(0, nullptr);
}

}

int main() {
    try {
        if (!OpenVulkanTestDevice()) return VulkanTestSkipped;
        for (const std::size_t stackBytes : {16384, 65536}) {
            FiberContext.fill(Fill);
            auto* fiber = reinterpret_cast<FiberObject*>(FiberStorage);
            Require(_sceFiberInitializeImpl_nid_postfix(fiber, "driver stack", RunOnFiber, stackBytes, FiberContext.data(), stackBytes, nullptr, 0) == 0, "fiber initialization failed");
#ifdef _WIN32
            DWORD previous = 0;
            Require(VirtualProtect(FiberContext.data(), 4096, PAGE_NOACCESS, &previous) != 0, "fiber guard installation failed");
#endif
            Require(sceFiberRun_nid_postfix(fiber, 0, nullptr) == 0, "fiber execution failed");
#ifdef _WIN32
            Require(VirtualProtect(FiberContext.data(), 4096, previous, &previous) != 0, "fiber guard removal failed");
#endif
            const auto words = stackBytes / sizeof(std::uint64_t);
            std::size_t untouched = 0;
            while (untouched < words && FiberContext[untouched] == Fill) ++untouched;
            const auto used = (words - untouched) * sizeof(std::uint64_t);
            const auto finalized = sceFiberFinalize(fiber);
            if (failure) std::rethrow_exception(failure);
            Require(finalized == 0, "fiber finalization failed: " + std::to_string(finalized));
            Require(used < stackBytes / 2, "driver calls used too much fiber stack: " + std::to_string(used));
            std::cout << "Driver calls and a 512 KiB workload used " << used << " bytes of a " << stackBytes << " byte fiber\n";
        }
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
