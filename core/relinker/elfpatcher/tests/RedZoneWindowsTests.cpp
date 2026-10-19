#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <elfpatcher/windows/WindowsRedZonePatcher.hpp>
#include <elfpatcher/windows/WindowsPeWriter.hpp>
#include <io/BufferUtils.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace Elfpatcher::Windows;
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t Canary = 0xa5c3e17b9d24f608ull;
constexpr std::uint32_t CodeAddress = 0x1000;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void emit(Bytes& bytes, const std::initializer_list<std::uint8_t> values) {
    bytes.insert(bytes.end(), values.begin(), values.end());
}

struct Fixture {
    Bytes Source;
    std::vector<Domain::ProgramHeader> Headers;

    explicit Fixture(const Bytes& code, const std::vector<std::uint32_t>& starts = {CodeAddress}) : Source(0x2400) {
        require(code.size() <= 0x1000, "Fixture code exceeds one page");
        Io::WriteU32(Source, 0, 0x464c457f);
        Source[4] = 2;
        Source[5] = 1;
        Source[6] = 1;
        Io::WriteU16(Source, 16, 3);
        Io::WriteU16(Source, 18, 62);
        Io::WriteU64(Source, 24, CodeAddress);
        std::copy(code.begin(), code.end(), Source.begin() + 0x200);
        Headers.push_back({1, 5, 0x200, CodeAddress, 0, code.size(), 0x1000, 0x1000});
        Headers.push_back({1, 6, 0x1400, 0x3000, 0, 0x1000, 0x1000, 0x1000});
        const auto size = 12 + starts.size() * 8;
        Headers.push_back({0x6474e550, 4, 0x1400, 0x3000, 0, size, size, 4});
        Source[0x1400] = 1;
        Source[0x1401] = 0x1b;
        Source[0x1402] = 3;
        Source[0x1403] = 0x3b;
        Io::WriteU32(Source, 0x1404, 0x80);
        Io::WriteU32(Source, 0x1408, static_cast<std::uint32_t>(starts.size()));
        for (std::size_t index = 0; index < starts.size(); ++index) {
            Io::WriteU32(Source, 0x140c + index * 8, starts[index] - 0x3000);
            Io::WriteU32(Source, 0x1410 + index * 8, 0x100);
        }
    }
};

struct Patched {
    std::vector<PeSection> Sections;
    std::uint32_t End;
    RedZonePatchResult Result;
};

Patched patch(const Fixture& fixture, const std::vector<std::uint32_t>& relocations = {}) {
    WindowsLoadImage image(fixture.Source, fixture.Headers);
    Patched output{image.BuildSections(), image.GetEndRva(), {}};
    output.Result = WindowsRedZonePatcher().Build(fixture.Source, fixture.Headers, image, output.Sections, output.End, relocations, "synthetic");
    return output;
}

class Allocation {
public:
    Allocation(const std::size_t size, const DWORD protection) : Pointer(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, protection)) {
        require(Pointer != nullptr, "VirtualAlloc failed");
    }
    ~Allocation() { VirtualFree(Pointer, 0, MEM_RELEASE); }
    Allocation(const Allocation&) = delete;
    Allocation& operator=(const Allocation&) = delete;
    void* Pointer;
};

struct State {
    std::array<std::uint64_t, 16> RedZone{};
    std::uint64_t Value = 0;
    std::uint64_t Flags = 0;
    std::uint64_t RspBefore = 0;
    std::uint64_t RspAfter = 0;
};

static_assert(offsetof(State, Value) == 128 && offsetof(State, Flags) == 136 && offsetof(State, RspBefore) == 144 && offsetof(State, RspAfter) == 152);

Bytes canaryFunction(const Bytes& instruction, const bool observeStack = true, std::size_t* instructionOffset = nullptr) {
    Bytes code;
    if (observeStack) emit(code, {0x68, 0x02, 0x02, 0, 0, 0x9d});
    emit(code, {0x48, 0xb8});
    Io::AppendU64(code, Canary);
    for (int index = 0; index < 16; ++index) emit(code, {0x48, 0x89, 0x44, 0x24, static_cast<std::uint8_t>(-128 + index * 8)});
    if (observeStack) emit(code, {0x48, 0x89, 0xa2, 0x90, 0, 0, 0});
    if (instructionOffset != nullptr) *instructionOffset = code.size();
    code.insert(code.end(), instruction.begin(), instruction.end());
    emit(code, {0x48, 0x89, 0x82, 0x80, 0, 0, 0});
    if (observeStack) emit(code, {0x48, 0x89, 0xa2, 0x98, 0, 0, 0});
    for (int index = 0; index < 16; ++index) {
        emit(code, {0x4c, 0x8b, 0x44, 0x24, static_cast<std::uint8_t>(-128 + index * 8), 0x4c, 0x89, 0x82});
        Io::AppendU32(code, static_cast<std::uint32_t>(index * 8));
    }
    if (observeStack) emit(code, {0x9c, 0x41, 0x58, 0x4c, 0x89, 0x82, 0x88, 0, 0, 0});
    code.push_back(0xc3);
    return code;
}

