#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/SrtWalker/SrtFlatSlotClasses.hpp"
#include "ImageTableAbi.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::DriverDetail {

bool CacheableResult(const ShaderRecompiler::RecompileResult& compiled) {
    return compiled.poisonedSrtReads == 0u && std::none_of(compiled.imageTablePoison.begin(), compiled.imageTablePoison.end(), [](const ShaderRecompiler::ImageTableEntryPoison& poison) {
        return poison.reason == static_cast<std::uint32_t>(ShaderRecompiler::ImageTableAbi::PoisonReason::Unmapped) || poison.reason == static_cast<std::uint32_t>(ShaderRecompiler::ImageTableAbi::PoisonReason::Inactive);
    });
}

void Driver::insertDispatch(std::uint64_t address, std::uint64_t key, bool noDispatchCache, bool profile, const std::shared_ptr<const ShaderSnapshot>& registeredShader, std::uint64_t forgetAtCapture, std::span<const ShaderRecompiler::MemoryRegion> memory, const std::shared_ptr<ShaderMemory>& shaderMemory, const std::vector<ShaderRecompiler::MemoryRegion>& captured, const std::shared_ptr<const ShaderRecompiler::ResourceCapture>& capture, const std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, const std::shared_ptr<DispatchEntry>& missedEntry, bool missedDiffering, std::shared_ptr<DispatchVariant>& attachVariant, DispatchPhaseTiming& phaseTiming) {
    if (!noDispatchCache) {
        auto fresh = std::make_shared<DispatchVariant>();
        fresh->compiled = compiledResult;
        fresh->shader = registeredShader;
        fresh->forgetSerial = forgetAtCapture;

        std::vector<ShaderRecompiler::MemoryRegion> srtRegions;
        std::size_t capturedWords = 0;
        for (const auto& region : captured) {
            const bool registered = std::any_of(memory.begin(), memory.end(), [&](const auto& known) { return region.guestAddress >= known.guestAddress && region.guestAddress < known.guestAddress + known.bytes.size(); });
            if (registered) continue;
            srtRegions.push_back(region);
            capturedWords += region.bytes.size() / sizeof(std::uint32_t);
        }
        const auto supplied = shaderMemory->SuppliedBlocks();
        fresh->runs.reserve(srtRegions.size());
        fresh->words.reserve(capturedWords);
        for (const auto& region : srtRegions) {
            fresh->runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
            const auto count = region.bytes.size() / sizeof(std::uint32_t);
            const auto offset = fresh->words.size();
            fresh->words.resize(offset + count);
            std::memcpy(fresh->words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
            AppendSharedWords(fresh->shared, fresh->runs.size() - 1u, offset, region.bytes, supplied);
        }

        DataWordPositionCounts dataCounts;
        if (dataHits() && !stampValidate() && capture != nullptr && !capture->readTrace.leaves.empty()) {
            for (std::size_t b = 0; b < compiledResult->bindings.size(); ++b) {
                if (compiledResult->bindings[b].role != ShaderRecompiler::DescriptorRole::FlattenedSrt) continue;
                fresh->flatBinding = static_cast<std::uint32_t>(b);
                dataCounts = DataWordPositions(fresh->runs, capture->readTrace.leaves, capture->readTrace.otherReads, fresh->words, compiledResult->bindings[b].guestDescriptor, fresh->dataPositions, fresh->dataSlots);
                break;
            }
        }
        bool stable = true;
        if (stampValidate()) {

            fresh->memory = shaderMemory;
            fresh->captured = captured;
            for (const auto& region : captured) {
                const auto begin = region.guestAddress;
                const auto end = begin + region.bytes.size();
                if (!fresh->spans.empty() && begin <= fresh->spans.back().first + fresh->spans.back().second + 65536) {
                    auto& last = fresh->spans.back();
                    last.second = static_cast<std::size_t>(std::max(last.first + last.second, end) - last.first);
                } else {
                    fresh->spans.emplace_back(begin, static_cast<std::size_t>(end - begin));
                }
            }
            std::uint64_t generation = 0;
            for (const auto& [begin, bytes] : fresh->spans) generation = std::max(generation, GuestMemory::CollectWrites(begin, bytes));
            stable = generation != 0;
            if (stable) {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);
                stable = captureStable(captured);
            }
            fresh->generation.store(generation, std::memory_order_release);
        } else if (insertCompare()) {
            const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);
            stable = captureStable(srtRegions);
        }
        std::lock_guard cacheLock(dispatchCacheMutex);
        if (profile && missedEntry != nullptr && missedDiffering) classifyDiffering(address, key, *missedEntry->variants.front(), *fresh, capture.get(), entryCounters);
        if (stable) {
            if (profile) {
                if (capture != nullptr && capture->plan != nullptr) {
                    const auto& plan = *capture->plan;
                    auto [classified, inserted] = entryCounters.flatSlotReasonsByPlan.try_emplace(capture->plan);
                    if (inserted) classified->second = ShaderRecompiler::Detail::ComputeFlatSlotReasons(plan);
                    const auto& reasons = classified->second;
                    std::size_t refined = 0;
                    for (const auto slot : fresh->dataSlots) {
                        if (slot < reasons.size() && reasons[slot] == ShaderRecompiler::Detail::UnusedDescriptor) ++refined;
                    }
                    if (refined != 0u) {
                        ++entryCounters.refinedDataInserts;
                        entryCounters.refinedDataPositions += refined;
                    }
                    if (!entryCounters.flatSlotReasonsByProgram.contains(address)) {
                        std::string details;
                        constexpr std::array<std::pair<std::uint16_t, const char*>, 9> names{{
                            {1u, "incomplete"}, {2u, "specialization"}, {4u, "table-column"}, {8u, "invalid-fill"},
                            {16u, "descriptor"}, {32u, "unused-descriptor"}, {64u, "branch"}, {128u, "uniform-fill"}, {256u, "address"}
                        }};
                        for (std::size_t slot = 0; slot < reasons.size(); ++slot) {
                            const auto* raw = plan.srtReads[slot].value;
                            const auto found = std::find_if(capture->readTrace.slotReads.begin(), capture->readTrace.slotReads.end(), [&](const auto& item) { return raw != nullptr && item.first == raw->Resolve(); });
                            const auto readAddress = found == capture->readTrace.slotReads.end() ? 0ull : static_cast<unsigned long long>(found->second);
                            std::string uses;
                            for (const auto& [flag, name] : names) {
                                if ((reasons[slot] & flag) == 0u) continue;
                                if (!uses.empty()) uses += "|";
                                uses += name;
                            }
                            char item[256];
                            std::snprintf(item, sizeof(item), " slot=%u@0x%llx:%s:selected=%s", plan.srtReads[slot].flatOffset, readAddress, uses.empty() ? "pure" : uses.c_str(), slot < plan.pureFlatSlots.size() && plan.pureFlatSlots[slot] != 0u ? "pure" : "impure");
                            details += item;
                        }
                        entryCounters.flatSlotReasonsByProgram.emplace(address, std::move(details));
                    }
                }
                const auto reason = !fresh->dataPositions.empty() ? 0u : !dataHits() ? 1u : stampValidate() ? 2u : capture == nullptr ? 3u : capture->readTrace.leaves.empty() ? 4u : fresh->flatBinding == DispatchVariant::NoFlatBinding ? 5u : 6u;
                ++entryCounters.dataInsertReasons[reason];
                auto& program = entryCounters.dataInsertsByProgram[address];
                ++program[reason];
                if (reason == 4u && capture->plan != nullptr) {
                    const auto& plan = *capture->plan;
                    const std::array<bool, 4> flags{!plan.srtPlanComplete, plan.requiresSpecializationMemory, std::any_of(plan.descriptorSources.begin(), plan.descriptorSources.end(), [](const auto& source) { return source.tableColumn.has_value(); }), std::none_of(plan.pureFlatSlots.begin(), plan.pureFlatSlots.end(), [](auto pure) { return pure != 0u; })};
                    for (std::size_t flag = 0; flag < flags.size(); ++flag) {
                        if (flags[flag]) {
                            ++entryCounters.dataNoLeafReasons[flag];
                            ++program[7 + flag];
                        }
                    }
                }
            }
            ++entryCounters.inserts;
            entryCounters.runsInserted += fresh->runs.size();
            if (!fresh->dataPositions.empty()) {
                ++entryCounters.dataInserts;
                entryCounters.dataPositionsInserted += fresh->dataPositions.size();
            }
            entryCounters.dataLeavesUnmapped += dataCounts.unmapped;
            entryCounters.dataLeavesMismatched += dataCounts.mismatched;
            entryCounters.dataLeavesAliased += dataCounts.aliased;
            attachVariant = fresh;
            const auto found = dispatchCache.find(key);
            if (found == dispatchCache.end()) {
                auto created = std::make_shared<DispatchEntry>();
                created->touched = dispatchCacheHits;
                accountVariant(*fresh, true);
                created->variants.push_back(std::move(fresh));
                dispatchOrder.push_front(key);
                created->order = dispatchOrder.begin();
                dispatchCache.emplace(key, std::move(created));
            } else if (found->second != missedEntry && !found->second->variants.empty() && found->second->variants.front()->runs == fresh->runs && SameCapturedWords(found->second->variants.front()->words, found->second->variants.front()->shared, fresh->words, fresh->shared)) {

                attachVariant = found->second->variants.front();
            } else {

                const auto& old = found->second;
                auto replacement = std::make_shared<DispatchEntry>();
                replacement->variants.reserve(dispatchVariants());
                accountVariant(*fresh, true);
                replacement->variants.push_back(std::move(fresh));
                for (const auto& kept : old->variants) {
                    if (replacement->variants.size() < dispatchVariants()) {
                        replacement->variants.push_back(kept);
                    } else {
                        accountVariant(*kept, false);
                        ++entryCounters.variantsEvicted;
                    }
                }
                ++entryCounters.variantsInserted;
                replacement->touched = dispatchCacheHits;
                replacement->order = old->order;
                dispatchOrder.splice(dispatchOrder.begin(), dispatchOrder, replacement->order);
                found->second = std::move(replacement);
            }

            static const bool lru = std::getenv("APS5_NO_DISPATCH_LRU") == nullptr;
            const auto over = [&] { return dispatchCache.size() > dispatchCacheEntries() || (dispatchCache.size() > 1 && dispatchCacheVariantBytes > DispatchCacheBytes()); };
            if (over()) {
                if (lru) {
                    while (over()) {
                        eraseDispatchEntry(dispatchCache.find(dispatchOrder.back()));
                        ++dispatchCacheEvictions;
                    }
                } else {
                    dispatchCacheEvictions += dispatchCache.size();
                    dispatchCache.clear();
                    dispatchOrder.clear();
                    dispatchCacheVariants = 0;
                    dispatchCacheVariantBytes = 0;
                }
            }
        } else {
            ++entryCounters.unstable;
        }
        phaseTiming.Phase(PhaseInsert);
    }
}

}
