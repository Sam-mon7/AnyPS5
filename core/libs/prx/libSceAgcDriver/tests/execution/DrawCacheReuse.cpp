#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::DrawMiss;
using AgcDriver::DriverDetail::DrawKeySightings;

alignas(256) constexpr std::array<std::uint32_t, 8> VertexCode{0x7e000208u, 0x7e020280u, 0x7e040280u, 0x7e0602f2u, 0xf80008cfu, 0x03020100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
alignas(256) std::array<std::byte, 1024> ColorMemory{};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};

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

std::vector<std::uint32_t> DrawCommands() {
    const auto color = reinterpret_cast<std::uintptr_t>(ColorMemory.data());
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(PixelCode.data());
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
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
    };
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 1u << 1u}, {0x8c, 0},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 1u << 1u}, {0x00c, std::bit_cast<std::uint32_t>(1.0f)},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    return commands;
}

void ThirdDrawHitsTheCache(bool libraries) {
    Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    auto commands = DrawCommands();
    Append(commands, 0x2d, {3, 2});
    Append(commands, 0x2d, {3, 2});
    Append(commands, 0x2d, {3, 2});
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
    const auto counters = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    const auto mode = std::string(libraries ? "with" : "without") + " graphics pipeline libraries";
    const auto layout = counters.misses[static_cast<std::size_t>(DrawMiss::Layout)];
    Require(counters.lookups == 3 && counters.absent == 2, mode + ": the three draws made " + std::to_string(counters.lookups) + " draw-cache lookups (" + std::to_string(counters.absent) + " without an entry), expected 3 lookups with 2 absent");
    Require(counters.inserts == 1, mode + ": the three draws inserted " + std::to_string(counters.inserts) + " draw entries, expected 1: a key is cached the second time it is seen");
    Require(layout == 0, mode + ": the repeated draw missed the draw cache on its push offset (" + std::to_string(layout) + " layout misses)");
    Require(counters.hits == 1, mode + ": the repeated draw did not reuse the cached draw (" + std::to_string(counters.hits) + " hits)");
}