class Executable {
public:
    explicit Executable(const std::vector<PeSection>& sections) : Memory(0x80000, PAGE_EXECUTE_READWRITE) {
        for (const auto& section : sections) {
            require(section.Rva < 0x80000 && section.Data.size() <= 0x80000 - section.Rva, "Executable fixture exceeds allocation");
            std::memcpy(static_cast<std::uint8_t*>(Memory.Pointer) + section.Rva, section.Data.data(), section.Data.size());
        }
        FlushInstructionCache(GetCurrentProcess(), Memory.Pointer, 0x80000);
    }

    void Run(std::uint64_t* page, State& state) const {
        reinterpret_cast<void (*)(std::uint64_t*, State*)>(static_cast<std::uint8_t*>(Memory.Pointer) + LoadRva)(page, &state);
    }

    void* Address(const std::uint32_t rva) const { return static_cast<std::uint8_t*>(Memory.Pointer) + rva; }

private:
    Allocation Memory;
};

bool intact(const State& state, const std::size_t count = 16) {
    return std::all_of(state.RedZone.begin(), state.RedZone.begin() + static_cast<std::ptrdiff_t>(count), [](const auto value) { return value == Canary; });
}

void equivalent(const Bytes& instruction, const bool store) {
    const Fixture fixture(canaryFunction(instruction));
    const WindowsLoadImage image(fixture.Source, fixture.Headers);
    const Executable original(image.BuildSections());
    const auto output = patch(fixture);
    const Executable converted(output.Sections);
    State before;
    State after;
    std::uint64_t first = 0x12345678abcdef01ull;
    auto second = first;
    original.Run(&first, before);
    converted.Run(&second, after);
    require(intact(before) && intact(after), "Nonfaulting execution corrupted the red zone");
    require(before.Value == after.Value && first == second, "Patched memory instruction changed its result");
    require((before.Flags & 0x8d5) == (after.Flags & 0x8d5), "Patched instruction changed flags");
    require(before.RspBefore == before.RspAfter && after.RspBefore == after.RspAfter, "Patched instruction did not restore RSP");
    require(store ? second == Canary : after.Value == 0x12345678abcdef01ull, "Fixture did not execute its memory operand");
    require(output.Result.PatchedMemoryInstructionCount != 0 || !WindowsRedZonePatcher::Enabled(), "Enabled pass did not patch a memory instruction");
}

