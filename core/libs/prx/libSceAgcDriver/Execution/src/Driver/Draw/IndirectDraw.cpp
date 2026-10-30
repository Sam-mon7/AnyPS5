#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "ShaderDiskCache.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::DriverDetail {

namespace {

IndirectCacheRefusal CheckIndirectPushConstants(const ShaderRecompiler::RecompileResult& result, const ShaderRecompiler::ResourceCapture* capture, const DrawProgram& program, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertexInfo, std::span<const std::size_t> patchedWords, bool allowShaderData) {
    using Refusal = IndirectCacheRefusal;
    const auto pushRefusal = [&](const char* reason) {
        ArchB::Recorder::Count(reason);
        static const bool trace = std::getenv("APS5_TRACE_INDIRECT_DRAWS") != nullptr;
        if (trace) std::fprintf(stderr, "[indirect-push] program 0x%llx %s data=%u push-bytes=%zu mapping=%zu storage=%zu\n", static_cast<unsigned long long>(program.binary.codeAddress), reason, result.shaderDataDwords, result.pushConstants.size(), result.shaderDataUserWords.size(), static_cast<std::size_t>(std::count_if(result.bindings.begin(), result.bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; })));
        return Refusal::PushMapping;
    };
    if (capture == nullptr || capture->plan == nullptr || capture->plan->userDataBase != program.firstUserSgpr || capture->snapshot.userData.size() != program.userData.size()) return Refusal::Capture;
    if (!capture->plan->resourceTrackingComplete || !capture->plan->srtPlanComplete || capture->plan->requiresSpecializationMemory) return Refusal::Plan;
    if (program.binary.stage != ShaderRecompiler::ShaderStage::Vertex && program.binary.stage != ShaderRecompiler::ShaderStage::Fragment) return Refusal::Path;
    const auto& mapping = result.shaderDataUserWords;
    if (mapping.empty()) return pushRefusal("fix_indirect_cpu_draw_push_empty");
    if (mapping.size() != result.memoryOffsetDword) return pushRefusal("fix_indirect_cpu_draw_push_memory_offset");
    if (mapping.size() > result.shaderDataDwords) return pushRefusal("fix_indirect_cpu_draw_push_data_size");
    const auto storage = std::find_if(result.bindings.begin(), result.bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; });
    if (allowShaderData && storage != result.bindings.end()) {
        if (storage->kind != ShaderRecompiler::DescriptorKind::StorageBuffer || storage->count != 1u || storage->guestDescriptor.size() != result.shaderDataDwords) return pushRefusal("fix_indirect_cpu_draw_push_storage_layout");
        if (std::any_of(storage + 1, result.bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; })) return pushRefusal("fix_indirect_cpu_draw_push_storage_duplicate");
    } else {
        if (result.shaderDataDwords > result.pushConstants.size() / sizeof(std::uint32_t)) return pushRefusal("fix_indirect_cpu_draw_push_capacity");
        if (storage != result.bindings.end()) return pushRefusal("fix_indirect_cpu_draw_push_storage");
    }
    std::uint64_t forwarded = 0;
    for (const auto word : mapping) {
        if (word >= ShaderRecompiler::SrtUserDataUse::TrackedWords || word >= program.userData.size()) return pushRefusal("fix_indirect_cpu_draw_push_word_range");
        if ((forwarded & (std::uint64_t{1} << word)) != 0) return pushRefusal("fix_indirect_cpu_draw_push_duplicate");
        forwarded |= std::uint64_t{1} << word;
    }
    if (forwarded != result.userDataRegisterReads) return pushRefusal("fix_indirect_cpu_draw_push_read_mask");
    const auto consumed = capture->userDataUse.asBase | capture->userDataUse.asValue;
    for (const auto word : patchedWords) {
        if (word >= program.userData.size()) return pushRefusal("fix_indirect_cpu_draw_push_patch_range");
    }
    for (std::size_t word = 0; word < program.userData.size(); ++word) {
        if (program.userData[word] == capture->snapshot.userData[word] && std::find(patchedWords.begin(), patchedWords.end(), word) == patchedWords.end()) continue;
        if (word >= ShaderRecompiler::SrtUserDataUse::TrackedWords) return pushRefusal("fix_indirect_cpu_draw_push_untracked");
        if ((consumed & (std::uint64_t{1} << word)) != 0) return Refusal::CaptureRead;
        if ((forwarded & (std::uint64_t{1} << word)) == 0) return pushRefusal("fix_indirect_cpu_draw_push_not_forwarded");
        if (word < program.hiddenUserWords) return Refusal::Path;
        if (vertexInfo && vertexInfo->resourcesNum != 0) {
            for (const auto reg : {vertexInfo->fetchAttribReg, vertexInfo->fetchBufferReg}) {
                for (const auto hidden : {0u, program.hiddenUserWords}) {
                    const auto first = static_cast<std::uint64_t>(reg) + hidden;
                    if (word == first || word == first + 1u) return Refusal::VertexDecode;
                }
            }
        }
    }
    return Refusal::None;
}

