#include "KytyGpuApi.hpp"

#include "common/archive.h"
#include "common/assert.h"
#include "common/debugCounters.h"
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
#include <tuple>
#include <array>
#include <map>
#include <shared_mutex>
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
    std::atomic<std::uint64_t> hostWrites{0};
    std::atomic<std::uint64_t> readFaults{0};
    std::atomic<std::uint64_t> faultNs{0};
    std::atomic<std::uint64_t> readFaultNs{0};
    std::atomic<std::uint64_t> protectNs{0};
    std::atomic<std::uint64_t> protectCalls{0};
    std::atomic<std::uint64_t> protectSyscalls{0};
    std::atomic<std::uint64_t> backingOpens{0};
    std::atomic<std::uint64_t> aliasCopies{0};
    std::atomic<std::uint64_t> backingNs{0};
};

Counts counts;

bool statsEnabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_KYTY_GPU_STATS");
        return text != nullptr && std::string_view(text) == "1";
    }();
    return enabled;
}

std::mutex hotMutex;
std::unordered_map<std::uint64_t, std::uint32_t> hotReads;
std::unordered_map<std::uint64_t, std::uint32_t> hotWrites;
std::unordered_map<std::uint64_t, std::uint32_t> hotReadSites;
std::map<std::tuple<std::uint64_t, std::uint64_t, bool>, std::uint32_t> writtenBindings;
std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, std::uint32_t> gpuWrites;

void noteHot(bool write, std::uint64_t address) {
    if (!statsEnabled()) return;
    std::lock_guard lock(hotMutex);
    ++(write ? hotWrites : hotReads)[address & ~std::uint64_t{0xfff}];
}

std::string takeHot(std::unordered_map<std::uint64_t, std::uint32_t>& pages) {
    std::vector<std::pair<std::uint32_t, std::uint64_t>> sorted;
    sorted.reserve(pages.size());
    for (const auto& [page, count] : pages) sorted.emplace_back(count, page);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string text = std::to_string(pages.size()) + " pages;";
    char item[48];
    for (std::size_t index = 0; index < std::min<std::size_t>(sorted.size(), 6); ++index) {
        std::snprintf(item, sizeof(item), " %llx:%u", static_cast<unsigned long long>(sorted[index].second), sorted[index].first);
        text += item;
    }
    pages.clear();
    return text;
}

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

constexpr std::uint64_t RegionBytes = 4ull << 20u;
constexpr std::size_t RegionPages = RegionBytes / PageBytes;
constexpr std::uint64_t LowerLimit = 1ull << 40u;
constexpr std::uint64_t ExtendedBase = 0x080000000000ull;
constexpr std::uint64_t ExtendedBytes = 512ull << 30u;
constexpr std::uint8_t StateOpen = 0;
constexpr std::uint8_t StateRead = 1;
constexpr std::uint8_t StateNoAccess = 2;

constexpr std::uint8_t AliasUnknown = 0;
constexpr std::uint8_t AliasMapped = 1;
constexpr std::uint8_t AliasUnavailable = 2;

struct Region {
    std::mutex mutex;
    std::uint8_t aliasState = AliasUnknown;
    std::uint8_t* alias = nullptr;
    std::array<std::atomic<std::uint8_t>, RegionPages> state{};
    std::array<std::uint8_t, RegionPages> hostWrites{};
    std::array<DWORD, RegionPages> original{};
};

std::unique_ptr<std::atomic<Region*>[]> lowerRegions{new std::atomic<Region*>[LowerLimit / RegionBytes]{}};
std::unique_ptr<std::atomic<Region*>[]> extendedRegions{new std::atomic<Region*>[ExtendedBytes / RegionBytes]{}};

std::atomic<Region*>* regionSlot(std::uint64_t address) {
    if (address < LowerLimit) return &lowerRegions[address / RegionBytes];
    if (address >= ExtendedBase && address - ExtendedBase < ExtendedBytes) return &extendedRegions[(address - ExtendedBase) / RegionBytes];
    return nullptr;
}

Region* findRegion(std::uint64_t address) {
    auto* slot = regionSlot(address);
    return slot != nullptr ? slot->load(std::memory_order_acquire) : nullptr;
}

