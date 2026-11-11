#include "prx/libSceAgcDriver/Execution/include/Driver/Replay/RegionCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/ThreadScratch.hpp"
#include "prx/libSceAgcDriver/Execution/include/Stage4Diag.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace AgcDriver::DriverDetail::Replay::RegionCensus {

struct Use {
    std::string role;
    std::uint32_t stage = 0, consumer = 0, input = 0;
    std::uint64_t address = 0, clock = 0;
    std::shared_ptr<const std::vector<std::byte>> bytes;
    int equal = -1, fault = -1;
    bool sameAddress = false;
};

struct Patch {
    std::uint32_t offset = 0, value = 0;
    bool alternating = false;
    std::vector<Use> uses;
    bool changed = true;
};

struct Metrics {
    std::uint64_t queue = 0, queueWait = 0, commit = 0, commitWait = 0, mutex = 0, gpu = 0, queueLocks = 0, commitLocks = 0, pipe = 0, failed = 0;
    std::uint64_t queueProbe = 0, commitProbe = 0;
    std::uint64_t queuePipeLocks = 0, commitPipeLocks = 0;
};

struct Packet {
    std::weak_ptr<Region> region;
    ResolvedItem item;
    std::vector<std::uint32_t> words;
    std::uint64_t tableClock = 0, inputClock = 0;
    Metrics metrics;
    bool operation = false;
    std::atomic<ResolvedKind> resolvedKind{ResolvedKind::Count};
    std::atomic<bool> complete{true};
    struct Pointer {
        std::weak_ptr<Region> source;
        std::uint32_t patch = 0, stage = 0, input = 0;
        std::uint64_t address = 0;
        const char* role = "user_data_pointer";
    };
    std::vector<Pointer> pointers;
};

struct Region {
    std::uint64_t frame = 0;
    std::uint64_t sequence = 0;
    std::uint32_t queue = 0, ordinal = 0, recognizedWords = 0;
    RegionMatch match = RegionMatch::New;
    std::vector<std::uint32_t> words;
    std::vector<Patch> patches;
    std::vector<std::shared_ptr<Packet>> packets;
    std::vector<ResolvedItem> resolved;
    std::size_t pending = 0;
    bool sealed = false;
    bool complete = true;
};

namespace {

bool timingOnly() {
    static const bool value = [] { const char* text = std::getenv("APS5_REPLAY_REGION_CENSUS_TIMING_ONLY"); return text != nullptr && text[0] == '1'; }();
    return value;
}

struct Origin {
    std::weak_ptr<Region> region;
    std::uint32_t offset = 0;
    std::uint32_t value = 0;
};

struct Local {
    std::shared_ptr<Region> region;
    std::shared_ptr<Packet> packet;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Origin> origins;
    std::uint64_t locks = 0;
    std::uint64_t observationNs = 0, commitObservation = 0;
    std::uint32_t observationDepth = 0;
    std::uint32_t waitDepth = 0;
    bool committing = false;
};

Local& local() {
    struct CensusLocalStorage {};
    return AgcDriver::ThreadScratch<Local, CensusLocalStorage>();
}

std::uint64_t now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Frame {
    std::uint64_t number = 0;
    bool sealed = false, reported = false;
    std::map<std::uint32_t, std::vector<std::shared_ptr<Region>>> queues;
};

struct Census {
    std::mutex mutex;
    std::deque<Frame> frames{Frame{}};
    std::FILE* output = nullptr;
    bool opened = false;
    std::map<std::string, std::string> counterNames;
};

Census& census() {
    static auto* value = new Census;
    return *value;
}

void count(const std::string& name, double value = 1) {
    auto& stored = census().counterNames[name];
    if (stored.empty()) stored = "rrc_" + name;
    ArchB::Recorder::Count(stored.c_str(), value);
}

void number(std::string& text, std::uint64_t value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    text.append(buffer, result.ptr);
}

void values(std::string& text, const std::vector<std::uint64_t>& words) {
    text += '[';
    bool first = true;
    for (const auto value : words) {
        if (!std::exchange(first, false)) text += ',';
        number(text, value);
    }
    text += ']';
}

const Region* findRegion(std::uint64_t frame, std::uint32_t queue, std::uint32_t ordinal) {
    for (const auto& candidate : census().frames) {
        if (candidate.number != frame) continue;
        const auto found = candidate.queues.find(queue);
        return found != candidate.queues.end() && ordinal < found->second.size() ? found->second[ordinal].get() : nullptr;
    }
    return nullptr;
}

Patch* patchAt(Region& region, std::uint32_t offset) {
    const auto found = std::lower_bound(region.patches.begin(), region.patches.end(), offset, [](const Patch& patch, std::uint32_t value) { return patch.offset < value; });
    return found != region.patches.end() && found->offset == offset ? &*found : nullptr;
}

const Use* olderUse(const Region& region, const Patch& patch, const Use& use) {
    if (region.frame < 2) return nullptr;
    const auto* older = findRegion(region.frame - 2, region.queue, region.ordinal);
    if (older == nullptr || older->sequence != region.sequence) return nullptr;
    for (const auto& candidate : older->patches) {
        if (candidate.offset != patch.offset) continue;
        for (const auto& prior : candidate.uses) {
            if (prior.role == use.role && prior.stage == use.stage && prior.consumer == use.consumer && prior.input == use.input) return &prior;
        }
    }
    return nullptr;
}

std::uint64_t byteHash(std::span<const std::byte> bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto value : bytes) hash = (hash ^ static_cast<std::uint8_t>(value)) * 0x100000001b3ull;
    return hash;
}

