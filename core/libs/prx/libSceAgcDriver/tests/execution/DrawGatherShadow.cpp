#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/GatherShadow.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdlib>
#include <functional>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::DriverDetail;
using AgcDriver::Graphics::Require;
using Role = ShaderRecompiler::DescriptorRole;

alignas(64) std::array<std::uint32_t, 8> Table{};
alignas(64) std::array<std::uint32_t, 8> MovedTable{};
alignas(64) std::array<std::uint32_t, 4> Parent{};
alignas(64) std::array<std::uint32_t, 4> Child{};
alignas(64) std::array<std::uint32_t, 4> MovedChild{};

constexpr std::uint64_t BufferBase = 0x0000123400004000ull;
constexpr std::uint64_t MovedBufferBase = 0x0000123400008000ull;
constexpr std::uint32_t Stride = 16;
constexpr std::uint32_t Records = 64;
constexpr std::uint32_t Format = 0x00027facu;
constexpr std::uint64_t ValueBase = 0x0000555500001000ull;

std::uint32_t Low(const void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const void* pointer) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u);
}

const GatherReader HostReader = [](std::uint64_t address, std::span<std::uint32_t> out) {
    if (address == 0 || address % 4 != 0) return false;
    std::memcpy(out.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), out.size_bytes());
    return true;
};

void FillTable(std::array<std::uint32_t, 8>& table, std::uint64_t base, std::uint32_t constant) {
    table = {static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(base >> 32u) | (Stride << 16u), Records, Format, constant, 3u, 0u, 0u};
}

struct Fixture {
    std::shared_ptr<DispatchVariant> variant = std::make_shared<DispatchVariant>();
    std::vector<std::uint32_t> user;
};

Fixture TableFixture() {
    FillTable(Table, BufferBase, 0x3f800000u);
    Fixture fixture;
    auto& variant = *fixture.variant;
    fixture.user = {Low(Table.data()), High(Table.data()), static_cast<std::uint32_t>(ValueBase), 0x11111111u, 0x22222222u, Records};
    auto compiled = std::make_shared<ShaderRecompiler::RecompileResult>();
    ShaderRecompiler::DescriptorBinding buffers{ShaderRecompiler::DescriptorKind::StorageBuffer, Role::GuestBuffers, 0, 0, 2, {}};
    buffers.guestDescriptor = {Table[0], Table[1], Table[2], Table[3], static_cast<std::uint32_t>(ValueBase), static_cast<std::uint32_t>(ValueBase >> 32u) | (Stride << 16u), Records, Format};
    ShaderRecompiler::DescriptorBinding flat{ShaderRecompiler::DescriptorKind::StorageBuffer, Role::FlattenedSrt, 0, 61, 1, {Table[0], 7u, Table[4], 9u}};
    ShaderRecompiler::DescriptorBinding data{ShaderRecompiler::DescriptorKind::StorageBuffer, Role::ShaderData, 0, 62, 1, {fixture.user[3], 0u, 0u, 0u}};
    compiled->bindings = {buffers, flat, data};
    compiled->pushConstants.resize(16);
    std::memcpy(compiled->pushConstants.data(), &fixture.user[3], sizeof(std::uint32_t));
    compiled->shaderDataUserWords = {3};
    variant.compiled = compiled;
    variant.runs = {{reinterpret_cast<std::uintptr_t>(Table.data()), reinterpret_cast<std::uintptr_t>(Table.data()) + sizeof(Table)}};
    variant.words.assign(Table.begin(), Table.end());
    variant.runBases = {0};
    variant.runParents.resize(1);
    variant.relocatableWords = 1u;
    variant.userWordsWalked = 1u | 2u;
    variant.valueWords = (1u << 2u) | (1u << 5u);
    variant.valueSlots = {{2u, 0u, 4u}, {5u, 0u, 6u}};
    variant.dataPositions = {4};
    variant.dataSlots = {2};
    variant.flatBinding = 1;
    variant.bufferBaseMasks = {{0u, 0xfffffffcu}, {1u, 0xffffu}};
    variant.bufferBasePatches = {{0u, 1u, 0u}, {0u, 0u, 0u}, {1u, 0u, 1u}};
    return fixture;
}

constexpr GatherOptions AllOn{true, true, true, true};

