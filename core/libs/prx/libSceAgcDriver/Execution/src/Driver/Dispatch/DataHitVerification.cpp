#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "Optimization/ResourceProgram.hpp"
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

}