void appendDifference(std::string& text, const ResolvedDifference& difference) {
    if (difference.equal) { text += "null"; return; }
    text += '[';
    number(text, difference.item); text += ',';
    number(text, difference.field); text += ',';
    number(text, difference.word); text += ',';
    number(text, difference.before); text += ',';
    number(text, difference.after); text += ']';
}

void report(Region& region) {
    const auto started = now();
    Metrics metrics;
    region.resolved.clear();
    std::array<std::uint64_t, static_cast<std::size_t>(ResolvedKind::Count)> kinds{};
    std::uint64_t outside = 0;
    for (const auto& packet : region.packets) {
        if (!packet->complete.load(std::memory_order_relaxed)) region.complete = false;
        if (const auto kind = packet->resolvedKind.load(std::memory_order_relaxed); kind != ResolvedKind::Count) {
            packet->item.kind = kind;
            packet->operation = true;
        }
        if (packet->operation) region.resolved.push_back(std::move(packet->item));
        if (packet->item.offset >= region.recognizedWords) {
            if (packet->operation) ++outside;
            continue;
        }
        metrics.queue += packet->metrics.queue;
        metrics.queueWait += packet->metrics.queueWait;
        metrics.commit += packet->metrics.commit;
        metrics.commitWait += packet->metrics.commitWait;
        metrics.mutex += packet->metrics.mutex;
        metrics.gpu += packet->metrics.gpu;
        metrics.queueLocks += packet->metrics.queueLocks;
        metrics.commitLocks += packet->metrics.commitLocks;
        metrics.pipe += packet->metrics.pipe;
        metrics.failed += packet->metrics.failed;
        metrics.queueProbe += packet->metrics.queueProbe;
        metrics.commitProbe += packet->metrics.commitProbe;
        metrics.queuePipeLocks += packet->metrics.queuePipeLocks;
        metrics.commitPipeLocks += packet->metrics.commitPipeLocks;
        if (packet->operation) {
            ++kinds[static_cast<std::size_t>(packet->item.kind)];
        }
    }
    const auto prefix = [&](const Region* before) -> std::span<const ResolvedItem> {
        if (before == nullptr) return {};
        const auto end = std::find_if(before->resolved.begin(), before->resolved.end(), [&](const ResolvedItem& item) { return item.offset >= region.recognizedWords; });
        return std::span(before->resolved).first(static_cast<std::size_t>(end - before->resolved.begin()));
    };
    const auto matched = prefix(&region);
    const auto* previous = region.frame != 0 ? findRegion(region.frame - 1, region.queue, region.ordinal) : nullptr;
    const auto* older = region.frame > 1 ? findRegion(region.frame - 2, region.queue, region.ordinal) : nullptr;
    if (previous != nullptr && previous->sequence != region.sequence) previous = nullptr;
    if (older != nullptr && older->sequence != region.sequence) older = nullptr;
    const auto previousItems = prefix(previous);
    const auto olderItems = prefix(older);
    const auto first = previous != nullptr ? CompareResolved(previousItems, matched) : ResolvedDifference{false};
    const auto second = older != nullptr ? CompareResolved(olderItems, matched) : ResolvedDifference{false};
    const auto signature = ResolvedSignature(matched);
    static const bool verify = [] { const char* value = std::getenv("APS5_ARCHB_REPLAY_REGION_CENSUS_VERIFY"); return value != nullptr && value[0] == '1'; }();
    if (verify) {
        for (const auto pair : {std::pair{previous, first}, std::pair{older, second}}) {
            if (pair.first == nullptr) continue;
            count("verify_checks");
            if ((ResolvedSignature(pair.first == previous ? previousItems : olderItems) == signature) != pair.second.equal) count("verify_mismatch");
        }
    }
    const bool recognized = region.match != RegionMatch::New;
    const bool incomplete = !region.complete || (previous != nullptr && !previous->complete) || (older != nullptr && !older->complete);
    const std::string category = !recognized ? "unrecognized" : metrics.failed != 0 ? "failed" : incomplete ? "unclassified" : timingOnly() ? previous != nullptr && older != nullptr && !first.equal && !second.equal ? "different" : "unclassified" : previous != nullptr && first.equal ? "n1" : older != nullptr && second.equal ? "n2" : "different";
    if (!region.complete) count("incomplete_regions");
    if (timingOnly()) count("timing_only_regions");
    count(category + "_regions");
    count("outside_items", static_cast<double>(outside));
    if (!timingOnly() && region.complete && recognized && previous != nullptr && previous->complete && first.equal) count("equal_n1");
    if (!timingOnly() && region.complete && recognized && older != nullptr && older->complete && second.equal) count("equal_n2");
    for (std::size_t i = 0; i < kinds.size(); ++i) count(category + '_' + std::string(ResolvedKindNames[i]), static_cast<double>(kinds[i]));
    count(category + "_q_ns", static_cast<double>(metrics.queue));
    count(category + "_q_wait_ns", static_cast<double>(metrics.queueWait));
    if (region.queue == 0) {
        count(category + "_q0_ns", static_cast<double>(metrics.queue));
        count(category + "_q0_wait_ns", static_cast<double>(metrics.queueWait));
        count(category + "_q0_probe_ns", static_cast<double>(metrics.queueProbe));
        count(category + "_q0_locks", static_cast<double>(metrics.queueLocks));
    }
    count(category + "_commit_ns", static_cast<double>(metrics.commit));
    count(category + "_commit_wait_ns", static_cast<double>(metrics.commitWait));
    count(category + "_mutex_ns", static_cast<double>(metrics.mutex));
    count(category + "_gpu_ns", static_cast<double>(metrics.gpu));
    count(category + "_gpu_locks", static_cast<double>(metrics.queueLocks + metrics.commitLocks));
    count(category + "_pipe_items", static_cast<double>(metrics.pipe));
    count(category + "_pipe_locks", static_cast<double>(metrics.queuePipeLocks + metrics.commitPipeLocks));
    count(category + "_q_probe_ns", static_cast<double>(metrics.queueProbe));
    count(category + "_commit_probe_ns", static_cast<double>(metrics.commitProbe));
    for (const auto pair : {std::pair{previous, first}, std::pair{older, second}}) {
        if (!recognized || pair.first == nullptr || pair.second.equal) continue;
        const auto field = pair.second.field < ResolvedFieldNames.size() ? ResolvedFieldNames[pair.second.field] : "item_count_or_kind";
        count(std::string(pair.first == previous ? "first_n1_" : "first_n2_") + std::string(field));
    }
    std::string text;
    const bool output = census().output != nullptr;
    if (output) {
        text.reserve(region.resolved.size() * 1024);
        text = "{\"frame\":"; number(text, region.frame);
        text += ",\"steady_ns\":"; number(text, started);
        text += ",\"sequence\":"; number(text, region.sequence);
        text += ",\"complete\":"; text += timingOnly() || !region.complete ? "false" : "true";
        text += ",\"queue\":"; number(text, region.queue);
        text += ",\"region\":"; number(text, region.ordinal);
        text += ",\"class\":\"" + category + "\",\"words\":"; number(text, region.recognizedWords);
        text += ",\"signature\":"; number(text, signature);
        text += ",\"matched_items\":"; number(text, matched.size());
        text += ",\"equal_n1\":"; text += region.complete && previous != nullptr && previous->complete && first.equal ? "true" : "false";
        text += ",\"equal_n2\":"; text += region.complete && older != nullptr && older->complete && second.equal ? "true" : "false";
        text += ",\"first_n1\":"; appendDifference(text, first);
        text += ",\"first_n2\":"; appendDifference(text, second);
        text += ",\"metrics\":[";
        for (const auto value : {metrics.queue, metrics.queueWait, metrics.commit, metrics.commitWait, metrics.mutex, metrics.gpu, metrics.queueLocks, metrics.commitLocks, metrics.pipe, metrics.failed, metrics.queueProbe, metrics.commitProbe, metrics.queuePipeLocks, metrics.commitPipeLocks}) { number(text, value); text += ','; }
        text.back() = ']';
        text += ",\"items\":[";
        bool firstItem = true;
        for (const auto& item : region.resolved) {
            if (!std::exchange(firstItem, false)) text += ',';
            text += '['; number(text, item.offset); text += ','; number(text, static_cast<std::uint64_t>(item.kind));
            for (const auto* fields : {&item.prepared, &item.committed}) {
                text += ",{";
                bool firstField = true;
                for (std::size_t i = 0; i < fields->size(); ++i) {
                    if ((*fields)[i].empty()) continue;
                    if (!std::exchange(firstField, false)) text += ',';
                    text += '"'; number(text, i); text += "\":";
                    values(text, (*fields)[i]);
                }
                text += '}';
            }
            text += ']';
        }
        text += "],\"patches\":[";
    }
    bool firstPatch = true;
    for (auto& patch : region.patches) {
        if (timingOnly()) break;
        count("patch_positions");
        if (!patch.changed) count("patch_positions_unchanged_word");
        if (patch.changed) count("patch_words");
        if (patch.changed && patch.alternating) count("patch_words_alternating");
        if (patch.uses.empty()) {
            const auto after = std::upper_bound(region.packets.begin(), region.packets.end(), patch.offset, [](std::uint32_t offset, const std::shared_ptr<Packet>& packet) { return offset < packet->item.offset; });
            const auto* source = after != region.packets.begin() ? std::prev(after)->get() : nullptr;
            const bool executed = source != nullptr && patch.offset < source->item.offset + source->words.size();
            const auto header = executed ? source->words[0] : 0u;
            const auto opcode = (header >> 8u) & 0xffu;
            const std::string role = !executed ? "not_executed" : Pm4::IsTagMarker(source->words) ? "tag_marker_payload" : opcode == 0x10 && ((header >> 2u) & 0x3fu) == 0 ? "nop_payload" : "unmapped_pm4_" + std::to_string(opcode);
            patch.uses.push_back({role, 0, executed ? source->item.offset : patch.offset, executed ? patch.offset - source->item.offset : 0});
        }
        if (output) {
            if (!std::exchange(firstPatch, false)) text += ',';
            text += '['; number(text, patch.offset); text += ','; number(text, patch.value); text += ','; number(text, patch.alternating); text += ",[";
        }
        bool firstUse = true;
        for (auto& use : patch.uses) {
            const auto* prior = olderUse(region, patch, use);
            if (use.bytes != nullptr && prior != nullptr && prior->bytes != nullptr) {
                use.equal = *use.bytes == *prior->bytes ? 1 : 0;
                use.sameAddress = use.address == prior->address && use.bytes->size() == prior->bytes->size();
                if (use.equal == 1) count("input_equal_n2");
                if (use.fault == 1 && use.sameAddress && use.equal == 0) count("cpu_fault_same_range_different_bytes");
            }
            count("patch_use_" + use.role);
            if (use.bytes != nullptr) count("input_sampled");
            if (use.fault == 1) count("input_fault_unchanged");
            if (use.fault == 2) count("input_fault_untracked");
            if (output) {
                if (!std::exchange(firstUse, false)) text += ',';
                text += "[\"" + use.role + "\",";
                for (const auto value : {static_cast<std::uint64_t>(use.stage), static_cast<std::uint64_t>(use.consumer), static_cast<std::uint64_t>(use.input), use.address, use.bytes != nullptr ? static_cast<std::uint64_t>(use.bytes->size()) : 0, use.clock, use.bytes != nullptr ? byteHash(*use.bytes) : 0, static_cast<std::uint64_t>(use.equal + 1), static_cast<std::uint64_t>(use.fault + 1), static_cast<std::uint64_t>(use.sameAddress)}) { number(text, value); text += ','; }
                text.back() = ']';
            }
        }
        if (output) { text += "],"; number(text, patch.changed); text += ']'; }
    }
    if (output) {
        text += "]}\n";
        if (std::fwrite(text.data(), 1, text.size(), census().output) != text.size()) count("output_errors");
    }
    count("report_ns", static_cast<double>(now() - started));
    region.packets.clear();
}

