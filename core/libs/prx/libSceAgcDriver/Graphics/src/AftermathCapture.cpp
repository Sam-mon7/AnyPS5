#include "prx/libSceAgcDriver/Graphics/include/AftermathCapture.hpp"
#include <GFSDK_Aftermath_GpuCrashDump.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#if !defined(ANYPS5_AFTERMATH_TESTING)
#include "AftermathSdkPath.hpp"
#endif

namespace AgcDriver::Graphics::Aftermath {
namespace {
using Clock = std::chrono::steady_clock;
constinit std::atomic<bool> abandoned{false};
struct Control;
constinit std::atomic<Control*> published{nullptr};
#if defined(ANYPS5_AFTERMATH_TESTING)
const Testing::Hooks* testHooks = nullptr;
#endif

// Callback diagnostics bypass CRT/profiling queues and cannot throw across the SDK ABI.
void callbackError(const char* text, DWORD size) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, size, &written, nullptr);
}

std::filesystem::path outputDirectory() {
    const auto* value = std::getenv("APS5_AFTERMATH_DIR");
    if (value == nullptr || *value == '\0') throw std::runtime_error("Aftermath: APS5_AFTERMATH_DIR must name an absolute local directory");
    auto path = std::filesystem::u8path(value).lexically_normal();
    if (!path.is_absolute() || path.root_name().native().starts_with(L"\\\\")) throw std::runtime_error("Aftermath: output directory must use an absolute local drive path");
    const auto drive = GetDriveTypeW(path.root_path().c_str());
    if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK) throw std::runtime_error("Aftermath: output directory must be on a local drive");
    std::filesystem::create_directories(path);
    return path;
}
}

struct Session {
    const std::filesystem::path output;
    const std::uint64_t serial;
    std::atomic<std::uint64_t> dumps{0};
    std::atomic<unsigned> callbacks{0};
    HMODULE library = nullptr;
    PFN_GFSDK_Aftermath_EnableGpuCrashDumps enable = nullptr;
    PFN_GFSDK_Aftermath_DisableGpuCrashDumps disable = nullptr;
    PFN_GFSDK_Aftermath_GetCrashDumpStatus status = nullptr;
    std::size_t owners = 0;
    bool loaded = false;
    bool enableAttempted = false;
    bool quarantined = false;
    bool lost = false;
    bool finished = false;
    Clock::time_point deadline{};
#if defined(ANYPS5_AFTERMATH_TESTING)
    const Testing::Hooks* hooks = testHooks;
#endif

    explicit Session(std::uint64_t id) : output(outputDirectory()), serial(id) {}
    ~Session() {
        if (loaded) {
#if defined(ANYPS5_AFTERMATH_TESTING)
            if (hooks->unload) hooks->unload();
#else
            FreeLibrary(library);
#endif
        }
#if defined(ANYPS5_AFTERMATH_TESTING)
        if (hooks && hooks->destroyed) hooks->destroyed();
#endif
    }

