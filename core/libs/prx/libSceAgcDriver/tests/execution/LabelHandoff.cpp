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
#include <utility>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DrawPipeline;
namespace GuestMemory = AgcDriver::GuestMemory;
namespace ArchB = AgcDriver::ArchB;
using Clock = std::chrono::steady_clock;
using GpuLock = std::unique_lock<GuestMemory::GpuMutexType>;
using CommitKind = DrawPipeline::CommitKind;
using ItemKind = DrawPipeline::ItemKind;
constexpr auto Timeout = std::chrono::seconds(5);
constexpr std::uint64_t PriorWork = 0x123456789abcdef0ull;
constexpr unsigned LabelCount = 16;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("label handoff: " + message);
}

std::chrono::nanoseconds DelayPastLimit() {
    const auto start = Clock::now();
    const auto deadline = start + DrawPipeline::LabelHandoffMaxSpan * 2;
    auto now = start;
    do {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        now = Clock::now();
    } while (now < deadline);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now - start);
}
template<std::ptrdiff_t TLimit>
void Await(std::counting_semaphore<TLimit>& signal, const char* message) {
    Require(signal.try_acquire_for(Timeout), message);
}

struct State {
    std::binary_semaphore entered{0}, release{0}, waiterStarted{0}, waiterCompleted{0};
    std::counting_semaphore<LabelCount> completed{0};
    std::atomic<bool> opened{false};
    alignas(8) std::array<std::byte, LabelCount * 16> memory{};
    std::vector<unsigned> order;
    std::array<bool, LabelCount> consumed{}, heldAfterConsume{};
    std::array<std::uint32_t, LabelCount> labelDepth{}, entryDepth{}, retainedDepth{};
    std::array<std::uint64_t, LabelCount> holdSerial{}, reacquiredSerial{};
    std::array<std::chrono::nanoseconds, LabelCount> delayElapsed{};
    std::uint32_t sourceDepth = 0, sourceRetainedDepth = 0, cleanupDepth = 0, resolveDepth = 99;
    std::uint64_t sourceSerial = 0, work = 0;
    bool waiterBlocked = false, waiterSawLabel = false, waiterSawWork = false, waiterSawNext = false;

    State() { order.reserve(LabelCount + 1); }
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(memory.data()); }
    void Open() {
        if (!opened.exchange(true, std::memory_order_acq_rel)) release.release();
    }
};

struct OpenOnExit {
    std::shared_ptr<State> state;
    ~OpenOnExit() { state->Open(); }
};

