// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from shadPS4 / Kyty SuperMedo src/loader/redZonePatcher.h for AnyPS5.

#ifndef ELFPATCHER_WINDOWS_WINDOWSREDZONEPATCHER_HPP
#define ELFPATCHER_WINDOWS_WINDOWSREDZONEPATCHER_HPP

#include <elfpatcher/windows/WindowsLoadImage.hpp>
#include <span>
#include <string_view>

namespace Elfpatcher::Windows {

struct RedZonePatchResult {
    std::uint64_t FunctionCount = 0;
    std::uint64_t InstructionCount = 0;
    std::uint64_t RedZoneFunctionCount = 0;
    std::uint64_t MemoryInstructionCount = 0;
    std::uint64_t ShortMemoryInstructionCount = 0;
    std::uint64_t PatchedMemoryInstructionCount = 0;
    std::uint64_t StackDependentMemoryInstructionCount = 0;
    std::uint64_t ControlFlowMemoryInstructionCount = 0;
    std::uint64_t UnrelocatableMemoryInstructionCount = 0;
    std::uint64_t IndirectRedZoneFunctionCount = 0;
    std::uint64_t SweptInstructionCount = 0;
    std::uint64_t LiveCallCount = 0;
    std::uint64_t RefusedFunctionCount = 0;
    std::uint64_t MissingEhFrameCount = 0;
};

class WindowsRedZonePatcher {
public:
    static bool Enabled();
    static bool VectorRegisters();
    RedZonePatchResult Build(const std::vector<std::uint8_t>& source, const std::vector<Domain::ProgramHeader>& headers, const WindowsLoadImage& image, std::vector<PeSection>& sections, std::uint32_t& nextRva, std::span<const std::uint32_t> baseRelocations = {}, std::string_view module = "executable") const;
};

}

#endif
