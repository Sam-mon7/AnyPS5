#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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

alignas(256) std::array<std::uint32_t, 4> First{};
alignas(256) std::array<std::uint32_t, 4> Stale{};
alignas(256) std::array<std::uint32_t, 4> Fresh{};
alignas(256) std::array<std::uint32_t, 4> FirstDescriptor{};
alignas(256) std::array<std::uint32_t, 4> SecondDescriptor{};
alignas(256) volatile std::uint32_t Gate = 1;
alignas(256) volatile std::uint32_t Done = 0;
alignas(256) volatile std::uint32_t UnrelatedLabel = 0;

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

void Count(std::vector<std::uint32_t>& words, const void* descriptor) {
    Program(words, CounterCode.data(), {Low(descriptor), High(descriptor)});
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
}

}

int main(int argc, char** argv) {
    using namespace AgcDriver::DriverDetail;
    try {
        const bool batchingTest = argc > 1;
        const bool graphics = batchingTest && std::string(argv[1]) == "--graphics";
        const auto submit = [&](Packet& packet) {
            return graphics ? sceAgcDriverSubmitDcb(&packet) : sceAgcDriverSubmitAcb(0x20, &packet);
        };
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        FirstDescriptor = Descriptor(First.data(), sizeof(First));
        SecondDescriptor = Descriptor(Stale.data(), sizeof(Stale));
        std::vector<std::uint32_t> warmUp;
        Count(warmUp, FirstDescriptor.data());
        Count(warmUp, SecondDescriptor.data());
        warmUp.insert(warmUp.end(), {0xc0033700u, 0x00100200u, Low(&Done), High(&Done), 2u});
        Packet first{warmUp.data(), static_cast<std::uint32_t>(warmUp.size()), 0, {}};
        Require(submit(first) == 0, "warm-up submit failed");
        AgcDriverWaitIdle_nid_postfix();
        Require(Done == 2 && First[0] == 1 && Stale[0] == 1, "the warm-up dispatches did not run");
        First[0] = 0;
        Stale[0] = 0;
        const auto fresh = Descriptor(Fresh.data(), sizeof(Fresh));
        const auto batchedBefore = captureTriesBatched.load();
        const auto triedBefore = captureTryRecords.load();
        const auto retriedBefore = captureRetries.load();
        std::vector<std::uint32_t> words;
        words.insert(words.end(), {0xc0053c00u, 0x13u, Low(&Gate), High(&Gate), 1u, 0xffffffffu, 0x19u});
        if (batchingTest) {
            Program(words, CounterCode.data(), {Low(FirstDescriptor.data()), High(FirstDescriptor.data())});
            words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(&UnrelatedLabel), High(&UnrelatedLabel), 0x12345678u});
            words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
            Program(words, CounterCode.data(), {Low(SecondDescriptor.data()), High(SecondDescriptor.data())});
        } else {
            Count(words, FirstDescriptor.data());
        }
        words.insert(words.end(), {0xc0063700u, 0x00100200u, Low(SecondDescriptor.data()), High(SecondDescriptor.data()), fresh[0], fresh[1], fresh[2], fresh[3]});
        if (batchingTest) words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
        else Count(words, SecondDescriptor.data());
        words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(&Done), High(&Done), 1u});
        Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
        Require(submit(packet) == 0, "submit failed");
        if (batchingTest) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (Done != 1) {
                Require(std::chrono::steady_clock::now() < deadline, "the submission did not finish");
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        AgcDriverWaitIdle_nid_postfix();
        Require(Done == 1, "the submission did not finish");
        Require(SecondDescriptor[0] == fresh[0] && SecondDescriptor[1] == fresh[1], "WRITE_DATA did not replace the descriptor");
        Require(First[0] == 1, "the first dispatch did not run");
        Require(Fresh[0] == 1 && Stale[0] == 0, "a dispatch whose descriptor a WRITE_DATA before it replaces used the old descriptor (stale " + std::to_string(Stale[0]) + ", fresh " + std::to_string(Fresh[0]) + ")");
        if (batchingTest) {
            Require(UnrelatedLabel == 0x12345678u, "a non-overlapping label was lost while batching its lock");
            if (std::getenv("APS5_NO_QUEUED_LABELS") != nullptr) Require(captureRetries.load() > retriedBefore, "a capture overlapping a queued descriptor write was not retried");
            if (std::getenv("APS5_NO_CAPTURE_LABEL_LOCK_BATCHING") == nullptr && (!graphics || DrawPipeline::Depth() == 0)) {
                Require(captureTriesBatched.load() > batchedBefore, "a non-overlapping capture did not batch its label lock");
                Require(captureTryRecords.load() == triedBefore, "an inline capture still took a separate label lock");
            } else {
                Require(captureTriesBatched.load() == batchedBefore, "capture label locks were batched with the optimization disabled");
            }
        }
        std::cout << "write data written ahead test passed\n";
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
