#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/CheapClock.hpp"
#include "prx/libSceAgcDriver/Execution/include/HostAllocator.hpp"
#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/DrawWriteRanges.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawProgramTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/GatherShadow.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/LiveSequence.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/RepeatCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRegisterKey.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include <atomic>
#include <array>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <unordered_set>
#include <cstdlib>
#include <map>
#include <cstring>
#include <unordered_map>
#include <exception>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <typeinfo>

namespace AgcDriver::DriverDetail {

namespace {

bool TraceShaderCorrelation() {
    static const bool enabled = std::getenv("APS5_TRACE_SHADER_CORRELATION") != nullptr;
    return enabled && AgcDriver::CaptureTrace::Enabled();
}

std::atomic<std::uint64_t>& NextDrawTraceToken() {
    static std::atomic<std::uint64_t> next{0};
    return next;
}

std::string ExceptionReason(const std::exception_ptr& failure) {
    try {
        if (failure != nullptr) std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
    }
    return "unknown exception";
}

const char* ExceptionType(const std::exception_ptr& failure) {
    try {
        if (failure != nullptr) std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        return typeid(error).name();
    } catch (...) {
    }
    return "unknown";
}

void TraceMaterializationFailure(std::uint64_t token, const Submission& submission, std::size_t packetOffset, const char* phase, std::size_t completedSubdraws, const std::exception_ptr& failure) {
    if (token == 0) return;
    try {
        const auto reason = ExceptionReason(failure);
        AgcDriver::CaptureTrace::Log("draw-materialization-failed token=%llu submission=%llu queue=%x offset=%zu phase=%s exception=%s reason=%.256s completedSubdraws=%zu",
                static_cast<unsigned long long>(token), static_cast<unsigned long long>(submission.serial), submission.queue, packetOffset, phase, ExceptionType(failure), reason.c_str(), completedSubdraws);
    } catch (...) {

    }
}

void TraceSubmissionFailure(std::uint64_t token, const Submission& submission, std::size_t packetOffset, std::size_t completedSubdraws, const std::exception_ptr& failure) {
    if (token == 0) return;
    try {
        const auto reason = ExceptionReason(failure);
        AgcDriver::CaptureTrace::Log("draw-submission-failed token=%llu submission=%llu queue=%x offset=%zu exception=%s reason=%.256s completedSubdraws=%zu",
                static_cast<unsigned long long>(token), static_cast<unsigned long long>(submission.serial), submission.queue, packetOffset, ExceptionType(failure), reason.c_str(), completedSubdraws);
    } catch (...) {

    }
}

void TraceDrawAttemptFailure(std::uint64_t token, const char* phase, const char* outcome, std::uint64_t submission, std::uint32_t queue, std::size_t packetOffset, std::string_view reason, std::string_view exceptionType = "none") {
    if (token == 0) return;
        AgcDriver::CaptureTrace::LogBestEffort("draw-attempt-failed token=%llu phase=%s outcome=%s submission=%llu queue=%x offset=%zu exception=%s reason=%.256s",
            static_cast<unsigned long long>(token), phase, outcome, static_cast<unsigned long long>(submission), queue, packetOffset, exceptionType.data(), reason.data());
}

void TraceDrawAttemptException(std::uint64_t token, const char* phase, std::uint64_t submission, std::uint32_t queue, std::size_t packetOffset, const std::exception_ptr& failure) noexcept {
    try {
        TraceDrawAttemptFailure(token, phase, "exception", submission, queue, packetOffset, ExceptionReason(failure), ExceptionType(failure));
    } catch (...) {

    }
}

const char* DrawVerdictName(DrawVerdict verdict) {
    switch (verdict) {
        case DrawVerdict::Drawn: return "Drawn";
        case DrawVerdict::Nothing: return "Nothing";
        case DrawVerdict::Rejected: return "Rejected";
    }
    return "Unknown";
}

std::string_view TrimmedReason(std::string_view reason) {
    if (reason.size() > 200) reason = reason.substr(0, 200);
    return reason;
    }

}


struct GatherLiveEntry {
    struct Register {
        Graphics::RegisterBank bank;
        std::uint32_t offset;
        std::uint32_t value;
    };
    std::vector<Register> registers;
    std::shared_ptr<const DrawDecode> decode;
    std::vector<std::shared_ptr<DispatchVariant>> variants;
    std::shared_ptr<GatherCommitTemplate> link;
    Pm4::DrawParameters packet;
    Pm4::DrawParameters derived;
    GatherOptions options;
    bool derivedReady = false;
    std::uint64_t allocationGeneration = 0;
};

struct GatherLiveDraw {
    std::shared_ptr<GatherLiveEntry> entry;
    std::shared_ptr<GatherDrawPlan> plan;
    Pm4::DrawParameters parameters;
    std::vector<Graphics::CompiledShader> stages;
    bool verify = false;
};

struct DrawTail {
    using GpuLock = std::unique_lock<std::remove_reference_t<decltype(GuestMemory::GpuMutex())>>;
    std::weak_ptr<DrawTail> self;
    ArchB::Recorder::Key recorderKey;
    bool profile = false;
    DrawCacheOutcome cacheOutcome = CacheNotLookedUp;
    std::array<double, DrawDriverPhaseCount> phaseMs{};
    std::uint64_t captures = 0;
    std::chrono::steady_clock::time_point phaseLap{};
    DrawPhaseTiming phaseTiming{false, phaseMs, phaseLap};
    Pm4::DrawParameters drawParameters{};
    Pm4::DrawParameters resolvedParameters{};
    bool pipelined = false;
    bool traceIndirect = false;
    std::uint64_t dumpTarget = 0;
    std::uint64_t dumpSlot1 = 0;
    std::shared_ptr<VulkanDevice> localDevice;
    bool registerKey = false;
    bool ubPointerFree = false;
    bool ubSkipCpuIndirect = false;
    // APS5_ARCHB_PIPE_INDIRECT_GPU_ARGS, sampled once per draw: the enqueue did not drain for arguments
    // queued items write; CPU reads of them drain first (drainBeforeArgumentRead).
    bool indirectGpuArgs = false;
    // APS5_ARCHB_PIPE_NARROW_DRAW_WRITES, sampled once per draw (the finisher takes it from the tail).
    bool narrowDrawWrites = false;
    // APS5_ARCHB_PIPE_NARROW_SLICED_DEPTH, sampled once per draw.
    bool narrowSlicedDepth = false;
    bool feedbackMask = false;
    bool pipelineStateReuse = false;
    bool memoVirtualQuery = false;
    bool imageTableSnapshotReuse = false;
    bool imageTableBitScanKeys = false;
    bool bufferBases = false;
    bool nestedRelocation = false;
    bool allocChurn = false;
    bool partialSnapshot = false;
    bool hostSlabAlloc = false;
    bool cheapTimers = false;
    bool gatherShadow = false;
    std::shared_ptr<GatherDrawPlan> gatherPlan;
    std::shared_ptr<GatherLiveDraw> live;
    std::shared_ptr<GatherLiveEntry> liveEntry;
    std::uint64_t liveKey = 0;
    LiveSequence<GatherLiveEntry>::Position livePosition;
    std::uint64_t liveUnmatchedKey = 0;
    Pm4::DrawParameters livePacket;
    bool liveEnabled = false;
    bool liveVerify = false;
    GatherOptions liveOptions;
    std::uint64_t drawKey = 0;
    std::shared_ptr<DrawEntry> entry;
    std::shared_ptr<const DrawDecode> decode;
    std::shared_ptr<const DrawDecode> relocatedDecode;
    std::shared_ptr<DrawEntry> dataEntry;
    std::uint64_t shapeKey = 0;
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    std::vector<DrawProgram> writablePrograms;
    const std::vector<DrawProgram>* programs = nullptr;
    std::vector<ShaderRecompiler::MemoryRegion> memory;
    std::vector<ShaderRecompiler::LinkedProgram> linked;
    std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>> vertexInfos;
    std::vector<std::vector<Graphics::DecodeRead>> decodeReads;
    std::unique_ptr<ShaderMemory> shaderMemory;
    std::vector<ShaderRecompiler::RecompileResult> results;
    std::vector<std::shared_ptr<const ShaderRecompiler::RecompileResult>> compiledStages;
    std::vector<Graphics::CompiledShader> stages;
    std::uint32_t pushCursorBytes = 0;
    std::vector<const ShaderRecompiler::RecompileResult*> programResults;
    std::vector<StageCapture> stageCaptures;
    std::vector<std::shared_ptr<DispatchVariant>> matched;
    std::vector<std::vector<ShaderRecompiler::MemoryRegion>> matchedRegions;
    std::vector<std::shared_ptr<DispatchVariant>> fresh;
    std::vector<bool> recompiled;
    DrawDataCandidates dataCandidates;
    std::vector<bool> reused;
    std::vector<StageDataHit> stageData;
    bool drawHit = false;
    bool verifyHit = false;
    bool stageEntries = false;
    bool patchedStages = false;
    const Submission* submission = nullptr;
    std::uint32_t submissionQueue = 0;
    std::uint64_t submissionSerial = 0;
    std::size_t packetOffset = 0;
    std::uint32_t packetOpcode = 0;
    std::uint32_t reg207 = 0;
    std::uint64_t epoch = 0;
    std::uint64_t ticket = 0;
    QueueState* queue = nullptr;
    std::span<const std::uint32_t> packet;
    std::string* rejected = nullptr;
    std::uint64_t traceToken = 0;
    VulkanDevice::DrawTraceContext trace{};
    std::string ownRejected;
    GpuLock* gpuLock = nullptr;
    GpuLock ownLock{GuestMemory::GpuMutex(), std::defer_lock};
};

namespace {

struct DrawLockTiming {
    bool profile;
    Graphics::DrawPreparation preparation;
    double prepareMs = 0;
    std::uint64_t heldBefore = 0;
    std::uint64_t collectedBefore = 0;
    bool started = false;

    void Start() {
        if (!profile || started) return;
        heldBefore = GuestMemory::ThreadGpuHeldNs();
        collectedBefore = GuestMemory::ThreadCollectedBytes();
        started = true;
    }

    ~DrawLockTiming() {
        if (!profile || !started) return;
        const auto heldMs = static_cast<double>(GuestMemory::ThreadGpuHeldNs() - heldBefore) / 1e6;
        const auto collected = GuestMemory::ThreadCollectedBytes() - collectedBefore;
        struct Counts {
            std::uint64_t draws = 0;
            std::uint64_t keys = 0;
            std::uint64_t keyInvalidated = 0;
            std::uint64_t cacheChanged = 0;
            std::uint64_t collectInvalidated = 0;
            std::uint64_t redone = 0;
            std::uint64_t ranges = 0;
            std::uint64_t unavailable = 0;
            std::uint64_t prelockBytes = 0;
            std::uint64_t lockedBytes = 0;
            std::uint64_t targetCandidates = 0;
            std::uint64_t targetReady = 0;
            std::uint64_t targetAccepted = 0;
            std::uint64_t targetLookupAccepted = 0;
            std::uint64_t targetFallbacks = 0;
            double targetProofUs = 0;
            double prepareMs = 0;
            double heldMs = 0;
            double maxHeldMs = 0;
        };
        static thread_local std::array<Counts, 2> counts{};
        static thread_local auto reportedAt = std::chrono::steady_clock::now();
        auto& count = counts[preparation.device != VK_NULL_HANDLE ? 1 : 0];
        ++count.draws;
        count.keys += preparation.keyUsed;
        count.keyInvalidated += preparation.keyInvalidated;
        count.cacheChanged += preparation.cacheChanged;
        count.collectInvalidated += preparation.collectInvalidated;
        count.redone += preparation.keyInvalidated || preparation.cacheChanged || preparation.collectInvalidated;
        count.ranges += preparation.ranges;
        count.unavailable += preparation.unavailable;
        count.prelockBytes += preparation.collectedBytes;
        count.lockedBytes += collected;
        for (const auto& target : preparation.targets) {
            if (target.device == VK_NULL_HANDLE) continue;
            ++count.targetCandidates;
            count.targetReady += target.ready;
            count.targetAccepted += target.accepted;
            count.targetLookupAccepted += target.lookupAccepted;
            count.targetFallbacks += !target.accepted;
        }
        count.targetProofUs += preparation.targetProofUs;
        count.prepareMs += prepareMs;
        count.heldMs += heldMs;
        count.maxHeldMs = std::max(count.maxHeldMs, heldMs);
        const auto now = std::chrono::steady_clock::now();
        if (now - reportedAt < std::chrono::seconds(10)) return;
        reportedAt = now;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            const auto& total = counts[i];
            if (total.draws == 0) continue;
            AgcDriver::ProfilePrint_nid_no_patch("[draw-lock] queue 0x%x %s (10 s): %llu scopes held %.3f ms (max %.3f ms), prelock %.3f ms; keys reused %llu, redo scopes %llu (keys invalidated %llu, cache probe changed %llu, collect invalidated %llu); target ranges %llu (%llu unavailable), walked prelock %.3f MiB, after lock %.3f MiB\n", GuestMemory::GpuLockThreadTag(), i != 0 ? "prelock" : "locked", static_cast<unsigned long long>(total.draws), total.heldMs, total.maxHeldMs, total.prepareMs, static_cast<unsigned long long>(total.keys), static_cast<unsigned long long>(total.redone), static_cast<unsigned long long>(total.keyInvalidated), static_cast<unsigned long long>(total.cacheChanged), static_cast<unsigned long long>(total.collectInvalidated), static_cast<unsigned long long>(total.ranges), static_cast<unsigned long long>(total.unavailable), static_cast<double>(total.prelockBytes) / 1048576.0, static_cast<double>(total.lockedBytes) / 1048576.0);
            AgcDriver::ProfilePrint_nid_no_patch("[draw-target-prelock] queue 0x%x %s (10 s): %llu draw scopes, hold %.3f us/scope; target-proof %s, candidates %llu, proved before lock %llu, successes %llu (lookup reused %llu), fallbacks/unconsumed %llu; preparation %.3f ms\n", GuestMemory::GpuLockThreadTag(), i != 0 ? "prelock" : "locked", static_cast<unsigned long long>(total.draws), total.heldMs * 1000.0 / total.draws, Graphics::StorageTexture::DrawTargetPrelockEnabled() ? "on" : "off", static_cast<unsigned long long>(total.targetCandidates), static_cast<unsigned long long>(total.targetReady), static_cast<unsigned long long>(total.targetAccepted), static_cast<unsigned long long>(total.targetLookupAccepted), static_cast<unsigned long long>(total.targetFallbacks), total.targetProofUs / 1000.0);
        }
        counts = {};
    }
};

void noteFinisherHandOff(bool handed) {
    static std::atomic<std::uint64_t> handedOff{0}, kept{0};
    static std::atomic<std::uint64_t> reportedHanded{0}, reportedKept{0};
    static const bool registered = [] {
        ArchB::Recorder::RegisterCounter("finisher_handoffs", handedOff);
        ArchB::Recorder::RegisterCounter("finisher_inline", kept);
        return true;
    }();
    static_cast<void>(registered);
    static std::atomic<std::int64_t> reportedAt{0};
    (handed ? handedOff : kept).fetch_add(1, std::memory_order_relaxed);
    static const bool stats = std::getenv("APS5_PIPELINE_STATS") != nullptr || std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!stats) return;
    const auto tenSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 10;
    if (reportedAt.exchange(tenSeconds, std::memory_order_relaxed) == tenSeconds) return;
    const auto handedNow = handedOff.load(std::memory_order_relaxed);
    const auto keptNow = kept.load(std::memory_order_relaxed);
    AgcDriver::ProfilePrint_nid_no_patch("[finisher] draws (10 s): %llu handed to the draw finisher, %llu finished on the worker%s\n", static_cast<unsigned long long>(handedNow - reportedHanded.exchange(handedNow)), static_cast<unsigned long long>(keptNow - reportedKept.exchange(keptNow)), DrawFinisher::DrainText().c_str());
}

}

namespace {

enum class HeldReason : std::size_t { Known, NoDecode, Size, NullVertex, NullFragment, NullOther, Bda, Buffer, Storage, Count };
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(HeldReason::Count)> heldReasons{};

void noteHeld(HeldReason reason) {
    heldReasons[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    static const bool profile = std::getenv("APS5_PIPELINE_STATS") != nullptr || std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    static std::atomic<std::uint64_t> total{0};
    const auto count = ++total;
    if (count % 262144u != 0u && (count > 262144u || (count & (count - 1u)) != 0u)) return;
    constexpr std::array<const char*, static_cast<std::size_t>(HeldReason::Count)> names{"targets only", "no decode", "stage count", "no variant (vertex)", "no variant (fragment)", "no variant (other)", "BDA stores", "written buffer", "storage image"};
    std::string text;
    for (std::size_t i = 0; i < names.size(); ++i) {
        char item[64];
        std::snprintf(item, sizeof(item), "%s %s %llu", i == 0 ? "" : ",", names[i], static_cast<unsigned long long>(count >= 262144u ? heldReasons[i].exchange(0) : heldReasons[i].load()));
        text += item;
    }
    std::fprintf(stderr, "[finisher-held] held draws' writes:%s\n", text.c_str());
}

std::optional<HeldReason> writesBeyondTargets(const ShaderRecompiler::RecompileResult& compiled) {
    using Role = ShaderRecompiler::DescriptorRole;
    for (const auto& binding : compiled.bindings) {
        if (binding.role == Role::BdaPagetable && Graphics::ShaderStoresThroughAddresses(compiled.spirv)) return HeldReason::Bda;
        if (binding.role == Role::GuestBuffers) {
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                if (element >= binding.Flags().bufferWritten.size() || binding.Flags().bufferWritten[element]) return HeldReason::Buffer;
            }
        }
        if (binding.role == Role::GuestImages && binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) {
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                if (element >= binding.Flags().imageWritten.size() || binding.Flags().imageWritten[element]) return HeldReason::Storage;
            }
        }
    }
    return std::nullopt;
}

DrawFinisher::Pending heldWrites(const DrawTail& tail) {
    DrawFinisher::Pending held;
    held.key = tail.drawKey;
    held.shape = tail.shapeKey;
    if (tail.decode == nullptr) {
        noteHeld(HeldReason::NoDecode);
        return held;
    }
    if (tail.matched.size() != tail.programs->size()) {
        noteHeld(HeldReason::Size);
        return held;
    }
    const auto& roles = tail.decode->roles;
    for (std::size_t i = 0; i < tail.matched.size(); ++i) {
        if (i < roles.size() && roles[i] == ShaderRecompiler::ProgramRole::GeometryBack) continue;
        const auto& variant = tail.matched[i];
        if (variant == nullptr || variant->compiled == nullptr) {
            const bool candidates = !tail.drawHit && i < tail.dataCandidates.size() && !tail.dataCandidates[i].empty();
            const auto& program = (*tail.programs)[i];
            if (!candidates && program.snapshot != nullptr && !ProgramWritesMemory(*program.snapshot, program.codeOffset) && (program.tailContinuation == nullptr || !ProgramWritesMemory(*program.tailContinuation, 0))) continue;
            if (!candidates) {
                const auto stage = (*tail.programs)[i].binary.stage;
                noteHeld(stage == ShaderRecompiler::ShaderStage::Fragment ? HeldReason::NullFragment : stage == ShaderRecompiler::ShaderStage::Vertex ? HeldReason::NullVertex : HeldReason::NullOther);
                return held;
            }
            for (const auto& [candidate, regions, feedbackHit, words] : tail.dataCandidates[i]) {
                if (candidate == nullptr || candidate->compiled == nullptr) {
                    noteHeld(HeldReason::NullOther);
                    return held;
                }
                if (const auto reason = writesBeyondTargets(*candidate->compiled)) {
                    noteHeld(*reason);
                    return held;
                }
            }
            continue;
        }
        if (const auto reason = writesBeyondTargets(*variant->compiled)) {
            noteHeld(*reason);
            return held;
        }
        if (i < tail.stageData.size() && tail.stageData[i].compiled != nullptr) {
            if (const auto reason = writesBeyondTargets(*tail.stageData[i].compiled)) {
                noteHeld(*reason);
                return held;
            }
        }
    }
    noteHeld(HeldReason::Known);
    held.writes = DrawWriteRanges(tail.decode->state, {}, ExactDrawWrites());
    held.unknown = false;
    return held;
}

GatherIndirectPlan GatherIndirectOf(const Pm4::DrawParameters& parameters) {
    GatherIndirectPlan plan;
    if (!parameters.indirect) return plan;
    const auto& indirect = *parameters.indirect;
    plan.present = true;
    plan.arguments = indirect.arguments;
    plan.countIndirect = indirect.countIndirect;
    plan.countAddress = indirect.countIndirect ? indirect.countAddress : 0;
    plan.count = indirect.count;
    plan.recordBytes = indirect.recordBytes;
    plan.rangeBytes = std::max<std::uint64_t>(indirect.RangeBytes(), indirect.recordBytes);
    return plan;
}

std::uint64_t GatherTargetAddress(const Graphics::State& graphics) {
    return graphics.hasColorTarget ? graphics.color.MemoryAddress() : 0;
}

GatherBufferLimits GatherLimitsOf(VulkanDevice& device, const Graphics::State& graphics) {
    struct LimitsOfDevice {
        std::uint64_t serial = 0;
        GatherBufferLimits limits;
    };
    auto& held = HostThreadLocal<LimitsOfDevice, LimitsOfDevice>();
    auto& serial = held.serial;
    auto& limits = held.limits;
    if (serial != device.Serial() || limits.maxRange == 0) {
        const auto context = device.graphicsContext();
        limits.maxRange = context.limits.maxStorageBufferRange;
        limits.alignment = static_cast<std::uint32_t>(std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 4));
        limits.wholeDwords = context.exactRobustLoads;
        serial = device.Serial();
    }
    auto result = limits;
    result.targetAddress = GatherTargetAddress(graphics);
    return result;
}

std::uint64_t LiveMix(std::uint64_t key, std::uint64_t value) {
    return (key ^ value) * 0x100000001b3ull;
}