    Clock::time_point now() const {
#if defined(ANYPS5_AFTERMATH_TESTING)
        if (hooks->now) return hooks->now();
#endif
        return Clock::now();
    }
    void sleep() const {
#if defined(ANYPS5_AFTERMATH_TESTING)
        if (hooks->sleep) { hooks->sleep(); return; }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    static void GFSDK_AFTERMATH_CALL dump(const void* bytes, std::uint32_t size, void* context) noexcept {
        auto& self = *static_cast<Session*>(context);
        self.callbacks.fetch_add(1, std::memory_order_acq_rel);
        struct Finished {
            Session& session;
            ~Finished() { session.callbacks.fetch_sub(1, std::memory_order_release); }
        } done{self};
        HANDLE file = INVALID_HANDLE_VALUE;
        try {
#if defined(ANYPS5_AFTERMATH_TESTING)
            if (self.hooks->beforeWrite) self.hooks->beforeWrite();
#endif
            const auto index = self.dumps.fetch_add(1, std::memory_order_relaxed) + 1;
            const auto name = L"aftermath-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(self.serial) + L"-" + std::to_wstring(index) + L".nv-gpudmp";
            const auto path = self.output / name;
            file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("dump create failed");
            DWORD written = 0;
            const bool ok = WriteFile(file, bytes, size, &written, nullptr) != FALSE && written == size;
            const bool closed = CloseHandle(file) != FALSE;
            file = INVALID_HANDLE_VALUE;
            if (!ok || !closed) throw std::runtime_error("dump write failed");
        } catch (...) {
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            constexpr char message[] = "[aftermath] local crash dump write failed\n";
            callbackError(message, sizeof(message) - 1);
        }
    }

    void initialize() {
#if defined(ANYPS5_AFTERMATH_TESTING)
        if (hooks == nullptr || hooks->load == nullptr || !hooks->load()) throw std::runtime_error("Aftermath: SDK load failed");
        loaded = true;
        enable = hooks->enable;
        disable = hooks->disable;
        status = hooks->status;
#else
        library = LoadLibraryExW(AftermathSdkPath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (library == nullptr) throw std::runtime_error("Aftermath: SDK DLL load failed");
        loaded = true;
        enable = std::bit_cast<PFN_GFSDK_Aftermath_EnableGpuCrashDumps>(GetProcAddress(library, "GFSDK_Aftermath_EnableGpuCrashDumps"));
        disable = std::bit_cast<PFN_GFSDK_Aftermath_DisableGpuCrashDumps>(GetProcAddress(library, "GFSDK_Aftermath_DisableGpuCrashDumps"));
        status = std::bit_cast<PFN_GFSDK_Aftermath_GetCrashDumpStatus>(GetProcAddress(library, "GFSDK_Aftermath_GetCrashDumpStatus"));
#endif
        if (!enable || !disable || !status) throw std::runtime_error("Aftermath: required SDK export missing");
#if defined(ANYPS5_AFTERMATH_TESTING)
        if (!hooks->pin || !hooks->pin()) throw std::runtime_error("Aftermath: callback module pin failed");
#else
        HMODULE callbackModule = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&dump), &callbackModule)) throw std::runtime_error("Aftermath: callback module pin failed");
#endif
        enableAttempted = true;
        const auto result = enable(GFSDK_Aftermath_Version_API, GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
                GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default, dump, nullptr, nullptr, nullptr, this);
        if (!GFSDK_Aftermath_SUCCEED(result)) throw std::runtime_error("Aftermath: EnableGpuCrashDumps failed: " + std::to_string(static_cast<unsigned>(result)));
        std::fprintf(stderr, "[aftermath] enabled Vulkan capture session=%llu output=%s; shader debug disabled\n", static_cast<unsigned long long>(serial), output.string().c_str());
    }

    void quarantine(const char* reason) noexcept {
        quarantined = true;
        finished = true;
        std::fprintf(stderr, "[aftermath] %s; retaining SDK/callback state until process exit\n", reason);
    }

    // Called under the one lifecycle mutex. Callbacks never need it; other errors join this wait.
    void wait() {
        if (!lost || finished || quarantined || abandoned.load(std::memory_order_acquire)) return;
        while (!abandoned.load(std::memory_order_acquire)) {
            if (now() >= deadline) { quarantine("capture status deadline expired (3000 ms)"); return; }
            GFSDK_Aftermath_CrashDump_Status state = GFSDK_Aftermath_CrashDump_Status_Unknown;
            const auto result = status(&state);
            if (!GFSDK_Aftermath_SUCCEED(result)) { quarantine("capture status query failed"); return; }
            if (state == GFSDK_Aftermath_CrashDump_Status_Finished || state == GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed) {
                finished = true;
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now() - (deadline - std::chrono::seconds(3))).count();
                std::fprintf(stderr, "[aftermath] capture status=%u session=%llu wait=%lld ms\n", static_cast<unsigned>(state), static_cast<unsigned long long>(serial), static_cast<long long>(elapsed));
                return;
            }
            sleep();
        }
    }
};