bool PatchIndirectPushConstants(ShaderRecompiler::RecompileResult& result, const ShaderRecompiler::ResourceCapture* capture, const DrawProgram& program, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertexInfo, bool allowShaderData) {
    if (CheckIndirectPushConstants(result, capture, program, vertexInfo, {}, allowShaderData) != IndirectCacheRefusal::None) return false;
    const auto storage = std::find_if(result.bindings.begin(), result.bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; });
    const auto& mapping = result.shaderDataUserWords;
    for (std::size_t slot = 0; slot < mapping.size(); ++slot) {
        if (storage != result.bindings.end()) storage->guestDescriptor[slot] = program.userData[mapping[slot]];
        else std::memcpy(result.pushConstants.data() + slot * sizeof(std::uint32_t), &program.userData[mapping[slot]], sizeof(std::uint32_t));
    }
    if (storage != result.bindings.end()) result.drawKeyMemo.words.store(nullptr, std::memory_order_relaxed);
    return true;
}

void CountIndirectStage(bool patched, bool verified) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    static std::atomic<std::uint64_t> patches{0}, materializations{0}, verifications{0};
    (patched ? patches : materializations).fetch_add(1, std::memory_order_relaxed);
    if (verified) verifications.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<std::chrono::steady_clock::rep> nextReport{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto due = nextReport.load(std::memory_order_relaxed);
    if (now < due || !nextReport.compare_exchange_strong(due, now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(10)).count(), std::memory_order_relaxed)) return;
    ProfilePrint_nid_no_patch("[indirect-push] stages (cumulative): %llu patched, %llu re-materialized, %llu verified\n", static_cast<unsigned long long>(patches.load(std::memory_order_relaxed)), static_cast<unsigned long long>(materializations.load(std::memory_order_relaxed)), static_cast<unsigned long long>(verifications.load(std::memory_order_relaxed)));
}

void CountIndirectFold(std::optional<Graphics::IndirectDrawPath> cpu, bool builtins, bool mismatch, bool shared) {
    static thread_local std::array<std::uint64_t, 4> counts{};
    static thread_local std::array<std::uint64_t, 4> paths{};
    ++paths[(cpu ? 2u : 0u) | (builtins ? 1u : 0u)];
    if (cpu == Graphics::IndirectDrawPath::NotFolded) ++counts[(mismatch ? 1u : 0u) | (shared ? 2u : 0u)];
    static thread_local auto reportedAt = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - reportedAt < std::chrono::seconds(10)) return;
    reportedAt = now;
    ProfilePrint_nid_no_patch("[indirect-fold] queue 0x%x NotFolded (10 s): k != p only %llu, shared/observed only %llu, both %llu\n", GuestMemory::GpuLockThreadTag(), static_cast<unsigned long long>(counts[1]), static_cast<unsigned long long>(counts[2]), static_cast<unsigned long long>(counts[3]));
    ProfilePrint_nid_no_patch("[indirect-fold] queue 0x%x classified paths (10 s): GPU %llu (builtin offsets %llu), CPU %llu (builtin offsets %llu)\n", GuestMemory::GpuLockThreadTag(), static_cast<unsigned long long>(paths[0] + paths[1]), static_cast<unsigned long long>(paths[1]), static_cast<unsigned long long>(paths[2] + paths[3]), static_cast<unsigned long long>(paths[3]));
    counts = {};
    paths = {};
}

}

IndirectCacheRefusal Driver::IndirectStageCacheRefusal(const ShaderRecompiler::RecompileResult& result, const ShaderRecompiler::ResourceCapture* capture, const DrawProgram& program, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertexInfo, std::span<const std::size_t> patchedWords, bool allowShaderData) {
    return CheckIndirectPushConstants(result, capture, program, vertexInfo, patchedWords, allowShaderData);
}

