#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HOSTALLOCATOR_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HOSTALLOCATOR_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace AgcDriver::HostAllocator {

struct Counters {
    std::atomic<std::uint64_t> served{0};
    std::atomic<std::uint64_t> fallbackExhausted{0};
    std::atomic<std::uint64_t> releaseOwned{0};
    std::atomic<std::uint64_t> releaseForeign{0};
    std::atomic<std::uint64_t> remoteFrees{0};
};

bool Active() noexcept;
void SetActive(bool active) noexcept;
bool InsideScope() noexcept;
void SetInsideScope(bool inside) noexcept;
Counters& GetCounters() noexcept;
bool Owns(const void* block) noexcept;
std::size_t Capacity() noexcept;

class Scope {
public:
    explicit Scope(bool active) noexcept : previousActive(Active()), previousInside(InsideScope()), changedActive(previousActive != active), changedInside(!previousInside) {
        if (changedActive) SetActive(active);
        if (changedInside) SetInsideScope(true);
    }
    ~Scope() {
        if (changedInside) SetInsideScope(previousInside);
        if (changedActive) SetActive(previousActive);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    bool previousActive;
    bool previousInside;
    bool changedActive;
    bool changedInside;
};

}

#endif
