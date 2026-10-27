#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

alignas(256) std::array<std::uint32_t, 7> CounterCode{0xf4080200u, 0xfa000000u, 0xbf8cc07fu, 0x7e000281u, 0xe0c80000u, 0x80020000u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 7> StoreCode{0xf4080200u, 0xfa000000u, 0xbf8cc07fu, 0x7e000202u, 0xe0700000u, 0x80020000u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 15> SpinCode{0xf4080200u, 0xfa000000u, 0xbe840381u, 0x9304ff04u, 0x0019660du, 0x8004ff04u, 0x3c6ef35fu, 0x80828102u, 0xbf078002u, 0xbf85fff9u, 0xbf8cc07fu, 0x7e000204u, 0xe0700000u, 0x80020000u, 0xbf810000u};

std::uint32_t Compute = 0x20u;
constexpr std::uint32_t OtherCompute = 0x28u;
constexpr std::uint32_t SpinIterations = 1u << 20u;

alignas(256) std::array<std::uint32_t, 4> Stale{};
alignas(256) std::array<std::uint32_t, 4> Fresh{};
alignas(256) std::array<std::uint32_t, 4> Target{};
alignas(256) std::array<std::uint32_t, 4> TargetDescriptor{};
alignas(256) std::array<std::uint32_t, 4> Spun{};
alignas(256) std::array<std::uint32_t, 4> SpunDescriptor{};
alignas(256) std::array<std::uint32_t, 3> Arguments{1, 1, 1};
alignas(256) std::array<std::uint32_t, 4> SecondTarget{};
alignas(256) volatile std::uint32_t Gate = 1;
alignas(256) volatile std::uint32_t Done = 0;
alignas(256) volatile std::uint32_t OtherDone = 0;

enum class Window { None, CpuStore, QueuedGpuStore, Epoch, Recapture, Indirect, Layout };

std::atomic<Window> Armed{Window::None};
std::atomic<std::uint64_t> AheadWaits{0};
std::atomic<std::uint64_t> SubmitWaits{0};
std::thread::id Submitter;
std::uint32_t SpinDispatches = 16;
std::vector<std::uint32_t> OtherWords;
bool SawPending = false;

std::uint32_t Low(const volatile void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const volatile void* pointer) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u);
}

std::array<std::uint32_t, 4> Descriptor(const void* pointer, std::uint32_t bytes) {
    return {Low(pointer), High(pointer) & 0xffffu, bytes, 0x31016facu};
}

void SetShaderRegister(std::vector<std::uint32_t>& words, std::uint32_t offset, std::uint32_t value) {
    words.insert(words.end(), {0xc0017600u, offset, value});
}

void Program(std::vector<std::uint32_t>& words, const void* code, std::initializer_list<std::uint32_t> userData) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(code));
    SetShaderRegister(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetShaderRegister(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    for (const std::uint32_t offset : {0x207u, 0x208u, 0x209u}) SetShaderRegister(words, offset, 1);
    SetShaderRegister(words, 0x212, 0);
    SetShaderRegister(words, 0x213, static_cast<std::uint32_t>(userData.size()) << 1u);
    std::uint32_t offset = 0x240;
    for (const auto value : userData) SetShaderRegister(words, offset++, value);
}

void Dispatch(std::vector<std::uint32_t>& words, const void* code, std::initializer_list<std::uint32_t> userData) {
    Program(words, code, userData);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
}

void WriteData(std::vector<std::uint32_t>& words, const volatile std::uint32_t* target, std::uint32_t value) {
    words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(target), High(target), value});
}

void Submit(std::uint32_t queue, std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require((queue == 0 ? sceAgcDriverSubmitDcb(&packet) : sceAgcDriverSubmitAcb(queue, &packet)) == 0, "compute submit failed");
}

void QueueStoreAhead() {
    OtherWords.clear();
    for (std::uint32_t index = 0; index < SpinDispatches; ++index) Dispatch(OtherWords, SpinCode.data(), {Low(SpunDescriptor.data()), High(SpunDescriptor.data()), SpinIterations});
    Dispatch(OtherWords, StoreCode.data(), {Low(TargetDescriptor.data()), High(TargetDescriptor.data()), Low(Fresh.data())});
    WriteData(OtherWords, &OtherDone, 1);
    Submit(OtherCompute, OtherWords);
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5) && OtherDone == 0) {
        if (AgcDriver::DriverDetail::Driver::QueryPendingSpan(reinterpret_cast<std::uintptr_t>(Target.data()), sizeof(Target), {}) != AgcDriver::ShaderMemory::PendingWrite::None) {
            SawPending = true;
            return;
        }
        std::this_thread::yield();
    }
}

