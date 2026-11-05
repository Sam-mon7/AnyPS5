#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "ThreadOwned.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Stage4Diag.hpp"
#include "prx/libc/include/GuestWriteFault.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ImageEpochs.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace AgcDriver::DriverDetail {

void Driver::appendEntryRegions(const DispatchVariant& variant, std::vector<ShaderRecompiler::MemoryRegion>& regions, const std::vector<std::uint32_t>* words) {
    const auto& source = words != nullptr ? *words : variant.words;
    regions.reserve(regions.size() + variant.runs.size());
    std::size_t offset = 0;
    for (const auto& [begin, end] : variant.runs) {
        const auto count = static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
        regions.push_back({begin, std::as_bytes(std::span<const std::uint32_t>(source).subspan(offset, count))});
        offset += count;
    }
}

std::shared_ptr<const ShaderRecompiler::RecompileResult> Driver::patchDataWords(const DispatchVariant& variant, const std::vector<std::uint32_t>& words, bool bufferBases) {
    auto patched = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
    if (!variant.dataPositions.empty()) {
        auto& descriptor = patched->bindings[variant.flatBinding].guestDescriptor;
        for (std::size_t k = 0; k < variant.dataPositions.size(); ++k) {
            if (variant.dataSlots[k] < descriptor.size()) descriptor[variant.dataSlots[k]] = words[variant.dataPositions[k]];
        }
    }
    if (bufferBases && !variant.bufferBaseMasks.empty() && !PatchBufferBases(variant, words, *patched)) return nullptr;
    return patched;
}

bool Driver::syncPendingRuns(std::uint64_t program, std::uint32_t queue, const ShaderRecompiler::RecompileResult& compiled, std::span<const ShaderRecompiler::MemoryRegion> regions, std::uint64_t& synced, PendingView& pending, std::optional<SampledReadScope>& sampling, std::span<const VouchedRegions> skipped) {
    using Policy = ShaderMemory::PendingWrite;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    for (std::size_t index = 0, range = 0; index < regions.size(); ++index) {
        if (range < skipped.size() && index == skipped[range].begin) {
            index = skipped[range++].end - 1u;
            continue;
        }
        const auto& region = regions[index];
        if (!pending.Overlaps(region.guestAddress, region.bytes.size())) continue;
        if (!sampling) sampling.emplace(siteSampling() ? evidenceValidations : evidenceReads);
        std::uint64_t ValidateCounters::*reason = &ValidateCounters::syncedOff;
        const auto policy = validateSkipEnabled() ? classifyPendingWrite(region.guestAddress, region.bytes.size(), reason, pending) : Policy::Sync;
        if (policy != Policy::Sync) continue;
        if (validateSkipEnabled() && (validateLegacy() ? !GuestMemory::EqualsCommittedUnsynced(region.guestAddress, region.bytes) : GuestMemory::CompareMapped(region.guestAddress, region.bytes) != GuestMemory::Compare::Equal)) {
            if (profile) {
                std::lock_guard lock(validateMutex);
                ++validateCounters.pending;
                ++validateCounters.unsyncedMisses;
                dataMissReasons[5].fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }
        std::vector<std::byte> before;
        if (writeEvidenceEnabled() && GuestMemory::Accessible(reinterpret_cast<const void*>(region.guestAddress), region.bytes.size())) {
            before.resize(region.bytes.size());
            std::memcpy(before.data(), reinterpret_cast<const void*>(region.guestAddress), before.size());
        }
        const auto waitsBefore = Graphics::Recorder::ThreadHookWaits();
        const bool timed = profile || traceCapSync();
        const auto waitedBefore = timed ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
        GuestMemory::FlushGpuWrites(region.guestAddress, region.bytes.size());
        const auto waited = timed ? Graphics::Recorder::ThreadWaitedMs() - waitedBefore : 0.0;
        pending.Load();
        if (!before.empty()) {
            if (!observationGuard() || Graphics::Recorder::ThreadHookWaits() != waitsBefore) observeRange(region.guestAddress, before);
            else observationsNoWait.fetch_add(1, std::memory_order_relaxed);
        }
        ++synced;
        if (profile) {
            std::lock_guard lock(validateMutex);
            auto& counters = validateCounters;
            ++counters.pending;
            ++(counters.*reason);
            counters.syncedWaitMs += waited;
            if (std::chrono::steady_clock::now() - counters.lastReport > std::chrono::seconds(10)) reportValidation(counters);
        }
        if (traceCapSync() && traceBudget()) {
            const auto begin = region.guestAddress;
            const auto end = begin + region.bytes.size();
            std::fprintf(stderr, "[capsync] dispatch-cache q0x%x program 0x%llx region 0x%llx+0x%zx synced before the gate: waited %.1f ms;%s; writers:%s\n", queue, static_cast<unsigned long long>(program), static_cast<unsigned long long>(begin), region.bytes.size(), waited, describeSelf(compiled, begin, end).c_str(), describeWriters(begin, end).c_str());
        }
    }
    return true;
}

bool Driver::vouchedValidation() {
    static const bool enabled = std::getenv("APS5_NO_VOUCHED_VALIDATION") == nullptr;
    return enabled;
}

namespace {
// Whether the calling thread's last validateVariant reached the byte compare (the shadow tells a
// decision taken on the bytes from a refusal by a gate or a serial).
thread_local bool variantCompared = false;
// Whether the calling thread's last validateVariant proved the guest bytes of every run equal to the
// variant's words exactly (Equal with no live data word, no masked feedback bit and no pending GPU
// write over a run): only then may a since move (CompareAndRestamp).
thread_local bool variantExact = false;

// Slice S7 (GEN_DRAW_VALIDATION): the generation verdict over the variant's runs at its since and
// the calling thread's ticket. Clean needs every block tracked, covered by a resetting walk that
// started at or after the thread's latest ordering point (a CPU store ordered before it was found
// and stamped by such a walk or an earlier one; blocks no sweep covered at the ticket are
// unknown_not_swept, never Clean), and every kind (data, cpu, meta, pending) at most the since; a
// GPU write announced over a run and still pending makes it Pending (the byte compare then decides
// with the known values it carries).
GuestMemory::Gen::DrawGen generationVerdict(const DispatchVariant& variant, const PendingView& pending) {
    namespace Gen = GuestMemory::Gen;
    const auto verdict = Gen::CheckRuns(Gen::Global(), variant.runs, variant.genSince.load(std::memory_order_acquire), Gen::Required(), variant.genProvenTicket.load(std::memory_order_acquire));
    if (verdict != Gen::DrawGen::Unknown && std::ranges::any_of(variant.runs, [&](const auto& run) { return pending.Overlaps(run.first, static_cast<std::size_t>(run.second - run.first)); })) return Gen::DrawGen::Pending;
    return verdict;
}
}

bool VariantValidationExact() {
    return variantExact;
}

void RestampVariant(const DispatchVariant& variant, std::uint64_t since, std::uint64_t ticket) {
    const auto raise = [](std::atomic<std::uint64_t>& field, std::uint64_t value) {
        auto current = field.load(std::memory_order_relaxed);
        while (current < value && !field.compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed)) {}
    };
    raise(variant.genSince, since);
    raise(variant.genProvenTicket, ticket);
}