void ClassifiesBaseValueAndStructureWords() {
    const auto fixture = TableFixture();
    const auto shape = BuildGatherTemplate(*fixture.variant, fixture.user, 0, AllOn);
    Require(shape->refusal == nullptr, "a plain table draw was refused a template");
    Require(shape->userClass[0] == GatherWord::TableBase && shape->userMask[0] == 0xfffffffcu, "the table pointer is not a masked base");
    Require(shape->userClass[1] == GatherWord::WalkedValue && shape->userMask[1] == 0, "the walked high pointer word is not structure");
    Require(shape->userClass[2] == GatherWord::ValueWord && shape->userMask[2] == 0xffffffffu, "the inline V# base is not a streamed value word");
    Require(shape->userClass[3] == GatherWord::PushWord && shape->userMask[3] == 0xffffffffu, "the forwarded user word is not a streamed push word");
    Require(shape->userClass[4] == GatherWord::Structure && shape->userMask[4] == 0, "an unused user word is not structure");
    Require(shape->userClass[5] == GatherWord::ValueRecord && shape->userMask[5] == 0, "the inline V# record count tagged as a value word is not structure");
    Require(shape->wordClass[0] == GatherWord::BufferBase && shape->wordMask[0] == 0xfffffffcu, "the V# base low word is not a masked buffer base");
    Require(shape->wordClass[1] == GatherWord::BufferBase && shape->wordMask[1] == 0xffffu, "the V# base high word does not keep its stride as structure");
    Require(shape->wordClass[2] == GatherWord::Structure && shape->wordClass[3] == GatherWord::Structure, "the V# size and format words are not structure");
    Require(shape->wordClass[4] == GatherWord::FlatData && shape->wordMask[4] == 0xffffffffu, "the data constant is not a streamed flat word");
    Require(shape->wordClass[5] == GatherWord::Structure && shape->wordMask[5] == 0, "the element count word is not structure");
    const auto off = BuildGatherTemplate(*fixture.variant, fixture.user, 0, GatherOptions{});
    Require(off->wordClass[0] == GatherWord::Structure && off->wordClass[4] == GatherWord::Structure, "with the old path's data and buffer-base switches off, their words must stay structure");
    std::vector<std::uint32_t> live;
    const auto same = EvaluateGather(*shape, fixture.user, HostReader, live, nullptr, true);
    Require(same.matched && !same.wordsChanged && same.differ == GatherDiffer::None, "an unchanged draw does not match its own template");
    auto bda = TableFixture();
    auto program = std::make_shared<ShaderRecompiler::RecompileResult>(*bda.variant->compiled);
    program->bindings.push_back({ShaderRecompiler::DescriptorKind::StorageBuffer, Role::BdaPagetable, 0, 60, 1, {}});
    bda.variant->compiled = program;
    Require(std::string(BuildGatherTemplate(*bda.variant, bda.user, 0, AllOn)->refusal) == "bda", "a BDA program was not refused");
}

constexpr std::uint64_t ClampBase = 0x0000000010000000ull;

std::uint64_t AllocationEnd(std::uint64_t) {
    return ClampBase + 16384u;
}

std::uint64_t ShorterAllocationEnd(std::uint64_t) {
    return ClampBase + 16380u;
}

void ExtentFollowsTheBindingRules() {
    const auto address = ClampBase + 16004u;
    const std::array<std::uint32_t, 4> words{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) | (12u << 16u), 32u, Format};
    const GatherBufferLimits limits{1u << 27u, 64u, 0u, &AllocationEnd};
    const auto extent = GatherBufferExtent(words, limits);
    Require(extent.bytes == 380u && extent.adjustment == 4u, "a stride-12, 32-record buffer 16,004 bytes into a 16,384-byte allocation is not clamped to 380 bytes (got " + std::to_string(extent.bytes) + ")");
    const GatherBufferLimits targeted{1u << 27u, 64u, address + 372u, &AllocationEnd};
    Require(GatherBufferExtent(words, targeted).bytes == 372u, "a buffer ending inside the colour target by less than a stride is not clamped to the target");
    GatherStagePlan plan;
    plan.vsharps.push_back({0u, 0u, words, address, 384u, extent.bytes, extent.adjustment});
    ShaderRecompiler::RecompileResult program;
    ShaderRecompiler::DescriptorBinding buffers{ShaderRecompiler::DescriptorKind::StorageBuffer, Role::GuestBuffers, 0, 0, 1, {words[0], words[1], words[2], words[3]}};
    program.bindings = {buffers};
    GatherCheck same;
    CheckGatherStage(plan, program, same, limits);
    Require(same.mismatched[static_cast<std::size_t>(GatherElement::Vsharp)] == 0, "the committer check rejects the bound extent it should accept");
    GatherCheck moved;
    CheckGatherStage(plan, program, moved, GatherBufferLimits{1u << 27u, 64u, 0u, &ShorterAllocationEnd});
    Require(moved.vsharpExtent == 1 && moved.vsharpAddress == 0 && moved.vsharpSize == 0, "the committer check does not see a bound extent that differs from the plan's");
}

void StructuralChangeIsDetected() {
    auto fixture = TableFixture();
    const auto shape = BuildGatherTemplate(*fixture.variant, fixture.user, 0, AllOn);
    std::vector<std::uint32_t> live;
    Table[5] = 4u;
    auto outcome = EvaluateGather(*shape, fixture.user, HostReader, live, nullptr, true);
    Require(!outcome.matched && outcome.differ == GatherDiffer::RunStructure, "a changed element count matched the template");
    Table[5] = 3u;
    Table[2] = Records * 2u;
    outcome = EvaluateGather(*shape, fixture.user, HostReader, live, nullptr, true);
    Require(!outcome.matched && outcome.differ == GatherDiffer::RunStructure, "a changed V# record count matched the template");
    Table[2] = Records;
    Table[1] = static_cast<std::uint32_t>(BufferBase >> 32u) | ((Stride * 2u) << 16u);
    outcome = EvaluateGather(*shape, fixture.user, HostReader, live, nullptr, true);
    Require(!outcome.matched, "a changed V# stride under the masked base word matched the template");
    Table[1] = static_cast<std::uint32_t>(BufferBase >> 32u) | (Stride << 16u);
    auto user = fixture.user;
    user[4] = 0x33333333u;
    outcome = EvaluateGather(*shape, user, HostReader, live, nullptr, true);
    Require(!outcome.matched && outcome.differ == GatherDiffer::UserStructure, "a changed structural user word matched the template");
    user = fixture.user;
    user[5] = Records * 2u;
    outcome = EvaluateGather(*shape, user, HostReader, live, nullptr, true);
    Require(!outcome.matched && outcome.differ == GatherDiffer::RecordCount, "a changed INLINE V# record count matched the template");
    user = fixture.user;
    user[0] |= 1u;
    outcome = EvaluateGather(*shape, user, HostReader, live, nullptr, true);
    Require(!outcome.matched && outcome.differ == GatherDiffer::TableBaseAlignment, "a table pointer with different low bits matched the template");
    user = fixture.user;
    user[1] ^= 1u;
    outcome = EvaluateGather(*shape, user, HostReader, live, nullptr, true);
    Require(!outcome.matched && outcome.differ == GatherDiffer::UserWalked, "a changed walked user word matched the template");
    outcome = EvaluateGather(*shape, std::span(fixture.user).first(4), HostReader, live, nullptr, true);
    Require(!outcome.matched && outcome.differ == GatherDiffer::Layout, "a draw with fewer user words matched the template");
}

