#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "ThreadOwned.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/RequestMemoryView.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace AgcDriver::DriverDetail {

namespace {

bool ProfileAhead() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

struct SpeculationCounts {
    std::uint64_t speculated = 0, adopted = 0, recaptured = 0;
    std::uint64_t taken = 0;
    std::array<std::uint64_t, 14> rejected{};
    double preparationMs = 0, validationMs = 0, queueMs = 0;
    double leadMs = 0, notReadyLeadMs = 0;
    std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
};

static_assert(std::is_trivially_destructible_v<SpeculationCounts>);

struct HelperCounts {
    std::mutex mutex;
    std::uint64_t completed = 0, prepared = 0, queueNs = 0, workNs = 0, preparationNs = 0;
};

HelperCounts& HelperStats() {
    static HelperCounts counts;
    return counts;
}

SpeculationCounts& SpeculationStats() {
    static thread_local SpeculationCounts counts;
    return counts;
}

void ReportSpeculation() {
    auto& stats = SpeculationStats();
    if (std::chrono::steady_clock::now() - stats.reported < std::chrono::seconds(10)) return;
    const double perAdopt = stats.adopted != 0 ? 1000.0 / static_cast<double>(stats.adopted) : 0.0;
    ProfilePrint_nid_no_patch("[dispatch-ahead] queue 0 (10 s): speculated %llu adopted %llu (recaptured %llu); rejected: not-ready %llu no-entry %llu no-invocation %llu footprint %llu state %llu key %llu epoch %llu layout %llu generation %llu pending %llu memory %llu cancelled %llu label %llu device %llu; estimated queue 0 preparation saved %.1f us/adopt (helper %.1f minus validation %.1f and scheduling %.1f us/adopt)\n",
        static_cast<unsigned long long>(stats.speculated), static_cast<unsigned long long>(stats.adopted), static_cast<unsigned long long>(stats.recaptured),
        static_cast<unsigned long long>(stats.rejected[0]), static_cast<unsigned long long>(stats.rejected[1]), static_cast<unsigned long long>(stats.rejected[2]), static_cast<unsigned long long>(stats.rejected[3]),
        static_cast<unsigned long long>(stats.rejected[4]), static_cast<unsigned long long>(stats.rejected[5]), static_cast<unsigned long long>(stats.rejected[6]), static_cast<unsigned long long>(stats.rejected[7]),
        static_cast<unsigned long long>(stats.rejected[8]), static_cast<unsigned long long>(stats.rejected[9]), static_cast<unsigned long long>(stats.rejected[10]), static_cast<unsigned long long>(stats.rejected[11]),
        static_cast<unsigned long long>(stats.rejected[12]), static_cast<unsigned long long>(stats.rejected[13]),
        (stats.preparationMs - stats.validationMs - stats.queueMs) * perAdopt, stats.preparationMs * perAdopt, stats.validationMs * perAdopt, stats.queueMs * perAdopt);
    auto& helpers = HelperStats();
    std::lock_guard lock(helpers.mutex);
    const double perJob = helpers.completed != 0 ? 0.001 / static_cast<double>(helpers.completed) : 0.0;
    const double perPrepared = helpers.prepared != 0 ? 0.001 / static_cast<double>(helpers.prepared) : 0.0;
    ProfilePrint_nid_no_patch("[dispatch-ahead] queue 0 timing (10 s): lead %.1f us/take (%llu takes), not-ready lead %.1f us/take; helpers %llu completed (%llu prepared), queue %.1f us/job, work %.1f us/job, preparation %.1f us/prepared\n",
        stats.taken != 0 ? stats.leadMs * 1000.0 / static_cast<double>(stats.taken) : 0.0, static_cast<unsigned long long>(stats.taken),
        stats.rejected[0] != 0 ? stats.notReadyLeadMs * 1000.0 / static_cast<double>(stats.rejected[0]) : 0.0,
        static_cast<unsigned long long>(helpers.completed), static_cast<unsigned long long>(helpers.prepared),
        static_cast<double>(helpers.queueNs) * perJob, static_cast<double>(helpers.workNs) * perJob, static_cast<double>(helpers.preparationNs) * perPrepared);
    helpers.completed = helpers.prepared = helpers.queueNs = helpers.workNs = helpers.preparationNs = 0;
    stats = {};
}

struct FrozenMemory {
    ShaderRecompiler::RequestMemoryView view;
    ShaderRecompiler::SrtRuntime runtime;

    FrozenMemory(std::span<const ShaderRecompiler::MemoryRegion> regions, std::span<const std::uint32_t> userData, std::uint64_t address) : view(regions), runtime(view.MakeRuntime(userData, address)) {}

