#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/CommandBuffer.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "prx/libc/include/Shutdown.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace AgcDriver::DriverDetail {

struct DriverTestAccess {
    static Submission CaptureWait(std::span<const std::uint32_t> packet) {
        Submission submission{};
        submission.commands.assign(packet.begin(), packet.end());
        auto& driver = Driver::Get();
        std::lock_guard lock(driver.mutex);
        driver.reserveOutputs(submission);
        submission.received = ++driver.eventSerial;
        return submission;
    }

    static void Bump() {
        Driver::bumpEpoch(&EpochBumps::submissions);
    }

    static void Wait(const Submission& submission) {
        Driver::Get().waitMemory(submission.commands, submission.queue, PacketHistory{}, submission.received, submission.heldAtSubmit.contains(0), false, Driver::skipHeldWaitEpoch(submission, 0));
    }

    static void RecordLabel(std::uint64_t address, std::uint32_t value, std::uint32_t queue) {
        auto& driver = Driver::Get();
        driver.noteLabelStore(address, std::as_bytes(std::span(&value, 1)), ++driver.eventSerial, queue);
    }
};

}

namespace {

struct alignas(64) LabelState {
    volatile std::uint32_t label = 0;
    volatile std::uint32_t entered = 0;
    volatile std::uint32_t after = 0;
    volatile std::uint32_t queued = 0;
};

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool CheckWaitEpochs() {
    using namespace AgcDriver::GuestMemory;
    using Access = AgcDriver::DriverDetail::DriverTestAccess;
    if (!WriteWatched()) return false;
    constexpr std::size_t block = 65536;
    constexpr std::size_t bytes = 2 * block;
#ifdef _WIN32
    auto* memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, block));
    GuestArena::GuestArenaCommit_nid_postfix(memory, bytes, PAGE_READWRITE, bytes);
#else
    auto* memory = static_cast<std::uint8_t*>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Require(memory != MAP_FAILED, "cannot allocate the wait-epoch fixture");
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(memory, bytes);
#endif
    struct Release {
        std::uint8_t* memory;
        ~Release() {
#ifdef _WIN32
            GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
#else
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(memory, bytes);
            munmap(memory, bytes);
#endif
        }
    } release{memory};
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto address = base + block;
    auto* label = reinterpret_cast<volatile std::uint32_t*>(address);
    const std::array<std::uint32_t, 7> packet{0xc0053c00u, 0x13u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 1, 0xffffffffu, 0x19u};
    Require(Watched(base, bytes), "the wait-epoch fixture is not watched");
    memory[0] = 1;
    *label = 0;
    auto submission = Access::CaptureWait(packet);
    Require(submission.heldAtSubmit.empty(), "an unsatisfied wait was captured as satisfied");
    Access::Bump();
    auto generation = CollectWrites(base, 4096);
    Require(generation != 0, "the wait-epoch fixture could not be collected");
    auto epoch = CollectEpoch();
    Access::RecordLabel(address, 1, 0x20);
    memory[0] = 2;
    *label = 1;
    Access::Wait(submission);
    Require(CollectEpoch() != epoch, "a CPU-signalled wait reused its collect epoch despite a GPU label history");
    CollectWrites(base, 4096);
    Require(!UnchangedSince(base, 4096, generation), "a CPU write before a CPU-signalled label was missed");

    *label = 0;
    submission = Access::CaptureWait(packet);
    Access::Bump();
    generation = CollectWrites(base, 4096);
    epoch = CollectEpoch();
    memory[0] = 3;
    std::thread producer([&] {
        Access::Bump();
        Access::RecordLabel(address, 1, 0x20);
    });
    producer.join();
    Access::Wait(submission);
    Require(CollectEpoch() != epoch, "another queue's recorded label reused the consumer's collect epoch");
    CollectWrites(base, 4096);
    Require(!UnchangedSince(base, 4096, generation), "a producer's epoch bump hid a CPU write from the consumer");

    Access::Bump();
    generation = CollectWrites(base, 4096);
    epoch = CollectEpoch();
    memory[0] = 4;
    *label = 1;
    submission = Access::CaptureWait(packet);
    Require(submission.heldAtSubmit.contains(0), "a satisfied wait was not captured");
    Access::Wait(submission);
    Require(CollectEpoch() != epoch, "a held wait observed after the epoch bump reused that epoch");
    CollectWrites(base, 4096);
    Require(!UnchangedSince(base, 4096, generation), "a CPU write before a newly captured held label was missed");

    generation = CollectWrites(base, 4096);
    memory[0] = 5;
    *label = 1;
    submission = Access::CaptureWait(packet);
    while (std::chrono::steady_clock::now() <= submission.heldAtSubmitAt) std::this_thread::yield();
    Access::Bump();
    const auto capturedGeneration = CollectWrites(base, 4096);
    Require(!UnchangedSince(base, 4096, generation), "the epoch covering a captured CPU signal missed its preceding write");
    generation = capturedGeneration;
    const bool disabled = std::getenv("APS5_NO_HELD_WAIT_EPOCH") != nullptr;
    for (const auto value : {1u, 0u}) {
        *label = value;
        epoch = CollectEpoch();
        Access::Wait(submission);
        Require((CollectEpoch() != epoch) == disabled, "a covered held wait did not obey APS5_NO_HELD_WAIT_EPOCH");
        CollectWrites(base, 4096);
        Require(UnchangedSince(base, 4096, generation), "a label store changed the separate data block");
    }
    return true;
}

void DelayExit() {
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

std::array<std::uint32_t, 5> WriteData(volatile std::uint32_t* destination) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(destination));
    return {0xc0033700, 0x00100200, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 1};
}