bool CapturedOverPendingWrite(std::span<const ShaderRecompiler::MemoryRegion> regions) {
    PendingView pending;
    pending.Load();
    return std::ranges::any_of(regions, [&](const ShaderRecompiler::MemoryRegion& region) { return pending.Overlaps(region.guestAddress, region.bytes.size()); });
}

GuestMemory::Gen::Verdict ShadowValidatedVariant(GuestMemory::Gen::Consumer consumer, const DispatchVariant& variant, EntryOutcome outcome, std::uint64_t before, bool exact) {
    namespace Gen = GuestMemory::Gen;
    const auto since = variant.genSince.load(std::memory_order_acquire);
    auto& table = Gen::Global();
    const auto required = Gen::Required();
    constexpr auto kinds = static_cast<std::uint8_t>(Gen::Data | Gen::Meta | Gen::Pending);
    const auto rank = [](Gen::Verdict verdict) { return verdict == Gen::Verdict::Changed ? 3 : verdict == Gen::Verdict::Unknown ? 2 : verdict == Gen::Verdict::PendingOnly ? 1 : 0; };
    const auto evaluate = [&](bool exhaustive, bool* needsSweep, Gen::UnknownReason* reason) {
        auto verdict = since == 0 ? Gen::Verdict::Unknown : Gen::Verdict::Clean;
        *reason = since == 0 ? Gen::UnknownReason::NoSince : Gen::UnknownReason::None;
        for (const auto& [begin, end] : variant.runs) {
            if (since == 0) break;
            Gen::UnknownReason why = Gen::UnknownReason::None;
            const auto next = exhaustive ? table.CheckExhaustive(begin, end, since, 0, kinds, &why) : table.Check(begin, end, since, 0, kinds, &why);
            if (needsSweep != nullptr && (required == 0 || !table.Covered(begin, end, required))) *needsSweep = true;
            if (rank(next) > rank(verdict)) {
                verdict = next;
                *reason = why;
            }
        }
        // Not covered at the thread's ticket: Check would sweep first (unknown_not_swept, not Clean).
        if (verdict == Gen::Verdict::Clean && (required == 0 || std::ranges::any_of(variant.runs, [&](const auto& run) { return !table.Covered(run.first, run.second, required); }))) {
            verdict = Gen::Verdict::Unknown;
            *reason = Gen::UnknownReason::NotSwept;
        }
        return verdict;
    };
    bool needsSweep = false;
    Gen::UnknownReason reason = Gen::UnknownReason::None;
    const auto verdict = evaluate(false, &needsSweep, &reason);
    bool unchanged;
    if (outcome == EntryOutcome::Equal) unchanged = true;
    else if (outcome == EntryOutcome::EqualData || (variantCompared && (outcome == EntryOutcome::Differing || outcome == EntryOutcome::Inaccessible))) unchanged = false;
    else {
        Gen::CountRefused(consumer);
        return verdict;
    }
    const auto classified = Gen::Classify(consumer, verdict, reason, unchanged, needsSweep);
    if (classified == Gen::Outcome::Conservative && verdict == Gen::Verdict::Changed) {
        bool dataMoved = false;
        for (const auto& [begin, end] : variant.runs) dataMoved = dataMoved || table.Check(begin, end, since, 0, Gen::Data | Gen::Cpu) == Gen::Verdict::Changed;
        if (!dataMoved) Gen::CountConservativeMeta(consumer);
    }
    const auto first = variant.runs.empty() ? std::pair<std::uint64_t, std::uint64_t>{0, 1} : variant.runs.front();
    if (classified == Gen::Outcome::Unsafe) Gen::ReportUnsafe(consumer, first.first, first.second, since, "the byte compare found a change");
    if (Gen::VerifySample()) {
        Gen::UnknownReason slowReason = Gen::UnknownReason::None;
        const auto slow = evaluate(true, nullptr, &slowReason);
        auto verified = slow == Gen::Verdict::Clean ? (unchanged ? Gen::Outcome::AgreeClean : Gen::Outcome::Unsafe) : (unchanged ? Gen::Outcome::Conservative : Gen::Outcome::AgreeChanged);
        if (slow != verdict) verified = Gen::Outcome::Unsafe;
        Gen::CountVerify(consumer, verified);
        if (verified == Gen::Outcome::Unsafe) Gen::ReportUnsafe(consumer, first.first, first.second, since, slow != verdict ? "verify: exhaustive verdict differs" : "verify: clean while the byte compare found a change");
    }
    if (verdict != Gen::Verdict::Clean && outcome == EntryOutcome::Equal && before != 0 && exact) {
        Gen::CountRestamp(consumer);
        // GEN_DRAW_VALIDATION restamps in validateVariant itself (with a since taken right before the compare).
        if (!Gen::DrawValidationActive()) RestampVariant(variant, before);
    }
    return verdict;
}

