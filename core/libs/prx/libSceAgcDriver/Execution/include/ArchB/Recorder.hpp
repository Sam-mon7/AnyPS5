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
        Phase category = Phase::Prepare;
        std::uint64_t generation = 0;
        bool active = false;
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
    static void phase(const char* name, const char* stage, Clock::duration elapsed);
    inline static std::atomic<bool> enabled{false};
    inline static std::atomic<std::uint64_t> generation{0};
    inline static thread_local Thread* local = nullptr;
    inline static thread_local Scope* currentScope = nullptr;
    inline static thread_local Work* currentWork = nullptr;
};

}

extern "C" void AgcDriverRecorderScopeBegin_nid_no_patch(AgcDriver::ArchB::Recorder::Scope* scope, const char* name, std::int64_t now, std::uint8_t phase);
extern "C" void AgcDriverRecorderScopeMark_nid_no_patch(AgcDriver::ArchB::Recorder::Scope* scope, const char* stage, std::int64_t now);
extern "C" void AgcDriverRecorderScopeEnd_nid_no_patch(AgcDriver::ArchB::Recorder::Scope* scope, std::int64_t now);

#endif
