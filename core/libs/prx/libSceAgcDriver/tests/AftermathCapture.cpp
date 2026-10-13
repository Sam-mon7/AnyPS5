#include "prx/libSceAgcDriver/Graphics/include/AftermathCapture.hpp"
#if defined(ANYPS5_AFTERMATH_TESTING)
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
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
namespace Capture = AgcDriver::Graphics::Aftermath;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    Check(_putenv_s(name, value ? value : "") == 0, "setting test environment failed");
#else
    Check((value ? setenv(name, value, 1) : unsetenv(name)) == 0, "setting test environment failed");
#endif
}

struct Output {
    fs::path path;
    explicit Output(std::string_view scenario) {
        Check(!scenario.empty() && std::all_of(scenario.begin(), scenario.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        }), "scenario name must contain only ASCII letters, digits or hyphens");
#ifdef _WIN32
        const auto pid = _getpid();
#else
        const auto pid = getpid();
#endif
        path = fs::absolute(fs::temp_directory_path()) / ("anyps5-aftermath-cpu-" + std::to_string(pid) + "-" + std::string(scenario) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Check(!fs::exists(path), "test output path already exists");
        SetEnv("APS5_AFTERMATH_DIR", path.string().c_str());
    }
    ~Output() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

#if defined(ANYPS5_AFTERMATH_TESTING)
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    void Hold() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }
    bool Wait() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 2s, [&] { return entered; });
    }
    void Release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};

struct Fake {
    std::atomic<unsigned> loads{0}, pins{0}, enables{0}, disables{0}, queries{0}, sleeps{0}, clocks{0}, unloads{0}, destroyed{0}, writes{0};
    std::atomic<long long> milliseconds{0};
    std::mutex eventMutex;
    std::vector<std::string> events;
    std::vector<GFSDK_Aftermath_CrashDump_Status> statuses{GFSDK_Aftermath_CrashDump_Status_Finished};
    GFSDK_Aftermath_Result enableResult = GFSDK_Aftermath_Result_Success;
    GFSDK_Aftermath_Result disableResult = GFSDK_Aftermath_Result_Success;
    GFSDK_Aftermath_Result statusResult = GFSDK_Aftermath_Result_Success;
    bool loadOkay = true, pinOkay = true, throwWrite = false;
    Gate *writeGate = nullptr, *sleepGate = nullptr, *disableGate = nullptr;
    PFN_GFSDK_Aftermath_GpuCrashDumpCb callback = nullptr;
    void* callbackData = nullptr;
    bool validEnableArguments = false;

    void Event(const char* event) {
        std::lock_guard lock(eventMutex);
        events.emplace_back(event);
    }
    std::size_t Index(std::string_view event) {
        std::lock_guard lock(eventMutex);
        const auto found = std::find(events.begin(), events.end(), event);
        Check(found != events.end(), "expected lifecycle event missing");
        return static_cast<std::size_t>(found - events.begin());
    }
    unsigned Activity() const {
        return loads + pins + enables + disables + queries + sleeps + clocks + unloads + destroyed + writes;
    }
} fake;

GFSDK_Aftermath_Result GFSDK_AFTERMATH_CALL Enable(GFSDK_Aftermath_Version version, uint32_t apis, uint32_t flags,
        PFN_GFSDK_Aftermath_GpuCrashDumpCb callback, PFN_GFSDK_Aftermath_ShaderDebugInfoCb debug,
        PFN_GFSDK_Aftermath_GpuCrashDumpDescriptionCb description, PFN_GFSDK_Aftermath_ResolveMarkerCb marker, void* data) {
    ++fake.enables;
    fake.Event("enable");
    fake.validEnableArguments = version == GFSDK_Aftermath_Version_API && apis == GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan &&
        flags == GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default && callback && data && !debug && !description && !marker;
    fake.callback = callback;
    fake.callbackData = data;
    return fake.enableResult;
}

GFSDK_Aftermath_Result GFSDK_AFTERMATH_CALL Disable() {
    ++fake.disables;
    fake.Event("disable-enter");
    if (fake.disableGate) fake.disableGate->Hold();
    fake.Event("disable-return");
    return fake.disableResult;
}

