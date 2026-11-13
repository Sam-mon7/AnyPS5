// Slice A1 of Stage 2 (APS5_ARCHB_PIPE_Q0_COMMITTER):
//   --locked      (also the default) the two former throw paths: Enqueue into a full pipeline and Drain while the calling
//                 thread holds the GPU mutex (also for the draw finisher). Neither may throw or hang;
//                 items commit in order and the caller's hold is back at its depth afterwards.
//   --sample-on   the switch alone pipelines queue 0 (depth 8, dispatch items).
//   --sample-env  APS5_PIPELINED_DRAWS=0 with the switch on: the variable decides alone.
//   --keep-registers   (with APS5_ARCHB_PIPE_KEEP_INDIRECT_REGS=1) a SET_CONTEXT_REG_INDIRECT whose table a
//                 queued item writes keeps the pipeline: its valid list was read at submission.
//   --registers-drain  (without it) the same packet drains, as before the cut.
//   --resolve-split    (APS5_ARCHB_PIPE_RESOLVE_SPLIT=1) an item's resolve runs on the committer right
//                 before its commit, once.
//   --resolve-pool     (and _POOL=1) resolves of queued items behind a blocked front item run ahead on
//                 resolver threads; the commits then take them without re-running, in FIFO order.
//   --resolve-conflict (pool) an item applied after a resolve began that writes one of the resolve's
//                 ranges makes the committer re-run it inline (pipe_resolve_conflicts); a write
//                 elsewhere does not.
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
        check(DrawPipeline::Depth() == 8 && DrawPipeline::Capacity() == 8, "the switch did not select depth 8");
    } else if (mode == "--sample-off") {
        check(pipelined && !viaSwitch, "the disabled switch did not preserve the upstream queue 0 pipeline");
        check(DrawPipeline::Depth() == 8 && DrawPipeline::Capacity() == 8, "the upstream default did not select depth 8");
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
    bool viaSwitch = false;
    check(DrawPipeline::SampleQueue0(viaSwitch) && viaSwitch, "the enabled switch did not select the local pipeline mode");

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

    check(DrawPipeline::SampleQueue0(viaSwitch) && !viaSwitch, "turning the local switch off disabled the upstream pipeline");
    check(DrawPipeline::Depth() == 8 && DrawPipeline::Capacity() == 8, "turning the local switch off changed the upstream depth");
    const auto drainsBefore = DrawPipeline::DrainRequestsByOpcode(0x100);
    const auto queuedBefore = DrawPipeline::Queue0().Outstanding();
    std::atomic<bool> copyQueued{false};
    std::thread releaser([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (DrawPipeline::Queue0().Outstanding() <= queuedBefore && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        copyQueued.store(DrawPipeline::Queue0().Outstanding() > queuedBefore, std::memory_order_release);
        release->store(true, std::memory_order_release);
    });
    const auto copy = DmaData(false, firstAddress, 0, secondAddress, static_cast<std::uint32_t>(sizeof(second)));
    words.assign(copy.begin(), copy.end());
    Submit(words);
    AgcDriverWaitIdle_nid_postfix();
    releaser.join();
    check(copyQueued.load(std::memory_order_acquire), "the upstream pipeline did not queue the copy behind the held store");
    check(DrawPipeline::DrainRequestsByOpcode(0x100) == drainsBefore, "the local switch transition unnecessarily disabled and drained the upstream pipeline");
    for (const auto word : first) check(word == 0x7a11c0deu, "the queued store did not land");
    for (const auto word : second) check(word == 0x7a11c0deu, "the upstream-pipelined copy ran before the queued store committed");
    check(!DrawPipeline::Queue0().Busy(), "items stayed queued after the transition");
    std::cout << "transition preserved upstream pipeline ordering\n";
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

// Stage 4.1/4.2: one resolvable item. Its resolve notes the thread and reads `read`; its commit notes
// the order and that the resolve came first.
struct ResolveProbe {
    std::atomic<int> resolves{0};
    std::atomic<bool> resolvedOffCommitter{false};
    std::atomic<bool> lastResolveOnCommitter{false};
    std::atomic<bool> resolvedBeforeCommit{true};
    std::thread::id committer;
};

void Resolve(std::string_view mode) {
    auto& pipeline = DrawPipeline::Queue0();
    check(DrawPipeline::ResolveSplitActive(), "test setup: PIPE_RESOLVE_SPLIT is off");
    const bool pool = mode != "--resolve-split";
    check(DrawPipeline::ResolvePoolActive() == pool, "test setup: PIPE_RESOLVE_POOL does not match the mode");
    using Range = DrawPipeline::Range;
    std::mutex orderMutex;
    std::vector<int> order;
    std::atomic<bool> release{false};
    std::atomic<bool> blocking{false};
    std::thread::id committerId;
    const auto blocker = [&] {
        committerId = std::this_thread::get_id();
        blocking.store(true);
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::lock_guard guard(orderMutex);
        order.push_back(-1);
    };
    const auto plain = [&](int index) {
        return [&, index] {
            std::lock_guard guard(orderMutex);
            order.push_back(index);
        };
    };
    std::vector<std::shared_ptr<ResolveProbe>> probes;
    const auto resolvable = [&](int index, Range read) {
        auto probe = std::make_shared<ResolveProbe>();
        probes.push_back(probe);
        auto slot = std::make_shared<DrawPipeline::ResolveSlot>();
        slot->run = [probe, read, &committerId](std::vector<Range>& reads) -> std::uint64_t {
            const bool onCommitter = std::this_thread::get_id() == committerId;
            probe->lastResolveOnCommitter.store(onCommitter);
            if (!onCommitter) probe->resolvedOffCommitter.store(true);
            probe->resolves.fetch_add(1);
            reads.push_back(read);
            return 7;
        };
        auto commit = [&, index, probe] {
            if (probe->resolves.load() == 0) probe->resolvedBeforeCommit.store(false);
            std::lock_guard guard(orderMutex);
            order.push_back(index);
        };
        pipeline.Enqueue(commit, {}, 0, {}, {}, true, DrawPipeline::ItemKind::Draw, std::move(slot));
    };
    const auto waitResolved = [&](std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            std::size_t done = 0;
            for (std::size_t i = 0; i < count; ++i) done += probes[i]->resolves.load() != 0;
            if (done == count) return;
            check(std::chrono::steady_clock::now() < deadline, "queued resolves did not run ahead while the front item blocked");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    const Range target{0x100000, 0x101000};
    if (mode == "--resolve-split") {
        pipeline.Enqueue(blocker, {});
        while (!blocking.load()) std::this_thread::yield();
        for (int i = 0; i < 4; ++i) resolvable(i, target);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (const auto& probe : probes) check(probe->resolves.load() == 0, "a resolve ran before the committer reached its item with the pool off");
        release.store(true);
        pipeline.Drain(DrawPipeline::DrainReason::Flush);
        for (const auto& probe : probes) {
            check(probe->resolves.load() == 1, "a resolve did not run exactly once");
            check(!probe->resolvedOffCommitter.load(), "a resolve ran off the committer with the pool off");
            check(probe->resolvedBeforeCommit.load(), "a commit ran before its resolve");
        }
    } else if (mode == "--resolve-pool") {
        pipeline.Enqueue(blocker, {});
        while (!blocking.load()) std::this_thread::yield();
        for (int i = 0; i < 6; ++i) resolvable(i, target);
        waitResolved(probes.size());
        release.store(true);
        pipeline.Drain(DrawPipeline::DrainReason::Flush);
        for (const auto& probe : probes) {
            check(probe->resolves.load() == 1, "a resolve ran ahead was run again without a conflict");
            check(probe->resolvedOffCommitter.load(), "a queued resolve did not run on a resolver thread");
        }
        const auto text = DrawPipeline::ResolveText();
        check(text.find("pool 6") != std::string::npos, "pipe_resolve_pool_items did not count the six resolves: " + text);
    } else {
        const Range elsewhere{0x200000, 0x201000};
        pipeline.Enqueue(blocker, {});
        while (!blocking.load()) std::this_thread::yield();
        resolvable(0, target);
        resolvable(1, elsewhere);
        waitResolved(2);
        pipeline.Enqueue(plain(10), {target});
        resolvable(2, target);
        resolvable(3, elsewhere);
        waitResolved(4);
        release.store(true);
        pipeline.Drain(DrawPipeline::DrainReason::Flush);
        check(probes[0]->resolves.load() == 1 && probes[1]->resolves.load() == 1, "a resolve with no item applied in between was re-run");
        check(probes[2]->resolves.load() == 2, "a resolve whose range an earlier unapplied item wrote was not re-run");
        check(probes[2]->lastResolveOnCommitter.load(), "the conflict re-resolve did not run on the committer");
        check(probes[3]->resolves.load() == 1, "a resolve whose range nobody wrote was re-run");
        const auto text = DrawPipeline::ResolveText();
        check(text.find("conflicts 1,") != std::string::npos, "pipe_resolve_conflicts did not count exactly the one conflict: " + text);
    }
    std::lock_guard guard(orderMutex);
    for (std::size_t i = 1; i < order.size(); ++i) {
        if (order[i] >= 0 && order[i - 1] >= 0 && order[i] < 10 && order[i - 1] < 10) check(order[i] > order[i - 1], "commits ran out of order");
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
        else if (mode.starts_with("--resolve")) Resolve(mode);
        else throw std::runtime_error("unknown mode");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
