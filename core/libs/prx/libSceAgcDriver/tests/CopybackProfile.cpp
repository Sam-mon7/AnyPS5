#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace {
using namespace AgcDriver::GuestMemory;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Model() {
    const auto bit = [](CopybackPath path) { return std::uint32_t{1} << static_cast<std::size_t>(path); };
    Require(CopybackDebugMask("") == 0 && CopybackDebugMask("label,eop,all,storage-extra,copy") == 0, "debug skip accepted a protected or inexact name");
    Require(CopybackDebugMask(",storage,,storage,copy-hle,staging-out,shadow-publish,label,eop,") == (bit(CopybackPath::Storage) | bit(CopybackPath::CopyHle) | bit(CopybackPath::StagingOut) | bit(CopybackPath::Shadow)), "debug skip list did not select exactly the allowed paths");
    CopybackTracker tracker;
    constexpr auto path = CopybackPath::CopiedWriter;
    constexpr auto other = CopybackPath::Image;
    constexpr auto index = static_cast<std::size_t>(path);
    tracker.Record(path, 0x10000, 8192, 100, true);
    tracker.Record(path, 0x11000, 4096, 200, true);
    Require(tracker.window[index].bytes == 12288 && tracker.window[index].ranges == 2 && tracker.window[index].pages == 2, "bytes, ranges or unique page union differ");
    Require(tracker.Armed() == 1, "unsampled range armed a probe");
    CopybackReader reader;
    reader.rip = 0x1234;
    reader.thread = 7;
    Require(!tracker.Read(0x11000, 1, reader, 250), "probe observed an unsampled page");
    Require(tracker.Read(0x10080, 1, reader, 300), "probe missed a CPU read hook");
    Require(!tracker.Read(0x10080, 1, reader, 400), "driver read observation was not one shot");
    reader.host = true;
    Require(tracker.Read(0x10000, 1, reader, 500), "host read was not separate from driver read");
    tracker.CpuWrite(0x10000, 4096);
    tracker.Record(other, 0x10000, 4096, 600, false);
    const auto& counts = tracker.window[index];
    Require(counts.retired == 1 && counts.driver == 1 && counts.host == 1 && counts.cpuWrite == 1 && counts.overwritten == 1 && counts.noObservedTouch == 0, "retirement lost the original path or access classes");
    Require(tracker.readers.size() == 2 && tracker.readers.front().ageNs == 200 && tracker.readers.front().page == 0x10000, "reader identity or time since copy-back differs");
    tracker.Record(other, 0x20000, 1, 700, true);
    tracker.Record(path, 0x20000, 1, 800, false);
    Require(tracker.window[static_cast<std::size_t>(other)].noObservedTouch == 1, "unobserved overwrite was not counted");
    CopybackTracker merged;
    const auto publication = merged.NewPublication();
    merged.Record(path, 0x60000, 1, 100, true, publication);
    merged.Record(path, 0x60010, 1, 200, true, publication);
    Require(merged.Armed() == 1 && merged.window[index].retired == 0, "spans of one merge store overwrote their own sample");
    tracker.CpuScope(path, 0, 10);
    tracker.CpuScope(path, 0, 20);
    tracker.CpuScope(other, 0x28, 40);
    Require(tracker.cpuScopes.size() == 2 && tracker.cpuScopes.at({path, 0}) == std::pair<std::uint64_t, std::uint64_t>{2, 30}, "CPU scopes lost their path, thread or inclusive time");
    tracker.NextWindow();
    Require(tracker.cpuScopes.empty(), "CPU scopes crossed report windows");
    Require(tracker.window[index].bytes == 0 && tracker.window[index].pages == 0, "window counters did not reset");
    CopybackTracker lifetime;
    lifetime.Record(path, 0x30000, 4096, 1000000000, true);
    Require(lifetime.Armed() == 1, "lifetime probe did not arm");
    lifetime.Forget(0x30000, 4096);
    Require(!lifetime.Read(0x30000, 1, reader, 1000000001), "forgotten page retained its reader");
    lifetime.Record(other, 0x50000, 1, 1000000002, true);
    Require(lifetime.Armed() == 1, "shutdown probe did not arm");
    lifetime.Disarm();
    Require(lifetime.Armed() == 0, "shutdown retained probes");
    tracker.Disarm();
    for (std::size_t i = 0; i < CopybackTracker::SampleEvery * 300; ++i) tracker.Record(path, 0x40000 + i * 4096, 1, 2000000000, true);
    Require(tracker.Armed() <= CopybackTracker::SamplesPerSecond && tracker.window[index].refused != 0, "sampling exceeded its per-second cap");
    tracker.Expire(33000000000ull);
    Require(tracker.Armed() == 0 && tracker.window[index].censored != 0, "expired probes were classified as untouched");
    tracker.Record(path, UINT64_MAX, 2, 0, false);
}

