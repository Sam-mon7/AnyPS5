#include "prx/libSceAgcDriver/Execution/include/ArchB/Switches.hpp"
#include "prx/libSceAgcDriver/Execution/include/ArchB/CheapClock.hpp"
#include "prx/libSceAgcDriver/Execution/include/Screenshot.hpp"
#include "prx/libc/include/HostThreadSlot.hpp"

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
#include <utility>

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
constexpr Feature GenShadowPrerequisites[] = {Feature::GenTable};
constexpr Feature GenLockfreePrerequisites[] = {Feature::GenTable};
constexpr Feature GenDrawValidationPrerequisites[] = {Feature::GenTable};
constexpr Feature PipeKeepIndirectRegsPrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature PipeIndirectGpuArgsPrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature PipeNarrowDrawWritesPrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature PipeNarrowSlicedDepthPrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature PipeNarrowDispatchWritesPrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature PipeKeepOwnLabelWaitsPrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature FixDrawSetPoolPrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature PipeDeepQueuePrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature PipeCommitterMutexPriorityPrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature PipeFullHysteresisPrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature PipeDispatchAliasPrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature PipeCollectSharePrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature PipeCopyKernelItemsPrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature PipeDrawReusePrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature PipeDrawLookupMemoPrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature WtFaultPrerequisites[] = {Feature::WtFault};
constexpr Feature ComputeSnapshotWaitPrerequisites[] = {Feature::FixComputeSyncUnlocked, Feature::FixPartialSnapshot};
constexpr Feature PipeResolveSplitPrerequisites[] = {Feature::PipeQ0Committer};
constexpr Feature PipeResolvePoolPrerequisites[] = {Feature::PipeQ0Committer, Feature::PipeResolveSplit};
constexpr Feature PrelockStampSkipPrerequisites[] = {Feature::WtFault, Feature::PipeResolveProofs};
constexpr Feature ArchcImageEpochsPrerequisites[] = {Feature::WtFault, Feature::WtFaultConsumers};
constexpr Feature ArchcTargetFastpathPrerequisites[] = {Feature::ArchcImageEpochs};
constexpr Feature ArchcTemplateTokenPrerequisites[] = {Feature::ArchcImageEpochs, Feature::PipeResolveProofs};
constexpr Feature ArchcDrawReplayPrerequisites[] = {Feature::ArchcTargetFastpath, Feature::ArchcTemplateToken, Feature::PipeQ0Committer};
constexpr Feature CLazyPublishPrerequisites[] = {Feature::WtFault, Feature::WtFaultConsumers};
constexpr Feature CWatchSplitPrerequisites[] = {Feature::WtFault};
constexpr Feature CLazyStoreHookPrerequisites[] = {Feature::CLazyPublish};
constexpr Feature CReplaySplitFacesPrerequisites[] = {Feature::ArchcDrawReplay};
constexpr Feature CGpuUploadCarryPrerequisites[] = {Feature::CGpuStagingCarry};
constexpr Feature CGpuBlockPublishPrerequisites[] = {Feature::CGpuStagingCarry, Feature::WtFault, Feature::WtFaultConsumers};
constexpr Feature CLazyCopybackPrerequisites[] = {Feature::CGpuStagingCarry};
constexpr Feature ArchcReplayCutPrerequisites[] = {Feature::ArchcDrawReplay};
constexpr Feature CLazyFenceDepthPrerequisites[] = {Feature::CLazyFence};
constexpr Feature CLabelLazyPrewaitPrerequisites[] = {Feature::CLazyPublish};
constexpr Feature DrawTemplatesLivePrerequisites[] = {Feature::DrawTemplates, Feature::DrawGatherShadow};
constexpr Feature DrawTemplatesPrerequisites[] = {Feature::ArchcDrawReplay, Feature::ZeroCopyStorage};
constexpr Feature LazyShadowPublishPrerequisites[] = {Feature::CLazyPublish};
constexpr Feature DepthRunRepeatPrerequisites[] = {Feature::ArchcReplayDepthEpoch};
constexpr std::array<SwitchInfo, static_cast<std::size_t>(Feature::Count)> Entries{{
    {Feature::GateRun, "GATE_RUN", false, false, {}},
    {Feature::GateNoop, "GATE_NOOP", false, true, {}},
    {Feature::Recorder, "RECORDER", false, true, {}},
#ifdef APS5_ARCHB_SWITCHES_TEST
    {Feature::TestDependent, "TEST_DEPENDENT", true, false, TestPrerequisites},
    {Feature::TestRuntimeDependent, "TEST_RUNTIME_DEPENDENT", false, true, TestRuntimePrerequisites},
#endif
    {Feature::UbTrustProofs, "UB_TRUST_PROOFS", false, true, {}},
    {Feature::UbNoLockInvalidation, "UB_NO_LOCK_INVALIDATION", false, true, {}},
    {Feature::UbPointerFreeDrawKeys, "UB_POINTER_FREE_DRAW_KEYS", false, true, {}},
    {Feature::UbSkipCpuIndirect, "UB_SKIP_CPU_INDIRECT", false, true, {}},
    {Feature::UbSkipDispatchProofWalk, "UB_SKIP_DISPATCH_PROOF_WALK", false, true, {}},
    {Feature::FixTsharpFeedbackMask, "FIX_TSHARP_FEEDBACK_MASK", false, true, {}},
    {Feature::FixPipelineStateReuse, "FIX_PIPELINE_STATE_REUSE", false, true, {}},
    {Feature::FixVqMemo, "FIX_VQ_MEMO", false, true, {}},
    {Feature::FixImageTableSnapshotReuse, "FIX_IMAGE_TABLE_SNAPSHOT_REUSE", false, true, {}},
    {Feature::FixImageTableBitScanKeys, "FIX_IMAGE_TABLE_BITSCAN_KEYS", false, true, {}},
    {Feature::FixBufferBaseRefresh, "FIX_BUFFER_BASE_REFRESH", false, true, {}},
    {Feature::FixAllocChurn, "FIX_ALLOC_CHURN", false, true, {}},
    {Feature::FixComputeSyncUnlocked, "FIX_COMPUTE_SYNC_UNLOCKED", false, true, {}},
    {Feature::FixWriteTrackingBookkeeping, "FIX_WRITE_TRACKING_BOOKKEEPING", false, true, {}},
    {Feature::GenTable, "GEN_TABLE", false, true, {}},
    {Feature::GenShadow, "GEN_SHADOW", false, true, GenShadowPrerequisites},
    {Feature::GenLockfree, "GEN_LOCKFREE", false, true, GenLockfreePrerequisites},
    {Feature::GenDrawValidation, "GEN_DRAW_VALIDATION", false, true, GenDrawValidationPrerequisites},
    {Feature::PipeQ0Committer, "PIPE_Q0_COMMITTER", false, true, {}},
    {Feature::PipeKeepIndirectRegs, "PIPE_KEEP_INDIRECT_REGS", false, true, PipeKeepIndirectRegsPrerequisites},
    {Feature::PipeIndirectGpuArgs, "PIPE_INDIRECT_GPU_ARGS", false, true, PipeIndirectGpuArgsPrerequisites},
    {Feature::PipeNarrowDrawWrites, "PIPE_NARROW_DRAW_WRITES", false, true, PipeNarrowDrawWritesPrerequisites},
    {Feature::PipeNarrowSlicedDepth, "PIPE_NARROW_SLICED_DEPTH", false, true, PipeNarrowSlicedDepthPrerequisites},
    {Feature::PipeNarrowDispatchWrites, "PIPE_NARROW_DISPATCH_WRITES", false, true, PipeNarrowDispatchWritesPrerequisites},
    {Feature::PipeKeepOwnLabelWaits, "PIPE_KEEP_OWN_LABEL_WAITS", false, true, PipeKeepOwnLabelWaitsPrerequisites},
    {Feature::FixDrawSetPool, "FIX_DRAW_SET_POOL", false, true, FixDrawSetPoolPrerequisites},
    {Feature::PipeDeepQueue, "PIPE_DEEP_QUEUE", false, true, PipeDeepQueuePrerequisites},
    {Feature::PipeCommitterMutexPriority, "PIPE_COMMITTER_MUTEX_PRIORITY", false, true, PipeCommitterMutexPriorityPrerequisites},
    {Feature::PipeFullHysteresis, "PIPE_FULL_HYSTERESIS", false, true, PipeFullHysteresisPrerequisites},
    {Feature::PipeDispatchAlias, "PIPE_DISPATCH_ALIAS", false, true, PipeDispatchAliasPrerequisites},
    {Feature::PipeCollectShare, "PIPE_COLLECT_SHARE", false, true, PipeCollectSharePrerequisites},
    {Feature::FixDispatchRelocation, "FIX_DISPATCH_RELOCATION", false, true, {}},
    {Feature::PipeCopyKernelItems, "PIPE_COPY_KERNEL_ITEMS", false, true, PipeCopyKernelItemsPrerequisites},
    {Feature::FixStableFailedCollects, "FIX_STABLE_FAILED_COLLECTS", false, true, {}},
    {Feature::PipeDrawReuse, "PIPE_DRAW_REUSE", false, true, PipeDrawReusePrerequisites},
    {Feature::PipeDrawLookupMemo, "PIPE_DRAW_LOOKUP_MEMO", false, true, PipeDrawLookupMemoPrerequisites},
    {Feature::FixStableFailedWide, "FIX_STABLE_FAILED_WIDE", false, true, {}},
    {Feature::FixSnapshotPageRefresh, "FIX_SNAPSHOT_PAGE_REFRESH", false, true, {}},
    {Feature::FixPartialSnapshot, "FIX_PARTIAL_SNAPSHOT", false, true, {}},
    {Feature::FixSnapshotCandidates, "FIX_SNAPSHOT_CANDIDATES", false, true, {}},
    {Feature::FixMovedWordUpload, "FIX_MOVED_WORD_UPLOAD", false, true, {}},
    {Feature::FixUnstampedAlias, "FIX_UNSTAMPED_ALIAS", false, true, {}},
    {Feature::FixPipelineKeyWriter, "FIX_PIPELINE_KEY_WRITER", false, true, {}},
    {Feature::GpuStagingCarry, "GPU_STAGING_CARRY", false, true, {}},
    // Write tracking by page protection (Stage 4 option 2): startup-only (the protected pages are set up
    // with the write tracker), VERIFY keeps GetWriteWatch as the reference, CONSUMERS answers the
    // commit-time freshness checks from the fault stamps (runtime: both arms of a gate track faults).
    {Feature::WtFault, "WT_FAULT", false, false, {}},
    {Feature::WtFaultVerify, "WT_FAULT_VERIFY", false, false, WtFaultPrerequisites},
    {Feature::WtFaultConsumers, "WT_FAULT_CONSUMERS", false, true, WtFaultPrerequisites},
    {Feature::WtFaultDirtyArm, "WT_FAULT_DIRTY_ARM", false, true, WtFaultPrerequisites},
    {Feature::PipeResolveSplit, "PIPE_RESOLVE_SPLIT", false, true, PipeResolveSplitPrerequisites},
    {Feature::PipeResolvePool, "PIPE_RESOLVE_POOL", false, true, PipeResolvePoolPrerequisites},
    {Feature::PipeResolveProofs, "PIPE_RESOLVE_PROOFS", false, true, WtFaultPrerequisites},
    {Feature::PipeResolveHandoff, "PIPE_RESOLVE_HANDOFF", false, true, PipeResolveSplitPrerequisites},
    {Feature::PageStateMemo, "PAGE_STATE_MEMO", false, true, {}},
    {Feature::PipeResolveSnapshots, "PIPE_RESOLVE_SNAPSHOTS", false, true, WtFaultPrerequisites},
    {Feature::WtSyncSkip, "WT_SYNC_SKIP", false, true, WtFaultPrerequisites},
    {Feature::TargetSetupMemo, "TARGET_SETUP_MEMO", false, true, {}},
    // Zero-copy draw inputs (Stage 4, 2026-11-04): read-only guest storage elements and vertex/index
    // ranges inside host imports are bound in place instead of snapshotted (see PrepareDrawBindings,
    // InPlaceDrawInput); _VERIFY snapshots anyway and checks the snapshot against guest memory at
    // submit and at GPU completion (Recorder zero-copy checks).
    {Feature::ZeroCopyStorage, "ZERO_COPY_STORAGE", false, true, {}},
    {Feature::ZeroCopyInputs, "ZERO_COPY_INPUTS", false, true, {}},
    // Census 2 (2026-11-04): the committer's unlocked draw preparation skips a template's surface
    // precollect when the template's stamp proof is armed (its locked proof then collects nothing).
    {Feature::PrelockStampSkip, "PRELOCK_STAMP_SKIP", false, true, PrelockStampSkipPrerequisites},
    // Census 2: the locked draw takes the template its unlocked preparation found (no second
    // resource-cache Find) while the cache index did not change in between.
    {Feature::PrelockFindHandoff, "PRELOCK_FIND_HANDOFF", false, true, {}},
    // Census 2: APS5_PRIVATE_COMPUTE=1 + APS5_PRIVATE_COMPUTE_ALL=1 as a runtime switch (sampled per
    // dispatch), so an alternating gate can toggle it: compute queues 0x20.. record bind+dispatch into
    // per-thread secondaries without the GPU mutex.
    {Feature::PrivateCompute, "PRIVATE_COMPUTE", false, true, {}},
    // Draws with moved buffers bind an immutable per-template set, cached by the moved content: moved
    // addresses read in place through their import, data words in a buffer of the set's own.
    {Feature::ZeroCopyMoved, "ZERO_COPY_MOVED", false, true, {}},
    {Feature::CGpuStagingCarry, "C_GPU_STAGING_CARRY", false, true, {}},
    {Feature::CGpuBlockPublish, "C_GPU_BLOCK_PUBLISH", false, true, CGpuBlockPublishPrerequisites},
    {Feature::CGpuUploadCarry, "C_GPU_UPLOAD_CARRY", false, true, CGpuUploadCarryPrerequisites},
    {Feature::CReplaySplitFaces, "C_REPLAY_SPLIT_FACES", false, true, CReplaySplitFacesPrerequisites},
    {Feature::ArchcImageEpochs, "C_IMAGE_EPOCHS", false, true, ArchcImageEpochsPrerequisites},
    {Feature::ArchcTargetFastpath, "C_TARGET_FASTPATH", false, true, ArchcTargetFastpathPrerequisites},
    {Feature::CLazyPublish, "C_LAZY_PUBLISH", false, true, CLazyPublishPrerequisites},
    // Arch C P5b: a census or lazy watch run that spans guest allocations (direct memory is committed in
    // 16 KiB allocations) is protected one allocation at a time instead of failing whole.
    {Feature::CWatchSplit, "C_WATCH_SPLIT", false, true, CWatchSplitPrerequisites},
    // Arch C P5b: a driver store into a lazy label range publishes and opens the range first (the service,
    // without the fault it would take under the GPU mutex).
    {Feature::CLazyStoreHook, "C_LAZY_STORE_HOOK", false, true, CLazyStoreHookPrerequisites},
    // Arch C P5b: a staged region's whole copy-back is recorded as a cancellable indirect copy
    // (VK_KHR_copy_memory_indirect) and cancelled before submit when the same shadow's next use in the
    // same batch carries it and copies it back whole, with nothing reading or writing the range between.
    {Feature::CLazyCopyback, "C_LAZY_COPYBACK", false, true, CLazyCopybackPrerequisites},
    {Feature::ArchcTemplateToken, "C_TEMPLATE_TOKEN", false, true, ArchcTemplateTokenPrerequisites},
    {Feature::ArchcDrawReplay, "C_DRAW_REPLAY", false, true, ArchcDrawReplayPrerequisites},
    // Arch C P3b: per-replay cuts of a C_DRAW_REPLAY slot hit (each with _VERIFY=N, see Draw.cpp archcSlots).
    // QUIET_TEMPLATE: the template's whole Revalidate is skipped while none of the serials its no-op pass
    // depends on moved since the slot's last passing proof; DEPTH_EPOCH: a repeat DepthSurfaceView of the
    // surface last drawn, with no depth event since, keeps only its write order; SLOT_BINDINGS: the slot keeps
    // its moved list and cached moved set, re-checking only the live overlap conditions.
    {Feature::ArchcReplayQuietTemplate, "C_REPLAY_QUIET_TEMPLATE", false, true, ArchcReplayCutPrerequisites},
    {Feature::ArchcReplayDepthEpoch, "C_REPLAY_DEPTH_EPOCH", false, true, ArchcReplayCutPrerequisites},
    {Feature::ArchcReplaySlotBindings, "C_REPLAY_SLOT_BINDINGS", false, true, ArchcReplayCutPrerequisites},
    // SLOT_INPUTS: the input step takes the slot's validated fragment outputs and stages (same decode, same
    // stage owners) instead of the validation memo and the state's pure validation checks.
    {Feature::ArchcReplaySlotInputs, "C_REPLAY_SLOT_INPUTS", false, true, ArchcReplayCutPrerequisites},
    // Arch C P3c (each with _VERIFY=N, see Draw.cpp archcSlots). PATCH_SLOTS: a data-word patched stage
    // without buffer-base words is keyed by its variant alone; the replay takes the moved list and the
    // bindings live, so the words reach only the moved data buffers. CONTINUE: a continued pass skips
    // Pipeline::Continue when the reuse state already holds the slot's pipeline, layout and inputs.
    {Feature::ArchcReplayPatchSlots, "C_REPLAY_PATCH_SLOTS", false, true, ArchcReplayCutPrerequisites},
    {Feature::ArchcReplayContinue, "C_REPLAY_CONTINUE", false, true, ArchcReplayCutPrerequisites},
    // Arch C P5c: a storage refresh's flush of aliased pending results records their write-back without
    // first waiting for the range's pending GPU writer; a driver access to the range in the refresh waits.
    {Feature::CLazyFence, "C_LAZY_FENCE", false, true, {}},
    {Feature::CLazyFenceDepth, "C_LAZY_FENCE_DEPTH", false, true, CLazyFenceDepthPrerequisites},
    // Arch C P8 (each with _VERIFY=N). COMPUTE_DEPTH_PROOF: the fast resource proof passes an element over a
    // depth surface when no stale plane, sync or pending clear of the surface lies in its range and the depth
    // surface set is the one the template's last full walk proved its objects under. COMPUTE_SUBMIT_MERGE: a
    // compute queue's end of submission leaves its recorded work in the open batch when its next submission is
    // already queued and no label is deferred (waiters submit labels they find in the open batch).
    {Feature::CComputeDepthProof, "C_COMPUTE_DEPTH_PROOF", false, true, {}},
    {Feature::CComputeSubmitMerge, "C_COMPUTE_SUBMIT_MERGE", false, true, {}},
    // Arch C P8: the stamp memo's pending-registry scan is skipped while a token made by a scan that found no
    // overlap holds (an ImageEpochs owner over the scanned ranges, source PendingInsert; no identity query).
    {Feature::CStampScanToken, "C_STAMP_SCAN_TOKEN", false, true, {}},
    // Arch C labels (_VERIFY=N): the compute-sync prewait before a dispatch's deferred labels (FIX_COMPUTE_SYNC_UNLOCKED,
    // StorageTexture::PrepareReadbackBeforeLabel) leaves out the ranges whose label-time store C_LAZY_PUBLISH defers: no
    // store is recorded for them at the label, so their GPU writer need not finish first.
    {Feature::CLabelLazyPrewait, "C_LABEL_LAZY_PREWAIT", false, true, CLabelLazyPrewaitPrerequisites},
    // Arch C labels: a compute queue records a queued label another queue is already blocked on by a try before its next
    // self-locking packet (a dispatch), instead of at that packet's lock after its unlocked preparation.
    {Feature::CLabelAwaitedEarly, "C_LABEL_AWAITED_EARLY", false, true, {}},
    // Arch C labels: a WAIT_REG_MEM past its spin also sleeps on its queue slot's event, which the thread recording a
    // label over the awaited dword sets (the 200 us poll sleep ends early; what satisfies the wait is unchanged).
    {Feature::CLabelWake, "C_LABEL_WAKE", false, true, {}},
    {Feature::FixDerivedIndexCache, "FIX_DERIVED_INDEX_CACHE", false, true, {}},
    {Feature::FixComputeSnapshotWait, "FIX_COMPUTE_SNAPSHOT_WAIT", false, true, ComputeSnapshotWaitPrerequisites},
    {Feature::FixHostSlabAlloc, "FIX_HOST_SLAB_ALLOC", false, true, {}},
    {Feature::FixCheapTimers, "FIX_CHEAP_TIMERS", false, true, {}},
    {Feature::ReplayRecognize, "REPLAY_RECOGNIZE", false, true, {}},
    {Feature::FixStatePacketFast, "FIX_STATE_PACKET_FAST", false, true, {}},
    {Feature::FixDrawRelocNested, "FIX_DRAW_RELOC_NESTED", false, true, {}},
    {Feature::FixSampledArrayGpu, "FIX_SAMPLED_ARRAY_GPU", false, true, {}},
    {Feature::DrawGatherShadow, "DRAW_GATHER_SHADOW", false, true, {}},
    {Feature::DrawTemplates, "DRAW_TEMPLATES", false, true, DrawTemplatesPrerequisites},
    {Feature::DrawTemplatesLive, "DRAW_TEMPLATES_LIVE", false, true, DrawTemplatesLivePrerequisites},
    {Feature::GpuAuthorityCensus, "GPU_AUTHORITY_CENSUS", false, true, {}},
    {Feature::LazyShadowPublish, "LAZY_SHADOW_PUBLISH", false, true, LazyShadowPublishPrerequisites},
    {Feature::DepthRunRepeat, "DEPTH_RUN_REPEAT", false, true, DepthRunRepeatPrerequisites},
    {Feature::DepthCopyKeepLive, "DEPTH_COPY_KEEP_LIVE", false, true, {}},
    {Feature::DispatchAddressReuse, "DISPATCH_ADDRESS_REUSE", false, true, {}},
    {Feature::DispatchUploadReuse, "DISPATCH_UPLOAD_REUSE", false, true, {}},
    {Feature::DispatchPartialReproof, "DISPATCH_PARTIAL_REPROOF", false, true, {}},
}};
std::array<std::atomic<bool>, Entries.size()> values{};
std::array<bool, Entries.size()> fromEnvironment{};
std::array<std::atomic<std::uint64_t>, Entries.size()> upperBoundHits{};
constexpr std::pair<Feature, const char*> UpperBoundCounters[] = {
    {Feature::UbTrustProofs, "ub_trust_proofs_hits"},
    {Feature::UbNoLockInvalidation, "ub_no_lock_invalidation_hits"},
    {Feature::UbPointerFreeDrawKeys, "ub_pointer_free_draw_keys_hits"},
    {Feature::UbSkipCpuIndirect, "ub_skip_cpu_indirect_hits"},
    {Feature::UbSkipDispatchProofWalk, "ub_skip_dispatch_proof_walk_hits"},
};
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
// Set once the table is read: Enabled() then skips std::call_once, which with MinGW's libstdc++
// stores two emulated thread-locals on every call (about 30 ns per switch read).
std::atomic<bool> initializedFast{false};

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
    bool screenshot = false;
    bool any = false;
    while (stream >> token) {
        any = true;
        const auto equals = token.find('=');
        if (equals == std::string::npos) return refuse("invalid token " + token);
        const auto name = token.substr(0, equals);
        const auto value = std::string_view(token).substr(equals + 1);
        if (name == "SCREENSHOT") {
            if (screenshot || value != "1") return refuse("invalid SCREENSHOT");
            screenshot = true;
            continue;
        }
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
    if (screenshot) AgcDriver::Screenshot::Request();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (const auto& [feature, name] : UpperBoundCounters) {
        const auto hits = upperBoundHits[static_cast<std::size_t>(feature)].load(std::memory_order_relaxed);
        if (hits != 0) std::fprintf(stderr, "[archb] %s=%llu t_ns=%lld\n", name, static_cast<unsigned long long>(hits), static_cast<long long>(nanos));
    }
    std::string text;
    for (const auto& entry : Entries) {
        if (entry.runtime) text += " " + std::string(entry.name) + "=" + (pending[static_cast<std::size_t>(entry.feature)] ? "1" : "0");
    }
    text += " GATE_DELAY_US=" + std::to_string(delay);
    if (screenshot) text += " SCREENSHOT=1";
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
        if (values[static_cast<std::size_t>(Feature::FixCheapTimers)].load(std::memory_order_acquire)) static_cast<void>(CheapClock::GetCalibration());
        std::fprintf(stderr, "%s\n", describe().c_str());
        initializedFast.store(true, std::memory_order_release);
    });
}

