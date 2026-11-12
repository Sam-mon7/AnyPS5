#include "prx/libSceAgcDriver/Graphics/include/ImageEpochs.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Graphics::ArchC {

namespace {

constexpr unsigned BlockShift = 16;
constexpr std::size_t FilterBits = std::size_t{1} << 20;
constexpr std::size_t DrainBatch = 512;

struct Slot {
    const void* image;
    std::atomic<std::uint64_t>* epoch;
    std::uint64_t begin;
    std::uint64_t end;
    std::uint32_t sources;
};

struct Record {
    std::vector<EpochRange> ranges;
    EpochCensus census;
};

constexpr std::array<const char*, static_cast<std::size_t>(EpochSource::Count)> SourceNames{"cpu_store", "hold", "open", "forget", "tracker_stamp", "gpu_note", "pending", "depth", "unwatch", "pending_insert"};

struct Counters {
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(EpochSource::Count)> invalidations{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(EpochSource::Count)> bumps{};
    std::atomic<std::uint64_t> drainEntries{0};
    std::atomic<std::uint64_t> drainNs{0};
    std::atomic<std::uint64_t> logLost{0};
    std::atomic<std::uint64_t> registered{0};
    std::atomic<std::uint64_t> dropped{0};
    Counters() {
        for (std::size_t i = 0; i < SourceNames.size(); ++i) {
            AgcDriver::ArchB::Recorder::RegisterCounter((new std::string("archc_epoch_invalidations_" + std::string(SourceNames[i])))->c_str(), invalidations[i]);
            AgcDriver::ArchB::Recorder::RegisterCounter((new std::string("archc_epoch_bumps_" + std::string(SourceNames[i])))->c_str(), bumps[i]);
        }
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_epoch_drain_entries", drainEntries);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_epoch_drain_ns", drainNs);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_epoch_log_lost", logLost);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_epoch_registered", registered);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_epoch_dropped", dropped);
    }
};

Counters& counters() {
    static Counters made;
    return made;
}

struct Index {
    AgcDriver::Mutex mutex;
    std::unordered_map<std::uint64_t, std::vector<Slot>> blocks;
    std::unordered_map<const void*, Record> records;
    std::array<std::atomic<std::uint64_t>, FilterBits / 64> filter{};
    std::atomic<std::uint64_t> loss{0};
    std::atomic<bool> everOn{false};
    std::uint64_t cursor = 0;
    std::uint64_t seenDropped = 0;
    std::uint64_t rangeBegin = 0;
    std::uint64_t rangeKind = 0;
    std::array<std::atomic<bool>, static_cast<std::size_t>(EpochSource::Count)> suppressed{};
    std::vector<std::uint64_t> entries = std::vector<std::uint64_t>(DrainBatch);
};

Index& index() {
    static Index made;
    return made;
}

std::size_t filterBit(std::uint64_t block) {
    auto mixed = block * 0x9e3779b97f4a7c15ull;
    mixed ^= mixed >> 29;
    return static_cast<std::size_t>(mixed & (FilterBits - 1));
}

bool filterAny(const Index& made, std::uint64_t begin, std::uint64_t end) {
    for (auto block = begin >> BlockShift; block <= (end - 1) >> BlockShift; ++block) {
        const auto bit = filterBit(block);
        if ((made.filter[bit / 64].load(std::memory_order_acquire) >> (bit % 64)) & 1) return true;
    }
    return false;
}

bool censusFromEnvironment() {
    const char* value = std::getenv("APS5_ARCHB_C_TARGET_CENSUS");
    return value != nullptr && value[0] == '1';
}

bool stampSelfExclusion() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_C_STAMP_SELF_EXCLUSION");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct SelfSlot {};
using CurrentSelf = HostThreadSlot<const StorageTexture*, SelfSlot>;

void stampObserver(std::uint64_t begin, std::uint64_t end, GuestMemory::TrackerEvent event) {
    if (!ImageEpochs::Active()) {
        ImageEpochs::NoteDropped();
        return;
    }
    if (event == GuestMemory::TrackerEvent::Stamp) ImageEpochs::Get().Invalidate(begin, end, EpochSource::TrackerStamp, stampSelfExclusion() ? SelfScope::Current() : nullptr);
    else ImageEpochs::Get().Invalidate(begin, end, event == GuestMemory::TrackerEvent::Unwatch ? EpochSource::Unwatch : EpochSource::Forget, nullptr);
}