void flush() {
    auto& state = census();
    for (auto& frame : state.frames) {
        if (frame.reported) continue;
        if (!frame.sealed) break;
        bool ready = true;
        for (const auto& [queue, regions] : frame.queues) {
            for (const auto& region : regions) if (!region->sealed || region->pending != 0) ready = false;
        }
        if (!ready) break;
        for (const auto& [queue, regions] : frame.queues) for (const auto& region : regions) report(*region);
        frame.reported = true;
        count("frames");
        if (state.output != nullptr) std::fflush(state.output);
    }
    while (state.frames.size() > 3 && state.frames[2].reported) state.frames.pop_front();
}

ResolvedKind packetKind(std::uint32_t opcode) {
    if (Pm4::DrawOpcode(opcode)) return ResolvedKind::Draw;
    if (opcode == 0x15 || opcode == 0x16) return ResolvedKind::Dispatch;
    if (opcode == 0x37 || opcode == 0x49) return ResolvedKind::Label;
    if (opcode == 0x40 || opcode == 0x50 || opcode == 0x83) return ResolvedKind::Copy;
    if (opcode == 0x3c || opcode == 0x93) return ResolvedKind::Wait;
    return ResolvedKind::Other;
}

void useAt(Region& region, std::uint32_t offset, Use use) {
    if (auto* patch = patchAt(region, offset)) {
        const auto found = std::find_if(patch->uses.begin(), patch->uses.end(), [&](const Use& candidate) { return candidate.role == use.role && candidate.stage == use.stage && candidate.consumer == use.consumer && candidate.input == use.input; });
        if (found == patch->uses.end()) patch->uses.push_back(std::move(use));
        else if (use.bytes != nullptr) *found = std::move(use);
    }
}

