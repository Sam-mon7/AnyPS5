#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

inline std::atomic<std::uint64_t>& LiveSequenceFrame() {
    static std::atomic<std::uint64_t> frame{0};
    return frame;
}

template <typename Entry>
class LiveSequence {
public:
    static constexpr std::size_t Frames = 2;
    static constexpr std::size_t MaxCandidates = 4;
    static constexpr std::uint64_t IdleFrames = 120;
    static constexpr std::uint64_t PrunePeriod = 256;

    struct Position {
        std::uint64_t key = 0;
        std::uint32_t ordinal = 0;
        bool valid = false;
    };

    struct Candidates {
        std::array<std::shared_ptr<Entry>, MaxCandidates> entries;
        std::size_t count = 0;

        bool Holds(const Entry* entry) const {
            for (std::size_t index = 0; index < count; ++index) if (entries[index].get() == entry) return true;
            return false;
        }
    };

    Position Next(std::uint64_t key, std::uint64_t frame) {
        if (frame != pruned_ && frame % PrunePeriod == 0) {
            pruned_ = frame;
            std::erase_if(slots_, [&](const auto& slot) { return frame - slot.second.frame > IdleFrames; });
        }
        auto& slot = slots_[key];
        if (slot.frame != frame || !slot.used) {
            if (slot.used) {
                for (std::size_t age = Frames - 1; age > 0; --age) slot.history[age] = std::move(slot.history[age - 1]);
                slot.history[0] = std::move(slot.current);
            }
            slot.current.clear();
            slot.next = 0;
            slot.frame = frame;
            slot.used = true;
        }
        return {key, slot.next++, true};
    }

    Candidates Predict(const Position& position) const {
        Candidates out;
        if (!position.valid) return out;
        const auto found = slots_.find(position.key);
        if (found == slots_.end()) return out;
        const auto& history = found->second.history;
        const auto add = [&](std::size_t age, std::int64_t ordinal) {
            if (ordinal < 0 || static_cast<std::size_t>(ordinal) >= history[age].size() || out.count == MaxCandidates) return;
            const auto& entry = history[age][static_cast<std::size_t>(ordinal)];
            if (entry != nullptr && !out.Holds(entry.get())) out.entries[out.count++] = entry;
        };
        const auto ordinal = static_cast<std::int64_t>(position.ordinal);
        for (std::size_t age = 0; age < Frames; ++age) add(age, ordinal);
        add(0, ordinal + 1);
        add(0, ordinal - 1);
        return out;
    }

    void Record(const Position& position, std::shared_ptr<Entry> entry) {
        if (!position.valid) return;
        const auto found = slots_.find(position.key);
        if (found == slots_.end()) return;
        auto& current = found->second.current;
        if (current.size() <= position.ordinal) current.resize(static_cast<std::size_t>(position.ordinal) + 1);
        current[position.ordinal] = std::move(entry);
    }

    void Clear() {
        slots_.clear();
    }

    std::size_t Keys() const {
        return slots_.size();
    }

private:
    struct Slot {
        std::uint64_t frame = 0;
        std::uint32_t next = 0;
        bool used = false;
        std::vector<std::shared_ptr<Entry>> current;
        std::array<std::vector<std::shared_ptr<Entry>>, Frames> history;
    };

    std::unordered_map<std::uint64_t, Slot> slots_;
    std::uint64_t pruned_ = 0;
};

}
