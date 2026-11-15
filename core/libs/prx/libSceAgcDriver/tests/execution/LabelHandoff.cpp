#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DrawPipeline;
namespace GuestMemory = AgcDriver::GuestMemory;
namespace ArchB = AgcDriver::ArchB;
using Clock = std::chrono::steady_clock;
using GpuLock = std::unique_lock<GuestMemory::GpuMutexType>;
constexpr auto Timeout = std::chrono::seconds(5);
constexpr std::uint64_t PriorWork = 0x123456789abcdef0ull;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("label handoff: " + message);
}

template<std::ptrdiff_t TLimit>
void Await(std::counting_semaphore<TLimit>& signal, const char* message) {
    Require(signal.try_acquire_for(Timeout), message);
}

struct State {
    std::binary_semaphore entered{0}, release{0}, waiterStarted{0};
    std::counting_semaphore<4> completed{0};
    std::atomic<bool> opened{false};
    alignas(8) std::array<std::byte, 32> memory{};
    std::vector<unsigned> order;
    std::array<bool, 2> consumed{}, heldAfterConsume{};
    std::array<std::uint32_t, 2> labelDepth{}, entryDepth{};
    std::uint32_t drawDepth = 0, retainedDepth = 0, afterLabelRetain = 0;
    std::uint32_t resolveDepth = 99;
    std::uint64_t work = 0;
    bool waiterBlocked = false, waiterSawEarly = false, waiterSawLabel = false;
    bool waiterSawWork = false, published = false;

    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(memory.data()); }
    void Open() {
        if (!opened.exchange(true, std::memory_order_acq_rel)) release.release();
    }
};

struct OpenOnExit {
    std::shared_ptr<State> state;
    ~OpenOnExit() { state->Open(); }
};

std::vector<std::byte> Bytes(std::size_t size, unsigned value = 0x31) {
    std::vector<std::byte> result(size);
    for (std::size_t index = 0; index < size; ++index) result[index] = static_cast<std::byte>(value + index);
    return result;
}

void Idle() {
    auto& pipeline = DrawPipeline::Queue0();
    const auto deadline = Clock::now() + Timeout;
    while (pipeline.Outstanding() != 0 && Clock::now() < deadline) std::this_thread::yield();
    Require(pipeline.Outstanding() == 0, "pipeline did not retire its items");
    pipeline.Drain(DrawPipeline::DrainReason::Submission);
    GpuLock lock(GuestMemory::GpuMutex(), std::try_to_lock);
    Require(lock.owns_lock(), "an idle pipeline retained GPU ownership");
    Require(GuestMemory::GpuMutex().DepthOnThisThread() == 1, "idle lock acquisition was recursive");
}

void QueueDraw(const std::shared_ptr<State>& state, bool nested = false, bool draw = true) {
    DrawPipeline::Queue0().Enqueue(draw ? DrawPipeline::CommitKind::Draw : DrawPipeline::CommitKind::Dispatch, [state, nested] {
        auto& gpu = GuestMemory::GpuMutex();
        std::lock_guard lock(gpu);
        GpuLock inner(gpu, std::defer_lock);
        if (nested) inner.lock();
        state->drawDepth = gpu.DepthOnThisThread();
        state->work = PriorWork;
        state->order.push_back(1);
        state->entered.release();
        state->release.acquire();
        DrawPipeline::RetainGpuMutexForLabel();
        state->retainedDepth = gpu.DepthOnThisThread();
    }, {}, 0, {}, {}, draw, draw ? DrawPipeline::ItemKind::Draw : DrawPipeline::ItemKind::Dispatch);
}

