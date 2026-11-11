#ifndef ANYPS5_LIBSCEAGCDRIVER_DRIVER_DRAW_POINTERSET_HPP
#define ANYPS5_LIBSCEAGCDRIVER_DRIVER_DRAW_POINTERSET_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace AgcDriver::DriverDetail {

class PointerSet {
public:
    void clear() {
        if (slots.empty()) slots.assign(1024, nullptr);
        else std::fill(slots.begin(), slots.end(), nullptr);
        used = 0;
    }
    bool insert(const void* value) {
        if ((used + 1) * 2 > slots.size()) grow();
        return place(value);
    }
    bool contains(const void* value) const {
        for (std::size_t slot = hash(value);; slot = (slot + 1) & (slots.size() - 1)) {
            if (slots[slot] == value) return true;
            if (slots[slot] == nullptr) return false;
        }
    }
private:
    std::size_t hash(const void* value) const { return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(value) >> 4u) * 0x9e3779b97f4a7c15ull >> 20u) & (slots.size() - 1); }
    bool place(const void* value) {
        for (std::size_t slot = hash(value);; slot = (slot + 1) & (slots.size() - 1)) {
            if (slots[slot] == value) return false;
            if (slots[slot] != nullptr) continue;
            slots[slot] = value;
            ++used;
            return true;
        }
    }
    void grow() {
        std::vector<const void*> old(slots.size() * 2, nullptr);
        old.swap(slots);
        used = 0;
        for (const auto* value : old) if (value != nullptr) place(value);
    }
    std::vector<const void*> slots;
    std::size_t used = 0;
};

}

#endif
