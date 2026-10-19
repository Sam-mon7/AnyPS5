#include "prx/libSceAgcDriver/Execution/include/KytyGpu.hpp"
#include "prx/libSceAgcDriver/kyty-gpu/include/KytyGpuApi.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include "prx/libkernel/AppMetadata/include/AppMetadata.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

extern "C" std::uint64_t APS5_VABI sceKernelReadTsc();
extern "C" std::uint64_t APS5_VABI sceKernelGetTscFrequency();

namespace AgcDriver::KytyGpu {

namespace {

struct Bridge {
    void* cookie;
    const KytyGpuEventHooks* hooks;
    ~Bridge() { hooks->release(cookie); }
};

struct PendingEqEvent {
    KernelEqueue eq;
    int id;
    void* udata;
};

struct Module {
    HMODULE handle = nullptr;
    KytyGpuStartFunction start = nullptr;
    KytyGpuSubmitFunction submitDcb = nullptr;
    KytyGpuSubmitComputeFunction submitAcb = nullptr;
    KytyGpuEqEventFunction addEqEvent = nullptr;
    KytyGpuEqEventFunction deleteEqEvent = nullptr;
    KytyGpuRangeFunction mapMemory = nullptr;
    KytyGpuRangeFunction unmapMemory = nullptr;
    KytyGpuShaderFunction mapShader = nullptr;
    KytyGpuLookupFunction videoOut = nullptr;
    KytyGpuHostWriteFunction hostWrite = nullptr;
    void (*suspendPoint)() = nullptr;
    void (*waitIdle)() = nullptr;
};

std::atomic<bool> started{false};
std::mutex startMutex;
std::vector<PendingEqEvent> pendingEqEvents;

KernelEqueueEvent& event(KytyGpuEventView* view) {
    return *reinterpret_cast<KernelEqueueEvent*>(view);
}

const KernelEqueueEvent& event(const KytyGpuEventView* view) {
    return *reinterpret_cast<const KernelEqueueEvent*>(view);
}

KytyGpuEvent toKyty(const KernelEvent& value) {
    return {value.ident, value.filter, value.flags, value.fflags, value.data, value.udata};
}

KernelEvent fromKyty(const KytyGpuEvent& value) {
    KernelEvent result;
    result.ident = value.ident;
    result.filter = value.filter;
    result.flags = value.flags;
    result.fflags = value.fflags;
    result.data = value.data;
    result.udata = value.udata;
    return result;
}

void readEvent(const KytyGpuEventView* view, bool* triggered, KytyGpuEvent* value) {
    *triggered = event(view).triggered;
    *value = toKyty(event(view).event);
}

void writeEvent(KytyGpuEventView* view, bool triggered, const KytyGpuEvent* value) {
    event(view).triggered = triggered;
    event(view).event = fromKyty(*value);
}

std::size_t pendingCount(const KytyGpuEventView* view) {
    return event(view).pendingEvents.size();
}

void pendingAt(const KytyGpuEventView* view, std::size_t index, KytyGpuEvent* value) {
    *value = toKyty(event(view).pendingEvents.at(index));
}

void setPending(KytyGpuEventView* view, const KytyGpuEvent* values, std::size_t count) {
    auto& pending = event(view).pendingEvents;
    pending.clear();
    for (std::size_t index = 0; index < count; ++index) pending.push_back(fromKyty(values[index]));
}

int addEvent(std::int64_t eq, const KytyGpuEvent* value, bool triggered, void* cookie, const KytyGpuEventHooks* hooks, bool trigger, bool reset, bool remove) {
    auto bridge = std::make_shared<Bridge>(Bridge{cookie, hooks});
    KernelEqueueEvent added{};
    added.triggered = triggered;
    added.event = fromKyty(*value);
    added.filter.data = bridge.get();
    added.filter.owner = bridge;
    if (trigger) {
        added.filter.triggerFunc = [](KernelEqueueEvent* target, void* data) {
            const auto* held = static_cast<Bridge*>(target->filter.data);
            held->hooks->trigger(held->cookie, reinterpret_cast<KytyGpuEventView*>(target), data);
        };
    }
    if (reset) {
        added.filter.resetFunc = [](KernelEqueueEvent* target) {
            const auto* held = static_cast<Bridge*>(target->filter.data);
            held->hooks->reset(held->cookie, reinterpret_cast<KytyGpuEventView*>(target));
        };
    }
    if (remove) {
        added.filter.deleteEventFunc = [](KernelEqueue queue, KernelEqueueEvent* target) {
            const auto* held = static_cast<Bridge*>(target->filter.data);
            held->hooks->remove(held->cookie, queue, reinterpret_cast<KytyGpuEventView*>(target));
        };
    }
    return EqueueAddEvent_nid_postfix(eq, added);
}

int triggerEvent(std::int64_t eq, std::uintptr_t ident, std::int16_t filter, void* data) {
    return EqueueTriggerEvent_nid_postfix(eq, ident, filter, data);
}

int deleteEvent(std::int64_t eq, std::uintptr_t ident, std::int16_t filter) {
    return EqueueDeleteEvent_nid_postfix(eq, ident, filter);
}

bool pinEqueue(std::int64_t eq) {
    return eq != 0 && EqueuePin_nid_postfix(eq) != nullptr;
}

std::uint64_t processTime() {
    return sceKernelGetProcessTime();
}

std::uint64_t processTimeCounter() {
    return sceKernelGetProcessTimeCounter();
}

std::uint64_t readTsc() {
    return sceKernelReadTsc();
}

std::uint64_t tscFrequency() {
    return sceKernelGetTscFrequency();
}

void titleId(char* value, std::size_t size) {
    const auto id = GetAppTitleId_nid_postfix();
    std::snprintf(value, size, "%.*s", static_cast<int>(sizeof(id.value)), id.value);
}

constexpr KytyGpuHostTable hostTable{
    &readEvent, &writeEvent, &pendingCount, &pendingAt, &setPending, &addEvent, &triggerEvent, &deleteEvent, &pinEqueue,
    &processTime, &processTimeCounter, &readTsc, &tscFrequency, &titleId,
};

template <typename Function>
Function symbol(HMODULE handle, const char* name) {
    auto* found = reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(handle, name)));
    if (found == nullptr) throw std::runtime_error(std::string("Kyty GPU: kytygpu.dll does not export ") + name);
    return found;
}

