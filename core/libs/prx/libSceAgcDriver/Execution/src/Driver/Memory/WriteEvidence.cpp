#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/WriteEvidence.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>

namespace AgcDriver::DriverDetail {

namespace {

void ProfilePendingRead(std::uint64_t address, std::size_t bytes, bool value, ShaderMemory::PendingWrite policy, std::uint64_t ValidateCounters::*reason, const std::optional<WrittenBuffer>& writer) {
    static constexpr std::array reasons{&ValidateCounters::syncedNoWriter, &ValidateCounters::syncedForeign, &ValidateCounters::syncedLargeRange, &ValidateCounters::syncedLabel, &ValidateCounters::syncedEvidence, &ValidateCounters::syncedWriterChanged, &ValidateCounters::syncedSample, &ValidateCounters::syncedImage, &ValidateCounters::syncedShadow, &ValidateCounters::syncedOff};
    static constexpr std::array<const char*, 10> names{"no-writer", "foreign", "range", "label", "evidence", "writer-changed", "sample", "image", "shadow", "off"};
    static constexpr std::array<const char*, 6> policies{"none", "sync", "raw", "verify-raw", "known", "verify-known"};
    using Key = std::array<std::uint64_t, 10>;
    struct Counts {
        std::uint64_t calls = 0, address = 0, bytes = 0, begin = 0, end = 0;
    };
    static AgcDriver::Mutex mutex;
    static std::map<Key, Counts> counts;
    static std::uint64_t overflow = 0;
    static auto last = std::chrono::steady_clock::now();
    const auto packet = GuestMemory::CurrentPacket();
    const auto found = std::find(reasons.begin(), reasons.end(), reason);
    const Key key{packet.queue, packet.opcode, static_cast<std::uint64_t>(GuestMemory::CurrentReadSite()), value, static_cast<std::uint64_t>(found - reasons.begin()), static_cast<std::uint64_t>(policy), writer ? writer->program : 0, writer ? writer->queue : 0, writer ? writer->fill ? 1u : writer->value ? 2u : 0u : 0u, bytes <= 4 ? 0u : bytes <= 4096 ? 1u : 2u};
    std::lock_guard lock(mutex);
    if (counts.size() < 256 || counts.contains(key)) {
        auto& count = counts[key];
        ++count.calls;
        count.address = address;
        count.bytes = bytes;
        count.begin = writer ? writer->begin : 0;
        count.end = writer ? writer->end : 0;
    } else {
        ++overflow;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(10)) return;
    last = now;
    std::vector<std::pair<Key, Counts>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        const auto priority = [](const auto& item) { return std::tuple{item.first[3] != 0 && item.first[5] == static_cast<std::uint64_t>(ShaderMemory::PendingWrite::Sync), item.first[3], item.second.calls}; };
        return priority(left) > priority(right);
    });
    ProfilePrint_nid_no_patch("[capture-policy] pending queries (10 s): %zu groups, %llu overflow; fill known values %llu entries, %llu reads served, %llu invalidated reads\n", counts.size(), static_cast<unsigned long long>(overflow), static_cast<unsigned long long>(fillKnownEntries.exchange(0)), static_cast<unsigned long long>(fillKnownReads.exchange(0)), static_cast<unsigned long long>(fillKnownInvalidated.exchange(0)));
    for (std::size_t index = 0; index < std::min<std::size_t>(sorted.size(), 32); ++index) {
        const auto& [item, count] = sorted[index];
        ProfilePrint_nid_no_patch("[capture-policy] q0x%llx opcode 0x%llx %s %s %s/%s: %llu queries; last read 0x%llx+0x%llx writer q0x%llx program 0x%llx 0x%llx+0x%llx %s\n", static_cast<unsigned long long>(item[0]), static_cast<unsigned long long>(item[1]), GuestMemory::ReadSiteName(static_cast<GuestMemory::ReadSite>(item[2])), item[3] ? "value" : "probe", policies[item[5]], item[5] == static_cast<std::uint64_t>(ShaderMemory::PendingWrite::Sync) && item[4] < names.size() ? names[item[4]] : "-", static_cast<unsigned long long>(count.calls), static_cast<unsigned long long>(count.address), static_cast<unsigned long long>(count.bytes), static_cast<unsigned long long>(item[7]), static_cast<unsigned long long>(item[6]), static_cast<unsigned long long>(count.begin), static_cast<unsigned long long>(count.end - count.begin), item[8] == 1 ? "fill" : item[8] == 2 ? "copy-known" : "unknown");
    }
    counts.clear();
    overflow = 0;
}

}

