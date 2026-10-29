#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace AgcDriver::ArchB {

namespace {

#ifdef APS5_ARCHB_SWITCHES_TEST
constexpr Feature TestPrerequisites[] = {Feature::GateRun};
constexpr Feature TestRuntimePrerequisites[] = {Feature::GateNoop};
#endif
constexpr std::array<SwitchInfo, static_cast<std::size_t>(Feature::Count)> Entries{{
    {Feature::GateRun, "GATE_RUN", false, false, {}},
    {Feature::GateNoop, "GATE_NOOP", false, true, {}},
    {Feature::Recorder, "RECORDER", false, true, {}},
#ifdef APS5_ARCHB_SWITCHES_TEST
    {Feature::TestDependent, "TEST_DEPENDENT", true, false, TestPrerequisites},
    {Feature::TestRuntimeDependent, "TEST_RUNTIME_DEPENDENT", false, true, TestRuntimePrerequisites},
#endif
}};
std::array<std::atomic<bool>, Entries.size()> values{};
std::array<bool, Entries.size()> fromEnvironment{};
std::atomic<std::int64_t> delayMicros{0};
std::filesystem::path controlFile;
std::chrono::steady_clock::time_point nextPoll;
std::uint64_t lastSequence = 0;
std::uint64_t lastSize = UINT64_MAX;
std::uint64_t lastModified = 0;
#ifdef APS5_ARCHB_SWITCHES_TEST
std::uint64_t controlPolls = 0;
#endif
std::once_flag initialized;

std::string describe() {
    std::string text = "[archb] switches";
    for (const auto& entry : Entries) {
        const auto index = static_cast<std::size_t>(entry.feature);
        text += " " + std::string(entry.name) + "=" + (values[index].load(std::memory_order_acquire) ? "1" : "0") + (fromEnvironment[index] ? "(env)" : "(default)") + (entry.runtime ? "(rt)" : "");
    }
    if (values[static_cast<std::size_t>(Feature::GateRun)].load(std::memory_order_acquire)) {
        text += " GATE_DELAY_US=" + std::to_string(delayMicros.load(std::memory_order_acquire)) + (std::getenv("APS5_ARCHB_GATE_DELAY_US") ? "(env)(rt)" : "(default)(rt)");
    }
    return text;
}

template<class T>
bool number(std::string_view text, T& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

void applyLine(const std::string& line) {
    std::istringstream stream(line);
    std::string token;
    std::uint64_t sequence = 0;
    const auto refuse = [&](const std::string& reason) {
        std::fprintf(stderr, "[archb] toggle refused seq=%llu %s\n", static_cast<unsigned long long>(sequence), reason.c_str());
        std::fflush(stderr);
    };
    if (!(stream >> token) || !number(token, sequence)) return refuse("invalid sequence");
    if (sequence <= lastSequence) return;
    for (const unsigned char character : line) {
        if (character > 127 || (character < 32 && character != '\t' && character != '\r')) return refuse("expected one ASCII line");
    }
    std::array<bool, Entries.size()> pending{};
    std::array<bool, Entries.size()> named{};
    for (std::size_t i = 0; i < values.size(); ++i) pending[i] = values[i].load(std::memory_order_acquire);
    auto delay = delayMicros.load(std::memory_order_acquire);
    bool delayNamed = false;
    bool any = false;
    while (stream >> token) {
        any = true;
        const auto equals = token.find('=');
        if (equals == std::string::npos) return refuse("invalid token " + token);
        const auto name = token.substr(0, equals);
        const auto value = std::string_view(token).substr(equals + 1);
        if (name == "GATE_DELAY_US") {
            std::uint64_t parsed = 0;
            if (delayNamed || !number(value, parsed) || parsed > INT64_MAX / 1000) return refuse("invalid GATE_DELAY_US");
            delayNamed = true;
            delay = static_cast<std::int64_t>(parsed);
            continue;
        }
        const SwitchInfo* found = nullptr;
        for (const auto& entry : Entries) if (name == entry.name) found = &entry;
        if (found == nullptr) return refuse("unknown " + name);
        if (!found->runtime) return refuse("startup-only " + name);
        const auto index = static_cast<std::size_t>(found->feature);
        if (named[index] || (value != "0" && value != "1")) return refuse("invalid " + name);
        named[index] = true;
        pending[index] = value == "1";
    }
    if (!any) return refuse("no values");
    bool changed;
    do {
        changed = false;
        for (const auto& entry : Entries) {
            const auto index = static_cast<std::size_t>(entry.feature);
            if (!entry.runtime || !pending[index]) continue;
            for (const auto prerequisite : entry.prerequisites) {
                if (pending.at(static_cast<std::size_t>(prerequisite))) continue;
                if (named[index]) return refuse(std::string(entry.name) + " prerequisite-disabled");
                pending[index] = false;
                changed = true;
                break;
            }
        }
    } while (changed);
    for (std::size_t i = 0; i < values.size(); ++i) values[i].store(pending[i], std::memory_order_release);
    delayMicros.store(delay, std::memory_order_release);
    lastSequence = sequence;
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::string text;
    for (const auto& entry : Entries) {
        if (entry.runtime) text += " " + std::string(entry.name) + "=" + (pending[static_cast<std::size_t>(entry.feature)] ? "1" : "0");
    }
    text += " GATE_DELAY_US=" + std::to_string(delay);
    std::fprintf(stderr, "[archb] toggle seq=%llu t_ns=%lld%s\n", static_cast<unsigned long long>(sequence), static_cast<long long>(nanos), text.c_str());
    std::fflush(stderr);
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
        if (values[static_cast<std::size_t>(Feature::GateRun)].load(std::memory_order_acquire)) {
            if (const char* value = std::getenv("APS5_ARCHB_GATE_DELAY_US")) {
                std::uint64_t parsed = 0;
                if (!number(std::string_view(value), parsed) || parsed > INT64_MAX / 1000) {
                    std::fprintf(stderr, "[archb] invalid GATE_DELAY_US=%s\n", value);
                    std::exit(EXIT_FAILURE);
                }
                delayMicros.store(static_cast<std::int64_t>(parsed), std::memory_order_release);
            }
            if (const char* path = std::getenv("APS5_ARCHB_CONTROL_FILE")) controlFile = path;
        }
        std::fprintf(stderr, "%s\n", describe().c_str());
    });
}

