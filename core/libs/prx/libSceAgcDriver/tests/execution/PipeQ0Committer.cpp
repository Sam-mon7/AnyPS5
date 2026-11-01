// Slice A1 of Stage 2 (APS5_ARCHB_PIPE_Q0_COMMITTER):
//   --locked      (also the default) the two former throw paths: Enqueue into a full pipeline and Drain while the calling
//                 thread holds the GPU mutex (also for the draw finisher). Neither may throw or hang;
//                 items commit in order and the caller's hold is back at its depth afterwards.
//   --sample-on   the switch alone pipelines queue 0 (depth 8, dispatch items).
//   --sample-off  neither the switch nor APS5_PIPELINED_DRAWS: unpipelined.
//   --sample-env  APS5_PIPELINED_DRAWS=0 with the switch on: the variable decides alone.
//   --transition  a runtime toggle off drains what pipelined submissions left queued before the next
//                 submission runs unpipelined.
//   --keep-registers   (with APS5_ARCHB_PIPE_KEEP_INDIRECT_REGS=1) a SET_CONTEXT_REG_INDIRECT whose table a
//                 queued item writes keeps the pipeline: its valid list was read at submission.
//   --registers-drain  (without it) the same packet drains, as before the cut.
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

using AgcDriver::DriverDetail::DrawFinisher;
using AgcDriver::DriverDetail::DrawPipeline;
namespace GuestMemory = AgcDriver::GuestMemory;

void check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("pipe q0 committer: " + what);
}

void setEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value == nullptr ? "" : value);
#else
    if (value == nullptr) unsetenv(name);
    else setenv(name, value, 1);
#endif
}

// Items that take the GPU mutex, like commits do: the committer blocks on the first one while this
// thread holds the mutex, so the pipeline fills and a drain cannot finish under the hold.
void LockedWaits() {
    auto& gpu = GuestMemory::GpuMutex();
    std::mutex orderMutex;
    std::vector<std::size_t> order;
    const auto item = [&](std::size_t index) {
        return [&, index] {
            std::lock_guard locked(gpu);
            std::lock_guard guard(orderMutex);
            order.push_back(index);
        };
    };
    const auto count = DrawPipeline::Capacity() + 4;
    const auto overcommitsBefore = DrawPipeline::FullOvercommits();
    const auto lockedBefore = DrawPipeline::LockedDrains();
    {
        std::lock_guard outer(gpu);
        std::lock_guard inner(gpu);
        check(gpu.DepthOnThisThread() == 2, "test setup: the GPU mutex is not held twice");
        for (std::size_t i = 0; i < count; ++i) DrawPipeline::Queue0().Enqueue(item(i), {});
        check(DrawPipeline::FullOvercommits() > overcommitsBefore, "a full pipeline under the GPU mutex was not taken past its depth");
        DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
        check(DrawPipeline::LockedDrains() == lockedBefore + 1, "a drain under the GPU mutex was not counted");
        check(!DrawPipeline::Queue0().Busy(), "the drain under the GPU mutex returned with items queued");
        check(gpu.DepthOnThisThread() == 2, "the drain under the GPU mutex did not restore the hold depth");
    }
    check(order.size() == count, "not every item committed");
    for (std::size_t i = 0; i < order.size(); ++i) check(order[i] == i, "items committed out of order");

    // The draw finisher (APS5_DRAW_PIPELINE) had the same throws; its depth is 16.
    order.clear();
    const auto finisherJobs = std::size_t{20};
    {
        std::lock_guard outer(gpu);
        for (std::size_t i = 0; i < finisherJobs; ++i) DrawFinisher::Get().Enqueue(item(i));
        DrawFinisher::Get().Drain();
        check(!DrawFinisher::Busy(), "the finisher drain under the GPU mutex returned with jobs queued");
        check(gpu.DepthOnThisThread() == 1, "the finisher drain did not restore the hold depth");
        for (std::size_t i = 0; i < 4; ++i) DrawFinisher::Get().Enqueue(item(finisherJobs + i));
        DrawFinisher::Get().Settle();
        check(gpu.DepthOnThisThread() == 1, "the finisher settle did not restore the hold depth");
    }
    check(order.size() == finisherJobs + 4, "not every finisher job ran");
    for (std::size_t i = 0; i < order.size(); ++i) check(order[i] == i, "finisher jobs ran out of order");
    std::cout << "locked enqueue and drains: no throw, in order\n";
}