void ImageDescriptorsRemainStructural() {
    auto fixture = TableFixture();
    alignas(64) std::array<std::uint32_t, 8> image{1, 2, 3, 4, 5, 6, 7, 8};
    auto compiled = std::make_shared<ShaderRecompiler::RecompileResult>(*fixture.variant->compiled);
    compiled->bindings.push_back({ShaderRecompiler::DescriptorKind::SampledImage, Role::GuestImages, 0, 10, 1, std::vector<std::uint32_t>(image.begin(), image.end())});
    fixture.variant->compiled = compiled;
    const auto address = reinterpret_cast<std::uintptr_t>(image.data());
    fixture.variant->runs.emplace_back(address, address + sizeof(image));
    fixture.variant->words.insert(fixture.variant->words.end(), image.begin(), image.end());
    fixture.variant->runBases.push_back(-1);
    fixture.variant->runParents.emplace_back();
    const auto shape = BuildGatherTemplate(*fixture.variant, fixture.user, 0, AllOn);
    image[0] ^= 0x100;
    std::vector<std::uint32_t> words;
    Require(!EvaluateGather(*shape, fixture.user, HostReader, words, nullptr, true).matched, "a changed image descriptor matched a live template");
}

void MovedBaseMatchesAndYieldsTheNewAddress() {
    const auto fixture = TableFixture();
    const auto shape = BuildGatherTemplate(*fixture.variant, fixture.user, 0, AllOn);
    FillTable(MovedTable, MovedBufferBase, 0x40000000u);
    auto user = fixture.user;
    user[0] = Low(MovedTable.data());
    user[2] = static_cast<std::uint32_t>(ValueBase + 0x200u);
    user[3] = 0x44444444u;
    Require(High(MovedTable.data()) == user[1], "the test tables are not in one 4 GiB window");
    std::vector<std::uint32_t> live;
    std::vector<std::uint64_t> addresses;
    const auto outcome = EvaluateGather(*shape, user, HostReader, live, &addresses, true);
    Require(outcome.matched && outcome.wordsChanged, "a moved table with the same structure did not match");
    Require(addresses.size() == 1 && addresses[0] == reinterpret_cast<std::uintptr_t>(MovedTable.data()), "the moved table was not read at its new address");
    GatherStagePlan plan;
    plan.knownReads.push_back({MovedBufferBase, {1u, 2u}});
    plan.reads.emplace_back(MovedBufferBase, MovedBufferBase + 8u);
    FillGatherPlan(*shape, user, live, plan);
    Require(plan.knownReads.empty() && plan.reads.empty(), "a reused stage plan keeps the previous draw's reads or known reads");
    Require(plan.vsharps.size() == 2, "the plan does not carry both V# elements");
    const auto element = [&](std::uint32_t index) -> const GatherVsharp& {
        for (const auto& vsharp : plan.vsharps) if (vsharp.binding == 0 && vsharp.element == index) return vsharp;
        throw std::runtime_error("the plan lacks V# element " + std::to_string(index));
    };
    Require(element(0).address == MovedBufferBase && element(0).size == static_cast<std::uint64_t>(Stride) * Records, "the plan does not hold the moved V# address and size");
    Require(element(1).address == ValueBase + 0x200u && element(1).size == static_cast<std::uint64_t>(Stride) * Records, "the plan does not hold the new inline V# address");
    const auto word = [&](std::uint32_t binding, std::uint32_t slot) {
        for (const auto& item : plan.words) if (item.binding == binding && item.word == slot) return item.value;
        throw std::runtime_error("the plan lacks a word for binding " + std::to_string(binding) + " slot " + std::to_string(slot));
    };
    Require(word(1, 2) == 0x40000000u && word(1, 0) == static_cast<std::uint32_t>(MovedBufferBase), "the plan's flat words are not the live words");
    Require(plan.slots.size() == 1 && plan.slots[0].first == 0 && plan.slots[0].second == 0x44444444u, "the plan's push word is not the live forwarded word");
    Require(plan.elements[static_cast<std::size_t>(GatherElement::Vsharp)] == 2 && plan.elements[static_cast<std::size_t>(GatherElement::Flat)] == 1 && plan.elements[static_cast<std::size_t>(GatherElement::ShaderData)] == 1 && plan.elements[static_cast<std::size_t>(GatherElement::Push)] == 1, "the plan's element counts are wrong");
    auto patched = std::make_shared<ShaderRecompiler::RecompileResult>(*fixture.variant->compiled);
    patched->bindings[0].guestDescriptor[0] = MovedTable[0];
    patched->bindings[0].guestDescriptor[1] = MovedTable[1];
    patched->bindings[0].guestDescriptor[4] = user[2];
    patched->bindings[1].guestDescriptor[0] = MovedTable[0];
    patched->bindings[1].guestDescriptor[2] = MovedTable[4];
    patched->bindings[2].guestDescriptor[0] = user[3];
    std::memcpy(patched->pushConstants.data(), &user[3], sizeof(std::uint32_t));
    GatherCheck check;
    CheckGatherStage(plan, *patched, check);
    std::uint64_t checked = 0, mismatched = 0;
    for (std::size_t kind = 0; kind < check.checked.size(); ++kind) {
        checked += check.checked[kind];
        mismatched += check.mismatched[kind];
    }
    Require(checked == 6 && mismatched == 0, "the plan disagrees with the patched program (" + std::to_string(mismatched) + " of " + std::to_string(checked) + ")");
    GatherCheck stale;
    CheckGatherStage(plan, *fixture.variant->compiled, stale);
    Require(stale.vsharpAddress == 2 && stale.vsharpSize == 0 && stale.mismatched[static_cast<std::size_t>(GatherElement::Push)] == 1 && stale.mismatched[static_cast<std::size_t>(GatherElement::Flat)] == 2, "the committer check did not see the unpatched program's stale values");
}