void RecentlyHitDrawSurvivesColdInsertions() {
    const char* capacity = std::getenv("APS5_DRAW_CACHE_ENTRIES");
    Require(capacity != nullptr && std::string(capacity) == "16", "draw-cache recency test requires APS5_DRAW_CACHE_ENTRIES=16");
    std::vector<float> values;
    for (unsigned key = 2; key <= 16; ++key) values.push_back(static_cast<float>(key));
    values.insert(values.end(), {1.0f, 17.0f, 1.0f, 1.0f, 2.0f});
    auto commands = DrawCommands();
    for (const auto value : values) {
        Append(commands, 0x76, {0x00c, std::bit_cast<std::uint32_t>(value)});
        Append(commands, 0x2d, {3, 2});
    }
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    const auto before = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw-cache recency submission failed");
    AgcDriverWaitIdle_nid_postfix();
    const auto after = AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters();
    Require(after.lookups - before.lookups == 20, "draw-cache recency workload did not execute all draws");
    Require(after.hits - before.hits == 3 && after.absent - before.absent == 17, "a recently hit draw was evicted before a cold entry");
    Require(after.touches - before.touches == 2, "draw-cache hits did not promote two non-front entries exactly once");
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

alignas(256) constexpr std::array<std::uint32_t, 16> IndirectVertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u, 0xf80008cfu, 0x04030201u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 8> IndirectPixelCode{
    0xf4000080u, 0xfa000000u, 0xbf8cc07fu, 0x7e000202u,
    0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u,
};
alignas(8) Header IndirectVertexHeader{};
alignas(8) Header IndirectPixelHeader{};
alignas(4096) std::array<std::uint32_t, 1024> FragmentInput{};
using IndirectRecord = std::array<std::uint32_t, 4>;
alignas(16) std::array<IndirectRecord, 3> IndirectRecords{};
alignas(16) std::uint32_t IndirectCount = 0;
std::array<std::atomic<unsigned>, 3> IndirectRecordReads{};

void ObserveIndirectCapture(std::uint64_t address, std::size_t bytes) {
    using namespace AgcDriver::GuestMemory;
    if (CurrentReadSite() == ReadSite::IndirectArguments && bytes == sizeof(IndirectRecord)) {
        for (std::size_t index = 0; index < IndirectRecords.size(); ++index) {
            if (address == reinterpret_cast<std::uintptr_t>(IndirectRecords[index].data())) ++IndirectRecordReads[index];
        }
    }
}

std::array<std::byte, 1024> SubmitIndirect(std::span<const IndirectRecord> records, bool cpuFallback, bool queuedArguments = false, bool inaccessibleInput = false, std::optional<std::uint32_t> drawCount = std::nullopt) {
    Require(!records.empty() && records.size() <= IndirectRecords.size(), "invalid indirect regression record count");
    const auto arguments = reinterpret_cast<std::uintptr_t>(IndirectRecords.data());
    static std::size_t inputSlot = 0;
    Require(inputSlot < FragmentInput.size(), "indirect regression exhausted fragment inputs");
    const auto input = inaccessibleInput ? 0x10000u + inputSlot++ * 4u : reinterpret_cast<std::uintptr_t>(&FragmentInput[inputSlot++]);
    std::array<std::byte, 1024> clear;
    clear.fill(std::byte{0x40});
    AgcDriver::GuestMemory::Write(reinterpret_cast<std::uintptr_t>(ColorMemory.data()), clear);
    const std::uint32_t red = std::bit_cast<std::uint32_t>(1.0f);
    if (inaccessibleInput) Require(!AgcDriver::GuestMemory::Accessible(reinterpret_cast<const void*>(input), sizeof(red)), "empty indirect fixture input is unexpectedly mapped");
    else AgcDriver::GuestMemory::Write(input, std::as_bytes(std::span(&red, 1)));
    const std::array<IndirectRecord, 3> empty{};
    AgcDriver::GuestMemory::Write(arguments, std::as_bytes(std::span(empty)));
    if (!queuedArguments) AgcDriver::GuestMemory::Write(arguments, std::as_bytes(records));
    const auto countAddress = reinterpret_cast<std::uintptr_t>(&IndirectCount);
    const auto initialCount = queuedArguments ? 0u : drawCount.value_or(0);
    AgcDriver::GuestMemory::Write(countAddress, std::as_bytes(std::span(&initialCount, 1)));

    auto commands = DrawCommands();
    const auto vertex = reinterpret_cast<std::uintptr_t>(IndirectVertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(IndirectPixelCode.data());
    Append(commands, 0x79, {0x24a, 0});
    Append(commands, 0x76, {0xc8, static_cast<std::uint32_t>(vertex >> 8u), static_cast<std::uint32_t>(vertex >> 40u)});
    Append(commands, 0x76, {0x8b, 3u << 1u, 0, 0, 0});
    Append(commands, 0x76, {0x008, static_cast<std::uint32_t>(pixel >> 8u), static_cast<std::uint32_t>(pixel >> 40u)});
    Append(commands, 0x76, {0x00b, 2u << 1u, static_cast<std::uint32_t>(input), static_cast<std::uint32_t>(input >> 32u)});
    Append(commands, 0x11, {1, static_cast<std::uint32_t>(arguments), static_cast<std::uint32_t>(arguments >> 32u)});
    if (queuedArguments) {
        Require(records.size() == 1, "queued-argument regression requires one record");
        Append(commands, 0x37, {0x00100200u, static_cast<std::uint32_t>(arguments), static_cast<std::uint32_t>(arguments >> 32u), records[0][0], records[0][1], records[0][2], records[0][3]});
        if (drawCount) Append(commands, 0x37, {0x00100200u, static_cast<std::uint32_t>(countAddress), static_cast<std::uint32_t>(countAddress >> 32u), *drawCount});
    }
    const auto instance = cpuFallback ? 0x8eu : 0x280u;
    if (records.size() == 1 && !drawCount) Append(commands, 0x24, {0, 0x280, instance, 2});
    else Append(commands, 0x2c, {0, 0x280, instance, 0x280u | (drawCount ? 1u << 30u : 0u), static_cast<std::uint32_t>(records.size()), drawCount ? static_cast<std::uint32_t>(countAddress) : 0u, drawCount ? static_cast<std::uint32_t>(countAddress >> 32u) : 0u, sizeof(IndirectRecord), 2});

    for (auto& count : IndirectRecordReads) count = 0;
    AgcDriver::GuestMemory::SetReadGuard(ObserveIndirectCapture);
    struct ResetReadGuard {
        ~ResetReadGuard() { AgcDriver::GuestMemory::SetReadGuard(nullptr); }
    } resetReadGuard;
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "indirect capture regression submission failed");
    AgcDriverWaitIdle_nid_postfix();
    if (cpuFallback) {
        const auto count = std::min(static_cast<std::uint32_t>(records.size()), drawCount.value_or(static_cast<std::uint32_t>(records.size())));
        for (std::size_t index = 0; index < IndirectRecordReads.size(); ++index) {
            Require(IndirectRecordReads[index] == (index < count ? 1u : 0u), "CPU indirect arguments were skipped or reread instead of reusing the ordered snapshot");
        }
    }
    return ColorMemory;
}

void EmptyCpuIndirectDrawsSkipFragmentCapture() {
    Registered(IndirectVertexHeader, IndirectVertexCode.data(), sizeof(IndirectVertexCode), 2);
    Registered(IndirectPixelHeader, IndirectPixelCode.data(), sizeof(IndirectPixelCode), 1);
    const std::array<IndirectRecord, 1> active{IndirectRecord{3, 1, 0, 0}};
    const auto rendered = SubmitIndirect(active, true);
    const auto kept = std::count(rendered.begin(), rendered.end(), std::byte{0x40});
    Require(kept != 0 && kept != rendered.size(), "the active indirect triangle did not draw an edge through the target");
    for (const auto record : {IndirectRecord{0, 1, 0, 0}, IndirectRecord{3, 0, 0, 0}}) {
        const std::array<IndirectRecord, 1> records{record};
        const auto empty = SubmitIndirect(records, true, false, true);
        Require(std::all_of(empty.begin(), empty.end(), [](std::byte value) { return value == std::byte{0x40}; }), "an empty CPU indirect draw changed the render target");
    }
    const std::array mixed{IndirectRecord{3, 0, 0, 0}, IndirectRecord{3, 1, 0, 0}, IndirectRecord{0, 1, 0, 0}};
    Require(SubmitIndirect(mixed, true) == rendered, "the active middle record in a mixed indirect packet rendered different pixels");
    Require(SubmitIndirect(active, true, true) == rendered, "queued argument writes were not visible before the empty-draw decision");
    const auto zeroCount = SubmitIndirect(active, true, false, true, 0);
    Require(std::all_of(zeroCount.begin(), zeroCount.end(), [](std::byte value) { return value == std::byte{0x40}; }), "a zero indirect count changed the render target");
    Require(SubmitIndirect(active, true, true, false, 1) == rendered, "queued indirect count and arguments were not visible before capture");
    Require(SubmitIndirect(mixed, true, false, false, 99) == rendered, "an indirect count exceeding the packet maximum was not clamped");
    Require(SubmitIndirect(active, false) == rendered, "the GPU-eligible indirect control rendered different pixels");
    const std::array<IndirectRecord, 1> zeroInstances{IndirectRecord{3, 0, 0, 0}};
    const auto emptyControl = SubmitIndirect(zeroInstances, false);
    Require(std::all_of(emptyControl.begin(), emptyControl.end(), [](std::byte value) { return value == std::byte{0x40}; }), "the empty GPU-eligible indirect control changed the render target");
}

alignas(256) constexpr std::array<std::uint32_t, 19> SharedPageVertexCode{
    0xf4000084u, 0xfa000000u, 0xbf8cc07fu,
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e080202u, 0xf80008cfu, 0x04030201u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};
alignas(8) Header SharedPageVertexHeader{};
alignas(4096) std::array<std::uint32_t, 1024> SharedCaptureInput{};
std::array<std::byte, 1024> SubmitSharedPageIndirect(float color, bool queuedColor) {
    static std::size_t slot = 0;
    Require(slot + 1 < SharedCaptureInput.size(), "shared-page regression exhausted inputs");
    const auto position = reinterpret_cast<std::uintptr_t>(&SharedCaptureInput[slot]);
    const auto fragment = position + sizeof(std::uint32_t);
    slot += 2;
    const std::array input{std::bit_cast<std::uint32_t>(1.0f), std::bit_cast<std::uint32_t>(color)};
    AgcDriver::GuestMemory::Write(position, std::as_bytes(std::span(input)));
    std::array<std::byte, 1024> clear;
    clear.fill(std::byte{0x40});
    AgcDriver::GuestMemory::Write(reinterpret_cast<std::uintptr_t>(ColorMemory.data()), clear);
    const auto arguments = reinterpret_cast<std::uintptr_t>(IndirectRecords.data());
    const IndirectRecord active{3, 1, 0, 0};
    AgcDriver::GuestMemory::Write(arguments, std::as_bytes(std::span(active)));

    auto commands = DrawCommands();
    const auto vertex = reinterpret_cast<std::uintptr_t>(SharedPageVertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(IndirectPixelCode.data());
    Append(commands, 0x79, {0x24a, 0});
    Append(commands, 0x76, {0xc8, static_cast<std::uint32_t>(vertex >> 8u), static_cast<std::uint32_t>(vertex >> 40u)});
    Append(commands, 0x76, {0x8b, 3u << 1u, static_cast<std::uint32_t>(position), static_cast<std::uint32_t>(position >> 32u), 0});
    Append(commands, 0x76, {0x008, static_cast<std::uint32_t>(pixel >> 8u), static_cast<std::uint32_t>(pixel >> 40u)});
    Append(commands, 0x76, {0x00b, 2u << 1u, static_cast<std::uint32_t>(fragment), static_cast<std::uint32_t>(fragment >> 32u)});
    Append(commands, 0x11, {1, static_cast<std::uint32_t>(arguments), static_cast<std::uint32_t>(arguments >> 32u)});
    if (queuedColor) Append(commands, 0x37, {0x00100200u, static_cast<std::uint32_t>(fragment), static_cast<std::uint32_t>(fragment >> 32u), std::bit_cast<std::uint32_t>(1.0f)});
    Append(commands, 0x24, {0, 0x280, 0x8e, 2});

    const auto retries = AgcDriver::DriverDetail::captureRetries.load();
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "shared-page indirect submission failed");
    AgcDriverWaitIdle_nid_postfix();
    if (queuedColor && AgcDriver::DriverDetail::DrawPipeline::Depth() == 0) {
        Require(AgcDriver::DriverDetail::captureRetries.load() == retries + 1, "the shared-page indirect draw did not exercise exactly one queued-label capture retry");
    }
    return ColorMemory;
}

void QueuedFragmentWordInvalidatesFrontPage() {
    using namespace AgcDriver::DriverDetail;
    Require(DeferLabels() && !QueuedLabelTable(), "shared-page regression requires deferred labels with the queued-label table disabled");
    Registered(SharedPageVertexHeader, SharedPageVertexCode.data(), sizeof(SharedPageVertexCode), 2);
    const auto oldColor = SubmitSharedPageIndirect(0.0f, false);
    const auto newColor = SubmitSharedPageIndirect(1.0f, false);
    Require(oldColor != newColor, "the shared-page fragment input did not affect rendered pixels");
    const auto kept = std::count(newColor.begin(), newColor.end(), std::byte{0x40});
    Require(kept != 0 && kept != newColor.size(), "the shared-page vertex input did not produce the expected triangle edge");
    Require(SubmitSharedPageIndirect(0.0f, true) == newColor, "fragment capture reused the stale front-stage page after publishing a queued color word");
}

}

int main() {
    try {
#ifdef _WIN32
        Require(_putenv_s("APS5_NO_QUEUED_LABELS", "1") == 0 && _putenv_s("APS5_LABEL_FLUSH_US", "60000000") == 0, "cannot configure deferred-label regression");
#else
        Require(setenv("APS5_NO_QUEUED_LABELS", "1", 1) == 0 && setenv("APS5_LABEL_FLUSH_US", "60000000", 1) == 0, "cannot configure deferred-label regression");
#endif
        SightingsKeepCollidingKeys();
        bool libraries = false;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            libraries = device->GraphicsPipelineLibraries();
        }
        if (!libraries && std::getenv("APS5_NO_GPL") == nullptr) return VulkanTestSkipped;
        ThirdDrawHitsTheCache(libraries);
        RecentlyHitDrawSurvivesColdInsertions();
        EmptyCpuIndirectDrawsSkipFragmentCapture();
        QueuedFragmentWordInvalidatesFrontPage();
        AgcDriverShutdown_nid_postfix();
        std::cout << "A repeated draw reused the cached draw " << (libraries ? "with" : "without") << " graphics pipeline libraries\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