void structural() {
    const bool enabled = WindowsRedZonePatcher::Enabled();
    const Fixture fixture({0x48, 0x89, 0x44, 0x24, 0xf8, 0x48, 0x8b, 0x01, 0x90, 0x90, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0xc3});
    const WindowsLoadImage image(fixture.Source, fixture.Headers);
    const auto baseline = image.BuildSections();
    const auto output = patch(fixture);
    const std::array<PeDirectory, 16> directories{};
    const auto original = WindowsPeWriter().Write(baseline, LoadRva, directories);
    const auto converted = WindowsPeWriter().Write(output.Sections, LoadRva, directories);
    if (!enabled) {
        require(original == converted && output.End == image.GetEndRva(), "Disabled pass changed PE bytes or image layout");
        require(output.Result.FunctionCount == 0, "Disabled pass decoded functions");
        auto malformed = fixture;
        malformed.Source[0x1400] = 0xff;
        require(patch(malformed).Result.FunctionCount == 0, "Disabled pass inspected unwind metadata");
        return;
    }
    require(original != converted && output.Sections.back().Name == ".redzone", "Enabled PE output has no distinct marker");
    require(output.Sections.back().Data[6] == 1, "Red-zone output version is wrong");
    require(output.Result.FunctionCount == 1 && output.Result.RedZoneFunctionCount == 1, "Function discovery missed the red-zone function");
    require(output.Result.MemoryInstructionCount == 1 && output.Result.ShortMemoryInstructionCount == 1 && output.Result.PatchedMemoryInstructionCount == 1 && output.Result.UnrelocatableMemoryInstructionCount == 0, "Short forward span was not patched exactly once");
    const Fixture backward({0x48, 0x89, 0x44, 0x24, 0xf8, 0x48, 0x8d, 0x44, 0x24, 0xf8, 0x90, 0x90, 0x48, 0x8b, 0x01, 0xc3});
    require(patch(backward).Result.PatchedMemoryInstructionCount == 1, "Backward span was not patched");
    const Fixture dead({0x48, 0x89, 0x44, 0x24, 0xf8, 0x48, 0x8b, 0x01, 0x90, 0x90, 0xc3});
    require(patch(dead).Result.MemoryInstructionCount == 0, "Dead red-zone data caused an unnecessary patch");
    const Fixture stack({0x48, 0x89, 0x44, 0x24, 0xf8, 0x48, 0x89, 0xa1, 0, 0, 0, 0, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0xc3});
    require(patch(stack).Result.StackDependentMemoryInstructionCount == 1, "RSP source operand was not refused");
    const bool precise = WindowsRedZonePatcher::VectorRegisters();
    for (const auto& operand : {Bytes{0x0f, 0x11, 0x21, 0x90, 0x90}, Bytes{0xc5, 0xf8, 0x11, 0x21, 0x90}, Bytes{0xc5, 0xd8, 0x58, 0x01, 0x90}, Bytes{0x66, 0x0f, 0x3a, 0x16, 0x21, 0}}) {
        Bytes code{0x48, 0x89, 0x44, 0x24, 0xf8};
        code.insert(code.end(), operand.begin(), operand.end());
        emit(code, {0x48, 0x8b, 0x44, 0x24, 0xf8, 0xc3});
        const auto result = patch(Fixture(code)).Result;
        require(result.MemoryInstructionCount == 1 && result.StackDependentMemoryInstructionCount == (precise ? 0u : 1u) && result.PatchedMemoryInstructionCount == (precise ? 1u : 0u), "Vector register 4 was classified against the vector-register switch");
    }
    for (const auto& operand : {Bytes{0xc4, 0xe2, 0xd8, 0xf2, 0x01}, Bytes{0x0f, 0xc3, 0x21, 0x90, 0x90}, Bytes{0xf2, 0x0f, 0x38, 0xf1, 0x21}}) {
        Bytes code{0x48, 0x89, 0x44, 0x24, 0xf8};
        code.insert(code.end(), operand.begin(), operand.end());
        emit(code, {0x48, 0x8b, 0x44, 0x24, 0xf8, 0xc3});
        require(patch(Fixture(code)).Result.StackDependentMemoryInstructionCount != 0, "A general-purpose RSP operand was admitted as a vector register");
    }
    const Fixture indirect({0x48, 0x89, 0x44, 0x24, 0xf8, 0x48, 0x8b, 0x01, 0xff, 0xe0});
    const auto indirectResult = patch(indirect).Result;
    require(indirectResult.IndirectRedZoneFunctionCount == 1 && indirectResult.UnrelocatableMemoryInstructionCount == 1, "Unresolved indirect branch admitted a short relocation span");
    require(patch(fixture, {LoadRva + 6}).Result.UnrelocatableMemoryInstructionCount == 1, "A PE base relocation was moved without rebasing");
    const Fixture replacement({0x48, 0x89, 0x44, 0x24, 0xf8, 0x48, 0x8b, 0xa1, 0, 0, 0, 0, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0xc3});
    const auto replaced = patch(replacement);
    require(replaced.Result.PatchedMemoryInstructionCount == 1 && replaced.Sections.back().Data[28] == 0xe9, "MOV RSP from memory incorrectly restores the old stack");
    Bytes branch{0x48, 0x89, 0x44, 0x24, 0xf8, 0x31, 0xc0, 0x74, 0x09, 0x48, 0x8b, 0x81, 0, 0, 0, 0, 0xeb, 0x05, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0xc3};
    require(patch(Fixture(branch)).Result.MemoryInstructionCount == 0, "Liveness leaked across a dead conditional path");
    branch[17] = 0;
    require(patch(Fixture(branch)).Result.PatchedMemoryInstructionCount == 1, "Liveness did not follow a direct branch to a red-zone read");
    const Fixture overlapping({0x48, 0x89, 0x44, 0x24, 0xf8, 0x74, 0x01, 0x48, 0x8b, 0x01, 0xc3});
    require(patch(overlapping).Result.RefusedFunctionCount == 1, "Overlapping instruction entries were accepted");
    auto missing = fixture;
    missing.Headers.pop_back();
    require(patch(missing).Result.MissingEhFrameCount == 1, "Missing unwind table was not reported");
    auto malformed = fixture;
    malformed.Source[0x1400] = 2;
    bool rejected = false;
    try { patch(malformed); } catch (const Domain::RelinkerException&) { rejected = true; }
    require(rejected, "Unsupported unwind version was accepted");
    malformed = fixture;
    malformed.Headers.back().FileSize = 13;
    rejected = false;
    try { patch(malformed); } catch (const Domain::RelinkerException&) { rejected = true; }
    require(rejected, "Truncated unwind table was accepted");
    auto unmapped = fixture;
    Io::WriteU32(unmapped.Source, 0x140c, 0x2000);
    rejected = false;
    try { patch(unmapped); } catch (const Domain::RelinkerException&) { rejected = true; }
    require(rejected, "Unmapped unwind function was accepted");
}

