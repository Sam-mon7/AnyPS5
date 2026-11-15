#include "prx/libc/include/GuestStacks.hpp"

#ifdef _WIN32
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"
#include <algorithm>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <windows.h>
#endif

namespace GuestStacks {
namespace {

#ifdef _WIN32
struct StackBounds {
    std::uintptr_t base;
    std::uintptr_t limit;
    std::uintptr_t deallocation;
};

constexpr std::size_t DeallocationStackOffset = 0x1478;

auto& rangesLock = *new std::mutex();
auto& changesLock = *new std::mutex();
auto& ranges = *new std::map<const void*, std::pair<std::uintptr_t, std::uintptr_t>>();

StackBounds CurrentBounds() {
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    auto* teb = reinterpret_cast<std::uint8_t*>(tib);
    return {reinterpret_cast<std::uintptr_t>(tib->StackBase), reinterpret_cast<std::uintptr_t>(tib->StackLimit),
            *reinterpret_cast<std::uintptr_t*>(teb + DeallocationStackOffset)};
}

void ApplyBounds(const StackBounds& bounds) {
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    auto* teb = reinterpret_cast<std::uint8_t*>(tib);
    tib->StackBase = reinterpret_cast<void*>(bounds.base);
    tib->StackLimit = reinterpret_cast<void*>(bounds.limit);
    *reinterpret_cast<std::uintptr_t*>(teb + DeallocationStackOffset) = bounds.deallocation;
}

bool RegisteredBounds(std::uintptr_t stackPointer, StackBounds& bounds) {
    std::lock_guard lock(rangesLock);
    for (const auto& [owner, range] : ranges) {
        const auto [low, high] = range;
        if (stackPointer < low || stackPointer >= high) continue;
        bounds = {high, low, low};
        return true;
    }
    return false;
}

bool HostBounds(std::uintptr_t stackPointer, StackBounds& bounds) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(stackPointer), &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    bounds = {base + memory.RegionSize, base, reinterpret_cast<std::uintptr_t>(memory.AllocationBase)};
    return true;
}
#endif

}

void GuestStackRegister_nid_no_patch(const void* pointer, std::size_t bytes) {
    GuestStackSet_nid_no_patch(pointer, pointer, bytes);
}

void GuestStackUnregister_nid_no_patch(const void* owner) {
    GuestStackSet_nid_no_patch(owner, nullptr, 0);
}

void GuestStackSet_nid_no_patch(const void* owner, const void* pointer, std::size_t bytes) {
#ifdef _WIN32
    const auto low = reinterpret_cast<std::uintptr_t>(pointer);
    const auto high = low + bytes;
    if (high < low) throw std::invalid_argument("Guest stack range overflows");
    std::lock_guard change(changesLock);
    std::pair<std::uintptr_t, std::uintptr_t> previous{};
    {
        std::lock_guard lock(rangesLock);
        const auto same = ranges.find(owner);
        if (same != ranges.end() && same->second == std::pair{low, high}) return;
        if (same != ranges.end()) previous = same->second;
    }
    if (bytes != 0) GuestArena::GuestArenaPinWritable_nid_postfix(pointer, bytes);
    {
        std::lock_guard lock(rangesLock);
        if (bytes != 0) ranges[owner] = {low, high};
        else ranges.erase(owner);
        if (bytes != 0) GuestArena::RefreshFaultStacks(low, bytes);
        if (previous.second > previous.first) GuestArena::RefreshFaultStacks(previous.first, previous.second - previous.first);
    }
    if (previous.second > previous.first) GuestArena::GuestArenaUnpinWritable_nid_postfix(reinterpret_cast<const void*>(previous.first), previous.second - previous.first);
#else
    (void)owner;
    (void)pointer;
    (void)bytes;
#endif
}

#ifdef _WIN32
std::mutex& RegistryMutex() { return rangesLock; }

bool OverlapsRegistered(std::uintptr_t first, std::uintptr_t stop) {
    return std::any_of(ranges.begin(), ranges.end(), [&](const auto& entry) {
        return first < entry.second.second && stop > entry.second.first;
    });
}
#endif

void APS5_VABI GuestStackSwitch_nid_no_patch(std::uintptr_t stackPointer) {
#ifdef _WIN32
    const auto current = CurrentBounds();
    if (stackPointer >= current.deallocation && stackPointer < current.base) return;
    StackBounds target{};
    if (RegisteredBounds(stackPointer, target)) ApplyBounds(target);
    else if (HostBounds(stackPointer, target)) {
        GuestStackRegister_nid_no_patch(reinterpret_cast<const void*>(target.limit), target.base - target.limit);
        ApplyBounds(target);
    }
#else
    (void)stackPointer;
#endif
}

}
