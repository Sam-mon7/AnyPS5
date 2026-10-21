#include "prx/libkernel/Pthread/include/DemonsSoulsIdle.hpp"
#include "SceTypes.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
int APS5_VABI scePthreadRename(Pthread thread, const char* name);
Pthread APS5_VABI scePthreadSelf();
}

static void Require(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

static void* APS5_VABI Worker(void*) {
    Require(scePthreadRename(scePthreadSelf(), "BPE JobWorkerThread CPU2") == 0, "Cannot rename worker");
    return nullptr;
}

static unsigned waits = 0;

static void APS5_VABI CountWait() {
    ++waits;
}

static void SetWait(std::uint8_t* thunk, std::uint64_t target) {
    DWORD previous = 0;
    Require(VirtualProtect(thunk, 0x1000, PAGE_READWRITE, &previous), "Cannot edit test callback");
    std::memcpy(thunk + 49, &target, sizeof(target));
    Require(VirtualProtect(thunk, 0x1000, previous, &previous), "Cannot restore test callback protection");
    Require(FlushInstructionCache(GetCurrentProcess(), thunk, 86), "Cannot flush test callback");
}

static void CheckPatch() {
    using namespace DemonsSoulsIdle;
    constexpr std::size_t imageSize = 0x850000;
    auto* image = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, imageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Require(image != nullptr, "Cannot allocate test image");
    auto* call = image + CallRva;
    auto* poll = image + PollRva;
    std::memcpy(call, CallBytes.data(), CallBytes.size());
    std::memcpy(poll, PollBytes.data(), PollBytes.size());
    std::uint32_t result = 0x12345601;
    std::uint8_t body[] = {
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0x8b, 0x00,
        0x48, 0x81, 0xc4, 0x88, 0x00, 0x00, 0x00,
        0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3,
    };
    const auto address = reinterpret_cast<std::uint64_t>(&result);
    std::memcpy(body + 2, &address, sizeof(address));
    std::memcpy(poll + 27, body, sizeof(body));
    Require(Install(image, PollRva) != nullptr, "Short image accepted");
    Require(Install(image, imageSize) != nullptr, "Non-executable image accepted");
    DWORD previous = 0;
    poll[0] ^= 1;
    Require(VirtualProtect(image, imageSize, PAGE_EXECUTE_READ, &previous), "Cannot protect test image");
    Require(Install(image, imageSize) != nullptr, "Changed poll signature accepted");
    Require(std::memcmp(call, CallBytes.data(), CallBytes.size()) == 0, "Rejected patch changed the caller");
    Require(VirtualProtect(image, imageSize, PAGE_READWRITE, &previous), "Cannot restore test image");
    poll[0] ^= 1;
    Require(VirtualProtect(image, imageSize, PAGE_EXECUTE_READ, &previous), "Cannot execute test image");
    Require(FlushInstructionCache(GetCurrentProcess(), image, imageSize), "Cannot flush test image");
    const char* failure = Install(image, imageSize);
    Require(failure == nullptr, failure ? failure : "Patch failed");
    Require(std::memcmp(call + 5, CallBytes.data() + 5, CallBytes.size() - 5) == 0, "Patch changed adjacent instructions");
    std::int32_t displacement = 0;
    std::memcpy(&displacement, call + 1, sizeof(displacement));
    auto* thunk = call + 5 + displacement;
    using Poll = std::uint32_t (APS5_VABI *)();
    const auto invoke = reinterpret_cast<Poll>(thunk);
    SetWait(thunk, reinterpret_cast<std::uint64_t>(&CountWait));
    for (unsigned i = 0; i < 16; ++i) Require(invoke() == result, "Ready result was not preserved");
    Require(waits == 0, "A runnable job slept");
    result = 0x12345600;
    for (unsigned i = 0; i < 16; ++i) Require(invoke() == result, "Idle result was not preserved");
    Require(waits == 16, "An idle scan did not wait exactly once");
    Require(Install(image, imageSize) != nullptr, "Already patched caller accepted again");
    SetWait(thunk, reinterpret_cast<std::uint64_t>(&Wait));
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 64; ++i) Require(invoke() == result, "Timer changed the poll result");
    Require(std::chrono::steady_clock::now() - start >= std::chrono::microseconds(64 * 50), "Idle timer returned too early");
    VirtualFree(thunk, 0, MEM_RELEASE);
    VirtualFree(image, 0, MEM_RELEASE);
    std::fprintf(stderr, "Job idle patch: passed\n");
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "patch") == 0) {
        CheckPatch();
        return 0;
    }
    for (const char* name : {"ordinary-worker", "BPE JobWorkerThread CPU0", "BPE JobWorkerThread CPU1"}) {
        Pthread thread = nullptr;
        Require(scePthreadCreate(&thread, nullptr, Worker, nullptr, name) == 0, "Cannot create worker");
        Require(scePthreadJoin(thread, nullptr) == 0, "Cannot join worker");
    }
    std::fprintf(stderr, "Job idle wait: passed\n");
}
