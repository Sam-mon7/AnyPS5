#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <bit>

namespace AgcDriver::DriverDetail {

namespace {

struct Q0WriterVersions {
    std::uint64_t epoch;
    std::uint64_t mapping;
    std::uint64_t write;
    std::uint64_t recorderWrite;
    std::uint64_t forget;
    std::uint64_t publication;
    std::uint64_t pending;
    std::uint64_t unwatch;
    bool operator==(const Q0WriterVersions&) const = default;
};

enum class Q0WriterRefusal {
    None,
    Queue,
    Address,
    Dynamic,
    Unproven,
    Mapping,
    Write,
    QueuedWrite,
    Forget,
    Publication,
    PendingImage,
    Ordering,
    Unstable
};

Q0WriterVersions q0WriterVersions(const GuestMemory::Q0WriterProof& proof) {
    return {
        GuestMemory::ThreadCollectEpoch(),
        GuestAllocations::GuestAllocationsGeneration_nid_postfix(),
        proof.write.load(std::memory_order_acquire),
        proof.queuedWrite.load(std::memory_order_acquire),
        GuestMemory::ForgetSerial(),
        proof.publication.load(std::memory_order_acquire),
        proof.pendingImage.load(std::memory_order_acquire),
        GuestMemory::UnwatchSerial()
    };
}

Q0WriterRefusal q0WriterEligible(const DispatchVariant& variant, std::uint32_t queue) {
    if (queue != 0) return Q0WriterRefusal::Queue;
    if (variant.compiled == nullptr || variant.compiled->bdaAbiVersion != 0) return Q0WriterRefusal::Address;
    if (!variant.dataPositions.empty() || !variant.bufferBaseMasks.empty() || !variant.feedback.memory.empty()) return Q0WriterRefusal::Dynamic;
    return Q0WriterRefusal::None;
}

Q0WriterRefusal q0WriterProof(const DispatchVariant& variant, std::uint32_t queue, std::uint64_t& comparedBytes) {
    const auto eligible = q0WriterEligible(variant, queue);
    if (eligible != Q0WriterRefusal::None) return eligible;
    const auto proof = variant.q0WriterProof.load(std::memory_order_acquire);
    if (proof == nullptr) return Q0WriterRefusal::Unproven;
    const auto epoch = variant.q0Epoch.load(std::memory_order_acquire);
    if (epoch == 0) return Q0WriterRefusal::Unproven;
    const auto current = q0WriterVersions(*proof);
    if (variant.q0Mapping.load(std::memory_order_relaxed) != current.mapping || variant.q0Unwatch.load(std::memory_order_relaxed) != current.unwatch) return Q0WriterRefusal::Mapping;
    if (variant.q0Write.load(std::memory_order_relaxed) != current.write) return Q0WriterRefusal::Write;
    if (variant.q0RecorderWrite.load(std::memory_order_relaxed) != current.recorderWrite) return Q0WriterRefusal::QueuedWrite;
    if ((current.forget & 1u) != 0 || variant.q0Forget.load(std::memory_order_relaxed) != current.forget) return Q0WriterRefusal::Forget;
    if (variant.q0Publication.load(std::memory_order_relaxed) != current.publication) return Q0WriterRefusal::Publication;
    if (variant.q0Pending.load(std::memory_order_relaxed) != current.pending) return Q0WriterRefusal::PendingImage;
    if (epoch != current.epoch) return Q0WriterRefusal::Ordering;
    comparedBytes = variant.q0ComparedBytes.load(std::memory_order_relaxed);
    return variant.q0Epoch.load(std::memory_order_acquire) == epoch ? Q0WriterRefusal::None : Q0WriterRefusal::Unstable;
}

const char* q0WriterRefusalCounter(Q0WriterRefusal refusal) {
    switch (refusal) {
    case Q0WriterRefusal::Queue: return "fix_q0_writer_refused_queue";
    case Q0WriterRefusal::Address: return "fix_q0_writer_refused_address";
    case Q0WriterRefusal::Dynamic: return "fix_q0_writer_refused_dynamic";
    case Q0WriterRefusal::Unproven: return "fix_q0_writer_refused_unproven";
    case Q0WriterRefusal::Mapping: return "fix_q0_writer_refused_mapping";
    case Q0WriterRefusal::Write: return "fix_q0_writer_refused_write";
    case Q0WriterRefusal::QueuedWrite: return "fix_q0_writer_refused_queued_write";
    case Q0WriterRefusal::Forget: return "fix_q0_writer_refused_forget";
    case Q0WriterRefusal::Publication: return "fix_q0_writer_refused_publication";
    case Q0WriterRefusal::PendingImage: return "fix_q0_writer_refused_pending_image";
    case Q0WriterRefusal::Ordering: return "fix_q0_writer_refused_ordering";
    case Q0WriterRefusal::Unstable: return "fix_q0_writer_refused_unstable";
    case Q0WriterRefusal::None: break;
    }
    return "fix_q0_writer_refused_unknown";
}

void publishQ0WriterProof(const DispatchVariant& variant, const Q0WriterVersions& versions, std::uint64_t comparedBytes) {
    variant.q0Epoch.store(0, std::memory_order_release);
    variant.q0Mapping.store(versions.mapping, std::memory_order_relaxed);
    variant.q0Write.store(versions.write, std::memory_order_relaxed);
    variant.q0RecorderWrite.store(versions.recorderWrite, std::memory_order_relaxed);
    variant.q0Forget.store(versions.forget, std::memory_order_relaxed);
    variant.q0Publication.store(versions.publication, std::memory_order_relaxed);
    variant.q0Pending.store(versions.pending, std::memory_order_relaxed);
    variant.q0Unwatch.store(versions.unwatch, std::memory_order_relaxed);
    variant.q0ComparedBytes.store(comparedBytes, std::memory_order_relaxed);
    variant.q0Epoch.store(versions.epoch, std::memory_order_release);
}

}

void Driver::lookupDraw(const Submission& submission, const std::shared_ptr<VulkanDevice>& localDevice, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<DrawProgram>& programs, const std::vector<ShaderRecompiler::ProgramRole>& roles, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, bool useDrawEntries, bool registerKey, bool profile, std::uint64_t& drawKey, std::uint64_t& shapeKey, std::shared_ptr<DrawEntry>& entry, std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool& drawHit, bool& verifyHit, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, const std::shared_ptr<DrawEntry>& dataEntry, DrawDataCandidates& dataCandidates, std::vector<StageDataHit>& stageData, const Pm4::DrawParameters& drawParameters, bool pointerFree, bool feedbackMask, bool bufferBases, bool q0WriterInvalidate) {
    PerformanceTimer recorderTiming("Driver.DrawLookup", ArchB::Recorder::Phase::Proof);
    using Role = ShaderRecompiler::ProgramRole;
    if (useDrawEntries) {
        if (pointerFree) ArchB::Switches::CountUpperBoundHit(ArchB::Feature::UbPointerFreeDrawKeys);
        std::shared_ptr<DrawEntry> feedbackEntry = dataEntry;
        bool feedbackAlias = false;
        if (!registerKey) {
            drawKey = 0xcbf29ce484222325ull;
            if (feedbackMask) shapeKey = drawKey;
            const auto mix = [&](std::uint64_t value) {
                drawKey ^= value;
                drawKey *= 0x100000001b3ull;
                if (feedbackMask) shapeKey = (shapeKey ^ value) * 0x100000001b3ull;
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
                if (!pointerFree) for (const auto word : program.userData) drawKey = (drawKey ^ word) * 0x100000001b3ull;
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
            if (entry == nullptr && feedbackMask) {
                const auto shape = drawShapes.find(shapeKey);
                if (shape != drawShapes.end()) {
                    const auto kept = drawCache.find(shape->second);
                    if (kept != drawCache.end()) feedbackEntry = kept->second;
                }
            }
            maybeReportDrawCache(profile);
        }
        if (feedbackMask && entry == nullptr && feedbackEntry != nullptr && feedbackEntry->stages.size() == programs.size()) {
            bool eligible = true, maskedUser = false;
            for (std::size_t i = 0; eligible && i < programs.size(); ++i) {
                if (roles[i] == Role::GeometryBack) continue;
                eligible = std::any_of(feedbackEntry->stages[i].begin(), feedbackEntry->stages[i].end(), [&](const auto& variant) {
                    bool used = false;
                    if (variant->userData.empty() && feedbackEntry->decode == nullptr && !programs[i].userData.empty()) return false;
                    const auto& before = variant->userData.empty() && feedbackEntry->decode != nullptr ? feedbackEntry->decode->programs[i].userData : variant->userData;
                    if (!FeedbackWordsEqual(before, programs[i].userData, variant->feedback.userData, &used)) return false;
                    maskedUser |= used;
                    return true;
                });
            }
            if (eligible && maskedUser) { entry = feedbackEntry; feedbackAlias = true; }
        }
        if (entry == nullptr) ArchB::Recorder::Count("draw_cache_miss_absent");
        const bool genShadow = GuestMemory::Gen::ShadowActive();
        const bool genQueue0 = genShadow && GuestMemory::GpuLockThreadTag() == 0;
        bool genAllClean = true;
        if (entry == nullptr && genShadow) GuestMemory::Gen::NoteDrawLookup(genQueue0, false, false);
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
                            if (!patchedWords.empty() && IndirectStageCacheRefusal(*variant->compiled, variant->indirectCapture.get(), programs[i], info, patchedWords) != IndirectCacheRefusal::None) continue;
                        }
                        if (!pointerFree && programs[i].binary.stage == ShaderRecompiler::ShaderStage::Mesh && !std::equal(variant->meshIndexDescriptor.begin(), variant->meshIndexDescriptor.end(), programs[i].userData.begin() + ShaderRecompiler::MeshIndexBufferUserWord)) {
                            if (!anyLayout) outcome = EntryOutcome::Differing;
                            anyLayout = true;
                            continue;
                        }
                        bool userFeedback = false;
                        if (feedbackMask && (!variant->userData.empty() || feedbackAlias)) {
                            if (variant->userData.empty() && entry->decode == nullptr && !programs[i].userData.empty()) continue;
                            const auto& before = variant->userData.empty() && entry->decode != nullptr ? entry->decode->programs[i].userData : variant->userData;
                            if (!FeedbackWordsEqual(before, programs[i].userData, variant->feedback.userData, &userFeedback)) continue;
                        }
                        bool memoryFeedback = false;
                        auto& regions = matchedRegions[i];
                        regions.clear();
                        appendEntryRegions(*variant, regions);
                        bool q0Hit = false;
                        std::uint64_t avoidedBytes = 0;
                        auto q0Before = Q0WriterVersions{};
                        auto q0After = Q0WriterVersions{};
                        auto result = EntryOutcome::Differing;
                        const auto genBefore = genShadow ? GuestMemory::Gen::CaptureSince() : 0;
                        if (q0WriterInvalidate) {
                            const auto refusal = q0WriterProof(*variant, submission.queue, avoidedBytes);
                            if (refusal == Q0WriterRefusal::None) {
                                q0Hit = true;
                                result = EntryOutcome::Equal;
                                liveData.clear();
                                ArchB::Recorder::Count("fix_q0_writer_hits");
                                ArchB::Recorder::Count("fix_q0_writer_avoided_bytes", avoidedBytes);
                            } else {
                                ArchB::Recorder::Count(q0WriterRefusalCounter(refusal));
                                auto proof = variant->q0WriterProof.load(std::memory_order_acquire);
                                if (proof == nullptr && q0WriterEligible(*variant, submission.queue) == Q0WriterRefusal::None) {
                                    auto registered = GuestMemory::RegisterQ0WriterProof(variant->runs);
                                    if (variant->q0WriterProof.compare_exchange_strong(proof, registered, std::memory_order_release, std::memory_order_acquire)) proof = std::move(registered);
                                }
                                if (proof != nullptr) q0Before = q0WriterVersions(*proof);
                                ++compared;
                                ArchB::Recorder::Count("fix_q0_writer_full_validations");
                                result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live, feedbackMask, &memoryFeedback, bufferBases);
                                if (proof != nullptr) q0After = q0WriterVersions(*proof);
                                if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) {
                                    if (proof != nullptr) q0Before = q0WriterVersions(*proof);
                                    ArchB::Recorder::Count("fix_q0_writer_full_validations");
                                    result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live, feedbackMask, &memoryFeedback, bufferBases);
                                    if (proof != nullptr) q0After = q0WriterVersions(*proof);
                                }
                                std::uint64_t comparedBytes = 0;
                                for (const auto& region : regions) comparedBytes += region.bytes.size();
                                if (proof != nullptr && result == EntryOutcome::Equal && VariantValidationCompared() && VariantValidationExact() && q0WriterEligible(*variant, submission.queue) == Q0WriterRefusal::None && q0Before == q0After && q0After.epoch != 0 && (q0After.forget & 1u) == 0) publishQ0WriterProof(*variant, q0After, comparedBytes);
                                else if (q0Before != q0After) ArchB::Recorder::Count("fix_q0_writer_refused_unstable");
                            }
                        } else {
                            ++compared;
                            result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live, feedbackMask, &memoryFeedback, bufferBases);
                            if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live, feedbackMask, &memoryFeedback, bufferBases);
                        }
                        const auto genVerdict = genShadow && !q0Hit ? ShadowValidatedVariant(GuestMemory::Gen::Consumer::DrawValidate, *variant, result, genBefore, VariantValidationExact()) : GuestMemory::Gen::Verdict::Unknown;
                        if (!anyLayout) outcome = result;
                        anyLayout = true;
                        if (result != EntryOutcome::Equal && result != EntryOutcome::EqualData) continue;
                        if (genVerdict != GuestMemory::Gen::Verdict::Clean) genAllClean = false;
                        if (result == EntryOutcome::EqualData) {
                            auto& hit = stageData[i];
                            hit.words = variant->words;
                            for (const auto& [position, value] : liveData) hit.words[position] = value;
                            regions.clear();
                            appendEntryRegions(*variant, regions, &hit.words);
                            hit.compiled = patchDataWords(*variant, hit.words, bufferBases);
                            if (hit.compiled == nullptr) { ArchB::Recorder::Count("fix_buffer_base_refused_patch"); hit.words.clear(); continue; }
                            hit.bufferBaseHit = bufferBases && std::any_of(liveData.begin(), liveData.end(), [&](const auto& word) { return WordMask(variant->bufferBaseMasks, word.first) != 0; });
                            if (hit.bufferBaseHit) ArchB::Recorder::Count("fix_buffer_base_hits");
                            ++dataStages;
                            dataWords += liveData.size();
                        }
                        stageData[i].feedbackHit = userFeedback || memoryFeedback;
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
            if (genShadow) GuestMemory::Gen::NoteDrawLookup(genQueue0, drawHit, drawHit && genAllClean);
            const bool feedbackHit = drawHit && std::any_of(stageData.begin(), stageData.end(), [](const auto& stage) { return stage.feedbackHit; });
            if (feedbackHit) ArchB::Recorder::Count("fix_tsharp_mask_hits");
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
                if ((dataStages != 0 || feedbackHit) && verifyDrawDataHits()) drawHit = false;
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
                auto& candidates = dataCandidates[i];
                for (const auto& variant : dataEntry->stages[i]) {
                    const auto& was = variant->userData.empty() ? dataEntry->decode->programs[i].userData : variant->userData;
                    bool usedEqual = was.size() == now.size(), userFeedback = false, memoryFeedback = false;
                    for (std::size_t w = 0; !pointerFree && w < now.size() && usedEqual; ++w) {
                        const auto difference = now[w] ^ was[w];
                        if (difference == 0u || (w < 64u && ((variant->userDataMask >> w) & 1u) == 0u)) continue;
                        if (feedbackMask && (difference & ~WordMask(variant->feedback.userData, w)) == 0u) userFeedback = true;
                        else usedEqual = false;
                    }
                    if (!usedEqual) continue;
                    if ((i == 0 && variant->pushOffset != 0) || std::any_of(candidates.begin(), candidates.end(), [&](const auto& candidate) { return candidate.variant->pushOffset == variant->pushOffset; })) continue;
                    std::vector<ShaderRecompiler::MemoryRegion> regions;
                    appendEntryRegions(*variant, regions);
                    auto result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, nullptr, feedbackMask, &memoryFeedback);
                    if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, nullptr, feedbackMask, &memoryFeedback);
                    if (result == EntryOutcome::Equal) candidates.push_back({variant, std::move(regions), userFeedback || memoryFeedback});
                }
            }
        }
        phaseTiming.Phase(DrawRowKeyLookupValidate);
    }
}

}
