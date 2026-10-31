#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineLibrary.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <filesystem>
#include <fstream>
#include <thread>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::DrawMiss;
using AgcDriver::DriverDetail::DrawKeySightings;

alignas(256) constexpr std::array<std::uint32_t, 8> VertexCode{0x7e000208u, 0x7e020280u, 0x7e040280u, 0x7e0602f2u, 0xf80008cfu, 0x03020100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 16> TriangleVertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u, 0xf80008cfu, 0x04030201u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> ConstantPixelCode{0xf4000080u, 0xfa000000u, 0xbf8cc07fu, 0x7e000202u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u};
alignas(256) std::array<std::byte, 1024> ColorMemory{};
alignas(256) std::array<float, 4> PixelConstant{};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};
alignas(8) Header TriangleVertexHeader{};
alignas(8) Header ConstantPixelHeader{};

const Shader* Registered(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
    return &header.shader;
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

std::vector<std::uint32_t> Draws(float scale, const std::uint32_t* vertexCode, std::uint32_t vertexUserSgprs, const std::uint32_t* pixelCode, std::uint32_t pixelUserSgprs, const std::vector<std::vector<std::uint32_t>>& pixelWords) {
    const auto color = reinterpret_cast<std::uintptr_t>(ColorMemory.data());
    const auto vertex = reinterpret_cast<std::uintptr_t>(vertexCode);
    const auto pixel = reinterpret_cast<std::uintptr_t>(pixelCode);
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(scale)}, {0x110, std::bit_cast<std::uint32_t>(scale)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
    };
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, vertexUserSgprs << 1u}, {0x8c, 0},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, pixelUserSgprs << 1u},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    for (const auto& words : pixelWords) {
        for (std::uint32_t k = 0; k < words.size(); ++k) Append(commands, 0x76, {0x00c + k, words[k]});
        Append(commands, 0x2d, {3, 2});
    }
    return commands;
}

AgcDriver::DriverDetail::DrawEntryCounters Submit(std::vector<std::uint32_t> commands) {
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
    return AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
}

AgcDriver::DriverDetail::DrawEntryCounters Submit(float scale, std::initializer_list<float> pixelWords) {
    std::vector<std::vector<std::uint32_t>> words;
    for (const auto word : pixelWords) words.push_back({std::bit_cast<std::uint32_t>(word)});
    return Submit(Draws(scale, VertexCode.data(), 1, PixelCode.data(), 1, words));
}

AgcDriver::DriverDetail::DrawEntryCounters SubmitConstantDraws(std::size_t draws) {
    const auto constant = reinterpret_cast<std::uintptr_t>(PixelConstant.data());
    const std::vector<std::vector<std::uint32_t>> words(draws, {static_cast<std::uint32_t>(constant), static_cast<std::uint32_t>(constant >> 32u)});
    return Submit(Draws(32.0f, TriangleVertexCode.data(), 0, ConstantPixelCode.data(), 2, words));
}

unsigned BrightestColorByte() {
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(ColorMemory.data()), ColorMemory.size());
    unsigned brightest = 0;
    for (const auto value : ColorMemory) brightest = std::max(brightest, static_cast<unsigned>(value));
    return brightest;
}

void ThirdDrawHitsTheCache(bool libraries) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    const auto counters = Submit(32.0f, {1.0f, 1.0f, 1.0f});
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const auto layout = counters.misses[static_cast<std::size_t>(DrawMiss::Layout)];
    Require(counters.lookups == 3 && counters.absent == 2, mode + ": the three draws made " + std::to_string(counters.lookups) + " draw-cache lookups (" + std::to_string(counters.absent) + " without an entry), expected 3 lookups with 2 absent");
    Require(counters.inserts == 1, mode + ": the three draws inserted " + std::to_string(counters.inserts) + " draw entries, expected 1: a key is cached the second time it is seen");
    Require(layout == 0, mode + ": the repeated draw missed the draw cache on its push offset (" + std::to_string(layout) + " layout misses)");
    Require(counters.hits == 1, mode + ": the repeated draw did not reuse the cached draw (" + std::to_string(counters.hits) + " hits)");
}

void UserDataDrawsReuseTheirShape(bool libraries) {
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const bool verify = std::getenv("APS5_VERIFY_DRAW_DATA_HITS") != nullptr;
    const auto before = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    const auto known = Submit(32.0f, {2.0f, 3.0f, 4.0f});
    Require(known.lookups - before.lookups == 3 && known.absent - before.absent == 3, mode + ": three draws with new user words did not miss the draw key three times");
    Require(known.dataHits - before.dataHits == 3, mode + ": " + std::to_string(known.dataHits - before.dataHits) + " of three draws that differ from a cached draw only in user words reused its decode, expected 3");
    Require(known.inserts == before.inserts, mode + ": a new key of a shape that has an entry was cached at its first sight");
    const auto stages = known.dataStagesReused + known.dataStagesCompiled - before.dataStagesReused - before.dataStagesCompiled;
    Require(stages == 6, mode + ": the three data-only hits accounted " + std::to_string(stages) + " stages, expected 6");
    Require(verify ? known.dataVerified - before.dataVerified == 3 : known.dataStagesReused - before.dataStagesReused == 3, mode + ": the data-only hits reused " + std::to_string(known.dataStagesReused - before.dataStagesReused) + " stages and verified " + std::to_string(known.dataVerified - before.dataVerified) + ", expected the 3 vertex stages, whose user words did not change");
    const auto fresh = Submit(16.0f, {5.0f, 6.0f, 7.0f});
    Require(fresh.inserts - known.inserts == 1, mode + ": three keys of a new shape inserted " + std::to_string(fresh.inserts - known.inserts) + " draw entries, expected 1: the shape's second key");
    Require(fresh.dataHits - known.dataHits == 1, mode + ": the third key of a new shape made " + std::to_string(fresh.dataHits - known.dataHits) + " data-only hits, expected 1");
}