void Sample(std::string_view mode) {
    bool viaSwitch = false;
    const bool pipelined = DrawPipeline::SampleQueue0(viaSwitch);
    if (mode == "--sample-on") {
        check(pipelined && viaSwitch, "the switch did not pipeline queue 0");
        check(DrawPipeline::Depth() == 0 && DrawPipeline::Capacity() == 8, "the switch did not select depth 8");
    } else if (mode == "--sample-off") {
        check(!pipelined && !viaSwitch, "queue 0 pipelined with neither the switch nor APS5_PIPELINED_DRAWS");
    } else {
        check(!pipelined && !viaSwitch, "the switch overrode APS5_PIPELINED_DRAWS=0");
    }
    std::cout << "sampled " << mode << '\n';
}

std::array<std::uint32_t, 7> DmaData(bool immediate, std::uint64_t source, std::uint32_t data, std::uint64_t destination, std::uint32_t bytes) {
    return {0xc0055000u, immediate ? (2u << 29u) : 0u, immediate ? data : static_cast<std::uint32_t>(source), immediate ? 0u : static_cast<std::uint32_t>(source >> 32u), static_cast<std::uint32_t>(destination), static_cast<std::uint32_t>(destination >> 32u), bytes};
}

void Submit(std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    check(sceAgcDriverSubmitDcb(&packet) == 0, "submission failed");
}