std::shared_ptr<DispatchVariant> Driver::RelocateIndirectStage(const std::shared_ptr<DispatchVariant>& variant, const DrawProgram& program, std::span<const std::uint32_t> previous, std::span<const std::size_t> patchedWords) {
    if (previous.size() != program.userData.size()) return nullptr;
    std::vector<std::size_t> moved;
    std::vector<std::size_t> runtime;
    for (std::size_t word = 0; word < previous.size(); ++word) {
        if (previous[word] == program.userData[word]) continue;
        if (word >= 64u) return nullptr;
        if ((variant->userDataMask & (std::uint64_t{1} << word)) == 0) continue;
        if (variant->indirectCapture != nullptr && std::find(patchedWords.begin(), patchedWords.end(), word) != patchedWords.end()) {
            runtime.push_back(word);
            continue;
        }
        if ((variant->relocatableWords & (std::uint64_t{1} << word)) == 0 || previous[word] == 0 || program.userData[word] == 0 || ((previous[word] ^ program.userData[word]) & 3u) != 0 || variant->runBases.size() != variant->runs.size()) return nullptr;
        moved.push_back(word);
    }
    if (moved.empty() && runtime.empty()) return variant;
    auto copy = std::make_shared<DispatchVariant>();
    copy->runs = variant->runs;
    copy->runBases = variant->runBases;
    copy->relocatableWords = variant->relocatableWords;
    copy->words = variant->words;
    copy->forgetSerial = variant->forgetSerial;
    copy->compiled = variant->compiled;
    copy->shader = variant->shader;
    copy->dataPositions = variant->dataPositions;
    copy->dataSlots = variant->dataSlots;
    copy->flatBinding = variant->flatBinding;
    copy->pushOffset = variant->pushOffset;
    copy->vertexInfo = variant->vertexInfo;
    copy->userDataMask = variant->userDataMask;
    copy->relocationCapture = variant->relocationCapture;
    auto proof = variant->indirectCapture != nullptr ? std::make_shared<ShaderRecompiler::ResourceCapture>(*variant->indirectCapture) : nullptr;
    for (const auto word : moved) {
        const auto delta = static_cast<std::int64_t>(program.userData[word] & ~3u) - static_cast<std::int64_t>(previous[word] & ~3u);
        for (std::size_t run = 0; run < copy->runs.size(); ++run) {
            if (copy->runBases[run] != static_cast<std::int8_t>(word)) continue;
            copy->runs[run].first += delta;
            copy->runs[run].second += delta;
        }
        if (proof != nullptr) proof->snapshot.userData[word] = program.userData[word];
    }
    runtime.insert(runtime.end(), moved.begin(), moved.end());
    for (const auto word : runtime) {
        const auto& forwarded = variant->compiled->shaderDataUserWords;
        for (std::uint32_t slot = 0; slot < forwarded.size(); ++slot) {
            if (forwarded[slot] == word) copy->shaderDataPatches.emplace_back(slot, program.userData[word]);
        }
    }
    copy->indirectCapture = std::move(proof);
    return copy;
}