void QueueLabel(const std::shared_ptr<State>& state, std::vector<std::byte> queued, std::vector<std::byte> actual,
                unsigned index = 0, DrawPipeline::CommitKind kind = DrawPipeline::CommitKind::EndOfPipe,
                std::uint32_t queue = 0, std::uint64_t addressDelta = 0, bool resolve = false, bool unaligned = false) {
    const auto address = state->Address() + index * 16 + (unaligned ? 1 : 0);
    std::shared_ptr<DrawPipeline::ResolveSlot> slot;
    if (resolve) {
        slot = std::make_shared<DrawPipeline::ResolveSlot>();
        slot->run = [state](std::vector<DrawPipeline::Range>&) {
            state->resolveDepth = GuestMemory::GpuMutex().DepthOnThisThread();
            return std::uint64_t{1};
        };
    }
    const auto length = queued.size();
    DrawPipeline::Queue0().Enqueue(kind, [state, actual = std::move(actual), index, queue, address, addressDelta] {
        auto& gpu = GuestMemory::GpuMutex();
        state->entryDepth[index] = gpu.DepthOnThisThread();
        state->consumed[index] = DrawPipeline::ConsumeLabelGpuMutex(queue, address + addressDelta, actual);
        state->heldAfterConsume[index] = gpu.HeldByThisThread();
        {
            GpuLock lock(gpu, std::defer_lock);
            if (state->heldAfterConsume[index]) lock = GpuLock(gpu, std::adopt_lock);
            else lock.lock();
            state->labelDepth[index] = gpu.DepthOnThisThread();
            state->order.push_back(index + 2);
            std::memcpy(state->memory.data() + index * 16, actual.data(), actual.size());
            if (index == 0) state->published = true;
            DrawPipeline::RetainGpuMutexForLabel();
            state->afterLabelRetain = gpu.DepthOnThisThread();
        }
        state->completed.release();
    }, {{address, address + length}}, address, std::move(queued), {}, false,
       kind == DrawPipeline::CommitKind::Other ? DrawPipeline::ItemKind::Other : DrawPipeline::ItemKind::Label, std::move(slot));
}

void OwnershipAndWaiter(bool enabled, std::size_t size) {
    auto state = std::make_shared<State>();
    OpenOnExit release{state};
    const auto first = Bytes(size);
    const auto second = Bytes(size, 0x71);
    QueueDraw(state);
    Await(state->entered, "draw never entered");
    std::jthread waiter([state, first](std::stop_token stop) {
        GuestMemory::TagGpuLockThread(0x28);
        {
            GpuLock lock(GuestMemory::GpuMutex(), std::try_to_lock);
            state->waiterBlocked = !lock.owns_lock();
        }
        state->waiterStarted.release();
        const auto deadline = Clock::now() + Timeout;
        while (!stop.stop_requested() && Clock::now() < deadline) {
            GpuLock lock(GuestMemory::GpuMutex(), std::try_to_lock);
            if (lock.owns_lock()) {
                if (state->published) {
                    state->waiterSawLabel = std::equal(first.begin(), first.end(), state->memory.begin());
                    state->waiterSawWork = state->work == PriorWork;
                    return;
                }
                state->waiterSawEarly = true;
            }
            std::this_thread::yield();
        }
    });
    Await(state->waiterStarted, "other-queue waiter never attempted ownership");
    QueueLabel(state, first, first);
    QueueLabel(state, second, second, 1);
    state->Open();
    Await(state->completed, "first label did not complete");
    Await(state->completed, "second label did not complete");
    waiter.join();
    Idle();
    Require(state->drawDepth == 1, "draw did not begin with one hold");
    Require(state->retainedDepth == (enabled ? 2u : 1u), "eligible draw did not retain exactly one extra hold");
    Require(state->consumed[0] == enabled, "eligible successor did not exercise the selected path");
    Require(state->entryDepth[0] == (enabled ? 1u : 0u), "first label inherited the wrong depth");
    Require(state->heldAfterConsume[0] == enabled, "consuming did not transfer exact ownership");
    Require(state->labelDepth[0] == 1 && state->labelDepth[1] == 1, "label work ran under a recursive hold");
    Require(!state->consumed[1] && state->entryDepth[1] == 0, "handoff chained through a second label");
    Require(state->afterLabelRetain == 1, "a label retained ownership for another label");
    Require(state->order == std::vector<unsigned>{1, 2, 3}, "draw and labels did not execute in FIFO order");
    Require(std::equal(first.begin(), first.end(), state->memory.begin()), "first label bytes differ from the original path");
    Require(std::equal(second.begin(), second.end(), state->memory.begin() + 16), "second label bytes differ from the original path");
    Require(state->waiterBlocked, "waiter acquired while draw still held the mutex");
    Require(state->waiterSawLabel && state->waiterSawWork, "other-queue waiter did not observe label and prior work");
    Require(!enabled || !state->waiterSawEarly, "other-queue waiter acquired between the retained draw and label");
}

