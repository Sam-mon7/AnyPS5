#include "prx/libc/include/CriticalPathTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using AgcDriver::DriverDetail::DrawPipeline;
using namespace std::chrono_literals;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Gates {
    std::array<std::counting_semaphore<8>, 4> ready{std::counting_semaphore<8>{0}, std::counting_semaphore<8>{0}, std::counting_semaphore<8>{0}, std::counting_semaphore<8>{0}};
    std::array<std::counting_semaphore<8>, 4> release{std::counting_semaphore<8>{0}, std::counting_semaphore<8>{0}, std::counting_semaphore<8>{0}, std::counting_semaphore<8>{0}};
    std::atomic<unsigned> committed{0};
};

void Full(bool hysteresis) {
    Require(DrawPipeline::Capacity() == 4, "capacity fixture requires four slots");
    Require(AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::PipeFullHysteresis) == hysteresis, "unexpected capacity hysteresis");
    auto& pipeline = DrawPipeline::Queue0();
    auto gates = std::make_shared<Gates>();
    constexpr std::array kinds{DrawPipeline::CommitKind::Dispatch, DrawPipeline::CommitKind::Store, DrawPipeline::CommitKind::DmaCopy, DrawPipeline::CommitKind::BufferFill};
    for (std::size_t index = 0; index < kinds.size(); ++index) {
        pipeline.Enqueue(kinds[index], [gates, index] {
            gates->ready[index].release();
            gates->release[index].acquire();
            gates->committed.fetch_add(1, std::memory_order_relaxed);
        }, {});
    }
    std::binary_semaphore started{0}, finished{0};
    std::exception_ptr producerError;
    std::thread producer([&] {
        CriticalPath::Name("capacity producer");
        started.release();
        try {
            pipeline.Enqueue(DrawPipeline::CommitKind::Draw, [gates] { gates->committed.fetch_add(1, std::memory_order_relaxed); }, {}, 0, {}, {}, true);
        } catch (...) {
            producerError = std::current_exception();
        }
        finished.release();
    });
    const bool producerStarted = started.try_acquire_for(5s);
    std::this_thread::sleep_for(40ms);
    const bool returnedEarly = finished.try_acquire();
    bool entered = true;
    const std::size_t target = hysteresis ? 1 : 0;
    for (std::size_t index = 0; index <= target; ++index) {
        entered = gates->ready[index].try_acquire_for(5s) && entered;
        gates->release[index].release();
    }
    const bool nextBlocked = gates->ready[target + 1].try_acquire_for(5s);
    const bool returnedAtTarget = returnedEarly || finished.try_acquire_for(5s);
    for (auto& gate : gates->release) gate.release();
    producer.join();
    pipeline.Drain(DrawPipeline::DrainReason::Submission);
    if (producerError) std::rethrow_exception(producerError);
    Require(producerStarted && entered && nextBlocked, "controlled queue did not reach its expected items");
    Require(!returnedEarly && returnedAtTarget, "capacity enqueue did not wait for its release threshold");
    Require(gates->committed.load(std::memory_order_relaxed) == 5, "capacity fixture changed the commit count");
}

void Queued() {
    auto& pipeline = DrawPipeline::Queue0();
    auto gates = std::make_shared<Gates>();
    pipeline.Enqueue(DrawPipeline::CommitKind::Draw, [gates] {
        gates->ready[0].release();
        gates->release[0].acquire();
        gates->committed.fetch_add(1, std::memory_order_relaxed);
    }, {}, 0, {}, {}, true);
    pipeline.Enqueue(DrawPipeline::CommitKind::Store, [gates] { gates->committed.fetch_add(1, std::memory_order_relaxed); }, {});
    pipeline.Enqueue(DrawPipeline::CommitKind::DmaCopy, [gates] { gates->committed.fetch_add(1, std::memory_order_relaxed); }, {});
    const bool entered = gates->ready[0].try_acquire_for(5s);
    gates->release[0].release();
    pipeline.Drain(DrawPipeline::DrainReason::Submission);
    Require(entered && gates->committed.load(std::memory_order_relaxed) == 3, "queued fixture lost a commit");
}

void Overcommit() {
    auto& pipeline = DrawPipeline::Queue0();
    std::atomic<unsigned> committed{0};
    const auto before = DrawPipeline::FullOvercommits();
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        for (unsigned index = 0; index < 5; ++index) {
            pipeline.Enqueue(DrawPipeline::CommitKind::Dispatch, [&] {
                std::lock_guard commitLock(AgcDriver::GuestMemory::GpuMutex());
                committed.fetch_add(1, std::memory_order_relaxed);
            }, {});
        }
    }
    pipeline.Drain(DrawPipeline::DrainReason::Submission);
    Require(DrawPipeline::FullOvercommits() == before + 1 && committed.load(std::memory_order_relaxed) == 5, "GPU-mutex overcommit did not preserve all five items");
}

void Idle(bool stop) {
    auto& pipeline = DrawPipeline::Queue0();
    pipeline.Enqueue(DrawPipeline::CommitKind::Draw, [] {}, {}, 0, {}, {}, true);
    pipeline.Drain(DrawPipeline::DrainReason::Submission);
    std::this_thread::sleep_for(40ms);
    if (!stop) {
        pipeline.Enqueue(DrawPipeline::CommitKind::Dispatch, [] {}, {});
        pipeline.Drain(DrawPipeline::DrainReason::Submission);
    }
}
}

int main(int argc, char** argv) {
    try {
        const auto mode = argc > 1 ? std::string_view(argv[1]) : std::string_view{"off"};
        Require(DrawPipeline::Capacity() == 4, "critical path fixture requires four slots");
        Require(CriticalPath::Enabled() == (mode != "off"), "unexpected critical path configuration");
        CriticalPath::Name("pipe fixture");
        CriticalPath::Present(0);
        if (mode == "full" || mode == "hysteresis") Full(mode == "hysteresis");
        else if (mode == "no-block" || mode == "off") Queued();
        else if (mode == "overcommit") Overcommit();
        else if (mode == "idle" || mode == "stop") Idle(mode == "stop");
        else throw std::runtime_error("unknown critical path fixture mode");
        DrawPipeline::Queue0().Stop();
        CriticalPath::Present(1);
        CriticalPath::Shutdown();
        std::cout << "draw pipeline critical path " << mode << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