void sample(Region& region, std::uint32_t offset, Use use, std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t clock) {
    auto* patch = patchAt(region, offset);
    if (patch == nullptr || bytes.empty()) return;
    use.address = address;
    use.clock = clock;
    const auto prior = [&]() -> Use {
        std::lock_guard lock(census().mutex);
        const auto* found = olderUse(region, *patch, use);
        return found != nullptr ? *found : Use{};
    }();
    if (prior.bytes != nullptr) {
        use.fault = prior.clock == 0 || GuestMemory::FaultUntracked(address, bytes.size(), prior.clock) ? 2 : GuestMemory::FaultUnchangedSince(address, bytes.size(), prior.clock) ? 1 : 0;
    }
    use.bytes = std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());
    useAt(region, offset, std::move(use));
}

}

bool Enabled() { return ArchB::Switches::Enabled(ArchB::Feature::ReplayRegionCensus); }
bool Active() { return Enabled() && !timingOnly() && local().packet != nullptr; }

Observation::Observation() {
    if (Active()) start = local().observationDepth++ == 0 ? now() : 1;
}

Observation::~Observation() {
    if (start == 0) return;
    --local().observationDepth;
    if (start != 1) local().observationNs += now() - start;
}

std::shared_ptr<Region> Begin(std::uint32_t queue, std::span<const std::uint32_t> commands, const RegionReport& report, bool endsFrame) {
    if (!Enabled()) return {};
    auto& state = census();
    std::lock_guard lock(state.mutex);
    if (!state.opened) {
        Stage4::DiagnosticWaitEnd.store(&EndWait, std::memory_order_release);
        Stage4::DiagnosticWaitBegin.store(&BeginWait, std::memory_order_release);
        state.opened = true;
        if (const char* path = std::getenv("APS5_REPLAY_REGION_CENSUS_PATH")) {
            state.output = std::fopen(path, "wb");
            if (state.output == nullptr) count("output_errors");
        }
    }
    auto result = std::make_shared<Region>();
    auto& frame = state.frames.back();
    auto& regions = frame.queues[queue];
    result->frame = frame.number;
    result->sequence = ArchB::Switches::Sequence();
    result->queue = queue;
    result->ordinal = static_cast<std::uint32_t>(regions.size());
    result->match = report.match;
    result->recognizedWords = report.prefixWords;
    result->words.assign(commands.begin(), commands.end());
    const auto* previous = frame.number != 0 ? findRegion(frame.number - 1, queue, result->ordinal) : nullptr;
    const auto* older = frame.number > 1 ? findRegion(frame.number - 2, queue, result->ordinal) : nullptr;
    if (!timingOnly() && previous != nullptr) {
        for (std::uint32_t offset = 0; offset < result->recognizedWords && offset < previous->words.size(); ++offset) {
            if (commands[offset] != previous->words[offset]) result->patches.push_back({offset, commands[offset], older != nullptr && offset < older->words.size() && commands[offset] == older->words[offset], {}});
        }
    }
    for (const auto* before : {previous, older}) {
        if (timingOnly()) break;
        if (before == nullptr) continue;
        for (const auto& patch : before->patches) {
            if (patch.offset >= result->recognizedWords) continue;
            if (std::none_of(result->patches.begin(), result->patches.end(), [&](const Patch& candidate) { return candidate.offset == patch.offset; })) result->patches.push_back({patch.offset, commands[patch.offset], older != nullptr && patch.offset < older->words.size() && commands[patch.offset] == older->words[patch.offset], {}, false});
        }
    }
    std::sort(result->patches.begin(), result->patches.end(), [](const Patch& a, const Patch& b) { return a.offset < b.offset; });
    regions.push_back(result);
    if (endsFrame) {
        frame.sealed = true;
        Frame next;
        next.number = frame.number + 1;
        state.frames.push_back(std::move(next));
    }
    return result;
}