Fixture NestedFixture() {
    Child = {0x10u, 0x20u, 0x30u, 0x40u};
    Parent = {Low(Child.data()), High(Child.data()), 5u, 0u};
    Fixture fixture;
    auto& variant = *fixture.variant;
    fixture.user = {Low(Parent.data()), High(Parent.data())};
    auto compiled = std::make_shared<ShaderRecompiler::RecompileResult>();
    compiled->bindings = {{ShaderRecompiler::DescriptorKind::StorageBuffer, Role::FlattenedSrt, 0, 61, 1, {Parent[0], Child[1], 0u, 0u}}};
    variant.compiled = compiled;
    const auto parent = reinterpret_cast<std::uintptr_t>(Parent.data());
    const auto child = reinterpret_cast<std::uintptr_t>(Child.data());
    variant.runs = {{parent, parent + sizeof(Parent)}, {child, child + sizeof(Child)}};
    variant.words = {Parent[0], Parent[1], Parent[2], Parent[3], Child[0], Child[1], Child[2], Child[3]};
    variant.runBases = {0, -1};
    variant.runParents = {{}, {0u, 0u}};
    variant.relocatableWords = 1u;
    variant.userWordsWalked = 3u;
    variant.nestedPointerProof = true;
    variant.nestedPointerPatches = {{0u, 0u, 0u, 0xffffffffu}};
    variant.dataPositions = {5};
    variant.dataSlots = {1};
    variant.flatBinding = 0;
    return fixture;
}

void NestedPointersAreCarriedWithTheirReads() {
    const auto fixture = NestedFixture();
    const auto shape = BuildGatherTemplate(*fixture.variant, fixture.user, 0, AllOn);
    Require(shape->nested && shape->runs[1].parentRun == 0, "the nested child run is not tied to its parent pointer");
    Require(shape->wordClass[0] == GatherWord::NestedPointer && shape->wordMask[0] == 0xfffffffcu && shape->wordMask[1] == 0xffffffffu, "the nested pointer words are not masked");
    MovedChild = {0x10u, 0x99u, 0x30u, 0x40u};
    Parent[0] = Low(MovedChild.data());
    Parent[1] = High(MovedChild.data());
    std::vector<std::uint32_t> live;
    std::vector<std::uint64_t> addresses;
    auto outcome = EvaluateGather(*shape, fixture.user, HostReader, live, &addresses, true);
    Require(outcome.matched && outcome.nestedRuns == 1, "a moved child table with the same structure did not match");
    Require(addresses.size() == 2 && addresses[1] == reinterpret_cast<std::uintptr_t>(MovedChild.data()), "the child table was not read through the live pointer");
    GatherStagePlan plan;
    FillGatherPlan(*shape, fixture.user, live, plan);
    bool pointer = false, data = false;
    for (const auto& word : plan.words) {
        pointer |= word.word == 0 && word.value == Low(MovedChild.data());
        data |= word.word == 1 && word.value == 0x99u;
    }
    Require(pointer && data, "the plan lacks the live nested pointer or the child's data word");
    MovedChild[2] = 0x31u;
    outcome = EvaluateGather(*shape, fixture.user, HostReader, live, nullptr, true);
    Require(!outcome.matched && outcome.differ == GatherDiffer::RunStructure, "a structural change in the moved child matched");
    MovedChild[2] = 0x30u;
    Parent[0] |= 2u;
    outcome = EvaluateGather(*shape, fixture.user, HostReader, live, nullptr, true);
    Require(!outcome.matched && outcome.differ == GatherDiffer::NestedPointer, "a nested pointer with different low bits matched");
    Parent[0] &= ~3u;
    const auto flatOnly = BuildGatherTemplate(*fixture.variant, fixture.user, 0, GatherOptions{true, true, true, false});
    Require(!flatOnly->nested && flatOnly->wordClass[0] == GatherWord::Structure, "without nested relocation the pointer words must be structure");
    outcome = EvaluateGather(*flatOnly, fixture.user, HostReader, live, nullptr, true);
    Require(!outcome.matched, "without nested relocation a moved child pointer matched");
}

}

