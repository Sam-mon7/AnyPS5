#include "prx/libSceAgcDriver/Graphics/include/KytyBufferCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "common/debugCounters.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace Common {

void KytyExit(const char* file, int line, const std::string& text) {
    throw std::runtime_error("Kyty buffer cache: " + text + " (" + file + ":" + std::to_string(line) + ")");
}

std::string KytyFormat(const char* format, ...) {
    std::array<char, 1024> text{};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text.data(), text.size(), format, arguments);
    va_end(arguments);
    return text.data();
}

namespace DebugCounters {

namespace {

std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Counter::Count)>& Values() {
    static std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Counter::Count)> values{};
    return values;
}

}

void Add(Counter counter, std::uint64_t value) {
    Values()[static_cast<std::size_t>(counter)].fetch_add(value, std::memory_order_relaxed);
}

std::uint64_t Take(Counter counter) {
    return Values()[static_cast<std::size_t>(counter)].exchange(0, std::memory_order_relaxed);
}

}

}

namespace Libs::Graphics {

void RequireVulkanSuccess(vk::Result result, const char* operation) {
    if (result != vk::Result::eSuccess) Common::KytyExit(__FILE__, __LINE__, std::string(operation) + " failed");
}

namespace GuestGpu {

bool IsGpuThread() {
    return AgcDriver::GuestMemory::GpuMutex().HeldByThisThread();
}

}

vk::CommandBuffer CommandBuffer::Handle() {
    auto* recorder = AgcDriver::Graphics::Recorder::Active();
    if (recorder == nullptr) Common::KytyExit(__FILE__, __LINE__, "no active recorder");
    return vk::CommandBuffer(recorder->Commands());
}

bool CommandBuffer::IsInvalid() const {
    return AgcDriver::Graphics::Recorder::Active() == nullptr || AgcDriver::Graphics::Recorder::InCompletion();
}

std::uint64_t CommandScheduler::CurrentTick() {
    auto* recorder = AgcDriver::Graphics::Recorder::Active();
    if (recorder == nullptr) return completed->load(std::memory_order_acquire) + 1;
    const auto tick = recorder->Submissions() + 1;
    if (tick != watched && !AgcDriver::Graphics::Recorder::InCompletion()) {
        watched = tick;
        recorder->OnComplete([completed = completed, tick] {
            auto seen = completed->load(std::memory_order_relaxed);
            while (seen < tick && !completed->compare_exchange_weak(seen, tick, std::memory_order_release)) {}
        });
    }
    return tick;
}

void CommandScheduler::Wait(std::uint64_t tick) {
    if (IsFree(tick)) return;
    auto* recorder = AgcDriver::Graphics::Recorder::Active();
    if (recorder == nullptr) return;
    if (tick > recorder->Submissions()) recorder->Submit();
    recorder->FinishUpTo(tick);
}

void CommandScheduler::PopPendingOperations() {
    if (auto* recorder = AgcDriver::Graphics::Recorder::Active(); recorder != nullptr && !AgcDriver::Graphics::Recorder::InCompletion()) recorder->Reap();
}

bool CommandScheduler::Active() const {
    return AgcDriver::Graphics::Recorder::Active() != nullptr;
}

bool CommandScheduler::InDeferredOperation() {
    return AgcDriver::Graphics::Recorder::InCompletion();
}

void CommandScheduler::defer(std::function<void()> operation) {
    auto* recorder = AgcDriver::Graphics::Recorder::Active();
    if (recorder == nullptr || AgcDriver::Graphics::Recorder::InCompletion()) {
        operation();
        return;
    }
    static_cast<void>(CurrentTick());
    recorder->OnComplete(std::move(operation));
}

struct PageManager::Impl {};

PageManager::PageManager() = default;
PageManager::~PageManager() = default;

std::uint64_t PageManager::GetPageSize() const {
    return TRACKER_PAGE_SIZE;
}

template <bool track>
void PageManager::UpdatePageWatchers(std::uint64_t, std::uint64_t) {}

std::atomic<std::uint64_t> protectRefusals{0};

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(std::uint64_t base_addr, RegionBits& mask) {
    if constexpr (is_read) {
        for (const auto [first, last] : mask) {
            const auto address = base_addr + first * TRACKER_PAGE_SIZE;
            const auto bytes = static_cast<std::size_t>((last - first) * TRACKER_PAGE_SIZE);
            if constexpr (track) {
                std::uint64_t calls = 0;
                if (!GuestArena::GuestArenaProtectGpuWrites_nid_no_patch(address, bytes, &calls)) protectRefusals.fetch_add(1, std::memory_order_relaxed);
            } else {
                GuestArena::GuestArenaDisarmGpuWriteWatch_nid_no_patch(address, bytes);
            }
        }
    }
}

template void PageManager::UpdatePageWatchers<true>(std::uint64_t, std::uint64_t);
template void PageManager::UpdatePageWatchers<false>(std::uint64_t, std::uint64_t);
template void PageManager::UpdatePageWatchersForRegion<true, false>(std::uint64_t, RegionBits&);
template void PageManager::UpdatePageWatchersForRegion<false, false>(std::uint64_t, RegionBits&);
template void PageManager::UpdatePageWatchersForRegion<true, true>(std::uint64_t, RegionBits&);
template void PageManager::UpdatePageWatchersForRegion<false, true>(std::uint64_t, RegionBits&);

bool BufferCache::SynchronizeBufferFromImage(Buffer&, std::uint64_t, std::uint64_t) {
    return false;
}

struct BufferCacheTestAccess {
    template <typename Visit>
    static void ForEachBuffer(BufferCache& cache, Visit&& visit) {
        for (const auto& [address, id] : cache.m_buffers) visit(cache.m_slot_buffers[id]);
    }
    static std::size_t Count(const BufferCache& cache) { return cache.m_buffers.size(); }
};

}