RegionScope::RegionScope(const std::shared_ptr<Region>& value) : region(value) {
    if (!Enabled() && region == nullptr) return;
    active = true;
    previous = std::exchange(local().region, region);
    previousPacket = std::exchange(local().packet, {});
}

RegionScope::~RegionScope() {
    if (!active) return;
    local().region = previous;
    local().packet = previousPacket;
    if (region == nullptr) return;
    std::lock_guard lock(census().mutex);
    region->sealed = true;
    flush();
}

PacketScope::PacketScope(std::size_t offset, std::span<const std::uint32_t> words, std::uint64_t tableClock) {
    if (!Enabled()) return;
    auto& state = local();
    if (state.region == nullptr || words.empty()) return;
    packet = std::make_shared<Packet>();
    packet->region = state.region;
    packet->item.offset = static_cast<std::uint32_t>(offset);
    const auto opcode = (words[0] >> 8u) & 0xffu;
    packet->item.kind = (words[0] >> 30u) == 3u ? packetKind(opcode) : ResolvedKind::Other;
    if (opcode == 0x50 && words.size() >= 7 && Pm4::DmaSource(words) == Pm4::DmaSourceData) packet->item.kind = ResolvedKind::Fill;
    packet->operation = packet->item.kind != ResolvedKind::Other;
    packet->tableClock = tableClock;
    packet->inputClock = GuestMemory::FaultClock();
    packet->words.assign(words.begin(), words.end());
    state.region->packets.push_back(packet);
    previous = std::exchange(state.packet, packet);
    previousMutexCensus = GuestMemory::SetGpuMutexCensusForThread(true);
    locks = state.locks;
    if (packet->operation) NoteWords(ResolvedField::Packet, words);
    if (const auto label = Pm4::DecodeLabelWrite(words)) {
        Note(ResolvedField::Arguments, {label->address, label->Bytes().size()});
        NoteBytes(ResolvedField::Arguments, label->Bytes());
        const auto low = opcode == 0x49 ? 3u : 2u;
        for (std::uint32_t i = 1; i < words.size(); ++i) useAt(*state.region, static_cast<std::uint32_t>(offset) + i, {i == low || i == low + 1 ? "label_address" : i > low + 1 ? "label_value" : "label_control", 0, static_cast<std::uint32_t>(offset), i, label->address});
    } else if (Pm4::DrawOpcode(opcode) || opcode == 0x15 || opcode == 0x16) {
        for (std::uint32_t i = 1; i < words.size(); ++i) useAt(*state.region, static_cast<std::uint32_t>(offset) + i, {Pm4::DrawOpcode(opcode) ? "draw_argument" : "dispatch_argument", 0, static_cast<std::uint32_t>(offset), i});
    }
    if (packet->item.kind == ResolvedKind::Wait) {
        const bool wide = opcode == 0x93;
        const auto bytes = Pm4::WaitAwaitedBytes(words);
        const auto address = (static_cast<std::uint64_t>(words[2]) & (bytes == 8 ? ~std::uint64_t{7} : ~std::uint64_t{3})) | (static_cast<std::uint64_t>(words[3]) << 32u);
        const auto reference = static_cast<std::uint64_t>(words[4]) | (wide ? static_cast<std::uint64_t>(words[5]) << 32u : 0);
        const auto mask = wide ? static_cast<std::uint64_t>(words[6]) | (static_cast<std::uint64_t>(words[7]) << 32u) : words[5];
        Note(ResolvedField::Arguments, {address, reference, mask, bytes, words[1] & 7u});
        for (std::uint32_t i = 1; i < words.size(); ++i) useAt(*state.region, static_cast<std::uint32_t>(offset) + i, {i == 2 || i == 3 ? "label_wait_address" : i == 4 || (wide && i == 5) ? "label_wait_value" : "label_wait_control", 0, static_cast<std::uint32_t>(offset), i, address});
    }
    if (const auto copy = Pm4::DecodeMemoryCopy(words)) {
        Note(ResolvedField::Arguments, {copy->source, copy->destination, copy->bytes});
        for (std::uint32_t i = 1; i < words.size(); ++i) {
            useAt(*state.region, static_cast<std::uint32_t>(offset) + i, {i == 2 || i == 3 ? "copy_source" : i == 4 || i == 5 ? "copy_destination" : "copy_control", 0, static_cast<std::uint32_t>(offset), i, i <= 3 ? copy->source : copy->destination});
            if (i == 2 || i == 3) packet->pointers.push_back({state.region, static_cast<std::uint32_t>(offset) + i, 0, i, copy->source, "copy_input"});
        }
    }
    if (opcode == 0x50 && words.size() >= 7 && Pm4::DmaSource(words) == Pm4::DmaSourceData) {
        const auto address = static_cast<std::uint64_t>(words[4]) | (static_cast<std::uint64_t>(words[5]) << 32u);
        Note(ResolvedField::Arguments, {address, Pm4::DmaBytes(words), words[2]});
        for (std::uint32_t i = 1; i < words.size(); ++i) useAt(*state.region, static_cast<std::uint32_t>(offset) + i, {i == 2 ? "fill_value" : i == 4 || i == 5 ? "fill_destination" : "fill_control", 0, static_cast<std::uint32_t>(offset), i, address});
    }
    if (opcode == 0x58) {
        packet->operation = true;
        NoteWords(ResolvedField::Packet, words);
        for (std::uint32_t i = 1; i < words.size(); ++i) useAt(*state.region, static_cast<std::uint32_t>(offset) + i, {i == 4 || i == 5 ? "acquire_range_base" : i == 2 || i == 3 ? "acquire_range_size" : "acquire_control", 0, static_cast<std::uint32_t>(offset), i});
    }
    if (words[0] == FlipPacketHeader || words[0] == RenderingWaitPacketHeader) {
        packet->operation = true;
        const bool flip = words[0] == FlipPacketHeader;
        NoteWords(ResolvedField::Packet, flip ? words : words.first(3));
        Note(ResolvedField::Arguments, {words[1], words[2]});
        if (flip) Note(ResolvedField::Arguments, {words[3], static_cast<std::uint64_t>(words[4]) | (static_cast<std::uint64_t>(words[5]) << 32u)});
        for (std::uint32_t i = 1; i < words.size(); ++i) useAt(*state.region, static_cast<std::uint32_t>(offset) + i, {i == 1 ? "video_output_handle" : i == 2 ? "video_output_index" : flip ? i == 3 ? "flip_mode" : "flip_argument" : "render_wait_padding", 0, static_cast<std::uint32_t>(offset), i});
    }
    if (opcode == 0x46 || opcode == 0x42) {
        packet->operation = true;
        NoteWords(ResolvedField::Packet, words);
        Note(ResolvedField::Arguments, {words[1]});
        const bool query = Pm4::SampleDump(words);
        if (query) Note(ResolvedField::Arguments, {Pm4::SampleDumpAddress(words), Pm4::SampleDumpBytes});
        for (std::uint32_t i = 1; i < words.size(); ++i) useAt(*state.region, static_cast<std::uint32_t>(offset) + i, {query && (i == 2 || i == 3) ? "query_destination" : "event_control", 0, static_cast<std::uint32_t>(offset), i});
    }
    start = now();
    observation = state.observationNs;
}