void readOriginals(Region& region, std::uint64_t base, std::size_t first, std::size_t last) {
    auto index = first;
    while (index < last) {
        MEMORY_BASIC_INFORMATION info{};
        const auto at = base + index * PageBytes;
        if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) != sizeof(info) || regionEnd(info) <= at) {
            region.original[index++] = 0;
            continue;
        }
        const auto stop = std::min<std::size_t>(last, (regionEnd(info) - base) / PageBytes);
        const DWORD protection = info.State == MEM_COMMIT ? info.Protect : 0;
        do {
            region.original[index++] = protection;
        } while (index < stop);
    }
}

Region* ensureRegion(std::uint64_t address) {
    auto* slot = regionSlot(address);
    if (slot == nullptr) return nullptr;
    if (auto* existing = slot->load(std::memory_order_acquire); existing != nullptr) return existing;
    auto* created = new Region();
    readOriginals(*created, address & ~(RegionBytes - 1), 0, RegionPages);
    Region* expected = nullptr;
    if (!slot->compare_exchange_strong(expected, created, std::memory_order_acq_rel)) {
        delete created;
        return expected;
    }
    return created;
}

std::uint8_t pageState(std::uint64_t address) {
    const auto* region = findRegion(address);
    return region != nullptr ? region->state[(address % RegionBytes) / PageBytes].load(std::memory_order_acquire) : StateOpen;
}

bool protectPages(std::uint64_t address, std::uint64_t bytes, Mode mode) {
    const bool open = mode == Mode::ReadWrite || mode == Mode::ExecuteReadWrite;
    const std::uint8_t desired = open ? StateOpen : (mode == Mode::NoAccess ? StateNoAccess : StateRead);
    const auto begin = address & ~(PageBytes - 1);
    const auto end = (address + bytes + PageBytes - 1) & ~(PageBytes - 1);
    counts.protectCalls.fetch_add(1, std::memory_order_relaxed);
    for (auto chunk = begin; chunk < end;) {
        const auto base = chunk & ~(RegionBytes - 1);
        const auto stop = std::min(end, base + RegionBytes);
        auto* region = open ? findRegion(chunk) : ensureRegion(chunk);
        if (region == nullptr) {
            if (!open) return false;
            chunk = stop;
            continue;
        }
        std::lock_guard lock(region->mutex);
        std::size_t runFirst = 0;
        std::size_t runLast = 0;
        DWORD runProtection = 0;
        const auto flush = [&] {
            if (runLast == runFirst) return true;
            DWORD previous = 0;
            counts.protectSyscalls.fetch_add(1, std::memory_order_relaxed);
            if (!open) {
                for (auto index = runFirst; index < runLast; ++index) region->state[index].store(desired, std::memory_order_release);
            }
            bool applied = VirtualProtect(reinterpret_cast<void*>(base + runFirst * PageBytes), (runLast - runFirst) * PageBytes, runProtection, &previous) != 0;
            if (!applied) {
                applied = true;
                for (auto index = runFirst; index < runLast; ++index) {
                    MEMORY_BASIC_INFORMATION page{};
                    if (!committed(base + index * PageBytes, page)) {
                        region->original[index] = 0;
                        region->state[index].store(StateOpen, std::memory_order_release);
                    } else if (!VirtualProtect(reinterpret_cast<void*>(base + index * PageBytes), PageBytes, runProtection, &previous)) {
                        applied = false;
                    }
                }
            }
            if (open) {
                for (auto index = runFirst; index < runLast; ++index) region->state[index].store(StateOpen, std::memory_order_release);
            }
            runFirst = runLast;
            return applied;
        };
        for (auto index = (chunk - base) / PageBytes; index < (stop - base) / PageBytes; ++index) {
            const auto current = region->state[index].load(std::memory_order_relaxed);
            if (current == desired) {
                if (!flush()) return false;
                runFirst = runLast = index + 1;
                continue;
            }
            if (!open && region->original[index] == 0) readOriginals(*region, base, index, index + 1);
            const DWORD original = region->original[index];
            if (original == 0 || (!open && region->hostWrites[index] != 0)) {
                if (!flush()) return false;
                region->state[index].store(original == 0 ? StateOpen : desired, std::memory_order_release);
                runFirst = runLast = index + 1;
                continue;
            }
            const DWORD wanted = open ? original : hostProtection(mode, original);
            if (runLast != runFirst && (wanted != runProtection || index != runLast)) {
                if (!flush()) return false;
            }
            if (runLast == runFirst) {
                runFirst = index;
                runProtection = wanted;
            }
            runLast = index + 1;
        }
        if (!flush()) return false;
        chunk = stop;
    }
    return true;
}