std::atomic<std::uint64_t> hotPages{0};
std::atomic<std::uint64_t> hotChecks{0};
std::atomic<std::uint64_t> storedPages{0};
std::atomic<std::uint64_t> storedHotPages{0};

bool storedPage(std::uint64_t page) {
    return GuestArena::GuestWtFaultStoredLastFrame_nid_no_patch(static_cast<std::uintptr_t>(page));
}

bool storedHotPage(std::uint64_t page) {
    return GuestArena::GuestWtFaultStoredLastFrame_nid_no_patch(static_cast<std::uintptr_t>(page)) && (GuestArena::GuestWtFaultWord_nid_no_patch(static_cast<std::uintptr_t>(page)) & GuestArena::WtFaultStateMask) == GuestArena::WtFaultOpen;
}

bool openPage(std::uint64_t page) {
    return (GuestArena::GuestWtFaultWord_nid_no_patch(static_cast<std::uintptr_t>(page)) & GuestArena::WtFaultStateMask) == GuestArena::WtFaultOpen;
}

void armObserver() {
    if (!ImageEpochs::CensusEnabled()) return;
    hotPages.fetch_add(ImageEpochs::Get().CountImagePages(&openPage), std::memory_order_relaxed);
    storedPages.fetch_add(ImageEpochs::Get().CountImagePages(&storedPage), std::memory_order_relaxed);
    storedHotPages.fetch_add(ImageEpochs::Get().CountImagePages(&storedHotPage), std::memory_order_relaxed);
    hotChecks.fetch_add(1, std::memory_order_relaxed);
}

}

ImageEpochs& ImageEpochs::Get() {
    static ImageEpochs made;
    return made;
}

bool ImageEpochs::CensusEnabled() {
    static const bool enabled = censusFromEnvironment();
    return enabled;
}

bool ImageEpochs::Active() {
    if (CensusEnabled()) return true;
    if (!AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::ArchcImageEpochs)) return false;
    auto& made = index();
    if (!made.everOn.load(std::memory_order_relaxed)) made.everOn.store(true, std::memory_order_release);
    return true;
}

EpochPublication ImageEpochs::Note(std::uint64_t begin, std::uint64_t end, EpochSource source, const void* except) {
    if (!Active()) {
        NoteDropped();
        return EpochPublication::Inactive;
    }
    return Get().Invalidate(begin, end, source, except);
}

void ImageEpochs::InvalidateAll() {
    index().loss.fetch_add(1, std::memory_order_acq_rel);
}

void ImageEpochs::NoteDropped() {
    if (index().everOn.load(std::memory_order_acquire)) counters().dropped.fetch_add(1, std::memory_order_relaxed);
}

void ImageEpochs::SetSuppressed(EpochSource source, bool suppressed) {
    index().suppressed[static_cast<std::size_t>(source)].store(suppressed, std::memory_order_release);
}

void ImageEpochs::InstallHooks() {
    static const bool installed = [] {
        static_cast<void>(counters());
        GuestMemory::SetStampObserver(&stampObserver);
        GuestMemory::SetArmObserver(&armObserver);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_census_image_hot_pages", hotPages);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_census_image_hot_arms", hotChecks);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_census_image_stored_pages", storedPages);
        AgcDriver::ArchB::Recorder::RegisterCounter("archc_census_image_stored_hot_pages", storedHotPages);
        return true;
    }();
    static_cast<void>(installed);
}

void ImageEpochs::Register(StorageTexture& image, std::span<const EpochRange> ranges) {
    RegisterOwner(&image, image.archcEpoch, ranges);
}

void ImageEpochs::RegisterOwner(const void* owner, std::atomic<std::uint64_t>& epoch, std::span<const EpochRange> ranges, std::uint32_t sources) {
    InstallHooks();
    auto& made = index();
    std::vector<EpochRange> added;
    {
        std::lock_guard lock(made.mutex);
        auto& record = made.records[owner];
        if (!record.census.registered) {
            record.census.registered = true;
            counters().registered.fetch_add(1, std::memory_order_relaxed);
        }
        for (const auto& range : ranges) {
            if (range.end <= range.begin) continue;
            if (std::any_of(record.ranges.begin(), record.ranges.end(), [&](const EpochRange& known) { return known.begin == range.begin && known.end == range.end; })) continue;
            record.ranges.push_back(range);
            added.push_back(range);
            for (auto block = range.begin >> BlockShift; block <= (range.end - 1) >> BlockShift; ++block) {
                made.blocks[block].push_back({owner, &epoch, range.begin, range.end, sources});
                const auto bit = filterBit(block);
                made.filter[bit / 64].fetch_or(std::uint64_t{1} << (bit % 64), std::memory_order_release);
            }
        }
    }
    if (added.empty()) return;
    epoch.fetch_add(1, std::memory_order_acq_rel);
    if (!AgcDriver::ArchB::Switches::Enabled(AgcDriver::ArchB::Feature::ArchcImageEpochs) || (sources & SourceBit(EpochSource::CpuStore)) == 0) return;
    for (const auto& range : added) GuestArena::GuestWtFaultSetCold_nid_no_patch(static_cast<std::uintptr_t>(range.begin), static_cast<std::size_t>(range.end - range.begin), true);
}