struct ShadowDraw {
    std::vector<DrawProgram> programs;
    DrawEntry entry;
    std::vector<std::shared_ptr<DispatchVariant>> matched;
    std::vector<std::uint64_t> reads;
    GatherReader reader;
    std::function<bool(std::uint64_t, std::size_t)> busyRange;
    GatherShadowStats stats;
    std::shared_ptr<GatherCommitTemplate> binding = std::make_shared<GatherCommitTemplate>();

    ShadowDraw(const Fixture& fixture, std::span<const std::uint32_t> liveUser) {
        DrawProgram program{};
        program.userData.assign(liveUser.begin(), liveUser.end());
        programs.push_back(program);
        auto decode = std::make_shared<DrawDecode>();
        DrawProgram captured{};
        captured.userData = fixture.user;
        decode->programs.push_back(captured);
        entry.decode = decode;
        entry.stages = {{fixture.variant}};
        matched = {fixture.variant};
        GatherCommitStage bound;
        bound.compiled = fixture.variant->compiled;
        if (bound.compiled != nullptr) for (std::size_t index = 0; index < bound.compiled->bindings.size(); ++index) {
            const auto& descriptor = bound.compiled->bindings[index];
            if (descriptor.role != Role::GuestBuffers) continue;
            for (std::uint32_t element = 0; element < descriptor.count; ++element) {
                std::array<std::uint32_t, 4> words;
                std::copy_n(descriptor.guestDescriptor.begin() + element * 4u, 4u, words.begin());
                const auto extent = GatherBufferExtent(words, {});
                bound.bindings.push_back({static_cast<std::uint32_t>(index), element, element, extent.bytes, extent.adjustment, extent.bytes + extent.adjustment});
            }
        }
        binding->stages.push_back(std::move(bound));
        binding->ready.store(true);

        reader = [this](std::uint64_t address, std::span<std::uint32_t> out) {
            reads.push_back(address);
            return HostReader(address, out);
        };
        busyRange = [](std::uint64_t, std::size_t) { return false; };
    }

    std::shared_ptr<GatherDrawPlan> Run(EntryOutcome outcome, const GatherIndirectPlan& indirect = {}, bool pendingAtValidation = false, bool live = false) {
        ArmGatherValidationLog(true);
        GatherValidationLog()->push_back({0u, matched.front(), outcome, false, pendingAtValidation});
        GatherShadowDraw draw;
        draw.programs = programs;
        draw.entry = &entry;
        draw.matched = matched;
        draw.hit = outcome == EntryOutcome::Equal || outcome == EntryOutcome::EqualData;
        draw.keyed = true;
        draw.options = AllOn;
        draw.indirect = indirect;
        draw.read = &reader;
        draw.busy = &busyRange;
        draw.stats = &stats;
        draw.commitTemplate = binding;
        draw.live = live;
        auto plan = RunGatherShadow(draw);
        ArmGatherValidationLog(false);
        return plan;
    }
};