std::uint64_t LivePacketKey(const Pm4::DrawParameters& draw) {
    auto key = LiveMix(0xcbf29ce484222325ull, draw.indexed);
    for (const auto value : {draw.indexCount, draw.indexSize, draw.instanceCount, draw.flags, draw.firstVertex, draw.firstInstance}) key = LiveMix(key, value);
    if (!draw.indirect) return LiveMix(key, 0);
    const auto& args = *draw.indirect;
    for (const auto value : {args.opcode, args.recordBytes, args.stride, args.count, static_cast<std::uint32_t>(args.countIndirect), args.baseVertexLocation, args.startInstanceLocation, args.drawIndexLocation, static_cast<std::uint32_t>(args.drawIndexEnabled), args.indxOffset, static_cast<std::uint32_t>(args.indexLimitKnown)}) key = LiveMix(key, value);
    return key;
}

bool LivePacketMatches(const Pm4::DrawParameters& a, const Pm4::DrawParameters& b) {
    if (a.indexCount != b.indexCount || a.indexSize != b.indexSize || a.instanceCount != b.instanceCount || a.flags != b.flags || a.indexed != b.indexed || a.firstVertex != b.firstVertex || a.firstInstance != b.firstInstance || a.indirect.has_value() != b.indirect.has_value()) return false;
    if (!a.indirect) return true;
    const auto& x = *a.indirect;
    const auto& y = *b.indirect;
    return x.opcode == y.opcode && x.recordBytes == y.recordBytes && x.stride == y.stride && x.count == y.count && x.countIndirect == y.countIndirect && x.baseVertexLocation == y.baseVertexLocation && x.startInstanceLocation == y.startInstanceLocation && x.drawIndexLocation == y.drawIndexLocation && x.drawIndexEnabled == y.drawIndexEnabled && x.indxOffset == y.indxOffset && x.indexLimitKnown == y.indexLimitKnown;
}

const Registers& LiveBank(const QueueState& queue, Graphics::RegisterBank bank) {
    if (bank == Graphics::RegisterBank::Context) return queue.context;
    if (bank == Graphics::RegisterBank::Shader) return queue.shader;
    return queue.userConfig;
}

std::uint32_t LiveRegister(const QueueState& queue, Graphics::RegisterBank bank, std::uint32_t offset) {
    const auto& registers = LiveBank(queue, bank);
    const auto found = registers.find(offset);
    return found != registers.end() ? found->second : 0;
}

bool LiveStateMatches(const GatherLiveEntry& entry, const QueueState& queue) {
    return std::all_of(entry.registers.begin(), entry.registers.end(), [&](const auto& value) { return LiveRegister(queue, value.bank, value.offset) == value.value; });
}

struct GatherLiveIndex {
    std::unordered_map<std::uint64_t, std::vector<std::shared_ptr<GatherLiveEntry>>> entries;
    std::unordered_map<std::uint64_t, std::size_t> cursors;
    LiveSequence<GatherLiveEntry> sequence;
    std::unordered_map<std::uint64_t, std::shared_ptr<GatherLiveEntry>> signatures;
};

void IndexLiveSignature(std::uint64_t key, const std::shared_ptr<GatherLiveEntry>& entry);

constexpr std::size_t LiveWays = 8;

GatherLiveIndex& LiveIndex() {
    struct LiveIndexStorage {};
    return HostThreadLocal<GatherLiveIndex, LiveIndexStorage>();
}

GatherOptions LiveOptions(const DrawTail& tail) {
    return tail.liveOptions;
}

bool LiveVerifyNext() {
    static const auto period = [] {
        const char* text = std::getenv("APS5_ARCHB_DRAW_TEMPLATES_LIVE_VERIFY");
        return text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
    }();
    if (period == 0) return false;
    struct LiveVerifyDraws {};
    return ++HostThreadLocal<std::uint64_t, LiveVerifyDraws>() % period == 0;
}

void MergeIgnoredWords(Graphics::DrawReferenceMasks::Words& into, std::span<const std::pair<std::uint32_t, std::uint32_t>> from) {
    if (from.empty()) return;
    into.insert(into.end(), from.begin(), from.end());
    std::sort(into.begin(), into.end());
    std::size_t kept = 0;
    for (std::size_t index = 1; index < into.size(); ++index) {
        if (into[index].first == into[kept].first) into[kept].second |= into[index].second;
        else into[++kept] = into[index];
    }
    into.resize(kept + 1);
}

bool LiveViewsCurrent(GatherLiveEntry& entry) {
    const auto generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    if (entry.allocationGeneration == generation) return true;
    for (const auto& stage : entry.link->stages) if (stage.registeredViews != nullptr && !Graphics::ShaderResources::RegisteredViewsCurrent(*stage.registeredViews)) {
        entry.link->valid.store(false, std::memory_order_release);
        return false;
    }
    if (GuestAllocations::GuestAllocationsGeneration_nid_postfix() != generation) return false;
    entry.allocationGeneration = generation;
    return true;
}

void CountLiveDraw(const Pm4::DrawParameters& draw, const char* name, double value = 1) {
    ArchB::Recorder::Count(name, value);
    if (!ArchB::Recorder::Enabled()) return;
    struct TypedNamesStorage {};
    auto& typedNames = HostThreadLocal<std::unordered_map<const char*, std::array<std::string, 2>>, TypedNamesStorage>();
    const auto [found, inserted] = typedNames.try_emplace(name);
    if (inserted) found->second = {std::string(name) + "_direct", std::string(name) + "_indirect"};
    ArchB::Recorder::Count(found->second[draw.indirect.has_value()].c_str(), value);
}

std::shared_ptr<GatherLiveEntry> LearnLive(const QueueState& queue, const DrawTail& tail) {
    if (tail.liveKey == 0) return {};
    if (const char* refused = !tail.drawHit ? "draw_templates_live_learn_refused_cache_miss" : tail.entry == nullptr ? "draw_templates_live_learn_refused_entry" : tail.decode == nullptr ? "draw_templates_live_learn_refused_decode" : !tail.drawParameters.indirect && !ArchB::DirectDrawEnabled() ? "draw_templates_live_learn_refused_direct_disabled" : tail.matched.size() != tail.decode->programs.size() ? "draw_templates_live_learn_refused_stage_count" : nullptr) {
        CountLiveDraw(tail.drawParameters, refused);
        return {};
    }
    if (tail.decode->state.stages.path != Graphics::ShaderPath::Vertex || tail.decode->state.stages.mesh || tail.decode->state.stages.tessellation || tail.decode->state.rectList) {
        CountLiveDraw(tail.drawParameters, "draw_templates_live_learn_refused_shader_path");
        return {};
    }
    std::vector<std::shared_ptr<DispatchVariant>> donors;
    for (const auto& variant : tail.matched) {
        if (variant == nullptr || variant->compiled == nullptr) {
            CountLiveDraw(tail.drawParameters, "draw_templates_live_learn_refused_variant");
            return {};
        }
        donors.push_back(variant->donor != nullptr ? variant->donor : variant);
    }
    auto& index = LiveIndex().entries;
    if (index.size() >= 4096 && !index.contains(tail.liveKey)) {
        index.clear();
        LiveIndex().cursors.clear();
        LiveIndex().sequence.Clear();
        LiveIndex().signatures.clear();
    }
    auto& bucket = index[tail.liveKey];
    const auto options = LiveOptions(tail);
    const auto matches = [&](const std::shared_ptr<GatherLiveEntry>& known) { return known->link->valid.load(std::memory_order_acquire) && known->variants == donors && known->options == options && LivePacketMatches(known->packet, tail.livePacket) && LiveViewsCurrent(*known) && LiveStateMatches(*known, queue); };
    for (const auto& known : bucket) if (matches(known)) return known;
    if (tail.livePosition.valid) {
        const auto predicted = LiveIndex().sequence.Predict(tail.livePosition);
        for (std::size_t guess = 0; guess < predicted.count; ++guess) if (matches(predicted.entries[guess])) {
            CountLiveDraw(tail.drawParameters, "draw_templates_live_sequence_relearn_avoided");
            return predicted.entries[guess];
        }
    }
    std::uint64_t variantsKey = 0;
    if (ArchB::Switches::Enabled(ArchB::Feature::DrawTemplatesLiveIndex)) {
        variantsKey = LiveMix(tail.liveKey, donors.size());
        for (const auto& donor : donors) variantsKey = LiveMix(variantsKey, reinterpret_cast<std::uintptr_t>(donor.get()));
        variantsKey |= 1u;
        const auto known = LiveIndex().signatures.find(variantsKey);
        if (known != LiveIndex().signatures.end() && matches(known->second)) {
            CountLiveDraw(tail.drawParameters, "draw_templates_live_index_relearn_avoided");
            return known->second;
        }
    }
    auto entry = std::make_shared<GatherLiveEntry>();
    entry->decode = tail.entry->decode;
    entry->variants = std::move(donors);
    entry->packet = tail.livePacket;
    entry->allocationGeneration = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    entry->options = options;
    entry->link = std::make_shared<GatherCommitTemplate>();
    const auto stateOwner = tail.decode->stateOrigin != nullptr ? tail.decode->stateOrigin : tail.decode;
    entry->link->state = {stateOwner, &stateOwner->state};
    for (std::size_t stage = 0; stage < entry->variants.size(); ++stage) {
        const auto& variant = entry->variants[stage];
        const auto& program = entry->decode->programs[stage];
        if (program.merged || program.binary.stage != ShaderRecompiler::ShaderStage::Vertex && program.binary.stage != ShaderRecompiler::ShaderStage::Fragment) {
            CountLiveDraw(tail.drawParameters, "draw_templates_live_learn_refused_stage");
            return {};
        }
        GatherCommitStage bound;
        bound.compiled = variant->compiled;
        bound.variant = variant;
        bound.stage = program.binary.stage;
        bound.pushOffset = variant->compiled->pushConstants.empty() ? 0u : variant->pushOffset;
        bound.registeredViews = variant->registeredViews;
        entry->link->stages.push_back(std::move(bound));
    }
    static const bool exactRegisters = std::getenv("APS5_DRAW_TEMPLATES_LIVE_EXACT_REGISTERS") != nullptr;
    const auto keyedRuntime = [&](std::uint32_t offset, bool& runtime) {
        runtime = true;
        if (offset == 0x082u || offset == 0x083u || offset == 0x102u || offset == 0x103u) return true;
        for (const auto& [base, limit] : {std::pair{0x00cu, 32u}, std::pair{0x04cu, 16u}, std::pair{0x08cu, 32u}, std::pair{0x10cu, 32u}}) {
            if (offset - base >= limit) continue;
            const auto resources = queue.shader.find(base - 1);
            const auto count = resources == queue.shader.end() ? 0u : ((resources->second >> 1u) & 0x1fu) | (((resources->second >> 27u) & 1u) << 5u);
            return offset - base < std::min(count, limit);
        }
        runtime = false;
        return false;
    };
    for (const auto& range : Graphics::DrawKeyRegisters) for (std::uint32_t offset = range.first; offset < range.first + range.count; ++offset) {
        const bool user = range.bank == Graphics::RegisterBank::Shader && std::any_of(entry->decode->programs.begin(), entry->decode->programs.end(), [&](const auto& program) { return offset >= program.userDataBase && offset - program.userDataBase < program.userData.size() - program.hiddenUserWords; });
        if (user) continue;
        bool runtime = false;
        const bool keyed = range.bank == Graphics::RegisterBank::Shader && keyedRuntime(offset, runtime);
        if (exactRegisters || keyed) entry->registers.push_back({range.bank, offset, LiveRegister(queue, range.bank, offset)});
    }
    if (bucket.size() >= LiveWays) {
        const auto stale = std::find_if(bucket.begin(), bucket.end(), [](const auto& known) { return !known->link->valid.load(std::memory_order_acquire); });
        bucket.erase(stale != bucket.end() ? stale : bucket.begin());
        CountLiveDraw(tail.drawParameters, "draw_templates_live_bucket_evictions");
    }
    bucket.push_back(entry);
    IndexLiveSignature(variantsKey, entry);
    CountLiveDraw(tail.drawParameters, "draw_templates_live_templates_learned");
    return entry;
}

void IndexLiveSignature(std::uint64_t key, const std::shared_ptr<GatherLiveEntry>& entry) {
    if (key == 0) return;
    auto& signatures = LiveIndex().signatures;
    if (signatures.size() >= 65536 && !signatures.contains(key)) signatures.clear();
    auto& slot = signatures[key];
    if (slot != entry) slot = entry;
}

std::shared_ptr<GatherLiveDraw> SelectLive(const QueueState& queue, const Submission& submission, DrawTail& tail) {
    const auto started = std::chrono::steady_clock::now();
    const bool sequenced = ArchB::Switches::Enabled(ArchB::Feature::DrawTemplatesLiveSequence);
    tail.livePosition = sequenced ? LiveIndex().sequence.Next(tail.liveKey, LiveSequenceFrame().load(std::memory_order_relaxed)) : LiveSequence<GatherLiveEntry>::Position{};
    const auto predicted = LiveIndex().sequence.Predict(tail.livePosition);
    if (predicted.count != 0) CountLiveDraw(tail.drawParameters, "draw_templates_live_sequence_predicted");
    const auto found = LiveIndex().entries.find(tail.liveKey);
    if (found == LiveIndex().entries.end() && predicted.count == 0) { CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_no_template"); return {}; }
    const auto options = LiveOptions(tail);
    struct LiveProgramsStorage {};
    auto& programs = HostThreadLocal<std::vector<DrawProgram>, LiveProgramsStorage>();
    static const bool refuseFeedback = std::getenv("APS5_DRAW_TEMPLATES_LIVE_NO_FEEDBACK") != nullptr;
    bool ready = false, feedback = false;
    auto& cursor = LiveIndex().cursors[tail.liveKey];
    const auto ways = found != LiveIndex().entries.end() ? found->second.size() : 0;
    const bool indexed = ArchB::Switches::Enabled(ArchB::Feature::DrawTemplatesLiveIndex);
    std::uint64_t unmatchedKey = 0;
    const auto attempt = [&](const std::shared_ptr<GatherLiveEntry>& entry) -> std::shared_ptr<GatherLiveDraw> {
        unmatchedKey = 0;
        if (!entry->link->valid.load(std::memory_order_acquire) || !entry->link->ready.load(std::memory_order_acquire) || !entry->derivedReady) return {};
        ready = true;
        if (const char* skipped = !LivePacketMatches(entry->packet, tail.livePacket) ? "draw_templates_live_q0_skip_packet" : entry->options != options ? "draw_templates_live_q0_skip_options" : !LiveStateMatches(*entry, queue) ? "draw_templates_live_q0_skip_state" : !LiveViewsCurrent(*entry) ? "draw_templates_live_q0_skip_views" : !DecodeProgramsCurrent(*entry->decode, *submission.shaders) ? "draw_templates_live_q0_skip_decode" : nullptr) {
            CountLiveDraw(tail.drawParameters, skipped);
            return {};
        }
        if (refuseFeedback && options.feedback && std::any_of(entry->variants.begin(), entry->variants.end(), [](const auto& variant) {
            return !variant->feedback.memory.empty() || !variant->feedback.userData.empty();
        })) {
            feedback = true;
            return {};
        }
        programs = entry->decode->programs;
        for (auto& program : programs) ReadUserWords(queue, program, false);
        GatherShadowDraw candidate;
        candidate.programs = programs;
        DrawEntry donor;
        donor.decode = entry->decode;
        candidate.entry = &donor;
        candidate.matched = entry->variants;
        candidate.hit = true;
        candidate.keyed = true;
        candidate.options = options;
        candidate.indirect = GatherIndirectOf(tail.drawParameters);
        candidate.limits = GatherLimitsOf(*tail.localDevice, *entry->link->state);
        candidate.live = true;
        candidate.commitTemplate = entry->link;
        CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_gathers");
        auto plan = RunGatherShadow(candidate);
        if (plan == nullptr || !plan->complete || entry->allocationGeneration != GuestAllocations::GuestAllocationsGeneration_nid_postfix()) {
            if (indexed && plan != nullptr && !plan->matched && (plan->incomplete & ~static_cast<std::uint32_t>(GatherUnmatched)) == 0) unmatchedKey = LiveSignatureKey(tail.liveKey, plan->signatures);
            if (plan == nullptr) CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_no_plan");
            else if (plan->complete) CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_allocation_generation");
            if (plan != nullptr) {
                if (plan->incomplete & GatherUnknownBindings) CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_unknown_bindings");
                if ((plan->incomplete & GatherUnmatched) && plan->differ == GatherDiffer::None) CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_unmatched_unclassified");
                if (plan->incomplete == GatherIncompleteNone && !plan->matched) CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_not_matched");
                if (plan->incomplete & GatherPendingWords) CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_pending_words");
                if (plan->incomplete & GatherPendingVsharp) CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_pending_vsharp");
                if (plan->incomplete & GatherPendingIndirect) CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_pending_indirect");
                if (plan->incomplete & GatherGenerationMoved) CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_generation");
                if (plan->incomplete & GatherBindingShape) CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_binding_shape");
                if (plan->incomplete & GatherNotTemplatable) CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_feedback");
                if (plan->differ != GatherDiffer::None) {
                    static const auto names = [] {
                        std::array<std::string, static_cast<std::size_t>(GatherDiffer::Count)> values;
                        for (std::size_t kind = 0; kind < values.size(); ++kind) values[kind] = std::string("draw_templates_live_q0_differ_") + GatherDifferName(static_cast<GatherDiffer>(kind));
                        return values;
                    }();
                    CountLiveDraw(tail.drawParameters, names[static_cast<std::size_t>(plan->differ)].c_str());
                    static const bool trace = std::getenv("APS5_DRAW_TEMPLATES_LIVE_TRACE_DIFFER") != nullptr;
                    static std::atomic<unsigned> traced{0};
                    if (trace && traced.fetch_add(1, std::memory_order_relaxed) % 37u == 0 && traced.load(std::memory_order_relaxed) < 37u * 120u) {
                        const auto& differing = entry->decode->programs[std::min<std::size_t>(plan->differStage, entry->decode->programs.size() - 1)];
                        std::fprintf(stderr, "[draw-templates-live] differ %s stage %u program 0x%llx index %u old %08x new %08x mask %08x ways %zu\n", GatherDifferName(plan->differ), plan->differStage, static_cast<unsigned long long>(differing.snapshot->CodeDigest()), plan->differIndex, plan->differOld, plan->differNew, plan->differMask, ways);
                    }
                }
            }
            return {};
        }
        auto live = std::make_shared<GatherLiveDraw>();
        live->entry = entry;
        live->plan = std::move(plan);
        live->parameters = entry->derived;
        live->parameters.indexAddress = tail.drawParameters.indexAddress;
        if (live->parameters.indirect) {
            live->parameters.indirect->arguments = tail.drawParameters.indirect->arguments;
            live->parameters.indirect->countAddress = tail.drawParameters.indirect->countAddress;
        } else {
            live->parameters = tail.drawParameters;
        }
        const auto& front = programs.front();
        const auto& compiled = *entry->link->stages.front().compiled;
        const auto constant = [&](std::int32_t sgpr, std::uint32_t base, std::uint32_t& out) {
            std::uint32_t value = 0;
            if (sgpr >= 0) {
                const auto index = static_cast<std::int64_t>(sgpr) - front.firstUserSgpr;
                if (index < 0 || static_cast<std::size_t>(index) >= front.userData.size()) return false;
                value = front.userData[static_cast<std::size_t>(index)];
            }
            if (value > std::numeric_limits<std::uint32_t>::max() - base) return false;
            out = base + value;
            return true;
        };
        bool argumentsValid = true;
        if (live->parameters.indirect) {
            auto& args = *live->parameters.indirect;
            using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
            argumentsValid = (args.vertexRule != Rule::Constant || constant(compiled.vertexOffsetSgpr, live->parameters.indexed ? 0u : args.indxOffset, args.vertexConstant)) && (args.instanceRule != Rule::Constant || constant(compiled.instanceOffsetSgpr, 0, args.instanceConstant)) && (tail.localDevice->DrawIndirectSupport().firstInstance || (args.instanceRule != Rule::InPlace && args.instanceConstant == 0));
        } else {
            argumentsValid = (compiled.vertexOffsetSgpr < 0 || constant(compiled.vertexOffsetSgpr, live->parameters.firstVertex, live->parameters.firstVertex)) && (compiled.instanceOffsetSgpr < 0 || constant(compiled.instanceOffsetSgpr, 0, live->parameters.firstInstance));
            const auto& direct = live->parameters;
            argumentsValid = argumentsValid && direct.indexCount != 0 && direct.instanceCount != 0;
            if (direct.indexed) argumentsValid = argumentsValid && (direct.indexSize == 2 || direct.indexSize == 4);
            else argumentsValid = argumentsValid && direct.indexAddress == 0 && direct.indexSize == 0 && direct.firstVertex <= std::numeric_limits<std::uint32_t>::max() - (direct.indexCount - 1u) && direct.firstInstance <= std::numeric_limits<std::uint32_t>::max() - (direct.instanceCount - 1u);
        }
        if (!argumentsValid) {
            CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_arguments");
            return {};
        }

        for (const auto& stage : entry->link->stages) live->stages.push_back({stage.stage, stage.compiled.get(), stage.pushOffset, stage.registeredViews.get()});
        live->verify = tail.liveVerify;
        if (indexed) IndexLiveSignature(LiveSignatureKey(tail.liveKey, live->plan->signatures), entry);
        return live;
    };
    const auto taken = [&](std::shared_ptr<GatherLiveDraw> live, const char* how) {
        if (sequenced) LiveIndex().sequence.Record(tail.livePosition, live->entry);
        if (how != nullptr) CountLiveDraw(tail.drawParameters, how);
        CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_taken");
        CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_ns", static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()));
        return live;
    };
    struct LiveTriedStorage {};
    auto& tried = HostThreadLocal<std::vector<const GatherLiveEntry*>, LiveTriedStorage>();
    tried.clear();
    const auto wasTried = [&](const GatherLiveEntry* entry) { return std::find(tried.begin(), tried.end(), entry) != tried.end(); };
    std::uint32_t indexTries = 0;
    for (std::size_t step = 0; step < predicted.count + ways; ++step) {
        const bool guessed = step < predicted.count;
        const auto way = guessed ? 0 : (cursor + step - predicted.count) % ways;
        const auto entry = guessed ? predicted.entries[step] : found->second[way];
        if (wasTried(entry.get())) continue;
        tried.push_back(entry.get());
        if (auto live = attempt(entry)) {
            if (!guessed) cursor = (way + 1) % ways;
            return taken(std::move(live), !sequenced ? nullptr : guessed ? step == 0 ? "draw_templates_live_sequence_first_guess" : "draw_templates_live_sequence_later_guess" : "draw_templates_live_sequence_scanned");
        }
        if (tail.liveUnmatchedKey == 0) tail.liveUnmatchedKey = unmatchedKey;
        while (unmatchedKey != 0 && indexTries < 2) {
            const auto known = LiveIndex().signatures.find(unmatchedKey);
            if (known == LiveIndex().signatures.end() || wasTried(known->second.get())) {
                if (known == LiveIndex().signatures.end()) CountLiveDraw(tail.drawParameters, "draw_templates_live_index_unknown");
                break;
            }
            const auto candidate = known->second;
            tried.push_back(candidate.get());
            ++indexTries;
            if (auto live = attempt(candidate)) return taken(std::move(live), "draw_templates_live_index_hits");
            CountLiveDraw(tail.drawParameters, "draw_templates_live_index_misses");
        }
    }
    if (feedback) CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_feedback");
    else if (ready) CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_structure");
    else CountLiveDraw(tail.drawParameters, "draw_templates_live_q0_refused_not_ready");
    return {};
}

RepeatCensus::Draw CensusDraw(const DrawTail& tail, std::uint64_t coarse) {
    using RepeatCensus::Mix;
    RepeatCensus::Draw out;
    const auto& packet = tail.livePacket;
    out.site = tail.liveKey != 0 ? tail.liveKey : Mix(Mix(0x51u, packet.indexed), packet.indirect ? packet.indirect->opcode : 0u);
    out.coarse = coarse != 0 ? coarse : out.site;
    if (tail.live == nullptr) return out;
    const auto& live = *tail.live;
    out.live = true;
    out.structure = Mix(reinterpret_cast<std::uintptr_t>(live.entry.get()), reinterpret_cast<std::uintptr_t>(live.plan->commitTemplate.get()));
    const auto& state = *live.entry->link->state;
    out.target = Mix(state.depth ? state.depth->address : 0u, state.hasColorTarget ? state.color.address : 0u);
    for (const auto& color : state.colors) out.target = Mix(out.target, color.address);
    std::uint64_t addresses = 0x61, constants = 0x62, arguments = 0x63;
    for (const auto& stage : live.plan->stages) {
        for (const auto& buffer : stage.vsharps) {
            addresses = Mix(addresses, (static_cast<std::uint64_t>(buffer.binding) << 32u) | buffer.element);
            addresses = Mix(addresses, buffer.address);
            addresses = Mix(addresses, buffer.size);
        }
        for (const auto& word : stage.words) {
            const bool address = word.source == GatherWord::TableBase || word.source == GatherWord::BufferBase || word.source == GatherWord::NestedPointer;
            auto& digest = address ? addresses : constants;
            digest = Mix(digest, (static_cast<std::uint64_t>(word.binding) << 32u) | word.word);
            digest = Mix(digest, word.value);
        }
        for (const auto& buffer : stage.buffers) {
            constants = Mix(constants, buffer.binding);
            for (const auto word : buffer.words) constants = Mix(constants, word);
        }
        for (std::size_t offset = 0; offset < stage.push.size(); offset += 8) {
            std::uint64_t chunk = 0;
            std::memcpy(&chunk, stage.push.data() + offset, std::min<std::size_t>(8, stage.push.size() - offset));
            constants = Mix(constants, chunk);
        }
    }
    const auto& parameters = live.parameters;
    addresses = Mix(addresses, parameters.indexAddress);
    if (live.plan->indirect.present) {
        addresses = Mix(addresses, live.plan->indirect.arguments);
        addresses = Mix(addresses, live.plan->indirect.countAddress);
        arguments = Mix(arguments, (static_cast<std::uint64_t>(live.plan->indirect.count) << 32u) | live.plan->indirect.recordBytes);
    } else {
        arguments = Mix(arguments, (static_cast<std::uint64_t>(parameters.indexCount) << 32u) | parameters.instanceCount);
        arguments = Mix(arguments, (static_cast<std::uint64_t>(parameters.firstVertex) << 32u) | parameters.firstInstance);
    }
    arguments = Mix(arguments, (static_cast<std::uint64_t>(parameters.indexSize) << 1u) | static_cast<std::uint64_t>(parameters.indexed));
    out.addresses = addresses;
    out.constants = constants;
    out.arguments = arguments;
    return out;
}

bool EmptyIndirectSkip() {
    static const bool enabled = std::getenv("APS5_NO_EMPTY_INDIRECT_SKIP") == nullptr;
    return enabled;
}

ArchB::Recorder::Key RecorderProgramKey(const std::vector<DrawProgram>& programs) {
    std::uint64_t digest = 0xcbf29ce484222325ull ^ programs.size();
    for (const auto& program : programs) {
        digest = (digest ^ program.snapshot->CodeDigest()) * 0x100000001b3ull;
        digest = (digest ^ program.codeOffset) * 0x100000001b3ull;
        digest = (digest ^ static_cast<std::uint64_t>(program.binary.stage)) * 0x100000001b3ull;
    }
    return {ArchB::Recorder::Kind::Draw, digest};
}

DrawProgramIdentity ProgramIdentity(const std::vector<DrawProgram>* programs) {
    DrawProgramIdentity identity;
    if (programs == nullptr || !DrawShaderProfileEnabled()) return identity;
    identity.digest = RecorderProgramKey(*programs).code;
    for (const auto& program : *programs) {
        if (identity.programs == DrawProfilePrograms) break;
        identity.code[identity.programs] = program.binary.codeAddress;
        identity.stage[identity.programs] = static_cast<std::uint8_t>(program.binary.stage);
        ++identity.programs;
    }
    return identity;
}

struct CpuIndirectProgramsStorage {};

std::unordered_map<std::uint64_t, bool>& CpuIndirectPrograms() {
    return HostThreadLocal<std::unordered_map<std::uint64_t, bool>, CpuIndirectProgramsStorage>();
}

// The write set a queued draw announces; APS5_ARCHB_PIPE_NARROW_DRAW_WRITES bounds programs on the BDA
// ABI without BDA writes and HTILE depth targets (counted when the bounded set has one of them, which
// without the switch would have made it unbounded).
// APS5_ARCHB_PIPE_NARROW_SLICED_DEPTH bounds single-sample depth targets whose view names slices
// (counted when the bounded set has one).
std::vector<DrawPipeline::Range> queuedDrawWrites(const DrawTail& tail, const Graphics::State& graphics, std::span<const Graphics::CompiledShader> stages) {
    auto writes = DrawWriteRanges(graphics, stages, ExactDrawWrites(), tail.narrowDrawWrites, tail.narrowSlicedDepth);
    if ((tail.narrowDrawWrites || tail.narrowSlicedDepth) && !UnboundedWrites(writes)) {
        if (tail.narrowDrawWrites && ((graphics.depth && graphics.depth->htileAddress != 0) || std::any_of(stages.begin(), stages.end(), [](const Graphics::CompiledShader& stage) { return stage.program != nullptr && stage.program->bdaAbiVersion != 0; }))) DrawPipeline::CountNarrowedDrawWrites();
        if (tail.narrowSlicedDepth && graphics.depth && (graphics.depth->firstSlice != 0 || graphics.depth->lastSlice != 0)) DrawPipeline::CountNarrowedSlicedDepth();
    }
    return writes;
}

// APS5_ARCHB_PIPE_INDIRECT_GPU_ARGS: before the enqueue side reads an indirect draw's records or count
// on the CPU, the queued items that write them commit (and their writes are announced, so the read's
// flush hook waits for the GPU), as the drain at the start of the draw did with the switch off.
void drainBeforeArgumentRead(const Pm4::DrawParameters::IndirectDraw& indirect, DrawPipeline::DrainCause cause) {
    auto& pipeline = DrawPipeline::Queue0();
    if (!pipeline.Busy()) return;
    const auto argumentBytes = static_cast<std::size_t>(std::max<std::uint64_t>(indirect.RangeBytes(), indirect.recordBytes));
    if (pipeline.Overlaps(indirect.arguments, argumentBytes) || (indirect.countIndirect && pipeline.Overlaps(indirect.countAddress, 4))) pipeline.Drain(DrawPipeline::DrainReason::Indirect, 0x100, cause);
}

}