GFSDK_Aftermath_Result GFSDK_AFTERMATH_CALL Status(GFSDK_Aftermath_CrashDump_Status* value) {
    const auto index = fake.queries.fetch_add(1);
    *value = fake.statuses[std::min<std::size_t>(index, fake.statuses.size() - 1)];
    if (*value == GFSDK_Aftermath_CrashDump_Status_Finished) fake.Event("finished");
    return fake.statusResult;
}

bool Load() { ++fake.loads; fake.Event("load"); return fake.loadOkay; }
bool Pin() { ++fake.pins; fake.Event("pin"); return fake.pinOkay; }
void Unload() { ++fake.unloads; fake.Event("unload"); }
void Destroyed() { ++fake.destroyed; fake.Event("destroyed"); }
auto Now() -> std::chrono::steady_clock::time_point { ++fake.clocks; return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(fake.milliseconds.load()); }
void Sleep() {
    const auto count = fake.sleeps.fetch_add(1);
    if (fake.sleepGate && count == 0) fake.sleepGate->Hold();
    fake.milliseconds.fetch_add(50);
    std::this_thread::yield();
}
void BeforeWrite() {
    ++fake.writes;
    if (fake.writeGate) fake.writeGate->Hold();
    if (fake.throwWrite) throw std::runtime_error("injected callback I/O failure");
}

Capture::Testing::Hooks Hooks() {
    Capture::Testing::Hooks hooks;
    hooks.enable = Enable;
    hooks.disable = Disable;
    hooks.status = Status;
    hooks.load = Load;
    hooks.pin = Pin;
    hooks.unload = Unload;
    hooks.now = Now;
    hooks.sleep = Sleep;
    hooks.beforeWrite = BeforeWrite;
    hooks.destroyed = Destroyed;
    return hooks;
}

void CheckNormalRelease(unsigned count = 1) {
    Check(fake.disables == count && fake.unloads == count && fake.destroyed == count, "normal release did not disable, unload and destroy exactly once per session");
    Check(fake.Index("disable-return") < fake.Index("unload"), "SDK unloaded before successful Disable returned");
}

void CheckQuarantine() {
    Check(fake.unloads == 0 && fake.destroyed == 0, "quarantined session lost DLL or callback storage");
    const auto enables = fake.enables.load();
    bool rejected = false;
    try { Capture::DeviceLease another; } catch (const std::exception&) { rejected = true; }
    Check(rejected && fake.enables == enables, "quarantine permitted another capture session");
}

void InvokeDump() {
    constexpr std::array<unsigned char, 9> bytes{0, 1, 2, 0xff, 0, 0x80, 3, 4, 0};
    Check(fake.callback && fake.callbackData, "Enable did not register callback state");
    fake.callback(bytes.data(), static_cast<uint32_t>(bytes.size()), fake.callbackData);
}