PacketScope::~PacketScope() {
    if (packet == nullptr) return;
    if (!timingOnly() && packet->item.kind == ResolvedKind::Wait) {
        const Observation observed;
        const auto& result = packet->item.prepared[static_cast<std::size_t>(ResolvedField::Outcome)];
        if (result.size() == 2) {
            const auto value = result.back();
            const auto bytes = Pm4::WaitAwaitedBytes(packet->words);
            const auto address = (static_cast<std::uint64_t>(packet->words[2]) & (bytes == 8 ? ~std::uint64_t{7} : ~std::uint64_t{3})) | (static_cast<std::uint64_t>(packet->words[3]) << 32u);
            if (const auto region = packet->region.lock()) for (const auto word : {2u, 3u}) sample(*region, packet->item.offset + word, {"label_compare_input", 0, packet->item.offset, word}, address, std::as_bytes(std::span(&value, 1)).first(bytes), packet->inputClock);
        }
    }
    const auto elapsed = now() - start;
    packet->metrics.queueProbe += local().observationNs - observation;
    packet->metrics.queue += elapsed > packet->metrics.queueProbe ? elapsed - packet->metrics.queueProbe : 0;
    packet->metrics.queueLocks += local().locks - locks;
    GuestMemory::SetGpuMutexCensusForThread(previousMutexCensus);
    local().packet = previous;
}

