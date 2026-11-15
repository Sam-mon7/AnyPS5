#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTSTACKS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTSTACKS_HPP

#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstddef>
#include <cstdint>
#ifdef _WIN32
#include <mutex>
#endif

namespace GuestStacks {

extern "C" {

void GuestStackRegister_nid_no_patch(const void* pointer, std::size_t bytes);
void GuestStackSet_nid_no_patch(const void* owner, const void* pointer, std::size_t bytes);
void GuestStackUnregister_nid_no_patch(const void* owner);
void APS5_VABI GuestStackSwitch_nid_no_patch(std::uintptr_t stackPointer);

}

#ifdef _WIN32
std::mutex& RegistryMutex();
bool OverlapsRegistered(std::uintptr_t first, std::uintptr_t stop);
#endif

}

#endif