void CheckDump(const Output& output, std::size_t expected) {
    const std::vector<unsigned char> bytes{0, 1, 2, 0xff, 0, 0x80, 3, 4, 0};
    std::size_t count = 0;
    if (fs::exists(output.path)) for (const auto& file : fs::recursive_directory_iterator(output.path)) {
        if (file.path().extension() != ".nv-gpudmp") continue;
        std::ifstream stream(file.path(), std::ios::binary);
        const std::vector<unsigned char> read{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        Check(read == bytes, "callback changed or truncated dump bytes");
        ++count;
    }
    Check(count == expected, "callback files missing or filenames collided");
}

void Lifecycle() {
    {
        Capture::DeviceLease first;
        Check(static_cast<bool>(first), "enabled lease is empty");
        SetEnv("APS5_AFTERMATH", nullptr);
        {
            Capture::DeviceLease second;
            Check(static_cast<bool>(second) && fake.enables == 1, "removing opt-in omitted a second lease from the active session");
            fake.Event("second-device-destroyed");
        }
        Check(fake.disables == 0, "first lease release disabled another live device");
        try {
            Capture::DeviceLease failedConstruction;
            throw std::runtime_error("injected construction failure");
        } catch (const std::runtime_error&) {}
        Check(fake.disables == 0 && fake.enables == 1, "construction unwind damaged another device's session");
        Capture::Notify(VK_SUCCESS);
        Capture::Notify(VK_TIMEOUT);
        Capture::JoinPending();
        Check(fake.queries == 0 && fake.sleeps == 0, "clean operation polled for a nonexistent crash");
        fake.Event("first-device-destroyed");
    }
    Check(fake.validEnableArguments, "basic Vulkan capture enabled unwanted callbacks or flags");
    Check(fake.Index("pin") < fake.Index("enable"), "callback module was not pinned before Enable");
    Check(fake.Index("first-device-destroyed") < fake.Index("disable-enter"), "Disable preceded last device destruction");
    CheckNormalRelease();
}

void StatusCases(std::string_view scenario) {
    if (scenario == "status-sequence") fake.statuses = {GFSDK_Aftermath_CrashDump_Status_Unknown, GFSDK_Aftermath_CrashDump_Status_NotStarted, GFSDK_Aftermath_CrashDump_Status_CollectingData, GFSDK_Aftermath_CrashDump_Status_Finished};
    else if (scenario == "collect-failed") fake.statuses = {GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed};
    else if (scenario == "query-failed") fake.statusResult = GFSDK_Aftermath_Result_FAIL_Unknown;
    else fake.statuses = {GFSDK_Aftermath_CrashDump_Status_NotStarted};
    {
        Capture::DeviceLease lease;
        Capture::Notify(VK_ERROR_DEVICE_LOST);
        const auto queries = fake.queries.load();
        const auto elapsed = fake.milliseconds.load();
        Capture::Notify(VK_ERROR_DEVICE_LOST);
        Capture::JoinPending();
        Check(fake.queries == queries && fake.milliseconds == elapsed, "repeated error restarted the capture wait");
        if (scenario == "status-sequence") Check(queries == 4 && elapsed == 150, "Unknown or NotStarted ended collection early");
        if (scenario == "timeout") Check(elapsed >= 3000 && elapsed <= 3050, "capture timeout was not the three-second deadline");
    }
    if (scenario == "query-failed" || scenario == "timeout") {
        Check(fake.disables == 0, "failed or unfinished collection called Disable");
        CheckQuarantine();
    } else CheckNormalRelease();
}

void CallbackCases(std::string_view scenario, const Output& output) {
    {
        Capture::DeviceLease lease;
        fake.throwWrite = scenario == "callback-throws";
        if (scenario == "callback-io-fails") {
            // Replace the session's empty output directory with a regular file, so opening
            // any dump filename beneath it fails without relying on host ACL privileges.
            std::error_code ignored;
            fs::remove(output.path, ignored);
            std::ofstream obstruction(output.path, std::ios::binary);
            Check(static_cast<bool>(obstruction), "could not create output obstruction");
        }
        InvokeDump();
        if (scenario == "callback-bytes") {
            InvokeDump();
            CheckDump(output, 2);
        }
        Check(fake.writes >= 1, "synthetic dump did not reach callback");
    }
    CheckNormalRelease();
}

void HeldCallback(std::string_view scenario, const Output& output) {
    Gate gate;
    fake.writeGate = &gate;
    fake.statuses = {GFSDK_Aftermath_CrashDump_Status_CollectingData};
    auto lease = std::make_unique<Capture::DeviceLease>();
    std::thread callback(InvokeDump);
    const bool entered = gate.Wait();
    if (!entered) { gate.Release(); callback.join(); Check(false, "dump callback did not enter"); }
    // Even a lying/mock Finished status cannot authorize freeing an entered callback.
    if (scenario == "held-callback-finished") fake.statuses = {GFSDK_Aftermath_CrashDump_Status_Finished};
    auto waiter = std::async(std::launch::async, [&] { Capture::Notify(VK_ERROR_DEVICE_LOST); lease.reset(); });
    const bool returned = waiter.wait_for(2s) == std::future_status::ready;
    const bool retained = fake.disables == 0 && fake.unloads == 0 && fake.destroyed == 0;
    gate.Release();
    callback.join();
    waiter.get();
    Check(returned, "capture waited for a held callback after its bounded status wait");
    Check(retained, "session disabled or freed while a callback was entered");
    if (scenario == "held-callback") Check(fake.milliseconds >= 3000 && fake.milliseconds <= 3050, "held callback changed timeout deadline");
    CheckDump(output, 1);
    CheckQuarantine();
}

void ConcurrentDeadline() {
    Gate gate;
    fake.sleepGate = &gate;
    fake.statuses = {GFSDK_Aftermath_CrashDump_Status_NotStarted};
    {
        Capture::DeviceLease lease;
        auto first = std::async(std::launch::async, [] { Capture::Notify(VK_ERROR_DEVICE_LOST); });
        const bool entered = gate.Wait();
        if (!entered) { gate.Release(); first.get(); Check(false, "first waiter never slept"); }
        fake.milliseconds = 2500;
        std::promise<void> joining;
        auto second = std::async(std::launch::async, [&] { joining.set_value(); Capture::Notify(VK_ERROR_DEVICE_LOST); });
        joining.get_future().wait();
        const bool joined = second.wait_for(50ms) == std::future_status::timeout;
        gate.Release();
        first.get();
        second.get();
        Check(joined, "concurrent caller returned while the original capture was still pending");
        Check(fake.milliseconds >= 3000 && fake.milliseconds <= 3100, "concurrent caller extended the original deadline");
    }
    Check(fake.disables == 0, "timed-out concurrent callers disabled capture");
    CheckQuarantine();
}

void SerializedAcquire() {
    Gate gate;
    fake.disableGate = &gate;
    auto first = std::make_unique<Capture::DeviceLease>();
    auto releasing = std::async(std::launch::async, [&] { first.reset(); });
    const bool entered = gate.Wait();
    if (!entered) { gate.Release(); releasing.get(); Check(false, "last release did not enter Disable"); }
    std::promise<void> starting;
    auto acquiring = std::async(std::launch::async, [&] { starting.set_value(); return std::make_unique<Capture::DeviceLease>(); });
    starting.get_future().wait();
    const bool serialized = acquiring.wait_for(50ms) == std::future_status::timeout && fake.enables == 1;
    gate.Release();
    releasing.get();
    auto second = acquiring.get();
    Check(serialized, "new Enable overlapped final Disable");
    second.reset();
    Check(fake.enables == 2, "clean released session could not be reacquired");
    CheckNormalRelease(2);
}

void AbandonLocked(const Output& output) {
    Gate lifecycle, callbackGate;
    fake.writeGate = &callbackGate;
    auto lease = std::make_unique<Capture::DeviceLease>();
    std::thread callback(InvokeDump);
    const bool callbackEntered = callbackGate.Wait();
    if (!callbackEntered) { callbackGate.Release(); callback.join(); Check(false, "callback did not enter before abandonment"); }
    std::thread locked([&] { Capture::Testing::WithLifecycleLock([](void* gate) { static_cast<Gate*>(gate)->Hold(); }, &lifecycle); });
    const bool lockEntered = lifecycle.Wait();
    if (!lockEntered) {
        lifecycle.Release(); callbackGate.Release(); locked.join(); callback.join();
        Check(false, "test did not hold lifecycle mutex");
    }
    const auto activity = fake.Activity();
    auto abandoning = std::async(std::launch::async, [&] {
        Capture::Abandon();
        Capture::Notify(VK_ERROR_DEVICE_LOST);
        Capture::JoinPending();
        lease.reset();
    });
    const bool returned = abandoning.wait_for(2s) == std::future_status::ready;
    const bool untouched = fake.Activity() == activity;
    lifecycle.Release(); callbackGate.Release();
    locked.join(); callback.join(); abandoning.get();
    Check(returned && untouched, "abandonment locked, waited, invoked SDK or freed callback state");
    Check(fake.disables == 0 && fake.unloads == 0 && fake.destroyed == 0, "abandoned session was not retained");
    CheckDump(output, 1);
}

void InitializationFailure(std::string_view scenario) {
    if (scenario == "load-failed") fake.loadOkay = false;
    if (scenario == "pin-failed") fake.pinOkay = false;
    if (scenario == "enable-failed") fake.enableResult = GFSDK_Aftermath_Result_FAIL_Unknown;
    if (scenario == "relative-output") SetEnv("APS5_AFTERMATH_DIR", "relative-aftermath-output");
    bool rejected = false;
    try { Capture::DeviceLease lease; } catch (const std::exception&) { rejected = true; }
    Check(rejected, "requested unavailable capture silently succeeded");
    if (scenario != "enable-failed") Check(fake.enables == 0, "initialization failure reached Enable");
    Check(fake.disables == 0, "initialization failure disabled an unenabled session");
    if (scenario == "enable-failed") {
        CheckQuarantine();
        InvokeDump(); // Failed Enable may still have retained the callback, so storage must survive.
    }
}
#endif
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "pass one isolated scenario name");
        const std::string_view scenario(argv[1]);
        Output output(scenario);
        SetEnv("APS5_AFTERMATH", "1");