bool Throws(const std::function<void()>& body) {
    try {
        body();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

void ShadowRunsThroughTheCacheAndHandsThePlanToTheCommitter() {
    const auto fixture = TableFixture();
    FillTable(MovedTable, MovedBufferBase, 0x40000000u);
    auto live = fixture.user;
    live[0] = Low(MovedTable.data());
    ShadowDraw shadow(fixture, live);
    const auto plan = shadow.Run(EntryOutcome::EqualData);
    Require(plan != nullptr && plan->complete && plan->incomplete == GatherIncompleteNone, "a moved table hit did not produce a complete plan");
    Require(shadow.stats.built == 1 && shadow.stats.trueMatch == 1 && shadow.stats.falseDiffer == 0, "the first shadow run did not build one template and match it");
    Require(plan->stages.size() == 1 && plan->stages[0].vsharps.size() == 2, "the shadow plan lacks the stage's V# elements");
    const auto again = shadow.Run(EntryOutcome::Equal);
    Require(again != nullptr && shadow.stats.built == 1 && shadow.stats.trueMatch == 2, "a second shadow run of the same variant rebuilt its template");
    auto patched = std::make_shared<ShaderRecompiler::RecompileResult>(*fixture.variant->compiled);
    patched->bindings[0].guestDescriptor[0] = MovedTable[0];
    patched->bindings[0].guestDescriptor[1] = MovedTable[1];
    patched->bindings[1].guestDescriptor[0] = MovedTable[0];
    patched->bindings[1].guestDescriptor[2] = MovedTable[4];
    plan->stages[0].program = patched.get();
    const std::vector<AgcDriver::Graphics::CompiledShader> stages{{ShaderRecompiler::ShaderStage::Vertex, patched.get(), 0u}};
    const auto check = CheckGatherDrawPlan(*plan, stages, 0u, {});
    std::uint64_t mismatched = 0;
    for (const auto count : check.mismatched) mismatched += count;
    Require(mismatched == 0 && check.stagesMissing == 0, "the committer check rejected the plan the shadow handed over");
    const std::vector<AgcDriver::Graphics::CompiledShader> other{{ShaderRecompiler::ShaderStage::Vertex, fixture.variant->compiled.get(), 0u}};
    Require(Throws([&] { CheckGatherDrawPlan(*plan, other, 0u, {}); }), "strict mode did not fail on a plan whose stage the committer does not bind");
}

void PendingSourcesRefuseOrLeaveThePlanIncomplete() {
    auto nested = NestedFixture();
    MovedChild = {0x10u, 0x99u, 0x30u, 0x40u};
    Parent[0] = Low(MovedChild.data());
    Parent[1] = High(MovedChild.data());
    ShadowDraw parentPending(nested, nested.user);
    const auto parent = reinterpret_cast<std::uintptr_t>(Parent.data());
    parentPending.busyRange = [parent](std::uint64_t address, std::size_t bytes) { return address < parent + sizeof(Parent) && parent < address + bytes; };
    const auto refused = parentPending.Run(EntryOutcome::Equal);
    Require(refused == nullptr && parentPending.stats.pendingRefused == 1 && parentPending.stats.falseDiffer == 0, "a draw whose parent table is pending a GPU write was not refused");
    const auto child = reinterpret_cast<std::uintptr_t>(MovedChild.data());
    Require(std::none_of(parentPending.reads.begin(), parentPending.reads.end(), [&](std::uint64_t address) { return address == child; }), "the child table was read through a pointer from a pending parent");
    ShadowDraw atValidation(nested, nested.user);
    Require(atValidation.Run(EntryOutcome::Equal, {}, true) == nullptr && atValidation.stats.pendingRefused == 1 && atValidation.reads.empty(), "a validation over a pending range was evaluated");
    const auto fixture = TableFixture();
    ShadowDraw vsharpPending(fixture, fixture.user);
    vsharpPending.busyRange = [](std::uint64_t address, std::size_t bytes) { return address < BufferBase + 1024u && BufferBase < address + bytes; };
    const auto pendingPlan = vsharpPending.Run(EntryOutcome::Equal);
    Require(pendingPlan != nullptr && !pendingPlan->complete && (pendingPlan->incomplete & GatherPendingVsharp) != 0, "a plan whose V# source is pending a GPU write is complete");
}

void IndirectArgumentsAreAPlanElement() {
    const auto fixture = TableFixture();
    const GatherIndirectPlan indirect{true, true, 0x0000777700000000ull, 0x0000777700001000ull, 4u, 20u, 80u};
    ShadowDraw shadow(fixture, fixture.user);
    const auto plan = shadow.Run(EntryOutcome::Equal, indirect);
    Require(plan != nullptr && plan->complete && plan->indirect == indirect, "the plan does not carry the indirect arguments");
    plan->stages[0].program = fixture.variant->compiled.get();
    const std::vector<AgcDriver::Graphics::CompiledShader> stages{{ShaderRecompiler::ShaderStage::Vertex, fixture.variant->compiled.get(), 0u}};
    const auto same = CheckGatherDrawPlan(*plan, stages, 0u, indirect);
    Require(same.checked[static_cast<std::size_t>(GatherElement::Indirect)] == 1 && same.mismatched[static_cast<std::size_t>(GatherElement::Indirect)] == 0, "the committer check rejects the indirect arguments it commits");
    auto moved = indirect;
    moved.arguments += 0x100u;
    Require(Throws([&] { CheckGatherDrawPlan(*plan, stages, 0u, moved); }), "the committer check does not see indirect arguments moved from the plan's");
    ShadowDraw pending(fixture, fixture.user);
    pending.busyRange = [&](std::uint64_t address, std::size_t) { return address == indirect.countAddress; };
    const auto pendingPlan = pending.Run(EntryOutcome::Equal, indirect);
    Require(pendingPlan != nullptr && !pendingPlan->complete && (pendingPlan->incomplete & GatherPendingIndirect) != 0, "a plan whose indirect count is pending a GPU write is complete");
}

void MovedNestedPointerOnADirectHitHasItsOwnClass() {
    auto nested = NestedFixture();
    MovedChild = {0x10u, 0x20u, 0x30u, 0x40u};
    Parent[0] = Low(MovedChild.data());
    Parent[1] = High(MovedChild.data());
    ShadowDraw shadow(nested, nested.user);
    Require(shadow.Run(EntryOutcome::Differing) == nullptr, "a direct miss produced a plan");
    Require(shadow.stats.nestedDirect == 1 && shadow.stats.falseMatch == 0, "a moved nested pointer on a direct hit is not counted in its own class");
    Parent[0] = Low(Child.data());
    Parent[1] = High(Child.data());
}

void FailedNestedDiagnosisDoesNotExemptAMatch() {
    auto fixture = NestedFixture();
    MovedChild = {0x10u, 0x20u, 0x30u, 0x40u};
    Parent[0] = Low(MovedChild.data());
    Parent[1] = High(MovedChild.data());
    ShadowDraw shadow(fixture, fixture.user);
    unsigned parents = 0;
    shadow.reader = [&](std::uint64_t address, std::span<std::uint32_t> out) {
        if (address == reinterpret_cast<std::uintptr_t>(Parent.data()) && ++parents > 1) return false;
        return HostReader(address, out);
    };
    Require(shadow.Run(EntryOutcome::Differing) == nullptr && shadow.stats.nestedDirect == 0 && shadow.stats.pendingRefused == 1, "a failed parent diagnosis was exempted as a nested pointer move");
    Parent[0] = Low(Child.data());
    Parent[1] = High(Child.data());
}

void GuardedReadsFallBackAndNeverFault() {
    FillTable(Table, BufferBase, 0x3f800000u);
    std::array<std::uint32_t, 8> copy{};
    bool owned = true;
    const auto result = AgcDriver::GuestMemory::CopyMappedGuarded(reinterpret_cast<std::uintptr_t>(Table.data()), std::as_writable_bytes(std::span(copy)), owned);
    Require(result == AgcDriver::GuestMemory::Compare::Equal && !owned && copy == Table, "a host table outside the guest arena was not read through the checked fallback");
#ifdef _WIN32
    auto* page = static_cast<std::byte*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Require(page != nullptr, "cannot allocate a host page");
    DWORD previous = 0;
    Require(VirtualProtect(page, 4096, PAGE_NOACCESS, &previous) != 0, "cannot protect the host page");
    owned = true;
    const auto denied = AgcDriver::GuestMemory::CopyMappedGuarded(reinterpret_cast<std::uintptr_t>(page), std::as_writable_bytes(std::span(copy)), owned);
    Require(denied == AgcDriver::GuestMemory::Compare::Unmapped && !owned, "a no-access page was not refused by the guarded read");
    VirtualFree(page, 0, MEM_RELEASE);
    auto* arena = GuestArena::GuestArenaAllocate_nid_postfix(65536, 65536);
    Require(arena != nullptr, "cannot allocate an arena guard page");
    GuestArena::GuestArenaCommit_nid_postfix(arena, 65536, PAGE_READWRITE, 4096);
    Require(VirtualProtect(arena, 4096, PAGE_READWRITE | PAGE_GUARD, &previous) != 0, "cannot guard the arena page");
    auto* recovery = AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS* info) -> LONG { return GuestArena::GuestArenaRecoverRead_nid_no_patch(info) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH; });
    const auto guarded = AgcDriver::GuestMemory::CopyMappedGuarded(reinterpret_cast<std::uintptr_t>(arena), std::as_writable_bytes(std::span(copy)), owned);
    MEMORY_BASIC_INFORMATION info{};
    Require(VirtualQuery(arena, &info, sizeof(info)) == sizeof(info) && (info.Protect & PAGE_GUARD) != 0, "the guarded reader consumed PAGE_GUARD");
    RemoveVectoredExceptionHandler(recovery);
    Require(guarded == AgcDriver::GuestMemory::Compare::Unmapped, "the guarded reader accepted PAGE_GUARD");
    VirtualProtect(arena, 4096, PAGE_READWRITE, &previous);
    GuestArena::GuestArenaReset_nid_postfix(arena, 65536);
    GuestArena::GuestArenaRelease_nid_postfix(arena, 65536);

#endif
}