void StageDataHitsRefreshTheirWords(bool libraries) {
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const bool verify = std::getenv("APS5_VERIFY_DRAW_DATA_HITS") != nullptr;
    const bool enabled = std::getenv("APS5_NO_DRAW_STAGE_DATA_HITS") == nullptr;
    Registered(TriangleVertexHeader, TriangleVertexCode.data(), sizeof(TriangleVertexCode), 2);
    Registered(ConstantPixelHeader, ConstantPixelCode.data(), sizeof(ConstantPixelCode), 1);
    ColorMemory.fill(std::byte{0});
    PixelConstant[0] = 0.25f;
    const auto before = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    const auto cached = SubmitConstantDraws(3);
    Require(cached.inserts - before.inserts == 1 && cached.hits - before.hits == 1, mode + ": three draws of a pixel shader reading a constant through its user words did not insert once and hit once");
    Require(!enabled || cached.stageDataInserts - before.stageDataInserts == 1, mode + ": the inserted draw recorded " + std::to_string(cached.stageDataInserts - before.stageDataInserts) + " stages with data positions, expected the fragment stage");
    Require(BrightestColorByte() == 64, mode + ": the draws of constant 0.25 wrote " + std::to_string(BrightestColorByte()) + ", expected 64");
    PixelConstant[0] = 1.0f;
    const auto refreshed = SubmitConstantDraws(1);
    const auto fragmentMisses = refreshed.misses[static_cast<std::size_t>(DrawMiss::FragmentDiffering)] - cached.misses[static_cast<std::size_t>(DrawMiss::FragmentDiffering)];
    if (!enabled) {
        Require(fragmentMisses == 1, mode + ": with stage data hits off, a changed constant did not miss on the fragment stage");
    } else if (verify) {
        Require(refreshed.stageDataVerified - cached.stageDataVerified == 1, mode + ": the stage data hit was verified " + std::to_string(refreshed.stageDataVerified - cached.stageDataVerified) + " times, expected once");
    } else {
        Require(refreshed.hits - cached.hits == 1 && fragmentMisses == 0, mode + ": a draw whose constant changed missed the cached draw");
        Require(refreshed.stageDataHits - cached.stageDataHits == 1 && refreshed.stageDataWords - cached.stageDataWords == 1, mode + ": the hit refreshed " + std::to_string(refreshed.stageDataWords - cached.stageDataWords) + " words in " + std::to_string(refreshed.stageDataHits - cached.stageDataHits) + " stages, expected 1 word in the fragment stage");
        Require(refreshed.inserts == cached.inserts, mode + ": a stage data hit inserted a draw entry");
    }
    Require(BrightestColorByte() == 255, mode + ": the draw of constant 1.0 wrote " + std::to_string(BrightestColorByte()) + ", expected 255");
}

void RefreshedStageDataVerifies() {
    Registered(TriangleVertexHeader, TriangleVertexCode.data(), sizeof(TriangleVertexCode), 2);
    Registered(ConstantPixelHeader, ConstantPixelCode.data(), sizeof(ConstantPixelCode), 1);
    ColorMemory.fill(std::byte{0});
    PixelConstant[0] = 0.25f;
    const auto cached = SubmitConstantDraws(3);
    Require(BrightestColorByte() == 64, "draw-entry verification: the initial constant did not render");
    PixelConstant[0] = 1.0f;
    const auto refreshed = SubmitConstantDraws(1);
    Require(refreshed.verifyHits - cached.verifyHits == 1 && refreshed.stageDataHits - cached.stageDataHits == 1, "draw-entry verification: the refreshed constant did not exercise a verified stage data hit");
    Require(refreshed.verifyMismatches == cached.verifyMismatches, "draw-entry verification: refreshed stage data differed from a fresh capture");
    Require(BrightestColorByte() == 255, "draw-entry verification: the refreshed constant did not render");
}

void RetainedPixelCacheTransitions() {
    auto commands = Draws(32.0f, VertexCode.data(), 1, PixelCode.data(), 1, {});
    Append(commands, 0x76, {0x00c, std::bit_cast<std::uint32_t>(1.0f)});
    const auto draw = [&] {
        for (unsigned i = 0; i < 3; ++i) Append(commands, 0x2d, {3, 2});
    };
    const auto skipped = [&] {
        Append(commands, 0x69, {0x8e, 0, 0});
        Append(commands, 0x69, {0x203, 0});
        Append(commands, 0x69, {0x2dc, 0xaa00});
        draw();
    };
    Append(commands, 0x69, {0x1c4, 1});
    skipped();
    Append(commands, 0x69, {0x8e, 0xf, 0xf});
    draw();
    skipped();
    Append(commands, 0x69, {0x203, 0x400});
    draw();
    skipped();
    Append(commands, 0x69, {0x8e, 0xf, 0xf});
    Append(commands, 0x69, {0x2dc, 0xaa01});
    draw();
    skipped();
    const auto before = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "retained pixel cache transitions failed to submit");
    AgcDriverWaitIdle_nid_postfix();
    const auto after = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    Require(after.lookups - before.lookups == 21 && after.absent - before.absent == 8 && after.inserts - before.inserts == 4 && after.hits - before.hits == 13, "color, execution or alpha-to-coverage transitions reused the wrong cached draw");
    if (std::getenv("APS5_VERIFY_DRAW_RECIPE") != nullptr) Require(after.verifyDecodes > before.verifyDecodes, "pixel cache transitions did not verify any cached decode");
    Require(after.verifyDecodeMismatches == before.verifyDecodeMismatches && after.facadeMismatches == before.facadeMismatches, "cached pixel transitions disagreed with fresh decoding");
}

