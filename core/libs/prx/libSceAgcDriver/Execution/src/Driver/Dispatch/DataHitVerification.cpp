#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include <map>
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

void Driver::verifyDispatchRelocationRecipe(const ShaderSnapshot& snapshot, std::size_t codeOffset, ShaderRecompiler::RecompileRequest request, std::span<const ShaderRecompiler::MemoryRegion> memory, std::uint64_t address, const DispatchVariant& variant, std::span<const std::uint32_t> liveWords, const ShaderRecompiler::RecompileResult& patched, bool feedbackMask, bool imageTableSnapshotReuse, bool imageTableBitScanKeys) {
    const auto fail = [&](const char* what) {
        std::fprintf(stderr, "[dispatch-cache] APS5_VERIFY_DISPATCH_RELOCATION_RECIPE: %s disagrees (program 0x%llx)\n", what, static_cast<unsigned long long>(address));
        std::fflush(stderr);
        std::abort();
    };
    auto verifyMemory = std::make_shared<ShaderMemory>(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter(), &QueryPendingSpan, feedbackMask);
    verifyMemory->SetImageTableSnapshotReuse(imageTableSnapshotReuse);
    verifyMemory->SetImageTableBitScanKeys(imageTableBitScanKeys);
    auto invocation = InvocationFor(snapshot, codeOffset, request);
    const auto capture = CaptureInvocation(*verifyMemory, snapshot, codeOffset, request, invocation);
    const auto result = invocation.Materialize(*capture);
    std::map<std::uint64_t, std::uint32_t> fresh;
    for (const auto& region : verifyMemory->Regions()) {
        const bool registered = std::any_of(memory.begin(), memory.end(), [&](const auto& known) { return region.guestAddress >= known.guestAddress && region.guestAddress < known.guestAddress + known.bytes.size(); });
        if (registered) continue;
        if (region.bytes.size() % sizeof(std::uint32_t) != 0) fail("the read alignment");
        for (std::size_t offset = 0; offset < region.bytes.size(); offset += sizeof(std::uint32_t)) {
            std::uint32_t value;
            std::memcpy(&value, region.bytes.data() + offset, sizeof(value));
            const auto [found, inserted] = fresh.emplace(region.guestAddress + offset, value);
            if (!inserted && found->second != value) fail("overlapping fresh reads");
        }
    }
    std::size_t position = 0;
    std::set<std::uint64_t> checked;
    for (const auto& [begin, end] : variant.runs) {
        if (end < begin || (end - begin) % sizeof(std::uint32_t) != 0) fail("the read ranges");
        for (auto at = begin; at < end; at += sizeof(std::uint32_t), ++position) {
            const auto found = fresh.find(at);
            if (found == fresh.end() || position >= liveWords.size()) fail("the read set");
            const auto mask = feedbackMask ? WordMask(variant.feedback.memory, position) : 0u;
            if (((found->second ^ liveWords[position]) & ~mask) != 0) fail("the captured bytes");
            checked.insert(at);
        }
    }
    if (position != liveWords.size() || checked.size() != fresh.size()) {
        std::fprintf(stderr, "[dispatch-relocation-verify] program=0x%llx run_words=%zu live_words=%zu candidate_unique=%zu fresh_unique=%zu runs=%zu snapshot_reuse=%u bit_scan_keys=%u descriptor_trace=%u\n", static_cast<unsigned long long>(address), position, liveWords.size(), checked.size(), fresh.size(), variant.runs.size(), imageTableSnapshotReuse, imageTableBitScanKeys, feedbackMask);
        std::size_t shown = 0;
        for (const auto& [at, value] : fresh) {
            static_cast<void>(value);
            if (checked.contains(at)) continue;
            std::fprintf(stderr, "[dispatch-relocation-verify] fresh_only=0x%llx\n", static_cast<unsigned long long>(at));
            if (++shown == 8) break;
        }
        for (std::size_t run = 0; run < std::min<std::size_t>(variant.runs.size(), 8); ++run) {
            const auto root = run < variant.runBases.size() ? variant.runBases[run] : -2;
            const auto parent = run < variant.runParents.size() ? variant.runParents[run].run : DispatchVariant::NoFlatBinding;
            std::fprintf(stderr, "[dispatch-relocation-verify] run=%zu begin=0x%llx end=0x%llx root=%d parent=%u\n", run, static_cast<unsigned long long>(variant.runs[run].first), static_cast<unsigned long long>(variant.runs[run].second), root, parent);
        }
        fail(position != liveWords.size() ? "the run word count" : "the read address coverage");
    }
    if (result->variantId != patched.variantId || result->runtimeAbiVersion != patched.runtimeAbiVersion || result->bdaAbiVersion != patched.bdaAbiVersion) fail("the compiled variant");
    if (!FeedbackBindingsEqual(variant, patched, *result, feedbackMask)) fail("the bindings or push constants");
    ArchB::Recorder::Count("dispatch_relocation_recipe_verified");
}

}
