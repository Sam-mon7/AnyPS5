#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr std::uintptr_t EbootOffset = 0x10000;
constexpr std::uintptr_t AllocateRva = 0x7f9cf0;
constexpr std::uintptr_t ContextTargetRva = 0x80a510;
constexpr std::uintptr_t ContextSiteRva = 0x903d2d;
constexpr std::array<std::uintptr_t, 3> SiteRvas{0x909509, 0x909537, ContextSiteRva};
constexpr std::array<std::uintptr_t, 4> TlsSiteRvas{0x7f9e07, 0x7f9e4c, 0x7fa4ad, 0x7fa55d};
constexpr std::uintptr_t ContextBridgeOffset = 0x100;
constexpr std::uintptr_t StatsOffset = 0x1000;

struct AuditedFunction {
    std::uintptr_t rva;
    std::size_t size;
    std::uint64_t hash;
};

constexpr std::array<AuditedFunction, 7> AuditedFunctions{{
    {0x909460, 990, 0x551381ceb6002115ull},
    {0x909410, 74, 0x0310824bfaa60e06ull},
    {0x903460, 361, 0x380005cc9ab9ca19ull},
    {0x7f9cf0, 626, 0xc05fedb6d19828f6ull},
    {0x7fa380, 603, 0x3dd03cac0f8f1bdeull},
    {0x903bb0, 923, 0x6c9f9b4d877a80d5ull},
    {0x80a510, 10, 0xd175875203953d78ull},
}};

constexpr std::array<std::uint8_t, 61> Bridge{
    0x48, 0x85, 0xff, 0x74, 0x33, 0x81, 0xfa, 0x00, 0x10, 0x00, 0x00, 0x73, 0x05,
    0xba, 0x00, 0x10, 0x00, 0x00, 0xf0, 0x48, 0xff, 0x05, 0, 0, 0, 0,
    0xf0, 0x48, 0x01, 0x3d, 0, 0, 0, 0,
    0x48, 0x81, 0xc7, 0xff, 0x0f, 0x00, 0x00,
    0x48, 0x81, 0xe7, 0x00, 0xf0, 0xff, 0xff,
    0xf0, 0x48, 0x01, 0x3d, 0, 0, 0, 0, 0xe9, 0, 0, 0, 0};

std::uint64_t Fingerprint(const std::uint8_t* bytes, std::size_t size) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t index = 0; index < size; ++index) hash = (hash ^ bytes[index]) * 0x100000001b3ull;
    return hash;
}

bool Readable(const std::uint8_t* address, std::size_t size) {
    MEMORY_BASIC_INFORMATION memory{};
    return VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT &&
           (memory.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY | PAGE_READONLY | PAGE_READWRITE)) != 0 &&
           static_cast<const std::uint8_t*>(memory.BaseAddress) + memory.RegionSize >= address + size;
}

bool Audited(const std::uint8_t* eboot, const AuditedFunction& function) {
    std::array<std::uint8_t, 1024> normalized{};
    std::memcpy(normalized.data(), eboot + function.rva, function.size);
    constexpr std::array<std::uint8_t, 12> TlsLoad{0x66, 0x66, 0x66, 0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    for (const auto site : TlsSiteRvas) {
        const auto start = site - 3;
        if (start < function.rva || start + TlsLoad.size() > function.rva + function.size) continue;
        auto* span = normalized.data() + (start - function.rva);
        const bool relinked = span[0] == 0xe9 && std::all_of(span + 5, span + 12, [](std::uint8_t value) { return value == 0x90; });
        if (relinked) std::memcpy(span, TlsLoad.data(), TlsLoad.size());
    }
    return Fingerprint(normalized.data(), function.size) == function.hash;
}

bool WriteRelative(std::uint8_t* operand, std::uintptr_t next, std::uintptr_t target) {
    const auto difference = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next);
    if (difference < INT32_MIN || difference > INT32_MAX) return false;
    const auto value = static_cast<std::int32_t>(difference);
    std::memcpy(operand, &value, sizeof(value));
    return true;
}

std::uintptr_t RelativeTarget(const std::uint8_t* call) {
    std::int32_t displacement = 0;
    std::memcpy(&displacement, call + 1, sizeof(displacement));
    return reinterpret_cast<std::uintptr_t>(call) + 5 + displacement;
}