    static bool Read(void* context, std::uint64_t address, std::uint32_t* value) {
        auto& self = *static_cast<FrozenMemory*>(context);
        if (!self.runtime.readMemory(self.runtime.userContext, address, value)) throw std::runtime_error("dispatch ahead read outside the copied footprint");
        return true;
    }

    static bool Accessible(void* context, std::uint64_t address, std::uint64_t bytes) {
        auto& self = *static_cast<FrozenMemory*>(context);
        if (!self.runtime.accessible(self.runtime.userContext, address, bytes)) throw std::runtime_error("dispatch ahead probe outside the copied footprint");
        return true;
    }

    ShaderRecompiler::SrtRuntime Runtime() {
        auto result = runtime;
        result.userContext = this;
        result.readMemory = &Read;
        result.readSpecializationMemory = &Read;
        result.accessible = &Accessible;
        return result;
    }
};

}

bool Driver::dispatchAheadEnabled() {
    static const bool enabled = [] {
        const auto* text = std::getenv("APS5_DISPATCH_AHEAD");
        return text != nullptr && std::string_view(text) == "1" && std::getenv("APS5_NO_DISPATCH_CACHE") == nullptr && std::getenv("APS5_DISPATCH_STAMP_VALIDATE") == nullptr && std::getenv("APS5_PROBE_DISPATCH") == nullptr;
    }();
    return enabled;
}

void Driver::SetAheadScheduleHook(void (*hook)()) {
    aheadScheduleHook.store(hook, std::memory_order_release);
}

std::map<std::size_t, std::shared_ptr<Driver::DispatchSpeculation>>& Driver::dispatchSpeculations() {
    static thread_local std::map<std::size_t, std::shared_ptr<DispatchSpeculation>>* slot = nullptr;
    return ShaderRecompiler::ThreadOwned(slot);
}

void Driver::rejectDispatchAhead(AheadRejection reason) {
    if (!ProfileAhead()) return;
    ++SpeculationStats().rejected[static_cast<std::size_t>(reason)];
    ReportSpeculation();
}

void Driver::adoptDispatchAhead(double preparationMs, double validationMs, bool cached) {
    if (!ProfileAhead()) return;
    auto& stats = SpeculationStats();
    ++stats.adopted;
    if (!cached) ++stats.recaptured;
    stats.preparationMs += preparationMs;
    stats.validationMs += validationMs;
    ReportSpeculation();
}

void Driver::noteDispatchAheadQueueMs(double ms) {
    SpeculationStats().queueMs += ms;
}

std::uint64_t Driver::dispatchKey(std::uint64_t address, std::uint32_t initiator, std::span<const std::uint32_t> userData, const ShaderRecompiler::ShaderComputeStageInfo& compute, const Registers& registers, const ShaderSnapshot* shader) {
    std::uint64_t key = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) { key = (key ^ value) * 0x100000001b3ull; };
    mix(address);
    mix(initiator & 0x8000u);
    for (const auto threads : compute.partialThreads) mix(threads);
    for (const auto word : userData) mix(word);
    static const bool keyHygiene = std::getenv("APS5_NO_DISPATCH_KEY_HYGIENE") == nullptr;
    if (keyHygiene) {
        for (const auto offset : {0x207u, 0x208u, 0x209u, 0x212u, 0x213u}) {
            const auto found = registers.find(offset);
            mix(found == registers.end() ? (1ull << 32u) : found->second);
        }
    } else {
        for (const auto& [offset, value] : registers) {
            mix(offset);
            mix(value);
        }
    }
    mix(reinterpret_cast<std::uintptr_t>(shader));
    return key;
}