const Module& module() {
    static const Module loaded = [] {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&module), &self)) {
            throw std::runtime_error("Kyty GPU: cannot find the AGC driver module");
        }
        std::wstring path(MAX_PATH, L'\0');
        path.resize(GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size())));
        const auto library = std::filesystem::path(path).parent_path() / L"kytygpu.dll";
        Module result;
        result.handle = LoadLibraryW(library.c_str());
        if (result.handle == nullptr) result.handle = LoadLibraryW(L"kytygpu.dll");
        if (result.handle == nullptr) throw std::runtime_error("Kyty GPU: cannot load " + library.string());
        result.start = symbol<KytyGpuStartFunction>(result.handle, "KytyGpuStart");
        result.submitDcb = symbol<KytyGpuSubmitFunction>(result.handle, "KytyGpuSubmitDcb");
        result.submitAcb = symbol<KytyGpuSubmitComputeFunction>(result.handle, "KytyGpuSubmitAcb");
        result.addEqEvent = symbol<KytyGpuEqEventFunction>(result.handle, "KytyGpuAddEqEvent");
        result.deleteEqEvent = symbol<KytyGpuEqEventFunction>(result.handle, "KytyGpuDeleteEqEvent");
        result.mapMemory = symbol<KytyGpuRangeFunction>(result.handle, "KytyGpuMapMemory");
        result.unmapMemory = symbol<KytyGpuRangeFunction>(result.handle, "KytyGpuUnmapMemory");
        result.mapShader = symbol<KytyGpuShaderFunction>(result.handle, "KytyGpuMapShader");
        result.videoOut = symbol<KytyGpuLookupFunction>(result.handle, "KytyGpuVideoOut");
        result.hostWrite = symbol<KytyGpuHostWriteFunction>(result.handle, "KytyGpuHostWrite");
        result.suspendPoint = symbol<void (*)()>(result.handle, "KytyGpuSuspendPoint");
        result.waitIdle = symbol<void (*)()>(result.handle, "KytyGpuWaitIdle");
        return result;
    }();
    return loaded;
}

void mapRanges(const GuestAllocations::Mapped& ranges, std::uint64_t) {
    for (const auto& weak : ranges) {
        if (const auto range = weak.lock(); range != nullptr) module().mapMemory(range->address, range->bytes);
    }
}

void unmapRange(std::uintptr_t address, std::size_t bytes) {
    module().unmapMemory(address, bytes);
}

void hostWrite(std::uintptr_t address, std::size_t bytes, bool begin) {
    module().hostWrite(address, bytes, begin);
}

}

bool Enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_KYTY_GPU");
        return text != nullptr && std::string_view(text) == "1";
    }();
    return enabled;
}

bool Started() {
    return started.load(std::memory_order_acquire);
}

