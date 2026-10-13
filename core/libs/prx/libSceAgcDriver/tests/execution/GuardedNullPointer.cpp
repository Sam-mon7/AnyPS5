#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 64;
constexpr std::uint32_t Default = 0x11111111u;
constexpr std::uint32_t Payload = 0x3f800000u;

alignas(256) std::array<std::uint32_t, Lanes> Output{};
alignas(256) std::array<std::uint32_t, 16> Root{};
alignas(256) std::array<std::uint32_t, 4> Data{Payload, 0u, 0u, 0u};
alignas(256) std::array<std::uint64_t, 1> Indirect{};
alignas(256) std::array<std::uint32_t, 8> Table{};
alignas(256) std::uint32_t BufferGuard = 0;

// The branch condition comes from a buffer read, so the CPU cannot prune the V# use.
alignas(256) constexpr std::array<std::uint32_t, 13> BufferBranchCode{
    0x34020082u, 0xe0300000u, 0x80020300u, 0xbf8c3f70u, 0x7e180503u, 0xbf06800cu, 0xbf850005u,
    0xe0300000u, 0x80000200u, 0xbf8c3f70u, 0xe0701000u, 0x80010201u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 19> BranchCode{
    0xf4080100u, 0xfa000000u, 0xf4000200u, 0xfa000010u, 0xf4040280u, 0xfa000018u, 0xbe8c03ffu, 0x11111111u,
    0xbf8cc07fu, 0xbf068008u, 0xbf850003u, 0xf4000305u, 0xfa000000u, 0xbf8cc07fu, 0x7e02020cu, 0x34040082u,
    0xe0701000u, 0x80010102u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 21> ExecCode{
    0xf4080100u, 0xfa000000u, 0xf4000200u, 0xfa000010u, 0xf4040280u, 0xfa000018u, 0xbe8c03ffu, 0x11111111u,
    0xbf8cc07fu, 0x7e02020cu, 0x34040082u, 0x7da80008u, 0xbf880004u, 0xf4000305u, 0xfa000000u, 0xbf8cc07fu,
    0x7e02020cu, 0xbefe04c1u, 0xe0701000u, 0x80010102u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 21> EmptyExecCode{
    0xf4080100u, 0xfa000000u, 0xf4000200u, 0xfa000010u, 0xf4040280u, 0xfa000018u, 0xbe8c03ffu, 0x11111111u,
    0xbf8cc07fu, 0x7e02020cu, 0x34040082u, 0x7da80008u, 0xbf800000u, 0xf4000305u, 0xfa000000u, 0xbf8cc07fu,
    0x7e02020cu, 0xbefe04c1u, 0xe0701000u, 0x80010102u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 22> ChainCode{
    0xf4080100u, 0xfa000000u, 0xf4000200u, 0xfa000010u, 0xf4040280u, 0xfa000018u, 0xbe8c03ffu, 0x11111111u,
    0xbf8cc07fu, 0xf4040385u, 0xfa000000u, 0xbf8cc07fu, 0xbf068008u, 0xbf850003u, 0xf4000307u, 0xfa000000u,
    0xbf8cc07fu, 0x7e02020cu, 0x34040082u, 0xe0701000u, 0x80010102u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 21> DescriptorCode{
    0xf4080100u, 0xfa000000u, 0xf4000200u, 0xfa000010u, 0xf4040280u, 0xfa000018u, 0x7e0202ffu, 0x11111111u,
    0xbf8cc07fu, 0xbf068008u, 0xbf850006u, 0xf4080405u, 0xfa000010u, 0xbf8cc07fu, 0xe0300000u, 0x80040100u,
    0xbf8c3f70u, 0x34040082u, 0xe0701000u, 0x80010102u, 0xbf810000u,
};

class StderrCapture {
public:
    StderrCapture() {
        std::fflush(stderr);
        file = std::tmpfile();
        Require(file != nullptr, "cannot open a temporary file for stderr");
#ifdef _WIN32
        saved = _dup(_fileno(stderr));
        _dup2(_fileno(file), _fileno(stderr));
#else
        saved = dup(fileno(stderr));
        dup2(fileno(file), fileno(stderr));
#endif
    }
    std::string Finish() {
        std::fflush(stderr);
#ifdef _WIN32
        _dup2(saved, _fileno(stderr));
        _close(saved);
#else
        dup2(saved, fileno(stderr));
        close(saved);
#endif
        std::rewind(file);
        std::string text;
        char buffer[4096];
        for (std::size_t read = 0; (read = std::fread(buffer, 1, sizeof(buffer), file)) != 0;) text.append(buffer, read);
        std::fclose(file);
        std::fwrite(text.data(), 1, text.size(), stderr);
        return text;
    }

private:
    std::FILE* file = nullptr;
    int saved = -1;
};

struct Outcome {
    std::vector<std::uint32_t> words;
    std::string log;
    std::uint64_t variantId = 0;
    std::uint64_t pipelineVariantId = 0;
    std::uint32_t poisoned = 0;
    bool cacheable = false;
};

template <std::size_t CodeWords>
Outcome Run(AgcDriver::VulkanDevice& device, const std::array<std::uint32_t, CodeWords>& code, std::uint64_t pointer, std::uint32_t guard) {
    const auto output = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Output.data()));
    Root = {};
    Root[0] = static_cast<std::uint32_t>(output);
    Root[1] = static_cast<std::uint32_t>((output >> 32u) & 0xffffu);
    Root[2] = static_cast<std::uint32_t>(sizeof(Output));
    Root[3] = 0x30027facu;
    Root[4] = guard;
    Root[6] = static_cast<std::uint32_t>(pointer);
    Root[7] = static_cast<std::uint32_t>(pointer >> 32u);
    std::fill(Output.begin(), Output.end(), 0xdeadbeefu);
    const auto root = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Root.data()));
    const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(root), static_cast<std::uint32_t>(root >> 32u)};
    const std::span<const std::uint32_t> span(code);
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(span.data()), span, 0, {}},
        {64, 0, userData, compute, std::nullopt, std::nullopt, {}},
        device.Target(),
        {0, 0, 0, 128}
    };
    AgcDriver::ShaderMemory memory({});
    const auto capture = memory.Capture(request);
    const auto regions = memory.Regions();
    request.context.memory = regions;
    const auto result = ShaderRecompiler::Recompile(request, *capture);
    Outcome outcome;
    outcome.variantId = result->variantId;
    outcome.pipelineVariantId = result->PipelineVariantId();
    outcome.poisoned = result->poisonedSrtReads;
    outcome.cacheable = AgcDriver::DriverDetail::CacheableResult(*result);
    StderrCapture log;
    try {
        device.Dispatch(*result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(span.data()));
        device.WaitIdle();
    } catch (...) {
        static_cast<void>(log.Finish());
        throw;
    }
    outcome.log = log.Finish();
    outcome.words.assign(Output.begin(), Output.end());
    return outcome;
}