EntryOutcome Driver::validateVariantLegacy(std::uint64_t program, std::uint32_t queue, const DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions, std::uint64_t& imagesFlushed, std::uint64_t& runsSynced, std::optional<SampledReadScope>& sampling, std::vector<std::pair<std::uint32_t, std::uint32_t>>* live, bool feedbackMask, bool* feedbackHit, bool bufferBases, GuestMemory::Gen::Consumer consumer) {
    namespace Gen = GuestMemory::Gen;
    ArchB::Recorder::Timer census("Q0.Variant", Stage4::Queue0Census());
    variantCompared = false;
    variantExact = false;
    if (live != nullptr) live->clear();
    if (feedbackHit != nullptr) *feedbackHit = false;
    if (DrawPipeline::Active()) {
        auto& pipeline = DrawPipeline::Queue0();
        if (pipeline.Busy() && pipeline.OverlapsAny(variant.runs)) pipeline.Drain(DrawPipeline::DrainReason::Capture, 0x100, DrawPipeline::DrainCause::CaptureVariant);
    }
    census.Mark("drain_check");
    const bool refreshBuffers = bufferBases && live != nullptr && !validateLegacy() && !variant.bufferBaseMasks.empty();
    const bool masked = live != nullptr && (!variant.dataPositions.empty() || refreshBuffers);
    const bool feedback = feedbackMask && !validateLegacy() && !variant.feedback.memory.empty();
    if (feedbackMask && validateLegacy() && !variant.feedback.memory.empty()) ArchB::Recorder::Count("fix_tsharp_mask_refused_legacy");
    const DataMask mask{masked ? std::span<const std::uint32_t>(variant.dataPositions) : std::span<const std::uint32_t>{}, live, feedback ? std::span<const std::pair<std::uint32_t, std::uint32_t>>(variant.feedback.memory) : std::span<const std::pair<std::uint32_t, std::uint32_t>>{}, feedbackHit, refreshBuffers ? std::span<const std::pair<std::uint32_t, std::uint32_t>>(variant.bufferBaseMasks) : std::span<const std::pair<std::uint32_t, std::uint32_t>>{}};
    if (bufferBases && validateLegacy() && !variant.bufferBaseMasks.empty()) ArchB::Recorder::Count("fix_buffer_base_refused_legacy");
    thread_local std::vector<VouchedRegions>* vouchedSlot = nullptr;
    auto& vouched = ShaderRecompiler::ThreadOwned(vouchedSlot);
    vouched.clear();
    if (!variant.shared.empty() && vouchedValidation() && !validateLegacy() && variant.runs.size() == regions.size()) VouchSharedRegions(variant.shared, regions.size(), &QueryPendingSpan, vouched);
    PendingView pending;
    pending.Load();
    if (refreshBuffers && std::ranges::any_of(variant.runs, [&](const auto& run) { return pending.Overlaps(run.first, static_cast<std::size_t>(run.second - run.first)); })) ArchB::Recorder::Count("fix_buffer_base_pending");
    if (feedback && std::ranges::any_of(variant.runs, [&](const auto& run) { return pending.Overlaps(run.first, static_cast<std::size_t>(run.second - run.first)); })) ArchB::Recorder::Count("fix_tsharp_mask_refused_pending");
    std::erase_if(vouched, [&](const VouchedRegions& range) { return pending.Overlaps(range.low, static_cast<std::size_t>(range.high - range.low)); });
    thread_local std::vector<std::pair<std::uint64_t, std::uint64_t>>* checkedSlot = nullptr;
    auto& checked = ShaderRecompiler::ThreadOwned(checkedSlot);
    checked.clear();
    if (!vouched.empty()) {
        for (std::size_t index = 0, range = 0; index < variant.runs.size(); ++index) {
            if (range < vouched.size() && index == vouched[range].begin) {
                checked.emplace_back(vouched[range].low, vouched[range].high);
                index = vouched[range++].end - 1u;
                continue;
            }
            checked.push_back(variant.runs[index]);
        }
    }
    const std::span<const std::pair<std::uint64_t, std::uint64_t>> runs = vouched.empty() ? std::span<const std::pair<std::uint64_t, std::uint64_t>>(variant.runs) : std::span<const std::pair<std::uint64_t, std::uint64_t>>(checked);
    census.Mark("vouch");
    if (!syncPendingRuns(program, queue, *variant.compiled, regions, runsSynced, pending, sampling, vouched)) return EntryOutcome::Differing;
    census.Mark("pending_sync");
    auto publish = Graphics::Recorder::PublishGeneration();
    auto pendingSerial = Graphics::StorageTexture::PendingSerial();
    auto forget = GuestMemory::ForgetSerial();

    if (Graphics::StorageTexture::AnyPendingOverlaps(runs) || Graphics::AnyShadowedOverlaps(runs)) {
        vouched.clear();
        for (const auto& region : regions) {
            if (Graphics::StorageTexture::FlushPending(region.guestAddress, region.bytes.size(), nullptr, "dispatch-cache entry")) ++imagesFlushed;
        }
        pending.Load();
        if (!syncPendingRuns(program, queue, *variant.compiled, regions, runsSynced, pending, sampling)) return EntryOutcome::Differing;
        publish = Graphics::Recorder::PublishGeneration();
        pendingSerial = Graphics::StorageTexture::PendingSerial();
        forget = GuestMemory::ForgetSerial();
        if (Graphics::StorageTexture::AnyPendingOverlaps(variant.runs) || Graphics::AnyShadowedOverlaps(variant.runs)) return EntryOutcome::FlushingImage;
    }
    census.Mark("storage");
    if ((forget & 1) != 0) return EntryOutcome::ForgetMoved;
    for (const auto& label : deferredLabels().labels) {
        for (const auto& [begin, end] : runs) {
            if (label.address < end && begin < label.address + label.size) return EntryOutcome::QueuedLabel;
        }
    }
    if (validateLegacy()) {
        for (const auto& region : regions) {
            if (!GuestMemory::Accessible(reinterpret_cast<const void*>(region.guestAddress), region.bytes.size())) return EntryOutcome::Inaccessible;
        }
    }

    pending.Load();
    census.Mark("labels");
    bool unmapped = false;
    variantCompared = true;
    // Slice S7 (GEN_DRAW_VALIDATION), after every gate above (drains, pending syncs, image flushes,
    // queued labels), where the byte compare would run: a Clean generation verdict means the guest
    // bytes of every run are the variant's words, which is what an exact compare would find (Equal,
    // no live data word, no feedback bit), so the compare is skipped. Otherwise the compare runs and,
    // when exact, moves the since to a generation taken before it (CompareAndRestamp) so the next
    // validation can be Clean. Verify mode compares a sampled Clean verdict's bytes anyway.
    const bool generations = Gen::DrawValidationActive() && !validateLegacy();
    auto verdict = Gen::DrawGen::Unknown;
    bool verifying = false;
    std::uint64_t before = 0;
    std::uint64_t ticket = 0;
    if (generations) {
        verdict = generationVerdict(variant, pending);
        before = Gen::CaptureSince();
        ticket = Gen::Required();
        if (verdict == Gen::DrawGen::Clean) {
            verifying = Gen::VerifySample();
            if (!verifying) {
                if (Graphics::Recorder::PublishGeneration() != publish) return EntryOutcome::PublishMoved;
                if (Graphics::StorageTexture::PendingSerial() != pendingSerial) return EntryOutcome::PendingMoved;
                if (GuestMemory::ForgetSerial() != forget) return EntryOutcome::ForgetMoved;
                Gen::CountDrawGen(consumer, Gen::DrawGen::Clean);
                variantCompared = false;
                variantExact = true;
                return EntryOutcome::Equal;
            }
        }
    }
    census.Mark("generations");
    const auto compared = validateCaptured(program, queue, regions, *variant.compiled, true, pending, &unmapped, &sampling, masked || feedback ? &mask : nullptr, vouched);
    census.Mark("compare");
    const bool exact = compared && (live == nullptr || live->empty()) && (feedbackHit == nullptr || !*feedbackHit);
    if (verifying) {
        Gen::CountDrawGen(consumer, exact ? Gen::DrawGen::Verified : Gen::DrawGen::Unsafe);
        Gen::CountVerify(consumer, exact ? Gen::Outcome::AgreeClean : Gen::Outcome::Unsafe);
        const auto first = variant.runs.front();
        if (!exact) Gen::ReportUnsafe(consumer, first.first, first.second, variant.genSince.load(std::memory_order_relaxed), "draw validation: clean while the byte compare found a change");
    }
    if (!compared) {
        if (generations && !verifying) Gen::CountDrawGen(consumer, verdict);
        return unmapped ? EntryOutcome::Inaccessible : EntryOutcome::Differing;
    }
    if (Graphics::Recorder::PublishGeneration() != publish) return EntryOutcome::PublishMoved;
    if (Graphics::StorageTexture::PendingSerial() != pendingSerial) return EntryOutcome::PendingMoved;
    if (GuestMemory::ForgetSerial() != forget) return EntryOutcome::ForgetMoved;
    variantExact = exact && verdict != Gen::DrawGen::Pending && !std::ranges::any_of(variant.runs, [&](const auto& run) { return pending.Overlaps(run.first, static_cast<std::size_t>(run.second - run.first)); });
    if (generations && !verifying) {
        Gen::CountDrawGen(consumer, verdict);
        if (variantExact && before != 0) {
            RestampVariant(variant, before, ticket);
            Gen::CountDrawGen(consumer, Gen::DrawGen::Restamped);
        }
    }
    return masked && !live->empty() ? EntryOutcome::EqualData : EntryOutcome::Equal;
}