void Driver::scheduleDispatchAhead(const Submission& submission, const QueueState& live, std::size_t from) {
    if (!dispatchAheadEnabled() || submission.queue != 0 || !dispatchSpeculations().empty() || submission.shaders == nullptr || DrawPipeline::Active()) return;
    const auto localDevice = device.Load();
    if (localDevice == nullptr) return;
    QueueState scratch = live;
    auto& pending = dispatchSpeculations();
    for (std::size_t cursor = from, packets = 0; cursor < submission.commands.size() && pending.size() < 8 && packets < 128; ++packets) {
        const auto header = submission.commands[cursor];
        if (Pm4::FillerPacket(header)) { ++cursor; continue; }
        const auto count = Pm4::PacketWords(header);
        if (count > submission.commands.size() - cursor || Pm4::Predicated(header)) break;
        const auto packet = std::span(submission.commands).subspan(cursor, count);
        const auto opcode = (header >> 8u) & 0xffu;
        if (opcode == 0x15 || opcode == 0x16) {
            if (opcode == 0x15 ? count != 5u : count != 3u && count != 4u) break;
            std::array<std::uint32_t, 5> direct{};
            std::uint64_t arguments = 0;
            try {
                if (opcode == 0x15) std::copy(packet.begin(), packet.end(), direct.begin());
                else {
                    arguments = Pm4::DispatchArgumentAddress(packet, scratch);
                    if (indirectArgumentsPath(packet, arguments) != IndirectGpu) break;
                    direct = {0xc0031500u, 0, 0, 0, packet.back()};
                }
            } catch (const std::exception&) { break; }
            if (dispatchAheadOutstanding.fetch_add(1, std::memory_order_acq_rel) >= 16) {
                dispatchAheadOutstanding.fetch_sub(1, std::memory_order_release);
                break;
            }
            auto work = std::make_shared<DispatchSpeculation>();
            work->result.epoch = GuestMemory::ThreadCollectEpoch();
            work->result.shader = scratch.shader;
            pending.emplace(cursor, work);
            if (ProfileAhead()) {
                ++SpeculationStats().speculated;
                work->queuedAt = std::chrono::steady_clock::now();
            }
            const bool submitted = SubmitDispatchPreparation([this, work, shaders = submission.shaders, localDevice, direct, arguments] {
                const auto started = ProfileAhead() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                try {
                    if (!work->cancelled.load(std::memory_order_acquire)) speculateDispatch(work, shaders, localDevice, direct, arguments);
                } catch (const std::exception&) {
                    work->rejection = AheadRejection::Footprint;
                }
                if (ProfileAhead()) {
                    const auto finished = std::chrono::steady_clock::now();
                    auto& stats = HelperStats();
                    std::lock_guard lock(stats.mutex);
                    stats.queueNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(started - work->queuedAt).count());
                    stats.workNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
                    if (work->result.compiledResult != nullptr) {
                        stats.preparationNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
                        ++stats.prepared;
                    }
                    ++stats.completed;
                }
                work->ready.store(true, std::memory_order_release);
                dispatchAheadOutstanding.fetch_sub(1, std::memory_order_release);
            });
            if (!submitted) {
                dispatchAheadOutstanding.fetch_sub(1, std::memory_order_release);
                work->ready.store(true, std::memory_order_release);
            }
            Pm4::NoteDispatchRegisters(packet, scratch);
        } else if (opcode == 0x76) {
            try { Pm4::Execute(packet, scratch); } catch (const std::exception&) { break; }
        } else break;
        cursor += count;
    }
    if (!pending.empty()) {
        if (const auto hook = aheadScheduleHook.load(std::memory_order_acquire)) hook();
    }
}

void Driver::takeDispatchAhead(const QueueState& live, std::size_t from) {
    if (!dispatchAheadEnabled()) return;
    auto& pending = dispatchSpeculations();
    const auto found = pending.find(from);
    if (found == pending.end()) return;
    const auto work = found->second;
    pending.erase(found);
    const bool ready = work->ready.load(std::memory_order_acquire);
    if (ProfileAhead()) {
        auto& stats = SpeculationStats();
        ++stats.taken;
        const auto leadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - work->queuedAt).count();
        stats.leadMs += leadMs;
        if (!ready) stats.notReadyLeadMs += leadMs;
    }
    if (!ready) {
        work->cancelled.store(true, std::memory_order_release);
        rejectDispatchAhead(AheadRejection::NotReady);
    } else if (work->result.compiledResult == nullptr) rejectDispatchAhead(work->rejection);
    else if (work->result.shader != live.shader) rejectDispatchAhead(AheadRejection::State);
    else resolvedAhead().insert_or_assign(from, std::move(work->result));
}

