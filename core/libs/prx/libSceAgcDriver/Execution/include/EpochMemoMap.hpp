#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_EPOCHMEMOMAP_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_EPOCHMEMOMAP_HPP

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::GuestMemory {

template<class Entry>
class EpochMemoMap {
public:
    static constexpr std::size_t KeepCap = 4096;
    enum class SlotState : std::uint8_t { Inserted, Reused, Current };

    void Enter(std::uint64_t epoch, bool keep, std::size_t cap = KeepCap) {
        if (epoch == current) return;
        current = epoch;
        if (!keep) {
            entries.clear();
            pool.clear();
            return;
        }
        while (!entries.empty() && pool.size() < cap) pool.push_back(entries.extract(entries.begin()));
        entries.clear();
    }

    const Entry* Find(std::uintptr_t key) const {
        const auto found = entries.find(key);
        return found != entries.end() && found->second.epoch == current ? &found->second : nullptr;
    }

    Entry& Slot(std::uintptr_t key, SlotState& state) {
        if (const auto found = entries.find(key); found != entries.end()) {
            state = found->second.epoch == current ? SlotState::Current : SlotState::Reused;
            if (state == SlotState::Reused) found->second = Entry{};
            return found->second;
        }
        if (!pool.empty()) {
            auto node = std::move(pool.back());
            pool.pop_back();
            node.key() = key;
            node.mapped() = Entry{};
            state = SlotState::Reused;
            return entries.insert(std::move(node)).position->second;
        }
        state = SlotState::Inserted;
        return entries.try_emplace(key).first->second;
    }

    std::uint64_t Epoch() const { return current; }
    std::size_t Size() const { return entries.size(); }
    std::size_t Pooled() const { return pool.size(); }

private:
    using Map = std::unordered_map<std::uintptr_t, Entry>;
    std::uint64_t current = 0;
    Map entries;
    std::vector<typename Map::node_type> pool;
};

}

#endif
