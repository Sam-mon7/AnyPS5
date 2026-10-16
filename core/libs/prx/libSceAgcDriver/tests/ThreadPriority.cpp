#include "prx/libSceAgcDriver/Execution/include/ThreadPriority.hpp"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace AgcDriver;

constexpr std::uint64_t Unlimited = std::numeric_limits<std::uint64_t>::max();
constexpr RtkitLimits Rtkit{20, 200000};

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void ParseTests() {
    for (const char* text : {static_cast<const char*>(nullptr), "", "off"}) Require(ParseThreadPriorityMode(text) == ThreadPriorityMode::Off, std::string("APS5_THREAD_PRIORITY=") + (text == nullptr ? "(unset)" : text) + " must leave priorities alone");
    Require(ParseThreadPriorityMode("rt") == ThreadPriorityMode::Realtime, "APS5_THREAD_PRIORITY=rt must ask for real-time priority");
    Require(ParseThreadPriorityMode("high") == ThreadPriorityMode::High, "APS5_THREAD_PRIORITY=high must ask for a nice level");
    for (const char* text : {"hgih", "HIGH", "realtime", "on", "1", "0", " high"}) {
        bool threw = false;
        try {
            static_cast<void>(ParseThreadPriorityMode(text));
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        Require(threw, std::string("APS5_THREAD_PRIORITY=") + text + " must be rejected");
    }
}

void ExpectRefused(const RealtimePlan& plan, const std::string& what) {
    Require(!plan.refusal.empty() && !plan.limit.has_value(), what + " must refuse real-time priority without touching RLIMIT_RTTIME");
}

void ExpectLimit(const RealtimePlan& plan, RttimeLimit expected, const std::string& what) {
    Require(plan.refusal.empty() && plan.limit.has_value(), what + " must set RLIMIT_RTTIME");
    Require(*plan.limit == expected, what + " set RLIMIT_RTTIME to soft " + std::to_string(plan.limit->soft) + " hard " + std::to_string(plan.limit->hard) + ", expected soft " + std::to_string(expected.soft) + " hard " + std::to_string(expected.hard));
}

void PlanTests() {
    ExpectRefused(PlanRealtime(10, std::nullopt, {Unlimited, Unlimited}, 200000), "an unreachable rtkit");
    ExpectRefused(PlanRealtime(10, std::nullopt, {30000, 150000}, 200000), "an unreachable rtkit");
    ExpectRefused(PlanRealtime(21, Rtkit, {Unlimited, Unlimited}, 200000), "a priority above rtkit's MaxRealtimePriority");
    ExpectRefused(PlanRealtime(10, RtkitLimits{20, 0}, {Unlimited, Unlimited}, 200000), "rtkit without an RTTimeUSecMax");
    ExpectLimit(PlanRealtime(20, Rtkit, {Unlimited, Unlimited}, 200000), {100000, 200000}, "an unlimited hard limit");
    ExpectLimit(PlanRealtime(10, Rtkit, {100000, Unlimited}, 200000), {100000, 200000}, "a lowered soft limit under an unlimited hard limit");
    ExpectLimit(PlanRealtime(10, Rtkit, {Unlimited, Unlimited}, 500000), {100000, 200000}, "APS5_THREAD_RTTIME_US above RTTimeUSecMax");
    ExpectLimit(PlanRealtime(10, Rtkit, {Unlimited, Unlimited}, 100000), {50000, 100000}, "APS5_THREAD_RTTIME_US below RTTimeUSecMax");
    ExpectLimit(PlanRealtime(10, Rtkit, {150000, 150000}, 200000), {75000, 150000}, "a hard limit rtkit accepts with a soft limit above half of it");
    const auto kept = PlanRealtime(10, Rtkit, {30000, 150000}, 200000);
    Require(kept.refusal.empty() && !kept.limit.has_value(), "limits rtkit already accepts must be left alone");
}

void RefusalTest() {
    bool threw = false;
    try {
        RaiseWorkerThreadPriority("test");
    } catch (const std::runtime_error& error) {
        threw = true;
        std::cout << "refused: " << error.what() << '\n';
    }
    Require(threw, "a refused priority request must throw");
}

void ServiceTest(bool expectRaised) {
    bool threw = false;
    int observed = 0;
    std::thread([&] {
        try {
            RaiseServiceThreadPriority("test service");
        } catch (...) {
            threw = true;
        }
#ifdef _WIN32
        observed = GetThreadPriority(GetCurrentThread());
#endif
    }).join();
    Require(!threw, "raising a service thread's priority must never throw");
    Require(ServiceThreadPriorityRequested() == expectRaised, std::string("APS5_SERVICE_THREAD_PRIORITY must be ") + (expectRaised ? "on" : "off") + " in this test");
#ifdef _WIN32
    const int expected = expectRaised ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL;
    Require(observed == expected, "service thread priority is " + std::to_string(observed) + ", expected " + std::to_string(expected));
#endif
}

}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "refused") {
            RefusalTest();
            return 0;
        }
        if (argc > 1 && (std::string(argv[1]) == "service-on" || std::string(argv[1]) == "service-off")) {
            ServiceTest(std::string(argv[1]) == "service-on");
            return 0;
        }
        ParseTests();
        PlanTests();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