void Driver::speculateDispatch(const std::shared_ptr<DispatchSpeculation>& work, const std::shared_ptr<const ShaderRegistry>& shaders, const std::shared_ptr<VulkanDevice>& localDevice, std::array<std::uint32_t, 5> packet, std::uint64_t arguments) {
    const auto started = ProfileAhead() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto& result = work->result;
    const auto& registers = result.shader;
    const auto address = (static_cast<std::uint64_t>(readRegister(registers, 0x20c)) << 8u) | (static_cast<std::uint64_t>(readRegister(registers, 0x20d) & 0xffu) << 40u);
    auto shader = RegisteredProgram(*shaders, address, {0});
    if (shader == nullptr) return;
    const auto offset = static_cast<std::size_t>((address - shader->codeAddress) / sizeof(std::uint32_t));
    std::vector<std::uint32_t> userData;
    const auto count = (readRegister(registers, 0x213) >> 1u) & 0x1fu;
    for (std::uint32_t index = 0; index < count; ++index) userData.push_back(readUserData(registers, 0x240u + index));
    auto compute = Graphics::DecodeComputeStageInfo(registers, shader->header);
    if (arguments == 0 && (packet[4] & 0x22u) != 0) return;
    if (arguments != 0 && (packet[4] & 0x20u) != 0) return;
    const auto key = dispatchKey(address, packet[4], userData, compute, registers, shader.get());
    std::shared_ptr<DispatchEntry> entry;
    {
        std::lock_guard lock(dispatchCacheMutex);
        const auto found = dispatchCache.find(key);
        if (found != dispatchCache.end()) entry = found->second;
    }
    if (entry == nullptr || entry->variants.empty()) return;
    const auto& variant = entry->variants.front();
    if (variant->shader != shader) return;
    result.layout = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    result.forget = GuestMemory::ForgetSerial();
    if (variant->forgetSerial != result.forget) {
        work->rejection = AheadRejection::Layout;
        return;
    }
    result.ownedWords.resize(variant->words.size());
    result.captured.push_back({shader->codeAddress, std::as_bytes(std::span(shader->code))});
    if (!shader->header.empty()) result.captured.push_back({shader->headerAddress, shader->header});
    std::size_t at = 0;
    std::vector<std::shared_ptr<const GuestAllocations::Range>> leases;
    work->rejection = AheadRejection::Footprint;
    for (const auto& [begin, end] : variant->runs) {
        for (auto address = begin; address < end;) {
            auto lease = GuestAllocations::GuestAllocationsLookup_nid_postfix(static_cast<std::uintptr_t>(address), false);
            if (lease == nullptr || !lease->readable || address < lease->address || address - lease->address >= lease->bytes) return;
            address = std::min(end, lease->address + lease->bytes);
            leases.push_back(std::move(lease));
        }
        const auto bytes = static_cast<std::size_t>(end - begin);
        const auto pendingWrites = Graphics::Recorder::PendingWriteSnapshot();
        if (Graphics::Recorder::SnapshotOverlaps(pendingWrites.get(), begin, bytes)) {
            work->rejection = AheadRejection::Pending;
            return;
        }
        const auto generation = GuestMemory::CollectWritesUncached(begin, bytes);
        const auto destination = std::as_writable_bytes(std::span(result.ownedWords)).subspan(at, bytes);
        if (GuestMemory::CopyMapped(begin, destination) != GuestMemory::Compare::Equal) return;
        result.stamps.push_back({begin, generation, bytes});
        result.captured.push_back({begin, destination});
        at += bytes;
    }
    leases.clear();
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiled;
    if (result.ownedWords == variant->words) {
        compiled = variant->compiled;
        result.keepVariant = variant;
        result.cached = true;
        result.validated = true;
    } else {
        work->rejection = AheadRejection::NoInvocation;
        ShaderRecompiler::RecompileRequest request{
            {ShaderRecompiler::ShaderStage::Compute, address, std::span(shader->code).subspan(offset), shader->headerAddress, shader->header},
            {(packet[4] & 0x8000u) != 0 ? 32u : 64u, 0, userData, compute, std::nullopt, std::nullopt, result.captured, DispatchFloatMode(*shader, registers)},
            localDevice->ComputeTarget((packet[4] & 0x8000u) != 0 ? 32u : 64u), {0, 0, 0, 128}
        };
        std::optional<ShaderRecompiler::PreparedShaderInvocation> invocation;
        {
            std::unique_lock lock(shader->prepared->mutex, std::try_to_lock);
            if (!lock.owns_lock() || shader->prepared->pending || shader->prepared->deferredPreparing) return;
            request.context.convertedSamples = std::ranges::find(shader->prepared->convertedSamples, offset) != shader->prepared->convertedSamples.end();
            for (const auto& prepared : shader->prepared->entries) {
                if (prepared.codeOffset != offset || !std::ranges::equal(ShaderRecompiler::GetPreparedCode(*prepared.handle), request.shader.code)) continue;
                invocation = ShaderRecompiler::PreparedShaderInvocation::TryCreate(request, prepared.handle);
                if (invocation) break;
            }
        }
        if (!invocation || work->cancelled.load(std::memory_order_acquire)) return;
        FrozenMemory memory(result.captured, userData, address);
        work->rejection = AheadRejection::Footprint;
        result.capture = invocation->Capture(memory.Runtime());
        compiled = invocation->Materialize(*result.capture);
    }
    if (work->cancelled.load(std::memory_order_acquire)) return;
    result.address = address;
    result.key = key;
    result.packet = packet;
    result.indirectArguments = arguments;
    result.registeredShader = std::move(shader);
    result.preparedDevice = localDevice;
    result.speculative = true;
    if (ProfileAhead()) {
        result.resolvedAt = std::chrono::steady_clock::now();
        result.preparationMs = std::chrono::duration<double, std::milli>(result.resolvedAt - started).count();
    }
    result.compiledResult = std::move(compiled);
}

