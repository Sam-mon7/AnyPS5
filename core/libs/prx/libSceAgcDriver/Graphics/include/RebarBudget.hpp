#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_REBARBUDGET_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_REBARBUDGET_HPP

#include <atomic>
#include <cstdint>

namespace AgcDriver::Graphics {

struct Context;

class RebarBudget {
public:
    explicit RebarBudget(std::uint64_t limit) : limit(limit) {}
    bool Reserve(std::uint64_t bytes) {
        auto held = live.load();
        const auto bound = limit.load();
        do {
            if (held > bound || bytes > bound - held) { ++overBudget; return false; }
        } while (!live.compare_exchange_weak(held, held + bytes));
        auto high = peak.load();
        while (high < held + bytes && !peak.compare_exchange_weak(high, held + bytes)) {}
        ++allocations;
        return true;
    }
    void Release(std::uint64_t bytes) { live.fetch_sub(bytes); }
    void Ineligible() { ++ineligible; }
    void Report();
    std::atomic<std::uint64_t> limit;
    std::atomic<std::uint64_t> live{0}, peak{0}, allocations{0}, ineligible{0}, overBudget{0};
};

RebarBudget& RebarMemory();
std::uint64_t RebarLimit(const Context& context);
bool RebarUploads();
bool RebarMirrors();

}

#endif