DrawVerdict Driver::draw(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::size_t packetOffset, std::string& rejected) {
    const bool hostSlabAlloc = ArchB::Switches::Enabled(ArchB::Feature::FixHostSlabAlloc);
    const HostAllocator::Scope hostSlabScope(hostSlabAlloc);
    const bool cheapTimers = ArchB::Switches::Enabled(ArchB::Feature::FixCheapTimers);
    const ArchB::CheapClock::Scope cheapTimersScope(cheapTimers);
    ArchB::Recorder::Work recorderWork;
    PerformanceTimer timing("Driver.Draw");
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto tailHolder = std::make_shared<DrawTail>();
    auto& tail = *tailHolder;
    tail.self = tailHolder;
    tail.feedbackMask = ArchB::Switches::Enabled(ArchB::Feature::FixTsharpFeedbackMask);
    tail.pipelineStateReuse = ArchB::Switches::Enabled(ArchB::Feature::FixPipelineStateReuse);
    tail.memoVirtualQuery = ArchB::Switches::Enabled(ArchB::Feature::FixVqMemo);
    tail.imageTableSnapshotReuse = ArchB::Switches::Enabled(ArchB::Feature::FixImageTableSnapshotReuse);
    tail.imageTableBitScanKeys = ArchB::Switches::Enabled(ArchB::Feature::FixImageTableBitScanKeys);
    tail.bufferBases = ArchB::Switches::Enabled(ArchB::Feature::FixBufferBaseRefresh);
    tail.nestedRelocation = ArchB::Switches::Enabled(ArchB::Feature::FixDrawRelocNested);
    // APS5_ARCHB_FIX_ALLOC_CHURN, once per draw: this thread's allocation sites follow it for the rest
    // of the draw, and the finisher and pipelined commits take the same value from the tail.
    tail.allocChurn = ArchB::Switches::Enabled(ArchB::Feature::FixAllocChurn);
    const ArchB::AllocChurnScope allocChurnScope(tail.allocChurn);
    tail.partialSnapshot = ArchB::Switches::Enabled(ArchB::Feature::FixPartialSnapshot);
    const ArchB::PartialSnapshotScope partialSnapshotScope(tail.partialSnapshot);
    tail.hostSlabAlloc = hostSlabAlloc;
    tail.cheapTimers = cheapTimers;
    auto& phaseMs = tail.phaseMs;
    auto& captures = tail.captures;
    auto& phaseLap = tail.phaseLap;
    phaseLap = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    tail.profile = profile;
    tail.phaseTiming.profile = profile;
    auto& phaseTiming = tail.phaseTiming;
    if (profile && packetStartedAt() != std::chrono::steady_clock::time_point{}) phaseMs[DrawRowPrologue] = std::chrono::duration<double, std::milli>(phaseLap - packetStartedAt()).count();

    const auto drawn = [&] {
        phaseTiming.Phase(DrawRowVectors);
        if (!profile) return DrawVerdict::Drawn;
        auto& pending = pendingDrawPhases();
        pending.phases = true;
        pending.captures = captures;
        pending.ms = phaseMs;
        pending.tailAt = phaseLap;
        pending.identity = ProgramIdentity(tail.programs);
        if (DrawShaderProfileEnabled()) {
            pending.cache = tail.cacheOutcome;
            pending.graphics = Graphics::TakeLastDrawPhases(pending.graphicsUs);
        }
        return DrawVerdict::Drawn;
    };
    const std::uint64_t traceToken = TraceShaderCorrelation() ? NextDrawTraceToken().fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    if (traceToken != 0) {
        AgcDriver::CaptureTrace::LogBestEffort("draw-attempt token=%llu submission=%llu queue=%x offset=%zu",
                static_cast<unsigned long long>(traceToken), static_cast<unsigned long long>(submission.serial), submission.queue, packetOffset);
    }
    auto& drawParameters = tail.drawParameters;
    try {
        drawParameters = Pm4::ResolveDraw(packet, queue);
        tail.resolvedParameters = drawParameters;
    } catch (...) {
        TraceDrawAttemptException(traceToken, "resolve-draw", submission.serial, submission.queue, packetOffset, std::current_exception());
        throw;
    }
    tail.pipelined = DrawPipeline::Active();
    const bool pipelined = tail.pipelined;
    tail.narrowDrawWrites = pipelined && ArchB::Switches::Enabled(ArchB::Feature::PipeNarrowDrawWrites);
    tail.narrowSlicedDepth = pipelined && ArchB::Switches::Enabled(ArchB::Feature::PipeNarrowSlicedDepth);
    static const bool pipelinedIndirect = std::getenv("APS5_NO_PIPELINED_INDIRECT") == nullptr;
    if (pipelined && drawParameters.indirect) {
        const auto& indirect = *drawParameters.indirect;
        auto& pipeline = DrawPipeline::Queue0();
        const bool busy = pipeline.Busy();
        const auto argumentBytes = static_cast<std::size_t>(std::max<std::uint64_t>(indirect.RangeBytes(), indirect.recordBytes));
        const bool arguments = busy && pipeline.Overlaps(indirect.arguments, argumentBytes);
        const bool count = busy && !arguments && indirect.countIndirect && pipeline.Overlaps(indirect.countAddress, 4);
        // APS5_ARCHB_PIPE_INDIRECT_GPU_ARGS: arguments (or a count) that queued items write do not drain
        // here. This drain only ordered the enqueue side of the draw after those commits: the draw is
        // enqueued (finishDraw) and committed after them either way, and at its commit the device
        // either reads the records on the GPU (behind a barrier on the import) or on the CPU at record
        // time (decide in Graphics/src/Draw.cpp), both after the writers were recorded. What the enqueue
        // side reads itself (the records of a CPU-side draw, the empty-draw skip) drains first.
        tail.indirectGpuArgs = pipelinedIndirect && ArchB::Switches::Enabled(ArchB::Feature::PipeIndirectGpuArgs);
        if (tail.indirectGpuArgs && (arguments || count)) {
            DrawPipeline::CountKeptIndirectArguments();
        } else if (!pipelinedIndirect || arguments || count) {
            auto cause = !pipelinedIndirect ? DrawPipeline::DrainCause::IndirectDisabled : arguments ? DrawPipeline::DrainCause::IndirectArgs : DrawPipeline::DrainCause::IndirectCount;
            std::optional<DrawPipeline::Range> diagWriter;
            if (pipelinedIndirect && arguments) {
                // Count-only: which queued item writes the arguments (the next drain cut depends on it).
                DrawPipeline::Range overlapped{};
                if (const auto writer = pipeline.NewestWriter(indirect.arguments, argumentBytes, &overlapped)) {
                    cause = *writer == DrawPipeline::ItemKind::Draw ? DrawPipeline::DrainCause::IndirectArgsDraw : *writer == DrawPipeline::ItemKind::Dispatch ? DrawPipeline::DrainCause::IndirectArgsDispatch : *writer == DrawPipeline::ItemKind::Label ? DrawPipeline::DrainCause::IndirectArgsLabel : DrawPipeline::DrainCause::IndirectArgsOther;
                    if (*writer == DrawPipeline::ItemKind::Draw && PipeWriteDiag()) diagWriter = overlapped;
                }
            }
            // APS5_PIPE_WRITE_DIAG: the arguments as the CPU sees them before the queued writer commits
            // and after it committed and its GPU work was flushed (the reads wait for the GPU).
            std::vector<std::byte> diagBefore;
            const auto readArguments = [&] {
                std::vector<std::byte> bytes(argumentBytes);
                try {
                    GuestMemory::Read(indirect.arguments, bytes, 4);
                } catch (const std::exception&) {
                    bytes.clear();
                }
                return bytes;
            };
            if (diagWriter) diagBefore = readArguments();
            pipeline.Drain(DrawPipeline::DrainReason::Indirect, 0x100, cause);
            if (diagWriter) {
                const auto after = readArguments();
                NoteIndirectWriterOverlap(indirect.arguments, argumentBytes, *diagWriter, diagBefore.empty() || after.empty() ? std::optional<bool>{} : std::optional<bool>{diagBefore != after});
            }
        }
    }
    auto& traceIndirect = tail.traceIndirect;
    if (const auto verdict = precheckDraw(queue, submission, packetOffset, packet, drawParameters, rejected, traceIndirect)) {
        TraceDrawAttemptFailure(traceToken, "precheck", "rejected", submission.serial, submission.queue, packetOffset, rejected);
        return *verdict;
    }
    phaseTiming.Phase(DrawRowPrecheck);
    using Stage = ShaderRecompiler::ShaderStage;
    using Role = ShaderRecompiler::ProgramRole;

    static const bool dumpGraphics = std::getenv("APS5_DUMP_GRAPHICS_SHADERS") != nullptr;
    static const std::uint64_t dumpTarget = [] { const char* text = std::getenv("APS5_DUMP_DRAW_SHADERS"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();

    static const std::uint64_t dumpSlot1 = [] { const char* text = std::getenv("APS5_DUMP_DRAW_SLOT1"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();

    static const bool lockedPrepare = std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr;
    std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
    auto& localDevice = tail.localDevice;
    if (lockedPrepare) {

        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        gpuLock.lock();
        timing.Mark("gpu_mutex_wait");
        phaseTiming.Phase(DrawRowLockWait);
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;

        recordLabelsForPacket(localDevice.get(), submission.queue);
        phaseTiming.Phase(DrawRowLabels);
    } else if ((localDevice = device.Load()) == nullptr) {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        std::lock_guard createLock(GuestMemory::GpuMutex());
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;
    }
    timing.Mark("device_setup");
    phaseTiming.Phase(DrawRowVectors);

    const bool useDrawEntries = drawEntries() && !ShaderRecompiler::DebugProbeActive() && !dumpGraphics && dumpTarget == 0 && dumpSlot1 == 0;
    tail.ubPointerFree = ArchB::Switches::Enabled(ArchB::Feature::UbPointerFreeDrawKeys);
    tail.ubSkipCpuIndirect = ArchB::Switches::Enabled(ArchB::Feature::UbSkipCpuIndirect);
    tail.registerKey = useDrawEntries && registerKeyEnabled() && !tail.ubPointerFree;
    const bool registerKey = tail.registerKey;
    tail.liveOptions = {drawStageDataHits(), drawStageDataHits() && tail.bufferBases && !validateLegacy(), tail.feedbackMask && !validateLegacy(), tail.nestedRelocation};
    tail.liveEnabled = ArchB::Switches::Enabled(ArchB::Feature::DrawTemplatesLive);
    tail.liveVerify = tail.liveEnabled && LiveVerifyNext();
    tail.livePacket = drawParameters;
    if (tail.liveEnabled) {
        if (const char* skipped = !registerKey ? "draw_templates_live_q0_skipped_no_register_key" : !pipelined ? "draw_templates_live_q0_skipped_not_pipelined" : submission.queue != 0 ? "draw_templates_live_q0_skipped_other_queue" : !drawParameters.indirect && !ArchB::DirectDrawEnabled() ? "draw_templates_live_q0_skipped_direct_disabled" : lockedPrepare ? "draw_templates_live_q0_skipped_locked_prepare" : traceToken != 0 ? "draw_templates_live_q0_skipped_trace" : !deferredLabels().labels.empty() ? "draw_templates_live_q0_skipped_deferred_labels" : nullptr) CountLiveDraw(drawParameters, skipped);
    }
    if (ArchB::Switches::Enabled(ArchB::Feature::DrawGatherShadow) && registerKey && pipelined && submission.queue == 0 && (drawParameters.indirect || ArchB::DirectDrawEnabled()) && !lockedPrepare && traceToken == 0 && deferredLabels().labels.empty()) {
        tail.liveKey = LiveMix(LiveMix(LiveMix(RegisterStructureKey(queue), DrawShaderSetKey(queue, *submission.shaders)), localDevice->Serial()), LivePacketKey(drawParameters));
        if (tail.liveEnabled) tail.live = SelectLive(queue, submission, tail);
        if (RepeatCensus::Enabled()) RepeatCensus::NoteDraw(CensusDraw(tail, LiveMix(DrawShaderSetKey(queue, *submission.shaders), LivePacketKey(drawParameters))));
        if (tail.live != nullptr && !tail.liveVerify) {
            tail.programs = &tail.live->entry->decode->programs;
            tail.submissionQueue = submission.queue;
            tail.packetOpcode = (packet[0] >> 8u) & 0xffu;
            enqueueGatherDraw(tail);
            timing.Mark("draw_enqueued");
            return drawn();
        }
    } else if (submission.queue == 0 && RepeatCensus::Enabled()) {
        RepeatCensus::NoteDraw(CensusDraw(tail, 0));
    }

    auto& drawKey = tail.drawKey;
    std::array<std::uint64_t, 5> drawKeyComponents{};
    auto& entry = tail.entry;
    auto& decode = tail.decode;
    auto& dataEntry = tail.dataEntry;
    auto& shapeKey = tail.shapeKey;
    const auto packetKey = [&](std::uint64_t key) {
        if (key != 0 && drawParameters.indirect) {
            const auto& indirect = *drawParameters.indirect;
            for (const auto value : {static_cast<std::uint32_t>(drawParameters.indexed), indirect.baseVertexLocation, indirect.startInstanceLocation, static_cast<std::uint32_t>(indirect.drawIndexEnabled), indirect.drawIndexLocation}) key = (key ^ (static_cast<std::uint64_t>(value) + 1u)) * 0x9e3779b97f4a7c15ull;
        }
        return key;
    };
    std::uint64_t drawIdentity = 0, drawAlias = 0;
    std::vector<DrawUserPatch> userPatches;
    if (registerKey) {
        const auto keyStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const bool nestedRelocation = tail.nestedRelocation;
        std::shared_ptr<const DrawRelocation> relocation;
        bool aliasFirst = false;
        if (drawRelocationEnabled()) {
            if (DrawFinisher::Busy() && drawRelocationPending.key != 0) DrawFinisher::Get().Drain();
            drawIdentity = packetKey(drawIdentityKey(queue, *submission.shaders));
            std::lock_guard cacheLock(drawCacheMutex);
            learnPendingRelocation();
            const auto known = drawRelocations.find(drawIdentity);
            if (known != drawRelocations.end()) {
                auto& trial = drawRelocationTrials[drawIdentity];
                if (trial.idle != 0 && !nestedRelocation) --trial.idle;
                else relocation = known->second;
                aliasFirst = relocation != nullptr && trial.productive && !nestedRelocation;
            }
        }
        drawKey = drawRegisterKey(queue, *submission.shaders, localDevice->Serial(), &shapeKey, traceDrawCache() ? &drawKeyComponents : nullptr, aliasFirst ? relocation.get() : nullptr, aliasFirst ? &drawAlias : nullptr);
        drawKey = packetKey(drawKey);
        shapeKey = packetKey(shapeKey);
        drawAlias = packetKey(drawAlias);
        DrawPipeline::NoteDrawKey(drawKey, shapeKey);
        if (DrawFinisher::Busy()) {
            bool shapeMatters = false;
            if (DrawFinisher::Overlap() && (drawDataHits() || tail.feedbackMask)) {
                std::lock_guard peekLock(drawCacheMutex);
                const auto shape = drawShapes.find(shapeKey);
                shapeMatters = !drawCache.contains(drawKey) && (shape == drawShapes.end() || !drawCache.contains(shape->second));
            }
            const bool drain = !DrawFinisher::Overlap() || DrawFinisher::Get().PendingKey(drawKey, shapeMatters ? shapeKey : 0);
            if (drain) DrawFinisher::Get().Drain();
            if (DrawFinisher::Overlap()) DrawFinisher::NoteDrain(DrawFinisher::DrainSite::Lookup, drain);
        }
        std::unique_lock cacheLock(drawCacheMutex);
        ++drawEntryCounters.lookups;
        ++drawEntryCounters.registerKeyLookups;
        if (profile) drawEntryCounters.keyUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - keyStart).count();
        const auto found = drawCache.find(drawKey);
        if (found != drawCache.end()) {
            ArchB::Recorder::Count("draw_reloc_direct");
            entry = found->second;
            decode = entry->decode;
        } else {
            ++drawEntryCounters.absent;
            static const bool nestedCensus = std::getenv("APS5_DRAW_RELOC_NESTED_CENSUS") != nullptr;
            std::uint64_t censusAlias = 0;
            if (nestedCensus) {
                const auto known = drawRelocations.find(drawIdentity);
                if (known != drawRelocations.end()) {
                    drawRegisterKey(queue, *submission.shaders, localDevice->Serial(), nullptr, nullptr, known->second.get(), &censusAlias, true);
                    censusAlias = packetKey(censusAlias);
                    if (censusAlias == 0) ArchB::Recorder::Count("draw_nested_census_alias_unreadable");
                }
            }
            ArchB::Recorder::Count(!drawRelocationEnabled() ? "draw_reloc_absent_off" : relocation != nullptr ? "draw_reloc_absent_shape" : drawRelocations.contains(drawIdentity) ? "draw_reloc_absent_idle" : "draw_reloc_absent_no_shape");
            if (relocation != nullptr && !aliasFirst) {
                cacheLock.unlock();
                drawRegisterKey(queue, *submission.shaders, localDevice->Serial(), nullptr, nullptr, relocation.get(), &drawAlias, nestedRelocation);
                drawAlias = packetKey(drawAlias);
                cacheLock.lock();
            }
            if (drawIdentity != 0) drawRelocationPending = {drawKey, drawAlias, drawIdentity, censusAlias, nestedRelocation};
            if (drawAlias != 0) relocateDrawEntry(queue, drawAlias, entry, decode, userPatches, nestedRelocation);
            else if (relocation != nullptr) ArchB::Recorder::Count("draw_reloc_alias_zero");
            ArchB::Recorder::Count(entry != nullptr ? "draw_reloc_entry" : "draw_reloc_none");
            if (nestedCensus && drawRelocations.contains(drawIdentity)) {
                if (entry == nullptr) censusNestedDraw(queue, censusAlias);
                else ArchB::Recorder::Count("draw_nested_census_existing_entry");
            }
            if (relocation != nullptr) {
                auto& trial = drawRelocationTrials[drawIdentity];
                ++trial.lookups;
                if (trial.lookups >= DrawRelocationTrial::Window) {
                    trial.productive = trial.hits * DrawRelocationTrial::MinimumHitShare >= trial.lookups;
                    if (!trial.productive) trial.idle = DrawRelocationTrial::IdleDraws;
                    trial.lookups = trial.hits = 0;
                }
            }
            if (traceDrawCache()) {
                auto& counters = drawEntryCounters;
                if (traceKeysEvicted.contains(drawKey)) ++counters.absentEvicted;
                else if (traceKeysSeen.contains(drawKey)) ++counters.absentNeverInserted;
                else if (traceShapes.contains(shapeKey)) ++counters.absentUserWords;
                else ++counters.absentNewRegisters;
            }
            if (decode == nullptr && (drawDataHits() || tail.feedbackMask) && !drawParameters.indirect) {
                if (const auto shape = drawShapes.find(shapeKey); shape != drawShapes.end()) {
                    const auto kept = drawCache.find(shape->second);
                    if (kept != drawCache.end() && kept->second->decode != nullptr && kept->second->shape == shapeKey) dataEntry = kept->second;
                }
                if (dataEntry != nullptr) ++drawEntryCounters.dataHits;
            }
        }
        if (traceDrawCache()) traceDrawKeyLookup(drawKeyComponents, found == drawCache.end());
        if (traceDrawCache()) {
            traceInsert(traceKeysSeen, drawKey);
            traceInsert(traceShapes, shapeKey);
        }
        maybeReportDrawCache(profile);
    }
    if (decode != nullptr && !DecodeProgramsCurrent(*decode, *submission.shaders)) {
        userPatches.clear();
        std::lock_guard cacheLock(drawCacheMutex);
        eraseDrawEntry(drawKey, entry);
        ++drawEntryCounters.absent;
        entry = nullptr;
        decode = nullptr;
    }
    if (dataEntry != nullptr && !DecodeProgramsCurrent(*dataEntry->decode, *submission.shaders)) dataEntry = nullptr;
    phaseTiming.Phase(DrawRowKeyLookupValidate);

    if (dataEntry != nullptr) {
        auto refreshed = std::make_shared<DrawDecode>(*dataEntry->decode);
        refreshed->stateOrigin = dataEntry->decode->stateOrigin != nullptr ? dataEntry->decode->stateOrigin : dataEntry->decode;
        for (auto& program : refreshed->programs) {
            ReadUserWords(queue, program, false);
            if (program.merged) InitializeMerged(queue, program, program.mergedPointer, program.mergedPointerRequired, false);
        }
        if (verifyDrawDataHits()) {
            const auto full = decodeDraw(queue, submission, localDevice->graphicsContext());
            if (!sameDecode(*refreshed, *full)) {
                std::size_t differing = 0;
                while (differing < full->programs.size() && differing < refreshed->programs.size() && full->programs[differing].userData == refreshed->programs[differing].userData) ++differing;
                char message[320];
                std::snprintf(message, sizeof(message), "APS5_VERIFY_DRAW_DATA_HITS: a data-only hit's decode differs from the full decode (key 0x%llx, shape 0x%llx, %zu / %zu programs, first with differing user words %zu, path %u / %u, target 0x%llx / 0x%llx)", static_cast<unsigned long long>(drawKey), static_cast<unsigned long long>(shapeKey), refreshed->programs.size(), full->programs.size(), differing, static_cast<unsigned>(refreshed->state.stages.path), static_cast<unsigned>(full->state.stages.path), static_cast<unsigned long long>(refreshed->state.color.address), static_cast<unsigned long long>(full->state.color.address));
                throw std::runtime_error(message);
            }
        }
        decode = std::move(refreshed);
    }
    if (decode != nullptr && !userPatches.empty() && verifyDrawRecipe()) {
        auto patched = std::make_shared<DrawDecode>(*decode);
        for (const auto& patch : userPatches) patched->programs.at(patch.program).userData.at(patch.index) = patch.value;
        decode = std::move(patched);
    }
    try {
        resolveDrawDecode(queue, submission, localDevice->graphicsContext(), decode, registerKey, drawKey, profile);
    } catch (...) {
        TraceDrawAttemptFailure(traceToken, "decode", "exception", submission.serial, submission.queue, packetOffset, ExceptionReason(std::current_exception()));
        throw;
    }
    const auto& graphics = decode->state;
    auto& pixel = tail.pixel;
    pixel = decode->pixel;
    const bool writesPrograms = graphics.stages.mesh || drawParameters.indirect || !userPatches.empty();
    auto& writablePrograms = tail.writablePrograms;
    if (writesPrograms) writablePrograms = decode->programs;
    for (const auto& patch : userPatches) writablePrograms[patch.program].userData[patch.index] = patch.value;
    tail.programs = writesPrograms ? &writablePrograms : &decode->programs;
    const auto& programs = *tail.programs;
    if (ArchB::Recorder::Enabled()) {
        tail.recorderKey = RecorderProgramKey(programs);
        recorderWork.SetKey(tail.recorderKey);
    }
    if (std::any_of(programs.begin(), programs.end(), [](const DrawProgram& program) { return SkippedShader(program.binary.codeAddress); })) return drawn();
    static const bool fromBase = std::getenv("APS5_NO_INDIRECT_BUILTIN_OFFSETS") == nullptr;
    const bool emptyIndirectSkip = !tail.ubSkipCpuIndirect && EmptyIndirectSkip() && !(fromBase && drawParameters.indexed);
    auto& cpuIndirectPrograms = CpuIndirectPrograms();
    const auto recordDraws = [&](const Pm4::DrawArguments& arguments) { return arguments.count != 0 && arguments.instances != 0 && (!drawParameters.indexed || !drawParameters.indirect->indexLimitKnown || arguments.firstVertexOrIndex < drawParameters.indexCount); };
    if (emptyIndirectSkip && drawParameters.indirect && !programs.empty()) {
        const auto known = cpuIndirectPrograms.find(programs.front().binary.codeAddress);
        if (known != cpuIndirectPrograms.end() && known->second) {
            const auto& indirect = *drawParameters.indirect;
            if (tail.indirectGpuArgs) drainBeforeArgumentRead(indirect, DrawPipeline::DrainCause::IndirectEmptySkip);
            recordQueuedLabelsBeforeRead(submission.queue);
            const auto readStart = std::chrono::steady_clock::now();
            const auto count = std::min(indirect.countIndirect ? Pm4::ReadDrawCount(indirect) : indirect.count, indirect.count);
            bool any = false;
            for (std::uint32_t record = 0; record < count && !any; ++record) any = recordDraws(Pm4::ReadDrawArguments(indirect, record));
            if (!any) {
                Graphics::CountEmptyIndirectDraw(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count());
                timing.Mark("empty_indirect_skipped");
                return drawn();
            }
        }
    }
    const auto setMeshIndexBuffer = [&](const Pm4::DrawParameters& parameters) {
        if (!graphics.stages.mesh) return;
        auto& words = writablePrograms.front().userData;
        require(programs.front().firstUserSgpr == 0 && words.size() >= ShaderRecompiler::MeshIndexBufferUserWord + 4, "mesh program lacks the hidden user words");
        const auto descriptor = Graphics::MeshIndexBufferDescriptor(parameters);
        std::copy(descriptor.begin(), descriptor.end(), words.begin() + ShaderRecompiler::MeshIndexBufferUserWord);
    };
    if (!drawParameters.indirect) setMeshIndexBuffer(drawParameters);
    else if (graphics.stages.mesh) setMeshIndexBuffer(Pm4::DrawParameters{drawParameters.indexAddress, std::max(drawParameters.indexCount, 1u), drawParameters.indexSize, 1, 0, drawParameters.indexed});
    const std::vector<Role>& roles = decode->roles;
    static const bool tracePrograms = std::getenv("APS5_TRACE_DRAW_PROGRAMS") != nullptr;
    if (tracePrograms) {
        std::vector<DrawProgramTraceEntry> identities;
        identities.reserve(programs.size());
        for (std::size_t i = 0; i < programs.size(); ++i) {
            identities.push_back({programs[i].binary.stage, roles[i], programs[i].snapshot->codeAddress, programs[i].binary.codeAddress});
        }
        const auto line = FormatDrawProgramTrace(submission.serial, submission.queue, identities);
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    phaseTiming.Phase(DrawRowDecode);

    const auto locate = [&](std::uint32_t location) -> std::optional<std::pair<std::size_t, std::size_t>> {
        const auto words = UserDataWords(programs, roles, location);
        if (words.empty()) return std::nullopt;
        return words.front();
    };
    if (drawParameters.indirect) {
        auto& indirect = *drawParameters.indirect;
        const auto sgprOf = [&](std::uint32_t location) -> std::int32_t {
            const auto word = locate(location);
            if (!word || word->first != 0) return -1;
            return static_cast<std::int32_t>(programs.front().firstUserSgpr + word->second);
        };
        indirect.baseVertexSgpr = sgprOf(indirect.baseVertexLocation);
        indirect.startInstanceSgpr = sgprOf(indirect.startInstanceLocation);
        indirect.drawIndexSgpr = sgprOf(indirect.drawIndexLocation);
        constexpr std::uint32_t NoLocation = 0x280u;
        const auto& front = programs.front();
        const bool plain = fromBase && drawParameters.indexed && !indirect.drawIndexEnabled && front.binary.stage == Stage::Vertex && !front.merged && graphics.stages.path == Graphics::ShaderPath::Vertex && !graphics.stages.mesh && !graphics.stages.tessellation && !graphics.rectList &&
                           localDevice->DrawIndirectSupport().drawParameters && Graphics::DecodeVertexStageInfo(front.binary.header, front.binary.headerAddress, front.userData, nullptr, false, 8u - front.firstUserSgpr).resourcesNum == 0;
        const auto frontOnly = [&](std::uint32_t location, std::int32_t sgpr) { return location == NoLocation || (sgpr >= 0 && UserDataWords(programs, roles, location).size() == 1); };
        indirect.instanceFromBase = plain && frontOnly(indirect.startInstanceLocation, indirect.startInstanceSgpr);
        indirect.vertexFromBase = plain && frontOnly(indirect.baseVertexLocation, indirect.baseVertexSgpr);
    }
    if (DrawFinisher::Busy()) {
        const bool drain = !DrawFinisher::Overlap();
        if (drain) DrawFinisher::Get().Drain();
        else DrawFinisher::NoteDrain(DrawFinisher::DrainSite::Reads, false);
    }
    auto& memory = tail.memory;
    auto& linked = tail.linked;
    memory.reserve(2 * programs.size() + 32);
    AppendProgramMemory(memory, programs);
    linked.reserve(programs.size());
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        linked.push_back({roles[i], program.binary, program.userDataBase, program.firstUserSgpr, program.userData});
    }
    timing.Mark("prepare");
    phaseTiming.Phase(DrawRowProgramPrepare);

    auto& vertexInfos = tail.vertexInfos;
    vertexInfos.resize(programs.size());
    auto& decodeReads = tail.decodeReads;
    decodeReads.resize(programs.size());
    const auto decodeVertexInfo = [&](std::size_t i) {
        const auto& program = programs[i];
        if (program.binary.stage == Stage::Fragment || roles[i] == Role::GeometryBack) return;
        vertexInfos[i] = DecodeOrderedVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData, decodeReads[i], 8u - program.firstUserSgpr);
        if (vertexInfos[i]) {
            vertexInfos[i]->paClVsOutCntl = PositionExportLayout(program.snapshot.get(), program.codeOffset, program.binary.code, readRegister(queue.context, 0x207), program.tailContinuation.get()) | graphics.layerExports;
            vertexInfos[i]->depthRemapMode = graphics.depthRemap.mode;
            Graphics::ApplyPointSize(graphics, *vertexInfos[i]);
        }
    };
    if (!registerKey) {
        for (std::size_t i = 0; i < programs.size(); ++i) decodeVertexInfo(i);
        phaseTiming.Phase(DrawRowDecode);
    }
    auto& shaderMemory = tail.shaderMemory;
    shaderMemory = std::make_unique<ShaderMemory>(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter(), &QueryPendingSpan, tail.feedbackMask || tail.bufferBases || tail.nestedRelocation);
    shaderMemory->SetImageTableSnapshotReuse(tail.imageTableSnapshotReuse);
    shaderMemory->SetImageTableBitScanKeys(tail.imageTableBitScanKeys);
    auto& results = tail.results;
    auto& compiledStages = tail.compiledStages;
    compiledStages.reserve(programs.size());
    auto& stages = tail.stages;
    stages.reserve(programs.size());
    auto& pushCursorBytes = tail.pushCursorBytes;

    auto& programResults = tail.programResults;
    programResults.assign(programs.size(), nullptr);

    auto& stageCaptures = tail.stageCaptures;
    stageCaptures.resize(programs.size());
    auto& matched = tail.matched;
    matched.resize(programs.size());
    auto& matchedRegions = tail.matchedRegions;
    matchedRegions.resize(programs.size());

    auto& fresh = tail.fresh;
    fresh.resize(programs.size());

    auto& recompiled = tail.recompiled;
    recompiled.assign(programs.size(), false);
    auto& dataCandidates = tail.dataCandidates;
    dataCandidates.resize(programs.size());
    auto& reused = tail.reused;
    reused.assign(programs.size(), false);
    auto& stageData = tail.stageData;
    stageData.resize(programs.size());
    auto& drawHit = tail.drawHit;
    auto& verifyHit = tail.verifyHit;
    tail.stageEntries = useDrawEntries && !(graphics.stages.tessellation && !drawParameters.indirect && drawParameters.instanceCount > 1u);
    const bool stageEntries = tail.stageEntries;
    std::array<std::uint64_t, static_cast<std::size_t>(DrawMiss::Count)> missesBefore{};
    static const std::uint64_t censusShader = [] {
        const char* text = std::getenv("APS5_IMAGE_TABLE_CENSUS_SHADER");
        return text != nullptr && std::getenv("APS5_IMAGE_TABLE_CENSUS") != nullptr ? std::strtoull(text, nullptr, 16) : 0ull;
    }();
    std::size_t censusStage = programs.size();
    for (std::size_t i = 0; censusShader != 0u && i < programs.size(); ++i) {
        if (programs[i].binary.codeAddress == censusShader) censusStage = i;
    }
    std::array<std::uint64_t, static_cast<std::size_t>(DrawMiss::Count)> censusMisses{};
    if (DrawShaderProfileEnabled() || censusStage < programs.size()) {
        std::lock_guard cacheLock(drawCacheMutex);
        missesBefore = drawEntryCounters.misses;
        censusMisses = drawEntryCounters.misses;
    }
    tail.gatherShadow = stageEntries && ArchB::Switches::Enabled(ArchB::Feature::DrawGatherShadow) && (!tail.liveEnabled || tail.liveVerify);
    struct GatherLogScope {
        bool armed;
        explicit GatherLogScope(bool on) : armed(on) {
            if (armed) ArmGatherValidationLog(true);
        }
        ~GatherLogScope() {
            if (armed) ArmGatherValidationLog(false);
        }
    } gatherLogScope(tail.gatherShadow);
    try {
        lookupDraw(submission, localDevice, graphics, pixel, programs, roles, vertexInfos, stageEntries, registerKey, profile, drawKey, entry, matched, matchedRegions, drawHit, verifyHit, phaseTiming, phaseMs, dataEntry, dataCandidates, stageData, shapeKey, drawParameters, tail.ubPointerFree, tail.feedbackMask, tail.bufferBases);
    } catch (...) {
        TraceDrawAttemptException(traceToken, "lookup", submission.serial, submission.queue, packetOffset, std::current_exception());
        throw;
    }
    if (censusStage < programs.size()) {
        const char* reason = drawHit ? "hit" : verifyHit ? "verify" : entry == nullptr ? "absent" : "miss-other";
        {
            std::lock_guard cacheLock(drawCacheMutex);
            constexpr std::array<const char*, static_cast<std::size_t>(DrawMiss::Count)> names{"front", "fragment", "other", "queued-stores", "layout", "gate", "stages"};
            for (std::size_t r = 0; r < censusMisses.size(); ++r) {
                if (drawEntryCounters.misses[r] != censusMisses[r]) reason = names[r];
            }
        }
        struct CensusTag {};
        auto& seen = HostThreadLocal<std::unordered_map<std::uint64_t, std::vector<std::uint32_t>>, CensusTag>();
        const auto& user = programs[censusStage].userData;
        std::string userDiff;
        if (const auto was = seen.find(censusShader); was != seen.end()) {
            for (std::size_t w = 0; w < std::max(user.size(), was->second.size()); ++w) {
                const auto now = w < user.size() ? user[w] : 0u, then = w < was->second.size() ? was->second[w] : 0u;
                if (now == then) continue;
                char text[48];
                std::snprintf(text, sizeof(text), " u%zu:%x->%x", w, then, now);
                userDiff += text;
            }
        } else {
            userDiff = " (first)";
        }
        seen[censusShader] = user;
        std::string stagesText;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            char text[48];
            std::snprintf(text, sizeof(text), "%s%llx/r%u", i == 0 ? "" : ",", static_cast<unsigned long long>(programs[i].binary.codeAddress), static_cast<unsigned>(roles[i]));
            stagesText += text;
        }
        std::fprintf(stderr, "[its-draw] serial=%llu queue=0x%x packet=0x%zx indirect=%u stage=%zu stages=%s key=%llx outcome=%s user=%zu diff:%s\n", static_cast<unsigned long long>(submission.serial), submission.queue, packetOffset, drawParameters.indirect ? 1u : 0u, censusStage, stagesText.c_str(), static_cast<unsigned long long>(drawKey), reason, user.size(), userDiff.c_str());
        if (!drawHit && entry != nullptr && censusStage < entry->stages.size()) {
            for (std::size_t rank = 0; rank < entry->stages[censusStage].size(); ++rank) {
                const auto& variant = *entry->stages[censusStage][rank];
                std::vector<ShaderRecompiler::MemoryRegion> regions;
                appendEntryRegions(variant, regions);
                std::uint64_t bytes = 0, differing = 0;
                std::string first;
                for (const auto& region : regions) {
                    bytes += region.bytes.size();
                    if (!GuestMemory::Accessible(reinterpret_cast<const void*>(region.guestAddress), region.bytes.size())) {
                        ++differing;
                        if (first.size() < 600) first += " [unmapped " + std::to_string(region.bytes.size()) + "B]";
                        continue;
                    }
                    const auto* live = reinterpret_cast<const std::uint32_t*>(region.guestAddress);
                    const auto* kept = reinterpret_cast<const std::uint32_t*>(region.bytes.data());
                    std::size_t words = 0, firstWord = 0;
                    for (std::size_t w = 0; w < region.bytes.size() / 4u; ++w) {
                        if (live[w] == kept[w]) continue;
                        if (words++ == 0) firstWord = w;
                    }
                    if (words == 0) continue;
                    ++differing;
                    if (first.size() < 600) {
                        char text[128];
                        std::snprintf(text, sizeof(text), " [0x%llx+0x%zx: %zu words, first +0x%zx %x->%x]", static_cast<unsigned long long>(region.guestAddress), region.bytes.size(), words, firstWord * 4u, kept[firstWord], live[firstWord]);
                        first += text;
                    }
                }
                std::fprintf(stderr, "[its-draw-variant] serial=%llu packet=0x%zx rank=%zu regions=%zu bytes=%llu differing=%llu user=%zu%s\n", static_cast<unsigned long long>(submission.serial), packetOffset, rank, regions.size(), static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(differing), variant.userData.size(), first.c_str());
            }
        }
    }
    if (DrawShaderProfileEnabled()) {
        std::lock_guard cacheLock(drawCacheMutex);
        tail.cacheOutcome = drawHit ? CacheHit : !registerKey ? CacheNoRegisterKey : entry == nullptr ? CacheNoEntry : CacheMissStages;
        constexpr std::array<DrawCacheOutcome, static_cast<std::size_t>(DrawMiss::Count)> missOutcomes{CacheMissFront, CacheMissFragment, CacheMissOther, CacheMissOther, CacheMissLayout, CacheMissGate, CacheMissStages};
        for (std::size_t reason = 0; reason < missesBefore.size(); ++reason) {
            if (!drawHit && drawEntryCounters.misses[reason] != missesBefore[reason]) tail.cacheOutcome = missOutcomes[reason];
        }
    }
    for (std::size_t i = 0; i < matched.size(); ++i) {
        if (matched[i] != nullptr) stageCaptures[i].registeredViews = matched[i]->registeredViews;
        if (matched[i] == nullptr || (matched[i]->shaderDataPatches.empty() && matched[i]->descriptorPatches.empty())) continue;
        applyShaderDataPatches(*matched[i]);
        if (stageData[i].compiled != nullptr) stageData[i].compiled = patchDataWords(*matched[i], stageData[i].words, stageData[i].bufferBaseHit);
    }
    tail.liveEntry = LearnLive(queue, tail);
    if (tail.live == nullptr && tail.liveEntry != nullptr) LiveIndex().sequence.Record(tail.livePosition, tail.liveEntry);
    if (tail.live == nullptr && tail.liveEntry != nullptr && tail.liveUnmatchedKey != 0 && ArchB::Switches::Enabled(ArchB::Feature::DrawTemplatesLiveIndex)) {
        IndexLiveSignature(tail.liveUnmatchedKey, tail.liveEntry);
        CountLiveDraw(tail.drawParameters, "draw_templates_live_index_aliases");
    }
    if (tail.gatherShadow || (tail.liveEntry != nullptr && !tail.liveEntry->link->ready.load(std::memory_order_acquire))) {
        GatherShadowDraw shadow;
        shadow.programs = programs;
        shadow.entry = entry.get();
        shadow.matched = matched;
        shadow.hit = drawHit || verifyHit;
        shadow.keyed = registerKey;
        shadow.options = {drawStageDataHits(), drawStageDataHits() && tail.bufferBases && !validateLegacy(), tail.feedbackMask && !validateLegacy(), tail.nestedRelocation};
        shadow.indirect = GatherIndirectOf(drawParameters);
        shadow.limits = GatherLimitsOf(*localDevice, graphics);
        shadow.live = tail.liveEnabled && !tail.liveVerify;
        shadow.commitTemplate = tail.liveEntry != nullptr ? tail.liveEntry->link : nullptr;
        tail.gatherPlan = RunGatherShadow(shadow);
        if (tail.liveEntry != nullptr && tail.gatherPlan != nullptr && tail.gatherPlan->matched && ArchB::Switches::Enabled(ArchB::Feature::DrawTemplatesLiveIndex)) IndexLiveSignature(LiveSignatureKey(tail.liveKey, tail.gatherPlan->signatures), tail.liveEntry);
        ArmGatherValidationLog(false);
        gatherLogScope.armed = false;
    }
    const auto relocatedDecode = decode;
    if (!userPatches.empty()) {
        std::lock_guard cacheLock(drawCacheMutex);
        if (drawHit || verifyHit) {
            ++drawRelocationCounters.hits;
            ++drawRelocationTrials[drawIdentity].hits;
            auto& aliases = tail.nestedRelocation ? nestedDrawAliases : drawAliases;
            if (const auto alias = aliases.find(drawAlias); alias != aliases.end()) {
                const auto donor = drawCache.find(alias->second);
                if (donor != drawCache.end() && drawCacheHits - donor->second->touched > drawCacheEntries() / 8) {
                    drawOrder.splice(drawOrder.begin(), drawOrder, donor->second->order);
                    donor->second->touched = drawCacheHits;
                    ++drawEntryCounters.touches;
                }
            }
        }
        if (profile && drawRelocationCounters.lookups >= 100000) reportDrawRelocation();
    }
    if (!userPatches.empty() && !drawHit) {
        auto current = std::make_shared<DrawDecode>(*decode);
        current->stateOrigin = decode->stateOrigin != nullptr ? decode->stateOrigin : decode;
        for (const auto& patch : userPatches) current->programs[patch.program].userData[patch.index] = patch.value;
        decode = std::move(current);
    }
    tail.patchedStages = std::any_of(stageData.begin(), stageData.end(), [](const StageDataHit& hit) { return hit.compiled != nullptr; });
    tail.relocatedDecode = relocatedDecode;
    std::uint64_t stageDataVerified = 0;
    const auto target = graphics.hasColorTarget ? graphics.color.address : 0;
    const VulkanDevice::DrawTraceContext trace{traceToken, submission.serial, submission.queue, packetOffset, target};
    const auto* tracePtr = trace.token != 0 ? &trace : nullptr;
    tail.traceToken = traceToken;
    tail.trace = trace;
    if (trace.token != 0) {
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& program = programs[i];
            AgcDriver::CaptureTrace::LogBestEffort("draw-selected token=%llu submission=%llu queue=%x offset=%zu snapshot=0x%llx code=%llx entryOffset=%zu entry=%llx stage=%u role=%u drawCacheHit=%u variantHit=%u target=%llx",
                    static_cast<unsigned long long>(trace.token), static_cast<unsigned long long>(submission.serial), submission.queue, packetOffset,
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(program.snapshot.get())), static_cast<unsigned long long>(program.snapshot->codeAddress), program.codeOffset,
                    static_cast<unsigned long long>(program.snapshot->codeAddress + program.codeOffset), static_cast<unsigned>(program.binary.stage), static_cast<unsigned>(roles[i]),
                    drawHit ? 1u : 0u, matched[i] != nullptr ? 1u : 0u, static_cast<unsigned long long>(target));
        }
    }

    if (registerKey) {

        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (matched[i] != nullptr && drawHit && !verifyDrawRecipe()) {
                if (matched[i]->vertexInfo != nullptr) vertexInfos[i] = *matched[i]->vertexInfo;
                continue;
            }
            if (!dataCandidates[i].empty() && !verifyDrawDataHits()) {
                const auto& info = dataCandidates[i].front().variant->vertexInfo;
                if (info != nullptr) vertexInfos[i] = *info;
                continue;
            }
            decodeVertexInfo(i);
            if (matched[i] != nullptr && verifyDrawRecipe() && (vertexInfos[i].has_value() != (matched[i]->vertexInfo != nullptr) || (vertexInfos[i] && !sameVertexInfo(*vertexInfos[i], *matched[i]->vertexInfo)))) {
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: stage %zu (program 0x%llx) of a hit has a vertex stage info unlike its variant's\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress));
                std::lock_guard cacheLock(drawCacheMutex);
                ++drawEntryCounters.verifyDecodeMismatches;
            }
        }
        phaseTiming.Phase(DrawRowDecode);
    }

    tail.submission = &submission;
    tail.submissionQueue = submission.queue;
    tail.packetOpcode = (packet[0] >> 8u) & 0xffu;
    tail.reg207 = readRegister(queue.context, 0x207);
    tail.dumpTarget = dumpTarget;
    tail.dumpSlot1 = dumpSlot1;
    tail.packetOffset = packetOffset;
    const bool handOff = pipelined && !drawParameters.indirect && !profile && !lockedPrepare && !dumpGraphics && dumpTarget == 0 && dumpSlot1 == 0 && !tracePrograms && !verifyDrawDataHits() && deferredLabels().labels.empty() && DrawFinisher::Enabled();
    if (handOff) {
        tail.epoch = DrawPipeline::EpochToken().load(std::memory_order_relaxed);
        tail.ticket = GuestMemory::Gen::Required();
        tail.submissionSerial = submission.serial;
        tail.packetOffset = static_cast<std::size_t>(packet.data() - submission.commands.data());
        noteFinisherHandOff(true);
        DrawFinisher::Pending held;
        if (DrawFinisher::Overlap()) held = heldWrites(tail);
        DrawFinisher::Get().Enqueue([this, tailHolder] {
            auto& job = *tailHolder;
            const HostAllocator::Scope hostSlabScope(job.hostSlabAlloc);
            const ArchB::CheapClock::Scope cheapTimersScope(job.cheapTimers);
            const ArchB::AllocChurnScope allocChurnScope(job.allocChurn);
            const ArchB::PartialSnapshotScope partialSnapshotScope(job.partialSnapshot);
            DrawPipeline::FollowEpoch(job.epoch, job.ticket);
            GuestMemory::SetCurrentPacket(job.packetOpcode, job.submissionQueue);
            ArchB::Recorder::Work recorderWork(job.recorderKey, ArchB::Recorder::Phase::Prepare, false);
            PerformanceTimer finishTiming("Driver.DrawFinish");
            try {
                if (finishDraw(job, finishTiming) == DrawVerdict::Rejected) throw std::runtime_error(job.ownRejected);
            } catch (const RefusedDraw& refused) {
                CaptureTrace::Log("draw-refused submission=%llu offset=%zu finisher=1 reason=%.256s", static_cast<unsigned long long>(job.submissionSerial), job.packetOffset, refused.what());
                ReportRefusedDraw(refused);
            } catch (const std::exception& error) {
                CaptureTrace::Log("draw-error submission=%llu offset=%zu finisher=1 reason=%.256s", static_cast<unsigned long long>(job.submissionSerial), job.packetOffset, error.what());
                throw std::runtime_error("draw at submission " + std::to_string(job.submissionSerial) + " offset " + std::to_string(job.packetOffset) + " (finished on the draw finisher): " + error.what());
            }
        }, std::move(held));
        timing.Mark("draw_handed_off");
        return drawn();
    }
    noteFinisherHandOff(false);
    if (DrawFinisher::Busy()) {
        DrawFinisher::Get().Drain();
        if (DrawFinisher::Overlap()) DrawFinisher::NoteDrain(DrawFinisher::DrainSite::Kept, true);
    }
    tail.queue = &queue;
    tail.packet = packet;
    tail.rejected = &rejected;
    tail.gpuLock = &gpuLock;
    return finishDraw(tail, timing);
}