std::optional<Driver::AheadRejection> Driver::validateDispatchAhead(const ResolvedDispatch& ahead) {
    if (ahead.epoch != GuestMemory::ThreadCollectEpoch()) return AheadRejection::Epoch;
    if (ahead.layout != GuestAllocations::GuestAllocationsGeneration_nid_postfix() || ahead.forget != GuestMemory::ForgetSerial()) return AheadRejection::Layout;
    for (const auto& stamp : ahead.stamps) {
        if (QueryPendingSpan(stamp.address, stamp.bytes, {}) != ShaderMemory::PendingWrite::None) return AheadRejection::Pending;
        GuestMemory::CollectWritesUncached(stamp.address, stamp.bytes);
        if (stamp.generation != 0 && !GuestMemory::UnchangedSince(stamp.address, stamp.bytes, stamp.generation)) return AheadRejection::Generation;
    }
    return std::nullopt;
}

std::map<std::size_t, Driver::ResolvedDispatch>& Driver::resolvedAhead() {
    static thread_local std::map<std::size_t, ResolvedDispatch>* resolved = nullptr;
    return ShaderRecompiler::ThreadOwned(resolved);
}

Driver::GroupCaptureStats& Driver::groupCaptureStats() {
    static thread_local GroupCaptureStats stats;
    return stats;
}

std::size_t& Driver::currentPacketOffset() {
    static thread_local std::size_t offset = 0;
    return offset;
}

bool& Driver::resolvingAhead() {
    static thread_local bool resolving = false;
    return resolving;
}

bool& Driver::gateOpened() {
    static thread_local bool opened = false;
    return opened;
}

std::uint64_t Driver::DispatchesAdoptedAhead() const {
    return dispatchesAdoptedAhead.load(std::memory_order_relaxed);
}

std::uint64_t Driver::DispatchesRejectedAhead() const {
    return dispatchesRejectedAhead.load(std::memory_order_relaxed);
}

void Driver::SetAheadAdoptHook(void (*hook)()) {
    aheadAdoptHook.store(hook, std::memory_order_release);
}

namespace {

std::size_t& PreviewedUntil() {
    static thread_local std::size_t until = 0;
    return until;
}

}

void Driver::clearResolvedAhead() {
    if (dispatchAheadEnabled()) {
        for (const auto& [offset, work] : dispatchSpeculations()) {
            work->cancelled.store(true, std::memory_order_release);
            rejectDispatchAhead(AheadRejection::Cancelled);
        }
        dispatchSpeculations().clear();
    }
    PreviewedUntil() = 0;
    auto& resolved = resolvedAhead();
    if (resolved.empty()) return;
    groupCaptureStats().stale += resolved.size();
    resolved.clear();
}

namespace {

struct RunLabel {
    std::uint64_t address;
    std::size_t size;
    std::uint64_t value;
};

bool InCode(const ShaderRecompiler::MemoryRegion& region, const std::shared_ptr<const ShaderSnapshot>& code) {
    if (code == nullptr) return false;
    if (region.guestAddress >= code->codeAddress && region.guestAddress < code->codeAddress + code->code.size() * sizeof(std::uint32_t)) return true;
    return !code->header.empty() && region.guestAddress >= code->headerAddress && region.guestAddress < code->headerAddress + code->header.size();
}

bool OverlapsRange(std::span<const ShaderRecompiler::MemoryRegion> regions, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, const std::shared_ptr<const ShaderSnapshot>& code) {
    for (const auto& region : regions) {
        if (InCode(region, code)) continue;
        for (const auto& [begin, end] : ranges) {
            if (begin < region.guestAddress + region.bytes.size() && region.guestAddress < end) return true;
        }
    }
    return false;
}

bool WrittenByRun(std::span<const std::uint32_t> wait, const std::vector<RunLabel>& labels) {
    const std::uint64_t awaited = wait[2] | (static_cast<std::uint64_t>(wait[3]) << 32u);
    const auto bytes = Pm4::WaitAwaitedBytes(wait);
    for (auto it = labels.rbegin(); it != labels.rend(); ++it) {
        if (it->address != awaited || it->size < bytes) continue;
        return Pm4::WaitComparesValue(wait, bytes == 8 ? it->value : (it->value & 0xffffffffu));
    }
    return false;
}

void NoteLabel(std::vector<RunLabel>& labels, std::uint64_t address, std::span<const std::byte> bytes) {
    if (bytes.size() != 4 && bytes.size() != 8) return;
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data(), bytes.size());
    labels.push_back({address, bytes.size(), value});
}

