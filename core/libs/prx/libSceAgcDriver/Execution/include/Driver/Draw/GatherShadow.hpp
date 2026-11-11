#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_GATHERSHADOW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_GATHERSHADOW_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {
struct CompiledShader;
struct State;
}

namespace AgcDriver::DriverDetail {

struct DrawProgram;
struct DrawEntry;

enum class GatherWord : std::uint8_t { Structure, Hidden, Callee, WalkedValue, TableBase, ValueWord, PushWord, FlatData, BufferBase, NestedPointer, Feedback, ValueRecord, Count };

enum class GatherDiffer : std::uint8_t { None, Layout, UserStructure, UserHidden, UserCallee, UserWalked, TableBaseAlignment, RunStructure, NestedPointer, PointerUnreadable, Unreadable, Pending, RecordCount, Count };

enum class GatherElement : std::uint8_t { Vsharp, Flat, ShaderData, Push, Indirect, Count };

struct GatherOptions {
    bool dataWords = false;
    bool bufferBases = false;
    bool feedback = false;
    bool nested = false;
    bool operator==(const GatherOptions&) const = default;
};

struct GatherRun {
    std::uint64_t address = 0;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    std::int32_t base = -1;
    std::uint32_t parentRun = DispatchVariant::NoFlatBinding;
    std::uint32_t parentWord = 0;
};

struct GatherTarget {
    bool user = false;
    bool slot = false;
    std::uint32_t source = 0;
    std::uint32_t binding = 0;
    std::uint32_t word = 0;
    std::uint32_t mask = 0xffffffffu;
};

struct GatherTemplate {
    std::vector<std::uint32_t> user;
    std::vector<std::uint32_t> userMask;
    std::vector<GatherWord> userClass;
    std::vector<GatherRun> runs;
    std::vector<std::uint32_t> words;
    std::vector<std::uint32_t> wordMask;
    std::vector<GatherWord> wordClass;
    std::vector<GatherTarget> targets;
    std::uint32_t pushOffset = 0;
    std::uint64_t signature = 0;
    bool nested = false;
    const char* refusal = nullptr;
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiled;
};

using GatherReader = std::function<bool(std::uint64_t address, std::span<std::uint32_t> out)>;

struct GatherOutcome {
    bool matched = false;
    bool wordsChanged = false;
    bool feedbackChanged = false;
    GatherDiffer differ = GatherDiffer::None;
    std::uint64_t signature = 0;
    std::uint32_t nestedRuns = 0;
    std::uint32_t differIndex = 0;
    std::uint32_t differOld = 0;
    std::uint32_t differNew = 0;
    std::uint32_t differMask = 0;
};

struct GatherDescriptorWord {
    std::uint32_t binding = 0;
    std::uint32_t word = 0;
    std::uint32_t value = 0;
    GatherWord source = GatherWord::Structure;
};

struct GatherVsharp {
    std::uint32_t binding = 0;
    std::uint32_t element = 0;
    std::array<std::uint32_t, 4> words{};
    std::uint64_t address = 0;
    std::uint64_t size = 0;
    std::uint64_t extent = 0;
    std::uint32_t adjustment = 0;
    std::uint64_t viewBytes = 0;
};

struct GatherIndirectPlan {
    bool present = false;
    bool countIndirect = false;
    std::uint64_t arguments = 0;
    std::uint64_t countAddress = 0;
    std::uint32_t count = 0;
    std::uint32_t recordBytes = 0;
    std::uint64_t rangeBytes = 0;
    bool operator==(const GatherIndirectPlan&) const = default;
};

struct GatherWordBuffer {
    std::uint32_t binding = 0;
    std::vector<std::uint32_t> words;
};

struct GatherVsharpBinding {
    std::uint32_t binding = 0;
    std::uint32_t element = 0;
    std::size_t allocation = 0;
    std::uint64_t extent = 0;
    std::uint32_t adjustment = 0;
    std::uint64_t viewBytes = 0;
};

struct GatherCommitStage {
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiled;
    std::shared_ptr<const void> variant;
    ShaderRecompiler::ShaderStage stage{};
    std::uint32_t pushOffset = 0;
    std::shared_ptr<const std::vector<ShaderRecompiler::RegisteredImageView>> registeredViews;
    std::vector<GatherVsharpBinding> bindings;
};

struct GatherCommitTemplate {
    std::shared_ptr<const Graphics::State> state;
    std::vector<GatherCommitStage> stages;
    std::atomic<bool> ready{false};
    std::atomic<bool> valid{true};
};

struct GatherKnownRead {
    std::uint64_t address = 0;
    std::vector<std::uint32_t> words;
};

struct GatherStagePlan {
    const ShaderRecompiler::RecompileResult* program = nullptr;
    std::vector<GatherDescriptorWord> words;
    std::vector<GatherVsharp> vsharps;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> slots;
    std::vector<GatherWordBuffer> buffers;
    std::vector<std::byte> push;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
    std::vector<GatherKnownRead> knownReads;
    bool ownerKnown = false;
    std::array<std::uint64_t, static_cast<std::size_t>(GatherElement::Count)> elements{};
    std::array<std::uint64_t, static_cast<std::size_t>(GatherElement::Count)> bytes{};
};

enum GatherIncomplete : std::uint32_t { GatherIncompleteNone = 0, GatherPendingWords = 1, GatherPendingVsharp = 2, GatherPendingIndirect = 4, GatherGenerationMoved = 8, GatherNotTemplatable = 16, GatherUnmatched = 32, GatherUnknownBindings = 64, GatherBindingShape = 128 };

struct GatherBufferLimits {
    std::uint64_t maxRange = 0;
    std::uint32_t alignment = 4;
    std::uint64_t targetAddress = 0;
    std::uint64_t (*readableEnd)(std::uint64_t) = nullptr;
    bool wholeDwords = false;
};

struct GatherDrawPlan {
    std::vector<GatherStagePlan> stages;
    GatherIndirectPlan indirect;
    GatherBufferLimits limits;
    std::uint32_t incomplete = GatherIncompleteNone;
    bool complete = false;
    bool matched = false;
    GatherDiffer differ = GatherDiffer::None;
    std::uint32_t differStage = 0;
    std::uint32_t differIndex = 0;
    std::uint32_t differOld = 0;
    std::uint32_t differNew = 0;
    std::uint32_t differMask = 0;
    std::shared_ptr<GatherCommitTemplate> commitTemplate;
};

struct GatherCheck {
    std::array<std::uint64_t, static_cast<std::size_t>(GatherElement::Count)> checked{};
    std::array<std::uint64_t, static_cast<std::size_t>(GatherElement::Count)> mismatched{};
    std::uint64_t vsharpAddress = 0;
    std::uint64_t vsharpSize = 0;
    std::uint64_t vsharpExtent = 0;
    std::uint64_t stagesMissing = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(GatherWord::Count)> mismatchedFrom{};
};

struct GatherValidation {
    std::uint32_t stage = 0;
    std::shared_ptr<DispatchVariant> variant;
    EntryOutcome outcome = EntryOutcome::Differing;
    bool refused = false;
    bool pending = false;
};

constexpr std::uint64_t GatherSmallElementBytes = 4096;

struct GatherExtent {
    std::uint64_t bytes = 0;
    std::uint32_t adjustment = 0;
};

GatherExtent GatherBufferExtent(const std::array<std::uint32_t, 4>& words, const GatherBufferLimits& limits);
std::shared_ptr<const GatherTemplate> BuildGatherTemplate(const DispatchVariant& variant, std::span<const std::uint32_t> userData, std::uint32_t hiddenUserWords, const GatherOptions& options);
GatherOutcome EvaluateGather(const GatherTemplate& shape, std::span<const std::uint32_t> liveUser, const GatherReader& read, std::vector<std::uint32_t>& liveWords, std::vector<std::uint64_t>* liveAddresses, bool diagnose);
void FillGatherPlan(const GatherTemplate& shape, std::span<const std::uint32_t> liveUser, std::span<const std::uint32_t> liveWords, GatherStagePlan& plan, const GatherBufferLimits& limits = {}, const GatherCommitStage* binding = nullptr);
void CheckGatherStage(const GatherStagePlan& plan, const ShaderRecompiler::RecompileResult& program, GatherCheck& check, const GatherBufferLimits& limits = {});
GatherCheck CheckGatherDrawPlan(const GatherDrawPlan& plan, std::span<const Graphics::CompiledShader> stages, std::uint64_t targetAddress, const GatherIndirectPlan& committed);
void CountGatherPlanUnchecked(const char* reason);
const char* GatherWordName(GatherWord word);
const char* GatherDifferName(GatherDiffer differ);
const char* GatherElementName(GatherElement element);

std::vector<GatherValidation>* GatherValidationLog();
bool GatherRunsPending(const DispatchVariant& variant);
void ArmGatherValidationLog(bool armed);

struct GatherShadowStats {
    std::uint64_t built = 0;
    std::uint64_t trueMatch = 0;
    std::uint64_t trueDiffer = 0;
    std::uint64_t falseMatch = 0;
    std::uint64_t nestedDirect = 0;
    std::uint64_t falseDiffer = 0;
    std::uint64_t pendingRefused = 0;
    std::uint64_t generationRefused = 0;
    std::uint64_t reads = 0;
};

struct GatherShadowDraw {
    std::span<const DrawProgram> programs;
    const DrawEntry* entry = nullptr;
    std::span<const std::shared_ptr<DispatchVariant>> matched;
    bool hit = false;
    bool keyed = false;
    GatherOptions options;
    GatherIndirectPlan indirect;
    GatherBufferLimits limits;
    const GatherReader* read = nullptr;
    const std::function<bool(std::uint64_t, std::size_t)>* busy = nullptr;
    GatherShadowStats* stats = nullptr;
    bool live = false;
    std::shared_ptr<GatherCommitTemplate> commitTemplate;
};

std::shared_ptr<GatherDrawPlan> RunGatherShadow(const GatherShadowDraw& draw);
std::vector<std::shared_ptr<const ShaderRecompiler::RecompileResult>> MaterializeGather(const GatherDrawPlan& plan);

}

#endif