bool Driver::RewalkIndirectStage(DispatchVariant& variant, const DrawProgram& program, std::uint64_t& captures) try {
    const auto& previous = variant.relocationCapture;
    if (previous == nullptr || previous->plan == nullptr || !previous->plan->resourceTrackingComplete || !previous->plan->srtPlanComplete || (variant.vertexInfo != nullptr && variant.vertexInfo->resourcesNum != 0)) {
        ArchB::Recorder::Count("fix_indirect_cpu_draw_rewalk_refused_plan");
        return false;
    }
    const SampledReadScope sampling(evidenceReads);
    ShaderMemory memory({}, &queryPendingWrite, &observePendingWrite, hookWaitCounter(), &QueryPendingSpan);
    const auto current = memory.Recapture(*previous, program.binary, program.userData);
    ++captures;
    const auto& before = previous->snapshot;
    const auto& after = current->snapshot;
    const auto relocatedDescriptors = [](const auto& before, const auto& after, std::uint32_t width, std::uint32_t highMask) {
        if (before.size() != after.size()) return false;
        for (std::size_t resource = 0; resource < before.size(); ++resource) {
            const auto& old = before[resource];
            auto live = after[resource];
            if (old == live) continue;
            if (old.dwordCount != width || live.dwordCount != width || (old.dwords[0] == 0u && (old.dwords[1] & highMask) == 0u) || (live.dwords[0] == 0u && (live.dwords[1] & highMask) == 0u)) return false;
            if (width == 4u && (old.dwords[2] == 0u || ((old.dwords[0] ^ live.dwords[0]) & 3u) != 0u)) return false;
            live.dwords[0] = old.dwords[0];
            live.dwords[1] = (live.dwords[1] & ~highMask) | (old.dwords[1] & highMask);
            if (old != live) return false;
        }
        return true;
    };
    const auto relocatedTables = [&] {
        if (before.tables.shader != after.tables.shader || before.tables.tables.size() != after.tables.tables.size()) return false;
        if (before.tables == after.tables) return true;
        for (std::size_t table = 0; table < before.tables.tables.size(); ++table) {
            const auto& old = before.tables.tables[table];
            const auto& live = after.tables.tables[table];
            if (old.size != live.size || old.records != live.records || old.keys != live.keys || old.fault != live.fault || old.outside != live.outside) return false;
        }
        return true;
    };
    if (!relocatedDescriptors(before.buffers, after.buffers, 4u, 0xffffu) || !relocatedDescriptors(before.images, after.images, 8u, 0xffu) || before.samplers != after.samplers || before.uniformFill != after.uniformFill || !relocatedTables() || !before.srtPoison.empty() || !after.srtPoison.empty() || before.nullRootReads != 0 || after.nullRootReads != 0 || before.flattenedSrt.size() != after.flattenedSrt.size()) {
        ArchB::Recorder::Count("fix_indirect_cpu_draw_rewalk_refused_snapshot");
        static const bool trace = std::getenv("APS5_TRACE_INDIRECT_DRAWS") != nullptr;
        if (trace) std::fprintf(stderr, "[indirect-cache] rewalk program 0x%llx refused buffers=%u images=%u samplers=%u tables=%u flat-size=%u poison=%u\n", static_cast<unsigned long long>(program.binary.codeAddress), before.buffers != after.buffers, before.images != after.images, before.samplers != after.samplers, before.tables != after.tables, before.flattenedSrt.size() != after.flattenedSrt.size(), !before.srtPoison.empty() || !after.srtPoison.empty() || before.nullRootReads != 0 || after.nullRootReads != 0);
        if (trace) {
            const auto differences = [&](const char* kind, const auto& old, const auto& live) {
                for (std::size_t resource = 0; resource < std::min(old.size(), live.size()); ++resource) {
                    for (std::size_t word = 0; word < old[resource].dwordCount; ++word) {
                        if (old[resource].dwords[word] != live[resource].dwords[word]) std::fprintf(stderr, "[indirect-cache] %s %zu word %zu stored=0x%x live=0x%x\n", kind, resource, word, old[resource].dwords[word], live[resource].dwords[word]);
                    }
                }
            };
            differences("buffer", before.buffers, after.buffers);
            differences("image", before.images, after.images);
            for (std::size_t table = 0; table < std::min(before.tables.tables.size(), after.tables.tables.size()); ++table) {
                const auto& old = before.tables.tables[table];
                const auto& live = after.tables.tables[table];
                if (old != live) std::fprintf(stderr, "[indirect-cache] table %zu base=0x%llx/0x%llx size=%u records=%u keys=%u fault=%u outside=%u codes=%u words=%u ranges=%u\n", table, static_cast<unsigned long long>(old.base), static_cast<unsigned long long>(live.base), old.size != live.size, old.records != live.records, old.keys != live.keys, old.fault != live.fault, old.outside != live.outside, old.codes != live.codes, old.words != live.words, old.readRanges != live.readRanges);
            }
        }
        return false;
    }
    auto compiled = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
    const auto bindingRefusal = [&] {
        ArchB::Recorder::Count("fix_indirect_cpu_draw_rewalk_refused_binding");
        static const bool trace = std::getenv("APS5_TRACE_INDIRECT_DRAWS") != nullptr;
        if (trace) std::fprintf(stderr, "[indirect-cache] rewalk program 0x%llx refused binding mapping\n", static_cast<unsigned long long>(program.binary.codeAddress));
        return false;
    };
    if (before.tables != after.tables && !ShaderRecompiler::RefreshCapturedImageTables(*compiled, *current)) return bindingRefusal();
    for (auto& binding : compiled->bindings) {
        const bool image = binding.role == ShaderRecompiler::DescriptorRole::GuestImages;
        if ((image && before.images != after.images) || (binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers && before.buffers != after.buffers)) {
            const auto& oldDescriptors = image ? before.images : before.buffers;
            const auto& liveDescriptors = image ? after.images : after.buffers;
            const auto width = image ? 8u : 4u;
            if (binding.guestDescriptor.size() != binding.count * width) return bindingRefusal();
            for (std::size_t offset = 0; offset < binding.guestDescriptor.size(); offset += width) {
                const auto first = binding.guestDescriptor.begin() + offset;
                if (image && std::all_of(first, first + width, [](auto word) { return word == 0u; })) continue;
                const ShaderRecompiler::DescriptorValue* replacement = nullptr;
                for (std::size_t resource = 0; resource < oldDescriptors.size(); ++resource) {
                    auto old = oldDescriptors[resource];
                    if (old.dwordCount != width) continue;
                    if (!std::equal(old.dwords.begin(), old.dwords.begin() + width, first)) {
                        if (!image) continue;
                        old.dwords[3] = (old.dwords[3] & ~0xfffu) | ShaderRecompiler::ShaderImageIdentitySwizzle;
                        if (!std::equal(old.dwords.begin(), old.dwords.begin() + width, first)) continue;
                    }
                    if (replacement != nullptr && *replacement != liveDescriptors[resource]) return bindingRefusal();
                    replacement = &liveDescriptors[resource];
                }
                if (replacement == nullptr) return bindingRefusal();
                std::copy_n(replacement->dwords.begin(), 2u, first);
            }
        }
        if (binding.role != ShaderRecompiler::DescriptorRole::FlattenedSrt) continue;
        if (binding.guestDescriptor != before.flattenedSrt) return bindingRefusal();
        binding.guestDescriptor = after.flattenedSrt;
    }
    if (before.images != after.images) {
        if (!compiled->runtimeImageResources.empty()) return bindingRefusal();
        const auto storage = std::find_if(compiled->bindings.begin(), compiled->bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; });
        auto data = storage != compiled->bindings.end() ? std::as_writable_bytes(std::span(storage->guestDescriptor)) : std::span(compiled->pushConstants);
        for (std::size_t resource = 0; compiled->runtimeImageCount != 0u && resource < before.images.size(); ++resource) {
            if (resource >= previous->plan->info.images.size() || resource >= compiled->runtimeImageCount) return bindingRefusal();
            if (previous->plan->info.images[resource].table != ShaderRecompiler::NoTable) continue;
            if (before.images[resource] == after.images[resource]) continue;
            const auto offset = compiled->imageMetadataDword * sizeof(std::uint32_t) + resource * sizeof(ShaderRecompiler::RuntimeAbi::ResourceMetadata) + offsetof(ShaderRecompiler::RuntimeAbi::ResourceMetadata, descriptor);
            const auto bytes = 8u * sizeof(std::uint32_t);
            if (offset > data.size() || bytes > data.size() - offset || std::memcmp(data.data() + offset, before.images[resource].dwords.data(), bytes) != 0) return bindingRefusal();
            std::memcpy(data.data() + offset, after.images[resource].dwords.data(), bytes);
        }
    }
    if (!before.tables.tables.empty()) {
        compiled->imageTableRanges = after.tables.ranges;
        compiled->imageTableReadRanges.clear();
        for (const auto& table : after.tables.tables) compiled->imageTableReadRanges.push_back(table.readRanges.Copy());
    }
    variant.compiled = std::move(compiled);
    variant.runs.clear();
    variant.runBases.clear();
    variant.words.clear();
    variant.shared.clear();
    variant.dataPositions.clear();
    variant.dataSlots.clear();
    variant.flatBinding = DispatchVariant::NoFlatBinding;
    variant.forgetSerial = GuestMemory::ForgetSerial();
    tagVariantRuns(variant, memory.Regions(), current->userDataUse, true, program.hiddenUserWords);
    recordStageDataPositions(variant, *current, {});
    if (variant.indirectCapture != nullptr) variant.indirectCapture = current;
    variant.relocationCapture = current;
    ArchB::Recorder::Count("fix_indirect_cpu_draw_rewalk_hits");
    return true;
} catch (const std::runtime_error& error) {
    ArchB::Recorder::Count("fix_indirect_cpu_draw_rewalk_refused_capture");
    static const bool trace = std::getenv("APS5_TRACE_INDIRECT_DRAWS") != nullptr;
    if (trace) std::fprintf(stderr, "[indirect-cache] rewalk program 0x%llx refused: %s\n", static_cast<unsigned long long>(program.binary.codeAddress), error.what());
    return false;
}