void HashCollisionStillChecksStructure() {
    const auto fixture = TableFixture();
    const auto shape = BuildGatherTemplate(*fixture.variant, fixture.user, 0, AllOn);
    auto changed = fixture.user;
    ++changed[4];
    std::vector<std::uint32_t> words;
    const auto different = EvaluateGather(*shape, changed, HostReader, words, nullptr, true);
    auto collision = *shape;
    collision.signature = different.signature;
    const auto checked = EvaluateGather(collision, changed, HostReader, words, nullptr, true);
    Require(!checked.matched && checked.differ != GatherDiffer::None, "a signature collision bypassed exact structural comparison");
}

void BindingOwnerAndPendingRangeAreExact() {
    const auto fixture = TableFixture();
    ShadowDraw shadow(fixture, fixture.user);
    auto& owner = shadow.binding->stages[0].bindings[0];
    owner.adjustment = 8;
    owner.viewBytes = owner.extent + 8;
    const auto plan = shadow.Run(EntryOutcome::Equal);
    Require(plan != nullptr && plan->complete && std::any_of(plan->stages[0].vsharps.begin(), plan->stages[0].vsharps.end(), [](const auto& value) { return value.element == 0 && value.adjustment == 8; }), "the published owner's adjustment was replaced by absolute address alignment");
    shadow.busyRange = [](std::uint64_t address, std::size_t bytes) { return address == BufferBase - 8 && bytes == Records * Stride + 8; };
    const auto pending = shadow.Run(EntryOutcome::Equal);
    Require(pending != nullptr && (pending->incomplete & GatherPendingVsharp) != 0, "pending checking omitted the owner's adjusted range");
    owner.adjustment = 3;
    owner.viewBytes = (owner.extent + 6) & ~std::uint64_t{3};
    shadow.busyRange = [](std::uint64_t address, std::size_t bytes) { return address == BufferBase - 3 && bytes == Records * Stride + 4; };
    const auto rounded = shadow.Run(EntryOutcome::Equal);
    Require(rounded != nullptr && (rounded->incomplete & GatherPendingVsharp) != 0, "pending checking omitted the DWORD-rounded tail");
}