void ripRelative() {
    std::size_t offset = 0;
    auto code = canaryFunction({0x48, 0x8b, 0x05, 0, 0, 0, 0}, true, &offset);
    Io::WriteU32(code, offset + 3, static_cast<std::uint32_t>(code.size() - offset - 7));
    Io::AppendU64(code, 0x1234);
    const Fixture fixture(code);
    const auto output = patch(fixture);
    const Executable executable(output.Sections);
    State state;
    executable.Run(nullptr, state);
    require(state.Value == 0x1234 && intact(state), "RIP-relative memory operand was not rebased");
}

void liveCall() {
    constexpr std::uint64_t returned = 0x12345678abcdef01ull;
    std::size_t offset = 0;
    auto code = canaryFunction({0xe8, 0, 0, 0, 0, 0x4c, 0x8d, 0x04, 0x24, 0x4c, 0x89, 0x82, 0x98, 0, 0, 0}, false, &offset);
    require(code.at(offset) == 0xe8 && std::find(code.begin(), code.end(), 0xe8) < code.begin() + static_cast<std::ptrdiff_t>(offset), "Live-call fixture must retain an earlier E8 operand byte");
    Io::WriteU32(code, offset + 1, static_cast<std::uint32_t>(code.size() - offset - 5));
    const auto callee = CodeAddress + static_cast<std::uint32_t>(code.size());
    emit(code, {0x48, 0x89, 0xa2, 0x90, 0, 0, 0, 0x48, 0xb8});
    Io::AppendU64(code, returned);
    code.push_back(0xc3);
    const Fixture fixture(code, {CodeAddress, callee});
    const auto output = patch(fixture);
    const Executable executable(output.Sections);
    State state;
    executable.Run(nullptr, state);
    const bool enabled = WindowsRedZonePatcher::Enabled();
    require(state.Value == returned, "Live call did not execute and return from its intended callee");
    require(state.RspBefore == state.RspAfter - (enabled ? 136 : 8), "Live-call trampoline did not restore the caller stack");
    if (enabled) {
        require(output.Result.LiveCallCount == 1 && intact(state), "Live direct call overwrote red-zone data");
    } else require(intact(state, 15) && state.RedZone.back() != Canary, "Unprotected call changed bytes outside its return-address slot");
}

void indirectCall() {
    Bytes instruction{0x49, 0xbb};
    Io::AppendU64(instruction, 0x12345678abcdef01ull);
    emit(instruction, {0xff, 0x11, 0x90, 0x90, 0x90, 0x4c, 0x89, 0xd8});
    auto code = canaryFunction(instruction);
    const auto calleeOffset = static_cast<std::uint32_t>(code.size());
    code.push_back(0xc3);
    const Fixture fixture(code, {CodeAddress, CodeAddress + calleeOffset});
    const auto output = patch(fixture);
    const Executable executable(output.Sections);
    auto callee = reinterpret_cast<std::uint64_t>(executable.Address(LoadRva + calleeOffset));
    State state;
    executable.Run(&callee, state);
    require(intact(state, 15), "Indirect call corrupted bytes outside its original return-address slot");
    require(state.Value == 0x12345678abcdef01ull, "Indirect memory call clobbered R11");
    require(state.RspBefore == state.RspAfter, "Indirect memory call changed RSP");
}