struct LabelOptions {
    CommitKind kind = CommitKind::EndOfPipe;
    std::uint32_t queue = 0;
    std::uint64_t addressDelta = 0;
    bool resolve = false, unaligned = false, delayConsume = false, delayRetain = false;
    bool reacquire = false, appendLate = false, startWaiter = false, waiterBoundary = false, afterConsumeWaiter = false;
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

void QueueSource(const std::shared_ptr<State>& state, CommitKind kind = CommitKind::Draw,
                 ItemKind item = ItemKind::Draw, bool nested = false, bool waitAfterRetain = false) {
    DrawPipeline::Queue0().Enqueue(kind, [state, nested, waitAfterRetain] {
        auto& gpu = GuestMemory::GpuMutex();
        std::lock_guard lock(gpu);
        GpuLock inner(gpu, std::defer_lock);
        if (nested) inner.lock();
        state->sourceDepth = gpu.DepthOnThisThread();
        state->sourceSerial = GuestMemory::GpuHoldSerial();
        state->work = PriorWork;
        state->order.push_back(1);
        state->entered.release();
        state->release.acquire();
        DrawPipeline::RetainGpuMutexForLabel();
        state->sourceRetainedDepth = gpu.DepthOnThisThread();
        DrawPipeline::RetainGpuMutexForLabel();
        Require(gpu.DepthOnThisThread() == state->sourceRetainedDepth, "repeated source retention added a second lease");
        if (waitAfterRetain) {
            state->waiterStarted.release();
            const auto deadline = Clock::now() + Timeout;
            while (!gpu.IsContended() && Clock::now() < deadline) std::this_thread::yield();
            Require(gpu.IsContended(), "waiter never contended with the retained source");
        }
    }, {}, 0, {}, {}, item == ItemKind::Draw, item);
}

void QueueLabel(const std::shared_ptr<State>& state, std::vector<std::byte> queued, std::vector<std::byte> actual,
                unsigned index = 0, LabelOptions options = {}) {
    const auto address = state->Address() + index * 16 + (options.unaligned ? 1 : 0);
    std::shared_ptr<DrawPipeline::ResolveSlot> slot;
    if (options.resolve) {
        slot = std::make_shared<DrawPipeline::ResolveSlot>();
        slot->run = [state](std::vector<DrawPipeline::Range>&) {
            state->resolveDepth = GuestMemory::GpuMutex().DepthOnThisThread();
            return std::uint64_t{1};
        };
    }
    const auto length = queued.size();
    DrawPipeline::Queue0().Enqueue(options.kind, [state, actual = std::move(actual), index, address, options] {
        auto& gpu = GuestMemory::GpuMutex();
        state->entryDepth[index] = gpu.DepthOnThisThread();
        if (options.waiterBoundary) {
            Require(state->entryDepth[index] == 0, "contended run reached the next label with retained ownership");
            Await(state->waiterCompleted, "other-queue waiter did not acquire between labels");
        }
        if (options.delayConsume) state->delayElapsed[index] = DelayPastLimit();
        state->consumed[index] = DrawPipeline::ConsumeLabelGpuMutex(options.queue, address + options.addressDelta, actual);
        state->heldAfterConsume[index] = gpu.HeldByThisThread();
        if (options.afterConsumeWaiter) {
            Require(!state->heldAfterConsume[index], "consume retained ownership in front of a blocked waiter");
            Await(state->waiterCompleted, "consume did not release ownership to the blocked waiter");
        }
        {
            GpuLock lock(gpu, std::defer_lock);
            if (state->heldAfterConsume[index]) lock = GpuLock(gpu, std::adopt_lock);
            else lock.lock();
            state->labelDepth[index] = gpu.DepthOnThisThread();
            state->holdSerial[index] = GuestMemory::GpuHoldSerial();
            state->order.push_back(index + 2);
            if (!actual.empty()) std::memcpy(state->memory.data() + index * 16, actual.data(), actual.size());
            if (options.startWaiter) {
                state->waiterStarted.release();
                const auto deadline = Clock::now() + Timeout;
                while (!gpu.IsContended() && Clock::now() < deadline) std::this_thread::yield();
                Require(gpu.IsContended(), "other-queue waiter never blocked on the GPU mutex");
            }
            if (options.delayRetain) state->delayElapsed[index] = DelayPastLimit();
            if (options.reacquire) {
                lock.unlock();
                lock.lock();
                state->reacquiredSerial[index] = GuestMemory::GpuHoldSerial();
            }
            if (options.appendLate) {
                const auto late = Bytes(8, 0x71);
                QueueLabel(state, late, late, index + 1);
            }
            DrawPipeline::RetainGpuMutexForLabel();
            state->retainedDepth[index] = gpu.DepthOnThisThread();
            DrawPipeline::RetainGpuMutexForLabel();
            Require(gpu.DepthOnThisThread() == state->retainedDepth[index], "repeated label retention added a second lease");
        }
        state->completed.release();
    }, {{address, address + length}}, address, std::move(queued), {}, false,
       options.kind == CommitKind::Other ? ItemKind::Other : ItemKind::Label, std::move(slot));
}

void Finish(const std::shared_ptr<State>& state, unsigned count) {
    state->Open();
    for (unsigned index = 0; index < count; ++index) Await(state->completed, "label did not complete");
    Idle();
    std::vector<unsigned> expected{1};
    for (unsigned index = 0; index < count; ++index) {
        expected.push_back(index + 2);
        Require(state->labelDepth[index] == 1, "label work ran under a recursive hold");
        Require(state->heldAfterConsume[index] == state->consumed[index], "consume did not transfer exact ownership");
        Require(state->retainedDepth[index] == 1 || state->retainedDepth[index] == 2, "label retained the wrong depth");
    }
    Require(state->order == expected, "source and labels did not execute in FIFO order");
}

void CheckBytes(const std::shared_ptr<State>& state, unsigned index, const std::vector<std::byte>& expected) {
    Require(std::equal(expected.begin(), expected.end(), state->memory.begin() + index * 16), "label bytes differ from the original path");
}

void BoundedRun(bool enabled, std::size_t size) {
    constexpr unsigned count = DrawPipeline::LabelHandoffMaxTransfers * 2 + 2;
    static_assert(count <= LabelCount);
    bool fullRun = !enabled;
    for (unsigned attempt = 0; attempt < (enabled ? 32u : 1u) && (!fullRun || attempt == 0); ++attempt) {
        auto state = std::make_shared<State>();
        OpenOnExit release{state};
        QueueSource(state);
        Await(state->entered, "bounded source never entered");
        for (unsigned index = 0; index < count; ++index) {
            const auto bytes = Bytes(size, 0x31 + index * 8);
            QueueLabel(state, bytes, bytes, index);
        }
        Finish(state, count);
        Require(state->sourceDepth == 1 && state->sourceRetainedDepth == (enabled ? 2u : 1u), "source retained the wrong depth");
        unsigned transfers = 0;
        auto serial = state->sourceSerial;
        for (unsigned index = 0; index < count; ++index) {
            CheckBytes(state, index, Bytes(size, 0x31 + index * 8));
            if (state->consumed[index]) {
                Require(enabled && state->entryDepth[index] == 1, "unexpected label lease");
                Require(state->holdSerial[index] == serial, "retained label crossed a real unlock");
                ++transfers;
                Require(transfers <= DrawPipeline::LabelHandoffMaxTransfers, "continuous ownership exceeded the transfer bound");
                fullRun |= transfers == DrawPipeline::LabelHandoffMaxTransfers;
            } else {
                Require(state->holdSerial[index] != serial, "fallback did not acquire a fresh outer hold");
                serial = state->holdSerial[index];
                transfers = 0;
            }
        }
        Require(state->retainedDepth[count - 1] == 1, "last label retained ownership into idle");
    }
    Require(fullRun, "no complete bounded label run was exercised");
}

void WaiterBetweenLabels(bool enabled) {
    auto state = std::make_shared<State>();
    OpenOnExit release{state};
    const auto first = Bytes(8), second = Bytes(4, 0x71);
    QueueSource(state);
    Await(state->entered, "waiter source never entered");
    std::jthread waiter([state, first] {
        GuestMemory::TagGpuLockThread(0x28);
        if (!state->waiterStarted.try_acquire_for(Timeout)) return;
        {
            GpuLock probe(GuestMemory::GpuMutex(), std::try_to_lock);
            state->waiterBlocked = !probe.owns_lock();
        }
        {
            GpuLock lock(GuestMemory::GpuMutex());
            state->waiterSawLabel = std::equal(first.begin(), first.end(), state->memory.begin());
            state->waiterSawWork = state->work == PriorWork;
            state->waiterSawNext = state->memory[16] != std::byte{0};
        }
        state->waiterCompleted.release();
    });
    LabelOptions firstOptions, secondOptions;
    firstOptions.startWaiter = true;
    secondOptions.waiterBoundary = true;
    QueueLabel(state, first, first, 0, firstOptions);
    QueueLabel(state, second, second, 1, secondOptions);
    Finish(state, 2);
    waiter.join();
    Require(state->sourceRetainedDepth == (enabled ? 2u : 1u), "waiter source did not exercise the selected path");
    Require(state->retainedDepth[0] == 1 && !state->consumed[1], "contended label retained ownership");
    Require(state->waiterBlocked && state->waiterSawLabel && state->waiterSawWork && !state->waiterSawNext,
            "other-queue waiter did not observe the exact boundary between labels");
    CheckBytes(state, 0, first);
    CheckBytes(state, 1, second);
}

void WaiterAtConsume(bool enabled) {
    auto state = std::make_shared<State>();
    OpenOnExit release{state};
    QueueSource(state, CommitKind::Draw, ItemKind::Draw, false, true);
    Await(state->entered, "contended-consume source never entered");
    std::jthread waiter([state] {
        GuestMemory::TagGpuLockThread(0x28);
        if (!state->waiterStarted.try_acquire_for(Timeout)) return;
        {
            GpuLock lock(GuestMemory::GpuMutex());
            state->waiterSawWork = state->work == PriorWork;
        }
        state->waiterCompleted.release();
    });
    LabelOptions options;
    options.afterConsumeWaiter = true;
    const auto bytes = Bytes(8);
    QueueLabel(state, bytes, bytes, 0, options);
    Finish(state, 1);
    waiter.join();
    Require(state->sourceRetainedDepth == (enabled ? 2u : 1u), "contended-consume source retained the wrong depth");
    Require(!state->consumed[0] && state->waiterSawWork, "contended consume did not give another queue ownership");
    CheckBytes(state, 0, bytes);
}

void FrozenRun(bool enabled) {
    bool exercised = !enabled;
    for (unsigned attempt = 0; attempt < (enabled ? 32u : 1u) && (!exercised || attempt == 0); ++attempt) {
        auto state = std::make_shared<State>();
        OpenOnExit release{state};
        QueueSource(state);
        Await(state->entered, "frozen-run source never entered");
        LabelOptions options;
        options.appendLate = true;
        const auto first = Bytes(4);
        QueueLabel(state, first, first, 0, options);
        Finish(state, 2);
        Require(state->sourceRetainedDepth == (enabled ? 2u : 1u), "frozen run had no initial retained snapshot");
        if (state->consumed[0]) {
            Require(state->retainedDepth[0] == 1 && !state->consumed[1], "a late enqueue extended the retained run");
            exercised = true;
        }
        CheckBytes(state, 0, first);
        CheckBytes(state, 1, Bytes(8, 0x71));
    }
    Require(exercised, "frozen-snapshot continuation was not exercised");
}
void ReacquireRun(bool enabled) {
    constexpr unsigned count = DrawPipeline::LabelHandoffMaxTransfers + 2;
    bool resumed = !enabled;
    for (unsigned attempt = 0; attempt < (enabled ? 32u : 1u) && (!resumed || attempt == 0); ++attempt) {
        auto state = std::make_shared<State>();
        OpenOnExit release{state};
        QueueSource(state);
        Await(state->entered, "reacquire source never entered");
        for (unsigned index = 0; index < count; ++index) {
            LabelOptions options;
            options.reacquire = index == DrawPipeline::LabelHandoffMaxTransfers - 1;
            const auto bytes = Bytes(4, 0x31 + index * 8);
            QueueLabel(state, bytes, bytes, index, options);
        }
        Finish(state, count);
        const auto boundary = DrawPipeline::LabelHandoffMaxTransfers - 1;
        Require(state->reacquiredSerial[boundary] != state->holdSerial[boundary], "runtime unlock/relock did not change the hold serial");
        if (state->consumed[boundary] && state->holdSerial[boundary] == state->sourceSerial && state->consumed[boundary + 1]) {
            Require(state->holdSerial[boundary + 1] == state->reacquiredSerial[boundary], "new batch did not use the reacquired outer hold");
            resumed = true;
        }
        if (!enabled) Require(std::none_of(state->consumed.begin(), state->consumed.end(), [](bool value) { return value; }), "disabled path retained after reacquisition");
    }
    Require(resumed, "runtime unlock/relock did not reset the bounded run budget");
}

void TimeLimit(bool enabled, bool consume) {
    bool exercised = !enabled;
    for (unsigned attempt = 0; attempt < (enabled ? 32u : 1u) && (!exercised || attempt == 0); ++attempt) {
        auto state = std::make_shared<State>();
        OpenOnExit release{state};
        QueueSource(state);
        Await(state->entered, "time-limit source never entered");
        LabelOptions options;
        options.delayConsume = consume;
        options.delayRetain = !consume;
        const auto bytes = Bytes(8);
        QueueLabel(state, bytes, bytes, 0, options);
        QueueLabel(state, bytes, bytes, 1);
        Finish(state, 2);
        Require(state->sourceRetainedDepth == (enabled ? 2u : 1u), "time-limit source retained the wrong depth");
        const auto elapsed = "; measured delay " + std::to_string(state->delayElapsed[0].count()) + " ns";
        Require(state->delayElapsed[0] >= DrawPipeline::LabelHandoffMaxSpan * 2, "fixture did not reach the expiry deadline" + elapsed);
        if (consume) {
            Require(!state->consumed[0] && !state->heldAfterConsume[0], "expired lease survived the consume check" + elapsed);
            exercised = true;
        } else if (state->consumed[0]) {
            Require(state->retainedDepth[0] == 1 && !state->consumed[1], "slow label tail extended an expired run" + elapsed);
            exercised = true;
        }
    }
    Require(exercised, "elapsed-time continuation limit was not exercised");
}
void TailUnwind() {
    auto state = std::make_shared<State>();
    OpenOnExit release{state};
    DrawPipeline::Queue0().Enqueue(CommitKind::Draw, [state] {
        struct Unwind {};
        auto& gpu = GuestMemory::GpuMutex();
        try {
            std::lock_guard lock(gpu);
            DrawPipeline::LabelHandoffTail tail;
            struct Cleanup {
                State& state;
                ~Cleanup() { state.cleanupDepth = GuestMemory::GpuMutex().DepthOnThisThread(); }
            } cleanup{*state};
            state->sourceDepth = gpu.DepthOnThisThread();
            state->sourceSerial = GuestMemory::GpuHoldSerial();
            state->work = PriorWork;
            state->order.push_back(1);
            state->entered.release();
            state->release.acquire();
            tail.armed = true;
            throw Unwind{};
        } catch (const Unwind&) {
            state->sourceRetainedDepth = gpu.DepthOnThisThread();
        }
    }, {}, 0, {}, {}, true, ItemKind::Draw);
    Await(state->entered, "unwinding source never entered");
    const auto bytes = Bytes(8);
    QueueLabel(state, bytes, bytes);
    Finish(state, 1);
    Require(state->sourceDepth == 1 && state->cleanupDepth == 1, "unwinding cleanup observed recursive ownership");
    Require(state->sourceRetainedDepth == 0, "armed tail retained ownership during exception unwinding");
    Require(!state->consumed[0] && state->entryDepth[0] == 0 && state->holdSerial[0] != state->sourceSerial,
            "successor inherited an unwinding source lease instead of acquiring a fresh hold");
    CheckBytes(state, 0, bytes);
}
void TailOrder(bool enabled) {
    for (const bool armed : {false, true}) {
        bool exercised = !enabled || !armed;
        for (unsigned attempt = 0; attempt < (exercised ? 1u : 32u) && (!exercised || attempt == 0); ++attempt) {
            auto state = std::make_shared<State>();
            OpenOnExit release{state};
            DrawPipeline::Queue0().Enqueue(CommitKind::Draw, [state, armed] {
                auto& gpu = GuestMemory::GpuMutex();
                std::lock_guard lock(gpu);
                DrawPipeline::LabelHandoffTail tail;
                struct Cleanup {
                    State& state;
                    ~Cleanup() { state.cleanupDepth = GuestMemory::GpuMutex().DepthOnThisThread(); }
                } cleanup{*state};
                state->sourceDepth = gpu.DepthOnThisThread();
                state->sourceSerial = GuestMemory::GpuHoldSerial();
                state->work = PriorWork;
                state->order.push_back(1);
                state->entered.release();
                state->release.acquire();
                tail.armed = armed;
                state->sourceRetainedDepth = gpu.DepthOnThisThread();
            }, {}, 0, {}, {}, true, ItemKind::Draw);
            Await(state->entered, "tail-order source never entered");
            const auto bytes = Bytes(8);
            QueueLabel(state, bytes, bytes);
            Finish(state, 1);
            Require(state->sourceDepth == 1 && state->sourceRetainedDepth == 1 && state->cleanupDepth == 1,
                    "tail retained ownership before later local cleanup finished");
            Require(state->entryDepth[0] == (enabled && armed ? 1u : 0u), "tail armed state did not control inherited ownership");
            if (state->consumed[0]) {
                Require(enabled && armed && state->holdSerial[0] == state->sourceSerial,
                        "tail handoff did not preserve the original outer hold");
                exercised = true;
            }
            CheckBytes(state, 0, bytes);
        }
        Require(exercised, "armed tail did not exercise ownership transfer after local cleanup");
    }
}
void SourceKinds(bool enabled) {
    const std::array sources{
        std::pair{CommitKind::Dispatch, ItemKind::Dispatch}, std::pair{CommitKind::DmaCopy, ItemKind::Dma},
        std::pair{CommitKind::DmaFill, ItemKind::Dma}, std::pair{CommitKind::DmaWrite, ItemKind::Dma},
        std::pair{CommitKind::Store, ItemKind::Label}, std::pair{CommitKind::BufferFill, ItemKind::Fill},
        std::pair{CommitKind::Seal, ItemKind::Other}
    };
    for (const auto& [kind, item] : sources) {
        bool exercised = !enabled;
        for (unsigned attempt = 0; attempt < (enabled ? 32u : 1u) && (!exercised || attempt == 0); ++attempt) {
            auto state = std::make_shared<State>();
            OpenOnExit release{state};
            QueueSource(state, kind, item);
            Await(state->entered, "audited source never entered");
            const auto bytes = Bytes(8);
            QueueLabel(state, bytes, bytes);
            Finish(state, 1);
            Require(state->sourceRetainedDepth == (enabled ? 2u : 1u), "audited source did not retain exactly one lease");
            if (state->consumed[0]) {
                Require(enabled && state->entryDepth[0] == 1 && state->holdSerial[0] == state->sourceSerial,
                        "audited source did not transfer its original outer hold");
                exercised = true;
            }
            CheckBytes(state, 0, bytes);
        }
        Require(exercised, "audited source lease did not survive item retirement for successor consumption");
    }
}
void UnconsumedLease(bool enabled) {
    auto state = std::make_shared<State>();
    OpenOnExit release{state};
    QueueSource(state);
    Await(state->entered, "unconsumed-lease source never entered");
    const auto bytes = Bytes(8);
    const auto address = state->Address();
    DrawPipeline::Queue0().Enqueue(CommitKind::EndOfPipe, [state] {
        state->entryDepth[0] = GuestMemory::GpuMutex().DepthOnThisThread();
        state->completed.release();
    }, {{address, address + bytes.size()}}, address, bytes, {}, false, ItemKind::Label);
    QueueLabel(state, bytes, bytes, 1);
    state->Open();
    Await(state->completed, "unused successor did not complete");
    Await(state->completed, "successor after unused lease did not complete");
    Idle();
    Require(state->sourceRetainedDepth == (enabled ? 2u : 1u), "unconsumed source retained the wrong depth");
    Require(!state->consumed[1] && state->entryDepth[1] == 0 && state->labelDepth[1] == 1,
            "unused lease escaped into a later label");
    CheckBytes(state, 1, bytes);
}

void NoSuccessor() {
    auto state = std::make_shared<State>();
    OpenOnExit release{state};
    QueueSource(state);
    Await(state->entered, "unpaired source never entered");
    state->Open();
    Idle();
    Require(state->sourceRetainedDepth == 1, "unpaired source retained the GPU mutex");
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
    auto queued = Bytes(scenario == "wide" ? 12 : scenario == "empty" ? 0 : 8);
    auto actual = queued;
    if (scenario == "payload") actual[3] ^= std::byte{0xff};
    LabelOptions options;
    options.kind = scenario == "store" ? CommitKind::Store : scenario == "other" ? CommitKind::Other : CommitKind::EndOfPipe;
    options.queue = scenario == "queue" ? 0x20 : 0;
    options.addressDelta = scenario == "address" ? 4 : 0;
    options.resolve = scenario == "resolve";
    options.unaligned = scenario == "unaligned";
    const bool mismatch = scenario == "payload" || scenario == "queue" || scenario == "address";
    QueueSource(state, CommitKind::Draw, ItemKind::Draw, nested);
    Await(state->entered, "fallback source never entered");
    QueueLabel(state, queued, actual, 0, options);
    Finish(state, 1);
    Require(state->sourceRetainedDepth == (nested ? 2u : enabled && mismatch ? 2u : 1u), std::string(scenario) + ": unexpected retained depth");
    Require(!state->consumed[0] && !state->heldAfterConsume[0], std::string(scenario) + ": refused lease was not released");
    Require(state->entryDepth[0] == (enabled && mismatch ? 1u : 0u), std::string(scenario) + ": wrong successor inherited ownership");
    CheckBytes(state, 0, actual);
    if (options.resolve) Require(state->resolveDepth == 0, "resolver ran with retained GPU ownership");
}

}

