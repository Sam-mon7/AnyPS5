#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

using DepthFastClearHarness::Compile;
using DepthFastClearHarness::Covered;
using DepthFastClearHarness::Draw;
using DepthFastClearHarness::HtileBytes;
using DepthFastClearHarness::Require;
using DepthFastClearHarness::Shaders;
using DepthFastClearHarness::Surface;

constexpr auto HtileFillBytes = static_cast<std::uint32_t>(HtileBytes);

class GuestBlock {
public:
    GuestBlock() {
        constexpr std::size_t bytes = 65536;
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(bytes, bytes));
#endif
        Require(block != nullptr, "depth fast clear submit: cannot allocate a guest block");
        std::memset(block, 0, bytes);
        GuestAllocations::Mutation().Add(block, bytes, true, true, true);
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    std::uint8_t* block = nullptr;
};

GuestBlock& NewBlock() {
    static auto* blocks = new std::deque<GuestBlock>();
    return blocks->emplace_back();
}

std::uint32_t Low(std::uint64_t value) { return static_cast<std::uint32_t>(value); }

std::uint32_t High(std::uint64_t value) { return static_cast<std::uint32_t>(value >> 32u); }

std::vector<std::uint32_t> Pm4Packet(std::uint32_t opcode, std::initializer_list<std::uint32_t> body) {
    std::vector<std::uint32_t> words{0xc0000000u | (static_cast<std::uint32_t>(body.size() - 1) << 16u) | (opcode << 8u)};
    words.insert(words.end(), body);
    return words;
}