void generatedStub() {
    std::size_t offset = 0;
    const Fixture fixture(canaryFunction({0x48, 0x8b, 0x81, 0, 0, 0, 0}, true, &offset));
    WindowsLoadImage image(fixture.Source, fixture.Headers);
    auto sections = image.BuildSections();
    auto next = image.GetEndRva();
    const auto stub = next;
    const auto site = CheckedRva(LoadRva + offset);
    Bytes body{0x48, 0x8b, 0x81, 0, 0, 0, 0, 0xe9, 0, 0, 0, 0};
    Io::WriteU32(body, 8, site + 7 - (stub + 12));
    sections.front().Data[offset] = 0xe9;
    Io::WriteU32(sections.front().Data, offset + 1, stub - (site + 5));
    sections.front().Data[offset + 5] = sections.front().Data[offset + 6] = 0x90;
    const auto stubIndex = sections.size();
    sections.push_back({".amdstub", stub, SectionRead | SectionExecute | 0x20u, std::move(body)});
    next = AlignRva(stub + sections.back().Data.size());
    const auto result = WindowsRedZonePatcher().Build(fixture.Source, fixture.Headers, image, sections, next, {}, "generated-stub");
    require(!WindowsRedZonePatcher::Enabled() || (sections[stubIndex].Data[0] == 0xe9 && result.RefusedFunctionCount == 0), "Pass did not follow and protect an existing AMD trampoline");
    const Executable executable(sections);
    State state;
    std::uint64_t page = 0x1234;
    executable.Run(&page, state);
    require(intact(state) && state.Value == 0x1234 && state.RspBefore == state.RspAfter, "Composed AMD and red-zone trampolines changed execution");
}

void jumpTables() {
    if (!WindowsRedZonePatcher::Enabled()) return;
    Bytes bounded{0x48, 0x89, 0x44, 0x24, 0xf8, 0x83, 0xf8, 0x01, 0x77, 0x22, 0x48, 0x8d, 0x15, 0x1c, 0, 0, 0, 0x48, 0x63, 0x04, 0x82, 0x48, 0x01, 0xd0, 0xff, 0xe0};
    emit(bounded, {0x48, 0x8b, 0x01, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0xc3, 0x48, 0x89, 0x01, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0xc3, 0xc3});
    Io::AppendU32(bounded, static_cast<std::uint32_t>(-19));
    Io::AppendU32(bounded, static_cast<std::uint32_t>(-10));
    const auto first = patch(Fixture(bounded)).Result;
    require(first.IndirectRedZoneFunctionCount == 0 && first.SweptInstructionCount == 0 && first.PatchedMemoryInstructionCount == 3 && first.UnrelocatableMemoryInstructionCount == 0, "Bounded jump-table targets were not proved and patched");
    Bytes scanned{0x48, 0x89, 0x44, 0x24, 0xf8, 0x48, 0x8d, 0x15, 0, 0, 0, 0, 0x48, 0x63, 0x04, 0x82, 0x48, 0x01, 0xd0, 0xff, 0xe0};
    emit(scanned, {0x48, 0x8b, 0x81, 0, 0, 0, 0, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0xc3, 0x48, 0x89, 0x81, 0, 0, 0, 0, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0xc3});
    Fixture fixture(scanned);
    Io::WriteU32(fixture.Source, 0x208, 0x3100 - (CodeAddress + 12));
    Io::WriteU32(fixture.Source, 0x1500, CodeAddress + 21 - 0x3100);
    Io::WriteU32(fixture.Source, 0x1504, CodeAddress + 34 - 0x3100);
    const auto second = patch(fixture).Result;
    require(second.IndirectRedZoneFunctionCount == 0 && second.SweptInstructionCount == 0 && second.PatchedMemoryInstructionCount == 3 && second.UnrelocatableMemoryInstructionCount == 0, "Scanned jump-table targets were not proved and patched");
}

void shortRelay() {
    if (!WindowsRedZonePatcher::Enabled()) return;
    Bytes code{0x48, 0x89, 0x44, 0x24, 0xf8, 0x74, 0x02, 0xeb, 0x02, 0xeb, 0x03, 0x48, 0x8b, 0x01, 0x48, 0x8b, 0x44, 0x24, 0xf8};
    code.insert(code.end(), 10, 0x90);
    code.push_back(0xc3);
    const auto output = patch(Fixture(code));
    require(output.Result.PatchedMemoryInstructionCount == 1 && output.Result.UnrelocatableMemoryInstructionCount == 0, "Short memory site did not find a nearby relay host");
    const auto& bytes = output.Sections.front().Data;
    require(bytes[11] == 0xeb && bytes[14] == 0xe9 && bytes[19] == 0xe9, "Relay jumps were not installed at the site and host");
}

