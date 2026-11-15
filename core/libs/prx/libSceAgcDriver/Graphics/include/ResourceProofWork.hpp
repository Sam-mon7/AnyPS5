#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESOURCEPROOFWORK_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESOURCEPROOFWORK_HPP

#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace AgcDriver::Graphics::ResourceProofWork {

enum class Phase : std::size_t { Prelude, Surface, Tail, Verify, Count };
enum class Kind : std::size_t { StagingIn, ShadowPublish, TextureUpload, TextureCopy, DepthFlush, TextureClear, StoragePublish, BlockPublish, DepthStore, Count };
enum class Path : std::size_t { Full, Partial, Fast, Failed, FullRefused, Count };
struct Value { std::uint64_t operations, bytes; };
struct EventTotals { std::uint64_t operations = 0, bytes = 0, proofs = 0; };
struct Statistics {
    std::array<std::array<std::uint64_t, static_cast<std::size_t>(Kind::Count)>, static_cast<std::size_t>(Path::Count)> proofsWith{};
    std::array<std::uint64_t, static_cast<std::size_t>(Path::Count)> proofs{}, failed{};
    std::array<std::array<std::array<EventTotals, static_cast<std::size_t>(Kind::Count)>, static_cast<std::size_t>(Phase::Count)>, static_cast<std::size_t>(Path::Count)> events{};
};
struct AtomicEventTotals { std::atomic<std::uint64_t> operations{0}, bytes{0}, proofs{0}; };
struct Counters {
    std::array<std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Kind::Count)>, static_cast<std::size_t>(Path::Count)> proofsWith{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Path::Count)> proofs{}, failed{};
    std::array<std::array<std::array<AtomicEventTotals, static_cast<std::size_t>(Kind::Count)>, static_cast<std::size_t>(Phase::Count)>, static_cast<std::size_t>(Path::Count)> events{};
};
Counters& Totals();
Statistics Snapshot();
class Scope;
bool Enabled();
Scope* Current();
void SetCurrent(Scope* scope);
inline bool Active() { return Enabled() && Current() != nullptr; }

class Scope {
public:
    explicit Scope(bool enabled) : enabled(enabled) {
        if (!Enabled()) { this->enabled = false; return; }
        attached = true;
        previous = Current();
        if (enabled) values = {};
        SetCurrent(enabled ? this : nullptr);
    }
    ~Scope() { Finish(Path::Failed, false); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Phase SetPhase(Phase next) { const auto old = phase; phase = next; return old; }
    Value Read(Phase stage, Kind kind) const { return enabled ? values[static_cast<std::size_t>(stage)][static_cast<std::size_t>(kind)] : Value{}; }
    void Add(Kind kind, std::uint64_t bytes, std::uint64_t operations) {
        if (!enabled) return;
        auto& value = values[static_cast<std::size_t>(phase)][static_cast<std::size_t>(kind)];
        value.operations += operations;
        value.bytes += bytes;
    }
    void Finish(Path path, bool success) {
        if (finished) return;
        finished = true;
        if (!attached) return;
        SetCurrent(previous);
        if (!enabled) return;
        if (!success) path = path == Path::Full || path == Path::FullRefused ? Path::FullRefused : Path::Failed;
        const auto index = static_cast<std::size_t>(path);
        const auto& labels = names();
        auto& totals = Totals();
        totals.proofs[index].fetch_add(1, std::memory_order_relaxed);
        if (!success) totals.failed[index].fetch_add(1, std::memory_order_relaxed);
        ArchB::Recorder::Count(labels.proofs[index][0].data());
        if (!success) ArchB::Recorder::Count(labels.proofs[index][1].data());
        std::array<bool, Kinds> seen{};
        for (std::size_t stage = 0; stage < values.size(); ++stage) {
            for (std::size_t kind = 0; kind < values[stage].size(); ++kind) {
                const auto& value = values[stage][kind];
                if (value.operations == 0) continue;
                if (stage != static_cast<std::size_t>(Phase::Verify)) seen[kind] = true;
                auto& total = totals.events[index][stage][kind];
                total.operations.fetch_add(value.operations, std::memory_order_relaxed);
                total.bytes.fetch_add(value.bytes, std::memory_order_relaxed);
                total.proofs.fetch_add(1, std::memory_order_relaxed);
                const auto& entry = labels.events[index][stage][kind];
                ArchB::Recorder::Count(entry[0].data(), static_cast<double>(value.operations));
                ArchB::Recorder::Count(entry[1].data(), static_cast<double>(value.bytes));
            }
        }
        for (std::size_t kind = 0; kind < seen.size(); ++kind) {
            if (!seen[kind]) continue;
            totals.proofsWith[index][kind].fetch_add(1, std::memory_order_relaxed);
            ArchB::Recorder::Count(labels.withWork[index][kind].data());
        }
    }
private:
    using Label = std::array<char, 112>;
    static constexpr std::size_t Paths = static_cast<std::size_t>(Path::Count);
    static constexpr std::size_t Phases = static_cast<std::size_t>(Phase::Count);
    static constexpr std::size_t Kinds = static_cast<std::size_t>(Kind::Count);
    struct Names {
        std::array<std::array<Label, 2>, Paths> proofs{};
        std::array<std::array<Label, Kinds>, Paths> withWork{};
        std::array<std::array<std::array<std::array<Label, 2>, Kinds>, Phases>, Paths> events{};
    };
    static const Names& names() {
        static const Names result = [] {
            Names value;
            constexpr const char* paths[] = {"full", "partial", "fast", "failed", "full_refused"};
            constexpr const char* stages[] = {"prelude", "surface", "tail", "verify"};
            constexpr const char* kinds[] = {"staging_in", "shadow_publish", "texture_upload", "texture_copy", "depth_flush", "texture_clear", "storage_publish", "block_publish", "depth_store"};
            constexpr const char* measures[] = {"operations", "bytes"};
            for (std::size_t path = 0; path < Paths; ++path) {
                std::snprintf(value.proofs[path][0].data(), value.proofs[path][0].size(), "dispatch_reproof_work_%s_proofs", paths[path]);
                std::snprintf(value.proofs[path][1].data(), value.proofs[path][1].size(), "dispatch_reproof_work_%s_failed", paths[path]);
                for (std::size_t kind = 0; kind < Kinds; ++kind) {
                    auto& label = value.withWork[path][kind];
                    std::snprintf(label.data(), label.size(), "dispatch_reproof_work_%s_any_%s_proofs", paths[path], kinds[kind]);
                }
                for (std::size_t stage = 0; stage < Phases; ++stage) {
                    for (std::size_t kind = 0; kind < Kinds; ++kind) {
                        for (std::size_t measure = 0; measure < 2; ++measure) {
                            auto& label = value.events[path][stage][kind][measure];
                            std::snprintf(label.data(), label.size(), "dispatch_reproof_work_%s_%s_%s_%s", paths[path], stages[stage], kinds[kind], measures[measure]);
                        }
                    }
                }
            }
            return value;
        }();
        return result;
    }
    bool enabled;
    bool finished = false;
    bool attached = false;
    Scope* previous = nullptr;
    Phase phase = Phase::Prelude;
    std::array<std::array<Value, Kinds>, Phases> values;
};

inline void Note(Kind kind, std::uint64_t bytes, std::uint64_t operations = 1) {
    if (!Enabled()) return;
    if (auto* scope = Current(); scope != nullptr) scope->Add(kind, bytes, operations);
}

}

#endif
