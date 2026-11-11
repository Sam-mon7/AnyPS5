#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>
#include <memory>
#include <stdexcept>

namespace {

using AgcDriver::DriverDetail::DrawFinisher;
using AgcDriver::DriverDetail::DrawPipeline;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void RetiredItemLeavesTheEnqueuingThread() {
    auto& pipeline = DrawPipeline::Queue0();
    auto held = std::make_shared<int>(1);
    const std::weak_ptr<int> watched = held;
    pipeline.Enqueue([held = std::move(held)] {}, {});
    pipeline.Drain(DrawPipeline::DrainReason::Submission);
    pipeline.Enqueue([] {}, {});
    Require(watched.expired(), "a retired draw pipeline item stayed in the enqueuing thread's storage after Enqueue returned");
    pipeline.Drain(DrawPipeline::DrainReason::Submission);
}

void RanJobLeavesTheEnqueuingThread() {
    auto& finisher = DrawFinisher::Get();
    auto held = std::make_shared<int>(1);
    const std::weak_ptr<int> watched = held;
    finisher.Enqueue([held = std::move(held)] {});
    finisher.Drain();
    finisher.Enqueue([] {});
    Require(watched.expired(), "a finished draw finisher job stayed in the enqueuing thread's storage after Enqueue returned");
    finisher.Drain();
}

struct Lifetime {
    std::mutex mutex;
    std::condition_variable changed;
    std::thread::id producer = std::this_thread::get_id();
    std::thread::id committer;
    std::array<unsigned, 64> destroyed{};
    std::size_t count = 0;
    std::size_t commits = 0;
    bool wrongThread = false;
    bool wrongOrder = false;
    bool release = false;
};

struct Probe {
    Lifetime& lifetime;
    std::size_t index;
    Probe(Lifetime& lifetime, std::size_t index) : lifetime(lifetime), index(index) {}
    ~Probe() {
        static_cast<void>(DrawPipeline::QueuedItems());
        std::lock_guard lock(lifetime.mutex);
        ++lifetime.destroyed[index];
        lifetime.wrongThread |= std::this_thread::get_id() == lifetime.producer || std::this_thread::get_id() == lifetime.committer || AgcDriver::GuestMemory::GpuMutex().HeldByThisThread();
        lifetime.wrongOrder |= index != lifetime.count;
        ++lifetime.count;
        lifetime.changed.notify_all();
    }
};

void AsyncItemsDrain(bool fail) {
    Require(AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::PipeAsyncReclaim), "asynchronous retirement switch is off");
    auto& pipeline = DrawPipeline::Queue0();
    Lifetime lifetime;
    using R = AgcDriver::ArchB::Recorder;
    R::Boundary(true, 1, R::Clock::now());
    const std::size_t count = fail ? 16 : lifetime.destroyed.size();
    const auto enqueue = [&](std::size_t i) {
        auto held = std::make_shared<Probe>(lifetime, i);
        auto resolve = std::make_shared<DrawPipeline::ResolveSlot>();
        resolve->run = [held](std::vector<DrawPipeline::Range>&) { return held->index; };
        pipeline.Enqueue([held = std::move(held), fail] {
            auto& lifetime = held->lifetime;
            std::unique_lock lock(lifetime.mutex);
            lifetime.committer = std::this_thread::get_id();
            ++lifetime.commits;
            if (fail) {
                lifetime.changed.wait(lock, [&] { return lifetime.release; });
                throw std::runtime_error("retirement test commit failure");
            }
        }, {}, 0, {}, {}, false, DrawPipeline::ItemKind::Other, std::move(resolve));
    };
    const auto first = fail ? count : count / 2;
    for (std::size_t i = 0; i < first; ++i) enqueue(i);
    if (fail) {
        {
            std::lock_guard lock(lifetime.mutex);
            lifetime.release = true;
        }
        lifetime.changed.notify_all();
    } else {
        pipeline.Drain(DrawPipeline::DrainReason::Submission);
        std::unique_lock lock(lifetime.mutex);
        Require(lifetime.changed.wait_for(lock, std::chrono::seconds(5), [&] { return lifetime.count == first; }), "retired items needed another enqueue before asynchronous destruction");
    }
    for (std::size_t i = first; i < count; ++i) enqueue(i);
    pipeline.Stop();
    pipeline.ReleaseRetired();
    Require(lifetime.commits == (fail ? 1 : count), "commit failure did not cancel the remaining items");
    Require(lifetime.count == count, "shutdown did not drain every retired item");
    Require(!lifetime.wrongThread, "retired item destroyed on the producer, committer, or under the GPU mutex");
    Require(!lifetime.wrongOrder, "retired items destroyed out of FIFO order");
    const auto counters = R::Collect(R::Clock::now()).counters;
    const auto verified = counters.find("r12_reclaim_verified");
    const auto mismatches = counters.find("r12_reclaim_verify_mismatches");
    Require(verified != counters.end() && verified->second == count && (mismatches == counters.end() || mismatches->second == 0), "reclaim lifetime verification missed an item or found an unsafe retirement");
    for (std::size_t i = 0; i < count; ++i) Require(lifetime.destroyed[i] == 1, "retired item was not destroyed exactly once");
}

}

int main(int argc, char** argv) {
    try {
        if (DrawPipeline::Depth() == 0) return 77;
        if (argc == 2) {
            AsyncItemsDrain(std::string_view(argv[1]) == "--failure");
            std::cout << "asynchronous retired items drain exactly once off the critical threads\n";
            return 0;
        }
        if (!AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::PipeAsyncReclaim)) RetiredItemLeavesTheEnqueuingThread();
        RanJobLeavesTheEnqueuingThread();
        std::cout << "retired draw pipeline items tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