void DebugSkips(std::string_view paths) {
    const auto mask = CopybackDebugMask(paths);
    for (std::size_t index = 0; index < CopybackNames.size(); ++index) Require(CopybackDebugSkip(static_cast<CopybackPath>(index)) == ((mask & (std::uint32_t{1} << index)) != 0), "cached debug skip differs from its environment");
    alignas(8) std::array<std::byte, 8> guest{};
    const std::array<std::byte, 8> payload{std::byte{0x52}};
    const auto address = reinterpret_cast<std::uintptr_t>(guest.data());
    SetFlushHook(nullptr);
    for (const auto path : {CopybackPath::Label, CopybackPath::Eop}) {
        const CopybackScope scope(path);
        Write(address, payload);
        Require(guest == payload, "debug copy-back skips suppressed a label or EOP store");
        guest.fill(std::byte{});
    }
}

void Runtime(bool enabled, bool reads) {
    Require(CopybackEnabled() == enabled && CopybackReadsEnabled() == reads, "profiling switches have the wrong default or dependency");
    alignas(4096) std::array<std::byte, 8192> guest{};
    const std::array<std::byte, 16> payload{std::byte{0x42}};
    const auto address = reinterpret_cast<std::uintptr_t>(guest.data());
    SetFlushHook(nullptr);
    {
        const CopybackScope path(CopybackPath::CopiedWriter);
        Write(address, payload);
    }
    std::array<std::byte, 16> actual{};
    Read(address, actual);
    Require(actual == payload, "profiling changed copied bytes");
    const std::array<std::byte, 16> changed{std::byte{0x43}};
    {
        const CopybackScope path(CopybackPath::Image);
        WriteChanged(address, changed, payload);
    }
    GuestArena::GuestArenaPrepareHostRead_nid_no_patch(guest.data(), actual.size());
    Read(address, actual);
    Require(actual == changed, "profiling changed the merge write-back");
    const auto before = CopybackCounts();
    const auto& copied = before.counts[static_cast<std::size_t>(CopybackPath::CopiedWriter)];
    const auto& image = before.counts[static_cast<std::size_t>(CopybackPath::Image)];
    Require(copied.bytes == (enabled ? 16u : 0u) && image.bytes == (enabled ? 1u : 0u), "disabled counters ran or merged stores counted unchanged bytes");
    Require(before.active == (reads ? 1u : 0u) && copied.driver == (reads ? 1u : 0u), "runtime read hooks did not observe the sampled page");
    CopybackForget(address, guest.size());
    const auto after = CopybackCounts();
    const auto& retired = after.counts[static_cast<std::size_t>(CopybackPath::Image)];
    Require(after.active == 0 && retired.host == (reads ? 1u : 0u) && retired.driver == (reads ? 1u : 0u), "host hook or clean disarm failed");
    CopybackDisarm();
    CopybackReport(1);
}
}

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "model";
        if (mode == "model") Model();
        else if (mode == "debug-skip") DebugSkips(argc > 2 ? argv[2] : "");
        else Runtime(mode == "counters" || mode == "reads", mode == "reads");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "copyback profile: %s\n", error.what());
        return 1;
    }
}