namespace {

enum class Q0CensusClass : std::uint8_t { Hit, New, Cpu, CpuUntracked, Stamp, Gate, Refused, Differs, Inexact, Count };
constexpr const char* Q0CensusCountNames[] = {"q0c_variant_hit", "q0c_variant_new", "q0c_variant_cpu", "q0c_variant_cpu_untracked", "q0c_variant_stamp", "q0c_variant_gate", "q0c_variant_refused", "q0c_variant_differs", "q0c_variant_inexact"};
constexpr const char* Q0CensusNsNames[] = {"q0c_variant_hit_ns", "q0c_variant_new_ns", "q0c_variant_cpu_ns", "q0c_variant_cpu_untracked_ns", "q0c_variant_stamp_ns", "q0c_variant_gate_ns", "q0c_variant_refused_ns", "q0c_variant_differs_ns", "q0c_variant_inexact_ns"};

std::int64_t censusNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Q0CensusClass pullClass(const DispatchVariant& variant, bool stamps) {
    const auto clock = variant.q0CensusClock.load(std::memory_order_acquire);
    if (clock == 0) return Q0CensusClass::New;
    bool untracked = false;
    for (const auto& [begin, end] : variant.runs) {
        const auto answer = GuestArena::GuestWtFaultChangedSince_nid_no_patch(static_cast<std::uintptr_t>(begin), static_cast<std::size_t>(end - begin), clock);
        if (answer == GuestArena::WtFaultAnswer::Changed) return Q0CensusClass::Cpu;
        if (answer != GuestArena::WtFaultAnswer::Unchanged) untracked = true;
    }
    if (untracked) return Q0CensusClass::CpuUntracked;
    if (!stamps) return Q0CensusClass::Hit;
    thread_local std::vector<GuestMemory::UnchangedQuery>* queriesSlot = nullptr;
    auto& queries = ShaderRecompiler::ThreadOwned(queriesSlot);
    queries.clear();
    const auto generation = variant.q0CensusGeneration.load(std::memory_order_acquire);
    for (const auto& [begin, end] : variant.runs) queries.push_back({begin, static_cast<std::size_t>(end - begin), generation});
    if (!queries.empty() && !GuestMemory::UnchangedSinceAll(queries)) return Q0CensusClass::Stamp;
    return Q0CensusClass::Hit;
}

namespace q0epochs {

namespace ArchC = Graphics::ArchC;

constexpr std::size_t MaxPages = 16;
constexpr std::size_t Ways = 4;
constexpr std::size_t Sets = 1024;

struct PageEpoch {
    std::atomic<std::uint64_t> epoch{0};
};

struct Pages {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::unique_ptr<PageEpoch>> pages;
    std::atomic<std::uint64_t> registered{0};
};

Pages& pages() {
    static auto* made = new Pages;
    return *made;
}

struct Token {
    const DispatchVariant* variant = nullptr;
    std::uint64_t serial = 0;
    std::uint64_t loss = 0;
    std::uint64_t lastUse = 0;
    std::size_t count = 0;
    std::array<PageEpoch*, MaxPages> pages{};
    std::array<std::uint64_t, MaxPages> epochs{};
};

struct Table {
    std::array<Token, Sets * Ways> slots;
    std::unordered_map<std::uint64_t, PageEpoch*> cache;
    std::uint64_t uses = 0;
    std::uint64_t verifyTick = 0;
};

Table& table() {
    thread_local Table* slot = nullptr;
    return ShaderRecompiler::ThreadOwned(slot);
}

std::size_t setOf(const DispatchVariant& variant) {
    return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(&variant) >> 4) * 0x9e3779b97f4a7c15ull >> 40) % Sets;
}