void SightingsKeepCollidingKeys() {
    DrawKeySightings sightings(DrawKeySightings::Ways);
    Require(!sightings.SeenBefore(0) && sightings.SeenBefore(0), "draw key 0 was not admitted at its second sight");
    Require(!sightings.SeenBefore(11) && !sightings.SeenBefore(12), "an unseen draw key was admitted");
    Require(sightings.SeenBefore(11) && sightings.SeenBefore(12) && sightings.SeenBefore(11), "two keys in one bucket, drawn in turn, were not admitted at their second sight");
    Require(!sightings.SeenBefore(13) && !sightings.SeenBefore(14), "an unseen draw key was admitted");
    Require(!sightings.SeenBefore(0), "the oldest key of a full bucket was kept over the newest");
    Require(sightings.SeenBefore(12) && sightings.SeenBefore(13) && sightings.SeenBefore(14), "a full bucket lost a key newer than the one it replaced");
}

void BufferBaseProofs() {
    using namespace ShaderRecompiler;
    using namespace AgcDriver::DriverDetail;
    auto plan = std::make_shared<IrResourcePlan>();
    plan->srtPlanComplete = plan->resourceTrackingComplete = true;
    plan->valueStorage.Reserve(8);
    plan->descriptorSources.resize(1);
    plan->descriptorSources[0].dwordCount = 4;
    plan->info.buffers.push_back({.source = 0, .read = true});
    ResourceCapture capture;
    capture.plan = plan;
    capture.snapshot.buffers.push_back({{0x2000, 0, 64, 0x30016fac}, 4, 15});
    DispatchVariant variant;
    variant.words = {0x2000, 0, 64, 0x30016fac};
    variant.runs.emplace_back(0x1000, 0x1010);
    auto compiled = std::make_shared<RecompileResult>();
    compiled->bindings.resize(2);
    compiled->bindings[0].role = DescriptorRole::GuestBuffers;
    compiled->bindings[0].count = 1;
    compiled->bindings[1].role = DescriptorRole::FlattenedSrt;
    for (auto& binding : compiled->bindings) binding.guestDescriptor = variant.words;
    variant.compiled = compiled;
    for (std::uint32_t w = 0; w < 4; ++w) {
        auto* value = plan->valueStorage.Create(IrOpcode::LoadAddressU32, IrType::U32, w);
        plan->descriptorSources[0].dwords[w] = value;
        plan->srtReads.push_back({.value = value, .flatOffset = w});
        capture.readTrace.slotReads.emplace_back(value, 0x1000 + w * 4);
    }
    RecordBufferBasePatches(variant, capture);
    Require(variant.bufferBaseMasks == WordBitMasks{{0, 0xfffffffcu}, {1, 0xffffu}}, "base proof did not preserve alignment and descriptor metadata");
    auto changed = variant.words;
    changed[0] = 0x3000;
    changed[1] = 1;
    auto result = *compiled;
    Require(PatchBufferBases(variant, changed, result), "base-only patch was refused");
    Require(result.bindings[0].guestDescriptor == changed && result.bindings[1].guestDescriptor == changed && compiled->bindings[0].guestDescriptor == variant.words, "base patch left a stale descriptor or mutated the cached result");
    for (const auto& [w, bits] : std::array<std::pair<std::size_t, std::uint32_t>, 9>{{{0, 1}, {1, 1u << 16}, {1, 1u << 31}, {2, 64}, {3, 1}, {3, 1u << 12}, {3, 1u << 21}, {3, 1u << 23}, {3, 1u << 30}}}) {
        changed = variant.words;
        changed[w] ^= bits;
        result = *compiled;
        Require(!PatchBufferBases(variant, changed, result), "base patch accepted alignment, stride, size, selector, format, swizzle or type changes");
    }
    changed = variant.words;
    changed[0] = 0;
    Require(!PatchBufferBases(variant, changed, result), "base patch accepted a null-buffer transition");
    plan->controlFlow.resize(1);
    plan->controlFlow[0].condition = plan->descriptorSources[0].dwords[0];
    RecordBufferBasePatches(variant, capture);
    Require(WordMask(variant.bufferBaseMasks, 0) == 0, "base proof accepted a capture control-flow dependency");
    plan->controlFlow.clear();
    auto* alias = plan->valueStorage.Create(IrOpcode::LoadAddressU32, IrType::U32, 4);
    capture.readTrace.slotReads.emplace_back(alias, 0x1000);
    RecordBufferBasePatches(variant, capture);
    Require(WordMask(variant.bufferBaseMasks, 0) == 0, "base proof accepted an aliased capture read");
    capture.readTrace.slotReads.pop_back();
    plan->info.buffers[0].scalar = true;
    RecordBufferBasePatches(variant, capture);
    Require(variant.bufferBaseMasks.empty(), "base proof accepted a scalar buffer");
    plan->info.buffers[0].scalar = false;
    plan->info.buffers[0].written = true;
    RecordBufferBasePatches(variant, capture);
    Require(variant.bufferBaseMasks.empty(), "base proof accepted a writable buffer");
}

