#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COPYBACKPROFILE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COPYBACKPROFILE_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string_view>
#include <vector>

namespace AgcDriver::GuestMemory {

enum class CopybackPath : std::uint8_t { HostStore, CopiedWriter, Mirror, StagingOut, Storage, Shadow, Image, Depth, CopyHle, Label, Eop, Dcc, Cmask, Htile, DirectGpu, Count };
inline constexpr std::array CopybackNames{"host-store", "copied-writer", "mirror", "staging-out", "storage", "shadow-publish", "image", "depth", "copy-hle", "label", "eop", "dcc", "cmask", "htile", "direct-gpu"};
static_assert(CopybackNames.size() == static_cast<std::size_t>(CopybackPath::Count));

constexpr std::uint32_t CopybackDebugMask(std::string_view paths) {
    std::uint32_t mask = 0;
    while (!paths.empty()) {
        const auto end = paths.find(',');
        const auto name = paths.substr(0, end);
        for (const auto path : {CopybackPath::Storage, CopybackPath::CopyHle, CopybackPath::StagingOut, CopybackPath::Shadow}) {
            const auto index = static_cast<std::size_t>(path);
            if (name == CopybackNames[index]) mask |= std::uint32_t{1} << index;
        }
        if (end == std::string_view::npos) break;
        paths.remove_prefix(end + 1);
    }
    return mask;
}

bool CopybackDebugSkip(CopybackPath path) noexcept;

struct CopybackCounters {
    std::uint64_t bytes = 0, ranges = 0, pages = 0, pageOverflow = 0;
    std::uint64_t sampled = 0, retired = 0, driver = 0, host = 0, cpuWrite = 0;
    std::uint64_t overwritten = 0, noObservedTouch = 0, censored = 0, refused = 0;
    std::uint64_t ns = 0, heldNs = 0, lockNs = 0;
};

struct CopybackReader {
    std::uintptr_t rip = 0, page = 0;
    std::uint64_t thread = 0, count = 0, ageNs = 0;
    CopybackPath path = CopybackPath::HostStore;
    std::uint8_t site = 0;
    bool host = false;
    std::array<char, 64> name{};
};

class CopybackTracker {
public:
    static constexpr std::uint64_t PageBytes = 4096;
    static constexpr std::size_t MaxProbes = 4096, MaxIntervals = 16384, MaxReaders = 64;
    static constexpr std::uint64_t SampleEvery = 64, SamplesPerSecond = 128;
    std::array<CopybackCounters, CopybackNames.size()> window{};
    std::vector<CopybackReader> readers;
    std::map<std::pair<CopybackPath, std::uint32_t>, std::pair<std::uint64_t, std::uint64_t>> cpuScopes;
    std::uint64_t cpuScopeOverflow = 0;

    void CpuScope(CopybackPath path, std::uint32_t threadTag, std::uint64_t ns) {
        const auto key = std::pair{path, threadTag};
        if (!cpuScopes.contains(key) && cpuScopes.size() >= MaxReaders) { ++cpuScopeOverflow; return; }
        auto& [count, elapsed] = cpuScopes[key];
        ++count;
        elapsed += ns;
    }

    std::uint64_t NewPublication() { return ++publication; }

    void Record(CopybackPath path, std::uint64_t address, std::size_t bytes, std::uint64_t now, bool sample, std::uint64_t generation = 0) {
        if (!valid(address, bytes)) return;
        if (generation == 0) generation = NewPublication();
        const auto index = static_cast<std::size_t>(path);
        auto& counts = window[index];
        counts.bytes += bytes;
        ++counts.ranges;
        addPages(index, address / PageBytes, (address + bytes - 1) / PageBytes + 1);
        visit(address, bytes, [&](auto found) {
            if (found->second.generation == generation) return false;
            retire(found->second, true);
            return true;
        });
        if (!sample || sequence[index]++ % SampleEvery != 0) return;
        const auto second = now / 1000000000;
        if (second != sampleSecond) { sampleSecond = second; samplesThisSecond = 0; }
        if (samplesThisSecond >= SamplesPerSecond || probes.size() >= MaxProbes) { ++counts.refused; return; }
        const auto first = address / PageBytes;
        const auto pages = (address + bytes - 1) / PageBytes + 1 - first;
        const auto page = (first + ((sequence[index] - 1) / SampleEvery) % pages) * PageBytes;
        if (!probes.emplace(page, Probe{path, now, generation}).second) return;
        ++samplesThisSecond;
        ++counts.sampled;
    }

    bool Read(std::uint64_t address, std::size_t bytes, CopybackReader reader, std::uint64_t now) {
        bool hit = false;
        visit(address, bytes, [&](auto found) {
            auto& probe = found->second;
            auto& seen = reader.host ? probe.host : probe.driver;
            if (seen) return false;
            seen = true;
            hit = true;
            reader.page = found->first;
            reader.path = probe.path;
            reader.ageNs = now >= probe.time ? now - probe.time : 0;
            auto entry = std::find_if(readers.begin(), readers.end(), [&](const auto& value) { return value.rip == reader.rip && value.thread == reader.thread && value.path == reader.path && value.site == reader.site && value.host == reader.host; });
            if (entry == readers.end()) {
                if (readers.size() < MaxReaders) { reader.count = 1; readers.push_back(reader); }
                else ++readerOverflow;
            } else { ++entry->count; entry->ageNs += reader.ageNs; }
            return false;
        });
        return hit;
    }