bool Driver::RefreshIndirectStage(ShaderRecompiler::RecompileResult& result, const ShaderRecompiler::ResourceCapture* capture, const DrawProgram& program, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertexInfo, const std::function<std::shared_ptr<const ShaderRecompiler::RecompileResult>()>& materialize, bool allowShaderData) {
    static const bool enabled = std::getenv("APS5_NO_INDIRECT_PUSH_PATCH") == nullptr;
    static const bool verify = std::getenv("APS5_VERIFY_INDIRECT_PUSH_PATCH") != nullptr;
    const bool patched = enabled && PatchIndirectPushConstants(result, capture, program, vertexInfo, allowShaderData);
    if (allowShaderData && patched && std::any_of(result.bindings.begin(), result.bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; })) ArchB::Recorder::Count("fix_indirect_cpu_draw_hits");
    else if (allowShaderData && !patched) ArchB::Recorder::Count("fix_indirect_cpu_draw_refresh_refused");
    if (!patched || verify) {
        const auto full = materialize();
        require(full != nullptr, "indirect stage materialization returned no result");
        if (patched) {
            std::vector<std::byte> patchedBytes, fullBytes;
            ShaderRecompiler::ShaderDiskCache::EncodeResult(result, patchedBytes);
            ShaderRecompiler::ShaderDiskCache::EncodeResult(*full, fullBytes);
            if (patchedBytes != fullBytes || result.variantId != full->variantId || result.specializationId != full->specializationId || result.cacheHit != full->cacheHit || result.meshBarycentrics != full->meshBarycentrics) {
                std::fprintf(stderr, "APS5_VERIFY_INDIRECT_PUSH_PATCH: program 0x%llx stage %u differs from re-materialization\n", static_cast<unsigned long long>(program.binary.codeAddress), static_cast<unsigned>(program.binary.stage));
                std::abort();
            }
        } else {
            result = *full;
        }
    }
    CountIndirectStage(patched, patched && verify);
    return patched;
}