bool WritesThroughPointers(const ShaderRecompiler::RecompileResult& compiled) {
    return std::any_of(compiled.bindings.begin(), compiled.bindings.end(), [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable; });
}

bool OverlapsLabel(std::span<const ShaderRecompiler::MemoryRegion> regions, const std::vector<RunLabel>& labels) {
    for (const auto& label : labels) {
        for (const auto& region : regions) {
            if (label.address < region.guestAddress + region.bytes.size() && region.guestAddress < label.address + label.size) return true;
        }
    }
    return false;
}

}

void Driver::resolveGroupAhead(const Submission& submission, const QueueState& live, std::size_t from) {
    static const bool allQueues = std::getenv("APS5_GROUP_CAPTURE_ALL_QUEUES") != nullptr;
    if (!allQueues && submission.queue == 0) return;
    auto& resolved = resolvedAhead();
    const auto localDevice = device.Load();
    if (resolved.contains(from) || localDevice == nullptr) return;
    if (submission.queue == 0 && DrawPipeline::Depth() != 0 && DrawPipeline::Queue0().Busy()) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Packet, (submission.commands[from] >> 8u) & 0xffu);
    auto& stats = groupCaptureStats();
    const auto begin = std::chrono::steady_clock::now();
    ++stats.runs;
    QueueState scratch = live;
    std::vector<RunLabel> labels;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> writes;
    bool unknownWrites = false;
    for (const auto& label : deferredLabels().labels) NoteLabel(labels, label.address, std::span<const std::byte>(label.bytes).first(label.size));
    struct Resolving {
        std::size_t offset;
        Resolving() : offset(currentPacketOffset()) { resolvingAhead() = true; }
        ~Resolving() {
            resolvingAhead() = false;
            currentPacketOffset() = offset;
        }
    } resolving;
    const auto& commands = submission.commands;
    if (submission.shaders != nullptr && from >= PreviewedUntil()) {
        bool hasFurtherDispatch = false;
        std::size_t limit = from < commands.size() ? from + Pm4::PacketWords(commands[from]) : commands.size();
        while (limit < commands.size()) {
            const auto header = commands[limit];
            if (Pm4::FillerPacket(header)) {
                ++limit;
                continue;
            }
            const auto count = Pm4::PacketWords(header);
            if (limit + count > commands.size()) break;
            const auto opcode = (header >> 8u) & 0xffu;
            if (header == FlipPacketHeader || header == RenderingWaitPacketHeader || opcode == 0x22 || opcode == 0x3f || opcode == 0x3c || opcode == 0x93) break;
            if (opcode == 0x15 || opcode == 0x16) hasFurtherDispatch = true;
            limit += count;
        }
        PreviewedUntil() = limit;
        if (hasFurtherDispatch) {
            static thread_local QueueState* previewSlot = nullptr;
            auto& preview = ShaderRecompiler::ThreadOwned(previewSlot);
            preview.shader = live.shader;
            preview.predication = live.predication;
            std::vector<PreparedShaders*> pendingShaders;
            std::vector<PreparedShaders*> submittedShaders;
            std::size_t scan = from;
            while (scan < limit) {
                const auto header = commands[scan];
                if (Pm4::FillerPacket(header)) {
                    ++scan;
                    continue;
                }
                const auto count = Pm4::PacketWords(header);
                if (scan + count > limit) break;
                const auto packet = std::span(commands).subspan(scan, count);
                const auto opcode = (header >> 8u) & 0xffu;
                const auto next = scan + count;
                if (opcode == 0x15 || opcode == 0x16) {
                    if (count < (opcode == 0x15 ? 5u : 3u)) break;
                    if (scan > from) {
                        const auto low = preview.shader.find(0x20c);
                        const auto high = preview.shader.find(0x20d);
                        if (low != preview.shader.end() && high != preview.shader.end()) {
                            const auto address = (static_cast<std::uint64_t>(low->second) << 8u) | (static_cast<std::uint64_t>(high->second & 0xffu) << 40u);
                            if (auto registered = RegisteredProgram(*submission.shaders, address, {0}); registered != nullptr && registered->type == 0) {
                                bool isPending = false;
                                bool isEmpty = false;
                                {
                                    std::lock_guard lock(registered->prepared->mutex);
                                    isPending = registered->prepared->pending;
                                    isEmpty = registered->prepared->entries.empty();
                                }
                                if (isPending) {
                                    if (std::ranges::find(pendingShaders, registered->prepared.get()) == pendingShaders.end()) {
                                        pendingShaders.push_back(registered->prepared.get());
                                    }
                                } else if (isEmpty && std::ranges::find(submittedShaders, registered->prepared.get()) == submittedShaders.end()) {
                                    const auto codeOffset = static_cast<std::size_t>((address - registered->codeAddress) / sizeof(std::uint32_t));
                                    if (codeOffset < registered->code.size()) {
                                        try {
                                            auto compute = Graphics::DecodeComputeStageInfo(preview.shader, registered->header);
                                            submittedShaders.push_back(registered->prepared.get());
                                            const auto rsrc2 = preview.shader.find(0x213);
                                            const auto userCount = rsrc2 != preview.shader.end() ? ((rsrc2->second >> 1u) & 0x1fu) : 0u;
                                            std::vector<std::uint32_t> userData(userCount, 0u);
                                            for (std::uint32_t i = 0; i < userCount; ++i) {
                                                if (const auto reg = preview.shader.find(0x240u + i); reg != preview.shader.end()) userData[i] = reg->second;
                                            }
                                            const auto wave = (packet.back() & 0x8000u) != 0 ? 32u : 64u;
                                            SubmitRegistrationPreparation([registered, localDevice, address, codeOffset, wave, compute, userData = std::move(userData), state = preview.shader]() mutable {
                                                std::vector<ShaderRecompiler::MemoryRegion> memory{{registered->codeAddress, std::as_bytes(std::span(registered->code))}};
                                                if (!registered->header.empty()) memory.push_back({registered->headerAddress, registered->header});
                                                ShaderRecompiler::RecompileRequest request{
                                                    {ShaderRecompiler::ShaderStage::Compute, address, std::span(registered->code).subspan(codeOffset), registered->headerAddress, registered->header},
                                                    {wave, 0, userData, compute, std::nullopt, std::nullopt, memory, DispatchFloatMode(*registered, state)},
                                                    localDevice->ComputeTarget(wave),
                                                    {0, 0, 0, 128}
                                                };
                                                try { static_cast<void>(InvocationFor(*registered, codeOffset, request)); } catch (const std::exception&) {}
                                            }, registered->prepared.get());
                                        } catch (const std::exception&) {}
                                    }
                                }
                            }
                        }
                    }
                    Pm4::NoteDispatchRegisters(packet, preview);
                } else if (Pm4::IndirectRegisterOpcode(opcode)) {
                    const auto found = submission.registerLists.find(scan);
                    if (count != 5 || found == submission.registerLists.end() || found->second.size() != static_cast<std::size_t>(packet[4]) * 2 || !Pm4::IndirectRegistersValid(found->second)) break;
                    Pm4::ExecuteIndirectRegisters(packet, found->second, preview);
                } else if (!Pm4::AccessesMemory(header) && opcode != 0x3c && opcode != 0x93 && opcode != 0x37 && opcode != 0x42 && opcode != 0x46 && opcode != 0x49 && opcode != 0x58) {
                    try {
                        Pm4::Execute(packet, preview);
                    } catch (const std::exception&) {
                        break;
                    }
                }
                scan = next;
            }
            for (auto it = pendingShaders.rbegin(); it != pendingShaders.rend(); ++it) {
                PrioritizeRegistrationPreparation(**it);
            }
        }
    }
    for (std::size_t cursor = from; cursor < commands.size();) {
        const auto header = commands[cursor];
        if (Pm4::FillerPacket(header)) {
            ++cursor;
            continue;
        }
        const auto count = Pm4::PacketWords(header);
        if (cursor + count > commands.size()) break;
        const auto packet = std::span(commands).subspan(cursor, count);
        const auto opcode = (header >> 8u) & 0xffu;
        const auto next = cursor + count;
        if (header == FlipPacketHeader || header == RenderingWaitPacketHeader || opcode == 0x22 || (Pm4::Predicated(header) && scratch.predication.operation != 0)) break;
        if (opcode == 0x3c || opcode == 0x93) {
            if (!Pm4::WaitSatisfiedUnchecked(packet) && !WrittenByRun(packet, labels)) break;
        } else if (opcode == 0x15 || opcode == 0x16) {
            if (count < (opcode == 0x15 ? 5u : 3u) || resolved.contains(cursor)) break;
            if (opcode == 0x16) {
                std::uint64_t arguments = 0;
                try {
                    arguments = Pm4::DispatchArgumentAddress(packet, scratch);
                } catch (const std::exception&) {
                    break;
                }
                const auto argumentsEnd = arguments + 3u * sizeof(std::uint32_t);
                const bool written = std::any_of(writes.begin(), writes.end(), [&](const auto& range) { return range.first < argumentsEnd && arguments < range.second; })
                    || std::any_of(labels.begin(), labels.end(), [&](const RunLabel& label) { return label.address < argumentsEnd && arguments < label.address + label.size; });
                if ((unknownWrites || written) && indirectArgumentsPath(packet, arguments) != IndirectGpu) {
                    ++stats.writtenBefore;
                    break;
                }
            }
            currentPacketOffset() = cursor;
            try {
                if (opcode == 0x15) dispatch(scratch, packet, submission);
                else dispatchIndirect(scratch, packet, submission);
                Pm4::NoteDispatchRegisters(packet, scratch);
            } catch (const std::exception&) {
                ++stats.failures;
                break;
            }
            if (const auto found = resolved.find(cursor); found != resolved.end()) {
                if (OverlapsLabel(found->second.captured, labels)) {
                    resolved.erase(found);
                    ++stats.labelOverlaps;
                    break;
                }
                if (OverlapsRange(found->second.captured, writes, found->second.registeredShader)) {
                    resolved.erase(found);
                    ++stats.writtenBefore;
                    break;
                }
                if (found->second.compiledResult != nullptr) appendWrittenRanges(*found->second.compiledResult, writes);
                if (found->second.compiledResult == nullptr || WritesThroughPointers(*found->second.compiledResult)) unknownWrites = true;
            } else {
                unknownWrites = true;
            }
        } else if (opcode == 0x37 || opcode == 0x49) {
            const auto label = Pm4::DecodeLabelWrite(packet);
            if (!label) break;
            NoteLabel(labels, label->address, label->Bytes());
            writes.emplace_back(label->address, label->address + label->Bytes().size());
        } else if (opcode != 0x3f && opcode != 0x42 && opcode != 0x46 && opcode != 0x58) {
            if (Pm4::AccessesMemory(header)) break;
            try {
                Pm4::Execute(packet, scratch);
            } catch (const std::exception&) {
                break;
            }
        }
        cursor = next;
    }
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    stats.resolveMs += ms;
    stats.resolveMaxMs = std::max(stats.resolveMaxMs, ms);
    reportGroupCapture(submission.queue);
}