    void CpuWrite(std::uint64_t address, std::size_t bytes) {
        visit(address, bytes, [](auto found) { found->second.cpuWrite = true; return false; });
    }

    void Forget(std::uint64_t address, std::size_t bytes) {
        visit(address, bytes, [&](auto found) { retire(found->second, false); return true; });
    }

    void Disarm() {
        for (const auto& [page, probe] : probes) retire(probe, false);
        probes.clear();
    }

    void Expire(std::uint64_t now) {
        for (auto found = probes.begin(); found != probes.end();) {
            if (now >= found->second.time && now - found->second.time >= 30000000000ull) {
                retire(found->second, false);
                found = probes.erase(found);
            } else ++found;
        }
    }

    void NextWindow() {
        cpuScopes.clear();
        cpuScopeOverflow = 0;
        for (std::size_t index = 0; index < window.size(); ++index) {
            window[index] = {};
            uniquePages[index].clear();
        }
    }

    std::size_t Armed() const { return probes.size(); }
    std::uint64_t ReaderOverflow() const { return readerOverflow; }

private:
    struct Probe {
        CopybackPath path;
        std::uint64_t time, generation;
        bool driver = false, host = false, cpuWrite = false;
    };
    std::map<std::uint64_t, Probe> probes;
    std::array<std::map<std::uint64_t, std::uint64_t>, CopybackNames.size()> uniquePages;
    std::array<std::uint64_t, CopybackNames.size()> sequence{};
    std::uint64_t sampleSecond = 0, samplesThisSecond = 0, readerOverflow = 0, publication = 0;

    static bool valid(std::uint64_t address, std::size_t bytes) { return bytes != 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address; }

    template<class TAction> void visit(std::uint64_t address, std::size_t bytes, TAction action) {
        if (!valid(address, bytes)) return;
        for (auto found = probes.lower_bound(address & ~(PageBytes - 1)); found != probes.end() && found->first < address + bytes;) {
            if (action(found)) found = probes.erase(found);
            else ++found;
        }
    }

    void retire(const Probe& probe, bool overwritten) {
        auto& counts = window[static_cast<std::size_t>(probe.path)];
        ++counts.retired;
        counts.driver += probe.driver;
        counts.host += probe.host;
        counts.cpuWrite += probe.cpuWrite;
        counts.overwritten += overwritten;
        counts.censored += !overwritten;
        counts.noObservedTouch += overwritten && !probe.driver && !probe.host && !probe.cpuWrite;
    }

    void addPages(std::size_t index, std::uint64_t first, std::uint64_t end) {
        auto& pages = uniquePages[index];
        auto& counts = window[index];
        auto found = pages.lower_bound(first);
        if (found != pages.begin() && std::prev(found)->second >= first) --found;
        if (pages.size() >= MaxIntervals && (found == pages.end() || found->first > end)) { ++counts.pageOverflow; return; }
        while (found != pages.end() && found->first <= end) {
            if (found->second < first) { ++found; continue; }
            first = std::min(first, found->first);
            end = std::max(end, found->second);
            counts.pages -= found->second - found->first;
            found = pages.erase(found);
        }
        pages.emplace(first, end);
        counts.pages += end - first;
    }
};

bool CopybackEnabled() noexcept;
bool CopybackReadsEnabled() noexcept;
void CopybackDropped() noexcept;
CopybackPath CopybackCurrentPath() noexcept;
std::uint64_t CopybackNewPublication() noexcept;
void CopybackRecord(CopybackPath path, std::uint64_t address, std::size_t bytes, std::uint64_t generation = 0) noexcept;
void CopybackRecordCurrent(std::uint64_t address, std::size_t bytes, std::uint64_t generation = 0) noexcept;
void CopybackRead(std::uint64_t address, std::size_t bytes, const void* caller, bool host = false) noexcept;
void CopybackCpuWrite(std::uint64_t address, std::size_t bytes) noexcept;
void CopybackForget(std::uint64_t address, std::size_t bytes) noexcept;
void CopybackDisarm() noexcept;
void CopybackReport(std::uint64_t presents) noexcept;
struct CopybackSnapshot {
    std::array<CopybackCounters, CopybackNames.size()> counts{};
    std::size_t active = 0;
};
CopybackSnapshot CopybackCounts() noexcept;

class CopybackScope {
public:
    explicit CopybackScope(CopybackPath path) noexcept;
    ~CopybackScope();
    CopybackScope(const CopybackScope&) = delete;
    CopybackScope& operator=(const CopybackScope&) = delete;

private:
    CopybackPath path, previous;
    std::uint64_t started = 0, held = 0;
};

}

#endif
