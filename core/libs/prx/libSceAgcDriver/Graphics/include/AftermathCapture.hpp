#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_AFTERMATHCAPTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_AFTERMATHCAPTURE_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#if defined(ANYPS5_AFTERMATH_TESTING)
#include <GFSDK_Aftermath_GpuCrashDump.h>
#include <chrono>
#endif

namespace AgcDriver::Graphics::Aftermath {

#if defined(ANYPS5_AFTERMATH_ENABLED)
struct Session;

class DeviceLease {
public:
    DeviceLease();
    ~DeviceLease();
    DeviceLease(const DeviceLease&) = delete;
    DeviceLease& operator=(const DeviceLease&) = delete;
    explicit operator bool() const noexcept { return session != nullptr; }
private:
    Session* session = nullptr;
};

void NotifyDeviceLost() noexcept;
inline void Notify(VkResult result) noexcept {
    if (result == VK_ERROR_DEVICE_LOST) NotifyDeviceLost();
}
void JoinPending() noexcept;
void Abandon() noexcept;
#else
class DeviceLease {
public:
    explicit operator bool() const noexcept { return false; }
};
inline void Notify(VkResult) noexcept {}
inline void JoinPending() noexcept {}
inline void Abandon() noexcept {}
#endif

#if defined(ANYPS5_AFTERMATH_TESTING)
namespace Testing {
struct Hooks {
    PFN_GFSDK_Aftermath_EnableGpuCrashDumps enable = nullptr;
    PFN_GFSDK_Aftermath_DisableGpuCrashDumps disable = nullptr;
    PFN_GFSDK_Aftermath_GetCrashDumpStatus status = nullptr;
    bool (*load)() = nullptr;
    bool (*pin)() = nullptr;
    void (*unload)() = nullptr;
    std::chrono::steady_clock::time_point (*now)() = nullptr;
    void (*sleep)() = nullptr;
    void (*beforeWrite)() = nullptr;
    void (*destroyed)() = nullptr;
};
void Install(const Hooks& hooks);
void WithLifecycleLock(void (*body)(void*), void* data);
}
#endif

}
#endif
