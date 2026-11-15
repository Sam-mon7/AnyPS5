#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchDetailTiming.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/SrtWalker/SrtAddressArithmetic.hpp"
#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"
#include <functional>
#include <map>
#include <set>
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

bool Driver::dispatchRelocationDirect(const DispatchVariant& variant, const ShaderRecompiler::ResourceCapture& capture) {
    if (capture.plan == nullptr || !dispatchRelocationRangesValid(variant) || variant.relocatableWords == 0 || variant.userData != capture.snapshot.userData || variant.runs.empty() || variant.runBases.size() != variant.runs.size() || variant.runParents.size() != variant.runs.size()) return false;
    const auto& plan = *capture.plan;
    if (!plan.srtPlanComplete || !plan.resourceTrackingComplete || plan.requiresSpecializationMemory || !plan.guardedSrtSlots.empty() || plan.uniformFill.fill.kind != ShaderRecompiler::UniformFillKind::None || !capture.snapshot.tables.tables.empty() || !capture.snapshot.tables.nested.empty() || !capture.snapshot.srtPoison.empty() || capture.snapshot.nullRootReads != 0 || capture.snapshot.uniformFill.kind != ShaderRecompiler::UniformFillKind::None) return false;
    if (std::any_of(plan.controlFlow.begin(), plan.controlFlow.end(), [](const auto& block) { return block.condition != nullptr; }) || std::any_of(plan.descriptorSources.begin(), plan.descriptorSources.end(), [](const auto& source) { return source.tableColumn.has_value(); })) return false;
    if (std::any_of(capture.userDataUse.nested.begin(), capture.userDataUse.nested.end(), [](const auto& read) { return read.second != 0; }) || std::any_of(variant.runParents.begin(), variant.runParents.end(), [](const auto& parent) { return parent.run != DispatchVariant::NoFlatBinding; })) return false;
    const auto userWord = [&](const ShaderRecompiler::IrValue* value) -> std::int32_t {
        if (value == nullptr) return -1;
        value = value->Resolve();
        if (value->HasImmediate() || value->Type() != ShaderRecompiler::IrType::U32 || value->Opcode() != ShaderRecompiler::IrOpcode::GetUserData || value->ArgumentCount() != 1 || value->Argument(0) == nullptr || value->Argument(0)->Type() != ShaderRecompiler::IrType::ScalarReg) return -1;
        const auto reg = ShaderRecompiler::RegIndex(static_cast<ShaderRecompiler::ScalarReg>(value->Argument(0)->Register().index));
        if (reg < plan.userDataBase || reg - plan.userDataBase >= variant.userData.size() || reg - plan.userDataBase >= ShaderRecompiler::SrtUserDataUse::TrackedWords) return -1;
        return static_cast<std::int32_t>(reg - plan.userDataBase);
    };
    std::map<std::uint64_t, std::int32_t> expected;
    std::set<const ShaderRecompiler::IrValue*> active, complete;
    std::function<bool(const ShaderRecompiler::IrValue*, std::size_t)> visit = [&](const ShaderRecompiler::IrValue* raw, std::size_t depth) {
        if (raw == nullptr || depth > 64) return false;
        const auto* value = raw->Resolve();
        if (value->HasImmediate() || complete.contains(value)) return true;
        if (!active.insert(value).second) return false;
        const auto opcode = value->Opcode();
        if (opcode == ShaderRecompiler::IrOpcode::Phi || ShaderRecompiler::Detail::IsRuntimeSelect(opcode) || opcode == ShaderRecompiler::IrOpcode::Void) return false;
        if (opcode == ShaderRecompiler::IrOpcode::GetUserData) {
            if (userWord(value) < 0) return false;
        } else if (opcode == ShaderRecompiler::IrOpcode::ReadConst) {
            if (value->ArgumentCount() != 2 || value->Argument(0) == nullptr || value->Argument(0)->Resolve()->Opcode() != ShaderRecompiler::IrOpcode::GetSrtResource || value->Argument(1) == nullptr) return false;
            const auto* slot = value->Argument(1)->Resolve();
            if (!slot->HasImmediate() || slot->Type() != ShaderRecompiler::IrType::U32 || slot->ImmediateU32() >= plan.srtReads.size() || !visit(plan.srtReads[slot->ImmediateU32()].value, depth + 1)) return false;
        } else {
            if (ShaderRecompiler::Detail::IsMemoryRead(opcode)) {
                if (opcode != ShaderRecompiler::IrOpcode::LoadAddressU32 || !ShaderRecompiler::Detail::IsRawRead(plan, *value) || value->ArgumentCount() != 4 || std::any_of(value->Arguments().begin(), value->Arguments().end(), [](const auto* argument) { return argument == nullptr; })) return false;
                const auto& memory = plan.memoryInfo[value->Flags<ShaderRecompiler::MemoryFlags>().index];
                const auto* highOffset = value->Argument(2)->Resolve();
                const auto* enabled = value->Argument(3)->Resolve();
                if (memory.addressIsFull || memory.dataBits != 32 || memory.dataDwords != 1 || !highOffset->HasImmediate() || highOffset->Type() != ShaderRecompiler::IrType::U32 || highOffset->ImmediateU32() != 0 || !enabled->HasImmediate() || enabled->Type() != ShaderRecompiler::IrType::U1 || !enabled->ImmediateBool()) return false;
                const auto* handle = value->Argument(0)->Resolve();
                const auto* offset = value->Argument(1)->Resolve();
                if ((handle->Opcode() != ShaderRecompiler::IrOpcode::GetAddressResource && handle->Opcode() != ShaderRecompiler::IrOpcode::CompositeConstructU64) || handle->ArgumentCount() != 2 || handle->Argument(0) == nullptr || handle->Argument(1) == nullptr || !offset->HasImmediate() || offset->Type() != ShaderRecompiler::IrType::U32) return false;
                const auto root = userWord(handle->Argument(0));
                if (root < 0 || (variant.relocatableWords >> root & 1u) == 0) return false;
                const auto* highValue = handle->Argument(1)->Resolve();
                std::uint32_t high = 0;
                if (highValue->HasImmediate() && highValue->Type() == ShaderRecompiler::IrType::U32) high = highValue->ImmediateU32();
                else {
                    const auto highWord = userWord(highValue);
                    if (highWord < 0 || (variant.relocatableWords >> highWord & 1u) != 0) return false;
                    high = variant.userData[highWord];
                }
                const auto base = ((std::uint64_t{high} << 32u) | variant.userData[root]) & ShaderRecompiler::Detail::AddressMask;
                if (base == 0) return false;
                const auto immediate = static_cast<std::int64_t>(static_cast<std::int32_t>(plan.memoryInfo[value->Flags<ShaderRecompiler::MemoryFlags>().index].offset));
                const auto relative = (immediate & ~std::int64_t{3}) + static_cast<std::int64_t>(offset->ImmediateU32() & ~3u);
                std::uint64_t address = 0;
                if (!ShaderRecompiler::Detail::AddSignedAddress(base & ~std::uint64_t{3}, relative, address)) return false;
                const auto [found, inserted] = expected.emplace(address, root);
                if (!inserted && found->second != root) return false;
            } else if (!ShaderRecompiler::Detail::IsRuntimeUniformOp(opcode) && !ShaderRecompiler::Detail::IsDescriptorHandle(opcode) && opcode != ShaderRecompiler::IrOpcode::GetSrtResource) return false;
            for (const auto* argument : value->Arguments()) if (!visit(argument, depth + 1)) return false;
        }
        active.erase(value);
        complete.insert(value);
        return true;
    };
    for (const auto& read : plan.srtReads) if (!visit(read.value, 0)) return false;
    for (const auto& source : plan.descriptorSources) {
        if (source.dwordCount > source.dwords.size()) return false;
        for (std::size_t word = 0; word < source.dwordCount; ++word) if (!visit(source.dwords[word], 0)) return false;
    }
    for (const auto& value : plan.valueStorage) if (!value.IsEmpty() && !visit(&value, 0)) return false;
    if (expected.empty()) return false;
    std::map<std::uint64_t, std::int32_t> observed;
    for (const auto& [address, root] : capture.userDataUse.reads) {
        if (root < 0) return false;
        const auto [found, inserted] = observed.emplace(address, root);
        if (!inserted && found->second != root) return false;
    }
    if (observed != expected) return false;
    observed.clear();
    std::size_t words = 0;
    for (std::size_t run = 0; run < variant.runs.size(); ++run) {
        const auto [begin, end] = variant.runs[run];
        const auto root = variant.runBases[run];
        if (root < 0 || root >= ShaderRecompiler::SrtUserDataUse::TrackedWords || (variant.relocatableWords >> root & 1u) == 0 || end < begin || (begin & 3u) != 0 || (end - begin) % sizeof(std::uint32_t) != 0 || (end - begin) / sizeof(std::uint32_t) > variant.words.size() - words) return false;
        for (auto address = begin; address < end; address += sizeof(std::uint32_t), ++words) if (!observed.emplace(address, root).second) return false;
    }
    return words == variant.words.size() && observed == expected;
}

