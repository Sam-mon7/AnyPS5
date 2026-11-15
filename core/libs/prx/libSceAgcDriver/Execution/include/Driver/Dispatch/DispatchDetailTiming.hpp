#pragma once

#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <optional>

namespace AgcDriver {

class DispatchDetailTiming {
public:
    explicit DispatchDetailTiming(const char* name) : on(ArchB::Switches::Enabled(ArchB::Feature::DispatchTiming)) { Next(name); }
    void Next(const char* name) { if (on) timer.emplace(name); }
    void End() { timer.reset(); }
    bool Enabled() const { return on; }
    bool Metadata() const { return on && CriticalPath::Capturing(); }
    void Point(const char* name, std::uint64_t object = 0, std::uint64_t value = 0) const {
        if (Metadata()) CriticalPath::Emit(CriticalPath::Kind::Point, name, object, value);
    }

private:
    bool on;
    std::optional<PerformanceTimer> timer;
};

}
