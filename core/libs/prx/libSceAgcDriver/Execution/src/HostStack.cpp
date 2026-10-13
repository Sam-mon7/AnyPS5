#include "prx/libSceAgcDriver/Execution/include/HostStack.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

extern "C" void Aps5RunOnStack_nid_no_patch(void* top, void (*function)(void*), void* argument);

#ifdef _WIN32
asm(R"(
    .text
    .globl Aps5RunOnStack_nid_no_patch
    .def Aps5RunOnStack_nid_no_patch; .scl 2; .type 32; .endef
    .seh_proc Aps5RunOnStack_nid_no_patch
Aps5RunOnStack_nid_no_patch:
    pushq %rbp
    .seh_pushreg %rbp
    movq %rsp, %rbp
    .seh_setframe %rbp, 0
    .seh_endprologue
    movq %rcx, %rsp
    subq $32, %rsp
    movq %r8, %rcx
    callq *%rdx
    movq %rbp, %rsp
    popq %rbp
    ret
    .seh_endproc
)");
#else
asm(R"(
    .text
    .globl Aps5RunOnStack_nid_no_patch
    .type Aps5RunOnStack_nid_no_patch, @function
Aps5RunOnStack_nid_no_patch:
    .cfi_startproc
    pushq %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq %rsp, %rbp
    .cfi_def_cfa_register %rbp
    movq %rdi, %rsp
    movq %rdx, %rdi
    callq *%rsi
    movq %rbp, %rsp
    popq %rbp
    .cfi_def_cfa %rsp, 8
    ret
    .cfi_endproc
    .size Aps5RunOnStack_nid_no_patch, .-Aps5RunOnStack_nid_no_patch
)");
#endif

namespace AgcDriver {

namespace {

constexpr std::size_t ReservedBytes = 16u << 20;
constexpr std::size_t CommittedBytes = 256u << 10;
constexpr std::size_t PageBytes = 4096;

struct HostStack {
    std::uint8_t* base = nullptr;
    std::uint8_t* top = nullptr;
#ifdef _WIN32
    void* limit = nullptr;
#endif

    void Allocate() {
        if (base != nullptr) return;
#ifdef _WIN32
        auto* reserved = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, ReservedBytes, MEM_RESERVE, PAGE_READWRITE));
        if (reserved == nullptr) throw std::runtime_error("AGC driver: cannot reserve a host stack");
        auto* committed = reserved + ReservedBytes - CommittedBytes;
        if (VirtualAlloc(committed, CommittedBytes, MEM_COMMIT, PAGE_READWRITE) == nullptr || VirtualAlloc(committed - PageBytes, PageBytes, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD) == nullptr) {
            VirtualFree(reserved, 0, MEM_RELEASE);
            throw std::runtime_error("AGC driver: cannot commit a host stack");
        }
        limit = committed;
#else
        void* mapped = mmap(nullptr, ReservedBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (mapped == MAP_FAILED) throw std::runtime_error("AGC driver: cannot map a host stack");
        auto* reserved = static_cast<std::uint8_t*>(mapped);
        if (mprotect(reserved, PageBytes, PROT_NONE) != 0) {
            munmap(reserved, ReservedBytes);
            throw std::runtime_error("AGC driver: cannot protect a host stack");
        }
#endif
        base = reserved;
        top = reserved + ReservedBytes;
    }

    ~HostStack() {
        if (base == nullptr) return;
#ifdef _WIN32
        VirtualFree(base, 0, MEM_RELEASE);
#else
        munmap(base, ReservedBytes);
#endif
    }
};

HostStack& CurrentHostStack() {
    struct HostStackStorage {};
    return HostThreadLocal<HostStack, HostStackStorage>();
}

}

bool OnHostStack() {
    const auto& stack = CurrentHostStack();
    const auto here = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    return stack.base != nullptr && here >= reinterpret_cast<std::uintptr_t>(stack.base) && here < reinterpret_cast<std::uintptr_t>(stack.top);
}

void RunOnHostStack(void (*function)(void*), void* argument) {
    auto& hostStack = CurrentHostStack();
    hostStack.Allocate();
#ifdef _WIN32
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    auto* teb = reinterpret_cast<std::uint8_t*>(tib);
    auto** deallocation = reinterpret_cast<void**>(teb + 0x1478);
    void* const savedBase = tib->StackBase;
    void* const savedLimit = tib->StackLimit;
    void* const savedDeallocation = *deallocation;
    tib->StackBase = hostStack.top;
    tib->StackLimit = hostStack.limit;
    *deallocation = hostStack.base;
    Aps5RunOnStack_nid_no_patch(hostStack.top, function, argument);
    hostStack.limit = tib->StackLimit;
    tib->StackBase = savedBase;
    tib->StackLimit = savedLimit;
    *deallocation = savedDeallocation;
#else
    Aps5RunOnStack_nid_no_patch(hostStack.top, function, argument);
#endif
}

}