void ImageEpochs::RegisterImage(StorageTexture& image, std::uint64_t cmaskAddress, std::uint64_t cmaskBytes) {
    const auto& descriptor = image.Descriptor();
    std::array<EpochRange, 3> ranges{};
    std::size_t used = 0;
    ranges[used++] = {descriptor.baseAddress, descriptor.baseAddress + image.GuestBytes()};
    if (descriptor.dccAddress != 0) ranges[used++] = {descriptor.dccAddress, descriptor.dccAddress + DccKeyBytes(image.GuestBytes())};
    if (cmaskAddress != 0 && cmaskBytes != 0) ranges[used++] = {cmaskAddress, cmaskAddress + cmaskBytes};
    Get().Register(image, std::span(ranges.data(), used));
}

void ImageEpochs::Unregister(const StorageTexture& image) {
    UnregisterOwner(&image);
}

void ImageEpochs::UnregisterOwner(const void* owner) {
    auto& made = index();
    std::lock_guard lock(made.mutex);
    const auto found = made.records.find(owner);
    if (found == made.records.end()) return;
    for (const auto& range : found->second.ranges) {
        for (auto block = range.begin >> BlockShift; block <= (range.end - 1) >> BlockShift; ++block) {
            const auto slots = made.blocks.find(block);
            if (slots == made.blocks.end()) continue;
            std::erase_if(slots->second, [&](const Slot& slot) { return slot.image == owner; });
            if (slots->second.empty()) made.blocks.erase(slots);
        }
    }
    made.records.erase(found);
}

bool ImageEpochs::Registered(const StorageTexture& image) {
    auto& made = index();
    std::lock_guard lock(made.mutex);
    return made.records.contains(static_cast<const void*>(&image));
}

EpochCensus ImageEpochs::Census(const StorageTexture& image) {
    auto& made = index();
    std::lock_guard lock(made.mutex);
    const auto found = made.records.find(static_cast<const void*>(&image));
    return found == made.records.end() ? EpochCensus{} : found->second.census;
}

EpochPublication ImageEpochs::Invalidate(std::uint64_t begin, std::uint64_t end, EpochSource source, const void* except) {
    if (end <= begin) return EpochPublication::NoRecipients;
    auto& made = index();
    const auto which = static_cast<std::size_t>(source);
    if (made.suppressed[which].load(std::memory_order_acquire)) return EpochPublication::Suppressed;
    if (!filterAny(made, begin, end)) return EpochPublication::NoRecipients;
    const bool census = CensusEnabled();
    const void* self = census ? SelfScope::Current() : nullptr;
    std::uint64_t bumped = 0;
    {
        std::lock_guard lock(made.mutex);
        for (auto block = begin >> BlockShift; block <= (end - 1) >> BlockShift; ++block) {
            const auto slots = made.blocks.find(block);
            if (slots == made.blocks.end()) continue;
            for (const auto& slot : slots->second) {
                if (slot.begin >= end || begin >= slot.end) continue;
                if (census) {
                    auto& record = made.records[slot.image].census;
                    if (source == EpochSource::TrackerStamp) ++(slot.image == self ? record.selfStamps : record.otherStamps);
                    else if (source == EpochSource::GpuNote) ++(slot.image == self ? record.selfNotes : record.otherNotes);
                    else if (source == EpochSource::Pending) ++record.pending;
                    else if (source == EpochSource::Depth) ++record.depth;
                }
                if (slot.image == except || (slot.sources & SourceBit(source)) == 0) continue;
                slot.epoch->fetch_add(1, std::memory_order_acq_rel);
                ++bumped;
            }
        }
    }
    if (bumped == 0) return EpochPublication::NoRecipients;
    counters().invalidations[which].fetch_add(1, std::memory_order_relaxed);
    counters().bumps[which].fetch_add(bumped, std::memory_order_relaxed);
    return EpochPublication::Published;
}