Token* find(Table& made, const DispatchVariant& variant) {
    auto* set = &made.slots[setOf(variant) * Ways];
    for (std::size_t way = 0; way < Ways; ++way) {
        if (set[way].variant == &variant && set[way].serial == variant.archcSerial) return &set[way];
    }
    return nullptr;
}

Token& victim(Table& made, const DispatchVariant& variant) {
    auto* set = &made.slots[setOf(variant) * Ways];
    Token* chosen = &set[0];
    for (std::size_t way = 0; way < Ways; ++way) {
        if (set[way].variant == &variant) return set[way];
        if (set[way].variant == nullptr) chosen = &set[way];
        else if (chosen->variant != nullptr && set[way].lastUse < chosen->lastUse) chosen = &set[way];
    }
    return *chosen;
}

PageEpoch* pageEpoch(Table& made, std::uint64_t page) {
    if (const auto known = made.cache.find(page); known != made.cache.end()) return known->second;
    auto& all = pages();
    PageEpoch* epoch = nullptr;
    bool fresh = false;
    {
        std::lock_guard lock(all.mutex);
        auto& owned = all.pages[page];
        if (owned == nullptr) {
            owned = std::make_unique<PageEpoch>();
            fresh = true;
        }
        epoch = owned.get();
    }
    if (fresh) {
        const ArchC::EpochRange range{page, page + 4096};
        ArchC::ImageEpochs::Get().RegisterOwner(epoch, epoch->epoch, std::span(&range, 1), ArchC::AllSources);
        all.registered.fetch_add(1, std::memory_order_relaxed);
        ArchB::Recorder::Count("c_q0_epochs_pages_registered");
    }
    made.cache.emplace(page, epoch);
    return epoch;
}

std::size_t pagesOf(const DispatchVariant& variant, std::array<std::uint64_t, MaxPages>& out) {
    std::size_t count = 0;
    for (const auto& [begin, end] : variant.runs) {
        if (end <= begin) continue;
        for (auto page = begin & ~std::uint64_t{0xfff}; page < end; page += 4096) {
            if (std::find(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(count), page) != out.begin() + static_cast<std::ptrdiff_t>(count)) continue;
            if (count == MaxPages) return MaxPages + 1;
            out[count++] = page;
        }
    }
    return count;
}

