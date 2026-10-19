#include "KytyGpuApi.hpp"

#include "common/archive.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "graphics/presentation/performanceOverlay.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/renderDoc.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window/windowInternal.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/agc.h"
#include "loader/systemContent.h"
#include "loader/timer.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <windows.h>
#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace EventQueue = Libs::LibKernel::EventQueue;
using Libs::Graphics::PageFaultAccess;
using Libs::Graphics::RenderContext;
using Mode = Common::VirtualMemory::Mode;

constexpr std::uint64_t PageBytes = 4096;

const KytyGpuHostTable* host = nullptr;
RenderContext* resources = nullptr;
Libs::Graphics::WindowContext* window = nullptr;
SDL_Window hostWindow;
std::atomic<bool> started{false};
std::once_flag configured;
std::once_flag shaders;
void* faultHandler = nullptr;

struct Counts {
    std::atomic<std::uint64_t> dcbs{0};
    std::atomic<std::uint64_t> acbs{0};
    std::atomic<std::uint64_t> adds{0};
    std::atomic<std::uint64_t> graphicsTriggers{0};
    std::atomic<std::uint64_t> videoTriggers{0};
    std::atomic<std::uint64_t> failedTriggers{0};
    std::atomic<std::uint64_t> faults{0};
    std::atomic<std::uint64_t> lateMaps{0};
    std::atomic<std::uint64_t> raceRetries{0};
};

Counts counts;

struct Protection {
    DWORD original;
    Mode mode;
};

std::mutex protectionMutex;
std::unordered_map<std::uint64_t, Protection> protections;

bool committed(std::uint64_t address, MEMORY_BASIC_INFORMATION& info) {
    return VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == sizeof(info) && info.State == MEM_COMMIT;
}

std::uint64_t regionEnd(const MEMORY_BASIC_INFORMATION& info) {
    return reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
}

DWORD hostProtection(Mode mode, DWORD original) {
    const bool execute = (original & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    switch (mode) {
    case Mode::NoAccess: return PAGE_NOACCESS;
    case Mode::Read: return execute ? PAGE_EXECUTE_READ : PAGE_READONLY;
    default: return original;
    }
}

bool protectPages(std::uint64_t address, std::uint64_t bytes, Mode mode) {
    const auto begin = address & ~(PageBytes - 1);
    const auto end = (address + bytes + PageBytes - 1) & ~(PageBytes - 1);
    std::lock_guard lock(protectionMutex);
    for (auto page = begin; page < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!committed(page, info)) {
            page = VirtualQuery(reinterpret_cast<const void*>(page), &info, sizeof(info)) == sizeof(info) ? std::max(page + PageBytes, regionEnd(info)) : page + PageBytes;
            continue;
        }
        const auto stop = std::min(end, regionEnd(info));
        for (auto at = page; at < stop; at += PageBytes) {
            const auto found = protections.find(at);
            const DWORD original = found != protections.end() ? found->second.original : info.Protect;
            const bool open = mode == Mode::ReadWrite || mode == Mode::ExecuteReadWrite;
            const DWORD wanted = open ? original : hostProtection(mode, original);
            DWORD previous = 0;
            if (!VirtualProtect(reinterpret_cast<void*>(at), PageBytes, wanted, &previous)) return false;
            if (open) {
                if (found != protections.end()) protections.erase(found);
            } else if (found != protections.end()) {
                found->second.mode = mode;
            } else {
                protections.emplace(at, Protection{original, mode});
            }
        }
        page = stop;
    }
    return true;
}

template <typename Copy>
bool backing(std::uint64_t address, std::uint64_t bytes, bool sparse, Copy&& copy) {
    for (auto at = address; at < address + bytes;) {
        MEMORY_BASIC_INFORMATION info{};
        const bool live = committed(at, info);
        const auto stop = std::min(address + bytes, regionEnd(info) > at ? regionEnd(info) : at + PageBytes);
        if (!live) {
            if (!sparse) return false;
            copy(at, stop - at, false);
            at = stop;
            continue;
        }
        for (auto page = at; page < stop;) {
            const auto next = std::min(stop, (page & ~(PageBytes - 1)) + PageBytes);
            std::unique_lock lock(protectionMutex);
            const auto found = protections.find(page & ~(PageBytes - 1));
            if (found == protections.end()) {
                lock.unlock();
                copy(page, next - page, true);
            } else {
                DWORD previous = 0;
                VirtualProtect(reinterpret_cast<void*>(page & ~(PageBytes - 1)), PageBytes, found->second.original, &previous);
                copy(page, next - page, true);
                VirtualProtect(reinterpret_cast<void*>(page & ~(PageBytes - 1)), PageBytes, previous, &previous);
            }
            page = next;
        }
        at = stop;
    }
    return true;
}