void BufferBasePath(const std::filesystem::path& control, bool indirect, bool recorder) {
    using namespace AgcDriver::DriverDetail;
    using R = AgcDriver::ArchB::Recorder;
    using S = AgcDriver::ArchB::Switches;
    using F = AgcDriver::ArchB::Feature;
    alignas(256) static constexpr std::array<std::uint32_t, 11> code{0xf4080100u, 0xfa000000u, 0xbf8cc07fu, 0xe0300000u, 0x80010000u, 0xbf8c3f70u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u};
    alignas(8) static Header header{};
    alignas(256) static std::array<std::uint32_t, 4> descriptor{};
    alignas(256) static std::array<std::array<float, 64>, 4> data{};
    alignas(256) static const std::array<std::uint32_t, 4> arguments{3, 1, 0, 0};
    R::Boundary(true, 0, R::Clock::now());
    Registered(TriangleVertexHeader, TriangleVertexCode.data(), sizeof(TriangleVertexCode), 2);
    Registered(header, code.data(), sizeof(code), 1);
    for (std::size_t i = 0; i < data.size(); ++i) data[i][0] = i == 0 ? 0.25f : 1.0f;
    const auto point = [&](std::size_t index) {
        const auto address = reinterpret_cast<std::uintptr_t>(data[index].data());
        descriptor = {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 64, 0x30016fac};
    };
    const auto draw = [&](std::size_t count) {
        const auto address = reinterpret_cast<std::uintptr_t>(descriptor.data());
        auto commands = Draws(32.0f, TriangleVertexCode.data(), 0, code.data(), 2, std::vector<std::vector<std::uint32_t>>(count, {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)}));
        if (indirect) {
            std::vector<std::uint32_t> changed;
            const auto records = reinterpret_cast<std::uintptr_t>(arguments.data());
            Append(changed, 0x11, {1, static_cast<std::uint32_t>(records), static_cast<std::uint32_t>(records >> 32u)});
            for (std::size_t offset = 0; offset < commands.size();) {
                const auto size = ((commands[offset] >> 16u) & 0x3fffu) + 2u;
                if (((commands[offset] >> 8u) & 0xffu) == 0x2d) Append(changed, 0x24, {0, 0x8c, 0, 2});
                else changed.insert(changed.end(), commands.begin() + offset, commands.begin() + offset + size);
                offset += size;
            }
            commands = std::move(changed);
        }
        return Submit(std::move(commands));
    };
    std::array<std::byte, ColorMemory.size()> reference{};
    unsigned sequence = 0;
    for (const bool enabled : {true, false, true}) {
        std::ofstream(control, std::ios::trunc) << ++sequence << " RECORDER=" << recorder << " FIX_BUFFER_BASE_REFRESH=" << enabled << '\n';
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        S::PollControl();
        Require(S::Enabled(F::FixBufferBaseRefresh) == enabled, "runtime buffer refresh toggle did not apply");
        R::Boundary(recorder, sequence, R::Clock::now());
        point(0);
        const auto before = draw(3);
        Require(BrightestColorByte() == 64, "initial buffer rendered " + std::to_string(BrightestColorByte()) + ", expected 64");
        R::Collect(R::Clock::now());
        point(sequence);
        ColorMemory.fill(std::byte{0});
        const auto after = draw(1);
        Require(BrightestColorByte() == 255, "base-only hit did not read the new buffer");
        Require(after.hits - before.hits == (enabled ? 1u : 0u), "base change took the wrong cache path");
        if (sequence == 1) reference = ColorMemory;
        else Require(reference == ColorMemory, "on/off/on buffer refresh changed frame bytes");
        const auto counts = R::Collect(R::Clock::now()).counters;
        const auto hit = counts.find("fix_buffer_base_hits");
        Require((hit != counts.end() && hit->second > 0) == (enabled && recorder), "buffer base refresh was not exercised");
        if (enabled && recorder && std::getenv("APS5_VERIFY_DRAW_DATA_HITS") != nullptr) {
            const auto verified = counts.find("fix_buffer_base_verified");
            Require(verified != counts.end() && verified->second == hit->second && !counts.contains("fix_buffer_base_verify_mismatches"), "base refresh was not verified against a full build");
        }
    }
    for (const auto& [word, bits] : std::array<std::pair<std::size_t, std::uint32_t>, 3>{{{1, 4u << 16}, {2, 32}, {3, 1u << 12}}}) {
        point(0);
        const auto before = draw(2);
        descriptor[word] ^= bits;
        const auto after = draw(1);
        Require(after.hits == before.hits, "stride, size or format change was accepted by the base-only path");
    }
}

void TsharpMaskProofs() {
    using namespace ShaderRecompiler;
    using namespace AgcDriver::DriverDetail;
    auto plan = std::make_shared<IrResourcePlan>();
    plan->srtPlanComplete = plan->resourceTrackingComplete = true;
    plan->valueStorage.Reserve(8);
    plan->descriptorSources.resize(1);
    plan->materializationSources = {0};
    plan->info.images.push_back({.source = 0});
    auto& descriptor = plan->descriptorSources[0];
    descriptor.dwordCount = 8;
    ResourceCapture capture;
    capture.plan = plan;
    capture.snapshot.images.resize(1);
    capture.snapshot.images[0].dwordCount = 8;
    capture.snapshot.images[0].availableDwordMask = 0xff;
    DispatchVariant variant;
    variant.compiled = std::make_shared<RecompileResult>();
    variant.runs = {{0x1000, 0x1020}};
    variant.words.resize(8);
    for (std::uint32_t word = 0; word < 8; ++word) {
        auto* raw = plan->valueStorage.Create(IrOpcode::LoadAddressU32, IrType::U32, word);
        descriptor.dwords[word] = raw;
        plan->srtReads.push_back({raw, word});
        plan->shaderFlatSlots.push_back(0);
        capture.readTrace.slotReads.emplace_back(raw, 0x1000 + 4 * word);
    }
    const auto record = [&] { RecordUncachedTsharpFeedbackMasks(variant, capture); };
    record();
    Require(variant.feedback.memory == WordBitMasks{{5, 0x020fff00}, {6, 0xff}}, "feedback mask did not identify exact memory words");
    for (std::uint32_t word = 0; word < 8; ++word) {
        for (std::uint32_t bit = 0; bit < 32; ++bit) {
            auto changed = variant.words;
            changed[word] ^= 1u << bit;
            const bool feedback = (word == 5 && ((1u << bit) & 0x020fff00) != 0) || (word == 6 && bit < 8);
            Require(FeedbackWordsEqual(variant.words, changed, variant.feedback.memory) == feedback, "a non-feedback T# bit was ignored");
            Require(!FeedbackWordsEqual(variant.words, changed, {}), "switch-off comparison ignored a changed T# bit");
        }
    }
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> split{{0x1000, 0x1018}, {0x1018, 0x1020}};
    Require(SameCapturedRuns(variant.runs, split) && SameCapturedRuns(split, variant.runs), "equivalent page-split captures compare differently");
    for (const auto& changed : std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>>{{{0x1000, 0x1014}, {0x1018, 0x1020}}, {{0x1004, 0x1024}}, {{0x1000, 0x1020}, {0x1000, 0x1004}}, {{0x1018, 0x1020}, {0x1000, 0x1018}}}) {
        Require(!SameCapturedRuns(variant.runs, changed), "a different captured address set compared equal");
    }
    variant.dataPositions = {5};
    record();
    Require(WordMask(variant.feedback.memory, 5) == 0, "a data position acquired a feedback mask");
    variant.dataPositions.clear();
    plan->shaderFlatSlots[5] = 1;
    record();
    Require(WordMask(variant.feedback.memory, 5) == 0, "a shader data read acquired a feedback mask");
    plan->shaderFlatSlots[5] = 0;
    variant.runs.push_back({0x1014, 0x1018});
    variant.words.push_back(0);
    record();
    Require(WordMask(variant.feedback.memory, 5) == 0, "an overlapping memory run acquired a feedback mask");
    variant.runs.pop_back();
    variant.words.pop_back();
    descriptor.dwords[4] = descriptor.dwords[5];
    record();
    Require(WordMask(variant.feedback.memory, 5) == 0, "a dimension alias acquired a feedback mask");
    descriptor.dwords[4] = plan->srtReads[4].value;
    plan->requiresSpecializationMemory = true;
    record();
    Require(variant.feedback.memory.empty(), "an uncertain plan acquired a feedback mask");
}

