#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchDetailTiming.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

bool Driver::dispatchRelocationRangesValid(const DispatchVariant& variant) {
    if (variant.runs.empty()) return false;
    std::size_t words = 0;
    for (std::size_t run = 0; run < variant.runs.size(); ++run) {
        const auto [begin, end] = variant.runs[run];
        if (end <= begin || (begin & 3u) != 0 || (end & 3u) != 0 || end - 1u > 0x0000ffffffffffffull || (end - begin) / sizeof(std::uint32_t) > variant.words.size() - words) return false;
        for (std::size_t previous = 0; previous < run; ++previous) if (begin < variant.runs[previous].second && variant.runs[previous].first < end) return false;
        words += static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
    }
    return words == variant.words.size();
}

std::shared_ptr<DispatchEntry> Driver::relocateDispatchRecipeEntry(std::unique_lock<AgcDriver::Mutex>& cacheLock, std::uint64_t address, std::uint64_t identity, std::span<const std::uint32_t> userData) {
    DispatchDetailTiming detail("Dispatch.Detail.RelocationCandidate");
    auto relocated = relocateDispatchEntry(cacheLock, identity, userData);
    if (relocated == nullptr) return nullptr;
    const auto shape = dispatchRelocations.find(identity);
    if (shape == dispatchRelocations.end()) return nullptr;
    bool readable = true;
    const auto alias = dispatchAliases.find(dispatchAliasKey(identity, userData, *shape->second, readable));
    if (!readable || alias == dispatchAliases.end()) return nullptr;
    const auto cached = dispatchCache.find(alias->second);
    if (cached == dispatchCache.end()) return nullptr;
    std::erase_if(relocated->variants, [&](const auto& copy) {
        if (!dispatchRelocationRangesValid(*copy)) {
            ArchB::Recorder::Count("dispatch_relocation_refused_range");
            detail.Point("dispatch.relocation_recipe.range_refused", address);
            return true;
        }
        for (const auto& donor : cached->second->variants) {
            if (donor->compiled != copy->compiled || donor->words != copy->words || donor->runBases != copy->runBases || donor->runs.size() != copy->runs.size() || donor->runBases.size() != donor->runs.size() || donor->userData.size() != userData.size()) continue;
            if (donor->recipe.load(std::memory_order_acquire) == nullptr) {
                ArchB::Recorder::Count("dispatch_relocation_recipe_missing");
                const auto failure = donor->recipeFailure.load(std::memory_order_relaxed);
                constexpr std::array<const char*, static_cast<std::size_t>(RecipeBuildFailure::Count)> reasons{
                    "dispatch_relocation_missing_not_attempted", "dispatch_relocation_missing_built", "dispatch_relocation_missing_disabled", "dispatch_relocation_missing_uncacheable", "dispatch_relocation_missing_overlay", "dispatch_relocation_missing_address_table", "dispatch_relocation_missing_lease", "dispatch_relocation_missing_copied_writes", "dispatch_relocation_missing_not_reusable", "dispatch_relocation_missing_completion", "dispatch_relocation_missing_objects", "dispatch_relocation_missing_key"};
                if (static_cast<std::size_t>(failure) < reasons.size()) ArchB::Recorder::Count(reasons[static_cast<std::size_t>(failure)]);
                detail.Point("dispatch.relocation_recipe.missing", address, static_cast<std::uint64_t>(failure));
                continue;
            }
            bool same = true;
            for (std::size_t run = 0; run < donor->runs.size(); ++run) {
                const auto base = donor->runBases[run];
                std::uint64_t delta = 0;
                if (base >= 0) {
                    if (static_cast<std::size_t>(base) >= userData.size()) { same = false; break; }
                    delta = std::uint64_t{userData[base] & ~3u} - std::uint64_t{donor->userData[base] & ~3u};
                }
                if (copy->runs[run].first != donor->runs[run].first + delta || copy->runs[run].second != donor->runs[run].second + delta) { same = false; break; }
            }
            if (!same) continue;
            copy->donor = donor;
            return false;
        }
        return true;
    });
    if (relocated->variants.empty()) {
        ArchB::Recorder::Count("dispatch_relocation_recipe_no_donor");
        detail.Point("dispatch.relocation_recipe.no_donor", address);
        return nullptr;
    }
    ArchB::Recorder::Count("dispatch_relocation_recipe_candidate");
    detail.Point("dispatch.relocation_recipe.candidate", address, relocated->variants.size());
    return relocated;
}