bool permits(std::uint64_t address, bool write) {
    MEMORY_BASIC_INFORMATION info{};
    if (!committed(address, info) || (info.Protect & PAGE_GUARD) != 0) return false;
    const DWORD protection = info.Protect & 0xffu;
    const bool writable = protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_WRITECOPY;
    return write ? writable : writable || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
}

LONG CALLBACK handleFault(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const auto address = static_cast<std::uint64_t>(record->ExceptionInformation[1]);
    {
        std::lock_guard lock(protectionMutex);
        if (!protections.contains(address & ~(PageBytes - 1))) {
            if (record->ExceptionInformation[0] == 8 || !permits(address, record->ExceptionInformation[0] == 1)) return EXCEPTION_CONTINUE_SEARCH;
            counts.raceRetries.fetch_add(1, std::memory_order_relaxed);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    counts.faults.fetch_add(1, std::memory_order_relaxed);
    const auto access = record->ExceptionInformation[0] == 1 ? PageFaultAccess::Write : PageFaultAccess::Read;
    if (resources == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    if (resources->HandleFault(access, address)) return EXCEPTION_CONTINUE_EXECUTION;
    MEMORY_BASIC_INFORMATION region{};
    if (!committed(address, region)) return EXCEPTION_CONTINUE_SEARCH;
    constexpr std::uint64_t block = 4ull << 20u;
    const auto begin = std::max(address & ~(block - 1), reinterpret_cast<std::uint64_t>(region.BaseAddress));
    const auto end = std::min((address & ~(block - 1)) + block, regionEnd(region));
    counts.lateMaps.fetch_add(1, std::memory_order_relaxed);
    resources->MapMemory(begin, end - begin);
    return resources->HandleFault(access, address) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

struct TerminateOnFatal {
    static constexpr const char* name = "AnyPS5 fatal exit";
    static void initialize() {}
    static void emergency_shutdown() {
        std::fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), 1);
    }
};

struct Cookie {
    EventQueue::KernelFilter filter;
};

KytyGpuEvent toHost(const EventQueue::KernelEvent& event) {
    return {event.ident, event.filter, event.flags, event.fflags, event.data, event.udata};
}

EventQueue::KernelEvent fromHost(const KytyGpuEvent& event) {
    EventQueue::KernelEvent result;
    result.ident = event.ident;
    result.filter = event.filter;
    result.flags = event.flags;
    result.fflags = event.fflags;
    result.data = event.data;
    result.udata = event.udata;
    return result;
}

EventQueue::KernelEqueueEvent load(const Cookie& cookie, const KytyGpuEventView* view) {
    EventQueue::KernelEqueueEvent event;
    KytyGpuEvent current{};
    host->readEvent(view, &event.triggered, &current);
    event.event = fromHost(current);
    event.filter = cookie.filter;
    const auto count = host->pendingCount(view);
    for (std::size_t index = 0; index < count; ++index) {
        KytyGpuEvent pending{};
        host->pendingAt(view, index, &pending);
        event.pending_events.push_back(fromHost(pending));
    }
    return event;
}

void store(const EventQueue::KernelEqueueEvent& event, KytyGpuEventView* view) {
    const auto current = toHost(event.event);
    host->writeEvent(view, event.triggered, &current);
    std::vector<KytyGpuEvent> pending;
    pending.reserve(event.pending_events.size());
    for (const auto& item : event.pending_events) pending.push_back(toHost(item));
    host->setPending(view, pending.data(), pending.size());
}

void hookTrigger(void* cookie, KytyGpuEventView* view, void* triggerData) {
    const auto& state = *static_cast<Cookie*>(cookie);
    auto event = load(state, view);
    state.filter.trigger_func(&event, triggerData);
    store(event, view);
}

void hookReset(void* cookie, KytyGpuEventView* view) {
    const auto& state = *static_cast<Cookie*>(cookie);
    auto event = load(state, view);
    state.filter.reset_func(&event);
    store(event, view);
}

void hookRemove(void* cookie, std::int64_t eq, KytyGpuEventView* view) {
    const auto& state = *static_cast<Cookie*>(cookie);
    auto event = load(state, view);
    state.filter.delete_event_func(eq, &event);
}

void hookRelease(void* cookie) {
    delete static_cast<Cookie*>(cookie);
}

constexpr KytyGpuEventHooks eventHooks{&hookTrigger, &hookReset, &hookRemove, &hookRelease};

std::chrono::steady_clock::time_point startTime() {
    static const auto start = std::chrono::steady_clock::now();
    return start;
}

void configure(std::uint32_t width, std::uint32_t height) {
    std::call_once(configured, [&] {
        Config::Initialize();
        Config::ConfigOptions options;
        options.screen_width = width;
        options.screen_height = height;
        const char* log = std::getenv("APS5_KYTY_GPU_LOG");
        options.printf_direction = log != nullptr && std::string_view(log) == "1" ? Config::LogDirection::Console : Config::LogDirection::Silent;
        options.present_mode = Config::PresentMode::Fifo;
        Config::Load(options);
        Log::Initialize();
    });
}

void ensureShaders() {
    std::call_once(shaders, [] { Libs::Graphics::ShaderInit(); });
}

void* videoOutFunction(std::string_view name) {
    namespace V = Libs::VideoOut;
    static const std::unordered_map<std::string_view, void*> functions{
        {"Open", reinterpret_cast<void*>(&V::VideoOutOpen)},
        {"Close", reinterpret_cast<void*>(&V::VideoOutClose)},
        {"SetFlipMaster", reinterpret_cast<void*>(&V::VideoOutSetFlipMaster)},
        {"AddBufferAttributeOption", reinterpret_cast<void*>(&V::VideoOutAddBufferAttributeOption)},
        {"SetBufferAttribute2", reinterpret_cast<void*>(&V::VideoOutSetBufferAttribute2)},
        {"SetFlipRate", reinterpret_cast<void*>(&V::VideoOutSetFlipRate)},
        {"AddFlipEvent", reinterpret_cast<void*>(&V::VideoOutAddFlipEvent)},
        {"AddVblankEvent", reinterpret_cast<void*>(&V::VideoOutAddVblankEvent)},
        {"AddPreVblankStartEvent", reinterpret_cast<void*>(&V::VideoOutAddPreVblankStartEvent)},
        {"AddOutputModeEvent", reinterpret_cast<void*>(&V::VideoOutAddOutputModeEvent)},
        {"DeleteFlipEvent", reinterpret_cast<void*>(&V::VideoOutDeleteFlipEvent)},
        {"DeleteVblankEvent", reinterpret_cast<void*>(&V::VideoOutDeleteVblankEvent)},
        {"DeletePreVblankStartEvent", reinterpret_cast<void*>(&V::VideoOutDeletePreVblankStartEvent)},
        {"RegisterBuffers2", reinterpret_cast<void*>(&V::VideoOutRegisterBuffers2)},
        {"SubmitChangeBufferAttribute2", reinterpret_cast<void*>(&V::VideoOutSubmitChangeBufferAttribute2)},
        {"UnregisterBuffers", reinterpret_cast<void*>(&V::VideoOutUnregisterBuffers)},
        {"SubmitFlip", reinterpret_cast<void*>(&V::VideoOutSubmitFlip)},
        {"GetFlipStatus", reinterpret_cast<void*>(&V::VideoOutGetFlipStatus)},
        {"IsFlipPending", reinterpret_cast<void*>(&V::VideoOutIsFlipPending)},
        {"GetVblankStatus", reinterpret_cast<void*>(&V::VideoOutGetVblankStatus)},
        {"GetEventId", reinterpret_cast<void*>(&V::VideoOutGetEventId)},
        {"GetEventData", reinterpret_cast<void*>(&V::VideoOutGetEventData)},
        {"GetEventCount", reinterpret_cast<void*>(&V::VideoOutGetEventCount)},
        {"WaitVblank", reinterpret_cast<void*>(&V::VideoOutWaitVblank)},
        {"GetOutputStatus", reinterpret_cast<void*>(&V::VideoOutGetOutputStatus)},
        {"AllowOutputResolutionWqhdDetection", reinterpret_cast<void*>(&V::VideoOutAllowOutputResolutionWqhdDetection)},
        {"VrrPegToFixedRate", reinterpret_cast<void*>(&V::VideoOutVrrPegToFixedRate)},
        {"VrrUnpegFromFixedRate", reinterpret_cast<void*>(&V::VideoOutVrrUnpegFromFixedRate)},
        {"InitializeOutputOptions", reinterpret_cast<void*>(&V::VideoOutInitializeOutputOptions)},
        {"IsOutputSupported", reinterpret_cast<void*>(&V::VideoOutIsOutputSupported)},
        {"ConfigureOutput", reinterpret_cast<void*>(&V::VideoOutConfigureOutput)},
        {"SetWindowModeMargins", reinterpret_cast<void*>(&V::VideoOutSetWindowModeMargins)},
        {"LatencyControlWaitBeforeInput", reinterpret_cast<void*>(&V::VideoOutLatencyControlWaitBeforeInput)},
        {"LatencyMeasureSetStartPoint", reinterpret_cast<void*>(&V::VideoOutLatencyMeasureSetStartPoint)},
        {"ColorSettingsSetGamma", reinterpret_cast<void*>(&V::VideoOutColorSettingsSetGamma)},
        {"AdjustColor", reinterpret_cast<void*>(&V::VideoOutAdjustColor)},
    };
    const auto found = functions.find(name);
    return found != functions.end() ? found->second : nullptr;
}

}

namespace Common {

struct ArchiveFile::Private {};

ArchiveFile::~ArchiveFile() = default;

uint64_t ArchiveFile::Size() const {
    return 0;
}

uint64_t ArchiveFile::Tell() const {
    return 0;
}

bool ArchiveFile::Seek(uint64_t) {
    return false;
}

void ArchiveFile::Read(void*, uint32_t, uint32_t* bytes_read) {
    if (bytes_read != nullptr) *bytes_read = 0;
}

bool IsArchivePath(const std::filesystem::path&) {
    return false;
}

std::optional<File::Info> GetArchiveInfo(const std::filesystem::path&) {
    return std::nullopt;
}

std::vector<File::DirEntry> GetArchiveDirEntries(const std::filesystem::path&) {
    return {};
}

std::filesystem::path GetArchiveHostPath(const std::filesystem::path& path) {
    return path;
}

std::unique_ptr<ArchiveFile> OpenArchiveFile(const std::filesystem::path&) {
    return nullptr;
}

}

namespace Libs::Graphics {

void GraphicsDbgDumpDcb(const char*, uint32_t, const uint32_t*) {}

namespace Gen5 {

bool AgcIsInternalDataPacket(uint32_t cmd_id, const uint32_t* payload) {
    constexpr uint32_t internalDataRegister = 0x342u;
    constexpr uint32_t drawIndirectMultiBegin = 0xc6000008u;
    constexpr uint32_t drawIndirectMultiEnd = 0xc6000000u;
    constexpr uint32_t waitUserDataBegin32 = 0xc8010000u;
    constexpr uint32_t waitUserDataBegin64 = 0xc8020000u;
    constexpr uint32_t waitUserDataEnd = 0xc8000000u;
    constexpr uint32_t waitUserDataBeginDwords = 4u;
    constexpr uint32_t internalDataDwords = 3u;
    if (payload == nullptr || ((cmd_id >> 8u) & 0xffu) != Pm4::IT_SET_UCONFIG_REG || payload[0] != internalDataRegister) {
        return false;
    }
    const auto dwords = KYTY_PM4_LEN(cmd_id);
    bool internal = false;
    if (dwords == waitUserDataBeginDwords) {
        const auto tag = payload[1] & 0xffff0000u;
        internal = tag == waitUserDataBegin32 || tag == waitUserDataBegin64;
    } else {
        internal = dwords == internalDataDwords && (payload[1] == waitUserDataEnd || payload[1] == drawIndirectMultiBegin || payload[1] == drawIndirectMultiEnd);
    }
    if (!internal) {
        std::fprintf(stderr, "[kyty-gpu] unknown internal data packet %08x:", cmd_id);
        for (uint32_t index = 0; index + 1 < dwords; ++index) std::fprintf(stderr, " %08x", payload[index]);
        std::fprintf(stderr, "\n");
    }
    return internal;
}

}

SystemOverlayVisualState GetSystemOverlayVisualState() noexcept {
    return {false, 0};
}

void ShutdownSystemOverlayInput() {}

struct SystemOverlay::Impl {};

SystemOverlay::SystemOverlay(GraphicContext&) {}

SystemOverlay::~SystemOverlay() = default;

bool SystemOverlay::PrepareFrame(vk::Extent2D, vk::Format, uint32_t) {
    return false;
}

void SystemOverlay::Record(vk::CommandBuffer, vk::ImageView) {}

void SystemOverlay::ReleaseVulkan() {}

bool PerformanceOverlayEnabled() noexcept {
    return false;
}

void PerformanceOverlayRecordFlip(uint32_t, uint32_t) noexcept {}

void RenderDocOnGuestFlip(RenderContext&) {}

}

namespace Libs::LibKernel {

uint64_t KYTY_SYSV_ABI KernelGetTscFrequency() {
    return host->tscFrequency();
}

uint64_t KYTY_SYSV_ABI KernelReadTsc() {
    return host->readTsc();
}

uint64_t KYTY_SYSV_ABI KernelGetProcessTime() {
    return host->processTime();
}

uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounter() {
    return host->processTimeCounter();
}

}

namespace Libs::LibKernel::EventQueue {

KernelEqueueRef KernelPinEqueue(KernelEqueue eq) {
    if (!host->pinEqueue(eq)) return nullptr;
    return KernelEqueueRef(reinterpret_cast<KernelEqueuePrivate*>(static_cast<std::uintptr_t>(eq)), [](KernelEqueuePrivate*) {});
}

int KYTY_SYSV_ABI KernelAddEvent(KernelEqueue eq, const KernelEqueueEvent& event) {
    auto* cookie = new Cookie{event.filter};
    const auto current = toHost(event.event);
    if (counts.adds.fetch_add(1, std::memory_order_relaxed) < 16) {
        std::fprintf(stderr, "[kyty-gpu] add event eq=%lld ident=%llu filter=%d\n", static_cast<long long>(eq), static_cast<unsigned long long>(event.event.ident), event.event.filter);
    }
    return host->addEvent(eq, &current, event.triggered, cookie, &eventHooks, event.filter.trigger_func != nullptr, event.filter.reset_func != nullptr, event.filter.delete_event_func != nullptr);
}

int KYTY_SYSV_ABI KernelTriggerEvent(KernelEqueue eq, uintptr_t ident, int16_t filter, void* trigger_data) {
    const int result = host->triggerEvent(eq, ident, filter, trigger_data);
    (filter == KERNEL_EVFILT_GRAPHICS ? counts.graphicsTriggers : counts.videoTriggers).fetch_add(1, std::memory_order_relaxed);
    if (result != 0 && counts.failedTriggers.fetch_add(1, std::memory_order_relaxed) < 8) {
        std::fprintf(stderr, "[kyty-gpu] trigger eq=%lld ident=%llu filter=%d failed: 0x%08x\n", static_cast<long long>(eq), static_cast<unsigned long long>(ident), filter, static_cast<unsigned>(result));
    }
    return result;
}

int KYTY_SYSV_ABI KernelDeleteEvent(KernelEqueue eq, uintptr_t ident, int16_t filter) {
    return host->deleteEvent(eq, ident, filter);
}

}

namespace Libs::LibKernel::Memory {

bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    return backing(vaddr, size, false, [&](std::uint64_t at, std::uint64_t length, bool) {
        std::memcpy(bytes + (at - vaddr), reinterpret_cast<const void*>(at), static_cast<std::size_t>(length));
    });
}

bool TryReadBufferBacking(uint64_t vaddr, void* data, uint64_t size) {
    if (resources != nullptr && Graphics::GuestRange{vaddr, size}.Valid()) {
        if (!Graphics::GuestGpu::IsGpuThread()) return false;
        auto& buffers = resources->GetBufferCache();
        if (buffers.HasGpuDirtyBytes(vaddr, size)) buffers.ReadMemory(vaddr, size);
    }
    return TryReadBacking(vaddr, data, size);
}

bool CopySparseMemory(uint64_t vaddr, void* data, uint64_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    return backing(vaddr, size, true, [&](std::uint64_t at, std::uint64_t length, bool live) {
        if (live) {
            std::memcpy(bytes + (at - vaddr), reinterpret_cast<const void*>(at), static_cast<std::size_t>(length));
        } else {
            std::memset(bytes + (at - vaddr), 0, static_cast<std::size_t>(length));
        }
    });
}

bool TryReadSparseBacking(uint64_t vaddr, void* data, uint64_t size) {
    return CopySparseMemory(vaddr, data, size);
}

uint64_t ClampRangeSize(uint64_t vaddr, uint64_t size) {
    std::uint64_t covered = 0;
    while (covered < size) {
        MEMORY_BASIC_INFORMATION info{};
        if (!committed(vaddr + covered, info)) break;
        covered = std::min(size, regionEnd(info) - vaddr);
    }
    if (covered == 0) {
        EXIT("Memory: attempted to access invalid address 0x%016" PRIx64 " with size 0x%016" PRIx64 "\n", vaddr, size);
    }
    return covered;
}

void WriteBacking(uint64_t vaddr, const void* data, uint64_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const bool written = backing(vaddr, size, false, [&](std::uint64_t at, std::uint64_t length, bool) {
        std::memcpy(reinterpret_cast<void*>(at), bytes + (at - vaddr), static_cast<std::size_t>(length));
    });
    if (!written) {
        EXIT("Memory: required direct-backing write failed, addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n", vaddr, size);
    }
}

void InstallGpuResources(Graphics::RenderContext* renderer) noexcept {
    resources = renderer;
}

bool ProtectGuestHostMemory(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode) {
    return protectPages(vaddr, size, mode);
}

}