bool runsArmed(const DispatchVariant& variant) {
    const auto clock = GuestArena::GuestWtFaultClock_nid_no_patch();
    for (const auto& [begin, end] : variant.runs) {
        if (GuestArena::GuestWtFaultChangedSince_nid_no_patch(static_cast<std::uintptr_t>(begin), static_cast<std::size_t>(end - begin), clock) != GuestArena::WtFaultAnswer::Unchanged) return false;
    }
    return true;
}

bool pipelineOverlaps(const DispatchVariant& variant) {
    if (!DrawPipeline::Active()) return false;
    auto& pipeline = DrawPipeline::Queue0();
    return pipeline.Busy() && pipeline.OverlapsAny(variant.runs);
}

bool labelsOverlap(const DispatchVariant& variant) {
    for (const auto& label : deferredLabels().labels) {
        for (const auto& [begin, end] : variant.runs) {
            if (label.address < end && begin < label.address + label.size) return true;
        }
    }
    return false;
}

std::uint64_t verifyPeriod() {
    static const std::uint64_t period = [] {
        const char* value = std::getenv("APS5_ARCHB_C_Q0_EPOCHS_VERIFY");
        return value != nullptr ? std::strtoull(value, nullptr, 0) : 0ull;
    }();
    return period;
}

struct Totals {
    std::atomic<std::uint64_t> hits{0}, misses{0}, tokens{0}, verifyChecks{0}, verifyMismatches{0}, stable{0}, refusedOpen{0}, refusedMoved{0}, exact{0}, verifyRaced{0};
    Totals() {
        ArchB::Recorder::RegisterCounter("archc_q0_hits", hits);
        ArchB::Recorder::RegisterCounter("archc_q0_misses", misses);
        ArchB::Recorder::RegisterCounter("archc_q0_tokens", tokens);
        ArchB::Recorder::RegisterCounter("archc_q0_verify_checks", verifyChecks);
        ArchB::Recorder::RegisterCounter("archc_q0_verify_mismatches", verifyMismatches);
        ArchB::Recorder::RegisterCounter("archc_q0_verify_raced", verifyRaced);
    }
};

Totals& totals() {
    static Totals made;
    return made;
}

enum class Check : std::uint8_t { Hit, NoToken, Drain, Loss, Epoch, Pipeline, Labels };
constexpr const char* CheckNames[] = {"c_q0_epochs_hits", "c_q0_epochs_miss_no_token", "c_q0_epochs_miss_drain", "c_q0_epochs_miss_loss", "c_q0_epochs_miss_epoch", "c_q0_epochs_miss_pipeline", "c_q0_epochs_miss_labels"};

Check check(Table& made, const DispatchVariant& variant, Token*& token) {
    token = find(made, variant);
    if (token == nullptr) return Check::NoToken;
    if (!ArchC::ImageEpochs::Get().DrainIfNew()) return Check::Drain;
    if (token->loss != ArchC::ImageEpochs::Get().Loss()) return Check::Loss;
    for (std::size_t i = 0; i < token->count; ++i) {
        if (token->pages[i]->epoch.load(std::memory_order_acquire) != token->epochs[i]) return Check::Epoch;
    }
    if (pipelineOverlaps(variant)) return Check::Pipeline;
    if (labelsOverlap(variant)) return Check::Labels;
    token->lastUse = ++made.uses;
    return Check::Hit;
}

}

}

Driver::Q0EpochCounts Driver::Q0EpochCounters() {
    auto& made = q0epochs::totals();
    return {made.hits.load(std::memory_order_relaxed), made.misses.load(std::memory_order_relaxed), made.tokens.load(std::memory_order_relaxed), made.verifyChecks.load(std::memory_order_relaxed), made.verifyMismatches.load(std::memory_order_relaxed), q0epochs::pages().registered.load(std::memory_order_relaxed), made.stable.load(std::memory_order_relaxed), made.refusedOpen.load(std::memory_order_relaxed), made.refusedMoved.load(std::memory_order_relaxed), made.exact.load(std::memory_order_relaxed)};
}

namespace {

struct DispatchItemTotals {
    std::atomic<std::uint64_t> items{0}, verifyChecks{0}, verifyMismatches{0}, verifySkipped{0};
    DispatchItemTotals() {
        ArchB::Recorder::RegisterCounter("c_q0_dispatch_items_indirect", items);
        ArchB::Recorder::RegisterCounter("c_q0_dispatch_items_verify_checks", verifyChecks);
        ArchB::Recorder::RegisterCounter("c_q0_dispatch_items_verify_mismatches", verifyMismatches);
        ArchB::Recorder::RegisterCounter("c_q0_dispatch_items_verify_skipped", verifySkipped);
    }
};

DispatchItemTotals& dispatchItemTotals() {
    static DispatchItemTotals made;
    return made;
}

std::uint64_t dispatchItemVerifyPeriod() {
    static const std::uint64_t period = [] {
        const char* value = std::getenv("APS5_ARCHB_C_Q0_DISPATCH_ITEMS_VERIFY");
        return value != nullptr ? std::strtoull(value, nullptr, 0) : 0ull;
    }();
    return period;
}

}

std::array<std::uint64_t, 3> Driver::Q0DispatchItemCounts() {
    auto& made = dispatchItemTotals();
    return {made.items.load(std::memory_order_relaxed), made.verifyChecks.load(std::memory_order_relaxed), made.verifyMismatches.load(std::memory_order_relaxed)};
}