void Driver::enqueueGatherDraw(DrawTail& tail) {
    const auto live = tail.live;
    CountLiveDraw(live->parameters, "draw_templates_live_enqueued");
    const auto link = live->entry->link;
    auto writes = queuedDrawWrites(tail, *link->state, live->stages);
    auto reads = QueuedDrawReadRanges(live->parameters, live->stages);
    for (const auto& stage : live->plan->stages) {
        for (const auto& range : stage.reads) reads.push_back(range);
        for (const auto& value : stage.vsharps) reads.push_back({value.address - value.adjustment, value.address - value.adjustment + value.viewBytes});
    }
    const auto epoch = DrawPipeline::EpochToken().load(std::memory_order_relaxed);
    const auto ticket = GuestMemory::Gen::Required();
    auto reference = live->verify ? tail.self.lock() : nullptr;
    auto commit = [this, live, link, reference, localDevice = tail.localDevice, commitQueue = tail.submissionQueue, commitPacket = tail.packetOpcode, epoch, ticket, pipelineStateReuse = tail.pipelineStateReuse, memoVirtualQuery = tail.memoVirtualQuery, hostSlabAlloc = tail.hostSlabAlloc, cheapTimers = tail.cheapTimers, allocChurn = tail.allocChurn, partialSnapshot = tail.partialSnapshot, recorderKey = tail.recorderKey] {
        const HostAllocator::Scope hostSlabScope(hostSlabAlloc);
        const ArchB::CheapClock::Scope cheapTimersScope(cheapTimers);
        const ArchB::AllocChurnScope allocChurnScope(allocChurn);
        const ArchB::PartialSnapshotScope partialSnapshotScope(partialSnapshot);
        ArchB::Recorder::Work recorderWork(recorderKey, ArchB::Recorder::Phase::Record, false);
        PerformanceTimer timing("Driver.DrawCommit");
        DrawPipeline::FollowEpoch(epoch, ticket);
        GuestMemory::SetCurrentPacket(commitPacket, commitQueue);
        Graphics::DrawSlotIdentity identity;
        identity.state = link->state;
        identity.gatherTemplate = link;
        identity.gather = live->plan.get();
        for (const auto& stage : link->stages) {
            Graphics::DrawSlotStage owner;
            owner.owner = stage.compiled;
            owner.variant = stage.variant;
            identity.stages.push_back(std::move(owner));
        }
        std::vector<Graphics::DrawReferenceMasks> referenceMasks;
        if (reference != nullptr) {
            referenceMasks.resize(link->stages.size());
            for (std::size_t i = 0; i < referenceMasks.size(); ++i) {
                const auto add = [&](const DispatchVariant* variant) {
                    if (variant == nullptr) return;
                    auto& masks = referenceMasks[i];
                    if (masks.bindings.size() < variant->feedback.bindings.size()) masks.bindings.resize(variant->feedback.bindings.size());
                    for (std::size_t binding = 0; binding < variant->feedback.bindings.size(); ++binding) MergeIgnoredWords(masks.bindings[binding], variant->feedback.bindings[binding]);
                    MergeIgnoredWords(masks.push, variant->feedback.pushConstants);
                };
                if (i < live->entry->variants.size()) add(live->entry->variants[i].get());
                if (i < reference->matched.size() && reference->matched[i] != nullptr) {
                    add(reference->matched[i].get());
                    add(reference->matched[i]->donor.get());
                }
            }
            identity.referenceMasks = referenceMasks;
            identity.referenceStages = reference->stages;
            identity.referenceDraw = &reference->drawParameters;
            for (std::size_t i = 0; i < live->plan->stages.size() && i < reference->stages.size(); ++i) live->plan->stages[i].program = reference->stages[i].program;
            CheckGatherDrawPlan(*live->plan, reference->stages, GatherTargetAddress(*link->state), GatherIndirectOf(reference->drawParameters));
        }
        const auto commitStarted = std::chrono::steady_clock::now();
        commitDraw(localDevice, commitQueue, *link->state, live->parameters, live->stages, {}, nullptr, {}, 0, nullptr, {}, 0, pipelineStateReuse, memoVirtualQuery, nullptr, &identity);
        CountLiveDraw(live->parameters, "draw_commit_live_ns", static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - commitStarted).count()));
        CountLiveDraw(live->parameters, "draw_commit_live_n");
    };
    auto held = std::make_shared<decltype(commit)>(std::move(commit));
    DrawPipeline::Queue0().Enqueue(DrawPipeline::CommitKind::Draw, [held] { (*held)(); }, std::move(writes), 0, {}, std::move(reads), true, DrawPipeline::ItemKind::Draw);
}