void BeforeAdoption() {
    const auto window = Armed.exchange(Window::None);
    if (window == Window::CpuStore) reinterpret_cast<volatile std::uint32_t&>(Target[0]) = Low(Fresh.data());
    else if (window == Window::QueuedGpuStore) QueueStoreAhead();
    else if (window == Window::Epoch) AgcDriver::GuestMemory::BumpCollectEpoch();
    else if (window == Window::Indirect) Arguments[0] = 2;
    else if (window == Window::Layout) GuestAllocations::GuestAllocationsInvalidate_nid_postfix(reinterpret_cast<std::uintptr_t>(Target.data()), sizeof(Target));
}

void AwaitAhead() {
    AgcDriver::DriverDetail::AwaitDispatchPreparation();
    ++AheadWaits;
    if (std::this_thread::get_id() == Submitter) ++SubmitWaits;
}

void WaitIdle() {
    AgcDriverWaitIdle_nid_postfix();
    for (const auto* buffer : {&Stale, &Fresh, &Target}) AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(buffer->data()), sizeof(*buffer));
}

void Reset() {
    WaitIdle();
    Stale = {};
    Fresh = {};
    Target = Descriptor(Stale.data(), sizeof(Stale));
    Arguments = {1, 1, 1};
    Done = 0;
    OtherDone = 0;
    SawPending = false;
}

void Gated(Window window) {
    Reset();
    if (window == Window::Recapture) Target = Descriptor(Fresh.data(), sizeof(Fresh));
    std::vector<std::uint32_t> words;
    words.insert(words.end(), {0xc0053c00u, 0x13u, Low(&Gate), High(&Gate), 1u, 0xffffffffu, 0x19u});
    if (window == Window::Indirect) {
        Program(words, CounterCode.data(), {Low(Target.data()), High(Target.data())});
        words.insert(words.end(), {0xc0021600u, Low(Arguments.data()), High(Arguments.data()), 0x8041u});
    } else Dispatch(words, CounterCode.data(), {Low(Target.data()), High(Target.data())});
    WriteData(words, &Done, 1);
    Armed = window;
    Submit(Compute, words);
    WaitIdle();
    Armed = Window::None;
    Require(Done == 1, "the gated submission did not finish");
}

std::string Counts() {
    return "(stale " + std::to_string(Stale[0]) + ", fresh " + std::to_string(Fresh[0]) + ")";
}

void RegisterChanges() {
    Reset();
    SecondTarget = Descriptor(Fresh.data(), sizeof(Fresh));
    std::vector<std::uint32_t> warm;
    Dispatch(warm, CounterCode.data(), {Low(SecondTarget.data()), High(SecondTarget.data())});
    Submit(Compute, warm);
    WaitIdle();
    Reset();
    std::vector<std::uint32_t> words;
    Dispatch(words, CounterCode.data(), {Low(Target.data()), High(Target.data())});
    Dispatch(words, CounterCode.data(), {Low(SecondTarget.data()), High(SecondTarget.data())});
    const auto adopted = AgcDriverDispatchesAdoptedAhead_nid_postfix();
    const auto waits = AheadWaits.load();
    Submit(Compute, words);
    WaitIdle();
    Require(AheadWaits.load() == waits + 1, "the register-change dispatches did not share one drained helper batch");
    Require(Stale[0] == 1 && Fresh[0] == 1, "the helper did not follow the shader register writes between dispatches " + Counts());
    Require(AgcDriverDispatchesAdoptedAhead_nid_postfix() == adopted + 2, "the helper adopted " + std::to_string(AgcDriverDispatchesAdoptedAhead_nid_postfix() - adopted) + " of 2 dispatches across register changes");
}

void LabelOrdering() {
    Reset();
    std::vector<std::uint32_t> words;
    Dispatch(words, CounterCode.data(), {Low(Target.data()), High(Target.data())});
    WriteData(words, Target.data(), Low(Fresh.data()));
    Dispatch(words, CounterCode.data(), {Low(Target.data()), High(Target.data())});
    Submit(Compute, words);
    WaitIdle();
    Require(Stale[0] == 1 && Fresh[0] == 1, "the helper crossed a queued descriptor label store " + Counts());
}

void MappingChange() {
    const auto rejected = AgcDriverDispatchesRejectedAhead_nid_postfix();
    Gated(Window::Layout);
    Require(Stale[0] == 1 && Fresh[0] == 0, "the mapping fallback used the wrong descriptor " + Counts());
    Require(AgcDriverDispatchesRejectedAhead_nid_postfix() == rejected + 1, "a helper result crossed a mapping invalidation");
}

bool WaitingOnGate() {
    for (const auto& wait : AgcDriver::DriverDetail::Driver::Get().AwaitedWaits()) {
        if (wait.address == reinterpret_cast<std::uintptr_t>(&Gate)) return true;
    }
    return false;
}