namespace Loader {

bool SystemContentParamSfoGetInt(const char*, int32_t*) {
    return false;
}

bool SystemContentParamSfoGetString(const char*, std::string*) {
    return false;
}

bool SystemContentParamSfoGetString(const char*, char*, size_t) {
    return false;
}

namespace Timer {

double GetTimeMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime()).count();
}

}

}

const char* SDL_GetError() {
    return "KytyPS5 GPU host";
}

void SDL_DestroyWindow(SDL_Window*) {}

void SDL_QuitSubSystem(std::uint32_t) {}

bool SDL_RunOnMainThread(SDL_MainThreadCallback callback, void* userdata, bool) {
    callback(userdata);
    return true;
}

bool SDL_GetWindowSizeInPixels(SDL_Window* target, int* width, int* height) {
    RECT rect{};
    if (target == nullptr || !GetClientRect(static_cast<HWND>(target->hwnd), &rect)) return false;
    *width = rect.right - rect.left;
    *height = rect.bottom - rect.top;
    return true;
}

const char* const* SDL_Vulkan_GetInstanceExtensions(std::uint32_t* count) {
    static const char* const extensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    *count = 2;
    return extensions;
}

void* SDL_Vulkan_GetVkGetInstanceProcAddr() {
    static const HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    return loader != nullptr ? reinterpret_cast<void*>(GetProcAddress(loader, "vkGetInstanceProcAddr")) : nullptr;
}

