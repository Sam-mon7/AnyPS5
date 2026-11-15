#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

struct DriverTestAccess {
    static void Device(std::shared_ptr<VulkanDevice> device) { Driver::Get().device = std::move(device); }
};

}

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::Driver;
using AgcDriver::DriverDetail::DrawPipeline;
namespace ArchB = AgcDriver::ArchB;
namespace GuestMemory = AgcDriver::GuestMemory;
using Clock = std::chrono::steady_clock;
using Counts = std::map<std::string, double>;
constexpr std::size_t BlockBytes = 262144;
constexpr std::uint32_t FillBytes = 131072;
constexpr std::uint64_t FillOffset = 65536;
constexpr unsigned UniqueLabels = 8;
constexpr unsigned LabelCount = UniqueLabels + 2;
constexpr unsigned MiddleLabel = 3;
constexpr auto Timeout = std::chrono::seconds(5);
constexpr std::uint32_t Guard = 0x8badf00du;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        block = std::aligned_alloc(65536, BlockBytes);
#endif
        Require(block != nullptr, "label run: cannot allocate registered guest memory");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }
    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(block); }
private:
    void* block = nullptr;
};

struct Shutdown {
    ~Shutdown() {
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
    }
};

struct Gate {
    std::shared_ptr<std::atomic<bool>> release = std::make_shared<std::atomic<bool>>(false);
    ~Gate() { Open(); }
    void Open() { release->store(true, std::memory_order_release); }
};

struct HostGate {
    std::uint64_t address;
    std::uint32_t value;
    ~HostGate() { Open(); }
    void Open() { *reinterpret_cast<volatile std::uint32_t*>(address) = value; }
};

std::uint32_t Low(std::uint64_t value) { return static_cast<std::uint32_t>(value); }
std::uint32_t High(std::uint64_t value) { return static_cast<std::uint32_t>(value >> 32u); }
std::uint32_t Word(std::uint64_t address) { return *reinterpret_cast<const volatile std::uint32_t*>(address); }
std::uint64_t LabelAddress(std::uint64_t base, unsigned index) { return base + 4096 + index * 16; }
std::uint32_t LabelLow(unsigned round, unsigned index) { return 0x12340000u + round * 32u + index + 1u; }
std::uint32_t LabelHigh(unsigned round, unsigned index) { return index % 2 == 0 ? Guard : 0xabcd0000u + round * 32u + index; }

template<typename TPredicate>
void Await(TPredicate predicate, const std::string& message) {
    const auto deadline = Clock::now() + Timeout;
    while (!predicate()) {
        Require(Clock::now() < deadline, "label run: " + message);
        std::this_thread::yield();
    }
}

void Append(std::vector<std::uint32_t>& words, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    words.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    words.insert(words.end(), payload);
}

void Eop(std::vector<std::uint32_t>& words, std::uint64_t address, std::uint64_t value, bool wide, bool interrupt = false) {
    Append(words, 0x49, {0x514u, ((wide ? 2u : 1u) << 29u) | (interrupt ? 2u << 24u : 0u), Low(address), High(address), Low(value), High(value), 0u});
}

void Wait(std::vector<std::uint32_t>& words, std::uint64_t address, std::uint32_t value) {
    Append(words, 0x3c, {0x13u, Low(address), High(address), value, 0xffffffffu, 0x19u});
}

void Submit(std::vector<std::uint32_t>& words, bool consumer = false) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require((consumer ? sceAgcDriverSubmitAcb(0x20, &packet) : sceAgcDriverSubmitDcb(&packet)) == 0, "label run: queue submission failed");
}

void CheckFill(std::uint64_t address, std::uint32_t pattern) {
    for (std::size_t offset = 0; offset < FillBytes; offset += 4) {
        if (Word(address + offset) != pattern) Require(false, "label run: label became visible before preceding GPU data, without a forced flush, at byte " + std::to_string(offset));
    }
}

void CheckLabels(std::uint64_t base, unsigned round, unsigned count) {
    for (unsigned index = 0; index < count; ++index) {
        const auto address = LabelAddress(base, index);
        Require(Word(address) == LabelLow(round, index) && Word(address + 4) == LabelHigh(round, index), "label run: mixed-width label bytes or FIFO order changed");
        Require(Word(address + 8) == Guard && Word(address + 12) == Guard, "label run: an EOP overwrote its guard words");
    }
}

double Count(const Counts& counts, const char* name) {
    const auto found = counts.find(name);
    return found == counts.end() ? 0.0 : found->second;
}

