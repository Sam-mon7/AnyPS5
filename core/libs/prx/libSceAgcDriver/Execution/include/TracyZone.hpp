#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_TRACYZONE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_TRACYZONE_HPP

#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include <cstdint>
#ifdef TRACY_ENABLE
#include <tracy/TracyC.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <stdexcept>
#endif

namespace AgcDriver {

class TracyZone {
public:
    explicit TracyZone([[maybe_unused]] const char* name) {
#ifdef TRACY_ENABLE
        if (ProfilingEnabled()) context = ___tracy_emit_zone_begin(location(name), 1);
#endif
    }

    TracyZone(const TracyZone&) = delete;
    TracyZone& operator=(const TracyZone&) = delete;

    ~TracyZone() {
        End();
    }

    void End() {
#ifdef TRACY_ENABLE
        if (context.active == 0) return;
        ___tracy_emit_zone_end(context);
        context.active = 0;
#endif
    }

    void Value([[maybe_unused]] std::uint64_t value) {
#ifdef TRACY_ENABLE
        if (context.active != 0) ___tracy_emit_zone_value(context, value);
#endif
    }

    void Name([[maybe_unused]] const char* name) {
#ifdef TRACY_ENABLE
        if (context.active != 0) ___tracy_emit_zone_name(context, name, std::strlen(name));
#endif
    }

    void Restart([[maybe_unused]] const char* name) {
        End();
#ifdef TRACY_ENABLE
        if (ProfilingEnabled()) context = ___tracy_emit_zone_begin(location(name), 1);
#endif
    }

    static bool Available() {
#ifdef TRACY_ENABLE
        return true;
#else
        return false;
#endif
    }

    static bool Connected() {
#ifdef TRACY_ENABLE
        return ___tracy_connected() != 0;
#else
        return false;
#endif
    }

    static void MarkFrame([[maybe_unused]] const char* name = nullptr) {
#ifdef TRACY_ENABLE
        if (ProfilingEnabled()) ___tracy_emit_frame_mark(name);
#endif
    }

    static void BeginFrame([[maybe_unused]] const char* name) {
#ifdef TRACY_ENABLE
        if (ProfilingEnabled()) ___tracy_emit_frame_mark_start(name);
#endif
    }

    static void EndFrame([[maybe_unused]] const char* name) {
#ifdef TRACY_ENABLE
        if (ProfilingEnabled()) ___tracy_emit_frame_mark_end(name);
#endif
    }

    static void NameThread([[maybe_unused]] const char* name) {
#ifdef TRACY_ENABLE
        ___tracy_set_thread_name(name);
#endif
    }

private:
#ifdef TRACY_ENABLE
    static const ___tracy_source_location_data* location(const char* name) {
        static constexpr std::size_t capacity = 256;
        static std::array<std::atomic<const char*>, capacity> names{};
        static std::array<___tracy_source_location_data, capacity> locations{};
        static std::mutex mutex;
        const auto start = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(name) >> 4);
        for (std::size_t probe = 0; probe < capacity; ++probe) {
            const auto index = (start + probe) % capacity;
            const auto* known = names[index].load(std::memory_order_acquire);
            if (known == name) return &locations[index];
            if (known != nullptr) continue;
            std::lock_guard lock(mutex);
            known = names[index].load(std::memory_order_relaxed);
            if (known == name) return &locations[index];
            if (known != nullptr) continue;
            locations[index] = {name, name, "", 0, 0};
            names[index].store(name, std::memory_order_release);
            return &locations[index];
        }
        throw std::runtime_error("Tracy: too many zone names");
    }

    TracyCZoneCtx context{};
#endif
};

}

#endif