bool SDL_Vulkan_CreateSurface(SDL_Window* target, VkInstance instance, const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    const auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (getProc == nullptr || target == nullptr) return false;
    const auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(getProc(instance, "vkCreateWin32SurfaceKHR"));
    if (create == nullptr) return false;
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = static_cast<HWND>(target->hwnd);
    return create(instance, &info, allocator, surface) == VK_SUCCESS;
}

extern "C" {

int __gxx_personality_v0(int, int, std::uint64_t, void*, void*) {
    std::abort();
}

__declspec(dllexport) int KytyGpuStart(const KytyGpuHostTable* table, void* hwnd, std::uint32_t width, std::uint32_t height) {
    if (started.load(std::memory_order_acquire)) return 0;
    host = table;
    static_cast<void>(startTime());
    Common::InitializeThreads();
    (new Common::Subsystems())->Initialize<TerminateOnFatal>();
    configure(width, height);
    faultHandler = AddVectoredExceptionHandler(1, &handleFault);
    hostWindow = SDL_Window{hwnd, static_cast<int>(width), static_cast<int>(height)};
    window = new Libs::Graphics::WindowContext();
    window->graphic_ctx.screen_width = width;
    window->graphic_ctx.screen_height = height;
    window->window = &hostWindow;
    window->CreateVulkan();
    auto& presenter = *window->presenter;
    auto& videoOut = Libs::VideoOut::VideoOutInit(width, height, presenter);
    auto& renderer = presenter.Renderer();
    renderer.InitializeGpu(&videoOut);
    ensureShaders();
    started.store(true, std::memory_order_release);
    std::thread([] {
        auto previous = window->loop.presented_frames.load(std::memory_order_relaxed);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            const auto now = window->loop.presented_frames.load(std::memory_order_relaxed);
            std::fprintf(stderr, "[kyty-gpu] %llu presents over 10 s (%.1f fps); totals dcbs=%llu acbs=%llu event-adds=%llu eop-triggers=%llu video-triggers=%llu failed-triggers=%llu faults=%llu late-maps=%llu race-retries=%llu\n", static_cast<unsigned long long>(now - previous), static_cast<double>(now - previous) / 10.0,
                         static_cast<unsigned long long>(counts.dcbs.load()), static_cast<unsigned long long>(counts.acbs.load()), static_cast<unsigned long long>(counts.adds.load()), static_cast<unsigned long long>(counts.graphicsTriggers.load()),
                         static_cast<unsigned long long>(counts.videoTriggers.load()), static_cast<unsigned long long>(counts.failedTriggers.load()), static_cast<unsigned long long>(counts.faults.load()), static_cast<unsigned long long>(counts.lateMaps.load()), static_cast<unsigned long long>(counts.raceRetries.load()));
            previous = now;
        }
    }).detach();
    return 0;
}