DrawVerdict Driver::finishDraw(DrawTail& tail, PerformanceTimer& timing) {
    using Stage = ShaderRecompiler::ShaderStage;
    using Role = ShaderRecompiler::ProgramRole;
    const bool profile = tail.profile;
    const auto traceToken = tail.traceToken;
    const auto& trace = tail.trace;
    const auto* tracePtr = trace.token != 0 ? &trace : nullptr;
    const auto packetOffset = tail.packetOffset;
    const auto packet = tail.packet;
    auto& phaseMs = tail.phaseMs;
    auto& phaseLap = tail.phaseLap;
    auto& captures = tail.captures;
    auto& phaseTiming = tail.phaseTiming;
    auto& drawParameters = tail.drawParameters;
    const bool pipelined = tail.pipelined;
    static const bool pipelinedIndirect = std::getenv("APS5_NO_PIPELINED_INDIRECT") == nullptr;
    const auto drainBeforeInline = [&](DrawPipeline::DrainCause cause) {
        if (pipelined && DrawPipeline::Queue0().Busy()) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Indirect, 0x100, cause);
    };
    auto& traceIndirect = tail.traceIndirect;
    const auto dumpTarget = tail.dumpTarget;
    const auto dumpSlot1 = tail.dumpSlot1;
    auto& gpuLock = tail.gpuLock != nullptr ? *tail.gpuLock : tail.ownLock;
    auto& localDevice = tail.localDevice;
    const bool registerKey = tail.registerKey;
    auto& drawKey = tail.drawKey;
    auto& entry = tail.entry;
    auto& decode = tail.decode;
    auto& dataEntry = tail.dataEntry;
    auto& shapeKey = tail.shapeKey;
    const auto& graphics = decode->state;
    auto& pixel = tail.pixel;
    auto& writablePrograms = tail.writablePrograms;
    const auto& programs = *tail.programs;
    const std::vector<Role>& roles = decode->roles;
    auto& memory = tail.memory;
    auto& linked = tail.linked;
    auto& vertexInfos = tail.vertexInfos;
    auto& decodeReads = tail.decodeReads;
    auto& shaderMemory = tail.shaderMemory;
    auto& results = tail.results;
    auto& compiledStages = tail.compiledStages;
    auto& stages = tail.stages;
    auto& pushCursorBytes = tail.pushCursorBytes;
    auto& programResults = tail.programResults;
    auto& stageCaptures = tail.stageCaptures;
    auto& matched = tail.matched;
    auto& matchedRegions = tail.matchedRegions;
    auto& fresh = tail.fresh;
    auto& recompiled = tail.recompiled;
    auto& dataCandidates = tail.dataCandidates;
    auto& reused = tail.reused;
    auto& stageData = tail.stageData;
    std::uint64_t dataStagesReused = 0, dataStagesCompiled = 0, dataVerified = 0;
    auto& drawHit = tail.drawHit;
    auto& verifyHit = tail.verifyHit;
    const bool stageEntries = tail.stageEntries;
    const bool patchedStages = tail.patchedStages;
    std::uint64_t stageDataVerified = 0;
    const Submission& submission = *tail.submission;
    std::string& rejected = tail.rejected != nullptr ? *tail.rejected : tail.ownRejected;
    static const QueueState noQueue{};
    const QueueState& queue = tail.queue != nullptr ? *tail.queue : noQueue;
    const auto drawn = [&] {
        phaseTiming.Phase(DrawRowVectors);
        if (!profile) return DrawVerdict::Drawn;
        auto& pending = pendingDrawPhases();
        pending.phases = true;
        pending.captures = captures;
        pending.ms = phaseMs;
        pending.tailAt = phaseLap;
        pending.identity = ProgramIdentity(tail.programs);
        if (DrawShaderProfileEnabled()) {
            pending.cache = tail.cacheOutcome;
            pending.graphics = Graphics::TakeLastDrawPhases(pending.graphicsUs);
        }
        return DrawVerdict::Drawn;
    };
    const auto setMeshIndexBuffer = [&](const Pm4::DrawParameters& parameters) {
        if (!graphics.stages.mesh) return;
        auto& words = writablePrograms.front().userData;
        require(programs.front().firstUserSgpr == 0 && words.size() >= ShaderRecompiler::MeshIndexBufferUserWord + 4, "mesh program lacks the hidden user words");
        const auto descriptor = Graphics::MeshIndexBufferDescriptor(parameters);
        std::copy(descriptor.begin(), descriptor.end(), words.begin() + ShaderRecompiler::MeshIndexBufferUserWord);
    };
    const auto locate = [&](std::uint32_t location) -> std::optional<std::pair<std::size_t, std::size_t>> {
        if (location == 0x280u) return std::nullopt;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (roles[i] == Role::Fragment || roles[i] == Role::GeometryBack || location < programs[i].userDataBase) continue;
            const auto word = location - programs[i].userDataBase + (8u - programs[i].firstUserSgpr);
            if (word < programs[i].userData.size()) return std::make_pair(i, static_cast<std::size_t>(word));
        }
        return std::nullopt;
    };
    const auto decodeVertexInfo = [&](std::size_t i) {
        const auto& program = programs[i];
        if (program.binary.stage == Stage::Fragment || roles[i] == Role::GeometryBack) return;
        decodeReads[i].clear();
        vertexInfos[i] = Graphics::DecodeVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData, &decodeReads[i], false, 8u - program.firstUserSgpr);
        if (vertexInfos[i]) {
            vertexInfos[i]->paClVsOutCntl = PositionExportLayout(program.snapshot.get(), program.codeOffset, program.binary.code, tail.reg207, program.tailContinuation.get()) | graphics.layerExports;
            vertexInfos[i]->depthRemapMode = graphics.depthRemap.mode;
            Graphics::ApplyPointSize(graphics, *vertexInfos[i]);
        }
    };
    const auto fold = [&](const ShaderRecompiler::RecompileResult& main, Pm4::DrawParameters& parameters) { FoldDrawOffsets(main, programs.front(), parameters); };

    const auto freshRuns = [&](std::size_t i, std::vector<std::pair<std::uint64_t, std::uint64_t>>& runs, std::vector<std::uint32_t>& words) {
        std::vector<ShaderRecompiler::MemoryRegion> regions(stageCaptures[i].regions.begin(), stageCaptures[i].regions.end());
        for (const auto& read : decodeReads[i]) regions.push_back({read.address, read.Bytes()});
        std::stable_sort(regions.begin(), regions.end(), [](const ShaderRecompiler::MemoryRegion& a, const ShaderRecompiler::MemoryRegion& b) { return a.guestAddress < b.guestAddress; });
        for (const auto& region : regions) {
            runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
            const auto count = region.bytes.size() / sizeof(std::uint32_t);
            const auto offset = words.size();
            words.resize(offset + count);
            std::memcpy(words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
        }
    };
    const auto joinedRuns = [](const std::vector<std::pair<std::uint64_t, std::uint64_t>>& runs) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> joined;
        for (const auto& run : runs) {
            if (!joined.empty() && joined.back().second == run.first) joined.back().second = run.second;
            else joined.push_back(run);
        }
        return joined;
    };
    const auto listRuns = [](const std::vector<std::pair<std::uint64_t, std::uint64_t>>& runs) {
        std::string text;
        for (std::size_t run = 0; run < runs.size() && run < 4; ++run) {
            char item[48];
            std::snprintf(item, sizeof(item), " [0x%llx,0x%llx)", static_cast<unsigned long long>(runs[run].first), static_cast<unsigned long long>(runs[run].second));
            text += item;
        }
        return text;
    };
    const auto verifyDataStage = [&](std::size_t i, const DrawDataCandidate& candidate) {
        const auto& kept = *candidate.variant;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
        std::vector<std::uint32_t> words;
        freshRuns(i, runs, words);
        const auto& compiled = *stageCaptures[i].compiled;
        const bool masked = tail.feedbackMask && candidate.feedbackHit;
        const bool sameRuns = tail.feedbackMask ? SameCapturedRuns(kept.runs, runs) : joinedRuns(kept.runs) == joinedRuns(runs);
        const char* what = !sameRuns ? "runs" : !FeedbackWordsEqual(kept.words, words, masked ? std::span<const std::pair<std::uint32_t, std::uint32_t>>(kept.feedback.memory) : std::span<const std::pair<std::uint32_t, std::uint32_t>>{}) ? "words" : vertexInfos[i].has_value() != (kept.vertexInfo != nullptr) || (vertexInfos[i] && !sameVertexInfo(*vertexInfos[i], *kept.vertexInfo)) ? "vertex stage info" : compiled.variantId != kept.compiled->variantId ? "variant" : (masked ? !FeedbackBindingsEqual(kept, *kept.compiled, compiled, true) : compiled.pushConstants != kept.compiled->pushConstants) ? (masked ? "bindings" : "push constants") : nullptr;
        ++dataVerified;
        if (candidate.feedbackHit) ArchB::Recorder::Count(what == nullptr ? "fix_tsharp_mask_verified" : "fix_tsharp_mask_verify_mismatches");
        if (what == nullptr) return;
        char message[640];
        std::snprintf(message, sizeof(message), "APS5_VERIFY_DRAW_DATA_HITS: stage %zu (program 0x%llx) of a data-only hit differs from its full build in its %s (%zu runs / %zu words kept, %zu / %zu fresh; variant 0x%llx / 0x%llx; key 0x%llx, shape 0x%llx; kept%s; fresh%s)", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), what, kept.runs.size(), kept.words.size(), runs.size(), words.size(), static_cast<unsigned long long>(kept.compiled->variantId), static_cast<unsigned long long>(compiled.variantId), static_cast<unsigned long long>(drawKey), static_cast<unsigned long long>(shapeKey), listRuns(joinedRuns(kept.runs)).c_str(), listRuns(joinedRuns(runs)).c_str());
        throw std::runtime_error(message);
    };
    const auto verifyStageData = [&](std::size_t i) {
        const auto& hit = stageData[i];
        const auto& kept = *matched[i];
        std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
        std::vector<std::uint32_t> words;
        freshRuns(i, runs, words);
        const auto& fresh = *stageCaptures[i].compiled;
        const auto& patched = hit.compiled != nullptr ? *hit.compiled : *kept.compiled;
        const auto& expectedWords = hit.compiled != nullptr ? hit.words : kept.words;
        const bool sameBindings = (tail.feedbackMask ? FeedbackBindingsEqual(kept, patched, fresh, true) : fresh.bindings.size() == patched.bindings.size() && std::equal(fresh.bindings.begin(), fresh.bindings.end(), patched.bindings.begin(), [](const auto& a, const auto& b) { return a.kind == b.kind && a.role == b.role && a.binding == b.binding && a.count == b.count && a.guestDescriptor == b.guestDescriptor; }));
        const bool sameRuns = tail.feedbackMask ? SameCapturedRuns(kept.runs, runs) : joinedRuns(kept.runs) == joinedRuns(runs);
        const char* what = !sameRuns ? "runs" : !FeedbackWordsEqual(expectedWords, words, tail.feedbackMask ? std::span<const std::pair<std::uint32_t, std::uint32_t>>(kept.feedback.memory) : std::span<const std::pair<std::uint32_t, std::uint32_t>>{}) ? "words" : fresh.variantId != patched.variantId ? "variant" : !tail.feedbackMask && fresh.pushConstants != patched.pushConstants ? "push constants" : !sameBindings ? "bindings" : nullptr;
        ++stageDataVerified;
        if (hit.bufferBaseHit) ArchB::Recorder::Count(what == nullptr ? "fix_buffer_base_verified" : "fix_buffer_base_verify_mismatches");
        if (hit.feedbackHit) ArchB::Recorder::Count(what == nullptr ? "fix_tsharp_mask_verified" : "fix_tsharp_mask_verify_mismatches");
        if (what == nullptr) return;
        char message[256];
        std::snprintf(message, sizeof(message), "APS5_VERIFY_DRAW_DATA_HITS: stage %zu (program 0x%llx) of a stage data hit differs from its full build in its %s (%zu data positions refreshed)", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), what, kept.dataPositions.size());
        throw std::runtime_error(message);
    };
    bool partialFeedbackHit = false;
    std::optional<Graphics::IndirectDrawPath> indirectCpu;
    std::size_t completedSubdraws = 0;
    std::vector<std::uint32_t> pushOffsets(programs.size(), 0);
    std::optional<ShaderRecompiler::MeshBarycentricLayout> meshBarycentrics;
    std::vector<std::size_t> resultIndex(programs.size(), 0);
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (roles[i] == Role::GeometryBack) continue;
        const auto& program = programs[i];
        pushCursorBytes = Graphics::StagePushOffset(pushCursorBytes, program.binary.stage, localDevice->GraphicsPipelineLibraries());
        pushOffsets[i] = pushCursorBytes;
        if (drawHit) {

            programResults[i] = stageData[i].compiled != nullptr ? stageData[i].compiled.get() : matched[i]->compiled.get();
            if (drawParameters.indirect) stageCaptures[i].capture = matched[i]->indirectCapture;
            if (stageCaptures[i].capture != nullptr) stageCaptures[i].userDataUse = stageCaptures[i].capture->userDataUse;
            memory.insert(memory.end(), matchedRegions[i].begin(), matchedRegions[i].end());
        } else {
            const auto candidate = std::find_if(dataCandidates[i].begin(), dataCandidates[i].end(), [&](const auto& kept) { return kept.variant->pushOffset == pushCursorBytes; });
            if (candidate != dataCandidates[i].end()) partialFeedbackHit |= candidate->feedbackHit;
            if (candidate != dataCandidates[i].end() && !verifyDrawDataHits()) {
                matched[i] = candidate->variant;
                stageCaptures[i].registeredViews = matched[i]->registeredViews;
                if (!candidate->words.empty()) stageData[i].words = std::move(candidate->words);
                matchedRegions[i] = std::move(candidate->regions);
                reused[i] = true;
                programResults[i] = stageData[i].compiled != nullptr ? stageData[i].compiled.get() : matched[i]->compiled.get();
                memory.insert(memory.end(), matchedRegions[i].begin(), matchedRegions[i].end());
            } else {
                if (!dataCandidates[i].empty() && !verifyDrawDataHits()) decodeVertexInfo(i);
                if (program.binary.stage == ShaderRecompiler::ShaderStage::Fragment) {
                    for (auto previous = i; previous-- > 0;) {
                        if (programResults[previous] == nullptr) continue;
                        pixel = Graphics::DefaultUnexportedInputs(decode->pixel, programResults[previous]->parameterExports);
                        break;
                    }
                }
                compiledStages.push_back(materializeDrawStage(i, pushCursorBytes, queue, submission, packetOffset, trace.token, programs, graphics, pixel, vertexInfos, memory, linked, drawParameters, localDevice, *shaderMemory, stageCaptures, recompiled, drawHit, matched, matchedRegions, profile, dumpTarget, dumpSlot1, captures, phaseTiming, phaseMs, rejected, meshBarycentrics));
                if (!rejected.empty()) return DrawVerdict::Rejected;
                programResults[i] = compiledStages.back().get();
                if (candidate != dataCandidates[i].end()) verifyDataStage(i, *candidate);
                if ((stageData[i].compiled != nullptr || stageData[i].feedbackHit) && verifyDrawDataHits()) verifyStageData(i);
            }
            if (dataEntry != nullptr && reused[i]) ++dataStagesReused;
            else if (dataEntry != nullptr) ++dataStagesCompiled;
        }
        const auto& result = *programResults[i];
        if (i == 0 && drawParameters.indirect) {
            indirectCpu = ClassifyIndirectDraw(result, graphics, programs.front(), localDevice, drawParameters, traceIndirect, vertexInfos.front());
            if (tail.liveEntry != nullptr && !indirectCpu && !tail.liveEntry->derivedReady) {
                tail.liveEntry->derived = drawParameters;
                tail.liveEntry->derivedReady = true;
            }
            if (tail.ubSkipCpuIndirect && indirectCpu) {
                ArchB::Switches::CountUpperBoundHit(ArchB::Feature::UbSkipCpuIndirect);
                return drawn();
            }
            if (EmptyIndirectSkip()) CpuIndirectPrograms()[programs.front().binary.codeAddress] = indirectCpu.has_value();
        } else if (i == 0) {
            fold(result, drawParameters);
            if (tail.liveEntry != nullptr && !tail.liveEntry->derivedReady) {
                tail.liveEntry->derived = drawParameters;
                tail.liveEntry->derivedReady = true;
            }
        }
        try {
            const auto slotEnd = (pushCursorBytes / Graphics::PipelinePushSlotBytes + 1u) * Graphics::PipelinePushSlotBytes;
            require(result.pushConstants.size() <= slotEnd - pushCursorBytes, "stage push constants exceed the pipeline push constant block");
            stages.push_back({program.binary.stage, &result, result.pushConstants.empty() ? 0u : pushCursorBytes, stageCaptures[i].registeredViews.get()});
            pushCursorBytes += static_cast<std::uint32_t>(result.pushConstants.size());
        } catch (...) {
            TraceMaterializationFailure(trace.token, submission, packetOffset, "push-constant-validation", completedSubdraws, std::current_exception());
            throw;
        }
    }

    if (partialFeedbackHit) ArchB::Recorder::Count("fix_tsharp_mask_hits");
    if (verifyHit && std::any_of(matched.begin(), matched.end(), [](const auto& variant) { return variant != nullptr && variant->nestedRelocated; })) {
        const auto flattened = [](const auto& runs, const auto& words) {
            std::vector<std::pair<std::uint64_t, std::uint32_t>> values;
            std::size_t position = 0;
            for (const auto& [begin, end] : runs) for (auto address = begin; address < end && position < words.size(); address += sizeof(std::uint32_t)) values.emplace_back(address, words[position++]);
            std::sort(values.begin(), values.end());
            return values;
        };
        for (std::size_t i = 0; i < matched.size(); ++i) {
            if (matched[i] == nullptr) continue;
            std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
            std::vector<std::uint32_t> words;
            freshRuns(i, runs, words);
            const auto& kept = *matched[i];
            const auto& hit = stageData[i];
            const auto& expected = hit.compiled != nullptr ? *hit.compiled : *kept.compiled;
            const auto& expectedWords = hit.compiled != nullptr ? hit.words : kept.words;
            const bool feedback = tail.feedbackMask && hit.feedbackHit;
            auto expectedValues = flattened(kept.runs, expectedWords);
            auto freshValues = flattened(runs, words);
            std::map<std::uint64_t, std::uint32_t> masks;
            if (feedback) {
                std::size_t position = 0;
                for (const auto& [begin, end] : kept.runs) for (auto address = begin; address < end; address += sizeof(std::uint32_t), ++position) {
                    const auto mask = WordMask(kept.feedback.memory, position);
                    const auto [found, inserted] = masks.emplace(address, mask);
                    if (!inserted) found->second &= mask;
                }
            }
            bool sameMemory = expectedWords.size() == words.size() && expectedValues.size() == freshValues.size();
            for (std::size_t word = 0; sameMemory && word < expectedValues.size(); ++word) {
                const auto& [address, value] = expectedValues[word];
                const auto mask = feedback ? masks.at(address) : 0u;
                sameMemory = address == freshValues[word].first && ((value ^ freshValues[word].second) & ~mask) == 0;
            }
            const bool same = stageCaptures[i].compiled != nullptr && sameMemory && FeedbackBindingsEqual(kept, expected, *stageCaptures[i].compiled, feedback);
            if (!same) {
                ArchB::Recorder::Count("fix_draw_reloc_nested_verify_mismatches");
                static std::atomic<unsigned> reports {0};
                if (reports.fetch_add(1) < 12) std::fprintf(stderr, "[draw-nested] verify: stage %zu differs in %s\n", i, sameMemory ? "patched bindings" : "captured addresses or words");
            }
        }
    }
    if (stageDataVerified != 0) {
        std::lock_guard cacheLock(drawCacheMutex);
        drawEntryCounters.stageDataVerified += stageDataVerified;
    }
    if (dataEntry != nullptr) {
        std::lock_guard cacheLock(drawCacheMutex);
        drawEntryCounters.dataStagesReused += dataStagesReused;
        drawEntryCounters.dataStagesCompiled += dataStagesCompiled;
        drawEntryCounters.dataVerified += dataVerified;
    }
    const bool shapeRefresh = Graphics::DrawPipelineEnabled();
    const auto refreshSeenTwice = [&] {
        std::uint64_t data = shapeKey;
        bool stored = false;
        const auto mix = [&data](std::uint64_t value) {
            data ^= value + 0x9e3779b97f4a7c15ull + (data << 6u) + (data >> 2u);
        };
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& capture = stageCaptures[i];
            if (reused[i] || capture.compiled == nullptr || !CacheableResult(*capture.compiled)) continue;
            stored = true;
            const auto& words = programs[i].userData;
            for (std::size_t w = 0; w < words.size(); ++w) {
                if (w >= 64u || (capture.userDataMask >> w & 1u) != 0) mix((static_cast<std::uint64_t>(i) << 40u) ^ (static_cast<std::uint64_t>(w) << 32u) ^ words[w]);
            }
        }
        if (!stored) return false;
        static std::array<std::atomic<std::uint64_t>, 4096> lastData{};
        return lastData[shapeKey % lastData.size()].exchange(data, std::memory_order_relaxed) == data;
    };
    const bool shapeNeedsEntry = shapeRefresh && registerKey && shapeKey != 0 && drawDataHits() && !drawParameters.indirect && dataEntry != nullptr && dataStagesCompiled != 0 && refreshSeenTwice();
    cacheDrawStages(stageEntries, drawHit, drawParameters, indirectCpu, programs, stageCaptures, vertexInfos, decodeReads, verifyHit, matched, stageData, fresh, drawKey, registerKey, decode, phaseTiming, reused, entry, shapeKey, shapeNeedsEntry, tail.feedbackMask, tail.bufferBases);
    timing.Mark("shader_compile_and_link");

    for (const auto& reads : decodeReads) {
        for (const auto& read : reads) memory.push_back({read.address, read.Bytes()});
    }

    if (tail.queue != nullptr && pipelined && !deferredLabels().labels.empty()) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Labels);
    if (recordQueuedLabelsAfterCapture(submission.queue, memory)) return draw(*tail.queue, packet, submission, packetOffset, rejected);

    bool rectListBuilt = false;

    std::size_t rectIndex = 0;
    const auto buildRectList = [&] {
        try {
            phaseTiming.Phase(DrawRowVectors);
            require(programs.size() == 2 && programResults[0] != nullptr && programResults[1] != nullptr, "rect-list requires vertex and fragment programs");
            auto rectangle = DrawRectangle(*programs[0].snapshot, programs[1].snapshot, programResults[0]->variantId, programResults[1]->variantId, localDevice->Target());
            auto& control = graphics.provokingVertexMode == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT ? rectangle.lastVertexControl : rectangle.control;
            if (rectListBuilt) {
                results[rectIndex] = std::move(control);
                results[rectIndex + 1] = std::move(rectangle.evaluation);
                phaseTiming.Phase(DrawRowRectList);
                return;
            }
            require(stages.size() == 2, "rect-list requires vertex and fragment programs");
            results.reserve(programs.size() + 2u);
            rectIndex = results.size();
            results.push_back(std::move(control));
            results.push_back(std::move(rectangle.evaluation));
            stages.insert(stages.begin() + 1, {{Stage::TessellationControl, &results[rectIndex], 0}, {Stage::TessellationEvaluation, &results[rectIndex + 1], 0}});
            rectListBuilt = true;
            phaseTiming.Phase(DrawRowRectList);
        } catch (...) {
            TraceMaterializationFailure(trace.token, submission, packetOffset, "rect-list", completedSubdraws, std::current_exception());
            throw;
        }
    };
    if (graphics.rectList) buildRectList();
    if (graphics.stages.tessellationGeometry) {
        static std::mutex captureMutex;
        static std::map<std::pair<std::uint32_t, std::uint32_t>, ShaderRecompiler::RecompileResult> captureShaders;
        const auto& geometry = *graphics.stages.tessellationGeometry;
        results.reserve(programs.size() + 2u);
        {
            std::lock_guard captureLock(captureMutex);
            auto found = captureShaders.find({geometry.esgsItemSize, geometry.primitivesPerGroup});
            if (found == captureShaders.end()) found = captureShaders.emplace(std::pair{geometry.esgsItemSize, geometry.primitivesPerGroup}, ShaderRecompiler::BuildTessellationCaptureGeometryShader(geometry, localDevice->Target(), localDevice->GeometryLimits())).first;
            results.push_back(found->second);
        }
        const auto evaluation = std::ranges::find_if(stages, [](const Graphics::CompiledShader& stage) { return stage.stage == Stage::TessellationEvaluation; });
        require(evaluation != stages.end(), "tessellation followed by a geometry stage has no domain program");
        stages.insert(std::next(evaluation), Graphics::CompiledShader{Stage::Geometry, &results.back(), 0});
    }
    if (!programs.empty() && programResults.back() != nullptr && programResults.back()->barycentricEmulation.active && graphics.stages.path == Graphics::ShaderPath::Geometry) {
        require(localDevice->Target().mesh.has_value(), "a pixel shader on the mesh path reads barycentrics, and this device has neither VK_KHR_fragment_shader_barycentric nor mesh shaders (VK_EXT_mesh_shader), so they cannot be provided or emulated");
        require(!graphics.rectList && graphics.stages.mesh && programs.size() == 2 && programResults[0] != nullptr && programResults[1] != nullptr && stages.size() == 2, "a pixel shader that reads barycentrics needs exactly one mesh program before it to emulate them");
        meshBarycentrics = ShaderRecompiler::MeshBarycentricLayoutOf(*programResults[1]);
        if (programResults[0]->meshBarycentrics != meshBarycentrics) {
            const auto pushBytes = programResults[0]->pushConstants.size();
            const auto exports = programResults[0]->parameterExports;
            compiledStages.push_back(materializeDrawStage(0, pushOffsets[0], queue, submission, packetOffset, traceToken, programs, graphics, pixel, vertexInfos, memory, linked, drawParameters, localDevice, *shaderMemory, stageCaptures, recompiled, drawHit, matched, matchedRegions, profile, dumpTarget, dumpSlot1, captures, phaseTiming, phaseMs, rejected, meshBarycentrics));
            if (!rejected.empty()) return DrawVerdict::Rejected;
            programResults[0] = compiledStages.back().get();
            stages[0].program = programResults[0];
            stages[0].registeredImageViews = stageCaptures[0].registeredViews.get();
            require(programResults[0]->pushConstants.size() == pushBytes && programResults[0]->parameterExports == exports, "pairing the mesh program with its pixel shader changed the push constants or the parameter exports the pixel shader was built for");
        }
        require(programResults[0]->meshBarycentrics == meshBarycentrics, "the mesh program was compiled for a different barycentric layout than its pixel shader");
    } else if (!graphics.rectList && !programs.empty() && programResults.back() != nullptr && programResults.back()->barycentricEmulation.active) {
        require(graphics.stages.path == Graphics::ShaderPath::Vertex && programs.size() == 2 && programResults[0] != nullptr && stages.size() == 2, "a pixel shader that reads barycentrics without VK_KHR_fragment_shader_barycentric needs a vertex shader before it");
        const auto geometryLimits = localDevice->GeometryLimits();
        results.reserve(programs.size() + 2u);
        if (geometryLimits || !localDevice->Target().tessellation) {
            results.push_back(ShaderRecompiler::BuildBarycentricGeometryShader(*programResults[0], *programResults[1], localDevice->Target(), geometryLimits));
            stages.insert(stages.begin() + 1, Graphics::CompiledShader{Stage::Geometry, &results.back(), 0});
        } else {
            require((graphics.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && (!graphics.primitiveRestart || !localDevice->PrimitiveListRestart())) || (graphics.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP && (!graphics.primitiveRestart || !drawParameters.indexed)), "a pixel shader that reads barycentrics without VK_KHR_fragment_shader_barycentric or geometry shaders needs a triangle list, with primitive restart only on a device without VK_EXT_primitive_topology_list_restart, or a triangle strip, with primitive restart only in a non-indexed draw");
            auto patches = ShaderRecompiler::BuildBarycentricTessellationShaders(*programResults[0], *programResults[1], localDevice->Target(), localDevice->MaxFragmentInputComponents(), graphics.provokingVertexMode == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT ? 2u : 0u);
            const auto first = results.size();
            results.push_back(std::move(patches.control));
            results.push_back(std::move(patches.evaluation));
            stages.insert(stages.begin() + 1, {{Stage::TessellationControl, &results[first], 0}, {Stage::TessellationEvaluation, &results[first + 1], 0}});
        }
    }
    std::vector<Graphics::GuestMemorySnapshot> snapshots;
    const auto snapshot = [&] {
        snapshots.clear();
        snapshots.reserve(memory.size());
        for (const auto& region : memory) snapshots.push_back({region.guestAddress, region.bytes});
    };
    snapshot();
    timing.Mark("post_compile_prepare");
    DrawLockTiming lockTiming{profile};
    const auto prepareForDraw = [&](const Pm4::DrawParameters& parameters, const DrawRecipe* recipe = nullptr) {
        if (gpuLock.owns_lock() || !Graphics::DrawPrelockEnabled()) return;
        const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        lockTiming.preparation = localDevice->PrepareDraw(graphics, parameters, stages, recipe, tail.memoVirtualQuery);
        if (profile) lockTiming.prepareMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    const auto lockForDraw = [&] {
        require(tail.queue != nullptr, "the draw finisher reached a draw it cannot commit through the pipeline");
        if (gpuLock.owns_lock()) {
            lockTiming.Start();
            return;
        }
        phaseTiming.Phase(DrawRowVectors);
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        gpuLock.lock();
        lockTiming.Start();
        timing.Mark("gpu_mutex_wait");
        phaseTiming.Phase(DrawRowLockWait);

        if (auto current = device.Load(); current != nullptr && current != localDevice) {
            static std::atomic<std::uint64_t> replaced{0};
            std::fprintf(stderr, "[draw] device replaced during unlocked preparation (%llu)\n", static_cast<unsigned long long>(++replaced));
            localDevice = std::move(current);
        }

        recordLabelsForPacket(localDevice.get(), submission.queue);
        phaseTiming.Phase(DrawRowLabels);
    };
    if (drawParameters.indirect && indirectCpu) {
        if (tail.gatherPlan != nullptr) {
            CountGatherPlanUnchecked("gather_shadow_plan_unchecked_cpu_indirect");
            tail.gatherPlan = nullptr;
        }
        const auto indirect = *drawParameters.indirect;
        results.reserve(programs.size() + 2u);
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (programResults[i] == nullptr) continue;
            resultIndex[i] = results.size();
            results.push_back(ShaderRecompiler::RecompileResult(*programResults[i]));
            for (auto& stage : stages) {
                if (stage.program == programResults[i]) stage.program = &results[resultIndex[i]];
            }
            programResults[i] = &results[resultIndex[i]];
        }
        if (tail.indirectGpuArgs) drainBeforeArgumentRead(indirect, DrawPipeline::DrainCause::IndirectCpuRead);
        recordQueuedLabelsBeforeRead(submission.queue);
        const auto readStart = std::chrono::steady_clock::now();
        const auto count = std::min(indirect.countIndirect ? Pm4::ReadDrawCount(indirect) : indirect.count, indirect.count);
        std::vector<Pm4::DrawArguments> records;
        for (std::uint32_t record = 0; record < count; ++record) records.push_back(Pm4::ReadDrawArguments(indirect, record));
        Graphics::CountIndirectDraw(*indirectCpu, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count());
        const auto baseVertexWords = UserDataWords(programs, roles, indirect.baseVertexLocation);
        const auto startInstanceWords = UserDataWords(programs, roles, indirect.startInstanceLocation);
        const auto drawIndexWords = indirect.drawIndexEnabled ? UserDataWords(programs, roles, indirect.drawIndexLocation) : std::vector<std::pair<std::size_t, std::size_t>>{};
        const auto drawable = [&](const Pm4::DrawArguments& arguments) { return arguments.count != 0 && arguments.instances != 0 && (!drawParameters.indexed || !indirect.indexLimitKnown || arguments.firstVertexOrIndex < drawParameters.indexCount); };
        std::size_t lastDrawable = records.size();
        for (std::size_t record = 0; record < records.size(); ++record) {
            if (drawable(records[record])) lastDrawable = record;
        }
        const bool pushWord = drawCachePushWord();
        const bool verifyPushWord = pushWord && verifyDrawPushWord();
        std::uint64_t wordsForwarded = 0, stagesRecompiled = 0, wordsRefused = 0, wordsVerified = 0;
        struct RecordWord {
            std::size_t program;
            std::size_t index;
            std::uint32_t value;
        };
        std::vector<RecordWord> recordWords;
        const auto countRecordWords = [&] {
            if (wordsForwarded == 0 && stagesRecompiled == 0 && wordsRefused == 0 && wordsVerified == 0) return;
            std::lock_guard cacheLock(drawCacheMutex);
            drawEntryCounters.recordWordsForwarded += wordsForwarded;
            drawEntryCounters.recordStagesRecompiled += stagesRecompiled;
            drawEntryCounters.recordWordsRefused += wordsRefused;
            drawEntryCounters.recordWordsVerified += wordsVerified;
        };
        const auto sameInfo = [&](const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& a, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& b) { return a.has_value() == b.has_value() && (!a || sameVertexInfo(*a, *b)); };
        const auto forwardRecordWords = [&](std::size_t programIndex, ShaderRecompiler::RecompileResult& result, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& previousInfo) {
            std::uint64_t walked = 0;
            if (stageCaptures[programIndex].capture != nullptr) walked = stageCaptures[programIndex].userDataUse.asBase | stageCaptures[programIndex].userDataUse.asValue;
            else if (matched[programIndex] != nullptr) walked = matched[programIndex]->userWordsWalked;
            else return false;
            if ((graphics.stages.mesh && programIndex == 0) || !sameInfo(previousInfo, vertexInfos[programIndex])) return false;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> slots;
            bool any = false;
            for (const auto& word : recordWords) {
                if (word.program != programIndex) continue;
                any = true;
                if (word.index >= ShaderRecompiler::SrtUserDataUse::TrackedWords || (walked >> word.index & 1u) != 0) return false;
                const auto& forwarded = result.shaderDataUserWords;
                const auto slot = std::find(forwarded.begin(), forwarded.end(), static_cast<std::uint32_t>(word.index));
                if (slot != forwarded.end()) slots.emplace_back(static_cast<std::uint32_t>(slot - forwarded.begin()), word.value);
            }
            if (!any) return false;
            for (const auto& [slot, value] : slots) patchForwardedWord(result, slot, value);
            return true;
        };
        const auto verifyForwardedWords = [&](std::size_t programIndex, const ShaderRecompiler::RecompileResult& forwarded, const ShaderRecompiler::RecompileResult& fresh) {
            const bool sameBindings = forwarded.bindings.size() == fresh.bindings.size() && std::equal(forwarded.bindings.begin(), forwarded.bindings.end(), fresh.bindings.begin(), [](const auto& a, const auto& b) { return a.kind == b.kind && a.role == b.role && a.binding == b.binding && a.count == b.count && a.guestDescriptor == b.guestDescriptor; });
            std::uint64_t walked = 0;
            for (const auto& word : recordWords) {
                if (word.program == programIndex && word.index < ShaderRecompiler::SrtUserDataUse::TrackedWords) walked |= (stageCaptures[programIndex].userDataUse.asBase | stageCaptures[programIndex].userDataUse.asValue) >> word.index & 1u;
            }
            const char* what = forwarded.variantId != fresh.variantId ? "variant" : forwarded.pushConstants != fresh.pushConstants ? "push constants" : !sameBindings ? "bindings" : forwarded.shaderDataUserWords != fresh.shaderDataUserWords ? "forwarded words" : walked != 0 ? "walked user word" : nullptr;
            ++wordsVerified;
            if (what == nullptr) return;
            char message[256];
            std::snprintf(message, sizeof(message), "APS5_VERIFY_DRAW_PUSH_WORD: stage %zu (program 0x%llx) with its record's user word forwarded as push data differs from its full build in its %s", programIndex, static_cast<unsigned long long>(programs[programIndex].binary.codeAddress), what);
            throw std::runtime_error(message);
        };
        bool recordedOne = false;
        for (std::uint32_t record = 0; record < records.size(); ++record) {
            const auto& arguments = records[record];
            if (traceIndirect) std::fprintf(stderr, "[draw]   record %u: count %u instances %u first %u vertexOffset %u startInstance %u\n", record, arguments.count, arguments.instances, arguments.firstVertexOrIndex, arguments.vertexOffset, arguments.firstInstance);
            if (arguments.count == 0 || arguments.instances == 0) continue;
            std::set<std::size_t> patched;
            recordWords.clear();
            const auto patch = [&](const std::vector<std::pair<std::size_t, std::size_t>>& words, std::uint32_t value) {
                for (const auto& [program, word] : words) {
                    writablePrograms[program].userData[word] = value;
                    patched.insert(program);
                    recordWords.push_back({program, word, value});
                }
            };
            patch(baseVertexWords, indirect.recordBytes == 20 ? arguments.vertexOffset : arguments.firstVertexOrIndex);
            patch(startInstanceWords, arguments.firstInstance);
            patch(drawIndexWords, record);
            if (graphics.stages.tessellation) {
                for (std::size_t i = 0; i < programs.size(); ++i) {
                    if (programs[i].binary.stage == Stage::TessellationControl || programs[i].binary.stage == Stage::TessellationEvaluation) patched.insert(i);
                }
            }
            Pm4::DrawParameters direct{0, arguments.count, 0, arguments.instances, drawParameters.flags, drawParameters.indexed, 0, 0};
            if (drawParameters.indexed) {
                if (indirect.indexLimitKnown) {
                    if (arguments.firstVertexOrIndex >= drawParameters.indexCount) continue;
                    direct.indexCount = std::min(arguments.count, drawParameters.indexCount - arguments.firstVertexOrIndex);
                } else {
                    const auto end = static_cast<std::uint64_t>(arguments.firstVertexOrIndex) + arguments.count;
                    require(end <= std::numeric_limits<std::uint32_t>::max(), "indirect draw index range overflow");
                    require(end * drawParameters.indexSize <= std::numeric_limits<std::uint64_t>::max() - drawParameters.indexAddress, "indirect draw index range address overflow");
                    direct.indexCount = arguments.count;
                }
                direct.indexAddress = drawParameters.indexAddress + static_cast<std::uint64_t>(arguments.firstVertexOrIndex) * drawParameters.indexSize;
                direct.indexSize = drawParameters.indexSize;
            } else {
                direct.firstVertex = indirect.indxOffset;
            }
            if (graphics.stages.mesh) {
                setMeshIndexBuffer(direct);
                patched.insert(0);
            }
            for (const auto programIndex : patched) {
                auto& result = results[resultIndex[programIndex]];
                const auto pushBytes = result.pushConstants.size();
                const auto previousInfo = vertexInfos[programIndex];
                decodeVertexInfo(programIndex);
                if (pushWord && forwardRecordWords(programIndex, result, previousInfo)) {
                    wordsForwarded += static_cast<std::uint64_t>(std::count_if(recordWords.begin(), recordWords.end(), [&](const RecordWord& word) { return word.program == programIndex; }));
                    if (!verifyPushWord) continue;
                    const auto fresh = materializeDrawStage(programIndex, pushOffsets[programIndex], queue, submission, packetOffset, trace.token, programs, graphics, pixel, vertexInfos, memory, linked, drawParameters, localDevice, *shaderMemory, stageCaptures, recompiled, drawHit, matched, matchedRegions, profile, dumpTarget, dumpSlot1, captures, phaseTiming, phaseMs, rejected, meshBarycentrics);
                    for (auto& stage : stages) if (stage.program == &result) stage.registeredImageViews = stageCaptures[programIndex].registeredViews.get();
                    if (!rejected.empty()) return DrawVerdict::Rejected;
                    verifyForwardedWords(programIndex, result, *fresh);
                    continue;
                }
                if (pushWord) wordsRefused += static_cast<std::uint64_t>(std::count_if(recordWords.begin(), recordWords.end(), [&](const RecordWord& word) { return word.program == programIndex; }));
                const auto* capture = !graphics.stages.mesh && !graphics.stages.tessellation && sameInfo(previousInfo, vertexInfos[programIndex]) ? stageCaptures[programIndex].capture.get() : nullptr;
                const bool pushPatched = RefreshIndirectStage(result, capture, programs[programIndex], vertexInfos[programIndex], [&] {
                    return materializeDrawStage(programIndex, pushOffsets[programIndex], queue, submission, packetOffset, trace.token, programs, graphics, pixel, vertexInfos, memory, linked, programs[programIndex].binary.stage == Stage::TessellationControl || programs[programIndex].binary.stage == Stage::TessellationEvaluation ? direct : drawParameters, localDevice, *shaderMemory, stageCaptures, recompiled, drawHit, matched, matchedRegions, profile, dumpTarget, dumpSlot1, captures, phaseTiming, phaseMs, rejected, meshBarycentrics);
                });
                for (auto& stage : stages) if (stage.program == &result) stage.registeredImageViews = stageCaptures[programIndex].registeredViews.get();
                if (!rejected.empty()) return DrawVerdict::Rejected;
                require(result.pushConstants.size() == pushBytes, "patched program changed its push constant layout");
                if (!pushPatched) ++stagesRecompiled;
            }
            fold(*programResults[0], direct);
            if (indirect.instanceFromBase) direct.firstInstance = arguments.firstInstance;
            if (indirect.vertexFromBase) direct.firstVertex = arguments.vertexOffset;
            if (graphics.rectList && patched.contains(0)) buildRectList();
            snapshot();
            if (auto known = localDevice->KnownDrawRejection(graphics, stages)) {
                rejected = std::move(*known);
                return DrawVerdict::Rejected;
            }
            if (pipelined && pipelinedIndirect && record == lastDrawable && !gpuLock.owns_lock() && deferredLabels().labels.empty()) {
                runCaptureCheckHook();
                if (tail.queue != nullptr && capturedMemoryChanged(memory)) {
                    const RecaptureScope recapture;
                    return draw(*tail.queue, tail.packet, submission, tail.packetOffset, rejected);
                }
                phaseTiming.Phase(DrawRowVectors);
                auto writes = queuedDrawWrites(tail, graphics, stages);
                if (PipeWriteDiag()) NoteDrawWriteTags(graphics, stages);
                const auto commitQueue = submission.queue;
                const auto commitPacket = tail.packetOpcode;
                const auto epoch = DrawPipeline::EpochToken().load(std::memory_order_relaxed);
                const auto ticket = GuestMemory::Gen::Required();
                auto commit = [this, pipelineStateReuse = tail.pipelineStateReuse, memoVirtualQuery = tail.memoVirtualQuery, allocChurn = tail.allocChurn, partialSnapshot = tail.partialSnapshot, hostSlabAlloc = tail.hostSlabAlloc, cheapTimers = tail.cheapTimers, recorderKey = tail.recorderKey, identity = ProgramIdentity(tail.programs), localDevice = std::move(localDevice), decode = std::move(decode), direct, stages = std::move(stages), snapshots = std::move(snapshots), commitQueue, commitPacket, epoch, ticket, writablePrograms = std::move(writablePrograms), results = std::move(results), memory = std::move(memory), linked = std::move(linked), stageCaptures = std::move(stageCaptures), matched = std::move(matched), matchedRegions = std::move(matchedRegions), fresh = std::move(fresh), decodeReads = std::move(decodeReads), shaderMemory = std::move(shaderMemory), entry = std::move(entry), dataEntry = std::move(dataEntry), compiledStages = std::move(compiledStages), stageData = std::move(stageData)] {
                    const HostAllocator::Scope hostSlabScope(hostSlabAlloc);
                    const ArchB::CheapClock::Scope cheapTimersScope(cheapTimers);
                    ArchB::Recorder::Work recorderWork(recorderKey, ArchB::Recorder::Phase::Record, false);
                    const ArchB::AllocChurnScope allocChurnScope(allocChurn);
                    const ArchB::PartialSnapshotScope partialSnapshotScope(partialSnapshot);
                    PerformanceTimer recorderTiming("Driver.DrawCommit");
                    DrawPipeline::FollowEpoch(epoch, ticket);
                    GuestMemory::SetCurrentPacket(commitPacket, commitQueue);
                    const auto committing = identity.programs != 0 ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    commitDraw(localDevice, commitQueue, decode->state, direct, stages, snapshots, nullptr, {}, 0, nullptr, {}, 0, pipelineStateReuse, memoVirtualQuery);
                    if (identity.programs != 0) addDrawCommit(identity, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - committing).count());
                };
                auto held = std::make_shared<decltype(commit)>(std::move(commit));
                DrawPipeline::Queue0().Enqueue(DrawPipeline::CommitKind::IndirectDraw, [held] { (*held)(); }, std::move(writes), 0, {}, {}, false, DrawPipeline::ItemKind::Draw);
                phaseTiming.Phase(DrawRowLockWait);
                countRecordWords();
                timing.Mark("draw_enqueued");
                return drawn();
            }
            drainBeforeInline(record != lastDrawable ? DrawPipeline::DrainCause::IndirectCpuRecords : DrawPipeline::DrainCause::IndirectInline);
            prepareForDraw(direct);
            lockForDraw();
            if (!recordedOne) {
                runCaptureCheckHook();
                if (tail.queue != nullptr && capturedMemoryChanged(memory, snapshots)) {
                    gpuLock.unlock();
                    const RecaptureScope recapture;
                    return draw(*tail.queue, tail.packet, submission, tail.packetOffset, rejected);
                }
            }
            recordedOne = true;
            noteDrawWriters(stages, submission.queue);
            phaseTiming.Phase(DrawRowVectors);
            localDevice->Draw(graphics, direct, stages, snapshots, nullptr, tracePtr, 0, &lockTiming.preparation, tail.pipelineStateReuse);
            phaseTiming.Phase(DrawRowGraphics);
        }
        countRecordWords();
        timing.Mark("draw_and_resource_release");
        return drawn();
    }

    std::vector<std::shared_ptr<DispatchVariant>> recipeStages;
    if (registerKey && !drawParameters.indirect && !patchedStages && !graphics.stages.tessellationGeometry && Graphics::DrawRecipes()) {
        recipeStages.reserve(programs.size());
        for (std::size_t i = 0; i < programs.size(); ++i) recipeStages.push_back(drawHit ? (matched[i] != nullptr && matched[i]->donor != nullptr ? matched[i]->donor : matched[i]) : fresh[i]);
        if (std::all_of(recipeStages.begin(), recipeStages.end(), [](const std::shared_ptr<DispatchVariant>& variant) { return variant == nullptr; })) recipeStages.clear();
    }
    const int nostageWhy = drawParameters.indirect || !recipeStages.empty() ? -1 : !registerKey ? 0 : patchedStages ? 1 : !Graphics::DrawRecipes() ? 2 : drawHit ? 3 : 4;
    const auto recipeDonor = drawHit && entry != nullptr ? entry->donor : nullptr;
    std::shared_ptr<const DrawRecipe> recipe;
    if (drawHit && !recipeStages.empty()) {
        recipe = findDrawRecipe(drawKey, recipeStages, recipeDonor);
        if (recipe == nullptr) VulkanDevice::NoteDrawRecipeMiss(VulkanDevice::DrawRecipePrecheck::NoRecipe);
    }
    if (recipe == nullptr) {
        if (auto known = localDevice->KnownDrawRejection(graphics, stages)) {
            rejected = std::move(*known);
            return DrawVerdict::Rejected;
        }
    }
    if (static const bool nameVariants = std::getenv("APS5_PROFILE_GPU_DRAWS") != nullptr; nameVariants && stages.size() == programs.size()) {
        static std::mutex namedMutex;
        static std::set<std::uint64_t> named;
        std::lock_guard namedLock(namedMutex);
        for (std::size_t i = 0; i < stages.size(); ++i) {
            if (stages[i].program == nullptr || !named.insert(stages[i].program->variantId).second) continue;
            std::fprintf(stderr, "[drawvariant] 0x%llx stage %d code 0x%llx\n", static_cast<unsigned long long>(stages[i].program->variantId), static_cast<int>(stages[i].stage), static_cast<unsigned long long>(programs[i].binary.codeAddress));
        }
    }
    if (tail.live != nullptr && !indirectCpu) {
        enqueueGatherDraw(tail);
        timing.Mark("draw_enqueued");
        return drawn();
    }
    if (pipelined && (!drawParameters.indirect || pipelinedIndirect) && (tail.queue == nullptr || deferredLabels().labels.empty())) {
        runCaptureCheckHook();
        if (capturedMemoryChanged(memory)) {
            const RecaptureScope recapture;
            if (tail.queue != nullptr) return draw(*tail.queue, tail.packet, submission, tail.packetOffset, rejected);
            restartFinishedDraw(tail);
            return finishDraw(tail, timing);
        }
        phaseTiming.Phase(DrawRowVectors);
        auto writes = queuedDrawWrites(tail, graphics, stages);
        if (PipeWriteDiag()) NoteDrawWriteTags(graphics, stages);
        const auto commitQueue = submission.queue;
        const auto commitPacket = tail.packetOpcode;
        const auto epoch = DrawFinisher::OnFinisherThread() ? tail.epoch : DrawPipeline::EpochToken().load(std::memory_order_relaxed);
        const auto ticket = DrawFinisher::OnFinisherThread() ? tail.ticket : GuestMemory::Gen::Required();
        auto reads = QueuedDrawReadRanges(drawParameters, stages);
        // Stage 4.1/4.2 (APS5_ARCHB_PIPE_RESOLVE_SPLIT, sampled per draw): the item's resolve runs the
        // closure with `resolving` set (ResolveDraw into `resolution`, on the committer or a resolver),
        // and the commit hands `resolution` to its preparation.
        const bool resolveSplit = DrawPipeline::ResolveSplitActive() && Graphics::DrawPrelockEnabled();
        if (tail.gatherPlan != nullptr) {
            for (std::size_t i = 0; i < tail.gatherPlan->stages.size() && i < programResults.size(); ++i) tail.gatherPlan->stages[i].program = programResults[i];
        }
        auto commit = [this, gatherPlan = std::move(tail.gatherPlan), pipelineStateReuse = tail.pipelineStateReuse, memoVirtualQuery = tail.memoVirtualQuery, allocChurn = tail.allocChurn, partialSnapshot = tail.partialSnapshot, hostSlabAlloc = tail.hostSlabAlloc, cheapTimers = tail.cheapTimers, recorderKey = tail.recorderKey, identity = ProgramIdentity(tail.programs), localDevice = std::move(localDevice), decode = std::move(decode), drawParameters, stages = std::move(stages), snapshots = std::move(snapshots), recipe = std::move(recipe), recipeStages = std::move(recipeStages), drawKey, recipeDonor, shape = tail.shapeKey, commitQueue, commitPacket, epoch, ticket, trace = trace, writablePrograms = std::move(writablePrograms), results = std::move(results), memory = std::move(memory), linked = std::move(linked), stageCaptures = std::move(stageCaptures), matched = std::move(matched), matchedRegions = std::move(matchedRegions), fresh = std::move(fresh), decodeReads = std::move(decodeReads), shaderMemory = std::move(shaderMemory), entry = std::move(entry), dataEntry = std::move(dataEntry), compiledStages = std::move(compiledStages), stageData = std::move(stageData), nostageWhy, resolution = Graphics::DrawPreparation{}](bool resolving, std::vector<DrawPipeline::Range>* resolveReads) mutable -> std::uint64_t {
            const HostAllocator::Scope hostSlabScope(hostSlabAlloc);
            const ArchB::CheapClock::Scope cheapTimersScope(cheapTimers);
            if (resolving) {
                DrawPipeline::FollowResolveEpoch(epoch);
                return localDevice->ResolveDraw(decode->state, drawParameters, stages, recipe.get(), memoVirtualQuery, resolution, *resolveReads);
            }
            ArchB::Recorder::Work recorderWork(recorderKey, ArchB::Recorder::Phase::Record, false);
            const ArchB::AllocChurnScope allocChurnScope(allocChurn);
            const ArchB::PartialSnapshotScope partialSnapshotScope(partialSnapshot);
            PerformanceTimer recorderTiming("Driver.DrawCommit");
            DrawPipeline::FollowEpoch(epoch, ticket);
            GuestMemory::SetCurrentPacket(commitPacket, commitQueue);
            const auto committing = identity.programs != 0 ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            Graphics::DrawSlotIdentity slotIdentity;
            bool identified = false;
            if ((drawParameters.indirect.has_value() || ArchB::DirectDrawEnabled()) && Graphics::ArchcReplayActive()) {
                const auto ownerOf = [&](const ShaderRecompiler::RecompileResult* program) -> std::shared_ptr<const void> {
                    for (const auto& data : stageData) {
                        if (data.compiled != nullptr && data.compiled.get() == program) return data.compiled;
                    }
                    for (const auto& variant : matched) {
                        if (variant != nullptr && variant->compiled != nullptr && variant->compiled.get() == program) return variant->compiled;
                    }
                    for (const auto& variant : fresh) {
                        if (variant != nullptr && variant->compiled != nullptr && variant->compiled.get() == program) return variant->compiled;
                    }
                    for (const auto& compiled : compiledStages) {
                        if (compiled != nullptr && compiled.get() == program) return compiled;
                    }
                    return {};
                };
                identified = decode != nullptr;
                slotIdentity.state = decode != nullptr && decode->stateOrigin != nullptr ? std::shared_ptr<const void>(decode->stateOrigin) : std::shared_ptr<const void>(decode);
                slotIdentity.stages.reserve(stages.size());
                for (const auto& stage : stages) {
                    Graphics::DrawSlotStage entry;
                    for (std::size_t i = 0; i < stageData.size() && i < matched.size(); ++i) {
                        if (stageData[i].compiled == nullptr || stageData[i].compiled.get() != stage.program || matched[i] == nullptr || matched[i]->compiled == nullptr) continue;
                        entry.owner = matched[i]->compiled;
                        entry.variant = matched[i];
                        entry.words = stageData[i].words;
                        entry.patched = true;
                        entry.bufferBases = stageData[i].bufferBaseHit;
                        break;
                    }
                    if (!entry.patched) entry.owner = ownerOf(stage.program);
                    if (entry.owner == nullptr) identified = false;
                    slotIdentity.stages.push_back(std::move(entry));
                }
            }
            if (gatherPlan != nullptr) {
                slotIdentity.gatherTemplate = gatherPlan->commitTemplate;
                slotIdentity.learning = gatherPlan.get();
            }
            if (gatherPlan != nullptr) CheckGatherDrawPlan(*gatherPlan, stages, GatherTargetAddress(decode->state), GatherIndirectOf(drawParameters));
            const bool nostageCensus = nostageWhy >= 0 && Graphics::ArchcNostageCensus();
            if (nostageCensus) Graphics::ArchcNostageArm(nostageWhy);
            const auto commitStarted = std::chrono::steady_clock::now();
            commitDraw(localDevice, commitQueue, decode->state, drawParameters, stages, snapshots, recipe, recipeStages, drawKey, recipeDonor, trace, shape, pipelineStateReuse, memoVirtualQuery, resolution.resolved ? &resolution : nullptr, identified ? &slotIdentity : nullptr);
            ArchB::Recorder::Count("draw_commit_old_ns", static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - commitStarted).count()));
            ArchB::Recorder::Count("draw_commit_old_n");
            {
                const auto commitNs = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - commitStarted).count());
                const bool direct = !drawParameters.indirect.has_value();
                ArchB::Recorder::Count(direct ? (recipe != nullptr ? "draw_commit_old_direct_recipe_ns" : "draw_commit_old_direct_plain_ns") : "draw_commit_old_indirect_ns", commitNs);
                ArchB::Recorder::Count(direct ? (recipe != nullptr ? "draw_commit_old_direct_recipe_n" : "draw_commit_old_direct_plain_n") : "draw_commit_old_indirect_n");
            }
            if (nostageCensus) Graphics::ArchcNostageArm(-1);
            if (identity.programs != 0) addDrawCommit(identity, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - committing).count());
            return 0;
        };
        auto held = std::make_shared<decltype(commit)>(std::move(commit));
        std::shared_ptr<DrawPipeline::ResolveSlot> resolveSlot;
        if (resolveSplit) {
            resolveSlot = std::make_shared<DrawPipeline::ResolveSlot>();
            resolveSlot->run = [held](std::vector<DrawPipeline::Range>& resolveReads) { return (*held)(true, &resolveReads); };
        }
        DrawPipeline::Queue0().Enqueue(DrawPipeline::CommitKind::Draw, [held] { (*held)(false, nullptr); }, std::move(writes), 0, {}, std::move(reads), true, DrawPipeline::ItemKind::Draw, std::move(resolveSlot));
        phaseTiming.Phase(DrawRowLockWait);
        timing.Mark("draw_enqueued");
        return drawn();
    }
    if (drawParameters.indirect) drainBeforeInline(DrawPipeline::DrainCause::IndirectInline);
    prepareForDraw(drawParameters, recipe.get());
    lockForDraw();
    runCaptureCheckHook();
    if (tail.queue != nullptr && capturedMemoryChanged(memory, snapshots)) {
        gpuLock.unlock();
        const RecaptureScope recapture;
        return draw(*tail.queue, tail.packet, submission, tail.packetOffset, rejected);
    }
    noteDrawWriters(stages, submission.queue);
    if (tail.gatherPlan != nullptr) {
        for (std::size_t i = 0; i < tail.gatherPlan->stages.size() && i < programResults.size(); ++i) tail.gatherPlan->stages[i].program = programResults[i];
        CheckGatherDrawPlan(*tail.gatherPlan, stages, GatherTargetAddress(graphics), GatherIndirectOf(drawParameters));
        tail.gatherPlan = nullptr;
    }
    phaseTiming.Phase(DrawRowVectors);
    if (recipe != nullptr) {
        if (localDevice->DrawFromRecipe(graphics, drawParameters, stages, snapshots, recipe, tracePtr, &lockTiming.preparation, tail.pipelineStateReuse) == RecipeOutcome::Recorded) {
            phaseTiming.Phase(DrawRowGraphics);
            timing.Mark("draw_and_resource_release");
            return drawn();
        }

        VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, VulkanDevice::RecipeKind::Draw);
    }
    std::shared_ptr<const DrawRecipe> built;
    localDevice->Draw(graphics, drawParameters, stages, snapshots, recipeStages.empty() ? nullptr : &built, tracePtr, tail.shapeKey, &lockTiming.preparation, tail.pipelineStateReuse);
    phaseTiming.Phase(DrawRowGraphics);
    if (built != nullptr) attachDrawRecipe(drawKey, recipeStages, std::move(built), recipeDonor);
    timing.Mark("draw_and_resource_release");
    return drawn();
}

