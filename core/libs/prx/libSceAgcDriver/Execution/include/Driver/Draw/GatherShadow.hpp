#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_GATHERSHADOW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_GATHERSHADOW_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {
struct CompiledShader;
}

namespace AgcDriver::DriverDetail {

struct DrawProgram;
struct DrawEntry;

enum class GatherWord : std::uint8_t { Structure, Hidden, Callee, WalkedValue, TableBase, ValueWord, PushWord, FlatData, BufferBase, NestedPointer, Feedback, Count };

enum class GatherDiffer : std::uint8_t { None, Layout, UserStructure, UserHidden, UserCallee, UserWalked, TableBaseAlignment, RunStructure, NestedPointer, PointerUnreadable, Unreadable, Pending, Count };

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
};

struct GatherStagePlan {
    const ShaderRecompiler::RecompileResult* program = nullptr;
    std::vector<GatherDescriptorWord> words;
    std::vector<GatherVsharp> vsharps;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> slots;
    std::array<std::uint64_t, static_cast<std::size_t>(GatherElement::Count)> elements{};
    std::array<std::uint64_t, static_cast<std::size_t>(GatherElement::Count)> bytes{};
};

struct GatherDrawPlan {
    std::vector<GatherStagePlan> stages;
    bool complete = false;
};

struct GatherCheck {
    std::array<std::uint64_t, static_cast<std::size_t>(GatherElement::Count)> checked{};
    std::array<std::uint64_t, static_cast<std::size_t>(GatherElement::Count)> mismatched{};
    std::uint64_t vsharpAddress = 0;
    std::uint64_t vsharpSize = 0;
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

std::shared_ptr<const GatherTemplate> BuildGatherTemplate(const DispatchVariant& variant, std::span<const std::uint32_t> userData, std::uint32_t hiddenUserWords, const GatherOptions& options);
GatherOutcome EvaluateGather(const GatherTemplate& shape, std::span<const std::uint32_t> liveUser, const GatherReader& read, std::vector<std::uint32_t>& liveWords, std::vector<std::uint64_t>* liveAddresses, bool diagnose);
void FillGatherPlan(const GatherTemplate& shape, std::span<const std::uint32_t> liveUser, std::span<const std::uint32_t> liveWords, GatherStagePlan& plan);
void CheckGatherStage(const GatherStagePlan& plan, const ShaderRecompiler::RecompileResult& program, GatherCheck& check);
void CheckGatherDrawPlan(const GatherDrawPlan& plan, std::span<const Graphics::CompiledShader> stages);
void CountGatherPlanUnchecked(const char* reason);
const char* GatherWordName(GatherWord word);
const char* GatherDifferName(GatherDiffer differ);
const char* GatherElementName(GatherElement element);

std::vector<GatherValidation>* GatherValidationLog();
bool GatherRunsPending(const DispatchVariant& variant);
void ArmGatherValidationLog(bool armed);

struct GatherShadowDraw {
    std::span<const DrawProgram> programs;
    const DrawEntry* entry = nullptr;
    std::span<const std::shared_ptr<DispatchVariant>> matched;
    bool hit = false;
    bool keyed = false;
    GatherOptions options;
    std::uint64_t indirectArguments = 0;
    std::uint64_t indirectBytes = 0;
    std::uint64_t indirectCountAddress = 0;
};

std::shared_ptr<GatherDrawPlan> RunGatherShadow(const GatherShadowDraw& draw);

}

#endif