void TsharpCachedProofs(bool user, bool refused) {
    using namespace ShaderRecompiler;
    using namespace AgcDriver::DriverDetail;
    using Recorder = AgcDriver::ArchB::Recorder;
    Recorder::Boundary(true, 0, Recorder::Clock::now());
    auto plan = std::make_shared<IrResourcePlan>();
    plan->srtPlanComplete = plan->resourceTrackingComplete = true;
    plan->userDataBase = 16;
    plan->valueStorage.Reserve(16);
    plan->descriptorSources.resize(1);
    plan->materializationSources = {0};
    plan->info.images.push_back({.source = 0});
    auto& descriptor = plan->descriptorSources[0];
    descriptor.dwordCount = 8;
    ResourceCapture capture;
    capture.plan = plan;
    capture.snapshot.userData.resize(8);
    capture.snapshot.images.resize(1);
    capture.snapshot.images[0].dwordCount = 8;
    auto compiled = std::make_shared<RecompileResult>();
    compiled->registrationModule = true;
    compiled->runtimeImageCount = 1;
    compiled->imageMetadataDword = 16;
    compiled->shaderDataUserWords = {5, 6};
    compiled->bindings.resize(3);
    compiled->bindings[0].role = DescriptorRole::FlattenedSrt;
    compiled->bindings[1].role = DescriptorRole::ShaderData;
    compiled->bindings[2].role = DescriptorRole::GuestImages;
    compiled->bindings[2].guestDescriptor.resize(8);
    DispatchVariant variant;
    variant.compiled = compiled;
    variant.runs = {{0x1000, 0x1020}};
    variant.words.resize(8);
    for (std::uint32_t word = 0; word < 8; ++word) {
        auto* raw = plan->valueStorage.Create(user ? IrOpcode::GetUserData : IrOpcode::LoadAddressU32, IrType::U32, word);
        if (user) {
            auto* reg = plan->valueStorage.Create(IrOpcode::Void, IrType::U32, word + 8);
            reg->SetRegister({RegisterBank::UserData, plan->userDataBase + word});
            raw->AddArgument(reg);
        } else capture.readTrace.slotReads.emplace_back(raw, 0x1000 + word * 4);
        descriptor.dwords[word] = raw;
        plan->srtReads.push_back({raw, word});
        plan->shaderFlatSlots.push_back(0);
    }
    if (refused) {
        descriptor.dwords[4] = descriptor.dwords[5];
        descriptor.dwords[7] = descriptor.dwords[6];
    }
    std::size_t calls = 0;
    const auto compare = [&](bool masks) {
        RecordTsharpFeedbackMasks(variant, capture);
        ++calls;
        const auto cached = variant.feedback;
        RecordUncachedTsharpFeedbackMasks(variant, capture);
        Require(cached == variant.feedback, "cached proof differs from the uncached builder");
        Require(!(user ? cached.userData : cached.memory).empty() == masks, "cached proof accepted or refused the wrong positions");
        if (masks) Require((user ? cached.userData : cached.memory) == WordBitMasks{{5, 0x020fff00}, {6, 0xff}}, "cached proof changed the feedback positions");
    };
    compare(!refused);
    compare(!refused);
    if (!refused) {
        capture.snapshot.images[0].dwords[5] = capture.snapshot.images[0].dwords[6] = 1;
        compare(false);
        capture.snapshot.images[0].dwords[5] = capture.snapshot.images[0].dwords[6] = 0;
        if (user) {
            capture.userDataUse.asBase = (1ull << 5) | (1ull << 6);
            compare(false);
            capture.userDataUse.asBase = 0;
            capture.snapshot.userData.resize(5);
            compare(false);
            capture.snapshot.userData.resize(8);
        } else {
            variant.dataPositions = {5, 6};
            compare(false);
            variant.dataPositions.clear();
            capture.readTrace.leaves = {{5, 0x1014}, {6, 0x1018}};
            compare(false);
            capture.readTrace.leaves.clear();
            capture.readTrace.slotReads.emplace_back(plan->srtReads[5].value, 0x2014);
            capture.readTrace.slotReads.emplace_back(plan->srtReads[6].value, 0x2018);
            compare(false);
            capture.readTrace.slotReads.resize(8);
            capture.readTrace.slotReads[4].second = 0x1014;
            capture.readTrace.slotReads[7].second = 0x1018;
            compare(false);
            capture.readTrace.slotReads[4].second = 0x1010;
            capture.readTrace.slotReads[7].second = 0x101c;
            variant.runs = {{0x3000, 0x3020}};
            compare(false);
            for (auto& read : capture.readTrace.slotReads) read.second += 0x2000;
            compare(true);
            IrValue unknown(IrOpcode::LoadAddressU32, IrType::U32, 99);
            capture.readTrace.slotReads.emplace_back(&unknown, 0x4000);
            compare(true);
            capture.readTrace.slotReads.pop_back();
        }
        compare(true);
    }
    const auto counters = Recorder::Collect(Recorder::Clock::now()).counters;
    const auto count = [&](const char* name) { const auto found = counters.find(name); return found == counters.end() ? 0.0 : found->second; };
    Require(count("fix_tsharp_build_plan_calls") == 1 && count("fix_tsharp_build_plan_hits") == calls - 1, "plan proof was rebuilt for another insert");
    Require(count("fix_tsharp_build_verified") == calls && count("fix_tsharp_build_verify_mismatches") == 0, "cached proof verification was incomplete or mismatched");
    Require(!refused || count("fix_tsharp_build_skipped") == calls, "plan-level role refusal was not reused");
    Require(refused || user || count("fix_tsharp_build_calls_fallback") == 1, "unknown trace value did not use the uncached builder");
}