void Add(Counts& target, const Counts& source) {
    for (const auto& [name, value] : source) target[name] += value;
}

Counts Run(std::uint64_t base, unsigned round, bool waiter, bool gated) {
    const auto ack = base + 512;
    const auto host = base + 768;
    const auto ordered = base + 8192;
    const auto pattern = 0x5eed0000u + round;
    const auto orderHigh = 0xcafe0000u + round;
    const auto orderLow = 0xbeef0000u + round;
    for (unsigned index = 0; index < UniqueLabels; ++index) {
        auto* words = reinterpret_cast<std::uint32_t*>(LabelAddress(base, index));
        words[0] = 0;
        words[1] = words[2] = words[3] = Guard;
    }
    *reinterpret_cast<std::uint64_t*>(ordered) = 0;
    *reinterpret_cast<std::uint32_t*>(ack) = 0;
    *reinterpret_cast<std::uint32_t*>(host) = 0;
    std::memset(reinterpret_cast<void*>(base + FillOffset), 0, FillBytes);
    HostGate hostGate{host, round};
    std::vector<std::uint32_t> producer, consumer;
    Append(producer, 0x50, {2u << 29u, pattern, 0u, Low(base + FillOffset), High(base + FillOffset), FillBytes});
    for (unsigned index = 0; index < UniqueLabels; ++index) {
        const auto value = (static_cast<std::uint64_t>(LabelHigh(round, index)) << 32u) | LabelLow(round, index);
        Eop(producer, LabelAddress(base, index), value, index % 2 != 0);
        if (gated && index == MiddleLabel) {
            Wait(producer, ack, round);
            Wait(producer, host, round);
        }
    }
    Eop(producer, ordered, (static_cast<std::uint64_t>(orderHigh) << 32u) | 0x11112222u, true);
    Eop(producer, ordered, orderLow, false, true);
    if (waiter) {
        const auto middle = LabelAddress(base, MiddleLabel);
        Wait(consumer, middle, LabelLow(round, MiddleLabel));
        Append(consumer, 0x37, {0x00100200u, Low(ack), High(ack), round});
        Submit(consumer, true);
        Await([&] {
            const auto waits = Driver::Get().AwaitedWaits();
            return std::any_of(waits.begin(), waits.end(), [&](const auto& wait) { return wait.address == middle && wait.bytes == 4 && wait.reference == LabelLow(round, MiddleLabel); });
        }, "ACB queue 0x20 never waited on the intermediate EOP");
        Require(Word(ack) == 0, "label run: other-queue acknowledgement preceded the label");
    }
    Gate gate;
    DrawPipeline::Queue0().Enqueue([release = gate.release] {
        Await([&] { return release->load(std::memory_order_acquire); }, "prequeue gate was not released");
    }, {});
    Submit(producer);
    const auto queued = 2u + (gated ? MiddleLabel + 1 : LabelCount);
    Await([&] { return DrawPipeline::Queue0().Outstanding() >= queued; }, "DMA and successor EOPs were not prequeued");
    gate.Open();
    for (unsigned index = 0; index < UniqueLabels; ++index) {
        const auto address = LabelAddress(base, index);
        Await([&] { return Word(address) == LabelLow(round, index) && Word(address + 4) == LabelHigh(round, index); }, "EOP did not become visible within five seconds");
        CheckFill(base + FillOffset, pattern);
        CheckLabels(base, round, index + 1);
        if (gated && index == MiddleLabel) {
            Await([&] { return Word(ack) == round; }, "ACB queue 0x20 did not acknowledge the intermediate label");
            CheckFill(base + FillOffset, pattern);
            for (unsigned later = index + 1; later < UniqueLabels; ++later) Require(Word(LabelAddress(base, later)) == 0, "label run: a later label overtook the acknowledged host boundary");
            hostGate.Open();
        }
    }
    Await([&] { return Word(ordered) == orderLow; }, "overlapping EOP sequence did not finish");
    Require(Word(ordered + 4) == orderHigh, "label run: 32-bit EOP lost the preceding 64-bit EOP high word");
    CheckLabels(base, round, UniqueLabels);
    if (waiter) Await([&] { return Word(ack) == round; }, "other-queue waiter did not make bounded progress");
    Require(Driver::Get().WaitIdleFor(Timeout), "label run: submission completion exceeded five seconds");
    AgcDriverWaitIdle_nid_postfix();
    auto counts = ArchB::Recorder::Collect(Clock::now()).counters;
    Require(Count(counts, "j3c_label_count") == LabelCount, "label run: EOP commit census did not observe every queued label");
    Require(Count(counts, "j3c_label_reason_0") > 0 && Count(counts, "j3c_label_recorded_stores") > 0,
            "label run: no actual GPU EOP stores; reason0=" + std::to_string(Count(counts, "j3c_label_reason_0")) + ", recorded=" + std::to_string(Count(counts, "j3c_label_recorded_stores")) + ", CPU=" + std::to_string(Count(counts, "j3c_label_cpu_writes")));
    Require(Count(counts, "j3c_failed_commits") == 0, "label run: a queued commit failed");
    std::cout << "label run " << round << (gated ? " gated waiter" : waiter ? " ready waiter" : " ready")
              << ": reason0=" << Count(counts, "j3c_label_reason_0") << ", recorded=" << Count(counts, "j3c_label_recorded_stores")
              << ", label-to-label=" << Count(counts, "label_handoff_from_label") << ", consumed=" << Count(counts, "label_handoff_consumed") << '\n';
    return counts;
}

}

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "off";
        Require(argc <= 2 && (mode == "off" || mode == "on" || mode == "verify"), "label run: invalid mode");
        ArchB::Switches::Initialize();
        GuestBlock memory;
        std::shared_ptr<AgcDriver::VulkanDevice> device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        AgcDriver::DriverDetail::DriverTestAccess::Device(device);
        Shutdown shutdown;
        const auto base = memory.Address();
        {
            std::lock_guard lock(GuestMemory::GpuMutex());
            GuestMemory::TagGpuLockThread(0);
            GuestMemory::SetCurrentPacket(0x49, 0);
            const std::array<std::uint32_t, 4> pattern{1, 2, 3, 4};
            Require(device->FillBuffer(base + FillOffset, FillBytes, pattern), "label run: registered GPU fill is unavailable");
            constexpr std::uint64_t probe = 0x123456789abcdef0ull;
            const auto reason = device->WriteLabelOnGpu(base, std::as_bytes(std::span(&probe, 1)), 1, 0, false);
            Require(reason == 0, "label run: registered probe did not use a GPU label store, reason=" + std::to_string(reason));
            device->SubmitRecorded(false);
            device->WaitIdle();
        }
        ArchB::Recorder::Boundary(true, 0, Clock::now());
        const bool enabled = ArchB::Switches::Enabled(ArchB::Feature::FixLabelHandoff);
        Require(enabled == (mode != "off"), "label run: requested mode differs from FIX_LABEL_HANDOFF");
        Require(ArchB::Switches::Enabled(ArchB::Feature::DrawHitCensus), "label run: DRAW_HIT_CENSUS must be enabled");
        Counts ready, all;
        for (unsigned round = 1; round <= 8; ++round) Add(ready, Run(base, round, false, false));
        const auto chained = Count(ready, "label_handoff_from_label");
        Require((chained > 0) == enabled, "label run: no-waiter ready run did not exercise real label-to-label retention exclusively when enabled");
        Require((Count(ready, "label_handoff_consumed") > 0) == enabled, "label run: no-waiter run did not consume a retained GPU hold exclusively when enabled");
        all = ready;
        Add(all, Run(base, 9, true, false));
        Add(all, Run(base, 10, true, true));
        if (mode == "verify") {
            Require(Count(all, "label_handoff_store_verified") >= Count(all, "j3c_label_reason_0"), "label run: VERIFY did not check recorded GPU store metadata");
            Require(Count(all, "label_handoff_plan_verified") == Count(all, "label_handoff_consumed"), "label run: VERIFY did not check every consumed FIFO plan");
            Require(Count(all, "label_handoff_run_verified") == Count(all, "label_handoff_consumed"), "label run: VERIFY did not check every consumed bounded run");
        } else {
            Require(Count(all, "label_handoff_store_verified") == 0 && Count(all, "label_handoff_plan_verified") == 0 && Count(all, "label_handoff_run_verified") == 0, "label run: verification ran outside VERIFY mode");
        }
        std::cout << "GPU label-run visibility " << mode << ": recorded stores=" << Count(all, "j3c_label_recorded_stores")
                  << ", reason0=" << Count(all, "j3c_label_reason_0") << ", ready label-to-label=" << chained
                  << ", metadata checks=" << Count(all, "label_handoff_store_verified") << ", plan checks=" << Count(all, "label_handoff_plan_verified")
                  << "; mixed-width FIFO, preceding GPU data and intermediate ACB waiter passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