bool Faulted(const Outcome& outcome) {
    return outcome.log.find("BDA access failed") != std::string::npos;
}

std::string Fault(std::uint64_t address, std::uint32_t pc, std::uint32_t bytes = 4u) {
    char text[96];
    std::snprintf(text, sizeof(text), "address=0x%llx instruction=0x%x bytes=%u", static_cast<unsigned long long>(address), pc, bytes);
    return text;
}

void RequireWords(const Outcome& outcome, std::uint32_t lanes, std::uint32_t inside, const std::string& what) {
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
        const auto expected = lane < lanes ? inside : Default;
        Require(outcome.words[lane] == expected, what + ": lane " + std::to_string(lane) + " wrote " + std::to_string(outcome.words[lane]) + ", expected " + std::to_string(expected));
    }
}

void RunTests(AgcDriver::VulkanDevice& device) {
    const auto valid = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Data.data()));
    void* reserved = nullptr;
#ifdef _WIN32
    reserved = VirtualAlloc(nullptr, 65536, MEM_RESERVE, PAGE_NOACCESS);
#else
    reserved = mmap(nullptr, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reserved == MAP_FAILED) reserved = nullptr;
#endif
    Require(reserved != nullptr, "cannot reserve an inaccessible range");
    const auto unmapped = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(reserved));

    const auto taken = Run(device, BranchCode, valid, 1u);
    Require(!Faulted(taken) && taken.poisoned == 0u && taken.cacheable, "guarded pointer: a mapped pointer faulted, was poisoned or left the caches:\n" + taken.log);
    RequireWords(taken, Lanes, Payload, "guarded pointer: a mapped pointer under a taken branch");
    const auto skippedNull = Run(device, BranchCode, 0u, 0u);
    Require(!Faulted(skippedNull) && skippedNull.poisoned == 1u && !skippedNull.cacheable, "guarded pointer: a null pointer behind a branch that skips its read faulted or stayed cacheable:\n" + skippedNull.log);
    RequireWords(skippedNull, 0u, 0u, "guarded pointer: a skipped read through a null pointer");
    const auto takenNull = Run(device, BranchCode, 0u, 1u);
    Require(takenNull.pipelineVariantId == skippedNull.pipelineVariantId && takenNull.pipelineVariantId == taken.pipelineVariantId && takenNull.variantId == taken.variantId, "guarded pointer: the poison or the branch condition changed the pipeline variant");
    Require(Faulted(takenNull) && takenNull.log.find(Fault(0u, 0x2cu)) != std::string::npos && takenNull.log.find("reason=1") != std::string::npos, "guarded pointer: a read through a null pointer that runs did not fault at its pc and address:\n" + takenNull.log);
    Require(std::all_of(takenNull.words.begin(), takenNull.words.end(), [](std::uint32_t word) { return word == 0u || word == 0xdeadbeefu; }), "guarded pointer: a faulting read did not return zero");
    const auto takenUnmapped = Run(device, BranchCode, unmapped, 1u);
    Require(Faulted(takenUnmapped) && takenUnmapped.log.find(Fault(unmapped, 0x2cu)) != std::string::npos, "guarded pointer: a read through an unmapped pointer that runs did not fault at its address:\n" + takenUnmapped.log);
    Require(takenUnmapped.pipelineVariantId == taken.pipelineVariantId, "guarded pointer: an unmapped pointer changed the pipeline variant");

    const auto someLanes = Run(device, ExecCode, valid, 5u);
    Require(!Faulted(someLanes), "guarded pointer: a mapped pointer under EXEC faulted:\n" + someLanes.log);
    RequireWords(someLanes, 5u, Payload, "guarded pointer: a mapped pointer read under EXEC");
    const auto noLanes = Run(device, ExecCode, 0u, 0u);
    Require(!Faulted(noLanes) && noLanes.poisoned == 1u, "guarded pointer: a null pointer read skipped by s_cbranch_execz faulted:\n" + noLanes.log);
    RequireWords(noLanes, 0u, 0u, "guarded pointer: a read skipped by s_cbranch_execz");
    const auto oneLane = Run(device, ExecCode, 0u, 1u);
    Require(Faulted(oneLane) && oneLane.log.find(Fault(0u, 0x34u)) != std::string::npos, "guarded pointer: a null pointer read with one EXEC lane did not fault:\n" + oneLane.log);
    const auto emptyMapped = Run(device, EmptyExecCode, valid, 0u);
    Require(!Faulted(emptyMapped), "guarded pointer: a mapped pointer read with an empty EXEC faulted:\n" + emptyMapped.log);
    RequireWords(emptyMapped, 0u, 0u, "guarded pointer: a mapped pointer read with an empty EXEC");
    const auto emptyNull = Run(device, EmptyExecCode, 0u, 0u);
    Require(Faulted(emptyNull) && emptyNull.log.find(Fault(0u, 0x34u)) != std::string::npos, "guarded pointer: a null pointer read that runs with an empty EXEC did not fault:\n" + emptyNull.log);
    Require(noLanes.pipelineVariantId == someLanes.pipelineVariantId && oneLane.pipelineVariantId == someLanes.pipelineVariantId && emptyNull.pipelineVariantId == emptyMapped.pipelineVariantId, "guarded pointer: the poison changed an EXEC program's pipeline variant");

    const auto chainSkipped = Run(device, ChainCode, 0u, 0u);
    Require(chainSkipped.poisoned == 3u && Faulted(chainSkipped) && chainSkipped.log.find(Fault(0u, 0x24u)) != std::string::npos, "guarded pointer: a pointer read through a null pointer, used only by a skipped read, did not fault at its own pc:\n" + chainSkipped.log);
    const auto chainTaken = Run(device, ChainCode, 0u, 1u);
    Require(Faulted(chainTaken) && chainTaken.log.find(Fault(0u, 0x24u)) != std::string::npos, "guarded pointer: a read through a pointer read through a null pointer did not fault at the first read:\n" + chainTaken.log);
    Indirect[0] = valid;
    const auto chainMapped = Run(device, ChainCode, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Indirect.data())), 1u);
    Require(!Faulted(chainMapped) && chainMapped.poisoned == 0u && chainMapped.cacheable && chainMapped.pipelineVariantId == chainTaken.pipelineVariantId && chainSkipped.pipelineVariantId == chainTaken.pipelineVariantId, "guarded pointer: a chain's poison changed its pipeline variant, or a mapped chain faulted:\n" + chainMapped.log);
    RequireWords(chainMapped, Lanes, Payload, "guarded pointer: a read through a mapped pointer chain");

    Table = {0u, 0u, 0u, 0u, static_cast<std::uint32_t>(valid), static_cast<std::uint32_t>((valid >> 32u) & 0xffffu), static_cast<std::uint32_t>(sizeof(Data)), 0x30027facu};
    const auto table = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Table.data()));
    const auto descriptorTaken = Run(device, DescriptorCode, table, 1u);
    Require(!Faulted(descriptorTaken) && descriptorTaken.poisoned == 0u && descriptorTaken.cacheable, "guarded pointer: a V# behind a mapped pointer faulted, was poisoned or left the caches:\n" + descriptorTaken.log);
    RequireWords(descriptorTaken, Lanes, Payload, "guarded pointer: a buffer read through a V# behind a mapped pointer");
    const auto descriptorSkipped = Run(device, DescriptorCode, 0u, 0u);
    Require(!Faulted(descriptorSkipped) && descriptorSkipped.poisoned == 4u && !descriptorSkipped.cacheable, "guarded pointer: a V# behind a branch that skips its null pointer faulted or stayed cacheable:\n" + descriptorSkipped.log);
    RequireWords(descriptorSkipped, 0u, 0u, "guarded pointer: a skipped V# load through a null pointer");
    const auto descriptorNull = Run(device, DescriptorCode, 0u, 1u);
    Require(descriptorNull.pipelineVariantId == descriptorSkipped.pipelineVariantId && Faulted(descriptorNull) && descriptorNull.log.find(Fault(0x10u, 0x2cu)) != std::string::npos, "guarded pointer: a V# load through a null pointer that runs did not fault at its pc and address:\n" + descriptorNull.log);
    const auto descriptorUnmapped = Run(device, DescriptorCode, unmapped, 1u);
    Require(descriptorUnmapped.pipelineVariantId == descriptorNull.pipelineVariantId && Faulted(descriptorUnmapped) && descriptorUnmapped.log.find(Fault(unmapped + 0x10u, 0x2cu)) != std::string::npos, "guarded pointer: a V# load through an unmapped pointer that runs did not fault at its address:\n" + descriptorUnmapped.log);
    Table = {};
    const auto descriptorZero = Run(device, DescriptorCode, table, 1u);
    Require(!Faulted(descriptorZero) && descriptorZero.poisoned == 0u && descriptorZero.cacheable && descriptorZero.pipelineVariantId == descriptorNull.pipelineVariantId, "guarded pointer: a zero V# read from mapped memory faulted, or its pipeline variant differs from a V# zeroed by poison:\n" + descriptorZero.log);
    RequireWords(descriptorZero, Lanes, 0u, "guarded pointer: a buffer read through a zero V#");