bool Switches::Enabled(Feature feature) {
    if (!initializedFast.load(std::memory_order_acquire)) Initialize();
    return values.at(static_cast<std::size_t>(feature)).load(std::memory_order_acquire);
}

namespace {
struct AllocChurnSlot {};
struct PartialSnapshotSlot {};
}

bool DirectDrawEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_ARCHB_DRAW_DIRECT");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return enabled;
}

bool AllocChurnActive() noexcept {
    return HostThreadSlot<bool, AllocChurnSlot>::Get();
}

AllocChurnScope::AllocChurnScope(bool active) noexcept : previous(HostThreadSlot<bool, AllocChurnSlot>::Get()) {
    HostThreadSlot<bool, AllocChurnSlot>::Set(active);
}

AllocChurnScope::~AllocChurnScope() {
    HostThreadSlot<bool, AllocChurnSlot>::Set(previous);
}

bool PartialSnapshotActive() noexcept {
    return HostThreadSlot<bool, PartialSnapshotSlot>::Get();
}

PartialSnapshotScope::PartialSnapshotScope(bool active) noexcept : previous(HostThreadSlot<bool, PartialSnapshotSlot>::Get()) {
    HostThreadSlot<bool, PartialSnapshotSlot>::Set(active);
}

PartialSnapshotScope::~PartialSnapshotScope() {
    HostThreadSlot<bool, PartialSnapshotSlot>::Set(previous);
}

void Switches::CountUpperBoundHit(Feature feature) {
    const auto previous = upperBoundHits.at(static_cast<std::size_t>(feature)).fetch_add(1, std::memory_order_relaxed);
    if (previous != 0) return;
    for (const auto& [entry, name] : UpperBoundCounters) {
        if (entry == feature) std::fprintf(stderr, "[archb] %s=1\n", name);
    }
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