alignas(256) std::array<std::uint32_t, 16> FeedbackTable{};
alignas(256) std::array<std::byte, 4096> FeedbackTexture{};
alignas(256) std::array<float, 32> FeedbackOutput{};
alignas(8) Header FeedbackHeader{};
alignas(256) std::array<std::uint32_t, 40> FeedbackCode{};

void TsharpCachePath(bool dispatch, bool user, bool toggle) {
    using namespace AgcDriver::DriverDetail;
    const bool enabled = AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixTsharpFeedbackMask);
    const bool verify = std::getenv("APS5_VERIFY_DRAW_DATA_HITS") != nullptr;
    AgcDriver::ArchB::Recorder::Boundary(true, 0, AgcDriver::ArchB::Recorder::Clock::now());
    FeedbackTexture.fill(std::byte{255});
    const auto texture = reinterpret_cast<std::uintptr_t>(FeedbackTexture.data());
    FeedbackTable = {static_cast<std::uint32_t>(texture >> 8), static_cast<std::uint32_t>(texture >> 40) | (56u << 20u), 0, 0x90000fac, 0, 0, 0, 0, std::bit_cast<std::uint32_t>(0.25f), 0, 0, 0, 0x92, 0x00fff000, 0, 0};
    std::vector<std::uint32_t> code;
    if (!user) {
        const auto pointer = dispatch ? 2u : 0u;
        code = {0xf40c0200u | pointer, 0xfa000000u, 0xf4000400u | pointer, 0xfa000020u, 0xf4080500u | pointer, 0xfa000030u, 0xbf8cc07fu};
    }
    code.insert(code.end(), {0x7e2802ffu, 0x3f000000u, 0x7e2a02ffu, 0x3f000000u, 0xf09c0f08u, user ? 0x00220a14u : 0x00a20a14u, 0xbf8c3f70u, 0x10001410u});
    if (dispatch) code.insert(code.end(), {0x7e020280u, 0xe0700000u, 0x80000001u, 0xbf810000u});
    else code.insert(code.end(), {0xf800180fu, 0x00000000u, 0xbf810000u});
    std::copy(code.begin(), code.end(), FeedbackCode.begin());
    Registered(FeedbackHeader, FeedbackCode.data(), code.size() * 4, dispatch ? 0 : 1);
    if (!dispatch) Registered(TriangleVertexHeader, TriangleVertexCode.data(), sizeof(TriangleVertexCode), 2);
    std::uint32_t unusedUserWord = 0;
    const auto submit = [&](std::size_t count) {
        const auto table = reinterpret_cast<std::uintptr_t>(FeedbackTable.data());
        const auto output = reinterpret_cast<std::uintptr_t>(FeedbackOutput.data());
        std::vector<std::uint32_t> words(user ? 17u : dispatch ? 6u : 2u);
        if (user) {
            std::copy_n(FeedbackTable.begin(), 8, words.begin() + 8);
            std::copy_n(FeedbackTable.begin() + 12, 4, words.begin() + 4);
            words[16] = FeedbackTable[8];
            words[0] = unusedUserWord;
        } else {
            words[dispatch ? 4 : 0] = static_cast<std::uint32_t>(table);
            words[dispatch ? 5 : 1] = static_cast<std::uint32_t>(table >> 32);
        }
        if (!dispatch) return Submit(Draws(32.0f, TriangleVertexCode.data(), 0, FeedbackCode.data(), static_cast<std::uint32_t>(words.size()), std::vector<std::vector<std::uint32_t>>(count, words))).hits;
        words[0] = static_cast<std::uint32_t>(output);
        words[1] = static_cast<std::uint32_t>(output >> 32) & 0xffffu;
        words[2] = sizeof(FeedbackOutput);
        words[3] = 0x31016facu;
        const auto address = reinterpret_cast<std::uintptr_t>(FeedbackCode.data());
        std::vector<std::uint32_t> commands;
        Append(commands, 0x76, {0x20c, static_cast<std::uint32_t>(address >> 8), static_cast<std::uint32_t>(address >> 40)});
        Append(commands, 0x76, {0x207, 1, 1, 1});
        Append(commands, 0x76, {0x213, static_cast<std::uint32_t>(words.size()) << 1});
        for (std::size_t w = 0; w < words.size(); ++w) Append(commands, 0x76, {0x240u + static_cast<std::uint32_t>(w), words[w]});
        for (std::size_t draw = 0; draw < count; ++draw) Append(commands, 0x15, {1, 1, 1, 0x8041});
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "feedback dispatch submission failed");
        AgcDriverWaitIdle_nid_postfix();
        return Driver::Get().DispatchCacheCounters().equal;
    };
    const auto result = [&] {
        if (!dispatch) return BrightestColorByte();
        AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(FeedbackOutput.data()), sizeof(FeedbackOutput));
        return static_cast<unsigned>(FeedbackOutput[0] * 255.0f + 0.5f);
    };
    auto hits = submit(3);
    Require(result() == 64, "initial feedback test did not render the expected constant");
    for (const auto& [word, bits] : std::array<std::pair<std::size_t, std::uint32_t>, 3>{{{6, 0xa5}, {5, 0x02000000}, {5, 0x000abc00}}}) {
        FeedbackTable[word] ^= bits;
        const auto now = submit(1);
        if ((now == hits + 1) != enabled) {
            std::fprintf(stderr, "feedback word %zu hits %llu -> %llu\n", word, static_cast<unsigned long long>(hits), static_cast<unsigned long long>(now));
            for (const auto& [name, value] : AgcDriver::ArchB::Recorder::Collect(AgcDriver::ArchB::Recorder::Clock::now()).counters) if (name.starts_with("fix_tsharp")) std::fprintf(stderr, "%s: %.0f\n", name.c_str(), value);
        }
        Require((now == hits + 1) == enabled, "feedback-only change took the wrong cache path");
        Require(result() == 64, "feedback-only change altered rendered output");
        hits = now;
    }
    if (!user) {
        FeedbackTable[6] ^= 0x5a;
        FeedbackTable[8] = std::bit_cast<std::uint32_t>(1.0f);
        const auto before = Driver::Get().DrawCacheCounters();
        const auto now = submit(1);
        Require((now == hits + 1) == enabled, "data and feedback change took the wrong cache path");
        Require(result() == 255, "data and feedback change retained stale scalar data");
        if (!dispatch && enabled && !verify) Require(Driver::Get().DrawCacheCounters().stageDataWords == before.stageDataWords + 1, "feedback words were refreshed as data");
        hits = now;
    }
    if (toggle) {
        const auto change = [&](unsigned sequence, bool on) {
            std::ofstream(std::getenv("APS5_ARCHB_CONTROL_FILE"), std::ios::trunc) << sequence << " FIX_TSHARP_FEEDBACK_MASK=" << on << '\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            AgcDriver::ArchB::Switches::PollControl();
            Require(AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixTsharpFeedbackMask) == on, "runtime feedback toggle was not applied");
        };
        change(1, false);
        FeedbackTable[6] ^= 0x10;
        const auto off = submit(1);
        Require(off == hits, "switch-off consumed a masked variant");
        change(2, true);
        FeedbackTable[6] ^= 0x20;
        const auto on = submit(1);
        Require(on == off + 1, "switch-on lost its existing masked variant");
        hits = on;
    }
    if (!dispatch && user && std::getenv("APS5_NO_DRAW_KEY") == nullptr && std::getenv("APS5_NO_DRAW_DATA_HITS") == nullptr) {
        FeedbackTable[6] ^= 0x40;
        unusedUserWord = 1;
        const auto before = Driver::Get().DrawCacheCounters();
        hits = submit(1);
        const auto after = Driver::Get().DrawCacheCounters();
        const auto stages = verify ? after.dataVerified - before.dataVerified : after.dataStagesReused - before.dataStagesReused;
        Require(stages == (enabled ? 2u : 1u), "partial stage reuse did not apply the feedback proof");
        Require(result() == 64, "partial feedback reuse changed rendered output");
    }
    for (const auto& [word, bits] : std::array<std::pair<std::size_t, std::uint32_t>, 7>{{{1, 3u << 30}, {2, 1u << 14}, {3, 1u}, {1, 1u << 20}, {5, 1u << 4}, {3, 1u << 16}, {3, 1u << 12}}}) {
        FeedbackTable[word] ^= bits;
        const auto missed = submit(1);
        Require(missed == hits, "a dimension, format, selector or mip-level change hit through the feedback mask");
    }
    FeedbackTable[0] += 1;
    const auto missed = submit(1);
    Require(missed == hits, "an address change hit through the feedback mask");
    const auto counts = AgcDriver::ArchB::Recorder::Collect(AgcDriver::ArchB::Recorder::Clock::now()).counters;
    const auto count = [&](const char* name) { const auto it = counts.find(name); return it == counts.end() ? 0.0 : it->second; };
    Require((count("fix_tsharp_mask_hits") > 0) == enabled, "feedback hits were not counted correctly");
    Require(!enabled || count("fix_tsharp_mask_positions") >= 2, "feedback positions were not counted");
    Require(count("fix_tsharp_mask_verify_mismatches") == 0, "feedback verification mismatched");
    Require(count("fix_tsharp_build_verify_mismatches") == 0, "cached feedback proof verification mismatched");
    Require(!enabled || !verify || dispatch || count("fix_tsharp_mask_verified") >= 3, "feedback-only hits were not verified");
}