void Start(void* window, std::uint32_t width, std::uint32_t height) {
    std::lock_guard lock(startMutex);
    if (Started()) return;
    const auto& loaded = module();
    if (loaded.start(&hostTable, window, width, height) != 0) throw std::runtime_error("Kyty GPU: start failed");
    std::size_t mapped = 0;
    std::uint64_t bytes = 0;
    for (const auto& range : GuestAllocations::GuestAllocationsAcquireAll_nid_postfix()) {
        if (range == nullptr || range->bytes == 0 || !range->readable) continue;
        const auto end = range->address + range->bytes;
        for (auto at = range->address; at < end;) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) != sizeof(info)) break;
            const auto stop = std::min<std::uint64_t>(end, reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize);
            if (info.State == MEM_COMMIT) {
                std::uint64_t runEnd = stop;
                while (runEnd < end) {
                    MEMORY_BASIC_INFORMATION next{};
                    if (VirtualQuery(reinterpret_cast<const void*>(runEnd), &next, sizeof(next)) != sizeof(next) || next.State != MEM_COMMIT) break;
                    runEnd = std::min<std::uint64_t>(end, reinterpret_cast<std::uint64_t>(next.BaseAddress) + next.RegionSize);
                }
                loaded.mapMemory(at, runEnd - at);
                ++mapped;
                bytes += runEnd - at;
                at = runEnd;
            } else {
                at = stop;
            }
        }
    }
    GuestAllocations::GuestAllocationsSetGpuMapObserver_nid_postfix(&mapRanges);
    GuestAllocations::GuestAllocationsSetUnmapObserver_nid_postfix(&unmapRange);
    GuestArena::GuestArenaSetHostWriteHook_nid_no_patch(&hostWrite);
    started.store(true, std::memory_order_release);
    for (const auto& pending : pendingEqEvents) loaded.addEqEvent(pending.eq, pending.id, pending.udata);
    pendingEqEvents.clear();
    std::fprintf(stderr, "[kyty-gpu] started on %ux%u; %zu committed guest runs (%llu MiB) mapped\n", width, height, mapped, static_cast<unsigned long long>(bytes >> 20));
}

void Submit(const Packet* packet, std::uint32_t queue) {
    if (!Started()) throw std::runtime_error("Kyty GPU: command buffer submitted before video out started the GPU");
    if (packet == nullptr || packet->addr == nullptr || packet->dw_num == 0) return;
    if (queue == 0) {
        module().submitDcb(packet->addr, packet->dw_num);
    } else {
        module().submitAcb(queue, packet->addr, packet->dw_num);
    }
}

int AddEqEvent(KernelEqueue eq, int id, void* udata) {
    std::lock_guard lock(startMutex);
    std::fprintf(stderr, "[kyty-gpu] sceAgcDriverAddEqEvent eq=%lld id=%d%s\n", static_cast<long long>(eq), id, Started() ? "" : " (before start)");
    if (Started()) return module().addEqEvent(eq, id, udata);
    if (eq == 0 || EqueuePin_nid_postfix(eq) == nullptr) throw std::runtime_error("sceAgcDriverAddEqEvent: invalid event queue");
    pendingEqEvents.push_back({eq, id, udata});
    return 0;
}

int DeleteEqEvent(KernelEqueue eq, int id) {
    std::lock_guard lock(startMutex);
    if (Started()) return module().deleteEqEvent(eq, id, nullptr);
    std::erase_if(pendingEqEvents, [&](const PendingEqEvent& pending) { return pending.eq == eq && pending.id == id; });
    return 0;
}

void RegisterShader(const Shader* shader) {
    const KytyGpuShader mapped{
        shader->type,
        shader->user_data,
        shader->input_semantics,
        shader->num_input_semantics,
        shader->shader_size,
        shader->scratch_size_dw_per_thread,
        reinterpret_cast<std::uint64_t>(shader->code),
    };
    module().mapShader(&mapped);
}

void SuspendPoint() {
    if (Started()) module().suspendPoint();
}

void WaitIdle() {
    if (Started()) module().waitIdle();
}

}

extern "C" bool AgcDriverKytyGpuEnabled_nid_no_patch() {
    return AgcDriver::KytyGpu::Enabled();
}

extern "C" void AgcDriverKytyGpuStart_nid_no_patch(void* window, std::uint32_t width, std::uint32_t height) {
    AgcDriver::KytyGpu::Start(window, width, height);
}

extern "C" void* AgcDriverKytyGpuVideoOut_nid_no_patch(const char* name) {
    return AgcDriver::KytyGpu::module().videoOut(name);
}
