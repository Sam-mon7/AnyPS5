#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_RECORDER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_RECORDER_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver::ArchB {

class Recorder {
public:
    using Clock = std::chrono::steady_clock;
    enum class Kind : std::uint8_t { Draw, Dispatch };
    enum class Phase : std::uint8_t { Prepare, Record, Proof, Bind, Inherit };
    enum class DrawOutcome : std::uint8_t { Unclassified, Hit, Miss };
    enum class Bucket : std::uint8_t { Lookup, Wait, Targets, Descriptors, Pipeline, Dynamic, Push, Draw, Barriers, Refresh, Bookkeeping, Count };
    enum class Metric : std::uint8_t {
        Calls, Continuations, VulkanCalls, VkcmdCalls, DescriptorWrites, DescriptorElements, ImageElements, BufferElements,
        DescriptorSets, DescriptorBinds, PipelineBinds, DistinctPipelines, DynamicCalls, PushCalls, DrawCalls,
        PassBegins, PassEnds, PassContinued, Barriers, ImageBarriers, LayoutTransitions, SnapshotBytes,
        BindingPreparations, BindingRewrites, PlainStages, OrderedAccess, DescriptorSetRequests, SnapshotCopiedBytes, RedundantPipelineBinds, RedundantDynamicCalls, RedundantPushCalls,
        TargetLookup, TargetView, TargetDepthLookup, TargetDepthCreated, TargetRefresh, TargetRefreshProved, TargetCollect, TargetAliasCheck, TargetFlush, TargetKeys, TargetValidate, TargetUpload,
        TargetClearCheck, TargetDepthClear, TargetDepthSync, TargetDepthBookkeeping, PassHazardCheck, PassCompatibility, RenderingInfo, FramebufferLookup, TargetCreated, TargetShadowCheck, TargetClears, TargetLayoutTransitions,
        TargetLookupNs, TargetRefreshNs, TargetWaitNs, TargetProofNs, TargetCollectNs, TargetAliasNs, TargetFlushNs, TargetKeysNs, TargetValidateNs, TargetUploadNs, TargetClearNs,
        TargetDepthLookupNs, TargetDepthPlanesNs, TargetDepthSyncNs, TargetDepthClearNs, TargetDepthBookkeepingNs, PassAccessNs, PassHazardNs, PassSetupNs, PassBeginNs, PipelineStateNs, Count
    };
    enum class State : std::uint8_t { Pipeline, Descriptors, Targets, Dynamic, Push, Count };
    struct Sequence {
        std::array<std::uint64_t, 512> words;
        std::size_t size = 0;
        bool overflow = false;
        void Add(std::uint64_t value) {
            if (size < words.size()) words[size++] = value;
            else overflow = true;
        }
        bool Same(const Sequence& other) const {
            return !overflow && !other.overflow && size != 0 && size == other.size && std::memcmp(words.data(), other.words.data(), size * sizeof(words[0])) == 0;
        }
    };
    class Draw {
    public:
        explicit Draw(DrawOutcome outcome = DrawOutcome::Unclassified, bool call = true);
        ~Draw();
        Draw(const Draw&) = delete;
        Draw& operator=(const Draw&) = delete;
        void Classify(DrawOutcome value) { outcome = value; }
        void Count(Metric metric, std::uint64_t value = 1) { counters[static_cast<std::size_t>(metric)] += value; }
        void Word(State state, std::uint64_t value) { states[static_cast<std::size_t>(state)].Add(value); }
        void Bytes(State state, const void* data, std::size_t size) {
            Word(state, size);
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (std::size_t offset = 0; offset < size; offset += sizeof(std::uint64_t)) {
                std::uint64_t word = 0;
                std::memcpy(&word, bytes + offset, std::min(sizeof(word), size - offset));
                Word(state, word);
            }
        }
        std::size_t StateSize(State state) const { return states[static_cast<std::size_t>(state)].size; }
        bool SameOperation(State state, std::size_t begin) const {
            if (outcome != DrawOutcome::Hit || priorStates == nullptr || multipleCommands || commands == 0 || commands != priorCommands || batch != priorBatch) return false;
            const auto& value = states[static_cast<std::size_t>(state)];
            const auto& prior = (*priorStates)[static_cast<std::size_t>(state)];
            return !value.overflow && !prior.overflow && begin < value.size && value.size <= prior.size && std::memcmp(value.words.data() + begin, prior.words.data() + begin, (value.size - begin) * sizeof(std::uint64_t)) == 0;
        }
        void Unknown(State state) { states[static_cast<std::size_t>(state)].overflow = true; }
        void Commands(std::uint64_t value, std::uint64_t epoch) {
            if (commands != 0 && (commands != value || batch != epoch)) multipleCommands = true;
            commands = value;
            batch = epoch;
        }
    private:
        friend class Recorder;
        friend class Scope;
        Draw* previous = nullptr;
        DrawOutcome outcome;
        std::uint64_t generation = 0;
        std::array<std::array<Clock::duration, static_cast<std::size_t>(Bucket::Count)>, 2> elapsed{};
        std::array<Clock::duration, static_cast<std::size_t>(Bucket::Count)> emission{};
        std::array<Clock::duration, static_cast<std::size_t>(Bucket::Count)> redundantEmission{};
        std::array<std::uint64_t, static_cast<std::size_t>(Metric::Count)> counters{};
        std::array<Sequence, static_cast<std::size_t>(State::Count)> states;
        const std::array<Sequence, static_cast<std::size_t>(State::Count)>* priorStates = nullptr;
        std::uint64_t priorCommands = 0;
        std::uint64_t priorBatch = 0;
        std::uint64_t commands = 0;
        std::uint64_t batch = 0;
        bool multipleCommands = false;
        bool active = false;
    };
    struct Key {
        Kind kind = Kind::Draw;
        std::uint64_t code = 0;
        bool operator==(const Key&) const = default;
    };
    struct Shader {
        Key key;
        std::uint64_t calls = 0;
        std::array<double, 4> ms{};
    };
    struct Snapshot {
        std::uint64_t frames = 0;
        std::uint64_t presented = 0;
        std::int64_t startNs = 0;
        std::int64_t endNs = 0;
        std::uint64_t sequence = 0;
        bool enabled = false;
        bool toggled = false;
        std::map<std::string, std::map<std::string, double>> phases;
        std::map<std::string, std::map<std::string, double>> threadCounters;
        std::map<std::string, double> counters;
        std::map<std::string, std::vector<Shader>> threadShaders;
        std::vector<Shader> shaders;
        Shader other;
        Shader total;
        double accountedMs = 0;
        double criticalPathMs = 0;
        std::string Json() const;
        std::string Line() const;
    };
    class Work;
    class Scope {
    public:
        void Begin(const char* name, Clock::time_point now, Phase phase = Phase::Inherit);
        void Mark(const char* stage, Clock::time_point now);
        void End(Clock::time_point now);
        void Counter(const char* value) { counter = value; }
        void SetPhase(Phase phase) { category = phase; }
        bool Active() const { return active; }
        void Repeated(bool value) { repeated = value; }
        void Measure(Metric value) { metric = value; }
    private:
        friend class Recorder;
        Scope* parent = nullptr;
        Work* work = nullptr;
        const char* name = nullptr;
        const char* counter = nullptr;
        Clock::duration elapsed{};
        Clock::time_point previous{};
        Clock::duration children{};
        Clock::duration heldBefore{};
        Clock::duration heldChildren{};
        bool repeated = false;
        Bucket bucket = Bucket::Bookkeeping;
        Metric metric = Metric::Count;
        Draw* draw = nullptr;
        Phase category = Phase::Prepare;
        std::uint64_t generation = 0;
        bool active = false;
    };
    class Timer {
    public:
        explicit Timer(const char* name, Phase phase = Phase::Inherit) { Begin(name, phase); }
        Timer(const char* name, Metric metric) { scope.Measure(metric); Begin(name); }
        ~Timer() { Finish(); }
        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;
        void Begin(const char* name, Phase phase = Phase::Inherit) { if (name != nullptr && CurrentDraw() != nullptr) scope.Begin(name, Clock::now(), phase); }
        void Mark(const char* stage) { if (scope.Active()) scope.Mark(stage, Clock::now()); }
        void Mark(const char* stage, Metric metric) {
            scope.Measure(metric);
            Mark(stage);
            scope.Measure(Metric::Count);
        }
        void Finish() { if (scope.Active()) scope.End(Clock::now()); }
    private:
        Scope scope;
    };
    class Work {
    public:
        explicit Work(Key key = {Kind::Draw, 0}, Phase phase = Phase::Prepare, bool call = true);
        ~Work();
        Work(const Work&) = delete;
        Work& operator=(const Work&) = delete;
        void SetKey(Key value) { key = value; }
        Key Identity() const { return key; }
    private:
        friend class Scope;
        Work* previous;
        Key key;
        Phase phase;
        bool call;
        std::uint64_t generation;
        std::array<Clock::duration, 4> elapsed{};
    };
    static std::uint64_t Generation() { return generation.load(std::memory_order_relaxed); }
    static bool Enabled() { return enabled.load(std::memory_order_relaxed); }
    static Draw* CurrentDraw() { return currentDraw; }
    static Draw* TargetDraw() { return currentDraw != nullptr && currentScope != nullptr && currentScope->bucket == Bucket::Targets ? currentDraw : nullptr; }
    static void GpuLock(bool held, Clock::time_point now);
    static void BindThread(const char* name);
    static void Boundary(bool on, std::uint64_t sequence, Clock::time_point now);
    static Snapshot Collect(Clock::time_point now);
    static void Count(const char* name, double value = 1);
    static void RegisterCounter(const char* name, const std::atomic<std::uint64_t>& counter);
    static std::uint64_t Digest(std::span<const std::uint32_t> code);
    static void AddShader(Key key, std::uint64_t calls, const std::array<Clock::duration, 4>& elapsed);

private:
    struct Thread;
    static Thread& thread();
    static Clock::duration heldTime(Clock::time_point now);
    static void phase(const char* name, const char* stage, Clock::duration elapsed);
    inline static std::atomic<bool> enabled{false};
    inline static std::atomic<std::uint64_t> generation{0};
    inline static thread_local Thread* local = nullptr;
    inline static thread_local Scope* currentScope = nullptr;
    inline static thread_local Work* currentWork = nullptr;
    inline static thread_local Draw* currentDraw = nullptr;
    inline static thread_local bool gpuHeld = false;
    inline static thread_local Clock::time_point gpuSince{};
    inline static thread_local Clock::duration gpuElapsed{};
};

}

extern "C" void AgcDriverRecorderScopeBegin_nid_no_patch(AgcDriver::ArchB::Recorder::Scope* scope, const char* name, std::int64_t now, std::uint8_t phase);
extern "C" void AgcDriverRecorderScopeMark_nid_no_patch(AgcDriver::ArchB::Recorder::Scope* scope, const char* stage, std::int64_t now);
extern "C" void AgcDriverRecorderScopeEnd_nid_no_patch(AgcDriver::ArchB::Recorder::Scope* scope, std::int64_t now);
extern "C" AgcDriver::ArchB::Recorder::Draw* AgcDriverRecorderDraw_nid_no_patch();
extern "C" bool AgcDriverRecorderTargets_nid_no_patch();
extern "C" void AgcDriverRecorderVulkanBegin_nid_no_patch(AgcDriver::ArchB::Recorder::Scope* scope, const char* name);

#endif