void GenerationWindowIncludesPlanValidation() {
    const auto fixture = TableFixture();
    ShadowDraw shadow(fixture, fixture.user);
    bool changed = false;
    shadow.busyRange = [&](std::uint64_t address, std::size_t) {
        if (address == BufferBase && !changed) {
            changed = true;
            GuestAllocations::GuestAllocationsInvalidate_nid_postfix(reinterpret_cast<std::uintptr_t>(Table.data()), sizeof(Table));
        }
        return false;
    };
    const auto plan = shadow.Run(EntryOutcome::Equal);
    Require(changed && plan != nullptr && !plan->complete && (plan->incomplete & GatherGenerationMoved) != 0, "a generation change during plan validation left a complete plan");
}

void ChangedIgnoredFeedbackBitsStayTemplatable() {
    auto fixture = TableFixture();
    fixture.variant->feedback.memory = {{6, 0xff}};
    Table[6] = 7;
    ShadowDraw shadow(fixture, fixture.user);
    const auto plan = shadow.Run(EntryOutcome::Equal, {}, false, true);
    Require(plan != nullptr && plan->matched && (plan->incomplete & GatherNotTemplatable) == 0, "a change confined to ignored feedback bits was refused live");
}

void CopiedProgramsClearPooledStateAndKeepRetainedOwners() {
    ShaderRecompiler::RecompileResult filled;
    filled.bindings.push_back({ShaderRecompiler::DescriptorKind::StorageBuffer, Role::GuestBuffers, 0, 0, 1, {1, 2, 3, 4}});
    filled.pushConstants = {std::byte{0x12}, std::byte{0x34}};
    filled.specialization = {{7, 99}};
    filled.shaderDataUserWords = {2, 3};
    filled.imageTableRanges = {{0x1000, 0x2000}};
    filled.imageTableReadRanges = {{{0x1000, 0x1010}}};
    filled.userDataRegisterReads = 3;
    filled.imageTableFaults = 9;
    filled.cacheHit = true;
    filled.drawKeyMemo.words.store(std::make_shared<const std::vector<std::uint32_t>>(std::initializer_list<std::uint32_t>{9, 8}));
    const auto retained = CopyDrawProgram(filled);
    Require(retained->drawKeyMemo.words.load() == nullptr, "copying a program retained the donor's result key memo");
    ShaderRecompiler::RecompileResult empty;
    for (std::size_t round = 0; round < 1026; ++round) {
        auto result = CopyDrawProgram(filled);
        Require(result->bindings.size() == 1 && result->pushConstants == filled.pushConstants && result->imageTableReadRanges == filled.imageTableReadRanges, "a pooled program lost its copied invocation");
        result->drawKeyMemo.words.store(std::make_shared<const std::vector<std::uint32_t>>(std::initializer_list<std::uint32_t>{77}));
    }
    for (std::size_t round = 0; round < 1026; ++round) {
        const auto result = CopyDrawProgram(empty);
        Require(result->bindings.empty() && result->pushConstants.empty() && result->specialization.empty() && result->shaderDataUserWords.empty() && result->imageTableRanges.empty() && result->imageTableReadRanges.empty() && result->userDataRegisterReads == 0 && result->imageTableFaults == 0 && !result->cacheHit && result->drawKeyMemo.words.load() == nullptr, "an empty invocation retained pooled program state");
    }
    Require(retained->bindings.size() == 1 && retained->bindings[0].guestDescriptor == filled.bindings[0].guestDescriptor && retained->pushConstants == filled.pushConstants && retained->imageTableFaults == filled.imageTableFaults, "cycling the program pool overwrote a retained program");
}

void StrictModeFailsOnAFalseDiffer() {
    const auto fixture = TableFixture();
    auto live = fixture.user;
    live[4] = 0x33333333u;
    ShadowDraw shadow(fixture, live);
    Require(Throws([&] { shadow.Run(EntryOutcome::Equal); }), "strict mode did not fail on a structural difference the old path accepted");
    ArmGatherValidationLog(false);
}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_DRAW_GATHER_SHADOW_STRICT", "1");
#else
    setenv("APS5_DRAW_GATHER_SHADOW_STRICT", "1", 1);
#endif
    try {
        ExtentFollowsTheBindingRules();
        ClassifiesBaseValueAndStructureWords();
        StructuralChangeIsDetected();
        ImageDescriptorsRemainStructural();
        MovedBaseMatchesAndYieldsTheNewAddress();
        NestedPointersAreCarriedWithTheirReads();
        ShadowRunsThroughTheCacheAndHandsThePlanToTheCommitter();
        PendingSourcesRefuseOrLeaveThePlanIncomplete();
        IndirectArgumentsAreAPlanElement();
        MovedNestedPointerOnADirectHitHasItsOwnClass();
        FailedNestedDiagnosisDoesNotExemptAMatch();
        GuardedReadsFallBackAndNeverFault();
        HashCollisionStillChecksStructure();
        BindingOwnerAndPendingRangeAreExact();
        GenerationWindowIncludesPlanValidation();
        ChangedIgnoredFeedbackBitsStayTemplatable();
        StrictModeFailsOnAFalseDiffer();
        CopiedProgramsClearPooledStateAndKeepRetainedOwners();
        std::cout << "Gather templates classify base, value and structural words, detect structural changes, and carry moved and nested addresses in the plan\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
