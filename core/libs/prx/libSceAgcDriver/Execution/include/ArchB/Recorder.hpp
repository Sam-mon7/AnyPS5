#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_RECORDER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ARCHB_RECORDER_HPP

#include <array>
#include <atomic>
#include <chrono>
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
    enum class DrawOutcome : std::uint8_t { None, Hit, Miss, Unclassified };
    class Draw {
    public:
        explicit Draw(DrawOutcome outcome = DrawOutcome::Unclassified);
        ~Draw();
        Draw(const Draw&) = delete;
        Draw& operator=(const Draw&) = delete;
        void Classify(DrawOutcome value);
    private:
        friend class Recorder;
        struct Pending {
            const char* name;
            const char* stage;
            Clock::duration elapsed;
            bool locked;
            bool counter;
            double value;
        };
        void Add(Pending value);
        Draw* previous = nullptr;
        DrawOutcome outcome;
        std::uint64_t generation = 0;
        std::vector<Pending> pending;
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
    private:
        Scope* parent = nullptr;
        Work* work = nullptr;
        const char* name = nullptr;
        const char* counter = nullptr;
        Clock::duration elapsed{};
        Clock::time_point previous{};
        Clock::duration children{};
        Clock::duration heldBefore{};
        Clock::duration heldChildren{};
        Phase category = Phase::Prepare;
        std::uint64_t generation = 0;
        bool active = false;
    };
    class Timer {
    public:
        explicit Timer(const char* name, Phase phase = Phase::Inherit) {
            Begin(name, phase);
        }
        void Begin(const char* name, Phase phase = Phase::Inherit) { if (name != nullptr && Enabled()) scope.Begin(name, Clock::now(), phase); }
        ~Timer() { Finish(); }
        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;
        void Mark(const char* stage) { if (scope.Active()) scope.Mark(stage, Clock::now()); }
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
    static bool RecordingDraw() { return Enabled() && currentDraw != nullptr && currentDraw->outcome != DrawOutcome::None; }
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
    static void phase(const char* name, const char* stage, Clock::duration elapsed, DrawOutcome outcome = DrawOutcome::None, bool locked = false);
    static void count(const char* name, double value, DrawOutcome outcome);
    static Clock::duration heldTime(Clock::time_point now);
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
extern "C" void AgcDriverRecorderVulkanBegin_nid_no_patch(AgcDriver::ArchB::Recorder::Scope* scope, const char* name);
extern "C" void AgcDriverRecorderCount_nid_no_patch(const char* name, double value);
extern "C" void AgcDriverRecorderReplayFrame_nid_no_patch(bool first);

#endif
