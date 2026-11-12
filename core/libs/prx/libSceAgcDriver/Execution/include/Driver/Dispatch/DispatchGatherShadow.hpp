#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCH_DISPATCHGATHERSHADOW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCH_DISPATCHGATHERSHADOW_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchGatherGraph.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace AgcDriver::DriverDetail::DispatchShadow {

inline constexpr std::size_t VariantLimit = 8;

enum class SerialPhase : std::uint8_t { ReadPre, ReadPost, EvaluateBegin, EvaluateEnd, ResolvedEntry, LearnIssued, LearnCompare, LearnEnd, FalseDiffer, MaterializedCompare, Count };

enum class Population : std::uint8_t { ImageRead, Guarded, ImageWrite, MergedControl, Count };
enum class PopulationStage : std::uint8_t {
    Calls, Evaluated, Matched, Gathered, MaterializedChecks, QueuedWithoutDrain,
    CommitChecks, CommittedWithoutRebuild, QueuedCommittedWithoutRebuild,
    FalseMatch, FalseDiffer, MaterializedMismatch, CommitMismatch, GuardedReads,
    FallbackReads, Nanoseconds, WriterEffectChecks, WriterEffectMismatch, Count
};

struct Counters {
    std::uint64_t calls = 0;
    std::uint64_t aheadRefused = 0;
    std::uint64_t aheadRefusedOther = 0;
    std::uint64_t aheadCandidates = 0;
    std::uint64_t aheadAdopted = 0;
    std::uint64_t aheadFallback = 0;
    std::uint64_t faultAbiCalls = 0;
    std::uint64_t addressTableCalls = 0;
    std::uint64_t callsRefused = 0;
    std::uint64_t indexFull = 0;
    std::uint64_t templatesBuilt = 0;
    std::uint64_t learnedOnMiss = 0;
    std::uint64_t variantSearches = 0;
    std::uint64_t variantHits = 0;
    std::uint64_t variantsLearned = 0;
    std::uint64_t variantFull = 0;
    std::uint64_t variantMissingCapture = 0;
    std::uint64_t matchedLostBeforeMaterializedSerial = 0;
    std::uint64_t matchedLostBeforeMaterializedViews = 0;
    std::uint64_t materializedLostSerial = 0;
    std::uint64_t materializedLostViews = 0;
    std::uint64_t variantStaleViews = 0;
    std::array<std::uint64_t, VariantLimit + 1> variantBucketUses{};
    std::array<std::uint64_t, VariantLimit> variantSelected{};
    std::array<std::uint64_t, VariantLimit> variantLearnedSlots{};
    std::uint64_t indexMiss = 0;
    std::uint64_t matched = 0;
    std::uint64_t gathered = 0;
    std::uint64_t evaluated = 0;
    std::uint64_t commitNanoseconds = 0;
    std::uint64_t queuedWithoutDrain = 0;
    std::uint64_t committedWithoutRebuild = 0;
    std::uint64_t queuedCommittedWithoutRebuild = 0;
    std::uint64_t falseMatch = 0;
    std::uint64_t falseDiffer = 0;
    std::uint64_t absentMatched = 0;
    std::uint64_t differingMatched = 0;
    std::uint64_t guardedReads = 0;
    std::uint64_t fallbackReads = 0;
    std::uint64_t unreadable = 0;
    std::uint64_t pending = 0;
    std::uint64_t queued = 0;
    std::uint64_t shadowed = 0;
    std::uint64_t labels = 0;
    std::uint64_t serialMoved = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(SerialPhase::Count)> serialPhases{};
    std::array<std::uint64_t, 8> serialComponents{};
    std::array<std::array<std::uint64_t, 8>, static_cast<std::size_t>(SerialPhase::Count)> serialChanges{};
    std::uint64_t poolBusy = 0;
    std::uint64_t verifiedResolutions = 0;
    std::uint64_t payloadReady = 0;
    std::uint64_t materializedChecks = 0;
    std::uint64_t materializedMismatch = 0;
    std::uint64_t commitChecks = 0;
    std::uint64_t writerMultiUnitChecks = 0;
    std::uint64_t commitMismatch = 0;
    std::uint64_t commitRefused = 0;
    std::array<std::uint64_t, 18> commitRefusals{};
    std::uint64_t nanoseconds = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DispatchGatherRefusal::Count)> graphRefusals{};
    std::array<std::uint64_t, static_cast<std::size_t>(DispatchGatherRefusal::Count)> aheadGraphRefusals{};
    std::array<std::array<std::uint64_t, static_cast<std::size_t>(PopulationStage::Count)>, static_cast<std::size_t>(Population::Count)> populations{};
    std::array<std::array<std::uint64_t, static_cast<std::size_t>(DispatchGatherRefusal::Count)>, static_cast<std::size_t>(Population::Count)> populationRefusals{};
    std::array<std::uint64_t, static_cast<std::size_t>(DispatchGatherDifference::Count)> differences{};
    std::array<std::uint64_t, static_cast<std::size_t>(DispatchGatherDifference::Count)> falseDifferences{};
};

struct Use;

struct CommitInput {
    const DispatchGatherUse* values = nullptr;
    std::array<std::uint32_t, 3> groups{};
    std::uint64_t arguments = 0;
    std::array<std::uint32_t, 3> threadGroup{};
};

bool Enabled();
bool Strict();
Counters ReadCounters();
void SetReadHook(void (*hook)(std::uint64_t, std::size_t, bool));
std::shared_ptr<Use> Begin(const ShaderRecompiler::RecompileRequest& request, const std::shared_ptr<const ShaderSnapshot>& shader, std::uint32_t queue, std::uint64_t arguments, bool fromAhead, bool resolving);
void AfterAdoption(const std::shared_ptr<Use>& use, bool adopted);
void Resolved(const std::shared_ptr<Use>& use, const ShaderRecompiler::ResourceCapture* capture, const ShaderRecompiler::RecompileResult& compiled, bool cached, bool validated, bool differing);
void SetPayload(const std::shared_ptr<Use>& use, const std::array<std::uint32_t, 3>& groups, std::uint64_t arguments, const std::array<std::uint32_t, 3>& threadGroup);
void Queued(const std::shared_ptr<Use>& use, bool readsClear, bool pipelined);
const CommitInput* ActiveCommit();
void CommitResult(bool compatible, bool supported, bool rebuilt, const char* reason);
void NoteVerificationNanoseconds(std::uint64_t nanoseconds);
void MarkRebuild();
void NoteWriterEffects(bool compatible, bool supported, std::uint32_t units);

class CommitScope {
public:
    explicit CommitScope(const std::shared_ptr<Use>& use);
    ~CommitScope();
    CommitScope(const CommitScope&) = delete;
    CommitScope& operator=(const CommitScope&) = delete;
private:
    Use* previous = nullptr;
};

}
#endif
