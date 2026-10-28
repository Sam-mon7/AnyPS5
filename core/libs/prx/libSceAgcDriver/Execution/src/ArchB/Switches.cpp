#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace AgcDriver::ArchB {

namespace {

#ifdef APS5_ARCHB_SWITCHES_TEST
constexpr Feature TestPrerequisites[] = {Feature::GateRun};
#endif
constexpr std::array<SwitchInfo, static_cast<std::size_t>(Feature::Count)> Entries{{
    {Feature::GateRun, "GATE_RUN", false, {}},
#ifdef APS5_ARCHB_SWITCHES_TEST
    {Feature::TestDependent, "TEST_DEPENDENT", true, TestPrerequisites},
#endif
}};
std::array<bool, Entries.size()> values{};
std::array<bool, Entries.size()> fromEnvironment{};
std::once_flag initialized;

std::string describe() {
    std::string text = "[archb] switches";
    for (const auto& entry : Entries) {
        const auto index = static_cast<std::size_t>(entry.feature);
        text += " " + std::string(entry.name) + "=" + (values[index] ? "1" : "0") + (fromEnvironment[index] ? "(env)" : "(default)");
    }
    return text;
}

}

void Switches::Initialize() {
    std::call_once(initialized, [] {
        for (const auto& entry : Entries) {
            const auto index = static_cast<std::size_t>(entry.feature);
            const auto variable = "APS5_ARCHB_" + std::string(entry.name);
            const char* value = std::getenv(variable.c_str());
            fromEnvironment[index] = value != nullptr;
            values[index] = entry.defaultOn;
            if (value == nullptr) continue;
            if (std::string_view(value) != "0" && std::string_view(value) != "1") {
                std::fprintf(stderr, "[archb] invalid %s=%s\n", entry.name, value);
                std::exit(EXIT_FAILURE);
            }
            values[index] = std::string_view(value) == "1";
        }
        bool changed;
        do {
            changed = false;
            for (const auto& entry : Entries) {
                const auto index = static_cast<std::size_t>(entry.feature);
                if (!values[index]) continue;
                for (const auto dependency : entry.prerequisites) {
                    const auto prerequisite = static_cast<std::size_t>(dependency);
                    if (values.at(prerequisite)) continue;
                    values[index] = false;
                    changed = true;
                    std::fprintf(stderr, "[archb] %s off: requires %s\n", entry.name, Entries.at(prerequisite).name);
                    break;
                }
            }
        } while (changed);
        std::fprintf(stderr, "%s\n", describe().c_str());
    });
}

bool Switches::Enabled(Feature feature) {
    Initialize();
    return values.at(static_cast<std::size_t>(feature));
}

std::string Switches::Describe() {
    Initialize();
    return describe();
}

std::span<const SwitchInfo> Switches::Table() {
    return Entries;
}

}
