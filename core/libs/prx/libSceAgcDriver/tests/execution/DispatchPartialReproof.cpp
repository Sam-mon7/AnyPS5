#include "DepthFastClearHarness.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ResourceProofWork.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <bit>
#include <cstring>
#include <mutex>

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

namespace {
using namespace DepthFastClearHarness;
using AgcDriver::Graphics::ShaderResources;
constexpr std::size_t BlockBytes = 1048576;
constexpr std::uint32_t Lanes = 32;
class Block {
public:
    Block() {
        Require(AgcDriver::GuestMemory::WriteWatched(), "partial reproof: write watching unavailable");
#ifdef _WIN32
        data = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes));
        if (data) GuestArena::GuestArenaCommit_nid_postfix(data, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        auto* mapped = mmap(nullptr, BlockBytes * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes * 2) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + BlockBytes - aligned);
            data = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, BlockBytes);
        }
#endif
        Require(data != nullptr, "partial reproof: guest allocation failed");
        std::memset(data, 0, BlockBytes);
        GuestAllocations::Mutation().Add(data, BlockBytes, true, true, true);
        Require(AgcDriver::GuestMemory::Watched(Address(), BlockBytes), "partial reproof: allocated block was not watched");
    }
    ~Block() {
        GuestAllocations::Mutation().Remove(data);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(data, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(data, BlockBytes);
#else
        munmap(data, BlockBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, BlockBytes);
#endif
    }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    std::uint8_t* Data() const { return data; }
    std::uint32_t* Words(std::size_t offset = 0) const { return reinterpret_cast<std::uint32_t*>(data + offset); }
    std::uint64_t Address(std::size_t offset = 0) const { return reinterpret_cast<std::uintptr_t>(data + offset); }
    void Remap() {
        GuestAllocations::Mutation().Remove(data);
        GuestAllocations::Mutation().Add(data, BlockBytes, true, true, true);
    }
private:
    std::uint8_t* data = nullptr;
};

void RecordDepth(AgcDriver::VulkanDevice& device, const Shaders& compiled, const Surface& surface, const DrawOptions& options, VkExtent2D extent = {Width, Height}, std::span<std::byte> pixels = Pixels) {
    std::fill(pixels.begin(), pixels.end(), std::byte{0});
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &compiled.vertex, 0},
        {ShaderStage::Fragment, &compiled.pixel, PixelPushOffset(compiled.vertexPush, device.Target())}
    }};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(pixels.data()), extent, VK_FORMAT_R8G8B8A8_UNORM, pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.depth = AgcDriver::Graphics::DepthTarget{surface.depth, surface.stencil, extent, surface.format, 1.0f, surface.clearStencil, surface.htile, surface.htileStencil};
    state.depth->firstSlice = surface.slice;
    state.depth->depthSwizzle = surface.swizzle;
    state.depthTest = options.depthTest;
    state.depthWrite = options.depthWrite;
    state.depthCompare = options.depthCompare;
    state.stencilTest = options.stencilTest;
    state.stencilFront = options.stencil;
    state.stencilBack = options.stencil;
    state.renderExtent = extent;
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(extent.height), static_cast<float>(extent.width), -static_cast<float>(extent.height), options.minDepth, options.maxDepth};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, extent};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Triangles().size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
}

