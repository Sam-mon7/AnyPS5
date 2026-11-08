#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include <cstdlib>
#include <cstring>

namespace AgcDriver::DriverDetail {

void Driver::lookupDispatch(std::uint64_t address, const Submission& submission, std::uint64_t key, bool pointerKey, bool noDispatchCache, bool traceCache, bool profile, std::span<const ShaderRecompiler::MemoryRegion> memory, DispatchPhaseTiming& phaseTiming, std::array<double, DriverPhaseCount>& phaseMs, std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, std::shared_ptr<DispatchVariant>& keepVariant, std::vector<ShaderRecompiler::MemoryRegion>& captured, std::vector<std::uint32_t>& liveWords, bool& dataHit, bool& cached, bool& validated, std::shared_ptr<DispatchEntry>& missedEntry, bool& missedDiffering, bool feedbackMask, std::uint64_t feedbackKey, std::span<const std::uint32_t> userData) {
    if (!noDispatchCache) {

        static const bool validateUnlocked = std::getenv("APS5_NO_UNLOCKED_VALIDATE") == nullptr;
        std::unique_lock cacheLock(dispatchCacheMutex);
        ++entryCounters.lookups;
        auto found = dispatchCache.find(key);
        if (found != dispatchCache.end() && found->second->pointerKey != pointerKey) found = dispatchCache.end();
        bool aliasLookup = false;
        if (!pointerKey && found == dispatchCache.end() && feedbackMask && feedbackKey != 0 && !stampValidate()) {
            const auto alias = dispatchFeedbackKeys.find(feedbackKey);
            if (alias != dispatchFeedbackKeys.end()) {
                found = dispatchCache.find(alias->second);
                if (found != dispatchCache.end()) { key = alias->second; aliasLookup = true; }
            }
        }
        std::shared_ptr<DispatchEntry> cacheEntry = found != dispatchCache.end() ? found->second : nullptr;
        std::shared_ptr<DispatchEntry> entry = cacheEntry;
        if (entry == nullptr) ++entryCounters.absent;
        // APS5_ARCHB_FIX_DISPATCH_RELOCATION: a moved user-data base whose content an entry holds.
        bool relocated = false;
        if (!pointerKey && entry == nullptr && feedbackKey != 0 && !stampValidate() && ArchB::Switches::Enabled(ArchB::Feature::FixDispatchRelocation)) {
            entry = relocateDispatchEntry(cacheLock, feedbackKey, userData);
            relocated = entry != nullptr;
        }
        if (pointerKey && entry != nullptr) {
            entry = rebaseDispatchEntry(entry, userData);
            if (entry == nullptr) ArchB::Recorder::Count("fix_dptr_refused_rebase");
        }
        if (validateUnlocked) cacheLock.unlock();
        phaseTiming.Phase(PhaseLookup);
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
            std::uint64_t imagesFlushed = 0, runsSynced = 0, retriesEqual = 0, retriesMoved = 0, compared = 0, runsCompared = 0;
            std::vector<ShaderRecompiler::MemoryRegion> regions;

            std::vector<std::pair<std::uint32_t, std::uint32_t>> liveData;
            std::vector<std::byte> pointerBytes;
            bool pointerPrefiltered = false;
            const auto pointerCandidate = [&](const DispatchVariant& candidate, std::span<const ShaderRecompiler::MemoryRegion> candidateRegions) {
                if (!pointerKey) return true;
                std::size_t position = 0;
                for (const auto& region : candidateRegions) {
                    pointerBytes.resize(region.bytes.size());
                    if (GuestMemory::CopyMapped(region.guestAddress, pointerBytes) != GuestMemory::Compare::Equal) return true;
                    const auto count = region.bytes.size() / sizeof(std::uint32_t);
                    for (std::size_t word = 0; word < count; ++word) {
                        std::uint32_t stored = 0, fresh = 0;
                        std::memcpy(&stored, region.bytes.data() + word * sizeof(std::uint32_t), sizeof(stored));
                        std::memcpy(&fresh, pointerBytes.data() + word * sizeof(std::uint32_t), sizeof(fresh));
                        const auto difference = stored ^ fresh;
                        if (difference == 0u || std::binary_search(candidate.dataPositions.begin(), candidate.dataPositions.end(), position + word) || (feedbackMask && (difference & ~WordMask(candidate.feedback.memory, position + word)) == 0u)) continue;
                        return false;
                    }
                    position += count;
                }
                return true;
            };
            const auto waitedBeforeValidate = profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
            if (!stampValidate()) {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);

                std::optional<SampledReadScope> sampling;

                for (std::size_t i = 0; i < variants.size(); ++i) {
                    if (aliasLookup && variants[i]->userData.empty()) continue;
                    bool userFeedback = false, memoryFeedback = false;
                    if (feedbackMask && !variants[i]->userData.empty() && !FeedbackWordsEqual(variants[i]->userData, userData, variants[i]->feedback.userData, &userFeedback)) continue;
                    regions.clear();
                    appendEntryRegions(*variants[i], regions);
                    if (!pointerCandidate(*variants[i], regions)) {
                        pointerPrefiltered = true;
                        continue;
                    }
                    ++compared;
                    runsCompared += variants[i]->runs.size();
                    const bool genShadow = GuestMemory::Gen::ShadowActive();
                    const auto genBefore = genShadow ? GuestMemory::Gen::CaptureSince() : 0;
                    auto result = validateVariant(address, submission.queue, *variants[i], regions, imagesFlushed, runsSynced, sampling, &liveData, feedbackMask, &memoryFeedback, false, GuestMemory::Gen::Consumer::DispatchValidate, pointerKey);

                    if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) {
                        result = validateVariant(address, submission.queue, *variants[i], regions, imagesFlushed, runsSynced, sampling, &liveData, feedbackMask, &memoryFeedback, false, GuestMemory::Gen::Consumer::DispatchValidate, pointerKey);
                        ++(result == EntryOutcome::Equal || result == EntryOutcome::EqualData ? retriesEqual : retriesMoved);
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
                    if (pointerKey) break;
                }
                current = variant != nullptr;
            } else {
                variant = variants.front();
                compared = 1;
                runsCompared = variant->runs.size();
                current = true;
                const GuestMemory::Gen::ConsumerScope genConsumer(GuestMemory::Gen::Consumer::DispatchSpans);
                for (const auto& [begin, bytes] : variant->spans) {
                    GuestMemory::CollectWrites(begin, bytes);
                    if (!GuestMemory::UnchangedSince(begin, bytes, generation)) {
                        current = false;
                        break;
                    }
                }
                if (!current) {

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
            phaseTiming.Phase(PhaseValidate);
            if (profile) {
                const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBeforeValidate, phaseMs[PhaseValidate]);
                phaseMs[PhaseValidate] -= waited;
                phaseMs[PhaseValidateWait] += waited;
            }
            if (!cacheLock.owns_lock()) cacheLock.lock();

            const auto again = dispatchCache.find(key);
            const bool untouched = again != dispatchCache.end() && again->second == cacheEntry && variants.front()->generation.load(std::memory_order_acquire) == generation;
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
            counters.runsValidated += runsCompared;
            counters.retriesEqual += retriesEqual;
            counters.retriesMoved += retriesMoved;
            counters.variantsCompared += compared;
            switch (outcome) {
                case EntryOutcome::Equal: ++counters.equal; break;
                case EntryOutcome::EqualData: ++counters.equal; break;
                case EntryOutcome::Differing: ++counters.differing; break;
                case EntryOutcome::Inaccessible: ++counters.inaccessible; break;
                case EntryOutcome::QueuedLabel: ++counters.queuedLabel; break;
                case EntryOutcome::FlushingImage: ++counters.flushingImage; break;
                case EntryOutcome::PublishMoved: ++counters.publishMoved; break;
                case EntryOutcome::PendingMoved: ++counters.pendingMoved; break;
                case EntryOutcome::ForgetMoved: ++counters.forgetMoved; break;
            }
            if (current) {
                if (relocated || pointerKey) applyShaderDataPatches(*variant);
                if (relocated) {
                    ArchB::Recorder::Count("fix_dispatch_relocation_hits");
                    if (dataHit) ArchB::Recorder::Count("fix_dispatch_relocation_data_hits");
                }
                if (feedbackHit) ArchB::Recorder::Count("fix_tsharp_mask_hits");
                if (pointerKey) {
                    ArchB::Recorder::Count("fix_dptr_served");
                    ArchB::Recorder::Count("fix_dptr_rebased_regions", variant->rebasedRegions);
                }
                ++counters.variantHitsByRank[rank];
                compiledResult = variant->compiled;
                if (dataHit) {

                    liveWords = variant->words;
                    for (const auto& [position, value] : liveData) liveWords[position] = value;
                    regions.clear();
                    appendEntryRegions(*variant, regions, &liveWords);
                    compiledResult = patchDataWords(*variant, liveWords);
                    ++counters.dataHits;
                    counters.dataWordsRefreshed += liveData.size();
                    ++counters.dataHitsByRank[rank];
                    if (pointerKey) ArchB::Recorder::Count("fix_dptr_data_refreshed", liveData.size());
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
                    if (restamped != 0) {
                        variant->generation.store(restamped, std::memory_order_release);
                        if (pointerKey && rank < cacheEntry->variants.size()) cacheEntry->variants[rank]->generation.store(restamped, std::memory_order_release);
                    }

                    if (rank != 0 && !pointerKey) {
                        auto rotated = std::make_shared<DispatchEntry>();
                        rotated->pointerKey = cacheEntry->pointerKey;
                        rotated->variants.reserve(variants.size());
                        rotated->variants.push_back(variants[rank]);
                        for (std::size_t i = 0; i < variants.size(); ++i) {
                            if (i != rank) rotated->variants.push_back(variants[i]);
                        }
                        rotated->touched = cacheEntry->touched;
                        rotated->order = cacheEntry->order;
                        again->second = std::move(rotated);
                    }

                    if (dispatchCacheHits - again->second->touched > dispatchCacheEntries() / 8) {
                        dispatchOrder.splice(dispatchOrder.begin(), dispatchOrder, again->second->order);
                        again->second->touched = dispatchCacheHits;
                        ++counters.touches;
                    }
                }
            } else {
                if (pointerKey) ArchB::Recorder::Count(compared == 0 && pointerPrefiltered ? "fix_dptr_refused_content" : "fix_dptr_refused_validation");
                if (traceCache) std::fprintf(stderr, "[dispatch-cache] 0x%llx captured memory changed\n", static_cast<unsigned long long>(address));

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
