#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/GatherShadow.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GpuAuthorityCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/DispatchBuildTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libSceAgcDriver/Execution/include/ThreadScratch.hpp"
#include "ThreadOwned.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PlaceholderImages.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ParameterArena.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MemoryAccounting.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ImageEpochs.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <memory>
#include <mutex>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <stdexcept>
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Optimization/include/Optimization/ShaderStageInputInfo.hpp"
#include "RdnaDecoder/include/RdnaDecoder/RdnaDescriptorFormat.hpp"
#include "IntermediateRepresentation/IrMetadata/ShaderStage.hpp"
#include "ImageTableAbi.hpp"
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libSceAgcDriver/Graphics/include/LazyCensus.hpp"

namespace AgcDriver::Graphics {
namespace {

bool overlap(std::uint64_t first, std::size_t firstSize, std::uint64_t second, std::size_t secondSize) {
    return first < second + secondSize && second < first + firstSize;
}

bool registeredImageViewsCurrent(std::span<const ShaderRecompiler::RegisteredImageView> views) {
    if (views.empty()) return true;
    std::vector<std::uintptr_t> addresses;
    addresses.reserve(views.size());
    for (const auto& view : views) addresses.push_back(static_cast<std::uintptr_t>(view.address));
    std::vector<std::shared_ptr<const GuestAllocations::Range>> ranges(views.size());
    GuestAllocations::GuestAllocationsLookupEach_nid_postfix(addresses.data(), addresses.size(), ranges.data());
    for (std::size_t index = 0; index < views.size(); ++index) {
        const auto& view = views[index];
        const auto& range = ranges[index];
        if (view.hasRange != (range != nullptr)) return false;
        if (range != nullptr && (range->address != view.rangeAddress || range->bytes != view.rangeBytes || range->allocationAddress != view.allocationAddress || range->allocationBytes != view.allocationBytes || range->backingSerial != view.backingSerial || range->readable != view.readable || range->writable != view.writable || range->gpu != view.gpu)) return false;
    }
    return true;
}

VkComponentSwizzle ComponentSwizzleFor(std::uint8_t dstSel) {
    switch (dstSel) {
        case 0: return VK_COMPONENT_SWIZZLE_ZERO;
        case 1: return VK_COMPONENT_SWIZZLE_ONE;
        case 4: return VK_COMPONENT_SWIZZLE_R;
        case 5: return VK_COMPONENT_SWIZZLE_G;
        case 6: return VK_COMPONENT_SWIZZLE_B;
        case 7: return VK_COMPONENT_SWIZZLE_A;
        default: throw std::runtime_error("AGC graphics: guest texture descriptor has an invalid destination channel selector " + std::to_string(dstSel));
    }
}

VkComponentMapping ViewComponents(const GuestTextureResource& resource) {
    if (IsConvertedTextureFormat(resource.format) && !IsRepackedTextureFormat(resource.format) && resource.format != static_cast<std::uint32_t>(ShaderRecompiler::IrBufferFormat::Format11_11_10Float)) return {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    const auto channel = [&](std::uint8_t dstSel) { return TextureComponentChannel(resource.format, ComponentSwizzleFor(dstSel)); };
    return {channel(resource.dstSelX), channel(resource.dstSelY), channel(resource.dstSelZ), channel(resource.dstSelW)};
}

// Sampled textures are reused across draws and dispatches while their guest bytes are unchanged; a
// byte copy of the guest surface validates each reuse, so CPU or GPU writes to it force a re-upload.
// The key is everything a lookup matches: the device, the eight descriptor words and the swizzle.
struct AllocationIdentity {
    std::uint64_t rangeAddress = 0;
    std::uint64_t rangeBytes = 0;
    std::uint64_t allocationAddress = 0;
    std::uint64_t allocationBytes = 0;
    std::uint64_t backingSerial = 0;
    std::uint32_t flags = 0;
    bool operator==(const AllocationIdentity&) const = default;
};

AllocationIdentity ImageAllocationIdentity(std::span<const std::uint32_t> words) {
    const auto address = TextureResourceAllocationAddress(words);
    if (!address) return {};
    const auto range = GuestAllocations::GuestAllocationsLookup_nid_postfix(static_cast<std::uintptr_t>(*address), false);
    if (range == nullptr) return {};
    return {range->address, range->bytes, range->allocationAddress, range->allocationBytes, range->backingSerial, (range->readable ? 1u : 0u) | (range->writable ? 2u : 0u) | (range->gpu ? 4u : 0u)};
}

struct TextureKey {
    VkDevice device;
    std::array<std::uint32_t, 8> words;
    std::array<std::uint32_t, 4> components;
    bool depthCompare = false;
    AllocationIdentity allocation;
    bool clearSnapshot = false;
    bool operator==(const TextureKey&) const = default;
};

std::uint64_t hashWords(std::uint64_t hash, const std::uint32_t* words, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) hash = (hash ^ words[i]) * 1099511628211ull;
    return hash;
}

struct TextureKeyHash {
    std::size_t operator()(const TextureKey& key) const noexcept {
        auto hash = 14695981039346656037ull ^ reinterpret_cast<std::uintptr_t>(key.device);
        hash = hashWords(hash, key.words.data(), key.words.size()) ^ static_cast<std::uint64_t>(key.depthCompare);
        for (const auto value : {key.allocation.rangeAddress, key.allocation.rangeBytes, key.allocation.allocationAddress, key.allocation.allocationBytes, key.allocation.backingSerial, static_cast<std::uint64_t>(key.allocation.flags)}) hash = (hash ^ value) * 1099511628211ull;
        hash = (hash ^ static_cast<std::uint64_t>(key.clearSnapshot)) * 1099511628211ull;
        return static_cast<std::size_t>(hashWords(hash, key.components.data(), key.components.size()));
    }
};

TextureKey MakeTextureKey(VkDevice device, std::span<const std::uint32_t> words, VkComponentMapping components, bool depthCompare = false) {
    TextureKey key{device, {}, {static_cast<std::uint32_t>(components.r), static_cast<std::uint32_t>(components.g), static_cast<std::uint32_t>(components.b), static_cast<std::uint32_t>(components.a)}};
    key.depthCompare = depthCompare;
    key.allocation = ImageAllocationIdentity(words);
    std::copy(words.begin(), words.end(), key.words.begin());
    key.words[5] &= ~((0xfffu << 8u) | (1u << 25u));
    key.words[6] &= ~0xffu;
    return key;
}

struct CachedTexture {
    TextureKey key;
    std::uint64_t address;
    std::vector<std::byte> bytes;
    std::shared_ptr<Texture> texture;
    // A fast-cleared surface is cached as its clear texels; it stays valid while the keys are unchanged.
    DccKeys keys = DccKeys::Uncompressed;
    // Write generation the snapshot is known current at (see GuestMemory::CollectWrites).
    std::uint64_t generation = 0;
    // A texture viewing a storage image on the GPU has no snapshot; it stays valid while that image
    // is still its surface's cached image and nothing wrote the guest memory since it matched.
    std::shared_ptr<StorageTexture> source;
    std::uint64_t sourceVersion = 0;
    std::uint64_t accounted = 0;
    std::uint64_t present = 0;
    std::optional<std::vector<std::pair<std::uint64_t, std::uint64_t>>> committed;
    // The untrackedKey the snapshot was last read or compared equal at, for a surface outside the
    // write watch; zero when none was taken.
    std::array<std::uint64_t, 4> untracked{};
    std::uint64_t surfaceBytes = 0;
    std::vector<DepthArrayLayer> depthArrayLayers;
};

// Entries in use order (front = most recent) with a hash index by key: a lookup is O(1) and the
// eviction takes the back. APS5_NO_TEXTURE_HASH=1 finds entries by scanning the list (the index is
// still kept), the linear lookup of before.
struct TextureCache {
    AgcDriver::Mutex mutex;
    std::list<CachedTexture> entries;
    std::unordered_map<TextureKey, std::list<CachedTexture>::iterator, TextureKeyHash> index;
    std::unordered_multimap<const StorageTexture*, std::list<CachedTexture>::iterator> views;
    std::vector<std::pair<std::vector<std::byte>, std::uint64_t>> spare;
    std::uint64_t spareBytes = 0;
    std::uint64_t bytes = 0;
    std::size_t passed = 0;
    std::chrono::steady_clock::time_point passedSince{};
    std::uint64_t firstUsesPresent = 0;
    std::unordered_set<TextureKey, TextureKeyHash> firstUses;
    struct Counts {
        std::uint64_t inserted = 0;
        std::uint64_t uncached = 0;
        std::uint64_t evicted = 0;
        std::uint64_t passed = 0;
        std::uint64_t stale = 0;
    } counts;
    std::chrono::steady_clock::time_point countsReported = std::chrono::steady_clock::now();
    VkPhysicalDevice budgetDevice = VK_NULL_HANDLE;
    std::chrono::steady_clock::time_point budgetRead{};
    std::uint64_t budget = 0;
    std::uint64_t reportedBudget = 0;
    std::uint64_t reportedStorage = 0;
    std::chrono::steady_clock::time_point reportedAt{};
    std::uint64_t erasures = 0;
};

TextureCache& Textures() {
    static TextureCache cache;
    return cache;
}

unsigned& sampledCacheHeldThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<unsigned, Slot>();
}
unsigned& storageCacheHeldThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<unsigned, Slot>();
}
unsigned& sampledCacheReclaimableThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<unsigned, Slot>();
}
unsigned& storageCacheReclaimableThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<unsigned, Slot>();
}

struct CacheHold {
    unsigned& depth;
    explicit CacheHold(unsigned& depth) : depth(depth) { ++depth; }
    ~CacheHold() { --depth; }
    CacheHold(const CacheHold&) = delete;
    CacheHold& operator=(const CacheHold&) = delete;
};

struct UncachedImages {
    AgcDriver::Mutex mutex;
    std::vector<std::weak_ptr<StorageTexture>> images;
    std::atomic<bool> any{false};
};

UncachedImages& Uncached() {
    static UncachedImages uncached;
    return uncached;
}

void noteUncached(const std::shared_ptr<StorageTexture>& image) {
    auto& uncached = Uncached();
    std::lock_guard lock(uncached.mutex);
    uncached.images.push_back(image);
    uncached.any.store(true, std::memory_order_release);
}

std::atomic<bool>& TexturesReleased() {
    static std::atomic<bool> released{false};
    return released;
}

void noteReleased(const std::shared_ptr<Texture>& texture) {
    texture->MarkReleased();
    TexturesReleased().store(true, std::memory_order_release);
}

bool SamplerCacheDisabled() {
    static const bool disabled = std::getenv("APS5_NO_SAMPLER_CACHE") != nullptr;
    return disabled;
}

bool TextureHashEnabled() {
    static const bool disabled = std::getenv("APS5_NO_TEXTURE_HASH") != nullptr;
    return !disabled;
}

std::list<CachedTexture>::iterator findTexture(TextureCache& cache, const TextureKey& key) {
    if (TextureHashEnabled()) {
        const auto found = cache.index.find(key);
        return found == cache.index.end() ? cache.entries.end() : found->second;
    }
    for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
        if (it->key == key) return it;
    }
    return cache.entries.end();
}

void eraseTexture(TextureCache& cache, std::list<CachedTexture>::iterator it) {
    cache.bytes -= it->accounted;
    if (it->source != nullptr) {
        for (auto [view, last] = cache.views.equal_range(it->source.get()); view != last; ++view) {
            if (view->second == it) {
                cache.views.erase(view);
                break;
            }
        }
    }
    if (it->texture.use_count() > 1) noteReleased(it->texture);
    cache.index.erase(it->key);
    cache.entries.erase(it);
    ++cache.erasures;
}

void dropUncachedViews(TextureCache& cache) {
    auto& uncached = Uncached();
    if (!uncached.any.load(std::memory_order_acquire)) return;
    std::vector<std::weak_ptr<StorageTexture>> images;
    {
        std::lock_guard lock(uncached.mutex);
        images.swap(uncached.images);
        uncached.any.store(false, std::memory_order_release);
    }
    for (const auto& weak : images) {
        const auto image = weak.lock();
        if (image == nullptr) continue;
        for (auto view = cache.views.find(image.get()); view != cache.views.end(); view = cache.views.find(image.get())) eraseTexture(cache, view->second);
    }
}

// Moves an entry to the front (most recently used).
void touchTexture(TextureCache& cache, std::list<CachedTexture>::iterator it) {
    cache.entries.splice(cache.entries.begin(), cache.entries, it);
    it->present = Recorder::Presents();
}

constexpr std::uint64_t SpareSnapshotBytes = 256ull << 20;

void parkSnapshot(TextureCache& cache, std::vector<std::byte>& bytes) {
    const auto size = bytes.size();
    if (size == 0) return;
    const auto present = Recorder::Presents();
    while (!cache.spare.empty() && (present - cache.spare.front().second > SpareSnapshotPresents || cache.spareBytes + size > SpareSnapshotBytes)) {
        cache.spareBytes -= cache.spare.front().first.size();
        cache.spare.erase(cache.spare.begin());
    }
    if (cache.spareBytes + size <= SpareSnapshotBytes) {
        cache.spareBytes += size;
        cache.spare.emplace_back(std::move(bytes), present);
    }
    bytes = std::vector<std::byte>{};
}

std::vector<std::byte> spareSnapshot(TextureCache& cache, std::size_t bytes) {
    const auto present = Recorder::Presents();
    while (!cache.spare.empty() && present - cache.spare.front().second > SpareSnapshotPresents) {
        cache.spareBytes -= cache.spare.front().first.size();
        cache.spare.erase(cache.spare.begin());
    }
    for (auto spare = cache.spare.begin(); spare != cache.spare.end(); ++spare) {
        if (spare->first.size() != bytes) continue;
        cache.spareBytes -= bytes;
        std::vector<std::byte> taken = std::move(spare->first);
        cache.spare.erase(spare);
        return taken;
    }
    return {};
}

bool admitTexture(TextureCache& cache, const TextureKey& key, std::uint64_t incoming, std::uint64_t budget) {
    if (cache.bytes + incoming <= budget) return true;
    if (const auto now = std::chrono::steady_clock::now(); now - cache.passedSince >= std::chrono::seconds(1)) {
        cache.passed = 0;
        cache.passedSince = now;
    }
    const auto present = Recorder::Presents();
    if (cache.firstUsesPresent != present) {
        cache.firstUses.clear();
        cache.firstUsesPresent = present;
    }
    const bool scan = incoming != 0 && present != 0;
    const bool firstUse = scan && !cache.firstUses.contains(key);
    const auto ceiling = budget + budget / 4u;
    while (!cache.entries.empty() && cache.bytes + incoming > budget) {
        const auto back = std::prev(cache.entries.end());
        const bool over = cache.bytes + incoming > ceiling;
        if (scan && !over && (firstUse ? back->present + 1u >= present : back->present == present)) {
            cache.firstUses.insert(key);
            ++cache.counts.uncached;
            return false;
        }
        const bool keep = back->accounted == 0 || (!over && back->texture.use_count() > 1);
        if (keep && cache.passed < cache.entries.size()) {
            cache.entries.splice(cache.entries.begin(), cache.entries, back);
            ++cache.passed;
            ++cache.counts.passed;
        } else if (keep && !over) {
            break;
        } else {
            eraseTexture(cache, back);
            ++cache.counts.evicted;
        }
    }
    return true;
}

void reportCacheCounts(TextureCache& cache, std::uint64_t budget) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - cache.countsReported < std::chrono::seconds(10)) return;
    cache.countsReported = now;
    const auto counts = std::exchange(cache.counts, {});
    AgcDriver::ProfilePrint_nid_no_patch("[texture-cache] (10 s): %llu textures cached, %llu used uncached (first use this frame with the least recently used entry used this frame or the last, or a later use with it used this frame), %llu evicted, %llu held ones passed over, %llu found changed; %llu MiB in %zu entries, budget %llu MiB, frame %llu\n", static_cast<unsigned long long>(counts.inserted), static_cast<unsigned long long>(counts.uncached), static_cast<unsigned long long>(counts.evicted), static_cast<unsigned long long>(counts.passed), static_cast<unsigned long long>(counts.stale), static_cast<unsigned long long>(cache.bytes >> 20u), cache.entries.size(), static_cast<unsigned long long>(budget >> 20u), static_cast<unsigned long long>(Recorder::Presents()));
}

struct ImageMemoEntry {
    VkDevice device = VK_NULL_HANDLE;
    std::array<std::uint32_t, 8> words{};
    GuestTextureResource resource{};
    std::uint64_t guestBytes = 0;
    AllocationIdentity allocation;
    VkComponentMapping components{};
    bool hasEntry = false;
    std::list<CachedTexture>::iterator entry{};
    std::uint64_t erasures = 0;
};

constexpr std::size_t ImageMemoSlots = 1024;
struct ImageMemoTag {};

ImageMemoEntry& imageMemoSlot(std::span<const std::uint32_t> words) {
    auto& imageMemo = HostThreadLocal<std::unique_ptr<std::array<ImageMemoEntry, ImageMemoSlots>>, ImageMemoTag>();
    if (imageMemo == nullptr) imageMemo = std::make_unique<std::array<ImageMemoEntry, ImageMemoSlots>>();
    const auto hash = hashWords(14695981039346656037ull, words.data(), words.size());
    return (*imageMemo)[(hash ^ (hash >> 29u)) % ImageMemoSlots];
}

struct SampledKeyProofEntry {
    std::uint64_t dccAddress = 0;
    std::uint64_t guestBytes = 0;
    std::uint32_t format = 0;
    bool dccAlphaOnMsb = false;
    DccKeyProof proof;
};

constexpr std::size_t SampledKeyProofSlots = 1024;
struct SampledKeyProofTag {};

SampledKeyProofEntry& sampledKeyProofSlot(std::uint64_t dccAddress, std::uint64_t guestBytes, std::uint32_t format, bool dccAlphaOnMsb) {
    auto& proofs = HostThreadLocal<std::unique_ptr<std::array<SampledKeyProofEntry, SampledKeyProofSlots>>, SampledKeyProofTag>();
    if (proofs == nullptr) proofs = std::make_unique<std::array<SampledKeyProofEntry, SampledKeyProofSlots>>();
    const auto hash = dccAddress ^ (guestBytes * 0x9e3779b97f4a7c15ull) ^ (static_cast<std::uint64_t>(format) << 32u) ^ (dccAlphaOnMsb ? 0x94d049bb133111ebull : 0ull);
    return (*proofs)[(hash ^ (hash >> 29u)) % SampledKeyProofSlots];
}

const ImageMemoEntry& decodedImage(VkDevice device, std::span<const std::uint32_t> words) {
    auto& slot = imageMemoSlot(words);
    const auto allocation = ImageAllocationIdentity(words);
    if (slot.device == device && slot.allocation == allocation && std::equal(words.begin(), words.end(), slot.words.begin())) return slot;
    slot.device = VK_NULL_HANDLE;
    slot.resource = DecodeTextureResource(words);
    slot.guestBytes = SurfaceGuestBytes(slot.resource);
    slot.allocation = allocation;
    slot.components = ViewComponents(slot.resource);
    std::copy(words.begin(), words.end(), slot.words.begin());
    slot.hasEntry = false;
    slot.device = device;
    return slot;
}

std::list<CachedTexture>::iterator findSampledEntry(TextureCache& cache, VkDevice device, const std::array<std::uint32_t, 8>& words, VkComponentMapping components) {
    auto& slot = imageMemoSlot(words);
    const bool same = slot.device == device && slot.allocation == ImageAllocationIdentity(words) && slot.words == words && slot.components.r == components.r && slot.components.g == components.g && slot.components.b == components.b && slot.components.a == components.a;
    if (same && slot.hasEntry && slot.erasures == cache.erasures) return slot.entry;
    const auto it = findTexture(cache, MakeTextureKey(device, words, components));
    if (same) {
        slot.hasEntry = it != cache.entries.end();
        slot.entry = it;
        slot.erasures = cache.erasures;
    }
    return it;
}

// APS5_PROFILE_DRAW: what the sampled-texture and storage-image lookups did, printed as [textures]
// every 10 s (cumulative). Storage-sourced sampled textures need no CPU read; snapshots do, and a
// pending read is a snapshot compare or read made while recorded work still wrote the surface (the
// flush hook may find that work already signaled, so this bounds the hook syncs from above; the
// [hooksync] line attributes the actual waits).
struct TextureCounters {
    std::atomic<std::uint64_t> fromStorage{0};
    std::atomic<std::uint64_t> snapshots{0};
    std::atomic<std::uint64_t> cleared{0};
    std::atomic<std::uint64_t> pendingReads{0};
    std::atomic<std::uint64_t> storageFallbacks{0};
    std::atomic<std::uint64_t> records{0};
    std::atomic<std::uint64_t> fastHits{0};
    std::atomic<std::uint64_t> fastMisses{0};
    std::atomic<std::uint64_t> storageHits{0};
    std::atomic<std::uint64_t> storageCreated{0};
    // Storage images a Revalidate refreshed directly instead of through the lookups (T1, see
    // ShaderResources::refreshOwnObjects).
    std::atomic<std::uint64_t> ownRefreshes{0};
    std::atomic<std::uint64_t> tablesReused{0};
    std::atomic<std::uint64_t> tablesReplayed{0};
    std::atomic<std::uint64_t> tablesResolved{0};
    std::atomic<std::uint64_t> tableWritesKept{0};
    std::atomic<std::uint64_t> tableWritesMade{0};
    std::atomic<std::uint64_t> variantSurfaces{0};
    std::atomic<std::uint64_t> variantSubstituted{0};
    std::atomic<std::uint64_t> variantRejected{0};
    // Snapshot hits over memory outside the write watch: kept by an unchanged UntrackedHint key, or
    // by a byte compare.
    std::atomic<std::uint64_t> untrackedKept{0};
    std::atomic<std::uint64_t> untrackedCompared{0};
    std::array<std::atomic<std::uint64_t>, 5> snapshotAccepted{};
    std::atomic<std::uint64_t> partialCollected{0};
    std::atomic<std::uint64_t> partialUnavailable{0};
    std::atomic<std::uint64_t> partialProven{0};
    std::atomic<std::int64_t> lastReport{0};
};

TextureCounters& TextureCounts() {
    static TextureCounters counters;
    return counters;
}

std::uint64_t& sampledLookupKeyScansThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<std::uint64_t, Slot>();
}
std::uint64_t& sampledLookupKeyProofsThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<std::uint64_t, Slot>();
}

bool TextureCountersReported() {
    static const bool reported = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return reported;
}

void reportTextureCounters() {
    if (!TextureCountersReported()) return;
    auto& counters = TextureCounts();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = counters.lastReport.load();
    if (nowMs - last < 10000 || !counters.lastReport.compare_exchange_strong(last, nowMs)) return;
    const auto count = [](const std::atomic<std::uint64_t>& value) { return static_cast<unsigned long long>(value.load(std::memory_order_relaxed)); };
    AgcDriver::ProfilePrint_nid_no_patch("[textures] partial snapshot fast proofs %llu\n", count(counters.partialProven));
    AgcDriver::ProfilePrint_nid_no_patch("[textures] snapshot accepted: clear %llu, whole unchanged %llu, subset unchanged %llu, hint %llu, compared %llu; partial collects %llu succeeded, %llu unavailable\n", count(counters.snapshotAccepted[0]), count(counters.snapshotAccepted[1]), count(counters.snapshotAccepted[2]), count(counters.snapshotAccepted[3]), count(counters.snapshotAccepted[4]), count(counters.partialCollected), count(counters.partialUnavailable));
    AgcDriver::ProfilePrint_nid_no_patch("[textures] sampled created: %llu from storage images, %llu cleared on the GPU, %llu snapshots (%llu snapshot reads over recorded writes inside cachedTexture, %llu storage-path fallbacks); stage-A records %llu: %llu fast hits, %llu full lookups; storage images %llu hits, %llu created, %llu own-object refreshes; image tables %llu reused, %llu resolved; dynamic-resolution variants: %llu surfaces, %llu entries substituted, %llu entries rejected; untracked snapshots %llu kept by the write-watch hint, %llu compared\n", count(counters.fromStorage), count(counters.cleared), count(counters.snapshots), count(counters.pendingReads), count(counters.storageFallbacks), count(counters.records), count(counters.fastHits), count(counters.fastMisses), count(counters.storageHits), count(counters.storageCreated), count(counters.ownRefreshes), count(counters.tablesReused), count(counters.tablesResolved), count(counters.variantSurfaces), count(counters.variantSubstituted), count(counters.variantRejected), count(counters.untrackedKept), count(counters.untrackedCompared));
}

// What the sampled-texture lookups on this thread proved their returned objects current against,
// taken by object into the ShaderResources being built or revalidated (captureValidation), which
// then repeats the proof from write stamps on its next Revalidate. The lookups run inside one build
// on one thread; records nobody takes (a build that threw, a resident target looked up outside a
// build, a thread that never captures) are dropped by the next capture or by the bound below.
struct LookupRecord {
    const void* object;
    GuestTextureResource resource;
    std::uint64_t bytes;
    DccKeys keys;
    std::uint64_t generation;
    const StorageTexture* source;
    bool depth = false;
    std::optional<std::vector<std::pair<std::uint64_t, std::uint64_t>>> committed;
};

constexpr std::size_t LookupLogBound = 1024;
constexpr std::size_t NoBuildLookups = std::numeric_limits<std::size_t>::max();
std::size_t& buildLookupsStartThreadState() {
    struct State { std::size_t value = NoBuildLookups; };
    struct Slot {};
    return AgcDriver::ThreadScratch<State, Slot>().value;
}
bool& buildLookupsPastBoundThreadState() {
    struct Slot {};
    return AgcDriver::ThreadScratch<bool, Slot>();
}
std::atomic<std::uint64_t> lookupBuildsPastBound{0};

std::vector<LookupRecord>& lookupLog() {
    struct Slot {};
    return AgcDriver::ThreadScratch<std::vector<LookupRecord>, Slot>();
}

void logLookup(LookupRecord record) {
    auto& log = lookupLog();
    if (log.size() >= LookupLogBound) {
        const auto dropped = std::min(LookupLogBound / 2, buildLookupsStartThreadState());
        if (dropped < LookupLogBound / 2) buildLookupsPastBoundThreadState() = true;
        log.erase(log.begin(), log.begin() + static_cast<std::ptrdiff_t>(dropped));
        if (buildLookupsStartThreadState() != NoBuildLookups) buildLookupsStartThreadState() -= dropped;
    }
    log.push_back(std::move(record));
}

void logTextureLookup(const CachedTexture& entry, const GuestTextureResource& resource, std::uint64_t generation) {
    static const bool partialProof = std::getenv("APS5_NO_PARTIAL_SNAPSHOT_PROOF") == nullptr;
    if (partialProof && entry.source == nullptr && entry.keys == DccKeys::Uncompressed && entry.committed.has_value() && entry.generation != 0 && !entry.texture->Released()) {
        logLookup({entry.texture.get(), resource, entry.surfaceBytes, entry.keys, entry.generation, nullptr, false, entry.committed});
    } else {
        logLookup({entry.texture.get(), resource, entry.surfaceBytes, entry.keys, generation, entry.source.get()});
    }
}

class BuildLookups {
public:
    BuildLookups() : outermost(buildLookupsStartThreadState() == NoBuildLookups) { buildLookupsStartThreadState() = std::min(buildLookupsStartThreadState(), lookupLog().size()); }
    ~BuildLookups() {
        if (!outermost) return;
        buildLookupsStartThreadState() = NoBuildLookups;
        if (buildLookupsPastBoundThreadState()) lookupBuildsPastBound.fetch_add(1, std::memory_order_relaxed);
        buildLookupsPastBoundThreadState() = false;
        auto& log = lookupLog();
        if (log.size() > LookupLogBound) log.erase(log.begin(), log.end() - static_cast<std::ptrdiff_t>(LookupLogBound));
        if (log.capacity() > LookupLogBound + LookupLogBound / 2) log.shrink_to_fit();
    }
    BuildLookups(const BuildLookups&) = delete;
    BuildLookups& operator=(const BuildLookups&) = delete;

private:
    bool outermost;
};

void refreshForView(StorageTexture& source, const GuestTextureResource& resource) {
    if (resource.dimension == TextureDimension::k2D && source.ImageLayers() > 1) source.RefreshLayers(resource.baseArray, 1);
    else source.Refresh();
}

std::shared_ptr<StorageTexture> cachedStorageTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, std::uint32_t mip, std::uint64_t guestBytes = 0, bool written = false, std::optional<std::pair<std::uint32_t, std::uint32_t>> layers = std::nullopt, StorageTexture::RefreshPreparation* preparation = nullptr);

std::uint64_t heldBytes(const StorageTexture& image) {
    return image.BudgetBytes();
}

bool MetadataMoved(const StorageTexture& image, const GuestTextureResource& resource) {
    return resource.dccAddress != 0 && image.Descriptor().dccAddress != resource.dccAddress && !image.ServesKeysAt(resource.dccAddress);
}

// Whether a sampled texture over `resource` can be a view of the surface's cached storage image
// instead of a CPU snapshot (see cachedTexture): the format has a storage form and is not block
// compressed, and the surface lives in host-imported memory, where the image uploads and refreshes
// GPU-direct (a snapshot of such a surface reads and compares its bytes on the CPU, waiting for
// the recorded work that wrote them). APS5_NO_SAMPLED_FROM_STORAGE=1 keeps every sampled texture a
// snapshot as before.
bool SampledFromStorageEligible(const Context& context, std::uint32_t format, std::uint64_t address, std::uint64_t guestBytes) {
    static const bool disabled = std::getenv("APS5_NO_SAMPLED_FROM_STORAGE") != nullptr;
    if (disabled || IsBlockCompressed(format) || !StorageFormatAvailable(context, format)) return false;
    return HostImportCovers(context, address, static_cast<std::size_t>(guestBytes));
}

bool SampledFromStorageEligible(const Context& context, const GuestTextureResource& resource, std::uint64_t guestBytes) {
    return SampledFromStorageEligible(context, resource.format, resource.baseAddress, guestBytes);
}

// Whether a sampled texture over a fast-cleared, storage-eligible surface views the cleared storage
// image (see cachedTexture). APS5_NO_CLEARED_VIEW=1 keeps such surfaces snapshots, as before.
bool ClearedViewEnabled() {
    static const bool disabled = std::getenv("APS5_NO_CLEARED_VIEW") != nullptr;
    return !disabled;
}

std::optional<VkClearColorValue> SampledClearColor(const Context& context, const GuestTextureResource& resource, DccKeys keys, bool depthCompare) {
    if (depthCompare || !IsDccClear(keys) || keys == DccKeys::ClearRegister) return std::nullopt;
    if (IsBlockCompressed(resource.format) || IsRepackedTextureFormat(resource.format) || SrgbExpandsToRgba8(context, resource.format)) return std::nullopt;
    const auto format = SampledTextureFormat(context, resource.format);
    if (format != DccClearTextureFormat(resource.format)) return std::nullopt;
    std::array<std::uint32_t, 4> pattern{};
    if (!FillDccClear(format, keys, resource.dccAlphaOnMsb, std::as_writable_bytes(std::span(pattern)))) return std::nullopt;
    VkClearColorValue clear{};
    if (!ClearColorForTexel(format, BytesPerElement(resource.format), pattern, clear)) return std::nullopt;
    return clear;
}

bool ClearSnapshotCacheEnabled() {
    static const bool enabled = std::getenv("APS5_NO_CLEAR_SNAPSHOT_CACHE") == nullptr;
    return enabled;
}

std::uint64_t DepthCopyMinBytes() {
    static const std::uint64_t bytes = [] {
        const char* text = std::getenv("APS5_DEPTH_COPY_MIN_KIB");
        return (text != nullptr ? std::strtoull(text, nullptr, 10) : 1024ull) << 10u;
    }();
    return bytes;
}

// Surfaces whose storage image could not be made (no writable committed pages, say): remembered
// so the sampled lookup does not throw and fall back on every use. A failure is keyed by the surface
// (address and size: the heap reuses addresses) and holds only while the guest mappings are what
// they were when it failed (the allocation generation): pages committed later, or another surface
// at the address, get a fresh attempt.
struct StorageFailures {
    AgcDriver::Mutex mutex;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> generations;
};

StorageFailures& StorageFailed() {
    static StorageFailures failures;
    return failures;
}

// The cached storage image for a sampled descriptor, or null when it cannot be made (then the
// snapshot path serves the descriptor, as before). Every null return counts as a fallback.
std::shared_ptr<StorageTexture> sampledStorageSource(const Context& context, const GuestTextureResource& resource, std::uint64_t guestBytes) {
    auto& counters = TextureCounts();
    auto& failures = StorageFailed();
    const auto surface = std::make_pair(resource.baseAddress, guestBytes);
    const auto generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    {
        std::lock_guard lock(failures.mutex);
        if (const auto it = failures.generations.find(surface); it != failures.generations.end()) {
            if (it->second == generation) {
                counters.storageFallbacks.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            failures.generations.erase(it);
        }
    }
    try {
        return cachedStorageTexture(context, {}, resource, 0, guestBytes);
    } catch (const std::exception& error) {
        counters.storageFallbacks.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(failures.mutex);
        if (failures.generations.size() < 4096 && failures.generations.emplace(surface, generation).second) std::fprintf(stderr, "[textures] sampled texture 0x%llx (%ux%u format %u) keeps the snapshot path: %s\n", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, error.what());
        return nullptr;
    }
}

std::uint64_t storageCacheBytes();

std::optional<std::uint32_t> largestDeviceLocalHeap(const VkPhysicalDeviceMemoryProperties& memory) {
    std::optional<std::uint32_t> heap;
    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(memory.memoryHeapCount, VK_MAX_MEMORY_HEAPS); ++i) {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 && (!heap || memory.memoryHeaps[i].size > memory.memoryHeaps[*heap].size)) heap = i;
    }
    return heap;
}

std::atomic<std::uint64_t> budgetOverride{0};

std::uint64_t forcedCacheBudget() {
    if (const auto overridden = budgetOverride.load(std::memory_order_relaxed); overridden != 0) return overridden;
    static const std::uint64_t forced = [] {
        const char* text = std::getenv("APS5_TEXTURE_CACHE_MIB");
        return text != nullptr ? std::strtoull(text, nullptr, 10) << 20u : 0u;
    }();
    return forced;
}

std::uint64_t storageBudget(const Context& context, std::uint64_t required = 0, std::optional<std::chrono::steady_clock::time_point>* calculatedAt = nullptr) {
    if (calculatedAt != nullptr) calculatedAt->reset();
    const auto forced = forcedCacheBudget();
    if (forced != 0) return forced;
    if (context.memoryProperties2 == nullptr || context.physical == VK_NULL_HANDLE) return TextureCacheBudget(context.memory);
    static struct {
        AgcDriver::Mutex mutex;
        VkPhysicalDevice device{VK_NULL_HANDLE};
        std::chrono::steady_clock::time_point read{};
        std::uint64_t budget{0};
        std::uint64_t ceiling{0};
        std::chrono::steady_clock::time_point calculatedAt{};
    } cached;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(cached.mutex);
    std::optional<VkPhysicalDeviceMemoryBudgetPropertiesEXT> reported;
    std::uint64_t sampled = 0;
    std::uint64_t storage = 0;
    const auto read = [&] {
        reported.emplace(VkPhysicalDeviceMemoryBudgetPropertiesEXT{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT});
        VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        properties.pNext = &*reported;
        context.memoryProperties2(context.physical, &properties);
        cached.device = context.physical;
        cached.read = now;
        sampled = SampledTextureMemory();
        storage = storageCacheBytes();
        cached.budget = StorageTextureBudget(context.memory, &*reported, sampled, storage);
        cached.ceiling = StorageTextureCeiling(context.memory, &*reported);
        cached.calculatedAt = std::chrono::steady_clock::now();
    };
    if (cached.device != context.physical || now - cached.read >= std::chrono::seconds(1)) read();
    const auto needed = std::min(required, cached.ceiling);
    if (needed > cached.budget && context.bufferPool != nullptr && context.bufferPool->ReclaimIdleDevice(needed - cached.budget, now) != 0) read();
    if (calculatedAt != nullptr) *calculatedAt = cached.calculatedAt;
    if (reported) MemoryAccounting::Report(context, *reported, sampled, storage, cached.budget);
    return cached.budget;
}

std::size_t storageEntries() {
    static const std::size_t entries = [] {
        const char* text = std::getenv("APS5_STORAGE_CACHE_ENTRIES");
        if (text == nullptr || *text == '\0') return std::size_t{2048};
        const auto parsed = std::strtoull(text, nullptr, 10);
        return parsed != 0 ? static_cast<std::size_t>(parsed) : std::numeric_limits<std::size_t>::max();
    }();
    return entries;
}

std::uint64_t sampledBudget(const Context& context, TextureCache& cache) {
    if (const auto forced = forcedCacheBudget(); forced != 0) return forced;
    if (context.memoryProperties2 == nullptr || context.physical == VK_NULL_HANDLE) return TextureCacheBudget(context.memory);
    const auto now = std::chrono::steady_clock::now();
    if (cache.budgetDevice == context.physical && now - cache.budgetRead < std::chrono::seconds(1)) return cache.budget;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT reported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    properties.pNext = &reported;
    context.memoryProperties2(context.physical, &properties);
    const auto sampled = SampledTextureMemory();
    const auto storage = storageCacheBytes();
    const auto budget = SampledTextureBudget(context.memory, &reported, sampled + storage, storage);
    const bool fresh = cache.budgetDevice != context.physical;
    std::optional<std::chrono::steady_clock::time_point> storageCalculatedAt;
    const auto storageLimit = storageBudget(context, 0, &storageCalculatedAt);
    if (const auto heap = largestDeviceLocalHeap(context.memory); heap && (fresh || SampledBudgetReportDue(cache.reportedBudget, budget, now - cache.reportedAt) || SampledBudgetReportDue(cache.reportedStorage, storageLimit, now - cache.reportedAt))) {
        char was[48] = "";
        char wasStorage[48] = "";
        char storageLimitAge[32] = "unavailable";
        if (storageCalculatedAt) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - *storageCalculatedAt);
            std::snprintf(storageLimitAge, sizeof(storageLimitAge), "%llu", static_cast<unsigned long long>(age.count()));
        }
        if (!fresh) {
            std::snprintf(was, sizeof(was), " (was %llu MiB)", static_cast<unsigned long long>(cache.reportedBudget >> 20u));
            std::snprintf(wasStorage, sizeof(wasStorage), " (was %llu MiB)", static_cast<unsigned long long>(cache.reportedStorage >> 20u));
        }
        std::fprintf(stderr, "[gpu] sampled texture cache budget %llu MiB%s: device-local heap %u has a VK_EXT_memory_budget budget of %llu MiB and uses %llu MiB, %llu MiB of it sampled textures (%llu MiB in the cache) and %llu MiB cached storage images; active storage image cache budget %llu MiB%s; storage_limit_age_ms=%s\n", static_cast<unsigned long long>(budget >> 20u), was, *heap, static_cast<unsigned long long>(reported.heapBudget[*heap] >> 20u), static_cast<unsigned long long>(reported.heapUsage[*heap] >> 20u), static_cast<unsigned long long>(sampled >> 20u), static_cast<unsigned long long>(cache.bytes >> 20u), static_cast<unsigned long long>(storage >> 20u), static_cast<unsigned long long>(storageLimit >> 20u), wasStorage, storageLimitAge);
        cache.reportedBudget = budget;
        cache.reportedStorage = storageLimit;
        cache.reportedAt = now;
    }
    cache.budgetDevice = context.physical;
    cache.budgetRead = now;
    cache.budget = budget;
    return budget;
}

// A snapshot over memory outside the write watch is compared with guest memory again only when this
// key changed: a write-watch mark in the range, a driver store, a mapping change or a recorded GPU
// write (as StorageTexture::compareUntracked). Taken before the snapshot is read or compared, so a
// CPU store racing with that leaves a mark for the next key. Zero when the range has no hint.
std::array<std::uint64_t, 4> untrackedKey(std::uint64_t address, std::size_t bytes) {
    if (GuestMemory::CollectEpoch() == 0) return {};
    const auto hint = GuestMemory::UntrackedHint(address, bytes);
    if (hint == 0) return {};
    const auto* recorder = Recorder::Active();
    const std::array<std::uint64_t, 4> key{hint, GuestMemory::NewestStamp(address, bytes), GuestMemory::ForgetSerial(), recorder != nullptr ? recorder->NewestWriteNote(address, bytes) : 0};
    return (key[2] & 1u) == 0 ? key : std::array<std::uint64_t, 4>{};
}

std::span<const std::uint32_t> firstLayerWords(std::span<const std::uint32_t> words, std::optional<ShaderRecompiler::DescriptorImageShape> shape, std::array<std::uint32_t, 8>& narrowed, bool report) {
    if (shape != ShaderRecompiler::DescriptorImageShape::Image2D || words.size() != narrowed.size() || IsNullSampledTextureResource(words)) return words;
    try {
        if (!FirstLayerDescriptor(words, narrowed)) return words;
    } catch (const std::runtime_error&) {
        return words;
    }
    if (report) {
        static std::atomic<std::uint64_t> narrowings{0};
        const auto count = narrowings.fetch_add(1) + 1;
        if (count <= 5 || (count & (count - 1)) == 0) {
            const auto resource = DecodeTextureResource(words);
            std::fprintf(stderr, "[gpu] 2D read of the 2D array 0x%llx (%ux%u format %u, slices %u..%u) built from slice %u alone; %llu such reads\n", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, resource.baseArray, resource.depthOrLastArray, resource.baseArray, static_cast<unsigned long long>(count));
        }
    }
    return narrowed;
}

bool layerRowsBinding(std::optional<ShaderRecompiler::DescriptorImageShape> shape, const GuestTextureResource& resource, bool depthCompare) {
    return !depthCompare && shape == ShaderRecompiler::DescriptorImageShape::Image2D && resource.dimension == TextureDimension::k1DArray;
}

void reportGuestAllocation(const GuestTextureResource& resource, std::uint64_t guestBytes) {
    const auto range = GuestAllocations::GuestAllocationsLookup_nid_postfix(resource.baseAddress, false);
    const auto geometry = DescribeSurface(resource);
    if (range == nullptr) {
        std::fprintf(stderr, "[gpu] view 0x%llx of %llu MiB lies in no registered guest allocation\n", static_cast<unsigned long long>(resource.baseAddress), static_cast<unsigned long long>(guestBytes >> 20u));
        return;
    }
    const auto end = range->address + range->bytes;
    const auto fitting = geometry.thick || geometry.layerBytes == 0 ? 0ull : std::min<std::uint64_t>(geometry.layers, (end - resource.baseAddress) / geometry.layerBytes);
    std::fprintf(stderr, "[gpu] view 0x%llx of %llu MiB lies in the guest allocation 0x%llx..0x%llx (%llu MiB, range 0x%llx..0x%llx, %s%s%s): %llu of its %u slices fit in the range\n", static_cast<unsigned long long>(resource.baseAddress), static_cast<unsigned long long>(guestBytes >> 20u), static_cast<unsigned long long>(range->allocationAddress), static_cast<unsigned long long>(range->allocationAddress + range->allocationBytes), static_cast<unsigned long long>(range->allocationBytes >> 20u), static_cast<unsigned long long>(range->address), static_cast<unsigned long long>(end), range->readable ? "r" : "-", range->writable ? "w" : "-", range->gpu ? "g" : "-", static_cast<unsigned long long>(fitting), geometry.layers);
}

// Large sampled textures (texture pools, atlases: hundreds of MiB to snapshot, detile and upload) are
// made once and then served from the cache; one made again on every lookup costs a whole frame. Each
// such creation prints why the cache did not serve it (the first 16 per process, then every 64th), so a
// session stuck re-making one names the cause (2026-11-03: the zoo's 1024x1024x256 BC3 pool at
// 0x1009000000 re-made every frame in ~1 of 6 sessions, 60 -> 205 ms/frame).
// APS5_LARGE_TEXTURE_REPORT_KIB moves the threshold (tests; 0 KiB reports every snapshot and view).
std::uint64_t LargeTextureReportBytes() {
    static const std::uint64_t bytes = [] {
        const char* text = std::getenv("APS5_LARGE_TEXTURE_REPORT_KIB");
        return text != nullptr && *text != '\0' ? std::strtoull(text, nullptr, 10) << 10u : 64ull << 20u;
    }();
    return bytes;
}

std::atomic<std::uint64_t> largeTexturesMade{0};
AgcDriver::Mutex largeReasonMutex;
std::string largeLastReason;

std::string LargeTextureStaleReason(const CachedTexture& entry, const StorageTexture* pendingSource, std::uint64_t guestBytes, DccKeys keys, bool stagedFlushed, const std::vector<std::pair<std::uint64_t, std::uint64_t>>* committed, std::uint64_t address) {
    char text[720];
    if (entry.source != nullptr) {
        std::snprintf(text, sizeof(text), "stale view (results pending now %d, staged clear flushed %d, keys %d)", pendingSource != nullptr ? 1 : 0, stagedFlushed ? 1 : 0, static_cast<int>(keys));
    } else if (pendingSource != nullptr) {
        const auto& pending = pendingSource->Descriptor();
        std::snprintf(text, sizeof(text), "stale snapshot: results now pending in the storage image 0x%llx (%ux%u, format %u, %llu KiB)", static_cast<unsigned long long>(pending.baseAddress), pending.width, pending.height, pending.format, static_cast<unsigned long long>(pendingSource->GuestBytes() >> 10u));
    } else if (entry.surfaceBytes != guestBytes) {
        std::snprintf(text, sizeof(text), "stale snapshot: surface bytes %llu -> %llu", static_cast<unsigned long long>(entry.surfaceBytes), static_cast<unsigned long long>(guestBytes));
    } else if (entry.keys != keys) {
        std::snprintf(text, sizeof(text), "stale snapshot: DCC keys %d -> %d", static_cast<int>(entry.keys), static_cast<int>(keys));
    } else {
        const auto surface = static_cast<std::size_t>(entry.surfaceBytes);
        const bool sameRanges = committed != nullptr ? entry.committed.has_value() && *entry.committed == *committed : !entry.committed.has_value();
        const bool pending = Recorder::SnapshotWriteOverlaps(address, surface);
        const bool unchanged = committed != nullptr ? GuestMemory::UnchangedCommittedSince(*committed, entry.generation) : GuestMemory::UnchangedSince(address, surface, entry.generation);
        // Where it changed: the 4 KiB pages UnchangedSince would refuse (none listed: untracked range or generation 0).
        std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
        const bool tracked = GuestMemory::ChangedPages(address, surface, entry.generation, runs);
        std::uint64_t changedBytes = 0;
        for (const auto& [begin, end] : runs) changedBytes += end - begin;
        // The newest stamp in its parts: block stamps, and the unstamped-store slots (driver stores
        // outside write watching, hashed by block modulo 4 GiB; see FIX_UNSTAMPED_ALIAS) with the store
        // that last set the newest one.
        const auto split = GuestMemory::SplitNewestStamp(address, surface);
        std::snprintf(text, sizeof(text), "changed: committed ranges %lld -> %lld (same %d), recorded GPU write over it %d, unchanged since generation %llu: %d (newest stamp %llu: block %llu, unstamped slot %llu of block 0x%llx last set by a store to 0x%llx..0x%llx), snapshot kept for compare %d, untracked key %d; pages tracked %d, %zu changed runs, %llu KiB, first at +0x%llx (%llu KiB)",
                      entry.committed.has_value() ? static_cast<long long>(entry.committed->size()) : -1ll, committed != nullptr ? static_cast<long long>(committed->size()) : -1ll, sameRanges ? 1 : 0, pending ? 1 : 0,
                      static_cast<unsigned long long>(entry.generation), unchanged ? 1 : 0, static_cast<unsigned long long>(GuestMemory::NewestStamp(address, surface)), static_cast<unsigned long long>(split.block), static_cast<unsigned long long>(split.unstamped),
                      static_cast<unsigned long long>(split.slotAddress), static_cast<unsigned long long>(split.storeBegin), static_cast<unsigned long long>(split.storeEnd), entry.bytes.empty() ? 0 : 1, entry.untracked[0] != 0 ? 1 : 0,
                      tracked ? 1 : 0, runs.size(), static_cast<unsigned long long>(changedBytes >> 10u), runs.empty() ? 0ull : static_cast<unsigned long long>(runs.front().first - address), runs.empty() ? 0ull : static_cast<unsigned long long>((runs.front().second - runs.front().first) >> 10u));
    }
    return text;
}

void LargeTextureReport(const GuestTextureResource& resource, std::uint64_t guestBytes, std::uint64_t accounted, bool fromStorage, const std::string& why, bool admitted, std::uint64_t cachedBytes, std::uint64_t budget) {
    const auto made = largeTexturesMade.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        std::lock_guard lock(largeReasonMutex);
        largeLastReason = why;
    }
    if (made > 16 && made % 64 != 0) return;
    std::fprintf(stderr, "[texture-cache] large texture 0x%llx (%ux%u, %u layers, format %u, %llu MiB guest, %llu MiB image, %s) made: %s; %s (cache %llu MiB, budget %llu MiB); %llu large textures made so far, frame %llu\n",
                 static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.depthOrLastArray + 1u, resource.format, static_cast<unsigned long long>(guestBytes >> 20u), static_cast<unsigned long long>(accounted >> 20u),
                 fromStorage ? "view" : "snapshot", why.c_str(), admitted ? "cached" : "used uncached", static_cast<unsigned long long>(cachedBytes >> 20u), static_cast<unsigned long long>(budget >> 20u), static_cast<unsigned long long>(made),
                 static_cast<unsigned long long>(Recorder::Presents()));
}

struct PendingLookup {
    std::shared_ptr<StorageTexture> source;
    bool depthCopy = false;
    bool srgbExpand = false;
    bool blockCopy = false;
    bool repackCopy = false;
};

PendingLookup pendingLookupSource(const Context& context, const GuestTextureResource& resource, std::uint64_t guestBytes, bool depthCompare, bool layerRows = false) {
    PendingLookup lookup;
    lookup.depthCopy = depthCompare && guestBytes >= DepthCopyMinBytes() && Texture::DepthCopyFormat(ResolveTextureFormat(resource.format)) != VK_FORMAT_UNDEFINED && resource.dimension != TextureDimension::k3D;
    lookup.srgbExpand = !depthCompare && SrgbExpandsToRgba8(context, resource.format);
    lookup.source = StorageTexture::FindPending(resource.baseAddress, guestBytes);
    if (layerRows) lookup.source.reset();
    auto& source = lookup.source;
    lookup.blockCopy = source != nullptr && !depthCompare && !lookup.srgbExpand && Texture::CanBlockCopyFrom(*source, resource);
    lookup.repackCopy = source != nullptr && !depthCompare && Texture::CanRepackCopyFrom(*source, resource);
    if (source != nullptr && !lookup.blockCopy && !lookup.repackCopy && (depthCompare ? !lookup.depthCopy || !Texture::CanDepthCopyFrom(*source, resource) : lookup.srgbExpand ? !Texture::CanSrgbExpandFrom(context, *source, resource) : !Texture::CanCopyFrom(*source, resource))) source.reset();
    if (source != nullptr && MetadataMoved(*source, resource)) source.reset();
    return lookup;
}

struct DepthArrayGpuCounters {
    std::atomic<std::uint64_t> candidates{0}, taken{0}, layers{0}, bytes{0}, verified{0}, mismatches{0};
};

DepthArrayGpuCounters& depthArrayGpuCounters() {
    static auto* counters = new DepthArrayGpuCounters();
    return *counters;
}

bool depthArrayVerifyNow() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_VERIFY_DEPTH_ARRAY_GPU");
        return value != nullptr && std::string_view(value) == "1";
    }();
    static const std::uint64_t every = [] {
        const char* value = std::getenv("APS5_VERIFY_DEPTH_ARRAY_GPU_EVERY");
        if (value == nullptr) return std::uint64_t{64};
        char* end = nullptr;
        const auto parsed = std::strtoull(value, &end, 10);
        return end != value && *end == '\0' && parsed != 0u ? static_cast<std::uint64_t>(parsed) : std::uint64_t{64};
    }();
    static std::atomic<std::uint64_t> uses{0};
    return enabled && uses.fetch_add(1, std::memory_order_relaxed) % every == 0u;
}

std::shared_ptr<Texture> depthArrayGpu(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components, std::uint64_t guestBytes, bool depthCompare) {
    if (!ArchB::Switches::Enabled(ArchB::Feature::DepthArrayGpu) || !depthCompare || resource.dimension != TextureDimension::k2DArray) return nullptr;
    GuestMemory::AssertGpuLockHeld("depthArrayGpu");
    auto& counters = depthArrayGpuCounters();
    counters.candidates.fetch_add(1, std::memory_order_relaxed);
    ArchB::Recorder::Count("depth_array_gpu_candidates");
    const auto refuse = [](const char* reason) -> std::shared_ptr<Texture> { ArchB::Recorder::Count(reason); return nullptr; };
    if (sampledCacheHeldThreadState() != 0 || storageCacheHeldThreadState() != 0 || ImportsHeldByThisThread()) return refuse("depth_array_gpu_refused_locks");
    auto* recorder = Recorder::Active();
    if (recorder == nullptr || recorder->Device() != context.device || Recorder::InCompletion() || context.detiler == nullptr) return refuse("depth_array_gpu_refused_recorder");
    const auto address = resource.baseAddress;
    const auto bytes = static_cast<std::size_t>(guestBytes);
    if (bytes == 0u || address > std::numeric_limits<std::uint64_t>::max() - bytes) return refuse("depth_array_gpu_refused_layout");
    if (recorder->QueuedStoreOverlaps(address, bytes) || recorder->QueuedKeyStoreOverlaps(address, bytes)) return refuse("depth_array_gpu_refused_queued_store");
    if (recorder->PendingCpuWritesOverlap(address, bytes) || recorder->CompletionLabelIn(address, bytes)) return refuse("depth_array_gpu_refused_cpu_store");
    if (recorder->PendingLeaseSerial(address, bytes) != 0u) return refuse("depth_array_gpu_refused_lease");
    if (AnyShadowedOverlaps(address, bytes)) return refuse("depth_array_gpu_refused_shadow");
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> range{{{address, address + bytes}}};
    if (StorageTexture::AnyPendingOverlaps(range)) return refuse("depth_array_gpu_refused_storage");
    if (!recorder->PendingWritesOver(address, bytes).empty()) return refuse("depth_array_gpu_refused_foreign_writer");
    std::vector<DepthArrayLayer> sources;
    if (const auto* reason = DepthArraySources(context, resource, sources)) return refuse(reason);
    const auto geometry = DescribeSurface(resource);
    if (geometry.guestBytes != guestBytes) return refuse("depth_array_gpu_refused_layout");
    const auto generation = GuestMemory::CollectWrites(address, bytes);
    if (generation == 0u) return refuse("depth_array_gpu_refused_generation");
    const auto key = MakeTextureKey(context.device, words, components, true);
    auto& cache = Textures();
    std::shared_ptr<Texture> result;
    std::vector<std::pair<VkImage, std::uint32_t>> copies;
    {
        std::lock_guard lock(cache.mutex);
        const CacheHold held(sampledCacheHeldThreadState());
        const auto it = findTexture(cache, key);
        if (it == cache.entries.end()) return refuse("depth_array_gpu_refused_cache");
        if (it->source != nullptr || it->texture->Released() || it->surfaceBytes != guestBytes || it->keys != DccKeys::Uncompressed || !it->texture->AcceptsDepthArray(resource)) return refuse("depth_array_gpu_refused_snapshot");
        if (it->generation == 0u || it->untracked[0] != 0u || it->committed.has_value()) return refuse("depth_array_gpu_refused_proof");
        std::vector<std::pair<std::uint64_t, std::uint64_t>> changed;
        if (!GuestMemory::ChangedPages(address, bytes, it->generation, changed) || !changed.empty()) return refuse("depth_array_gpu_refused_cpu_changed");
        for (const auto& source : sources) {
            if (std::find(it->depthArrayLayers.begin(), it->depthArrayLayers.end(), source) == it->depthArrayLayers.end()) copies.emplace_back(source.image, source.layer);
        }
        result = it->texture;
        if (!copies.empty()) {
            parkSnapshot(cache, it->bytes);
            result->UpdateDepthArrayLayers(resource, copies);
            it->depthArrayLayers = sources;
        }
        it->generation = generation;
        touchTexture(cache, it);
        logTextureLookup(*it, resource, generation);
    }
    counters.taken.fetch_add(1, std::memory_order_relaxed);
    counters.layers.fetch_add(copies.size(), std::memory_order_relaxed);
    counters.bytes.fetch_add(geometry.layerBytes * copies.size(), std::memory_order_relaxed);
    ArchB::Recorder::Count("depth_array_gpu_taken");
    ArchB::Recorder::Count("depth_array_gpu_layers", static_cast<double>(copies.size()));
    ArchB::Recorder::Count("depth_array_gpu_bytes", static_cast<double>(geometry.layerBytes * copies.size()));
    if (!copies.empty() && depthArrayVerifyNow()) {
        static_cast<void>(FlushDepthPlanes(address, bytes));
        CompletePendingCpuWrites(context, address, bytes);
        std::vector<std::byte> snapshot(bytes);
        ReadTextureSurface(resource, DccKeys::Uncompressed, snapshot);
        const auto reference = std::make_shared<Texture>(context, *context.detiler, resource, components, snapshot, true, false);
        result->VerifySnapshot(*reference, resource, "depth_array_gpu_mismatches", &counters.mismatches);
        counters.verified.fetch_add(1, std::memory_order_relaxed);
        ArchB::Recorder::Count("depth_array_gpu_verified");
    }
    return result;
}

std::shared_ptr<Texture> gpuSampledArray(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components, std::uint64_t guestBytes, bool depthCompare, bool layerRows) {
    const auto refuse = [](const char* reason) -> std::shared_ptr<Texture> {
        ArchB::Recorder::Count(reason);
        return nullptr;
    };
    ArchB::Recorder::Count("fix_sampled_array_gpu_candidates");
    if (!ArchB::PartialSnapshotActive()) return refuse("fix_sampled_array_gpu_refused_partial_off");
    if (!depthCompare || layerRows || resource.dimension != TextureDimension::k2DArray || resource.depthOrLastArray == 0 || resource.baseArray != 0 || resource.baseLevel != 0) return refuse("fix_sampled_array_gpu_refused_shape");
    if (resource.dccAddress != 0) return refuse("fix_sampled_array_gpu_refused_dcc");
    if (Texture::DepthCopyFormat(ResolveTextureFormat(resource.format)) == VK_FORMAT_UNDEFINED) return refuse("fix_sampled_array_gpu_refused_format");
    if (sampledCacheHeldThreadState() != 0 || storageCacheHeldThreadState() != 0 || ImportsHeldByThisThread()) return refuse("fix_sampled_array_gpu_refused_locks");
    auto* recorder = Recorder::Active();
    const auto address = resource.baseAddress;
    const auto bytes = static_cast<std::size_t>(guestBytes);
    if (recorder == nullptr || recorder->Device() != context.device || Recorder::InCompletion() || context.detiler == nullptr) return refuse("fix_sampled_array_gpu_refused_recorder");
    if (recorder->QueuedStoreOverlaps(address, bytes) || recorder->QueuedKeyStoreOverlaps(address, bytes)) return refuse("fix_sampled_array_gpu_refused_queued_store");
    if (recorder->PendingCpuWritesOverlap(address, bytes) || recorder->CompletionLabelIn(address, bytes)) return refuse("fix_sampled_array_gpu_refused_cpu_store");
    if (recorder->PendingLeaseSerial(address, bytes) != 0) return refuse("fix_sampled_array_gpu_refused_lease");
    if (AnyShadowedOverlaps(address, bytes)) {
        if (!LazyShadowPublishEnabled()) return refuse("fix_sampled_array_gpu_refused_shadow");
        StorageTexture::PublishShadowsOnly(address, bytes, PublishScope::Whole, "sampled array import");
    }
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> range{{{address, address + bytes}}};
    if (StorageTexture::AnyPendingOverlaps(range)) return refuse("fix_sampled_array_gpu_refused_storage");
    const auto pending = recorder->PendingWritesOver(address, bytes);
    if (pending.empty()) return refuse("fix_sampled_array_gpu_refused_writer");
    const auto geometry = DescribeSurface(resource);
    if (geometry.thick || geometry.levelMajor || geometry.imageDepth != 1u || geometry.imageLayers != geometry.layers || geometry.layerBytes == 0 || geometry.GuestLayerOffset(geometry.layers) != guestBytes) return refuse("fix_sampled_array_gpu_refused_layout");
    auto generation = GuestMemory::CollectWrites(address, bytes);
    std::optional<std::vector<std::pair<std::uint64_t, std::uint64_t>>> committed;
    if (generation == 0) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        generation = GuestMemory::CollectCommittedWrites(address, bytes, ranges);
        if (generation != 0) committed = std::move(ranges);
    }
    if (generation == 0) return refuse("fix_sampled_array_gpu_refused_generation");
    const auto key = MakeTextureKey(context.device, words, components, depthCompare);
    auto& cache = Textures();
    std::lock_guard lock(cache.mutex);
    const CacheHold held(sampledCacheHeldThreadState());
    const auto it = findTexture(cache, key);
    if (it == cache.entries.end()) return refuse("fix_sampled_array_gpu_refused_cache");
    if (it->source != nullptr || it->texture->ViewsStorageImage() || it->texture->Released() || it->surfaceBytes != guestBytes || it->keys != DccKeys::Uncompressed) return refuse("fix_sampled_array_gpu_refused_snapshot");
    if (it->generation == 0 || it->untracked[0] != 0 || it->committed != committed) return refuse("fix_sampled_array_gpu_refused_proof");
    std::vector<std::pair<std::uint64_t, std::uint64_t>> changed;
    if (!GuestMemory::ChangedPages(address, bytes, it->generation, changed)) return refuse("fix_sampled_array_gpu_refused_pages");
    for (const auto& write : pending) changed.emplace_back(write.begin, write.end);
    std::vector<bool> selected(geometry.layers, false);
    for (const auto& [from, to] : changed) {
        const auto begin = std::max(from, address);
        const auto end = std::min(to, address + bytes);
        if (begin >= end) continue;
        const auto first = static_cast<std::uint32_t>((begin - address) / geometry.layerBytes);
        const auto last = static_cast<std::uint32_t>((end - 1u - address) / geometry.layerBytes);
        if (last >= geometry.layers) return refuse("fix_sampled_array_gpu_refused_layout");
        for (auto layer = first; layer <= last; ++layer) selected[layer] = true;
    }
    std::vector<std::uint32_t> layers;
    for (std::uint32_t layer = 0; layer < geometry.layers; ++layer) {
        if (!selected[layer]) continue;
        if (ExistingHostImportFor(context, address + geometry.GuestLayerOffset(layer), static_cast<std::size_t>(geometry.layerBytes)) == nullptr) return refuse("fix_sampled_array_gpu_refused_import");
        layers.push_back(layer);
    }
    if (layers.empty() || layers.size() * 2u > geometry.layers) return refuse("fix_sampled_array_gpu_refused_majority");
    parkSnapshot(cache, it->bytes);
    it->texture->UpdateSnapshotLayers(*context.detiler, resource, layers, {}, true);
    it->generation = generation;
    it->committed = std::move(committed);
    it->untracked = {};
    ArchB::Recorder::Count("fix_sampled_array_gpu_taken");
    ArchB::Recorder::Count("fix_sampled_array_gpu_layers", static_cast<double>(layers.size()));
    ArchB::Recorder::Count("fix_sampled_array_gpu_bytes", static_cast<double>(geometry.layerBytes * layers.size()));
    ArchB::Recorder::Count("fix_compute_snapshot_prewait_waits", 0);
    static const bool verify = [] {
        const char* value = std::getenv("APS5_ARCHB_FIX_SAMPLED_ARRAY_GPU_VERIFY");
        return value != nullptr && std::string_view(value) == "1";
    }();
    static std::atomic<std::uint64_t> samples{0};
    if (verify && (samples.fetch_add(1, std::memory_order_relaxed) & 63u) == 0u) {
        std::vector<std::byte> snapshot(bytes);
        ReadTextureSurface(resource, DccKeys::Uncompressed, snapshot);
        const auto reference = std::make_shared<Texture>(context, *context.detiler, resource, components, snapshot, depthCompare, layerRows);
        it->texture->VerifySnapshot(*reference, resource, "fix_sampled_array_gpu_verify_mismatches");
        ArchB::Recorder::Count("fix_sampled_array_gpu_verified");
    }
    touchTexture(cache, it);
    logTextureLookup(*it, resource, generation);
    return it->texture;
}

// `guestBytes` is the surface size when the caller described the surface already (0: described here).
std::shared_ptr<Texture> cachedTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& requested, VkComponentMapping components, std::uint64_t guestBytes = 0, bool depthCompare = false, VkImageViewType multisampleViewType = VK_IMAGE_VIEW_TYPE_2D, bool layerRows = false, bool snapshotWait = true) {
    const GuestMemory::Gen::ConsumerScope genConsumer(GuestMemory::Gen::Consumer::TextureLookup);
    Require(!layerRows || (requested.dimension == TextureDimension::k1DArray && !depthCompare), "only a sampled 1D array is laid out as rows of a 2D image");
    const auto resource = layerRows ? requested : ArrayLayerWindow(requested, context.limits.maxImageArrayLayers);
    if (resource.baseAddress != requested.baseAddress) guestBytes = 0;
    if (resource.dimension == TextureDimension::k2DMsaa) {
        Require(!depthCompare, "native multisampled depth comparison sampling is not implemented");
        if (auto depth = DepthSurfaceTexture(context, words, resource, components, multisampleViewType)) {
            logLookup({depth.get(), resource, 0, DccKeys::Uncompressed, 0, nullptr, true});
            return depth;
        }
        auto target = MultisampleTargetTexture(context, resource, components, multisampleViewType);
        logLookup({target.get(), resource, 0, DccKeys::Uncompressed, 0, nullptr, true});
        return target;
    }
    if (resource.fmaskCmaskAddress != 0u) {
        Require(!depthCompare, "FMASK depth comparison sampling is not implemented");
        Require(resource.baseArray == 0u && resource.depthOrLastArray == 0u, "native FMASK array sampling is not implemented");
        CompletePendingCpuWrites(context, resource.fmaskCmaskAddress, CmaskBytes(resource.width, resource.height));
        auto native = MultisampleTargetFmaskTexture_nid_no_patch(context, resource, components);
        Require(native != nullptr, "compressed FMASK sampling without a matching resident native target is not implemented");
        return native;
    }
    CaptureTrace::Log("sampled-lookup address=%llx width=%u height=%u dcc=%llx", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, static_cast<unsigned long long>(resource.dccAddress));
    RequireNoMultisampleTarget(resource.baseAddress, guestBytes != 0 ? static_cast<std::size_t>(guestBytes) : 1u, "a texture read");
    if (auto* recorder = Recorder::Active(); recorder != nullptr) recorder->BoundKeptBytes();
    Require(!layerRows || !DepthSurfaceAt(resource.baseAddress), "sampling a native depth surface as a 1D array past maxImageArrayLayers is not implemented");
    if (auto depth = layerRows ? nullptr : DepthSurfaceTexture(context, words, resource, components)) {
        logLookup({depth.get(), resource, 0, DccKeys::Uncompressed, 0, nullptr, true});
        return depth;
    }
    const auto depthBitsWidth = words.size() >= 4 ? ShaderRecompiler::DepthBitsTextureWidth(words[1], words[3]) : 0u;
    constexpr auto float32 = static_cast<std::uint32_t>(ShaderRecompiler::IrBufferFormat::Format32Float);
    if (depthBitsWidth == 32u && resource.format != float32) {
        auto bits = resource;
        bits.format = float32;
        return cachedTexture(context, words, bits, components, guestBytes, depthCompare, VK_IMAGE_VIEW_TYPE_2D, layerRows, snapshotWait);
    }
    constexpr auto unorm16 = static_cast<std::uint32_t>(ShaderRecompiler::IrBufferFormat::Format16UNorm);
    if (depthBitsWidth == 16u && resource.format != unorm16) {
        auto normalized = resource;
        normalized.format = unorm16;
        return cachedTexture(context, words, normalized, components, guestBytes, depthCompare, VK_IMAGE_VIEW_TYPE_2D, layerRows, snapshotWait);
    }
    const auto viewedWindow = [&] {
        const auto window = ViewedArrayWindow(resource, false);
        if (window.baseAddress != resource.baseAddress || resource.dccAddress == 0) return window;
        if (ViewedArrayWindow(resource, true).baseAddress == resource.baseAddress) return window;
        if (TextureClearKeys(resource, guestBytes != 0 ? guestBytes : SurfaceGuestBytes(resource)) != DccKeys::Uncompressed) return window;
        return ViewedArrayWindow(resource, true);
    };
    if (const auto window = viewedWindow(); window.baseAddress != resource.baseAddress) {
        static std::atomic<std::uint64_t> windows{0};
        const auto count = windows.fetch_add(1) + 1;
        if (count <= 5 || (count & (count - 1)) == 0) std::fprintf(stderr, "[gpu] sampled array view 0x%llx (%ux%u format %u, dimension %d, slices %u..%u) served from its viewed slices at 0x%llx; %llu such views\n", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, static_cast<int>(resource.dimension), resource.baseArray, resource.depthOrLastArray, static_cast<unsigned long long>(window.baseAddress), static_cast<unsigned long long>(count));
        return cachedTexture(context, words, window, components, 0, depthCompare, VK_IMAGE_VIEW_TYPE_2D, layerRows, snapshotWait);
    }
    if (guestBytes == 0) guestBytes = SurfaceGuestBytes(resource);
    if (guestBytes >= (1ull << 30u)) {
        static std::atomic<std::uint64_t> large{0};
        const auto count = large.fetch_add(1) + 1;
        if (count <= 5 || (count & (count - 1)) == 0) {
            std::fprintf(stderr, "[gpu] sampled view 0x%llx (%ux%u format %u, dimension %d, tile %d, slices %u..%u) covers %llu MiB; %llu such views\n", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, static_cast<int>(resource.dimension), static_cast<int>(resource.tileMode), resource.baseArray, resource.depthOrLastArray, static_cast<unsigned long long>(guestBytes >> 20u), static_cast<unsigned long long>(count));
            reportGuestAllocation(resource, guestBytes);
        }
    }
    static const bool disabled = std::getenv("APS5_NO_TEXTURE_CACHE") != nullptr;
    const bool profile = LookupOutcomes::Profiled();
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::optional<DccKeys> scanned;
    const auto scanKeys = [&] {
        if (scanned.has_value()) return *scanned;
        if (resource.dccAddress == 0) {
            scanned = DccKeys::Uncompressed;
            return *scanned;
        }
        const auto scanStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        auto& memo = sampledKeyProofSlot(resource.dccAddress, guestBytes, resource.format, resource.dccAlphaOnMsb);
        if (memo.dccAddress != resource.dccAddress || memo.guestBytes != guestBytes || memo.format != resource.format || memo.dccAlphaOnMsb != resource.dccAlphaOnMsb) {
            memo.dccAddress = resource.dccAddress;
            memo.guestBytes = guestBytes;
            memo.format = resource.format;
            memo.dccAlphaOnMsb = resource.dccAlphaOnMsb;
            memo.proof = {};
        }
        const auto scans = KeyProofCounts().scanned;
        const auto proofs = KeyProofCounts().proved;
        scanned = ProvedClearKeys(resource, guestBytes, memo.proof);
        if (KeyProofCounts().scanned != scans) ++sampledLookupKeyScansThreadState();
        if (KeyProofCounts().proved != proofs) ++sampledLookupKeyProofsThreadState();
        if (profile) LookupOutcomes::Add(LookupOutcomes::DccScan, scanStart);
        return *scanned;
    };
    auto& counters = TextureCounts();
    const auto address = resource.baseAddress;
    const auto bytes = static_cast<std::size_t>(guestBytes);
    if (profile) StorageTexture::ProfilePendingAccess(resource, guestBytes, StorageTexture::PendingAccess::Read);
    if (!layerRows) {
        if (auto texture = depthArrayGpu(context, words, resource, components, guestBytes, depthCompare)) return texture;
    }
    static_cast<void>(FlushDepthPlanes(address, bytes));
    if (snapshotWait && ArchB::Switches::Enabled(ArchB::Feature::FixSampledArrayGpu) && depthCompare && (Recorder::SnapshotWriteOverlaps(address, bytes) || (LazyShadowPublishEnabled() && AnyShadowedOverlaps(address, bytes)))) {
        if (auto texture = gpuSampledArray(context, words, resource, components, guestBytes, depthCompare, layerRows)) return texture;
    }
    if (snapshotWait && ArchB::PartialSnapshotActive() && Recorder::ComputeSyncEnabled() && ArchB::Switches::Enabled(ArchB::Feature::FixComputeSnapshotWait) && sampledCacheHeldThreadState() == 0 && storageCacheHeldThreadState() == 0 && !ImportsHeldByThisThread() && (depthCompare || !SampledFromStorageEligible(context, resource, guestBytes)) && Recorder::SnapshotWriteOverlaps(address, bytes)) {
        if (auto* recorder = Recorder::Active(); recorder != nullptr) {
            const auto target = recorder->PendingWriterSerial(address, bytes);
            if (target != 0) {
                ArchB::Recorder::Count("fix_compute_snapshot_prewait_candidates");
                ArchB::Recorder::Count(depthCompare ? "fix_compute_snapshot_prewait_depth" : "fix_compute_snapshot_prewait_no_import");
                if (recorder->FinishComputeSyncUnlocked(target)) {
                    ArchB::Recorder::Count("fix_compute_snapshot_prewait_waits");
                    return cachedTexture(context, words, requested, components, guestBytes, depthCompare, multisampleViewType, layerRows, false);
                }
            }
        }
    }
    // A storage image whose results for this surface (or for a mip chain containing it) are still on
    // the GPU supplies the texture by a view of it; anything else needs those results in guest
    // memory first.
    auto lookup = pendingLookupSource(context, resource, guestBytes, depthCompare, layerRows);
    const bool depthCopy = lookup.depthCopy;
    const bool srgbExpand = lookup.srgbExpand;
    auto source = std::move(lookup.source);
    const bool blockCopy = lookup.blockCopy;
    std::optional<DccKeys> keys;
    bool clearThroughKeys = false;
    if (source != nullptr && resource.dccAddress != 0 && IsDccClear(source->FilledKeys())) {
        keys = scanKeys();
        if (*keys == source->FilledKeys()) {
            std::array<std::byte, 16> probe{};
            if (!FillDccClear(DccClearTextureFormat(resource.format), *keys, resource.dccAlphaOnMsb, probe)) {
                char text[256];
                std::snprintf(text, sizeof(text), "AGC graphics: sampled texture 0x%llx (%ux%u format %u, dcc 0x%llx) reads %s DCC keys filled over its pending image, a clear value the format has no encoding for", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, static_cast<unsigned long long>(resource.dccAddress), DccKeysName(*keys));
                throw std::runtime_error(text);
            }
            static const bool traceKeys = std::getenv("APS5_TRACE_DCC_KEYS") != nullptr;
            if (traceKeys) std::fprintf(stderr, "[dcc-keys] sampled 0x%llx through keys 0x%llx reads the %s fill, not the pending image\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(resource.dccAddress), DccKeysName(*keys));
            source.reset();
            clearThroughKeys = true;
        }
    }
    if (source != nullptr && resource.dccAddress != 0 && (keys.has_value() ? *keys : scanKeys()) == DccKeys::ClearSingle) source.reset();

    // Otherwise a surface in host-imported memory is viewed through its cached storage image (made
    // here when there is none): its refresh after a CPU or GPU write is a GPU-direct detile from the
    // import, recorded behind the producer, so no bytes are read or compared on the CPU and nothing
    // waits for the producer. A fast-cleared surface (keys) is viewed only through an image whose own
    // descriptor carries the DCC address (below); it stays a snapshot otherwise, its texels not read.
    if ((!depthCompare || depthCopy) && !layerRows && source == nullptr && !clearThroughKeys && SampledFromStorageEligible(context, resource, guestBytes)) {

        keys = scanKeys();
        if (*keys == DccKeys::Uncompressed) {
            source = sampledStorageSource(context, resource, guestBytes);
            if (source != nullptr && depthCompare && !Texture::CanDepthCopyFrom(*source, resource)) source.reset();
            if (source != nullptr && srgbExpand && !Texture::CanSrgbExpandFrom(context, *source, resource)) source.reset();
        } else if (!depthCompare && ClearedViewEnabled() && StorageClearAvailable(context, resource.format, *keys)) {
            // A fast-cleared surface is viewed as well, through its image cleared on the GPU (the
            // lookup's Refresh, see StorageTexture::upload), when the image's own descriptor names
            // the same DCC metadata so the refresh sees the keys. A snapshot of the clear texels
            // (filled and uploaded on the CPU, then replaced by a view once results are pending: two
            // 4K uploads per clear at the movie stage) serves the surface otherwise, as before.
            auto candidate = sampledStorageSource(context, resource, guestBytes);
            if (candidate != nullptr && candidate->Descriptor().dccAddress == resource.dccAddress) source = std::move(candidate);
        }
        scanned.reset();
        // The lookup's Refresh may have flushed another image's results over the memory (which
        // marks the keys uncompressed): a surface that stays a snapshot re-reads them behind that
        // flush, as it does behind its own. APS5_NO_KEYS_RESCAN=1 keeps the keys scanned above.
        if (source != nullptr && IsRepackedTextureFormat(resource.format) && !Texture::CanRepackCopyFrom(*source, resource)) source.reset();
        static const bool rescan = std::getenv("APS5_NO_KEYS_RESCAN") == nullptr;
        if (rescan && source == nullptr) keys.reset();
    }
    // Pages of the surface written since they were last collected are stamped now, before any
    // UnchangedSince (here and on a cache hit) looks at them: the checks only compare stamped blocks,
    // so a write the game made since the last walk is invisible until a collect stamps its page. The
    // walk is memoized per packet, so a texture viewed from a storage image repeats it for free.
    auto generation = GuestMemory::CollectWrites(address, bytes);
    if (source != nullptr && !GuestMemory::UnchangedSince(address, bytes, source->GenerationFor(address, bytes))) {
        // The CPU wrote the memory while results were pending: Refresh merges them the usual way.
        refreshForView(*source, resource);
        scanned.reset();
    }
    // Results stored to guest memory just now are stamped newer than `generation`; a snapshot read
    // after them is current at the generation of a second (memoized) collect. The store marks the
    // surface's DCC keys uncompressed, so the keys are read after it.
    const auto flushStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const bool stagedClear = source != nullptr && resource.dccAddress != 0 && [&] {
        const auto observedKeys = keys.has_value() ? *keys : scanKeys();
        return IsDccClear(observedKeys) || observedKeys == DccKeys::ClearSingle;
    }();
    bool stagedFlushed = false;
    const bool flushAsked = (source == nullptr || stagedClear) && !clearThroughKeys;
    if (flushAsked && StorageTexture::FlushPending(address, bytes, nullptr, "sampled texture")) {
        if (profile) LookupOutcomes::Add(LookupOutcomes::PendingFlush, flushStart);
        generation = GuestMemory::CollectWrites(address, bytes);
        if (stagedClear) {
            stagedFlushed = true;
            keys = DccKeys::Uncompressed;
        } else {
            keys.reset();
        }
        if (stagedFlushed) source.reset();
    }
    if (flushAsked) scanned.reset();
    if (!keys.has_value()) keys = scanKeys();
    if (disabled) {
        if (source != nullptr && depthCompare) {
            auto texture = std::make_shared<Texture>(context, source, resource, components, Texture::DepthCopyTag{});
            texture->RefreshDepthCopy();
            return texture;
        }
        if (source != nullptr && blockCopy) {
            auto texture = std::make_shared<Texture>(context, source, resource, components, Texture::BlockCopyTag{});
            texture->RefreshDepthCopy();
            return texture;
        }
        if (source != nullptr && IsRepackedTextureFormat(resource.format)) {
            auto texture = std::make_shared<Texture>(context, source, resource, components, Texture::RepackCopyTag{});
            texture->RefreshDepthCopy();
            return texture;
        }
        if (source != nullptr && srgbExpand) {
            auto texture = std::make_shared<Texture>(context, source, resource, components, Texture::SrgbExpandTag{});
            texture->RefreshDepthCopy();
            return texture;
        }
        if (source != nullptr) return std::make_shared<Texture>(context, source, resource, components);
        if (const auto clear = SampledClearColor(context, resource, *keys, depthCompare)) return std::make_shared<Texture>(context, *context.detiler, resource, components, std::span<const std::byte>{}, false, layerRows, &*clear);
        std::vector<std::byte> snapshot(bytes);
        ReadTextureSurface(resource, *keys, snapshot);
        return std::make_shared<Texture>(context, *context.detiler, resource, components, snapshot, depthCompare, layerRows);
    }
    auto snapshotGeneration = generation;
    using CommittedRanges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
    std::optional<CommittedRanges> committed;
    // APS5_ARCHB_FIX_ALLOC_CHURN: the committed ranges stay in a per-thread scratch (`scratchCommitted`,
    // with `committed` left empty) and are copied only into an entry that keeps them, into its
    // existing vector when it has one. The helpers below answer as `committed` would have.
    const CommittedRanges* scratchCommitted = nullptr;
    if (source == nullptr && generation == 0 && AgcDriver::ArchB::AllocChurnActive()) {
        struct RangesSlotStorage {};
        auto& ranges = AgcDriver::ThreadScratch<CommittedRanges, RangesSlotStorage>();
        if (const auto partial = GuestMemory::CollectCommittedWritesInPlace(address, bytes, ranges); partial != 0) {
            snapshotGeneration = partial;
            scratchCommitted = &ranges;
            if (profile) counters.partialCollected.fetch_add(1, std::memory_order_relaxed);
        } else if (profile) counters.partialUnavailable.fetch_add(1, std::memory_order_relaxed);
    } else if (source == nullptr && generation == 0) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        if (const auto partial = GuestMemory::CollectCommittedWrites(address, bytes, ranges); partial != 0) {
            snapshotGeneration = partial;
            committed = std::move(ranges);
            if (profile) counters.partialCollected.fetch_add(1, std::memory_order_relaxed);
        } else if (profile) counters.partialUnavailable.fetch_add(1, std::memory_order_relaxed);
    }
    const bool hasCommitted = committed.has_value() || scratchCommitted != nullptr;
    const auto committedRanges = [&]() -> const CommittedRanges& { return scratchCommitted != nullptr ? *scratchCommitted : *committed; };
    const auto sameCommitted = [&](const std::optional<CommittedRanges>& stored) {
        return scratchCommitted != nullptr ? stored.has_value() && *stored == *scratchCommitted : stored == committed;
    };
    const auto storeCommitted = [&](std::optional<CommittedRanges>& stored) {
        if (scratchCommitted == nullptr) stored = std::move(committed);
        else if (stored.has_value()) stored->assign(scratchCommitted->begin(), scratchCommitted->end());
        else stored.emplace(scratchCommitted->begin(), scratchCommitted->end());
    };
    const auto untracked = source == nullptr && generation == 0 && !hasCommitted && *keys == DccKeys::Uncompressed ? untrackedKey(address, bytes) : std::array<std::uint64_t, 4>{};
    auto& cache = Textures();
    auto key = MakeTextureKey(context.device, words, components, depthCompare);
    key.clearSnapshot = ClearSnapshotCacheEnabled() && source == nullptr && IsDccClear(*keys);
    std::lock_guard lock(cache.mutex);
    const CacheHold held(sampledCacheHeldThreadState());
    const auto createTexture = [&](auto&&... args) {
        const CacheHold reclaimable(sampledCacheReclaimableThreadState());
        return std::make_shared<Texture>(context, std::forward<decltype(args)>(args)...);
    };
    dropUncachedViews(cache);
    std::vector<std::byte> recycled;
    // Why a large sampled texture is made (see LargeTextureReport): an ordinary lookup returns a cached
    // one, so a large texture made again and again is a cost worth naming.
    const bool large = bytes >= LargeTextureReportBytes();
    std::string largeWhy = "absent";
    if (large) {
        std::size_t sameAddress = 0;
        std::array<std::uint32_t, 8> differing{};
        for (const auto& other : cache.entries) {
            if (other.address != address) continue;
            ++sameAddress;
            for (std::size_t i = 0; i < differing.size(); ++i) differing[i] |= other.key.words[i] ^ key.words[i];
        }
        if (sameAddress != 0) {
            char text[160];
            std::snprintf(text, sizeof(text), "absent, %zu entries at the address under other keys (words differing %08x %08x %08x %08x %08x %08x %08x %08x)", sameAddress, differing[0], differing[1], differing[2], differing[3], differing[4], differing[5], differing[6], differing[7]);
            largeWhy = text;
        }
    }
    if (auto it = findTexture(cache, key); it != cache.entries.end()) {
        if (it->source != nullptr) {
            // The view follows the storage image, whatever the GPU wrote to it since; guest memory
            // written meanwhile is taken in by refreshing the image. A fast clear the image cannot
            // see (keys, and no pending results to prefer) ends the view: a snapshot holds the clear.
            if (!stagedFlushed && (source == nullptr || source == it->source) && (source != nullptr || *keys == DccKeys::Uncompressed) && StorageImageServesKeys(*it->source, resource.dccAddress)) {
                if (!GuestMemory::UnchangedSince(address, bytes, it->source->GenerationFor(address, bytes))) refreshForView(*it->source, resource);
                if (it->texture->IsDepthCopy()) it->texture->RefreshDepthCopy();
                it->keys = *keys;
                touchTexture(cache, it);
                logLookup({it->texture.get(), resource, guestBytes, *keys, 0, it->source.get()});
                reportTextureCounters();
                if (profile) LookupOutcomes::Add(*keys != DccKeys::Uncompressed ? LookupOutcomes::SampledHitClearedView : LookupOutcomes::SampledHitView, start);
                return it->texture;
            }
        } else if (source == nullptr && it->surfaceBytes == guestBytes && it->keys == *keys) {
            // Unwritten pages need no comparison; partially resident textures compare only committed
            // pages. The compare goes through the flush hook: it waits for recorded work over the
            // surface (counted, and named for the [hooksync] line).
            const auto equalsCommitted = [&] {
                if (it->bytes.empty()) return false;
                if (Recorder::SnapshotWriteOverlaps(address, bytes)) counters.pendingReads.fetch_add(1, std::memory_order_relaxed);
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::TextureCompare);
                return GuestMemory::EqualsCommitted(address, it->bytes);
            };
            const auto unchanged = [&] {
                if (!sameCommitted(it->committed)) return false;
                if (AgcDriver::ArchB::PartialSnapshotActive() && Recorder::SnapshotWriteOverlaps(address, static_cast<std::size_t>(it->surfaceBytes))) return false;
                return hasCommitted ? !Recorder::SnapshotWriteOverlaps(address, static_cast<std::size_t>(it->surfaceBytes)) && GuestMemory::UnchangedCommittedSince(committedRanges(), it->generation) : GuestMemory::UnchangedSince(address, static_cast<std::size_t>(it->surfaceBytes), it->generation);
            };
            const auto kept = [&] {
                if (untracked[0] == 0) return false;
                const bool same = it->untracked == untracked && !Recorder::SnapshotWriteOverlaps(address, bytes);
                (same ? counters.untrackedKept : counters.untrackedCompared).fetch_add(1, std::memory_order_relaxed);
                return same;
            };
            const auto acceptance = [&] {
                if (*keys != DccKeys::Uncompressed && *keys != DccKeys::ClearSingle) return 0;
                if (unchanged()) return hasCommitted ? 2 : 1;
                if (kept()) return 3;
                if ((!AgcDriver::ArchB::PartialSnapshotActive() || DescribeSurface(resource).layers < 2u) && equalsCommitted()) return 4;
                return -1;
            }();
            if (acceptance >= 0) {
                if (profile) counters.snapshotAccepted[static_cast<std::size_t>(acceptance)].fetch_add(1, std::memory_order_relaxed);
                it->generation = snapshotGeneration;
                storeCommitted(it->committed);
                it->untracked = untracked;
                touchTexture(cache, it);
                logTextureLookup(*it, resource, generation);
                reportTextureCounters();
                if (profile) LookupOutcomes::Add(LookupOutcomes::SampledHitSnapshot, start);
                return it->texture;
            }
        }
        if (AgcDriver::ArchB::PartialSnapshotActive()) {
            const char* refusal = nullptr;
            SurfaceGeometry geometry;
            if (it->source != nullptr || it->texture->ViewsStorageImage()) refusal = "fix_psnap_refused_view";
            else if (source != nullptr) refusal = "fix_psnap_refused_storage_source";
            else if (it->surfaceBytes != guestBytes) refusal = "fix_psnap_refused_size";
            else if (it->keys != DccKeys::Uncompressed || *keys != DccKeys::Uncompressed) refusal = "fix_psnap_refused_dcc";
            else if (!sameCommitted(it->committed)) refusal = "fix_psnap_refused_committed";
            else if (it->generation == 0 || it->untracked[0] != 0) refusal = "fix_psnap_refused_generation";
            else if (it->texture->Released()) refusal = "fix_psnap_refused_released";
            else if (Recorder::Active() == nullptr) refusal = "fix_psnap_refused_recorder";
            else if (srgbExpand) refusal = "fix_psnap_refused_srgb";
            else if (IsRepackedTextureFormat(resource.format)) refusal = "fix_psnap_refused_repacked";
            else if (layerRows) refusal = "fix_psnap_refused_layout";
            else {
                geometry = DescribeSurface(resource);
                if (resource.dimension == TextureDimension::k3D || geometry.thick || geometry.imageDepth != 1u) refusal = "fix_psnap_refused_3d";
                else if (geometry.layers < 2u || geometry.imageLayers != geometry.layers || geometry.layerBytes == 0u || geometry.GuestLayerOffset(geometry.layers) != geometry.guestBytes) refusal = "fix_psnap_refused_layout";
                else {
                    for (std::uint32_t layer = 0; layer <= geometry.layers && refusal == nullptr; ++layer) {
                        if (geometry.GuestLayerOffset(layer) != static_cast<std::uint64_t>(layer) * geometry.layerBytes) refusal = "fix_psnap_refused_layout";
                    }
                }
            }
            std::uint64_t refreshedGeneration = 0;
            std::optional<CommittedRanges> refreshedCommitted;
            std::vector<std::uint32_t> changedLayers;
            if (refusal == nullptr) {
                GuestMemory::FlushGpuWrites(address, bytes);
                if (StorageTexture::FindPending(address, guestBytes) != nullptr) refusal = "fix_psnap_refused_storage_source";
            }
            if (refusal == nullptr && TextureClearKeys(resource, guestBytes) != DccKeys::Uncompressed) refusal = "fix_psnap_refused_dcc";
            if (refusal == nullptr) {
                refreshedGeneration = GuestMemory::CollectWrites(address, bytes);
                if (refreshedGeneration == 0) {
                    CommittedRanges ranges;
                    refreshedGeneration = GuestMemory::CollectCommittedWrites(address, bytes, ranges);
                    if (refreshedGeneration != 0) refreshedCommitted = std::move(ranges);
                }
                if (refreshedGeneration == 0) refusal = "fix_psnap_refused_generation";
                else if (it->committed != refreshedCommitted) refusal = "fix_psnap_refused_committed";
            }
            if (refusal == nullptr) {
                std::vector<std::pair<std::uint64_t, std::uint64_t>> changed;
                if (!GuestMemory::ChangedPages(address, bytes, it->generation, changed)) refusal = "fix_psnap_refused_pages";
                else {
                    for (const auto& [runBegin, runEnd] : changed) {
                        const auto begin = std::max(runBegin, address);
                        const auto end = std::min(runEnd, address + bytes);
                        if (begin >= end) continue;
                        const auto first = static_cast<std::uint32_t>((begin - address) / geometry.layerBytes);
                        const auto last = static_cast<std::uint32_t>((end - 1u - address) / geometry.layerBytes);
                        if (last >= geometry.layers) {
                            refusal = "fix_psnap_refused_layout";
                            break;
                        }
                        for (auto layer = first; layer <= last; ++layer) {
                            if (changedLayers.empty() || changedLayers.back() != layer) changedLayers.push_back(layer);
                        }
                    }
                    if (refusal == nullptr && changedLayers.empty()) refusal = "fix_psnap_refused_empty";
                    else if (refusal == nullptr && changedLayers.size() * 2u > geometry.layers) refusal = "fix_psnap_refused_majority";
                }
            }
            if (refusal == nullptr) {
                it->texture->UpdateSnapshotLayers(*context.detiler, resource, changedLayers, it->bytes);
                it->generation = refreshedGeneration;
                it->committed = std::move(refreshedCommitted);
                it->untracked = {};
                it->keys = DccKeys::Uncompressed;
                AgcDriver::ArchB::Recorder::Count("fix_psnap_updates");
                AgcDriver::ArchB::Recorder::Count("fix_psnap_layers", static_cast<double>(changedLayers.size()));
                AgcDriver::ArchB::Recorder::Count("fix_psnap_bytes", static_cast<double>(geometry.layerBytes * changedLayers.size()));
                static const bool verify = [] {
                    const char* value = std::getenv("APS5_ARCHB_FIX_PARTIAL_SNAPSHOT_VERIFY");
                    return value != nullptr && std::string_view(value) == "1";
                }();
                static const bool verifyPrewait = [] {
                    const char* value = std::getenv("APS5_ARCHB_FIX_COMPUTE_SNAPSHOT_WAIT_VERIFY");
                    return value != nullptr && std::string_view(value) == "1";
                }();
                static std::atomic<std::uint64_t> verifySamples{0};
                if ((verify || (verifyPrewait && ArchB::Switches::Enabled(ArchB::Feature::FixComputeSnapshotWait))) && (verifySamples.fetch_add(1, std::memory_order_relaxed) & 63u) == 0u) {
                    std::vector<std::byte> full(bytes);
                    ReadTextureSurface(resource, DccKeys::Uncompressed, full);
                    const auto reference = createTexture(*context.detiler, resource, components, full, depthCompare, layerRows);
                    it->texture->VerifySnapshot(*reference, resource);
                    AgcDriver::ArchB::Recorder::Count("fix_psnap_verify_updates");
                }
                touchTexture(cache, it);
                logTextureLookup(*it, resource, refreshedGeneration);
                reportTextureCounters();
                if (profile) LookupOutcomes::Add(LookupOutcomes::SampledHitSnapshot, start);
                return it->texture;
            }
            AgcDriver::ArchB::Recorder::Count(refusal);
        }
        if (large) largeWhy = LargeTextureStaleReason(*it, source.get(), guestBytes, *keys, stagedFlushed, hasCommitted ? &committedRanges() : nullptr, address);
        if (it->bytes.size() == bytes) recycled = std::move(it->bytes);
        else parkSnapshot(cache, it->bytes);
        eraseTexture(cache, it);
        ++cache.counts.stale;
    }
    const auto clear = source == nullptr ? SampledClearColor(context, resource, *keys, depthCompare) : std::nullopt;
    const bool noSnapshot = source != nullptr || clear.has_value();
    if (noSnapshot) parkSnapshot(cache, recycled);
    else if (recycled.empty()) recycled = spareSnapshot(cache, bytes);
    CachedTexture entry{key, address, noSnapshot ? std::vector<std::byte>{} : (recycled.empty() ? std::vector<std::byte>(bytes) : std::move(recycled)), nullptr, *keys, snapshotGeneration};
    storeCommitted(entry.committed);
    entry.surfaceBytes = bytes;
    if (source != nullptr) {
        entry.source = source;
        entry.sourceVersion = source->Version();
        if (depthCompare) {
            entry.texture = createTexture(source, resource, components, Texture::DepthCopyTag{});
            entry.texture->RefreshDepthCopy();
        } else if (blockCopy) {
            entry.texture = createTexture(source, resource, components, Texture::BlockCopyTag{});
            entry.texture->RefreshDepthCopy();
        } else if (IsRepackedTextureFormat(resource.format)) {
            entry.texture = createTexture(source, resource, components, Texture::RepackCopyTag{});
            entry.texture->RefreshDepthCopy();
        } else if (srgbExpand) {
            entry.texture = createTexture(source, resource, components, Texture::SrgbExpandTag{});
            entry.texture->RefreshDepthCopy();
        } else {
            entry.texture = createTexture(source, resource, components);
        }
        entry.accounted = source->Cached() ? 0u : heldBytes(*source);
        counters.fromStorage.fetch_add(1, std::memory_order_relaxed);
    } else if (clear) {
        entry.texture = createTexture(*context.detiler, resource, components, std::span<const std::byte>{}, false, layerRows, &*clear);
        entry.accounted = entry.texture->AllocationBytes();
        counters.cleared.fetch_add(1, std::memory_order_relaxed);
    } else {
        if (AgcDriver::ArchB::PartialSnapshotActive()) AgcDriver::ArchB::Recorder::Count("fix_psnap_full_makes");
        // Snapshot before the upload so a write racing with it is caught by the next comparison.
        const bool pending = *keys == DccKeys::Uncompressed && Recorder::SnapshotWriteOverlaps(address, bytes);
        if (pending) counters.pendingReads.fetch_add(1, std::memory_order_relaxed);
        ReadTextureSurface(resource, *keys, entry.bytes);
        if (!pending) entry.untracked = untracked;
        static const bool traceTextures = std::getenv("APS5_TRACE_TEXTURES") != nullptr;
        if (traceTextures) {
            std::size_t nonzero = 0;
            for (std::size_t i = 0; i < entry.bytes.size(); i += 64) nonzero += entry.bytes[i] != std::byte{0};
            std::fprintf(stderr, "[texture] 0x%llx %ux%u format %u tile %d: %zu of %zu sampled bytes nonzero\n", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, static_cast<int>(resource.tileMode), nonzero, entry.bytes.size() / 64);
        }
        entry.texture = createTexture(*context.detiler, resource, components, entry.bytes, depthCompare, layerRows);
        entry.accounted = entry.texture->AllocationBytes();
        constexpr std::size_t SnapshotCompareBytes = 256u << 10u;
        if (entry.bytes.size() > SnapshotCompareBytes && (entry.generation != 0 || entry.untracked[0] != 0)) parkSnapshot(cache, entry.bytes);
        counters.snapshots.fetch_add(1, std::memory_order_relaxed);
    }
    const auto budget = sampledBudget(context, cache);
    auto texture = entry.texture;
    const auto cachedBefore = cache.bytes;
    const bool admitted = admitTexture(cache, key, entry.accounted, budget);
    if (large) LargeTextureReport(resource, guestBytes, entry.accounted, source != nullptr, largeWhy, admitted, cachedBefore, budget);
    if (!admitted) {
        noteReleased(texture);
        logTextureLookup(entry, resource, generation);
        reportCacheCounts(cache, budget);
        reportTextureCounters();
        if (profile) LookupOutcomes::Add(source != nullptr ? LookupOutcomes::SampledMadeView : LookupOutcomes::SampledMadeSnapshot, start);
        return texture;
    }
    cache.bytes += entry.accounted;
    entry.present = Recorder::Presents();
    ++cache.counts.inserted;
    logTextureLookup(entry, resource, generation);
    cache.entries.push_front(std::move(entry));
    cache.index[key] = cache.entries.begin();
    if (source != nullptr) cache.views.emplace(source.get(), cache.entries.begin());
    reportCacheCounts(cache, budget);
    reportTextureCounters();
    if (profile) LookupOutcomes::Add(source != nullptr ? LookupOutcomes::SampledMadeView : LookupOutcomes::SampledMadeSnapshot, start);
    return texture;
}

// Storage images stay on the GPU between dispatches: while guest memory still holds what an image was
// last uploaded from or written back as, its next use skips the upload and detile.
struct StorageKey {
    VkDevice device;
    std::array<std::uint32_t, 8> words;
    bool operator==(const StorageKey&) const = default;
};

struct StorageKeyHash {
    std::size_t operator()(const StorageKey& key) const noexcept {
        return static_cast<std::size_t>(hashWords(14695981039346656037ull ^ reinterpret_cast<std::uintptr_t>(key.device), key.words.data(), key.words.size()));
    }
};

struct CachedStorageTexture {
    StorageKey key;
    std::uint32_t mip;
    std::shared_ptr<StorageTexture> texture;
    std::uint64_t accounted = 0;
};

// As TextureCache: use order with a hash index by key, plus one by image for StorageImageCached.
struct StorageTextureCache {
    AgcDriver::Mutex mutex;
    std::list<CachedStorageTexture> entries;
    std::unordered_map<StorageKey, std::list<CachedStorageTexture>::iterator, StorageKeyHash> index;
    std::unordered_map<const StorageTexture*, std::list<CachedStorageTexture>::iterator> byImage;
    std::atomic<std::uint64_t> bytes{0};
};

StorageTextureCache& StorageTextures() {
    static StorageTextureCache cache;
    return cache;
}

std::uint64_t storageCacheBytes() {
    return StorageTextures().bytes.load(std::memory_order_relaxed);
}

std::list<CachedStorageTexture>::iterator findStorage(StorageTextureCache& cache, const StorageKey& key) {
    if (TextureHashEnabled()) {
        const auto found = cache.index.find(key);
        return found == cache.index.end() ? cache.entries.end() : found->second;
    }
    for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
        if (it->key == key) return it;
    }
    return cache.entries.end();
}

std::list<CachedStorageTexture>::iterator findStorageByImage(StorageTextureCache& cache, VkDevice device, const StorageTexture* image) {
    if (TextureHashEnabled()) {
        const auto found = cache.byImage.find(image);
        return found == cache.byImage.end() || found->second->key.device != device ? cache.entries.end() : found->second;
    }
    for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
        if (it->key.device == device && it->texture.get() == image) return it;
    }
    return cache.entries.end();
}

// Evicts an entry: its pending results go to guest memory first (the image may die with the entry).
std::shared_ptr<StorageTexture> evictStorage(StorageTextureCache& cache, std::list<CachedStorageTexture>::iterator it, bool store = true, const char* reason = "cache eviction") {
    auto texture = it->texture;
    it->texture->SetCached(false);
    noteUncached(it->texture);
    if (store) it->texture->Flush(reason);
    else it->texture->Detach();
    cache.bytes -= it->accounted;
    cache.index.erase(it->key);
    cache.byImage.erase(it->texture.get());
    cache.entries.erase(it);
    return texture;
}

// Storage images are shared by every descriptor of one surface (address, extent, layers, format, tile
// mode): the image holds the whole mip chain, and render targets in the same memory attach to it.
std::array<std::uint32_t, 8> SurfaceKey(const Context& context, const GuestTextureResource& resource) {
    // Guest formats that store in the same Vulkan format share the image (views carry the difference).
    return {static_cast<std::uint32_t>(resource.baseAddress | resource.pipeBankXor), static_cast<std::uint32_t>(resource.baseAddress >> 32u), resource.width, resource.height, (resource.depthOrLastArray << 16u) | (resource.mipCount & 0xffffu), (static_cast<std::uint32_t>(resource.tileMode) << 12u) | (static_cast<std::uint32_t>(resource.dimension) << 20u), resource.baseArray, static_cast<std::uint32_t>(StorageFormatForGuest(context, resource.format))};
}

struct ExtendedSurfaces {
    AgcDriver::Mutex mutex;
    std::map<std::array<std::uint32_t, 8>, std::uint32_t> levels;
    std::atomic<bool> any{false};
    std::atomic<std::uint64_t> serial{1};
};

ExtendedSurfaces& ExtendedChains() {
    static ExtendedSurfaces surfaces;
    return surfaces;
}

std::uint32_t AllocatedLevels(const GuestTextureResource& resource) {
    return resource.allocatedMipCount != 0 ? resource.allocatedMipCount : resource.mipCount;
}

GuestTextureResource StorageSurface(const Context& context, const GuestTextureResource& viewed) {
    auto& surfaces = ExtendedChains();
    const bool extended = viewed.mipCount > AllocatedLevels(viewed);
    if (!extended && !surfaces.any.load(std::memory_order_acquire)) return viewed;
    auto surface = viewed;
    surface.mipCount = AllocatedLevels(viewed);
    const auto identity = SurfaceKey(context, surface);
    std::lock_guard lock(surfaces.mutex);
    if (extended) {
        auto& levels = surfaces.levels[identity];
        if (levels < viewed.mipCount) {
            levels = viewed.mipCount;
            surfaces.serial.fetch_add(1, std::memory_order_release);
        }
        surfaces.any.store(true, std::memory_order_release);
        surface.mipCount = levels;
    } else if (const auto it = surfaces.levels.find(identity); it != surfaces.levels.end()) {
        surface.mipCount = it->second;
    } else {
        surface.mipCount = viewed.mipCount;
    }
    return surface;
}

struct WrittenStorage {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> bytes;
    std::vector<WrittenStorageExtent> extents;
};

static WrittenStorage WrittenStorageBytes(const GuestTextureResource& viewed, std::uint32_t mip) {
    WrittenStorage written;
    const auto geometry = DescribeSurface(viewed);
    if (geometry.thick || mip >= geometry.mips.size()) return written;
    const auto& level = geometry.mips[mip];
    const auto bytesPerElement = BytesPerElement(viewed.format);
    const auto covered = CoveredMipBytes(viewed.tileMode, bytesPerElement, level);
    const bool blocks = viewed.tileMode != TextureTileMode::kLinear && !level.tail;
    const auto first = geometry.layers > 1u && geometry.imageDepth <= 1u ? std::min(viewed.baseArray, geometry.layers) : 0u;
    for (auto layer = first; layer < geometry.layers; ++layer) {
        if (!geometry.HasLayer(mip, layer)) continue;
        const auto begin = viewed.baseAddress + geometry.GuestOffset(layer, level);
        for (const auto& [rangeBegin, rangeEnd] : covered) {
            if (!written.bytes.empty() && written.bytes.back().second == begin + rangeBegin) written.bytes.back().second = begin + rangeEnd;
            else written.bytes.emplace_back(begin + rangeBegin, begin + rangeEnd);
        }
        if (blocks) written.extents.push_back({begin, level.width, level.height, bytesPerElement, ThinBlockLayout(viewed.tileMode, bytesPerElement), level.blocksPerRow});
    }
    return written;
}

// `guestBytes` is the surface size when the caller described the surface already (0: described here).
std::shared_ptr<StorageTexture> cachedStorageTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& requested, std::uint32_t mip, std::uint64_t guestBytes, bool written, std::optional<std::pair<std::uint32_t, std::uint32_t>> layers, StorageTexture::RefreshPreparation* preparation) {
    const auto viewed = ArrayLayerWindow(requested, context.limits.maxImageArrayLayers);
    if (viewed.baseAddress != requested.baseAddress) guestBytes = 0;
    Require(viewed.dimension != TextureDimension::k2DMsaa, "storage image access to a multisampled texture is not implemented");
    const auto viewedBytes = guestBytes != 0 ? guestBytes : SurfaceGuestBytes(viewed);
    if (LookupOutcomes::Profiled()) StorageTexture::ProfilePendingAccess(viewed, viewedBytes, StorageTexture::PendingAccess::Read);
    RequireNoMultisampleTarget(viewed.baseAddress, static_cast<std::size_t>(viewedBytes), "a storage image access");
    if (auto depth = DepthSurfaceStorage(context, words, viewed, mip)) return depth;
    if (viewedBytes >= (1ull << 30u)) {
        static std::atomic<std::uint64_t> large{0};
        const auto count = large.fetch_add(1) + 1;
        if (count <= 5 || (count & (count - 1)) == 0) {
            std::fprintf(stderr, "[gpu] storage view 0x%llx (%ux%u format %u, dimension %d, tile %d, slices %u..%u, mip %u) covers %llu MiB; %llu such views\n", static_cast<unsigned long long>(viewed.baseAddress), viewed.width, viewed.height, viewed.format, static_cast<int>(viewed.dimension), static_cast<int>(viewed.tileMode), viewed.baseArray, viewed.depthOrLastArray, mip, static_cast<unsigned long long>(viewedBytes >> 20u), static_cast<unsigned long long>(count));
            reportGuestAllocation(viewed, viewedBytes);
        }
    }
    static const bool disabled = std::getenv("APS5_NO_TEXTURE_CACHE") != nullptr;
    static const bool traceOverlap = std::getenv("APS5_TRACE_DEPTH_STORAGE_OVERLAP") != nullptr;
    const bool overlapTraced = traceOverlap && DepthSurfaceOverlaps(context.device, viewed.baseAddress, viewedBytes);
    const bool stalePlanes = DepthPlanesStale();
    const auto flushStart = overlapTraced ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::size_t flushedRanges = 0;
    if ((stalePlanes || written) && (written || !layers || disabled)) flushedRanges = FlushDepthPlanes(viewed.baseAddress, static_cast<std::size_t>(viewedBytes), written ? DepthPlaneScope::Whole : DepthPlaneScope::Overlapped).size();
    if (overlapTraced) {
        static std::mutex reportedMutex;
        static std::map<std::uint64_t, std::uint64_t> reported;
        const auto key = viewed.baseAddress ^ (static_cast<std::uint64_t>(viewed.format) << 52u) ^ (static_cast<std::uint64_t>(written) << 63u);
        std::uint64_t seen = 0;
        {
            std::lock_guard lock(reportedMutex);
            if (reported.size() < 256 || reported.contains(key)) seen = ++reported[key];
        }
        if (seen == 1 || seen == 100) {
            std::fprintf(stderr, "[depth-storage-overlap] #%llu storage 0x%llx+0x%llx %ux%u format %u dimension %d tile %d slices %u..%u mip %u base level %u written %d stale planes %d layers %d: flush %.2f ms, %zu ranges stored; overlapping:%s\n", static_cast<unsigned long long>(seen),
                         static_cast<unsigned long long>(viewed.baseAddress), static_cast<unsigned long long>(viewedBytes), viewed.width, viewed.height, viewed.format, static_cast<int>(viewed.dimension), static_cast<int>(viewed.tileMode), viewed.baseArray, viewed.depthOrLastArray, mip, viewed.baseLevel,
                         written ? 1 : 0, stalePlanes ? 1 : 0, layers.has_value() ? 1 : 0, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - flushStart).count(), flushedRanges,
                         DescribeDepthOverlaps(context.device, viewed.baseAddress, viewedBytes).c_str());
        }
    }
    if (written && DepthSurfaceOverlaps(context.device, viewed.baseAddress, viewedBytes)) {
        const auto storage = WrittenStorageBytes(viewed, mip);
        RetireCoveredDepthSurfaces(context.device, storage.bytes, storage.extents);
    }
    if (disabled) return StorageTexture::Create(context, *context.detiler, viewed, mip);
    static_cast<void>(words);
    if (auto* recorder = Recorder::Active(); recorder != nullptr) recorder->BoundKeptBytes();
    const bool profile = LookupOutcomes::Profiled();
    auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto& counters = TextureCounts();
    const auto resource = StorageSurface(context, viewed);
    const StorageKey key{context.device, SurfaceKey(context, resource)};
    auto& cache = StorageTextures();
    std::vector<std::shared_ptr<StorageTexture>> evicted;
    std::shared_ptr<StorageTexture> texture;
    {
        std::lock_guard lock(cache.mutex);
        const CacheHold held(storageCacheHeldThreadState());
        if (resource.mipCount > AllocatedLevels(resource)) {
            auto allocated = resource;
            allocated.mipCount = AllocatedLevels(resource);
            if (const auto found = findStorage(cache, {context.device, SurfaceKey(context, allocated)}); found != cache.entries.end()) evicted.push_back(evictStorage(cache, found));
        }
        auto it = findStorage(cache, key);
        std::shared_ptr<StorageTexture> detached;
        if (it != cache.entries.end() && MetadataMoved(*it->texture, resource)) {
            detached = evictStorage(cache, it, false);
            evicted.push_back(detached);
            it = cache.entries.end();
        }
        if (it != cache.entries.end()) {
            if (layers) it->texture->RefreshLayers(layers->first, layers->second);
            else it->texture->Refresh(preparation);
            cache.entries.splice(cache.entries.begin(), cache.entries, it);
            texture = it->texture;
            if (TextureCountersReported()) counters.storageHits.fetch_add(1, std::memory_order_relaxed);
            if (profile) LookupOutcomes::Add(LookupOutcomes::StorageHit, start);
        } else {
            if (guestBytes == 0) guestBytes = SurfaceGuestBytes(resource);
            bool flushed = false;
            auto handOver = StorageTexture::PrepareCreation(context, resource, guestBytes, std::move(detached), flushed);
            if (flushed && profile) start = LookupOutcomes::Add(LookupOutcomes::PendingFlush, start);
            std::shared_ptr<StorageTexture> created;
            {
                const CacheHold reclaimable(storageCacheReclaimableThreadState());
                created = StorageTexture::Create(context, *context.detiler, resource, mip);
            }
            created->AdoptHandOver(std::move(handOver));
            CachedStorageTexture entry{key, mip, std::move(created)};
            static const bool keepNew = std::getenv("APS5_NO_KEEP_NEW_STORAGE") == nullptr;
            if (auto* recorder = Recorder::Active(); keepNew && recorder != nullptr && GuestMemory::GpuMutex().HeldByThisThread() && recorder->Recording()) recorder->Keep(entry.texture);
            entry.accounted = heldBytes(*entry.texture);
            const auto budget = storageBudget(context, cache.bytes + entry.accounted);
            while (!cache.entries.empty()) {
                const bool overBudget = cache.bytes + entry.accounted > budget;
                if (!overBudget && cache.entries.size() < storageEntries()) break;
                evicted.push_back(evictStorage(cache, std::prev(cache.entries.end()), true, overBudget ? "cache eviction" : "entry cap"));
            }
            cache.bytes += entry.accounted;
            texture = entry.texture;
            cache.entries.push_front(std::move(entry));
            cache.index[key] = cache.entries.begin();
            cache.byImage[texture.get()] = cache.entries.begin();
            texture->SetCached(true);
            counters.storageCreated.fetch_add(1, std::memory_order_relaxed);
            if (profile) LookupOutcomes::Add(LookupOutcomes::StorageMade, start);
        }
    }
    for (const auto& image : evicted) {
        std::vector<std::shared_ptr<ShaderResources>> resources;
        SharedResourceCache().RemoveImage(image.get(), &resources);
        if (auto* recorder = Recorder::Active(); recorder != nullptr && recorder->Recording()) {
            if (image.use_count() > 1) recorder->KeepBytes(image.get(), static_cast<std::size_t>(heldBytes(*image)));
            for (auto& resource : resources) {
                resource->KeepStorageImageBytes(*recorder);
                recorder->Keep(std::move(resource));
            }
        }
    }
    if (!evicted.empty()) {
        auto& sampled = Textures();
        std::lock_guard lock(sampled.mutex);
        dropUncachedViews(sampled);
    }
    return texture;
}

}

std::uint64_t TextureCacheBudget(const VkPhysicalDeviceMemoryProperties& memory) {
    const auto heap = largestDeviceLocalHeap(memory);
    return std::max<std::uint64_t>(2048ull << 20u, heap ? memory.memoryHeaps[*heap].size / 4u : 0u);
}

std::uint64_t StorageTextureCeiling(const VkPhysicalDeviceMemoryProperties& memory, const VkPhysicalDeviceMemoryBudgetPropertiesEXT* reported) {
    const auto heap = largestDeviceLocalHeap(memory);
    if (reported == nullptr || !heap || reported->heapBudget[*heap] == 0) return TextureCacheBudget(memory);
    return std::max<std::uint64_t>(512ull << 20u, std::min<std::uint64_t>(memory.memoryHeaps[*heap].size / 2u, reported->heapBudget[*heap] / 2u));
}

std::uint64_t StorageTextureBudget(const VkPhysicalDeviceMemoryProperties& memory, const VkPhysicalDeviceMemoryBudgetPropertiesEXT* reported, std::uint64_t sampledBytes, std::uint64_t storageBytes) {
    const auto heap = largestDeviceLocalHeap(memory);
    if (reported == nullptr || !heap || reported->heapBudget[*heap] == 0) return TextureCacheBudget(memory);
    const auto budget = reported->heapBudget[*heap];
    const auto usage = reported->heapUsage[*heap];
    const auto reserved = usage - std::min<std::uint64_t>(usage, sampledBytes + storageBytes) + sampledBytes + budget / 8u;
    return std::min<std::uint64_t>(StorageTextureCeiling(memory, reported), std::max<std::uint64_t>(512ull << 20u, budget > reserved ? budget - reserved : 0u));
}

std::uint64_t SampledTextureBudget(const VkPhysicalDeviceMemoryProperties& memory, const VkPhysicalDeviceMemoryBudgetPropertiesEXT* reported, std::uint64_t textureBytes, std::uint64_t storageBytes) {
    const auto heap = largestDeviceLocalHeap(memory);
    if (reported == nullptr || !heap || reported->heapBudget[*heap] == 0) return TextureCacheBudget(memory);
    const auto budget = reported->heapBudget[*heap];
    const auto usage = reported->heapUsage[*heap];
    const auto reserved = usage - std::min<std::uint64_t>(usage, textureBytes) + std::max(TextureCacheBudget(memory), storageBytes) + budget / 8u;
    return std::max<std::uint64_t>(2048ull << 20u, budget > reserved ? budget - reserved : 0u);
}

bool SampledBudgetReportDue(std::uint64_t reported, std::uint64_t budget, std::chrono::steady_clock::duration sinceReport) {
    const auto change = budget > reported ? budget - reported : reported - budget;
    return change > reported / 10u && sinceReport >= std::chrono::seconds(10);
}

std::optional<std::uint32_t> LargestDeviceLocalHeap(const VkPhysicalDeviceMemoryProperties& memory) {
    return largestDeviceLocalHeap(memory);
}

std::uint64_t StorageTextureCacheBudget(const Context& context) {
    return storageBudget(context);
}

void OverrideCacheBudgetForTest(std::uint64_t bytes) {
    budgetOverride.store(bytes, std::memory_order_relaxed);
}

TextureCacheUse TextureCacheUsage() {
    TextureCacheUse use;
    {
        auto& sampled = Textures();
        std::lock_guard lock(sampled.mutex);
        dropUncachedViews(sampled);
        use.sampledEntries = sampled.entries.size();
        use.sampledBytes = sampled.bytes;
        use.spareEntries = sampled.spare.size();
        use.spareBytes = sampled.spareBytes;
    }
    use.storageBytes = storageCacheBytes();
    use.largeMade = largeTexturesMade.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(largeReasonMutex);
        use.largeLastReason = largeLastReason;
    }
    return use;
}

std::uint64_t SampledTextureCacheBudget(const Context& context) {
    auto& cache = Textures();
    std::lock_guard lock(cache.mutex);
    return sampledBudget(context, cache);
}

DepthArrayGpuStatistics DepthArrayGpuCounts() {
    const auto& counters = depthArrayGpuCounters();
    return {counters.candidates.load(), counters.taken.load(), counters.layers.load(), counters.bytes.load(), counters.verified.load(), counters.mismatches.load()};
}

std::shared_ptr<Texture> CachedSampledTexture(const Context& context, std::span<const std::uint32_t> words, VkImageViewType multisampleViewType) {
    const auto resource = DecodeTextureResource(words);
    return cachedTexture(context, words, resource, ViewComponents(resource), 0, false, multisampleViewType);
}

bool SampledTexturesShareEntry(std::span<const std::uint32_t> first, std::span<const std::uint32_t> second) {
    Require(first.size() == 8 && second.size() == 8, "sampled texture keys take eight descriptor words");
    const VkComponentMapping components{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    const auto firstKey = MakeTextureKey(VK_NULL_HANDLE, first, components);
    const auto secondKey = MakeTextureKey(VK_NULL_HANDLE, second, components);
    return firstKey == secondKey && TextureKeyHash{}(firstKey) == TextureKeyHash{}(secondKey);
}

void FlushCachedTextures(VkDevice device) {
    Require(device != VK_NULL_HANDLE, "cannot flush textures without a Vulkan device");
    GuestMemory::AssertGpuLockHeld("FlushCachedTextures");
    auto& cache = StorageTextures();
    std::lock_guard lock(cache.mutex);
    const CacheHold held(storageCacheHeldThreadState());
    for (const auto& entry : cache.entries) {
        if (entry.key.device == device) entry.texture->Flush();
    }
}

void ClearCachedTextures(VkDevice device) {
    Require(device != VK_NULL_HANDLE, "cannot clear textures without a Vulkan device");
    auto& sampled = Textures();
    {
        std::lock_guard lock(sampled.mutex);
        const CacheHold held(sampledCacheHeldThreadState());
        for (auto it = sampled.entries.begin(); it != sampled.entries.end();) {
            if (it->key.device == device) eraseTexture(sampled, it++);
            else ++it;
        }
        sampled.spare.clear();
        sampled.spareBytes = 0;
    }
    auto& storage = StorageTextures();
    std::vector<std::shared_ptr<StorageTexture>> cleared;
    {
        std::lock_guard lock(storage.mutex);
        const CacheHold held(storageCacheHeldThreadState());
        for (auto it = storage.entries.begin(); it != storage.entries.end();) {
            if (it->key.device != device) {
                ++it;
                continue;
            }
            cleared.push_back(it->texture);
            it->texture->SetCached(false);
            noteUncached(it->texture);
            storage.bytes -= it->accounted;
            storage.index.erase(it->key);
            storage.byImage.erase(it->texture.get());
            it = storage.entries.erase(it);
        }
    }
    for (const auto& image : cleared) {
        std::vector<std::shared_ptr<ShaderResources>> resources;
        SharedResourceCache().RemoveImage(image.get(), &resources);
        if (auto* recorder = Recorder::Active(); recorder != nullptr && recorder->Recording()) {
            for (auto& resource : resources) {
                resource->KeepStorageImageBytes(*recorder);
                recorder->Keep(std::move(resource));
            }
        }
    }
}

std::uint64_t ReleaseTextureCaches(std::uint64_t bytes, bool newImage) {
    struct ReleasingThreadStorage {};
    auto& releasing = AgcDriver::ThreadScratch<bool, ReleasingThreadStorage>();
    if (releasing) return 0;
    releasing = true;
    struct Done {
        bool& flag;
        ~Done() { flag = false; }
    } done{releasing};
    const auto releasedBefore = ImageMemoryReleasedBytes();
    std::uint64_t freed = 0;
    std::size_t sampledEntries = 0;
    std::size_t storageEntries = 0;
    std::size_t storageKept = 0;
    bool sampledSkipped = false;
    bool storageSkipped = false;
    {
        auto& cache = Textures();
        std::unique_lock lock(cache.mutex, std::defer_lock);
        const bool inherited = sampledCacheHeldThreadState() != 0;
        if ((inherited && (!newImage || sampledCacheReclaimableThreadState() == 0)) || (!inherited && !lock.try_lock())) {
            sampledSkipped = true;
        } else {
            for (auto it = cache.entries.end(); it != cache.entries.begin() && freed < bytes;) {
                --it;
                if (it->texture.use_count() != 1 || (it->source != nullptr && it->source->Dirty())) continue;
                const auto next = std::next(it);
                eraseTexture(cache, it);
                ++cache.counts.evicted;
                ++sampledEntries;
                freed = ImageMemoryReleasedBytes() - releasedBefore;
                it = next;
            }
        }
    }
    if (freed < bytes) {
        auto& cache = StorageTextures();
        std::unique_lock<GuestMemory::GpuMutexType> gpu(GuestMemory::GpuMutex(), std::defer_lock);
        const bool inherited = storageCacheHeldThreadState() != 0;
        if ((inherited && (!newImage || storageCacheReclaimableThreadState() == 0)) || !gpu.try_lock()) {
            storageSkipped = true;
        } else {
            try {
                while (freed < bytes) {
                    std::shared_ptr<StorageTexture> image;
                    {
                        std::unique_lock lock(cache.mutex, std::defer_lock);
                        if (!inherited && !lock.try_lock()) {
                            storageSkipped = true;
                            break;
                        }
                        storageKept = 0;
                        for (auto it = cache.entries.end(); it != cache.entries.begin();) {
                            --it;
                            if (it->texture->Dirty()) {
                                ++storageKept;
                                continue;
                            }
                            image = evictStorage(cache, it);
                            break;
                        }
                        if (image == nullptr) break;
                    }
                    std::vector<std::shared_ptr<ShaderResources>> resources;
                    const bool removed = SharedResourceCache().TryRemoveImage(image.get(), &resources);
                    if (auto* recorder = Recorder::Active(); recorder != nullptr && recorder->Recording()) {
                        for (auto& resource : resources) recorder->Keep(std::move(resource));
                    }
                    resources.clear();
                    ++storageEntries;
                    image.reset();
                    freed = ImageMemoryReleasedBytes() - releasedBefore;
                    if (!removed) {
                        storageSkipped = true;
                        break;
                    }
                }
            } catch (const std::exception& error) {
                std::fprintf(stderr, "[gpu] texture cache release stopped: %s\n", error.what());
            }
        }
    }
    freed = ImageMemoryReleasedBytes() - releasedBefore;
    std::fprintf(stderr, "[gpu] texture caches released %llu MiB of the %llu MiB asked for: %zu sampled entries%s, %zu storage images%s, %zu with pending results kept\n", static_cast<unsigned long long>(freed >> 20u), static_cast<unsigned long long>(bytes >> 20u), sampledEntries, sampledSkipped ? " (cache busy, skipped)" : "", storageEntries, storageSkipped ? " (cache or device busy, skipped)" : "", storageKept);
    return freed;
}

bool StorageImageCached(const Context& context, const StorageTexture* image) {
    auto& cache = StorageTextures();
    std::lock_guard lock(cache.mutex);
    const auto it = findStorageByImage(cache, context.device, image);
    if (it == cache.entries.end()) return false;
    cache.entries.splice(cache.entries.begin(), cache.entries, it);
    return true;
}

namespace {

// StorageImageCached for several images under one acquisition of the cache mutex.
bool StorageImagesCached(const Context& context, std::span<const StorageTexture* const> images) {
    if (images.empty()) return true;
    auto& cache = StorageTextures();
    std::lock_guard lock(cache.mutex);
    for (const auto* image : images) {
        const auto it = findStorageByImage(cache, context.device, image);
        if (it == cache.entries.end()) return false;
        cache.entries.splice(cache.entries.begin(), cache.entries, it);
    }
    return true;
}

}

StorageTexture::RefreshPreparation PrepareStorageSurface(const Context& context, const GuestTextureResource& resource) {
    static const bool disabled = std::getenv("APS5_NO_TEXTURE_CACHE") != nullptr;
    if (disabled || !StorageTexture::DrawTargetPrelockEnabled() || GuestMemory::GpuMutex().HeldByThisThread() || resource.dimension != TextureDimension::k2D || resource.mipCount != 1 || resource.depthOrLastArray != 0 || resource.baseArray != 0) return {};
    const auto serial = ExtendedChains().serial.load(std::memory_order_acquire);
    const auto viewed = ArrayLayerWindow(resource, context.limits.maxImageArrayLayers);
    const StorageKey key{context.device, SurfaceKey(context, StorageSurface(context, viewed))};
    auto& cache = StorageTextures();
    std::shared_ptr<StorageTexture> image;
    {
        std::lock_guard lock(cache.mutex);
        const auto found = findStorage(cache, key);
        if (found == cache.entries.end()) return {};
        image = found->texture;
    }
    auto prepared = image->PrepareRefresh();
    prepared.device = context.device;
    prepared.lookupKey = SurfaceKey(context, resource);
    prepared.storageSerial = serial;
    return prepared;
}

std::shared_ptr<StorageTexture> CachedStorageSurface(const Context& context, const GuestTextureResource& resource, StorageTexture::RefreshPreparation* preparation) {
    if (preparation != nullptr && preparation->ready && preparation->device == context.device &&
        resource.dimension == TextureDimension::k2D && resource.mipCount == 1 && resource.depthOrLastArray == 0 && resource.baseArray == 0 &&
        preparation->storageSerial == ExtendedChains().serial.load(std::memory_order_acquire) &&
        preparation->lookupKey == SurfaceKey(context, resource) && !DepthPlanesStale() && GuestMemory::GpuMutex().HeldByThisThread()) {
        const bool profile = LookupOutcomes::Profiled();
        const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto hold = GuestMemory::GpuHoldSerial();
        const auto image = preparation->image.lock();
        if (auto* recorder = Recorder::Active(); recorder != nullptr) recorder->BoundKeptBytes();
        if (image != nullptr && !DepthSurfaceOverlaps(context.device, image->Descriptor().baseAddress, image->GuestBytes()) &&
            StorageImageServesKeys(*image, resource.dccAddress) && StorageImageCached(context, image.get())) {
            RequireNoMultisampleTarget(image->Descriptor().baseAddress, static_cast<std::size_t>(image->GuestBytes()), "a storage image access");
            if (image->TryPreparedRefresh(*preparation) && hold == GuestMemory::GpuHoldSerial() &&
                preparation->storageSerial == ExtendedChains().serial.load(std::memory_order_acquire) && !DepthPlanesStale()) {
                TextureCounts().storageHits.fetch_add(1, std::memory_order_relaxed);
                if (profile) {
                    preparation->lookupAccepted = true;
                    LookupOutcomes::Add(LookupOutcomes::StorageHit, start);
                }
                return image;
            }
        }
    }
    return cachedStorageTexture(context, {}, resource, 0, 0, false, std::nullopt, preparation);
}

std::uint64_t StorageSurfaceSerial() {
    return ExtendedChains().serial.load(std::memory_order_acquire);
}

std::shared_ptr<StorageTexture> CachedStorageLayers(const Context& context, const GuestTextureResource& resource, std::uint32_t firstLayer, std::uint32_t layerCount) {
    return cachedStorageTexture(context, {}, resource, 0, 0, false, std::pair{firstLayer, layerCount});
}

bool StorageImageServesKeys(const StorageTexture& image, std::uint64_t dccAddress) {
    GuestTextureResource resource{};
    resource.dccAddress = dccAddress;
    return !MetadataMoved(image, resource);
}

std::shared_ptr<StorageTexture> RepeatStorageSurface(const Context& context, const GuestTextureResource& requested, const std::shared_ptr<StorageTexture>& stored) {
    static const bool disabled = std::getenv("APS5_NO_TEXTURE_CACHE") != nullptr;
    if (disabled || stored == nullptr) return nullptr;
    const auto viewed = ArrayLayerWindow(requested, context.limits.maxImageArrayLayers);
    Require(viewed.dimension != TextureDimension::k2DMsaa, "storage image access to a multisampled texture is not implemented");
    const auto viewedBytes = DescribeSurface(viewed).guestBytes;
    RequireNoMultisampleTarget(viewed.baseAddress, static_cast<std::size_t>(viewedBytes), "a storage image access");
    if (DepthSurfaceStorage(context, {}, viewed, 0) != nullptr) return nullptr;
    if (DepthPlanesStale()) static_cast<void>(FlushDepthPlanes(viewed.baseAddress, static_cast<std::size_t>(viewedBytes), DepthPlaneScope::Overlapped));
    if (auto* recorder = Recorder::Active(); recorder != nullptr) recorder->BoundKeptBytes();
    const bool profile = LookupOutcomes::Profiled();
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto resource = StorageSurface(context, viewed);
    auto& cache = StorageTextures();
    std::lock_guard lock(cache.mutex);
    const CacheHold held(storageCacheHeldThreadState());
    if (resource.mipCount > AllocatedLevels(resource)) return nullptr;
    const auto it = findStorageByImage(cache, context.device, stored.get());
    if (it == cache.entries.end() || MetadataMoved(*it->texture, resource)) return nullptr;
    it->texture->Refresh();
    cache.entries.splice(cache.entries.begin(), cache.entries, it);
    TextureCounts().storageHits.fetch_add(1, std::memory_order_relaxed);
    if (profile) LookupOutcomes::Add(LookupOutcomes::StorageHit, start);
    return it->texture;
}

namespace {

const char* roleName(ShaderRecompiler::DescriptorRole role) {
    switch (role) {
        case ShaderRecompiler::DescriptorRole::GuestBuffers: return "GuestBuffers";
        case ShaderRecompiler::DescriptorRole::GuestImages: return "GuestImages";
        case ShaderRecompiler::DescriptorRole::GuestSamplers: return "GuestSamplers";
        case ShaderRecompiler::DescriptorRole::Gds: return "Gds";
        case ShaderRecompiler::DescriptorRole::BdaPagetable: return "BdaPagetable";
        case ShaderRecompiler::DescriptorRole::FaultBuffer: return "FaultBuffer";
        case ShaderRecompiler::DescriptorRole::FlattenedSrt: return "FlattenedSrt";
        case ShaderRecompiler::DescriptorRole::ShaderData: return "ShaderData";
        case ShaderRecompiler::DescriptorRole::ImageTable: return "ImageTable";
        case ShaderRecompiler::DescriptorRole::SamplerTable: return "SamplerTable";
        case ShaderRecompiler::DescriptorRole::ImageTableMap: return "ImageTableMap";
        case ShaderRecompiler::DescriptorRole::ImageStorageTable: return "ImageStorageTable";
    }
    throw std::runtime_error("AGC graphics: unknown descriptor role");
}

const char* kindName(ShaderRecompiler::DescriptorKind kind) {
    switch (kind) {
        case ShaderRecompiler::DescriptorKind::UniformBuffer: return "UniformBuffer";
        case ShaderRecompiler::DescriptorKind::StorageBuffer: return "StorageBuffer";
        case ShaderRecompiler::DescriptorKind::UniformTexelBuffer: return "UniformTexelBuffer";
        case ShaderRecompiler::DescriptorKind::StorageTexelBuffer: return "StorageTexelBuffer";
        case ShaderRecompiler::DescriptorKind::SampledImage: return "SampledImage";
        case ShaderRecompiler::DescriptorKind::StorageImage: return "StorageImage";
        case ShaderRecompiler::DescriptorKind::Sampler: return "Sampler";
    }
    throw std::runtime_error("AGC graphics: unknown descriptor kind");
}

// Consecutive identical storage descriptors address successive mips of one texture (dynamic-mip
// storage writes), so the second and later ones share the first one's image lookup.
// APS5_NO_STORAGE_DEDUPE=1 looks each one up (and refreshes the image) separately as before.
bool StorageDedupeEnabled() {
    static const bool disabled = std::getenv("APS5_NO_STORAGE_DEDUPE") != nullptr;
    return !disabled;
}

// Whether storage element `element` of `binding` repeats the previous element's eight words.
bool SameAsPreviousStorageElement(const ShaderRecompiler::DescriptorBinding& binding, std::uint32_t element) {
    if (element == 0) return false;
    const auto words = binding.guestDescriptor.begin() + static_cast<std::size_t>(element) * 8u;
    return std::equal(words, words + 8, words - 8);
}

}

ShaderResources::ShaderResources(const Context& context, const ShaderRecompiler::RecompileResult& vertex, const ShaderRecompiler::RecompileResult& fragment, const ColorTarget& target, std::uint64_t indexAddress, std::size_t indexBytes) : ShaderResources(context, std::array<CompiledShader, 2>{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &fragment, static_cast<std::uint32_t>(vertex.pushConstants.size())}}}, target, 1, indexAddress, indexBytes) {}

void ShaderResources::KeepStorageImageBytes(Recorder& recorder) const {
    const auto keep = [&](const std::shared_ptr<StorageTexture>& image) {
        if (image != nullptr && !image->Cached()) recorder.KeepBytes(image.get(), static_cast<std::size_t>(image->BudgetBytes()));
    };
    for (const auto& image : storageTextures) keep(image);
    for (const auto& texture : textures) {
        if (texture != nullptr) keep(texture->SharedStorageSource());
    }
}

ShaderResources::ShaderResources(const Context& context, std::span<const CompiledShader> shaders, const ColorTarget& target, std::uint32_t colorAttachments, std::uint64_t indexAddress, std::size_t indexBytes, std::span<const GuestMemorySnapshot> snapshots, bool deviceStaging) : context(context), guestMemory(context) {
    drawBuild = true;
    const bool addressTables = std::ranges::any_of(shaders, [](const CompiledShader& shader) { return shader.program != nullptr && std::ranges::any_of(shader.program->bindings, [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable; }); });
    if (deviceStaging && !addressTables) guestMemory.AllowDeviceStaging();
    prepareAddressBindings(shaders, snapshots);
    build(shaders, &target, colorAttachments, indexAddress, indexBytes);
}

ShaderResources::ShaderResources(const Context& context, const CompiledShader& compute, std::span<const GuestMemorySnapshot> snapshots) : ShaderResources(context, compute, snapshots, false) {}

namespace {

// Whether a compute stage maps registered guest memory through BDA tables (see prepareAddressBindings).
bool UsesAddressTables(const CompiledShader& compute) {
    Require(compute.program != nullptr, "missing compiled shader");
    return std::any_of(compute.program->bindings.begin(), compute.program->bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable; });
}

}

ShaderResources::ShaderResources(const Context& context, const CompiledShader& compute, std::span<const GuestMemorySnapshot> snapshots, bool deferred, std::uint64_t dispatchThreads, std::optional<std::pair<std::uint64_t, std::size_t>> htileClear, bool allowDispatchSpaceReuse) : context(context), guestMemory(context), allowDispatchSpaceReuse(allowDispatchSpaceReuse), dispatchThreads(dispatchThreads), htileClear(htileClear), deferredCompute(compute), deferredSnapshots(snapshots) {
    Require(compute.stage == ShaderRecompiler::ShaderStage::Compute, "compute resources require a compute shader");
    this->allowDispatchSpaceReuse = allowDispatchSpaceReuse && compute.program != nullptr && compute.program->workgroupMemoryDwords == 0 && compute.program->scratchPoolSlotDwords == 0;
    // Every use of a compute build is a recorded dispatch that calls MarkGpuWrites, which staged
    // buffers need (a synchronous draw's use would not).
    guestMemory.AllowDeviceStaging();
    static const std::uint64_t addressStagingMinThreads = [] {
        const char* value = std::getenv("APS5_BDA_STAGING_MIN_THREADS");
        return value != nullptr ? std::strtoull(value, nullptr, 10) : 512ull;
    }();
    if (dispatchThreads != 0 && dispatchThreads < addressStagingMinThreads) guestMemory.DisallowAddressStaging();
    const std::span<const CompiledShader> shaders(&deferredCompute, 1);
    if (!deferred) {
        prepareAddressBindings(shaders, snapshots);
        build(shaders, nullptr, 0, 0, 0);
        forgetDeferredInputs();
        return;
    }
    // Address-based shaders pin and mirror registered memory in prepareAddressBindings (reconciling
    // imports retires buffers to the recorder, refreshing mirrors waits for recorded work): that is
    // device-lock work, so their whole build waits for Complete(). Without tables the call only
    // checks the bindings.
    lockedBuild = UsesAddressTables(compute);
    if (lockedBuild) return;
    unlockedPrepare = true;
    prepareAddressBindings(shaders, snapshots);
    buildPrepare(shaders, nullptr, 0, 0, 0);
}

void ShaderResources::Complete() {
    Require(!completed, "shader resources were already completed");
    const std::span<const CompiledShader> shaders(&deferredCompute, 1);
    if (lockedBuild) {
        prepareAddressBindings(shaders, deferredSnapshots);
        buildPrepare(shaders, nullptr, 0, 0, 0);
    }
    buildComplete();
    forgetDeferredInputs();
}

void ShaderResources::forgetDeferredInputs() {
    // The compiled shader and the captured regions belong to the dispatch, which returns while this
    // object lives on in the resource cache and the recorder: nothing may reach for them after the
    // build, so they are dropped rather than left dangling.
    deferredCompute = {};
    deferredSnapshots = {};
}

void ShaderResources::build(std::span<const CompiledShader> shaders, const ColorTarget* target, std::uint32_t colorAttachments, std::uint64_t indexAddress, std::size_t indexBytes) {
    buildPrepare(shaders, target, colorAttachments, indexAddress, indexBytes);
    buildComplete();
}

namespace {

VkDescriptorSetLayout CreateSetLayout(const Context& context, std::span<const VkDescriptorSetLayoutBinding> bindings, bool updateAfterBind, std::span<const VkDescriptorBindingFlags> flags) {
    Require(flags.size() == bindings.size(), "descriptor binding flag count differs from layout");
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bindingFlags.bindingCount = static_cast<std::uint32_t>(flags.size());
    bindingFlags.pBindingFlags = flags.data();
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.pNext = std::ranges::any_of(flags, [](auto value) { return value != 0; }) ? &bindingFlags : nullptr;
    info.flags = updateAfterBind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &info, nullptr, &layout), "vkCreateDescriptorSetLayout");
    return layout;
}

std::vector<VkDescriptorPoolSize> PoolSizes(std::span<const VkDescriptorSetLayoutBinding> bindings) {
    std::map<VkDescriptorType, std::uint32_t> counts;
    for (const auto& binding : bindings) counts[binding.descriptorType] += binding.descriptorCount;
    std::vector<VkDescriptorPoolSize> sizes;
    for (const auto& [type, count] : counts) sizes.push_back({type, count});
    return sizes;
}

VkDescriptorPool CreateSetPool(const Context& context, std::span<const VkDescriptorPoolSize> sizes, bool updateAfterBind) {
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.flags = updateAfterBind ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0;
    info.maxSets = 1;
    info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    info.pPoolSizes = sizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &info, nullptr, &pool), "vkCreateDescriptorPool");
    return pool;
}

// APS5_PROFILE_DRAW: the [resources] phase totals. Each thread accumulates its builds' phases in
// arrays of its own and merges them into the shared totals every 1000 of its builds (and when it
// ends), so no build takes the shared mutex per phase; the totals lag by up to 999 builds per
// worker.
constexpr std::size_t BuildPhaseCount = static_cast<std::size_t>(ShaderResources::BuildPhase::Count);
constexpr std::array<const char*, BuildPhaseCount> BuildPhaseNames{"bindings", "precollect", "guest memory upload", "descriptors", "stage A", "images", "bda", "stage B"};

struct BuildProfile {
    AgcDriver::Mutex mutex;
    std::array<double, BuildPhaseCount> ms{};
    std::uint64_t builds = 0;
};

BuildProfile& Builds() {
    static BuildProfile profile;
    return profile;
}

bool BuildProfiled() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

struct ThreadBuildProfile {
    std::array<double, BuildPhaseCount> ms{};
    std::uint64_t builds = 0;
    ~ThreadBuildProfile() { merge(); }
    void merge() {
        auto& profile = Builds();
        std::lock_guard lock(profile.mutex);
        for (std::size_t i = 0; i < BuildPhaseCount; ++i) profile.ms[i] += ms[i];
        profile.builds += builds;
        ms = {};
        if (builds == 0) return;
        builds = 0;
        std::string report;
        for (std::size_t i = 0; i < BuildPhaseCount; ++i) report += " " + std::string(BuildPhaseNames[i]) + "=" + std::to_string(static_cast<long long>(profile.ms[i])) + "ms";
        AgcDriver::ProfilePrint_nid_no_patch("[resources] %llu builds, phase totals:%s\n", static_cast<unsigned long long>(profile.builds), report.c_str());
    }
};

ThreadBuildProfile& ThreadBuilds() {
    struct ProfileSlotStorage {};
    auto& profile = AgcDriver::ThreadScratch<ThreadBuildProfile, ProfileSlotStorage>();
    return profile;
}

void addBuildPhase(ShaderResources::BuildPhase which, double ms) {
    ThreadBuilds().ms[static_cast<std::size_t>(which)] += ms;
}

// APS5_PROFILE_DRAW: guest buffer elements bound by descriptor (addGuestBuffer) and how many the
// recompiler proved read-only, plus the pending-write notes their uses skipped (MarkGpuWrites),
// printed as [buffers] every 10 s from MarkGpuWrites. Cumulative.
struct BufferWriteCounters {
    std::atomic<std::uint64_t> elements{0};
    std::atomic<std::uint64_t> readOnly{0};
    std::atomic<std::uint64_t> notesSkipped{0};
    std::atomic<std::int64_t> lastReport{0};
};

BufferWriteCounters& BufferWrites() {
    static BufferWriteCounters counters;
    return counters;
}

}

double ShaderResources::phase(BuildPhase which) {
    if (!BuildProfiled() && !AgcDriver::DispatchBuildTrace::Enabled()) return 0.0;
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration<double, std::milli>(now - phaseStart).count();
    if (BuildProfiled()) addBuildPhase(which, ms);
    if (BuildProfiled() && ms > 50) AgcDriver::ProfilePrint_nid_no_patch("[resources] %s took %.0f ms (%zu textures, %zu storage images, %zu buffers, bda %d)\n", BuildPhaseNames[static_cast<std::size_t>(which)], ms, textures.size(), storageTextures.size(), allocations.size(), usesBda ? 1 : 0);
    phaseStart = now;
    return ms;
}

namespace {

std::uint32_t IrStage(ShaderRecompiler::ShaderStage stage) {
    using Ir = ShaderRecompiler::IrShaderStage;
    switch (stage) {
        case ShaderRecompiler::ShaderStage::Compute: return static_cast<std::uint32_t>(Ir::Compute);
        case ShaderRecompiler::ShaderStage::Vertex: return static_cast<std::uint32_t>(Ir::Vertex);
        case ShaderRecompiler::ShaderStage::Fragment: return static_cast<std::uint32_t>(Ir::Pixel);
        case ShaderRecompiler::ShaderStage::Mesh: return static_cast<std::uint32_t>(Ir::Mesh);
        case ShaderRecompiler::ShaderStage::Local: return static_cast<std::uint32_t>(Ir::Local);
        case ShaderRecompiler::ShaderStage::TessellationControl: return static_cast<std::uint32_t>(Ir::TessellationControl);
        case ShaderRecompiler::ShaderStage::TessellationEvaluation: return static_cast<std::uint32_t>(Ir::TessellationEvaluation);
        case ShaderRecompiler::ShaderStage::Geometry: break;
    }
    return static_cast<std::uint32_t>(Ir::Unknown);
}

bool TableReuseEnabled() {
    static const bool disabled = std::getenv("APS5_NO_TABLE_REUSE") != nullptr;
    return !disabled;
}

bool TableReplay() {
    static const bool disabled = std::getenv("APS5_NO_TABLE_REPLAY") != nullptr;
    return !disabled;
}

struct TableState {
    std::uint64_t epoch = 0;
    std::uint64_t unwatched = 0;
    std::uint64_t allocations = 0;
    std::uint64_t depthSurfaces = 0;
    bool operator==(const TableState&) const = default;
};

TableState CurrentTableState() {
    if (!TableReuseEnabled()) return {};
    return {GuestMemory::ThreadCollectEpoch(), GuestMemory::UnwatchSerial(), GuestAllocations::GuestAllocationsGeneration_nid_postfix(), DepthSurfacesSerial()};
}

constexpr auto NoRepresentative = std::numeric_limits<std::uint32_t>::max();

std::vector<std::uint32_t> variantRepresentatives(VkDevice device, const ShaderRecompiler::DescriptorBinding& binding, std::uint64_t elementBase, std::uint64_t capacity);

struct ReusedTableElement {
    TextureKey key{};
    std::weak_ptr<Texture> texture;
    GuestTextureResource resource{};
    std::uint64_t guestBytes = 0;
    bool firstLayer = false;
    bool rejected = false;
    std::string message;
    bool live = false;
};

struct ReusedTable {
    VkDevice device = VK_NULL_HANDLE;
    TableState state;
    std::uint64_t pendingSerial = 0;
    std::vector<std::uint32_t> words;
    std::vector<ShaderRecompiler::DescriptorImageShape> shapes;
    ShaderRecompiler::ElementFlags depthCompare;
    std::vector<ReusedTableElement> elements;
    std::vector<GuestMemory::UnchangedQuery> queries;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::vector<std::uint64_t> imageSamplers;
    std::vector<std::uint32_t> samplers;
    std::vector<std::string> bound;
    bool replayable = false;
};

constexpr std::size_t ReusedTableCount = 4;

std::vector<ReusedTable>& ReusedTables() {
    struct SlotStorage {};
    return AgcDriver::ThreadScratch<std::vector<ReusedTable>, SlotStorage>();
}

bool SameTable(const ReusedTable& table, VkDevice device, const ShaderRecompiler::DescriptorBinding& binding) {
    return table.device == device && table.words == binding.guestDescriptor && table.shapes == binding.imageShapes && table.depthCompare == binding.Flags().imageDepthCompare;
}

ReusedTable* FindReusedTable(VkDevice device, const ShaderRecompiler::DescriptorBinding& binding, const TableState& state) {
    if (state.epoch == 0) return nullptr;
    auto& tables = ReusedTables();
    std::erase_if(tables, [&](const ReusedTable& table) { return table.state != state; });
    for (auto& table : tables) {
        if (SameTable(table, device, binding)) return &table;
    }
    return nullptr;
}

bool TableCurrent(ReusedTable& table) {
    const auto serial = StorageTexture::PendingSerial();
    if (serial != table.pendingSerial) {
        if (StorageTexture::AnyPendingOverlaps(table.ranges) || StorageTexture::PendingSerial() != serial) return false;
        table.pendingSerial = serial;
    }
    return GuestMemory::UnchangedSinceAll(table.queries);
}

bool CachedTableTextures(const ReusedTable& table, std::vector<std::shared_ptr<Texture>>& cached) {
    auto& cache = Textures();
    std::lock_guard lock(cache.mutex);
    std::vector<std::list<CachedTexture>::iterator> entries;
    entries.reserve(table.queries.size());
    for (const auto& element : table.elements) {
        if (element.rejected || element.live) continue;
        const auto it = findTexture(cache, element.key);
        if (it == cache.entries.end() || it->texture != element.texture.lock()) return false;
        entries.push_back(it);
    }
    cached.reserve(entries.size());
    for (const auto it : entries) {
        cached.push_back(it->texture);
        touchTexture(cache, it);
    }
    return true;
}

void RememberTable(ReusedTable&& table, const ShaderRecompiler::DescriptorBinding& binding) {
    auto& tables = ReusedTables();
    std::erase_if(tables, [&](const ReusedTable& entry) { return SameTable(entry, table.device, binding); });
    table.words = binding.guestDescriptor;
    table.shapes = binding.imageShapes;
    table.depthCompare = binding.Flags().imageDepthCompare;
    tables.insert(tables.begin(), std::move(table));
    if (tables.size() > ReusedTableCount) tables.resize(ReusedTableCount);
}

std::string HexText(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(value));
    return text;
}

std::string DescriptorLimitError(std::string_view description, std::uint64_t required, std::uint64_t limit) {
    return std::string(description) + ": required " + std::to_string(required) + ", device limit " + std::to_string(limit);
}

void ValidateDescriptorLayout(const Context& context, std::span<const VkDescriptorSetLayoutBinding> bindings, bool updateAfterBind) {
    const auto& indexed = context.descriptorIndexingLimits;
    const bool sampledUpdateAfterBind = updateAfterBind && context.sampledImageUpdateAfterBind;
    const auto maxStageResources = updateAfterBind ? indexed.maxPerStageUpdateAfterBindResources : context.limits.maxPerStageResources;
    const auto maxStageSampledImages = sampledUpdateAfterBind ? indexed.maxPerStageDescriptorUpdateAfterBindSampledImages : context.limits.maxPerStageDescriptorSampledImages;
    const auto maxStageSamplers = sampledUpdateAfterBind ? indexed.maxPerStageDescriptorUpdateAfterBindSamplers : context.limits.maxPerStageDescriptorSamplers;
    const auto maxSetSampledImages = sampledUpdateAfterBind ? indexed.maxDescriptorSetUpdateAfterBindSampledImages : context.limits.maxDescriptorSetSampledImages;
    const auto maxSetSamplers = sampledUpdateAfterBind ? indexed.maxDescriptorSetUpdateAfterBindSamplers : context.limits.maxDescriptorSetSamplers;
    std::map<VkDescriptorType, std::uint64_t> heapTotals;
    std::map<std::pair<VkShaderStageFlags, VkDescriptorType>, std::uint64_t> stageTotals;
    std::map<VkShaderStageFlags, std::uint64_t> stageResources;
    for (const auto& layout : bindings) {
        heapTotals[layout.descriptorType] += layout.descriptorCount;
        for (std::uint32_t bit = 0u; bit < 32u; ++bit) {
            const auto stage = VkShaderStageFlags{1u} << bit;
            if ((layout.stageFlags & stage) == 0u) continue;
            const auto count = stageTotals[{stage, layout.descriptorType}] += layout.descriptorCount;
            const auto all = stageResources[stage] += layout.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ? 0u : layout.descriptorCount;
            Require(all <= maxStageResources, "typed heaps exceed per-stage resource capacity");
            if (layout.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) Require(count <= maxStageSampledImages, "typed heaps exceed per-stage sampled image capacity");
            if (layout.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) Require(count <= context.limits.maxPerStageDescriptorStorageImages, "typed heaps exceed per-stage storage image capacity");
            if (layout.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER && count > maxStageSamplers) throw std::runtime_error(DescriptorLimitError("typed heaps exceed per-stage sampler capacity", count, maxStageSamplers));
        }
    }
    Require(heapTotals[VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE] <= maxSetSampledImages && heapTotals[VK_DESCRIPTOR_TYPE_STORAGE_IMAGE] <= context.limits.maxDescriptorSetStorageImages, "typed heaps exceed descriptor set capacity");
    if (heapTotals[VK_DESCRIPTOR_TYPE_SAMPLER] > maxSetSamplers) throw std::runtime_error(DescriptorLimitError("typed heaps exceed descriptor set sampler capacity", heapTotals[VK_DESCRIPTOR_TYPE_SAMPLER], maxSetSamplers));
}

void ValidateRuntimeResources(const CompiledShader& shader, std::span<const std::uint32_t> words) {
    Require(shader.program != nullptr, "missing compiled shader");
    const auto& program = *shader.program;
    const auto& descriptors = program.bindings;
    constexpr auto bindingCount = static_cast<std::uint32_t>(ShaderRecompiler::RuntimeAbi::Binding::Count);
    std::array<const ShaderRecompiler::DescriptorBinding*, bindingCount> heaps{};
    if (program.runtimeImageCount != 0u) {
        for (const auto& binding : descriptors) {
            if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages) continue;
            auto& heap = heaps[binding.binding % bindingCount];
            Require(heap == nullptr, "duplicate runtime image binding");
            heap = &binding;
        }
    }
    Require(words.size() == program.shaderDataDwords, "invalid compact shader data size");
    constexpr auto metadataWords = sizeof(ShaderRecompiler::RuntimeAbi::ResourceMetadata) / sizeof(std::uint32_t);
    Require(program.imageMetadataDword <= words.size() && program.runtimeImageCount <= (words.size() - program.imageMetadataDword) / metadataWords, "runtime image metadata exceeds shader data");
    for (const auto index : program.runtimeImageResources) {
        Require(index < program.runtimeImageCount, "runtime image resource exceeds its compact layout");
        const auto offset = program.imageMetadataDword + index * metadataWords;
        const auto kind = words[offset];
        const auto first = words[offset + 1u];
        const auto count = words[offset + 2u];
        Require(count != 0u, "runtime image metadata has no descriptor elements");
        Require(kind < heaps.size() && heaps[kind] != nullptr, "runtime metadata references an unbound image heap");
        Require(first < heaps[kind]->count && count <= heaps[kind]->count - first, "runtime metadata exceeds its bound heap");
    }
}

}

void ShaderResources::buildPrepare(std::span<const CompiledShader> shaders, const ColorTarget* target, std::uint32_t colorAttachments, std::uint64_t indexAddress, std::size_t indexBytes) {
    registeredImageViews.clear();
    for (const auto& shader : shaders) if (shader.registeredImageViews != nullptr) registeredImageViews.insert(registeredImageViews.end(), shader.registeredImageViews->begin(), shader.registeredImageViews->end());
    Require(registeredImageViewsCurrent(registeredImageViews), "AGC graphics: registered image range changed after resource capture");
    PerformanceTimer frameTiming("Resources.Prepare");
    const auto stageStart = std::chrono::steady_clock::now();
    phaseStart = stageStart;
    if (BuildProfiled()) {
        auto& profile = ThreadBuilds();
        if (++profile.builds % 1000 == 0) profile.merge();
    }
    try {
        Require(!shaders.empty() && context.limits.maxBoundDescriptorSets >= 1, "shader descriptor set exceeds device limits");
        if (shaders.size() == 1u) refreshResourceKey = ContentKey(shaders.front(), false, true);
        std::size_t plannedBindings = 0;
        std::size_t plannedAllocations = 0;
        for (const auto& shader : shaders) {
            if (shader.program == nullptr) continue;
            plannedBindings += shader.program->bindings.size();
            for (const auto& binding : shader.program->bindings) {
                if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages && binding.role != ShaderRecompiler::DescriptorRole::GuestSamplers) plannedAllocations += binding.count;
            }
        }
        bindings.reserve(plannedBindings);
        allocations.reserve(plannedAllocations);
        std::vector<std::uint32_t> occupied;
        occupied.reserve(plannedBindings);
        for (const auto& shader : shaders) {
            Require(shader.program != nullptr, "missing compiled shader");
            ShaderRecompiler::RuntimeAbi::RequireVersion(shader.program->runtimeAbiVersion);
            if (shader.program->poisonedSrtReads != 0) {
                const auto& programBindings = shader.program->bindings;
                const auto flat = std::find_if(programBindings.begin(), programBindings.end(), [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::FlattenedSrt; });
                if (flat != programBindings.end()) srtGuardFaults.push_back({IrStage(shader.stage), flat->guestDescriptor});
            }
            const VkShaderStageFlags flags = VulkanStage(shader.stage);
            std::uint64_t stageStorageBuffers = 0;
            std::uint64_t stageResources = shader.stage == ShaderRecompiler::ShaderStage::Fragment ? colorAttachments : 0;
            Require(stageResources <= context.limits.maxPerStageResources, "shader descriptors exceed per-stage limits");
            std::vector<std::size_t> offsetsInData;
            std::int64_t shaderData = -1;
            const auto firstSampler = samplers.size();
            const auto firstDeferred = deferredImages.size();
            const auto requireStageLimits = [&] {
                if (stageStorageBuffers <= context.limits.maxPerStageDescriptorStorageBuffers && stageResources <= context.limits.maxPerStageResources) return;
                const auto& limits = context.descriptorIndexingLimits;
                Require(stageStorageBuffers <= limits.maxPerStageDescriptorUpdateAfterBindStorageBuffers && stageResources <= limits.maxPerStageUpdateAfterBindResources, "shader descriptors exceed per-stage limits");
                updateAfterBind = true;
            };
            for (const auto& binding : shader.program->bindings) {
                Require(binding.descriptorSet == ShaderRecompiler::RuntimeAbi::DescriptorSet || (shader.stage == ShaderRecompiler::ShaderStage::Fragment && binding.descriptorSet == ShaderRecompiler::RuntimeAbi::FragmentDescriptorSet), "unexpected descriptor set: a shader resource must use its stage's descriptor set");
                if (binding.role == ShaderRecompiler::DescriptorRole::ImageTable || binding.role == ShaderRecompiler::DescriptorRole::ImageStorageTable) {
                    const bool storage = binding.role == ShaderRecompiler::DescriptorRole::ImageStorageTable;
                    Require(binding.binding == (storage ? ShaderRecompiler::RuntimeAbi::ImageStorageTableBinding : ShaderRecompiler::RuntimeAbi::ImageTableBinding) && binding.kind == (storage ? ShaderRecompiler::DescriptorKind::StorageImage : ShaderRecompiler::DescriptorKind::SampledImage) && binding.guestDescriptor.size() == static_cast<std::size_t>(binding.count) * 8u && binding.imageShapes.size() == binding.count && binding.Flags().imageDepthCompare.size() == binding.count && binding.imageSamplers.size() == binding.count, "image table binding is malformed");
                    auto& table = tableStage(shader);
                    Require(storage ? !table.storage : !table.sampled, "a stage binds two image tables");
                    Require(table.images == nullptr || table.images->count == binding.count, "a stage's image tables disagree on their elements");
                    (storage ? table.storage : table.sampled) = true;
                    table.images = &binding;
                    table.firstSampler = firstSampler;
                    continue;
                }
                Require(std::find(occupied.begin(), occupied.end(), binding.binding) == occupied.end(), "duplicate shader binding");
                occupied.push_back(binding.binding);
                if (binding.role == ShaderRecompiler::DescriptorRole::SamplerTable) {
                    addSamplerTable(binding, flags, tableStage(shader));
                    continue;
                }
                if (binding.role == ShaderRecompiler::DescriptorRole::ImageTableMap) {
                    Require(binding.kind == ShaderRecompiler::DescriptorKind::StorageBuffer && binding.count == 1 && binding.guestDescriptor.size() >= ShaderRecompiler::ImageTableAbi::HeaderWords && !binding.readOnly && binding.guestDescriptor[ShaderRecompiler::ImageTableAbi::HeaderVersion] == ShaderRecompiler::ImageTableAbi::Version, "image table map binding is malformed");
                    const auto size = binding.guestDescriptor.size() * sizeof(std::uint32_t);
                    Require(size <= context.limits.maxStorageBufferRange, "image table map exceeds the storage buffer range limit");
                    stageStorageBuffers += 1;
                    stageResources += 1;
                    storageBuffers += 1;
                    requireStageLimits();
                    auto& table = tableStage(shader);
                    Require(table.mapAllocation == std::numeric_limits<std::size_t>::max(), "a stage binds two image table maps");
                    table.map = binding.guestDescriptor;
                    allocations.push_back({0, size, false, std::make_unique<Buffer>(context, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT), ShaderRecompiler::DescriptorRole::ImageTableMap});
                    table.mapAllocation = allocations.size() - 1u;
                    bindings.push_back({{binding.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flags, nullptr}, {allocations.size() - 1u}});
                    continue;
                }
                const bool addressRole = binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable || binding.role == ShaderRecompiler::DescriptorRole::FaultBuffer;
                const bool bufferRole = addressRole || binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers || binding.role == ShaderRecompiler::DescriptorRole::ShaderData || binding.role == ShaderRecompiler::DescriptorRole::FlattenedSrt || binding.role == ShaderRecompiler::DescriptorRole::Gds;
                const bool imageRole = binding.role == ShaderRecompiler::DescriptorRole::GuestImages || binding.role == ShaderRecompiler::DescriptorRole::GuestSamplers;
                if (imageRole) {
                    if (binding.kind == ShaderRecompiler::DescriptorKind::SampledImage || binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) stageResources += binding.count;
                    requireStageLimits();
                    addImageBinding(binding, flags);
                    continue;
                }
                if (!bufferRole) Require(false, std::string("unsupported descriptor role ") + roleName(binding.role));
                if (binding.kind != ShaderRecompiler::DescriptorKind::StorageBuffer) Require(false, std::string("unsupported descriptor kind ") + kindName(binding.kind) + " for role " + roleName(binding.role) + ": only StorageBuffer is supported");
                Require(!binding.readOnly, "read-only descriptors are unsupported because the recompiler emits no NonWritable decoration");
                Require(binding.count != 0, "empty descriptor binding");
                stageStorageBuffers += binding.count;
                stageResources += binding.count;
                storageBuffers += binding.count;
                requireStageLimits();
                Binding item{{binding.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, binding.count, flags, nullptr}, {}};
                if (binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
                    Require(binding.guestDescriptor.size() == static_cast<std::uint64_t>(binding.count) * 4, "guest buffer descriptor must contain four DWORDs per array element");
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        // An element the recompiler did not classify (a producer without the
                        // vector) counts as written, like imageWritten below.
                        const bool written = element >= binding.Flags().bufferWritten.size() || binding.Flags().bufferWritten[element];
                        const bool atomic = element < binding.Flags().bufferAtomic.size() && binding.Flags().bufferAtomic[element];
                        const bool read = element < binding.Flags().bufferRead.size() && binding.Flags().bufferRead[element];
                        const std::uint32_t extent = element < binding.bufferExtent.size() ? binding.bufferExtent[element] : 0u;
                        const auto index = addGuestBuffer(std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 4, 4), target, indexAddress, indexBytes, written, atomic, read, extent);
                        const bool classified = element < binding.Flags().bufferWritten.size() && element < binding.Flags().bufferRead.size() && element < binding.Flags().bufferAtomic.size();
                        allocations[index].writeOnly = classified && allocations[index].guest && allocations[index].written && binding.Flags().bufferWritten[element] && !read && !atomic;
                        const auto& push = shader.program->pushConstants;
                        const auto pushDataBytes = shader.program->stencilReferences.empty() ? push.size() : shader.program->stencilReferenceDword * sizeof(std::uint32_t);
                        if (pushDataBytes != 0u) {
                            const auto position = shader.program->memoryOffsetDword * 4u + element;
                            Require(position < pushDataBytes, "guest buffer offset lies outside the shader's push constants");
                            allocations[index].pushByte = static_cast<std::int32_t>(shader.pushConstantOffset + position);
                        } else {
                            allocations[index].dataByte = shader.program->memoryOffsetDword * 4u + element;
                            offsetsInData.push_back(index);
                        }
                        item.allocations.push_back(index);
                    }
                } else if (addressRole) {
                    item.allocations.push_back(allocations.size());
                    allocations.push_back({0, 0, false, nullptr, binding.role});
                } else if (binding.role == ShaderRecompiler::DescriptorRole::Gds) {
                    Require(binding.count == 1 && binding.guestDescriptor.empty(), "invalid GDS descriptor contract");
                    const auto gds = Pm4::GdsAddress();
                    guestMemory.AddWritable(gds, Pm4::GdsBytes, true);
                    item.allocations.push_back(allocations.size());
                    allocations.push_back({gds, Pm4::GdsBytes, true, nullptr, ShaderRecompiler::DescriptorRole::ShaderData, true});
                } else {
                    Require(binding.count == 1, "shader data and flattened SRT descriptors must not be arrays");
                    Require(!binding.guestDescriptor.empty(), "empty shader data descriptor");
                    if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData) {
                        ValidateRuntimeResources(shader, binding.guestDescriptor);
                    }
                    item.allocations.push_back(addDataBuffer(binding.guestDescriptor));
                    if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData) shaderData = static_cast<std::int64_t>(item.allocations.back());
                }
                bindings.push_back(std::move(item));
            }
            for (const auto index : offsetsInData) allocations[index].dataAllocation = shaderData;
            for (auto index = firstDeferred; index < deferredImages.size(); ++index) {
                deferredImages[index].firstSampler = firstSampler;
                deferredImages[index].samplerCount = samplers.size() - firstSampler;
            }
            for (auto& table : tableStages) {
                if (table.program == shader.program && table.images != nullptr) table.samplerCount = samplers.size() - table.firstSampler;
            }
        }
        splitSets = SplitsSets(context, shaders);
        std::array<VkShaderStageFlags, 2> tableFlags{};
        std::array<VkShaderStageFlags, 2> storageTableFlags{};
        for (auto& table : tableStages) {
            Require(table.mapAllocation != std::numeric_limits<std::size_t>::max(), "an image table stage has no map");
            const auto& map = table.map;
            Require(map.size() >= ShaderRecompiler::ImageTableAbi::HeaderWords, "image table map is truncated");
            const auto elements = table.images != nullptr ? table.images->count : 0u;
            Require(map[ShaderRecompiler::ImageTableAbi::HeaderElementCount] == elements, "image table map disagrees with its image binding");
            const auto faults = map[ShaderRecompiler::ImageTableAbi::HeaderElementFaults];
            const auto samplerFaults = map[ShaderRecompiler::ImageTableAbi::HeaderSamplerFaults];
            Require(static_cast<std::uint64_t>(faults) + elements <= map.size() && static_cast<std::uint64_t>(samplerFaults) + map[ShaderRecompiler::ImageTableAbi::HeaderSamplerCount] / 4u <= map.size(), "image table map is truncated");
            if (table.images == nullptr) continue;
            table.set = splitSets && table.stage == ShaderRecompiler::ShaderStage::Fragment ? 1u : 0u;
            table.elementBase = tableElements[table.set];
            table.map[ShaderRecompiler::ImageTableAbi::HeaderElementBase] = table.elementBase;
            tableElements[table.set] += elements;
            if (table.sampled) tableFlags[table.set] |= VulkanStage(table.stage);
            if (table.storage) storageTableFlags[table.set] |= VulkanStage(table.stage);
        }
        for (std::uint32_t set = 0; set < storageTableFlags.size(); ++set) {
            if (storageTableFlags[set] == 0) continue;
            bindings.push_back({{ShaderRecompiler::RuntimeAbi::ImageStorageTableBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, StorageTableCapacity(context), storageTableFlags[set], nullptr}, {}, {}, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, static_cast<std::int32_t>(set)});
            storageTableBindings[set] = bindings.size() - 1u;
        }
        for (std::uint32_t set = 0; set < tableFlags.size(); ++set) {
            if (tableFlags[set] == 0) continue;
            Require(context.imageTableCapacity != 0, "image tables need runtimeDescriptorArray, descriptorBindingPartiallyBound and descriptorBindingVariableDescriptorCount, which the device lacks");
            const VkDescriptorBindingFlags imageTableFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT | (context.imageTableUpdateAfterBind ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT : 0);
            bindings.push_back({{ShaderRecompiler::RuntimeAbi::ImageTableBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, context.imageTableCapacity, tableFlags[set], nullptr}, {}, {}, imageTableFlags, static_cast<std::int32_t>(set)});
            tableBindings[set] = bindings.size() - 1u;
        }
        if (storageBuffers > context.limits.maxDescriptorSetStorageBuffers) {
            Require(storageBuffers <= context.descriptorIndexingLimits.maxDescriptorSetUpdateAfterBindStorageBuffers, "pipeline descriptors exceed device limits");
            updateAfterBind = true;
        }
        timing.bindingsMs = phase(BuildPhase::Bindings);
        // For every build, locked ones included: their stage B then takes the fast path too, and the
        // collects cost the same wherever they run.
        if (precollectImages()) phase(BuildPhase::Precollect);
        if (drawBuild && !usesBda && std::all_of(allocations.begin(), allocations.end(), [&](const Allocation& allocation) { return !allocation.guest || allocation.pushByte >= 0 || (allocation.dataAllocation >= 0 && allocation.dataByte < allocations[static_cast<std::size_t>(allocation.dataAllocation)].size); })) guestMemory.AllowAdjustedRegions();
        requireGuestBuffers();
        guestMemory.UploadPrepare(usesBda);
        timing.uploadMs = phase(BuildPhase::Upload);
        splitSets = SplitsSets(context, shaders);
        std::array<std::vector<VkDescriptorSetLayoutBinding>, 2> descriptions;
        bool tablesUpdateAfterBind = false;
        for (const auto& binding : bindings) {
            const auto set = setOf(binding);
            descriptions[set].push_back(binding.layout);
            (set == 0 ? layoutBindingFlags : fragmentBindingFlags).push_back(binding.flags | (updateAfterBind && binding.layout.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT : 0));
            tablesUpdateAfterBind |= (binding.flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) != 0;
            auto& key = set == 0 ? layoutKey : fragmentLayoutKey;
            key.insert(key.end(), {binding.layout.binding, static_cast<std::uint32_t>(binding.layout.descriptorType), binding.layout.descriptorCount, binding.layout.stageFlags});
        }
        updateAfterBind = updateAfterBind || tablesUpdateAfterBind;
        if (updateAfterBind) {
            layoutKey.push_back(VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);
            if (splitSets) fragmentLayoutKey.push_back(VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);
        }
        static const bool noLayoutCache = std::getenv("APS5_NO_LAYOUT_CACHE") != nullptr;
        static const bool noPoolCache = std::getenv("APS5_NO_POOL_CACHE") != nullptr;
        const auto tableDescriptorCount = [&](std::size_t set) { return std::max(1u, std::min(tableElements[set], context.imageTableCapacity)); };
        const auto tableVariableCount = [&](std::size_t set) { return tableBindings[set] != std::numeric_limits<std::size_t>::max() ? std::optional<std::uint32_t>(tableDescriptorCount(set)) : std::nullopt; };
        const auto splitPoolSizes = [&](std::size_t set) {
            std::map<VkDescriptorType, std::uint32_t> counts;
            for (std::size_t index = 0; index < bindings.size(); ++index) {
                if (setOf(bindings[index]) != set) continue;
                counts[bindings[index].layout.descriptorType] += index == tableBindings[set] ? tableDescriptorCount(set) : bindings[index].layout.descriptorCount;
            }
            std::vector<VkDescriptorPoolSize> sizes;
            for (const auto& [type, count] : counts) sizes.push_back({type, count});
            return sizes;
        };
        const auto allocateVariableSet = [&](VkDescriptorPool from, VkDescriptorSetLayout layout, std::optional<std::uint32_t> variableCount) {
            const std::uint32_t variable = variableCount.value_or(0u);
            VkDescriptorSetVariableDescriptorCountAllocateInfoEXT variableInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT};
            variableInfo.descriptorSetCount = 1;
            variableInfo.pDescriptorCounts = &variable;
            VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocation.pNext = variableCount.has_value() ? &variableInfo : nullptr;
            allocation.descriptorPool = from;
            allocation.descriptorSetCount = 1;
            allocation.pSetLayouts = &layout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocation, &set), "vkAllocateDescriptorSets");
            return set;
        };
        const auto createLayout = [&](const std::vector<std::uint32_t>& key, const std::vector<VkDescriptorSetLayoutBinding>& description, const std::vector<VkDescriptorBindingFlags>& flags, bool& owns) {
            if (context.descriptorCache != nullptr && !noLayoutCache) return context.descriptorCache->Layout(key, description, updateAfterBind, flags);
            ValidateDescriptorLayout(context, description, updateAfterBind);
            owns = true;
            return CreateSetLayout(context, description, updateAfterBind, flags);
        };
        _layout = createLayout(layoutKey, descriptions[0], layoutBindingFlags, ownsLayout);
        if (splitSets) {
            fragment.layout = createLayout(fragmentLayoutKey, descriptions[1], fragmentBindingFlags, fragment.ownsLayout);
            {
                const auto sizes = splitPoolSizes(1);
                const auto variableCount = tableVariableCount(1);
                if (context.descriptorCache != nullptr && !noPoolCache) {
                    fragment.cacheAllocation = context.descriptorCache->Allocate(fragment.layout, sizes, updateAfterBind, variableCount);
                    fragment.set = fragment.cacheAllocation.set;
                }
                if (fragment.set == VK_NULL_HANDLE) {
                    fragment.pool = CreateSetPool(context, sizes, updateAfterBind);
                    fragment.set = allocateVariableSet(fragment.pool, fragment.layout, variableCount);
                }
            }
        }
        if (splitSets) {
            const auto sizes = splitPoolSizes(0);
            const auto variableCount = tableVariableCount(0);
            if (context.descriptorCache != nullptr && !noPoolCache) {
                cacheAllocation = context.descriptorCache->Allocate(_layout, sizes, updateAfterBind, variableCount);
                _set = cacheAllocation.set;
            }
            if (_set == VK_NULL_HANDLE) {
                pool = CreateSetPool(context, sizes, updateAfterBind);
                _set = allocateVariableSet(pool, _layout, variableCount);
            }
        }
        if (!splitSets && !bindings.empty()) {
            // The set is sized from the plan: every image element becomes exactly one descriptor
            // when stage B looks it up.
            std::vector<VkDescriptorPoolSize> sizes;
            if (storageBuffers != 0) sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<std::uint32_t>(storageBuffers)});
            const auto tableDescriptors = tableBindings[0] != std::numeric_limits<std::size_t>::max() ? tableDescriptorCount(0) : 0u;
            if (plannedSampledImages + tableDescriptors != 0) sizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, plannedSampledImages + tableDescriptors});
            const auto storageTableDescriptors = storageTableBindings[0] != std::numeric_limits<std::size_t>::max() ? bindings[storageTableBindings[0]].layout.descriptorCount : 0u;
            if (plannedStorageImages + storageTableDescriptors != 0) sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, plannedStorageImages + storageTableDescriptors});
            std::uint32_t samplerDescriptors = 0;
            for (const auto& binding : bindings) {
                if (binding.layout.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) samplerDescriptors += binding.layout.descriptorCount;
            }
            if (samplerDescriptors != 0) sizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLER, samplerDescriptors});
            const auto variableCount = tableVariableCount(0);
            if (context.descriptorCache != nullptr && !noPoolCache) {
                const auto allocated = context.descriptorCache->Allocate(_layout, sizes, updateAfterBind, variableCount);
                _set = allocated.set;
                cacheAllocation = allocated;
            }
            if (_set == VK_NULL_HANDLE) {
                pool = CreateSetPool(context, sizes, updateAfterBind);
                const std::uint32_t variable = variableCount.value_or(0u);
                VkDescriptorSetVariableDescriptorCountAllocateInfoEXT variableInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT};
                variableInfo.descriptorSetCount = 1;
                variableInfo.pDescriptorCounts = &variable;
                VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                allocation.pNext = variableCount.has_value() ? &variableInfo : nullptr;
                allocation.descriptorPool = pool;
                allocation.descriptorSetCount = 1;
                allocation.pSetLayouts = &_layout;
                Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocation, &_set), "vkAllocateDescriptorSets");
            }
        }
        writeOnlyRanges.clear();
        for (const auto& item : allocations) {
            if (!item.writeOnly || item.size == 0) continue;
            const auto begin = item.address;
            const auto end = item.address + item.size;
            const bool shared = std::any_of(allocations.begin(), allocations.end(), [&](const Allocation& other) { return !other.writeOnly && other.size != 0 && other.address < end && begin < other.address + other.size; });
            if (!shared) writeOnlyRanges.emplace_back(begin, end);
        }
        timing.descriptorsMs = phase(BuildPhase::Descriptors);
        if (BuildProfiled() || AgcDriver::DispatchBuildTrace::Enabled()) {
            timing.prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stageStart).count();
            if (BuildProfiled()) addBuildPhase(BuildPhase::StageA, timing.prepareMs);
        }
    } catch (...) {
        release();
        throw;
    }
}

bool SplitsSets(const Context& context, std::span<const CompiledShader> shaders) {
    const auto fragmentSet = [](const CompiledShader& shader) { return shader.program != nullptr && std::any_of(shader.program->bindings.begin(), shader.program->bindings.end(), [](const auto& binding) { return binding.descriptorSet == ShaderRecompiler::RuntimeAbi::FragmentDescriptorSet; }); };
    const auto fragmentStage = [](const CompiledShader& shader) { return shader.stage == ShaderRecompiler::ShaderStage::Fragment; };
    return std::any_of(shaders.begin(), shaders.end(), fragmentSet) || (context.graphicsPipelineLibrary && std::any_of(shaders.begin(), shaders.end(), fragmentStage));
}

DescribedSetLayouts DescribeSetLayouts(const Context& context, std::span<const CompiledShader> shaders) {
    Require(context.descriptorCache != nullptr, "described set layouts need the descriptor cache");
    DescribedSetLayouts result;
    std::array<std::vector<VkDescriptorSetLayoutBinding>, 2> descriptions;
    const bool split = SplitsSets(context, shaders);
    std::array<std::vector<VkDescriptorBindingFlags>, 2> bindingFlags;
    std::array<VkShaderStageFlags, 2> tableFlags{};
    std::array<VkShaderStageFlags, 2> storageTableFlags{};
    bool updateAfterBind = false;
    bool tablesUpdateAfterBind = false;
    std::uint64_t storageBuffers = 0;
    for (const auto& shader : shaders) {
        Require(shader.program != nullptr, "missing compiled shader");
        const VkShaderStageFlags flags = VulkanStage(shader.stage);
        std::uint64_t stageStorageBuffers = 0;
        std::uint64_t stageResources = 0;
        for (const auto& binding : shader.program->bindings) {
            const auto set = binding.descriptorSet == ShaderRecompiler::RuntimeAbi::FragmentDescriptorSet ? 1 : 0;
            if (binding.role == ShaderRecompiler::DescriptorRole::ImageTable) {
                tableFlags[split && shader.stage == ShaderRecompiler::ShaderStage::Fragment ? 1 : 0] |= flags;
                continue;
            }
            if (binding.role == ShaderRecompiler::DescriptorRole::ImageStorageTable) {
                storageTableFlags[split && shader.stage == ShaderRecompiler::ShaderStage::Fragment ? 1 : 0] |= flags;
                continue;
            }
            if (binding.role == ShaderRecompiler::DescriptorRole::SamplerTable) {
                descriptions[set].push_back({binding.binding, VK_DESCRIPTOR_TYPE_SAMPLER, context.samplerTableCapacity, flags, nullptr});
                bindingFlags[set].push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | (context.samplerTableUpdateAfterBind ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT : 0));
                tablesUpdateAfterBind |= context.samplerTableUpdateAfterBind;
                continue;
            }
            const bool image = binding.role == ShaderRecompiler::DescriptorRole::GuestImages || binding.role == ShaderRecompiler::DescriptorRole::GuestSamplers;
            const auto type = !image ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : binding.kind == ShaderRecompiler::DescriptorKind::StorageImage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : binding.kind == ShaderRecompiler::DescriptorKind::SampledImage ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
            if (type != VK_DESCRIPTOR_TYPE_SAMPLER) stageResources += binding.count;
            if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                stageStorageBuffers += binding.count;
                storageBuffers += binding.count;
            }
            if (stageStorageBuffers > context.limits.maxPerStageDescriptorStorageBuffers || stageResources > context.limits.maxPerStageResources) updateAfterBind = true;
            descriptions[set].push_back({binding.binding, type, binding.count, flags, nullptr});
            bindingFlags[set].push_back(0);
        }
    }
    for (std::size_t set = 0; set < storageTableFlags.size(); ++set) {
        if (storageTableFlags[set] == 0) continue;
        descriptions[set].push_back({ShaderRecompiler::RuntimeAbi::ImageStorageTableBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, StorageTableCapacity(context), storageTableFlags[set], nullptr});
        bindingFlags[set].push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
    }
    for (std::size_t set = 0; set < tableFlags.size(); ++set) {
        if (tableFlags[set] == 0) continue;
        descriptions[set].push_back({ShaderRecompiler::RuntimeAbi::ImageTableBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, context.imageTableCapacity, tableFlags[set], nullptr});
        bindingFlags[set].push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT | (context.imageTableUpdateAfterBind ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT : 0));
        tablesUpdateAfterBind |= context.imageTableUpdateAfterBind;
    }
    if (storageBuffers > context.limits.maxDescriptorSetStorageBuffers) updateAfterBind = true;
    for (std::size_t set = 0; set < descriptions.size(); ++set) {
        for (std::size_t index = 0; index < descriptions[set].size(); ++index) {
            if (updateAfterBind && descriptions[set][index].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) bindingFlags[set][index] |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        }
    }
    updateAfterBind = updateAfterBind || tablesUpdateAfterBind;
    for (std::size_t set = 0; set < descriptions.size(); ++set) {
        auto& key = set == 0 ? result.mainKey : result.fragmentKey;
        for (const auto& binding : descriptions[set]) key.insert(key.end(), {binding.binding, static_cast<std::uint32_t>(binding.descriptorType), binding.descriptorCount, binding.stageFlags});
        if (updateAfterBind && (set == 0 || split)) key.push_back(VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);
    }
    result.layouts.split = split;
    result.layouts.main = context.descriptorCache->Layout(result.mainKey, descriptions[0], updateAfterBind, bindingFlags[0]);
    if (split) result.layouts.fragment = context.descriptorCache->Layout(result.fragmentKey, descriptions[1], updateAfterBind, bindingFlags[1]);
    return result;
}

// APS5_ARCHB_FIX_DRAW_SET_POOL. The sets of a draw of this object are a copy of the object's own set
// (descriptorWrites, written in full and fixed since buildComplete) with the snapshot elements
// pointed at the draw's snapshots. A set back from its completed batch still holds that copy, so the
// next draw only rewrites the elements either draw points at a snapshot (each other element holds
// the object's own descriptor, as in the full rewrite). Nothing else writes a pooled set: it left
// the descriptor cache with the draw that wrote it in full and returns to it only when the pool goes.
struct ShaderResources::DrawSetPool {
    struct Entry {
        DescriptorCache::SetAllocation allocation;
        DescriptorCache::SetAllocation fragmentAllocation;
        std::vector<std::size_t> overridden;
        // APS5_ARCHB_PIPE_DRAW_REUSE: the set's held descriptors and snapshot slots (see DrawBindings).
        std::vector<DrawBindings::Held> held;
        std::shared_ptr<Buffer> slotBuffer;
        std::vector<std::size_t> slotElements;
        std::vector<VkDeviceSize> slotOffsets;
        std::vector<std::size_t> slotSizes;
        std::uint64_t serial = 0;
    };
    static constexpr std::size_t Capacity = 64;
    DescriptorCache* cache = nullptr;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout fragmentLayout = VK_NULL_HANDLE;
    AgcDriver::Mutex mutex;
    std::vector<Entry> entries;

    DrawSetPool(DescriptorCache* cache, VkDescriptorSetLayout layout, VkDescriptorSetLayout fragmentLayout) : cache(cache), layout(layout), fragmentLayout(fragmentLayout) {}
    DrawSetPool(const DrawSetPool&) = delete;
    DrawSetPool& operator=(const DrawSetPool&) = delete;
    ~DrawSetPool() {
        for (const auto& entry : entries) {
            if (entry.allocation.set != VK_NULL_HANDLE) cache->Recycle(layout, entry.allocation);
            if (entry.fragmentAllocation.set != VK_NULL_HANDLE) cache->Recycle(fragmentLayout, entry.fragmentAllocation);
        }
    }
    bool Take(Entry& taken) {
        std::lock_guard lock(mutex);
        if (entries.empty()) return false;
        taken = std::move(entries.back());
        entries.pop_back();
        return true;
    }
    // APS5_ARCHB_PIPE_DRAW_REUSE: among the newest MatchScan (4) sets, the one that saves the most descriptor
    // writes: slots holding exactly these small elements and sizes count each of them (and one more, the
    // slot buffer not made anew), and each shared snapshot (`selected[k]` with `snapshots[k].buffer` set)
    // whose element already holds that buffer, offset and range counts one; ties go to the newest.
    static constexpr std::size_t MatchScan = 4;
    bool TakeMatching(Entry& taken, std::span<const std::size_t> slotElements, std::span<const std::size_t> sizes, std::span<const std::size_t> selected, std::span<const DrawBindings::Snapshot> snapshots, bool& matched) {
        std::lock_guard lock(mutex);
        matched = false;
        if (entries.empty()) return false;
        auto pick = entries.end() - 1;
        std::size_t best = 0;
        bool bestSlots = false;
        std::size_t scanned = 0;
        for (auto it = entries.end(); it != entries.begin() && scanned < MatchScan; ++scanned) {
            --it;
            const bool slots = (it->slotBuffer != nullptr || slotElements.empty()) && std::ranges::equal(it->slotElements, slotElements) && std::ranges::equal(it->slotSizes, sizes);
            std::size_t score = slots ? slotElements.size() + 1 : 0;
            if (it->held.size() == it->overridden.size()) {
                for (std::size_t k = 0; k < selected.size(); ++k) {
                    const auto& snapshot = snapshots[k];
                    if (snapshot.buffer == nullptr) continue;
                    const auto position = std::find(it->overridden.begin(), it->overridden.end(), selected[k]);
                    if (position == it->overridden.end()) continue;
                    const auto& held = it->held[static_cast<std::size_t>(position - it->overridden.begin())];
                    if (held.offset == snapshot.offset && held.range == snapshot.bytes && !held.buffer.owner_before(snapshot.buffer) && !snapshot.buffer.owner_before(held.buffer)) ++score;
                }
            }
            if (score > best) {
                best = score;
                pick = it;
                bestSlots = slots;
            }
        }
        matched = bestSlots && best != 0;
        if (best == 0) {
            // No set saves anything: the newest, as Take; it may still hold matching (empty) slots.
            pick = entries.end() - 1;
            matched = (pick->slotBuffer != nullptr || slotElements.empty()) && std::ranges::equal(pick->slotElements, slotElements) && std::ranges::equal(pick->slotSizes, sizes);
        }
        taken = std::move(*pick);
        entries.erase(pick);
        return true;
    }
    // False when full (or out of memory): the caller releases the sets as before.
    bool Give(Entry&& entry) noexcept {
        std::lock_guard lock(mutex);
        if (entries.size() >= Capacity) return false;
        try {
            entries.push_back(std::move(entry));
        } catch (...) {
            return false;
        }
        return true;
    }
};

void ShaderResources::buildComplete() {
    PerformanceTimer frameTiming("Resources.Complete");
    BuildLookups lookups;
    AgcDriver::ArchB::Recorder::Count(drawBuild ? "resources_complete_draw" : usesBda ? "resources_complete_dispatch_address_table" : usesFaultBuffer ? "resources_complete_dispatch_fault_only" : "resources_complete_dispatch_plain");
    if (!GuestMemory::IsCommitterThread()) AgcDriver::ArchB::Recorder::Count(GuestMemory::GpuMutex().HeldByThisThread() ? "resources_complete_other_locked" : "resources_complete_other_unlocked");
    Require(!completed, "cannot update a published descriptor heap");
    Require(registeredImageViewsCurrent(registeredImageViews), "AGC graphics: registered image range changed after resource preparation");
    const auto stageStart = std::chrono::steady_clock::now();
    phaseStart = stageStart;
    try {
        // The lookups run in plan order: Revalidate walks the bindings the same way, and consecutive
        // storage elements of one mip chain share the previous element's image.
        previousSampled = {};
        for (auto& source : samplerSources) {
            source.singleLevelImage = source.mipmappedImage = false;
            source.pinnedLod.reset();
        }
        for (const auto& deferred : deferredImages) resolveImageBinding(*deferred.binding, bindings[deferred.index], std::span<const std::shared_ptr<Sampler>>(samplers).subspan(deferred.firstSampler, deferred.samplerCount));
        previousSampled = {};
        deferredImages.clear();
        applySamplerVariants();
        // The records served their purpose: each holds the cache entry's objects as of stage A,
        // which would otherwise keep a replaced texture or an evicted storage image (and its device
        // memory) alive, outside the caches' budgets, for as long as this object is cached.
        imageRecords.clear();
        imageRecords.shrink_to_fit();
        nextImageRecord = 0;
        Require(textures.size() == plannedSampledImages && storageTextures.size() == plannedStorageImages, "image lookups disagree with the descriptor plan");
        resolveImageTables();
        Require(registeredImageViewsCurrent(registeredImageViews), "AGC graphics: registered image range changed during resource materialization");
        const auto imagesMs = phase(BuildPhase::Images);
        timing.imagesMs += imagesMs;
        timing.bindingsMs += imagesMs;
        requireGuestBuffers();
        guestMemory.UploadFinish(usesBda, htileClear);
        timing.uploadMs += phase(BuildPhase::Upload);
        if (usesBda) bda = std::make_unique<BdaResources>(context, guestMemory, faultReportOnly);
        else if (usesFaultBuffer) bda = std::make_unique<BdaResources>(context, faultReportOnly);
        if (bda) bda->SetSrtGuards(std::move(srtGuardFaults));
        finishImageTables();
        timing.bdaMs += phase(BuildPhase::Bda);
        if (_set != VK_NULL_HANDLE || fragment.set != VK_NULL_HANDLE) {
            // One update call for the whole set: the info arrays are sized up front so every write's
            // pointer into them stays valid until the call.
            std::size_t bufferCount = 0;
            std::size_t imageCount = 0;
            for (const auto& binding : bindings) {
                bufferCount += binding.allocations.size();
                if (binding.layout.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) imageCount += binding.flags != 0 ? binding.imageAllocations.size() : binding.layout.descriptorCount;
            }
            auto& buffers = descriptorBuffers;
            auto& images = descriptorImages;
            buffers.reserve(bufferCount);
            images.reserve(imageCount);
            auto& writes = descriptorWrites;
            writes.reserve(bindings.size());
            std::vector<std::pair<std::size_t, std::size_t>> keptWrites;
            const auto absentImage = [this](std::uint32_t binding) {
                constexpr auto bindingCount = static_cast<std::uint32_t>(ShaderRecompiler::RuntimeAbi::Binding::Count);
                const auto view = context.nullDescriptors ? VK_NULL_HANDLE : context.placeholderImages->View(binding % bindingCount);
                return VkDescriptorImageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            };
            for (const auto& binding : bindings) {
                if (binding.flags != 0) {
                    const auto set = setOf(binding);
                    const bool sampled = binding.layout.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    const auto& allocation = set == 0 ? cacheAllocation : fragment.cacheAllocation;
                    const bool recorded = sampled && set < tableWrites.size() && allocation.layout != VK_NULL_HANDLE;
                    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
                    const auto firstWrite = writes.size();
                    const auto firstImage = images.size();
                    for (std::size_t first = 0; first < binding.imageAllocations.size();) {
                        if (binding.imageAllocations[first] == std::numeric_limits<std::size_t>::max()) {
                            ++first;
                            continue;
                        }
                        auto last = first;
                        while (last < binding.imageAllocations.size() && binding.imageAllocations[last] != std::numeric_limits<std::size_t>::max()) ++last;
                        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                        write.dstSet = set == 0 ? _set : fragment.set;
                        write.dstBinding = binding.layout.binding;
                        write.dstArrayElement = static_cast<std::uint32_t>(first);
                        write.descriptorCount = static_cast<std::uint32_t>(last - first);
                        write.descriptorType = binding.layout.descriptorType;
                        write.pImageInfo = images.data() + images.size();
                        for (auto element = first; element < last; ++element) {
                            const auto index = binding.imageAllocations[element];
                            if (binding.layout.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) images.push_back({samplers[index]->Handle(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                            else if (binding.layout.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) images.push_back({VK_NULL_HANDLE, storageAtomic64[index] ? storageTextures[index]->Atomic64View(storageMips[index], storageFirstLayer[index]) : storageAtomic[index] ? storageTextures[index]->AtomicView(storageMips[index], storageFirstLayer[index]) : storageTextures[index]->StorageView(storageMips[index], storageFirstLayer[index], ShaderRecompiler::RuntimeAbi::UintStorageImageBinding(binding.layout.binding)), VK_IMAGE_LAYOUT_GENERAL});
                            else images.push_back({VK_NULL_HANDLE, textureFirstLayer[index] ? textures[index]->FirstLayerView() : textures[index]->View(), textures[index]->Layout()});
                        }
                        if (recorded) runs.emplace_back(write.dstArrayElement, write.descriptorCount);
                        writes.push_back(write);
                        first = last;
                    }
                    if (!recorded) continue;
                    const auto& held = allocation.table;
                    auto& counters = TextureCounts();
                    const bool heldAlive = held != nullptr && held->textures != nullptr && std::none_of(held->textures->begin(), held->textures->end(), [](const std::weak_ptr<Texture>& texture) { return texture.expired(); });
                    if (heldAlive && held->binding == binding.layout.binding && held->runs == runs && std::equal(held->images.begin(), held->images.end(), images.begin() + static_cast<std::ptrdiff_t>(firstImage), images.end(), [](const VkDescriptorImageInfo& a, const VkDescriptorImageInfo& b) { return a.sampler == b.sampler && a.imageView == b.imageView && a.imageLayout == b.imageLayout; })) {
                        keptWrites.emplace_back(firstWrite, writes.size());
                        counters.tableWritesKept.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        counters.tableWritesMade.fetch_add(1, std::memory_order_relaxed);
                    }
                    tableWrites[set] = {true, false, binding.layout.binding, firstImage, images.size() - firstImage, std::move(runs)};
                    continue;
                }
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = setOf(binding) == 0 ? _set : fragment.set;
                write.dstBinding = binding.layout.binding;
                write.descriptorCount = binding.layout.descriptorCount;
                write.descriptorType = binding.layout.descriptorType;
                switch (binding.layout.descriptorType) {
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                        write.pBufferInfo = buffers.data() + buffers.size();
                        for (std::size_t element = 0; element < binding.allocations.size(); ++element) {
                            const auto index = binding.allocations[element];
                            if (allocations[index].role == ShaderRecompiler::DescriptorRole::FaultBuffer) faultDescriptors.push_back({writes.size(), buffers.size(), static_cast<std::uint32_t>(element)});
                            buffers.push_back(descriptor(allocations[index]));
                        }
                        break;
                    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                        write.pImageInfo = images.data() + images.size();
                        for (const auto index : binding.imageAllocations) {
                            if (index == std::numeric_limits<std::size_t>::max()) images.push_back(absentImage(binding.layout.binding));
                            else images.push_back({VK_NULL_HANDLE, textureSrgb[index] ? textures[index]->SrgbView(textureFirstLayer[index], textureBaseLevel[index]) : textureBaseLevel[index] ? textures[index]->BaseLevelView() : textureFirstLayer[index] ? textures[index]->FirstLayerView() : textures[index]->View(), textures[index]->Layout()});
                        }
                        Require(binding.imageAllocations.size() == binding.layout.descriptorCount, "descriptor allocations disagree with compact binding");
                        break;
                    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                        write.pImageInfo = images.data() + images.size();
                        for (const auto index : binding.imageAllocations) {
                            if (index == std::numeric_limits<std::size_t>::max()) images.push_back(absentImage(binding.layout.binding));
                            else if (storageTextures[index] == nullptr) images.push_back({VK_NULL_HANDLE, multisampleStorageViews[index], VK_IMAGE_LAYOUT_GENERAL});
                            else images.push_back({VK_NULL_HANDLE, storageAtomic64[index] ? storageTextures[index]->Atomic64View(storageMips[index], storageFirstLayer[index]) : storageAtomic[index] ? storageTextures[index]->AtomicView(storageMips[index], storageFirstLayer[index]) : storageTextures[index]->StorageView(storageMips[index], storageFirstLayer[index], ShaderRecompiler::RuntimeAbi::UintStorageImageBinding(binding.layout.binding)), VK_IMAGE_LAYOUT_GENERAL});
                        }
                        Require(binding.imageAllocations.size() == binding.layout.descriptorCount, "descriptor allocations disagree with compact binding");
                        break;
                    case VK_DESCRIPTOR_TYPE_SAMPLER:
                        write.pImageInfo = images.data() + images.size();
                        for (const auto index : binding.imageAllocations) images.push_back({samplers[index]->Handle(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                        Require(binding.imageAllocations.size() == binding.layout.descriptorCount, "descriptor allocations disagree with compact binding");
                        break;
                    default: throw std::runtime_error("AGC graphics: ShaderResources encountered an unknown descriptor type while writing the descriptor set");
                }
                writes.push_back(write);
            }
            if (keptWrites.empty()) {
                context.Resolved(&DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
            } else {
                std::vector<VkWriteDescriptorSet> made;
                made.reserve(writes.size());
                std::size_t next = 0;
                for (std::size_t index = 0; index < writes.size(); ++index) {
                    if (next < keptWrites.size() && index == keptWrites[next].first) {
                        index = keptWrites[next++].second - 1u;
                        continue;
                    }
                    made.push_back(writes[index]);
                }
                if (!made.empty()) context.Resolved(&DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(made.size()), made.data(), 0, nullptr);
            }
            for (auto& table : tableWrites) table.written = table.recorded;
            // The writes are final from here on (completed): what a pooled draw set keeps.
            if (!usesBda && context.descriptorCache != nullptr) drawSetPool = std::make_shared<DrawSetPool>(context.descriptorCache, _layout, fragment.layout);
        }
        for (const auto& allocation : allocations) {
            if (allocation.adjustment == 0) continue;
            if (allocation.pushByte >= 0) {
                pushPatches.emplace_back(static_cast<std::uint32_t>(allocation.pushByte), allocation.adjustment);
                continue;
            }
            Require(allocation.dataAllocation >= 0, "a guest buffer off the storage buffer offset alignment in a shader without shader data is not implemented");
            auto& data = allocations[static_cast<std::size_t>(allocation.dataAllocation)];
            Require(data.buffer != nullptr && allocation.dataByte < data.size, "guest buffer offset lies outside the shader's data buffer");
            data.buffer->Bytes()[allocation.dataByte] = static_cast<std::byte>(allocation.adjustment);
            dataPatches.push_back({static_cast<std::size_t>(allocation.dataAllocation), allocation.dataByte, allocation.adjustment});
        }
        timing.descriptorsMs += phase(BuildPhase::Descriptors);
        computeSnapshotCandidates();
        noteReusable();
        completed = true;
        if (BuildProfiled() || AgcDriver::DispatchBuildTrace::Enabled()) {
            timing.completeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stageStart).count();
            if (BuildProfiled()) {
                addBuildPhase(BuildPhase::StageB, timing.completeMs);
                reportDescriptorCaches();
            }
        }
    } catch (...) {
        release();
        throw;
    }
}

// Whether this build can serve later dispatches of the same content (see ContentKey): nothing to do
// once the GPU completed (no BDA, no copied written buffers, no lease) and every guest buffer bound
// in place through a host import (or staged in device memory from one, see
// GuestBufferMemory::DirectRegions) whose identity is recorded for Revalidate.
namespace {

// APS5_NO_TEMPLATE_DATA_REFRESH=1: compute keys keep the ShaderData/FlattenedSrt words and no
// template refreshes its data buffers (see ShaderResources::ContentKey).
bool TemplateDataRefresh() {
    static const bool enabled = std::getenv("APS5_NO_TEMPLATE_DATA_REFRESH") == nullptr;
    return enabled;
}

bool DataRole(ShaderRecompiler::DescriptorRole role) {
    return role == ShaderRecompiler::DescriptorRole::ShaderData || role == ShaderRecompiler::DescriptorRole::FlattenedSrt;
}

// The largest data buffer vkCmdUpdateBuffer refreshes; a template with a bigger one is not reused.
constexpr std::size_t MaxRefreshBytes = 65536;

// FNV-1a over one data buffer's words, its count first (DataWordsHash).
constexpr std::uint64_t FnvOffset = 14695981039346656037ull;
constexpr std::uint64_t FnvPrime = 1099511628211ull;
void mixDataWords(std::uint64_t& hash, std::span<const std::uint32_t> words) {
    hash = (hash ^ static_cast<std::uint64_t>(words.size())) * FnvPrime;
    for (const auto word : words) hash = (hash ^ word) * FnvPrime;
}

// ResourceCache::Find and Touch calls (the [rescache] line).
std::atomic<std::uint64_t> resourceCacheFinds{0};
std::atomic<std::uint64_t> resourceCacheTouches{0};
std::atomic<std::uint64_t> depthGuardRefusals{0};

}

namespace {

bool LoopGuardEnabled() {
    static const bool enabled = std::getenv("APS5_LOOP_GUARD") != nullptr;
    return enabled;
}

}

void ShaderResources::noteReusable() {
    // Taken whether or not the object turns out reusable: the lookups' records are this build's.
    captureValidation();
    reusable = false;
    reuseBlockReason = "not-recorded";
    importsReconcilable = false;
    spaceReuse = false;
    directRegions.clear();
    const bool spaceReusable = !htileClear.has_value() && (drawBuild ? !SpaceReuseDisabled() : allowDispatchSpaceReuse && DispatchSpaceReuseEnabled()) && usesBda && bdaReadOnly && guestMemory.SpaceOnly() && !guestMemory.HasCopiedWrites() && multisampleSources.empty() && !(bda != nullptr && LoopGuardEnabled());
    const auto blocked = [&](const char* reason) { reuseBlockReason = reason; };
    if (ArchB::Switches::Enabled(ArchB::Feature::DepthArrayGpu) && !depthArrayRanges.empty()) return blocked("depth-array-per-use-refresh");
    if (!spaceReusable) {
        if (htileClear.has_value()) return blocked("htile-clear");
        if (guestMemory.HasCopiedWrites()) return blocked("copied-writes");
        if (HoldsLease()) return blocked("holds-lease");
        if (usesBda) return blocked("uses-bda");
        if (!multisampleSources.empty()) return blocked("multisample-sources");
        if (bda != nullptr && LoopGuardEnabled()) return blocked("loop-guard");
    }
    if (std::any_of(textures.begin(), textures.end(), [](const std::shared_ptr<Texture>& texture) { return texture != nullptr && texture->RefreshedPerUse(); })) return blocked("texture-per-use-refresh");
    if (TemplateDataRefresh() && std::any_of(allocations.begin(), allocations.end(), [](const Allocation& allocation) { return allocation.buffer != nullptr && !allocation.guest && allocation.role != ShaderRecompiler::DescriptorRole::ImageTableMap && allocation.size > MaxRefreshBytes; })) return blocked("data-refresh-size");
    if (spaceReusable) {
        spaceReuse = true;
        reuseBlockReason = "none";
        reusable = true;
        return;
    }
    const auto regions = guestMemory.DirectRegions();
    if (!regions.has_value()) return blocked("direct-regions-unavailable");
    bool mirrored = false;
    for (const auto& [begin, end] : *regions) {
        // No reconcile here: the upload just took these imports, and the set is about to be recorded
        // against them.
        auto serial = HostImportSerial(context, begin, static_cast<std::size_t>(end - begin), false);
        // Read-only image mirrors (exe ranges) are as stable as imports; their serials have the top
        // bit set, so the two spaces never collide.
        const bool imported = serial != 0;
        if (serial == 0) serial = ImageMirrorSerial(context, begin, static_cast<std::size_t>(end - begin));
        if (serial == 0) return blocked("import-or-mirror-missing");
        directRegions.push_back({begin, end, serial});
        mirrored = mirrored || !imported;
    }
    bool bounded = false;
    for (const auto& cut : guestMemory.CutRegions()) {
        for (auto& region : directRegions) {
            if (region.begin != cut.begin) continue;
            region.regionEnd = cut.end;
            region.committedEnd = cut.committedEnd;
            bounded = true;
        }
    }
    importsReconcilable = !mirrored && !bounded;
    reuseBlockReason = "none";
    reusable = true;
}

std::uint64_t ShaderResources::DepthGuardRefusals() {
    return depthGuardRefusals.load(std::memory_order_relaxed);
}

bool ShaderResources::NeverReusable(std::span<const CompiledShader> shaders) {
    const bool loopGuard = LoopGuardEnabled();
    return std::any_of(shaders.begin(), shaders.end(), [loopGuard](const CompiledShader& shader) {
        return shader.program != nullptr && std::any_of(shader.program->bindings.begin(), shader.program->bindings.end(), [&](const ShaderRecompiler::DescriptorBinding& binding) { return (binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable && (SpaceReuseDisabled() || ShaderStoresThroughAddresses(shader.program->spirv))) || (loopGuard && binding.role == ShaderRecompiler::DescriptorRole::FaultBuffer); });
    });
}

bool ShaderResources::DispatchSpaceReuseEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_DISPATCH_SPACE_REUSE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

bool ShaderResources::SpaceReuseDisabled() {
    static const bool disabled = std::getenv("APS5_NO_ADDRESS_DRAW_REUSE") != nullptr;
    return disabled;
}

// APS5_PROFILE_DRAW: the per-device descriptor caches' counters, every 10 s.
void ShaderResources::reportDescriptorCaches() const {
    static AgcDriver::Mutex reportMutex;
    static auto lastReport = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(reportMutex);
    if (now - lastReport < std::chrono::seconds(10)) return;
    lastReport = now;
    const auto descriptors = context.descriptorCache != nullptr ? context.descriptorCache->Counters() : DescriptorCache::Stats{};
    const auto samplerHits = context.samplerCache != nullptr ? context.samplerCache->Hits() : 0;
    const auto samplerMisses = context.samplerCache != nullptr ? context.samplerCache->Misses() : 0;
    AgcDriver::ProfilePrint_nid_no_patch("[descriptors] layouts %llu hits / %llu created, sets %llu from %llu pools and %llu dedicated pools (live sets %llu, exhausted pools %llu, reopened %llu, recycled sets held %llu), samplers %llu hits / %llu created\n", static_cast<unsigned long long>(descriptors.layoutHits), static_cast<unsigned long long>(descriptors.layoutMisses), static_cast<unsigned long long>(descriptors.sets), static_cast<unsigned long long>(descriptors.pools), static_cast<unsigned long long>(descriptors.dedicatedPools), static_cast<unsigned long long>(descriptors.liveSets), static_cast<unsigned long long>(descriptors.exhaustedPools), static_cast<unsigned long long>(descriptors.reopenedPools), static_cast<unsigned long long>(descriptors.recycledSets), static_cast<unsigned long long>(samplerHits), static_cast<unsigned long long>(samplerMisses));
}

template<typename TVisitor>
void VisitContentKey(const CompiledShader& shader, bool dataWords, bool movableBuffers, const TVisitor& visit) {
    Require(shader.program != nullptr, "missing compiled shader");
    const auto& program = *shader.program;
    visit(dataWords ? 1u : 0u);
    visit(static_cast<std::uint32_t>(shader.stage));
    visit(static_cast<std::uint32_t>(program.PipelineVariantId()));
    visit(static_cast<std::uint32_t>(program.PipelineVariantId() >> 32u));
    visit(static_cast<std::uint32_t>(program.bindings.size()));
    const auto packBits = [&](const ShaderRecompiler::ElementFlags& bits) {
        visit(static_cast<std::uint32_t>(bits.size()));
        std::uint32_t word = 0;
        for (std::size_t i = 0; i < bits.size(); ++i) {
            if (bits[i]) word |= 1u << (i % 32u);
            if (i % 32u == 31u || i + 1 == bits.size()) {
                visit(word);
                word = 0;
            }
        }
    };
    for (const auto& binding : program.bindings) {
        for (const auto word : {static_cast<std::uint32_t>(binding.kind), static_cast<std::uint32_t>(binding.role), binding.descriptorSet, binding.binding, binding.count, binding.readOnly ? 1u : 0u, binding.imageShape.has_value() ? static_cast<std::uint32_t>(*binding.imageShape) + 1u : 0u, static_cast<std::uint32_t>(binding.guestDescriptor.size())}) visit(word);
        if (movableBuffers && binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers && binding.guestDescriptor.size() == static_cast<std::size_t>(binding.count) * 4u) {
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                const auto* words = binding.guestDescriptor.data() + static_cast<std::size_t>(element) * 4u;
                const bool written = element >= binding.Flags().bufferWritten.size() || binding.Flags().bufferWritten[element];
                const bool empty = words[2] == 0 || (words[0] == 0 && (words[1] & 0xffffu) == 0);
                if (written || empty) { for (std::uint32_t index = 0; index < 4u; ++index) visit(words[index]); }
                else { for (const auto word : {0u, words[1] & 0xffff0000u, 0u, words[3]}) visit(word); }
            }
        } else if ((dataWords && !movableBuffers) || !DataRole(binding.role)) {
            for (const auto word : binding.guestDescriptor) visit(word);
        }
        packBits(binding.Flags().imageWritten);
        packBits(binding.Flags().samplerDepthCompare);
        packBits(binding.Flags().imageDepthCompare);
        packBits(binding.Flags().imageAtomic);
        packBits(binding.Flags().samplerUnnormalized);
        packBits(binding.Flags().samplerIntegerBorder);
        packBits(binding.Flags().samplerCoordinatesIgnored);
        packBits(binding.Flags().imageUnnormalized);
        packBits(binding.Flags().imageUnnormalizedBaseLevel);
        // Read-only elements are bound without a write set: an object built for one written set
        // must not serve a build with another (the variant implies it, this makes it explicit).
        packBits(binding.Flags().bufferWritten);
        visit(static_cast<std::uint32_t>(binding.imageSamplers.size()));
        for (const auto sampler : binding.imageSamplers) {
            visit(static_cast<std::uint32_t>(sampler));
            visit(static_cast<std::uint32_t>(sampler >> 32u));
        }
        visit(static_cast<std::uint32_t>(binding.imageShapes.size()));
        for (const auto shape : binding.imageShapes) visit(static_cast<std::uint32_t>(shape));
    }
}

std::vector<std::uint32_t> ShaderResources::ContentKey(const CompiledShader& shader, bool dataWords, bool movableBuffers) {
    std::vector<std::uint32_t> key;
    AppendContentKey(shader, dataWords, movableBuffers, key);
    return key;
}

void ShaderResources::AppendContentKey(const CompiledShader& shader, bool dataWords, bool movableBuffers, std::vector<std::uint32_t>& key) {
    Require(shader.program != nullptr, "missing compiled shader");
    std::size_t words = key.size() + 5;
    const auto addWords = [&](std::size_t count) {
        if (count > key.max_size() - words) throw std::length_error("resource content key exceeds vector limits");
        words += count;
    };
    for (const auto& binding : shader.program->bindings) {
        addWords(20);
        addWords(binding.imageSamplers.size() * 2u);
        addWords(binding.imageShapes.size());
        if ((dataWords && !movableBuffers) || !DataRole(binding.role)) addWords(binding.guestDescriptor.size());
        const auto& flags = binding.Flags();
        for (const auto* bits : {&flags.imageWritten, &flags.samplerDepthCompare, &flags.imageDepthCompare, &flags.imageAtomic, &flags.samplerUnnormalized, &flags.samplerIntegerBorder, &flags.samplerCoordinatesIgnored, &flags.imageUnnormalized, &flags.imageUnnormalizedBaseLevel, &flags.bufferWritten}) addWords(bits->size() / 32u + (bits->size() % 32u != 0 ? 1u : 0u));
    }
    key.reserve(words);
    VisitContentKey(shader, dataWords, movableBuffers, [&](std::uint32_t word) { key.push_back(word); });
}

namespace {

using Component = ContentKeyComponent;

bool LabelShaderKey(std::span<const std::uint32_t> key, std::size_t at, std::size_t end, bool movable, std::vector<std::uint8_t>& labels) {
    const auto label = [&](std::size_t first, std::size_t count, Component component) {
        if (first + count > end) return false;
        std::fill(labels.begin() + static_cast<std::ptrdiff_t>(first), labels.begin() + static_cast<std::ptrdiff_t>(first + count), static_cast<std::uint8_t>(component));
        return true;
    };
    if (!label(at, 5, Component::Layout)) return false;
    const bool dataWords = key[at] != 0;
    const auto bindings = key[at + 4];
    at += 5;
    for (std::uint32_t binding = 0; binding < bindings; ++binding) {
        if (!label(at, 8, Component::Layout)) return false;
        const auto role = static_cast<ShaderRecompiler::DescriptorRole>(key[at + 1]);
        const auto size = key[at + 7];
        at += 8;
        const auto words = DataRole(role) && !(dataWords && !movable) ? 0u : size;
        if (at + words > end) return false;
        for (std::uint32_t i = 0; i < words; ++i) {
            auto component = Component::Other;
            if (role == ShaderRecompiler::DescriptorRole::GuestBuffers) component = static_cast<Component>(static_cast<std::size_t>(Component::BufferWord0) + i % 4u);
            else if (role == ShaderRecompiler::DescriptorRole::GuestImages || role == ShaderRecompiler::DescriptorRole::ImageTable) component = Component::Image;
            else if (role == ShaderRecompiler::DescriptorRole::GuestSamplers || role == ShaderRecompiler::DescriptorRole::SamplerTable) component = Component::Sampler;
            else if (DataRole(role)) component = Component::ShaderData;
            labels[at + i] = static_cast<std::uint8_t>(component);
        }
        at += words;
        for (int flags = 0; flags < 10; ++flags) {
            if (at >= end) return false;
            const auto count = 1 + (static_cast<std::size_t>(key[at]) + 31) / 32;
            if (!label(at, count, Component::Other)) return false;
            at += count;
        }
        if (at >= end) return false;
        const auto samplers = static_cast<std::size_t>(key[at]);
        if (!label(at, 1 + samplers, Component::Sampler)) return false;
        at += 1 + samplers;
        if (at >= end) return false;
        const auto shapes = static_cast<std::size_t>(key[at]);
        if (!label(at, 1 + shapes, Component::Image)) return false;
        at += 1 + shapes;
    }
    return at == end;
}

bool LabelContentKey(std::span<const std::uint32_t> key, bool draw, bool movable, std::vector<std::uint8_t>& labels) {
    labels.assign(key.size(), static_cast<std::uint8_t>(Component::Layout));
    if (!draw) {
        if (LabelShaderKey(key, 0, key.size(), movable, labels)) return true;
        if (key.size() < 2) return false;
        labels.assign(key.size(), static_cast<std::uint8_t>(Component::Layout));
        if (!LabelShaderKey(key, 0, key.size() - 2, movable, labels)) return false;
        labels[key.size() - 2] = static_cast<std::uint8_t>(Component::Other);
        labels[key.size() - 1] = static_cast<std::uint8_t>(Component::Other);
        return true;
    }
    if (key.size() < 4) return false;
    std::fill(labels.begin() + 1, labels.begin() + 3, static_cast<std::uint8_t>(Component::Other));
    std::size_t at = 4;
    for (std::uint32_t stage = 0; stage < key[3]; ++stage) {
        if (at >= key.size()) return false;
        const auto size = static_cast<std::size_t>(key[at]);
        ++at;
        if (at + size > key.size() || !LabelShaderKey(key, at, at + size, movable, labels)) return false;
        at += size;
    }
    if (at >= key.size()) return false;
    for (auto tail = at + 1; tail < key.size(); ++tail) labels[tail] = static_cast<std::uint8_t>(Component::DrawTarget);
    return true;
}

}

bool ShaderResources::DiffContentKeys(std::span<const std::uint32_t> previous, std::span<const std::uint32_t> key, bool draw, ContentKeyDiff& diff) {
    diff = {};
    std::vector<std::uint8_t> labels;
    bool labelled = false;
    for (const bool movable : {draw, false}) {
        labelled = LabelContentKey(key, draw, movable, labels);
        if (labelled) break;
    }
    if (!labelled || previous.size() != key.size()) {
        diff[static_cast<std::size_t>(Component::Layout)] = true;
        return false;
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
        if (previous[i] != key[i]) diff[labels[i]] = true;
    }
    return true;
}

bool MatchesContentKey(const CompiledShader& shader, std::span<const std::uint32_t> key) {
    std::size_t cursor = 0;
    bool equal = true;
    VisitContentKey(shader, false, true, [&](std::uint32_t word) {
        equal &= cursor < key.size() && key[cursor] == word;
        ++cursor;
    });
    return equal && cursor == key.size();
}

namespace {

// APS5_PROFILE_DRAW: Revalidate outcomes and time, printed as [rescache] every 10 s next to the
// device's hit/miss line: failed = the object could not be reused (whichever image path it took);
// of the reused ones, fast = every image proved current from stamps, full = the lookups were repeated.
// Why the fast path left an object to the full walk (the "fast-fail by reason" counts).
using FastFail = ShaderResources::FastFail;
constexpr std::array<const char*, static_cast<std::size_t>(FastFail::Count)> FastFailNames{"no record", "collect", "pending image", "evicted image", "memory changed", "keys", "cleared view", "storage keys", "depth surface", "replaced texture", "audit"};
using OwnRefreshFallback = ShaderResources::OwnRefreshFallback;
constexpr std::array<const char*, static_cast<std::size_t>(OwnRefreshFallback::Count)> OwnRefreshFallbackNames{"disabled", "snapshot texture", "cleared view", "foreign view", "surface key", "not imported", "uncached", "re-run failed"};

enum class EpochRefusal : std::size_t { Unlocked, Disabled, NoEpoch, Cold, Epoch, Writes, Tracker, Mapping, Pending, Depth, Cache, Images, Imports, Audit, Verify, PerUse, Count };
constexpr std::array<const char*, static_cast<std::size_t>(EpochRefusal::Count)> EpochRefusalNames{"unlocked", "proof override", "no epoch", "cold or ineligible", "epoch changed", "write or foreign GPU holder", "tracker changed", "mapping changed", "pending image", "depth identity", "cache identity", "image version or keys", "imports", "audit", "verification", "per-use failure"};
enum class EpochIneligible : std::size_t { Records, Depth, Partial, Tracking, Cache, Count };
constexpr std::array<const char*, static_cast<std::size_t>(EpochIneligible::Count)> EpochIneligibleNames{"no records", "native or overlapping depth", "partial snapshot", "untracked texels or keys", "cache identity"};

struct RevalidateProfile {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> nanoseconds{0};
    std::atomic<std::uint64_t> fast{0};
    std::atomic<std::uint64_t> full{0};
    std::atomic<std::uint64_t> failed{0};
    std::array<std::atomic<std::uint64_t>, 3> epochHits{};
    std::array<std::atomic<std::uint64_t>, 3> epochRepeats{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(EpochRefusal::Count)> epochRefusals{};
    std::atomic<std::uint64_t> epochRemembered{0};
    std::atomic<std::uint64_t> epochUnstable{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(EpochIneligible::Count)> epochIneligible{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(FastFail::Count)> fastFails{};
    // Epoch gate: registry scans skipped because the pending serial was unchanged, and import
    // serial loops skipped because the table's identity was unchanged.
    std::atomic<std::uint64_t> serialSkips{0};
    std::atomic<std::uint64_t> importSkips{0};
    // Elements with DCC keys the fast path accepted through the images' key proofs.
    std::atomic<std::uint64_t> keysProven{0};
    std::atomic<std::uint64_t> storageKeysProven{0};
    // The 'pending image' fast-fails by what overlapped: a surface with an own object (the query
    // excepts it, so the overlapping image is foreign to the surface) or a snapshot texture (no own
    // object); and the fails whose overlapping surfaces all have an own object (the T1 trim of
    // design_cpu_final applies: flush the foreign image, refresh the own object, no full walk).
    std::atomic<std::uint64_t> pendingForeign{0};
    std::atomic<std::uint64_t> pendingSnapshot{0};
    std::atomic<std::uint64_t> pendingT1Eligible{0};
    // The full walks by the reason of the fast proof that preceded them (fastFails also counts the
    // Pending failures the own-object refresh resolved), the Pending failures it resolved (with the
    // objects it refreshed: storage images, view sources) and those it left to the walk by reason;
    // view surfaces a foreign image overlapped that the proof accepted because FindPending names
    // their own source; surfaces a foreign image was still pending over after their own object's
    // refresh (its alias, a straddling image) that the re-run accepted; proofs the verify switch
    // checked against the full walk.
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(FastFail::Count)> fullByReason{};
    std::atomic<std::uint64_t> ownRefreshed{0};
    std::atomic<std::uint64_t> ownStorageRefreshes{0};
    std::atomic<std::uint64_t> ownViewRefreshes{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(OwnRefreshFallback::Count)> ownFallbacks{};
    std::atomic<std::uint64_t> ownSourceViews{0};
    std::atomic<std::uint64_t> refreshedOverlaps{0};
    std::atomic<std::uint64_t> proofsVerified{0};
    std::atomic<std::int64_t> lastReport{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(FastFail::Count)> fullNanoseconds{};
    std::atomic<std::uint64_t> refusedSampled{0};
    std::atomic<std::uint64_t> refusedStorage{0};
    std::atomic<std::uint64_t> refusedUncommitted{0};
    std::atomic<std::uint64_t> refusedUnwatched{0};
    std::atomic<std::uint64_t> refusedOther{0};
    std::mutex refusedMutex;
    std::unordered_map<std::uint64_t, std::uint64_t> refusedSurfaces;
};

RevalidateProfile& Revalidations() {
    static RevalidateProfile profile;
    return profile;
}

void countFastFail(FastFail reason) {
    constexpr const char* names[] = {"proof_fast_fail_no_record", "proof_fast_fail_collect", "proof_fast_fail_pending", "proof_fast_fail_evicted", "proof_fast_fail_changed", "proof_fast_fail_keys", "proof_fast_fail_cleared_view", "proof_fast_fail_storage_keys", "proof_fast_fail_depth", "proof_fast_fail_replaced", "proof_fast_fail_audit"};
    AgcDriver::ArchB::Recorder::Count(names[static_cast<std::size_t>(reason)]);
    if (!GuestMemory::IsCommitterThread()) AgcDriver::ArchB::Recorder::Count(reason == FastFail::Depth ? "proof_other_fast_fail_depth" : reason == FastFail::Pending ? "proof_other_fast_fail_pending" : "proof_other_fast_fail_rest");
    if (BuildProfiled()) Revalidations().fastFails[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void countPendingFail(std::span<const StorageTexture::PendingQuery> pending) {
    if (!BuildProfiled()) return;
    auto& profile = Revalidations();
    bool eligible = true;
    for (const auto& query : pending) {
        if (!query.overlaps) continue;
        if (query.except != nullptr) {
            profile.pendingForeign.fetch_add(1, std::memory_order_relaxed);
        } else {
            profile.pendingSnapshot.fetch_add(1, std::memory_order_relaxed);
            eligible = false;
        }
    }
    if (eligible) profile.pendingT1Eligible.fetch_add(1, std::memory_order_relaxed);
}

void countKeysProven(bool storage) {
    if (BuildProfiled()) (storage ? Revalidations().storageKeysProven : Revalidations().keysProven).fetch_add(1, std::memory_order_relaxed);
}

void countFullWalk(FastFail reason) {
    if (BuildProfiled() && reason != FastFail::Count) Revalidations().fullByReason[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void countFullWalkTime(FastFail reason, std::chrono::steady_clock::time_point start) {
    if (!BuildProfiled() || reason == FastFail::Count) return;
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    Revalidations().fullNanoseconds[static_cast<std::size_t>(reason)].fetch_add(static_cast<std::uint64_t>(nanoseconds), std::memory_order_relaxed);
}

constexpr std::size_t RefusedSurfaceLimit = 65536;

void countCollectRefusal(bool storage, std::uint64_t address, std::size_t bytes) {
    if (!BuildProfiled()) return;
    auto& profile = Revalidations();
    (storage ? profile.refusedStorage : profile.refusedSampled).fetch_add(1, std::memory_order_relaxed);
    const bool accessible = GuestMemory::Accessible(reinterpret_cast<const void*>(address), bytes);
    const bool watched = accessible && GuestMemory::Watched(address, bytes);
    (!accessible ? profile.refusedUncommitted : !watched ? profile.refusedUnwatched : profile.refusedOther).fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(profile.refusedMutex);
    if (profile.refusedSurfaces.size() < RefusedSurfaceLimit || profile.refusedSurfaces.contains(address)) profile.refusedSurfaces[address] = bytes;
}

void countOwnRefreshFallback(OwnRefreshFallback reason) {
    if (BuildProfiled()) Revalidations().ownFallbacks[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void countOwnRefresh(bool storage) {
    TextureCounts().ownRefreshes.fetch_add(1, std::memory_order_relaxed);
    if (BuildProfiled()) (storage ? Revalidations().ownStorageRefreshes : Revalidations().ownViewRefreshes).fetch_add(1, std::memory_order_relaxed);
}

// APS5_NO_OWN_IMAGE_REFRESH=1: a Pending failure of the fast proof takes the full walk, as before
// the own-object refresh (T1).
bool OwnImageRefreshEnabled() {
    static const bool disabled = std::getenv("APS5_NO_OWN_IMAGE_REFRESH") != nullptr;
    return !disabled;
}

// APS5_VERIFY_PROOFS=1: after every own-object refresh the full walk runs beside the proof and the
// process aborts when it would return another object or upload again (a different decision).
bool VerifyProofs() {
    static const bool enabled = std::getenv("APS5_VERIFY_PROOFS") != nullptr;
    return enabled;
}

// Whether a sampled surface's keys are the very keys of the image the view follows (the same
// metadata, extent, format and alpha placement: the same scan), so the image's proof serves it.
bool SameKeySurface(const StorageTexture* source, const GuestTextureResource& resource, std::uint64_t guestBytes) {
    if (source == nullptr) return false;
    const auto& own = source->Descriptor();
    return own.dccAddress == resource.dccAddress && source->GuestBytes() == guestBytes && own.format == resource.format && own.dccAlphaOnMsb == resource.dccAlphaOnMsb;
}

void countRevalidate(bool fast, bool ok, std::chrono::steady_clock::time_point start) {
    auto& profile = Revalidations();
    const auto now = std::chrono::steady_clock::now();
    profile.calls.fetch_add(1, std::memory_order_relaxed);
    profile.nanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count()), std::memory_order_relaxed);
    (!ok ? profile.failed : fast ? profile.fast : profile.full).fetch_add(1, std::memory_order_relaxed);
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    auto last = profile.lastReport.load();
    if (nowMs - last < 10000 || !profile.lastReport.compare_exchange_strong(last, nowMs)) return;
    const auto byReason = [](auto& counts, const auto& names) {
        std::string reasons;
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto count = counts[i].exchange(0, std::memory_order_relaxed);
            if (count == 0) continue;
            reasons += " " + std::string(names[i]) + " " + std::to_string(count);
        }
        return reasons;
    };
    const auto reasons = byReason(profile.fastFails, FastFailNames);
    const auto fullReasons = byReason(profile.fullByReason, FastFailNames);
    const auto fallbacks = byReason(profile.ownFallbacks, OwnRefreshFallbackNames);
    if (GuestMemory::ResourceEpochProofsEnabled()) {
        const auto refusals = byReason(profile.epochRefusals, EpochRefusalNames);
        const auto ineligible = byReason(profile.epochIneligible, EpochIneligibleNames);
        AgcDriver::ProfilePrint_nid_no_patch("[rescache] resource epoch proofs (10 s): hits inline draw %llu, dispatch %llu, pipelined draw %llu; repeat calls inline draw %llu, dispatch %llu, pipelined draw %llu; remembered %llu, not retained %llu; refused:%s; ineligible:%s\n", static_cast<unsigned long long>(profile.epochHits[0].exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.epochHits[1].exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.epochHits[2].exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.epochRepeats[0].exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.epochRepeats[1].exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.epochRepeats[2].exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.epochRemembered.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.epochUnstable.exchange(0, std::memory_order_relaxed)), refusals.c_str(), ineligible.c_str());
    }
    std::string walkMs;
    for (std::size_t i = 0; i < FastFailNames.size(); ++i) {
        const auto nanoseconds = profile.fullNanoseconds[i].exchange(0, std::memory_order_relaxed);
        if (nanoseconds == 0) continue;
        char text[64];
        std::snprintf(text, sizeof(text), " %s %.1f", FastFailNames[i], static_cast<double>(nanoseconds) / 1e6);
        walkMs += text;
    }
    std::uint64_t refusedSurfaces = 0;
    std::uint64_t refusedBytes = 0;
    {
        std::lock_guard lock(profile.refusedMutex);
        refusedSurfaces = profile.refusedSurfaces.size();
        for (const auto& surface : profile.refusedSurfaces) refusedBytes += surface.second;
        profile.refusedSurfaces.clear();
    }
    AgcDriver::ProfilePrint_nid_no_patch("[rescache] revalidate (10 s): %llu calls %.1f ms: fast %llu, full %llu, failed %llu; fast-fail by reason:%s; full walks: T1 refreshed %llu (%llu storage images, %llu view sources, %llu views served by their own pending source, %llu overlaps left by the refresh accepted), full by reason:%s, T1 fallback by reason:%s, proofs verified %llu; fast-fail pending image: own 0 (the query excepts the own object), foreign %llu, snapshot %llu, T1 eligible %llu; keys proven: %llu sampled, %llu storage; epoch gate: %llu registry scans skipped, %llu import loops skipped; lookup log: %llu builds and full walks kept their records past the bound; full walk ms by reason:%s; collect refusals: sampled %llu, storage %llu (uncommitted pages %llu, unwatched %llu, other %llu) over %llu surfaces, %.1f MiB\n", static_cast<unsigned long long>(profile.calls.exchange(0, std::memory_order_relaxed)), profile.nanoseconds.exchange(0, std::memory_order_relaxed) / 1e6, static_cast<unsigned long long>(profile.fast.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.full.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.failed.exchange(0, std::memory_order_relaxed)), reasons.c_str(), static_cast<unsigned long long>(profile.ownRefreshed.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.ownStorageRefreshes.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.ownViewRefreshes.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.ownSourceViews.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.refreshedOverlaps.exchange(0, std::memory_order_relaxed)), fullReasons.c_str(), fallbacks.c_str(), static_cast<unsigned long long>(profile.proofsVerified.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.pendingForeign.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.pendingSnapshot.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.pendingT1Eligible.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.keysProven.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.storageKeysProven.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.serialSkips.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.importSkips.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(lookupBuildsPastBound.exchange(0, std::memory_order_relaxed)), walkMs.c_str(), static_cast<unsigned long long>(profile.refusedSampled.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.refusedStorage.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.refusedUncommitted.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.refusedUnwatched.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(profile.refusedOther.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(refusedSurfaces), static_cast<double>(refusedBytes) / 1048576.0);
}

// APS5_NO_EPOCH_REVALIDATE=1: the per-element registry, cache and stamp checks and the per-region
// flush and import lookups of every Revalidate, as before the epoch gate.
bool EpochRevalidate() {
    static const bool enabled = std::getenv("APS5_NO_EPOCH_REVALIDATE") == nullptr;
    return enabled;
}

std::chrono::steady_clock::duration TemplateAuditInterval() {
    static const auto interval = [] {
        const char* text = std::getenv("APS5_TEMPLATE_AUDIT_MS");
        return std::chrono::milliseconds(text != nullptr ? std::strtoull(text, nullptr, 10) : 1000ull);
    }();
    return interval;
}

bool DepthBlocksProof(VkDevice device, std::uint64_t address, std::size_t bytes) {
    static const bool staleOnly = std::getenv("APS5_DEPTH_PROOF_STALE_ONLY") != nullptr;
    return staleOnly ? DepthSurfaceBlocksMemoryProof(device, address, bytes) : DepthSurfaceOverlaps(device, address, bytes);
}

bool AuditEveryInterval() {
    static const bool always = std::getenv("APS5_TEMPLATE_AUDIT_ALWAYS") != nullptr;
    return always;
}

bool TemplateAuditDue(std::chrono::steady_clock::time_point last) {
    const auto interval = TemplateAuditInterval();
    return interval.count() != 0 && std::chrono::steady_clock::now() - last >= interval;
}

std::uint32_t ImportAuditBudget() {
    static const auto budget = [] {
        const char* text = std::getenv("APS5_TEMPLATE_AUDIT_BUDGET");
        return static_cast<std::uint32_t>(text != nullptr ? std::strtoul(text, nullptr, 10) : 8ul);
    }();
    return budget;
}

bool ImportAuditAdmitted() {
    static std::atomic<std::int64_t> slot{-1};
    static std::atomic<std::uint32_t> admitted{0};
    const auto budget = ImportAuditBudget();
    if (budget == 0) return true;
    const auto now = std::chrono::steady_clock::now().time_since_epoch() / std::chrono::milliseconds(4);
    auto seen = slot.load(std::memory_order_relaxed);
    if (seen != now && slot.compare_exchange_strong(seen, now, std::memory_order_relaxed)) admitted.store(0, std::memory_order_relaxed);
    return admitted.fetch_add(1, std::memory_order_relaxed) < budget;
}

}

void LastRecordIndices(std::span<const void* const> logged, std::span<const void* const> objects, std::vector<std::size_t>& indices) {
    indices.assign(objects.size(), logged.size());
    struct WantedSlotStorage {};
    auto& wanted = AgcDriver::ThreadScratch<std::vector<std::pair<const void*, std::size_t>>, WantedSlotStorage>();
    wanted.clear();
    for (std::size_t i = 0; i < objects.size(); ++i) wanted.emplace_back(objects[i], i);
    const auto byObject = [](const std::pair<const void*, std::size_t>& left, const std::pair<const void*, std::size_t>& right) { return std::less<const void*>{}(left.first, right.first); };
    std::sort(wanted.begin(), wanted.end(), byObject);
    std::size_t open = wanted.size();
    for (auto at = logged.size(); at-- > 0 && open != 0;) {
        const auto [first, last] = std::equal_range(wanted.begin(), wanted.end(), std::pair<const void*, std::size_t>{logged[at], 0}, byObject);
        if (first == last || indices[first->second] != logged.size()) continue;
        for (auto it = first; it != last; ++it) indices[it->second] = at;
        open -= static_cast<std::size_t>(last - first);
    }
    if (wanted.capacity() > LookupLogBound + LookupLogBound / 2) {
        wanted.clear();
        wanted.shrink_to_fit();
    }
}

bool ShaderResources::keepsTemplateRecords() const {
    static const bool always = std::getenv("APS5_KEEP_TEMPLATE_RECORDS") != nullptr;
    return always || !(usesBda || (usesFaultBuffer && LoopGuardEnabled()));
}

// Turns the lookups' records into this object's per-texture validation records (see
// ValidatedSurface); the last record of an object is the freshest.
void ShaderResources::captureValidation() {
    if (!keepsTemplateRecords()) {
        validatedTextures.clear();
        lookupLog().clear();
        return;
    }
    const auto& log = lookupLog();
    struct LoggedSlotStorage {};
    auto& logged = AgcDriver::ThreadScratch<std::vector<const void*>, LoggedSlotStorage>();
    struct ObjectsSlotStorage {};
    auto& objects = AgcDriver::ThreadScratch<std::vector<const void*>, ObjectsSlotStorage>();
    struct IndicesSlotStorage {};
    auto& indices = AgcDriver::ThreadScratch<std::vector<std::size_t>, IndicesSlotStorage>();
    logged.clear();
    for (const auto& entry : log) logged.push_back(entry.object);
    objects.clear();
    for (const auto& texture : textures) objects.push_back(texture.get());
    LastRecordIndices(logged, objects, indices);
    validatedTextures.assign(textures.size(), {});
    for (std::size_t i = 0; i < textures.size(); ++i) {
        if (indices[i] == log.size()) continue;
        const auto& found = log[indices[i]];
        validatedTextures[i] = {found.resource, found.bytes, found.keys, found.generation, 0, found.source, found.depth, true, found.committed};
    }
    lookupLog().clear();
    const auto trim = [](auto& scratch) {
        if (scratch.capacity() <= LookupLogBound + LookupLogBound / 2) return;
        scratch.clear();
        scratch.shrink_to_fit();
    };
    trim(logged);
    trim(objects);
    trim(indices);
}

ShaderResources::CollectRefusals ShaderResources::CollectRefusalCounts() {
    auto& profile = Revalidations();
    CollectRefusals counts;
    counts.sampled = profile.refusedSampled.load(std::memory_order_relaxed);
    counts.storage = profile.refusedStorage.load(std::memory_order_relaxed);
    counts.uncommitted = profile.refusedUncommitted.load(std::memory_order_relaxed);
    counts.unwatched = profile.refusedUnwatched.load(std::memory_order_relaxed);
    counts.other = profile.refusedOther.load(std::memory_order_relaxed);
    std::lock_guard lock(profile.refusedMutex);
    counts.surfaces = profile.refusedSurfaces.size();
    for (const auto& surface : profile.refusedSurfaces) counts.bytes += surface.second;
    return counts;
}

std::array<std::uint64_t, static_cast<std::size_t>(ShaderResources::FastFail::Count)> ShaderResources::FullWalkNanoseconds() {
    std::array<std::uint64_t, static_cast<std::size_t>(FastFail::Count)> result{};
    auto& profile = Revalidations();
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = profile.fullNanoseconds[i].load(std::memory_order_relaxed);
    return result;
}

std::size_t ShaderResources::LookupLogCapacity() {
    return lookupLog().capacity();
}

bool ShaderResources::collectPartialSnapshot(ValidatedSurface& surface) {
    if (surface.source != nullptr || surface.keys != DccKeys::Uncompressed || !surface.committed.has_value() || surface.generation == 0) return false;
    if (ArchB::Switches::Enabled(ArchB::Feature::WtFault)) {
        for (const auto& [begin, end] : *surface.committed) {
            const auto bytes = static_cast<std::size_t>(end - begin);
            if (!GuestMemory::FaultUnchangedSince(begin, bytes, surface.generation) && GuestMemory::CollectWritesUncached(begin, bytes) == 0) return false;
        }
    }
    if (AgcDriver::ArchB::AllocChurnActive()) {
        // APS5_ARCHB_FIX_ALLOC_CHURN: collected into a per-thread scratch that keeps its capacity.
        struct CurrentSlotStorage {};
        auto& current = AgcDriver::ThreadScratch<std::vector<std::pair<std::uint64_t, std::uint64_t>>, CurrentSlotStorage>();
        surface.collected = GuestMemory::CollectCommittedWritesInPlace(surface.resource.baseAddress, static_cast<std::size_t>(surface.bytes), current);
        return surface.collected != 0 && current == *surface.committed;
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> current;
    surface.collected = GuestMemory::CollectCommittedWrites(surface.resource.baseAddress, static_cast<std::size_t>(surface.bytes), current);
    return surface.collected != 0 && current == *surface.committed;
}

bool ShaderResources::finishPartialSnapshots(std::uint64_t forgetBefore) {
    for (auto& surface : validatedTextures) {
        if (surface.committed.has_value() && !collectPartialSnapshot(surface)) return false;
    }
    std::size_t proved = 0;
    for (std::size_t i = 0; i < validatedTextures.size(); ++i) {
        const auto& surface = validatedTextures[i];
        if (!surface.committed.has_value()) continue;
        const auto address = surface.resource.baseAddress;
        const auto bytes = static_cast<std::size_t>(surface.bytes);
        if (textures[i]->Released() || SampledFromStorageEligible(context, surface.resource, surface.bytes) || Recorder::SnapshotWriteOverlaps(address, bytes) || AnyShadowedOverlaps(address, bytes) || !GuestMemory::UnchangedCommittedSince(*surface.committed, surface.generation)) return false;
        ++proved;
    }
    if (proved != 0) {
        if (std::any_of(textures.begin(), textures.end(), [](const auto& texture) { return texture->Released(); })) return false;
        if ((forgetBefore & 1u) != 0 || forgetBefore != GuestMemory::ForgetSerial()) return false;
        if (BuildProfiled()) TextureCounts().partialProven.fetch_add(proved, std::memory_order_relaxed);
    }
    return true;
}

// Proves every texture and storage image still current without repeating its lookup: exactly the
// "unchanged" branches of cachedTexture and StorageTexture::Refresh, evaluated from write stamps,
// the pending-results registry and the DCC keys. Per element: collect first (stamps come only from
// collects, and the generation a record moves to must predate the checks, so a write landing between
// them is stamped newer: the rule of Driver's dispatch cache), then no other image may have results
// pending over the memory (a lookup would flush them into it), a storage image must still be the
// cache's image of its surface, the memory must be unchanged since the object's content matched it,
// and the DCC keys must be what the content was made under whenever the surface has any (a fast
// clear touches only the keys). Anything else, including a failed collect (memory outside the arena,
// uncommitted pages), is left to the full walk.
// The epoch gate: while the pending registry's serial is what this object's last proof saw
// (pendingSerialSeen), the registry is what it was then (every change bumps the serial under the
// registry mutex), so the pending-image checks are skipped; otherwise one scan answers them for
// every surface at once. The stamp checks go through one tracker lock, the cache checks through
// the images' cached flags and one touch of the cache.
namespace {
bool ComputeCensus();

struct DepthProofCounters {
    std::atomic<std::uint64_t> blocked{0}, accepted{0}, wouldAccept{0}, noWalk{0}, surfaces{0}, stale{0}, unlocked{0}, verified{0}, mismatches{0};
};

DepthProofCounters& depthProofCounters() {
    static auto* made = [] {
        auto* c = new DepthProofCounters();
        const std::pair<const char*, std::atomic<std::uint64_t>*> named[] = {
            {"c_depth_proof_blocked", &c->blocked}, {"c_depth_proof_accepted", &c->accepted}, {"c_depth_proof_would_accept", &c->wouldAccept},
            {"c_depth_proof_refused_no_walk", &c->noWalk}, {"c_depth_proof_refused_surfaces", &c->surfaces}, {"c_depth_proof_refused_stale", &c->stale},
            {"c_depth_proof_refused_unlocked", &c->unlocked}, {"c_depth_proof_verified", &c->verified}, {"c_depth_proof_verify_mismatches", &c->mismatches}};
        for (const auto& [name, counter] : named) AgcDriver::ArchB::Recorder::RegisterCounter(name, *counter);
        return c;
    }();
    return *made;
}

bool depthProofOn() {
    return ArchB::Switches::Enabled(ArchB::Feature::CComputeDepthProof);
}

bool depthProofVerifyNow() {
    static const std::uint64_t period = [] {
        const char* value = std::getenv("APS5_ARCHB_C_COMPUTE_DEPTH_PROOF_VERIFY");
        return value == nullptr ? std::uint64_t{0} : static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10));
    }();
    if (period == 0) return false;
    static std::atomic<std::uint64_t> uses{0};
    return (uses.fetch_add(1, std::memory_order_relaxed) + 1) % period == 0;
}
}

bool ShaderResources::depthProofAccepts(std::uint64_t address, std::uint64_t bytes) {
    const bool on = depthProofOn();
    const bool census = ComputeCensus();
    if (!on && !census) return false;
    auto& c = depthProofCounters();
    c.blocked.fetch_add(1, std::memory_order_relaxed);
    const auto refuse = [](std::atomic<std::uint64_t>& counter) {
        counter.fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    if (!GuestMemory::GpuMutex().HeldByThisThread()) return refuse(c.unlocked);
    if (depthWalkSerial == 0) return refuse(c.noWalk);
    if (DepthSurfacesSerial() != depthWalkSerial || DepthSurfacesReleased() != depthWalkReleased) return refuse(c.surfaces);
    if (DepthSurfaceBlocksMemoryProof(context.device, address, bytes)) return refuse(c.stale);
    if (!on) return refuse(c.wouldAccept);
    c.accepted.fetch_add(1, std::memory_order_relaxed);
    depthProofAccepted = true;
    return true;
}

bool ShaderResources::fastRevalidate(std::uint64_t serialBefore, std::span<const PendingOverlap> refreshed, FastFail& reason, std::vector<PendingOverlap>& overlapping, bool& accepted) {
    reason = FastFail::Count;
    overlapping.clear();
    accepted = false;
    depthProofAccepted = false;
    if (!EpochRevalidate()) return fastRevalidateEach();
    const auto forgetBefore = GuestMemory::ForgetSerial();
    const auto fail = [&reason](FastFail why) {
        reason = why;
        countFastFail(why);
        return false;
    };
    if (validatedTextures.size() != textures.size()) return fail(FastFail::NoRecord);
    const bool unchanged = pendingSerialSeen != 0 && pendingSerialSeen == serialBefore;
    const bool keyProofs = KeyFastPath();
    struct QueriesTag {};
    auto& queries = ThreadScratch<std::vector<GuestMemory::UnchangedQuery>, QueriesTag>();
    struct PendingTag {};
    auto& pending = ThreadScratch<std::vector<StorageTexture::PendingQuery>, PendingTag>();
    // The element each pending query stands for, in query order.
    struct OwnersTag {};
    auto& owners = ThreadScratch<std::vector<PendingOverlap>, OwnersTag>();
    struct ImagesTag {};
    auto& images = ThreadScratch<std::vector<const StorageTexture*>, ImagesTag>();
    struct ScannedKeysTag {};
    auto& scannedKeys = ThreadScratch<std::vector<DccKeys>, ScannedKeysTag>();
    queries.clear();
    pending.clear();
    owners.clear();
    images.clear();
    scannedKeys.assign(textures.size(), DccKeys::Uncompressed);
    const auto query = [&](std::uint64_t begin, std::uint64_t end, const StorageTexture* except, const StorageTexture* identity, PendingOverlap owner) {
        pending.push_back({begin, end, except, identity, false});
        owners.push_back(owner);
    };
    for (std::size_t i = 0; i < textures.size(); ++i) {
        auto& surface = validatedTextures[i];
        if (!surface.valid) return fail(FastFail::NoRecord);
        if (surface.depth) {
            if (!DepthSurfaceHolds(context, surface.resource, textures[i].get())) return fail(FastFail::Depth);
            continue;
        }
        if (textures[i]->Released()) return fail(FastFail::Replaced);
        const auto address = surface.resource.baseAddress;
        const auto bytes = static_cast<std::size_t>(surface.bytes);
        if (DepthBlocksProof(context.device, address, bytes) && !depthProofAccepts(address, bytes)) return fail(FastFail::Depth);
        if (!FlushDepthPlanes(address, bytes).empty()) return fail(FastFail::Changed);
        if (!surface.committed.has_value()) {
            surface.collected = GuestMemory::CollectWrites(address, bytes);
            if (surface.collected == 0) {
                countCollectRefusal(false, address, bytes);
                return fail(FastFail::Collect);
            }
        }
        const auto* source = surface.source;
        if (ClearSnapshotCacheEnabled() && source != nullptr && surface.resource.dccAddress != 0 && IsDccClear(source->FilledKeys())) return fail(FastFail::Keys);
        if (!keyProofs) {
            // Without proofs the keys are compared with the record's (a cleared view's identity
            // below is the record's too): the scan repeats on every call, as before.
            if (surface.resource.dccAddress != 0 && TextureClearKeys(surface.resource, surface.bytes) != surface.keys) return fail(FastFail::Keys);
            scannedKeys[i] = surface.keys;
            if (!unchanged || surface.committed.has_value()) query(address, address + bytes, source, source != nullptr && surface.keys != DccKeys::Uncompressed ? source : nullptr, {i, false, source != nullptr && surface.keys == DccKeys::Uncompressed});
        } else if (source != nullptr) {
            // The view follows its image, so it stays valid while the image would Refresh as
            // unchanged: the image's own keys must be what its content was uploaded under (its
            // proof answers without a scan while they are unstamped), and its memory unchanged
            // since its generation (the query below). The surface's own keys come from the same
            // proof when they are the image's, else from the texture's.
            const auto& own = source->Descriptor();
            const auto sourceKeys = source->ProvedKeys();
            if (sourceKeys != source->UploadedKeys()) return fail(FastFail::Keys);
            const auto keys = surface.resource.dccAddress == 0 ? DccKeys::Uncompressed : SameKeySurface(source, surface.resource, surface.bytes) ? sourceKeys : ProvedClearKeys(surface.resource, surface.bytes, textures[i]->KeyProof());
            if (surface.resource.dccAddress != 0 && surface.resource.dccAddress != own.dccAddress && (keys != DccKeys::Uncompressed || !StorageImageServesKeys(*source, surface.resource.dccAddress))) return fail(FastFail::Keys);
            scannedKeys[i] = keys;
            // A view of a fast-cleared surface stays one only while its image still has results
            // pending over it (cachedTexture's hit rule), unless the image's own descriptor names
            // the surface's metadata (the lookup views the cleared image then). The identity is
            // asked whatever the registry's serial did: the keys may have moved since the last
            // proof while the registry did not.
            if (keys != DccKeys::Uncompressed && !(own.dccAddress == surface.resource.dccAddress && ClearedViewEnabled() && StorageClearAvailable(context, surface.resource.format, keys))) query(address, address + bytes, source, source, {i, false, false});
            else if (!unchanged) query(address, address + bytes, source, nullptr, {i, false, keys == DccKeys::Uncompressed});
            if (surface.resource.dccAddress != 0 || own.dccAddress != 0) countKeysProven(false);
        } else {
            // A snapshot holds its clear texels or the guest bytes: the keys must be what it was made under.
            const auto keys = surface.resource.dccAddress != 0 ? ProvedClearKeys(surface.resource, surface.bytes, textures[i]->KeyProof()) : DccKeys::Uncompressed;
            if (keys != surface.keys) return fail(FastFail::Keys);
            scannedKeys[i] = keys;
            if (!unchanged || surface.committed.has_value()) query(address, address + bytes, nullptr, nullptr, {i, false, false});
            if (surface.resource.dccAddress != 0) countKeysProven(false);
        }
        if (source != nullptr) {
            if (!source->Cached()) return fail(FastFail::Evicted);
            queries.push_back({address, bytes, source->GenerationFor(address, bytes)});
            images.push_back(source);
        } else if (!surface.committed.has_value()) {
            queries.push_back({address, bytes, surface.generation});
        }
    }
    for (std::size_t i = 0; i < storageTextures.size(); ++i) {
        if (storageTextures[i] != nullptr && (i >= storageKeys.size() || !StorageImageServesKeys(*storageTextures[i], storageKeys[i]))) return fail(FastFail::StorageKeys);
        if (i != 0 && storageTextures[i] == storageTextures[i - 1]) continue;
        const auto* image = storageTextures[i].get();
        if (image == nullptr) return fail(FastFail::NoRecord);
        const auto& own = image->Descriptor();
        const auto address = own.baseAddress;
        const auto bytes = static_cast<std::size_t>(image->GuestBytes());
        if (DepthBlocksProof(context.device, address, bytes) && !depthProofAccepts(address, bytes)) {
            depthGuardRefusals.fetch_add(1, std::memory_order_relaxed);
            return fail(FastFail::Depth);
        }
        if (GuestMemory::CollectWrites(address, bytes) == 0) {
            countCollectRefusal(true, address, bytes);
            return fail(FastFail::Collect);
        }
        if (own.dccAddress != 0) {
            // Refresh's unchanged branch (its key compare against the keys the content was
            // uploaded under, through the image's proof; the memory query below), minus its
            // byte-compare fallback; the image's generation is left where it is.
            if (!keyProofs || image->ProvedKeys() != image->UploadedKeys()) return fail(FastFail::StorageKeys);
            countKeysProven(true);
        }
        if (!unchanged) query(address, address + bytes, image, nullptr, {i, true, false});
        if (!image->Cached()) return fail(FastFail::Evicted);
        queries.push_back({address, bytes, image->GenerationFor(address, bytes)});
        images.push_back(image);
    }
    if (!pending.empty()) {
        if (!StorageTexture::ScanPending(pending)) return fail(FastFail::ClearedView);
        // A foreign image over a view under uncompressed keys is no failure while FindPending
        // names the view's own source: the lookup views the source (cachedTexture's hit rule)
        // and touches nothing else. A surface whose own object T1 refreshed in this Revalidate
        // (`refreshed`) is no failure either: a storage element's lookup is that Refresh and
        // returns the image whatever stays registered over it; a view's lookup hits on the source
        // when FindPending names it, and views the cached image of its key (the source, proved by
        // refreshOwnObjects) when FindPending names nothing. Every other overlap is the full
        // walk's, or T1's (Revalidate).
        const auto refreshedOwner = [&](const PendingOverlap& owner) {
            return std::find_if(refreshed.begin(), refreshed.end(), [&](const PendingOverlap& entry) { return entry.element == owner.element && entry.storage == owner.storage; });
        };
        for (std::size_t k = 0; k < pending.size(); ++k) {
            if (!pending[k].overlaps) continue;
            if (OwnImageRefreshEnabled() && owners[k].viewUncompressed && pending[k].found == pending[k].except) {
                if (BuildProfiled()) Revalidations().ownSourceViews.fetch_add(1, std::memory_order_relaxed);
                accepted = true;
                continue;
            }
            const auto lookupAccepts = [&](const PendingOverlap& entry) {
                const auto& surface = validatedTextures[entry.element];
                if (surface.resource.dccAddress != 0) return false;
                const auto viewed = pendingLookupSource(context, surface.resource, surface.bytes, entry.depthCompare).source;
                return viewed.get() == pending[k].except || (viewed == nullptr && entry.sourceEligible && entry.sourceRefreshed);
            };
            if (const auto entry = refreshedOwner(owners[k]); entry != refreshed.end() && (entry->storage || pending[k].found == pending[k].except || (pending[k].found == nullptr && entry->sourceEligible) || lookupAccepts(*entry))) {
                if (BuildProfiled()) Revalidations().refreshedOverlaps.fetch_add(1, std::memory_order_relaxed);
                accepted = true;
                continue;
            }
            overlapping.push_back(owners[k]);
        }
        if (!overlapping.empty()) {
            countPendingFail(pending);
            return fail(FastFail::Pending);
        }
    } else if (unchanged && BuildProfiled()) {
        Revalidations().serialSkips.fetch_add(1, std::memory_order_relaxed);
    }
    if (!finishPartialSnapshots(forgetBefore)) return fail(FastFail::Changed);
    if (!GuestMemory::UnchangedSinceAll(queries)) return fail(FastFail::Changed);
    if (!StorageImagesCached(context, images)) return fail(FastFail::Evicted);
    for (const auto* image : images) image->NoteProved();
    for (std::size_t i = 0; i < validatedTextures.size(); ++i) {
        auto& surface = validatedTextures[i];
        if (surface.source == nullptr) surface.generation = surface.collected;
        surface.keys = scannedKeys[i];
    }
    return true;
}

bool ShaderResources::fastRevalidateEach() {
    if (validatedTextures.size() != textures.size()) return false;
    const auto forgetBefore = GuestMemory::ForgetSerial();
    for (std::size_t i = 0; i < textures.size(); ++i) {
        auto& surface = validatedTextures[i];
        if (!surface.valid) return false;
        if (surface.depth) {
            if (!DepthSurfaceHolds(context, surface.resource, textures[i].get())) return false;
            continue;
        }
        if (textures[i]->Released()) return false;
        const auto address = surface.resource.baseAddress;
        const auto bytes = static_cast<std::size_t>(surface.bytes);
        if (DepthBlocksProof(context.device, address, bytes)) return false;
        if (!FlushDepthPlanes(address, bytes).empty()) return false;
        if (!surface.committed.has_value()) {
            surface.collected = GuestMemory::CollectWrites(address, bytes);
            if (surface.collected == 0) return false;
        }
        if (PendingStorageOverlaps(address, bytes, surface.source)) return false;
        if (surface.source != nullptr && surface.resource.dccAddress != 0 && surface.resource.dccAddress != surface.source->Descriptor().dccAddress) return false;
        if (ClearSnapshotCacheEnabled() && surface.source != nullptr && surface.resource.dccAddress != 0 && IsDccClear(surface.source->FilledKeys())) return false;
        if (surface.resource.dccAddress != 0 && TextureClearKeys(surface.resource, surface.bytes) != surface.keys) return false;
        // A view made under fast-clear keys stays one only while its image still has results pending
        // over the surface (the lookup prefers them to the clear); once they are flushed the clear
        // the image cannot see wins and the lookup makes a snapshot (cachedTexture's hit rule).
        if (surface.source != nullptr && surface.keys != DccKeys::Uncompressed && StorageTexture::FindPending(address, surface.bytes).get() != surface.source) return false;
    }
    for (std::size_t i = 0; i < storageTextures.size(); ++i) {
        if (storageTextures[i] != nullptr && (i >= storageKeys.size() || (storageKeys[i] != 0 && storageKeys[i] != storageTextures[i]->Descriptor().dccAddress))) return false;
        // Consecutive elements of one mip chain share the image (see addStorageImageBinding).
        if (i != 0 && storageTextures[i] == storageTextures[i - 1]) continue;
        const auto* image = storageTextures[i].get();
        if (image == nullptr) return false;
        // StorageTexture::Refresh checks the image's own surface, not the element's descriptor (one
        // image serves every descriptor of its memory, with or without a DCC address), and compares
        // its keys with the keys it was uploaded under, which only the image knows (they move to
        // Uncompressed on every write-back; a fast clear after it puts the key bytes back to what a
        // record saw while the image holds last frame's results), so a surface with keys is left to
        // the full walk. Without a DCC address both sides of that compare are Uncompressed whatever
        // happened, and the memory checks below are Refresh's.
        const auto& own = image->Descriptor();
        if (own.dccAddress != 0) return false;
        const auto address = own.baseAddress;
        const auto bytes = static_cast<std::size_t>(image->GuestBytes());
        if (DepthBlocksProof(context.device, address, bytes)) {
            depthGuardRefusals.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (GuestMemory::CollectWrites(address, bytes) == 0) return false;
        if (PendingStorageOverlaps(address, bytes, image)) return false;
    }
    if (!finishPartialSnapshots(forgetBefore)) return false;
    for (const auto& surface : validatedTextures) {
        if (surface.depth) continue;
        const auto address = surface.resource.baseAddress;
        const auto bytes = static_cast<std::size_t>(surface.bytes);
        if (surface.source != nullptr) {
            if (!StorageImageCached(context, surface.source) || !GuestMemory::UnchangedSince(address, bytes, surface.source->GenerationFor(address, bytes))) return false;
        } else if (!surface.committed.has_value() && !GuestMemory::UnchangedSince(address, bytes, surface.generation)) return false;
    }
    for (std::size_t i = 0; i < storageTextures.size(); ++i) {
        if (i != 0 && storageTextures[i] == storageTextures[i - 1]) continue;
        const auto* image = storageTextures[i].get();
        const auto address = image->Descriptor().baseAddress;
        const auto bytes = static_cast<std::size_t>(image->GuestBytes());
        if (!StorageImageCached(context, image) || !GuestMemory::UnchangedSince(address, bytes, image->GenerationFor(address, bytes))) return false;
    }
    // Every check passed: snapshot textures are current at the collects issued before them. Storage
    // images keep their own generation: nothing was stamped above it, so the blocks a later write
    // stamps are the same set either way.
    for (auto& surface : validatedTextures) {
        if (surface.source == nullptr) surface.generation = surface.collected;
    }
    return true;
}

ShaderResources::OwnRefreshFallback ShaderResources::refreshOwnObjects(std::span<const CompiledShader> shaders, std::span<PendingOverlap> overlapping) {
    const auto listed = [&](std::size_t element, bool storage) {
        return std::find_if(overlapping.begin(), overlapping.end(), [&](const PendingOverlap& overlap) { return overlap.element == element && overlap.storage == storage; });
    };
    // A sampled view's lookup (cachedTexture): the pending image FindPending serves the surface
    // by, if it fits the view, else the cached storage image of the surface's key, refreshed; the
    // view stays the lookup's answer only when that image is its own source. `sourceEligible` is
    // recorded whichever branch answers: a later refresh of this call may flush the source, and
    // the re-run then judges the surface by the other branch.
    const auto refreshView = [&](std::size_t i, PendingOverlap& overlap, bool depthCompare) {
        const auto& surface = validatedTextures[i];
        const auto& source = textures[i]->SharedStorageSource();
        if (source == nullptr || source.get() != surface.source) return OwnRefreshFallback::Snapshot;
        if (!source->Cached()) return OwnRefreshFallback::Uncached;
        if (!StorageImageServesKeys(*source, surface.resource.dccAddress)) return OwnRefreshFallback::Keys;
        const auto address = surface.resource.baseAddress;
        const auto bytes = static_cast<std::size_t>(surface.bytes);
        const bool imported = SampledFromStorageEligible(context, surface.resource, surface.bytes);
        overlap.sourceEligible = imported && SurfaceKey(context, source->Descriptor()) == SurfaceKey(context, surface.resource);
        overlap.depthCompare = depthCompare;
        overlap.sourceRefreshed = false;
        const auto found = pendingLookupSource(context, surface.resource, surface.bytes, depthCompare).source;
        if (found != nullptr) {
            if (found != source) return OwnRefreshFallback::ForeignView;
            if (!GuestMemory::UnchangedSince(address, bytes, source->GenerationFor(address, bytes))) {
                countOwnRefresh(false);
                refreshForView(*source, surface.resource);
            }
            return OwnRefreshFallback::Count;
        }
        if (!imported) return OwnRefreshFallback::NotImported;
        if (!overlap.sourceEligible) return OwnRefreshFallback::SurfaceKey;
        countOwnRefresh(false);
        // The cache's image of the key is this source while it is cached (an entry is replaced
        // only after its image left the cache), so the lookup would refresh and return it.
        source->Refresh();
        overlap.sourceRefreshed = true;
        return OwnRefreshFallback::Count;
    };
    // A storage element's lookup (cachedStorageTexture): the cached image of its surface key,
    // refreshed; the key is the element's words', which mapped to this image at the build.
    const auto refreshStorage = [&](std::size_t i) {
        auto& image = storageTextures[i];
        if (image == nullptr || !image->Cached()) return OwnRefreshFallback::Uncached;
        if (i >= storageKeys.size() || !StorageImageServesKeys(*image, storageKeys[i])) return OwnRefreshFallback::Keys;
        countOwnRefresh(true);
        image->Refresh();
        return OwnRefreshFallback::Count;
    };
    std::size_t textureIndex = 0;
    std::size_t storageIndex = 0;
    for (const auto& shader : shaders) {
        if (shader.program == nullptr) return OwnRefreshFallback::Uncached;
        for (const auto& binding : shader.program->bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages) continue;
            if (binding.kind == ShaderRecompiler::DescriptorKind::SampledImage) {
                for (std::uint32_t element = 0; element < binding.count; ++element) {
                    const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u);
                    if (IsNullSampledTextureResource(words)) continue;
                    if (textureIndex >= textures.size()) return OwnRefreshFallback::Uncached;
                    const auto index = textureIndex++;
                    const auto overlap = listed(index, false);
                    if (overlap == overlapping.end()) continue;
                    if (const auto fallback = refreshView(index, *overlap, !binding.Flags().imageDepthCompare.empty() && binding.Flags().imageDepthCompare.at(element)); fallback != OwnRefreshFallback::Count) return fallback;
                }
            } else if (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) {
                for (std::uint32_t element = 0; element < binding.count; ++element) {
                    const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u);
                    if (IsNullTextureResource(words)) continue;
                    if (storageIndex >= storageTextures.size()) return OwnRefreshFallback::Uncached;
                    const auto index = storageIndex++;
                    if (listed(index, true) == overlapping.end()) continue;
                    if (const auto fallback = refreshStorage(index); fallback != OwnRefreshFallback::Count) return fallback;
                }
            }
        }
    }
    for (const auto& table : tableStages) {
        const auto* images = table.elementTextures.empty() ? nullptr : tableImages(shaders, table.stage);
        for (std::size_t element = 0; element < table.elementTextures.size(); ++element) {
            const auto index = table.elementTextures[element];
            if (index == std::numeric_limits<std::size_t>::max()) continue;
            if (!table.representatives.empty() && table.representatives[element] != NoRepresentative) continue;
            if (index != textureIndex++ || index >= textures.size()) return OwnRefreshFallback::Uncached;
            const auto overlap = listed(index, false);
            if (overlap == overlapping.end()) continue;
            if (images == nullptr || images->count != table.elementTextures.size()) return OwnRefreshFallback::Uncached;
            if (const auto fallback = refreshView(index, *overlap, images->Flags().imageDepthCompare.at(element)); fallback != OwnRefreshFallback::Count) return fallback;
        }
    }
    return OwnRefreshFallback::Count;
}

bool AnyPendingCpuWrites(const Context& context) {
    if (Recorder::InCompletion() || Recorder::Active() == nullptr) return false;
    return (context.copiedWriters != nullptr && !context.copiedWriters->empty()) || !DrawCopiedWriters()->empty() || Recorder::PendingCompletionLabels() != 0;
}

bool CompletePendingCpuWrites(const Context& context, std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || Recorder::InCompletion()) return false;
    auto* recorder = Recorder::Active();
    if (recorder == nullptr) return false;
    const auto drawWriters = DrawCopiedWriters();
    const bool labels = Recorder::PendingCompletionLabels() != 0;
    if ((context.copiedWriters == nullptr || context.copiedWriters->empty()) && drawWriters->empty() && !labels) return false;
    if (!recorder->PendingWriteOverlaps(address, bytes)) return false;
    const auto overlaps = [&](const auto& writers) {
        return std::any_of(writers.begin(), writers.end(), [&](const auto& writer) { return writer->CopiedWritesOverlap(address, bytes); });
    };
    if ((context.copiedWriters != nullptr && overlaps(*context.copiedWriters)) || overlaps(*drawWriters) || (labels && recorder->CompletionLabelIn(address, bytes))) {
        Recorder::CountSync(4);
        recorder->SyncThrough(address, bytes);
        return true;
    }
    return false;
}

const ShaderRecompiler::DescriptorBinding* ShaderResources::tableImages(std::span<const CompiledShader> shaders, ShaderRecompiler::ShaderStage stage) {
    for (const auto& shader : shaders) {
        if (shader.stage != stage || shader.program == nullptr) continue;
        for (const auto& binding : shader.program->bindings) {
            if (binding.role == ShaderRecompiler::DescriptorRole::ImageTable) return &binding;
        }
    }
    return nullptr;
}

bool ShaderResources::EpochProofs() {
    return DrawPipelineEnabled();
}

namespace {
bool PartialSnapshotsUnchanged(const auto& surfaces) {
    for (const auto& surface : surfaces) {
        if (!surface.committed.has_value()) continue;
        for (const auto& [begin, end] : *surface.committed) {
            if (!GuestMemory::FaultUnchangedSince(begin, static_cast<std::size_t>(end - begin), surface.generation)) return false;
        }
    }
    return true;
}

bool HasSurfaceTracking(const auto& surfaces, const auto& images) {
    const auto tracked = [](std::uint64_t address, std::size_t bytes) {
        return GuestMemory::Watched(address, bytes) && GuestMemory::Accessible(reinterpret_cast<const void*>(address), bytes);
    };
    for (const auto& surface : surfaces) {
        if (!surface.valid) return false;
        if (surface.committed.has_value()) {
            if (surface.source != nullptr || surface.keys != DccKeys::Uncompressed || surface.generation == 0 || surface.committed->empty()) return false;
            for (const auto& [begin, end] : *surface.committed) {
                if (end <= begin || !GuestMemory::Watched(begin, static_cast<std::size_t>(end - begin))) return false;
            }
            continue;
        }
        if (!surface.depth && !tracked(surface.resource.baseAddress, static_cast<std::size_t>(surface.bytes))) return false;
    }
    for (const auto& image : images) {
        if (image == nullptr || !tracked(image->Descriptor().baseAddress, static_cast<std::size_t>(image->GuestBytes()))) return false;
    }
    return true;
}

void noteEpochProof(bool reused, bool verified, bool failed) {
    static std::atomic<std::uint64_t> reuses{0}, proofs{0}, checks{0}, failures{0};
    if (verified) checks.fetch_add(1, std::memory_order_relaxed);
    if (failed) failures.fetch_add(1, std::memory_order_relaxed);
    const auto count = (reused ? reuses : proofs).fetch_add(1, std::memory_order_relaxed) + 1u;
    static const bool report = std::getenv("APS5_PROFILE_DRAW") != nullptr || std::getenv("APS5_VERIFY_DRAW_PIPELINE") != nullptr;
    if (!report) return;
    if (count % 262144u == 0u || (count & (count - 1u)) == 0u) std::fprintf(stderr, "[epoch-proofs] resource proofs: %llu reused the epoch's, %llu proved; verify: %llu checked, %llu failed; storage refreshes reused %llu\n", static_cast<unsigned long long>(reuses.load()), static_cast<unsigned long long>(proofs.load()), static_cast<unsigned long long>(checks.load()), static_cast<unsigned long long>(failures.load()), static_cast<unsigned long long>(StorageTexture::epochRefreshes.load()));
}
}

const char* ShaderResources::FastFailName(std::size_t reason) {
    return reason < FastFailNames.size() ? FastFailNames[reason] : "none";
}

ShaderResources::ProofExtent ShaderResources::Extent() const {
    ProofExtent extent;
    for (const auto& region : directRegions) {
        ++extent.directRegions;
        extent.directBytes += region.end - region.begin;
    }
    for (const auto& range : describedRanges) {
        ++extent.describedRanges;
        extent.describedBytes += range.bytes;
    }
    for (const auto& image : storageTextures) {
        ++extent.storageImages;
        extent.storageBytes += image->GuestBytes();
    }
    return extent;
}

namespace {
bool quietProofsActive();
bool pendingScanVerifyNow();
// C_REPLAY_QUIET_TEMPLATE whole-proof counts: 0 hits, 1 VERIFY checks, 2 VERIFY mismatches (returns the previous count).
std::uint64_t quietProofCount(int which);
}

namespace {
bool ComputeCensus() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_C_COMPUTE_CENSUS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct ProofCensusNames {
    const char* calls;
    const char* ns;
};

ProofCensusNames proofCensusNames(const ShaderResources::ProofReport& report, bool ok) {
    static constexpr std::array<ProofCensusNames, 6> fixed{{{"c8_rv_quiet_n", "c8_rv_quiet_ns"}, {"c8_rv_epoch_n", "c8_rv_epoch_ns"}, {"c8_rv_stamp_n", "c8_rv_stamp_ns"}, {"c8_rv_fast_n", "c8_rv_fast_ns"}, {"c8_rv_own_n", "c8_rv_own_ns"}, {"c8_rv_failed_n", "c8_rv_failed_ns"}}};
    static const auto full = [] {
        std::array<ProofCensusNames, static_cast<std::size_t>(ShaderResources::FastFail::Count) + 1> names{};
        for (std::size_t i = 0; i < names.size(); ++i) {
            const char* reason = i < static_cast<std::size_t>(ShaderResources::FastFail::Count) ? ShaderResources::FastFailName(i) : "none";
            names[i] = {(new std::string(std::string("c8_rv_full_") + reason + "_n"))->c_str(), (new std::string(std::string("c8_rv_full_") + reason + "_ns"))->c_str()};
        }
        return names;
    }();
    if (!ok) return fixed[5];
    if (report.quiet) return fixed[0];
    if (report.epochReused) return fixed[1];
    if (report.stampReused) return fixed[2];
    if (report.path == ShaderResources::ProofPath::OwnRefreshed) return fixed[4];
    if (report.path == ShaderResources::ProofPath::Full) return full[std::min<std::size_t>(report.fastFail, full.size() - 1)];
    return fixed[3];
}
}

bool ShaderResources::Revalidate(std::span<const CompiledShader> shaders, ProofReport* report) {
    if (!ComputeCensus()) return revalidateCensused(shaders, report);
    ProofReport local;
    auto* used = report != nullptr ? report : &local;
    const auto start = std::chrono::steady_clock::now();
    const bool ok = revalidateCensused(shaders, used);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    const auto names = proofCensusNames(*used, ok);
    AgcDriver::ArchB::Recorder::Count(names.calls);
    AgcDriver::ArchB::Recorder::Count(names.ns, static_cast<double>(ns));
    return ok;
}

bool ShaderResources::revalidateCensused(std::span<const CompiledShader> shaders, ProofReport* report) {
    if (ArchB::Switches::Enabled(ArchB::Feature::DepthArrayGpu)) {
        for (const auto& [address, bytes] : depthArrayRanges) {
            if (!DepthStalePlaneOverlaps(address, address + bytes)) continue;
            if (report != nullptr) {
                *report = {ProofPath::Full, ProofFailure::Other};
                report->detail.check = "depth-array-refresh";
            }
            provedEpoch = 0;
            return false;
        }
    }
    PerformanceTimer recorderTiming("Resources.Proof", AgcDriver::ArchB::Recorder::Phase::Proof);
    const GuestMemory::Gen::ConsumerScope genConsumer(GuestMemory::Gen::Consumer::ResourceProof);
    if (!registeredImageViewsCurrent(registeredImageViews)) {
        if (report != nullptr) {
            *report = {ProofPath::Full, ProofFailure::Other};
            if (AgcDriver::DispatchBuildTrace::Enabled()) report->detail.check = "registered-image-views-changed";
        }
        provedEpoch = 0;
        return false;
    }
    static const int trustScope = [] {
        const char* value = std::getenv("APS5_UB_TRUST_PROOFS_SCOPE");
        return value == nullptr ? 0 : std::strcmp(value, "committer") == 0 ? 1 : std::strcmp(value, "other") == 0 ? 2 : 0;
    }();
    const bool trustProofs = ArchB::Switches::Enabled(ArchB::Feature::UbTrustProofs) && (trustScope == 0 || (trustScope == 1) == GuestMemory::IsCommitterThread());
    if (trustProofs && provedOnce && HasSurfaceTracking(validatedTextures, storageTextures)) {
        ArchB::Switches::CountUpperBoundHit(ArchB::Feature::UbTrustProofs);
        if (report != nullptr) *report = {ProofPath::Full, ProofFailure::None};
        return true;
    }
    if (GuestMemory::ResourceEpochProofsEnabled()) return revalidateFull(shaders, report, true);
    // C_REPLAY_QUIET_TEMPLATE: the whole pass skipped while its quiet proof holds (see QuietProof); VERIFY runs
    // the pass and requires a stamp-memo pass that did nothing.
    if (!EpochProofs() && EpochRevalidate() && reusable && !shaders.empty() && quietProofsActive() && GuestMemory::GpuMutex().HeldByThisThread() && quietProofHolds() && guestMemory.StagingDecisionsCurrent()) {
        if (pendingScanVerifyNow()) {
            quietProofCount(1);
            const bool ok = revalidateFull(shaders, report);
            if (!ok || !lastProofQuiet.load(std::memory_order_relaxed)) {
                const auto previous = quietProofCount(2);
                if (previous < 8) std::fprintf(stderr, "[archc] quiet proof VERIFY mismatch: the pass %s while the quiet proof held\n", ok ? "did work" : "failed");
            }
            return ok;
        }
        quietProofCount(0);
        if (auto* recorder = Recorder::Active(); recorder != nullptr) {
            try {
                guestMemory.RecordStagingCopies(*recorder);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "[resources] staging copies of a reused build failed: %s\n", error.what());
                quietProof.valid = false;
                setStampValid(false);
                AgcDriver::ArchB::Recorder::Count("proof_failed");
                if (report != nullptr) {
                    *report = {ProofPath::Fast, ProofFailure::Other};
                    if (AgcDriver::DispatchBuildTrace::Enabled()) report->detail = {"staging-record-failed", "guest-buffer"};
                }
                return false;
            }
        }
        pendingSerialSeen = StorageTexture::PendingSerial();
        provedOnce = true;
        AgcDriver::ArchB::Recorder::Count("proof_fast_pass_quiet");
        if (report != nullptr) {
            *report = {ProofPath::Fast, ProofFailure::None};
            report->quiet = true;
        }
        return true;
    }
    if (EpochProofs()) {
        const auto epoch = GuestMemory::CollectEpoch();
        const auto proof = GuestMemory::ProofSerial();
        const auto pending = StorageTexture::PendingSerial();
        const bool partial = std::any_of(validatedTextures.begin(), validatedTextures.end(), [](const auto& surface) { return surface.committed.has_value(); });
        const auto before = partial ? currentStampSerials() : ResourceEpochProof{};
        if (epoch != 0 && provedEpoch == epoch && provedProof == proof && provedPending == pending && (partial ? resourceEpochProof == before && PartialSnapshotsUnchanged(validatedTextures) && !HoldsReleasedTexture() && HasCachedStorageImages() : canRememberResourceEpoch())) {
            static const bool verify = std::getenv("APS5_VERIFY_DRAW_PIPELINE") != nullptr;
            struct ReusesThreadStorage {};
            auto& reuses = AgcDriver::ThreadScratch<std::uint64_t, ReusesThreadStorage>();
            if (verify && ++reuses % 64u == 0u) {
                const bool ok = revalidateFull(shaders, report);
                noteEpochProof(true, true, !ok);
                return ok;
            }
            if (report != nullptr) *report = {ProofPath::Full, ProofFailure::None};
            AgcDriver::ArchB::Recorder::Count("proof_fast_pass_epoch_memo");
            noteEpochProof(true, false, false);
            return true;
        }
        // Write-generation shadow: a memo refused only because a global serial moved (same collect
        // epoch), whose re-proof then passed on the tracker's footprint evidence (gen_shadow.
        // resource_proof.global_only_refusals: what footprint-local evidence would have kept).
        const bool globalOnly = epoch != 0 && provedEpoch == epoch && (provedProof != proof || provedPending != pending);
        const bool ok = revalidateFull(shaders, report);
        if (globalOnly && ok && GuestMemory::Gen::ShadowActive()) GuestMemory::Gen::NoteGlobalOnlyRefusal();
        noteEpochProof(false, false, false);
        if (ok && canRememberResourceEpoch() && (!partial || ((before.forgotten & 1u) == 0 && currentStampSerials() == before))) {
            provedEpoch = epoch;
            provedProof = proof;
            provedPending = pending;
            resourceEpochProof = before;
        } else {
            provedEpoch = 0;
        }
        return ok;
    }
    return revalidateFull(shaders, report);
}

ShaderResources::ResourceEpochProof ShaderResources::currentResourceEpochProof() {
#ifdef _WIN32
    const auto committed = GuestArena::GuestArenaCommitGeneration_nid_postfix();
#else
    const std::uint64_t committed = 0;
#endif
    return {GuestMemory::CollectEpoch(), GuestMemory::ResourceProofSerial(), StorageTexture::PendingSerial(), GuestMemory::TrackerGeneration(), GuestAllocations::GuestAllocationsGeneration_nid_postfix(), committed, GuestMemory::ForgetSerial(), GuestMemory::UnwatchSerial(), DepthSurfacesSerial(), DepthSurfacesReleased()};
}

namespace {
// Stage 4.2b: stamp-memo outcomes (recorder counters pipe_proof_memo_*).
enum class StampRefusal : std::size_t { Cold, Unlocked, Audit, Serials, Cache, Images, Imports, Stored, Stamped, Partial, Pending, Forgetting, Unwatched, Depth, Commits, Count };
// Why a passed proof was not kept: the serials or imports moved during it, a forget was under way,
// or the object is not eligible (canRememberResourceEpoch: depth, partial, untracked, records).
enum class StampSkip : std::size_t { Moved, Forgetting, Ineligible, Count };
struct StampProofCounters {
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> remembered{0};
    std::atomic<std::uint64_t> verified{0};
    std::atomic<std::uint64_t> mismatches{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(StampRefusal::Count)> refusals{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(StampSkip::Count)> skips{};
    std::array<std::atomic<std::uint64_t>, 5> ineligible{};
    std::atomic<std::uint64_t> pendingScans{0};
    // Proofs reaching revalidateFull by thread: the committer, another thread holding the GPU mutex,
    // another thread without it (the memo needs the mutex); memo hits off the committer.
    std::atomic<std::uint64_t> callsCommitter{0};
    std::atomic<std::uint64_t> callsOtherLocked{0};
    std::atomic<std::uint64_t> callsOtherUnlocked{0};
    std::atomic<std::uint64_t> hitsOther{0};
};

StampProofCounters& stampProofCounters() {
    static auto* made = [] {
        auto* counters = new StampProofCounters();
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_proof_memo_hits", counters->hits);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_proof_memo_remembered", counters->remembered);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_proof_memo_verified", counters->verified);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_proof_memo_mismatches", counters->mismatches);
        static constexpr std::array<const char*, static_cast<std::size_t>(StampRefusal::Count)> names{"pipe_proof_memo_cold", "pipe_proof_memo_unlocked", "pipe_proof_memo_audit", "pipe_proof_memo_serials", "pipe_proof_memo_cache", "pipe_proof_memo_images", "pipe_proof_memo_imports", "pipe_proof_memo_stored", "pipe_proof_memo_stamped", "pipe_proof_memo_partial", "pipe_proof_memo_pending", "pipe_proof_memo_forgetting", "pipe_proof_memo_unwatched", "pipe_proof_memo_depth", "pipe_proof_memo_commits"};
        for (std::size_t i = 0; i < names.size(); ++i) AgcDriver::ArchB::Recorder::RegisterCounter(names[i], counters->refusals[i]);
        static constexpr std::array<const char*, static_cast<std::size_t>(StampSkip::Count)> skipNames{"pipe_proof_memo_skip_moved", "pipe_proof_memo_skip_forgetting", "pipe_proof_memo_skip_ineligible"};
        for (std::size_t i = 0; i < skipNames.size(); ++i) AgcDriver::ArchB::Recorder::RegisterCounter(skipNames[i], counters->skips[i]);
        static constexpr std::array<const char*, 5> ineligibleNames{"pipe_proof_memo_ineligible_records", "pipe_proof_memo_ineligible_depth", "pipe_proof_memo_ineligible_partial", "pipe_proof_memo_ineligible_tracking", "pipe_proof_memo_ineligible_cache"};
        for (std::size_t i = 0; i < ineligibleNames.size(); ++i) AgcDriver::ArchB::Recorder::RegisterCounter(ineligibleNames[i], counters->ineligible[i]);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_proof_memo_pending_scans", counters->pendingScans);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_proof_calls_committer", counters->callsCommitter);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_proof_calls_other_locked", counters->callsOtherLocked);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_proof_calls_other_unlocked", counters->callsOtherUnlocked);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_proof_memo_hits_other", counters->hitsOther);
        return counters;
    }();
    return *made;
}

bool stampProofsActive() {
    return ArchB::Switches::Enabled(ArchB::Feature::PipeResolveProofs) && ArchB::Switches::Enabled(ArchB::Feature::WtFault);
}

bool stampProofsVerify() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_PIPE_RESOLVE_PROOFS_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

enum class TemplateTokenRefusal : std::size_t { None, Generation, Drain, Epoch, Loss, Open, Count };
constexpr std::array<const char*, static_cast<std::size_t>(TemplateTokenRefusal::Count)> TemplateTokenRefusalNames{"none", "generation", "drain", "epoch", "loss", "open"};

struct TemplateTokenCounters {
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> made{0};
    std::atomic<std::uint64_t> verifyChecks{0};
    std::atomic<std::uint64_t> verifyMismatches{0};
    std::atomic<std::uint64_t> attempts{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(TemplateTokenRefusal::Count)> refused{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(TemplateTokenRefusal::Count)> notMade{};
};

TemplateTokenCounters& templateTokenCounters() {
    static auto* made = [] {
        auto* counters = new TemplateTokenCounters();
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_template_token_hits", counters->hits);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_template_tokens", counters->made);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_template_token_verify_checks", counters->verifyChecks);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_template_token_verify_mismatches", counters->verifyMismatches);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_template_token_attempts", counters->attempts);
        for (std::size_t i = 0; i < TemplateTokenRefusalNames.size(); ++i) {
            AgcDriver::ArchB::Recorder::RegisterCounter((new std::string(std::string("archc_template_token_refused_") + TemplateTokenRefusalNames[i]))->c_str(), counters->refused[i]);
            AgcDriver::ArchB::Recorder::RegisterCounter((new std::string(std::string("archc_template_token_unmade_") + TemplateTokenRefusalNames[i]))->c_str(), counters->notMade[i]);
        }
        return counters;
    }();
    return *made;
}

bool templateTokensActive() {
    return ArchB::Switches::Enabled(ArchB::Feature::ArchcTemplateToken) && ArchB::Switches::Enabled(ArchB::Feature::ArchcImageEpochs);
}

std::uint64_t templateTokenVerifyPeriod() {
    static const std::uint64_t period = [] {
        const char* value = std::getenv("APS5_ARCHB_C_TEMPLATE_TOKEN_VERIFY");
        return value == nullptr ? std::uint64_t{0} : static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10));
    }();
    return period;
}

bool templateTokenVerifyNow() {
    const auto period = templateTokenVerifyPeriod();
    if (period == 0) return false;
    struct HitsThreadStorage {};
    auto& hits = AgcDriver::ThreadScratch<std::uint64_t, HitsThreadStorage>();
    return ++hits % period == 0;
}

constexpr std::uint32_t TemplateTokenSources = ArchC::SourceBit(ArchC::EpochSource::CpuStore) | ArchC::SourceBit(ArchC::EpochSource::Hold) | ArchC::SourceBit(ArchC::EpochSource::Open) | ArchC::SourceBit(ArchC::EpochSource::Forget) | ArchC::SourceBit(ArchC::EpochSource::TrackerStamp) | ArchC::SourceBit(ArchC::EpochSource::Depth) | ArchC::SourceBit(ArchC::EpochSource::Unwatch);
}

namespace {
struct PendingScanCounters {
    std::atomic<std::uint64_t> hits{0}, live{0}, made{0}, verifyChecks{0}, verifyMismatches{0};
    std::array<std::atomic<std::uint64_t>, 3> refused{};
    std::atomic<std::uint64_t> regions{0}, scans{0};
};

PendingScanCounters& pendingScanCounters() {
    static auto* made = [] {
        auto* counters = new PendingScanCounters();
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_replay_p3b_quiet_template_hits", counters->hits);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_replay_p3b_quiet_template_live", counters->live);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_replay_p3b_quiet_template_made", counters->made);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_replay_p3b_quiet_template_verify_checks", counters->verifyChecks);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_replay_p3b_quiet_template_verify_mismatches", counters->verifyMismatches);
        static constexpr std::array<const char*, 3> refusedNames{"archc_replay_p3b_quiet_template_refused_drain", "archc_replay_p3b_quiet_template_refused_epoch", "archc_replay_p3b_quiet_template_refused_loss"};
        for (std::size_t i = 0; i < refusedNames.size(); ++i) AgcDriver::ArchB::Recorder::RegisterCounter(refusedNames[i], counters->refused[i]);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_replay_p3b_quiet_template_regions", counters->regions);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_replay_p3b_quiet_template_scans", counters->scans);
        return counters;
    }();
    return *made;
}

bool pendingScanTokensActive() {
    return ArchB::Switches::Enabled(ArchB::Feature::ArchcReplayQuietTemplate) && ArchB::Switches::Enabled(ArchB::Feature::ArchcImageEpochs);
}

bool quietProofsActive() {
    return pendingScanTokensActive() && !stampProofsVerify() && stampProofsActive();
}

struct QuietProofCounters {
    std::atomic<std::uint64_t> hits{0}, made{0}, verifyChecks{0}, verifyMismatches{0};
};

QuietProofCounters& quietProofCounters() {
    static auto* made = [] {
        auto* counters = new QuietProofCounters();
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_replay_p3b_quiet_proof_hits", counters->hits);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_replay_p3b_quiet_proof_made", counters->made);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_replay_p3b_quiet_proof_verify_checks", counters->verifyChecks);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_replay_p3b_quiet_proof_verify_mismatches", counters->verifyMismatches);
        return counters;
    }();
    return *made;
}

std::uint64_t quietProofCount(int which) {
    auto& counters = quietProofCounters();
    auto& counter = which == 0 ? counters.hits : which == 1 ? counters.verifyChecks : counters.verifyMismatches;
    return counter.fetch_add(1, std::memory_order_relaxed);
}

bool pendingScanVerifyNow() {
    static const std::uint64_t period = [] {
        const char* value = std::getenv("APS5_ARCHB_C_REPLAY_QUIET_TEMPLATE_VERIFY");
        return value == nullptr ? std::uint64_t{0} : static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10));
    }();
    if (period == 0) return false;
    static std::atomic<std::uint64_t> uses{0};
    return (uses.fetch_add(1, std::memory_order_relaxed) + 1) % period == 0;
}
}

bool ShaderResources::pendingScanHolds() {
    if (!pendingScanToken.valid) return false;
    auto& refused = pendingScanCounters().refused;
    const auto refuse = [&](std::size_t why) {
        refused[why].fetch_add(1, std::memory_order_relaxed);
        pendingScanToken.valid = false;
        return false;
    };
    if (!ArchC::ImageEpochs::Get().DrainIfNew()) return refuse(0);
    if (pendingScanEpoch.load(std::memory_order_acquire) != pendingScanToken.epoch) return refuse(1);
    if (ArchC::ImageEpochs::Get().Loss() != pendingScanToken.loss) return refuse(2);
    return true;
}

bool ShaderResources::quietProofHolds() {
    if (!quietProof.valid) return false;
    const auto refuse = [&] {
        quietProof.valid = false;
        return false;
    };
    if (!stampProof.valid || stampProof.generation != quietProof.generation) return refuse();
    if (TemplateAuditDue(lastFullWalk) || AnyPendingCpuWrites(context)) return false;
    const auto forgotten = GuestMemory::ForgetSerial();
    if ((forgotten & 1u) != 0 || forgotten != quietProof.forgotten || GuestMemory::UnwatchSerial() != quietProof.unwatched || DepthSurfacesSerial() != quietProof.depth || DepthSurfacesReleased() != quietProof.depthReleased || DepthStalePlanesSerial() != quietProof.stalePlanes) return refuse();
    if ((!directRegions.empty() || !guestMemory.Writes().empty()) && !(pendingSerialSeen != 0 && pendingSerialSeen == StorageTexture::PendingSerial()) && !pendingScanHolds()) return refuse();
    if (!HostImportsUnchanged(context, quietProof.imports)) return refuse();
    return true;
}

bool ShaderResources::archcTokenHolds() {
    auto& counters = templateTokenCounters();
    const auto refuse = [&](TemplateTokenRefusal why) {
        archcToken.valid = false;
        counters.refused[static_cast<std::size_t>(why)].fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    if (!archcToken.valid) return false;
    if (archcToken.generation != stampProof.generation) return refuse(TemplateTokenRefusal::Generation);
    if (!ArchC::ImageEpochs::Get().DrainIfNew()) return refuse(TemplateTokenRefusal::Drain);
    if (archcEpoch.load(std::memory_order_acquire) != archcToken.epoch) return refuse(TemplateTokenRefusal::Epoch);
    if (ArchC::ImageEpochs::Get().Loss() != archcToken.loss) return refuse(TemplateTokenRefusal::Loss);
    if (templateTokenVerifyNow()) {
        counters.verifyChecks.fetch_add(1, std::memory_order_relaxed);
        bool unchanged = true;
        struct VerifySlotStorage {};
        auto& queries = AgcDriver::ThreadScratch<std::vector<GuestMemory::UnchangedQuery>, VerifySlotStorage>();
        queries.clear();
        for (const auto& [begin, end] : stampProof.ranges) {
            const auto bytes = static_cast<std::size_t>(end - begin);
            if (!GuestMemory::FaultUnchangedSince(begin, bytes, stampProof.generation)) unchanged = false;
            queries.push_back({begin, bytes, stampProof.generation});
        }
        if (unchanged && !queries.empty() && !GuestMemory::UnchangedSinceAll(queries)) unchanged = false;
        if (!unchanged) {
            const auto previous = counters.verifyMismatches.fetch_add(1, std::memory_order_relaxed);
            if (previous < 8) std::fprintf(stderr, "[archc] template token VERIFY mismatch: the stamp proof's ranges changed while the token held\n");
            archcToken.valid = false;
            return false;
        }
    }
    counters.hits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

ArchcTemplateTokenTotals ArchcTemplateTokenCounts() {
    auto& counters = templateTokenCounters();
    std::uint64_t unmade = 0;
    for (const auto& count : counters.notMade) unmade += count.load();
    return {counters.hits.load(), counters.made.load(), counters.verifyChecks.load(), counters.verifyMismatches.load(), counters.attempts.load(), unmade, counters.notMade[static_cast<std::size_t>(TemplateTokenRefusal::Open)].load()};
}

void ShaderResources::archcTokenMake(bool drained, std::uint64_t epochBefore, std::uint64_t lossBefore) {
    auto& counters = templateTokenCounters();
    const auto unmade = [&](TemplateTokenRefusal why) { counters.notMade[static_cast<std::size_t>(why)].fetch_add(1, std::memory_order_relaxed); };
    archcToken.valid = false;
    if (!drained || !ArchC::ImageEpochs::Get().DrainIfNew()) return unmade(TemplateTokenRefusal::Drain);
    if (archcEpoch.load(std::memory_order_acquire) != epochBefore) return unmade(TemplateTokenRefusal::Epoch);
    if (ArchC::ImageEpochs::Get().Loss() != lossBefore) return unmade(TemplateTokenRefusal::Loss);
    const auto clock = GuestArena::GuestWtFaultClock_nid_no_patch();
    for (const auto& [begin, end] : stampProof.ranges) {
        if (GuestArena::GuestWtFaultChangedSince_nid_no_patch(static_cast<std::uintptr_t>(begin), static_cast<std::size_t>(end - begin), clock) != GuestArena::WtFaultAnswer::Unchanged) return unmade(TemplateTokenRefusal::Open);
    }
    archcToken.valid = true;
    archcToken.epoch = epochBefore;
    archcToken.loss = lossBefore;
    archcToken.generation = stampProof.generation;
    counters.made.fetch_add(1, std::memory_order_relaxed);
}

ShaderResources::ResourceEpochProof ShaderResources::currentStampSerials() {
#ifdef _WIN32
    const auto committed = GuestArena::GuestArenaCommitGeneration_nid_postfix();
#else
    const std::uint64_t committed = 0;
#endif
    return {0, 0, StorageTexture::PendingSerial(), 0, GuestAllocations::GuestAllocationsGeneration_nid_postfix(), committed, GuestMemory::ForgetSerial(), GuestMemory::UnwatchSerial(), DepthSurfacesSerial(), DepthSurfacesReleased()};
}

namespace {
struct StampScanCounters {
    std::atomic<std::uint64_t> hits{0}, scans{0}, made{0}, refusedEpoch{0}, refusedDrain{0}, identity{0}, verified{0}, mismatches{0};
};

StampScanCounters& stampScanCounters() {
    static auto* made = [] {
        auto* c = new StampScanCounters();
        const std::pair<const char*, std::atomic<std::uint64_t>*> named[] = {
            {"c_stamp_scan_token_hits", &c->hits}, {"c_stamp_scan_token_scans", &c->scans}, {"c_stamp_scan_token_made", &c->made},
            {"c_stamp_scan_token_refused_epoch", &c->refusedEpoch}, {"c_stamp_scan_token_refused_drain", &c->refusedDrain}, {"c_stamp_scan_token_identity", &c->identity},
            {"c_stamp_scan_token_verified", &c->verified}, {"c_stamp_scan_token_verify_mismatches", &c->mismatches}};
        for (const auto& [name, counter] : named) AgcDriver::ArchB::Recorder::RegisterCounter(name, *counter);
        return c;
    }();
    return *made;
}

bool stampScanTokensActive() {
    return ArchB::Switches::Enabled(ArchB::Feature::CStampScanToken) && ArchB::Switches::Enabled(ArchB::Feature::ArchcImageEpochs);
}

bool stampScanVerifyNow() {
    static const std::uint64_t period = [] {
        const char* value = std::getenv("APS5_ARCHB_C_STAMP_SCAN_TOKEN_VERIFY");
        return value == nullptr ? std::uint64_t{0} : static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10));
    }();
    if (period == 0) return false;
    static std::atomic<std::uint64_t> uses{0};
    return (uses.fetch_add(1, std::memory_order_relaxed) + 1) % period == 0;
}
}

bool ShaderResources::stampScanPending(bool& overlaps) {
    overlaps = false;
    const auto scanNow = [&] {
        struct ScanSlotStorage {};
        auto& scan = AgcDriver::ThreadScratch<std::vector<StorageTexture::PendingQuery>, ScanSlotStorage>();
        scan.assign(stampProof.pending.begin(), stampProof.pending.end());
        stampProofCounters().pendingScans.fetch_add(1, std::memory_order_relaxed);
        if (!StorageTexture::ScanPending(scan)) return false;
        overlaps = std::any_of(scan.begin(), scan.end(), [](const StorageTexture::PendingQuery& query) { return query.overlaps; });
        return true;
    };
    if (!stampScanTokensActive()) return scanNow();
    auto& c = stampScanCounters();
    if (std::any_of(stampProof.pending.begin(), stampProof.pending.end(), [](const StorageTexture::PendingQuery& query) { return query.pending != nullptr; })) {
        c.identity.fetch_add(1, std::memory_order_relaxed);
        stampScanToken.valid = false;
        return scanNow();
    }
    if (stampScanToken.valid) {
        if (!ArchC::ImageEpochs::Get().DrainIfNew()) {
            c.refusedDrain.fetch_add(1, std::memory_order_relaxed);
            stampScanToken.valid = false;
        } else if (stampScanEpoch.load(std::memory_order_acquire) != stampScanToken.epoch || ArchC::ImageEpochs::Get().Loss() != stampScanToken.loss) {
            c.refusedEpoch.fetch_add(1, std::memory_order_relaxed);
            stampScanToken.valid = false;
        } else if (!stampScanVerifyNow()) {
            c.hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        } else {
            c.verified.fetch_add(1, std::memory_order_relaxed);
            const bool scanned = scanNow();
            if (!scanned || overlaps) {
                const auto count = c.mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count <= 8) std::fprintf(stderr, "[archc] C_STAMP_SCAN_TOKEN VERIFY mismatch: the scan %s while the token held\n", scanned ? "found an overlap" : "failed");
                stampScanToken.valid = false;
            }
            return scanned;
        }
    }
    if (!stampScanToken.registered) {
        struct RangesSlotStorage {};
        auto& ranges = AgcDriver::ThreadScratch<std::vector<ArchC::EpochRange>, RangesSlotStorage>();
        ranges.clear();
        for (const auto& query : stampProof.pending) ranges.push_back({query.begin, query.end});
        ArchC::ImageEpochs::Get().RegisterOwner(&stampScanEpoch, stampScanEpoch, ranges, ArchC::SourceBit(ArchC::EpochSource::PendingInsert));
        stampScanToken.registered = true;
    }
    const bool drained = ArchC::ImageEpochs::Get().DrainIfNew();
    const auto epoch = stampScanEpoch.load(std::memory_order_acquire);
    const auto loss = ArchC::ImageEpochs::Get().Loss();
    c.scans.fetch_add(1, std::memory_order_relaxed);
    if (!scanNow()) return false;
    if (!overlaps && drained && ArchC::ImageEpochs::Get().DrainIfNew() && stampScanEpoch.load(std::memory_order_acquire) == epoch && ArchC::ImageEpochs::Get().Loss() == loss) {
        stampScanToken = {true, true, epoch, loss};
        c.made.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool ShaderResources::stampProofHolds() {
    auto& counters = stampProofCounters();
    const auto refuse = [&](StampRefusal reason) {
        counters.refusals[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    if (!stampProof.valid) return refuse(StampRefusal::Cold);
    if (!GuestMemory::GpuMutex().HeldByThisThread()) return refuse(StampRefusal::Unlocked);
    if (TemplateAuditDue(lastFullWalk)) return refuse(StampRefusal::Audit);
    // Serials: a forget under way refuses; mapping, commit and forget changes elsewhere are left to the
    // per-range fault check below (a mapping or protection change forgets its pages, which then read as
    // untracked), except the commit generation while partial snapshots are covered (their committed
    // layout); unwatches and depth-surface changes refuse; a pending-registry change rescans.
    const auto now = currentStampSerials();
    if ((now.forgotten & 1u) != 0) return refuse(StampRefusal::Forgetting);
    if (now.unwatched != stampProof.serials.unwatched) return refuse(StampRefusal::Unwatched);
    if (now.depth != stampProof.serials.depth || now.depthReleased != stampProof.serials.depthReleased) return refuse(StampRefusal::Depth);
    if (!stampProof.partials.empty() && now.commits != stampProof.serials.commits) return refuse(StampRefusal::Commits);
    if (HoldsReleasedTexture() || !HasCachedStorageImages()) return refuse(StampRefusal::Cache);
    for (const auto& entry : stampProof.images) {
        if (entry.image->Version() != entry.version || entry.image->Descriptor().dccAddress != entry.keys) return refuse(StampRefusal::Images);
    }
    if (HostImportsIdentity(context, false) != stampProof.imports) return refuse(StampRefusal::Imports);
    if (now.pending != stampProof.serials.pending && !stampProof.pending.empty()) {
        bool overlaps = false;
        if (!stampScanPending(overlaps) || overlaps) return refuse(StampRefusal::Pending);
    }
    for (const auto index : stampProof.partials) {
        const auto& surface = validatedTextures[index];
        const auto address = surface.resource.baseAddress;
        const auto bytes = static_cast<std::size_t>(surface.bytes);
        if (SampledFromStorageEligible(context, surface.resource, surface.bytes) || Recorder::SnapshotWriteOverlaps(address, bytes) || AnyShadowedOverlaps(address, bytes)) return refuse(StampRefusal::Partial);
    }
    const bool tokens = templateTokensActive() && stampProof.partials.empty() && !stampProof.ranges.empty();
    if (tokens && archcTokenHolds()) {
        counters.hits.fetch_add(1, std::memory_order_relaxed);
        if (!GuestMemory::IsCommitterThread()) counters.hitsOther.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    bool tokenDrained = false;
    std::uint64_t tokenEpoch = 0;
    std::uint64_t tokenLoss = 0;
    if (tokens) {
        templateTokenCounters().attempts.fetch_add(1, std::memory_order_relaxed);
        if (!archcToken.registered || archcRanges != stampProof.ranges) {
            struct RangesSlotStorage {};
            auto& ranges = AgcDriver::ThreadScratch<std::vector<ArchC::EpochRange>, RangesSlotStorage>();
            ranges.clear();
            for (const auto& [begin, end] : stampProof.ranges) ranges.push_back({begin, end});
            ArchC::ImageEpochs::Get().RegisterOwner(this, archcEpoch, ranges, TemplateTokenSources);
            archcToken.registered = true;
            archcRanges = stampProof.ranges;
        }
        tokenDrained = ArchC::ImageEpochs::Get().DrainIfNew();
        tokenEpoch = archcEpoch.load(std::memory_order_acquire);
        tokenLoss = ArchC::ImageEpochs::Get().Loss();
    }
    struct QueriesSlotStorage {};
    auto& queries = AgcDriver::ThreadScratch<std::vector<GuestMemory::UnchangedQuery>, QueriesSlotStorage>();
    queries.clear();
    for (const auto& [begin, end] : stampProof.ranges) {
        const auto bytes = static_cast<std::size_t>(end - begin);
        if (!GuestMemory::FaultUnchangedSince(begin, bytes, stampProof.generation)) return refuse(StampRefusal::Stored);
        queries.push_back({begin, bytes, stampProof.generation});
    }
    if (!queries.empty() && !GuestMemory::UnchangedSinceAll(queries)) return refuse(StampRefusal::Stamped);
    if (tokens) archcTokenMake(tokenDrained, tokenEpoch, tokenLoss);
    counters.hits.fetch_add(1, std::memory_order_relaxed);
    if (!GuestMemory::IsCommitterThread()) counters.hitsOther.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ShaderResources::rememberStampProof(std::uint64_t generation, const ResourceEpochProof& serials, const HostImportsProof& imports) {
    setStampValid(false);
    archcToken.valid = false;
    stampProof.images.clear();
    stampProof.ranges.clear();
    auto& skips = stampProofCounters().skips;
    if (generation == 0 || (serials.forgotten & 1u) != 0) {
        skips[static_cast<std::size_t>(StampSkip::Forgetting)].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!stampEligible()) {
        skips[static_cast<std::size_t>(StampSkip::Ineligible)].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    stampProof.partials.clear();
    stampProof.pending.clear();
    stampScanToken.valid = false;
    stampScanToken.registered = false;
    const auto range = [&](std::uint64_t address, std::uint64_t bytes) {
        if (address != 0 && bytes != 0) stampProof.ranges.emplace_back(address, address + bytes);
    };
    const auto image = [&](const StorageTexture* source) {
        if (source == nullptr) return;
        stampProof.images.push_back({source, source->Version(), source->Descriptor().dccAddress});
        const auto& own = source->Descriptor();
        range(own.baseAddress, source->GuestBytes());
        if (own.dccAddress != 0) range(own.dccAddress, DccKeyBytes(source->GuestBytes()));
    };
    for (std::size_t index = 0; index < validatedTextures.size(); ++index) {
        const auto& surface = validatedTextures[index];
        const auto begin = surface.resource.baseAddress;
        const auto end = begin + surface.bytes;
        stampProof.pending.push_back({begin, end, surface.source, surface.source != nullptr && surface.keys != DccKeys::Uncompressed ? surface.source : nullptr, false});
        if (surface.committed.has_value()) {
            for (const auto& [first, stop] : *surface.committed) range(first, stop - first);
            stampProof.partials.push_back(index);
            continue;
        }
        range(surface.resource.baseAddress, surface.bytes);
        if (surface.resource.dccAddress != 0) range(surface.resource.dccAddress, DccKeyBytes(surface.bytes));
        image(surface.source);
    }
    for (const auto& storage : storageTextures) {
        image(storage.get());
        if (storage != nullptr) stampProof.pending.push_back({storage->Descriptor().baseAddress, storage->Descriptor().baseAddress + storage->GuestBytes(), storage.get(), nullptr, false});
    }
    stampProof.generation = generation;
    stampProof.serials = serials;
    stampProof.imports = imports;
    setStampValid(true);
    stampProofCounters().remembered.fetch_add(1, std::memory_order_relaxed);
}

bool ShaderResources::stampEligible() const {
    if (validatedTextures.size() != textures.size()) return false;
    if (HoldsReleasedTexture() || !HasCachedStorageImages()) return false;
    const auto trackedKeys = [](const GuestTextureResource& resource, std::uint64_t bytes) {
        return resource.dccAddress == 0 || GuestMemory::Watched(resource.dccAddress, DccKeyBytes(bytes));
    };
    for (const auto& surface : validatedTextures) {
        if (!surface.valid || surface.depth || DepthSurfaceOverlaps(context.device, surface.resource.baseAddress, surface.bytes)) return false;
        if (surface.committed.has_value()) continue;
        if (!trackedKeys(surface.resource, surface.bytes) || (surface.source == nullptr ? surface.generation == 0 : surface.source->Generation() == 0)) return false;
        if (!GuestMemory::Watched(surface.resource.baseAddress, static_cast<std::size_t>(surface.bytes))) return false;
        if (surface.source != nullptr && !trackedKeys(surface.source->Descriptor(), surface.source->GuestBytes())) return false;
    }
    for (const auto& image : storageTextures) {
        if (image == nullptr) return false;
        if (DepthSurfaceOverlaps(context.device, image->Descriptor().baseAddress, image->GuestBytes())) return false;
        if (image->Generation() == 0 || !trackedKeys(image->Descriptor(), image->GuestBytes()) || !GuestMemory::Watched(image->Descriptor().baseAddress, static_cast<std::size_t>(image->GuestBytes()))) return false;
    }
    return HasSurfaceTracking(validatedTextures, storageTextures);
}

bool ShaderResources::canRememberResourceEpoch() const {
    const auto fail = [](EpochIneligible reason) {
        if (BuildProfiled()) Revalidations().epochIneligible[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
        if (stampProofsActive() && static_cast<std::size_t>(reason) < 5) stampProofCounters().ineligible[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    if (validatedTextures.size() != textures.size()) return fail(EpochIneligible::Records);
    if (HoldsReleasedTexture() || !HasCachedStorageImages()) return fail(EpochIneligible::Cache);
    const auto trackedKeys = [](const GuestTextureResource& resource, std::uint64_t bytes) {
        return resource.dccAddress == 0 || GuestMemory::Watched(resource.dccAddress, DccKeyBytes(bytes));
    };
    for (const auto& surface : validatedTextures) {
        if (!surface.valid) return fail(EpochIneligible::Records);
        if (surface.depth || DepthSurfaceOverlaps(context.device, surface.resource.baseAddress, surface.bytes)) return fail(EpochIneligible::Depth);
        if (surface.committed.has_value()) continue;
        if (!trackedKeys(surface.resource, surface.bytes) || (surface.source == nullptr ? surface.generation == 0 : surface.source->Generation() == 0)) return fail(EpochIneligible::Tracking);
        if (!GuestMemory::Watched(surface.resource.baseAddress, static_cast<std::size_t>(surface.bytes))) return fail(EpochIneligible::Tracking);
        if (surface.source != nullptr && !trackedKeys(surface.source->Descriptor(), surface.source->GuestBytes())) return fail(EpochIneligible::Tracking);
    }
    for (const auto& image : storageTextures) {
        if (image == nullptr) return fail(EpochIneligible::Records);
        if (DepthSurfaceOverlaps(context.device, image->Descriptor().baseAddress, image->GuestBytes())) return fail(EpochIneligible::Depth);
        if (image->Generation() == 0 || !trackedKeys(image->Descriptor(), image->GuestBytes()) || !GuestMemory::Watched(image->Descriptor().baseAddress, static_cast<std::size_t>(image->GuestBytes()))) return fail(EpochIneligible::Tracking);
    }
    if (!HasSurfaceTracking(validatedTextures, storageTextures)) return fail(EpochIneligible::Tracking);
    return true;
}

bool ShaderResources::revalidateFull(std::span<const CompiledShader> shaders, ProofReport* report, bool epochProofs) {
    const bool skipDispatchProofWalk = ArchB::Switches::Enabled(ArchB::Feature::UbSkipDispatchProofWalk);
    if (skipDispatchProofWalk && !drawBuild && HasSurfaceTracking(validatedTextures, storageTextures)) {
        ArchB::Switches::CountUpperBoundHit(ArchB::Feature::UbSkipDispatchProofWalk);
        if (report != nullptr) *report = {ProofPath::Full, ProofFailure::None};
        return true;
    }
    const bool traceStages = report != nullptr && report->trace;
    if (report != nullptr) *report = {ProofPath::Full, ProofFailure::Other};
    if (report != nullptr) report->trace = traceStages;
    auto stageLap = traceStages ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    double stageWaited = traceStages ? Recorder::ThreadWaitedMs() : 0.0;
    const auto markStage = [&](std::size_t stage) {
        if (!traceStages) return;
        const auto now = std::chrono::steady_clock::now();
        const auto waited = Recorder::ThreadWaitedMs();
        report->stageMs[stage] += std::chrono::duration<double, std::milli>(now - stageLap).count();
        report->stageWaitMs[stage] += waited - stageWaited;
        stageLap = now;
        stageWaited = waited;
    };
    const bool diagnose = report != nullptr && AgcDriver::DispatchBuildTrace::Enabled();
    const auto failed = [&](const char* check, const char* role = "unknown", std::size_t binding = std::numeric_limits<std::size_t>::max(), std::size_t element = std::numeric_limits<std::size_t>::max()) {
        if (diagnose && std::strcmp(report->detail.check, "not-recorded") == 0) report->detail = {check, role, binding, element};
        return false;
    };
    if (!reusable) return failed("not-reusable");
    if (ArchB::Switches::Enabled(ArchB::Feature::DepthArrayGpu)) {
        for (const auto& [address, bytes] : depthArrayRanges) {
            if (DepthStalePlaneOverlaps(address, address + bytes)) return failed("depth-array-refresh");
        }
    }
    if (shaders.empty()) return failed("missing-shaders");
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const bool stampProofs = !epochProofs && stampProofsActive() && GuestMemory::GpuMutex().HeldByThisThread();
    if (stampProofsActive()) {
        auto& counters = stampProofCounters();
        (GuestMemory::IsCommitterThread() ? counters.callsCommitter : GuestMemory::GpuMutex().HeldByThisThread() ? counters.callsOtherLocked : counters.callsOtherUnlocked).fetch_add(1, std::memory_order_relaxed);
    }
    const auto stampGeneration = stampProofs ? GuestMemory::WriteGeneration() : 0;
    const auto stampSerials = stampProofs ? currentStampSerials() : ResourceEpochProof{};
    const auto stampImports = stampProofs ? HostImportsIdentity(context, false) : HostImportsProof{};
    if (!stampProofs) setStampValid(false);
    ArchC::ProbeAt(ArchC::Probe::RvPrelude);
    // C_REPLAY_QUIET_TEMPLATE: whether this pass did anything a quiet proof would skip.
    bool proofWork = false;
    lastProofQuiet.store(false, std::memory_order_relaxed);
    for (const auto& region : directRegions) {
        if (!FlushDepthPlanes(region.begin, static_cast<std::size_t>(region.end - region.begin)).empty()) proofWork = true;
        if (CompletePendingCpuWrites(context, region.begin, static_cast<std::size_t>(region.end - region.begin))) proofWork = true;
    }
    for (const auto& range : describedRanges) {
        if (!range.imageTable) RequireNoMultisampleColorTarget(range.address, static_cast<std::size_t>(range.bytes), "a cached image access");
        CompletePendingCpuWrites(context, range.address, static_cast<std::size_t>(range.bytes));
        if (range.dccAddress != 0) CompletePendingCpuWrites(context, range.dccAddress, DccKeyBytes(range.bytes));
    }
    for (const auto& image : storageTextures) {
        CompletePendingCpuWrites(context, image->Descriptor().baseAddress, static_cast<std::size_t>(image->GuestBytes()));
        if (image->Descriptor().dccAddress != 0) CompletePendingCpuWrites(context, image->Descriptor().dccAddress, DccKeyBytes(image->GuestBytes()));
    }
    markStage(0);
    ArchC::ProbeAt(ArchC::Probe::RvComplete);
    // APS5_NO_FAST_REVALIDATE=1 always repeats the lookups.
    static const bool noFast = std::getenv("APS5_NO_FAST_REVALIDATE") != nullptr;
    const auto epochBefore = epochProofs ? currentResourceEpochProof() : ResourceEpochProof{};
    const auto importsBefore = epochProofs ? HostImportsIdentity(context, false) : HostImportsProof{};
    bool epochReused = false;
    if (epochProofs) {
        const auto refusal = [&] {
            if (!GuestMemory::GpuMutex().HeldByThisThread()) return EpochRefusal::Unlocked;
            if (noFast || !EpochRevalidate() || VerifyProofs()) return EpochRefusal::Disabled;
            if (epochBefore.epoch == 0) return EpochRefusal::NoEpoch;
            if (resourceEpochProof.epoch == 0) return EpochRefusal::Cold;
            if (resourceEpochProof.epoch != epochBefore.epoch) return EpochRefusal::Epoch;
            if (resourceEpochProof.writes != epochBefore.writes) return EpochRefusal::Writes;
            if (resourceEpochProof.pending != epochBefore.pending) return EpochRefusal::Pending;
            if (resourceEpochProof.allocations != epochBefore.allocations || resourceEpochProof.commits != epochBefore.commits || resourceEpochProof.forgotten != epochBefore.forgotten || (epochBefore.forgotten & 1u) != 0 || resourceEpochProof.unwatched != epochBefore.unwatched) return EpochRefusal::Mapping;
            if (resourceEpochProof.depth != epochBefore.depth || resourceEpochProof.depthReleased != epochBefore.depthReleased) return EpochRefusal::Depth;
            if (resourceEpochProof.tracker != epochBefore.tracker) return EpochRefusal::Tracker;
            if (!PartialSnapshotsUnchanged(validatedTextures)) return EpochRefusal::Tracker;
            if (HoldsReleasedTexture() || !HasCachedStorageImages()) return EpochRefusal::Cache;
            for (const auto& entry : epochImages) {
                if (entry.image->Version() != entry.version || entry.image->Descriptor().dccAddress != entry.keys) return EpochRefusal::Images;
            }
            if (epochImports != importsBefore) return EpochRefusal::Imports;
            if (TemplateAuditDue(lastFullWalk)) return EpochRefusal::Audit;
            static const bool verify = std::getenv("APS5_VERIFY_DRAW_PIPELINE") != nullptr;
            struct ReusesThreadStorage {};
            auto& reuses = AgcDriver::ThreadScratch<std::uint64_t, ReusesThreadStorage>();
            if (verify && ++reuses % 64u == 0u) return EpochRefusal::Verify;
            return EpochRefusal::Count;
        }();
        epochReused = refusal == EpochRefusal::Count;
        if (!epochReused) resourceEpochProof = {};
        if (profile) {
            const auto use = !drawBuild ? 1 : DrawPipelineEnabled() ? 2 : 0;
            if (epochBefore.epoch != 0 && profiledResourceEpoch == epochBefore.epoch) Revalidations().epochRepeats[use].fetch_add(1, std::memory_order_relaxed);
            profiledResourceEpoch = epochBefore.epoch;
            if (!epochReused) Revalidations().epochRefusals[static_cast<std::size_t>(refusal)].fetch_add(1, std::memory_order_relaxed);
        }
    }
    bool stampReused = false;
    const auto otherStarted = std::chrono::steady_clock::now();
    const auto finish = [&](bool fast, bool ok, ProofFailure failure = ProofFailure::Other) {
        if (ok) AgcDriver::ArchB::Recorder::Count(fast ? (epochReused ? "proof_fast_pass_epoch" : stampReused ? "proof_fast_pass_stamp_memo" : "proof_fast_pass_stamps") : "proof_full_pass");
        if (!ok) setStampValid(false);
        if (!ok) AgcDriver::ArchB::Recorder::Count("proof_failed");
        if (!GuestMemory::IsCommitterThread()) {
            AgcDriver::ArchB::Recorder::Count(!ok ? "proof_other_failed" : !fast ? "proof_other_full_pass" : epochReused ? "proof_other_pass_epoch" : stampReused ? "proof_other_pass_stamp_memo" : "proof_other_pass_stamps");
            AgcDriver::ArchB::Recorder::Count(drawBuild ? "proof_other_draw_resources" : "proof_other_dispatch_resources");
            AgcDriver::ArchB::Recorder::Count("proof_other_ns", static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - otherStarted).count()));
        }
        if (ok) provedOnce = true;
        if (epochProofs && !ok) resourceEpochProof = {};
        if (report != nullptr) report->epochReused = epochReused && ok;
        if (profile && epochReused) {
            if (ok) Revalidations().epochHits[!drawBuild ? 1 : DrawPipelineEnabled() ? 2 : 0].fetch_add(1, std::memory_order_relaxed);
            else Revalidations().epochRefusals[static_cast<std::size_t>(EpochRefusal::PerUse)].fetch_add(1, std::memory_order_relaxed);
        }
        if (profile) countRevalidate(fast, ok, start);
        if (report != nullptr) {
            report->failure = ok ? ProofFailure::None : failure;
            if (ok) report->detail = {};
        }
        return ok;
    };
    // Loaded before any check (the memo rule of fastRevalidate): kept as the memo only when the
    // registry did not move through the whole proof, flushes of this call included.
    const auto serialBefore = StorageTexture::PendingSerial();
    // (1) Textures and storage images, from stamps when every element allows it (fastRevalidate),
    // else by the same lookups as the build (which refresh or replace them as guest memory changed):
    // they must hand back the very objects the set's views belong to. The build appended them stage
    // by stage, binding by binding, so the walk repeats that order.
    const auto fullWalk = [&] {
        PerformanceTimer recorderTiming("Resources.FullWalk", AgcDriver::ArchB::Recorder::Phase::Proof);
        recorderTiming.RecorderCounter("full_walk_ms");
        AgcDriver::ArchB::Recorder::Count("full_walks");
        lastFullWalk = std::chrono::steady_clock::now();
        walkImportGeneration = HostImportSetGeneration();
        depthWalkSerial = 0;
        const auto walkSerial = DepthSurfacesSerial();
        const auto walkReleased = DepthSurfacesReleased();
        lookupLog().clear();
        BuildLookups lookups;
        std::size_t textureIndex = 0;
        std::size_t storageIndex = 0;
        for (const auto& shader : shaders) {
            if (shader.program == nullptr) return failed("missing-shader-program");
            for (const auto& binding : shader.program->bindings) {
                if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages) continue;
                if (binding.kind == ShaderRecompiler::DescriptorKind::SampledImage) {
                    const auto elementWords = binding.count != 0 ? binding.guestDescriptor.size() / binding.count : 0;
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        const auto raw = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
                        std::array<std::uint32_t, 8> narrowed{};
                        const auto words = firstLayerWords(raw, binding.imageShape, narrowed, false);
                        if (IsNullSampledTextureResource(words)) continue;
                        const auto resource = DecodeTextureResource(words);
                        const VkComponentMapping components = ViewComponents(resource);
                        const VkImageViewType multisampleViewType = binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2DMultisampleArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
                        const bool depthCompare = !binding.Flags().imageDepthCompare.empty() && binding.Flags().imageDepthCompare.at(element);
                        if (textureIndex >= textures.size()) return failed("sampled-count", "sampled-image", binding.binding, element);
                        const auto current = cachedTexture(context, words, resource, components, 0, depthCompare, multisampleViewType, layerRowsBinding(binding.imageShape, resource, depthCompare));
                        if (current != textures[textureIndex]) {
                            failed("sampled-identity", "sampled-image", binding.binding, element);
                            if (diagnose) {
                                report->detail.currentAddress = resource.baseAddress;
                                report->detail.priorIdentity = reinterpret_cast<std::uintptr_t>(textures[textureIndex].get());
                                report->detail.currentIdentity = reinterpret_cast<std::uintptr_t>(current.get());
                                if (textureIndex < validatedTextures.size() && validatedTextures[textureIndex].valid) {
                                    report->detail.priorAddress = validatedTextures[textureIndex].resource.baseAddress;
                                    report->detail.priorBytes = validatedTextures[textureIndex].bytes;
                                }
                            }
                            return false;
                        }
                        ++textureIndex;
                    }
                } else if (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) {
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u);
                        if (IsNullTextureResource(words)) continue;
                        if (storageIndex >= storageTextures.size()) return failed("storage-count", "storage-image", binding.binding, element);
                        std::shared_ptr<StorageTexture> expected;
                        if (SameAsPreviousStorageElement(binding, element) && StorageDedupeEnabled()) expected = storageTextures[storageIndex - 1];
                        else expected = cachedStorageTexture(context, words, DecodeTextureResource(words), storageMips[storageIndex], 0, storageWritten[storageIndex]);
                        if (expected != storageTextures[storageIndex]) {
                            failed("storage-identity", "storage-image", binding.binding, element);
                            if (diagnose) {
                                const auto& prior = storageTextures[storageIndex];
                                report->detail.priorIdentity = reinterpret_cast<std::uintptr_t>(prior.get());
                                report->detail.currentIdentity = reinterpret_cast<std::uintptr_t>(expected.get());
                                if (prior != nullptr) {
                                    report->detail.priorAddress = prior->Descriptor().baseAddress;
                                    report->detail.priorBytes = prior->GuestBytes();
                                }
                                if (expected != nullptr) {
                                    report->detail.currentAddress = expected->Descriptor().baseAddress;
                                    report->detail.currentBytes = expected->GuestBytes();
                                }
                            }
                            return false;
                        }
                        ++storageIndex;
                    }
                }
            }
        }
        const auto currentTableEntry = [&](const ShaderRecompiler::DescriptorBinding& images, std::uint32_t element) {
            if (textureIndex >= textures.size()) return failed("image-table-texture-count", "image-table", images.binding, element);
            const auto raw = std::span<const std::uint32_t>(images.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u);
            std::array<std::uint32_t, 8> narrowed{};
            const auto words = firstLayerWords(raw, images.imageShapes.at(element), narrowed, false);
            const auto resource = DecodeTextureResource(words);
            const bool depthCompare = images.Flags().imageDepthCompare.at(element);
            const VkImageViewType multisampleViewType = images.imageShapes.at(element) == ShaderRecompiler::DescriptorImageShape::Image2DMultisampleArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
            const auto current = cachedTexture(context, words, resource, ViewComponents(resource), 0, depthCompare, multisampleViewType, layerRowsBinding(images.imageShapes.at(element), resource, depthCompare));
            if (current == textures[textureIndex]) return true;
            failed("image-table-identity", "image-table", images.binding, element);
            if (diagnose) {
                report->detail.currentAddress = resource.baseAddress;
                report->detail.priorIdentity = reinterpret_cast<std::uintptr_t>(textures[textureIndex].get());
                report->detail.currentIdentity = reinterpret_cast<std::uintptr_t>(current.get());
            }
            return false;
        };
        for (const auto& table : tableStages) {
            if (table.elementTextures.empty()) continue;
            const auto* images = tableImages(shaders, table.stage);
            if (images == nullptr) return failed("image-table-missing", "image-table");
            if (images->count != table.elementTextures.size()) return failed("image-table-count", "image-table", images->binding);
            std::vector<std::shared_ptr<Texture>> cached;
            if (auto* reused = FindReusedTable(context.device, *images, CurrentTableState()); reused != nullptr && TableCurrent(*reused) && CachedTableTextures(*reused, cached)) {
                auto next = cached.begin();
                bool same = true;
                for (std::uint32_t element = 0; same && element < images->count; ++element) {
                    const auto& entry = reused->elements[element];
                    const auto index = table.elementTextures[element];
                    if (entry.rejected) {
                        same = index == std::numeric_limits<std::size_t>::max();
                        if (!same) failed("image-table-rejected-element", "image-table", images->binding, element);
                        continue;
                    }
                    if (entry.live) {
                        if (index == std::numeric_limits<std::size_t>::max()) continue;
                        same = index == textureIndex && currentTableEntry(*images, element);
                        if (!same) failed("image-table-live-index", "image-table", images->binding, element);
                        ++textureIndex;
                        continue;
                    }
                    const auto& texture = *next++;
                    if (index == std::numeric_limits<std::size_t>::max()) continue;
                    same = index == textureIndex && textureIndex < textures.size() && textures[textureIndex] == texture;
                    if (!same) {
                        failed(index != textureIndex ? "image-table-cached-index" : textureIndex >= textures.size() ? "image-table-texture-count" : "image-table-cached-identity", "image-table", images->binding, element);
                        if (diagnose && index == textureIndex && textureIndex < textures.size()) {
                            report->detail.priorIdentity = reinterpret_cast<std::uintptr_t>(textures[textureIndex].get());
                            report->detail.currentIdentity = reinterpret_cast<std::uintptr_t>(texture.get());
                        }
                    }
                    ++textureIndex;
                }
                if (!same) return failed("image-table-cached-structure", "image-table", images->binding);
                TextureCounts().tablesReused.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (!table.representatives.empty() && table.representatives != variantRepresentatives(context.device, *images, table.elementBase, context.imageTableCapacity)) return failed("image-table-representatives", "image-table", images->binding);
            for (std::uint32_t element = 0; element < images->count; ++element) {
                if (table.elementTextures[element] == std::numeric_limits<std::size_t>::max()) continue;
                if (!table.representatives.empty() && table.representatives[element] != NoRepresentative) continue;
                if (table.elementTextures[element] != textureIndex) return failed("image-table-index", "image-table", images->binding, element);
                if (!currentTableEntry(*images, element)) return false;
                ++textureIndex;
            }
        }
        if (textureIndex != textures.size() || storageIndex != storageTextures.size()) return failed("walk-resource-count");
        // The walk proved every object current again: the records move to what it proved, or one
        // spurious stamp (a label sharing a 64 KiB block with a surface's edge) would keep this
        // object on the full walk for good.
        captureValidation();
        if (GuestMemory::GpuMutex().HeldByThisThread() && walkSerial != 0 && DepthSurfacesSerial() == walkSerial && DepthSurfacesReleased() == walkReleased) {
            depthWalkSerial = walkSerial;
            depthWalkReleased = walkReleased;
        }
        return true;
    };
    if (stampProofs && !noFast && !epochReused && stampProofHolds()) {
        stampReused = true;
        if (stampProofsVerify()) {
            struct VerifyOverlapSlotThreadStorage {};
            auto& verifyOverlapSlot = AgcDriver::ThreadScratch<std::vector<PendingOverlap>*, VerifyOverlapSlotThreadStorage>();
            struct VerifyRefreshedSlotThreadStorage {};
            auto& verifyRefreshedSlot = AgcDriver::ThreadScratch<std::vector<PendingOverlap>*, VerifyRefreshedSlotThreadStorage>();
            auto& verifyOverlap = ShaderRecompiler::ThreadOwned(verifyOverlapSlot);
            auto& verifyRefreshed = ShaderRecompiler::ThreadOwned(verifyRefreshedSlot);
            verifyRefreshed.clear();
            FastFail verifyReason = FastFail::Count;
            bool verifyAccepted = false;
            auto& counters = stampProofCounters();
            counters.verified.fetch_add(1, std::memory_order_relaxed);
            if (!fastRevalidate(serialBefore, verifyRefreshed, verifyReason, verifyOverlap, verifyAccepted)) {
                const auto count = counters.mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((count & (count - 1)) == 0) std::fprintf(stderr, "[rescache] PIPE_RESOLVE_PROOFS_VERIFY: the stamp memo held where the fast proof failed (reason %zu; %llu so far)\n", static_cast<std::size_t>(verifyReason), static_cast<unsigned long long>(count));
                stampReused = false;
                setStampValid(false);
            }
        }
    }
    bool fast = epochReused || stampReused;
    if (fast && report != nullptr) report->path = ProofPath::Fast;
    if (report != nullptr) report->stampReused = stampReused;
    if (epochReused) {
        for (const auto& entry : epochImages) entry.image->NoteProved();
    }
    if (stampReused) {
        for (const auto& entry : stampProof.images) entry.image->NoteProved();
    }
    ArchC::ProbeAt(ArchC::Probe::RvStamp);
    if (!epochReused && !stampReused) {
        struct OverlappingTag {};
        auto& overlapping = ThreadScratch<std::vector<PendingOverlap>, OverlappingTag>();
        struct RefreshedTag {};
        auto& refreshed = ThreadScratch<std::vector<PendingOverlap>, RefreshedTag>();
        refreshed.clear();
        FastFail reason = FastFail::Count;
        bool accepted = false;
        markStage(1);
        fast = !noFast && fastRevalidate(serialBefore, refreshed, reason, overlapping, accepted);
        // T1 (design_cpu_final M3, rule RT1): a Pending failure whose overlapping images are foreign
        // to the surfaces is resolved by the own objects' refresh (what the walk's lookups would do to
        // them) and the fast proof run again, which is then authoritative (it accepts what stays
        // pending over the refreshed surfaces, see fastRevalidate); a fallback, or a second failure,
        // takes the full walk as before.
        bool ownRefreshed = false;
        if (!fast && !overlapping.empty()) {
            auto fallback = OwnRefreshFallback::Count;
            if (!OwnImageRefreshEnabled()) fallback = OwnRefreshFallback::Disabled;
            else {
                refreshed = overlapping;
                fallback = refreshOwnObjects(shaders, refreshed);
            }
            if (fallback == OwnRefreshFallback::Count) {
                fast = fastRevalidate(serialBefore, refreshed, reason, overlapping, accepted);
                if (fast) ownRefreshed = true;
                else fallback = OwnRefreshFallback::Rerun;
            }
            if (fallback != OwnRefreshFallback::Count) countOwnRefreshFallback(fallback);
        }
        markStage(2);
        if (fast && TemplateAuditDue(lastFullWalk) && (AuditEveryInterval() || epochProofs || stampProofsActive() || (walkImportGeneration != HostImportSetGeneration() && ImportAuditAdmitted()))) {
            fast = false;
            ownRefreshed = false;
            reason = FastFail::Audit;
        }
        if (report != nullptr) {
            report->path = !fast ? ProofPath::Full : ownRefreshed ? ProofPath::OwnRefreshed : ProofPath::Fast;
            report->audited = !fast && reason == FastFail::Audit;
        }
        if (report != nullptr) report->fastFail = static_cast<std::size_t>(reason);
        if (!fast) {
            countFullWalk(reason);
            const auto walkStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const bool walked = fullWalk();
            countFullWalkTime(reason, walkStart);
            if (!walked) {
                const auto failure = [&] {
                    switch (reason) {
                        case FastFail::Pending: return ProofFailure::Pending;
                        case FastFail::Evicted: case FastFail::Replaced: return ProofFailure::Evicted;
                        case FastFail::Changed: return ProofFailure::Changed;
                        case FastFail::Keys: case FastFail::ClearedView: case FastFail::StorageKeys: return ProofFailure::Keys;
                        default: return ProofFailure::Other;
                    }
                }();
                return finish(false, false, failure);
            }
        } else if (depthProofAccepted && depthProofVerifyNow()) {
            auto& c = depthProofCounters();
            c.verified.fetch_add(1, std::memory_order_relaxed);
            struct DepthVersionsSlotStorage {};
            auto& versions = AgcDriver::ThreadScratch<std::vector<std::pair<const StorageTexture*, std::uint64_t>>, DepthVersionsSlotStorage>();
            versions.clear();
            for (const auto& surface : validatedTextures) {
                if (surface.source != nullptr) versions.emplace_back(surface.source, surface.source->Version());
            }
            for (const auto& image : storageTextures) versions.emplace_back(image.get(), image->Version());
            const auto texturesBefore = textures;
            const bool same = fullWalk();
            const auto moved = std::find_if(versions.begin(), versions.end(), [](const auto& entry) { return entry.first->Version() != entry.second; });
            if (!same || moved != versions.end() || textures != texturesBefore) {
                const auto count = c.mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count <= 8) std::fprintf(stderr, "[archc] C_COMPUTE_DEPTH_PROOF VERIFY mismatch: the full walk %s\n", !same ? "returned another object" : "uploaded");
                if (!same) return finish(false, false, ProofFailure::Changed);
            }
        } else if (ownRefreshed || accepted) {
            if (profile && ownRefreshed) Revalidations().ownRefreshed.fetch_add(1, std::memory_order_relaxed);
            if (VerifyProofs()) {
                // The walk after the refresh, or beside a proof that accepted a foreign overlap, must
                // find every object in place and current: another object, or an upload (a moved
                // content version), is a decision the proof got wrong.
                struct VersionsTag {};
                auto& versions = ThreadScratch<std::vector<std::pair<const StorageTexture*, std::uint64_t>>, VersionsTag>();
                versions.clear();
                for (const auto& surface : validatedTextures) {
                    if (surface.source != nullptr) versions.emplace_back(surface.source, surface.source->Version());
                }
                for (const auto& image : storageTextures) versions.emplace_back(image.get(), image->Version());
                const bool same = fullWalk();
                const auto moved = std::find_if(versions.begin(), versions.end(), [](const auto& entry) { return entry.first->Version() != entry.second; });
                if (!same || moved != versions.end()) {
                    std::fprintf(stderr, "[rescache] APS5_VERIFY_PROOFS: the T1 proof (%s) disagrees with the full walk (%s)\n", ownRefreshed ? "own-object refresh" : "accepted overlap", !same ? "another object" : "an upload");
                    std::fflush(stderr);
                    std::abort();
                }
                if (profile) Revalidations().proofsVerified.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    markStage(3);
    // (2) The imported buffers the set reads in place: results of storage images pending in them go
    // to guest memory first (as an upload does), then each import must still be the one the set was
    // written against. This comes last because the lookups and flushes above can reconcile imports
    // themselves; nothing else touches them between here and the dispatch being recorded. Under the
    // epoch gate the flush runs only for regions one registry scan finds pending images over (none
    // while the serial is the memo's), and the serial loop only while the import table's identity
    // moved since the last proof (a retire bumps its epoch, a registry change its generation).
    const auto serialLoop = [&] {
        for (const auto& region : directRegions) {
            auto serial = HostImportSerial(context, region.begin, static_cast<std::size_t>(region.end - region.begin), true);
            if (serial == 0) serial = ImageMirrorSerial(context, region.begin, static_cast<std::size_t>(region.end - region.begin));
            if (serial != region.serial) {
                failed("import-identity", "direct-region", std::numeric_limits<std::size_t>::max(), static_cast<std::size_t>(&region - directRegions.data()));
                if (diagnose) {
                    report->detail.priorAddress = report->detail.currentAddress = region.begin;
                    report->detail.priorBytes = report->detail.currentBytes = region.end - region.begin;
                    report->detail.priorIdentity = region.serial;
                    report->detail.currentIdentity = serial;
                }
                return false;
            }
            if (region.regionEnd != 0) {
                const auto committed = GuestMemory::DescribeCommitted(region.begin, static_cast<std::size_t>(region.regionEnd - region.begin));
                if (committed.ranges.size() != 1 || committed.ranges.front().first != region.begin || committed.ranges.front().second != region.committedEnd) {
                    failed("committed-extent", "direct-region", std::numeric_limits<std::size_t>::max(), static_cast<std::size_t>(&region - directRegions.data()));
                    if (diagnose) {
                        report->detail.priorAddress = region.begin;
                        report->detail.priorBytes = region.committedEnd - region.begin;
                        if (committed.ranges.size() == 1) {
                            report->detail.currentAddress = committed.ranges.front().first;
                            report->detail.currentBytes = committed.ranges.front().second - committed.ranges.front().first;
                        }
                    }
                    return false;
                }
            }
        }
        return true;
    };
    if (EpochRevalidate()) {
        if ((!directRegions.empty() || !guestMemory.Writes().empty()) && !(pendingSerialSeen != 0 && pendingSerialSeen == StorageTexture::PendingSerial())) {
            // C_REPLAY_QUIET_TEMPLATE (holding the GPU mutex): the scan below is skipped while its token holds.
            // A pending image overlapping a region can only appear through the registry, which notes every
            // insert as an ImageEpochs PendingInsert event over the image (bumping pendingScanEpoch); a fresh unit
            // shadow only through a retile (MarkShadowed), noted the same way over its whole units; a dropped note
            // shows as a loss.
            auto& scanCounters = pendingScanCounters();
            const bool scanToken = pendingScanTokensActive() && GuestMemory::GpuMutex().HeldByThisThread();
            const bool scanHolds = scanToken && pendingScanHolds();
            const bool scanVerify = scanHolds && pendingScanVerifyNow();
            if (scanHolds && !scanVerify) {
                scanCounters.hits.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (scanToken) scanCounters.live.fetch_add(1, std::memory_order_relaxed);
                if (scanVerify) scanCounters.verifyChecks.fetch_add(1, std::memory_order_relaxed);
                if (scanToken && !pendingScanToken.registered) {
                    struct ScanRangesSlotStorage {};
                    auto& scanRanges = AgcDriver::ThreadScratch<std::vector<ArchC::EpochRange>, ScanRangesSlotStorage>();
                    scanRanges.clear();
                    for (const auto& region : directRegions) scanRanges.push_back({region.begin, region.end});
                    for (const auto& [begin, end] : guestMemory.Writes()) scanRanges.push_back({begin, end});
                    ArchC::ImageEpochs::Get().RegisterOwner(&pendingScanEpoch, pendingScanEpoch, scanRanges, ArchC::SourceBit(ArchC::EpochSource::PendingInsert));
                    pendingScanToken.registered = true;
                }
                const bool scanDrained = scanToken && ArchC::ImageEpochs::Get().DrainIfNew();
                const auto scanEpoch = pendingScanEpoch.load(std::memory_order_acquire);
                const auto scanLoss = ArchC::ImageEpochs::Get().Loss();
                struct RegionsTag {};
                auto& regions = ThreadScratch<std::vector<StorageTexture::PendingQuery>, RegionsTag>();
                regions.clear();
                for (const auto& region : directRegions) regions.push_back({region.begin, region.end, nullptr, nullptr, false});
                for (const auto& [begin, end] : guestMemory.Writes()) regions.push_back({begin, end, nullptr, nullptr, false});
                StorageTexture::ScanPending(regions);
                scanCounters.scans.fetch_add(1, std::memory_order_relaxed);
                scanCounters.regions.fetch_add(regions.size(), std::memory_order_relaxed);
                ArchC::ProbeAt(ArchC::Probe::RvScan);
                // A region over a unit shadow's fresh results (a retile bumped the serial) is
                // published by the flush, as one over a pending image is stored.
                bool& flushed = proofWork;
                for (const auto& region : regions) {
                    if (region.overlaps || AnyShadowedOverlaps(region.begin, static_cast<std::size_t>(region.end - region.begin))) {
                        flushed = true;
                        StorageTexture::FlushPending(region.begin, static_cast<std::size_t>(region.end - region.begin), nullptr, "imported buffer region");
                    }
                }
                if (scanVerify && flushed) {
                    const auto previous = scanCounters.verifyMismatches.fetch_add(1, std::memory_order_relaxed);
                    if (previous < 8) std::fprintf(stderr, "[archc] quiet template VERIFY mismatch: the pending scan found a region to flush while its token held\n");
                }
                if (scanToken) {
                    pendingScanToken.valid = !flushed && scanDrained && ArchC::ImageEpochs::Get().DrainIfNew() && pendingScanEpoch.load(std::memory_order_acquire) == scanEpoch && ArchC::ImageEpochs::Get().Loss() == scanLoss;
                    if (pendingScanToken.valid) {
                        pendingScanToken.epoch = scanEpoch;
                        pendingScanToken.loss = scanLoss;
                        scanCounters.made.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
        if (importsReconcilable ? HostImportsReconciledUnchanged(context, importsProof) : HostImportsUnchanged(context, importsProof)) {
            if (profile) Revalidations().importSkips.fetch_add(1, std::memory_order_relaxed);
        } else {
            proofWork = true;
            if (!serialLoop()) return finish(fast, false, ProofFailure::Imports);
            importsProof = HostImportsIdentity(context);
        }
    } else {
        for (const auto& region : directRegions) StorageTexture::FlushPending(region.begin, static_cast<std::size_t>(region.end - region.begin), nullptr, "imported buffer region");
        for (const auto& [begin, end] : guestMemory.Writes()) StorageTexture::FlushPending(begin, static_cast<std::size_t>(end - begin), nullptr, "written buffer range");
        if (!serialLoop()) return finish(fast, false, ProofFailure::Imports);
    }
    markStage(4);
    ArchC::ProbeAt(ArchC::Probe::RvImports);
    // (3) Buffers staged in device memory (GuestBufferMemory::AllowDeviceStaging) are copied in
    // from their imports anew for this use, after the flushes above and before the work is
    // recorded; a failure to record leaves the object unusable for this dispatch, not the batch.
    if (!guestMemory.StagingDecisionsCurrent()) {
        failed("staging-decision-changed", "guest-buffer");
        return finish(fast, false, ProofFailure::Changed);
    }
    if (auto* recorder = Recorder::Active(); recorder != nullptr) {
        try {
            guestMemory.RecordStagingCopies(*recorder);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[resources] staging copies of a reused build failed: %s\n", error.what());
            failed("staging-record-failed", "guest-buffer");
            return finish(fast, false);
        }
    }
    ArchC::ProbeAt(ArchC::Probe::RvStaging);
    pendingSerialSeen = EpochRevalidate() && StorageTexture::PendingSerial() == serialBefore ? serialBefore : 0;
    for (const auto& texture : textures) {
        if (texture != nullptr && texture->IsDepthCopy()) texture->RefreshDepthCopy();
    }
    if (stampProofs && !stampReused) {
        if (StorageTexture::PendingSerial() == serialBefore && currentStampSerials() == stampSerials && HostImportsIdentity(context, false) == stampImports) rememberStampProof(stampGeneration, stampSerials, stampImports);
        else {
            setStampValid(false);
            stampProofCounters().skips[static_cast<std::size_t>(StampSkip::Moved)].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (epochProofs) {
        if (GuestMemory::GpuMutex().HeldByThisThread() && epochBefore.epoch != 0 && (epochBefore.forgotten & 1u) == 0 && (epochReused || canRememberResourceEpoch()) && currentResourceEpochProof() == epochBefore && HostImportsIdentity(context, false) == importsBefore) {
            if (!epochReused) {
                epochImages.clear();
                const auto remember = [&](const StorageTexture* image) {
                    if (image != nullptr) epochImages.push_back({image, image->Version(), image->Descriptor().dccAddress});
                };
                for (const auto& surface : validatedTextures) remember(surface.source);
                for (const auto& image : storageTextures) remember(image.get());
                if (profile) Revalidations().epochRemembered.fetch_add(1, std::memory_order_relaxed);
            }
            resourceEpochProof = epochBefore;
            epochImports = importsBefore;
        } else {
            resourceEpochProof = {};
            if (profile) Revalidations().epochUnstable.fetch_add(1, std::memory_order_relaxed);
        }
    }
    markStage(5);
    // C_REPLAY_QUIET_TEMPLATE: arm the quiet proof after a stamp-memo pass that did nothing, of an object whose
    // proof is buffers only.
    const bool quietPass = stampReused && !proofWork && EpochRevalidate() && textures.empty() && storageTextures.empty() && describedRanges.empty() && stampProof.ranges.empty() && stampProof.images.empty() && stampProof.partials.empty() && stampProof.pending.empty();
    lastProofQuiet.store(quietPass, std::memory_order_relaxed);
    if (GuestMemory::GpuMutex().HeldByThisThread()) {
        quietProof.valid = false;
        if (quietPass && quietProofsActive()) {
            const auto now = currentStampSerials();
            const auto identity = HostImportsIdentity(context, true);
            const bool planes = std::none_of(directRegions.begin(), directRegions.end(), [](const auto& region) { return DepthStalePlaneOverlaps(region.begin, region.end); });
            if (planes && (now.forgotten & 1u) == 0 && now.forgotten == stampProof.serials.forgotten && now.unwatched == stampProof.serials.unwatched && now.depth == stampProof.serials.depth && now.depthReleased == stampProof.serials.depthReleased && identity == importsProof && identity == stampProof.imports && identity.device != VK_NULL_HANDLE) {
                quietProof = {true, stampProof.generation, now.unwatched, now.forgotten, now.depth, now.depthReleased, DepthStalePlanesSerial(), identity};
                quietProofCounters().made.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return finish(fast, true);
}

namespace {

// APS5_PROFILE_DRAW: what makes content keys miss. On a miss the key is compared with the last key
// seen for the same shader variant(s) and the first differing word is charged to its binding's role
// (and to its index within the V# or T# element), printed as [rescache] every 10 s: it says whether
// V# bases of ring allocations, SRT words or image descriptors churn.
struct KeyMissWindow {
    std::uint64_t compared = 0;
    std::uint64_t noCandidate = 0;
    std::uint64_t unlabelled = 0;
    std::uint64_t identical = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(ContentKeyComponent::Count)> differing{};
};

struct ChurnCounts {
    std::array<std::uint64_t, 8> roles{};
    std::array<std::uint64_t, 4> bufferWords{};
    std::array<std::uint64_t, 8> imageWords{};
    std::uint64_t firstSeen = 0;
    std::uint64_t sameKey = 0;
    std::uint64_t layout = 0;
    std::uint64_t drawWords = 0;
    KeyMissWindow window;
};

// Dispatch keys and draw keys are counted apart: what churns a draw's key decides the draw steps.
struct ChurnProfile {
    AgcDriver::Mutex mutex;
    std::unordered_map<std::uint64_t, ResourceCache::Key> lastByVariant;
    ChurnCounts dispatch;
    ChurnCounts draws;
    std::unordered_map<std::uint64_t, std::uint32_t> tracedByVariant;
    std::size_t tracedLines = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

constexpr std::size_t MaxTracedLines = 256;
constexpr std::uint32_t MaxTracedLinesPerVariant = 4;

const char* componentName(std::size_t component) {
    constexpr std::array<const char*, static_cast<std::size_t>(ContentKeyComponent::Count)> names{"layout", "V# word 0", "V# word 1", "V# word 2", "V# word 3", "image", "sampler", "shader-data", "other", "draw target/index"};
    return names[component];
}

void traceMiss(ChurnProfile& churn, bool draw, std::uint64_t variants, const ContentKeyDiff* parts);

ChurnProfile& Churn() {
    static ChurnProfile profile;
    return profile;
}

// Charges word `diff` of `key` to the binding of the ContentKey that starts at `at` (the layout of
// ShaderResources::ContentKey), moving `at` past that key; false when `diff` lies beyond it.
bool chargeShaderKey(ChurnCounts& profile, const ResourceCache::Key& key, std::size_t& at, std::size_t end, std::size_t diff) {
    if (at + 5 > end || diff < at + 5) {
        ++profile.layout;
        return true;
    }
    const bool dataWords = key[at] != 0;
    const auto bindings = key[at + 4];
    at += 5;
    for (std::uint32_t binding = 0; binding < bindings; ++binding) {
        if (at + 8 > end || diff < at + 8) {
            ++profile.layout;
            return true;
        }
        const auto kind = key[at];
        const auto role = key[at + 1];
        const auto descriptorWords = dataWords || !DataRole(static_cast<ShaderRecompiler::DescriptorRole>(role)) ? key[at + 7] : 0u;
        at += 8;
        if (diff < at + descriptorWords) {
            if (role < profile.roles.size()) ++profile.roles[role];
            const auto index = diff - at;
            if (role == static_cast<std::uint32_t>(ShaderRecompiler::DescriptorRole::GuestBuffers)) ++profile.bufferWords[index % 4];
            else if (role == static_cast<std::uint32_t>(ShaderRecompiler::DescriptorRole::GuestImages) && (kind == static_cast<std::uint32_t>(ShaderRecompiler::DescriptorKind::SampledImage) || kind == static_cast<std::uint32_t>(ShaderRecompiler::DescriptorKind::StorageImage))) ++profile.imageWords[index % 8];
            return true;
        }
        at += descriptorWords;
        for (int flags = 0; flags < 10; ++flags) {
            if (at >= end) {
                ++profile.layout;
                return true;
            }
            const auto words = 1 + (static_cast<std::size_t>(key[at]) + 31) / 32;
            if (diff < at + words) {
                ++profile.layout;
                return true;
            }
            at += words;
        }
        for (const auto listRole : {ShaderRecompiler::DescriptorRole::GuestSamplers, ShaderRecompiler::DescriptorRole::GuestImages}) {
            if (at >= end) {
                ++profile.layout;
                return true;
            }
            const auto words = 1 + static_cast<std::size_t>(key[at]);
            if (diff < at + words) {
                ++profile.roles[static_cast<std::size_t>(listRole)];
                return true;
            }
            at += words;
        }
    }
    return false;
}

}

std::shared_ptr<ShaderResources> ResourceCache::Find(const Key& key) {
    resourceCacheFinds.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    const auto found = index.find(key);
    if (found == index.end()) {
        noteReturn(key);
        noteMiss(key);
        return nullptr;
    }
    entries.splice(entries.begin(), entries, found->second);
    noteUse(key);
    return found->second->second;
}

void ResourceCache::noteEvicted(const Key& key) {
    if (fixedCapacity) return;
    const auto hash = KeyHash{}(key);
    if (const auto found = evictedKeys.find(hash); found != evictedKeys.end()) evictedOrder.erase(found->second);
    evictedOrder.push_front(hash);
    evictedKeys[hash] = evictedOrder.begin();
    while (evictedOrder.size() > capacity) {
        evictedKeys.erase(evictedOrder.back());
        evictedOrder.pop_back();
    }
    if (++evictionsSinceCheck < capacity / 4u) return;
    const bool churning = returnsSinceCheck * 2u > evictionsSinceCheck;
    evictionsSinceCheck = 0;
    returnsSinceCheck = 0;
    if (!churning || capacity >= MaximumEntries) return;
    const auto previous = capacity;
    capacity = std::min(capacity * 2u, MaximumEntries);
    if (shrankLast) shrinkWindow = std::min(shrinkWindow * 2u, MaximumShrinkWindow);
    shrankLast = false;
    usedKeys.clear();
    usesSinceCheck = 0;
    std::fprintf(stderr, "[rescache] capacity %zu -> %zu entries: keys it evicted keep coming back\n", previous, capacity);
}

void ResourceCache::noteUse(const Key& key) {
    if (fixedCapacity || capacity <= baseEntries) return;
    usedKeys.insert(KeyHash{}(key));
    if (++usesSinceCheck < capacity * shrinkWindow) return;
    const auto used = usedKeys.size();
    const auto uses = usesSinceCheck;
    usedKeys.clear();
    usesSinceCheck = 0;
    if (used * 4u > capacity) return;
    const auto previous = capacity;
    capacity = std::max(capacity / 2u, baseEntries);
    shrankLast = true;
    evictionsSinceCheck = 0;
    returnsSinceCheck = 0;
    std::fprintf(stderr, "[rescache] capacity %zu -> %zu entries: %zu keys served the last %zu uses\n", previous, capacity, used, uses);
}

void ResourceCache::noteReturn(const Key& key) {
    if (fixedCapacity) return;
    const auto found = evictedKeys.find(KeyHash{}(key));
    if (found == evictedKeys.end()) return;
    evictedOrder.erase(found->second);
    evictedKeys.erase(found);
    ++returnsSinceCheck;
}

namespace {

void traceMiss(ChurnProfile& churn, bool draw, std::uint64_t variants, const ContentKeyDiff* parts) {
    if (churn.tracedLines >= MaxTracedLines || ++churn.tracedByVariant[variants] > MaxTracedLinesPerVariant) return;
    ++churn.tracedLines;
    std::string changed = parts == nullptr ? "first-seen" : "";
    for (std::size_t i = 0; parts != nullptr && i < parts->size(); ++i) {
        if (!(*parts)[i]) continue;
        if (!changed.empty()) changed += ", ";
        changed += componentName(i);
    }
    if (changed.empty()) changed = "no-component-change";
    const auto hashed = (variants ^ 0x9e3779b97f4a7c15ull) * 0xff51afd7ed558ccdull;
    AgcDriver::ProfilePrint_nid_no_patch("[keymiss] resource-content %s shader set %016llx: %s\n", draw ? "draw" : "dispatch", static_cast<unsigned long long>(hashed ^ (hashed >> 33u)), changed.c_str());
}

}

void ResourceCache::noteMiss(const Key& key) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    static const bool trace = std::getenv("APS5_TRACE_RESOURCE_KEY_MISSES") != nullptr;
    if ((!profile && !trace) || key.size() < 5) return;
    // The variant(s) the key belongs to: a compute key starts with the data-words flag, the stage
    // and its variant, a draw key with a marker, the device and its stages' keys (Draw.cpp
    // DrawResourceKey).
    const bool draw = key[0] == 0xffffffffu;
    std::uint64_t variants = 0;
    if (draw) {
        std::size_t at = 4;
        for (std::uint32_t stage = 0; stage < key[3] && at + 4 < key.size(); ++stage) {
            variants = variants * 1000003ull ^ (key[at + 3] | (static_cast<std::uint64_t>(key[at + 4]) << 32u));
            at += 1 + key[at];
        }
    } else {
        variants = key[2] | (static_cast<std::uint64_t>(key[3]) << 32u);
    }
    auto& churn = Churn();
    std::lock_guard lock(churn.mutex);
    auto& counts = draw ? churn.draws : churn.dispatch;
    const auto found = churn.lastByVariant.find(variants);
    if (found == churn.lastByVariant.end()) {
        ++counts.firstSeen;
        ++counts.window.noCandidate;
        if (trace) traceMiss(churn, draw, variants, nullptr);
        churn.lastByVariant.emplace(variants, key);
    } else {
        const auto& previous = found->second;
        const auto common = std::min(key.size(), previous.size());
        std::size_t diff = 0;
        while (diff < common && key[diff] == previous[diff]) ++diff;
        if (diff == common && key.size() == previous.size()) {
            ++counts.sameKey;
        } else if (draw) {
            std::size_t at = 4;
            bool charged = diff < at;
            if (charged) ++counts.layout;
            for (std::uint32_t stage = 0; stage < key[3] && at < key.size() && !charged; ++stage) {
                const auto end = std::min(key.size(), at + 1 + static_cast<std::size_t>(key[at]));
                if (diff == at) {
                    ++counts.layout;
                    charged = true;
                } else if (diff < end) {
                    ++at;
                    if (!chargeShaderKey(counts, key, at, end, diff)) ++counts.layout;
                    charged = true;
                }
                at = end;
            }
            if (!charged) ++counts.drawWords;
        } else {
            std::size_t at = 0;
            if (!chargeShaderKey(counts, key, at, key.size(), diff)) ++counts.layout;
        }
        if (trace) {
            ContentKeyDiff parts;
            ++counts.window.compared;
            if (!ShaderResources::DiffContentKeys(previous, key, draw, parts)) ++counts.window.unlabelled;
            bool anyPart = false;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (!parts[i]) continue;
                ++counts.window.differing[i];
                anyPart = true;
            }
            if (!anyPart) ++counts.window.identical;
            traceMiss(churn, draw, variants, &parts);
        }
        found->second = key;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - churn.lastReport < std::chrono::seconds(10)) return;
    churn.lastReport = now;
    const auto report = [](const char* what, const ChurnCounts& churn) {
        std::string line = std::string("[rescache] miss churn (") + what + "), first differing word by role:";
        char item[256];
        for (std::size_t role = 0; role < churn.roles.size(); ++role) {
            if (churn.roles[role] == 0) continue;
            std::snprintf(item, sizeof(item), " %s %llu", roleName(static_cast<ShaderRecompiler::DescriptorRole>(role)), static_cast<unsigned long long>(churn.roles[role]));
            line += item;
            if (role == static_cast<std::size_t>(ShaderRecompiler::DescriptorRole::GuestBuffers)) {
                std::snprintf(item, sizeof(item), " (V# word %llu/%llu/%llu/%llu)", static_cast<unsigned long long>(churn.bufferWords[0]), static_cast<unsigned long long>(churn.bufferWords[1]), static_cast<unsigned long long>(churn.bufferWords[2]), static_cast<unsigned long long>(churn.bufferWords[3]));
                line += item;
            } else if (role == static_cast<std::size_t>(ShaderRecompiler::DescriptorRole::GuestImages)) {
                std::snprintf(item, sizeof(item), " (T# word %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu)", static_cast<unsigned long long>(churn.imageWords[0]), static_cast<unsigned long long>(churn.imageWords[1]), static_cast<unsigned long long>(churn.imageWords[2]), static_cast<unsigned long long>(churn.imageWords[3]), static_cast<unsigned long long>(churn.imageWords[4]), static_cast<unsigned long long>(churn.imageWords[5]), static_cast<unsigned long long>(churn.imageWords[6]), static_cast<unsigned long long>(churn.imageWords[7]));
                line += item;
            }
        }
        std::snprintf(item, sizeof(item), "; layout %llu, draw target/index %llu, same key %llu (not inserted or evicted), first seen %llu", static_cast<unsigned long long>(churn.layout), static_cast<unsigned long long>(churn.drawWords), static_cast<unsigned long long>(churn.sameKey), static_cast<unsigned long long>(churn.firstSeen));
        line += item;
        AgcDriver::ProfilePrint_nid_no_patch("%s\n", line.c_str());
    };
    if (profile) {
        report("dispatch", churn.dispatch);
        report("draws", churn.draws);
    }
    const auto keyMiss = [](const char* what, KeyMissWindow& window) {
        const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
        const auto part = [&](ContentKeyComponent component) { return count(window.differing[static_cast<std::size_t>(component)]); };
        AgcDriver::ProfilePrint_nid_no_patch("[keymiss] resource-content %s (10 s): misses %llu; differing: layout %llu, V# word0 %llu, V# word1 %llu, V# word2 %llu, V# word3 %llu, image %llu, sampler %llu, shader-data %llu, other %llu, draw target/index %llu; no-candidate %llu; identical %llu; not comparable %llu\n", what, count(window.compared + window.noCandidate), part(ContentKeyComponent::Layout), part(ContentKeyComponent::BufferWord0), part(ContentKeyComponent::BufferWord1), part(ContentKeyComponent::BufferWord2), part(ContentKeyComponent::BufferWord3), part(ContentKeyComponent::Image), part(ContentKeyComponent::Sampler), part(ContentKeyComponent::ShaderData), part(ContentKeyComponent::Other), part(ContentKeyComponent::DrawTarget), count(window.noCandidate), count(window.identical), count(window.unlabelled));
        window = KeyMissWindow{};
    };
    if (trace) {
        keyMiss("dispatch", churn.dispatch.window);
        keyMiss("draw", churn.draws.window);
    }
}

void ResourceCache::Insert(const Key& key, std::shared_ptr<ShaderResources> resources, std::vector<std::shared_ptr<ShaderResources>>* evicted) {
    mutationSerial.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard lock(mutex);
    dropReleasedTextures(evicted);
    if (!resources->HasCachedStorageImages() || resources->HoldsReleasedTexture()) return;
    if (const auto found = index.find(key); found != index.end()) {
        found->second->second->SetCacheMembership(0);
        if (evicted != nullptr) evicted->push_back(std::move(found->second->second));
        entries.erase(found->second);
        index.erase(found);
    }
    // APS5_ARCHB_PIPE_DRAW_LOOKUP_MEMO: every insertion a serial of its own (ShaderResources::CacheMembership).
    static std::atomic<std::uint64_t> membershipSerial{0};
    resources->SetCacheMembership(membershipSerial.fetch_add(1, std::memory_order_relaxed) + 1);
    entries.emplace_front(key, std::move(resources));
    index.emplace(key, entries.begin());
    if (capacity == 0) {
        const char* value = std::getenv("APS5_RESOURCE_CACHE_ENTRIES");
        const auto parsed = value ? std::strtoull(value, nullptr, 10) : 0ull;
        fixedCapacity = parsed != 0;
        capacity = fixedCapacity ? static_cast<std::size_t>(parsed) : baseEntries;
    }
    noteUse(key);
    while (entries.size() > capacity) {
        noteEvicted(entries.back().first);
        entries.back().second->SetCacheMembership(0);
        if (evicted != nullptr) evicted->push_back(std::move(entries.back().second));
        index.erase(entries.back().first);
        entries.pop_back();
    }
    if (entries.front().second->TableElements() == 0) return;
    std::uint64_t tables = 0;
    for (const auto& entry : entries) tables += entry.second->TableElements();
    for (auto it = std::prev(entries.end()); tables > TableElementBudget() && it != entries.begin();) {
        const auto elements = it->second->TableElements();
        if (elements == 0) {
            --it;
            continue;
        }
        tables -= elements;
        it->second->SetCacheMembership(0);
        if (evicted != nullptr) evicted->push_back(std::move(it->second));
        index.erase(it->first);
        it = std::prev(entries.erase(it));
    }
}

void ResourceCache::Remove(const Key& key, const ShaderResources* object) {
    mutationSerial.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard lock(mutex);
    if (object != nullptr) {
        const auto found = index.find(key);
        if (found == index.end() || found->second->second.get() != object) return;
    }
    erase(key);
}

void ResourceCache::RemoveImage(const StorageTexture* image, std::vector<std::shared_ptr<ShaderResources>>* evicted) {
    std::lock_guard lock(mutex);
    removeImage(image, evicted);
}

bool ResourceCache::TryRemoveImage(const StorageTexture* image, std::vector<std::shared_ptr<ShaderResources>>* evicted) {
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    removeImage(image, evicted);
    return true;
}

void ResourceCache::removeImage(const StorageTexture* image, std::vector<std::shared_ptr<ShaderResources>>* evicted) {
    mutationSerial.fetch_add(1, std::memory_order_acq_rel);
    for (auto it = entries.begin(); it != entries.end();) {
        if (!it->second->ReadsImage(image)) {
            ++it;
            continue;
        }
        it->second->SetCacheMembership(0);
        if (evicted != nullptr) evicted->push_back(std::move(it->second));
        index.erase(it->first);
        it = entries.erase(it);
    }
}

void ResourceCache::dropReleasedTextures(std::vector<std::shared_ptr<ShaderResources>>* evicted) {
    mutationSerial.fetch_add(1, std::memory_order_acq_rel);
    if (!TexturesReleased().exchange(false, std::memory_order_acq_rel)) return;
    for (auto it = entries.begin(); it != entries.end();) {
        if (!it->second->HoldsReleasedTexture()) {
            ++it;
            continue;
        }
        it->second->SetCacheMembership(0);
        if (evicted != nullptr) evicted->push_back(std::move(it->second));
        index.erase(it->first);
        it = entries.erase(it);
    }
}

bool ResourceCache::Touch(const Key& key) {
    resourceCacheTouches.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    const auto found = index.find(key);
    if (found == index.end()) return false;
    entries.splice(entries.begin(), entries, found->second);
    noteUse(key);
    return true;
}

std::uint64_t ResourceCache::Finds() {
    return resourceCacheFinds.load(std::memory_order_relaxed);
}

std::uint64_t ResourceCache::Touches() {
    return resourceCacheTouches.load(std::memory_order_relaxed);
}

void ResourceCache::Clear() {
    mutationSerial.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard lock(mutex);
    for (const auto& entry : entries) entry.second->SetCacheMembership(0);
    index.clear();
    entries.clear();
    evictedKeys.clear();
    evictedOrder.clear();
    evictionsSinceCheck = 0;
    returnsSinceCheck = 0;
    usedKeys.clear();
    usesSinceCheck = 0;
    shrinkWindow = baseShrinkWindow;
    shrankLast = false;
    if (!fixedCapacity) capacity = 0;
}

void ResourceCache::SetBase(std::size_t entries, std::size_t window) {
    std::lock_guard lock(mutex);
    baseEntries = entries;
    baseShrinkWindow = window;
}

std::size_t ResourceCache::Size() const {
    std::lock_guard lock(mutex);
    return entries.size();
}

std::size_t ResourceCache::Capacity() const {
    std::lock_guard lock(mutex);
    return capacity == 0 ? baseEntries : capacity;
}

std::uint64_t ResourceCache::TableElementBudget() {
    static const std::uint64_t elements = [] {
        const char* value = std::getenv("APS5_RESOURCE_CACHE_TABLE_ELEMENTS");
        const auto parsed = value ? std::strtoull(value, nullptr, 10) : 65536ull;
        return static_cast<std::uint64_t>(parsed != 0 ? parsed : 65536ull);
    }();
    return elements;
}

std::uint64_t ResourceCache::TableElements() const {
    std::lock_guard lock(mutex);
    std::uint64_t tables = 0;
    for (const auto& entry : entries) tables += entry.second->TableElements();
    return tables;
}

void ResourceCache::erase(const Key& key) {
    mutationSerial.fetch_add(1, std::memory_order_acq_rel);
    const auto found = index.find(key);
    if (found == index.end()) return;
    found->second->second->SetCacheMembership(0);
    entries.erase(found->second);
    index.erase(found);
}

ResourceCache& SharedResourceCache() {
    static auto* cache = new ResourceCache();
    return *cache;
}

std::shared_ptr<ShaderResources> ResourceCacheMemo::Find(ResourceCache& cache, const ResourceCache::Key& key, bool verify, Counters& counters) {
    if (entries.empty()) entries.resize(Entries);
    const auto hash = static_cast<std::uint64_t>(ResourceCache::HashKey(key));
    auto& entry = entries[static_cast<std::size_t>(hash % Entries)];
    if (entry.membership != 0 && entry.hash == hash && entry.key == key) {
        auto held = entry.resources.lock();
        if (held != nullptr && held->CacheMembership() == entry.membership) {
            counters.hits.fetch_add(1, std::memory_order_relaxed);
            if (++entry.uses % TouchEvery == 0) {
                cache.Touch(key);
                counters.touches.fetch_add(1, std::memory_order_relaxed);
            }
            if (!verify) return held;
            auto found = cache.Find(key);
            counters.verified.fetch_add(1, std::memory_order_relaxed);
            if (found != held) counters.mismatches.fetch_add(1, std::memory_order_relaxed);
            return found;
        }
        counters.stale.fetch_add(1, std::memory_order_relaxed);
    } else {
        counters.misses.fetch_add(1, std::memory_order_relaxed);
    }
    auto found = cache.Find(key);
    entry.membership = 0;
    if (found != nullptr) {
        entry.hash = hash;
        entry.key.assign(key.begin(), key.end());
        entry.resources = found;
        entry.membership = found->CacheMembership();
        entry.uses = 0;
    }
    return found;
}

namespace {
std::size_t KeptSets() {
    static const std::size_t count = [] {
        const char* text = std::getenv("APS5_KEPT_DESCRIPTOR_SETS");
        return text != nullptr ? static_cast<std::size_t>(std::strtoull(text, nullptr, 10)) : std::size_t{8};
    }();
    return count;
}

}

DescriptorCache::DescriptorCache(const Context& context) : context(context), destroyLayout(context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")), destroyPool(context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")), resetPool(context.Function<PFN_vkResetDescriptorPool>("vkResetDescriptorPool")), freeDescriptorSets(context.individuallyFreeDescriptorSets ? context.Function<PFN_vkFreeDescriptorSets>("vkFreeDescriptorSets") : nullptr) {
    kept.reserve(KeptSets() + 1u);
}

DescriptorCache::~DescriptorCache() {
    for (const auto& pool : pools) destroyPool(context.device, pool.handle, nullptr);
    for (const auto& pool : updateAfterBindPools) destroyPool(context.device, pool.handle, nullptr);
    for (const auto pool : dedicated) destroyPool(context.device, pool, nullptr);
    for (const auto& set : kept) destroyPool(context.device, set.pool, nullptr);
    for (const auto& [key, layout] : layouts) destroyLayout(context.device, layout, nullptr);
}

VkDescriptorSetLayout DescriptorCache::Layout(std::span<const std::uint32_t> key, std::span<const VkDescriptorSetLayoutBinding> bindings, bool updateAfterBind, std::span<const VkDescriptorBindingFlags> flags) {
    std::vector<VkDescriptorBindingFlags> defaults;
    if (flags.empty()) {
        for (const auto& binding : bindings) defaults.push_back(updateAfterBind && binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT : 0);
        flags = defaults;
    }
    Require(flags.size() == bindings.size(), "descriptor binding flag count differs from layout");
    std::lock_guard lock(mutex);
    std::vector<std::uint32_t> keyCopy(key.begin(), key.end());
    keyCopy.push_back(static_cast<std::uint32_t>(flags.size()));
    keyCopy.insert(keyCopy.end(), flags.begin(), flags.end());
    if (const auto found = layouts.find(keyCopy); found != layouts.end()) {
        ++stats.layoutHits;
        return found->second;
    }
    ValidateDescriptorLayout(context, bindings, updateAfterBind);
    ++stats.layoutMisses;
    const auto layout = CreateSetLayout(context, bindings, updateAfterBind, flags);
    try {
        layouts.emplace(std::move(keyCopy), layout);
    } catch (...) {
        destroyLayout(context.device, layout, nullptr);
        throw;
    }
    return layout;
}

namespace {
std::uint32_t SetDescriptorLimit(const Context& context, VkDescriptorType type, bool updateAfterBind) {
    const auto& limits = context.limits;
    const auto& indexed = context.descriptorIndexingLimits;
    const bool sampledUpdateAfterBind = updateAfterBind && context.sampledImageUpdateAfterBind;
    switch (type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return updateAfterBind ? indexed.maxDescriptorSetUpdateAfterBindStorageBuffers : limits.maxDescriptorSetStorageBuffers;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return sampledUpdateAfterBind ? indexed.maxDescriptorSetUpdateAfterBindSampledImages : limits.maxDescriptorSetSampledImages;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return limits.maxDescriptorSetStorageImages;
        case VK_DESCRIPTOR_TYPE_SAMPLER: return sampledUpdateAfterBind ? indexed.maxDescriptorSetUpdateAfterBindSamplers : limits.maxDescriptorSetSamplers;
        default: throw std::runtime_error("AGC graphics: descriptor set uses an unsupported descriptor type " + std::to_string(type));
    }
}

// What one chain pool holds; a set needing more of any type gets a dedicated pool.
constexpr std::uint32_t ChainPoolSets = 1024;
constexpr std::array<VkDescriptorPoolSize, 4> ChainPoolSizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 512}}};
constexpr std::array<VkDescriptorPoolSize, 4> ChainPoolCreateSizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096 + 2}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 512}}};
}

DescriptorCache::SetAllocation DescriptorCache::Allocate(VkDescriptorSetLayout layout, std::span<const VkDescriptorPoolSize> sizes, bool updateAfterBind, std::optional<std::uint32_t> variableCount) {
    bool fits = true;
    for (const auto& size : sizes) {
        const auto limit = SetDescriptorLimit(context, size.type, updateAfterBind);
        Require(size.descriptorCount <= limit, "descriptor set exceeds the device's per-set descriptor limit");
        const auto capacity = std::find_if(ChainPoolSizes.begin(), ChainPoolSizes.end(), [&](const auto& item) { return item.type == size.type; });
        if (capacity == ChainPoolSizes.end() || size.descriptorCount > capacity->descriptorCount) fits = false;
    }
    std::lock_guard lock(mutex);
    const auto allocate = context.Resolved(&DeviceFunctions::allocateDescriptorSets, "vkAllocateDescriptorSets");
    const std::uint32_t variable = variableCount.value_or(0u);
    VkDescriptorSetVariableDescriptorCountAllocateInfoEXT variableInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT};
    variableInfo.descriptorSetCount = 1;
    variableInfo.pDescriptorCounts = &variable;
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.pNext = variableCount.has_value() ? &variableInfo : nullptr;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &layout;
    if (!fits) {
        dedicated.reserve(dedicated.size() + 1);
        const auto same = std::find_if(kept.begin(), kept.end(), [&](const SetAllocation& set) {
            return set.layout == layout && set.updateAfterBind == updateAfterBind && set.variableCount == variableCount && std::equal(set.sizes.begin(), set.sizes.end(), sizes.begin(), sizes.end(), [](const VkDescriptorPoolSize& a, const VkDescriptorPoolSize& b) { return a.type == b.type && a.descriptorCount == b.descriptorCount; });
        });
        if (same != kept.end()) {
            auto reused = std::move(*same);
            kept.erase(same);
            dedicated.push_back(reused.pool);
            ++stats.reusedSets;
            ++stats.sets;
            return reused;
        }
        const auto pool = CreateSetPool(context, sizes, updateAfterBind);
        allocation.descriptorPool = pool;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const auto result = allocate(context.device, &allocation, &set);
        if (result != VK_SUCCESS) {
            destroyPool(context.device, pool, nullptr);
            Check(result, "vkAllocateDescriptorSets");
        }
        dedicated.push_back(pool);
        ++stats.dedicatedPools;
        ++stats.sets;
        return {set, pool, 0, updateAfterBind, layout, std::vector<VkDescriptorPoolSize>(sizes.begin(), sizes.end()), variableCount, {}};
    }
    auto& chain = updateAfterBind ? updateAfterBindPools : pools;
    for (std::size_t index = chain.size(); index != 0; --index) {
        auto& pool = chain[index - 1];
        if (pool.exhausted) continue;
        allocation.descriptorPool = pool.handle;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const auto result = allocate(context.device, &allocation, &set);
        if (result == VK_SUCCESS) {
            ++stats.sets;
            ++pool.liveSets;
            return {set, pool.handle, index - 1, updateAfterBind, VK_NULL_HANDLE, {}, variableCount, {}};
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) Check(result, "vkAllocateDescriptorSets");
        pool.exhausted = true;
    }
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = updateAfterBind ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0;
    if (context.individuallyFreeDescriptorSets) poolInfo.flags |= VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = ChainPoolSets;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(ChainPoolCreateSizes.size());
    poolInfo.pPoolSizes = ChainPoolCreateSizes.data();
    chain.reserve(chain.size() + 1);
    VkDescriptorPool pool = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    chain.push_back({pool});
    ++stats.pools;
    allocation.descriptorPool = pool;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Check(allocate(context.device, &allocation, &set), "vkAllocateDescriptorSets");
    chain.back().liveSets = 1;
    ++stats.sets;
    return {set, pool, chain.size() - 1, updateAfterBind, VK_NULL_HANDLE, {}, variableCount, {}};
}

DescriptorCache::SetAllocation DescriptorCache::AllocateRewritten(VkDescriptorSetLayout layout, std::span<const VkDescriptorPoolSize> sizes, bool updateAfterBind, std::optional<std::uint32_t> variableCount) {
    {
        std::lock_guard lock(mutex);
        if (const auto found = recycled.find({layout, variableCount.value_or(0u)}); found != recycled.end() && !found->second.empty()) {
            const auto allocation = found->second.back();
            found->second.pop_back();
            return allocation;
        }
    }
    return Allocate(layout, sizes, updateAfterBind, variableCount);
}

void DescriptorCache::Recycle(VkDescriptorSetLayout layout, const SetAllocation& allocation) noexcept {
    if (allocation.set == VK_NULL_HANDLE || allocation.pool == VK_NULL_HANDLE) return;
    {
        std::lock_guard lock(mutex);
        try {
            auto& list = recycled[{layout, allocation.variableCount.value_or(0u)}];
            if (list.size() < 4096) {
                list.push_back(allocation);
                return;
            }
        } catch (...) {
        }
    }
    Free(allocation);
}

void DescriptorCache::Free(const SetAllocation& allocation, std::shared_ptr<const TableDescriptors> table) noexcept {
    if (allocation.set == VK_NULL_HANDLE || allocation.pool == VK_NULL_HANDLE) return;
    std::lock_guard lock(mutex);
    if (const auto found = std::find(dedicated.begin(), dedicated.end(), allocation.pool); found != dedicated.end()) {
        dedicated.erase(found);
        if (KeptSets() == 0u || allocation.layout == VK_NULL_HANDLE) {
            destroyPool(context.device, allocation.pool, nullptr);
            return;
        }
        if (kept.size() >= KeptSets()) {
            destroyPool(context.device, kept.back().pool, nullptr);
            kept.pop_back();
        }
        kept.insert(kept.begin(), allocation);
        kept.front().table = std::move(table);
        ++stats.keptSets;
        return;
    }
    auto& chain = allocation.updateAfterBind ? updateAfterBindPools : pools;
    if (allocation.poolIndex >= chain.size()) std::terminate();
    auto& pool = chain[allocation.poolIndex];
    if (pool.handle != allocation.pool || pool.liveSets == 0) std::terminate();
    if (freeDescriptorSets != nullptr && freeDescriptorSets(context.device, allocation.pool, 1, &allocation.set) != VK_SUCCESS) std::terminate();
    if (--pool.liveSets != 0) {
        static const bool reuse = std::getenv("APS5_NO_DESCRIPTOR_POOL_REUSE") == nullptr;
        if (reuse && pool.exhausted && freeDescriptorSets != nullptr && pool.liveSets <= ChainPoolSets / 2) {
            pool.exhausted = false;
            ++stats.reopenedPools;
        }
        return;
    }
    if (resetPool(context.device, pool.handle, 0) != VK_SUCCESS) std::terminate();
    pool.exhausted = false;
}

DescriptorCache::Stats DescriptorCache::Counters() const {
    std::lock_guard lock(mutex);
    auto counters = stats;
    for (const auto* chain : {&pools, &updateAfterBindPools}) {
        for (const auto& pool : *chain) {
            counters.liveSets += pool.liveSets;
            counters.exhaustedPools += pool.exhausted ? 1u : 0u;
        }
    }
    for (const auto& [key, list] : recycled) counters.recycledSets += list.size();
    return counters;
}

std::uint64_t ShaderResources::guestBufferBytes(std::span<const std::uint32_t> words) const {
    const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1] & ~0x40000000u, words[2], words[3]}};
    const auto address = descriptor.Base48();
    auto byteSize = descriptor.GetSize();
    if (byteSize > context.limits.maxStorageBufferRange) {
        const auto mappedEnd = RegisteredReadableEnd(address);
        if (mappedEnd <= address) byteSize = 0;
        else if (mappedEnd - address < context.limits.maxStorageBufferRange) byteSize = mappedEnd - address;
    }
    if (byteSize == 0 || address == 0) return 0;
    if (const std::uint64_t stride = descriptor.Stride(); stride != 0 && byteSize <= context.limits.maxStorageBufferRange) {
        constexpr std::uint64_t GuestPageBytes = 0x4000;
        const auto end = address + byteSize;
        const auto overhang = end % GuestPageBytes;
        if (overhang != 0 && overhang < stride) {
            const auto allocationEnd = RegisteredReadableEnd(address);
            if (allocationEnd > address && allocationEnd < end && end - allocationEnd < stride) byteSize = allocationEnd - address;
        }
    }
    return byteSize;
}

std::uint64_t ShaderResources::BytesUpToTarget(std::uint64_t address, std::uint64_t bytes, std::uint64_t stride, const ColorTarget* target) {
    if (target == nullptr || stride == 0 || address >= target->MemoryAddress() || address + bytes <= target->MemoryAddress() || address + bytes - target->MemoryAddress() >= stride) return bytes;
    return target->MemoryAddress() - address;
}

std::size_t ShaderResources::addGuestBuffer(std::span<const std::uint32_t> words, const ColorTarget* target, std::uint64_t indexAddress, std::size_t indexBytes, bool written, bool atomic, bool read, std::uint32_t extent) {
    Require(words.size() == 4, "buffer descriptor must contain four DWORDs");
    const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1] & ~0x40000000u, words[2], words[3]}};
    Require(descriptor.Type() == 0u, "buffer descriptor uses an unsupported type");
    const auto address = descriptor.Base48();
    const auto byteSize = guestBufferBytes(words);
    if (byteSize == 0 || address == 0) {
        allocations.push_back({0, EmptyBufferBytes, false, nullptr, ShaderRecompiler::DescriptorRole::GuestBuffers, false});
        allocations.back().oversized = address != 0 && descriptor.GetSize() != 0;
        return allocations.size() - 1;
    }
    Require(byteSize <= context.limits.maxStorageBufferRange, "shader buffer exceeds descriptor range limit");
    Require(byteSize <= std::numeric_limits<std::size_t>::max(), "shader buffer size exceeds host address space");
    const auto size = static_cast<std::size_t>(BytesUpToTarget(address, byteSize, descriptor.Stride(), written ? nullptr : target));
    Require(target == nullptr || !overlap(address, size, target->MemoryAddress(), target->bytes), "shader buffer aliases the render target");
    // APS5_ALL_BUFFERS_WRITTEN=1: every element is noted as written, as before bufferWritten existed.
    static const bool allWritten = std::getenv("APS5_ALL_BUFFERS_WRITTEN") != nullptr;
    Require(!written || !overlap(address, size, indexAddress, indexBytes), "writable shader buffer aliases the index buffer");
    written = written || allWritten;
    const std::uint64_t recordBytes = descriptor.Stride() != 0 ? descriptor.Stride() : extent;
    constexpr std::uint64_t WidestBufferStoreBytes = 16;
    const bool swept = (read || (context.discreteGpu && descriptor.Stride() > WidestBufferStoreBytes)) && dispatchThreads != 0 && recordBytes != 0 && dispatchThreads * recordBytes * 2u >= byteSize;
    if (written) guestMemory.AddWritable(address, size, atomic, swept);
    else {
        guestMemory.AddReadable(address, size);
        ++readOnlyBuffers;
    }
    if (BuildProfiled()) {
        auto& counters = BufferWrites();
        counters.elements.fetch_add(1, std::memory_order_relaxed);
        if (!written) counters.readOnly.fetch_add(1, std::memory_order_relaxed);
    }
    allocations.push_back({address, size, true, nullptr, ShaderRecompiler::DescriptorRole::ShaderData, written});
    return allocations.size() - 1;
}

std::string ShaderResources::Describe() const {
    const auto sample = [](std::uint64_t address, std::uint64_t bytes) {
        // Heaps bound whole can include uncommitted pages; those ranges are not sampled.
        if (!GuestMemory::Accessible(reinterpret_cast<const void*>(address), static_cast<std::size_t>(bytes))) return -1.0;
        std::size_t nonzero = 0;
        std::size_t samples = 0;
        const auto* data = reinterpret_cast<const std::uint32_t*>(address);
        const auto words = bytes / 4;
        const auto step = std::max<std::uint64_t>(1, words / 4096);
        for (std::uint64_t i = 0; i < words; i += step, ++samples) nonzero += data[i] != 0;
        return samples == 0 ? 0.0 : static_cast<double>(nonzero) / samples;
    };
    std::string text;
    char line[160];
    for (const auto& range : describedRanges) {
        std::snprintf(line, sizeof(line), " %s 0x%llx+0x%llx(%ux%u f%u t%d) nz=%.2f", range.kind, static_cast<unsigned long long>(range.address), static_cast<unsigned long long>(range.bytes), range.width, range.height, range.format, range.tileMode, sample(range.address, range.bytes));
        text += line;
        if (range.dccAddress != 0) {
            const auto keys = ReadDccKeys(range.dccAddress, range.bytes);
            std::snprintf(line, sizeof(line), " dcc=%s@0x%llx", DccKeysName(keys), static_cast<unsigned long long>(range.dccAddress));
            text += line;
            if (keys == DccKeys::Mixed) {
                // Where the keys change: first key, how many leading keys match it, and the next key.
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(range.dccAddress);
                const auto count = static_cast<std::size_t>(range.bytes / 256u);
                std::size_t run = 1;
                while (run < count && bytes[run] == bytes[0]) ++run;
                std::snprintf(line, sizeof(line), "(%02x x%zu then %02x of %zu)", bytes[0], run, run < count ? bytes[run] : 0u, count);
                text += line;
            }
        }
    }
    // APS5_TRACE_DISPATCH_IO=2 also lists the words of small buffers (constants).
    static const bool words = [] { const char* value = std::getenv("APS5_TRACE_DISPATCH_IO"); return value != nullptr && value[0] == '2'; }();
    const auto appendWords = [&](const std::uint32_t* data, std::size_t bytes) {
        if (!words || bytes > 0x200) return;
        text += " [";
        for (std::size_t i = 0; i < bytes / 4; ++i) {
            char word[12];
            std::snprintf(word, sizeof(word), "%s%08x", i == 0 ? "" : " ", data[i]);
            text += word;
        }
        text += "]";
    };
    for (const auto& allocation : allocations) {
        if (allocation.guest) {
            std::snprintf(line, sizeof(line), " buffer%s 0x%llx+0x%zx nz=%.2f", allocation.written ? "" : "(ro)", static_cast<unsigned long long>(allocation.address), allocation.size, sample(allocation.address, allocation.size));
            text += line;
            if (GuestMemory::Accessible(reinterpret_cast<const void*>(allocation.address), allocation.size)) appendWords(reinterpret_cast<const std::uint32_t*>(allocation.address), allocation.size);
        } else if (allocation.buffer) {
            const auto bytes = allocation.buffer->Bytes();
            std::snprintf(line, sizeof(line), " data+0x%zx nz=%.2f", allocation.size, sample(reinterpret_cast<std::uint64_t>(bytes.data()), allocation.size));
            text += line;
            appendWords(reinterpret_cast<const std::uint32_t*>(bytes.data()), allocation.size);
        }
    }
    return text;
}

std::size_t ShaderResources::addDataBuffer(std::span<const std::uint32_t> words) {
    const auto size = words.size() * sizeof(std::uint32_t);
    Require(size <= context.limits.maxStorageBufferRange, "shader data buffer exceeds descriptor range limit");
    const bool refreshable = TemplateDataRefresh() && size <= MaxRefreshBytes;
    auto buffer = std::make_unique<Buffer>(context, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::memcpy(buffer->Bytes().data(), words.data(), size);
    Allocation allocation{0, size, false, std::move(buffer)};
    if (refreshable && keepsTemplateRecords()) allocation.dataWords.assign(words.begin(), words.end());
    allocations.push_back(std::move(allocation));
    mixDataWords(dataWordsHash, allocations.back().dataWords);
    return allocations.size() - 1;
}

void ShaderResources::rehashDataWords() {
    dataWordsHash = FnvOffset;
    // Data buffers are the non-guest allocations with a buffer (an address-role allocation has
    // none), appended in binding order by buildPrepare: the order DataWordsHash(shader) hashes.
    for (const auto& allocation : allocations) {
        if (!allocation.guest && allocation.buffer != nullptr && allocation.role != ShaderRecompiler::DescriptorRole::ImageTableMap) mixDataWords(dataWordsHash, allocation.dataWords);
    }
}

std::uint64_t ShaderResources::DataWordsHash(const CompiledShader& shader) {
    Require(shader.program != nullptr, "missing compiled shader");
    std::uint64_t hash = FnvOffset;
    for (const auto& binding : shader.program->bindings) {
        if (DataRole(binding.role)) mixDataWords(hash, binding.guestDescriptor);
    }
    return hash;
}

bool ShaderResources::DataWordsDiffer(const CompiledShader& shader) const {
    Require(shader.program != nullptr, "missing compiled shader");
    const auto& program = *shader.program;
    for (const auto& binding : program.bindings) {
        if (!DataRole(binding.role)) continue;
        const auto held = std::ranges::find_if(bindings, [&](const Binding& candidate) { return candidate.layout.binding == binding.binding; });
        if (held == bindings.end() || held->allocations.size() != 1) return true;
        const auto& allocation = allocations[held->allocations.front()];
        if (allocation.dataWords.size() != binding.guestDescriptor.size() || !std::equal(allocation.dataWords.begin(), allocation.dataWords.end(), binding.guestDescriptor.begin())) return true;
    }
    return false;
}

bool ShaderResources::RefreshData(VkCommandBuffer commands, const CompiledShader& shader, Recorder* recorder) {
    Require(shader.program != nullptr, "missing compiled shader");
    Require(!refreshResourceKey.empty() && MatchesContentKey(shader, refreshResourceKey), "runtime data refresh cannot replace bound resources");
    return refreshData(commands, shader, recorder);
}

bool ShaderResources::refreshData(VkCommandBuffer commands, const CompiledShader& shader, Recorder* recorder) {
    Require(shader.program != nullptr, "missing compiled shader");
    const auto& program = *shader.program;
    bool recorded = false;
    // One [gputime] range of class TemplateDataRefresh around the updates, begun at the first one
    // (a refresh that finds every word equal records nothing and times nothing).
    auto timing = Recorder::NoTiming;
    std::uint64_t refreshedBytes = 0;
    for (const auto& binding : program.bindings) {
        if (!DataRole(binding.role)) continue;
        const auto held = std::ranges::find_if(bindings, [&](const Binding& candidate) { return candidate.layout.binding == binding.binding; });
        Require(held != bindings.end(), "template bindings disagree with the shader");
        Require(held->allocations.size() == 1, "data binding without its buffer");
        auto& allocation = allocations[held->allocations.front()];
        const auto size = binding.guestDescriptor.size() * sizeof(std::uint32_t);
        Require(allocation.buffer != nullptr && !allocation.guest && allocation.size == size && size <= MaxRefreshBytes, "template data buffer cannot take the dispatch's words");
        if (allocation.dataWords.size() == binding.guestDescriptor.size() && std::equal(allocation.dataWords.begin(), allocation.dataWords.end(), binding.guestDescriptor.begin())) continue;
        if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData) ValidateRuntimeResources(shader, binding.guestDescriptor);
        if (recorder != nullptr && timing == Recorder::NoTiming) timing = recorder->BeginGpuTiming(Recorder::CommandClass::TemplateDataRefresh);
        writeDataWords(commands, held->allocations.front(), binding.guestDescriptor);
        allocation.dataWords.assign(binding.guestDescriptor.begin(), binding.guestDescriptor.end());
        refreshedBytes += size;
        recorded = true;
    }
    if (timing != Recorder::NoTiming) recorder->EndGpuTiming(timing, refreshedBytes);
    if (recorded) rehashDataWords();
    return recorded;
}

std::optional<std::size_t> ShaderResources::shaderDataAllocation(const CompiledShader& shader) const {
    Require(shader.program != nullptr, "missing compiled shader");
    const auto& program = *shader.program;
    if (program.bindings.size() != bindings.size()) return std::nullopt;
    for (std::size_t index = 0; index < program.bindings.size(); ++index) {
        if (program.bindings[index].role != ShaderRecompiler::DescriptorRole::ShaderData || bindings[index].allocations.size() != 1) continue;
        const auto allocation = bindings[index].allocations.front();
        if (allocations[allocation].guest || allocations[allocation].buffer == nullptr) return std::nullopt;
        return allocation;
    }
    return std::nullopt;
}

VkBuffer ShaderResources::shaderDataBuffer(const CompiledShader& shader) const {
    const auto allocation = shaderDataAllocation(shader);
    return allocation.has_value() ? allocations[*allocation].buffer->Handle() : VK_NULL_HANDLE;
}

void ShaderResources::forgetShaderDataWords(const CompiledShader& shader) {
    if (const auto allocation = shaderDataAllocation(shader)) {
        allocations[*allocation].dataWords.clear();
        rehashDataWords();
    }
}

void ShaderResources::writeDataWords(VkCommandBuffer commands, std::size_t allocation, std::span<const std::uint32_t> words) const {
    const auto& buffer = *allocations[allocation].buffer;
    const auto size = words.size() * sizeof(std::uint32_t);
    if (std::none_of(dataPatches.begin(), dataPatches.end(), [&](const DataPatch& patch) { return patch.allocation == allocation; })) {
        context.Resolved(&DeviceFunctions::cmdUpdateBuffer, "vkCmdUpdateBuffer")(commands, buffer.Handle(), 0, size, words.data());
        return;
    }
    std::vector<std::uint32_t> patched(words.begin(), words.end());
    auto* bytes = reinterpret_cast<std::byte*>(patched.data());
    for (const auto& patch : dataPatches) {
        if (patch.allocation == allocation && patch.byte < size) bytes[patch.byte] = static_cast<std::byte>(patch.adjustment);
    }
    context.Resolved(&DeviceFunctions::cmdUpdateBuffer, "vkCmdUpdateBuffer")(commands, buffer.Handle(), 0, size, patched.data());
}

void ShaderResources::DescribedRanges(std::vector<std::pair<std::uint64_t, std::uint64_t>>& out) const {
    out.reserve(out.size() + describedRanges.size());
    for (const auto& range : describedRanges) out.emplace_back(range.address, range.address + range.bytes);
}

void ShaderResources::PrecollectSurfaces(bool memoVirtualQuery) const {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) {
        for (const auto& range : describedRanges) GuestMemory::CollectWrites(range.address, static_cast<std::size_t>(range.bytes), memoVirtualQuery);
        return;
    }
    struct Counts {
        std::uint64_t calls = 0;
        std::uint64_t ranges = 0;
        std::uint64_t failed = 0;
        std::uint64_t requested = 0;
        std::uint64_t walked = 0;
        double succeededMs = 0;
        double failedMs = 0;
    };
    struct CountsThreadStorage {};
    auto& counts = AgcDriver::ThreadScratch<std::array<Counts, 3>, CountsThreadStorage>();
    static thread_local auto reportedAt = std::chrono::steady_clock::now();
    const auto packet = GuestMemory::CurrentPacket();
    auto& count = counts[packet.opcode == 0x15 ? 0 : packet.opcode == 0x16 ? 1 : 2];
    ++count.calls;
    const auto walked = GuestMemory::ThreadCollectedBytes();
    for (const auto& range : describedRanges) {
        const auto start = std::chrono::steady_clock::now();
        const auto generation = GuestMemory::CollectWrites(range.address, static_cast<std::size_t>(range.bytes), memoVirtualQuery);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ++count.ranges;
        count.requested += range.bytes;
        if (generation == 0) {
            ++count.failed;
            count.failedMs += ms;
        } else count.succeededMs += ms;
    }
    count.walked += GuestMemory::ThreadCollectedBytes() - walked;
    const auto now = std::chrono::steady_clock::now();
    if (now - reportedAt < std::chrono::seconds(10)) return;
    reportedAt = now;
    constexpr std::array<const char*, 3> names{"direct", "indirect", "other"};
    for (std::size_t kind = 0; kind < counts.size(); ++kind) {
        const auto& total = counts[kind];
        if (total.calls == 0) continue;
        std::fprintf(stderr, "[precollect] queue 0x%x %s (10 s, %llu calls): %llu ranges (%llu unavailable), requested %.1f MiB, walked %.1f MiB; us per call: succeeded %.1f unavailable %.1f\n", packet.queue, names[kind], static_cast<unsigned long long>(total.calls), static_cast<unsigned long long>(total.ranges), static_cast<unsigned long long>(total.failed), static_cast<double>(total.requested) / 1048576.0, static_cast<double>(total.walked) / 1048576.0, total.succeededMs * 1000.0 / static_cast<double>(total.calls), total.failedMs * 1000.0 / static_cast<double>(total.calls));
    }
    counts = {};
}

void ShaderResources::addImageBinding(const ShaderRecompiler::DescriptorBinding& binding, VkShaderStageFlags flags) {
    Require(binding.count != 0, "empty descriptor binding");
    const auto heap = static_cast<ShaderRecompiler::RuntimeAbi::Binding>(binding.binding % static_cast<std::uint32_t>(ShaderRecompiler::RuntimeAbi::Binding::Count));
    const auto capacity = binding.count;
    Require(binding.count <= ShaderRecompiler::RuntimeAbi::HeapCapacity(heap), "typed descriptor heap capacity exceeded");
    const auto localBinding = static_cast<std::uint32_t>(heap);
    const bool sampledHeap = localBinding >= ShaderRecompiler::RuntimeAbi::FirstImageBinding && localBinding < ShaderRecompiler::RuntimeAbi::FirstStorageImageBinding;
    const bool storageHeap = localBinding >= ShaderRecompiler::RuntimeAbi::FirstStorageImageBinding && localBinding < static_cast<std::uint32_t>(ShaderRecompiler::RuntimeAbi::Binding::Samplers);
    Require((binding.kind == ShaderRecompiler::DescriptorKind::SampledImage && sampledHeap) || (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage && storageHeap) || (binding.kind == ShaderRecompiler::DescriptorKind::Sampler && heap == ShaderRecompiler::RuntimeAbi::Binding::Samplers), "resource class disagrees with typed descriptor heap");
    Require(binding.kind == ShaderRecompiler::DescriptorKind::Sampler || context.nullDescriptors || context.placeholderImages != nullptr, "typed image heaps require nullDescriptor or placeholder images");
    if (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) {
        // Storage images are guest textures the shader writes (looked up in stage B).
        Require(binding.role == ShaderRecompiler::DescriptorRole::GuestImages, "storage image binding has a non-image role");
        Require(binding.guestDescriptor.size() == static_cast<std::size_t>(binding.count) * 8u, "guest storage image descriptors must contain 8 dwords each");
        Require(context.detiler != nullptr, "device texture detiler is unavailable");
        Require(capacity <= context.limits.maxPerStageDescriptorStorageImages, "shader storage-image descriptors exceed per-stage limits");
        bindings.push_back({{binding.binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, capacity, flags, nullptr}, {}, {}});
        plannedStorageImages += binding.count;
        deferredImages.push_back({&binding, bindings.size() - 1});
        return;
    }
    const bool sampledImage = binding.kind == ShaderRecompiler::DescriptorKind::SampledImage;
    const bool samplerKind = binding.kind == ShaderRecompiler::DescriptorKind::Sampler;
    if (!(sampledImage || samplerKind)) Require(false, std::string("unsupported descriptor kind ") + kindName(binding.kind) + " for role " + roleName(binding.role));
    Require((sampledImage && binding.role == ShaderRecompiler::DescriptorRole::GuestImages) || (samplerKind && binding.role == ShaderRecompiler::DescriptorRole::GuestSamplers), "guest image descriptor role disagrees with its kind");
    Require(binding.guestDescriptor.size() % binding.count == 0, "guest image descriptor size is not a multiple of the binding count");
    const auto elementWords = binding.guestDescriptor.size() / binding.count;

    Binding item{{binding.binding, sampledImage ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, capacity, flags, nullptr}, {}, {}};

    if (sampledImage) {
        Require(elementWords == 8, "guest texture descriptor must contain 8 dwords");
        Require(binding.imageShape.has_value(), "guest image binding is missing an image shape");
        Require(context.detiler != nullptr, "device texture detiler is unavailable");
        Require(context.textureCache != nullptr, "device texture cache is unavailable");
        Require(capacity <= context.limits.maxPerStageDescriptorSampledImages, "shader sampled-image descriptors exceed per-stage limits");
        plannedSampledImages += binding.count;
        Require(plannedSampledImages <= context.limits.maxDescriptorSetSampledImages, "pipeline sampled-image descriptors exceed device limits");
    } else {
        Require(elementWords == 4, "guest sampler descriptor must contain 4 dwords");
        if (capacity > context.limits.maxPerStageDescriptorSamplers) throw std::runtime_error(DescriptorLimitError("shader sampler descriptors exceed per-stage limits", capacity, context.limits.maxPerStageDescriptorSamplers));
        Require(binding.Flags().samplerDepthCompare.size() == binding.count, "guest sampler binding is missing depth comparison metadata");
        Require(binding.Flags().samplerUnnormalized.empty() || binding.Flags().samplerUnnormalized.size() == binding.count, "guest sampler binding has unnormalized coordinate metadata of another size");
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            auto guestWords = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
            std::array<std::uint32_t, 4> normalizedWords{};
            if (element < binding.Flags().samplerCoordinatesIgnored.size() && binding.Flags().samplerCoordinatesIgnored[element]) {
                std::copy(guestWords.begin(), guestWords.end(), normalizedWords.begin());
                normalizedWords[0] &= ~(1u << 15u);
                guestWords = normalizedWords;
            }
            const auto words = guestWords;
            const bool compareEnable = binding.Flags().samplerDepthCompare.at(element);
            const bool unnormalized = element < binding.Flags().samplerUnnormalized.size() && binding.Flags().samplerUnnormalized[element];
            const bool integerBorder = element < binding.Flags().samplerIntegerBorder.size() && binding.Flags().samplerIntegerBorder[element];
            if (context.samplerCache != nullptr && !SamplerCacheDisabled()) {
                samplers.push_back(context.samplerCache->Get(context, words, compareEnable, unnormalized, true, integerBorder));
            } else {
                auto resource = DecodeSamplerResource(words, unnormalized, true, integerBorder);
                resource.compareEnable = compareEnable;
                samplers.push_back(std::make_shared<Sampler>(context, resource));
            }
            samplerSources.push_back({{words[0], words[1], words[2], words[3]}, compareEnable, unnormalized, integerBorder});
            item.imageAllocations.push_back(samplers.size() - 1);
        }
        Require(samplers.size() <= context.limits.maxDescriptorSetSamplers, "pipeline sampler descriptors exceed device limits");
    }

    bindings.push_back(std::move(item));
    if (sampledImage) deferredImages.push_back({&binding, bindings.size() - 1});
}

namespace {

// Calls `visit(binding, element, words)` for every sampled and storage image element of a compiled
// shader's bindings, in plan order (the order addImageBinding and resolveImageBinding walk).
template <typename Visit>
void forEachImageElement(const ShaderRecompiler::RecompileResult& program, Visit&& visit) {
    for (const auto& binding : program.bindings) {
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages || binding.count == 0) continue;
        if (binding.kind != ShaderRecompiler::DescriptorKind::SampledImage && binding.kind != ShaderRecompiler::DescriptorKind::StorageImage) continue;
        const auto elementWords = binding.guestDescriptor.size() / binding.count;
        for (std::uint32_t element = 0; element < binding.count; ++element) visit(binding, element, std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords));
    }
}

}

bool ShaderResources::precollectImages() {
    // The image lookups of stage B (cachedTexture, StorageTexture::Refresh) each start with a
    // GuestMemory::CollectWrites of their surface, the GetWriteWatch walk that is most of the
    // lookups' time under the lock. The walk is memoized per thread epoch, and the worker thread
    // passes no ordering point between the stages (see GuestMemory::BumpCollectEpoch), so walking
    // every surface here makes stage B's collects memo hits: the walk leaves the lock, the lookups'
    // checks stay where they were. A memo miss (the ring overflowed, an uncommitted page) just walks
    // under the lock as before; a descriptor stage B rejects is left to it. APS5_NO_PRECOLLECT=1
    // disables the pass.
    // The pass also leaves one ImageRecord per element: the decode and the surface description are
    // made once for the build, and a sampled element's cache entry is taken here (a hash lookup
    // under the cache's own mutex) so that stage B, under the device lock, runs only the
    // fastRevalidate predicate on it (see fastTexture). The collect comes before the entry is read,
    // as the predicate's rule demands (the generation the entry moves to must predate the checks).
    static const bool disabled = std::getenv("APS5_NO_PRECOLLECT") != nullptr;
    static const bool noRecords = std::getenv("APS5_NO_STAGE_A_IMAGES") != nullptr;
    if (disabled || deferredImages.empty()) return false;
    imageRecords.clear();
    nextImageRecord = 0;
    auto& counters = TextureCounts();
    for (const auto& deferred : deferredImages) {
        const auto& binding = *deferred.binding;
        const auto elementWords = binding.guestDescriptor.size() / binding.count;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const bool sampled = binding.kind == ShaderRecompiler::DescriptorKind::SampledImage;
            const auto raw = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
            std::array<std::uint32_t, 8> narrowed{};
            const auto words = firstLayerWords(raw, sampled ? binding.imageShape : std::nullopt, narrowed, false);
            if (sampled && !imageRecords.empty()) {
                const auto& previous = imageRecords.back();
                if (previous.sampled && previous.decoded && words.size() == 8 && std::equal(words.begin(), words.end(), previous.words.begin()) && (binding.Flags().imageDepthCompare.empty() || !binding.Flags().imageDepthCompare.at(element))) {
                    ImageRecord repeated;
                    repeated.sampled = true;
                    repeated.decoded = previous.decoded;
                    repeated.words = previous.words;
                    repeated.resource = previous.resource;
                    repeated.guestBytes = previous.guestBytes;
                    repeated.components = previous.components;
                    imageRecords.push_back(repeated);
                    continue;
                }
            }
            ImageRecord record;
            record.sampled = sampled;
            if (sampled ? !IsNullSampledTextureResource(words) : !IsNullTextureResource(words)) {
                const auto* memo = words.size() == 8 ? &decodedImage(context.device, words) : nullptr;
                record.resource = memo != nullptr ? memo->resource : DecodeTextureResource(words);
                record.guestBytes = memo != nullptr ? memo->guestBytes : SurfaceGuestBytes(record.resource);
                record.generation = GuestMemory::CollectWrites(record.resource.baseAddress, static_cast<std::size_t>(record.guestBytes));
                record.decoded = true;
                if (record.sampled && !noRecords && words.size() == 8 && (binding.Flags().imageDepthCompare.empty() || !binding.Flags().imageDepthCompare.at(element))) {
                    std::copy(words.begin(), words.end(), record.words.begin());
                    record.components = memo->components;
                    record.keys = TextureClearKeys(record.resource, record.guestBytes);
                    auto& cache = Textures();
                    std::lock_guard lock(cache.mutex);
                    if (const auto it = findSampledEntry(cache, context.device, record.words, record.components); it != cache.entries.end()) {
                        record.texture = it->texture;
                        record.source = it->source;
                        record.entryKeys = it->keys;
                        record.entryGeneration = it->generation;
                        if (TextureCountersReported()) counters.records.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            imageRecords.push_back(std::move(record));
        }
    }
    return true;
}

std::shared_ptr<Texture> ShaderResources::fastTexture(ImageRecord& record) {
    const GuestMemory::Gen::ConsumerScope genConsumer(GuestMemory::Gen::Consumer::TextureLookup);
    if (record.texture == nullptr || record.texture->RefreshedPerUse() || record.texture->IsDepthCopy()) return nullptr;
    struct Outcome {
        bool profile;
        std::chrono::steady_clock::time_point start;
        bool hit = false;
        ~Outcome() {
            if (profile) LookupOutcomes::Add(hit ? LookupOutcomes::SampledFast : LookupOutcomes::SampledFastMiss, start);
        }
    } outcome{LookupOutcomes::Profiled(), LookupOutcomes::Profiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
    // Exactly the "unchanged" branches of cachedTexture, from write stamps, the pending-results
    // registry and the DCC keys, the way fastRevalidate proves a built object current: the collect
    // first (a memo hit: stage A walked the range), then no other image may have results pending
    // over the memory (a lookup would flush them into it, or view them instead), a storage-sourced
    // view needs its image to be the cache's image of the surface and current with guest memory,
    // a snapshot needs the memory unchanged since its content matched, and the keys must be what
    // the content was made under whenever the surface has any (re-read under the lock: a flush
    // between the stages marks them uncompressed).
    const auto address = record.resource.baseAddress;
    const auto bytes = static_cast<std::size_t>(record.guestBytes);
    RequireNoMultisampleColorTarget(address, bytes != 0 ? bytes : 1u, "a texture read");
    if (ArchB::Switches::Enabled(ArchB::Feature::DepthArrayGpu) && DepthStalePlaneOverlaps(address, address + bytes)) return nullptr;
    if (DepthSurfaceAt(address) || !FlushDepthPlanes(address, bytes).empty()) return nullptr;
    if (GuestMemory::CollectWrites(address, bytes) == 0) return nullptr;
    if (PendingStorageOverlaps(address, bytes, record.source.get())) return nullptr;
    auto keys = record.keys;
    if (record.resource.dccAddress != 0) {
        const auto scanStart = outcome.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        // Through the proof of the image whose keys they are (the view's source when the surface
        // is the image's, else the texture's): a scan only when the key range was stamped.
        keys = ProvedClearKeys(record.resource, record.guestBytes, SameKeySurface(record.source.get(), record.resource, record.guestBytes) ? record.source->KeyProof() : record.texture->KeyProof());
        if (outcome.profile) LookupOutcomes::Add(LookupOutcomes::DccScan, scanStart);
    }
    // The keys the entry was made under (a view of a pending render target was made under its clear
    // keys and stays valid while they are unchanged); a change is the full lookup's to judge.
    if (keys != record.entryKeys) return nullptr;
    if (record.source != nullptr) {
        if (!StorageImageCached(context, record.source.get()) || !GuestMemory::UnchangedSince(address, bytes, record.source->GenerationFor(address, bytes))) return nullptr;
        if (record.resource.dccAddress != 0 && IsDccClear(record.source->FilledKeys())) return nullptr;
        // Under fast-clear keys the view holds only while its image's results are still pending over
        // the surface; flushed, the clear the image cannot see makes the lookup take a snapshot.
        if (keys != DccKeys::Uncompressed && StorageTexture::FindPending(address, record.guestBytes) != record.source) return nullptr;
    } else if ((keys == DccKeys::Uncompressed || keys == DccKeys::ClearSingle) && !GuestMemory::UnchangedSince(address, bytes, record.entryGeneration)) {
        return nullptr;
    }
    // The entry must still be the cache's, holding this object (an evicted object is not reused: the
    // next lookup would make another), and it takes the stage-A generation like a hit would.
    auto& cache = Textures();
    std::lock_guard lock(cache.mutex);
    const auto it = findSampledEntry(cache, context.device, record.words, record.components);
    if (it == cache.entries.end() || it->texture != record.texture || it->committed.has_value()) return nullptr;
    if (it->source == nullptr) it->generation = record.generation;
    touchTexture(cache, it);
    logLookup({record.texture.get(), record.resource, record.guestBytes, keys, record.source != nullptr ? 0 : record.generation, record.source.get()});
    outcome.hit = true;
    return std::move(record.texture);
}

void ShaderResources::applySamplerVariants() {
    for (std::size_t index = 0; index < samplerSources.size(); ++index) {
        const auto& source = samplerSources[index];
        const auto variant = SingleLevelSamplerWords(source.words, source.singleLevelImage, source.mipmappedImage);
        if (!variant.has_value() && !source.pinnedLod.has_value()) continue;
        const auto words = source.pinnedLod.has_value() ? PinnedLodSamplerWords(variant.has_value() ? *variant : source.words, *source.pinnedLod) : *variant;
        if (context.samplerCache != nullptr && !SamplerCacheDisabled()) {
            samplers[index] = context.samplerCache->Get(context, words, source.compareEnable, source.unnormalized, true, source.integerBorder);
        } else {
            auto resource = DecodeSamplerResource(words, source.unnormalized, true, source.integerBorder);
            resource.compareEnable = source.compareEnable;
            samplers[index] = std::make_shared<Sampler>(context, resource);
        }
    }
}

void ShaderResources::resolveImageBinding(const ShaderRecompiler::DescriptorBinding& binding, Binding& item, std::span<const std::shared_ptr<Sampler>> shaderSamplers) {
    auto& counters = TextureCounts();
    // The element's stage-A record, when the pass ran (records follow the plan order exactly).
    const auto nextRecord = [&]() -> ImageRecord* { return nextImageRecord < imageRecords.size() ? &imageRecords[nextImageRecord++] : nullptr; };
    if (binding.kind == ShaderRecompiler::DescriptorKind::SampledImage) {
        Require(binding.imageSamplers.size() == binding.count, "guest sampled image binding is missing its image-sampler pairs");
        const auto elementWords = binding.guestDescriptor.size() / binding.count;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const auto raw = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
            std::array<std::uint32_t, 8> narrowed{};
            const auto words = firstLayerWords(raw, binding.imageShape, narrowed, true);
            auto* record = nextRecord();
            if (IsNullSampledTextureResource(words)) {
                item.imageAllocations.push_back(std::numeric_limits<std::size_t>::max());
                --plannedSampledImages;
                continue;
            }
            const auto resource = record != nullptr && record->decoded ? record->resource : DecodeTextureResource(words);
            const auto minLod = context.shaderImageMinLod && resource.minLod > resource.baseLevel * 256u && resource.lastLevel > resource.baseLevel ? std::min(resource.minLod - resource.baseLevel * 256u, (resource.lastLevel - resource.baseLevel) * 256u) : 0u;
            if (minLod != 0u && !shaderSamplers.empty()) {
                const auto firstSampler = static_cast<std::size_t>(shaderSamplers.data() - samplers.data());
                for (std::uint32_t sampler = 0; sampler < shaderSamplers.size() && sampler < 32u; ++sampler) {
                    auto& source = samplerSources.at(firstSampler + sampler);
                    if (((binding.imageSamplers[element] >> sampler) & 1u) == 0 || !HoldsLodAtZero(source.words)) continue;
                    Require(!source.pinnedLod.has_value() || *source.pinnedLod == minLod, "an S# that holds the LOD at 0 is paired with images of different minimum LODs");
                    source.pinnedLod = minLod;
                }
            }
            const bool forcedSrgb = ForcesDegammaOn(resource.format, binding.imageSamplers[element], shaderSamplers);
            RequireNonSeamlessCube(resource.dimension == TextureDimension::kCube, binding.imageSamplers[element], shaderSamplers);
            RequireDisablesDegammaOn(resource.format, binding.imageSamplers[element], shaderSamplers);
            const bool depthCompare = !binding.Flags().imageDepthCompare.empty() && binding.Flags().imageDepthCompare.at(element);
            const bool firstLayer = binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2D && (resource.dimension == TextureDimension::k2DArray || (depthCompare && resource.dimension == TextureDimension::k1DArray));
            const bool depth1D = depthCompare && ((binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2D && resource.dimension == TextureDimension::k1D) || (binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2DArray && resource.dimension == TextureDimension::k1DArray));
            const bool firstSample = binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2D && resource.dimension == TextureDimension::k2DMsaa;
            const bool layerRows = layerRowsBinding(binding.imageShape, resource, depthCompare);
            Require(!layerRows || (resource.baseArray <= resource.depthOrLastArray && resource.depthOrLastArray - resource.baseArray >= context.limits.maxImageArrayLayers), "a 1D array within maxImageArrayLayers was bound as rows of a 2D image");
            if (!firstLayer && !depth1D && !firstSample && !layerRows && !MatchesGuestDimension(*binding.imageShape, resource.dimension)) throw std::runtime_error("AGC graphics: guest texture dimension disagrees with the shader's declared image shape (shape " + std::to_string(static_cast<int>(*binding.imageShape)) + ", dimension " + std::to_string(static_cast<int>(resource.dimension)) + ")");
            const VkComponentMapping components = ViewComponents(resource);
            const auto guestBytes = record != nullptr && record->decoded ? record->guestBytes : SurfaceGuestBytes(resource);
            CompletePendingCpuWrites(context, resource.baseAddress, static_cast<std::size_t>(guestBytes));
            if (resource.dccAddress != 0) CompletePendingCpuWrites(context, resource.dccAddress, DccKeyBytes(guestBytes));
            std::shared_ptr<Texture> texture;
            const VkImageViewType multisampleViewType = binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2DMultisampleArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
            if (previousSampled.texture != nullptr && previousSampled.depthCompare == depthCompare && previousSampled.multisampleViewType == multisampleViewType && previousSampled.firstSample == firstSample && previousSampled.layerRows == layerRows && words.size() == 8 && std::equal(words.begin(), words.end(), previousSampled.words.begin())) texture = previousSampled.texture;
            if (texture == nullptr && !firstSample && record != nullptr && record->texture != nullptr) {
                texture = fastTexture(*record);
                if (TextureCountersReported()) (texture != nullptr ? counters.fastHits : counters.fastMisses).fetch_add(1, std::memory_order_relaxed);
            }
            if (texture == nullptr && firstSample) {
                Require(!depthCompare, "reading sample 0 of a multisampled texture with a depth comparison through a 2D image is not implemented");
                texture = DepthSurfaceAt(resource.baseAddress) ? DepthSurfaceFirstSample(context, words, resource, components) : MultisampleTargetFirstSample(context, resource, components);
            }
            if (texture == nullptr) texture = cachedTexture(context, words, resource, components, guestBytes, depthCompare, multisampleViewType, layerRows);
            if (resource.dimension == TextureDimension::k2DMsaa) multisampleSources.push_back(resource.baseAddress);
            bool baseLevel = false;
            if (element < binding.Flags().imageUnnormalized.size() && binding.Flags().imageUnnormalized[element]) {
                const auto range = texture->SampledViewRange(firstLayer);
                const bool exactAtBase = element < binding.Flags().imageUnnormalizedBaseLevel.size() && binding.Flags().imageUnnormalizedBaseLevel[element];
                const bool singleLevel = range.levels == 1u && resource.baseLevel == 0u;
                const bool supported = range.layers == 1u && EffectiveMinLod(resource) == 0.0f && (singleLevel || exactAtBase);
                if (!supported || (range.type != VK_IMAGE_VIEW_TYPE_1D && range.type != VK_IMAGE_VIEW_TYPE_2D)) throw std::runtime_error("AGC graphics: guest texture sampled with unnormalized coordinates is not a single-layer 1D or 2D view without a minimum LOD clamp, and is neither a single-level view at mip 0 nor read only by image_sample_lz through an S# without minimum LOD or bias, which is not implemented (base level " + std::to_string(resource.baseLevel) + ", levels " + std::to_string(range.levels) + ", layers " + std::to_string(range.layers) + ", view type " + std::to_string(static_cast<int>(range.type)) + ")");
                baseLevel = range.levels > 1u;
            }
            RequireFilterMinmax(context, texture->ViewFormat(), binding.imageSamplers[element], shaderSamplers);
            RequireBorderSwizzle(resource.bcSwizzle, binding.imageSamplers[element], shaderSamplers);
            if (forcedSrgb && !texture->HasSrgbView()) Require(false, "a texture of guest format " + std::to_string(resource.format) + ", which is not sRGB and has no sRGB view, is sampled through a sampler that forces sRGB decoding, which is not implemented");
            if (!shaderSamplers.empty()) {
                const bool singleLevel = DescriptorSingleLevel(words);
                const auto firstSampler = static_cast<std::size_t>(shaderSamplers.data() - samplers.data());
                for (auto mask = binding.imageSamplers[element]; mask != 0u; mask &= mask - 1u) {
                    const auto sampler = static_cast<std::uint32_t>(std::countr_zero(mask));
                    if (sampler >= shaderSamplers.size()) break;
                    auto& source = samplerSources.at(firstSampler + sampler);
                    (singleLevel ? source.singleLevelImage : source.mipmappedImage) = true;
                }
            }
            if (words.size() == 8) {
                std::copy(words.begin(), words.end(), previousSampled.words.begin());
                previousSampled.depthCompare = depthCompare;
                previousSampled.multisampleViewType = multisampleViewType;
                previousSampled.firstSample = firstSample;
                previousSampled.layerRows = layerRows;
                previousSampled.texture = texture;
            }
            if (depthCompare && !firstLayer && !depth1D && !firstSample && !layerRows && words.size() == 8u && resource.dimension == TextureDimension::k2DArray) {
                depthArrayRanges.emplace_back(resource.baseAddress, guestBytes);
            }
            textures.push_back(std::move(texture));
            textureFirstLayer.push_back(firstLayer);
            textureBaseLevel.push_back(baseLevel);
            textureSrgb.push_back(forcedSrgb);
            describedRanges.push_back({"texture", resource.baseAddress, guestBytes, resource.width, resource.height, resource.format, static_cast<int>(resource.tileMode), resource.dccAddress});
            item.imageAllocations.push_back(textures.size() - 1);
        }
        // The line is due even when every element took the fast path (cachedTexture reports too).
        reportTextureCounters();
        return;
    }
    previousSampled = {};
    // Consecutive identical storage descriptors address successive mips of one texture (dynamic-mip
    // storage writes).
    std::uint32_t mipOffset = 0;
    for (std::uint32_t element = 0; element < binding.count; ++element) {
        const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u);
        const bool sameAsPrevious = SameAsPreviousStorageElement(binding, element);
        if (sameAsPrevious) ++mipOffset;
        else mipOffset = 0;
        const auto* record = nextRecord();
        if (IsNullTextureResource(words)) {
            item.imageAllocations.push_back(std::numeric_limits<std::size_t>::max());
            --plannedStorageImages;
            continue;
        }
        const auto resource = record != nullptr && record->decoded ? record->resource : DecodeTextureResource(words);
        const bool firstLayer = binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2D && resource.dimension == TextureDimension::k2DArray;
        if (binding.imageShape.has_value() && !firstLayer && !MatchesGuestDimension(*binding.imageShape, resource.dimension)) throw std::runtime_error("AGC graphics: guest storage texture dimension disagrees with the shader's declared image shape (shape " + std::to_string(static_cast<int>(*binding.imageShape)) + ", dimension " + std::to_string(static_cast<int>(resource.dimension)) + ")");
        if (resource.dimension == TextureDimension::k2DMsaa) {
            Require(!(element < binding.Flags().imageAtomic.size() && binding.Flags().imageAtomic[element]) && !(element < binding.Flags().imageAtomic64.size() && binding.Flags().imageAtomic64[element]), "storage image atomics on a multisampled texture are not implemented");
            storageTextures.push_back(nullptr);
            multisampleStorageViews.resize(storageTextures.size(), VK_NULL_HANDLE);
            const bool uintStorage = ShaderRecompiler::RuntimeAbi::UintStorageImageBinding(binding.binding);
            multisampleStorageViews.back() = MultisampleTargetStorageView(context, resource, uintStorage);
            multisampleSources.push_back(resource.baseAddress);
            storageMips.push_back(0);
            storageKeys.push_back(0);
            storageFirstLayer.push_back(false);
            storageWritten.push_back(false);
            storageAtomic.push_back(false);
            storageAtomic64.push_back(false);
            item.imageAllocations.push_back(storageTextures.size() - 1);
            continue;
        }
        const auto mip = std::min(resource.baseLevel + mipOffset, resource.mipCount - 1u);
        Require(resource.minLod <= mip * 256u, "guest storage texture descriptor clamps its minimum LOD above the level it addresses, which is not implemented");
        const auto guestBytes = record != nullptr && record->decoded ? record->guestBytes : SurfaceGuestBytes(resource);
        CompletePendingCpuWrites(context, resource.baseAddress, static_cast<std::size_t>(guestBytes));
        if (resource.dccAddress != 0) CompletePendingCpuWrites(context, resource.dccAddress, DccKeyBytes(guestBytes));
        // Images the shader only reads have nothing to store back.
        const bool written = element >= binding.Flags().imageWritten.size() || binding.Flags().imageWritten[element];
        // The same surface as the previous element: its image was just looked up and refreshed.
        if (sameAsPrevious && StorageDedupeEnabled()) storageTextures.push_back(storageTextures.back());
        else storageTextures.push_back(cachedStorageTexture(context, words, resource, mip, guestBytes, written));
        storageMips.push_back(mip);
        storageKeys.push_back(resource.dccAddress);
        storageFirstLayer.push_back(firstLayer);
        storageWritten.push_back(written);
        storageAtomic.push_back(element < binding.Flags().imageAtomic.size() && binding.Flags().imageAtomic[element]);
        storageAtomic64.push_back(element < binding.Flags().imageAtomic64.size() && binding.Flags().imageAtomic64[element]);
        describedRanges.push_back({"storage", resource.baseAddress, guestBytes, resource.width, resource.height, resource.format, static_cast<int>(resource.tileMode), resource.dccAddress});
        item.imageAllocations.push_back(storageTextures.size() - 1);
    }
}

std::uint32_t StorageTableCapacity(const Context& context) {
    return std::min<std::uint32_t>({context.imageTableCapacity, 4096u, context.limits.maxPerStageDescriptorStorageImages / 4u});
}

namespace {

std::uint64_t Room(std::uint64_t limit, std::uint64_t reserve) {
    return limit > reserve ? limit - reserve : 0u;
}

std::uint32_t ImageTableCapacityFor(const Context& context, bool updateAfterBind) {
    if (!context.runtimeDescriptorArray || !context.partiallyBound || !context.variableDescriptorCount || !context.descriptorIndexing) return 0u;
    if (updateAfterBind && !context.sampledImageUpdateAfterBind) return 0u;
    constexpr std::uint64_t sampledHeaps = static_cast<std::uint64_t>(ShaderRecompiler::RuntimeAbi::FirstStorageImageBinding - ShaderRecompiler::RuntimeAbi::FirstImageBinding) * ShaderRecompiler::RuntimeAbi::SampledHeapCapacity;
    constexpr std::uint64_t storageHeaps = static_cast<std::uint64_t>(static_cast<std::uint32_t>(ShaderRecompiler::RuntimeAbi::Binding::Samplers) - ShaderRecompiler::RuntimeAbi::FirstStorageImageBinding) * ShaderRecompiler::RuntimeAbi::StorageHeapCapacity;
    constexpr std::uint64_t buffers = ShaderRecompiler::RuntimeAbi::BufferCapacity + 8u;
    const auto& indexed = context.descriptorIndexingLimits;
    const std::uint64_t stageImages = updateAfterBind ? indexed.maxPerStageDescriptorUpdateAfterBindSampledImages : context.limits.maxPerStageDescriptorSampledImages;
    const std::uint64_t stageResources = updateAfterBind ? indexed.maxPerStageUpdateAfterBindResources : context.limits.maxPerStageResources;
    const std::uint64_t setImages = updateAfterBind ? indexed.maxDescriptorSetUpdateAfterBindSampledImages : context.limits.maxDescriptorSetSampledImages;
    std::uint64_t capacity = static_cast<std::uint64_t>(ShaderRecompiler::RuntimeAbi::StageCount) * ShaderRecompiler::ImageTableAbi::ImageElementBudget;
    capacity = std::min(capacity, Room(stageImages, sampledHeaps));
    capacity = std::min(capacity, Room(stageResources, sampledHeaps + storageHeaps + buffers));
    capacity = std::min(capacity, Room(setImages, sampledHeaps * ShaderRecompiler::RuntimeAbi::StageCount));
    return static_cast<std::uint32_t>(capacity);
}

std::uint32_t SamplerTableCapacityFor(const Context& context, bool updateAfterBind) {
    if (!context.runtimeDescriptorArray || !context.partiallyBound) return 0u;
    if (updateAfterBind && !context.sampledImageUpdateAfterBind) return 0u;
    const auto& indexed = context.descriptorIndexingLimits;
    const std::uint64_t stageSamplers = updateAfterBind ? indexed.maxPerStageDescriptorUpdateAfterBindSamplers : context.limits.maxPerStageDescriptorSamplers;
    const std::uint64_t setSamplers = updateAfterBind ? indexed.maxDescriptorSetUpdateAfterBindSamplers : context.limits.maxDescriptorSetSamplers;
    std::uint64_t capacity = ShaderRecompiler::ImageTableAbi::SamplerElementBudget;
    capacity = std::min(capacity, Room(stageSamplers, ShaderRecompiler::RuntimeAbi::SamplerHeapCapacity));
    capacity = std::min(capacity, Room(setSamplers / ShaderRecompiler::RuntimeAbi::StageCount, ShaderRecompiler::RuntimeAbi::SamplerHeapCapacity));
    return static_cast<std::uint32_t>(capacity - capacity % ShaderRecompiler::ImageTableAbi::SamplerEntryElements);
}

}

std::uint64_t& DeviceProcLookups() {
    struct CountStorage {};
    return AgcDriver::ThreadScratch<std::uint64_t, CountStorage>();
}

std::uint32_t ImageTableCapacity(const Context& context) {
    const auto capacity = ImageTableCapacityFor(context, false);
    return capacity != 0u ? capacity : ImageTableCapacityFor(context, true);
}

std::uint32_t SamplerTableCapacity(const Context& context) {
    const auto capacity = SamplerTableCapacityFor(context, false);
    return capacity != 0u ? capacity : SamplerTableCapacityFor(context, true);
}

bool ImageTableUpdateAfterBind(const Context& context) {
    return ImageTableCapacityFor(context, false) == 0u && ImageTableCapacityFor(context, true) != 0u;
}

bool SamplerTableUpdateAfterBind(const Context& context) {
    return SamplerTableCapacityFor(context, false) == 0u && SamplerTableCapacityFor(context, true) != 0u;
}

ShaderResources::TableStage& ShaderResources::tableStage(const CompiledShader& shader) {
    for (auto& table : tableStages) {
        if (table.program == shader.program) return table;
    }
    auto& table = tableStages.emplace_back();
    table.stage = shader.stage;
    table.program = shader.program;
    table.poison = shader.program->imageTablePoison;
    table.messages.assign(table.poison.size(), nullptr);
    return table;
}

std::uint32_t ShaderResources::rejectTableElement(TableStage& table, std::span<const std::uint32_t> words, const std::string& message) {
    ShaderRecompiler::ImageTableEntryPoison poison{};
    std::copy(words.begin(), words.end(), poison.words.begin());
    poison.dwordCount = static_cast<std::uint32_t>(words.size());
    poison.resource = ShaderRecompiler::NoTable;
    poison.reason = static_cast<std::uint32_t>(ShaderRecompiler::ImageTableAbi::PoisonReason::DriverRejected);
    table.poison.push_back(poison);
    const bool repeated = !table.messages.empty() && table.messages.back() != nullptr && *table.messages.back() == message;
    table.messages.push_back(repeated ? table.messages.back() : std::make_shared<const std::string>(message));
    faultChecks = true;
    return ShaderRecompiler::ImageTableAbi::PoisonCode(static_cast<std::uint32_t>(table.poison.size() - 1u));
}

void ShaderResources::addSamplerTable(const ShaderRecompiler::DescriptorBinding& binding, VkShaderStageFlags flags, TableStage& table) {
    namespace Abi = ShaderRecompiler::ImageTableAbi;
    Require(binding.kind == ShaderRecompiler::DescriptorKind::Sampler && binding.guestDescriptor.size() == static_cast<std::size_t>(binding.count) * 4u && binding.Flags().samplerDepthCompare.size() == binding.count && binding.count % Abi::SamplerEntryElements == 0u, "sampler table binding is malformed");
    Require(context.samplerTableCapacity != 0, "sampler tables need runtimeDescriptorArray and descriptorBindingPartiallyBound, which the device lacks");
    const VkDescriptorBindingFlags samplerTableFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | (context.samplerTableUpdateAfterBind ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT : 0);
    Binding item{{binding.binding, VK_DESCRIPTOR_TYPE_SAMPLER, context.samplerTableCapacity, flags, nullptr}, {}, {}, samplerTableFlags};
    static const bool noSamplerCache = std::getenv("APS5_NO_SAMPLER_CACHE") != nullptr;
    for (std::uint32_t entry = 0; entry < binding.count / Abi::SamplerEntryElements; ++entry) {
        const auto first = entry * Abi::SamplerEntryElements;
        const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(first) * 4u, 4u);
        std::string rejected;
        if (first + Abi::SamplerEntryElements > context.samplerTableCapacity) rejected = "the sampler table exceeds the device's capacity of " + std::to_string(context.samplerTableCapacity) + " samplers";
        for (std::uint32_t element = first; rejected.empty() && element < first + Abi::SamplerEntryElements; ++element) {
            try {
                const auto elementWords = std::span<const std::uint32_t, 4>(binding.guestDescriptor.data() + static_cast<std::size_t>(element) * 4u, 4u);
                auto resource = DecodeSamplerResource(elementWords);
                resource.compareEnable = binding.Flags().samplerDepthCompare.at(element);
                Sampler::Validate(context, resource);
                Require(!Sampler::ShaderAppliesLodBias(resource), "a sampler table entry with a LOD bias beyond the sampler limit needs a per-entry shader bias, which is not implemented");
                Require(!Sampler::ReadsOpaqueBlackBorder(resource), "a sampler table entry with an opaque black border needs dynamic texture BC-swizzle pairing, which is not implemented");
                Require(resource.addressModeU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER && resource.addressModeV != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER && resource.addressModeW != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, "a sampler table entry with a border needs dynamic texture numeric-class pairing, which is not implemented");
                Require(!resource.nonSeamlessCube || context.nonSeamlessCubeMap, "a sampler table entry that disables seamless cube filtering needs VK_EXT_non_seamless_cube_map because dynamic texture/sampler pairing is not implemented");
                Require(!SingleLevelSamplerWords(elementWords, true, false).has_value(), "a sampler table entry with anisotropic ANISO_OVERRIDE needs dynamic texture mip-level pairing, which is not implemented");
            } catch (const std::runtime_error& error) {
                rejected = error.what();
            }
        }
        if (!rejected.empty()) {
            table.samplerRejections.emplace_back(entry, rejectTableElement(table, words, rejected));
            item.imageAllocations.insert(item.imageAllocations.end(), Abi::SamplerEntryElements, std::numeric_limits<std::size_t>::max());
            continue;
        }
        for (std::uint32_t element = first; element < first + Abi::SamplerEntryElements; ++element) {
            const auto elementWords = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 4u, 4u);
            const bool compareEnable = binding.Flags().samplerDepthCompare.at(element);
            if (context.samplerCache != nullptr && !noSamplerCache) {
                samplers.push_back(context.samplerCache->Get(context, elementWords, compareEnable, false));
            } else {
                auto resource = DecodeSamplerResource(elementWords, false);
                resource.compareEnable = compareEnable;
                samplers.push_back(std::make_shared<Sampler>(context, resource));
            }
            samplerSources.push_back({{elementWords[0], elementWords[1], elementWords[2], elementWords[3]}, compareEnable, false});
            item.imageAllocations.push_back(samplers.size() - 1u);
        }
    }
    const auto setSamplers = context.samplerTableUpdateAfterBind ? context.descriptorIndexingLimits.maxDescriptorSetUpdateAfterBindSamplers : context.limits.maxDescriptorSetSamplers;
    Require(samplers.size() <= setSamplers, "pipeline sampler descriptors exceed device limits");
    bindings.push_back(std::move(item));
}

namespace {

void noteVariantSurface(std::uint64_t address, std::size_t members, const GuestTextureResource& chosen, const char* rule) {
    static std::mutex mutex;
    static std::unordered_set<std::uint64_t> seen;
    {
        std::lock_guard lock(mutex);
        if (seen.size() >= 4096u || !seen.insert(address).second) return;
    }
    TextureCounts().variantSurfaces.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[textures] surface 0x%llx has %zu dynamic-resolution variants in one image table; they share the %ux%u texture (chosen by %s)\n", static_cast<unsigned long long>(address), members, chosen.width, chosen.height, rule);
}

std::vector<std::uint32_t> variantRepresentatives(VkDevice device, const ShaderRecompiler::DescriptorBinding& binding, std::uint64_t elementBase, std::uint64_t capacity) {
    struct Member {
        std::uint32_t element;
        GuestTextureResource resource;
    };
    using FamilyKey = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;
    std::map<FamilyKey, std::vector<Member>> families;
    for (std::uint32_t element = 0; element < binding.count && elementBase + element < capacity; ++element) {
        if (element < binding.Flags().imageWritten.size() && binding.Flags().imageWritten[element]) continue;
        const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u);
        GuestTextureResource resource{};
        try {
            if (IsNullTextureResource(words)) continue;
            resource = DecodeTextureResource(words);
        } catch (const std::runtime_error&) {
            continue;
        }
        const FamilyKey key{resource.baseAddress, resource.pipeBankXor, resource.format, static_cast<std::uint32_t>(resource.tileMode), static_cast<std::uint32_t>(resource.dimension), resource.mipCount, resource.baseLevel, resource.lastLevel, resource.depthOrLastArray, resource.baseArray};
        families[key].push_back({element, resource});
    }
    std::vector<std::uint32_t> representatives;
    for (const auto& [key, members] : families) {
        const auto& first = members.front().resource;
        const auto otherExtent = [&](const Member& member) { return member.resource.width != first.width || member.resource.height != first.height; };
        if (std::none_of(members.begin(), members.end(), otherExtent)) continue;
        const auto withExtent = [&](std::uint32_t width, std::uint32_t height) {
            return std::find_if(members.begin(), members.end(), [&](const Member& member) { return member.resource.width == width && member.resource.height == height; });
        };
        auto chosen = members.end();
        const char* rule = "newest write";
        if (const auto record = StorageTexture::LastWrite(first.baseAddress); record && record->tileMode == first.tileMode && record->dimension == first.dimension && BytesPerElement(record->format) == BytesPerElement(first.format)) chosen = withExtent(record->width, record->height);
        if (chosen == members.end()) {
            rule = "active depth surface";
            if (const auto depth = ActiveDepthExtent(device, first.baseAddress)) chosen = withExtent(depth->width, depth->height);
        }
        if (chosen == members.end()) {
            rule = "largest accessible";
            std::uint64_t chosenBytes = 0;
            std::uint64_t chosenExtent = 0;
            for (auto member = members.begin(); member != members.end(); ++member) {
                try {
                    const auto bytes = DescribeSurface(member->resource).guestBytes;
                    const auto extent = static_cast<std::uint64_t>(member->resource.width) * member->resource.height;
                    if (bytes < chosenBytes || (bytes == chosenBytes && extent <= chosenExtent) || !GuestMemory::Accessible(reinterpret_cast<const void*>(member->resource.baseAddress), static_cast<std::size_t>(bytes))) continue;
                    chosen = member;
                    chosenBytes = bytes;
                    chosenExtent = extent;
                } catch (const std::runtime_error&) {
                }
            }
            if (chosen == members.end()) continue;
        }
        for (const auto& member : members) {
            if (member.resource.width == chosen->resource.width && member.resource.height == chosen->resource.height) continue;
            if (binding.imageShapes.at(member.element) != binding.imageShapes.at(chosen->element) || binding.Flags().imageDepthCompare.at(member.element) != binding.Flags().imageDepthCompare.at(chosen->element)) continue;
            if (representatives.empty()) representatives.assign(binding.count, NoRepresentative);
            representatives[member.element] = chosen->element;
        }
        noteVariantSurface(first.baseAddress, members.size(), chosen->resource, rule);
    }
    return representatives;
}

}

void ShaderResources::resolveImageTables() {
    const auto none = [](const std::array<std::size_t, 2>& set) { return set[0] == std::numeric_limits<std::size_t>::max() && set[1] == std::numeric_limits<std::size_t>::max(); };
    if (none(tableBindings) && none(storageTableBindings)) return;
    auto& counters = TextureCounts();
    for (std::size_t set = 0; set < tableBindings.size(); ++set) {
        if (storageTableBindings[set] != std::numeric_limits<std::size_t>::max()) bindings[storageTableBindings[set]].imageAllocations.assign(std::min(tableElements[set], StorageTableCapacity(context)), std::numeric_limits<std::size_t>::max());
        if (tableBindings[set] != std::numeric_limits<std::size_t>::max()) bindings[tableBindings[set]].imageAllocations.assign(std::min(tableElements[set], context.imageTableCapacity), std::numeric_limits<std::size_t>::max());
    }
    for (auto& table : tableStages) {
        if (table.images == nullptr) continue;
        auto* item = tableBindings[table.set] != std::numeric_limits<std::size_t>::max() ? &bindings[tableBindings[table.set]] : nullptr;
        const auto& binding = *table.images;
        const auto faults = table.map[ShaderRecompiler::ImageTableAbi::HeaderElementFaults];
        const auto stageSamplers = std::span<const std::shared_ptr<Sampler>>(samplers).subspan(table.firstSampler, table.samplerCount);
        table.elementTextures.assign(binding.count, std::numeric_limits<std::size_t>::max());
        const auto capacityMessage = [&] { return "the image table exceeds the device's capacity of " + std::to_string(context.imageTableCapacity) + " images"; };
        std::vector<std::uint32_t> samplerKey;
        for (std::uint32_t sampler = 0; sampler < table.samplerCount; ++sampler) {
            const auto& source = samplerSources.at(table.firstSampler + sampler);
            samplerKey.insert(samplerKey.end(), source.words.begin(), source.words.end());
            samplerKey.push_back((source.compareEnable ? 1u : 0u) | (source.unnormalized ? 2u : 0u) | (source.integerBorder ? 4u : 0u));
        }
        const auto place = [&](std::uint32_t element, std::shared_ptr<Texture> texture, const GuestTextureResource& resource, std::uint64_t guestBytes, bool firstLayer) {
            textures.push_back(std::move(texture));
            textureFirstLayer.push_back(firstLayer);
            describedRanges.push_back({"texture", resource.baseAddress, guestBytes, resource.width, resource.height, resource.format, static_cast<int>(resource.tileMode), resource.dccAddress, true});
            table.elementTextures[element] = textures.size() - 1u;
            item->imageAllocations[static_cast<std::size_t>(table.elementBase) + element] = textures.size() - 1u;
        };
        const auto refusal = [&](std::uint32_t element, std::span<const std::uint32_t> words, const std::shared_ptr<Texture>& texture, const GuestTextureResource& resource) -> std::string {
            try {
                RequireFilterMinmax(context, texture->ViewFormat(), binding.imageSamplers.at(element), stageSamplers);
                RequireBorderSwizzle(resource.bcSwizzle, binding.imageSamplers.at(element), stageSamplers);
                RequireNonSeamlessCube(resource.dimension == TextureDimension::kCube, binding.imageSamplers.at(element), stageSamplers);
                RequireDisablesDegammaOn(resource.format, binding.imageSamplers.at(element), stageSamplers);
                const auto numeric = ShaderRecompiler::DepthBitsTextureWidth(words[1], words[3]) != 0u ? ShaderRecompiler::IrTextureNumericClass::Float : ShaderRecompiler::SampledTextureNumericClass(static_cast<ShaderRecompiler::IrBufferFormat>(resource.format));
                const bool integerImage = numeric == ShaderRecompiler::IrTextureNumericClass::Uint || numeric == ShaderRecompiler::IrTextureNumericClass::Sint;
                for (auto mask = binding.imageSamplers.at(element); mask != 0u; mask &= mask - 1u) {
                    const auto sampler = static_cast<std::uint32_t>(std::countr_zero(mask));
                    if (sampler >= stageSamplers.size()) break;
                    const auto& source = samplerSources.at(table.firstSampler + sampler);
                    const auto descriptor = DecodeSamplerResource(source.words, source.unnormalized, true, source.integerBorder);
                    const bool border = descriptor.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || descriptor.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || descriptor.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                    Require(!border || source.integerBorder == integerImage, "an image table entry's sampler border color numeric class disagrees with its sampled image class, which needs dynamic pairing and is not implemented");
                    Require(!SingleLevelSamplerWords(source.words, true, false).has_value(), "an image table entry paired with anisotropic ANISO_OVERRIDE needs dynamic texture mip-level pairing, which is not implemented");
                }
            } catch (const std::runtime_error& error) {
                return error.what();
            }
            return {};
        };
        const auto bind = [&](std::uint32_t element, std::span<const std::uint32_t> words, std::shared_ptr<Texture> texture, const GuestTextureResource& resource, std::uint64_t guestBytes, bool firstLayer) -> std::string {
            auto message = refusal(element, words, texture, resource);
            if (!message.empty()) {
                table.map[faults + element] = rejectTableElement(table, words, message);
                return message;
            }
            place(element, std::move(texture), resource, guestBytes, firstLayer);
            return {};
        };
        struct DecodedEntry {
            GuestTextureResource resource{};
            std::uint64_t guestBytes = 0;
            bool firstLayer = false;
        };
        const auto decodeEntry = [&](std::uint32_t element, std::span<const std::uint32_t> words, const GuestTextureResource* predecoded = nullptr) {
            DecodedEntry decoded;
            decoded.resource = predecoded != nullptr ? *predecoded : DecodeTextureResource(words);
            const auto& resource = decoded.resource;
            const auto shape = binding.imageShapes.at(element);
            const bool depthCompare = element < binding.Flags().imageDepthCompare.size() && binding.Flags().imageDepthCompare[element];
            decoded.firstLayer = shape == ShaderRecompiler::DescriptorImageShape::Image2D && (resource.dimension == TextureDimension::k2DArray || (depthCompare && resource.dimension == TextureDimension::k1DArray));
            const bool depth1D = depthCompare && ((shape == ShaderRecompiler::DescriptorImageShape::Image2D && resource.dimension == TextureDimension::k1D) || (shape == ShaderRecompiler::DescriptorImageShape::Image2DArray && resource.dimension == TextureDimension::k1DArray));
            if (!decoded.firstLayer && !depth1D && !MatchesGuestDimension(shape, resource.dimension)) throw std::runtime_error("AGC graphics: guest texture dimension disagrees with the table entry's image shape (shape " + std::to_string(static_cast<int>(shape)) + ", dimension " + std::to_string(static_cast<int>(resource.dimension)) + ")");
            decoded.guestBytes = SurfaceGuestBytes(resource);
            if (!GuestMemory::Accessible(reinterpret_cast<const void*>(resource.baseAddress), static_cast<std::size_t>(decoded.guestBytes)) && GuestMemory::CommittedRanges(resource.baseAddress, static_cast<std::size_t>(decoded.guestBytes), false).empty()) throw std::runtime_error("AGC graphics: the table entry's surface at 0x" + HexText(resource.baseAddress) + " (0x" + HexText(decoded.guestBytes) + " bytes) is not mapped");
            static_cast<void>(ResolveTextureFormat(resource.format));
            return decoded;
        };
        struct SampledEntry {
            std::array<std::uint32_t, 8> words{};
            GuestTextureResource resource{};
            std::uint64_t guestBytes = 0;
            VkComponentMapping components{};
            bool depthCompare = false;
        };
        const auto prepareSampled = [&](std::uint32_t element, std::span<const std::uint32_t> words, const DecodedEntry& decoded) {
            SampledEntry sampled;
            sampled.resource = decoded.resource;
            sampled.guestBytes = decoded.guestBytes;
            std::array<std::uint32_t, 8> narrowed{};
            const auto sampledWords = firstLayerWords(words, binding.imageShapes.at(element), narrowed, true);
            std::copy(sampledWords.begin(), sampledWords.end(), sampled.words.begin());
            if (sampledWords.data() != words.data()) {
                sampled.resource = DecodeTextureResource(sampledWords);
                sampled.guestBytes = DescribeSurface(sampled.resource).guestBytes;
            }
            CompletePendingCpuWrites(context, sampled.resource.baseAddress, static_cast<std::size_t>(sampled.guestBytes));
            if (sampled.resource.dccAddress != 0) CompletePendingCpuWrites(context, sampled.resource.dccAddress, DccKeyBytes(sampled.guestBytes));
            sampled.components = ViewComponents(sampled.resource);
            sampled.depthCompare = binding.Flags().imageDepthCompare.at(element);
            return sampled;
        };
        const auto lookupSampled = [&](std::uint32_t element, const SampledEntry& sampled) {
            const VkImageViewType multisampleViewType = binding.imageShapes.at(element) == ShaderRecompiler::DescriptorImageShape::Image2DMultisampleArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
            Require(context.imageViewMinLod || EffectiveMinLod(sampled.resource) == 0.0f, "a sampled image table entry has a minimum LOD clamp, which needs VK_EXT_image_view_min_lod");
            return cachedTexture(context, sampled.words, sampled.resource, sampled.components, sampled.guestBytes, sampled.depthCompare, multisampleViewType);
        };
        const auto resolveLive = [&](std::uint32_t element, std::span<const std::uint32_t> words) -> std::string {
            DecodedEntry decoded;
            try {
                decoded = decodeEntry(element, words);
            } catch (const std::runtime_error& error) {
                table.map[faults + element] = rejectTableElement(table, words, error.what());
                return error.what();
            }
            const auto sampled = prepareSampled(element, words, decoded);
            std::shared_ptr<Texture> texture;
            try {
                texture = lookupSampled(element, sampled);
            } catch (const std::runtime_error& error) {
                table.map[faults + element] = rejectTableElement(table, words, error.what());
                return error.what();
            }
            return bind(element, sampled.words, std::move(texture), sampled.resource, sampled.guestBytes, decoded.firstLayer);
        };
        const auto state = CurrentTableState();
        std::vector<std::shared_ptr<Texture>> cached;
        const bool storageTable = std::find(binding.Flags().imageWritten.begin(), binding.Flags().imageWritten.end(), true) != binding.Flags().imageWritten.end();
        auto* reused = storageTable || item == nullptr ? nullptr : FindReusedTable(context.device, binding, state);
        if (reused != nullptr && AnyPendingCpuWrites(context)) {
            bool landed = false;
            for (const auto& entry : reused->elements) {
                if (!entry.rejected && CompletePendingCpuWrites(context, entry.resource.baseAddress, static_cast<std::size_t>(entry.guestBytes))) landed = true;
            }
            if (landed && CurrentTableState() != state) reused = nullptr;
        }
        if (reused != nullptr && TableCurrent(*reused) && CachedTableTextures(*reused, cached)) {
            const bool replay = TableReplay() && reused->replayable && reused->imageSamplers == binding.imageSamplers && reused->samplers == samplerKey;
            if (!replay) reused->bound.assign(binding.count, {});
            auto next = cached.begin();
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u);
                const auto& entry = reused->elements[element];
                if (static_cast<std::uint64_t>(table.elementBase) + element >= context.imageTableCapacity) {
                    if (!entry.rejected && !entry.live) ++next;
                    table.map[faults + element] = rejectTableElement(table, words, capacityMessage());
                } else if (entry.rejected) {
                    table.map[faults + element] = rejectTableElement(table, words, entry.message);
                } else if (entry.live) {
                    auto message = resolveLive(element, words);
                    if (!replay) reused->bound[element] = std::move(message);
                } else {
                    if (!replay) {
                        reused->bound[element] = bind(element, words, std::move(*next++), entry.resource, entry.guestBytes, entry.firstLayer);
                    } else if (!reused->bound[element].empty()) {
                        ++next;
                        table.map[faults + element] = rejectTableElement(table, words, reused->bound[element]);
                    } else {
                        place(element, std::move(*next++), entry.resource, entry.guestBytes, entry.firstLayer);
                    }
                }
            }
            if (!replay) {
                reused->imageSamplers = binding.imageSamplers;
                reused->samplers = samplerKey;
                reused->replayable = static_cast<std::uint64_t>(table.elementBase) + binding.count <= context.imageTableCapacity;
            }
            (replay ? counters.tablesReplayed : counters.tablesReused).fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        ReusedTable fresh;
        fresh.bound.assign(binding.count, {});
        bool reusable = state.epoch != 0;
        if (binding.count > 1) table.representatives = variantRepresentatives(context.device, binding, table.elementBase, context.imageTableCapacity);
        const auto& representatives = table.representatives;
        std::vector<bool> representativeNeeded(representatives.size(), false);
        for (const auto representative : representatives) {
            if (representative != NoRepresentative) representativeNeeded[representative] = true;
        }
        std::unordered_map<std::uint32_t, GuestTextureResource> representativeSources;
        std::vector<std::uint32_t> substitutes;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u);
            const auto absolute = static_cast<std::uint64_t>(table.elementBase) + element;
            if (absolute >= context.imageTableCapacity) {
                table.map[faults + element] = rejectTableElement(table, words, capacityMessage());
                reusable = false;
                continue;
            }
            if (!representatives.empty() && representatives[element] != NoRepresentative) {
                reusable = false;
                substitutes.push_back(element);
                continue;
            }
            std::string failure;
            const auto predecoded = TryDecodeTextureResource(words, &failure);
            if (!predecoded.has_value()) {
                table.map[faults + element] = rejectTableElement(table, words, failure);
                if (reusable) fresh.elements.push_back({{}, {}, {}, 0, false, true, failure});
                continue;
            }
            DecodedEntry decoded;
            try {
                decoded = decodeEntry(element, words, &*predecoded);
            } catch (const std::runtime_error& error) {
                table.map[faults + element] = rejectTableElement(table, words, error.what());
                if (reusable) fresh.elements.push_back({{}, {}, {}, 0, false, true, error.what()});
                continue;
            }
            const auto& resource = decoded.resource;
            const auto guestBytes = decoded.guestBytes;
            const bool firstLayer = decoded.firstLayer;
            if (element < binding.Flags().imageWritten.size() && binding.Flags().imageWritten[element]) {
                reusable = false;
                const auto storageBinding = storageTableBindings[table.set];
                if (storageBinding == std::numeric_limits<std::size_t>::max()) {
                    table.map[faults + element] = rejectTableElement(table, words, "set " + std::to_string(table.set) + " has no image storage table, but the shader stores to its table element " + std::to_string(absolute));
                    continue;
                }
                if (absolute >= StorageTableCapacity(context)) {
                    table.map[faults + element] = rejectTableElement(table, words, "the stored table element " + std::to_string(absolute) + " of set " + std::to_string(table.set) + " is past the image storage table's capacity of " + std::to_string(StorageTableCapacity(context)) + " images");
                    continue;
                }
                CompletePendingCpuWrites(context, resource.baseAddress, static_cast<std::size_t>(guestBytes));
                if (resource.dccAddress != 0) CompletePendingCpuWrites(context, resource.dccAddress, DccKeyBytes(guestBytes));
                const auto mip = std::min(resource.baseLevel, resource.mipCount - 1u);
                if (resource.minLod > mip * 256u) {
                    table.map[faults + element] = rejectTableElement(table, words, "the table entry's storage T# clamps its minimum LOD above the level it addresses, which is not implemented");
                    continue;
                }
                try {
                    storageTextures.push_back(cachedStorageTexture(context, words, resource, mip, guestBytes, true));
                } catch (const std::runtime_error& error) {
                    table.map[faults + element] = rejectTableElement(table, words, error.what());
                    continue;
                }
                storageMips.push_back(mip);
                storageKeys.push_back(resource.dccAddress);
                storageFirstLayer.push_back(firstLayer);
                storageWritten.push_back(true);
                storageAtomic.push_back(element < binding.Flags().imageAtomic.size() && binding.Flags().imageAtomic[element]);
                storageAtomic64.push_back(element < binding.Flags().imageAtomic64.size() && binding.Flags().imageAtomic64[element]);
                describedRanges.push_back({"storage", resource.baseAddress, guestBytes, resource.width, resource.height, resource.format, static_cast<int>(resource.tileMode), resource.dccAddress, true});
                bindings[storageBinding].imageAllocations[absolute] = storageTextures.size() - 1u;
                continue;
            }
            if (item == nullptr) {
                table.map[faults + element] = rejectTableElement(table, words, "a sampled image table entry has no sampled image table binding");
                reusable = false;
                continue;
            }
            const auto sampled = prepareSampled(element, words, decoded);
            std::shared_ptr<Texture> texture;
            try {
                texture = lookupSampled(element, sampled);
            } catch (const std::runtime_error& error) {
                table.map[faults + element] = rejectTableElement(table, words, error.what());
                if (reusable) fresh.elements.push_back({{}, {}, {}, 0, false, true, error.what()});
                continue;
            }
            if (reusable) {
                const auto& log = lookupLog();
                const auto* looked = log.empty() ? nullptr : &log.back();
                if (looked != nullptr && looked->object == texture.get() && looked->source == nullptr && looked->generation != 0 && looked->keys == DccKeys::Uncompressed && sampled.resource.dccAddress == 0) {
                    fresh.elements.push_back({MakeTextureKey(context.device, sampled.words, sampled.components, sampled.depthCompare), texture, sampled.resource, sampled.guestBytes, firstLayer, false, {}});
                    fresh.queries.push_back({sampled.resource.baseAddress, static_cast<std::size_t>(sampled.guestBytes), looked->generation});
                    fresh.ranges.emplace_back(sampled.resource.baseAddress, sampled.resource.baseAddress + sampled.guestBytes);
                } else {
                    fresh.elements.push_back({{}, {}, {}, 0, firstLayer, false, {}, true});
                }
            }
            if (!representativeNeeded.empty() && representativeNeeded[element]) representativeSources.emplace(element, sampled.resource);
            fresh.bound[element] = bind(element, sampled.words, std::move(texture), sampled.resource, sampled.guestBytes, firstLayer);
        }
        for (const auto element : substitutes) {
            const auto representative = representatives[element];
            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(representative) * 8u, 8u);
            const auto index = table.elementTextures[representative];
            const auto source = representativeSources.find(representative);
            const auto message = index == std::numeric_limits<std::size_t>::max() || source == representativeSources.end() ? std::string("the dynamic-resolution variant this entry borrows from was rejected") : refusal(element, words, textures[index], source->second);
            if (!message.empty()) {
                table.map[faults + element] = rejectTableElement(table, words, message);
                counters.variantRejected.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            table.elementTextures[element] = index;
            item->imageAllocations[static_cast<std::size_t>(table.elementBase) + element] = index;
            counters.variantSubstituted.fetch_add(1, std::memory_order_relaxed);
        }
        counters.tablesResolved.fetch_add(1, std::memory_order_relaxed);
        const bool anyKept = std::any_of(fresh.elements.begin(), fresh.elements.end(), [](const ReusedTableElement& element) { return !element.live; });
        if (reusable && anyKept && CurrentTableState() == state) {
            const auto serial = StorageTexture::PendingSerial();
            if (!StorageTexture::AnyPendingOverlaps(fresh.ranges) && StorageTexture::PendingSerial() == serial) {
                fresh.device = context.device;
                fresh.state = state;
                fresh.pendingSerial = serial;
                fresh.imageSamplers = binding.imageSamplers;
                fresh.samplers = std::move(samplerKey);
                fresh.replayable = static_cast<std::uint64_t>(table.elementBase) + binding.count <= context.imageTableCapacity;
                RememberTable(std::move(fresh), binding);
            }
        }
    }
    reportTextureCounters();
}

std::uint64_t SampledLookupKeyScans() {
    return sampledLookupKeyScansThreadState();
}

std::uint64_t SampledLookupKeyProofs() {
    return sampledLookupKeyProofsThreadState();
}

std::uint64_t SampledTexturesCleared() {
    return TextureCounts().cleared.load(std::memory_order_relaxed);
}

ImageTableReuseCounts ImageTableReuses() {
    const auto& counters = TextureCounts();
    return {counters.tablesReused.load(std::memory_order_relaxed) + counters.tablesReplayed.load(std::memory_order_relaxed), counters.tablesResolved.load(std::memory_order_relaxed), counters.tablesReplayed.load(std::memory_order_relaxed), counters.tableWritesKept.load(std::memory_order_relaxed), counters.tableWritesMade.load(std::memory_order_relaxed), counters.variantSurfaces.load(std::memory_order_relaxed), counters.variantSubstituted.load(std::memory_order_relaxed), counters.variantRejected.load(std::memory_order_relaxed)};
}

void ShaderResources::finishImageTables() {
    namespace Abi = ShaderRecompiler::ImageTableAbi;
    if (tableStages.empty()) return;
    const auto overlaps = [](std::uint64_t base, std::uint64_t size, std::uint64_t address, std::uint64_t bytes) { return size != 0 && bytes != 0 && (base <= address ? address - base < size : base - address < bytes); };
    std::vector<BdaResources::ImageTableFaults> faults;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (auto& table : tableStages) {
        auto& map = table.map;
        for (const auto& [entry, code] : table.samplerRejections) map[map[Abi::HeaderSamplerFaults] + entry] = code;
        for (std::uint32_t index = 0; index < map[Abi::HeaderTableCount]; ++index) {
            const auto header = Abi::TableHeader(index);
            if (map[header + Abi::TableFault] != 0u) continue;
            const auto base = static_cast<std::uint64_t>(map[header + Abi::TableBaseLow]) | (static_cast<std::uint64_t>(map[header + Abi::TableBaseHigh]) << 32u);
            const auto size = static_cast<std::uint64_t>(map[header + Abi::TableSizeLow]) | (static_cast<std::uint64_t>(map[header + Abi::TableSizeHigh]) << 32u);
            const auto tableOverlaps = [&](std::uint64_t address, std::uint64_t bytes) {
                if (overlaps(base, size, address, bytes)) return true;
                if (index >= table.program->imageTableReadRanges.size()) return false;
                const auto& reads = table.program->imageTableReadRanges[index];
                return std::any_of(reads.begin(), reads.end(), [&](const auto& range) { return overlaps(range.first, range.second, address, bytes); });
            };
            bool written = false;
            for (const auto& allocation : allocations) written = written || (allocation.guest && allocation.written && tableOverlaps(allocation.address, allocation.size));
            for (std::size_t image = 0; image < storageTextures.size(); ++image) written = written || (storageWritten[image] && storageTextures[image] != nullptr && tableOverlaps(storageTextures[image]->Descriptor().baseAddress, storageTextures[image]->GuestBytes()));
            if (!written) continue;
            ShaderRecompiler::ImageTableEntryPoison poison{};
            poison.resource = index;
            poison.reason = static_cast<std::uint32_t>(Abi::PoisonReason::HeapWritten);
            table.poison.push_back(poison);
            table.messages.push_back(std::make_shared<const std::string>("the table at 0x" + HexText(base) + " is written by its own dispatch"));
            map[header + Abi::TableFault] = Abi::PoisonCode(static_cast<std::uint32_t>(table.poison.size() - 1u));
            faultChecks = true;
        }
        map[Abi::HeaderPoisonCount] = static_cast<std::uint32_t>(table.poison.size());
        auto& allocation = allocations.at(table.mapAllocation);
        Require(allocation.buffer != nullptr && allocation.size == map.size() * sizeof(std::uint32_t), "image table map buffer disagrees with the map");
        std::memcpy(allocation.buffer->Bytes().data(), map.data(), allocation.size);
        faults.push_back({IrStage(table.stage), table.program->imageTableShader, table.poison, table.messages});
        ranges.insert(ranges.end(), table.program->imageTableRanges.begin(), table.program->imageTableRanges.end());
    }
    if (bda == nullptr) return;
    bda->SetImageTables(std::move(faults), std::move(ranges));
}

std::vector<PresyncSurface> ShaderResources::PresyncSurfaces() const {
    std::vector<PresyncSurface> surfaces;
    // A surface's lookup reads guest memory on the CPU unless it is served GPU-direct from a host
    // import: a storage image of imported memory uploads and refreshes from the import, and a
    // sampled texture of one views that image (SampledFromStorageEligible), so neither waits for
    // the producer.
    const auto consider = [&](std::uint32_t format, std::uint64_t address, std::uint64_t guestBytes, bool sampled) {
        const bool gpuDirect = sampled ? SampledFromStorageEligible(context, format, address, guestBytes) : HostImportCovers(context, address, static_cast<std::size_t>(guestBytes));
        if (!gpuDirect) surfaces.push_back({address, guestBytes, false});
    };
    static const bool keyRanges = std::getenv("APS5_NO_DCC_PRESYNC") == nullptr;
    const auto considerKeys = [&](std::uint64_t dccAddress, std::uint64_t guestBytes) {
        const auto keyBytes = DccKeyBytes(guestBytes);
        if (keyRanges && dccAddress != 0 && keyBytes != 0) surfaces.push_back({dccAddress, keyBytes, true});
    };
    if (completed) {
        // A cached object (the resource cache): Revalidate repeats the lookups of the objects the
        // build made, which never change afterwards (a lookup returning another object fails the
        // revalidation instead), so the surfaces come from the build's own record of them. A
        // sampled texture viewing a storage image refreshes like that image; a snapshot texture
        // over an eligible surface is replaced by a view without reading. Storage images are
        // checked on their own surface (a mip chain can exceed the element's descriptor). Read
        // without the device lock: these members are fixed once the build completed.
        std::size_t textureIndex = 0;
        for (const auto& range : describedRanges) {
            if (std::strcmp(range.kind, "texture") == 0 && textureIndex < textures.size()) {
                const auto& texture = textures[textureIndex++];
                if (texture->ViewsStorageImage()) consider(range.format, range.address, range.bytes, false);
                else consider(range.format, range.address, range.bytes, true);
                considerKeys(range.dccAddress, range.bytes);
            }
        }
        for (std::size_t index = 0; index < storageTextures.size(); ++index) {
            if (storageTextures[index] == nullptr || (index != 0 && storageTextures[index] == storageTextures[index - 1])) continue;
            const auto& image = *storageTextures[index];
            consider(image.Descriptor().format, image.Descriptor().baseAddress, image.GuestBytes(), false);
            considerKeys(image.Descriptor().dccAddress, image.GuestBytes());
        }
        return surfaces;
    }
    if (!imageRecords.empty()) {
        for (const auto& record : imageRecords) {
            if (!record.decoded) continue;
            consider(record.resource.format, record.resource.baseAddress, record.guestBytes, record.sampled);
            considerKeys(record.resource.dccAddress, record.guestBytes);
        }
        return surfaces;
    }
    // Stage A has not run (an address-based build, or the pass is off): decoded from the shader.
    if (deferredCompute.program == nullptr) return surfaces;
    forEachImageElement(*deferredCompute.program, [&](const ShaderRecompiler::DescriptorBinding& binding, std::uint32_t, std::span<const std::uint32_t> words) {
        if (IsNullTextureResource(words)) return;
        const auto resource = DecodeTextureResource(words);
        const auto guestBytes = SurfaceGuestBytes(resource);
        consider(resource.format, resource.baseAddress, guestBytes, binding.kind == ShaderRecompiler::DescriptorKind::SampledImage);
        considerKeys(resource.dccAddress, guestBytes);
    });
    for (const auto& binding : deferredCompute.program->bindings) {
        if (binding.role != ShaderRecompiler::DescriptorRole::ImageTable) continue;
        for (std::size_t element = 0; element < binding.guestDescriptor.size() / 8u; ++element) {
            const auto resource = TryDecodeTextureResource(std::span<const std::uint32_t>(binding.guestDescriptor).subspan(element * 8u, 8u));
            if (!resource.has_value()) continue;
            try {
                const auto guestBytes = SurfaceGuestBytes(*resource);
                consider(resource->format, resource->baseAddress, guestBytes, true);
                considerKeys(resource->dccAddress, guestBytes);
            } catch (const std::runtime_error&) {
            }
        }
    }
    return surfaces;
}

ShaderResources::~ShaderResources() {
    if (archcToken.registered) ArchC::ImageEpochs::Get().UnregisterOwner(this);
    if (pendingScanToken.registered) ArchC::ImageEpochs::Get().UnregisterOwner(&pendingScanEpoch);
    if (stampScanToken.registered || stampScanEpoch.load(std::memory_order_relaxed) != 1) ArchC::ImageEpochs::Get().UnregisterOwner(&stampScanEpoch);
    release();
}

std::shared_ptr<const TableDescriptors> ShaderResources::keptTable(std::size_t set, std::shared_ptr<const std::vector<std::weak_ptr<Texture>>>& held) noexcept {
    const auto& table = tableWrites[set];
    if (!table.written || KeptSets() == 0u || table.firstImage + table.images > descriptorImages.size()) return nullptr;
    try {
        if (held == nullptr) {
            std::vector<std::weak_ptr<Texture>> weak;
            weak.reserve(textures.size());
            for (const auto& texture : textures) {
                if (texture != nullptr) weak.push_back(texture);
            }
            held = std::make_shared<const std::vector<std::weak_ptr<Texture>>>(std::move(weak));
        }
        auto kept = std::make_shared<TableDescriptors>();
        kept->binding = table.binding;
        kept->runs = table.runs;
        kept->images.assign(descriptorImages.begin() + static_cast<std::ptrdiff_t>(table.firstImage), descriptorImages.begin() + static_cast<std::ptrdiff_t>(table.firstImage + table.images));
        kept->textures = held;
        return kept;
    } catch (...) {
        return nullptr;
    }
}

void ShaderResources::release() noexcept {
    // A set from the cache's pool chain goes back to it; a dedicated pool dies with its set.
    std::shared_ptr<const std::vector<std::weak_ptr<Texture>>> held;
    if (cacheAllocation.pool && context.descriptorCache != nullptr) context.descriptorCache->Free(cacheAllocation, keptTable(0, held));
    if (pool) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
    if (_layout && ownsLayout) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, _layout, nullptr);
    if (fragment.cacheAllocation.pool && context.descriptorCache != nullptr) context.descriptorCache->Free(fragment.cacheAllocation, keptTable(1, held));
    if (fragment.pool) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, fragment.pool, nullptr);
    if (fragment.layout && fragment.ownsLayout) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, fragment.layout, nullptr);
    cacheAllocation = {};
    pool = VK_NULL_HANDLE;
    _set = VK_NULL_HANDLE;
    _layout = VK_NULL_HANDLE;
    fragment = {};
    tableWrites = {};
}

std::uint32_t ShaderResources::setOf(const Binding& binding) const {
    constexpr auto count = static_cast<std::uint32_t>(ShaderRecompiler::RuntimeAbi::Binding::Count);
    if (binding.set >= 0) return static_cast<std::uint32_t>(binding.set);
    return splitSets && binding.layout.binding / count == static_cast<std::uint32_t>(ShaderRecompiler::RuntimeAbi::Stage::Fragment) ? 1u : 0u;
}

VkDescriptorSetLayout ShaderResources::Layout() const {
    return _layout;
}

std::vector<VkDescriptorSetLayoutBinding> ShaderResources::LayoutBindings() const {
    std::vector<VkDescriptorSetLayoutBinding> result;
    for (const auto& binding : bindings) result.push_back(binding.layout);
    return result;
}

bool ShaderResources::LayoutUpdateAfterBind() const {
    return updateAfterBind;
}

namespace {
struct DrawSetPoolCounters {
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
    std::atomic<std::uint64_t> fullElements{0};
    std::atomic<std::uint64_t> writtenElements{0};
    // APS5_PROFILE_DRAW or APS5_PIPELINE_STATS only: the descriptor part of PrepareDrawBindings (set, writes, keeps), with
    // the switch on or off.
    // Indexed by whether the call used the pool (the switch as sampled by that call).
    std::array<std::atomic<std::uint64_t>, 2> setCalls{};
    std::array<std::atomic<std::uint64_t>, 2> setNs{};
};
DrawSetPoolCounters& drawSetPoolCounters() {
    static auto* counters = [] {
        auto* made = new DrawSetPoolCounters();
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_draw_set_pool_hits", made->hits);
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_draw_set_pool_misses", made->misses);
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_draw_set_pool_full_elements", made->fullElements);
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_draw_set_pool_written_elements", made->writtenElements);
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_draw_set_pool_set_calls", made->setCalls[1]);
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_draw_set_pool_set_ns", made->setNs[1]);
        return made;
    }();
    return *counters;
}
}

namespace {
// APS5_ARCHB_PIPE_DRAW_REUSE (Stage 3 B1) counters.
struct DrawReuseCounters {
    std::atomic<std::uint64_t> draws{0};          // committer draws with snapshots under the switch
    std::atomic<std::uint64_t> pooled{0};         // of them, on a set back from the pool
    std::atomic<std::uint64_t> noWrite{0};        // of those, no descriptor written at all
    std::atomic<std::uint64_t> slotHits{0};       // a pooled set whose slots matched the small snapshots
    std::atomic<std::uint64_t> slotRewrites{0};   // slots laid out anew (a set without matching slots)
    std::atomic<std::uint64_t> slotBytes{0};      // snapshot bytes copied into slots
    std::atomic<std::uint64_t> sharedElements{0}; // selected snapshots above the upload limit or moved word buffers (shared path)
    std::atomic<std::uint64_t> skippedElements{0};// pooled-set elements whose held descriptor already matched (not written)
    std::atomic<std::uint64_t> writtenElements{0};// pooled-set elements written
    std::atomic<std::uint64_t> verified{0};
    std::atomic<std::uint64_t> mismatches{0};     // _VERIFY: a pooled set's previous batch had not completed, or a skipped element's buffer differs
};
DrawReuseCounters& drawReuseCounters() {
    static auto* counters = [] {
        auto* made = new DrawReuseCounters();
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_reuse_draws", made->draws);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_reuse_pooled", made->pooled);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_reuse_no_write", made->noWrite);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_reuse_slot_hits", made->slotHits);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_reuse_slot_rewrites", made->slotRewrites);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_reuse_slot_bytes", made->slotBytes);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_reuse_shared_elements", made->sharedElements);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_reuse_skipped_elements", made->skippedElements);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_reuse_written_elements", made->writtenElements);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_reuse_verified", made->verified);
        AgcDriver::ArchB::Recorder::RegisterCounter("pipe_draw_reuse_mismatches", made->mismatches);
        return made;
    }();
    return *counters;
}

// APS5_DRAW_BINDINGS_DIAG: PrepareDrawBindings by snapshot path (see DrawBindingsDiagText).
struct BindingsDiag {
    std::atomic<std::uint64_t> calls{0}, empty{0}, elements{0}, totalNs{0}, selectNs{0};
    std::atomic<std::uint64_t> movedWords{0}, slotted{0}, slottedBytes{0};
    std::atomic<std::uint64_t> unwrittenSmall{0}, unwrittenLarge{0}, unwrittenLargeBytes{0};
    std::atomic<std::uint64_t> uploads{0}, uploadBytes{0}, uploadNs{0};
    std::atomic<std::uint64_t> large{0}, largeBytes{0}, collectNs{0}, reusableNs{0}, reused{0}, created{0}, createdBytes{0}, createNs{0};
    // Attribution of the selection's remaining time: InPlaceReads, the per-element checks (override
    // lookup, guest/written/WritesOverlap/in-place/pending), moved word buffers, the unwritten fast path
    // (stamp, lookup, note after the snapshot).
    std::atomic<std::uint64_t> readsNs{0}, checkNs{0}, checkElements{0}, checkPassed{0}, movedNs{0}, unwrittenNs{0}, noteNs{0};
};
BindingsDiag& bindingsDiag() {
    static auto* diag = new BindingsDiag();
    return *diag;
}
std::uint64_t diagNs(std::chrono::steady_clock::time_point from) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - from).count());
}

bool drawReuseVerify() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_PIPE_DRAW_REUSE_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
}

std::string ShaderResources::DrawReuseText() {
    auto& counters = drawReuseCounters();
    const auto get = [](const std::atomic<std::uint64_t>& value) { return static_cast<unsigned long long>(value.load(std::memory_order_relaxed)); };
    char text[512];
    std::snprintf(text, sizeof(text), "[draw-reuse] cumulative: %llu draws, %llu on pooled sets, %llu with no descriptor write; slots: %llu hits, %llu layouts written, %.1f MiB copied; %llu shared snapshots; pooled-set elements skipped %llu, written %llu; verified %llu, mismatches %llu",
        get(counters.draws), get(counters.pooled), get(counters.noWrite), get(counters.slotHits), get(counters.slotRewrites), static_cast<double>(get(counters.slotBytes)) / 1048576.0,
        get(counters.sharedElements), get(counters.skippedElements), get(counters.writtenElements), get(counters.verified), get(counters.mismatches));
    return text;
}

namespace {
// APS5_ARCHB_FIX_SNAPSHOT_CANDIDATES (see ShaderResources::snapshotCandidates).
struct SnapshotCandidateCounters {
    std::atomic<std::uint64_t> draws{0}, fallbacks{0}, skipped{0}, verified{0}, mismatches{0};
    std::atomic<std::uint64_t> builds{0}, buildNs{0}, buildPairs{0};
};

SnapshotCandidateCounters& snapshotCandidateCounters() {
    static auto* counters = [] {
        auto* made = new SnapshotCandidateCounters();
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_snapshot_candidates_draws", made->draws);
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_snapshot_candidates_fallbacks", made->fallbacks);
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_snapshot_candidates_skipped", made->skipped);
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_snapshot_candidates_verified", made->verified);
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_snapshot_candidates_mismatches", made->mismatches);
        return made;
    }();
    return *counters;
}

bool snapshotCandidatesVerify() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_FIX_SNAPSHOT_CANDIDATES_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
}

ShaderResources::SnapshotCandidateStats ShaderResources::SnapshotCandidateTotals() {
    auto& counters = snapshotCandidateCounters();
    const auto get = [](const std::atomic<std::uint64_t>& value) { return value.load(std::memory_order_relaxed); };
    return {get(counters.draws), get(counters.fallbacks), get(counters.skipped), get(counters.verified), get(counters.mismatches)};
}

std::string ShaderResources::SnapshotFixesText() {
    const auto pages = Recorder::PageRefreshTotals();
    const auto candidates = SnapshotCandidateTotals();
    auto& counters = snapshotCandidateCounters();
    const auto builds = counters.builds.load(std::memory_order_relaxed);
    const auto movedWords = MovedWordTotals();
    if (pages.refreshes + pages.fallbacks + candidates.draws + candidates.fallbacks + builds + movedWords.uploaded + movedWords.verified == 0) return {};
    const auto u = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    char text[512];
    std::snprintf(text, sizeof(text), "[snapshot-fixes] cumulative: page refresh %llu refreshes, %llu fallbacks, %.1f MiB compared (blocks %.1f MiB, verify only), %.1f MiB copied, %llu pages verified, page mismatches %llu; candidates %llu draws, %llu fallbacks, %llu elements skipped, %llu verified, candidate mismatches %llu",
        u(pages.refreshes), u(pages.fallbacks), static_cast<double>(pages.comparedBytes) / 1048576.0, static_cast<double>(pages.blockBytes) / 1048576.0, static_cast<double>(pages.copiedBytes) / 1048576.0, u(pages.verifiedPages), u(pages.mismatches),
        u(candidates.draws), u(candidates.fallbacks), u(candidates.skipped), u(candidates.verified), u(candidates.mismatches));
    char more[320];
    std::snprintf(more, sizeof(more), "; candidate lists built %llu in %.1f ms (%llu element x read pairs); moved words %llu held, %llu uploaded, %llu created, %llu verified, moved mismatches %llu", u(builds), static_cast<double>(counters.buildNs.load(std::memory_order_relaxed)) / 1e6, u(counters.buildPairs.load(std::memory_order_relaxed)),
        u(movedWords.held), u(movedWords.uploaded), u(movedWords.created), u(movedWords.verified), u(movedWords.mismatches));
    return std::string(text) + more;
}

namespace {
// APS5_ARCHB_FIX_MOVED_WORD_UPLOAD: upstream puts a moved data buffer's words into a draw upload slice
// (bump allocation in the recorder's upload chunk, never written again once filled). The switch keeps
// its counters and verification. A live record (snapfix-count-1103-0918) measured the moved word path at 1.27 us per
// PrepareDrawBindings call (1.12 moved buffers each, ~0.9 ms/f on the committer). The 8 held word
// buffers keep (buffer, offset) for an identical later draw, as before. _VERIFY checks a held slice still
// holds its words before reusing it.
struct MovedWordCounters {
    std::atomic<std::uint64_t> held{0}, uploaded{0}, created{0}, verified{0}, mismatches{0};
};

MovedWordCounters& movedWordCounters() {
    static auto* counters = [] {
        auto* made = new MovedWordCounters();
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_moved_word_upload_held", made->held);
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_moved_word_upload_uploaded", made->uploaded);
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_moved_word_upload_created", made->created);
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_moved_word_upload_verified", made->verified);
        AgcDriver::ArchB::Recorder::RegisterCounter("fix_moved_word_upload_mismatches", made->mismatches);
        return made;
    }();
    return *counters;
}

bool movedWordUploadVerify() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_FIX_MOVED_WORD_UPLOAD_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
}

ShaderResources::MovedWordStats ShaderResources::MovedWordTotals() {
    auto& counters = movedWordCounters();
    const auto get = [](const std::atomic<std::uint64_t>& value) { return value.load(std::memory_order_relaxed); };
    return {get(counters.held), get(counters.uploaded), get(counters.created), get(counters.verified), get(counters.mismatches)};
}

bool ShaderResources::staticSnapshotCheck(std::size_t index, std::span<const std::pair<std::uint64_t, std::uint64_t>> reads) const {
    const auto& item = allocations[index];
    if (!item.guest || item.written || guestMemory.WritesOverlap(item.address, item.size)) return false;
    return std::any_of(reads.begin(), reads.end(), [&](const auto& range) { return item.address >= range.first && item.address < range.second && item.size <= range.second - item.address; });
}

void ShaderResources::computeSnapshotCandidates() {
    const auto start = std::chrono::steady_clock::now();
    struct Timed {
        std::chrono::steady_clock::time_point start;
        ~Timed() {
            auto& counters = snapshotCandidateCounters();
            counters.builds.fetch_add(1, std::memory_order_relaxed);
            counters.buildNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
        }
    } timed{start};
    snapshotCandidates.clear();
    snapshotCandidatesKnown = false;
    if ((_set == VK_NULL_HANDLE && fragment.set == VK_NULL_HANDLE) || usesBda || !guestMemory.InPlaceReadsSettled()) return;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
    guestMemory.InPlaceReads(reads);
    snapshotCandidateCounters().buildPairs.fetch_add(reads.size() * allocations.size(), std::memory_order_relaxed);
    snapshotCandidates.resize(allocations.size());
    for (std::size_t index = 0; index < allocations.size(); ++index) snapshotCandidates[index] = staticSnapshotCheck(index, reads) ? 1 : 0;
    snapshotCandidatesKnown = true;
}

std::string ShaderResources::DrawBindingsDiagText() {
    if (!Recorder::SnapshotDiagEnabled()) return {};
    auto& d = bindingsDiag();
    auto& r = Recorder::SnapshotDiag();
    const auto get = [](const std::atomic<std::uint64_t>& value) { return static_cast<unsigned long long>(value.load(std::memory_order_relaxed)); };
    char text[1024];
    std::snprintf(text, sizeof(text), "[draw-bindings] cumulative: calls %llu empty %llu elements %llu total_ns %llu select_ns %llu | moved_words %llu slotted %llu slotted_bytes %llu | unwritten_small %llu unwritten_large %llu unwritten_large_bytes %llu | uploads %llu upload_bytes %llu upload_ns %llu | large %llu large_bytes %llu collect_ns %llu reusable_ns %llu reused %llu created %llu created_bytes %llu create_ns %llu | storage exact %llu refreshed %llu refresh_failed %llu containing %llu missing %llu compared_bytes %llu copied_bytes %llu compare_ns %llu",
        get(d.calls), get(d.empty), get(d.elements), get(d.totalNs), get(d.selectNs), get(d.movedWords), get(d.slotted), get(d.slottedBytes), get(d.unwrittenSmall), get(d.unwrittenLarge), get(d.unwrittenLargeBytes),
        get(d.uploads), get(d.uploadBytes), get(d.uploadNs), get(d.large), get(d.largeBytes), get(d.collectNs), get(d.reusableNs), get(d.reused), get(d.created), get(d.createdBytes), get(d.createNs),
        get(r.exact), get(r.refreshed), get(r.refreshFailed), get(r.containing), get(r.missing), get(r.comparedBytes), get(r.copiedBytes), get(r.compareNs));
    std::string line = text;
    char more[256];
    std::snprintf(more, sizeof(more), " | reads_ns %llu check_ns %llu check_elements %llu check_passed %llu moved_ns %llu unwritten_ns %llu note_ns %llu",
        get(d.readsNs), get(d.checkNs), get(d.checkElements), get(d.checkPassed), get(d.movedNs), get(d.unwrittenNs), get(d.noteNs));
    return line + more;
}

std::string ShaderResources::DrawSetPoolText() {
    auto& counters = drawSetPoolCounters();
    char text[384];
    const auto perCall = [&](std::size_t mode) {
        const auto calls = counters.setCalls[mode].load(std::memory_order_relaxed);
        return calls == 0 ? 0.0 : static_cast<double>(counters.setNs[mode].load(std::memory_order_relaxed)) / 1e3 / static_cast<double>(calls);
    };
    std::snprintf(text, sizeof(text), "[draw-set-pool] cumulative: %llu pooled sets reused, %llu sets written in full; elements written %llu (a full rewrite of the reused sets: %llu); set work: switch off %llu calls %.2f us each, on %llu calls %.2f us each",
        static_cast<unsigned long long>(counters.hits.load(std::memory_order_relaxed)), static_cast<unsigned long long>(counters.misses.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(counters.writtenElements.load(std::memory_order_relaxed)), static_cast<unsigned long long>(counters.fullElements.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(counters.setCalls[0].load(std::memory_order_relaxed)), perCall(0), static_cast<unsigned long long>(counters.setCalls[1].load(std::memory_order_relaxed)), perCall(1));
    return text;
}

ShaderResources::DrawBindings::~DrawBindings() {
    if (cache == nullptr) return;
    if (pool != nullptr && pool->Give(DrawSetPool::Entry{allocation, fragmentAllocation, std::move(overridden), std::move(held), std::move(slotBuffer), std::move(slotElements), std::move(slotOffsets), std::move(slotSizes), serial})) return;
    const auto release = [&](VkDescriptorSetLayout setLayout, const DescriptorCache::SetAllocation& set) {
        if (set.set == VK_NULL_HANDLE) return;
        if (setLayout != VK_NULL_HANDLE) cache->Recycle(setLayout, set);
        else cache->Free(set);
    };
    release(layout, allocation);
    release(fragmentLayout, fragmentAllocation);
}

std::optional<std::vector<ShaderResources::MovedBuffer>> ShaderResources::MovedReadOnlyBuffers(std::span<const CompiledShader> shaders, Recorder& recorder, const ColorTarget* target, bool* patchedData) const {
    return MovedReadOnlyBuffers(shaders, recorder, target, patchedData, nullptr);
}

std::optional<std::vector<ShaderResources::MovedBuffer>> ShaderResources::MovedReadOnlyBuffers(std::span<const CompiledShader> shaders, Recorder& recorder, const ColorTarget* target, bool* patchedData, MoveRefusal* refusal) const {
    std::vector<MovedBuffer> moved;
    std::size_t movedBytes = 0;
    const auto refuse = [&](MoveRefusal why) {
        if (refusal != nullptr) *refusal = why;
        return std::optional<std::vector<MovedBuffer>>{};
    };
    if ((_set == VK_NULL_HANDLE && fragment.set == VK_NULL_HANDLE) || (usesBda && !spaceReuse)) return moved;
    for (const auto& shader : shaders) {
        if (shader.program == nullptr) return refuse(MoveRefusal::Shape);
        for (const auto& binding : shader.program->bindings) {
            if (DataRole(binding.role)) {
                const auto kept = std::find_if(bindings.begin(), bindings.end(), [&](const Binding& item) { return item.layout.binding == binding.binding; });
                if (kept == bindings.end() || kept->allocations.size() != 1) return refuse(MoveRefusal::Shape);
                const auto index = kept->allocations.front();
                const auto& item = allocations[index];
                if (item.guest || item.buffer == nullptr || item.size != binding.guestDescriptor.size() * sizeof(std::uint32_t)) return refuse(MoveRefusal::Shape);
                const bool patched = std::any_of(dataPatches.begin(), dataPatches.end(), [&](const DataPatch& patch) { return patch.allocation == index; });
                const bool same = !item.dataWords.empty() ? item.dataWords == binding.guestDescriptor : !patched && std::memcmp(item.buffer->Bytes().data(), binding.guestDescriptor.data(), item.size) == 0;
                if (same) continue;
                if (item.dataWords.empty() && patched) {
                    if (patchedData != nullptr) *patchedData = true;
                    return refuse(MoveRefusal::Bindings);
                }
                if (moved.empty()) moved.reserve(allocations.size());
                moved.push_back({index, 0, item.size, binding.guestDescriptor});
                continue;
            }
            if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
            const auto kept = std::find_if(bindings.begin(), bindings.end(), [&](const Binding& item) { return item.layout.binding == binding.binding; });
            if (kept == bindings.end() || kept->allocations.size() != binding.count || binding.guestDescriptor.size() != static_cast<std::size_t>(binding.count) * 4u) return refuse(MoveRefusal::Shape);
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                const auto* words = binding.guestDescriptor.data() + static_cast<std::size_t>(element) * 4u;
                const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1], words[2], words[3]}};
                const auto address = descriptor.Base48();
                const bool written = element >= binding.Flags().bufferWritten.size() || binding.Flags().bufferWritten[element];
                const auto size = BytesUpToTarget(address, descriptor.GetSize(), descriptor.Stride(), written ? nullptr : target);
                const auto index = kept->allocations[element];
                const auto& item = allocations[index];
                const bool empty = size == 0 || address == 0;
                if (!item.guest) {
                    if (empty) continue;
                    return refuse(MoveRefusal::NotGuest);
                }
                if (item.address == address && item.size == size) continue;
                if (written || empty || item.written) return refuse(MoveRefusal::Written);
                if (size > context.limits.maxStorageBufferRange) return refuse(MoveRefusal::Limit);
                const auto begin = address - item.adjustment;
                const auto bytes = static_cast<std::size_t>(GuestBufferMemory::ViewBytes(size, item.adjustment));
                if (bytes > maxDrawSnapshotBytes - movedBytes) return refuse(MoveRefusal::Limit);
                if (!GuestMemory::Accessible(reinterpret_cast<const void*>(begin), bytes)) return refuse(MoveRefusal::Inaccessible);
                if (guestMemory.WritesOverlap(begin, bytes) || recorder.PendingWriteOverlaps(begin, bytes) || PendingStorageOverlaps(begin, bytes, nullptr) || AnyShadowedOverlaps(begin, bytes)) return refuse(MoveRefusal::Overlap);
                if (moved.empty()) moved.reserve(allocations.size());
                moved.push_back({index, address, static_cast<std::size_t>(size)});
                movedBytes += bytes;
            }
        }
    }
    return moved;
}

std::vector<const DrawReferenceMasks::Words*> ShaderResources::MovedIgnoredWords(std::span<const CompiledShader> shaders, std::span<const DrawReferenceMasks> masks, std::span<const MovedBuffer> moved) const {
    std::vector<const DrawReferenceMasks::Words*> ignored(moved.size(), nullptr);
    for (std::size_t stage = 0; stage < shaders.size() && stage < masks.size(); ++stage) {
        if (shaders[stage].program == nullptr) continue;
        const auto& programBindings = shaders[stage].program->bindings;
        for (std::size_t index = 0; index < programBindings.size() && index < masks[stage].bindings.size(); ++index) {
            if (!DataRole(programBindings[index].role) || masks[stage].bindings[index].empty()) continue;
            const auto kept = std::find_if(bindings.begin(), bindings.end(), [&](const Binding& item) { return item.layout.binding == programBindings[index].binding; });
            if (kept == bindings.end() || kept->allocations.size() != 1) continue;
            for (std::size_t element = 0; element < moved.size(); ++element) {
                if (!moved[element].words.empty() && moved[element].allocation == kept->allocations.front()) ignored[element] = &masks[stage].bindings[index];
            }
        }
    }
    return ignored;
}

bool ShaderResources::MovedStillReadOnly(std::span<const MovedBuffer> moved, Recorder& recorder) const {
    for (const auto& entry : moved) {
        if (!entry.words.empty()) continue;
        if (entry.allocation >= allocations.size()) return false;
        const auto& item = allocations[entry.allocation];
        const auto begin = entry.address - item.adjustment;
        const auto bytes = static_cast<std::size_t>(GuestBufferMemory::ViewBytes(entry.size, item.adjustment));
        if (!GuestMemory::Accessible(reinterpret_cast<const void*>(begin), bytes)) return false;
        if (guestMemory.WritesOverlap(begin, bytes) || recorder.PendingWriteOverlaps(begin, bytes) || PendingStorageOverlaps(begin, bytes, nullptr) || AnyShadowedOverlaps(begin, bytes)) return false;
    }
    return true;
}

const char* ShaderResources::MoveRefusalName(MoveRefusal refusal) {
    switch (refusal) {
        case MoveRefusal::None: return "none";
        case MoveRefusal::Shape: return "bindings differ";
        case MoveRefusal::NotGuest: return "not read in place";
        case MoveRefusal::Written: return "written or emptied";
        case MoveRefusal::Limit: return "over the limit";
        case MoveRefusal::Inaccessible: return "unmapped";
        case MoveRefusal::Overlap: return "pending write";
        case MoveRefusal::Bindings: return "no bindings made";
        case MoveRefusal::Count: break;
    }
    return "?";
}

std::optional<std::vector<ShaderResources::MovedBuffer>> ShaderResources::MovedDispatchBuffers(const CompiledShader& shader, Recorder* recorder, std::size_t maxBytes, MoveProof& proof) const {
    proof = {};
    const bool diagnose = AgcDriver::DispatchBuildTrace::Enabled();
    FailureDetail candidate;
    const auto refuse = [&](MoveRefusal why, const char* check) {
        proof.refusal = why;
        if (diagnose) {
            proof.detail = candidate;
            proof.detail.check = check;
        }
        return std::optional<std::vector<MovedBuffer>>{};
    };
    std::vector<MovedBuffer> moved;
    if (shader.program == nullptr) return refuse(MoveRefusal::Shape, "missing-shader-program");
    if (usesBda) return refuse(MoveRefusal::Shape, "uses-bda");
    if (_set == VK_NULL_HANDLE) return moved;
    for (const auto& binding : shader.program->bindings) {
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
        if (diagnose) candidate = {"not-recorded", "guest-buffer", binding.binding};
        const auto kept = std::find_if(bindings.begin(), bindings.end(), [&](const Binding& item) { return item.layout.binding == binding.binding; });
        if (kept == bindings.end()) return refuse(MoveRefusal::Shape, "binding-missing");
        if (kept->allocations.size() != binding.count) return refuse(MoveRefusal::Shape, "binding-count");
        if (binding.guestDescriptor.size() != static_cast<std::size_t>(binding.count) * 4u) return refuse(MoveRefusal::Shape, "descriptor-word-count");
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const auto* words = binding.guestDescriptor.data() + static_cast<std::size_t>(element) * 4u;
            const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1], words[2], words[3]}};
            const auto address = descriptor.Base48();
            const auto size = guestBufferBytes(std::span<const std::uint32_t>(words, 4));
            const auto& item = allocations[kept->allocations[element]];
            if (diagnose) {
                candidate.element = element;
                candidate.priorAddress = item.address;
                candidate.currentAddress = address;
                candidate.priorBytes = item.size;
                candidate.currentBytes = size;
            }
            const bool empty = size == 0 || address == 0;
            if (!item.guest) {
                if (empty) continue;
                return refuse(MoveRefusal::NotGuest, "not-guest");
            }
            if (item.address == address && item.size == size) continue;
            const bool written = element >= binding.Flags().bufferWritten.size() || binding.Flags().bufferWritten[element];
            if (written) return refuse(MoveRefusal::Written, "current-buffer-written");
            if (empty) return refuse(MoveRefusal::Written, "empty-buffer");
            if (item.written) return refuse(MoveRefusal::Written, "prior-buffer-written");
            if (size > context.limits.maxStorageBufferRange) return refuse(MoveRefusal::Limit, "storage-buffer-range-limit");
            const auto begin = address - item.adjustment;
            const auto bytes = static_cast<std::size_t>(GuestBufferMemory::ViewBytes(size, item.adjustment));
            if (maxBytes != 0 && proof.bytes + bytes > maxBytes) return refuse(MoveRefusal::Limit, "snapshot-byte-limit");
            if (!GuestMemory::Accessible(reinterpret_cast<const void*>(begin), bytes)) return refuse(MoveRefusal::Inaccessible, "inaccessible-buffer");
            const bool pending = recorder != nullptr ? recorder->PendingWriteOverlaps(begin, bytes) : Recorder::SnapshotWriteOverlaps(begin, bytes);
            const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> range{{{begin, begin + bytes}}};
            if (guestMemory.WritesOverlap(begin, bytes)) return refuse(MoveRefusal::Overlap, "own-write-overlap");
            if (pending) return refuse(MoveRefusal::Overlap, "pending-write-overlap");
            if (StorageTexture::AnyPendingOverlaps(range)) return refuse(MoveRefusal::Overlap, "pending-image-overlap");
            if (AnyShadowedOverlaps(begin, bytes)) return refuse(MoveRefusal::Overlap, "shadow-overlap");
            moved.push_back({kept->allocations[element], address, static_cast<std::size_t>(size)});
            ++proof.buffers;
            proof.bytes += bytes;
        }
    }
    return moved;
}

std::size_t ShaderResources::MaxDrawSnapshotBytes() {
    const char* text = std::getenv("APS5_DRAW_SNAPSHOT_MAX_MIB");
    const auto mib = text != nullptr ? std::strtoull(text, nullptr, 10) : 64ull;
    return static_cast<std::size_t>(mib * 1024ull * 1024ull);
}

std::size_t ShaderResources::DrawUploadLimit() {
    static const std::size_t limit = [] {
        const char* value = std::getenv("APS5_DRAW_UPLOAD_MAX");
        return value != nullptr ? static_cast<std::size_t>(std::strtoull(value, nullptr, 10)) : std::size_t{4096};
    }();
    return limit;
}

namespace {
// APS5_ZC_MOVED_CENSUS (diagnostic, read once): PrepareDrawBindings' cost and what its moved buffers are.
// Per call: all calls and their ns; calls with moved buffers, their ns, their moved-address and data-word
// elements, the moved-address elements an import serves at a storage-aligned offset, and the calls whose
// key (this template, every moved element's index, address, size and words) was seen in the last 8,192
// calls with moved buffers (about 12 frames): what a set cache keyed by the moved content would hit.
struct MovedCensusCounters {
    std::atomic<std::uint64_t> calls{0}, ns{0}, movedCalls{0}, movedNs{0}, addresses{0}, words{0}, importable{0}, repeats{0}, setCalls{0}, wordsNs{0};
};

MovedCensusCounters* movedCensus() {
    static MovedCensusCounters* made = [] {
        const char* value = std::getenv("APS5_ZC_MOVED_CENSUS");
        if (value == nullptr || std::strcmp(value, "1") != 0) return static_cast<MovedCensusCounters*>(nullptr);
        auto* counters = new MovedCensusCounters();
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_pdb_calls", counters->calls);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_pdb_ns", counters->ns);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_pdb_moved_calls", counters->movedCalls);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_pdb_moved_ns", counters->movedNs);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_pdb_moved_addresses", counters->addresses);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_pdb_moved_words", counters->words);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_pdb_moved_importable", counters->importable);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_pdb_moved_repeats", counters->repeats);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_pdb_set_calls", counters->setCalls);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_pdb_words_ns", counters->wordsNs);
        return counters;
    }();
    return made;
}
}

namespace {
// APS5_ARCHB_ZERO_COPY_MOVED counters: draws served by a cached set, sets built, draws refused (a moved
// range not served by an import at a usable offset, a pending GPU write, or a GPU store already in the
// open batch: the guest may see that label and rewrite the range before this draw reads it), entries
// dropped because the import table changed, and the _VERIFY checks and mismatches.
struct MovedSetCounters {
    std::atomic<std::uint64_t> hits{0}, built{0}, refusedImport{0}, refusedPending{0}, refusedStore{0}, stale{0}, verified{0}, mismatches{0};
};

MovedSetCounters& movedSetCounters() {
    static auto* made = [] {
        auto* counters = new MovedSetCounters();
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_moved_hits", counters->hits);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_moved_built", counters->built);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_moved_refused_import", counters->refusedImport);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_moved_refused_pending", counters->refusedPending);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_moved_refused_store", counters->refusedStore);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_moved_stale", counters->stale);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_moved_verified", counters->verified);
        AgcDriver::ArchB::Recorder::RegisterCounter("zc_moved_mismatches", counters->mismatches);
        return counters;
    }();
    return *made;
}

bool movedSetsVerify() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_ZERO_COPY_MOVED_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
}

// One immutable set per moved content: the object's own descriptors, the moved addresses pointing at
// their imports and the data words in a buffer of the set's own. Never written after it is built, so
// any number of in-flight draws may bind it; an entry holds it while cached, every draw while its batch
// runs, and the last holder recycles it. Valid while the import table keeps the identity it was built
// under (HostImportsUnchanged: no import retired, so every import buffer it points at still lives).
struct ShaderResources::MovedSetCache {
    struct Set {
        DescriptorCache* cache = nullptr;
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout fragmentLayout = VK_NULL_HANDLE;
        DescriptorCache::SetAllocation allocation;
        DescriptorCache::SetAllocation fragmentAllocation;
        std::vector<std::shared_ptr<Buffer>> words;
        std::vector<VkDescriptorBufferInfo> buffers;
        ~Set() {
            if (cache == nullptr) return;
            if (allocation.set != VK_NULL_HANDLE) cache->Recycle(layout, allocation);
            if (fragmentAllocation.set != VK_NULL_HANDLE) cache->Recycle(fragmentLayout, fragmentAllocation);
        }
    };
    struct Entry {
        std::vector<std::uint64_t> content;
        std::shared_ptr<Set> set;
        HostImportsProof proof;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
        // Per moved address: the import buffer and offset the set points at (_VERIFY).
        std::vector<std::pair<VkBuffer, VkDeviceSize>> targets;
        std::uint64_t lastUse = 0;
    };
    // Keyed by a hash of the content (the content itself decides a hit). Over capacity, every entry not
    // used in the last Capacity uses goes.
    static constexpr std::size_t Capacity = 1024;
    std::unordered_map<std::uint64_t, Entry> entries;
    std::uint64_t uses = 0;
};

std::shared_ptr<ShaderResources::DrawBindings> ShaderResources::cachedMovedBindings(Recorder& recorder, std::span<const MovedBuffer> moved, bool captureDescriptors) const {
    auto& counters = movedSetCounters();
    if (context.descriptorCache == nullptr || !faultDescriptors.empty()) return {};
    // A GPU store already in the open batch may be a label the guest waits on: once the GPU stores it,
    // the guest may rewrite a moved range before this later draw reads it in place (the in-flight
    // rewrites the zero-copy census found were all of moved ranges). Such draws keep their snapshots.
    if (recorder.OpenGpuStores() != 0) {
        counters.refusedStore.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    struct ContentSlotStorage {};
    auto& content = AgcDriver::ThreadScratch<std::vector<std::uint64_t>, ContentSlotStorage>();
    content.clear();
    struct TargetsSlotStorage {};
    auto& targets = AgcDriver::ThreadScratch<std::vector<std::pair<VkBuffer, VkDeviceSize>>, TargetsSlotStorage>();
    targets.clear();
    struct ReadsSlotStorage {};
    auto& reads = AgcDriver::ThreadScratch<std::vector<std::pair<std::uint64_t, std::uint64_t>>, ReadsSlotStorage>();
    reads.clear();
    const VkDeviceSize alignment = std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 4);
    for (const auto& entry : moved) {
        content.push_back(entry.allocation);
        content.push_back(entry.address);
        content.push_back(entry.size);
        content.push_back(entry.words.size());
        for (const auto word : entry.words) content.push_back(word);
        if (!entry.words.empty()) continue;
        const auto& item = allocations[entry.allocation];
        const auto begin = entry.address - item.adjustment;
        const auto bytes = static_cast<std::size_t>(GuestBufferMemory::ViewBytes(entry.size, item.adjustment));
        if (bytes == 0 || recorder.PendingWriteOverlaps(begin, bytes)) {
            counters.refusedPending.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        reads.emplace_back(begin, begin + bytes);
    }
    // The import buffer and offset of every moved range (a miss builds with them, VERIFY compares them
    // with the entry's); false when one is not served at a usable offset.
    const auto lookupTargets = [&] {
        targets.clear();
        for (const auto& [begin, end] : reads) {
            const auto* import = ExistingHostImportFor(context, begin, static_cast<std::size_t>(end - begin));
            if (import == nullptr || HostImportOffset(*import, begin) % alignment != 0) {
                counters.refusedImport.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            targets.emplace_back(import->buffer, HostImportOffset(*import, begin));
        }
        return true;
    };
    if (movedSetCache == nullptr) movedSetCache = std::make_shared<MovedSetCache>();
    auto& cache = *movedSetCache;
    ++cache.uses;
    std::uint64_t key = 0xcbf29ce484222325ull;
    for (const auto value : content) key = (key ^ value) * 0x100000001b3ull + (key >> 31u);
    auto found = cache.entries.find(key);
    if (found != cache.entries.end() && found->second.content != content) {
        cache.entries.erase(found);
        found = cache.entries.end();
    }
    if (found != cache.entries.end() && !HostImportsUnchanged(context, found->second.proof)) {
        counters.stale.fetch_add(1, std::memory_order_relaxed);
        cache.entries.erase(found);
        found = cache.entries.end();
    }
    if (found == cache.entries.end()) {
        if (!lookupTargets()) return {};
        const auto proof = HostImportsIdentity(context);
        if (proof == HostImportsProof{}) return {};
        auto set = std::make_shared<MovedSetCache::Set>();
        set->cache = context.descriptorCache;
        std::array<bool, 2> described{};
        std::array<std::vector<VkDescriptorPoolSize>, 2> counts;
        for (const auto& binding : bindings) {
            const auto which = setOf(binding);
            described[which] = true;
            const auto count = (binding.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) != 0 ? std::max(1u, static_cast<std::uint32_t>(binding.imageAllocations.size())) : binding.layout.descriptorCount;
            if (count == 0) continue;
            auto& sizes = counts[which];
            const auto held = std::find_if(sizes.begin(), sizes.end(), [&](const VkDescriptorPoolSize& size) { return size.type == binding.layout.descriptorType; });
            if (held != sizes.end()) held->descriptorCount += count;
            else sizes.push_back({binding.layout.descriptorType, count});
        }
        for (auto& sizes : counts) std::sort(sizes.begin(), sizes.end(), [](const VkDescriptorPoolSize& a, const VkDescriptorPoolSize& b) { return a.type < b.type; });
        const auto variableOf = [&](std::size_t which) { return tableBindings[which] != std::numeric_limits<std::size_t>::max() ? std::optional<std::uint32_t>(std::max(1u, static_cast<std::uint32_t>(bindings[tableBindings[which]].imageAllocations.size()))) : std::nullopt; };
        if (described[0] || splitSets) {
            set->allocation = set->cache->AllocateRewritten(_layout, counts[0], updateAfterBind, variableOf(0));
            set->layout = _layout;
            Require(set->allocation.set != VK_NULL_HANDLE, "cached moved set allocation failed");
        }
        if (described[1] || splitSets) {
            set->fragmentAllocation = set->cache->AllocateRewritten(fragment.layout, counts[1], updateAfterBind, variableOf(1));
            set->fragmentLayout = fragment.layout;
            Require(set->fragmentAllocation.set != VK_NULL_HANDLE, "cached moved set allocation failed");
        }
        auto& buffers = set->buffers;
        buffers = descriptorBuffers;
        auto writes = descriptorWrites;
        std::size_t target = 0;
        for (const auto& entry : moved) {
            // The element's descriptor slot: its binding's write, at the element's position.
            bool placed = false;
            for (const auto& binding : bindings) {
                const auto position = std::find(binding.allocations.begin(), binding.allocations.end(), entry.allocation);
                if (position == binding.allocations.end()) continue;
                const auto original = setOf(binding) == 0 ? _set : fragment.set;
                const auto write = std::find_if(writes.begin(), writes.end(), [&](const VkWriteDescriptorSet& candidate) { return candidate.dstBinding == binding.layout.binding && candidate.dstSet == original && candidate.pBufferInfo != nullptr; });
                Require(write != writes.end(), "a moved element has no buffer descriptor write");
                const auto slot = static_cast<std::size_t>(write->pBufferInfo - descriptorBuffers.data()) + static_cast<std::size_t>(position - binding.allocations.begin());
                if (entry.words.empty()) {
                    buffers[slot] = wholeDwordRange({targets[target].first, targets[target].second, static_cast<VkDeviceSize>(reads[target].second - reads[target].first)});
                    ++target;
                } else {
                    auto words = std::make_shared<Buffer>(context, entry.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                    auto* bytes = words->Bytes().data();
                    std::memcpy(bytes, entry.words.data(), entry.size);
                    for (const auto& patch : dataPatches) {
                        if (patch.allocation == entry.allocation && patch.byte < entry.size) bytes[patch.byte] = static_cast<std::byte>(patch.adjustment);
                    }
                    buffers[slot] = {words->Handle(), 0, entry.size};
                    set->words.push_back(std::move(words));
                }
                placed = true;
                break;
            }
            Require(placed, "a moved element has no binding");
        }
        for (auto& write : writes) {
            write.dstSet = write.dstSet == _set ? set->allocation.set : set->fragmentAllocation.set;
            if (write.pBufferInfo != nullptr) write.pBufferInfo = buffers.data() + (write.pBufferInfo - descriptorBuffers.data());
        }
        if (!writes.empty()) context.Resolved(&DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        if (cache.entries.size() >= MovedSetCache::Capacity) {
            const auto floor = cache.uses > MovedSetCache::Capacity ? cache.uses - MovedSetCache::Capacity : 0;
            std::erase_if(cache.entries, [&](const auto& entry) { return entry.second.lastUse < floor; });
            if (cache.entries.size() >= MovedSetCache::Capacity) cache.entries.clear();
        }
        found = cache.entries.emplace(key, MovedSetCache::Entry{content, std::move(set), proof, reads, targets, 0}).first;
        counters.built.fetch_add(1, std::memory_order_relaxed);
    } else {
        counters.hits.fetch_add(1, std::memory_order_relaxed);
        if (movedSetsVerify()) {
            counters.verified.fetch_add(1, std::memory_order_relaxed);
            if (!lookupTargets() || found->second.targets != targets || found->second.reads != reads) counters.mismatches.fetch_add(1, std::memory_order_relaxed);
        }
    }
    auto& entry = found->second;
    entry.lastUse = cache.uses;
    auto result = std::make_shared<DrawBindings>();
    result->allocation = entry.set->allocation;
    result->fragmentAllocation = entry.set->fragmentAllocation;
    result->cachedSet = entry.set;
    if (captureDescriptors) result->capturedDescriptors = entry.set->buffers;
    result->inPlaceReads = entry.reads;
    result->movedProof = entry.proof;
    static const bool noNotes = std::getenv("APS5_ZC_MOVED_NO_READNOTES") != nullptr;
    if (!noNotes)
    for (const auto& [begin, end] : entry.reads) recorder.NotePendingRead(begin, static_cast<std::size_t>(end - begin), Recorder::ReadKind::DispatchElement);
    recorder.Keep(result);
    return result;
}

ShaderResources::TemplateSets::~TemplateSets() {
    if (cache == nullptr) return;
    for (std::size_t flight = 0; flight < allocations.size(); ++flight) {
        if (allocations[flight].set != VK_NULL_HANDLE) cache->Recycle(layout, allocations[flight]);
        if (fragmentAllocations[flight].set != VK_NULL_HANDLE) cache->Recycle(fragmentLayout, fragmentAllocations[flight]);
    }
}

std::shared_ptr<ShaderResources::TemplateSets> ShaderResources::MakeTemplateSets(std::span<const TemplateElement> elements, std::span<const std::pair<VkBuffer, VkDeviceSize>> flights) const {
    if (context.descriptorCache == nullptr || usesBda || flights.empty()) return {};
    if (_set == VK_NULL_HANDLE && fragment.set == VK_NULL_HANDLE) return {};
    const auto proof = HostImportsIdentity(context);
    if (proof == HostImportsProof{}) return {};
    auto made = std::make_shared<TemplateSets>();
    made->elements.assign(elements.begin(), elements.end());
    for (const auto& element : elements) {
        if (element.allocation >= allocations.size()) return {};
        std::optional<std::size_t> slot;
        for (const auto& binding : bindings) {
            const auto position = std::find(binding.allocations.begin(), binding.allocations.end(), element.allocation);
            if (position == binding.allocations.end()) continue;
            const auto original = setOf(binding) == 0 ? _set : fragment.set;
            const auto write = std::find_if(descriptorWrites.begin(), descriptorWrites.end(), [&](const VkWriteDescriptorSet& candidate) { return candidate.dstBinding == binding.layout.binding && candidate.dstSet == original && candidate.pBufferInfo != nullptr; });
            if (write == descriptorWrites.end() || static_cast<std::uint32_t>(position - binding.allocations.begin()) >= write->descriptorCount) return {};
            slot = static_cast<std::size_t>(write->pBufferInfo - descriptorBuffers.data()) + static_cast<std::size_t>(position - binding.allocations.begin());
            break;
        }
        if (!slot.has_value() || *slot >= descriptorBuffers.size() || std::find(made->streamed.begin(), made->streamed.end(), *slot) != made->streamed.end()) return {};
        made->streamed.push_back(*slot);
    }
    made->cache = context.descriptorCache;
    made->layout = _layout;
    made->fragmentLayout = fragment.layout;
    made->images = descriptorImages;
    made->proof = proof;
    AddTemplateFlights(*made, flights);
    return made;
}

void ShaderResources::AddTemplateFlights(TemplateSets& made, std::span<const std::pair<VkBuffer, VkDeviceSize>> flights) const {
    prepareFaultSlot();
    std::array<bool, 2> described{};
    std::array<std::vector<VkDescriptorPoolSize>, 2> counts;
    for (const auto& binding : bindings) {
        const auto which = setOf(binding);
        described[which] = true;
        const auto count = (binding.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) != 0 ? std::max(1u, static_cast<std::uint32_t>(binding.imageAllocations.size())) : binding.layout.descriptorCount;
        if (count == 0) continue;
        auto& sizes = counts[which];
        const auto held = std::find_if(sizes.begin(), sizes.end(), [&](const VkDescriptorPoolSize& size) { return size.type == binding.layout.descriptorType; });
        if (held != sizes.end()) held->descriptorCount += count;
        else sizes.push_back({binding.layout.descriptorType, count});
    }
    for (auto& sizes : counts) std::sort(sizes.begin(), sizes.end(), [](const VkDescriptorPoolSize& a, const VkDescriptorPoolSize& b) { return a.type < b.type; });
    const auto variableOf = [&](std::size_t which) { return tableBindings[which] != std::numeric_limits<std::size_t>::max() ? std::optional<std::uint32_t>(std::max(1u, static_cast<std::uint32_t>(bindings[tableBindings[which]].imageAllocations.size()))) : std::nullopt; };
    for (const auto& [buffer, base] : flights) {
        auto& allocation = made.allocations.emplace_back();
        auto& fragmentAllocation = made.fragmentAllocations.emplace_back();
        if (described[0] || splitSets) {
            allocation = made.cache->AllocateRewritten(_layout, counts[0], updateAfterBind, variableOf(0));
            Require(allocation.set != VK_NULL_HANDLE, "template set allocation failed");
            ++made.sets;
        }
        if (described[1] || splitSets) {
            fragmentAllocation = made.cache->AllocateRewritten(fragment.layout, counts[1], updateAfterBind, variableOf(1));
            Require(fragmentAllocation.set != VK_NULL_HANDLE, "template set allocation failed");
            ++made.sets;
        }
        auto& buffers = made.buffers.emplace_back(descriptorBuffers);
        for (std::size_t index = 0; index < made.elements.size(); ++index) {
            if (!made.elements[index].inPlace) buffers[made.streamed[index]] = wholeDwordRange({buffer, base + made.elements[index].offset, static_cast<VkDeviceSize>(made.elements[index].bytes)});
        }
        auto writes = descriptorWrites;
        for (auto& write : writes) {
            write.dstSet = write.dstSet == _set ? allocation.set : fragmentAllocation.set;
            if (write.pBufferInfo != nullptr) write.pBufferInfo = buffers.data() + (write.pBufferInfo - descriptorBuffers.data());
        }
        if (!writes.empty()) context.Resolved(&DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        made.writes += writes.size();
    }
}

void ShaderResources::KeepTemplateSets(std::shared_ptr<TemplateSets> sets) const {
    constexpr std::size_t Held = 256;
    if (templateSetsHeld.size() >= Held) templateSetsHeld.erase(templateSetsHeld.begin(), templateSetsHeld.begin() + static_cast<std::ptrdiff_t>(Held / 2));
    templateSetsHeld.push_back(std::move(sets));
}

std::pair<std::uint64_t, std::size_t> ShaderResources::MovedView(const MovedBuffer& entry) const {
    const auto adjustment = allocations[entry.allocation].adjustment;
    return {entry.address - adjustment, entry.viewBytes != 0 ? entry.viewBytes : static_cast<std::size_t>(GuestBufferMemory::ViewBytes(entry.size, adjustment))};
}

std::pair<std::size_t, std::size_t> ShaderResources::UpdateTemplateFlight(TemplateSets& sets, std::size_t flight, std::span<const std::pair<std::size_t, VkDescriptorBufferInfo>> inPlace) const {
    if (faultDescriptors.empty() && inPlace.empty()) return {0, 0};
    prepareFaultSlot();
    struct FaultWritesStorage {};
    auto& writes = AgcDriver::ThreadScratch<std::vector<VkWriteDescriptorSet>, FaultWritesStorage>();
    writes.clear();
    for (const auto& [index, info] : inPlace) {
        const auto slot = sets.streamed[index];
        auto& held = sets.buffers[flight][slot];
        if (held.buffer == info.buffer && held.offset == info.offset && held.range == info.range) continue;
        held = info;
        const auto write = std::find_if(descriptorWrites.begin(), descriptorWrites.end(), [&](const VkWriteDescriptorSet& candidate) { return candidate.pBufferInfo != nullptr && slot >= static_cast<std::size_t>(candidate.pBufferInfo - descriptorBuffers.data()) && slot < static_cast<std::size_t>(candidate.pBufferInfo - descriptorBuffers.data()) + candidate.descriptorCount; });
        Require(write != descriptorWrites.end(), "an in-place template element has no buffer descriptor write");
        auto made = *write;
        made.dstSet = made.dstSet == _set ? sets.allocations[flight].set : sets.fragmentAllocations[flight].set;
        made.dstArrayElement = write->dstArrayElement + static_cast<std::uint32_t>(slot - static_cast<std::size_t>(write->pBufferInfo - descriptorBuffers.data()));
        made.descriptorCount = 1;
        made.pBufferInfo = &held;
        writes.push_back(made);
    }
    const auto placed = writes.size();
    for (const auto& fault : faultDescriptors) {
        auto& held = sets.buffers[flight][fault.buffer];
        const auto& current = descriptorBuffers[fault.buffer];
        if (held.buffer == current.buffer && held.offset == current.offset && held.range == current.range) continue;
        held = current;
        auto write = descriptorWrites[fault.write];
        write.dstSet = write.dstSet == _set ? sets.allocations[flight].set : sets.fragmentAllocations[flight].set;
        write.dstArrayElement = fault.element;
        write.descriptorCount = 1;
        write.pBufferInfo = &held;
        writes.push_back(write);
    }
    if (!writes.empty()) context.Resolved(&DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return {placed, writes.size() - placed};
}

void ShaderResources::DropTemplateSets(const TemplateSets* sets) const {
    std::erase_if(templateSetsHeld, [&](const std::shared_ptr<TemplateSets>& held) { return held.get() == sets; });
}

std::size_t ShaderResources::StreamBytes(const MovedBuffer& entry) const {
    if (!entry.words.empty()) return entry.size;
    return entry.viewBytes != 0 ? entry.viewBytes : static_cast<std::size_t>(GuestBufferMemory::ViewBytes(entry.size, allocations[entry.allocation].adjustment));
}

void ShaderResources::StreamElement(const MovedBuffer& entry, std::byte* destination) const {
    if (!entry.words.empty()) {
        std::memcpy(destination, entry.words.data(), entry.size);
        for (const auto& patch : dataPatches) {
            if (patch.allocation == entry.allocation && patch.byte < entry.size) destination[patch.byte] = static_cast<std::byte>(patch.adjustment);
        }
        return;
    }
    std::memcpy(destination, reinterpret_cast<const void*>(entry.address - allocations[entry.allocation].adjustment), StreamBytes(entry));
}

bool ShaderResources::ExpectedElementBytes(const MovedBuffer& entry, std::vector<std::byte>& out) const {
    if (entry.allocation >= allocations.size()) return false;
    if (!entry.words.empty()) {
        out.assign(entry.size, std::byte{0});
        std::memcpy(out.data(), entry.words.data(), std::min(entry.size, entry.words.size_bytes()));
        for (const auto& patch : dataPatches) {
            if (patch.allocation == entry.allocation && patch.byte < out.size()) out[patch.byte] = static_cast<std::byte>(patch.adjustment);
        }
        return true;
    }
    const auto& item = allocations[entry.allocation];
    const auto begin = entry.address - item.adjustment;
    const auto bytes = static_cast<std::size_t>(GuestBufferMemory::ViewBytes(entry.size, item.adjustment));
    if (!GuestMemory::Accessible(reinterpret_cast<const void*>(begin), bytes)) return false;
    out.resize(bytes);
    std::memcpy(out.data(), reinterpret_cast<const void*>(begin), bytes);
    return true;
}

bool ShaderResources::RegisteredViewsCurrent(std::span<const ShaderRecompiler::RegisteredImageView> views) {
    return registeredImageViewsCurrent(views);
}

bool ShaderResources::GatherBindings(DriverDetail::GatherCommitTemplate& target, const DriverDetail::GatherDrawPlan& plan, std::span<const MovedBuffer> moved) const {
    if (target.ready.load(std::memory_order_acquire)) return true;
    if (target.stages.size() != plan.stages.size()) return false;
    for (std::size_t stage = 0; stage < plan.stages.size(); ++stage) {
        auto& out = target.stages[stage];
        out.bindings.clear();
        for (const auto& vsharp : plan.stages[stage].vsharps) {
            if (vsharp.binding >= out.compiled->bindings.size()) return false;
            const auto& source = out.compiled->bindings[vsharp.binding];
            const auto bound = std::find_if(bindings.begin(), bindings.end(), [&](const auto& item) { return item.layout.binding == source.binding; });
            if (bound == bindings.end() || vsharp.element >= bound->allocations.size()) return false;
            const auto index = bound->allocations[vsharp.element];
            const auto& allocation = allocations[index];
            if (!allocation.guest || allocation.written) return false;
            const auto current = std::find_if(moved.begin(), moved.end(), [&](const auto& item) { return item.allocation == index; });
            const auto extent = current != moved.end() ? current->size : static_cast<std::size_t>(allocation.size);
            auto bytes = TemplateDescriptor({VK_NULL_HANDLE, 0, static_cast<VkDeviceSize>(GuestBufferMemory::ViewBytes(extent, allocation.adjustment))}).range;
            if (current == moved.end()) {
                const auto set = setOf(*bound) == 0 ? _set : fragment.set;
                const auto write = std::find_if(descriptorWrites.begin(), descriptorWrites.end(), [&](const auto& write) { return write.dstSet == set && write.dstBinding == bound->layout.binding && write.pBufferInfo != nullptr; });
                if (write == descriptorWrites.end()) return false;
                bytes = descriptorBuffers[static_cast<std::size_t>(write->pBufferInfo - descriptorBuffers.data()) + vsharp.element].range;
            }
            if (bytes == 0 || bytes > context.limits.maxStorageBufferRange) return false;
            out.bindings.push_back({vsharp.binding, vsharp.element, index, extent, allocation.adjustment, bytes});
        }
    }
    return true;
}

std::optional<std::vector<ShaderResources::MovedBuffer>> ShaderResources::GatherBuffers(const DriverDetail::GatherDrawPlan& plan, Recorder& recorder, MoveRefusal& refusal) const {
    const auto fail = [&](MoveRefusal why) { refusal = why; return std::optional<std::vector<MovedBuffer>>{}; };
    const bool timing = ArchC::probing.load(std::memory_order_relaxed);
    enum Check : std::size_t { CheckPendingWrite, CheckPendingStorage, CheckShadowed, CheckWrites, CheckAccessible, CheckCount };
    static constexpr const char* checkNames[CheckCount] = {"gather_buffers_ns_pending_write", "gather_buffers_ns_pending_storage", "gather_buffers_ns_shadowed", "gather_buffers_ns_writes", "gather_buffers_ns_accessible"};
    const auto timed = [&](Check check, auto&& test) {
        if (!timing) return test();
        const auto start = std::chrono::steady_clock::now();
        const bool result = test();
        AgcDriver::ArchB::Recorder::Count(checkNames[check], static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()));
        return result;
    };
    const auto pendingWrite = [&](std::uint64_t address, std::size_t bytes) { return timed(CheckPendingWrite, [&] { return recorder.PendingWriteOverlaps(address, bytes); }); };
    const auto pendingStorage = [&](std::uint64_t address, std::size_t bytes) { return timed(CheckPendingStorage, [&] { return PendingStorageOverlaps(address, bytes, nullptr); }); };
    const auto shadowed = [&](std::uint64_t address, std::size_t bytes) { return timed(CheckShadowed, [&] { return AnyShadowedOverlaps(address, bytes); }); };
    if (!plan.complete || plan.commitTemplate == nullptr || !plan.commitTemplate->valid.load(std::memory_order_acquire) || !plan.commitTemplate->ready.load(std::memory_order_acquire) || usesBda) return fail(MoveRefusal::Shape);
    std::vector<MovedBuffer> moved;
    moved.reserve(allocations.size());
    for (std::size_t stage = 0; stage < plan.stages.size(); ++stage) {
        const auto& source = plan.stages[stage];
        for (const auto& read : source.reads) {
            const auto known = std::find_if(source.knownReads.begin(), source.knownReads.end(), [&](const auto& item) { return item.address == read.first; });
            if (known != source.knownReads.end()) {
                if (pendingStorage(read.first, static_cast<std::size_t>(read.second - read.first)) || shadowed(read.first, static_cast<std::size_t>(read.second - read.first))) return fail(MoveRefusal::Overlap);
                AgcDriver::ArchB::Recorder::Count("draw_templates_live_commit_known_reads");
                continue;
            }
            if (pendingWrite(read.first, static_cast<std::size_t>(read.second - read.first)) || pendingStorage(read.first, static_cast<std::size_t>(read.second - read.first)) || shadowed(read.first, static_cast<std::size_t>(read.second - read.first))) return fail(MoveRefusal::Overlap);
        }
        const auto& shape = plan.commitTemplate->stages[stage];
        for (const auto& vsharp : source.vsharps) {
            const auto known = std::find_if(shape.bindings.begin(), shape.bindings.end(), [&](const auto& item) { return item.binding == vsharp.binding && item.element == vsharp.element; });
            if (known == shape.bindings.end() || known->allocation >= allocations.size()) return fail(MoveRefusal::Shape);
            const auto& item = allocations[known->allocation];
            if (!item.guest || item.written || item.adjustment != vsharp.adjustment || known->extent != vsharp.extent || known->viewBytes != vsharp.viewBytes || vsharp.address < vsharp.adjustment) return fail(MoveRefusal::Shape);
            const auto begin = vsharp.address - vsharp.adjustment;
            const auto bytes = static_cast<std::size_t>(vsharp.viewBytes);
            if (!timed(CheckAccessible, [&] { return GuestMemory::Accessible(reinterpret_cast<const void*>(begin), bytes); })) return fail(MoveRefusal::Inaccessible);
            if (timed(CheckWrites, [&] { return guestMemory.WritesOverlap(begin, bytes); }) || pendingWrite(begin, bytes) || pendingStorage(begin, bytes) || shadowed(begin, bytes)) return fail(MoveRefusal::Overlap);
            if (vsharp.address == item.address && vsharp.extent == item.size) continue;
            moved.push_back({known->allocation, vsharp.address, static_cast<std::size_t>(vsharp.extent), {}, bytes});
        }
        for (const auto& buffer : source.buffers) {
            if (buffer.binding >= shape.compiled->bindings.size()) return fail(MoveRefusal::Shape);
            const auto& binding = shape.compiled->bindings[buffer.binding];
            const auto bound = std::find_if(bindings.begin(), bindings.end(), [&](const auto& item) { return item.layout.binding == binding.binding; });
            if (bound == bindings.end() || bound->allocations.size() != 1) return fail(MoveRefusal::Shape);
            const auto index = bound->allocations.front();
            const auto& item = allocations[index];
            if (item.guest || item.buffer == nullptr || item.size != buffer.words.size() * sizeof(std::uint32_t)) return fail(MoveRefusal::Shape);
            moved.push_back({index, 0, static_cast<std::size_t>(item.size), buffer.words});
        }
    }
    return moved;
}

bool ShaderResources::BoundElementBytes(const MovedBuffer& entry, const DrawBindings* bound, VkDescriptorBufferInfo& descriptor, std::vector<std::byte>& bytes) const {
    std::optional<std::size_t> slot;
    for (const auto& binding : bindings) {
        const auto at = std::find(binding.allocations.begin(), binding.allocations.end(), entry.allocation);
        if (at == binding.allocations.end()) continue;
        const auto original = setOf(binding) == 0 ? _set : fragment.set;
        const auto write = std::find_if(descriptorWrites.begin(), descriptorWrites.end(), [&](const VkWriteDescriptorSet& value) { return value.dstSet == original && value.dstBinding == binding.layout.binding && value.pBufferInfo != nullptr; });
        if (write == descriptorWrites.end()) return false;
        slot = static_cast<std::size_t>(write->pBufferInfo - descriptorBuffers.data()) + static_cast<std::size_t>(at - binding.allocations.begin());
        break;
    }
    const auto& buffers = bound != nullptr ? bound->capturedDescriptors : descriptorBuffers;
    if (!slot || *slot >= buffers.size()) return false;
    descriptor = buffers[*slot];
    const auto copy = [&](Buffer& buffer) {
        if (buffer.Handle() != descriptor.buffer) return false;
        auto mapped = buffer.Bytes();
        if (descriptor.offset > mapped.size() || descriptor.range > mapped.size() - descriptor.offset) return false;
        bytes.assign(mapped.begin() + static_cast<std::ptrdiff_t>(descriptor.offset), mapped.begin() + static_cast<std::ptrdiff_t>(descriptor.offset + descriptor.range));
        return true;
    };
    if (bound != nullptr) {
        for (const auto& snapshot : bound->snapshots) if (copy(*snapshot.buffer)) return true;
        if (bound->cachedSet != nullptr) {
            const auto cached = std::static_pointer_cast<MovedSetCache::Set>(bound->cachedSet);
            for (const auto& word : cached->words) if (copy(*word)) return true;
        }
    }
    const auto& allocation = allocations[entry.allocation];
    if (allocation.buffer != nullptr && copy(*allocation.buffer)) return true;
    if (!entry.words.empty()) return false;
    const auto [begin, size] = MovedView(entry);
    const auto* import = ExistingHostImportFor(context, begin, size);
    if (import == nullptr || descriptor.buffer != import->buffer || descriptor.offset != HostImportOffset(*import, begin) || descriptor.range > size) return false;
    bytes.resize(static_cast<std::size_t>(descriptor.range));
    return GuestMemory::CopyMapped(begin, bytes, true) == GuestMemory::Compare::Equal;
}

std::size_t ShaderResources::TemplateStaticMismatches(const TemplateSets& sets, std::size_t flight) const {
    const auto& buffers = sets.buffers[flight];
    if (buffers.size() != descriptorBuffers.size() || sets.images.size() != descriptorImages.size()) return 1;
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index < buffers.size(); ++index) {
        if (std::find(sets.streamed.begin(), sets.streamed.end(), index) != sets.streamed.end()) continue;
        if (buffers[index].buffer != descriptorBuffers[index].buffer || buffers[index].offset != descriptorBuffers[index].offset || buffers[index].range != descriptorBuffers[index].range) ++mismatches;
    }
    for (std::size_t index = 0; index < descriptorImages.size(); ++index) {
        if (sets.images[index].sampler != descriptorImages[index].sampler || sets.images[index].imageView != descriptorImages[index].imageView || sets.images[index].imageLayout != descriptorImages[index].imageLayout) ++mismatches;
    }
    return mismatches;
}

std::shared_ptr<ShaderResources::DrawBindings> ShaderResources::PrepareDrawBindings(Recorder& recorder, std::span<const MovedBuffer> moved, bool movedOnly, const BdaResources* faultRecord, bool captureDescriptors) const {
    requireGuestBuffers(moved);
    if ((_set == VK_NULL_HANDLE && fragment.set == VK_NULL_HANDLE) || (usesBda && !spaceReuse)) return {};
    prepareFaultSlot();
    struct MovedCensusScope {
        MovedCensusCounters* counters;
        bool moved;
        std::chrono::steady_clock::time_point start;
        bool set = false;
        ~MovedCensusScope() {
            if (counters == nullptr) return;
            const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
            counters->calls.fetch_add(1, std::memory_order_relaxed);
            counters->ns.fetch_add(ns, std::memory_order_relaxed);
            if (set) counters->setCalls.fetch_add(1, std::memory_order_relaxed);
            if (!moved) return;
            counters->movedCalls.fetch_add(1, std::memory_order_relaxed);
            counters->movedNs.fetch_add(ns, std::memory_order_relaxed);
        }
    } movedCensusScope{movedCensus(), !moved.empty(), movedCensus() != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
    if (movedCensusScope.counters != nullptr && !moved.empty()) {
        auto* counters = movedCensusScope.counters;
        std::uint64_t key = std::hash<const void*>{}(this);
        const auto mix = [&](std::uint64_t value) { key = (key ^ value) * 0x100000001b3ull + (key >> 29u); };
        const VkDeviceSize alignment = std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 4);
        for (const auto& entry : moved) {
            mix(entry.allocation);
            mix(entry.address);
            mix(entry.size);
            for (const auto word : entry.words) mix(word);
            if (!entry.words.empty()) {
                counters->words.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            counters->addresses.fetch_add(1, std::memory_order_relaxed);
            const auto begin = entry.address - allocations[entry.allocation].adjustment;
            const auto bytes = static_cast<std::size_t>(GuestBufferMemory::ViewBytes(entry.size, allocations[entry.allocation].adjustment));
            const auto* import = ExistingHostImportFor(context, begin, bytes);
            if (import != nullptr && HostImportOffset(*import, begin) % alignment == 0) counters->importable.fetch_add(1, std::memory_order_relaxed);
        }
        struct RingSlotThreadStorage {};
        auto& ringSlot = AgcDriver::ThreadScratch<std::vector<std::uint64_t>*, RingSlotThreadStorage>();
        struct SeenSlotThreadStorage {};
        auto& seenSlot = AgcDriver::ThreadScratch<std::unordered_map<std::uint64_t, std::uint32_t>*, SeenSlotThreadStorage>();
        struct RingNextThreadStorage {};
        auto& ringNext = AgcDriver::ThreadScratch<std::size_t, RingNextThreadStorage>();
        auto& ring = ShaderRecompiler::ThreadOwned(ringSlot);
        auto& seen = ShaderRecompiler::ThreadOwned(seenSlot);
        constexpr std::size_t Window = 8192;
        if (seen.contains(key)) counters->repeats.fetch_add(1, std::memory_order_relaxed);
        if (ring.size() < Window) {
            ring.push_back(key);
        } else {
            const auto old = ring[ringNext];
            if (auto found = seen.find(old); found != seen.end() && --found->second == 0) seen.erase(found);
            ring[ringNext] = key;
            ringNext = (ringNext + 1) % Window;
        }
        ++seen[key];
    }
    // APS5_ARCHB_ZERO_COPY_MOVED (on top of ZERO_COPY_STORAGE, which leaves every other element as the
    // object's set binds it): a cached immutable set for this moved content; nothing when refused.
    if (!moved.empty() && Recorder::ZeroCopyStorageActive() && AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::ZeroCopyMoved)) {
        if (auto cached = cachedMovedBindings(recorder, moved, captureDescriptors)) return cached;
    }
    // APS5_DRAW_BINDINGS_DIAG: the call's time by snapshot path (diagnostic).
    const bool diag = Recorder::SnapshotDiagEnabled();
    const auto diagStart = diag ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    struct DiagTotal {
        bool on;
        std::chrono::steady_clock::time_point start;
        ~DiagTotal() {
            if (on) bindingsDiag().totalNs.fetch_add(diagNs(start), std::memory_order_relaxed);
        }
    } diagTotal{diag, diagStart};
    struct ReadsSlotStorage {};
    auto& reads = AgcDriver::ThreadScratch<std::vector<std::pair<std::uint64_t, std::uint64_t>>, ReadsSlotStorage>();
    // APS5_ARCHB_FIX_SNAPSHOT_CANDIDATES: the checks that depend only on this object come from the list
    // made at build; _VERIFY makes them as well (their answer is used) and counts a differing element.
    const bool candidates = AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixSnapshotCandidates);
    const bool useCandidates = candidates && snapshotCandidatesKnown && snapshotCandidates.size() == allocations.size() && guestMemory.InPlaceReadsSettled();
    const bool verifyCandidates = useCandidates && snapshotCandidatesVerify();
    if (candidates) (useCandidates ? snapshotCandidateCounters().draws : snapshotCandidateCounters().fallbacks).fetch_add(1, std::memory_order_relaxed);
    std::uint64_t candidateSkips = 0;
    struct CandidateSkips {
        const std::uint64_t& count;
        ~CandidateSkips() {
            if (count != 0) snapshotCandidateCounters().skipped.fetch_add(count, std::memory_order_relaxed);
        }
    } candidateSkipsReport{candidateSkips};
    {
        const auto readsStart = diag ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (!useCandidates || verifyCandidates) guestMemory.InPlaceReads(reads);
        else reads.clear();
        if (diag) bindingsDiag().readsNs.fetch_add(diagNs(readsStart), std::memory_order_relaxed);
    }
    const auto maxSnapshotBytes = maxDrawSnapshotBytes;
    const auto uploadLimit = DrawUploadLimit();
    std::size_t totalSnapshotBytes = 0;
    auto result = std::make_shared<DrawBindings>();
    struct SelectedSlotStorage {};
    auto& selected = AgcDriver::ThreadScratch<std::vector<std::size_t>, SelectedSlotStorage>();
    selected.clear();
    std::array<std::uint64_t, 3> stamp{};
    bool stamped = false;
    const auto keep = [&](DrawBindings::Snapshot snapshot) {
        if (result->snapshots.empty()) result->snapshots.reserve(allocations.size());
        result->snapshots.push_back(std::move(snapshot));
    };
    // APS5_ARCHB_PIPE_DRAW_REUSE (Stage 3 B1): on the queue-0 committer, with the set pool, a selected
    // snapshot within the upload limit goes into the pooled set's own slot buffer instead of an upload
    // slice (the same selection and bytes, copied at the same point); larger snapshots and moved word
    // buffers keep their shared path. The set remembers the descriptor each of its snapshot elements
    // holds (DrawBindings::held), so the pool's next draw writes only the elements whose buffer,
    // offset or range changed: matching slots, a shared snapshot unchanged or refreshed in place, and
    // a held moved word buffer need no write.
    const bool slotted = drawSetPool != nullptr && AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::PipeDrawReuse) &&
        AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixDrawSetPool) && GuestMemory::IsCommitterThread();
    struct SlotElementsSlotThreadStorage {};
    auto& slotElementsSlot = AgcDriver::ThreadScratch<std::vector<std::size_t>*, SlotElementsSlotThreadStorage>();
    struct SlotSizesSlotThreadStorage {};
    auto& slotSizesSlot = AgcDriver::ThreadScratch<std::vector<std::size_t>*, SlotSizesSlotThreadStorage>();
    struct SlotSnapshotsSlotThreadStorage {};
    auto& slotSnapshotsSlot = AgcDriver::ThreadScratch<std::vector<std::size_t>*, SlotSnapshotsSlotThreadStorage>();
    auto& slotElements = ShaderRecompiler::ThreadOwned(slotElementsSlot);
    auto& slotSizes = ShaderRecompiler::ThreadOwned(slotSizesSlot);
    auto& slotSnapshots = ShaderRecompiler::ThreadOwned(slotSnapshotsSlot);
    slotElements.clear();
    slotSizes.clear();
    slotSnapshots.clear();
    // APS5_ARCHB_ZERO_COPY_STORAGE: a guest element stays as the template's set binds it (a read-only
    // element inside a host import is read in place there, its read noted by MarkGpuWrites); only
    // moved buffers (data words, or another address) still get a binding of their own.
    const bool zeroCopy = Recorder::ZeroCopyStorageActive();
    const bool zeroCopyChecks = Recorder::ZeroCopyChecks();
    std::size_t zeroCopySkipped = 0;
    struct ZeroCopyReport {
        const bool& on;
        const std::size_t& skipped;
        ~ZeroCopyReport() {
            if (on) Recorder::CountZeroCopyStorage(skipped);
        }
    } zeroCopyReport{zeroCopy, zeroCopySkipped};
    for (std::size_t index = 0; index < allocations.size(); ++index) {
        const auto& item = allocations[index];
        const auto checkStart = diag || movedCensusScope.counters != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto override = std::find_if(moved.begin(), moved.end(), [&](const MovedBuffer& entry) { return entry.allocation == index; });
        if (override != moved.end() && !override->words.empty()) {
            struct MovedTime {
                bool on;
                MovedCensusCounters* census;
                std::chrono::steady_clock::time_point start;
                ~MovedTime() {
                    if (on) bindingsDiag().movedNs.fetch_add(diagNs(start), std::memory_order_relaxed);
                    if (census != nullptr) census->wordsNs.fetch_add(diagNs(start), std::memory_order_relaxed);
                }
            } movedTime{diag, movedCensusScope.counters, checkStart};
            constexpr std::size_t MovedWordBufferSlots = 8;
            const bool reportUploadWords = AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixMovedWordUpload);
            const auto fill = [&](std::byte* bytes) {
                std::memcpy(bytes, override->words.data(), override->size);
                for (const auto& patch : dataPatches) {
                    if (patch.allocation == index && patch.byte < override->size) bytes[patch.byte] = static_cast<std::byte>(patch.adjustment);
                }
            };
            const bool cacheWords = reportUploadWords || slotted;
            const auto held = cacheWords ? std::find_if(movedWordBuffers.begin(), movedWordBuffers.end(), [&](const MovedWordBuffer& entry) { return entry.allocation == index && entry.size == override->size && entry.uploaded == reportUploadWords && std::ranges::equal(entry.words, override->words); }) : movedWordBuffers.end();
            std::shared_ptr<Buffer> buffer;
            VkDeviceSize wordOffset = 0;
            if (held != movedWordBuffers.end()) {
                buffer = held->buffer;
                wordOffset = held->offset;
                if (reportUploadWords) {
                    movedWordCounters().held.fetch_add(1, std::memory_order_relaxed);
                    if (movedWordUploadVerify()) {
                        struct ExpectedSlotStorage {};
                        auto& expected = AgcDriver::ThreadScratch<std::vector<std::byte>, ExpectedSlotStorage>();
                        expected.resize(override->size);
                        fill(expected.data());
                        auto& counters = movedWordCounters();
                        counters.verified.fetch_add(1, std::memory_order_relaxed);
                        if (std::memcmp(buffer->Bytes().data() + wordOffset, expected.data(), override->size) != 0) counters.mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } else {
                auto slice = recorder.DrawUpload(override->size);
                fill(slice.bytes);
                buffer = std::move(slice.buffer);
                wordOffset = slice.offset;
                if (reportUploadWords) {
                    movedWordCounters().uploaded.fetch_add(1, std::memory_order_relaxed);
                }
                if (cacheWords) {
                    MovedWordBuffer entry{index, {override->words.begin(), override->words.end()}, override->size, buffer, wordOffset, reportUploadWords};
                    if (movedWordBuffers.size() < MovedWordBufferSlots) movedWordBuffers.push_back(std::move(entry));
                    else movedWordBuffers[movedWordBufferNext++ % MovedWordBufferSlots] = std::move(entry);
                }
            }
            if (diag) bindingsDiag().movedWords.fetch_add(1, std::memory_order_relaxed);
            if (slotted) drawReuseCounters().sharedElements.fetch_add(1, std::memory_order_relaxed);
            selected.push_back(index);
            keep({0, std::move(buffer), wordOffset, override->size});
            continue;
        }
        if (zeroCopy && override == moved.end()) {
            ++zeroCopySkipped;
            continue;
        }
        std::uint64_t address = item.address;
        std::size_t size = item.size;
        struct CheckTime {
            bool on;
            std::chrono::steady_clock::time_point start;
            bool passed = false;
            ~CheckTime() {
                if (!on) return;
                auto& d = bindingsDiag();
                d.checkElements.fetch_add(1, std::memory_order_relaxed);
                if (passed) d.checkPassed.fetch_add(1, std::memory_order_relaxed);
                d.checkNs.fetch_add(diagNs(start), std::memory_order_relaxed);
            }
        };
        std::optional<CheckTime> checkTime;
        if (diag) checkTime.emplace(true, checkStart);
        if (override != moved.end()) {
            address = override->address;
            size = override->size;
        } else if (useCandidates) {
            bool pass = snapshotCandidates[index] != 0;
            if (verifyCandidates) {
                const bool checked = staticSnapshotCheck(index, reads);
                auto& counters = snapshotCandidateCounters();
                counters.verified.fetch_add(1, std::memory_order_relaxed);
                if (checked != pass) counters.mismatches.fetch_add(1, std::memory_order_relaxed);
                pass = checked;
            }
            if (movedOnly || !pass) {
                ++candidateSkips;
                continue;
            }
            if (recorder.PendingWriteOverlaps(item.address, item.size)) continue;
        } else {
            if (movedOnly || !item.guest || item.written || guestMemory.WritesOverlap(item.address, item.size)) continue;
            const bool direct = std::any_of(reads.begin(), reads.end(), [&](const auto& range) { return item.address >= range.first && item.address < range.second && item.size <= range.second - item.address; });
            if (!direct || recorder.PendingWriteOverlaps(item.address, item.size)) continue;
        }
        if (checkTime) {
            checkTime->passed = true;
            checkTime.reset();
        }
        const auto begin = address - item.adjustment;
        const auto bytes = static_cast<std::size_t>(GuestBufferMemory::ViewBytes(size, item.adjustment));
        if (override == moved.end() && (bytes > maxSnapshotBytes || totalSnapshotBytes + bytes > maxSnapshotBytes)) continue;
        if (slotted && bytes != 0 && bytes <= uploadLimit) {
            // Copied into the set's slot once the set is chosen (below).
            slotElements.push_back(index);
            slotSizes.push_back(bytes);
            slotSnapshots.push_back(result->snapshots.size());
            selected.push_back(index);
            keep({begin, nullptr, 0, bytes});
            totalSnapshotBytes += bytes;
            if (diag) {
                bindingsDiag().slotted.fetch_add(1, std::memory_order_relaxed);
                bindingsDiag().slottedBytes.fetch_add(bytes, std::memory_order_relaxed);
            }
            continue;
        }
        const auto unwrittenStart = diag ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto registryGeneration = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        if (!stamped) {
            stamp = {GuestMemory::TrackerGeneration(), GuestMemory::CollectEpoch(), registryGeneration};
            stamped = true;
        }
        VkDeviceSize offset = 0;
        auto buffer = recorder.UnwrittenDrawSnapshot(begin, bytes, stamp, &offset);
        if (diag) bindingsDiag().unwrittenNs.fetch_add(diagNs(unwrittenStart), std::memory_order_relaxed);
        static const bool verifySnapshots = std::getenv("APS5_VERIFY_DRAW_SNAPSHOTS") != nullptr;
        if (verifySnapshots && buffer != nullptr) {
            static std::atomic<std::uint64_t> checked{0}, differing{0};
            const auto fast = buffer->Bytes().subspan(static_cast<std::size_t>(offset), bytes);
            VkDeviceSize slowOffset = 0;
            const auto slow = recorder.ReusableDrawSnapshot(begin, bytes, Recorder::SnapshotUse::Storage, nullptr, GuestMemory::CollectWrites(begin, bytes), &slowOffset);
            const bool same = (slow == buffer && slowOffset == offset) || (slow != nullptr && slow->Bytes().size() >= slowOffset + fast.size() && std::memcmp(slow->Bytes().data() + slowOffset, fast.data(), fast.size()) == 0) || (slow == nullptr && std::memcmp(fast.data(), reinterpret_cast<const void*>(begin), bytes) == 0);
            if (!same) differing.fetch_add(1);
            if (checked.fetch_add(1) % 200000 == 199999) std::fprintf(stderr, "[draw-snapshots] verify: %llu fast answers checked, %llu differing%c", static_cast<unsigned long long>(checked.load()), static_cast<unsigned long long>(differing.load()), 10);
        }
        if (buffer == nullptr) {
            const bool uploaded = bytes != 0 && bytes <= uploadLimit;
            if (uploaded) {
                const auto uploadStart = diag ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                auto slice = recorder.DrawUpload(bytes);
                std::memcpy(slice.bytes, reinterpret_cast<const void*>(begin), bytes);
                buffer = std::move(slice.buffer);
                offset = slice.offset;
                if (diag) {
                    auto& d = bindingsDiag();
                    d.uploads.fetch_add(1, std::memory_order_relaxed);
                    d.uploadBytes.fetch_add(bytes, std::memory_order_relaxed);
                    d.uploadNs.fetch_add(diagNs(uploadStart), std::memory_order_relaxed);
                }
            } else if (Recorder::ArmedSnapshotsActive() && (buffer = recorder.ArmedDrawSnapshot(begin, bytes, Recorder::SnapshotUse::Storage, nullptr, &offset)) != nullptr) {
                if (diag) bindingsDiag().reused.fetch_add(1, std::memory_order_relaxed);
                if (Recorder::ArmedSnapshotsVerify()) {
                    VkDeviceSize slowOffset = 0;
                    const auto slow = recorder.ReusableDrawSnapshot(begin, bytes, Recorder::SnapshotUse::Storage, nullptr, GuestMemory::CollectWrites(begin, bytes), &slowOffset);
                    Recorder::NoteArmedSnapshotVerify(slow == buffer && slowOffset == offset);
                }
            } else {
                auto phaseStart = diag ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                const auto generation = GuestMemory::CollectWrites(begin, bytes);
                if (diag) {
                    bindingsDiag().collectNs.fetch_add(diagNs(phaseStart), std::memory_order_relaxed);
                    phaseStart = std::chrono::steady_clock::now();
                }
                buffer = recorder.ReusableDrawSnapshot(begin, bytes, Recorder::SnapshotUse::Storage, nullptr, generation, &offset);
                if (diag) {
                    bindingsDiag().reusableNs.fetch_add(diagNs(phaseStart), std::memory_order_relaxed);
                    if (buffer != nullptr) bindingsDiag().reused.fetch_add(1, std::memory_order_relaxed);
                    phaseStart = std::chrono::steady_clock::now();
                }
                if (buffer == nullptr) {
                    buffer = std::make_shared<Buffer>(context, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
                    std::memcpy(buffer->Bytes().data(), reinterpret_cast<const void*>(begin), bytes);
                    recorder.KeepDrawSnapshot(begin, bytes, generation, registryGeneration, buffer);
                    if (diag) {
                        auto& d = bindingsDiag();
                        d.created.fetch_add(1, std::memory_order_relaxed);
                        d.createdBytes.fetch_add(bytes, std::memory_order_relaxed);
                        d.createNs.fetch_add(diagNs(phaseStart), std::memory_order_relaxed);
                    }
                }
            }
            const auto noteStart = diag ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            if (GuestMemory::TrackerGeneration() != stamp[0]) stamped = false;
            else if (!uploaded || stamp[1] != 0) recorder.NoteUnwrittenDrawSnapshot(begin, bytes, stamp, buffer, offset);
            if (diag) bindingsDiag().noteNs.fetch_add(diagNs(noteStart), std::memory_order_relaxed);
        } else if (diag) {
            auto& d = bindingsDiag();
            if (bytes <= uploadLimit) {
                d.unwrittenSmall.fetch_add(1, std::memory_order_relaxed);
            } else {
                d.unwrittenLarge.fetch_add(1, std::memory_order_relaxed);
                d.unwrittenLargeBytes.fetch_add(bytes, std::memory_order_relaxed);
            }
        }
        if (diag && bytes > uploadLimit) {
            bindingsDiag().large.fetch_add(1, std::memory_order_relaxed);
            bindingsDiag().largeBytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        if (slotted) drawReuseCounters().sharedElements.fetch_add(1, std::memory_order_relaxed);
        if (zeroCopyChecks) recorder.NoteZeroCopyCheck(begin, bytes, buffer, offset, override != moved.end() ? Recorder::ZeroCopyKind::Override : bytes <= uploadLimit ? Recorder::ZeroCopyKind::StorageSmall : Recorder::ZeroCopyKind::StorageLarge);
        selected.push_back(index);
        keep({begin, std::move(buffer), offset, bytes});
        totalSnapshotBytes += bytes;
        CaptureTrace::Log("draw-snapshot batch=%llu address=%llx bytes=%zu", static_cast<unsigned long long>(recorder.Submissions() + 1), static_cast<unsigned long long>(begin), bytes);
    }
    if (diag) {
        auto& d = bindingsDiag();
        d.calls.fetch_add(1, std::memory_order_relaxed);
        d.elements.fetch_add(allocations.size(), std::memory_order_relaxed);
        d.selectNs.fetch_add(diagNs(diagStart), std::memory_order_relaxed);
        if (selected.empty()) d.empty.fetch_add(1, std::memory_order_relaxed);
    }
    if (selected.empty()) return {};
    movedCensusScope.set = true;
    result->captureDescriptors = captureDescriptors;
    writeDrawSets(*result, selected, faultRecord, &recorder, slotted, slotElements, slotSizes, slotSnapshots);
    recorder.Keep(result);
    for (const auto& snapshot : result->snapshots) recorder.KeepBytes(snapshot.buffer.get(), snapshot.buffer->Bytes().size());
    return result;
}

bool ShaderResources::RecordPrivateDescriptors(VkDescriptorSet destination, const std::function<void()>& record, HostImportsProof& proof) const {
    Require(completed && !splitSets && !updateAfterBind, "private compute descriptors require a completed fixed set");
    std::vector<std::array<std::uint64_t, 3>> imports;
    for (const auto& region : directRegions) {
        if ((region.serial >> 63u) == 0) imports.push_back({region.begin, region.end, region.serial});
    }
    return RecordWithHostImports(context, imports, [&] {
        auto writes = descriptorWrites;
        for (auto& write : writes) write.dstSet = destination;
        if (!writes.empty()) context.Resolved(&DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        record();
    }, proof);
}

void ShaderResources::writeDrawSets(DrawBindings& result, std::span<const std::size_t> selected, const BdaResources* faultRecord, Recorder* recorder, bool slotted, std::span<const std::size_t> slotElements, std::span<const std::size_t> slotSizes, std::span<const std::size_t> slotSnapshots) const {
    static const bool timeSets = std::getenv("APS5_PROFILE_DRAW") != nullptr || std::getenv("APS5_PIPELINE_STATS") != nullptr;
    struct SetTiming {
        std::chrono::steady_clock::time_point start;
        std::size_t mode = 0;
        ~SetTiming() {
            if (start == std::chrono::steady_clock::time_point{}) return;
            auto& counters = drawSetPoolCounters();
            counters.setCalls[mode].fetch_add(1, std::memory_order_relaxed);
            counters.setNs[mode].fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
        }
    } setTiming{timeSets ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
    Require(context.descriptorCache != nullptr, "draw snapshots require a descriptor cache");
    result.cache = context.descriptorCache;
    // APS5_ARCHB_FIX_DRAW_SET_POOL (sampled per call): a set back from an earlier draw of this object
    // keeps everything but the snapshot elements (see DrawSetPool).
    const bool pooled = recorder != nullptr && drawSetPool != nullptr && AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::FixDrawSetPool);
    setTiming.mode = pooled ? 1 : 0;
    DrawSetPool::Entry reused;
    bool slotMatched = false;
    const bool reuse = pooled && (slotted ? drawSetPool->TakeMatching(reused, slotElements, slotSizes, selected, result.snapshots, slotMatched) : drawSetPool->Take(reused));
    const bool verifyReuse = slotted && drawReuseVerify();
    if (slotted) {
        auto& counters = drawReuseCounters();
        counters.draws.fetch_add(1, std::memory_order_relaxed);
        if (reuse) {
            counters.pooled.fetch_add(1, std::memory_order_relaxed);
            if (verifyReuse) {
                // The pool only holds sets whose draw was released (its batch completed).
                counters.verified.fetch_add(1, std::memory_order_relaxed);
                if (reused.serial > recorder->CompletedThrough()) counters.mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!slotElements.empty()) {
            // The slots: the matched set's own buffer, or a new one laid out for this selection.
            std::shared_ptr<Buffer> slotBuffer;
            std::vector<VkDeviceSize> slotOffsets;
            if (slotMatched) {
                slotBuffer = std::move(reused.slotBuffer);
                slotOffsets = std::move(reused.slotOffsets);
                counters.slotHits.fetch_add(1, std::memory_order_relaxed);
            } else {
                const VkDeviceSize alignment = std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 16);
                VkDeviceSize used = 0;
                slotOffsets.reserve(slotSizes.size());
                for (const auto bytes : slotSizes) {
                    used = (used + alignment - 1) & ~(alignment - 1);
                    slotOffsets.push_back(used);
                    used += bytes;
                }
                slotBuffer = std::make_shared<Buffer>(context, static_cast<std::size_t>(used), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
                counters.slotRewrites.fetch_add(1, std::memory_order_relaxed);
            }
            std::size_t copied = 0;
            for (std::size_t slot = 0; slot < slotSnapshots.size(); ++slot) {
                auto& snapshot = result.snapshots[slotSnapshots[slot]];
                std::memcpy(slotBuffer->Bytes().data() + slotOffsets[slot], reinterpret_cast<const void*>(snapshot.address), snapshot.bytes);
                snapshot.buffer = slotBuffer;
                snapshot.offset = slotOffsets[slot];
                copied += snapshot.bytes;
                CaptureTrace::Log("draw-snapshot batch=%llu address=%llx bytes=%zu", static_cast<unsigned long long>(recorder->Submissions() + 1), static_cast<unsigned long long>(snapshot.address), snapshot.bytes);
            }
            counters.slotBytes.fetch_add(copied, std::memory_order_relaxed);
            result.slotBuffer = std::move(slotBuffer);
            result.slotElements.assign(slotElements.begin(), slotElements.end());
            result.slotOffsets = std::move(slotOffsets);
            result.slotSizes.assign(slotSizes.begin(), slotSizes.end());
        }
        result.serial = recorder->OpenSerial();
        result.held.reserve(result.snapshots.size());
        for (const auto& snapshot : result.snapshots) result.held.push_back({snapshot.buffer, snapshot.offset, snapshot.bytes});
    }
    // The sets go to the pool only once written (a set is pooled only holding this object's writes
    // and the snapshot elements `overridden` names); until then a failure releases them as before.
    const auto adopt = [&] {
        if (!pooled) return;
        result.overridden.assign(selected.begin(), selected.end());
        result.pool = drawSetPool;
    };
    if (reuse) {
        result.allocation = reused.allocation;
        result.fragmentAllocation = reused.fragmentAllocation;
        if (result.allocation.set != VK_NULL_HANDLE) result.layout = _layout;
        if (result.fragmentAllocation.set != VK_NULL_HANDLE) result.fragmentLayout = fragment.layout;
    } else {
    std::array<bool, 2> described{};
    struct CountsSlotStorage {};
    auto& counts = AgcDriver::ThreadScratch<std::array<std::vector<VkDescriptorPoolSize>, 2>, CountsSlotStorage>();
    for (auto& sizes : counts) sizes.clear();
    for (const auto& binding : bindings) {
        const auto set = setOf(binding);
        described[set] = true;
        const auto count = (binding.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) != 0 ? std::max(1u, static_cast<std::uint32_t>(binding.imageAllocations.size())) : binding.layout.descriptorCount;
        if (count == 0) continue;
        auto& sizes = counts[set];
        const auto held = std::find_if(sizes.begin(), sizes.end(), [&](const VkDescriptorPoolSize& entry) { return entry.type == binding.layout.descriptorType; });
        if (held != sizes.end()) held->descriptorCount += count;
        else sizes.push_back({binding.layout.descriptorType, count});
    }
    for (auto& sizes : counts) std::sort(sizes.begin(), sizes.end(), [](const VkDescriptorPoolSize& a, const VkDescriptorPoolSize& b) { return a.type < b.type; });
    const auto sizesOf = [&](std::size_t set) { return std::span<const VkDescriptorPoolSize>(counts[set]); };
    const auto variableOf = [&](std::size_t set) { return tableBindings[set] != std::numeric_limits<std::size_t>::max() ? std::optional<std::uint32_t>(std::max(1u, static_cast<std::uint32_t>(bindings[tableBindings[set]].imageAllocations.size()))) : std::nullopt; };
    if (described[0] || splitSets) {
        result.allocation = result.cache->AllocateRewritten(_layout, sizesOf(0), updateAfterBind, variableOf(0));
        result.layout = _layout;
        Require(result.allocation.set != VK_NULL_HANDLE, "draw snapshot descriptor allocation failed");
    }
    if (described[1] || splitSets) {
        result.fragmentAllocation = result.cache->AllocateRewritten(fragment.layout, sizesOf(1), updateAfterBind, variableOf(1));
        result.fragmentLayout = fragment.layout;
        Require(result.fragmentAllocation.set != VK_NULL_HANDLE, "draw snapshot descriptor allocation failed");
    }
    }
    struct BuffersSlotStorage {};
    auto& buffers = AgcDriver::ThreadScratch<std::vector<VkDescriptorBufferInfo>, BuffersSlotStorage>();
    buffers = descriptorBuffers;
    if (faultRecord != nullptr) {
        for (const auto& fault : faultDescriptors) buffers[fault.buffer] = faultRecord->Fault();
    }
    struct WritesSlotStorage {};
    auto& writes = AgcDriver::ThreadScratch<std::vector<VkWriteDescriptorSet>, WritesSlotStorage>();
    struct IndicesSlotStorage {};
    auto& snapshotIndices = AgcDriver::ThreadScratch<std::vector<std::size_t>, IndicesSlotStorage>();
    snapshotIndices.assign(allocations.size(), selected.size());
    for (std::size_t index = 0; index < selected.size(); ++index) snapshotIndices[selected[index]] = index;
    if (reuse) {
        // The elements this draw or the set's previous draw points at a snapshot get this draw's
        // snapshot, or the object's own descriptor where only the previous draw had one; every other
        // element still holds the object's own descriptor from the set's full write.
        struct MarksSlotStorage {};
        auto& marks = AgcDriver::ThreadScratch<std::vector<std::uint8_t>, MarksSlotStorage>();
        marks.assign(allocations.size(), 0);
        for (const auto index : selected) marks[index] = 1;
        for (const auto index : reused.overridden) {
            if (index < marks.size()) marks[index] = 1;
        }
        // APS5_ARCHB_PIPE_DRAW_REUSE: an element the set's previous draw already pointed at this draw's
        // snapshot buffer (the same live object: an expired one never matches), offset and range keeps
        // its descriptor. With _VERIFY such elements are written anyway and checked.
        const bool cached = slotted && reused.held.size() == reused.overridden.size();
        std::size_t skipped = 0;
        if (cached) {
            struct HeldAtSlotStorage {};
            auto& heldAt = AgcDriver::ThreadScratch<std::vector<std::size_t>, HeldAtSlotStorage>();
            heldAt.assign(allocations.size(), reused.held.size());
            for (std::size_t position = 0; position < reused.overridden.size(); ++position) {
                if (reused.overridden[position] < heldAt.size()) heldAt[reused.overridden[position]] = position;
            }
            for (std::size_t snapshotIndex = 0; snapshotIndex < selected.size(); ++snapshotIndex) {
                const auto index = selected[snapshotIndex];
                const auto position = heldAt[index];
                if (position == reused.held.size()) continue;
                const auto& held = reused.held[position];
                const auto& snapshot = result.snapshots[snapshotIndex];
                const bool sameObject = !held.buffer.owner_before(snapshot.buffer) && !snapshot.buffer.owner_before(held.buffer) && !held.buffer.expired();
                if (!sameObject || held.offset != snapshot.offset || held.range != snapshot.bytes) continue;
                ++skipped;
                if (!verifyReuse) {
                    marks[index] = 0;
                    continue;
                }
                auto& counters = drawReuseCounters();
                counters.verified.fetch_add(1, std::memory_order_relaxed);
                const auto live = held.buffer.lock();
                if (live == nullptr || live->Handle() != snapshot.buffer->Handle()) counters.mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        }
        for (std::size_t index = 0; index < allocations.size(); ++index) {
            if (allocations[index].role == ShaderRecompiler::DescriptorRole::FaultBuffer) marks[index] = 1;
        }
        writes.clear();
        std::size_t full = 0;
        std::size_t written = 0;
        auto nextTemplate = descriptorWrites.begin();
        for (const auto& binding : bindings) {
            const auto original = setOf(binding) == 0 ? _set : fragment.set;
            const auto destination = setOf(binding) == 0 ? result.allocation.set : result.fragmentAllocation.set;
            while (nextTemplate != descriptorWrites.end() && nextTemplate->dstBinding == binding.layout.binding && nextTemplate->dstSet == original) {
                const auto& write = *nextTemplate++;
                full += write.descriptorCount;
                if (binding.allocations.empty()) continue;
                const auto offset = static_cast<std::size_t>(write.pBufferInfo - descriptorBuffers.data());
                for (std::size_t element = 0; element < binding.allocations.size(); ++element) {
                    const auto snapshotIndex = snapshotIndices[binding.allocations[element]];
                    if (snapshotIndex == selected.size()) continue;
                    const auto& snapshot = result.snapshots[snapshotIndex];
                    buffers[offset + element] = wholeDwordRange({snapshot.buffer->Handle(), snapshot.offset, snapshot.bytes});
                }
                for (std::size_t element = 0; element < binding.allocations.size();) {
                    if (marks[binding.allocations[element]] == 0) {
                        ++element;
                        continue;
                    }
                    auto last = element;
                    while (last < binding.allocations.size() && marks[binding.allocations[last]] != 0) ++last;
                    auto run = write;
                    run.dstSet = destination;
                    run.dstArrayElement = write.dstArrayElement + static_cast<std::uint32_t>(element);
                    run.descriptorCount = static_cast<std::uint32_t>(last - element);
                    run.pBufferInfo = buffers.data() + offset + element;
                    writes.push_back(run);
                    written += last - element;
                    element = last;
                }
            }
        }
        Require(nextTemplate == descriptorWrites.end(), "draw snapshot writes disagree with their descriptor bindings");
        auto& counters = drawSetPoolCounters();
        counters.hits.fetch_add(1, std::memory_order_relaxed);
        counters.fullElements.fetch_add(full, std::memory_order_relaxed);
        counters.writtenElements.fetch_add(written, std::memory_order_relaxed);
        if (slotted) {
            auto& reuseCounters = drawReuseCounters();
            reuseCounters.skippedElements.fetch_add(skipped, std::memory_order_relaxed);
            reuseCounters.writtenElements.fetch_add(written, std::memory_order_relaxed);
            if (writes.empty()) reuseCounters.noWrite.fetch_add(1, std::memory_order_relaxed);
        }
        if (!writes.empty()) context.Resolved(&DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        if (result.captureDescriptors) result.capturedDescriptors = buffers;
        adopt();
        return;
    }
    writes = descriptorWrites;
    if (pooled) {
        std::size_t full = 0;
        for (const auto& write : writes) full += write.descriptorCount;
        auto& counters = drawSetPoolCounters();
        counters.misses.fetch_add(1, std::memory_order_relaxed);
        counters.writtenElements.fetch_add(full, std::memory_order_relaxed);
    }
    auto nextWrite = writes.begin();
    for (const auto& binding : bindings) {
        const auto original = setOf(binding) == 0 ? _set : fragment.set;
        while (nextWrite != writes.end() && nextWrite->dstBinding == binding.layout.binding && nextWrite->dstSet == original) {
            auto& write = *nextWrite++;
            write.dstSet = setOf(binding) == 0 ? result.allocation.set : result.fragmentAllocation.set;
            if (binding.allocations.empty()) continue;
            const auto offset = static_cast<std::size_t>(write.pBufferInfo - descriptorBuffers.data());
            write.pBufferInfo = buffers.data() + offset;
            for (std::size_t element = 0; element < binding.allocations.size(); ++element) {
                const auto snapshotIndex = snapshotIndices[binding.allocations[element]];
                if (snapshotIndex == selected.size()) continue;
                const auto& snapshot = result.snapshots[snapshotIndex];
                buffers[offset + element] = wholeDwordRange({snapshot.buffer->Handle(), snapshot.offset, snapshot.bytes});
            }
        }
    }
    Require(nextWrite == writes.end(), "draw snapshot writes disagree with their descriptor bindings");
    context.Resolved(&DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    if (result.captureDescriptors) result.capturedDescriptors = buffers;
    adopt();
}

ShaderResources::DrawUse::~DrawUse() {
    if (ownRecord != nullptr) ownRecord->store(false, std::memory_order_release);
}

std::shared_ptr<ShaderResources::DrawUse> ShaderResources::BeginDrawUse(Recorder& recorder) const {
    if (!spaceReuse || bda == nullptr || !faultChecks || faultDescriptors.empty()) return {};
    auto use = std::make_shared<DrawUse>();
    if (!ownRecordBusy->exchange(true, std::memory_order_acq_rel)) {
        use->ownRecord = ownRecordBusy;
        return use;
    }
    use->record = std::make_shared<BdaResources>(context, faultReportOnly);
    use->record->NoteUse(recorder.Device() == context.device ? recorder.OpenSerial() : std::numeric_limits<std::uint64_t>::max());
    use->bindings = std::make_shared<DrawBindings>();
    writeDrawSets(*use->bindings, {}, use->record.get());
    recorder.Keep(use->bindings);
    return use;
}

void ShaderResources::CompleteDrawUse(const DrawUse& use) const {
    if (bda != nullptr) bda->CheckUse(use.record.get());
}

void ShaderResources::BindDrawUse(VkCommandBuffer commands, VkPipelineLayout layout, const DrawUse* use) const {
    if (use == nullptr || use->bindings == nullptr) {
        Bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout);
        return;
    }
    requireGuestBuffers();
    const auto set = use->bindings->allocation.set;
    const auto fragmentSet = use->bindings->fragmentAllocation.set;
    if (set != VK_NULL_HANDLE) context.Resolved(&DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    if (fragmentSet != VK_NULL_HANDLE) context.Resolved(&DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, ShaderRecompiler::RuntimeAbi::FragmentDescriptorSet, 1, &fragmentSet, 0, nullptr);
}

void ShaderResources::prepareFaultSlot() const {
    if (bda == nullptr) return;
    const auto* recorder = Recorder::Active();
    bda->NoteUse(recorder != nullptr && recorder->Device() == context.device ? recorder->OpenSerial() : std::numeric_limits<std::uint64_t>::max());
    if (!bda->TakeFaultSlot(context)) return;
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(faultDescriptors.size());
    for (const auto& fault : faultDescriptors) {
        descriptorBuffers[fault.buffer] = bda->Fault();
        auto write = descriptorWrites[fault.write];
        write.dstArrayElement = fault.element;
        write.descriptorCount = 1;
        write.pBufferInfo = &descriptorBuffers[fault.buffer];
        writes.push_back(write);
    }
    if (!writes.empty()) context.Resolved(&DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void ShaderResources::requireGuestBuffers(std::span<const MovedBuffer> moved) const {
    for (std::size_t index = 0; index < allocations.size(); ++index) {
        const auto& item = allocations[index];
        if (!item.guest) continue;
        const auto override = std::find_if(moved.begin(), moved.end(), [&](const MovedBuffer& entry) { return entry.allocation == index; });
        const auto address = override != moved.end() ? override->address : item.address;
        const auto bytes = override != moved.end() ? override->size : item.size;
        RequireNoResidentDepthRead_nid_no_patch(address, bytes, htileClear);
        RequireNoMultisampleColorTarget(address, bytes, "a shader buffer access");
    }
}

void ShaderResources::Bind(VkCommandBuffer commands, VkPipelineBindPoint bindPoint, VkPipelineLayout layout) const {
    PerformanceTimer recorderTiming("Resources.Bind", AgcDriver::ArchB::Recorder::Phase::Bind);
    requireGuestBuffers();
    prepareFaultSlot();
    if (_set != VK_NULL_HANDLE) context.Resolved(&DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, bindPoint, layout, 0, 1, &_set, 0, nullptr);
    if (fragment.set != VK_NULL_HANDLE) context.Resolved(&DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, bindPoint, layout, ShaderRecompiler::RuntimeAbi::FragmentDescriptorSet, 1, &fragment.set, 0, nullptr);
}

namespace {
// Debug aid: APS5_GPU_NO_WRITEBACK=1 keeps GPU results out of guest memory.
bool SkipWriteBack() {
    static const bool skip = std::getenv("APS5_GPU_NO_WRITEBACK") != nullptr;
    return skip;
}
}

void ShaderResources::WriteBack() {
    std::exception_ptr unaligned;
    try {
        WriteBackBuffers();
    } catch (const BdaUnalignedAccess&) {
        unaligned = std::current_exception();
    }
    if (!guestMemory.Writes().empty() || std::find(storageWritten.begin(), storageWritten.end(), true) != storageWritten.end() || (usesBda && !bdaReadOnly)) GuestMemory::BreakProofs();
    if (!SkipWriteBack()) {
        for (std::size_t index = 0; index < storageTextures.size(); ++index) {
            if (!storageWritten[index] || storageTextures[index] == nullptr) continue;
            storageTextures[index]->MarkDirty();
            static_cast<void>(DepthSurfaceStoreBack(context, *storageTextures[index]));
        }
    }
    if (unaligned) std::rethrow_exception(unaligned);
}

void ShaderResources::MarkGpuWrites(Recorder& recorder) {
    if (bda != nullptr && recorder.Device() == context.device) bda->NoteUse(recorder.OpenSerial());
    // The ranges this use reads in place through their host imports (read-only and written elements
    // alike, and an address-based build's whole leased heaps), before the writes: a CPU store into
    // one of them (the copy HLE) must not land before the recorded work read it.
    static const bool readOnlyLeaseSkip = std::getenv("APS5_NO_READ_ONLY_LEASE_SKIP") == nullptr;
    const auto kind = !guestMemory.HoldsLease() ? Recorder::ReadKind::DispatchElement : readOnlyLeaseSkip && usesBda && bdaReadOnly ? Recorder::ReadKind::AddressBasedReadOnly : Recorder::ReadKind::AddressBased;
    const bool mayStore = !BdaReadOnly();
    guestMemory.NoteStagingAccesses(recorder, [&] {
        if (auto split = guestMemory.SplitInPlaceReads()) {
            recorder.NotePendingReadList(std::move(split->space), kind, mayStore);
            recorder.NotePendingReads(split->own, kind, mayStore);
        } else if (AgcDriver::ArchB::AllocChurnActive()) {
            // APS5_ARCHB_FIX_ALLOC_CHURN: the same ranges in a per-thread scratch (NotePendingReads copies them).
            struct ReadsSlotStorage {};
            auto& reads = AgcDriver::ThreadScratch<std::vector<std::pair<std::uint64_t, std::uint64_t>>, ReadsSlotStorage>();
            guestMemory.InPlaceReads(reads);
            recorder.NotePendingReads(reads, kind, mayStore);
        } else {
            recorder.NotePendingReads(guestMemory.InPlaceReads(), kind, mayStore);
        }
    });
    if (GpuAuthorityCensus::Enabled()) for (const auto& [begin, end] : guestMemory.Writes()) GpuAuthorityCensus::Produced(begin, end);
    const auto inRun = std::exchange(afterRunSelf, nullptr);
    if (SkipWriteBack()) return;
    for (std::size_t index = 0; index < storageTextures.size(); ++index) {
        if (!storageWritten[index] || storageTextures[index] == nullptr) continue;
        storageTextures[index]->MarkDirty();
        static_cast<void>(DepthSurfaceStoreBack(context, *storageTextures[index]));
    }
    // Written sub-ranges of buffers the GPU copied out of a host import go back into it by the GPU,
    // recorded here after the work: those regions then need no CPU write-back (HasCopiedWrites),
    // and the note and mark below cover them like direct writes.
    const ArchC::LazyCensus::UseScope censusScope;
    if (inRun != nullptr && recorder.RunOpen() && guestMemory.HasGpuCopies()) {
        recorder.AfterRun([inRun](Recorder& after) {
            const ArchC::LazyCensus::UseScope deferredCensusScope;
            inRun->guestMemory.RecordCopyBacks(after);
            inRun->guestMemory.NoteStagingCarryPoints(after);
            inRun->guestMemory.NoteStagingCarryTokens(after);
        });
        guestMemory.MarkStagedWrites();
    } else {
        guestMemory.RecordCopyBacks(recorder);
    }
    // Only the written elements' ranges (AddWritable): a read-only element is neither noted here
    // nor marked as a direct write, so CPU reads of its memory never wait for this work.
    guestMemory.NoteStagingWrites(recorder);
    guestMemory.NoteStagingCarryPoints(recorder);
    guestMemory.MarkDirectWrites();
    guestMemory.NoteStagingCarryTokens(recorder);
    if (!BuildProfiled()) return;
    auto& counters = BufferWrites();
    // Recorded uses only (dispatches and recorded draws): a synchronous draw writes back in
    // WriteBack() and publishes no pending writes, so it has no notes to skip.
    counters.notesSkipped.fetch_add(readOnlyBuffers, std::memory_order_relaxed);
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = counters.lastReport.load();
    if (nowMs - last < 10000 || !counters.lastReport.compare_exchange_strong(last, nowMs)) return;
    const auto elements = counters.elements.load();
    const auto readOnly = counters.readOnly.load();
    AgcDriver::ProfilePrint_nid_no_patch("[buffers] descriptor elements bound (cumulative): %llu total, %llu written, %llu read-only; %llu pending-write notes skipped\n", static_cast<unsigned long long>(elements), static_cast<unsigned long long>(elements - readOnly), static_cast<unsigned long long>(readOnly), static_cast<unsigned long long>(counters.notesSkipped.load()));
}

void ShaderResources::CheckFaults() const {
    if (bda) bda->CheckFault();
}

void ShaderResources::WriteBackBuffers() {
    std::exception_ptr unaligned;
    if (bda) {
        try {
            bda->CheckFault();
        } catch (const BdaUnalignedAccess&) {
            unaligned = std::current_exception();
        }
    }
    if (!SkipWriteBack()) guestMemory.WriteBack();
    if (unaligned) std::rethrow_exception(unaligned);
}

std::span<const std::byte> ShaderResources::AddressTableBytes() const {
    return bda != nullptr ? bda->TableBytes() : std::span<const std::byte>{};
}

std::shared_ptr<const void> ShaderResources::pinDispatchFaultSlot(const std::shared_ptr<ShaderResources>& resources) {
    if (resources->bda == nullptr) return {};
    resources->bda->PinFaultSlot();
    return std::shared_ptr<const void>(resources.get(), [resources](const void*) { resources->bda->UnpinFaultSlot(); });
}

const char* ShaderResources::CompareDispatchSpace(const ShaderResources& fresh) const {
    if (!spaceReuse || !fresh.guestMemory.SpaceOnly() || !fresh.BdaReadOnly() || fresh.HasCopiedWrites()) return "eligibility";
    if (context.device != fresh.context.device || layoutKey != fresh.layoutKey || fragmentLayoutKey != fresh.fragmentLayoutKey || layoutBindingFlags != fresh.layoutBindingFlags || fragmentBindingFlags != fresh.fragmentBindingFlags) return "layouts";
    if (!std::ranges::equal(AddressTableBytes(), fresh.AddressTableBytes())) return "address table";
    if (GpuWrites() != fresh.GpuWrites() || WriteOnlyRanges() != fresh.WriteOnlyRanges() || InPlaceReads() != fresh.InPlaceReads() || DeviceReads() != fresh.DeviceReads()) return "memory ranges";
    if (SampledTextures() != fresh.SampledTextures() || StorageImages() != fresh.StorageImages() || storageWritten != fresh.storageWritten || storageMips != fresh.storageMips || storageFirstLayer != fresh.storageFirstLayer || storageAtomic != fresh.storageAtomic || storageAtomic64 != fresh.storageAtomic64 || textureFirstLayer != fresh.textureFirstLayer || textureBaseLevel != fresh.textureBaseLevel || textureSrgb != fresh.textureSrgb) return "images";
    if (tableElements != fresh.tableElements || bindings.size() != fresh.bindings.size() || allocations.size() != fresh.allocations.size() || descriptorBuffers.size() != fresh.descriptorBuffers.size() || descriptorImages.size() != fresh.descriptorImages.size()) return "binding counts";
    for (std::size_t i = 0; i < descriptorImages.size(); ++i) {
        const auto& a = descriptorImages[i];
        const auto& b = fresh.descriptorImages[i];
        if (a.sampler != b.sampler || a.imageView != b.imageView || a.imageLayout != b.imageLayout) return "image descriptors";
    }
    for (std::size_t i = 0; i < allocations.size(); ++i) {
        const auto& a = allocations[i];
        const auto& b = fresh.allocations[i];
        if (a.address != b.address || a.size != b.size || a.guest != b.guest || a.role != b.role || a.written != b.written || a.writeOnly != b.writeOnly || a.adjustment != b.adjustment || a.pushByte != b.pushByte || a.dataAllocation != b.dataAllocation || a.dataByte != b.dataByte || a.oversized != b.oversized || (a.buffer == nullptr) != (b.buffer == nullptr)) return "allocations";
        if (a.buffer != nullptr && (!a.buffer->Mapped() || !b.buffer->Mapped() || !std::ranges::equal(a.buffer->Bytes(), b.buffer->Bytes()))) return "data bytes";
    }
    if (descriptorWrites.size() != fresh.descriptorWrites.size()) return "descriptor write count";
    for (std::size_t i = 0; i < descriptorWrites.size(); ++i) {
        const auto& a = descriptorWrites[i];
        const auto& b = fresh.descriptorWrites[i];
        if (a.dstBinding != b.dstBinding || a.dstArrayElement != b.dstArrayElement || a.descriptorCount != b.descriptorCount || a.descriptorType != b.descriptorType || (a.dstSet == _set) != (b.dstSet == fresh._set) || (a.pBufferInfo == nullptr) != (b.pBufferInfo == nullptr) || (a.pImageInfo == nullptr) != (b.pImageInfo == nullptr)) return "descriptor write coverage";
        if (a.pBufferInfo != nullptr && a.pBufferInfo - descriptorBuffers.data() != b.pBufferInfo - fresh.descriptorBuffers.data()) return "descriptor buffer indices";
        if (a.pImageInfo != nullptr && a.pImageInfo - descriptorImages.data() != b.pImageInfo - fresh.descriptorImages.data()) return "descriptor image indices";
        if (a.pNext != nullptr || b.pNext != nullptr || a.pTexelBufferView != nullptr || b.pTexelBufferView != nullptr) return "unverified descriptor extension";
    }
    std::size_t buffer = 0;
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const auto& a = bindings[i];
        const auto& b = fresh.bindings[i];
        if (a.layout.binding != b.layout.binding || a.layout.descriptorType != b.layout.descriptorType || a.layout.descriptorCount != b.layout.descriptorCount || a.layout.stageFlags != b.layout.stageFlags || a.flags != b.flags || a.set != b.set || a.allocations != b.allocations || a.imageAllocations != b.imageAllocations) return "bindings";
        for (const auto allocation : a.allocations) {
            if (buffer >= descriptorBuffers.size()) return "buffer descriptor coverage";
            const auto& x = descriptorBuffers[buffer];
            const auto& y = fresh.descriptorBuffers[buffer++];
            const auto& item = allocations[allocation];
            if (x.range != y.range) return "buffer ranges";
            if (item.guest || item.role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
                if (x.buffer != y.buffer || x.offset != y.offset) return "guest buffer mappings";
            } else if (item.buffer != nullptr) {
                if (x.buffer != item.buffer->Handle() || y.buffer != fresh.allocations[allocation].buffer->Handle() || x.offset != y.offset) return "data buffer mappings";
            } else if (item.role == ShaderRecompiler::DescriptorRole::BdaPagetable) {
                const auto xTable = bda->Table();
                const auto yTable = fresh.bda->Table();
                if (x.buffer != xTable.buffer || x.offset != xTable.offset || y.buffer != yTable.buffer || y.offset != yTable.offset) return "table descriptor";
            } else if (item.role == ShaderRecompiler::DescriptorRole::FaultBuffer) {
                const auto xFault = bda->Fault();
                const auto yFault = fresh.bda->Fault();
                if (x.buffer != xFault.buffer || x.offset != xFault.offset || y.buffer != yFault.buffer || y.offset != yFault.offset) return "fault descriptor";
            } else return "unverified buffer role";
        }
    }
    return buffer == descriptorBuffers.size() ? nullptr : "buffer descriptor coverage";
}

bool ShaderResources::AddressTableGuardCurrent() {
    if (bda == nullptr) return true;
    if (const auto cached = guestMemory.CachedAddressTable(); cached.has_value()) return bda->TableGuardCurrent(context.device, *cached->ranges);
    return bda->TableGuardCurrent(context.device, guestMemory.AddressRanges());
}

bool ShaderResources::ReadsOverlap(std::uint64_t address, std::size_t bytes) const {
    static const bool walk = std::getenv("APS5_NO_READS_OVERLAP_WALK") == nullptr;
    if (walk) return guestMemory.InPlaceReadsOverlap(address, bytes);
    const auto overlaps = [&](const std::vector<std::pair<std::uint64_t, std::uint64_t>>& reads) {
        return std::any_of(reads.begin(), reads.end(), [&](const auto& range) { return address < range.second && range.first < address + bytes; });
    };
    if (AgcDriver::ArchB::AllocChurnActive()) {
        // APS5_ARCHB_FIX_ALLOC_CHURN: the same ranges in a per-thread scratch.
        struct ReadsSlotStorage {};
        auto& reads = AgcDriver::ThreadScratch<std::vector<std::pair<std::uint64_t, std::uint64_t>>, ReadsSlotStorage>();
        guestMemory.InPlaceReads(reads);
        return overlaps(reads);
    }
    return overlaps(guestMemory.InPlaceReads());
}

void ShaderResources::ComputeChainRanges(std::vector<std::pair<std::uint64_t, std::uint64_t>>& reads, std::vector<std::pair<std::uint64_t, std::uint64_t>>& writes) const {
    guestMemory.DeviceReads(reads);
    if (AgcDriver::ArchB::AllocChurnActive()) {
        // APS5_ARCHB_FIX_ALLOC_CHURN: the same ranges through a per-thread scratch.
        struct InPlaceSlotStorage {};
        auto& inPlace = AgcDriver::ThreadScratch<std::vector<std::pair<std::uint64_t, std::uint64_t>>, InPlaceSlotStorage>();
        guestMemory.InPlaceReads(inPlace);
        reads.insert(reads.end(), inPlace.begin(), inPlace.end());
    } else {
        const auto inPlace = guestMemory.InPlaceReads();
        reads.insert(reads.end(), inPlace.begin(), inPlace.end());
    }
    writes = guestMemory.Writes();
    const auto noteImage = [&](const StorageTexture& image, bool written) {
        const auto begin = image.Descriptor().baseAddress;
        Require(image.GuestBytes() <= std::numeric_limits<std::uint64_t>::max() - begin, "compute image guest range overflows");
        const auto end = begin + image.GuestBytes();
        reads.emplace_back(begin, end);
        if (written) writes.emplace_back(begin, end);
    };
    for (std::size_t index = 0; index < storageTextures.size(); ++index) {
        if (storageTextures[index] != nullptr) noteImage(*storageTextures[index], storageWritten[index]);
    }
    for (const auto& texture : textures) {
        if (texture != nullptr && texture->StorageSource() != nullptr) noteImage(*texture->StorageSource(), false);
    }
}

void ShaderResources::RunAccesses(std::vector<std::array<std::uint64_t, 3>>& out) const {
    out.clear();
    guestMemory.RunAccesses(out);
    if (usesBda) out.push_back({0, std::numeric_limits<std::uint64_t>::max(), Recorder::RunAccessWritten});
    for (const auto& [image, written] : StorageImages()) {
        const auto handle = reinterpret_cast<std::uint64_t>(image);
        out.push_back({handle, handle + 1, Recorder::RunAccessImage | (written ? Recorder::RunAccessWritten : 0u)});
    }
}

std::vector<std::pair<VkImage, bool>> ShaderResources::StorageImages() const {
    std::vector<std::pair<VkImage, bool>> images;
    for (std::size_t index = 0; index < storageTextures.size(); ++index) {
        if (storageTextures[index] != nullptr) images.emplace_back(storageTextures[index]->Image(), storageWritten[index]);
    }
    for (const auto& texture : textures) {
        if (texture != nullptr && texture->StorageSource() != nullptr) images.emplace_back(texture->StorageSource()->Image(), false);
    }
    return images;
}

bool ShaderResources::HasCachedStorageImages() const {
    if (std::any_of(storageTextures.begin(), storageTextures.end(), [](const auto& image) { return image != nullptr && !image->Cached(); })) return false;
    return std::none_of(textures.begin(), textures.end(), [](const auto& texture) { return texture != nullptr && texture->StorageSource() != nullptr && !texture->StorageSource()->Cached(); });
}

bool ShaderResources::HoldsReleasedTexture() const {
    return std::any_of(textures.begin(), textures.end(), [](const auto& texture) { return texture != nullptr && texture->Released(); });
}

bool ShaderResources::ReadsImage(const StorageTexture* image) const {
    if (image == nullptr) return false;
    for (const auto& texture : textures) {
        if (texture != nullptr && texture->StorageSource() == image) return true;
    }
    return std::any_of(storageTextures.begin(), storageTextures.end(), [&](const auto& storage) { return storage.get() == image; });
}

}
