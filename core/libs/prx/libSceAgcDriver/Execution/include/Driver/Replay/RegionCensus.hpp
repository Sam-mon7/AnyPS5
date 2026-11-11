#ifndef AGCDRIVER_DRIVER_REPLAY_REGIONCENSUS_HPP
#define AGCDRIVER_DRIVER_REPLAY_REGIONCENSUS_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Replay/RegionRecognizer.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace AgcDriver::Pm4 { struct DrawParameters; }

namespace AgcDriver::DriverDetail::Replay {

enum class ResolvedKind : std::uint8_t { Draw, Dispatch, Label, Copy, Fill, Wait, Other, Count };
enum class ResolvedField : std::uint8_t { State, Programs, Entry, Variants, Slot, Replay, Pipeline, Layout, Sets, Buffers, Images, Targets, Arguments, ResourceKey, Recipe, UserData, Push, Packet, Outcome, Count };
inline constexpr std::array<std::string_view, static_cast<std::size_t>(ResolvedField::Count)> ResolvedFieldNames{
    "state", "programs", "entry", "variants", "slot", "replay", "pipeline", "layout", "sets", "buffers", "images", "targets", "arguments", "resource_key", "recipe", "user_data", "push", "packet", "outcome"};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(ResolvedKind::Count)> ResolvedKindNames{"draw", "dispatch", "label", "copy", "fill", "wait", "other"};

struct ResolvedItem {
    using Fields = std::array<std::vector<std::uint64_t>, static_cast<std::size_t>(ResolvedField::Count)>;
    std::uint32_t offset = 0;
    ResolvedKind kind = ResolvedKind::Other;
    Fields prepared;
    Fields committed;
    bool operator==(const ResolvedItem&) const = default;
};

struct ResolvedDifference {
    bool equal = true;
    std::size_t item = 0;
    std::size_t field = 0;
    std::size_t word = 0;
    std::uint64_t before = 0;
    std::uint64_t after = 0;
};

std::uint64_t ResolvedSignature(std::span<const ResolvedItem> items);
ResolvedDifference CompareResolved(std::span<const ResolvedItem> before, std::span<const ResolvedItem> after);

namespace RegionCensus {

struct Region;
struct Packet;
bool Enabled();
bool Active();
class Observation {
public:
    Observation();
    ~Observation();
private:
    std::uint64_t start = 0;
};
std::shared_ptr<Region> Begin(std::uint32_t queue, std::span<const std::uint32_t> commands, const RegionReport& report, bool endsFrame);

class RegionScope {
public:
    explicit RegionScope(const std::shared_ptr<Region>& region);
    ~RegionScope();
private:
    std::shared_ptr<Region> region;
    std::shared_ptr<Region> previous;
    std::shared_ptr<Packet> previousPacket;
    bool active = false;
};

class PacketScope {
public:
    PacketScope(std::size_t offset, std::span<const std::uint32_t> words, std::uint64_t tableClock = 0);
    ~PacketScope();
private:
    std::shared_ptr<Packet> packet;
    std::shared_ptr<Packet> previous;
    std::uint64_t start = 0, locks = 0, observation = 0;
    bool previousMutexCensus = false;
};

std::shared_ptr<Packet> CapturePipe();
class CommitScope {
public:
    explicit CommitScope(std::shared_ptr<Packet> packet);
    ~CommitScope();
private:
    std::shared_ptr<Packet> packet;
    std::shared_ptr<Packet> previous;
    bool previousCommitting = false;
};

void Note(ResolvedField field, std::span<const std::uint64_t> values);
void SetKind(ResolvedKind kind);
inline void Note(ResolvedField field, std::initializer_list<std::uint64_t> values) { Note(field, std::span(values.begin(), values.size())); }
void NoteWords(ResolvedField field, std::span<const std::uint32_t> words);
void NoteBytes(ResolvedField field, std::span<const std::byte> bytes);
void UnobservedBindings();
void NoteArguments(std::span<const std::uint64_t> values);
void RegisterWrite(std::uint32_t opcode, std::uint32_t offset, std::uint32_t value);
void Registers(std::span<const std::uint32_t> words, std::span<const std::uint32_t> pairs);
void RegisterReadClock(std::uint64_t clock);
void UserData(std::uint32_t stage, std::uint32_t base, std::span<const std::uint32_t> words);
void Input(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t clock = 0);
void DrawArguments(const Pm4::DrawParameters& draw);
void CommitTime(double elapsed, std::uint64_t mutex, double gpu, bool failed);
void GpuLock();
void PipeLock(const std::shared_ptr<Packet>& packet = {});
std::uint64_t BeginWait();
void EndWait(std::uint64_t start);

template<typename TValue> std::uint64_t Identity(TValue value) {
    if constexpr (std::is_pointer_v<TValue>) return reinterpret_cast<std::uintptr_t>(value);
    else return static_cast<std::uint64_t>(value);
}

}

}

#endif