void SubmissionPreparation(bool replaceDescriptor, bool inheritedState = false, Window window = Window::None) {
    Reset();
    Gate = 0;
    struct ReleaseGate {
        ~ReleaseGate() { Gate = 1; }
    } release;
    std::vector<std::uint32_t> earlier{0xc0053c00u, 0x13u, Low(&Gate), High(&Gate), 1u, 0xffffffffu, 0x19u};
    if (inheritedState) SetShaderRegister(earlier, 0x250, 7);
    WriteData(earlier, &Done, 1);
    Submit(Compute, earlier);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!WaitingOnGate() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    Require(WaitingOnGate() && Done == 0, "the earlier submission did not remain executing on its wait");
    std::vector<std::uint32_t> later;
    SetShaderRegister(later, 0x200, 0x8041u);
    for (const auto offset : {0x201u, 0x202u, 0x203u}) SetShaderRegister(later, offset, 1);
    Dispatch(later, CounterCode.data(), {Low(Target.data()), High(Target.data())});
    const auto submitWaits = SubmitWaits.load();
    const auto adopted = AgcDriverDispatchesAdoptedAhead_nid_postfix();
    const auto rejected = AgcDriverDispatchesRejectedAhead_nid_postfix();
    Submit(Compute, later);
    Require(SubmitWaits.load() == submitWaits + 1, "the queued dispatch was not prepared on receipt");
    Require(WaitingOnGate() && Done == 0, "the earlier submission finished before preparation completed");
    Require(AgcDriverDispatchesAdoptedAhead_nid_postfix() == adopted, "the queued dispatch executed before the gate opened");
    if (replaceDescriptor) reinterpret_cast<volatile std::uint32_t&>(Target[0]) = Low(Fresh.data());
    Armed = window;
    Gate = 1;
    WaitIdle();
    Armed = Window::None;
    Require(Done == 1, "the earlier submission did not complete");
    Require(Stale[0] == (replaceDescriptor ? 0u : 1u) && Fresh[0] == (replaceDescriptor ? 1u : 0u), "the submission helper used stale input " + Counts());
    Require(AgcDriverDispatchesAdoptedAhead_nid_postfix() == adopted + (replaceDescriptor || inheritedState || window != Window::None ? 0u : 1u), "the submission helper adoption count was wrong");
    if (replaceDescriptor || window != Window::None) Require(AgcDriverDispatchesRejectedAhead_nid_postfix() == rejected + 1, "the submission helper missed the intervening guest store or epoch change");
    std::cout << "submission preparation " << (inheritedState ? "inherited state rejected" : replaceDescriptor ? "intervening guest store rejected" : window != Window::None ? "epoch change rejected" : "adopted behind executing submission") << ": passed\n";
}

}