bool Switches::Enabled(Feature feature) {
    Initialize();
    return values.at(static_cast<std::size_t>(feature)).load(std::memory_order_acquire);
}

void Switches::PollControl() {
    if (controlFile.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < nextPoll) return;
    nextPoll = now + std::chrono::milliseconds(50);
#ifdef APS5_ARCHB_SWITCHES_TEST
    ++controlPolls;
#endif
    std::uint64_t size = 0;
    std::uint64_t modified = 0;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExW(controlFile.c_str(), GetFileExInfoStandard, &attributes)) return;
    size = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    modified = (static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32) | attributes.ftLastWriteTime.dwLowDateTime;
#else
    struct stat attributes{};
    if (stat(controlFile.c_str(), &attributes) != 0) return;
    size = static_cast<std::uint64_t>(attributes.st_size);
#ifdef __APPLE__
    modified = static_cast<std::uint64_t>(attributes.st_mtimespec.tv_sec) * 1000000000 + attributes.st_mtimespec.tv_nsec;
#else
    modified = static_cast<std::uint64_t>(attributes.st_mtim.tv_sec) * 1000000000 + attributes.st_mtim.tv_nsec;
#endif
#endif
    if (size == lastSize && modified == lastModified) return;
    std::ifstream stream(controlFile, std::ios::binary);
    if (!stream) return;
    std::string line((std::istreambuf_iterator<char>(stream)), {});
    if (stream.bad()) return;
    lastSize = size;
    lastModified = modified;
    if (!line.empty() && line.back() == '\n') line.pop_back();
    applyLine(line);
}

std::uint64_t Switches::Sequence() {
    return lastSequence;
}

std::int64_t Switches::GateDelayMicros() {
    Initialize();
    return delayMicros.load(std::memory_order_acquire);
}

#ifdef APS5_ARCHB_SWITCHES_TEST
std::uint64_t Switches::ControlPolls() {
    return controlPolls;
}
#endif

std::string Switches::Describe() {
    Initialize();
    return describe();
}

std::span<const SwitchInfo> Switches::Table() {
    return Entries;
}

}

extern "C" std::int64_t AgcDriverGateDelayMicros_nid_no_patch() {
    return AgcDriver::ArchB::Switches::GateDelayMicros();
}