#ifdef _WIN32
    VirtualFree(reserved, 0, MEM_RELEASE);
#else
    munmap(reserved, 65536);
#endif
}

void RunBufferTypeTests(AgcDriver::VulkanDevice& device, bool formatted, bool exec) {
    const auto descriptor = [](const void* data, std::uint32_t bytes) {
        const auto address = reinterpret_cast<std::uintptr_t>(data);
        return std::array<std::uint32_t, 4>{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
    };
    const auto valid = descriptor(Data.data(), sizeof(Data));
    // T# shape from the failing dispatch; it is neither a valid V# nor a host range to bind.
    constexpr std::array<std::uint32_t, 4> invalid{0x027a5a00u, 0xc1600000u, 0x007040c7u, 0x90900924u};
    const auto output = descriptor(Output.data(), sizeof(Output));
    const auto guard = descriptor(&BufferGuard, sizeof(BufferGuard));
    auto code = BufferBranchCode;
    if (formatted) code[7] = 0xe0000000u; // buffer_load_format_x
    if (exec) {
        code[5] = 0x7da8000cu; // v_cmpx_gt_u32 s12, v0
        code[6] = 0xbf800000u; // no branch: the vector instruction itself must honor EXEC
    }
    std::array<std::uint32_t, 12> userData{};
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    std::copy(guard.begin(), guard.end(), userData.begin() + 8);
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {Lanes, 0, userData, compute, std::nullopt, std::nullopt, memory}, device.ComputeTarget(Lanes), {0, 0, 0, 128}
    };
    for (const bool bad : {false, true}) {
        for (const bool active : {false, true}) {
            const auto& input = bad ? invalid : valid;
            std::copy(input.begin(), input.end(), userData.begin());
            BufferGuard = active ? 1u : 0u;
            Output.fill(0xdeadbeefu);
            AgcDriver::ShaderMemory shaderMemory({});
            const auto capture = shaderMemory.Capture(request);
            const auto result = ShaderRecompiler::Recompile(request, *capture);
            Outcome outcome;
            StderrCapture log;
            try {
                device.Dispatch(*result, 1, 1, 1, {}, request.shader.codeAddress);
                device.WaitIdle();
            } catch (...) {
                static_cast<void>(log.Finish());
                throw;
            }
            outcome.log = log.Finish();
            Require(Faulted(outcome) == (bad && active), "guarded V#: descriptor fault did not follow the GPU branch:\n" + outcome.log);
            if (bad && active) Require(outcome.log.find("reason=8") != std::string::npos && outcome.log.find(Fault(0x027a5a00u, 0x1cu, 0u)) != std::string::npos, "guarded V#: fault did not identify the descriptor and instruction:\n" + outcome.log);
            for (std::uint32_t lane = 0u; lane < Lanes; ++lane) Require(Output[lane] == (active && !bad && (!exec || lane == 0u) ? Payload : 0xdeadbeefu), "guarded V#: skipped or invalid buffer accessed output");
        }
    }
    request.target.bdaAbiVersion = 0u;
    bool rejected = false;
    try {
        static_cast<void>(ShaderRecompiler::Recompile(request));
    } catch (const std::exception& error) {
        rejected = std::string(error.what()).find("buffer descriptor uses an unsupported type") != std::string::npos;
    }
    Require(rejected, "guarded V#: a target without the fault ABI silently accepted an invalid descriptor");
}

