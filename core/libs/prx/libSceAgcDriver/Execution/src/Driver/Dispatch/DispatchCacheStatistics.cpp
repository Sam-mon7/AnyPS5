#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "Optimization/ResourceProgram.hpp"

namespace AgcDriver::DriverDetail {

void Driver::classifyDiffering(std::uint64_t program, std::uint64_t key, const DispatchVariant& old, const DispatchVariant& fresh, const ShaderRecompiler::ResourceCapture* capture, EntryCounters& counters) {
    ++counters.differingClassified;
    ++counters.differingByProgram[program];
    const auto valueSet = [](const DispatchVariant& variant) {
        std::uint64_t hash = 0xcbf29ce484222325ull;
        const auto mix = [&](std::uint64_t value) {
            hash ^= value;
            hash *= 0x100000001b3ull;
        };
        mix(variant.runs.size());
        for (const auto& [begin, end] : variant.runs) {
            mix(begin);
            mix(end);
        }
        mix(variant.words.size());
        for (const auto word : variant.words) mix(word);
        return hash;
    };
    auto& ring = priorValueSets[key];
    if (std::find(ring.begin(), ring.end(), valueSet(fresh)) != ring.end()) ++counters.differingMatchedPrior;
    ring.push_front(valueSet(old));
    while (ring.size() > 3) ring.pop_back();
    if (priorValueSets.size() > dispatchCacheEntries()) priorValueSets.clear();
    if (old.runs != fresh.runs || old.words.size() != fresh.words.size()) {
        ++counters.dataDifferingReasons[0];
        ++counters.differingRunsChanged;
        ++counters.differingWalk;
        return;
    }
    const auto baseWord = [&](const std::vector<ShaderRecompiler::DescriptorValue>& values, std::uint32_t value) {
        return std::any_of(values.begin(), values.end(), [&](const ShaderRecompiler::DescriptorValue& descriptor) { return descriptor.dwordCount >= 2 && (descriptor.dwords[0] == value || descriptor.dwords[1] == value); });
    };
    std::size_t words = 0, addressWords = 0, dataWords = 0, outsideMask = 0;
    std::size_t run = 0, first = 0;
    for (std::size_t i = 0; i < fresh.words.size(); ++i) {
        if (old.words[i] == fresh.words[i]) continue;
        while (run < fresh.runs.size() && i >= first + (fresh.runs[run].second - fresh.runs[run].first) / sizeof(std::uint32_t)) {
            first += (fresh.runs[run].second - fresh.runs[run].first) / sizeof(std::uint32_t);
            ++run;
        }
        const auto address = fresh.runs[run].first + (i - first) * sizeof(std::uint32_t);
        const auto masked = std::binary_search(old.dataPositions.begin(), old.dataPositions.end(), i);
        if (!masked) ++outsideMask;
        const auto leaf = capture != nullptr ? std::find_if(capture->readTrace.leaves.begin(), capture->readTrace.leaves.end(), [&](const auto& item) { return item.second == address; }) : std::vector<std::pair<std::uint32_t, std::uint64_t>>::const_iterator{};
        const bool isLeaf = capture != nullptr && leaf != capture->readTrace.leaves.end();
        const bool other = capture != nullptr && std::binary_search(capture->readTrace.otherReads.begin(), capture->readTrace.otherReads.end(), address);
        const auto role = masked ? 0u : isLeaf && other ? 2u : isLeaf ? 1u : other ? 3u : 4u;
        auto slot = isLeaf ? leaf->first : DispatchVariant::NoFlatBinding;
        if (!isLeaf && capture != nullptr && capture->plan != nullptr) {
            const auto observed = std::find_if(capture->readTrace.slotReads.begin(), capture->readTrace.slotReads.end(), [&](const auto& item) { return item.second == address; });
            if (observed != capture->readTrace.slotReads.end()) {
                const auto& reads = capture->plan->srtReads;
                const auto read = std::find_if(reads.begin(), reads.end(), [&](const auto& item) { return item.value != nullptr && item.value->Resolve() == observed->first; });
                if (read != reads.end()) slot = read->flatOffset;
            }
        }
        ++counters.differingWordRoles[program][{i, address, role, slot}];
        ++words;
        counters.differingPositions.insert(i);
        counters.differingFirstPosition = std::min(counters.differingFirstPosition, i);
        counters.differingLastPosition = std::max(counters.differingLastPosition, i);
        const auto value = fresh.words[i];
        if (capture != nullptr && (baseWord(capture->snapshot.buffers, value) || baseWord(capture->snapshot.images, value))) ++addressWords;
        else if (capture != nullptr && std::find(capture->snapshot.flattenedSrt.begin(), capture->snapshot.flattenedSrt.end(), value) != capture->snapshot.flattenedSrt.end()) ++dataWords;
    }
    counters.differingWords += words;
    ++counters.dataDifferingReasons[old.dataPositions.empty() ? 1u : outsideMask != 0 ? 2u : 3u];
    ++counters.differingWordBuckets[words <= 1 ? 0 : words <= 4 ? 1 : words <= 16 ? 2 : 3];
    if (words == 0) ++counters.differingWalk;
    else if (addressWords == words) ++counters.differingAddress;
    else if (dataWords == words) ++counters.differingData;
    else if (addressWords + dataWords == 0) ++counters.differingWalk;
    else ++counters.differingMixed;
}

void Driver::reportDispatchCache(EntryCounters& counters) {
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto validated = counters.lookups - counters.absent;
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] %llu lookups (10 s): %llu no entry, %llu validated by value in %.1f us each: %llu equal, %llu differing, %llu page not mapped, %llu queued label, %llu image being stored, %llu publish generation moved, %llu pending serial moved, %llu forget serial moved (%llu pending images stored first, %llu pending runs synced first, %llu equal entries inserted before a forget; gate retried %llu: %llu equal, %llu moved again); runs per entry %.1f validated / %.1f inserted; %llu misses found the entry replaced meanwhile; %llu inserts (%llu captures not kept: unstable), %llu evictions in total, %zu entries, %llu LRU moves\n", count(counters.lookups), count(counters.absent), count(validated), validated != 0 ? counters.validateUs / static_cast<double>(validated) : 0.0, count(counters.equal), count(counters.differing), count(counters.inaccessible), count(counters.queuedLabel), count(counters.flushingImage), count(counters.publishMoved), count(counters.pendingMoved), count(counters.forgetMoved), count(counters.imagesFlushed), count(counters.runsSynced), count(counters.forgetSinceInsert), count(counters.retriesEqual + counters.retriesMoved), count(counters.retriesEqual), count(counters.retriesMoved), validated != 0 ? static_cast<double>(counters.runsValidated) / static_cast<double>(validated) : 0.0, counters.inserts != 0 ? static_cast<double>(counters.runsInserted) / static_cast<double>(counters.inserts) : 0.0, count(counters.replaced), count(counters.inserts), count(counters.unstable), count(dispatchCacheEvictions), dispatchCache.size(), count(counters.touches));
    std::vector<std::pair<std::uint64_t, std::uint64_t>> programs(counters.differingByProgram.begin(), counters.differingByProgram.end());
    std::sort(programs.begin(), programs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string top;
    for (std::size_t i = 0; i < programs.size() && i < 8; ++i) {
        char text[48];
        std::snprintf(text, sizeof(text), " 0x%llx x%llu", static_cast<unsigned long long>(programs[i].first), count(programs[i].second));
        top += text;
    }
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] differing by class (10 s, %llu classified): address-only %llu, data-only %llu, walk %llu, mixed %llu (runs changed %llu); matched one of the last 3 value sets %llu; differing words %llu in total (misses with 1 / 2-4 / 5-16 / >16 words: %llu / %llu / %llu / %llu), %zu distinct positions (first %zu, last %zu); top programs by differing:%s\n", count(counters.differingClassified), count(counters.differingAddress), count(counters.differingData), count(counters.differingWalk), count(counters.differingMixed), count(counters.differingRunsChanged), count(counters.differingMatchedPrior), count(counters.differingWords), count(counters.differingWordBuckets[0]), count(counters.differingWordBuckets[1]), count(counters.differingWordBuckets[2]), count(counters.differingWordBuckets[3]), counters.differingPositions.size(), counters.differingPositions.empty() ? std::size_t{0} : counters.differingFirstPosition, counters.differingLastPosition, top.c_str());
    std::string ranks;
    for (std::size_t rank = 0; rank < dispatchVariants(); ++rank) {
        char text[32];
        std::snprintf(text, sizeof(text), "%s%llu", rank == 0 ? "" : " / ", count(counters.variantHitsByRank[rank]));
        ranks += text;
    }
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] variants (10 s, k = %zu): hits by rank 1..k %s; %.2f variants compared per validation; %llu inserted into an entry, %llu evicted beyond k; %.2f variants per entry (%llu over %zu entries, ~%.1f MiB)\n", dispatchVariants(), ranks.c_str(), validated != 0 ? static_cast<double>(counters.variantsCompared) / static_cast<double>(validated) : 0.0, count(counters.variantsInserted), count(counters.variantsEvicted), dispatchCache.empty() ? 0.0 : static_cast<double>(dispatchCacheVariants) / static_cast<double>(dispatchCache.size()), count(dispatchCacheVariants), dispatchCache.size(), static_cast<double>(dispatchCacheVariantBytes) / (1024.0 * 1024.0));
    std::string dataRanks;
    for (std::size_t rank = 0; rank < dispatchVariants(); ++rank) {
        char text[32];
        std::snprintf(text, sizeof(text), "%s%llu", rank == 0 ? "" : " / ", count(counters.dataHitsByRank[rank]));
        dataRanks += text;
    }
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] data hits (10 s): %llu (%llu words refreshed; by rank 1..k %s; %llu data variants missed on pending runs), verified %llu; inserts with data positions %llu of %llu (%.1f positions each), leaves skipped: unmapped %llu, mismatched %llu, aliased %llu\n", count(counters.dataHits), count(counters.dataWordsRefreshed), dataRanks.c_str(), count(dataPendingMisses.exchange(0, std::memory_order_relaxed)), count(counters.dataVerified), count(counters.dataInserts), count(counters.inserts), counters.dataInserts != 0 ? static_cast<double>(counters.dataPositionsInserted) / static_cast<double>(counters.dataInserts) : 0.0, count(counters.dataLeavesUnmapped), count(counters.dataLeavesMismatched), count(counters.dataLeavesAliased));
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] data insert reasons (10 s): positions %llu, disabled %llu, stamp validation %llu, no capture %llu, no leaves %llu, no flat binding %llu, leaves rejected %llu; no-leaf plan flags (overlap): incomplete %llu, specialization memory %llu, table columns %llu, no pure slots %llu\n", count(counters.dataInsertReasons[0]), count(counters.dataInsertReasons[1]), count(counters.dataInsertReasons[2]), count(counters.dataInsertReasons[3]), count(counters.dataInsertReasons[4]), count(counters.dataInsertReasons[5]), count(counters.dataInsertReasons[6]), count(counters.dataNoLeafReasons[0]), count(counters.dataNoLeafReasons[1]), count(counters.dataNoLeafReasons[2]), count(counters.dataNoLeafReasons[3]));
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] refined data inserts (10 s): %llu with %llu newly pure positions\n", count(counters.refinedDataInserts), count(counters.refinedDataPositions));
    for (const auto& [program, slots] : counters.flatSlotReasonsByProgram) {
        AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] flat slot reasons program 0x%llx (first insert this 10 s; address 0 = not read; reasons overlap):%s\n", count(program), slots.c_str());
    }
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] data differing reasons (10 s, first variant versus recapture): layout changed %llu, no recorded positions %llu, outside recorded positions %llu, all changes covered or no byte change %llu; validation failures (variant attempts, including draw stages): masked non-data/unmapped %llu, known pending %llu, raw pending %llu, synced pending %llu, legacy/nonpending %llu; sync prefilter failures (all variants) %llu\n", count(counters.dataDifferingReasons[0]), count(counters.dataDifferingReasons[1]), count(counters.dataDifferingReasons[2]), count(counters.dataDifferingReasons[3]), count(dataMissReasons[0].exchange(0, std::memory_order_relaxed)), count(dataMissReasons[1].exchange(0, std::memory_order_relaxed)), count(dataMissReasons[2].exchange(0, std::memory_order_relaxed)), count(dataMissReasons[3].exchange(0, std::memory_order_relaxed)), count(dataMissReasons[4].exchange(0, std::memory_order_relaxed)), count(dataMissReasons[5].exchange(0, std::memory_order_relaxed)));
    for (const auto& [site, counts] : counters.dataLookupsBySite) {
        AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] data site queue 0x%x %s (10 s): %llu entry lookups, %llu hits, %llu data hits\n", site.first, site.second ? "indirect" : "direct", count(counts[0]), count(counts[1]), count(counts[2]));
    }
    constexpr std::array<const char*, 5> roles{"recorded-data", "unrecorded-leaf", "aliased-leaf", "walk/descriptor", "untraced"};
    for (std::size_t index = 0; index < programs.size() && index < 8; ++index) {
        if (const auto inserts = counters.dataInsertsByProgram.find(programs[index].first); inserts != counters.dataInsertsByProgram.end()) {
            const auto& reasons = inserts->second;
            AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] data inserts program 0x%llx (10 s): positions %llu, disabled %llu, stamp validation %llu, no capture %llu, no leaves %llu, no flat binding %llu, leaves rejected %llu; no-leaf flags (overlap): incomplete %llu, specialization memory %llu, table columns %llu, no pure slots %llu\n", count(programs[index].first), count(reasons[0]), count(reasons[1]), count(reasons[2]), count(reasons[3]), count(reasons[4]), count(reasons[5]), count(reasons[6]), count(reasons[7]), count(reasons[8]), count(reasons[9]), count(reasons[10]));
        }
        const auto found = counters.differingWordRoles.find(programs[index].first);
        if (found == counters.differingWordRoles.end()) continue;
        std::vector<std::pair<std::tuple<std::size_t, std::uint64_t, std::uint32_t, std::uint32_t>, std::uint64_t>> details(found->second.begin(), found->second.end());
        std::sort(details.begin(), details.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string text;
        for (std::size_t word = 0; word < details.size() && word < 16; ++word) {
            const auto& [position, address, role, slot] = details[word].first;
            char item[160];
            std::snprintf(item, sizeof(item), " pos=%zu@0x%llx:%s:slot=%u x%llu", position, count(address), roles[role], slot, count(details[word].second));
            text += item;
        }
        AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] data words program 0x%llx (10 s, top 16 of %zu position/address/role/slot combinations; slot 4294967295 = unknown):%s\n", count(programs[index].first), details.size(), text.c_str());
    }
    counters = EntryCounters{};
}

}
