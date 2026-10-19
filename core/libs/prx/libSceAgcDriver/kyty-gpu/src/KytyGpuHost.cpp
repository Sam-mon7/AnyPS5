#include "KytyGpuApi.hpp"

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
#include "graphics/host_gpu/renderer/cache/gpuResourceManager.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
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
#include "loader/demonsSoulsWarp.h"
#include "loader/timer.h"
#include "live-counters.h"

#include "SDL.h"
#include "SDL_vulkan.h"

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
Libs::Graphics::GpuResourceManager* gpuResources = nullptr;
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
    std::atomic<std::uint64_t> flips{0};
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
    if (gpuResources == nullptr || !committed(address, region)) return;
    const auto begin = std::max(address & ~(RegionBytes - 1), reinterpret_cast<std::uint64_t>(region.BaseAddress));
    const auto end = std::min((address & ~(RegionBytes - 1)) + RegionBytes, regionEnd(region));
    counts.lateMaps.fetch_add(1, std::memory_order_relaxed);
    addMapping(begin, end);
    gpuResources->MapMemory(begin, end - begin);
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
    if (gpuResources == nullptr) return false;
    if (access == PageFaultAccess::Read) counts.readFaults.fetch_add(1, std::memory_order_relaxed);
    if (!statsEnabled()) {
        if (gpuResources->HandleFault(access, address)) return true;
        mapLate(address);
        return gpuResources->HandleFault(access, address);
    }
    const auto started = std::chrono::steady_clock::now();
    noteHot(access == PageFaultAccess::Write, address);
    bool handled = gpuResources->HandleFault(access, address);
    if (!handled) {
        mapLate(address);
        handled = gpuResources->HandleFault(access, address);
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

constexpr std::array<std::pair<const char*, const char*>, 47> recommendedSwitches{{
    {"KYTY_SRT_NATIVE", "1"}, {"KYTY_SRT_PREDICATES", "1"}, {"KYTY_PREPARATION_SCRATCH", "1"}, {"KYTY_PREPARATION_LOOKUP", "1"},
    {"KYTY_PREPARATION_TRIM", "1"}, {"KYTY_SPECIALIZATION_GUARD", "1"}, {"KYTY_PIPELINE_INDEX", "1"}, {"KYTY_BINDING_SCRATCH", "1"},
    {"KYTY_DRAW_RUN_RANGES", "1"}, {"KYTY_BUFFER_RESIDENCY", "1"}, {"KYTY_COPY_FEEDBACK", "1"}, {"KYTY_ASYNC_LOD_STATS", "1"},
    {"KYTY_BACKING_READ", "1"}, {"KYTY_STREAM_UPLOAD", "1"}, {"KYTY_FRAME_PIPELINE", "1"}, {"KYTY_WRITE_WINDOW_HANDOFF", "1"},
    {"KYTY_READBACK_DETACH", "1"}, {"KYTY_ASYNC_WRITE_READBACK", "1"}, {"KYTY_READBACK_SLOTS", "1"}, {"KYTY_ASYNC_UPLOAD", "2"},
    {"KYTY_BUFFER_RECLAIM", "1"}, {"KYTY_UNMAP_PROTECT_SKIP", "1"}, {"KYTY_BDA_DIRTY_REGIONS", "1"},
    {"KYTY_GLOBAL_BARRIER_DEDUPE", "1"}, {"KYTY_RANGE_SET_FAST", "1"}, {"KYTY_IMAGE_GRANULES", "2"},
    {"KYTY_TEXTURE_RESOLVE_PAGES", "1"}, {"KYTY_PARTIAL_IMAGE_DIRTY", "1"}, {"KYTY_PARTIAL_ROW_BANDS", "1"},
    {"KYTY_ASYNC_REPROTECT", "1"}, {"KYTY_READBACK_NARROW", "1"}, {"KYTY_READBACK_QUEUE", "1"}, {"KYTY_IMAGE_BARRIER_DEDUPE", "1"},
    {"KYTY_IMAGE_POOL", "1"}, {"KYTY_PENDING_DRAIN", "1"}, {"KYTY_VULKAN_RECORDING", "1"}, {"KYTY_DEFERRED_SUBMIT", "1"},
    {"KYTY_NATIVE_XPR", "1"}, {"KYTY_DRAW_PACKETS", "1"}, {"KYTY_NATIVE_XPR_PREDICT", "1"}, {"KYTY_NATIVE_IMAGE_PROOF", "1"},
    {"KYTY_ASYNC_XPR_PIPELINES", "1"}, {"KYTY_NATIVE_XPR_RELOCATE", "1"}, {"KYTY_NATIVE_XPR_STORE_BUDGET", "256"},
    {"KYTY_NATIVE_XPR_KEEP_FRAMES", "600"}, {"KYTY_NATIVE_XPR_INSTANCES", "1"}, {"KYTY_DISPATCH_BATCH", "128"},
}};

const std::string& renderCpus() {
    static const std::string cpus = [] {
        std::string list;
        const auto processors = std::thread::hardware_concurrency();
        if (processors >= 4) {
            for (unsigned index = 1; index < processors / 2; ++index) list += (list.empty() ? "" : ",") + std::to_string(index * 2);
        }
        return list;
    }();
    return cpus;
}

void configure(std::uint32_t width, std::uint32_t height) {
    std::call_once(configured, [&] {
        Config::Initialize();
        Config::ConfigOptions options;
        options.screen_width = width;
        options.screen_height = height;
        const char* log = std::getenv("APS5_KYTY_GPU_LOG");
        options.printf_direction = log != nullptr && std::string_view(log) == "1" ? Config::OutputDirection::Console : Config::OutputDirection::Silent;
        options.present_mode = Config::PresentMode::Mailbox;
        Config::Load(options);
        Log::Initialize();
    });
}

void ensureShaders() {
    std::call_once(shaders, [] { Libs::Graphics::ShaderInit(); });
}

KYTY_SYSV_ABI int videoOutSetFlipMaster(int, int) {
    return 0;
}

KYTY_SYSV_ABI void videoOutAddBufferAttributeOption(void* attribute, std::uint64_t option) {
    constexpr std::size_t optionOffset = 24;
    std::uint64_t value = 0;
    std::memcpy(&value, static_cast<std::uint8_t*>(attribute) + optionOffset, sizeof(value));
    value |= option;
    std::memcpy(static_cast<std::uint8_t*>(attribute) + optionOffset, &value, sizeof(value));
}

KYTY_SYSV_ABI int videoOutAllowOutputResolutionWqhdDetection(int) {
    return 0;
}

KYTY_SYSV_ABI int videoOutVrrPegToFixedRate(int, std::uint64_t, std::uint64_t) {
    return 0;
}

KYTY_SYSV_ABI int videoOutVrrUnpegFromFixedRate(int) {
    return 0;
}

KYTY_SYSV_ABI int videoOutSubmitFlip(int handle, int index, int flipMode, std::int64_t flipArg) {
    counts.flips.fetch_add(1, std::memory_order_relaxed);
    return Libs::VideoOut::VideoOutSubmitFlip(handle, index, flipMode, flipArg);
}

void* videoOutFunction(std::string_view name) {
    namespace V = Libs::VideoOut;
    static const std::unordered_map<std::string_view, void*> functions{
        {"Open", reinterpret_cast<void*>(&V::VideoOutOpen)},
        {"Close", reinterpret_cast<void*>(&V::VideoOutClose)},
        {"SetFlipMaster", reinterpret_cast<void*>(&videoOutSetFlipMaster)},
        {"AddBufferAttributeOption", reinterpret_cast<void*>(&videoOutAddBufferAttributeOption)},
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
        {"SubmitFlip", reinterpret_cast<void*>(&videoOutSubmitFlip)},
        {"GetFlipStatus", reinterpret_cast<void*>(&V::VideoOutGetFlipStatus)},
        {"IsFlipPending", reinterpret_cast<void*>(&V::VideoOutIsFlipPending)},
        {"GetVblankStatus", reinterpret_cast<void*>(&V::VideoOutGetVblankStatus)},
        {"GetEventId", reinterpret_cast<void*>(&V::VideoOutGetEventId)},
        {"GetEventData", reinterpret_cast<void*>(&V::VideoOutGetEventData)},
        {"GetEventCount", reinterpret_cast<void*>(&V::VideoOutGetEventCount)},
        {"WaitVblank", reinterpret_cast<void*>(&V::VideoOutWaitVblank)},
        {"GetOutputStatus", reinterpret_cast<void*>(&V::VideoOutGetOutputStatus)},
        {"AllowOutputResolutionWqhdDetection", reinterpret_cast<void*>(&videoOutAllowOutputResolutionWqhdDetection)},
        {"VrrPegToFixedRate", reinterpret_cast<void*>(&videoOutVrrPegToFixedRate)},
        {"VrrUnpegFromFixedRate", reinterpret_cast<void*>(&videoOutVrrUnpegFromFixedRate)},
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

bool SystemOverlay::PrepareFrame(vk::Extent2D, vk::Format, uint32_t, SystemOverlayHud*) {
    return false;
}

void SystemOverlay::Record(vk::CommandBuffer, vk::ImageView) {}

void SystemOverlay::ReleaseVulkan() {}

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

std::uint8_t* backingPointer(std::uint64_t address, std::uint64_t bytes) {
    const auto base = address & ~(RegionBytes - 1);
    if (bytes == 0 || bytes > base + RegionBytes - address || mappedRun(address, bytes) != bytes) return nullptr;
    auto* region = ensureRegion(base);
    if (region == nullptr) return nullptr;
    std::lock_guard lock(region->mutex);
    auto* alias = regionAlias(*region, base);
    return alias != nullptr ? alias + (address - base) : nullptr;
}

bool guestRange(std::uint64_t address, std::uint64_t bytes) {
    return Libs::Graphics::GuestRange{address, bytes}.Valid();
}

namespace Libs::LibKernel::Memory {

thread_local uint64_t g_srt_shader_hash = 0;

bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    return backing(vaddr, size, false, false, [&](std::uint64_t at, std::uint64_t length, std::uint8_t* where) {
        std::memcpy(bytes + (at - vaddr), where, static_cast<std::size_t>(length));
    });
}

bool TryReadBufferBacking(uint64_t vaddr, void* data, uint64_t size) {
    if (gpuResources != nullptr && Graphics::GuestRange{vaddr, size}.Valid()) {
        if (!Graphics::GuestGpu::IsGpuThread()) return false;
        auto& buffers = gpuResources->GetBufferCache();
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

bool TryReadBackingToHost(uint64_t vaddr, void* data, uint64_t size) {
    return TryReadBacking(vaddr, data, size);
}

bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    return backing(vaddr, size, false, true, [&](std::uint64_t at, std::uint64_t length, std::uint8_t* where) {
        std::memcpy(where, bytes + (at - vaddr), static_cast<std::size_t>(length));
    });
}

const uint8_t* TryGetBackingPointer(uint64_t vaddr, uint64_t size) {
    return backingPointer(vaddr, size);
}

bool TryGetBackingPieces(uint64_t vaddr, uint64_t size, std::vector<std::pair<const uint8_t*, uint64_t>>& pieces) {
    pieces.clear();
    for (auto at = vaddr; at < vaddr + size;) {
        const auto next = std::min(vaddr + size, (at & ~(RegionBytes - 1)) + RegionBytes);
        const auto* pointer = backingPointer(at, next - at);
        if (pointer == nullptr) return false;
        if (!pieces.empty() && pieces.back().first + pieces.back().second == pointer) {
            pieces.back().second += next - at;
        } else {
            pieces.emplace_back(pointer, next - at);
        }
        at = next;
    }
    return true;
}

bool TryReadGpuCleanBackingToHost(uint64_t vaddr, void* data, uint64_t size) {
    if (gpuResources != nullptr && guestRange(vaddr, size)) {
        if (!Graphics::GuestGpu::IsGpuThread() || gpuResources->GetBufferCache().HasGpuDirtyBytes(vaddr, size) ||
            gpuResources->GetTextureCache().IsRegionGpuModified(vaddr, size)) {
            return false;
        }
    }
    return TryReadBackingToHost(vaddr, data, size);
}

bool IsUniqueGuestBackingRange(uint64_t vaddr, uint64_t size) {
    return host->uniqueBacking != nullptr && host->uniqueBacking(vaddr, size);
}

bool TryReadGpuCleanBackingOnWatchedPage(uint64_t vaddr, void* data, uint64_t size) {
    return gpuResources != nullptr && Graphics::GuestGpu::IsGpuThread() && guestRange(vaddr, size) &&
           gpuResources->HasReadWatchers(vaddr, size) && TryReadGpuCleanBackingToHost(vaddr, data, size);
}

bool TryReadGpuShaderSpan(uint64_t vaddr, void* data, uint64_t size, bool clean) {
    if (data == nullptr || size < 8 || size > (clean ? 4096u : 64u) || size % 4 != 0 || gpuResources == nullptr ||
        !Graphics::GuestGpu::IsGpuThread() || !guestRange(vaddr, size)) {
        return false;
    }
    if (!clean && !gpuResources->HasReadWatchers(vaddr, size)) {
        auto* out = static_cast<std::uint8_t*>(data);
        const auto* in = reinterpret_cast<const std::uint8_t*>(vaddr);
        std::uint64_t at = 0;
        for (; at + 8 <= size; at += 8) std::memcpy(out + at, in + at, 8);
        if (at < size) std::memcpy(out + at, in + at, 4);
        return true;
    }
    return TryReadGpuCleanBackingToHost(vaddr, data, size);
}

bool SyncGpuCleanBacking(uint64_t vaddr, uint64_t size) {
    if (gpuResources == nullptr || !guestRange(vaddr, size)) return true;
    if (!Graphics::GuestGpu::IsGpuThread() || gpuResources->GetTextureCache().IsRegionGpuModified(vaddr, size)) return false;
    if (gpuResources->GetBufferCache().HasGpuDirtyBytes(vaddr, size)) gpuResources->GetBufferCache().ReadMemory(vaddr, size);
    return true;
}

bool IsFullyMapped(uint64_t vaddr, uint64_t size) {
    return ClampRangeSize(vaddr, size) == size;
}

void MappedParts(uint64_t vaddr, uint64_t size, std::vector<std::pair<uint64_t, uint64_t>>* parts) {
    parts->clear();
    const auto add = [&](std::uint64_t begin, std::uint64_t end) {
        if (!parts->empty() && parts->back().first + parts->back().second == begin) {
            parts->back().second += end - begin;
        } else {
            parts->emplace_back(begin, end - begin);
        }
    };
    for (auto at = vaddr; at < vaddr + size;) {
        if (const auto known = mappedRun(at, vaddr + size - at); known != 0) {
            add(at, at + known);
            at += known;
            continue;
        }
        MEMORY_BASIC_INFORMATION info{};
        const bool live = committed(at, info);
        const auto stop = std::min(vaddr + size, regionEnd(info) > at ? regionEnd(info) : (at & ~(PageBytes - 1)) + PageBytes);
        if (live) add(at, stop);
        at = stop;
    }
}

bool TryReadMappedOrZero(uint64_t vaddr, void* data, uint64_t size) {
    return CopySparseMemory(vaddr, data, size);
}

bool TryReadPrtBacking(uint64_t vaddr, void* data, uint64_t size, const char** failure_reason) {
    const bool read = CopySparseMemory(vaddr, data, size);
    if (failure_reason != nullptr) *failure_reason = read ? nullptr : "sparse-backing-copy-failed";
    return read;
}

bool RestoreGuestWritable(uint64_t vaddr, uint64_t size) {
    for (auto page = vaddr & ~(PageBytes - 1); page < vaddr + size; page += PageBytes) {
        auto* region = findRegion(page);
        if (region == nullptr) return false;
        const auto original = region->original[(page % RegionBytes) / PageBytes] & 0xffu;
        if (original != PAGE_READWRITE && original != PAGE_EXECUTE_READWRITE) return false;
    }
    return protectPages(vaddr, size, Common::VirtualMemory::Mode::ReadWrite);
}

bool TryPrepareHostWrite(uint64_t vaddr, uint64_t size) {
    return gpuResources != nullptr && guestRange(vaddr, size) && gpuResources->InvalidateMemory(vaddr, size);
}

bool HandleGpuFault(Graphics::PageFaultAccess access, uint64_t fault_vaddr) noexcept {
    return gpuResources != nullptr && gpuResources->HandleFault(access, fault_vaddr);
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
        static std::atomic<std::uint32_t> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
            std::fprintf(stderr, "[kyty-gpu] buffer range at an unmapped address ignored, addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n", vaddr, size);
        }
        return 0;
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

void InstallGpuResources(Graphics::GpuResourceManager* manager) noexcept {
    gpuResources = manager;
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

Common::Time GetTime() {
    return Common::Time(static_cast<int>(GetTimeMs()));
}

}

}

namespace Loader::DemonsSoulsWarp {

const std::vector<Spawn>& Spawns() {
    static const std::vector<Spawn> none;
    return none;
}

bool Arm(std::string_view, std::string_view) {
    return false;
}

void Disarm() {}

std::string HudText() {
    return {};
}

}

void Libs::Graphics::WindowContext::UpdateTitle() {}

extern "C" [[gnu::used]] volatile std::atomic_uint32_t kyty_local_backing_read_mode{0};

extern "C" __declspec(dllexport) const char* KytyHostSwitch(const char* name) {
    if (const char* value = std::getenv(name); value != nullptr) return value;
    const char* safe = std::getenv("APS5_KYTY_GPU_SAFE");
    if (safe != nullptr && std::string_view(safe) == "1") return nullptr;
    if (std::string_view(name) == "KYTY_RENDER_CPUS") return renderCpus().empty() ? nullptr : renderCpus().c_str();
    for (const auto& [key, value] : recommendedSwitches) {
        if (std::string_view(name) == key) return value;
    }
    return nullptr;
}

const char* SDL_GetError() {
    return "KytyPS5 GPU host";
}

void SDL_DestroyWindow(SDL_Window*) {}

void SDL_QuitSubSystem(Uint32) {}

void SDL_SetWindowTitle(SDL_Window*, const char*) {}

const char* SDL_GetWindowTitle(SDL_Window*) {
    return nullptr;
}

SDL_bool SDL_GetWindowWMInfo(SDL_Window*, SDL_SysWMinfo*) {
    return SDL_FALSE;
}

void SDL_PumpEvents() {}

int SDL_GetWindowDisplayIndex(SDL_Window*) {
    return -1;
}

int SDL_GetCurrentDisplayMode(int, SDL_DisplayMode*) {
    return -1;
}

SDL_bool SDL_Vulkan_GetInstanceExtensions(SDL_Window*, unsigned int* count, const char** names) {
    static const char* const extensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    if (names != nullptr) {
        for (unsigned int index = 0; index < std::min<unsigned int>(*count, 2); ++index) names[index] = extensions[index];
    }
    *count = 2;
    return SDL_TRUE;
}

void* SDL_Vulkan_GetVkGetInstanceProcAddr() {
    static const HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    return loader != nullptr ? reinterpret_cast<void*>(GetProcAddress(loader, "vkGetInstanceProcAddr")) : nullptr;
}

SDL_bool SDL_Vulkan_CreateSurface(SDL_Window* target, VkInstance instance, VkSurfaceKHR* surface) {
    const auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (getProc == nullptr || target == nullptr) return SDL_FALSE;
    const auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(getProc(instance, "vkCreateWin32SurfaceKHR"));
    if (create == nullptr) return SDL_FALSE;
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = static_cast<HWND>(target->hwnd);
    return create(instance, &info, nullptr, surface) == VK_SUCCESS ? SDL_TRUE : SDL_FALSE;
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
    resources = &renderer;
    renderer.InitializeGpu(&videoOut);
    ensureShaders();
    started.store(true, std::memory_order_release);
    std::thread([] {
        auto previous = counts.flips.load(std::memory_order_relaxed);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            const auto now = counts.flips.load(std::memory_order_relaxed);
            std::fprintf(stderr, "[kyty-gpu] %llu flips over 10 s (%.1f fps); totals dcbs=%llu acbs=%llu event-adds=%llu eop-triggers=%llu video-triggers=%llu failed-triggers=%llu faults=%llu late-maps=%llu race-retries=%llu host-writes=%llu read-faults=%llu fault-ms=%llu read-fault-ms=%llu protect-ms=%llu protect-calls=%llu protect-syscalls=%llu backing-opens=%llu alias-copies=%llu\n", static_cast<unsigned long long>(now - previous), static_cast<double>(now - previous) / 10.0,
                         static_cast<unsigned long long>(counts.dcbs.load()), static_cast<unsigned long long>(counts.acbs.load()), static_cast<unsigned long long>(counts.adds.load()), static_cast<unsigned long long>(counts.graphicsTriggers.load()),
                         static_cast<unsigned long long>(counts.videoTriggers.load()), static_cast<unsigned long long>(counts.failedTriggers.load()), static_cast<unsigned long long>(counts.faults.load()), static_cast<unsigned long long>(counts.lateMaps.load()), static_cast<unsigned long long>(counts.raceRetries.load()), static_cast<unsigned long long>(counts.hostWrites.load()), static_cast<unsigned long long>(counts.readFaults.load()), static_cast<unsigned long long>(counts.faultNs.load() / 1000000), static_cast<unsigned long long>(counts.readFaultNs.load() / 1000000), static_cast<unsigned long long>(counts.protectNs.load() / 1000000), static_cast<unsigned long long>(counts.protectCalls.load()), static_cast<unsigned long long>(counts.protectSyscalls.load()), static_cast<unsigned long long>(counts.backingOpens.load()), static_cast<unsigned long long>(counts.aliasCopies.load()));
            previous = now;
            if (statsEnabled()) {
                static std::array<std::uint64_t, LiveCounters::Count> last{};
                std::string text;
                for (std::size_t index = 0; index < LiveCounters::Count; ++index) {
                    const auto value = LiveCounters::Value(index);
                    if (value != last[index]) text += std::string(" ") + LiveCounters::Names[index] + "=" + std::to_string(value - last[index]);
                    last[index] = value;
                }
                std::fprintf(stderr, "[kyty-gpu] 10 s counters:%s\n", text.c_str());
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
    if (gpuResources == nullptr || bytes == 0) return;
    addMapping(address, address + bytes);
    if (Libs::Graphics::GuestRange{address, bytes}.Valid()) gpuResources->MapMemory(address, bytes);
}

__declspec(dllexport) void KytyGpuUnmapMemory(std::uint64_t address, std::uint64_t bytes) {
    if (gpuResources == nullptr || bytes == 0) return;
    removeMapping(address, address + bytes);
    if (Libs::Graphics::GuestRange{address, bytes}.Valid()) gpuResources->UnmapMemory(address, bytes, true);
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
    if (gpuResources == nullptr || bytes == 0) return;
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
    resources->GetGpu().Done();
}

__declspec(dllexport) void KytyGpuWaitIdle() {
    resources->GetGpu().WaitForIdle();
}

}