SampledReadScope::SampledReadScope(std::atomic<std::uint64_t>& reads) : previous(Driver::sampledRead()) {
    Driver::sampledRead() = Driver::writeEvidenceEnabled() && reads.fetch_add(1, std::memory_order_relaxed) % Driver::writeEvidenceSampleEvery() == 0;
}

SampledReadScope::~SampledReadScope() { Driver::sampledRead() = previous; }

void PendingView::Load() {
    if (Driver::validateLegacy()) return;
    const auto current = Graphics::Recorder::PublishGeneration();
    if (loaded && current == generation) return;
    snapshot = Graphics::Recorder::PendingWriteSnapshot();
    generation = current;
    loaded = true;
}

bool PendingView::Overlaps(std::uint64_t address, std::size_t bytes) const {
    return Driver::validateLegacy() ? Graphics::Recorder::SnapshotWriteOverlaps(address, bytes) : Graphics::Recorder::SnapshotOverlaps(snapshot.get(), address, bytes);
}

bool& Driver::sampledRead() {
    static thread_local bool sampled = false;
    return sampled;
}

bool Driver::writeEvidenceEnabled() {
    static const bool enabled = std::getenv("APS5_NO_WRITE_EVIDENCE") == nullptr;
    return enabled;
}

bool Driver::writerKeyedEvidence() {
    static const bool keyed = std::getenv("APS5_NO_WRITER_KEYED_EVIDENCE") == nullptr;
    return keyed;
}

bool Driver::foreignWriters() {
    static const bool noted = std::getenv("APS5_NO_FOREIGN_WRITERS") == nullptr;
    return noted;
}

bool Driver::observationGuard() {
    static const bool guard = std::getenv("APS5_NO_OBSERVATION_GUARD") == nullptr;
    return guard;
}

bool Driver::siteSampling() {
    static const bool perSite = std::getenv("APS5_NO_SITE_SAMPLING") == nullptr;
    return perSite;
}

std::uint64_t Driver::hookWaits() {
    return Graphics::Recorder::ThreadHookWaits();
}

ShaderMemory::HookWaitCounter Driver::hookWaitCounter() {
    return observationGuard() ? &hookWaits : nullptr;
}

bool Driver::validateSkipEnabled() {
    static const bool enabled = std::getenv("APS5_NO_VALIDATE_SKIP") == nullptr;
    return enabled;
}

bool Driver::validateLegacy() {
    static const bool legacy = std::getenv("APS5_VALIDATE_LEGACY") != nullptr;
    return legacy;
}

bool Driver::gateRetry() {
    static const bool retry = std::getenv("APS5_NO_GATE_RETRY") == nullptr && !validateLegacy();
    return retry;
}

bool Driver::validateSkipVerify() {
    static const bool verify = std::getenv("APS5_VALIDATE_SYNC_SKIP") != nullptr;
    return verify;
}

std::uint32_t Driver::writeEvidenceAfter() {
    static const std::uint32_t value = [] { const char* text = std::getenv("APS5_WRITE_EVIDENCE_AFTER"); return text ? static_cast<std::uint32_t>(std::atoi(text)) : 4u; }();
    return value;
}