std::shared_mutex mappingMutex;
std::map<std::uint64_t, std::uint64_t> mappings;

void addMapping(std::uint64_t begin, std::uint64_t end) {
    std::unique_lock lock(mappingMutex);
    auto it = mappings.upper_bound(begin);
    if (it != mappings.begin() && std::prev(it)->second >= begin) {
        --it;
        begin = it->first;
        end = std::max(end, it->second);
        it = mappings.erase(it);
    }
    while (it != mappings.end() && it->first <= end) {
        end = std::max(end, it->second);
        it = mappings.erase(it);
    }
    mappings.emplace(begin, end);
}

void removeMapping(std::uint64_t begin, std::uint64_t end) {
    std::unique_lock lock(mappingMutex);
    auto it = mappings.upper_bound(begin);
    if (it != mappings.begin()) --it;
    while (it != mappings.end() && it->first < end) {
        const auto first = it->first;
        const auto last = it->second;
        if (last <= begin) {
            ++it;
            continue;
        }
        it = mappings.erase(it);
        if (first < begin) mappings.emplace(first, begin);
        if (end < last) it = mappings.emplace(end, last).first;
    }
}

std::uint64_t mappedRun(std::uint64_t address, std::uint64_t size) {
    std::shared_lock lock(mappingMutex);
    auto it = mappings.upper_bound(address);
    if (it == mappings.begin()) return 0;
    --it;
    return address < it->second ? std::min(size, it->second - address) : 0;
}

void mapLate(std::uint64_t address) {
    MEMORY_BASIC_INFORMATION region{};
    if (resources == nullptr || !committed(address, region)) return;
    const auto begin = std::max(address & ~(RegionBytes - 1), reinterpret_cast<std::uint64_t>(region.BaseAddress));
    const auto end = std::min((address & ~(RegionBytes - 1)) + RegionBytes, regionEnd(region));
    counts.lateMaps.fetch_add(1, std::memory_order_relaxed);
    addMapping(begin, end);
    resources->MapMemory(begin, end - begin);
}

std::uint8_t* regionAlias(Region& region, std::uint64_t base) {
    if (region.aliasState == AliasUnknown) {
        auto* alias = host->mapAlias != nullptr ? static_cast<std::uint8_t*>(host->mapAlias(base, RegionBytes)) : nullptr;
        region.alias = alias;
        region.aliasState = alias != nullptr ? AliasMapped : AliasUnavailable;
    }
    return region.alias;
}

void dropAliases(std::uint64_t begin, std::uint64_t end) {
    for (auto base = begin & ~(RegionBytes - 1); base < end; base += RegionBytes) {
        auto* region = findRegion(base);
        if (region == nullptr) continue;
        std::uint8_t* alias = nullptr;
        {
            std::lock_guard lock(region->mutex);
            alias = region->alias;
            region->alias = nullptr;
            region->aliasState = AliasUnknown;
        }
        if (alias != nullptr) host->unmapAlias(alias);
    }
}