void PipelineStateReusePath(const std::filesystem::path& control) {
    using R = AgcDriver::ArchB::Recorder;
    using S = AgcDriver::ArchB::Switches;
    using F = AgcDriver::ArchB::Feature;
    R::Boundary(true, 0, R::Clock::now());
    Registered(TriangleVertexHeader, TriangleVertexCode.data(), sizeof(TriangleVertexCode), 2);
    Registered(ConstantPixelHeader, ConstantPixelCode.data(), sizeof(ConstantPixelCode), 1);
    PixelConstant[0] = 0.25f;
    SubmitConstantDraws(5);
    R::Collect(R::Clock::now());
    std::array<std::byte, ColorMemory.size()> reference{};
    unsigned sequence = 0;
    for (const bool enabled : {false, true, false, true}) {
        std::ofstream(control, std::ios::trunc) << ++sequence << " FIX_PIPELINE_STATE_REUSE=" << enabled << '\n';
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        S::PollControl();
        Require(S::Enabled(F::FixPipelineStateReuse) == enabled, "runtime pipeline reuse toggle did not apply");
        ColorMemory.fill(std::byte{0});
        SubmitConstantDraws(12);
        Require(BrightestColorByte() == 64, "pipeline reuse changed rendered color");
        if (sequence == 1) reference = ColorMemory;
        else Require(reference == ColorMemory, "on/off/on pipeline reuse changed frame bytes");
        const auto counts = R::Collect(R::Clock::now()).counters;
        for (const auto& [name, value] : counts) if (name.starts_with("fix_pso_reuse_")) std::cout << name << "=" << value << "\n";
        const auto count = [&](const char* name) { const auto found = counts.find(name); return found == counts.end() ? 0.0 : found->second; };
        for (const auto* name : {"fix_pso_reuse_state_hits", "fix_pso_reuse_skipped_dynamic", "fix_pso_reuse_skipped_binds", "fix_pso_reuse_skipped_push"}) {
            std::cout << name << "=" << count(name) << " enabled=" << enabled << '\n';
            Require(count(name) == 0, std::string("untracked draw reused pipeline state: ") + name);
        }
        Require((count("fix_pso_reuse_refused_untracked_draw") > 0) == enabled, "untracked draw refusal was not counted");
        Require(count("recorder_counter_overflow") == 0, "pipeline reuse counters overflowed");
    }
    PixelConstant[0] = 1.0f;
    SubmitConstantDraws(4);
    Require(BrightestColorByte() == 255, "pipeline reuse retained stale push or resource bytes");
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--buffer-base-proof") {
            BufferBaseProofs();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]).starts_with("--buffer-base")) {
            struct Control {
                std::filesystem::path path = std::filesystem::temp_directory_path() / ("agc-buffer-base-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
                ~Control() { std::filesystem::remove(path); }
            } control;
#ifdef _WIN32
            _putenv_s("APS5_ARCHB_GATE_RUN", "1");
            _putenv_s("APS5_ARCHB_CONTROL_FILE", control.path.string().c_str());
#else
            setenv("APS5_ARCHB_GATE_RUN", "1", 1);
            setenv("APS5_ARCHB_CONTROL_FILE", control.path.string().c_str(), 1);
#endif
            const auto probe = OpenVulkanTestDevice();
            if (!probe) return VulkanTestSkipped;
            BufferBasePath(control.path, std::string_view(argv[1]) == "--buffer-base-indirect", std::string_view(argv[1]) != "--buffer-base-no-recorder");
            AgcDriverShutdown_nid_postfix();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--tsharp-proof") {
            TsharpMaskProofs();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]).starts_with("--tsharp-cached-")) {
            const std::string_view mode(argv[1]);
            TsharpCachedProofs(mode.find("user") != std::string_view::npos, mode.find("refused") != std::string_view::npos);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]).starts_with("--tsharp-")) {
            const std::string_view mode(argv[1]);
            const bool toggle = mode.find("toggle") != std::string_view::npos;
            struct ControlFile {
                std::filesystem::path path;
                ~ControlFile() { if (!path.empty()) std::filesystem::remove(path); }
            } control;
            if (toggle) {
                control.path = std::filesystem::temp_directory_path() / ("agc-f1-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
#ifdef _WIN32
                _putenv_s("APS5_ARCHB_GATE_RUN", "1");
                _putenv_s("APS5_ARCHB_CONTROL_FILE", control.path.string().c_str());
#else
                setenv("APS5_ARCHB_GATE_RUN", "1", 1);
                setenv("APS5_ARCHB_CONTROL_FILE", control.path.string().c_str(), 1);
#endif
            }
            const auto probe = OpenVulkanTestDevice();
            if (!probe) return VulkanTestSkipped;
            TsharpCachePath(mode.find("dispatch") != std::string_view::npos, mode.find("user") != std::string_view::npos, toggle);
            AgcDriverShutdown_nid_postfix();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--pipeline-state-reuse") {
            struct Control {
                std::filesystem::path path = std::filesystem::temp_directory_path() / ("agc-h4-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
                ~Control() { std::filesystem::remove(path); }
            } control;
#ifdef _WIN32
            _putenv_s("APS5_ARCHB_GATE_RUN", "1");
            _putenv_s("APS5_ARCHB_CONTROL_FILE", control.path.string().c_str());
#else
            setenv("APS5_ARCHB_GATE_RUN", "1", 1);
            setenv("APS5_ARCHB_CONTROL_FILE", control.path.string().c_str(), 1);
#endif
            const auto probe = OpenVulkanTestDevice();
            if (!probe) return VulkanTestSkipped;
            PipelineStateReusePath(control.path);
            AgcDriverShutdown_nid_postfix();
            return 0;
        }
        SightingsKeepCollidingKeys();
        bool libraries = false;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            libraries = device->GraphicsPipelineLibraries();
        }
        if (!libraries && std::getenv("APS5_NO_GPL") == nullptr) return VulkanTestSkipped;
        if (std::getenv("APS5_VERIFY_DRAW_ENTRIES") != nullptr) {
            RefreshedStageDataVerifies();
        } else {
            ThirdDrawHitsTheCache(libraries);
            UserDataDrawsReuseTheirShape(libraries);
            StageDataHitsRefreshTheirWords(libraries);
            RetainedPixelCacheTransitions();
        }
        AgcDriverShutdown_nid_postfix();
        Require(AgcDriver::Graphics::PipelineLibraryWorkers() == 0, "the driver's shutdown left the pipeline library worker running");
        std::cout << "A repeated draw reused the cached draw " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