std::uint32_t Driver::writeEvidenceSampleEvery() {
    static const std::uint32_t value = [] { const char* text = std::getenv("APS5_WRITE_EVIDENCE_SAMPLE"); const auto parsed = text ? std::atoi(text) : 16; return parsed > 0 ? static_cast<std::uint32_t>(parsed) : 16u; }();
    return value;
}

std::uint64_t Driver::writeEvidenceMaxBytes() {
    static const std::uint64_t value = [] { const char* text = std::getenv("APS5_WRITE_EVIDENCE_MAX_KIB"); return (text ? std::strtoull(text, nullptr, 10) : 16ull) << 10u; }();
    return value;
}

bool Driver::traceCapSync() {
    static const bool trace = std::getenv("APS5_TRACE_CAPSYNC") != nullptr;
    return trace;
}

void Driver::observeDword(std::uint64_t address, bool unchanged) {
    if (!writeEvidenceEnabled()) return;
    (unchanged ? observedUnchanged : observedChanged).fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(writtenBuffersMutex);
    if (dwordEvidence.size() >= DwordEvidenceEntries && !dwordEvidence.contains(address)) dwordEvidence.clear();
    auto& evidence = dwordEvidence[address];
    if (writerKeyedEvidence()) {
        const auto writer = newestWriterLocked(address, address + 4);
        const std::uint64_t program = writer ? writer->program : 0, begin = writer ? writer->begin : 0, end = writer ? writer->end : 0;
        if (program != evidence.program || begin != evidence.begin || end != evidence.end) evidence = DwordEvidence{0, 0, program, begin, end};
    }
    if (!unchanged) {
        evidence.streak = 0;
        ++evidence.changed;
    } else if (evidence.streak < (1u << 30u)) {
        ++evidence.streak;
    }
}

void Driver::observeRange(std::uint64_t address, std::span<const std::byte> before) {
    if (before.empty() || !GuestMemory::Accessible(reinterpret_cast<const void*>(address), before.size())) return;
    const auto* now = reinterpret_cast<const std::byte*>(address);
    for (std::size_t offset = 0; offset + 4 <= before.size(); offset += 4) {
        observeDword(address + offset, std::memcmp(before.data() + offset, now + offset, 4) == 0);
    }
}

void Driver::observePendingWrite(std::uint64_t address, bool unchanged) {
    Get().observeDword(address, unchanged);
}

bool Driver::knownValueCurrent(const WrittenBuffer& writer) {
    const auto bytes = static_cast<std::size_t>(writer.end - writer.begin);
    GuestMemory::CollectWrites(writer.begin, bytes);
    return GuestMemory::UnchangedSince(writer.begin, bytes, writer.generation);
}