void Driver::lookupDispatch(std::uint64_t address, const Submission& submission, std::uint64_t key, bool noDispatchCache, bool traceCache, bool profile, std::span<const ShaderRecompiler::MemoryRegion> memory, DispatchPhaseTiming& phaseTiming, std::array<double, DriverPhaseCount>& phaseMs, std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, std::shared_ptr<DispatchVariant>& keepVariant, std::vector<ShaderRecompiler::MemoryRegion>& captured, std::vector<std::uint32_t>& liveWords, bool& dataHit, bool& cached, bool& validated, std::shared_ptr<DispatchEntry>& missedEntry, bool& missedDiffering, bool feedbackMask, std::uint64_t feedbackKey, std::span<const std::uint32_t> userData, const std::shared_ptr<DispatchEntry>& relocatedEntry) {
    DispatchDetailTiming detail("Dispatch.Detail.LookupLockFind");
    if (!noDispatchCache) {

        static const bool validateUnlocked = std::getenv("APS5_NO_UNLOCKED_VALIDATE") == nullptr;
        std::unique_lock cacheLock(dispatchCacheMutex);
        ++entryCounters.lookups;
        auto found = dispatchCache.find(key);
        bool aliasLookup = false;
        if (relocatedEntry == nullptr && found == dispatchCache.end() && feedbackMask && feedbackKey != 0 && !stampValidate()) {
            const auto alias = dispatchFeedbackKeys.find(feedbackKey);
            if (alias != dispatchFeedbackKeys.end()) {
                found = dispatchCache.find(alias->second);
                if (found != dispatchCache.end()) { key = alias->second; aliasLookup = true; }
            }
        }
        std::shared_ptr<DispatchEntry> entry = relocatedEntry != nullptr ? relocatedEntry : found != dispatchCache.end() ? found->second : nullptr;
        if (entry == nullptr) ++entryCounters.absent;
        detail.Point(entry == nullptr ? "dispatch.lookup.absent" : aliasLookup ? "dispatch.lookup.alias" : "dispatch.lookup.exact", address, key);
        // APS5_ARCHB_FIX_DISPATCH_RELOCATION: a moved user-data base whose content an entry holds.
        bool relocated = relocatedEntry != nullptr;
        if (entry == nullptr && feedbackKey != 0 && !stampValidate() && ArchB::Switches::Enabled(ArchB::Feature::FixDispatchRelocation)) {
            entry = relocateDispatchEntry(cacheLock, feedbackKey, userData);
            relocated = entry != nullptr;
            detail.Point("dispatch.lookup.relocated", address, relocated);
        }
        if (validateUnlocked) cacheLock.unlock();
        phaseTiming.Phase(PhaseLookup);
        detail.Next("Dispatch.Detail.Validate");
        if (entry != nullptr) {
            validated = true;
            const auto& variants = entry->variants;

            std::shared_ptr<DispatchVariant> variant;
            std::size_t rank = 0;
            const auto generation = variants.front()->generation.load(std::memory_order_acquire);
            bool current = false;
            bool feedbackHit = false;
            if (feedbackMask && stampValidate()) ArchB::Recorder::Count("fix_tsharp_mask_refused_stamp");
            std::uint64_t restamped = 0;

            auto outcome = EntryOutcome::Differing;
            std::uint64_t imagesFlushed = 0, runsSynced = 0, retriesEqual = 0, retriesMoved = 0, compared = 0;
            std::vector<ShaderRecompiler::MemoryRegion> regions;

            std::vector<std::pair<std::uint32_t, std::uint32_t>> liveData;
            const auto waitedBeforeValidate = profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
            if (!stampValidate()) {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);

                std::optional<SampledReadScope> sampling;

                for (std::size_t i = 0; i < variants.size(); ++i) {
                    if (relocated && !dispatchRelocationRangesValid(*variants[i])) {
                        ArchB::Recorder::Count("dispatch_relocation_refused_range");
                        detail.Point("dispatch.relocation_recipe.range_refused", address);
                        continue;
                    }
                    if (variants[i]->registeredViews != nullptr && !CaptureRegisteredViewsCurrent(*variants[i]->registeredViews)) {
                        detail.Point("dispatch.validation.registered_views", i);
                        if (variants[i]->donor != nullptr) ArchB::Recorder::Count("dispatch_relocation_validation_registered_views");
                        continue;
                    }
                    if (aliasLookup && variants[i]->userData.empty()) {
                        detail.Point("dispatch.validation.alias_empty", i);
                        continue;
                    }
                    bool userFeedback = false, memoryFeedback = false;
                    if (feedbackMask && !variants[i]->userData.empty() && !FeedbackWordsEqual(variants[i]->userData, userData, variants[i]->feedback.userData, &userFeedback)) {
                        detail.Point("dispatch.validation.user_words", i);
                        if (variants[i]->donor != nullptr) ArchB::Recorder::Count("dispatch_relocation_validation_user_words");
                        continue;
                    }
                    regions.clear();
                    appendEntryRegions(*variants[i], regions);
                    ++compared;
                    const bool genShadow = GuestMemory::Gen::ShadowActive();
                    const auto genBefore = genShadow ? GuestMemory::Gen::CaptureSince() : 0;
                    auto result = validateVariant(address, submission.queue, *variants[i], regions, imagesFlushed, runsSynced, sampling, &liveData, feedbackMask, &memoryFeedback, false, GuestMemory::Gen::Consumer::DispatchValidate, &liveWords);

                    if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) {
                        result = validateVariant(address, submission.queue, *variants[i], regions, imagesFlushed, runsSynced, sampling, &liveData, feedbackMask, &memoryFeedback, false, GuestMemory::Gen::Consumer::DispatchValidate, &liveWords);
                        ++(result == EntryOutcome::Equal || result == EntryOutcome::EqualData ? retriesEqual : retriesMoved);
                    }
                    detail.Point("dispatch.validation", i, static_cast<std::uint64_t>(result));
                    if (variants[i]->donor != nullptr) {
                        constexpr std::array<const char*, 11> reasons{
                            "dispatch_relocation_validation_equal", "dispatch_relocation_validation_equal_data", "dispatch_relocation_validation_differing", "dispatch_relocation_validation_queued_stores", "dispatch_relocation_validation_inaccessible", "dispatch_relocation_validation_queued_label", "dispatch_relocation_validation_flushing_image", "dispatch_relocation_validation_publish_moved", "dispatch_relocation_validation_pending_moved", "dispatch_relocation_validation_forget_moved", "dispatch_relocation_validation_unknown"};
                        const auto outcomeIndex = std::min<std::size_t>(static_cast<std::size_t>(result), reasons.size() - 1);
                        ArchB::Recorder::Count(reasons[outcomeIndex]);
                        detail.Point("dispatch.relocation_recipe.validation", address, static_cast<std::uint64_t>(result));
                    }
                    if (genShadow) static_cast<void>(ShadowValidatedVariant(GuestMemory::Gen::Consumer::DispatchValidate, *variants[i], result, genBefore, VariantValidationExact()));
                    if (i == 0) outcome = result;
                    if (result == EntryOutcome::Equal || result == EntryOutcome::EqualData) {
                        outcome = result;
                        variant = variants[i];
                        rank = i;
                        dataHit = result == EntryOutcome::EqualData;
                        feedbackHit = userFeedback || memoryFeedback;
                        break;
                    }
                }
                current = variant != nullptr;
            } else {
                variant = variants.front();
                current = variant->registeredViews == nullptr || CaptureRegisteredViewsCurrent(*variant->registeredViews);
                if (!current) variant.reset();
                if (variant != nullptr) {
                    compared = 1;
                    current = true;
                    const GuestMemory::Gen::ConsumerScope genConsumer(GuestMemory::Gen::Consumer::DispatchSpans);
                    for (const auto& [begin, bytes] : variant->spans) {
                        GuestMemory::CollectWrites(begin, bytes);
                        if (!GuestMemory::UnchangedSince(begin, bytes, generation)) {
                            current = false;
                            break;
                        }
                    }
                }
                if (!current && variant != nullptr) {

                    std::uint64_t collected = 0;
                    for (const auto& [begin, bytes] : variant->spans) collected = std::max(collected, GuestMemory::CollectWrites(begin, bytes));
                    bool same = collected != 0;
                    if (same) {
                        const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);
                        PendingView pending;
                        pending.Load();
                        same = validateCaptured(address, submission.queue, variant->captured, *variant->compiled, false, pending);
                    }
                    if (same) {
                        restamped = collected;
                        current = true;
                        // The tracker said changed, the bytes are equal: the restamp CompareAndRestamp generalizes.
                        if (GuestMemory::Gen::ShadowActive()) GuestMemory::Gen::CountRestamp(GuestMemory::Gen::Consumer::DispatchSpans);
                    }
                }
                if (current) outcome = EntryOutcome::Equal;
            }
            detail.Point("dispatch.validation.final", address, static_cast<std::uint64_t>(outcome));
            detail.Point("dispatch.validation.current", address, (current ? 1u : 0u) | (stampValidate() ? 2u : 0u));
            phaseTiming.Phase(PhaseValidate);
            if (profile) {
                const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBeforeValidate, phaseMs[PhaseValidate]);
                phaseMs[PhaseValidate] -= waited;
                phaseMs[PhaseValidateWait] += waited;
            }
            detail.Next("Dispatch.Detail.LookupRelock");
            if (!cacheLock.owns_lock()) cacheLock.lock();

            const auto again = dispatchCache.find(key);
            const bool untouched = again != dispatchCache.end() && again->second == entry && variants.front()->generation.load(std::memory_order_acquire) == generation;
            auto& counters = entryCounters;
            if (profile) {
                auto& site = counters.dataLookupsBySite[{submission.queue, indirectSource().arguments != 0}];
                ++site[0];
                if (current) ++site[1];
                if (dataHit) ++site[2];
            }
            counters.validateUs += phaseMs[PhaseValidate] * 1000;
            counters.imagesFlushed += imagesFlushed;
            counters.runsSynced += runsSynced;
            for (std::size_t i = 0; i < compared && i < variants.size(); ++i) counters.runsValidated += variants[i]->runs.size();
            counters.retriesEqual += retriesEqual;
            counters.retriesMoved += retriesMoved;
            counters.variantsCompared += compared;
            switch (outcome) {
                case EntryOutcome::Equal: ++counters.equal; break;
                case EntryOutcome::EqualData: ++counters.equal; break;
                case EntryOutcome::Differing: ++counters.differing; break;
                case EntryOutcome::QueuedStores: ++counters.queuedStores; break;
                case EntryOutcome::Inaccessible: ++counters.inaccessible; break;
                case EntryOutcome::QueuedLabel: ++counters.queuedLabel; break;
                case EntryOutcome::FlushingImage: ++counters.flushingImage; break;
                case EntryOutcome::PublishMoved: ++counters.publishMoved; break;
                case EntryOutcome::PendingMoved: ++counters.pendingMoved; break;
                case EntryOutcome::ForgetMoved: ++counters.forgetMoved; break;
            }
            if (current) {
                detail.Next("Dispatch.Detail.HitMaterialize");
                if (relocated) {
                    applyShaderDataPatches(*variant);
                    ArchB::Recorder::Count("fix_dispatch_relocation_hits");
                    if (dataHit) ArchB::Recorder::Count("fix_dispatch_relocation_data_hits");
                }
                if (feedbackHit) ArchB::Recorder::Count("fix_tsharp_mask_hits");
                ++counters.variantHitsByRank[rank];
                compiledResult = variant->compiled;
                if (dataHit) {

                    if (liveWords.empty()) liveWords = variant->words;
                    for (const auto& [position, value] : liveData) liveWords[position] = value;
                    compiledResult = patchDataWords(*variant, liveWords);
                    ++counters.dataHits;
                    counters.dataWordsRefreshed += liveData.size();
                    ++counters.dataHitsByRank[rank];
                }
                if (variant->donor != nullptr) {
                    const auto recipe = VulkanDevice::RelocateDispatchRecipe(*variant->donor->compiled, *compiledResult, variant->donor->recipe.load(std::memory_order_acquire));
                    if (recipe != nullptr) {
                        variant->recipe.store(recipe, std::memory_order_release);
                        ArchB::Recorder::Count("dispatch_relocation_recipe_hit");
                        detail.Point("dispatch.relocation_recipe.hit", address);
                    } else {
                        ArchB::Recorder::Count("dispatch_relocation_recipe_refused");
                        detail.Point("dispatch.relocation_recipe.refused", address);
                    }
                }
                if (!liveWords.empty()) {
                    regions.clear();
                    appendEntryRegions(*variant, regions, &liveWords);
                }
                if (stampValidate()) {
                    captured = variant->captured;
                } else {

                    captured.reserve(memory.size() + regions.size());
                    captured.assign(memory.begin(), memory.end());
                    captured.insert(captured.end(), regions.begin(), regions.end());
                    if (variant->forgetSerial != GuestMemory::ForgetSerial()) ++counters.forgetSinceInsert;
                }
                keepVariant = variant;
                cached = true;
                ++dispatchCacheHits;
                if (untouched) {
                    if (restamped != 0) variant->generation.store(restamped, std::memory_order_release);

                    if (rank != 0) {
                        auto rotated = std::make_shared<DispatchEntry>();
                        rotated->variants.reserve(variants.size());
                        rotated->variants.push_back(variant);
                        for (std::size_t i = 0; i < variants.size(); ++i) {
                            if (i != rank) rotated->variants.push_back(variants[i]);
                        }
                        rotated->touched = entry->touched;
                        rotated->order = entry->order;
                        again->second = std::move(rotated);
                    }

                    if (dispatchCacheHits - again->second->touched > dispatchCacheEntries() / 8) {
                        dispatchOrder.splice(dispatchOrder.begin(), dispatchOrder, again->second->order);
                        again->second->touched = dispatchCacheHits;
                        ++counters.touches;
                    }
                }
            } else {
                if (traceCache) std::fprintf(stderr, "[dispatch-cache] 0x%llx %s\n", static_cast<unsigned long long>(address), outcome == EntryOutcome::QueuedStores ? "queued stores over the captured memory" : "captured memory changed");

                missedEntry = entry;
                missedDiffering = outcome == EntryOutcome::Differing;
                if (!untouched) ++counters.replaced;
            }
            if (profile && std::chrono::steady_clock::now() - counters.lastReport > std::chrono::seconds(10)) {
                counters.lastReport = std::chrono::steady_clock::now();
                reportDispatchCache(counters);
            }
            phaseTiming.Phase(PhaseRelock);
        }
    }
}

}
