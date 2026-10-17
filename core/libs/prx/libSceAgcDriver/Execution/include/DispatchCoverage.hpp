#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DISPATCHCOVERAGE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DISPATCHCOVERAGE_HPP

#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace AgcDriver::DispatchCoverage {

enum Reason : std::size_t {
    Candidate, InitialMiss, InitialReads, InitialVerify, RecipeUnbounded, Pipelined,
    LookupNotCached, LookupNoVariant, HitNoRecipe, HitNoRecipeBda, HitNoRecipeFault, HitNoRecipeGds, HitNoRecipeOther,
    AttachVariant, AttachStamp, AttachPoison, AttachMissing,
    BuildNoDestination, BuildDisabled, BuildUncacheable, BuildBda, BuildLease, BuildCopiedWrites, BuildNotReusable, BuildCompletion, BuildNoObjects, BuildEmptyKey, Built,
    Count
};

inline constexpr std::array<const char*, Count> Names{
    "candidate", "initial-miss", "initial-reads", "initial-verify", "recipe-unbounded", "pipelined",
    "lookup-not-cached", "lookup-no-variant", "hit-no-recipe", "hit-no-recipe-bda", "hit-no-recipe-fault", "hit-no-recipe-gds", "hit-no-recipe-other",
    "attach-variant", "attach-stamp", "attach-poison", "attach-missing",
    "build-no-destination", "build-disabled", "build-uncacheable", "build-bda", "build-lease", "build-copied-writes", "build-not-reusable-other", "build-completion", "build-no-objects", "build-empty-key", "built"
};
inline constexpr std::array<const char*, 3> Scopes{"q0", "other-queues", "no-packet"};
inline std::array<std::array<std::atomic<std::uint64_t>, Count>, Scopes.size()> Counts{};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_PROFILE_DISPATCH_COVERAGE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

inline void Note(Reason reason) {
    if (!Enabled()) return;
    const auto packet = GuestMemory::CurrentPacket();
    const auto scope = packet.opcode == GuestMemory::NoPacket ? 2u : packet.queue == 0 ? 0u : 1u;
    Counts[scope][reason].fetch_add(1, std::memory_order_relaxed);
}

inline void Report() {
    if (!Enabled()) return;
    static std::atomic<std::int64_t> lastReport{0};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = lastReport.load(std::memory_order_relaxed);
    if (last != 0 && now - last < 10000) return;
    if (!lastReport.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
    const double seconds = last == 0 ? 0.0 : static_cast<double>(now - last) / 1000.0;
    for (std::size_t scope = 0; scope < Scopes.size(); ++scope) {
        std::string totals;
        for (std::size_t i = 0; i < Count; ++i) {
            totals += ' ';
            totals += Names[i];
            totals += '=';
            totals += std::to_string(Counts[scope][i].load(std::memory_order_relaxed));
        }
        std::fprintf(stderr, "[dispatch-coverage] %s %.3f s since report; cumulative:%s\n", Scopes[scope], seconds, totals.c_str());
    }
}

}

#endif