std::vector<std::uint32_t> WaitingCommands(LabelState& state) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&state.label));
    const auto entered = WriteData(&state.entered);
    const std::array<std::uint32_t, 7> wait{0xc0053c00, 0x13, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 1, 0xffffffffu, 0x19};
    const auto after = WriteData(&state.after);
    std::vector<std::uint32_t> words(entered.begin(), entered.end());
    words.insert(words.end(), wait.begin(), wait.end());
    words.insert(words.end(), after.begin(), after.end());
    return words;
}

void Submit(std::uint32_t queue, const std::vector<std::uint32_t>& words) {
    Packet packet{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
    const auto result = queue == 0 ? sceAgcDriverSubmitDcb(&packet) : sceAgcDriverSubmitAcb(queue, &packet);
    Require(result == 0, "shutdown wait submission failed");
}

void CheckCommandBufferSubmission() {
    struct MockQueueContext {
        std::uint32_t unused = 0;
        std::uint32_t queue_id = 0;
    };
    MockQueueContext graphicsCtx{0, 0};
    MockQueueContext computeCtx{0, 0x20};
    MockQueueContext invalidCtx{0, 1};

    Packet emptyPacket{nullptr, 0, 0, {}};
    Require(sceAgcDriverSubmitCommandBuffer(nullptr, &emptyPacket) == 22, "null context submit must return EINVAL");
    Require(sceAgcDriverSubmitCommandBuffer(&graphicsCtx, nullptr) == 22, "null packet submit must return EINVAL");
    Require(sceAgcDriverSubmitMultiCommandBuffers(nullptr, nullptr, nullptr, 0) == 0, "count 0 multi submit must return 0");

    std::uint32_t* cmdBufs[] = {nullptr};
    const std::uint32_t cmdSizes[] = {0};
    Require(sceAgcDriverSubmitMultiCommandBuffers(nullptr, cmdBufs, cmdSizes, 1) == 22, "null context multi submit must return EINVAL");
    Require(sceAgcDriverSubmitMultiCommandBuffers(&graphicsCtx, nullptr, cmdSizes, 1) == 22, "null cmdBufs multi submit must return EINVAL");
    Require(sceAgcDriverSubmitMultiCommandBuffers(&graphicsCtx, cmdBufs, nullptr, 1) == 22, "null cmdSizes multi submit must return EINVAL");
    Require(sceAgcDriverSubmitMultiCommandBuffers(&graphicsCtx, cmdBufs, cmdSizes, 0) == 0, "empty multi submission failed");
    Require(sceAgcDriverSubmitMultiCommandBuffers(&graphicsCtx, cmdBufs, cmdSizes, 1) == 0, "graphics context multi submit failed");
    Require(sceAgcDriverSubmitMultiCommandBuffers(&computeCtx, cmdBufs, cmdSizes, 1) == 0, "compute context multi submit failed");

    bool submitRejected = false;
    try {
        sceAgcDriverSubmitCommandBuffer(&invalidCtx, &emptyPacket);
    } catch (const std::runtime_error&) {
        submitRejected = true;
    }
    Require(submitRejected, "unsupported queue context was not rejected");

    bool multiRejected = false;
    try {
        sceAgcDriverSubmitMultiCommandBuffers(&invalidCtx, cmdBufs, cmdSizes, 1);
    } catch (const std::runtime_error&) {
        multiRejected = true;
    }
    Require(multiRejected, "unsupported queue context for multi submit was not rejected");
}

void CheckShutdownCancellation() {
    static std::array<LabelState, 3> states{};
    constexpr std::array<std::uint32_t, 3> queues{0, 0x20, 0x28};
    for (std::size_t index = 0; index < queues.size(); ++index) {
        Submit(queues[index], WaitingCommands(states[index]));
        const auto queued = WriteData(&states[index].queued);
        Submit(queues[index], {queued.begin(), queued.end()});
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (const auto& state : states) {
        while (state.entered != 1) {
            Require(std::chrono::steady_clock::now() < deadline, "queue did not reach its unsatisfied wait");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    for (const auto& state : states) {
        Require(state.label == 0, "unsatisfied label was unexpectedly signalled");
        Require(state.after == 0 && state.queued == 0, "unsatisfied wait completed before shutdown");
    }
    const auto started = std::chrono::steady_clock::now();
    LibcRunShutdown_nid_postfix();
    Require(std::chrono::steady_clock::now() - started < std::chrono::seconds(2), "shutdown did not promptly cancel waiting queues");
    for (const auto& state : states) {
        Require(state.label == 0, "shutdown signalled an unsatisfied label");
        Require(state.after == 0, "shutdown executed commands after an unsatisfied wait");
        Require(state.queued == 0, "shutdown executed a pending submission behind an unsatisfied wait");
    }
    LibcRunShutdown_nid_postfix();
}

}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--resolution-shutdown") == 0) {
        AgcDriverWaitIdle_nid_postfix();
        LibcRegisterShutdown_nid_postfix(DelayExit);
        LibcRequestExit_nid_postfix(0);
        AgcDriverResolveShaderAbi_nid_postfix(nullptr, {}, {});
        return 1;
    }
    try {
        if (argc == 2 && std::strcmp(argv[1], "--wait-epochs") == 0) {
            const bool watched = CheckWaitEpochs();
            LibcRunShutdown_nid_postfix();
            std::puts(watched ? "wait epoch tests passed" : "no write watching: skipped");
            return watched ? 0 : 77;
        }
        CheckCommandBufferSubmission();
        CheckShutdownCancellation();
        std::puts("unsatisfied label waits cancelled cleanly at shutdown");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        try {
            LibcRunShutdown_nid_postfix();
        } catch (const std::exception& shutdown) {
            std::fprintf(stderr, "shutdown: %s\n", shutdown.what());
        }
        return 1;
    }
}