bool Driver::q0DispatchItemsActive() {
    return ArchB::Switches::Enabled(ArchB::Feature::CQ0DispatchItems);
}

void Driver::q0DispatchItemCommitted(std::span<const Graphics::GuestMemorySnapshot> snapshots) {
    auto& made = dispatchItemTotals();
    const auto count = made.items.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto period = dispatchItemVerifyPeriod();
    if (period == 0 || count % period != 0) return;
    made.verifyChecks.fetch_add(1, std::memory_order_relaxed);
    PendingView pending;
    pending.Load();
    for (const auto& snapshot : snapshots) {
        if (snapshot.bytes.empty()) continue;
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> range{{{snapshot.address, snapshot.address + snapshot.bytes.size()}}};
        if (pending.Overlaps(snapshot.address, snapshot.bytes.size()) || Graphics::StorageTexture::AnyPendingOverlaps(range)) {
            made.verifySkipped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (GuestMemory::CompareMapped(snapshot.address, snapshot.bytes) == GuestMemory::Compare::Differs) {
            made.verifyMismatches.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "[c-q0-dispatch-items] VERIFY mismatch: snapshot 0x%llx+0x%zx changed between queue 0's capture and the commit\n", static_cast<unsigned long long>(snapshot.address), snapshot.bytes.size());
        }
    }
}

bool Driver::q0EpochsActive() {
    return ArchB::Switches::Enabled(ArchB::Feature::CQ0Epochs) && Graphics::ArchC::ImageEpochs::Active() && !validateLegacy() && !GuestMemory::Gen::DrawValidationActive();
}