#if defined(ANYPS5_AFTERMATH_TESTING)
        auto hooks = Hooks();
        if (scenario == "missing-export") hooks.status = nullptr;
        Capture::Testing::Install(hooks);
        if (scenario == "unset") {
            SetEnv("APS5_AFTERMATH", nullptr);
            { Capture::DeviceLease lease; Check(!static_cast<bool>(lease), "unset capture produced an enabled lease"); }
            SetEnv("APS5_AFTERMATH", "1");
            { Capture::DeviceLease late; Check(!static_cast<bool>(late), "late opt-in enabled capture after an earlier untracked device lease"); }
            Capture::Notify(VK_ERROR_DEVICE_LOST);
            Capture::JoinPending();
            Capture::Abandon();
            Check(fake.Activity() == 0 && !fs::exists(output.path), "unset capture had loader, SDK, clock or output activity");
        } else if (scenario == "lifecycle") Lifecycle();
        else if (scenario == "original-error") {
            Capture::DeviceLease lease;
            bool preserved = false;
            try { AgcDriver::Graphics::Check(VK_ERROR_DEVICE_LOST, "injected fence"); }
            catch (const std::runtime_error& error) { preserved = std::string_view(error.what()) == "AGC graphics: injected fence: Vulkan result -4"; }
            Check(preserved && fake.queries == 1, "capture changed the common Vulkan error or bypassed its wait");
            const auto queries = fake.queries.load();
            preserved = false;
            try { AgcDriver::Graphics::Check(VK_ERROR_OUT_OF_HOST_MEMORY, "allocation"); }
            catch (const std::runtime_error& error) { preserved = std::string_view(error.what()) == "AGC graphics: allocation: Vulkan result -1"; }
            Check(preserved && fake.queries == queries, "non-device-loss error changed or queried crash status");
        }
        else if (scenario == "status-sequence" || scenario == "collect-failed" || scenario == "query-failed" || scenario == "timeout") StatusCases(scenario);
        else if (scenario == "callback-bytes" || scenario == "callback-throws" || scenario == "callback-io-fails") CallbackCases(scenario, output);
        else if (scenario == "held-callback" || scenario == "held-callback-finished") HeldCallback(scenario, output);
        else if (scenario == "concurrent-deadline") ConcurrentDeadline();
        else if (scenario == "serialized-acquire") SerializedAcquire();
        else if (scenario == "abandon-locked") AbandonLocked(output);
        else if (scenario == "disable-failed") {
            fake.disableResult = GFSDK_Aftermath_Result_FAIL_Unknown;
            { Capture::DeviceLease lease; }
            Check(fake.disables == 1, "last lease did not attempt Disable");
            CheckQuarantine();
            InvokeDump(); // Context and callback code must still be valid after failed Disable.
            CheckDump(output, 1);
        } else if (scenario == "load-failed" || scenario == "pin-failed" || scenario == "enable-failed" || scenario == "relative-output" || scenario == "missing-export") InitializationFailure(scenario);
        else Check(false, "unknown enabled capture scenario");
#else
        Check(scenario == "disabled", "SDK-disabled binary accepts only disabled");
        { Capture::DeviceLease first, second; Check(!static_cast<bool>(first) && !static_cast<bool>(second), "disabled build returned an enabled lease"); }
        Capture::Notify(VK_ERROR_DEVICE_LOST);
        Capture::JoinPending();
        Capture::Abandon();
        Check(!fs::exists(output.path), "disabled build created capture output");
#endif
        std::cout << "Aftermath CPU " << scenario << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Aftermath CPU regression failed: " << error.what() << '\n';
        return 1;
    }
}