int main(int argc, char** argv) {
    try {
        ArchB::Switches::Initialize();
        ArchB::Recorder::Boundary(true, 0, ArchB::Recorder::Clock::now());
        const bool enabled = ArchB::Switches::Enabled(ArchB::Feature::FixLabelHandoff);
        const std::string_view mode = argc > 1 ? argv[1] : "off";
        Require(enabled == (mode != "off"), "requested mode differs from the feature switch");
        BoundedRun(enabled, 4);
        BoundedRun(enabled, 8);
        WaiterBetweenLabels(enabled);
        WaiterAtConsume(enabled);
        FrozenRun(enabled);
        ReacquireRun(enabled);
        TimeLimit(enabled, false);
        TimeLimit(enabled, true);
        TailUnwind();
        TailOrder(enabled);
        SourceKinds(enabled);
        UnconsumedLease(enabled);
        NoSuccessor();
        for (const auto scenario : {"store", "other", "wide", "empty", "unaligned", "resolve", "nested", "payload", "queue", "address"}) Fallback(enabled, scenario);
        const auto snapshot = ArchB::Recorder::Collect(ArchB::Recorder::Clock::now());
        const auto count = [&](const char* name) {
            const auto found = snapshot.counters.find(name);
            return found == snapshot.counters.end() ? 0.0 : found->second;
        };
        const auto consumed = count("label_handoff_consumed");
        Require((consumed > 0) == enabled, "handoff consumption coverage differs from expected");
        Require(count("label_handoff_plan_verified") == (mode == "verify" ? consumed : 0.0), "VERIFY did not check every consumed lease exclusively in verify mode");
        Require((count("label_handoff_run_verified") > 0) == (mode == "verify"), "VERIFY did not exercise bounded run planning");
        if (enabled) {
            Require(count("label_handoff_contended") > 0, "blocking waiter did not refuse tail retention");
            Require(count("label_handoff_contended_consume") > 0, "blocked waiter did not refuse consumption");
            Require(count("label_handoff_run_limit") > 0, "bounded or frozen run termination was not exercised");
            Require(count("label_handoff_time_limit") > 0, "elapsed-time tail limit was not exercised");
            Require(count("label_handoff_time_limit_consume") > 0, "elapsed-time consume limit was not exercised");
        }
        DrawPipeline::Queue0().Stop();
        GpuLock lock(GuestMemory::GpuMutex(), std::try_to_lock);
        Require(lock.owns_lock(), "stopped pipeline retained GPU ownership");
        std::cout << "label handoff " << mode << ": bounded runs, waiter between labels, frozen snapshots, reacquisition, time limits, audited sources and fallback checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        DrawPipeline::Queue0().Stop();
        return 1;
    }
}