std::shared_ptr<Packet> CapturePipe() {
    if (!Enabled()) return {};
    const Observation observation;
    auto packet = local().packet;
    if (packet == nullptr) return {};
    const auto region = packet->region.lock();
    if (region == nullptr) return {};
    std::lock_guard lock(census().mutex);
    ++region->pending;
    ++packet->metrics.pipe;
    return packet;
}

CommitScope::CommitScope(std::shared_ptr<Packet> value) : packet(std::move(value)) {
    if (packet == nullptr) return;
    previous = std::exchange(local().packet, packet);
    previousCommitting = std::exchange(local().committing, true);
    local().commitObservation = local().observationNs;
}

CommitScope::~CommitScope() {
    if (packet == nullptr) return;
    local().packet = previous;
    local().committing = previousCommitting;
    if (const auto region = packet->region.lock()) {
        std::lock_guard lock(census().mutex);
        --region->pending;
        flush();
    }
}

void Note(ResolvedField field, std::span<const std::uint64_t> words) {
    if (!Enabled()) return;
    const Observation observation;
    auto& state = local();
    if (state.packet == nullptr) return;
    if (timingOnly() && (state.packet->item.kind != ResolvedKind::Wait || (field != ResolvedField::Arguments && field != ResolvedField::Outcome))) return;
    auto& target = (state.committing ? state.packet->item.committed : state.packet->item.prepared)[static_cast<std::size_t>(field)];
    if (state.packet->item.kind == ResolvedKind::Wait && field == ResolvedField::Outcome) target.clear();
    target.push_back(words.size());
    target.insert(target.end(), words.begin(), words.end());
}

void SetKind(ResolvedKind kind) {
    if (!Active()) return;
    local().packet->resolvedKind.store(kind, std::memory_order_relaxed);
}

void NoteWords(ResolvedField field, std::span<const std::uint32_t> words) {
    if (!Active()) return;
    const Observation observation;
    std::vector<std::uint64_t> values(words.begin(), words.end());
    Note(field, values);
}

void NoteBytes(ResolvedField field, std::span<const std::byte> bytes) {
    if (!Active()) return;
    const Observation observation;
    Note(field, {bytes.size(), byteHash(bytes)});
}

void UnobservedBindings() {
    if (!Active()) return;
    local().packet->complete.store(false, std::memory_order_relaxed);
    count("unobserved_bindings");
}

void NoteArguments(std::span<const std::uint64_t> values) { Note(ResolvedField::Arguments, values); }

void RegisterWrite(std::uint32_t opcode, std::uint32_t offset, std::uint32_t value) {
    if (!Enabled()) return;
    if (timingOnly()) return;
    const Observation observation;
    auto& state = local();
    if (state.packet == nullptr || state.committing || state.region == nullptr) return;
    auto& packet = *state.packet;
    const bool shader = opcode == 0x76 || opcode == 0x63;
    const auto bank = shader ? 0u : opcode == 0x69 || opcode == 0x9f ? 1u : 2u;
    const bool indirect = Pm4::IndirectRegisterOpcode(opcode);
    const auto source = indirect ? 1u : 2u + offset - (packet.words[1] & ~0x70000000u);
    state.origins[{bank, offset}] = {state.region, packet.item.offset + source, value};
    useAt(*state.region, packet.item.offset + source, {shader ? "shader_register" : bank == 1 ? "context_register" : "uconfig_register", 0, packet.item.offset, offset});
}

void RegisterReadClock(std::uint64_t clock) { if (local().packet != nullptr) local().packet->tableClock = clock; }