__declspec(dllexport) void KytyGpuSubmitDcb(const std::uint32_t* commands, std::uint32_t dwords) {
    counts.dcbs.fetch_add(1, std::memory_order_relaxed);
    resources->GetGpu().Submit(std::span<const std::uint32_t>{commands, dwords}, {});
}

__declspec(dllexport) void KytyGpuSubmitAcb(std::uint32_t queue, const std::uint32_t* commands, std::uint32_t dwords) {
    counts.acbs.fetch_add(1, std::memory_order_relaxed);
    resources->GetGpu().SubmitCompute(queue, std::span<const std::uint32_t>{commands, dwords});
}

__declspec(dllexport) int KytyGpuAddEqEvent(std::int64_t eq, int id, void* udata) {
    return Libs::Graphics::Sync::AddEqEvent(*resources, eq, id, udata);
}

__declspec(dllexport) int KytyGpuDeleteEqEvent(std::int64_t eq, int id, void*) {
    return Libs::Graphics::Sync::DeleteEqEvent(*resources, eq, id);
}

__declspec(dllexport) void KytyGpuMapMemory(std::uint64_t address, std::uint64_t bytes) {
    if (resources != nullptr && Libs::Graphics::GuestRange{address, bytes}.Valid()) resources->MapMemory(address, bytes);
}

