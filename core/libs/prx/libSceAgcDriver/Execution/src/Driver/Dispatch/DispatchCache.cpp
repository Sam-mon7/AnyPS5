#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <cstdlib>
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"

namespace AgcDriver::DriverDetail {

bool Driver::stampValidate() {
    static const bool stamp = std::getenv("APS5_DISPATCH_STAMP_VALIDATE") != nullptr;
    return stamp;
}

bool Driver::dataHits() {
    static const bool hits = std::getenv("APS5_NO_DATA_HITS") == nullptr && VulkanDevice::TemplateDataRefresh();
    return hits;
}

bool Driver::verifyDataHits() {
    static const bool verify = std::getenv("APS5_VERIFY_DATA_HITS") != nullptr;
    return verify;
}

bool Driver::verifyDispatchPointerSample() {
    static const std::uint64_t interval = [] {
        const char* text = std::getenv("APS5_ARCHB_FIX_DISPATCH_POINTER_KEYS_VERIFY");
        return text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
    }();
    static std::atomic<std::uint64_t> served{0};
    return interval != 0 && (served.fetch_add(1, std::memory_order_relaxed) + 1) % interval == 0;
}

std::uint64_t DispatchPointerKey(std::uint64_t identity, std::span<const std::uint32_t> userData, std::uint64_t pointerWords) {
    std::uint64_t key = identity ^ 0xd6e8feb86659fd93ull;
    const auto mix = [&](std::uint64_t value) { key = (key ^ value) * 0x100000001b3ull; };
    mix(userData.size());
    for (std::size_t index = 0; index < userData.size(); ++index) {
        mix(index < 64u && ((pointerWords >> index) & 1u) != 0u ? 0x9e3779b900000000ull | index : userData[index]);
    }
    return key;
}

std::size_t Driver::dispatchVariants() {
    static const std::size_t variants = [] {
        if (stampValidate()) return std::size_t{1};
        const char* text = std::getenv("APS5_DISPATCH_VARIANTS");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 4ull;
        return static_cast<std::size_t>(std::clamp<unsigned long long>(parsed, 1, MaxDispatchVariants));
    }();
    return variants;
}

bool Driver::insertCompare() {
    static const bool compare = std::getenv("APS5_INSERT_COMPARE") != nullptr;
    return compare;
}

std::size_t Driver::dispatchCacheEntries() {
    static const std::size_t entries = [] {
        const char* text = std::getenv("APS5_DISPATCH_CACHE_ENTRIES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        if (parsed != 0) return static_cast<std::size_t>(parsed);
        if (stampValidate()) return std::size_t{4096};
        return dispatchVariants() > 2 ? std::size_t{65536} : std::size_t{131072};
    }();
    return entries;
}

std::uint64_t Driver::DispatchCacheBytes() {
    static const std::uint64_t bytes = [] {
        const char* text = std::getenv("APS5_DISPATCH_CACHE_BYTES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        return parsed != 0 ? static_cast<std::uint64_t>(parsed) : std::uint64_t{256} << 20u;
    }();
    return bytes;
}

EntryCounters Driver::DispatchCacheCounters() {
    std::lock_guard lock(dispatchCacheMutex);
    return entryCounters;
}

Driver::DispatchCacheFootprint Driver::DispatchCacheSize() {
    std::lock_guard lock(dispatchCacheMutex);
    return {dispatchCache.size(), dispatchCacheVariantBytes};
}

std::uint64_t Driver::variantBytes(const DispatchVariant& variant) {
    std::uint64_t feedbackBytes = variant.feedback.bindings.size() * sizeof(WordBitMasks);
    for (const auto& binding : variant.feedback.bindings) feedbackBytes += binding.size() * sizeof(std::pair<std::uint32_t, std::uint32_t>);
    return variant.bufferBaseMasks.size() * sizeof(WordBitMasks::value_type) + variant.bufferBasePatches.size() * sizeof(DispatchVariant::BufferBasePatch) + variant.feedbackPatches.size() * sizeof(DispatchVariant::FeedbackPatch) + feedbackBytes + sizeof(DispatchVariant) + (variant.feedback.memory.size() + variant.feedback.userData.size() + variant.feedback.pushConstants.size()) * sizeof(std::pair<std::uint32_t, std::uint32_t>) + variant.userData.size() * sizeof(std::uint32_t) + variant.words.size() * sizeof(std::uint32_t) + variant.shared.size() * sizeof(SharedWords) + variant.runs.size() * sizeof(std::pair<std::uint64_t, std::uint64_t>) + variant.rebasedRuns.size() * sizeof(std::uint8_t) + variant.captured.size() * sizeof(ShaderRecompiler::MemoryRegion) + (variant.vertexInfo != nullptr ? sizeof(ShaderRecompiler::ShaderVertexStageInfo) : 0) + (variant.indirectCapture != nullptr ? sizeof(ShaderRecompiler::ResourceCapture) + variant.indirectCapture->snapshot.userData.size() * sizeof(std::uint32_t) : 0);
}

void Driver::accountVariant(const DispatchVariant& variant, bool added) {
    if (added) {
        const auto masks = variant.feedback.memory.size() + variant.feedback.userData.size();
        if (masks != 0u) ArchB::Recorder::Count("fix_tsharp_mask_positions", masks);
        ++dispatchCacheVariants;
        dispatchCacheVariantBytes += variantBytes(variant);
    } else {
        --dispatchCacheVariants;
        dispatchCacheVariantBytes -= variantBytes(variant);
    }
}

void Driver::eraseDispatchEntry(std::unordered_map<std::uint64_t, std::shared_ptr<DispatchEntry>>::iterator it) {
    for (const auto& variant : it->second->variants) accountVariant(*variant, false);
    dispatchOrder.erase(it->second->order);
    dispatchCache.erase(it);
}

}