void Registers(std::span<const std::uint32_t> words, std::span<const std::uint32_t> pairs) {
    if (!Enabled()) return;
    if (timingOnly()) return;
    const Observation observation;
    auto& state = local();
    if (state.packet == nullptr || state.region == nullptr) return;
    const auto opcode = (words[0] >> 8u) & 0xffu;
    std::vector<std::uint32_t> data;
    if (words[3] == 0x80000000u) data.assign(pairs.begin(), pairs.end());
    else for (std::size_t i = 1; i < pairs.size(); i += 2) data.push_back(pairs[i]);
    const auto address = static_cast<std::uint64_t>(words[1]) | (static_cast<std::uint64_t>(words[2]) << 32u);
    for (const auto word : {1u, 2u}) sample(*state.region, state.packet->item.offset + word, {opcode == 0x63 ? "shader_register_table" : opcode == 0x9f ? "context_register_table" : "uconfig_register_table", 0, state.packet->item.offset, word}, address, std::as_bytes(std::span(data)), state.packet->tableClock);
}

void UserData(std::uint32_t stage, std::uint32_t base, std::span<const std::uint32_t> words) {
    if (!Enabled()) return;
    if (timingOnly()) return;
    const Observation observation;
    auto& state = local();
    if (state.packet == nullptr || state.region == nullptr || state.committing) return;
    Note(ResolvedField::UserData, {stage, base});
    NoteWords(ResolvedField::UserData, words);
    for (std::uint32_t i = 0; i < words.size(); ++i) {
        const auto found = state.origins.find({0u, base + i});
        if (found == state.origins.end() || found->second.value != words[i]) continue;
        const auto source = found->second.region.lock();
        if (source == nullptr || source->frame != state.region->frame) continue;
        useAt(*source, found->second.offset, {"user_data_value", stage, state.packet->item.offset, i});
        for (const auto low : {i, i == 0 ? i : i - 1}) {
            if (low + 1 >= words.size()) continue;
            const auto address = static_cast<std::uint64_t>(words[low]) | (static_cast<std::uint64_t>(words[low + 1]) << 32u);
            state.packet->pointers.push_back({source, found->second.offset, stage, low, address});
        }
    }
}

void Input(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t clock) {
    if (!Enabled()) return;
    if (timingOnly()) return;
    const Observation observation;
    auto& state = local();
    if (state.packet == nullptr || state.committing || bytes.empty()) return;
    if (clock == 0) clock = state.packet->inputClock;
    for (const auto& pointer : state.packet->pointers) {
        if (pointer.address < address || pointer.address - address >= bytes.size()) continue;
        if (const auto region = pointer.source.lock()) sample(*region, pointer.patch, {pointer.role, pointer.stage, state.packet->item.offset, pointer.input}, pointer.address, bytes.subspan(static_cast<std::size_t>(pointer.address - address)), clock);
    }
}

void DrawArguments(const Pm4::DrawParameters& draw) {
    if (!Active()) return;
    Note(ResolvedField::Arguments, {draw.indexAddress, draw.indexCount, draw.indexSize, draw.instanceCount, draw.flags, draw.indexed, draw.firstVertex, draw.firstInstance});
    if (!draw.indirect) return;
    const auto& args = *draw.indirect;
    Note(ResolvedField::Arguments, {args.arguments, args.opcode, args.recordBytes, args.stride, args.count, args.countIndirect, args.countAddress, args.baseVertexLocation, args.startInstanceLocation, args.drawIndexLocation, args.drawIndexEnabled, args.indxOffset, args.indexLimitKnown, static_cast<std::uint64_t>(args.vertexRule), static_cast<std::uint64_t>(args.instanceRule), args.vertexConstant, args.instanceConstant, static_cast<std::uint64_t>(args.baseVertexSgpr), static_cast<std::uint64_t>(args.startInstanceSgpr), static_cast<std::uint64_t>(args.drawIndexSgpr), args.instanceFromBase, args.vertexFromBase});
}

void CommitTime(double elapsed, std::uint64_t mutex, double gpu, bool failed) {
    if (!Enabled()) return;
    auto& state = local();
    if (state.packet == nullptr || !state.committing) return;
    const auto probe = state.observationNs - state.commitObservation;
    state.packet->metrics.commitProbe += probe;
    state.packet->metrics.commit += elapsed > probe ? static_cast<std::uint64_t>(elapsed) - probe : 0;
    state.packet->metrics.mutex += mutex;
    state.packet->metrics.gpu += static_cast<std::uint64_t>(gpu);
    if (failed) ++state.packet->metrics.failed;
}

void GpuLock() {
    if (!Enabled()) return;
    auto& state = local();
    ++state.locks;
    if (state.packet != nullptr && state.committing) ++state.packet->metrics.commitLocks;
}

void PipeLock(const std::shared_ptr<Packet>& packet) {
    if (!Enabled()) return;
    if (packet != nullptr) { ++packet->metrics.commitPipeLocks; return; }
    auto& state = local();
    if (state.packet != nullptr) ++(state.committing ? state.packet->metrics.commitPipeLocks : state.packet->metrics.queuePipeLocks);
}

std::uint64_t BeginWait() {
    if (!Enabled()) return 0;
    auto& state = local();
    if (state.packet == nullptr) return 0;
    return state.waitDepth++ == 0 ? now() : 1;
}

void EndWait(std::uint64_t start) {
    if (start == 0) return;
    auto& state = local();
    --state.waitDepth;
    if (start == 1 || state.packet == nullptr) return;
    (state.committing ? state.packet->metrics.commitWait : state.packet->metrics.queueWait) += now() - start;
}

}
