#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/GatherShadow.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
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
    fixture.user = {Low(Table.data()), High(Table.data()), static_cast<std::uint32_t>(ValueBase), 0x11111111u, 0x22222222u};
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
    variant.valueWords = 1u << 2u;
    variant.valueSlots = {{2u, 0u, 4u}};
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
    FillGatherPlan(*shape, user, live, plan);
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

int main() {
    try {
        ClassifiesBaseValueAndStructureWords();
        StructuralChangeIsDetected();
        MovedBaseMatchesAndYieldsTheNewAddress();
        NestedPointersAreCarriedWithTheirReads();
        std::cout << "Gather templates classify base, value and structural words, detect structural changes, and carry moved and nested addresses in the plan\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