bool ImageEpochs::DrainIfNew() {
    auto& made = index();
    const auto dropped = counters().dropped.load(std::memory_order_acquire);
    if (dropped != made.seenDropped) {
        std::lock_guard lock(made.mutex);
        made.seenDropped = dropped;
        made.loss.fetch_add(1, std::memory_order_acq_rel);
    }
    const auto head = GuestArena::GuestWtFaultLogHead_nid_no_patch();
    if (head == made.cursor) return true;
    const auto start = std::chrono::steady_clock::now();
    std::lock_guard lock(made.mutex);
    std::uint64_t drained = 0;
    while (made.cursor < head) {
        bool lost = false;
        const auto count = GuestArena::GuestWtFaultLogRead_nid_no_patch(&made.cursor, made.entries.data(), made.entries.size(), &lost);
        if (lost) {
            made.loss.fetch_add(1, std::memory_order_acq_rel);
            counters().logLost.fetch_add(1, std::memory_order_relaxed);
        }
        if (count == 0 && !lost) break;
        drained += count;
        for (std::size_t i = 0; i < count; ++i) {
            const auto entry = made.entries[i];
            const auto kind = entry & 7u;
            const auto page = entry & ~std::uint64_t{0xfff};
            std::uint64_t first = page;
            std::uint64_t stop = page + 4096;
            EpochSource source = EpochSource::CpuStore;
            if (kind == GuestArena::WtFaultLogHold || kind == GuestArena::WtFaultLogOpen || kind == GuestArena::WtFaultLogForgetBegin) {
                made.rangeBegin = page;
                made.rangeKind = kind;
                continue;
            }
            if (kind == GuestArena::WtFaultLogForgetEnd) {
                source = made.rangeKind == GuestArena::WtFaultLogHold ? EpochSource::Hold : made.rangeKind == GuestArena::WtFaultLogOpen ? EpochSource::Open : EpochSource::Forget;
                first = made.rangeBegin;
                stop = page;
                made.rangeKind = 0;
            } else if (kind != GuestArena::WtFaultLogStore) {
                continue;
            }
            if (stop <= first || made.suppressed[static_cast<std::size_t>(source)].load(std::memory_order_acquire) || !filterAny(made, first, stop)) continue;
            std::uint64_t bumped = 0;
            for (auto block = first >> BlockShift; block <= (stop - 1) >> BlockShift; ++block) {
                const auto slots = made.blocks.find(block);
                if (slots == made.blocks.end()) continue;
                for (const auto& slot : slots->second) {
                    if (slot.begin >= stop || first >= slot.end || (slot.sources & SourceBit(source)) == 0) continue;
                    slot.epoch->fetch_add(1, std::memory_order_acq_rel);
                    ++bumped;
                }
            }
            if (bumped == 0) continue;
            counters().invalidations[static_cast<std::size_t>(source)].fetch_add(1, std::memory_order_relaxed);
            counters().bumps[static_cast<std::size_t>(source)].fetch_add(bumped, std::memory_order_relaxed);
        }
    }
    counters().drainEntries.fetch_add(drained, std::memory_order_relaxed);
    counters().drainNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
    return made.cursor >= head;
}

std::uint64_t ImageEpochs::Loss() const {
    return index().loss.load(std::memory_order_acquire);
}

std::size_t ImageEpochs::CountImagePages(bool (*open)(std::uint64_t page)) {
    auto& made = index();
    std::vector<EpochRange> ranges;
    {
        std::lock_guard lock(made.mutex);
        for (const auto& [image, record] : made.records) ranges.insert(ranges.end(), record.ranges.begin(), record.ranges.end());
    }
    std::sort(ranges.begin(), ranges.end(), [](const EpochRange& a, const EpochRange& b) { return a.begin < b.begin; });
    std::size_t pages = 0;
    std::uint64_t covered = 0;
    for (const auto& range : ranges) {
        for (auto page = std::max(range.begin & ~std::uint64_t{0xfff}, covered); page < range.end; page += 4096) {
            if (open(page)) ++pages;
        }
        covered = std::max(covered, (range.end + 0xfff) & ~std::uint64_t{0xfff});
    }
    return pages;
}

SelfScope::SelfScope(const StorageTexture* image) : previous(CurrentSelf::Get()) {
    CurrentSelf::Set(image);
}

SelfScope::~SelfScope() {
    CurrentSelf::Set(previous);
}

const StorageTexture* SelfScope::Current() {
    return CurrentSelf::Get();
}

}
