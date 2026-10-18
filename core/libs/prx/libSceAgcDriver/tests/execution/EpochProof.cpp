#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/RecordProof.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>

namespace {
using namespace AgcDriver::DriverDetail;
using namespace AgcDriver::PageTracker;
constexpr std::size_t InputBytes = 0x4000, OutputBytes = 0x10000;
alignas(256) constexpr std::array<std::uint32_t, 9> Code{0xf4000200u, 0xfa000000u, 0xbf8cc07fu, 0x34020082u, 0x7e040208u, 0x4a040481u, 0xe0701000u, 0x80010201u, 0xbf810000u};
std::uint64_t observedInput = 0;
std::atomic<std::uint64_t> captureReads{0};

void observeRead(std::uint64_t address, std::size_t bytes) {
    if (AgcDriver::GuestMemory::CurrentReadSite() == AgcDriver::GuestMemory::ReadSite::Capture && address < observedInput + InputBytes && observedInput < address + bytes) captureReads.fetch_add(1, std::memory_order_relaxed);
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void dispatch(const std::uint32_t* input, std::uint32_t* output, std::uint32_t expected) {
    const auto code = reinterpret_cast<std::uintptr_t>(Code.data());
    const auto source = reinterpret_cast<std::uintptr_t>(input);
    const auto destination = reinterpret_cast<std::uintptr_t>(output);
    std::vector<std::uint32_t> words{
        0xc0027600u, 0x20cu, static_cast<std::uint32_t>(code >> 8u), static_cast<std::uint32_t>(code >> 40u),
        0xc0037600u, 0x207u, 32u, 1u, 1u,
        0xc0017600u, 0x213u, 8u << 1u,
        0xc0087600u, 0x240u, static_cast<std::uint32_t>(source), static_cast<std::uint32_t>(source >> 32u), 128u, 0x31016facu,
        static_cast<std::uint32_t>(destination), static_cast<std::uint32_t>((destination >> 32u) & 0xffffu), 128u, 0x31016facu,
        0xc0031500u, 1u, 1u, 1u, 0x8041u,
    };
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    require(sceAgcDriverSubmitDcb(&packet) == 0, "epoch proof dispatch submit failed");
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(destination, 128);
    for (std::size_t lane = 0; lane < 32; ++lane) require(output[lane] == expected, "epoch proof reused stale scalar data");
}

void run() {
    Initialize();
    auto* input = static_cast<std::uint32_t*>(GuestArena::GuestArenaAllocate_nid_postfix(InputBytes, RegionBytes));
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, InputBytes, nullptr);
    require(section != nullptr, "epoch proof input section allocation failed");
    GuestArena::GuestArenaMap_nid_postfix(input, InputBytes, section, 0, PAGE_READWRITE);
    CloseHandle(section);
    auto* output = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, OutputBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(output != nullptr, "epoch proof output allocation failed");
    GuestAllocations::Mutation().Add(input, InputBytes, true, true, false);
    GuestAllocations::Mutation().Add(output, OutputBytes, true, true, true);
    input[0] = 7;
    const auto address = reinterpret_cast<std::uintptr_t>(input);
    std::array<void*, 16> pages{};
    std::size_t count = pages.size();
    require(GuestArena::GuestArenaCollectWrites_nid_postfix(address, InputBytes, pages.data(), &count, true), "epoch proof section observation failed");
    Shader shader{};
    shader.file_header = 0x34333231;
    shader.version = 0x18;
    shader.header_size = sizeof(Shader);
    shader.shader_size = sizeof(Code);
    shader.code = Code.data();
    AgcDriverRegisterShader_nid_postfix(&shader);
    observedInput = address;
    AgcDriver::GuestMemory::SetReadGuard(observeRead);
    for (int warm = 0; warm < 4; ++warm) dispatch(input, output, 8);
    require(captureReads.load(std::memory_order_relaxed) != 0, "dispatch fixture did not capture the protected input");
    if (RecordProof::Enabled()) {
        const auto before = RecordProof::Counts();
        const auto readsBefore = captureReads.load(std::memory_order_relaxed);
        require(before.recorded != 0 && before.results[static_cast<std::size_t>(RecordProofResult::Accepted)] != 0, "dispatch did not warm an epoch proof");
        DWORD previous = 0;
        require(VirtualProtect(input, InputBytes, PAGE_NOACCESS, &previous), "epoch proof noaccess protection failed");
        try {
            dispatch(input, output, 8);
        } catch (...) {
            DWORD ignored = 0;
            VirtualProtect(input, InputBytes, previous, &ignored);
            throw;
        }
        MEMORY_BASIC_INFORMATION state{};
        const auto queried = VirtualQuery(input, &state, sizeof(state));
        DWORD ignored = 0;
        require(VirtualProtect(input, InputBytes, previous, &ignored), "epoch proof protection restore failed");
        const auto after = RecordProof::Counts();
        require(queried == sizeof(state) && state.Protect == PAGE_NOACCESS && after.results[0] == before.results[0] + 1 && captureReads.load(std::memory_order_relaxed) == readsBefore, "dispatch lookup read captured guest pages");
    }
    const auto beforeCpu = RecordProof::Counts();
    {
        GuestArena::HostWrite write(input, sizeof(*input));
        require(write.Open(), "epoch proof CPU write boundary failed");
        input[0] = 11;
    }
    dispatch(input, output, 12);
    if (RecordProof::Enabled()) require(RecordProof::Counts().results[0] == beforeCpu.results[0], "CPU write did not invalidate dispatch proof");
    {
        GuestArena::HostWrite write(input, sizeof(*input));
        require(write.Open(), "epoch proof CPU restore boundary failed");
        input[0] = 7;
    }
    for (int warm = 0; warm < 3; ++warm) dispatch(input, output, 8);
    if (RecordProof::Enabled()) {
        const auto beforeGpu = RecordProof::Counts();
        GpuWrite({address, sizeof(*input)});
        dispatch(input, output, 8);
        require(RecordProof::Counts().results[0] == beforeGpu.results[0], "GPU declaration did not invalidate dispatch proof");
        Get()->InvalidateRange({address, InputBytes});
        const auto beforeUnknown = RecordProof::Counts();
        dispatch(input, output, 8);
        require(RecordProof::Counts().results[static_cast<std::size_t>(RecordProofResult::Coverage)] > beforeUnknown.results[static_cast<std::size_t>(RecordProofResult::Coverage)], "unknown coverage did not fall back in dispatch lookup");
    } else {
        require(RecordProof::Counts() == RecordProofCounts{} && RecordProof::ProfileLine().empty(), "off-mode dispatch changed proof counters or output");
    }
    AgcDriverShutdown_nid_postfix();
    AgcDriver::GuestMemory::SetReadGuard(nullptr);
    GuestAllocations::Mutation().Remove(input);
    GuestAllocations::Mutation().Remove(output);
    GuestArena::GuestArenaReset_nid_postfix(input, InputBytes);
    GuestArena::GuestArenaRelease_nid_postfix(input, InputBytes);
    VirtualFree(output, 0, MEM_RELEASE);
}
}
#endif

int main() {
#ifdef _WIN32
    try {
        if (!AgcDriver::PageTracker::Enabled()) return VulkanTestSkipped;
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        run();
        std::cout << "epoch proof dispatch lookup passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        AgcDriver::GuestMemory::SetReadGuard(nullptr);
        return 1;
    }
#else
    return VulkanTestSkipped;
#endif
}