namespace {
struct Control {
    std::mutex mutex;
    Session* session = nullptr;
    std::uint64_t serial = GetTickCount64();
};

Control& control() {
    // ponytail: diagnostic holder never destructs; explicit last-device release owns SDK cleanup.
    static auto* const value = new Control;
    published.store(value, std::memory_order_release);
    return *value;
}
}

DeviceLease::DeviceLease() {
    // Decide before the first VkDevice; later environment changes cannot create an unleased device
    // in an enabled session or enable collection after an earlier device already existed.
    static const bool requested = std::getenv("APS5_AFTERMATH") != nullptr;
    if (!requested) return;
    if (abandoned.load(std::memory_order_acquire)) throw std::runtime_error("Aftermath: capture was abandoned during teardown");
    auto& owner = control();
    std::lock_guard lock(owner.mutex);
    if (abandoned.load(std::memory_order_acquire)) throw std::runtime_error("Aftermath: capture was abandoned during teardown");
    if (owner.session && owner.session->quarantined) throw std::runtime_error("Aftermath: failed capture session cannot be re-enabled");
    if (!owner.session) {
        auto created = std::make_unique<Session>(++owner.serial);
        try {
            created->initialize();
        } catch (...) {
            if (created->enableAttempted) {
                // A failed SDK initialization has no documented partial-callback drain guarantee.
                created->quarantined = true;
                owner.session = created.release();
            }
            throw;
        }
        owner.session = created.release();
    }
    session = owner.session;
    ++session->owners;
}

DeviceLease::~DeviceLease() {
    if (!session || abandoned.load(std::memory_order_acquire)) return;
    try {
        auto* owner = published.load(std::memory_order_acquire);
        std::lock_guard lock(owner->mutex);
        if (abandoned.load(std::memory_order_acquire)) return;
        if (--session->owners != 0) return;
        session->wait();
        if (abandoned.load(std::memory_order_acquire) || session->quarantined) return;
        if (session->callbacks.load(std::memory_order_acquire) != 0) { session->quarantine("callback still active at shutdown"); return; }
        const auto result = session->disable();
        if (abandoned.load(std::memory_order_acquire)) return;
        if (!GFSDK_Aftermath_SUCCEED(result)) { session->quarantine("DisableGpuCrashDumps failed"); return; }
        if (session->callbacks.load(std::memory_order_acquire) != 0) { session->quarantine("callback still active after Disable"); return; }
        std::fprintf(stderr, "[aftermath] disabled after last device session=%llu\n", static_cast<unsigned long long>(session->serial));
        delete session;
        owner->session = nullptr;
    } catch (...) {
        Abandon();
    }
}

void NotifyDeviceLost() noexcept {
    if (abandoned.load(std::memory_order_acquire)) return;
    auto* owner = published.load(std::memory_order_acquire);
    if (owner == nullptr) return;
    try {
        std::lock_guard lock(owner->mutex);
        if (abandoned.load(std::memory_order_acquire)) return;
        auto* value = owner->session;
        if (!value || value->quarantined) return;
        if (!value->lost) {
            value->lost = true;
            value->deadline = value->now() + std::chrono::seconds(3);
        }
        value->wait();
    } catch (...) {
        Abandon();
    }
}

void JoinPending() noexcept {
    if (abandoned.load(std::memory_order_acquire)) return;
    auto* owner = published.load(std::memory_order_acquire);
    if (owner == nullptr) return;
    try {
        std::lock_guard lock(owner->mutex);
        if (!abandoned.load(std::memory_order_acquire) && owner->session) owner->session->wait();
    } catch (...) {
        Abandon();
    }
}

void Abandon() noexcept { abandoned.store(true, std::memory_order_release); }

#if defined(ANYPS5_AFTERMATH_TESTING)
namespace Testing {
void Install(const Hooks& hooks) {
    if (published.load(std::memory_order_acquire) || testHooks) throw std::runtime_error("Aftermath test hooks must be installed once before capture");
    testHooks = &hooks;
}
void WithLifecycleLock(void (*body)(void*), void* data) {
    auto& owner = control();
    std::lock_guard lock(owner.mutex);
    body(data);
}
}
#endif
}