void Transition() {
    using AgcDriver::ArchB::Feature;
    using AgcDriver::ArchB::Switches;
    const auto path = std::filesystem::temp_directory_path() / ("pipe-q0-control-" + std::to_string(getpid()) + ".txt");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove(path); }
    } cleanup{path};
    setEnvironment("APS5_PIPELINED_DRAWS", nullptr);
    setEnvironment("APS5_ARCHB_GATE_RUN", "1");
    setEnvironment("APS5_ARCHB_GATE_DELAY_US", "0");
    setEnvironment("APS5_ARCHB_PIPE_Q0_COMMITTER", "1");
    setEnvironment("APS5_ARCHB_CONTROL_FILE", path.string().c_str());
    Switches::Initialize();
    check(Switches::Enabled(Feature::PipeQ0Committer), "the switch did not start on");

    alignas(256) static std::array<std::uint32_t, 64> first{};
    alignas(256) static std::array<std::uint32_t, 64> second{};
    first.fill(0);
    second.fill(0);
    const auto firstAddress = reinterpret_cast<std::uintptr_t>(first.data());
    const auto secondAddress = reinterpret_cast<std::uintptr_t>(second.data());

    // A held item keeps the pipelined submission's DMA store (and its deferred completion) queued.
    auto release = std::make_shared<std::atomic<bool>>(false);
    struct Release {
        std::shared_ptr<std::atomic<bool>> flag;
        ~Release() { flag->store(true, std::memory_order_release); }
    } releaseAtExit{release};
    DrawPipeline::Queue0().Enqueue([release] {
        while (!release->load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }, {});
    std::vector<std::uint32_t> words;
    const auto store = DmaData(true, 0, 0x7a11c0deu, firstAddress, static_cast<std::uint32_t>(sizeof(first)));
    words.assign(store.begin(), store.end());
    Submit(words);
    const auto queuedBy = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (DrawPipeline::Queue0().Outstanding() < 2 && std::chrono::steady_clock::now() < queuedBy) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(DrawPipeline::Queue0().Outstanding() >= 2, "the pipelined submission did not queue its store");
    check(first[0] == 0, "the queued store landed while held");

    std::ofstream(path, std::ios::binary | std::ios::trunc) << "1 PIPE_Q0_COMMITTER=0\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    Switches::PollControl();
    check(!Switches::Enabled(Feature::PipeQ0Committer), "the switch did not turn off");

    // The next submission copies what the queued store writes; unpipelined it would read 0s unless
    // the transition drain commits the queued items first.
    const auto drainsBefore = DrawPipeline::DrainRequestsByOpcode(0x100);
    std::thread releaser([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (DrawPipeline::DrainRequestsByOpcode(0x100) == drainsBefore && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        release->store(true, std::memory_order_release);
    });
    const auto copy = DmaData(false, firstAddress, 0, secondAddress, static_cast<std::uint32_t>(sizeof(second)));
    words.assign(copy.begin(), copy.end());
    Submit(words);
    AgcDriverWaitIdle_nid_postfix();
    releaser.join();
    check(DrawPipeline::DrainRequestsByOpcode(0x100) > drainsBefore, "the unpipelined submission did not drain the pipeline first");
    for (const auto word : first) check(word == 0x7a11c0deu, "the queued store did not land");
    for (const auto word : second) check(word == 0x7a11c0deu, "the unpipelined copy ran before the queued store committed");
    check(!DrawPipeline::Queue0().Busy(), "items stayed queued after the transition");
    std::cout << "transition drained\n";
}

void Registers(bool keep) {
    using AgcDriver::ArchB::Feature;
    using AgcDriver::ArchB::Switches;
    check(Switches::Enabled(Feature::PipeQ0Committer) && Switches::Enabled(Feature::PipeKeepIndirectRegs) == keep, "unexpected switch environment");
    // One valid offset/value pair (CB_TARGET_MASK); a held item claims to write the table.
    alignas(256) static std::array<std::uint32_t, 2> table{0x8eu, 0xfu};
    const auto address = reinterpret_cast<std::uintptr_t>(table.data());
    auto release = std::make_shared<std::atomic<bool>>(false);
    struct Release {
        std::shared_ptr<std::atomic<bool>> flag;
        ~Release() { flag->store(true, std::memory_order_release); }
    } releaseAtExit{release};
    DrawPipeline::Queue0().Enqueue([release] {
        while (!release->load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }, {{address, address + sizeof(table)}});
    const auto before = DrawPipeline::DrainRequestsByOpcode(0x9f);
    std::vector<std::uint32_t> words{0xc0039f00u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 0x80000000u, 1u};
    std::thread releaser([&] {
        // Kept: the submission ends with its completion queued behind the held item. Drained: the
        // packet asks for the drain, which waits for the held item.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (DrawPipeline::DrainRequestsByOpcode(0x9f) != before) break;
            if (keep && DrawPipeline::Queue0().Outstanding() >= 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        release->store(true, std::memory_order_release);
    });
    Submit(words);
    releaser.join();
    AgcDriverWaitIdle_nid_postfix();
    const auto requests = DrawPipeline::DrainRequestsByOpcode(0x9f) - before;
    if (keep) check(requests == 0, "SET_CONTEXT_REG_INDIRECT drained although its list was read at submission");
    else check(requests == 1, "SET_CONTEXT_REG_INDIRECT over queued writes did not drain without the cut");
    std::cout << (keep ? "register table kept the pipeline" : "register table drained the pipeline") << '\n';
}

}

int main(int argc, char** argv) {
    try {
        const auto mode = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
        if (mode.empty() || mode == "--locked") LockedWaits();
        else if (mode.starts_with("--sample")) Sample(mode);
        else if (mode == "--transition") Transition();
        else if (mode == "--keep-registers") Registers(true);
        else if (mode == "--registers-drain") Registers(false);
        else throw std::runtime_error("unknown mode");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