void Submit(const std::vector<std::vector<std::uint32_t>>& packets, std::uint32_t queue = 0) {
    std::vector<std::uint32_t> words;
    for (const auto& packet : packets) words.insert(words.end(), packet.begin(), packet.end());
    Packet dcb{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require((queue == 0 ? sceAgcDriverSubmitDcb(&dcb) : sceAgcDriverSubmitAcb(queue, &dcb)) == 0, "depth fast clear submit: submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

void DmaFill(const GuestBlock& htile, std::uint32_t bytes, std::uint32_t pattern) {
    Submit({Pm4Packet(0x50, {0x40000000u, pattern, 0, Low(htile.Address()), High(htile.Address()), bytes})});
}

constexpr std::array<std::uint32_t, 9> FillKernel{0xd7460004u, 0x04010c08u, 0x7e000204u, 0x7e020205u, 0x7e040206u, 0x7e060207u, 0xe01c2000u, 0x80000004u, 0xbf810000u};
constexpr std::array<std::uint32_t, 17> HtileKernel{0xd7460000u, 0x04010c06u, 0x34000084u, 0xdc388000u, 0x04020000u, 0xbf8c3f70u, 0xd7710008u, 0x00120805u, 0xd7710009u, 0x00120a05u, 0xd771000au, 0x00120c05u, 0xd771000bu, 0x00120e05u, 0xdc788000u, 0x00020800u, 0xbf810000u};

std::atomic<std::uint64_t> observedArguments{0};
std::atomic<unsigned> argumentReads{0};

void ObserveArguments(std::uint64_t address, std::size_t bytes) {
    if (address == observedArguments.load() && bytes == 3u * sizeof(std::uint32_t)
        && AgcDriver::GuestMemory::CurrentReadSite() == AgcDriver::GuestMemory::ReadSite::IndirectArguments) ++argumentReads;
}

void DispatchHtile(const GuestBlock& code, const GuestBlock& htile, std::uint32_t offset, std::array<std::uint32_t, 3> count, std::uint32_t initiator, bool indirect, bool queuedCount, std::uint32_t lastThreads) {
    auto& arguments = NewBlock();
    auto& gate = NewBlock();
    std::memcpy(arguments.Data(), count.data(), sizeof(count));
    *reinterpret_cast<std::uint32_t*>(gate.Data()) = 1;
    if (queuedCount) std::memset(arguments.Data(), 0, sizeof(count));
    const auto kernel = code.Address();
    const auto base = htile.Address() + offset;
    std::vector<std::vector<std::uint32_t>> packets{
        Pm4Packet(0x76, {0x20c, Low(kernel >> 8u)}),
        Pm4Packet(0x76, {0x20d, High(kernel >> 8u) & 0xffu}),
        Pm4Packet(0x76, {0x207, (initiator & 2u) != 0 ? 64u | (lastThreads << 16u) : 64u, (initiator & 2u) != 0 ? 0x10001u : 1u, (initiator & 2u) != 0 ? 0x10001u : 1u}),
        Pm4Packet(0x76, {0x212, 0, (6u << 1u) | 0x80u}),
        Pm4Packet(0x76, {0x240, 0, 0, Low(base), High(base), 0, 0xff0u}),
    };
    if (indirect) {
        // A satisfied wait opens capture-ahead on the compute queue. With no preceding
        // argument writer the unresolved HTILE packet is parked before ordered dispatch.
        packets.push_back(Pm4Packet(0x3c, {0x13u, Low(gate.Address()), High(gate.Address()), 1u, 0xffffffffu, 0x19u}));
        if (queuedCount) packets.push_back(Pm4Packet(0x37, {0x00100200u, Low(arguments.Address()), High(arguments.Address()), count[0], count[1], count[2]}));
        packets.push_back(Pm4Packet(0x16, {Low(arguments.Address()), High(arguments.Address()), initiator}));
    } else {
        packets.push_back(Pm4Packet(0x15, {count[0], count[1], count[2], initiator}));
    }
    observedArguments = arguments.Address();
    argumentReads = 0;
    AgcDriver::GuestMemory::SetReadGuard(ObserveArguments);
    struct ResetReadGuard {
        ~ResetReadGuard() { AgcDriver::GuestMemory::SetReadGuard(nullptr); }
    } resetReadGuard;
    Submit(packets, indirect ? 0x20u : 0u);
    Require(!indirect || argumentReads.load() != 0, "the recognized GPU-indirect HTILE kernel did not resolve its actual argument counts");
}

void DispatchFill(const GuestBlock& code, const GuestBlock& htile, std::uint32_t bytes) {
    const auto kernel = code.Address();
    const auto base = htile.Address();
    Submit({
        Pm4Packet(0x76, {0x20c, Low(kernel >> 8u)}),
        Pm4Packet(0x76, {0x20d, High(kernel >> 8u) & 0xffu}),
        Pm4Packet(0x76, {0x207, 64}),
        Pm4Packet(0x76, {0x208, 1}),
        Pm4Packet(0x76, {0x209, 1}),
        Pm4Packet(0x76, {0x213, (8u << 1u) | 0x80u}),
        Pm4Packet(0x76, {0x240, Low(base)}),
        Pm4Packet(0x76, {0x241, (High(base) & 0xffffu) | (16u << 16u)}),
        Pm4Packet(0x76, {0x242, bytes / 16u}),
        Pm4Packet(0x76, {0x243, 0x4bfacu}),
        Pm4Packet(0x76, {0x244, 0}),
        Pm4Packet(0x76, {0x245, 0}),
        Pm4Packet(0x76, {0x246, 0}),
        Pm4Packet(0x76, {0x247, 0}),
        Pm4Packet(0x15, {bytes / 1024u, 1, 1, 0x41}),
    });
}

bool StoresFills(AgcDriver::VulkanDevice& device, const GuestBlock& probe) {
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    std::array<std::uint32_t, 4> pattern{};
    const bool stored = device.FillBuffer(probe.Address(), 4096, pattern);
    device.WaitIdle();
    return stored;
}

void DmaFillClearsDepth(AgcDriver::VulkanDevice& device, const Shaders& shaders) {
    const auto& htile = NewBlock();
    const Surface surface{NewBlock().Address(), 0, htile.Address(), false, VK_FORMAT_D32_SFLOAT};
    Require(Draw(device, shaders, surface, {.depthWrite = true}) == Covered, "a new depth surface was not cleared to DB_DEPTH_CLEAR (1.0)");
    Require(Draw(device, shaders, surface, {}) == 0u, "a draw at depth 0.5 passed LESS against the 0.5 written before it");
    DmaFill(htile, HtileFillBytes - 4u, 0u);
    Require(Draw(device, shaders, surface, {}) == 0u, "a DMA_DATA fill shorter than the HTILE cleared the depth");
    DmaFill(htile, HtileFillBytes, 0xffffffffu);
    Require(Draw(device, shaders, surface, {}) == 0u, "a DMA_DATA fill expanded to ZMask 0xf cleared the depth");
    DmaFill(htile, HtileFillBytes, 0u);
    Require(Draw(device, shaders, surface, {.depthWrite = true}) == Covered, "a DMA_DATA fill of 0 over the HTILE did not clear the depth to DB_DEPTH_CLEAR (1.0)");
    Require(Draw(device, shaders, surface, {}) == 0u, "the depth written after the DMA_DATA clear was lost");
}

void DispatchFillClearsDepth(AgcDriver::VulkanDevice& device, const Shaders& shaders) {
    const auto& htile = NewBlock();
    auto& code = NewBlock();
    std::memcpy(code.Data(), FillKernel.data(), sizeof(FillKernel));
    const Surface surface{NewBlock().Address(), 0, htile.Address(), false, VK_FORMAT_D32_SFLOAT};
    Require(Draw(device, shaders, surface, {.depthWrite = true}) == Covered, "a new depth surface was not cleared to DB_DEPTH_CLEAR (1.0)");
    Require(Draw(device, shaders, surface, {}) == 0u, "a draw at depth 0.5 passed LESS against the 0.5 written before it");
    DispatchFill(code, htile, HtileFillBytes);
    Require(Draw(device, shaders, surface, {.depthWrite = true}) == Covered, "a compute fill of 0 over the HTILE did not clear the depth to DB_DEPTH_CLEAR (1.0)");
    Require(Draw(device, shaders, surface, {}) == 0u, "the depth written after the compute fill clear was lost");
}

void DepthOnlyFillKeepsStencil(AgcDriver::VulkanDevice& device, const Shaders& shaders) {
    const auto& htile = NewBlock();
    const Surface surface{NewBlock().Address(), NewBlock().Address(), htile.Address(), true, VK_FORMAT_D32_SFLOAT_S8_UINT};
    const VkStencilOpState writeFive{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xffu, 0xffu, 5u};
    const VkStencilOpState equalFive{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xffu, 0u, 5u};
    const VkStencilOpState equalZero{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xffu, 0u, 0u};
    Require(Draw(device, shaders, surface, {.depthWrite = true, .stencilTest = true, .stencil = writeFive}) == Covered, "the stencil write over a new depth-stencil surface did not cover the target");
    DmaFill(htile, HtileFillBytes, 0x3f0u);
    Require(Draw(device, shaders, surface, {.depthTest = false, .stencilTest = true, .stencil = equalFive}) == Covered, "a depth-only fill (SMem 3) cleared the stencil");
    Require(Draw(device, shaders, surface, {}) == Covered, "a depth-only fill did not clear the depth to DB_DEPTH_CLEAR (1.0)");
    DmaFill(htile, HtileFillBytes, 0xf0u);
    Require(Draw(device, shaders, surface, {.depthTest = false, .stencilTest = true, .stencil = equalZero}) == Covered, "a depth and stencil fast clear (SMem 0) did not clear the stencil to 0");
}

void HtileKernelClearsSlices(AgcDriver::VulkanDevice& device, const Shaders& shaders) {
    Require(std::getenv("APS5_NO_GPU_INDIRECT") == nullptr, "HTILE GPU-indirect coverage requires APS5_NO_GPU_INDIRECT to be unset");
    auto& htile = NewBlock();
    auto& code = NewBlock();
    std::memcpy(code.Data(), HtileKernel.data(), sizeof(HtileKernel));
    const Surface first{NewBlock().Address(), 0, htile.Address(), false, VK_FORMAT_D32_SFLOAT};
    const Surface second{NewBlock().Address(), 0, htile.Address() + HtileBytes, false, VK_FORMAT_D32_SFLOAT};
    constexpr std::uint32_t groups = HtileFillBytes / (64u * 16u);
    struct Case {
        const char* name;
        std::uint32_t offset;
        std::array<std::uint32_t, 3> count;
        std::uint32_t initiator;
        bool indirect;
        bool queuedCount;
        bool first;
        bool second;
        std::uint32_t writtenBytes;
        std::uint32_t lastThreads = 64;
    };
    const std::array cases{
        Case{"direct first slice", 0, {groups, 1, 1}, 0x41, false, false, true, false, HtileFillBytes},
        Case{"direct second slice", HtileFillBytes, {groups, 1, 1}, 0x41, false, false, false, true, HtileFillBytes},
        Case{"direct whole array", 0, {groups * 2u, 1, 1}, 0x41, false, false, true, true, HtileFillBytes * 2u},
        Case{"thread dimensions", HtileFillBytes, {HtileFillBytes / 16u, 1, 1}, 0x61, false, false, false, true, HtileFillBytes},
        Case{"short thread dimensions", 0, {HtileFillBytes / 16u - 1u, 1, 1}, 0x61, false, false, false, false, HtileFillBytes - 16u},
        Case{"partial last group", 0, {groups, 1, 1}, 0x43, false, false, false, false, HtileFillBytes - 16u, 63},
        Case{"zero X", 0, {0, 1, 1}, 0x41, false, false, false, false, 0},
        Case{"zero Y", 0, {groups, 0, 1}, 0x41, false, false, false, false, 0},
        Case{"zero Z", 0, {groups, 1, 0}, 0x41, false, false, false, false, 0},
        Case{"GPU indirect ahead", HtileFillBytes, {groups, 1, 1}, 0x41, true, false, false, true, HtileFillBytes},
        Case{"GPU indirect queued arguments", 0, {groups * 2u, 1, 1}, 0x41, true, true, true, true, HtileFillBytes * 2u},
        Case{"GPU indirect zero", 0, {groups, 0, 1}, 0x41, true, false, false, false, 0},
    };
    for (const auto& test : cases) {
        const auto message = std::string("HTILE kernel ") + test.name;
        Require(Draw(device, shaders, first, {.depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS}) == Covered, message + ": first setup failed");
        Require(Draw(device, shaders, second, {.depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS}) == Covered, message + ": second setup failed");
        Require(Draw(device, shaders, first, {}) == 0u && Draw(device, shaders, second, {}) == 0u, message + ": depth writes were not retained");
        std::memset(htile.Data(), 0xff, HtileBytes * 2u);
        DispatchHtile(code, htile, test.offset, test.count, test.initiator, test.indirect, test.queuedCount, test.lastThreads);
        Require(Draw(device, shaders, first, {}) == (test.first ? Covered : 0u), message + ": first slice pixels differ");
        Require(Draw(device, shaders, second, {}) == (test.second ? Covered : 0u), message + ": second slice pixels differ");
        const auto* metadata = reinterpret_cast<const std::uint32_t*>(htile.Data());
        for (std::size_t word = 0; word < HtileBytes * 2u / sizeof(*metadata); ++word) {
            const auto byte = word * sizeof(*metadata);
            const bool written = byte >= test.offset && byte - test.offset < test.writtenBytes;
            Require(metadata[word] == (written ? 0xff0u : 0xffffffffu), message + ": actual compute metadata write differs at byte " + std::to_string(byte));
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto shaders = Compile(*device);
        DmaFillClearsDepth(*device, shaders);
        DepthOnlyFillKeepsStencil(*device, shaders);
        const bool computeRan = StoresFills(*device, NewBlock());
        if (!computeRan) {
            LibcRunShutdown_nid_postfix();
            std::puts("skipped: required compute HTILE/indirect coverage needs writable host imports");
            return VulkanTestSkipped;
        }
        DispatchFillClearsDepth(*device, shaders);
        HtileKernelClearsSlices(*device, shaders);
        LibcRunShutdown_nid_postfix();
        std::puts("depth fast clear submit tests passed, including recognized HTILE direct/thread/indirect and zero-work cases");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { LibcRunShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
