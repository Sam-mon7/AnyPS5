#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include <cstdlib>
#include <cstring>

namespace AgcDriver::DriverDetail {

void Driver::verifyDataHit(const ShaderSnapshot& snapshot, std::size_t codeOffset, ShaderRecompiler::RecompileRequest request, std::span<const ShaderRecompiler::MemoryRegion> memory, std::uint64_t address, const DispatchVariant& variant, std::span<const std::uint32_t> liveWords, const ShaderRecompiler::RecompileResult& patched, bool feedbackMask) {
    const auto fail = [&](const char* what, std::size_t position, std::size_t slot) {
        std::fprintf(stderr, "[dispatch-cache] APS5_VERIFY_DATA_HITS: %s disagrees (program 0x%llx, position %zu, slot %zu)\n", what, static_cast<unsigned long long>(address), position, slot);
        std::fflush(stderr);
        std::abort();
    };
    auto verifyMemory = std::make_shared<ShaderMemory>(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter(), &QueryPendingSpan);
    auto invocation = InvocationFor(snapshot, codeOffset, request);
    const auto capture = CaptureInvocation(*verifyMemory, snapshot, codeOffset, request, invocation);
    const auto regions = verifyMemory->Regions();
    request.context.memory = regions;
    const auto result = invocation.Materialize(*capture);
    std::size_t position = 0, run = 0;
    for (const auto& region : regions) {
        const bool registered = std::any_of(memory.begin(), memory.end(), [&](const auto& known) { return region.guestAddress >= known.guestAddress && region.guestAddress < known.guestAddress + known.bytes.size(); });
        if (registered) continue;
        if (run >= variant.runs.size() || variant.runs[run].first != region.guestAddress || variant.runs[run].second != region.guestAddress + region.bytes.size()) fail("the read set", position, run);
        const auto count = region.bytes.size() / sizeof(std::uint32_t);
        if (position + count > liveWords.size()) fail("the captured bytes", position, run);
        for (std::size_t word = 0; word < count; ++word) {
            std::uint32_t fresh;
            std::memcpy(&fresh, region.bytes.data() + word * 4u, 4u);
            if (((fresh ^ liveWords[position + word]) & ~(feedbackMask ? WordMask(variant.feedback.memory, position + word) : 0u)) != 0u) fail("the captured bytes", position + word, run);
        }
        position += count;
        ++run;
    }
    if (run != variant.runs.size()) fail("the read set", position, run);
    if (result->variantId != patched.variantId) fail("the variant", 0, 0);
    if (!FeedbackBindingsEqual(variant, patched, *result, feedbackMask)) fail("the bindings", 0, 0);
    std::lock_guard cacheLock(dispatchCacheMutex);
    ++entryCounters.dataVerified;
}

void Driver::verifyDispatchPointerHit(const ShaderSnapshot& snapshot, std::size_t codeOffset, ShaderRecompiler::RecompileRequest request, std::span<const ShaderRecompiler::MemoryRegion> memory, std::uint64_t address, const DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> captured, const ShaderRecompiler::RecompileResult& prepared, bool feedbackMask, bool dataHit) {
    auto verifyMemory = std::make_shared<ShaderMemory>(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter(), &QueryPendingSpan, feedbackMask);
    auto invocation = InvocationFor(snapshot, codeOffset, request);
    const auto capture = CaptureInvocation(*verifyMemory, snapshot, codeOffset, request, invocation);
    const auto freshCaptured = verifyMemory->Regions();
    request.context.memory = freshCaptured;
    const auto fresh = invocation.Materialize(*capture);
    const auto capturedBytes = [](std::span<const ShaderRecompiler::MemoryRegion> regions) {
        std::vector<std::pair<std::uint64_t, std::byte>> bytes;
        for (const auto& region : regions) {
            for (std::size_t i = 0; i < region.bytes.size(); ++i) bytes.emplace_back(region.guestAddress + i, region.bytes[i]);
        }
        std::sort(bytes.begin(), bytes.end());
        bytes.erase(std::unique(bytes.begin(), bytes.end()), bytes.end());
        return bytes;
    };
    const auto servedBytes = capturedBytes(captured);
    const auto legacyBytes = capturedBytes(freshCaptured);
    const char* mismatch = servedBytes == legacyBytes ? nullptr : "captured bytes";
    if (mismatch != nullptr) {
        auto served = servedBytes.begin();
        auto legacy = legacyBytes.begin();
        while (served != servedBytes.end() && legacy != legacyBytes.end() && *served == *legacy) {
            ++served;
            ++legacy;
        }
        const auto differing = served == servedBytes.end() ? legacy->first : legacy == legacyBytes.end() ? served->first : std::min(served->first, legacy->first);
        const auto servedByte = served != servedBytes.end() && served->first == differing ? static_cast<int>(std::to_integer<unsigned char>(served->second)) : -1;
        const auto legacyByte = legacy != legacyBytes.end() && legacy->first == differing ? static_cast<int>(std::to_integer<unsigned char>(legacy->second)) : -1;
        const auto region = std::find_if(freshCaptured.begin(), freshCaptured.end(), [&](const auto& item) { return differing >= item.guestAddress && differing - item.guestAddress < item.bytes.size(); });
        const auto fallback = std::find_if(captured.begin(), captured.end(), [&](const auto& item) { return differing >= item.guestAddress && differing - item.guestAddress < item.bytes.size(); });
        const auto regionAddress = region != freshCaptured.end() ? region->guestAddress : fallback != captured.end() ? fallback->guestAddress : differing;
        const auto regionSize = region != freshCaptured.end() ? region->bytes.size() : fallback != captured.end() ? fallback->bytes.size() : 1u;
        bool rebased = false;
        bool refreshed = false;
        std::size_t position = 0;
        for (std::size_t run = 0; run < variant.runs.size(); ++run) {
            const auto [begin, end] = variant.runs[run];
            const auto bytes = static_cast<std::size_t>(end - begin);
            if (differing >= begin && differing < end) {
                rebased = run < variant.rebasedRuns.size() && variant.rebasedRuns[run] != 0;
                if (dataHit && servedByte >= 0 && position + (differing - begin) / sizeof(std::uint32_t) < variant.words.size()) {
                    const auto original = std::as_bytes(std::span(variant.words))[position * sizeof(std::uint32_t) + differing - begin];
                    refreshed = servedByte != static_cast<int>(std::to_integer<unsigned char>(original));
                }
                break;
            }
            position += bytes / sizeof(std::uint32_t);
        }
        char servedText[8], legacyText[8];
        if (servedByte < 0) std::snprintf(servedText, sizeof(servedText), "missing");
        else std::snprintf(servedText, sizeof(servedText), "%02x", servedByte);
        if (legacyByte < 0) std::snprintf(legacyText, sizeof(legacyText), "missing");
        else std::snprintf(legacyText, sizeof(legacyText), "%02x", legacyByte);
        std::fprintf(stderr, "[dispatch-cache] pointer verification captured mismatch program=0x%llx region=0x%llx size=%zu offset=%llu served=%s legacy=%s rebased=%u refreshed=%u\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(regionAddress), regionSize, static_cast<unsigned long long>(differing - regionAddress), servedText, legacyText, rebased ? 1u : 0u, refreshed ? 1u : 0u);
        std::fflush(stderr);
    }
    if (mismatch == nullptr && prepared.PipelineVariantId() != fresh->PipelineVariantId()) mismatch = "compiled variant";
    if (mismatch == nullptr && prepared.Identity() != fresh->Identity()) mismatch = "pipeline identity";
    if (mismatch == nullptr && prepared.pushConstants != fresh->pushConstants) mismatch = "push words";
    if (mismatch == nullptr && prepared.bindings.size() != fresh->bindings.size()) mismatch = "descriptor count";
    for (std::size_t i = 0; mismatch == nullptr && i < prepared.bindings.size(); ++i) {
        const auto& a = prepared.bindings[i];
        const auto& b = fresh->bindings[i];
        if (a.kind != b.kind || a.role != b.role || a.descriptorSet != b.descriptorSet || a.binding != b.binding || a.count != b.count || a.readOnly != b.readOnly || a.imageShape != b.imageShape || a.imageShapes != b.imageShapes || a.imageSamplers != b.imageSamplers || a.bufferExtent != b.bufferExtent || a.Flags() != b.Flags()) mismatch = "descriptor shape";
        else if (a.guestDescriptor != b.guestDescriptor) mismatch = "descriptor words";
    }
    if (mismatch == nullptr) {
        ArchB::Recorder::Count("fix_dptr_verified");
        return;
    }
    ArchB::Recorder::Count("fix_dptr_verify_mismatches");
    char where[96];
    std::snprintf(where, sizeof(where), "dispatch pointer-key verification failed for compute shader 0x%llx: %s", static_cast<unsigned long long>(address), mismatch);
    throw std::runtime_error(where);
}

}
