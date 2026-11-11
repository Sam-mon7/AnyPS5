#pragma once

#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

extern "C" {
bool APS5_VABI CriticalPathEnabled_nid_no_patch() noexcept;
bool APS5_VABI CriticalPathCapturing_nid_no_patch() noexcept;
std::uint64_t APS5_VABI CriticalPathEmit_nid_no_patch(unsigned kind, const char* name, std::uint64_t object, std::uint64_t value, std::uint64_t peer) noexcept;
void APS5_VABI CriticalPathShutdown_nid_no_patch() noexcept;
}

namespace CriticalPath {
enum class Kind { ThreadName, PhaseBegin, PhaseEnd, PhaseMark, WaitBegin, WaitEnd, Release, Point, Present, Handoff, GpuSubmitBegin, GpuSubmitEnd, GpuObserved, LabelBatch };
inline bool Enabled() noexcept { return CriticalPathEnabled_nid_no_patch(); }
inline bool Capturing() noexcept { return CriticalPathCapturing_nid_no_patch(); }
inline std::uint64_t Emit(Kind kind, const char* name, std::uint64_t object = 0, std::uint64_t value = 0, std::uint64_t peer = 0) noexcept {
    return CriticalPathEmit_nid_no_patch(static_cast<unsigned>(kind), name, object, value, peer);
}
inline void Name(const char* name) noexcept { Emit(Kind::ThreadName, name); }
inline std::uint64_t Present(std::uint64_t frame = 0) noexcept { return Emit(Kind::Present, "present", 0, frame); }
inline std::uint64_t Object(const void* pointer) noexcept { return reinterpret_cast<std::uintptr_t>(pointer); }
inline void Shutdown() noexcept { CriticalPathShutdown_nid_no_patch(); }
class Wait {
public:
    explicit Wait(const char* name, std::uint64_t object = 0) noexcept : object(object), begin(Emit(Kind::WaitBegin, name, object)) {}
    Wait(const Wait&) = delete;
    Wait& operator=(const Wait&) = delete;
    void End(std::uint64_t peer = 0, const char* cause = nullptr) noexcept {
        if (begin) { Emit(Kind::WaitEnd, cause ? cause : "unknown", object, begin, peer); begin = 0; }
    }
    ~Wait() { End(); }
private:
    std::uint64_t object;
    std::uint64_t begin;
};
class Scope {
public:
    explicit Scope(const char* name, std::uint64_t object = 0) noexcept : object(object), begin(Emit(Kind::PhaseBegin, name, object)) {}
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void Mark(const char* name) noexcept { if (begin) Emit(Kind::PhaseMark, name, object, begin); }
    void End() noexcept { if (begin) { Emit(Kind::PhaseEnd, "end", object, begin); begin = 0; } }
    ~Scope() { End(); }
private:
    std::uint64_t object;
    std::uint64_t begin;
};
namespace Detail {
template<typename TValue, std::size_t TCapacity>
class Ring {
    static_assert(TCapacity > 0);
public:
    bool Push(const TValue& value, std::size_t reserved = 0) noexcept {
        const auto write = head.load(std::memory_order_relaxed);
        if (reserved >= TCapacity || write - tail.load(std::memory_order_acquire) >= TCapacity - reserved) return false;
        values[write % TCapacity] = value;
        head.store(write + 1, std::memory_order_release);
        return true;
    }
    bool Pop(TValue& value) noexcept {
        const auto read = tail.load(std::memory_order_relaxed);
        if (read == head.load(std::memory_order_acquire)) return false;
        value = values[read % TCapacity];
        tail.store(read + 1, std::memory_order_release);
        return true;
    }
private:
    std::array<TValue, TCapacity> values{};
    alignas(64) std::atomic<std::uint64_t> head{0};
    alignas(64) std::atomic<std::uint64_t> tail{0};
};
}
}