namespace Libs::LibKernel::Memory {

namespace {

template <typename Piece>
void ForEachCommitted(std::uint64_t address, std::uint64_t size, Piece&& piece) {
    for (const auto& [begin, end] : AgcDriver::GuestMemory::CommittedRanges(address, static_cast<std::size_t>(size))) piece(begin, end);
}

}

bool CopySparseMemory(std::uint64_t address, void* destination, std::uint64_t size) {
    auto* bytes = static_cast<std::uint8_t*>(destination);
    std::memset(bytes, 0, static_cast<std::size_t>(size));
    ForEachCommitted(address, size, [&](std::uint64_t begin, std::uint64_t end) {
        std::memcpy(bytes + (begin - address), reinterpret_cast<const void*>(begin), static_cast<std::size_t>(end - begin));
    });
    return true;
}

bool TryReadSparseBacking(std::uint64_t address, void* destination, std::uint64_t size) {
    return CopySparseMemory(address, destination, size);
}

void WriteBacking(std::uint64_t address, const void* source, std::uint64_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(source);
    ForEachCommitted(address, size, [&](std::uint64_t begin, std::uint64_t end) {
        for (auto page = begin & ~std::uint64_t{4095}; page < end; page += 4096) {
            const auto from = std::max(page, begin);
            const auto to = std::min(page + 4096, end);
            if (GuestArena::GuestArenaHasGpuOverlay_nid_no_patch(page, 4096)) {
                std::array<std::uint8_t, 4096> staged{};
                GuestArena::GuestArenaSnapshotGpuPage_nid_no_patch(page, staged.data());
                std::memcpy(staged.data() + (from - page), bytes + (from - address), static_cast<std::size_t>(to - from));
                GuestArena::GuestArenaPublishGpuPage_nid_no_patch(page, staged.data(), nullptr);
            } else {
                std::memcpy(reinterpret_cast<void*>(from), bytes + (from - address), static_cast<std::size_t>(to - from));
            }
        }
        AgcDriver::GuestMemory::MarkWritten(begin, static_cast<std::size_t>(end - begin));
    });
}

}