int main(int argc, char** argv) {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        const bool helperCore = argc == 2 && std::string(argv[1]) == "--helper-core";
        const bool helperSubmit = argc == 2 && std::string(argv[1]) == "--helper-submit";
        const bool helper = helperCore || helperSubmit || (argc == 2 && std::string(argv[1]) == "--helper");
        Submitter = std::this_thread::get_id();
        Shader shader{};
        if (helper) {
            Compute = 0;
            shader.file_header = 0x34333231;
            shader.version = 0x18;
            shader.header_size = sizeof(Shader);
            shader.shader_size = sizeof(CounterCode);
            shader.code = CounterCode.data();
            AgcDriverRegisterShader_nid_postfix(&shader);
            GuestAllocations::Mutation mapping;
            if (!mapping.Covers(Target.data(), sizeof(Target))) mapping.Add(Target.data(), sizeof(Target), true, true, false);
            if (!mapping.Covers(SecondTarget.data(), sizeof(SecondTarget))) mapping.Add(SecondTarget.data(), sizeof(SecondTarget), true, true, false);
            AgcDriver::DriverDetail::Driver::Get().SetAheadScheduleHook(&AwaitAhead);
        }
        Require(High(Stale.data()) == High(Fresh.data()), "the stale and fresh buffers differ in their high address word");
        TargetDescriptor = Descriptor(Target.data(), sizeof(Target));
        SpunDescriptor = Descriptor(Spun.data(), sizeof(Spun));
        AgcDriverSetAheadAdoptHook_nid_postfix(&BeforeAdoption);
        Reset();
        std::vector<std::uint32_t> warmUp;
        Dispatch(warmUp, CounterCode.data(), {Low(Target.data()), High(Target.data())});
        WriteData(warmUp, &Done, 1);
        Submit(Compute, warmUp);
        std::vector<std::uint32_t> otherWarmUp;
        Dispatch(otherWarmUp, SpinCode.data(), {Low(SpunDescriptor.data()), High(SpunDescriptor.data()), SpinIterations});
        Dispatch(otherWarmUp, StoreCode.data(), {Low(TargetDescriptor.data()), High(TargetDescriptor.data()), Low(Fresh.data())});
        Submit(OtherCompute, otherWarmUp);
        WaitIdle();
        Require(Done == 1 && Target[0] == Low(Fresh.data()), "the warm-up dispatches did not run");

        if (helperSubmit) {
            SubmissionPreparation(false);
            SubmissionPreparation(true);
            SubmissionPreparation(false, false, Window::Epoch);
            SubmissionPreparation(false, true);
            AgcDriverSetAheadAdoptHook_nid_postfix(nullptr);
            AgcDriver::DriverDetail::Driver::Get().SetAheadScheduleHook(nullptr);
            AgcDriverShutdown_nid_postfix();
            return 0;
        }

        Gated(Window::None);
        Require(Stale[0] == 1 && Fresh[0] == 0, "the gated dispatch without a store in the window did not count into the stale buffer " + Counts());
        const auto adopted = AgcDriverDispatchesAdoptedAhead_nid_postfix();
        Require(adopted != 0, "the gated dispatch was not resolved ahead and adopted");

        if (helper) {
            Gated(Window::Recapture);
            Require(Fresh[0] == 1 && Stale[0] == 0, "the helper did not recapture the changed descriptor " + Counts());
            Require(AgcDriverDispatchesAdoptedAhead_nid_postfix() == adopted + 1, "the helper recapture was not adopted");
            const auto adoptedBeforeIndirect = AgcDriverDispatchesAdoptedAhead_nid_postfix();
            Gated(Window::Indirect);
            Require(Stale[0] == 2 && Fresh[0] == 0, "the helper froze GPU indirect argument counts " + Counts());
            Require(AgcDriverDispatchesAdoptedAhead_nid_postfix() == adoptedBeforeIndirect + 1, "the indirect helper result was not adopted");
            const auto rejectedBeforeEpoch = AgcDriverDispatchesRejectedAhead_nid_postfix();
            Gated(Window::Epoch);
            Require(Stale[0] == 1 && Fresh[0] == 0, "the epoch fallback used the wrong descriptor " + Counts());
            Require(AgcDriverDispatchesRejectedAhead_nid_postfix() == rejectedBeforeEpoch + 1, "a helper result crossed a collect epoch");
            RegisterChanges();
            LabelOrdering();
        }

        auto rejected = AgcDriverDispatchesRejectedAhead_nid_postfix();
        Gated(Window::CpuStore);
        Require(Fresh[0] == 1 && Stale[0] == 0, "a dispatch resolved ahead whose descriptor the CPU replaced before its adoption used the old descriptor " + Counts());
        Require(AgcDriverDispatchesRejectedAhead_nid_postfix() == rejected + 1, "the CPU store in the adoption window was not caught by the adoption check");
        std::cout << "cpu store before adoption: passed\n";
        if (helperCore) {
            MappingChange();
            AgcDriverSetAheadAdoptHook_nid_postfix(nullptr);
            AgcDriver::DriverDetail::Driver::Get().SetAheadScheduleHook(nullptr);
            AgcDriverShutdown_nid_postfix();
            std::cout << "dispatch helper adoption checks passed\n";
            return 0;
        }

        bool conclusive = false;
        for (int attempt = 0; attempt < 4 && !conclusive; ++attempt, SpinDispatches *= 4) {
            rejected = AgcDriverDispatchesRejectedAhead_nid_postfix();
            Gated(Window::QueuedGpuStore);
            conclusive = SawPending;
            if (!conclusive) {
                std::cout << "queued gpu store before adoption: the store finished before the adoption with " << SpinDispatches << " spin dispatches, retrying\n";
                continue;
            }
            Require(Fresh[0] == 1 && Stale[0] == 0, "a dispatch resolved ahead whose descriptor a GPU store queued on another queue replaces before its adoption used the old descriptor " + Counts());
            Require(AgcDriverDispatchesRejectedAhead_nid_postfix() == rejected + 1, "the queued GPU store in the adoption window was not caught by the adoption check");
        }
        if (helper) MappingChange();
        AgcDriverSetAheadAdoptHook_nid_postfix(nullptr);
        if (helper) AgcDriver::DriverDetail::Driver::Get().SetAheadScheduleHook(nullptr);
        if (!conclusive) {
            std::cout << "queued gpu store before adoption: never observed the store pending at the adoption\n";
            AgcDriverShutdown_nid_postfix();
            return VulkanTestSkipped;
        }
        std::cout << "queued gpu store before adoption: passed\n";
        std::cout << "dispatch ahead adopt check test passed\n";
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}