alignas(256) constexpr std::array<std::uint32_t, 14> SampleCode{
    0x7e3c0280u, 0x7e3e0280u, 0xf09c0108u, 0x00a10a1eu,
    0xf09c0108u, 0x00a30b1eu, 0xbf8c3f70u, 0x34060082u,
    0xe0701000u, 0x80000a03u, 0xe0701080u, 0x80000b03u,
    0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 9> StoreCode{
    0x7e280d00u, 0x102828ffu, 0x3c800000u, 0x7e3c0300u, 0x7e3e0280u,
    0xf0201108u, 0x0001141eu, 0xbf810000u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 10> AliasStoreCode{
    0x7e280280u, 0x7e2a0280u, 0x7e2c0280u, 0x7e2e0280u,
    0x7e3c0300u, 0x7e3e0280u, 0xf0201f08u, 0x0001141eu,
    0xbf810000u, 0xbf810000u,
};
std::array<std::uint32_t, 8> ImageWords(const Block& block, std::size_t offset = 0) {
    const auto address = block.Address(offset);
    return {static_cast<std::uint32_t>(address >> 8), static_cast<std::uint32_t>(address >> 40) | (22u << 20) | (3u << 30),
        ((Width - 1u) >> 2) | ((Height - 1u) << 14), 0x90000facu, 0, 0, 0, 0};
}

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::span<const std::uint32_t> data) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{32, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, data, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128},
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

void Run(AgcDriver::VulkanDevice& device, const std::string& scenario) {
    Block primary, stable, output, color;
    std::fill_n(primary.Words(), BlockBytes / 4, std::bit_cast<std::uint32_t>(0.125f));
    std::fill_n(stable.Words(), BlockBytes / 4, std::bit_cast<std::uint32_t>(0.75f));
    const auto images = ImageWords(primary, (scenario == "depth" || scenario == "depthtwo") ? 256 : 0);
    const auto other = ImageWords(stable, scenario == "depthtwo" ? 256 : 0);
    std::array<std::uint32_t, 24> data{};
    const auto buffer = DepthFastClearHarness::BufferDescriptor(output.Data(), 0, BlockBytes);
    std::copy(buffer.begin(), buffer.end(), data.begin());
    data[3] = 0x31016facu;
    std::copy(images.begin(), images.end(), data.begin() + 4);
    std::copy(other.begin(), other.end(), data.begin() + 12);
    data[20] = 0x92u;
    const auto sample = Compile(device, SampleCode, data);
    auto writerData = data;
    if (scenario != "own") { writerData[6] = 7u | (63u << 14u); writerData[5] = (writerData[5] & ~(0x1ffu << 20u)) | (56u << 20u); }
    const auto writer = Compile(device, scenario == "own" ? std::span<const std::uint32_t>(StoreCode) : std::span<const std::uint32_t>(AliasStoreCode), writerData);
    auto secondData = writerData;
    std::copy(other.begin(), other.end(), secondData.begin() + 4);
    secondData[6] = 7u | (63u << 14u); secondData[5] = (secondData[5] & ~(0x1ffu << 20u)) | (56u << 20u);
    const auto secondWriter = Compile(device, AliasStoreCode, secondData);
    const auto drawShaders = DepthFastClearHarness::Compile(device);
    const Surface surface{primary.Address(), 0, 0, false, VK_FORMAT_D32_SFLOAT, 0, 0, 24};
    const auto colorBytes = std::as_writable_bytes(std::span(color.Words(), BlockBytes / 4));
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    const bool enabled = AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::DispatchPartialReproof);
    const bool depth = scenario == "depth" || scenario == "depthtwo";
    const Surface secondDepth{stable.Address(), 0, 0, false, VK_FORMAT_D32_SFLOAT, 0, 0, 24};
    if (depth) RecordDepth(device, drawShaders, surface, {.depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS}, {128, 128}, colorBytes);
    if (scenario == "depthtwo") RecordDepth(device, drawShaders, secondDepth, {.depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS}, {128, 128}, colorBytes);
    std::shared_ptr<const AgcDriver::Recipe> recipe;
    device.Dispatch(sample, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(SampleCode.data()), {}, &recipe);
    device.WaitIdle();
    Require(recipe != nullptr, "partial reproof: initial dispatch has no recipe");
    const auto resources = recipe->templateRef.lock();
    Require(resources != nullptr && resources->Reusable(), "partial reproof: initial resource is not reusable");
    const auto check = [&](float first, float second = 0.75f) {
        for (std::size_t lane = 0; lane < Lanes; ++lane) {
            Require(output.Words()[lane] == std::bit_cast<std::uint32_t>(first), "partial reproof: primary texel mismatch at lane " + std::to_string(lane) + " got " + std::to_string(output.Words()[lane]));
            Require(output.Words()[lane + Lanes] == std::bit_cast<std::uint32_t>(second), "partial reproof: untouched texel mismatch");
        }
    };
    check(depth ? 0.5f : 0.125f, scenario == "depthtwo" ? 0.5f : 0.75f);
    const auto originalTextures = resources->SampledTextures();
    Require(originalTextures.size() == 2, "partial reproof: sampler did not retain two surfaces");
    const auto submissions = context.recorder->Submissions();
    float expected = 0.375f;
    float expectedOther = 0.75f;
    if (depth) {
        RecordDepth(device, drawShaders, surface, {.depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS, .minDepth = 0.5f, .maxDepth = 1.0f}, {128, 128}, colorBytes);
        if (scenario == "depthtwo") RecordDepth(device, drawShaders, secondDepth, {.depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS, .minDepth = 0.5f, .maxDepth = 1.0f}, {128, 128}, colorBytes);
        expected = 0.75f;
    } else if (scenario == "storage" || scenario == "two" || scenario == "own") {
        device.Dispatch(writer, 1, 1, 1);
        if (scenario == "two") {
            device.Dispatch(secondWriter, 1, 1, 1);
            expectedOther = 0.0f;
        }
        expected = 0.0f;
        if (scenario != "own") {
            const auto alias = AgcDriver::Graphics::StorageTexture::FindPending(primary.Address(), Width * Height * 4);
            Require(alias != nullptr && alias.get() != originalTextures.front()->StorageSource(), "partial reproof: foreign producer did not leave a distinct pending storage image");
        }
    } else {
        if (scenario == "remap") primary.Remap();
        std::fill_n(primary.Words(), BlockBytes / 4, std::bit_cast<std::uint32_t>(expected));
    }
    Require(context.recorder->Submissions() == submissions, "partial reproof: producer submitted before the consumer proof");
    const auto workBefore = AgcDriver::Graphics::ResourceProofWork::Snapshot();
    const auto before = ShaderResources::PartialReproofCounts();
    const AgcDriver::Graphics::CompiledShader compute{ShaderRecompiler::ShaderStage::Compute, &sample, 0};
    bool proved;
    ShaderResources::ProofReport proof;
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        proved = resources->Revalidate(std::span(&compute, 1), &proof);
    }
    const auto after = ShaderResources::PartialReproofCounts();
    std::cout << "proof diagnostic " << scenario << " proved=" << proved << " path=" << static_cast<unsigned>(proof.path)
              << " fast=" << ShaderResources::FastFailName(proof.fastFail) << " calls=" << after.calls - before.calls << " passes=" << after.passes - before.passes
              << " surfaces=" << after.surfaces - before.surfaces << " fallbacks=" << after.fallbacks - before.fallbacks;
    for (std::size_t i = 0; i < after.reasons.size(); ++i) if (after.reasons[i] != before.reasons[i]) std::cout << " reason" << i << '=' << after.reasons[i] - before.reasons[i];
    std::cout << std::endl;
    if (std::getenv("APS5_ARCHB_C_DRAW_REPLAY_STEPS") != nullptr) {
        namespace Work = AgcDriver::Graphics::ResourceProofWork;
        const auto workAfter = Work::Snapshot();
        std::uint64_t operations = 0;
        for (std::size_t path = 0; path < workAfter.events.size(); ++path) {
            for (std::size_t phase = 0; phase < workAfter.events[path].size(); ++phase) {
                for (std::size_t kind = 0; kind < workAfter.events[path][phase].size(); ++kind) {
                    const auto& old = workBefore.events[path][phase][kind];
                    const auto& now = workAfter.events[path][phase][kind];
                    if (now.operations == old.operations) continue;
                    std::cout << "work path=" << path << " phase=" << phase << " kind=" << kind << " operations=" << now.operations - old.operations << " bytes=" << now.bytes - old.bytes << '\n';
                    if (phase != static_cast<std::size_t>(Work::Phase::Verify)) operations += now.operations - old.operations;
                }
            }
        }
        if (depth) {
            const auto path = static_cast<std::size_t>(enabled ? Work::Path::Partial : Work::Path::Full);
            Require(workAfter.proofs[path] > workBefore.proofs[path], "partial reproof: census classified the wrong proof path");
            Require(operations > 0, "partial reproof: census missed actual depth refresh work");
        } else if (!proved && (scenario == "storage" || scenario == "two")) {
            const auto refused = static_cast<std::size_t>(Work::Path::FullRefused);
            const auto passed = static_cast<std::size_t>(Work::Path::Full);
            Require(workAfter.proofs[refused] > workBefore.proofs[refused], "partial reproof: failed full walk missed the refused census cohort");
            Require(workAfter.proofs[passed] == workBefore.proofs[passed], "partial reproof: failed full walk contaminated the passing census cohort");
        }
    }
    Require(after.mismatches == before.mismatches, "partial reproof: VERIFY mismatch");
    if (enabled && depth) {
        Require(resources->SampledTextures() == originalTextures, "partial reproof: refreshed lookup replaced a sampled object");
        Require(proved && after.passes > before.passes, "partial reproof: changed surface did not take a partial pass");
        Require(after.surfaces - before.surfaces == (scenario == "depthtwo" ? 2u : 1u), "partial reproof: refreshed an untouched surface");
        if (std::getenv("APS5_VERIFY_DISPATCH_PARTIAL_REPROOF") != nullptr) Require(after.verified > before.verified, "partial reproof: VERIFY did not run");
    }
    if (enabled && (scenario == "storage" || scenario == "two")) {
        const auto identity = static_cast<std::size_t>(ShaderResources::PartialReproofFallback::Identity);
        Require(after.passes == before.passes && after.reasons[identity] > before.reasons[identity], "partial reproof: foreign image identity was not refused");
    }
    if (!enabled) Require(after.calls == before.calls, "partial reproof: disabled path entered partial proof");
    if (scenario == "remap") Require(after.passes == before.passes, "partial reproof: allocation remap reused stale import proof");
    device.Dispatch(sample, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(SampleCode.data()));
    device.WaitIdle();
    check(expected, expectedOther);
    std::cout << "partial reproof " << scenario << " enabled=" << enabled << " proved=" << proved
              << " path=" << static_cast<unsigned>(proof.path) << " fast=" << ShaderResources::FastFailName(proof.fastFail) << " calls=" << after.calls - before.calls << " passes=" << after.passes - before.passes
              << " surfaces=" << after.surfaces - before.surfaces << " verified=" << after.verified - before.verified << '\n';
}
}

int main(int argc, char** argv) {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const std::string scenario = argc > 1 ? argv[1] : "storage";
        Require(scenario == "storage" || scenario == "depth" || scenario == "depthtwo" || scenario == "cpu" || scenario == "remap" || scenario == "two" || scenario == "own", "unknown partial reproof scenario");
        Run(*device, scenario);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