namespace AgcDriver::Graphics::KytyBufferCache {

namespace {

constexpr std::uint64_t Block = 65536;
constexpr std::uint64_t FaultWindow = 4u << 20u;

struct State {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    std::unique_ptr<Libs::Graphics::GraphicContext> graphics;
    std::unique_ptr<Libs::Graphics::CommandScheduler> scheduler;
    std::unique_ptr<Libs::Graphics::PageManager> pages;
    std::unique_ptr<Libs::Graphics::TextureCache> textures;
    std::unique_ptr<Libs::Graphics::BufferCache> cache;
    std::unordered_map<std::uint64_t, std::uint64_t> generations;
    std::uint64_t polledEpoch = ~std::uint64_t{0};
    std::vector<std::uint64_t> unmapped;
    std::mutex unmappedMutex;
    Statistics counts;
    Statistics reported;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

State& state() {
    static State value;
    return value;
}

bool pageFault(std::uintptr_t page, bool write);

void teardown(State& s) {
    s.cache.reset();
    s.textures.reset();
    s.pages.reset();
    s.scheduler.reset();
    if (s.allocator != VK_NULL_HANDLE) vmaDestroyAllocator(s.allocator);
    s.allocator = VK_NULL_HANDLE;
    s.graphics.reset();
    s.generations.clear();
    s.device = VK_NULL_HANDLE;
}

Libs::Graphics::BufferCache* ensure(const Context& context) {
    auto& s = state();
    if (s.cache != nullptr && s.device == context.device) return s.cache.get();
    if (s.instance == VK_NULL_HANDLE || s.getInstanceProcAddr == nullptr || context.device == VK_NULL_HANDLE) return nullptr;
    teardown(s);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(s.instance), s.getInstanceProcAddr, vk::Device(context.device), context.deviceProc);
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = s.getInstanceProcAddr;
    functions.vkGetDeviceProcAddr = context.deviceProc;
    VmaAllocatorCreateInfo info{};
    info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    info.physicalDevice = context.physical;
    info.device = context.device;
    info.instance = s.instance;
    info.vulkanApiVersion = VK_API_VERSION_1_1;
    info.pVulkanFunctions = &functions;
    if (vmaCreateAllocator(&info, &s.allocator) != VK_SUCCESS) throw std::runtime_error("Kyty buffer cache: cannot create the memory allocator");
    s.graphics = std::make_unique<Libs::Graphics::GraphicContext>();
    s.graphics->device = vk::Device(context.device);
    s.graphics->physical_device = vk::PhysicalDevice(context.physical);
    s.graphics->allocator = s.allocator;
    s.graphics->physical_device_properties = s.graphics->physical_device.getProperties();
    s.graphics->memory = [pool = GetBufferPool(context)] {
        const auto report = pool->Vram().Current();
        return std::pair<std::uint64_t, std::uint64_t>{report.budget, report.usage};
    };
    s.scheduler = std::make_unique<Libs::Graphics::CommandScheduler>();
    s.pages = std::make_unique<Libs::Graphics::PageManager>();
    s.textures = std::make_unique<Libs::Graphics::TextureCache>();
    s.cache = std::make_unique<Libs::Graphics::BufferCache>(*s.graphics, *s.scheduler, *s.pages, *s.textures);
    s.device = context.device;
    GuestArena::GuestArenaSetGpuPageFault_nid_no_patch(pageFault);
    return s.cache.get();
}

bool pageFault(std::uintptr_t page, bool write) {
    std::lock_guard gpu(GuestMemory::GpuMutex());
    auto& s = state();
    if (s.cache == nullptr) return false;
    const auto base = static_cast<std::uint64_t>(page) & ~std::uint64_t{4095};
    if (!s.cache->IsRegionGpuModified(base, 4096)) return false;
    ++s.counts.faults;
    s.cache->ReadMemory(base, 4096, write);
    return true;
}

void pollCpuWrites(State& s, std::uint64_t begin, std::uint64_t end) {
    const auto first = begin / Block * Block;
    const auto last = (end + Block - 1) / Block * Block;
    const auto blocks = static_cast<std::size_t>((last - first) / Block);
    const auto generation = GuestMemory::CollectWrites(first, static_cast<std::size_t>(last - first));
    if (generation == 0) {
        s.cache->InvalidateMemory(begin, end - begin);
        ++s.counts.invalidations;
        return;
    }
    std::vector<std::uint64_t> current(blocks, 0);
    for (std::size_t index = 0; index < blocks; ++index) {
        if (const auto found = s.generations.find(first / Block + index); found != s.generations.end()) current[index] = found->second;
    }
    std::vector<std::uint8_t> changed(blocks, 0);
    GuestMemory::ChangedBlocks(first, static_cast<std::size_t>(last - first), current, changed);
    for (std::size_t index = 0; index < blocks;) {
        if (changed[index] == 0) {
            ++index;
            continue;
        }
        auto stop = index;
        while (stop < blocks && changed[stop] != 0) s.generations[first / Block + stop++] = generation;
        const auto from = std::max(begin, first + index * Block);
        const auto to = std::min(end, first + stop * Block);
        if (from < to) {
            s.cache->InvalidateMemory(from, to - from);
            ++s.counts.invalidations;
        }
        index = stop;
    }
}

void report(State& s) {
    const auto now = std::chrono::steady_clock::now();
    if (now - s.lastReport < std::chrono::seconds(10)) return;
    s.lastReport = now;
    using Common::DebugCounters::Counter;
    s.counts.uploadBytes += Common::DebugCounters::Take(Counter::BufferUploadBytes);
    s.counts.downloadBytes += Common::DebugCounters::Take(Counter::BufferDownloadBytes);
    s.counts.evictions += Common::DebugCounters::Take(Counter::BufferEvictions);
    std::uint64_t buffers = 0, bytes = 0;
    Libs::Graphics::BufferCacheTestAccess::ForEachBuffer(*s.cache, [&](const Libs::Graphics::Buffer& buffer) {
        ++buffers;
        bytes += buffer.Size();
    });
    const auto& c = s.counts;
    const auto& r = s.reported;
    ProfilePrint_nid_no_patch("[kyty-buffers] (10 s) descriptors %llu (%llu written), address builds %llu with %llu ranges, unmapped faults %llu (%llu windows mapped), CPU invalidations %llu, uploads %.1f MiB, downloads %.1f MiB, GPU-page faults %llu, evictions %llu, protect refusals %llu; %llu buffers, %.1f MiB\n",
        static_cast<unsigned long long>(c.descriptors - r.descriptors), static_cast<unsigned long long>(c.writtenDescriptors - r.writtenDescriptors),
        static_cast<unsigned long long>(c.addressBuilds - r.addressBuilds), static_cast<unsigned long long>(c.addressRanges - r.addressRanges),
        static_cast<unsigned long long>(c.unmapped - r.unmapped), static_cast<unsigned long long>(c.mapped - r.mapped),
        static_cast<unsigned long long>(c.invalidations - r.invalidations), (c.uploadBytes - r.uploadBytes) / 1048576.0, (c.downloadBytes - r.downloadBytes) / 1048576.0,
        static_cast<unsigned long long>(c.faults - r.faults), static_cast<unsigned long long>(c.evictions - r.evictions),
        static_cast<unsigned long long>(Libs::Graphics::protectRefusals.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(buffers), bytes / 1048576.0);
    s.reported = s.counts;
}

}

bool Enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("APS5_KYTY_BUFFER_CACHE");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return enabled;
}

void SetInstance(VkInstance instance, PFN_vkGetInstanceProcAddr getInstanceProcAddr) {
    if (!Enabled()) return;
    auto& s = state();
    s.instance = instance;
    s.getInstanceProcAddr = getInstanceProcAddr;
}

std::optional<VkDescriptorBufferInfo> Descriptor(const Context& context, std::uint64_t address, std::size_t bytes, bool written, std::uint32_t& adjustment) {
    if (!Enabled() || bytes == 0 || Recorder::Active() == nullptr || Recorder::InCompletion()) return std::nullopt;
    GuestMemory::AssertGpuLockHeld("Kyty buffer cache descriptor");
    auto* cache = ensure(context);
    if (cache == nullptr) return std::nullopt;
    auto& s = state();
    pollCpuWrites(s, address, address + bytes);
    const auto [buffer, offset] = cache->ObtainBuffer(address, bytes, written);
    const auto alignment = std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 4);
    adjustment = static_cast<std::uint32_t>(offset % alignment);
    auto range = adjustment % 4 == 0 ? bytes + adjustment : 4 * (adjustment / 4 + bytes / 4 + 1);
    range = std::min<std::uint64_t>(range, buffer->Size() - (offset - adjustment));
    ++s.counts.descriptors;
    if (written) ++s.counts.writtenDescriptors;
    report(s);
    return VkDescriptorBufferInfo{static_cast<VkBuffer>(buffer->Handle()), offset - adjustment, range};
}