void Driver::reportGroupCapture(std::uint32_t queue) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    auto& stats = groupCaptureStats();
    const auto now = std::chrono::steady_clock::now();
    if (now - stats.reported < std::chrono::seconds(10)) return;
    AgcDriver::ProfilePrint_nid_no_patch("[groupcap] queue 0x%x (10 s): %llu runs resolved %llu dispatches (%llu cache hits) in %.1f ms (max %.2f ms); %llu adopted %.2f ms after the resolve on average, max %.2f ms; mismatches: address %llu key %llu memory %llu; %llu resolve failures, %llu stale, %llu label overlaps, %llu read an earlier dispatch's output, %llu rejected by the device, %llu dropped because the argument re-read differed\n",
                                         queue, static_cast<unsigned long long>(stats.runs), static_cast<unsigned long long>(stats.resolved), static_cast<unsigned long long>(stats.resolvedHits), stats.resolveMs, stats.resolveMaxMs,
                                         static_cast<unsigned long long>(stats.adopted), stats.adopted != 0 ? stats.ageMs / static_cast<double>(stats.adopted) : 0.0, stats.ageMaxMs, static_cast<unsigned long long>(stats.addressMismatches), static_cast<unsigned long long>(stats.keyMismatches), static_cast<unsigned long long>(stats.memoryMismatches),
                                         static_cast<unsigned long long>(stats.failures), static_cast<unsigned long long>(stats.stale), static_cast<unsigned long long>(stats.labelOverlaps), static_cast<unsigned long long>(stats.writtenBefore), static_cast<unsigned long long>(stats.deviceRejects), static_cast<unsigned long long>(stats.argumentMismatches));
    stats = GroupCaptureStats{};
    stats.reported = now;
}

}