__declspec(dllexport) void KytyGpuUnmapMemory(std::uint64_t address, std::uint64_t bytes) {
    if (resources != nullptr && Libs::Graphics::GuestRange{address, bytes}.Valid()) resources->UnmapMemory(address, bytes);
}

__declspec(dllexport) void KytyGpuMapShader(const KytyGpuShader* shader) {
    ensureShaders();
    Libs::Graphics::ShaderMappedData map;
    map.type = static_cast<Libs::Graphics::Prospero::ShaderBinaryType>(shader->type);
    map.user_data = static_cast<Libs::Graphics::ShaderUserData*>(shader->userData);
    map.input_semantics = static_cast<Libs::Graphics::ShaderSemantic*>(shader->inputSemantics);
    map.num_input_semantics = shader->inputSemanticCount;
    map.code_size_bytes = shader->codeBytes;
    map.scratch_size_dwords = shader->scratchDwords;
    Libs::Graphics::ShaderMapUserData(shader->code, map);
}

__declspec(dllexport) void* KytyGpuVideoOut(const char* name) {
    return videoOutFunction(name);
}

__declspec(dllexport) void KytyGpuSuspendPoint() {
    resources->GetGpu().SuspendPoint();
}

__declspec(dllexport) void KytyGpuWaitIdle() {
    resources->GetGpu().WaitForIdle();
}

}