void AddressRanges(const Context& context, std::vector<ShaderRecompiler::BdaAbi::Range>& ranges) {
    if (!Enabled() || Recorder::Active() == nullptr || Recorder::InCompletion()) return;
    GuestMemory::AssertGpuLockHeld("Kyty buffer cache address ranges");
    auto* cache = ensure(context);
    if (cache == nullptr) return;
    auto& s = state();
    std::vector<std::uint64_t> faults;
    {
        std::lock_guard lock(s.unmappedMutex);
        faults.swap(s.unmapped);
    }
    std::sort(faults.begin(), faults.end());
    faults.erase(std::unique(faults.begin(), faults.end()), faults.end());
    for (const auto address : faults) {
        const auto range = GuestAllocations::GuestAllocationsLookup_nid_postfix(static_cast<std::uintptr_t>(address), false);
        if (range == nullptr || !range->readable) continue;
        const auto window = address / FaultWindow * FaultWindow;
        const auto begin = std::max<std::uint64_t>(range->address, window);
        const auto end = std::min<std::uint64_t>(range->address + range->bytes, window + FaultWindow);
        if (begin >= end || cache->IsRegionRegistered(begin, end - begin)) continue;
        pollCpuWrites(s, begin, end);
        static_cast<void>(cache->ObtainBuffer(begin, end - begin, false));
        ++s.counts.mapped;
    }
    if (const auto epoch = GuestMemory::CollectEpochBumps(); epoch != s.polledEpoch || !faults.empty()) {
        s.polledEpoch = epoch;
        Libs::Graphics::RangeSet mapped;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
        Libs::Graphics::BufferCacheTestAccess::ForEachBuffer(*cache, [&](const Libs::Graphics::Buffer& buffer) {
            if (buffer.CpuAddress() != 0) spans.emplace_back(buffer.CpuAddress(), buffer.Size());
        });
        for (const auto& [address, bytes] : spans) {
            pollCpuWrites(s, address, address + bytes);
            mapped.Add(address, bytes);
        }
        cache->SynchronizeBda(mapped);
    }
    Libs::Graphics::BufferCacheTestAccess::ForEachBuffer(*cache, [&](const Libs::Graphics::Buffer& buffer) {
        if (buffer.CpuAddress() == 0 || buffer.BufferDeviceAddress() == 0) return;
        ranges.push_back({buffer.CpuAddress(), buffer.CpuAddress() + buffer.Size(), buffer.BufferDeviceAddress(), ShaderRecompiler::BdaAbi::Read | ShaderRecompiler::BdaAbi::Write, 0});
        ++s.counts.addressRanges;
    });
    ++s.counts.addressBuilds;
    cache->RunGarbageCollector();
    report(s);
}

void NoteUnmapped(std::uint64_t address) {
    if (!Enabled()) return;
    auto& s = state();
    std::lock_guard lock(s.unmappedMutex);
    s.unmapped.push_back(address);
    ++s.counts.unmapped;
}

void Flush(std::uint64_t address, std::size_t bytes) {
    if (!Enabled() || bytes == 0) return;
    auto& s = state();
    if (s.cache == nullptr || Recorder::InCompletion()) return;
    std::lock_guard gpu(GuestMemory::GpuMutex());
    if (!s.cache->IsRegionGpuModified(address, bytes)) return;
    ++s.counts.downloads;
    s.cache->ReadMemory(address, bytes, false);
}

void Clear(VkDevice device) {
    auto& s = state();
    if (s.device == device) teardown(s);
}

Statistics Counters() {
    return state().counts;
}

}