void NoSuccessor() {
    auto state = std::make_shared<State>();
    OpenOnExit release{state};
    QueueDraw(state);
    Await(state->entered, "unpaired draw never entered");
    state->Open();
    Idle();
    Require(state->retainedDepth == 1, "unpaired draw retained the GPU mutex");
    const auto bytes = Bytes(8);
    QueueLabel(state, bytes, bytes);
    Await(state->completed, "later standalone label did not complete");
    Idle();
    Require(!state->consumed[0] && state->entryDepth[0] == 0, "later label consumed a stale lease");
}

void Fallback(bool enabled, std::string_view scenario) {
    auto state = std::make_shared<State>();
    OpenOnExit release{state};
    const bool nested = scenario == "nested";
    const bool draw = scenario != "dispatch";
    auto queued = Bytes(scenario == "wide" ? 12 : 8);
    auto actual = queued;
    if (scenario == "payload") actual[3] ^= std::byte{0xff};
    const auto kind = scenario == "store" ? DrawPipeline::CommitKind::Store : scenario == "other" ? DrawPipeline::CommitKind::Other : DrawPipeline::CommitKind::EndOfPipe;
    const bool mismatch = scenario == "payload" || scenario == "queue" || scenario == "address";
    QueueDraw(state, nested, draw);
    Await(state->entered, "fallback draw never entered");
    QueueLabel(state, queued, actual, 0, kind, scenario == "queue" ? 0x20 : 0,
               scenario == "address" ? 4 : 0, scenario == "resolve", scenario == "unaligned");
    state->Open();
    Await(state->completed, "fallback successor did not complete");
    Idle();
    Require(state->retainedDepth == (nested ? 2u : enabled && mismatch ? 2u : 1u), std::string(scenario) + ": unexpected retained depth");
    Require(!state->consumed[0] && !state->heldAfterConsume[0], std::string(scenario) + ": refused lease was not released");
    Require(state->entryDepth[0] == (enabled && mismatch ? 1u : 0u), std::string(scenario) + ": wrong successor inherited ownership");
    Require(state->labelDepth[0] == 1, std::string(scenario) + ": fallback acquired recursively");
    Require(state->order == std::vector<unsigned>{1, 2}, std::string(scenario) + ": fallback changed FIFO order");
    Require(std::equal(actual.begin(), actual.end(), state->memory.begin()), std::string(scenario) + ": fallback used queued bytes instead of caller bytes");
    if (scenario == "resolve") Require(state->resolveDepth == 0, "resolver ran with retained GPU ownership");
}

}

int main(int argc, char** argv) {
    try {
        ArchB::Switches::Initialize();
        ArchB::Recorder::Boundary(true, 0, ArchB::Recorder::Clock::now());
        const bool enabled = ArchB::Switches::Enabled(ArchB::Feature::FixLabelHandoff);
        const std::string_view mode = argc > 1 ? argv[1] : "off";
        Require(enabled == (mode != "off"), "requested mode differs from the feature switch");
        OwnershipAndWaiter(enabled, 4);
        OwnershipAndWaiter(enabled, 8);
        NoSuccessor();
        for (const auto scenario : {"store", "other", "wide", "unaligned", "resolve", "nested", "dispatch", "payload", "queue", "address"}) Fallback(enabled, scenario);
        const auto snapshot = ArchB::Recorder::Collect(ArchB::Recorder::Clock::now());
        const auto count = [&](const char* name) {
            const auto found = snapshot.counters.find(name);
            return found == snapshot.counters.end() ? 0.0 : found->second;
        };
        Require(count("label_handoff_consumed") == (enabled ? 2.0 : 0.0), "handoff consumption coverage differs from expected");
        Require(count("label_handoff_plan_verified") == (mode == "verify" ? 2.0 : 0.0), "VERIFY was not exercised exclusively in verify mode");
        DrawPipeline::Queue0().Stop();
        GpuLock lock(GuestMemory::GpuMutex(), std::try_to_lock);
        Require(lock.owns_lock(), "stopped pipeline retained GPU ownership");
        std::cout << "label handoff " << mode << ": ownership, FIFO, 4/8-byte labels, cross-thread waiter and fallback checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        DrawPipeline::Queue0().Stop();
        return 1;
    }
}