void Driver::restartFinishedDraw(DrawTail& tail) {
    using Stage = ShaderRecompiler::ShaderStage;
    using Role = ShaderRecompiler::ProgramRole;
    const auto changed = [](const DrawProgram& program) {
        return std::any_of(program.memory.begin(), program.memory.end(), [](const ShaderRecompiler::MemoryRegion& region) { return !region.bytes.empty() && GuestMemory::CompareMapped(region.guestAddress, region.bytes) != GuestMemory::Compare::Equal; });
    };
    if (std::any_of(tail.programs->begin(), tail.programs->end(), changed)) {
        if (tail.programs != &tail.writablePrograms) {
            tail.writablePrograms = *tail.programs;
            tail.programs = &tail.writablePrograms;
        }
        for (auto& program : tail.writablePrograms) {
            if (program.nullPixel || !changed(program)) continue;
            auto snapshot = ProgramSnapshot(*tail.submission->shaders, program.binary.codeAddress, {program.snapshot->type});
            if (snapshot == program.snapshot) continue;
            const auto& fresh = *snapshot;
            program.codeOffset = static_cast<std::size_t>((program.binary.codeAddress - fresh.codeAddress) / sizeof(std::uint32_t));
            program.binary.code = std::span(fresh.code).subspan(program.codeOffset);
            program.binary.headerAddress = fresh.headerAddress;
            program.binary.header = fresh.header;
            program.memory = {{{fresh.codeAddress, std::as_bytes(std::span(fresh.code))}, {fresh.headerAddress, fresh.header}}};
            if (fresh.header.empty()) NoteRawGraphicsDraw(fresh);
            program.snapshot = std::move(snapshot);
        }
    }
    const auto& programs = *tail.programs;
    const auto& graphics = tail.decode->state;
    const auto& roles = tail.decode->roles;
    const auto count = programs.size();
    tail.gatherPlan.reset();
    tail.live.reset();
    tail.liveEntry.reset();
    tail.drawParameters = tail.resolvedParameters;
    tail.memory.clear();
    AppendProgramMemory(tail.memory, programs);
    tail.results.clear();
    tail.compiledStages.clear();
    tail.stages.clear();
    tail.pushCursorBytes = 0;
    tail.programResults.assign(count, nullptr);
    tail.stageCaptures.assign(count, {});
    tail.matched.assign(count, nullptr);
    tail.matchedRegions.assign(count, {});
    tail.fresh.assign(count, nullptr);
    tail.recompiled.assign(count, false);
    tail.dataCandidates.assign(count, {});
    tail.reused.assign(count, false);
    tail.stageData.assign(count, {});
    tail.dataEntry = nullptr;
    tail.drawHit = false;
    tail.verifyHit = false;
    tail.patchedStages = false;
    for (std::size_t i = 0; i < count; ++i) {
        tail.decodeReads[i].clear();
        tail.vertexInfos[i].reset();
        const auto& program = programs[i];
        if (program.binary.stage == Stage::Fragment || roles[i] == Role::GeometryBack) continue;
        tail.vertexInfos[i] = DecodeOrderedVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData, tail.decodeReads[i], 8u - program.firstUserSgpr);
        if (!tail.vertexInfos[i]) continue;
        tail.vertexInfos[i]->paClVsOutCntl = PositionExportLayout(program.snapshot.get(), program.codeOffset, program.binary.code, tail.reg207, program.tailContinuation.get()) | graphics.layerExports;
        tail.vertexInfos[i]->depthRemapMode = graphics.depthRemap.mode;
        Graphics::ApplyPointSize(graphics, *tail.vertexInfos[i]);
    }
    tail.shaderMemory = std::make_unique<ShaderMemory>(tail.memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter(), &QueryPendingSpan);
}