void RunImageTypeTests(AgcDriver::VulkanDevice& device, bool exec) {
    alignas(4096) static std::array<std::uint32_t, 4096> texels{};
    const auto textureAddress = reinterpret_cast<std::uintptr_t>(texels.data());
    const std::array<std::uint32_t, 8> valid{static_cast<std::uint32_t>(textureAddress >> 8u), static_cast<std::uint32_t>((textureAddress >> 40u) & 0xffu) | (20u << 20u), 0u, 0x90000facu, 0u, 0u, 0u, 0u};
    constexpr std::array<std::uint32_t, 8> invalid{0x3f000000u, 0x3d4ccccdu, 0x3f000000u, 0x3f000000u, 0x69300ee0u, 2u, 0x71u, 0u};
    auto nullType = valid;
    nullType[3] = 0x249u;
    const auto descriptor = [](const void* data, std::uint32_t bytes) {
        const auto address = reinterpret_cast<std::uintptr_t>(data);
        return std::array<std::uint32_t, 4>{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
    };
    const auto output = descriptor(Output.data(), sizeof(Output));
    const auto guard = descriptor(&BufferGuard, sizeof(BufferGuard));
    const std::array<std::uint32_t, 6> operations{0xf0001108u, 0xf0900108u, 0xf0380108u, 0xf0201108u, 0xf03c2108u, 0xf1000108u};
    for (const auto operation : operations) {
        const bool sample = operation == operations[1] || operation == operations[5];
        const bool query = operation == operations[2];
        const bool write = operation == operations[3];
        std::vector<std::uint32_t> code{
            0x34020082u, 0xe0300000u, 0x80030300u, 0xbf8c3f70u, 0x7e200503u, 0xbf068010u, 0xbf850000u,
            0x7e080280u, 0x7e0a0280u, 0x7e0c0280u, 0x7e0402ffu, Payload,
            0xbe900380u, 0xbe910380u, 0xbe920380u, 0xbe930380u,
            operation, 0x00000204u | (sample ? 0x00800000u : 0u),
            0xbf8c3f70u, 0xe0701000u, 0x80020201u, 0xbf810000u,
        };
        code[6] |= static_cast<std::uint32_t>(code.size() - 8u);
        if (exec) {
            code[5] = 0x7da80010u;
            code[6] = 0xbf800000u;
        }
        std::array<std::uint32_t, 16> userData{};
        std::copy(output.begin(), output.end(), userData.begin() + 8);
        std::copy(guard.begin(), guard.end(), userData.begin() + 12);
        const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
        ShaderRecompiler::RecompileRequest request{
            {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
            {Lanes, 0, userData, compute, std::nullopt, std::nullopt, {}}, device.ComputeTarget(Lanes), {0, 0, 0, 128}
        };
        for (const auto& image : {valid, invalid, nullType}) {
            const bool bad = image == invalid;
            const bool null = image == nullType;
            std::copy(image.begin(), image.end(), userData.begin());
            for (const bool active : {false, true}) {
                BufferGuard = active ? 1u : 0u;
                texels.fill(Payload);
                Output.fill(0xdeadbeefu);
                AgcDriver::ShaderMemory shaderMemory({});
                const auto capture = shaderMemory.Capture(request);
                const auto result = ShaderRecompiler::Recompile(request, *capture);
                Require(result->invalidResourceDescriptors == (bad ? 1u : 0u), "guarded T#: invalid descriptor count differs");
                Outcome outcome;
                StderrCapture log;
                try {
                    device.Dispatch(*result, 1, 1, 1, {}, request.shader.codeAddress);
                    device.WaitIdle();
                } catch (...) {
                    static_cast<void>(log.Finish());
                    throw;
                }
                outcome.log = log.Finish();
                Require(Faulted(outcome) == (bad && active), "guarded T#: descriptor fault did not follow the GPU branch/EXEC:\n" + outcome.log);
                if (bad && active) Require(outcome.log.find("reason=8") != std::string::npos && outcome.log.find(Fault(0xcd3f00000000ull, 0x40u, 0u)) != std::string::npos, "guarded T#: fault lost the original descriptor address or image PC:\n" + outcome.log);
                const auto value = write ? Payload : null ? 0u : query ? 1u : Payload;
                for (std::uint32_t lane = 0u; lane < Lanes; ++lane) Require(Output[lane] == (active && !bad && (!exec || lane == 0u) ? value : 0xdeadbeefu), "guarded T#: skipped/invalid image changed output, or valid result differs");
                Require(std::ranges::all_of(texels, [](std::uint32_t word) { return word == Payload; }), "guarded T#: image store/atomic changed a backing sentinel");
            }
        }
    }
}

void RunRegisteredImageTests(AgcDriver::VulkanDevice& device) {
    using namespace ShaderRecompiler;
    const auto target = device.Target();
    if (!target.registrationModules) {
        std::puts("registered image metadata test skipped: graphics pipeline libraries unavailable");
        return;
    }
    constexpr std::uint32_t Extent = 16u;
    alignas(256) static std::array<std::uint32_t, Extent * Extent * 4u> pixels{};
    alignas(4096) static std::array<std::uint32_t, 4096> texels{};
    constexpr std::array<std::array<float, 4>, 3> positions{{{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}}};
    constexpr std::array<std::uint32_t, 6> vertexCode{0xe0382000u, 0x80000005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
    const auto vertexAddress = reinterpret_cast<std::uintptr_t>(positions.data());
    const std::array<std::uint32_t, 4> vertexUsers{static_cast<std::uint32_t>(vertexAddress), static_cast<std::uint32_t>(vertexAddress >> 32u) | (16u << 16u), 3u, 0x01016facu};
    const RecompileRequest vertexRequest{{ShaderStage::Vertex, 0x33000u, vertexCode, 0, {}}, {32, 0, vertexUsers, {}, {}, ShaderVertexStageInfo{}, {}}, target, {0, 0, 0, 128}};
    const auto vertex = Recompile(vertexRequest);
    const std::array<std::uint32_t, 16> code{0x7e000280u, 0x7e020280u, 0x7e0402ffu, Payload, 0xe0300000u, 0x80020300u, 0xbf8c3f70u, 0x7e180503u, 0xbf06800cu, 0xbf850003u, 0xf0001108u, 0x00000200u, 0xbf8c3f70u, 0xf800180fu, 0x02020202u, 0xbf810000u};
    const auto textureAddress = reinterpret_cast<std::uintptr_t>(texels.data());
    const std::array<std::uint32_t, 8> valid{static_cast<std::uint32_t>(textureAddress >> 8u), static_cast<std::uint32_t>((textureAddress >> 40u) & 0xffu) | (22u << 20u), 0u, 0x90000facu, 0u, 0u, 0u, 0u};
    auto invalid = valid;
    invalid[3] = 0x30000facu;
    auto null = valid;
    null[3] = 0x249u;
    const auto guardAddress = reinterpret_cast<std::uintptr_t>(&BufferGuard);
    std::array<std::uint32_t, 12> users{};
    const std::array<std::uint32_t, 4> guard{static_cast<std::uint32_t>(guardAddress), static_cast<std::uint32_t>(guardAddress >> 32u), sizeof(BufferGuard), 0x31016facu};
    std::copy(guard.begin(), guard.end(), users.begin() + 8);
    AgcDriver::Registers context{{0x1b6, 0x8000}, {0x1b3, 0}, {0x1b4, 0}, {0x203, 0}, {0x1c5, 9u}};
    RecompileRequest request{{ShaderStage::Fragment, 0x34000u, code, 0, {}}, {32, 0, users, {}, AgcDriver::Graphics::DecodePixelStageInfo(context, {}), {}, {}}, target, PixelPushLayout(0u, target)};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0, 32, 32, {}, {}};
    state.color = {reinterpret_cast<std::uintptr_t>(pixels.data()), {Extent, Extent}, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(pixels), 0};
    state.color.elementBytes = 16u;
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Extent, Extent};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Extent), static_cast<float>(Extent), -static_cast<float>(Extent), 0, 1};
    state.scissor = {{0, 0}, {Extent, Extent}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15u;
    state.blends = {state.blend};
    const AgcDriver::Pm4::DrawParameters draw{0, 3, 0, 1, 0, false};
    std::uint64_t pipeline = 0u;
    for (const auto& image : {valid, invalid, null, valid}) {
        std::copy(image.begin(), image.end(), users.begin());
        for (const bool active : {false, true}) {
            BufferGuard = active ? 1u : 0u;
            texels.fill(0x3e800000u);
            pixels.fill(0xdeadbeefu);
            const auto pixel = Recompile(request);
            Require(pixel.registrationModule && pixel.invalidResourceDescriptors == (image == invalid ? 1u : 0u), "registered image: descriptor flag was lost");
            if (pipeline == 0u) pipeline = pixel.PipelineVariantId();
            Require(pixel.PipelineVariantId() == pipeline, "registered image: descriptor state specialized the module");
            const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderStage::Vertex, &vertex, 0}, {ShaderStage::Fragment, &pixel, PixelPushOffset(0u, target)}}};
            Outcome outcome;
            const bool fault = active && image == invalid;
            StderrCapture log;
            try {
                device.Draw(state, draw, shaders);
                device.WaitIdle();
            } catch (const std::runtime_error& error) {
                if (!fault) {
                    static_cast<void>(log.Finish());
                    throw;
                }
                outcome.log = error.what();
            } catch (...) {
                static_cast<void>(log.Finish());
                throw;
            }
            outcome.log += log.Finish();
            Require(Faulted(outcome) == fault, "registered image: metadata fault did not follow the GPU branch:\n" + outcome.log);
            if (fault) Require(outcome.log.find("reason=8") != std::string::npos && outcome.log.find(Fault(textureAddress, 0x28u, 0u)) != std::string::npos, "registered image: fault lost original address or PC:\n" + outcome.log);
            else {
                const auto expected = !active ? Payload : image == null ? 0u : 0x3e800000u;
                const auto mismatch = std::ranges::find_if(pixels, [&](std::uint32_t word) { return word != expected; });
                Require(mismatch == pixels.end(), "registered image: valid, skipped, or TYPE0 pixel differs: active=" + std::to_string(active) + " type=" + std::to_string(image[3] >> 28u) + " expected=" + std::to_string(expected) + " first=" + std::to_string(pixels[0]) + " index=" + std::to_string(mismatch - pixels.begin()));
            }
        }
    }
    std::puts("registered image metadata GPU tests passed");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        RunTests(*device);
        for (const bool formatted : {false, true}) {
            for (const bool exec : {false, true}) RunBufferTypeTests(*device, formatted, exec);
        }
        for (const bool exec : {false, true}) RunImageTypeTests(*device, exec);
        RunRegisteredImageTests(*device);
        std::puts("guarded null pointer tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