template <typename Copy>
bool backing(std::uint64_t address, std::uint64_t bytes, bool sparse, bool write, Copy&& copy) {
    for (auto at = address; at < address + bytes;) {
        auto run = at;
        while (run < address + bytes) {
            const auto state = pageState(run);
            if (state != StateOpen && (write || state == StateNoAccess)) break;
            run = std::min(address + bytes, (run & ~(PageBytes - 1)) + PageBytes);
        }
        if (run > at) {
            if (mappedRun(at, run - at) < run - at) {
                for (auto piece = at; piece < run;) {
                    MEMORY_BASIC_INFORMATION info{};
                    const bool live = committed(piece, info);
                    const auto stop = std::min(run, regionEnd(info) > piece ? regionEnd(info) : piece + PageBytes);
                    if (!live && !sparse) return false;
                    copy(piece, stop - piece, live ? reinterpret_cast<std::uint8_t*>(piece) : nullptr);
                    piece = stop;
                }
            } else {
                copy(at, run - at, reinterpret_cast<std::uint8_t*>(at));
            }
            at = run;
            continue;
        }
        const auto page = at & ~(PageBytes - 1);
        const auto next = std::min(address + bytes, page + PageBytes);
        auto* region = findRegion(page);
        std::lock_guard lock(region->mutex);
        const auto index = (page % RegionBytes) / PageBytes;
        if (auto* alias = regionAlias(*region, page & ~(RegionBytes - 1)); alias != nullptr) {
            counts.aliasCopies.fetch_add(1, std::memory_order_relaxed);
            copy(at, next - at, alias + (at % RegionBytes));
            at = next;
            continue;
        }
        counts.backingOpens.fetch_add(1, std::memory_order_relaxed);
        DWORD previous = 0;
        VirtualProtect(reinterpret_cast<void*>(page), PageBytes, region->original[index], &previous);
        copy(at, next - at, reinterpret_cast<std::uint8_t*>(at));
        VirtualProtect(reinterpret_cast<void*>(page), PageBytes, previous, &previous);
        at = next;
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

bool notifyAccess(PageFaultAccess access, std::uint64_t address) {
    if (resources == nullptr) return false;
    if (access == PageFaultAccess::Read) counts.readFaults.fetch_add(1, std::memory_order_relaxed);
    if (!statsEnabled()) {
        if (resources->HandleFault(access, address)) return true;
        mapLate(address);
        return resources->HandleFault(access, address);
    }
    const auto started = std::chrono::steady_clock::now();
    noteHot(access == PageFaultAccess::Write, address);
    bool handled = resources->HandleFault(access, address);
    if (!handled) {
        mapLate(address);
        handled = resources->HandleFault(access, address);
    }
    const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    counts.faultNs.fetch_add(elapsed, std::memory_order_relaxed);
    if (access == PageFaultAccess::Read) counts.readFaultNs.fetch_add(elapsed, std::memory_order_relaxed);
    return handled;
}

LONG CALLBACK handleFault(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const auto address = static_cast<std::uint64_t>(record->ExceptionInformation[1]);
    if (pageState(address) == StateOpen) {
        if (record->ExceptionInformation[0] == 8 || !permits(address, record->ExceptionInformation[0] == 1)) return EXCEPTION_CONTINUE_SEARCH;
        counts.raceRetries.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    counts.faults.fetch_add(1, std::memory_order_relaxed);
    const auto access = record->ExceptionInformation[0] == 1 ? PageFaultAccess::Write : PageFaultAccess::Read;
    if (access == PageFaultAccess::Read && statsEnabled()) {
        std::lock_guard lock(hotMutex);
        ++hotReadSites[reinterpret_cast<std::uint64_t>(record->ExceptionAddress)];
    }
    return notifyAccess(access, address) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
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
        options.present_mode = Config::PresentMode::Mailbox;
        Config::Load(options);
        Log::Initialize();
        if (statsEnabled()) Common::DebugCounters::SetEnabled(true);
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
    return backing(vaddr, size, false, false, [&](std::uint64_t at, std::uint64_t length, std::uint8_t* where) {
        std::memcpy(bytes + (at - vaddr), where, static_cast<std::size_t>(length));
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
    return backing(vaddr, size, true, false, [&](std::uint64_t at, std::uint64_t length, std::uint8_t* where) {
        if (where != nullptr) {
            std::memcpy(bytes + (at - vaddr), where, static_cast<std::size_t>(length));
        } else {
            std::memset(bytes + (at - vaddr), 0, static_cast<std::size_t>(length));
        }
    });
}

bool TryReadSparseBacking(uint64_t vaddr, void* data, uint64_t size) {
    return CopySparseMemory(vaddr, data, size);
}

uint64_t ClampRangeSize(uint64_t vaddr, uint64_t size) {
    if (const auto known = mappedRun(vaddr, size); known != 0) return known;
    std::uint64_t covered = 0;
    while (covered < size) {
        MEMORY_BASIC_INFORMATION info{};
        if (!committed(vaddr + covered, info)) break;
        covered = std::min(size, regionEnd(info) - vaddr);
    }
    if (covered == 0) {
        EXIT("Memory: attempted to access invalid address 0x%016" PRIx64 " with size 0x%016" PRIx64 "\n", vaddr, size);
    }
    addMapping(vaddr, vaddr + covered);
    return covered;
}

void WriteBacking(uint64_t vaddr, const void* data, uint64_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const bool written = backing(vaddr, size, false, true, [&](std::uint64_t at, std::uint64_t length, std::uint8_t* where) {
        std::memcpy(where, bytes + (at - vaddr), static_cast<std::size_t>(length));
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

bool SystemContentParamSfoGetString(const char* name, std::string* value) {
    if (host == nullptr || value == nullptr || name == nullptr || std::string_view(name) != "TITLE_ID") return false;
    char id[32] = {};
    host->titleId(id, sizeof(id));
    *value = id;
    return !value->empty();
}

bool SystemContentParamSfoGetString(const char* name, char* value, size_t value_size) {
    std::string text;
    if (value == nullptr || value_size == 0 || !SystemContentParamSfoGetString(name, &text)) return false;
    std::snprintf(value, value_size, "%s", text.c_str());
    return true;
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
            std::fprintf(stderr, "[kyty-gpu] %llu presents over 10 s (%.1f fps); totals dcbs=%llu acbs=%llu event-adds=%llu eop-triggers=%llu video-triggers=%llu failed-triggers=%llu faults=%llu late-maps=%llu race-retries=%llu host-writes=%llu read-faults=%llu fault-ms=%llu read-fault-ms=%llu protect-ms=%llu protect-calls=%llu protect-syscalls=%llu backing-opens=%llu alias-copies=%llu\n", static_cast<unsigned long long>(now - previous), static_cast<double>(now - previous) / 10.0,
                         static_cast<unsigned long long>(counts.dcbs.load()), static_cast<unsigned long long>(counts.acbs.load()), static_cast<unsigned long long>(counts.adds.load()), static_cast<unsigned long long>(counts.graphicsTriggers.load()),
                         static_cast<unsigned long long>(counts.videoTriggers.load()), static_cast<unsigned long long>(counts.failedTriggers.load()), static_cast<unsigned long long>(counts.faults.load()), static_cast<unsigned long long>(counts.lateMaps.load()), static_cast<unsigned long long>(counts.raceRetries.load()), static_cast<unsigned long long>(counts.hostWrites.load()), static_cast<unsigned long long>(counts.readFaults.load()), static_cast<unsigned long long>(counts.faultNs.load() / 1000000), static_cast<unsigned long long>(counts.readFaultNs.load() / 1000000), static_cast<unsigned long long>(counts.protectNs.load() / 1000000), static_cast<unsigned long long>(counts.protectCalls.load()), static_cast<unsigned long long>(counts.protectSyscalls.load()), static_cast<unsigned long long>(counts.backingOpens.load()), static_cast<unsigned long long>(counts.aliasCopies.load()));
            previous = now;
            if (statsEnabled()) {
                using Common::DebugCounters::Counter;
                static std::array<std::uint64_t, static_cast<std::size_t>(Counter::Count)> last{};
                const auto delta = [](Counter counter) {
                    const auto index = static_cast<std::size_t>(counter);
                    const auto value = Common::DebugCounters::Detail::g_counters[index].load(std::memory_order_relaxed);
                    const auto result = value - last[index];
                    last[index] = value;
                    return static_cast<unsigned long long>(result);
                };
                std::fprintf(stderr, "[kyty-gpu] 10 s: draws=%llu dispatches=%llu submits=%llu gpu-waits=%llu gpu-wait-ms=%llu buffer-up-mib=%llu buffer-down-mib=%llu texture-up-mib=%llu texture-down-mib=%llu buffer-evictions=%llu texture-evictions=%llu pipelines=%llu\n",
                             delta(Counter::Draws), delta(Counter::Dispatches), delta(Counter::QueueSubmits), delta(Counter::GpuWaits), delta(Counter::GpuWaitNs) / 1000000, delta(Counter::BufferUploadBytes) >> 20, delta(Counter::BufferDownloadBytes) >> 20,
                             delta(Counter::TextureUploadBytes) >> 20, delta(Counter::TextureDownloadBytes) >> 20, delta(Counter::BufferEvictions), delta(Counter::TextureEvictions), delta(Counter::PipelinesCreated));
            }
            if (statsEnabled()) {
                std::lock_guard lock(hotMutex);
                std::uint64_t hottest = 0;
                std::uint32_t hottestCount = 0;
                for (const auto& [page, count] : hotReads) {
                    if (count > hottestCount) {
                        hottest = page;
                        hottestCount = count;
                    }
                }
                if (hottestCount != 0) {
                    std::vector<std::pair<std::uint32_t, std::tuple<std::uint64_t, std::uint64_t, bool>>> covering;
                    for (const auto& [key, count] : writtenBindings) {
                        const auto& [address, size, sentinel] = key;
                        if (address <= hottest && hottest < address + size) covering.emplace_back(count, key);
                    }
                    std::sort(covering.begin(), covering.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                    std::string text;
                    for (std::size_t index = 0; index < std::min<std::size_t>(covering.size(), 6); ++index) {
                        const auto& [address, size, sentinel] = covering[index].second;
                        char item[96];
                        std::snprintf(item, sizeof(item), " %llx+%llx%s:%u", static_cast<unsigned long long>(address), static_cast<unsigned long long>(size), sentinel ? "(unbounded)" : "", covering[index].first);
                        text += item;
                    }
                    std::fprintf(stderr, "[kyty-gpu] bindings covering hottest read page %llx (%u faults, %zu bindings):%s\n", static_cast<unsigned long long>(hottest), hottestCount, covering.size(), text.c_str());
                    std::vector<std::pair<std::uint32_t, std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>>> writers;
                    for (const auto& [key, count] : gpuWrites) {
                        const auto& [address, size, caller] = key;
                        if (address <= hottest && hottest < address + size) writers.emplace_back(count, key);
                    }
                    std::sort(writers.begin(), writers.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                    std::string callers;
                    for (std::size_t index = 0; index < std::min<std::size_t>(writers.size(), 6); ++index) {
                        const auto& [address, size, caller] = writers[index].second;
                        HMODULE module = nullptr;
                        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(caller), &module);
                        char item[128];
                        std::snprintf(item, sizeof(item), " %llx+%llx from +%llx:%u", static_cast<unsigned long long>(address), static_cast<unsigned long long>(size), static_cast<unsigned long long>(caller - reinterpret_cast<std::uint64_t>(module)), writers[index].first);
                        callers += item;
                    }
                    std::fprintf(stderr, "[kyty-gpu] GPU writes covering it (%zu):%s\n", writers.size(), callers.c_str());
                }
                const auto reads = takeHot(hotReads);
                const auto writes = takeHot(hotWrites);
                std::vector<std::pair<std::uint32_t, std::uint64_t>> sites;
                for (const auto& [site, count] : hotReadSites) sites.emplace_back(count, site);
                std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                std::string text;
                char item[64];
                for (std::size_t index = 0; index < std::min<std::size_t>(sites.size(), 6); ++index) {
                    HMODULE module = nullptr;
                    char name[MAX_PATH] = "?";
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(sites[index].second), &module)) {
                        GetModuleFileNameA(module, name, sizeof(name));
                    }
                    const char* base = std::strrchr(name, '\\');
                    std::snprintf(item, sizeof(item), " %s+%llx:%u", base != nullptr ? base + 1 : name, static_cast<unsigned long long>(sites[index].second - reinterpret_cast<std::uint64_t>(module)), sites[index].first);
                    text += item;
                }
                hotReadSites.clear();
                std::vector<std::pair<std::uint32_t, std::tuple<std::uint64_t, std::uint64_t, bool>>> bindings;
                for (const auto& [key, count] : writtenBindings) bindings.emplace_back(count, key);
                std::sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) { return static_cast<double>(a.first) * static_cast<double>(std::get<1>(a.second)) > static_cast<double>(b.first) * static_cast<double>(std::get<1>(b.second)); });
                std::string written;
                for (std::size_t index = 0; index < std::min<std::size_t>(bindings.size(), 8); ++index) {
                    const auto& [address, size, sentinel] = bindings[index].second;
                    char item[96];
                    std::snprintf(item, sizeof(item), " %llx+%llx%s:%u", static_cast<unsigned long long>(address), static_cast<unsigned long long>(size), sentinel ? "(unbounded)" : "", bindings[index].first);
                    written += item;
                }
                std::fprintf(stderr, "[kyty-gpu] written bindings (%zu distinct):%s\n", writtenBindings.size(), written.c_str());
                writtenBindings.clear();
                gpuWrites.clear();
                std::fprintf(stderr, "[kyty-gpu] read fault sites:%s\n", text.c_str());
                std::fprintf(stderr, "[kyty-gpu] read faults: %s\n[kyty-gpu] write faults: %s\n", reads.c_str(), writes.c_str());
            }
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
    if (resources == nullptr || bytes == 0) return;
    addMapping(address, address + bytes);
    if (Libs::Graphics::GuestRange{address, bytes}.Valid()) resources->MapMemory(address, bytes);
}

__declspec(dllexport) void KytyGpuUnmapMemory(std::uint64_t address, std::uint64_t bytes) {
    if (resources == nullptr || bytes == 0) return;
    removeMapping(address, address + bytes);
    if (Libs::Graphics::GuestRange{address, bytes}.Valid()) resources->UnmapMemory(address, bytes);
    dropAliases(address, address + bytes);
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

__declspec(dllexport) void KytyGpuHostWrite(std::uint64_t address, std::uint64_t bytes, bool begin) {
    if (resources == nullptr || bytes == 0) return;
    if (begin) counts.hostWrites.fetch_add(1, std::memory_order_relaxed);
    const auto end = address + bytes;
    for (auto page = address & ~(PageBytes - 1); page < end; page += PageBytes) {
        auto* region = begin ? ensureRegion(page) : findRegion(page);
        if (region == nullptr) continue;
        const auto index = (page % RegionBytes) / PageBytes;
        bool tracked = false;
        {
            std::lock_guard lock(region->mutex);
            if (begin) {
                ++region->hostWrites[index];
            } else if (region->hostWrites[index] != 0) {
                --region->hostWrites[index];
            }
            tracked = region->state[index].load(std::memory_order_relaxed) != StateOpen;
        }
        if (tracked) notifyAccess(PageFaultAccess::Write, std::max(page, address));
    }
}

bool captureEnabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_KYTY_GPU_CAPTURE");
        return text != nullptr && std::string_view(text) == "1";
    }();
    return enabled;
}

__declspec(dllexport) bool KytyHostCaptureEnabled() {
    return captureEnabled();
}

__declspec(dllexport) const char* KytyHostCaptureRequest() {
    if (!captureEnabled()) return nullptr;
    static const auto paths = [] {
        char directory[MAX_PATH] = {};
        GetCurrentDirectoryA(MAX_PATH, directory);
        const std::string base(directory);
        return std::pair<std::string, std::string>{base + "\\kyty-capture.request", base + "\\kyty-capture.bmp"};
    }();
    if (GetFileAttributesA(paths.first.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    DeleteFileA(paths.first.c_str());
    return paths.second.c_str();
}

__declspec(dllexport) void KytyHostCaptureWrite(const char* path, const void* pixels, std::uint32_t width, std::uint32_t height, bool rgba) {
    if (path == nullptr || pixels == nullptr || width == 0 || height == 0) return;
    const auto rowBytes = static_cast<std::size_t>(width) * 4u;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = static_cast<DWORD>(file.bfOffBits + rowBytes * height);
    const std::string temporary = std::string(path) + ".tmp";
    FILE* out = std::fopen(temporary.c_str(), "wb");
    if (out == nullptr) return;
    std::fwrite(&file, sizeof(file), 1, out);
    std::fwrite(&info, sizeof(info), 1, out);
    std::vector<std::uint8_t> row(rowBytes);
    const auto* source = static_cast<const std::uint8_t*>(pixels);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(row.data(), source + y * rowBytes, rowBytes);
        if (rgba) {
            for (std::size_t x = 0; x < rowBytes; x += 4) std::swap(row[x], row[x + 2]);
        }
        std::fwrite(row.data(), 1, rowBytes, out);
    }
    std::fclose(out);
    MoveFileExA(temporary.c_str(), path, MOVEFILE_REPLACE_EXISTING);
    std::fprintf(stderr, "[kyty-gpu] captured %ux%u to %s\n", width, height, path);
}

void KytyHostNoteGpuWrite(std::uint64_t address, std::uint64_t size, const void* caller) {
    if (!statsEnabled()) return;
    std::lock_guard lock(hotMutex);
    ++gpuWrites[{address, size, reinterpret_cast<std::uint64_t>(caller)}];
}

void KytyHostNoteWrittenBinding(std::uint64_t address, std::uint64_t size, bool sentinel) {
    if (!statsEnabled()) return;
    std::lock_guard lock(hotMutex);
    ++writtenBindings[{address, size, sentinel}];
}

__declspec(dllexport) void KytyGpuSuspendPoint() {
    resources->GetGpu().SuspendPoint();
}

__declspec(dllexport) void KytyGpuWaitIdle() {
    resources->GetGpu().WaitForIdle();
}

}