EntryOutcome Driver::validateVariant(std::uint64_t program, std::uint32_t queue, const DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions, std::uint64_t& imagesFlushed, std::uint64_t& runsSynced, std::optional<SampledReadScope>& sampling, std::vector<std::pair<std::uint32_t, std::uint32_t>>* live, bool feedbackMask, bool* feedbackHit, bool bufferBases, GuestMemory::Gen::Consumer consumer) {
    const bool census = Stage4::Queue0Census() && ArchB::Recorder::Enabled();
    const bool epochs = q0EpochsActive();
    if (!census && !epochs) return validateVariantLegacy(program, queue, variant, regions, imagesFlushed, runsSynced, sampling, live, feedbackMask, feedbackHit, bufferBases, consumer);
    namespace E = q0epochs;
    E::Token* token = nullptr;
    auto& made = E::table();
    bool verifying = false;
    if (epochs) {
        const auto checked = E::check(made, variant, token);
        ArchB::Recorder::Count(E::CheckNames[static_cast<std::size_t>(checked)]);
        (checked == E::Check::Hit ? E::totals().hits : E::totals().misses).fetch_add(1, std::memory_order_relaxed);
        if (checked == E::Check::Hit) {
            const auto period = E::verifyPeriod();
            verifying = period != 0 && ++made.verifyTick % period == 0;
            if (!verifying) {
                variantCompared = false;
                variantExact = true;
                if (live != nullptr) live->clear();
                if (feedbackHit != nullptr) *feedbackHit = false;
                return EntryOutcome::Equal;
            }
        }
    }
    std::optional<ArchB::Recorder::Timer> overhead;
    if (census) overhead.emplace("Q0.CensusOverhead", true);
    auto cls = census ? pullClass(variant, false) : Q0CensusClass::New;
    const auto clock = GuestArena::GuestWtFaultClock_nid_no_patch();
    const std::uint64_t generation = 0;
    overhead.reset();
    const auto pulled = epochs && !verifying && variant.q0CensusClock.load(std::memory_order_acquire) != 0 ? (census ? cls : pullClass(variant, false)) : Q0CensusClass::New;
    const bool stable = pulled == Q0CensusClass::Hit;
    std::array<std::uint64_t, E::MaxPages> pageAddresses{};
    std::array<E::PageEpoch*, E::MaxPages> pageEpochs{};
    std::array<std::uint64_t, E::MaxPages> before{};
    std::size_t pageCount = 0;
    std::uint64_t loss = 0;
    bool candidate = false;
    if (pulled == Q0CensusClass::CpuUntracked) {
        std::array<std::uint64_t, E::MaxPages> untracked{};
        const auto count = E::pagesOf(variant, untracked);
        for (std::size_t i = 0; count <= E::MaxPages && i < count; ++i) static_cast<void>(E::pageEpoch(made, untracked[i]));
        ArchB::Recorder::Count("c_q0_epochs_registered_untracked");
    }
    if (stable) {
        E::totals().stable.fetch_add(1, std::memory_order_relaxed);
        pageCount = E::pagesOf(variant, pageAddresses);
        if (census) {
            static constexpr const char* PageCounts[] = {"q0c_pages_0", "q0c_pages_1", "q0c_pages_2", "q0c_pages_3", "q0c_pages_4", "q0c_pages_5_8", "q0c_pages_9_16", "q0c_pages_more"};
            ArchB::Recorder::Count(PageCounts[pageCount <= 4 ? pageCount : pageCount <= 8 ? 5 : pageCount <= 16 ? 6 : 7]);
        }
        if (pageCount > E::MaxPages) {
            ArchB::Recorder::Count("c_q0_epochs_refused_pages");
        } else {
            for (std::size_t i = 0; i < pageCount; ++i) pageEpochs[i] = E::pageEpoch(made, pageAddresses[i]);
            candidate = Graphics::ArchC::ImageEpochs::Get().DrainIfNew();
            loss = Graphics::ArchC::ImageEpochs::Get().Loss();
            for (std::size_t i = 0; i < pageCount; ++i) before[i] = pageEpochs[i]->epoch.load(std::memory_order_acquire);
        }
    }
    const auto flushedBefore = imagesFlushed;
    const auto syncedBefore = runsSynced;
    const auto drainsBefore = DrawPipeline::DrainRequestsByOpcode(0x100);
    const auto start = census ? censusNowNs() : 0;
    const auto outcome = validateVariantLegacy(program, queue, variant, regions, imagesFlushed, runsSynced, sampling, live, feedbackMask, feedbackHit, bufferBases, consumer);
    const auto spent = census ? censusNowNs() - start : 0;
    const bool exact = outcome == EntryOutcome::Equal && variantExact;
    if (epochs && exact) E::totals().exact.fetch_add(1, std::memory_order_relaxed);
    variant.q0CensusClock.store(exact ? clock : 0, std::memory_order_release);
    variant.q0CensusGeneration.store(exact ? generation : 0, std::memory_order_release);
    if (verifying) {
        ArchB::Recorder::Count("c_q0_epochs_verify_checks");
        E::totals().verifyChecks.fetch_add(1, std::memory_order_relaxed);
        const bool raced = outcome == EntryOutcome::PublishMoved || outcome == EntryOutcome::PendingMoved || outcome == EntryOutcome::ForgetMoved;
        if (raced) {
            ArchB::Recorder::Count("c_q0_epochs_verify_raced");
            E::totals().verifyRaced.fetch_add(1, std::memory_order_relaxed);
        } else if (!exact) {
            ArchB::Recorder::Count("c_q0_epochs_verify_mismatches");
            E::totals().verifyMismatches.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "[c-q0-epochs] VERIFY mismatch: program 0x%llx queue 0x%x outcome %u exact %d runs %zu first 0x%llx\n", static_cast<unsigned long long>(program), queue, static_cast<unsigned>(outcome), exact ? 1 : 0, variant.runs.size(), variant.runs.empty() ? 0ull : static_cast<unsigned long long>(variant.runs.front().first));
            if (token != nullptr) token->variant = nullptr;
        }
    }
    if (candidate) {
        bool minted = exact && Graphics::ArchC::ImageEpochs::Get().DrainIfNew() && Graphics::ArchC::ImageEpochs::Get().Loss() == loss;
        for (std::size_t i = 0; minted && i < pageCount; ++i) minted = pageEpochs[i]->epoch.load(std::memory_order_acquire) == before[i];
        if (minted && !E::runsArmed(variant)) {
            minted = false;
            ArchB::Recorder::Count("c_q0_epochs_refused_open");
            E::totals().refusedOpen.fetch_add(1, std::memory_order_relaxed);
        }
        if (minted) {
            auto& slot = E::victim(made, variant);
            slot.variant = &variant;
            slot.serial = variant.archcSerial;
            slot.loss = loss;
            slot.lastUse = ++made.uses;
            slot.count = pageCount;
            slot.pages = pageEpochs;
            slot.epochs = before;
            ArchB::Recorder::Count("c_q0_epochs_tokens");
            E::totals().tokens.fetch_add(1, std::memory_order_relaxed);
        } else if (exact) {
            ArchB::Recorder::Count("c_q0_epochs_refused_moved");
            E::totals().refusedMoved.fetch_add(1, std::memory_order_relaxed);
        }
    }
    static const bool trace = std::getenv("APS5_ARCHB_C_Q0_EPOCHS_TRACE") != nullptr;
    if (trace && epochs) std::fprintf(stderr, "[c-q0-epochs] queue 0x%x program 0x%llx runs %zu first 0x%llx clock %d stable %d pages %zu candidate %d outcome %u exact %d\n", queue, static_cast<unsigned long long>(program), variant.runs.size(), variant.runs.empty() ? 0ull : static_cast<unsigned long long>(variant.runs.front().first), clock != 0 ? 1 : 0, stable ? 1 : 0, pageCount, candidate ? 1 : 0, static_cast<unsigned>(outcome), exact ? 1 : 0);
    if (trace && epochs) std::fflush(stderr);
    if (!census) return outcome;
    const bool gated = imagesFlushed != flushedBefore || runsSynced != syncedBefore || DrawPipeline::DrainRequestsByOpcode(0x100) != drainsBefore;
    if (cls == Q0CensusClass::Hit) {
        if (outcome == EntryOutcome::Differing || outcome == EntryOutcome::Inaccessible) cls = Q0CensusClass::Differs;
        else if (outcome != EntryOutcome::Equal && outcome != EntryOutcome::EqualData) cls = Q0CensusClass::Refused;
        else if (!exact) cls = Q0CensusClass::Inexact;
        else if (gated) cls = Q0CensusClass::Gate;
    }
    const auto index = static_cast<std::size_t>(cls);
    ArchB::Recorder::Count(Q0CensusCountNames[index]);
    ArchB::Recorder::Count(Q0CensusNsNames[index], static_cast<double>(spent));
    ArchB::Recorder::Count("q0c_variant_runs", static_cast<double>(variant.runs.size()));
    ArchB::Recorder::Count("q0c_variant_words", static_cast<double>(variant.words.size()));
    if (exact) ArchB::Recorder::Count("q0c_variant_exact");
    if (gated) ArchB::Recorder::Count("q0c_variant_gated");
    return outcome;
}

}
