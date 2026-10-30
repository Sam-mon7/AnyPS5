#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include <algorithm>
#include <bit>

namespace AgcDriver::DriverDetail {

void Driver::lookupDraw(const Submission& submission, const std::shared_ptr<VulkanDevice>& localDevice, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<DrawProgram>& programs, const std::vector<ShaderRecompiler::ProgramRole>& roles, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, bool useDrawEntries, bool registerKey, bool profile, std::uint64_t& drawKey, std::shared_ptr<DrawEntry>& entry, std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool& drawHit, bool& verifyHit, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, const std::shared_ptr<DrawEntry>& dataEntry, DrawDataCandidates& dataCandidates, std::vector<StageDataHit>& stageData, const Pm4::DrawParameters& drawParameters, bool pointerFree, bool allowIndirectShaderData, std::uint64_t& captures) {
    PerformanceTimer recorderTiming("Driver.DrawLookup", ArchB::Recorder::Phase::Proof);
    using Role = ShaderRecompiler::ProgramRole;
    static const bool traceIndirect = std::getenv("APS5_TRACE_INDIRECT_DRAWS") != nullptr;
    if (traceIndirect && drawParameters.indirect) {
        std::fprintf(stderr, "[indirect-cache] lookup program 0x%llx key=0x%llx entries=%u entry=%u data-entry=%u fix=%u\n", static_cast<unsigned long long>(programs.front().binary.codeAddress), static_cast<unsigned long long>(drawKey), useDrawEntries, entry != nullptr, dataEntry != nullptr, allowIndirectShaderData);
        for (std::size_t i = 0; i < programs.size(); ++i) {
            std::fprintf(stderr, "[indirect-cache] stage %zu user:", i);
            for (const auto word : programs[i].userData) std::fprintf(stderr, " %08x", word);
            std::fprintf(stderr, "\n");
        }
    }
    if (useDrawEntries) {
        if (pointerFree) ArchB::Switches::CountUpperBoundHit(ArchB::Feature::UbPointerFreeDrawKeys);
        if (!registerKey) {
            drawKey = 0xcbf29ce484222325ull;
            const auto mix = [&](std::uint64_t value) {
                drawKey ^= value;
                drawKey *= 0x100000001b3ull;
            };
            if (drawParameters.indirect) {
                const auto& indirect = *drawParameters.indirect;
                mix(drawParameters.indexed);
                mix(indirect.baseVertexLocation);
                mix(indirect.startInstanceLocation);
                mix(indirect.drawIndexEnabled);
                mix(indirect.drawIndexLocation);
            }
            mix(localDevice->Serial());
            mix(static_cast<std::uint64_t>(graphics.stages.path));
            mix(graphics.stages.registerValue);
            mix(graphics.stages.vertexWaveSize);
            mix(graphics.stages.fragmentWaveSize);
            mix(graphics.stages.mesh.has_value());
            if (graphics.stages.mesh) {
                const auto& mesh = *graphics.stages.mesh;
                for (const auto value : {mesh.inputPrimitive, mesh.primitivesPerGroup, mesh.verticesPerGroup, mesh.maxVertices, mesh.maxPrimitives, mesh.threadsPerGroup, mesh.ldsSizeDwords, mesh.provokingVertex, mesh.esgsItemSize, mesh.outputPrimitiveSize}) mix(value);
            }
            mix(graphics.stages.tessellation.has_value());
            if (graphics.stages.tessellation) {
                const auto& tess = *graphics.stages.tessellation;
                for (const auto value : {tess.inputControlPoints, tess.outputControlPoints, tess.domain, tess.partitioning, tess.outputTopology, tess.patchesPerGroup, tess.primitiveGroupSize, static_cast<std::uint32_t>(tess.breakAtInstance)}) mix(value);
                mix(tess.mergedHull);
            }
            mix(graphics.rectList);
            mix(programs.size());
            for (std::size_t i = 0; i < programs.size(); ++i) {
                const auto& program = programs[i];
                mix(reinterpret_cast<std::uintptr_t>(program.snapshot.get()));
                mix(program.codeOffset);
                mix(static_cast<std::uint64_t>(roles[i]));
                mix(static_cast<std::uint64_t>(program.binary.stage));
                mix(program.userDataBase);
                mix(program.firstUserSgpr);
                mix(program.userData.size());
                if (!pointerFree) for (const auto word : program.userData) mix(word);
                mix(vertexInfos[i].has_value());
                if (!vertexInfos[i]) continue;
                const auto& vertex = *vertexInfos[i];
                require(vertex.resourcesNum <= vertex.resources.size(), "vertex stage info resource count exceeds its table");
                mix(vertex.resourcesNum);
                mix(vertex.fetchAttribReg);
                mix(vertex.fetchBufferReg);
                mix(vertex.fetchEmbedded);
                mix(vertex.paClVsOutCntl);
                mix(vertex.pointList);
                mix(std::bit_cast<std::uint32_t>(vertex.pointSize));
                mix(std::bit_cast<std::uint32_t>(vertex.pointSizeMin));
                mix(std::bit_cast<std::uint32_t>(vertex.pointSizeMax));
                for (std::uint32_t r = 0; r < vertex.resourcesNum; ++r) {
                    for (const auto field : vertex.resources[r].fields) mix(field);
                    const auto& destination = vertex.resourcesDst[r];
                    mix(static_cast<std::uint32_t>(destination.registerStart));
                    mix(static_cast<std::uint32_t>(destination.registersNum));
                    mix(static_cast<std::uint32_t>(destination.attrId));
                    mix(destination.fetchIndex);
                }
            }
            require(pixel.interpolatorCount <= pixel.interpolatorSettings.size(), "pixel stage info interpolator count exceeds its table");
            mix(pixel.interpolatorCount);
            for (std::uint32_t i = 0; i < pixel.interpolatorCount; ++i) mix(pixel.interpolatorSettings[i]);
            mix(pixel.inputAddr);
            for (const bool flag : {pixel.wave32, pixel.hasPerspectiveCenterVgpr, pixel.perspectiveCentroid, pixel.posX, pixel.posY, pixel.posZ, pixel.posW, pixel.frontFace, pixel.frontFaceAllBits, pixel.ancillary, pixel.sampleShading, pixel.noPerspective, pixel.linearCentroid, pixel.pixelKillEnable, pixel.depthExportEnable, pixel.sampleMaskExportEnable, pixel.earlyZ, pixel.executeOnNoop}) mix(flag);
            mix(static_cast<std::uint64_t>(pixel.conservativeZExport));
            mix(pixel.orderedPixelShader);
            mix(pixel.stencilRefExportEnable);
            mix(pixel.stencilOperationValueExport);
            for (const auto value : pixel.targetOutputMode) mix(value);
            for (const auto value : pixel.targetExportMapping) mix(value);
            for (const auto value : pixel.targetExportPacking) mix(static_cast<std::uint64_t>(value));
            mix(pixel.dualSourceBlend);
            mix(pixel.reversedBlend);
            mix(pixel.targetMerge.target);
            mix(pixel.targetMerge.mask);
            mix(pixel.targetMerge.samples);
            for (const auto word : pixel.targetMerge.descriptor) mix(word);
            mix(pixel.targetMerge.depthStencilWrites);
            mix(pixel.targetMerge.depthStencilTests);
            mix(pixel.alphaToCoverage);
            mix(static_cast<std::uint64_t>(pixel.rasterPrimitive));
            mix(pixel.ditheredAlphaToCoverage);
            mix(pixel.quadPixelMask);
            for (const auto mask : pixel.quadSampleMask) mix(mask);
            mix(pixel.iteratesSamples);
            mix(static_cast<std::uint64_t>(pixel.positionLocation));
            for (const auto location : pixel.sampleLocations) mix(location);
            for (const auto order : pixel.sampleOrder) mix(order);
            std::lock_guard cacheLock(drawCacheMutex);
            ++drawEntryCounters.lookups;
            const auto found = drawCache.find(drawKey);
            if (found != drawCache.end()) entry = found->second;
            else ++drawEntryCounters.absent;
            maybeReportDrawCache(profile);
        }
        if (entry == nullptr) ArchB::Recorder::Count("draw_cache_miss_absent");
        if (entry != nullptr) {
            const auto waitedBeforeValidate = profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
            std::optional<DrawMiss> miss;
            std::vector<std::size_t> ranks(programs.size(), 0);
            std::uint64_t stageValidations = 0, stageEqual = 0, compared = 0, imagesFlushed = 0, runsSynced = 0, dataStages = 0, dataWords = 0;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> liveData;
            auto* live = drawStageDataHits() ? &liveData : nullptr;
            if (entry->stages.size() != programs.size()) miss = DrawMiss::Stages;

            std::uint32_t cursor = 0;
            {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);

                std::optional<SampledReadScope> sampling;
                for (std::size_t i = 0; !miss && i < programs.size(); ++i) {
                    if (roles[i] == Role::GeometryBack) continue;
                    ++stageValidations;
                    cursor = Graphics::StagePushOffset(cursor, programs[i].binary.stage, localDevice->GraphicsPipelineLibraries());
                    const auto& variants = entry->stages[i];
                    auto outcome = EntryOutcome::Differing;
                    bool anyLayout = false;
                    for (std::size_t rank = 0; rank < variants.size(); ++rank) {
                        const auto& variant = variants[rank];
                        if (variant->pushOffset != cursor) continue;
                        if (!pointerFree && drawParameters.indirect && variant->indirectCapture != nullptr) {
                            const auto& indirect = *drawParameters.indirect;
                            std::vector<std::size_t> patchedWords;
                            for (const auto location : {indirect.baseVertexLocation, indirect.startInstanceLocation, indirect.drawIndexEnabled ? indirect.drawIndexLocation : 0x280u}) {
                                for (const auto& [program, word] : UserDataWords(programs, roles, location)) {
                                    if (program == i) patchedWords.push_back(word);
                                }
                            }
                            const auto info = variant->vertexInfo != nullptr ? std::optional(*variant->vertexInfo) : std::nullopt;
                            if (!patchedWords.empty()) {
                                const auto refusal = IndirectStageCacheRefusal(*variant->compiled, variant->indirectCapture.get(), programs[i], info, patchedWords, allowIndirectShaderData);
                                if (traceIndirect) std::fprintf(stderr, "[indirect-cache] stage %zu variant %zu refusal=%u\n", i, rank, static_cast<unsigned>(refusal));
                                if (refusal != IndirectCacheRefusal::None) continue;
                            }
                        }
                        if (!pointerFree && programs[i].binary.stage == ShaderRecompiler::ShaderStage::Mesh && !std::equal(variant->meshIndexDescriptor.begin(), variant->meshIndexDescriptor.end(), programs[i].userData.begin() + ShaderRecompiler::MeshIndexBufferUserWord)) {
                            if (!anyLayout) outcome = EntryOutcome::Differing;
                            anyLayout = true;
                            continue;
                        }
                        auto& regions = matchedRegions[i];
                        regions.clear();
                        appendEntryRegions(*variant, regions);
                        ++compared;
                        auto result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live);
                        if (traceIndirect && drawParameters.indirect) std::fprintf(stderr, "[indirect-cache] stage %zu variant %zu validation=%u\n", i, rank, static_cast<unsigned>(result));
                        if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live);
                        if (!anyLayout) outcome = result;
                        anyLayout = true;
                        if (result != EntryOutcome::Equal && result != EntryOutcome::EqualData) continue;
                        if (result == EntryOutcome::EqualData) {
                            auto& hit = stageData[i];
                            hit.words = variant->words;
                            for (const auto& [position, value] : liveData) hit.words[position] = value;
                            regions.clear();
                            appendEntryRegions(*variant, regions, &hit.words);
                            hit.compiled = patchDataWords(*variant, hit.words);
                            ++dataStages;
                            dataWords += liveData.size();
                        }
                        matched[i] = variant;
                        ranks[i] = rank;
                        ++stageEqual;
                        cursor += static_cast<std::uint32_t>(variant->compiled->pushConstants.size());
                        break;
                    }
                    if (matched[i] != nullptr) continue;
                    if (!anyLayout) miss = DrawMiss::Layout;
                    else if (outcome != EntryOutcome::Differing) miss = DrawMiss::Gate;
                    else miss = roles[i] == Role::Fragment ? DrawMiss::FragmentDiffering : i == 0 ? DrawMiss::FrontDiffering : DrawMiss::OtherDiffering;
                }
            }
            drawHit = !miss;
            constexpr const char* recorderMisses[] = {"draw_cache_miss_front", "draw_cache_miss_fragment", "draw_cache_miss_other", "draw_cache_miss_layout", "draw_cache_miss_gate", "draw_cache_miss_stages"};
            ArchB::Recorder::Count(miss ? recorderMisses[static_cast<std::size_t>(*miss)] : "draw_cache_hits");
            std::lock_guard cacheLock(drawCacheMutex);
            auto& counters = drawEntryCounters;
            counters.stageValidations += stageValidations;
            counters.stageEqual += stageEqual;
            counters.variantsCompared += compared;
            if (drawHit) {
                ++counters.hits;
                counters.stageDataHits += dataStages;
                counters.stageDataWords += dataWords;
                if (registerKey) ++counters.registerKeyHits;
                ++drawCacheHits;
                bool rotate = false;
                for (std::size_t i = 0; i < programs.size(); ++i) {
                    if (matched[i] == nullptr) continue;
                    ++counters.variantHitsByRank[ranks[i]];
                    if (ranks[i] != 0) rotate = true;
                }

                const auto again = drawCache.find(drawKey);
                if (again != drawCache.end() && again->second == entry) {
                    if (rotate) {
                        auto rotated = std::make_shared<DrawEntry>();
                        rotated->decode = entry->decode;
                        rotated->stages = entry->stages;
                        rotated->shape = entry->shape;
                        rotated->recipes.store(entry->recipes.load());
                        for (std::size_t i = 0; i < programs.size(); ++i) {
                            if (ranks[i] == 0) continue;
                            auto& variants = rotated->stages[i];
                            variants.erase(variants.begin() + static_cast<std::ptrdiff_t>(ranks[i]));
                            variants.insert(variants.begin(), matched[i]);
                        }
                        rotated->touched = entry->touched;
                        rotated->order = entry->order;
                        again->second = std::move(rotated);
                    }
                    if (drawCacheHits - again->second->touched > drawCacheEntries() / 8) {
                        drawOrder.splice(drawOrder.begin(), drawOrder, again->second->order);
                        again->second->touched = drawCacheHits;
                        ++counters.touches;
                    }
                }
                if (verifyDrawEntries()) {
                    ++counters.verifyHits;
                    verifyHit = true;
                    drawHit = false;
                }
                if (dataStages != 0 && verifyDrawDataHits()) drawHit = false;
            } else {
                ++counters.misses[static_cast<std::size_t>(*miss)];
                if (registerKey && entry->decode != nullptr) ++counters.decodePartial;
            }
            if (profile) {

                phaseTiming.Phase(DrawRowKeyLookupValidate);
                const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBeforeValidate, phaseMs[DrawRowKeyLookupValidate]);
                phaseMs[DrawRowKeyLookupValidate] -= waited;
                phaseMs[DrawRowValidateWait] += waited;
                counters.validateUs += phaseMs[DrawRowKeyLookupValidate] * 1000;
            }
        } else if (dataEntry != nullptr && graphics.stages.path == Graphics::ShaderPath::Vertex && dataEntry->stages.size() == programs.size() && dataEntry->decode->programs.size() == programs.size()) {
            std::uint64_t imagesFlushed = 0, runsSynced = 0;
            const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);
            std::optional<SampledReadScope> sampling;
            for (std::size_t i = 0; i < programs.size(); ++i) {
                if (roles[i] == Role::GeometryBack || programs[i].userData.size() != dataEntry->decode->programs[i].userData.size()) continue;
                const auto& now = programs[i].userData;
                const auto& was = dataEntry->decode->programs[i].userData;
                auto& candidates = dataCandidates[i];
                std::vector<std::size_t> patchedWords;
                if (allowIndirectShaderData && drawParameters.indirect) {
                    const auto& indirect = *drawParameters.indirect;
                    for (const auto location : {indirect.baseVertexLocation, indirect.startInstanceLocation, indirect.drawIndexEnabled ? indirect.drawIndexLocation : 0x280u}) {
                        for (const auto& [program, word] : UserDataWords(programs, roles, location)) {
                            if (program == i) patchedWords.push_back(word);
                        }
                    }
                }
                for (const auto& kept : dataEntry->stages[i]) {
                    const auto variant = allowIndirectShaderData && drawParameters.indirect ? RelocateIndirectStage(kept, programs[i], was, patchedWords) : kept;
                    if (variant == nullptr) continue;
                    bool usedEqual = true;
                    for (std::size_t w = 0; !pointerFree && !(allowIndirectShaderData && drawParameters.indirect) && w < now.size() && usedEqual; ++w) {
                        if (now[w] != was[w] && (w >= 64u || ((variant->userDataMask >> w) & 1u) != 0)) usedEqual = false;
                    }
                    if (!usedEqual) continue;
                    if (!patchedWords.empty() && variant->indirectCapture != nullptr) {
                        const auto info = variant->vertexInfo != nullptr ? std::optional(*variant->vertexInfo) : std::nullopt;
                        const auto refusal = IndirectStageCacheRefusal(*variant->compiled, variant->indirectCapture.get(), programs[i], info, patchedWords, true);
                        if (traceIndirect) std::fprintf(stderr, "[indirect-cache] candidate stage %zu refusal=%u\n", i, static_cast<unsigned>(refusal));
                        if (refusal != IndirectCacheRefusal::None) continue;
                    }
                    if ((i == 0 && variant->pushOffset != 0) || std::any_of(candidates.begin(), candidates.end(), [&](const auto& candidate) { return candidate.first->pushOffset == variant->pushOffset; })) continue;
                    std::vector<ShaderRecompiler::MemoryRegion> regions;
                    appendEntryRegions(*variant, regions);
                    std::vector<std::pair<std::uint32_t, std::uint32_t>> liveData;
                    auto* live = allowIndirectShaderData && drawParameters.indirect && drawStageDataHits() ? &liveData : nullptr;
                    auto result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live);
                    if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live);
                    if (traceIndirect && drawParameters.indirect) std::fprintf(stderr, "[indirect-cache] candidate stage %zu relocation=%u validation=%u\n", i, variant != kept, static_cast<unsigned>(result));
                    if (traceIndirect && drawParameters.indirect && result == EntryOutcome::Differing) {
                        std::size_t position = 0;
                        for (const auto& region : regions) {
                            std::vector<std::uint32_t> words(region.bytes.size() / sizeof(std::uint32_t));
                            if (GuestMemory::CopyMapped(region.guestAddress, std::as_writable_bytes(std::span(words))) != GuestMemory::Compare::Equal) break;
                            bool different = false;
                            for (std::size_t word = 0; word < words.size(); ++word) {
                                if (words[word] == variant->words[position + word]) continue;
                                std::fprintf(stderr, "[indirect-cache] differing stage %zu address=0x%llx stored=0x%x live=0x%x data=%u positions=%zu\n", i, static_cast<unsigned long long>(region.guestAddress + word * sizeof(std::uint32_t)), variant->words[position + word], words[word], std::binary_search(variant->dataPositions.begin(), variant->dataPositions.end(), position + word), variant->dataPositions.size());
                                different = true;
                                break;
                            }
                            if (different) break;
                            position += words.size();
                        }
                    }
                    if (allowIndirectShaderData && drawParameters.indirect && variant != kept && result == EntryOutcome::Differing && RewalkIndirectStage(*variant, programs[i], captures)) {
                        regions.clear();
                        liveData.clear();
                        appendEntryRegions(*variant, regions);
                        result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live);
                        if (traceIndirect) std::fprintf(stderr, "[indirect-cache] rewalk stage %zu validation=%u\n", i, static_cast<unsigned>(result));
                    }
                    if (result != EntryOutcome::Equal && result != EntryOutcome::EqualData) continue;
                    if (variant != kept) {
                        applyShaderDataPatches(*variant);
                    }
                    if (result == EntryOutcome::EqualData) {
                        auto& hit = stageData[i];
                        hit.words = variant->words;
                        for (const auto& [position, value] : liveData) hit.words[position] = value;
                        regions.clear();
                        appendEntryRegions(*variant, regions, &hit.words);
                        hit.compiled = patchDataWords(*variant, hit.words);
                    }
                    candidates.emplace_back(variant, std::move(regions));
                    if (allowIndirectShaderData && drawParameters.indirect) break;
                }
            }
            if (allowIndirectShaderData && drawParameters.indirect) {
                std::uint32_t cursor = 0;
                drawHit = true;
                for (std::size_t i = 0; i < programs.size(); ++i) {
                    cursor = Graphics::StagePushOffset(cursor, programs[i].binary.stage, localDevice->GraphicsPipelineLibraries());
                    const auto candidate = std::find_if(dataCandidates[i].begin(), dataCandidates[i].end(), [&](const auto& kept) { return kept.first->pushOffset == cursor; });
                    if (candidate == dataCandidates[i].end()) {
                        drawHit = false;
                        break;
                    }
                    matched[i] = candidate->first;
                    matchedRegions[i] = candidate->second;
                    cursor += static_cast<std::uint32_t>(matched[i]->compiled->pushConstants.size());
                }
                if (drawHit) {
                    ArchB::Recorder::Count("fix_indirect_cpu_draw_runtime_word_hits");
                    std::lock_guard cacheLock(drawCacheMutex);
                    ++drawEntryCounters.hits;
                    ++drawCacheHits;
                    if (verifyDrawEntries() || verifyDrawDataHits()) {
                        verifyHit = true;
                        drawHit = false;
                    }
                } else {
                    ArchB::Recorder::Count("fix_indirect_cpu_draw_runtime_word_misses");
                    std::fill(matched.begin(), matched.end(), nullptr);
                    std::fill(stageData.begin(), stageData.end(), StageDataHit{});
                }
                for (auto& candidates : dataCandidates) candidates.clear();
            }
        }
        phaseTiming.Phase(DrawRowKeyLookupValidate);
    }
}

}