bool PatchCall(std::uint8_t* call, std::uintptr_t target) {
    std::array<std::uint8_t, 5> patch{0xe8};
    if (!WriteRelative(patch.data() + 1, reinterpret_cast<std::uintptr_t>(call) + 5, target)) return false;
    DWORD previous = 0;
    if (!VirtualProtect(call, patch.size(), PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(call, patch.data(), patch.size());
    VirtualProtect(call, patch.size(), previous, &previous);
    return FlushInstructionCache(GetCurrentProcess(), call, patch.size()) != 0;
}

const char* Install() {
    auto* image = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
    const std::uintptr_t imageSize = headers->OptionalHeader.SizeOfImage;
    auto* eboot = image + EbootOffset;
    for (const auto& function : AuditedFunctions) {
        if (EbootOffset + function.rva + function.size > imageSize || !Readable(eboot + function.rva, function.size) || !Audited(eboot, function)) return "code signature differs";
    }
    for (const auto site : SiteRvas) {
        const auto expected = site == ContextSiteRva ? ContextTargetRva : AllocateRva;
        if (eboot[site] != 0xe8 || RelativeTarget(eboot + site) != reinterpret_cast<std::uintptr_t>(eboot + expected)) return "code signature differs";
    }
    std::uint8_t* cave = nullptr;
    const auto first = (reinterpret_cast<std::uintptr_t>(image) + imageSize + 0xffff) & ~std::uintptr_t{0xffff};
    for (auto at = first; cave == nullptr && at < reinterpret_cast<std::uintptr_t>(image) + 0x70000000; at += 0x10000)
        cave = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (cave == nullptr) return "no code cave within a call's reach";
    const auto base = reinterpret_cast<std::uintptr_t>(cave);
    std::memset(cave, 0xcc, StatsOffset);
    std::memcpy(cave, Bridge.data(), Bridge.size());
    std::memcpy(cave + ContextBridgeOffset, Bridge.data(), Bridge.size());
    auto* context = cave + ContextBridgeOffset;
    const bool built =
        WriteRelative(cave + 0x16, base + 0x1a, base + StatsOffset) &&
        WriteRelative(cave + 0x1e, base + 0x22, base + StatsOffset + 8) &&
        WriteRelative(cave + 0x34, base + 0x38, base + StatsOffset + 16) &&
        WriteRelative(cave + 0x39, base + 0x3d, reinterpret_cast<std::uintptr_t>(eboot + AllocateRva)) &&
        WriteRelative(context + 0x16, base + ContextBridgeOffset + 0x1a, base + StatsOffset + 0x80) &&
        WriteRelative(context + 0x1e, base + ContextBridgeOffset + 0x22, base + StatsOffset + 0x88) &&
        WriteRelative(context + 0x34, base + ContextBridgeOffset + 0x38, base + StatsOffset + 0x90) &&
        WriteRelative(context + 0x39, base + ContextBridgeOffset + 0x3d, reinterpret_cast<std::uintptr_t>(eboot + ContextTargetRva));
    DWORD previous = 0;
    if (!built || !VirtualProtect(cave, StatsOffset, PAGE_EXECUTE_READ, &previous) || !FlushInstructionCache(GetCurrentProcess(), cave, StatsOffset)) {
        VirtualFree(cave, 0, MEM_RELEASE);
        return "cannot build the allocation bridges";
    }
    for (const auto site : SiteRvas) {
        if (!PatchCall(eboot + site, base + (site == ContextSiteRva ? ContextBridgeOffset : 0))) return "cannot redirect an allocation call";
    }
    return nullptr;
}

[[maybe_unused]] const bool installed = [] {
    const char* value = std::getenv("APS5_GPU_BUFFER_PAGES");
    if (value == nullptr || std::strcmp(value, "1") != 0) return false;
    const char* failure = Install();
    if (failure != nullptr)
        std::fprintf(stderr, "[gpu-buffer-pages] not installed: %s\n", failure);
    else
        std::fprintf(stderr, "[gpu-buffer-pages] constant buffers and render contexts now take whole 4 KiB pages\n");
    return failure == nullptr;
}();

}
#endif