struct Fault {
    void* Page;
    unsigned Count = 0;
    bool Unprotected = false;
    std::uint64_t Rsp = 0;
};

thread_local Fault* activeFault = nullptr;

LONG CALLBACK handler(EXCEPTION_POINTERS* exception) {
    auto* fault = activeFault;
    const auto* record = exception->ExceptionRecord;
    if (fault == nullptr || record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[1] < reinterpret_cast<std::uintptr_t>(fault->Page) || record->ExceptionInformation[1] - reinterpret_cast<std::uintptr_t>(fault->Page) >= 4096) return EXCEPTION_CONTINUE_SEARCH;
    ++fault->Count;
    fault->Rsp = exception->ContextRecord->Rsp;
    DWORD previous = 0;
    fault->Unprotected = VirtualProtect(fault->Page, 4096, PAGE_READWRITE, &previous) != 0;
    return fault->Unprotected ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

int faultCanary(const bool store, const bool vectorStore = false) {
    const Fixture fixture(canaryFunction(vectorStore ? Bytes{0xc4, 0xe1, 0xf9, 0x6e, 0xe0, 0xc4, 0xe3, 0x5d, 0x38, 0xe4, 0x01, 0xc5, 0xfe, 0x7f, 0x21} : store ? Bytes{0x48, 0x89, 0x81, 0, 0, 0, 0} : Bytes{0x48, 0x8b, 0x81, 0, 0, 0, 0}));
    const auto output = patch(fixture);
    const Executable executable(output.Sections);
    Allocation page(4096, PAGE_READWRITE);
    *static_cast<std::uint64_t*>(page.Pointer) = 0x1234;
    DWORD previous = 0;
    require(VirtualProtect(page.Pointer, 4096, store ? PAGE_READONLY : PAGE_NOACCESS, &previous) != 0, "Cannot protect fault fixture page");
    Fault fault{page.Pointer};
    const auto registration = AddVectoredExceptionHandler(1, handler);
    require(registration != nullptr, "Cannot register vectored exception handler");
    activeFault = &fault;
    State state;
    executable.Run(static_cast<std::uint64_t*>(page.Pointer), state);
    activeFault = nullptr;
    RemoveVectoredExceptionHandler(registration);
    require(fault.Count == 1 && fault.Unprotected, "Fault fixture did not resume exactly one protected-page access");
    require(state.RspBefore == state.RspAfter, "Faulting instruction changed RSP");
    require(store ? *static_cast<std::uint64_t*>(page.Pointer) == Canary : state.Value == 0x1234, "Faulting instruction did not complete");
    if (WindowsRedZonePatcher::Enabled() && (!vectorStore || WindowsRedZonePatcher::VectorRegisters())) {
        require(intact(state), "Protected-page fault corrupted the patched red-zone canaries");
        require(fault.Rsp == state.RspBefore - 128, "Fault was not delivered below the original red zone");
    } else {
        require(fault.Rsp == state.RspBefore, "Disabled pass lowered RSP");
        if (intact(state)) { std::cout << "SKIP: this Windows exception delivery preserved the unpatched canaries\n"; return 77; }
        std::cout << "Unpatched Windows exception delivery corrupted red-zone canaries\n";
    }
    return 0;
}

}

int main(const int argc, char* argv[]) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "unit";
        if (mode == "fault-store") return faultCanary(true);
        if (mode == "fault-load") return faultCanary(false);
        if (mode == "fault-vector-store") return faultCanary(true, true);
        require(mode == "unit", "Unknown red-zone test mode");
        structural();
        equivalent({0x48, 0x8b, 0x81, 0, 0, 0, 0}, false);
        equivalent({0x48, 0x89, 0x81, 0, 0, 0, 0}, true);
        if (WindowsRedZonePatcher::VectorRegisters()) equivalent({0x66, 0x48, 0x0f, 0x6e, 0xe0, 0x66, 0x0f, 0xd6, 0x21}, true);
        ripRelative();
        liveCall();
        indirectCall();
        generatedStub();
        jumpTables();
        shortRelay();
        std::cout << "Red-zone Windows tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