void Driver::insertDispatch(std::uint64_t address, std::uint64_t key, bool noDispatchCache, bool profile, const std::shared_ptr<const ShaderSnapshot>& registeredShader, std::uint64_t forgetAtCapture, std::span<const ShaderRecompiler::MemoryRegion> memory, const std::shared_ptr<ShaderMemory>& shaderMemory, const std::vector<ShaderRecompiler::MemoryRegion>& captured, const std::shared_ptr<const ShaderRecompiler::ResourceCapture>& capture, const std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, const std::shared_ptr<DispatchEntry>& missedEntry, bool missedDiffering, std::shared_ptr<DispatchVariant>& attachVariant, DispatchPhaseTiming& phaseTiming, std::uint64_t genAtCapture, bool feedbackMask, std::uint64_t feedbackKey) {
    DispatchDetailTiming detail("Dispatch.Detail.InsertVariant");
    detail.Point("dispatch.insert.request", address, key);
    if (!noDispatchCache) {
        auto fresh = std::make_shared<DispatchVariant>();
        fresh->compiled = compiledResult;
        if (capture != nullptr && !capture->snapshot.registeredImages.empty()) fresh->registeredViews = std::make_shared<const std::vector<ShaderRecompiler::RegisteredImageView>>(capture->snapshot.registeredImages);
        fresh->shader = registeredShader;
        fresh->forgetSerial = forgetAtCapture;
        fresh->genSince.store(genAtCapture, std::memory_order_relaxed);

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
        // APS5_ARCHB_FIX_DISPATCH_RELOCATION (sampled per insert): runs split and tagged by the user
        // word they are read through, as for draw stages.
        const bool relocation = capture != nullptr && feedbackKey != 0 && !stampValidate() && (ArchB::Switches::Enabled(ArchB::Feature::FixDispatchRelocation) || ArchB::Switches::Enabled(ArchB::Feature::DispatchRelocationRecipe));
        if (relocation) {
            tagVariantRuns(*fresh, srtRegions, capture->userDataUse, true, 0, supplied);
            if (fresh->userData.empty()) fresh->userData = capture->snapshot.userData;
            if (!dispatchRelocationDirect(*fresh, *capture)) {
                fresh->relocatableWords = 0;
                ArchB::Recorder::Count("dispatch_relocation_refused_topology");
                detail.Point("dispatch.relocation_recipe.shape_refused", address);
            }
        } else {
            for (const auto& region : srtRegions) {
                fresh->runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
                const auto count = region.bytes.size() / sizeof(std::uint32_t);
                const auto offset = fresh->words.size();
                fresh->words.resize(offset + count);
                std::memcpy(fresh->words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
                AppendSharedWords(fresh->shared, fresh->runs.size() - 1u, offset, region.bytes, supplied);
            }
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
        if (feedbackMask && !stampValidate() && capture != nullptr) RecordTsharpFeedbackMasks(*fresh, *capture);
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
        // The relocation shape and this entry's alias (content reads, before the cache mutex).
        std::shared_ptr<const DrawRelocation> relocationShape;
        std::uint64_t relocationAlias = 0;
        if (relocation && stable) {
            relocationShape = dispatchRelocationShape(*fresh);
            bool readable = true;
            if (relocationShape != nullptr) relocationAlias = dispatchAliasKey(feedbackKey, fresh->userData, *relocationShape, readable);
            if (!readable) relocationAlias = 0;
        }
        std::lock_guard cacheLock(dispatchCacheMutex);
        if (relocationShape != nullptr) {
            dispatchRelocations[feedbackKey] = relocationShape;
            if (relocationAlias != 0) {
                if (dispatchAliases.size() >= dispatchCacheEntries()) dispatchAliases.clear();
                dispatchAliases[relocationAlias] = key;
                ArchB::Recorder::Count("fix_dispatch_relocation_learned");
            }
        }
        if (profile && missedEntry != nullptr && missedDiffering) classifyDiffering(address, key, *missedEntry->variants.front(), *fresh, capture.get(), entryCounters);
        if (stable) {
            detail.Point("dispatch.insert.stable", address, key);
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
                        constexpr std::array<std::pair<std::uint16_t, const char*>, 10> names{{
                            {1u, "incomplete"}, {2u, "specialization"}, {4u, "table-column"}, {8u, "invalid-fill"},
                            {16u, "descriptor"}, {32u, "unused-descriptor"}, {64u, "branch"}, {128u, "uniform-fill"}, {256u, "address"},
                            {512u, "item-free-branch"}
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
            if (feedbackMask && feedbackKey != 0 && !fresh->feedback.userData.empty()) {
                if (dispatchFeedbackKeys.size() >= dispatchCacheEntries()) dispatchFeedbackKeys.clear();
                dispatchFeedbackKeys[feedbackKey] = key;
            }
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
                        detail.Point("dispatch.evict.key", dispatchOrder.back(), dispatchCache.size());
                        eraseDispatchEntry(dispatchCache.find(dispatchOrder.back()));
                        ++dispatchCacheEvictions;
                    }
                } else {
                    dispatchCacheEvictions += dispatchCache.size();
                    detail.Point("dispatch.evict.clear", 0, dispatchCache.size());
                    dispatchCache.clear();
                    dispatchOrder.clear();
                    dispatchCacheVariants = 0;
                    dispatchCacheVariantBytes = 0;
                }
            }
        } else {
            ++entryCounters.unstable;
            detail.Point("dispatch.insert.unstable", address, key);
        }
        phaseTiming.Phase(PhaseInsert);
    }
}

}