namespace {

namespace archcSlotCensus {

enum Kind : std::size_t { RecipeHit, RecipeMiss, Indirect, NoStages, BuildNotRequested, BuildDisabled, BuildUncacheable, BuildWaited, BuildIndirect, BuildNotReusable, BuildMovedDisabled, BuildMultisampled, BuildReordered, BuildBuilt, Unknown, IndirectReplayed, IndirectVerified, IndirectRefused, IndirectNew, KindCount };
constexpr std::array<const char*, KindCount> KindNames{"recipe_hit", "recipe_miss", "indirect", "no_stages", "build_not_requested", "build_disabled", "build_uncacheable", "build_waited", "build_indirect", "build_not_reusable", "build_moved_disabled", "build_multisampled", "build_reordered", "build_built", "unknown", "indirect_replayed", "indirect_verified", "indirect_refused", "indirect_new"};

struct Counters {
    std::array<std::atomic<std::uint64_t>, KindCount> draws{};
    std::array<std::atomic<std::uint64_t>, KindCount> ns{};
    std::array<std::atomic<std::uint64_t>, KindCount> lockedNs{};
    Counters() {
        for (std::size_t i = 0; i < KindCount; ++i) {
            ArchB::Recorder::RegisterCounter((new std::string(std::string("archc_slot_draws_") + KindNames[i]))->c_str(), draws[i]);
            ArchB::Recorder::RegisterCounter((new std::string(std::string("archc_slot_ns_") + KindNames[i]))->c_str(), ns[i]);
            ArchB::Recorder::RegisterCounter((new std::string(std::string("archc_slot_locked_ns_") + KindNames[i]))->c_str(), lockedNs[i]);
        }
    }
};

bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_ARCHB_C_SLOT_CENSUS");
        return value != nullptr && value[0] == '1';
    }();
    return on;
}