bool Driver::IndirectBuiltinPlanSupported(const ShaderRecompiler::RecompileRequest& request) {
    try {
        if (!request.graphics) return false;
        auto ordinary = request;
        ordinary.graphics->instanceFromBase = false;
        ordinary.graphics->vertexFromBase = false;
        const auto plan = ShaderRecompiler::GetResourcePlan(ordinary);
        return plan != nullptr && plan->resourceTrackingComplete && plan->srtPlanComplete && !plan->requiresSpecializationMemory &&
               std::none_of(plan->valueStorage.begin(), plan->valueStorage.end(), [&](const auto& value) {
                   if (value.Opcode() != ShaderRecompiler::IrOpcode::GetUserData || value.ArgumentCount() != 1) return false;
                   const auto sgpr = static_cast<std::int32_t>(value.Argument(0)->Register().index);
                   return (request.graphics->instanceFromBase && sgpr == request.graphics->baseInstanceSgpr) || (request.graphics->vertexFromBase && sgpr == request.graphics->baseVertexSgpr);
               });
    } catch (const std::runtime_error&) {
        return false;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

std::uint32_t Driver::drawUserWord(const DrawProgram& program, std::int32_t sgpr) {
    require(sgpr >= 0 && static_cast<std::uint32_t>(sgpr) >= program.firstUserSgpr, "invalid draw offset SGPR");
    const auto index = static_cast<std::uint32_t>(sgpr) - program.firstUserSgpr;
    require(index < program.userData.size(), "draw offset SGPR exceeds user data");
    return program.userData[index];
}

void Driver::FoldDrawOffsets(const ShaderRecompiler::RecompileResult& result, const DrawProgram& program, Pm4::DrawParameters& parameters) {
    if (result.vertexOffsetSgpr >= 0) {
        const auto offset = drawUserWord(program, result.vertexOffsetSgpr);
        require(offset <= std::numeric_limits<std::uint32_t>::max() - parameters.firstVertex, "draw vertex offset overflow");
        parameters.firstVertex += offset;
    }
    if (result.instanceOffsetSgpr >= 0) parameters.firstInstance = drawUserWord(program, result.instanceOffsetSgpr);
}

std::optional<Graphics::IndirectDrawPath> Driver::ClassifyIndirectDraw(const ShaderRecompiler::RecompileResult& result, const Graphics::State& graphics, const DrawProgram& frontProgram, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertexInfo, const std::shared_ptr<VulkanDevice>& localDevice, Pm4::DrawParameters& drawParameters, bool traceIndirect) {
    std::optional<Graphics::IndirectDrawPath> indirectCpu;
    auto& indirect = *drawParameters.indirect;
    using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
    using Path = Graphics::IndirectDrawPath;
    const auto support = localDevice->DrawIndirectSupport();
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    bool notFoldedMismatch = false, notFoldedShared = false;
    const auto fail = [&](Path reason) { if (!indirectCpu) indirectCpu = reason; };
    if (drawParameters.indexed && !indirect.indexLimitKnown) fail(Path::IndexLimitUnknown);
    else if (drawParameters.indexed && !GuestMemory::Accessible(reinterpret_cast<const void*>(drawParameters.indexAddress), static_cast<std::size_t>(drawParameters.indexCount) * drawParameters.indexSize)) fail(Path::IndexRange);
    constexpr std::uint32_t NoLocation = 0x280u;
    const bool meshPath = graphics.stages.mesh.has_value() && !graphics.stages.tessellation && !graphics.rectList;
    const bool meshGpuSide = meshPath && drawParameters.indexed && indirect.count == 1 && !indirect.countIndirect && indirect.baseVertexLocation == NoLocation && indirect.startInstanceLocation == NoLocation && (!indirect.drawIndexEnabled || indirect.drawIndexLocation == NoLocation);
    static const bool rectListOnGpu = std::getenv("APS5_NO_RECT_INDIRECT") == nullptr;
    const bool rectListGpuSide = rectListOnGpu && graphics.rectList && !graphics.stages.mesh && !graphics.stages.tessellation;
    if ((graphics.stages.path != Graphics::ShaderPath::Vertex || (graphics.rectList && !rectListGpuSide)) && !meshGpuSide) fail(Path::NonVertexPath);
    if (indirect.drawIndexEnabled && indirect.drawIndexSgpr >= 0) fail(Path::DrawIndex);
    if (indirect.countIndirect && !support.count) fail(Path::FeatureGap);
    if (indirect.countIndirect && indirect.count > 1 && !support.multi) fail(Path::FeatureGap);
    const auto reads = [&](std::int32_t sgpr) {
        const auto word = static_cast<std::int64_t>(sgpr) - static_cast<std::int64_t>(frontProgram.firstUserSgpr);
        if (word < 0 || word >= 64) return true;
        if (vertexInfo && vertexInfo->resourcesNum != 0) {
            for (const auto reg : {static_cast<std::int64_t>(vertexInfo->fetchAttribReg), static_cast<std::int64_t>(vertexInfo->fetchBufferReg)}) {
                if (word == reg || word == reg + 1) return true;
            }
        }
        return ((result.userDataRegisterReads >> word) & 1u) != 0;
    };
    const auto dimension = [&](std::int32_t k, std::int32_t p, bool shared, bool conflict, std::uint32_t x, Rule& rule, std::uint32_t& constant) {
        if (p < 0 || (k < 0 && !conflict && !reads(p))) {
            rule = Rule::Constant;
            const auto offset = k >= 0 ? drawUserWord(frontProgram, k) : 0u;
            require(offset <= std::numeric_limits<std::uint32_t>::max() - x, "draw vertex offset overflow");
            constant = x + offset;
        } else if (conflict) fail(Path::FetchUnknown);
        else if (k != p || shared) {
            fail(Path::NotFolded);
            if (profile) {
                notFoldedMismatch |= k != p;
                notFoldedShared |= shared;
            }
        } else if (x != 0) fail(Path::IndxOffset);
        else rule = Rule::InPlace;
    };
    if (indirect.vertexFromBase) indirect.vertexRule = Rule::InPlace;
    else dimension(result.vertexOffsetSgpr, indirect.baseVertexSgpr, result.vertexOffsetShared, result.vertexOffsetConflict, drawParameters.indexed ? 0u : indirect.indxOffset, indirect.vertexRule, indirect.vertexConstant);
    if (indirect.instanceFromBase) indirect.instanceRule = Rule::InPlace;
    else dimension(result.instanceOffsetSgpr, indirect.startInstanceSgpr, result.instanceOffsetShared, result.instanceOffsetConflict, 0u, indirect.instanceRule, indirect.instanceConstant);
    if (!support.firstInstance && (indirect.instanceRule == Rule::InPlace || indirect.instanceConstant != 0)) fail(Path::FeatureGap);
    if ((indirect.vertexRule == Rule::Constant || indirect.instanceRule == Rule::Constant) && indirect.count > 256) fail(Path::FeatureGap);

    static const std::uint64_t vertexCap = [] { const char* text = std::getenv("APS5_INDIRECT_VERTEX_MIB"); return (text ? std::strtoull(text, nullptr, 10) : 64ull) << 20u; }();
    for (const auto& attribute : result.vertexAttributes) {
        const auto stride = (attribute.resource.fields[1] >> 16u) & 0x3fffu;
        const auto extent = stride == 0 ? static_cast<std::uint64_t>(attribute.resource.fields[2]) : static_cast<std::uint64_t>(attribute.resource.fields[2]) * stride;
        if (extent > vertexCap) fail(Path::VertexRange);
    }
    if (traceIndirect) {
        const auto location = [](std::uint32_t value, std::int32_t sgpr) { char text[24]; if (value == 0x280u) std::snprintf(text, sizeof(text), "none"); else std::snprintf(text, sizeof(text), "0x%x(s%d)", value, sgpr); return std::string(text); };
        const auto rule = [](Rule value, std::uint32_t constant) { char text[24]; if (value == Rule::InPlace) std::snprintf(text, sizeof(text), "in-place"); else std::snprintf(text, sizeof(text), "const %u", constant); return std::string(text); };
        std::string decision = indirectCpu ? "cpu (" + std::string(Graphics::IndirectDrawPathName(*indirectCpu)) + ")" : "gpu vertex=" + rule(indirect.vertexRule, indirect.vertexConstant) + " instance=" + rule(indirect.instanceRule, indirect.instanceConstant);
        std::fprintf(stderr, "[draw] indirect program 0x%llx opcode 0x%x args 0x%llx stride %u count %u%s locs base=%s inst=%s idx=%s%s analyzer v=%d i=%d shared=%d/%d conflict=%d/%d indx=%u user reads 0x%llx (first %u, hidden %u) -> %s\n", static_cast<unsigned long long>(frontProgram.binary.codeAddress), indirect.opcode, static_cast<unsigned long long>(indirect.arguments), indirect.stride, indirect.count, indirect.countIndirect ? " (indirect)" : "", location(indirect.baseVertexLocation, indirect.baseVertexSgpr).c_str(), location(indirect.startInstanceLocation, indirect.startInstanceSgpr).c_str(), location(indirect.drawIndexLocation, indirect.drawIndexSgpr).c_str(), indirect.drawIndexEnabled ? " (enabled)" : "", result.vertexOffsetSgpr, result.instanceOffsetSgpr, result.vertexOffsetShared ? 1 : 0, result.instanceOffsetShared ? 1 : 0, result.vertexOffsetConflict ? 1 : 0, result.instanceOffsetConflict ? 1 : 0, indirect.indxOffset, static_cast<unsigned long long>(result.userDataRegisterReads), static_cast<unsigned>(frontProgram.firstUserSgpr), static_cast<unsigned>(frontProgram.hiddenUserWords), decision.c_str());
    }
    constexpr std::array<const char*, static_cast<std::size_t>(Path::Count)> counters{
        "fix_indirect_cpu_draw_path_gpu", "fix_indirect_cpu_draw_path_not_folded", "fix_indirect_cpu_draw_path_fetch_unknown",
        "fix_indirect_cpu_draw_path_non_vertex", "fix_indirect_cpu_draw_path_index_offset", "fix_indirect_cpu_draw_path_draw_index",
        "fix_indirect_cpu_draw_path_vertex_range", "fix_indirect_cpu_draw_path_feature_gap", "fix_indirect_cpu_draw_path_pending_image",
        "fix_indirect_cpu_draw_path_pending_label_or_copy", "fix_indirect_cpu_draw_path_not_imported", "fix_indirect_cpu_draw_path_disabled",
        "fix_indirect_cpu_draw_path_index_range", "fix_indirect_cpu_draw_path_index_limit_unknown"
    };
    if (indirectCpu) ArchB::Recorder::Count(counters[static_cast<std::size_t>(*indirectCpu)]);
    if (profile) CountIndirectFold(indirectCpu, indirect.vertexFromBase || indirect.instanceFromBase, notFoldedMismatch, notFoldedShared);
    return indirectCpu;
}

}