ShaderMemory::PendingWrite Driver::classifyPendingWrite(std::uint64_t address, std::size_t bytes, std::uint64_t ValidateCounters::*& reason, const PendingView& pending, std::span<std::byte> known) {
    using Policy = ShaderMemory::PendingWrite;
    if (!pending.Overlaps(address, bytes)) return Policy::None;

    std::optional<WrittenBuffer> writer;
    const auto classify = [&] {
        if (Graphics::AnyShadowedOverlaps(address, bytes)) {
            reason = &ValidateCounters::syncedShadow;
            return Policy::Sync;
        }
        reason = &ValidateCounters::syncedOff;
        if (!writeEvidenceEnabled()) return Policy::Sync;
        writer = newestWriter(address, address + bytes);
        if (!writer) {
            reason = &ValidateCounters::syncedNoWriter;
            return Policy::Sync;
        }
        if (writer->program == 0 && !writer->fill) {
            reason = &ValidateCounters::syncedForeign;
            return Policy::Sync;
        }
        if (!writer->fill && (writer->end - writer->begin > writeEvidenceMaxBytes() || writer->begin < 4 || pending.Overlaps(writer->begin - 4, 4) || pending.Overlaps(writer->end, 4))) {
            reason = &ValidateCounters::syncedLargeRange;
            return Policy::Sync;
        }
        const auto first = address & ~3ull;
        const auto limit = address + bytes;
        if (Graphics::Recorder::WideLabelIn(first, static_cast<std::size_t>(limit - first))) {
            reason = &ValidateCounters::syncedLabel;
            return Policy::Sync;
        }
        for (std::uint64_t dword = first; dword < limit; dword += 4) {
            if (Graphics::Recorder::LookupLabelValue(dword, 4, 0).has_value()) {
                reason = &ValidateCounters::syncedLabel;
                return Policy::Sync;
            }
        }
        if (writer->value && writer->begin <= address && limit <= writer->end && (known.empty() || known.size() == bytes) && knownValueCurrent(*writer)) {
            if (writer->fill) {
                const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> ranges{{{address, limit}}};
                if (Graphics::StorageTexture::AnyPendingOverlaps(ranges)) {
                    reason = &ValidateCounters::syncedImage;
                    return Policy::Sync;
                }
            }
            if (!known.empty()) {
                std::memcpy(known.data(), writer->value->data() + (address - writer->begin), bytes);
                knownValueReads.fetch_add(1, std::memory_order_relaxed);
                static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
                if (profile && writer->fill) fillKnownReads.fetch_add(1, std::memory_order_relaxed);
            }
            return knownValueVerify() ? Policy::VerifyKnownValue : Policy::KnownValue;
        }
        if (writer->fill) {
            static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
            if (profile && !known.empty()) fillKnownInvalidated.fetch_add(1, std::memory_order_relaxed);
            reason = &ValidateCounters::syncedWriterChanged;
            return Policy::Sync;
        }
        {
            std::lock_guard lock(writtenBuffersMutex);
            for (std::uint64_t dword = first; dword < limit; dword += 4) {
                const auto found = dwordEvidence.find(dword);
                if (found == dwordEvidence.end() || found->second.streak < writeEvidenceAfter()) {
                    reason = &ValidateCounters::syncedEvidence;
                    return Policy::Sync;
                }
                if (writerKeyedEvidence() && (found->second.program != writer->program || found->second.begin != writer->begin || found->second.end != writer->end)) {
                    reason = &ValidateCounters::syncedWriterChanged;
                    return Policy::Sync;
                }
            }
        }
        if (sampledRead()) {
            reason = &ValidateCounters::syncedSample;
            return Policy::Sync;
        }
        return validateSkipVerify() ? Policy::VerifyRaw : Policy::Raw;
    };
    const auto policy = classify();
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (profile) ProfilePendingRead(address, bytes, !known.empty(), policy, reason, writer);
    return policy;
}

ShaderMemory::PendingWrite Driver::queryPendingWrite(std::uint64_t address, std::size_t bytes, std::span<std::byte> known) {
    if (DrawPipeline::Active()) {
        auto& pipeline = DrawPipeline::Queue0();
        if (pipeline.Busy() && pipeline.Overlaps(address, bytes)) pipeline.Drain(DrawPipeline::DrainReason::Capture);
    }
    std::uint64_t ValidateCounters::*reason = nullptr;
    PendingView pending;
    pending.Load();
    return Get().classifyPendingWrite(address, bytes, reason, pending, known);
}

ShaderMemory::PendingWrite Driver::QueryPendingSpan(std::uint64_t address, std::size_t bytes, std::span<std::byte> known) {
    if (DrawPipeline::Active()) {
        auto& pipeline = DrawPipeline::Queue0();
        if (pipeline.Busy() && pipeline.Overlaps(address, bytes)) return ShaderMemory::PendingWrite::Sync;
    }
    std::uint64_t ValidateCounters::*reason = nullptr;
    PendingView pending;
    pending.Load();
    return Get().classifyPendingWrite(address, bytes, reason, pending, known);
}

}