Counters& counters() {
    static Counters made;
    return made;
}

std::uint64_t now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Probe {
    bool on = enabled();
    std::uint64_t start = on ? now() : 0;
    std::uint64_t locked = 0;
    Kind kind = Unknown;
    ~Probe() {
        if (!on) return;
        const auto end = now();
        auto& made = counters();
        made.draws[kind].fetch_add(1, std::memory_order_relaxed);
        made.ns[kind].fetch_add(end - start, std::memory_order_relaxed);
        if (locked != 0) made.lockedNs[kind].fetch_add(end - locked, std::memory_order_relaxed);
    }
    void Locked() {
        if (on) locked = now();
    }
    void Replay() {
        const auto outcome = Graphics::ArchcTakeReplayOutcome();
        if (!on || kind != Indirect || outcome < 0) return;
        kind = static_cast<Kind>(IndirectReplayed + static_cast<std::size_t>(outcome));
    }
    void Built() {
        if (!on) return;
        const auto build = Graphics::ArchcTakeRecipeBuild();
        if (kind == RecipeMiss) return;
        kind = build < 0 ? Unknown : static_cast<Kind>(BuildNotRequested + static_cast<std::size_t>(build));
    }
};

}

}

void Driver::commitDraw(std::shared_ptr<VulkanDevice> localDevice, std::uint32_t queue, const Graphics::State& graphics, const Pm4::DrawParameters& drawParameters, std::span<const Graphics::CompiledShader> stages, std::span<const Graphics::GuestMemorySnapshot> snapshots, const std::shared_ptr<const DrawRecipe>& recipe, const std::vector<std::shared_ptr<DispatchVariant>>& recipeStages, std::uint64_t drawKey, const std::shared_ptr<DrawEntry>& recipeDonor, VulkanDevice::DrawTraceContext trace, std::uint64_t shape, bool pipelineStateReuse, bool memoVirtualQuery, Graphics::DrawPreparation* resolved, const Graphics::DrawSlotIdentity* identity) {
    archcSlotCensus::Probe slotProbe;
    if (slotProbe.on) {
        static_cast<void>(Graphics::ArchcTakeRecipeBuild());
        slotProbe.kind = drawParameters.indirect ? archcSlotCensus::Indirect : recipeStages.empty() ? archcSlotCensus::NoStages : archcSlotCensus::Unknown;
    }
    const auto* tracePtr = trace.token != 0 ? &trace : nullptr;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    DrawLockTiming lockTiming{profile};
    GuestMemory::HoldLogMark(GuestMemory::HoldMark::PrelockBegin);
    const bool directReplay = !drawParameters.indirect && ArchB::DirectDrawEnabled() && Graphics::ArchcReplayActive();
    const bool replayLikely = identity != nullptr && recipe == nullptr && (identity->gather != nullptr || Graphics::ArchcReplayLikely(localDevice->Device(), graphics, drawParameters, stages, identity, pipelineStateReuse));
    const bool nostageTimed = Graphics::ArchcNostageArmed();
    const auto nostageNow = [] { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); };
    std::array<std::uint64_t, 6> nostageAt{};
    if (nostageTimed) nostageAt[0] = nostageNow();
    if (Graphics::DrawPrelockEnabled() && !replayLikely) {
        const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        static const bool aheadCensus = GuestMemory::CollectAheadCensus() || GuestMemory::CommitterCollectCensus();
        struct PrelockPhase {
            bool on;
            explicit PrelockPhase(bool active) : on(active) {
                if (on) GuestMemory::SetCommitterPrelock(true);
            }
            ~PrelockPhase() {
                if (on) GuestMemory::SetCommitterPrelock(false);
            }
        } prelockPhase{aheadCensus};
        lockTiming.preparation = localDevice->PrepareDraw(graphics, drawParameters, stages, recipe.get(), memoVirtualQuery, resolved);
        if (profile) lockTiming.prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    GuestMemory::HoldLogMark(GuestMemory::HoldMark::PrelockEnd);
    if (nostageTimed) nostageAt[1] = nostageNow();
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    lockTiming.Start();
    slotProbe.Locked();
    if (nostageTimed) nostageAt[2] = nostageNow();
    if (auto current = device.Load(); current != nullptr && current != localDevice) localDevice = std::move(current);
    struct SubmitDue {
        Driver& driver;
        const std::shared_ptr<VulkanDevice>& device;
        std::array<std::uint64_t, 6>* census;
        ~SubmitDue() noexcept(false) {
            if (std::uncaught_exceptions() != 0) return;
            const auto now = [] { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); };
            const auto before = census != nullptr ? now() : 0;
            driver.submitDueAfterCommit(device.get());
            if (census != nullptr) {
                const auto& at = *census;
                Graphics::ArchcNostageWrapper(at[1] - at[0], at[2] - at[1], at[3] - at[2], at[4] - at[3], at[5] - at[4], before - at[5], now() - before);
            }
        }
    } submitDue{*this, localDevice, nostageTimed ? &nostageAt : nullptr};
    recordLabelsForPacket(localDevice.get(), queue);
    if (nostageTimed) nostageAt[3] = nostageNow();
    noteDrawWriters(stages, queue);
    if (nostageTimed) nostageAt[4] = nostageNow();
    if (recipe != nullptr) {
        if (localDevice->DrawFromRecipe(graphics, drawParameters, stages, snapshots, recipe, tracePtr, &lockTiming.preparation, pipelineStateReuse, directReplay ? identity : nullptr) == RecipeOutcome::Recorded) {
            slotProbe.kind = archcSlotCensus::RecipeHit;
            return;
        }
        slotProbe.kind = archcSlotCensus::RecipeMiss;
        VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, VulkanDevice::RecipeKind::Draw);
    }
    std::shared_ptr<const DrawRecipe> built;
    localDevice->Draw(graphics, drawParameters, stages, snapshots, recipeStages.empty() ? nullptr : &built, tracePtr, shape, &lockTiming.preparation, pipelineStateReuse, recipe == nullptr || directReplay ? identity : nullptr);
    if (nostageTimed) nostageAt[5] = nostageNow();
    slotProbe.Replay();
    if (slotProbe.kind != archcSlotCensus::Indirect && slotProbe.kind < archcSlotCensus::IndirectReplayed && slotProbe.kind != archcSlotCensus::NoStages) slotProbe.Built();
    if (built != nullptr) attachDrawRecipe(drawKey, recipeStages, std::move(built), recipeDonor);
}

}
