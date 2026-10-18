# GPU lanes (G8)

## First slice

`APS5_GPU_LANES=1` replaces the queue-thread startup with one cooperative
scheduler, four preparation workers, and one ordered execution worker. The
existing PM4 copy/validation path still snapshots and flattens indirect buffers.
Each guest queue has a cursor and a FIFO of submissions. Only its head cursor
can advance, and only after its preceding chunk has completed recording.

The preparation workers currently copy immutable packet/register-list slices.
The ordered execution worker runs the existing draw/dispatch implementation,
including its GPU mutex, resource validation, barriers, recorder, and optional
`APS5_SUBMIT_WORKER`. This preserves the current resource-resolution ordering.
It does not yet provide parallel resource preparation or parallel Vulkan
recording. It deliberately submits more frequently at chunk boundaries and is
not a performance configuration yet.

The bounded draw committer is disabled in this mode (`DrawPipeline::Depth()` is
zero). Its legacy callbacks are executed inline by the ordered worker through
the existing packet path. This prevents a second asynchronous hand-off from
outliving a chunk or moving thread-local labels between queues. Unset, `0`, and
nonexact values retain the original queue threads and draw hand-off. Both new
switches accept exactly `1` and are cached on first use.

No recompiler, output-version, or resident-heap-mirror policy changes are made.
The implementation is original code; it does not copy Kyty source.

## Stage ownership

| Stage | Owns | May share |
| --- | --- | --- |
| Submission acceptance | Immutable copied PM4, shader registry snapshot, flip reservations, original serial, conditional targets and indirect register lists | Driver submission mutex for accepting and retiring serials |
| Cooperative parser | Per-queue cursor, conditional/predicated control decisions, wait descriptor, queue FIFO, next chunk boundary | Read-only packet snapshots; guest labels are checked before advancing |
| Preparation lane | A chunk's immutable state, validated epoch proofs, pinned resource versions, descriptor/build scratch and its command pool | Concurrent cache lookups with independent lifetime pins |
| Recording lane | Its command buffers, local resource-state transitions and descriptor allocations | Immutable pipeline objects; no shared mutable recorder batch |
| Ordered execution/submitter | Runnable chunk order, dependency edges, queue submission, global resource-state publication | Queue access through `Graphics::OrderedQueueCall` |
| Completion | Fence/timeline retirement, publication of guest-visible stores and reuse of command pools/resources | Existing recorder completion and write-back machinery |

The compatibility implementation keeps preparation resource-free. Queue state,
deferred labels, cache validation, and recorder mutations stay on the ordered
execution worker, with a fresh collect epoch at each execution/control chunk.
Four workers therefore do not imply four independent legacy
recorders. `CommandPools` supplies a lazily created pool per worker and Vulkan
queue family for callers that can already record independently. A pool rejects
use from another worker thread. The caller must prove GPU completion before
resetting/reusing its command buffers, and must retain the owning Vulkan device
through recording and submission.

## Chunks and control flow

The target boundaries are the end of a render pass, 128 draws, or a PM4 sync
point, whichever comes first. Boundaries never split a packet, a rendering
scope, or a conditional decision from the state that decides it.

This slice cuts after 128 draws, flips, rendering waits, `PFP_SYNC_ME`,
`EVENT_WRITE`, `RELEASE_MEM`, and `ACQUIRE_MEM`, and before memory waits and
conditional/indirect control decisions. Register updates and their consumers
remain in order. Existing draws complete their own rendering scopes, so there
is no open Vulkan render pass across slices. The render-pass-aware cut should
replace this conservative policy when the G6 recorder owns complete passes.

Conditional targets and indirect register-list offsets remain relative to the
original copied submission; slices remap only the packet-associated lists and
output reservations. `COND_EXEC`, predicated indirect buffers, and predicated
waits are resolved on the ordered worker after prior work. A false branch
skips its waits. REWIND retains its guest-tail pointer and suspends until the
release bit is set, then copies/validates/reserves the next tail before parsing
it. Only the original submission serial is retired; tail label-write accounting
is accumulated until that retirement.

`WAIT_REG_MEM` and `WAIT_REG_MEM_64` leave the cursor on the wait while the
comparison is false. The scheduler checks other queues and retries after a
200 microsecond condition-variable timeout. It never places an unsatisfied
wait in a worker job. The existing held-at-submit proof remains accepted.
Otherwise this slice requires the value in guest memory; it does not speculate
from a pending GPU label. Recorder completion reaping continues while queues
are suspended. Idle storage results retain a 50 ms quiet-period publication
path. Shutdown cancels suspended cursors and joins the scheduler before device
teardown. Rendering waits, resource-read synchronization and flip-room waits
retain their existing blocking implementation; they are not PM4 memory waits.

## Epoch-based resolution

An epoch is evidence about a specific resource range and mapping, not a snapshot
of its bytes. The final parallel resolver must capture the G3 range token at
parse time after all earlier logical writes to that range have been declared.
Its proof carries the exact range and each region's mapping, CPU and GPU epoch,
plus writer-chunk identity and pinned cache-object generation. Shader/SRT,
indirect argument, texture and render-target ranges all need their own tokens.

Before publishing prepared resources, validate that each token remains covered
and `DirtySince(range, token)` is false. Uncovered mappings, changed tokens,
open CPU writers, pending GPU producers and alias changes force serial
resolution or re-preparation after the relevant producer. Do not replace a
failed token with a newer epoch and continue using bytes prepared earlier.
GPU-produced inputs must resolve against the producer's resource version and
completion edge, rather than stale guest-memory contents.

The observation-only tracker in this base cannot yet prove all of those cases.
This slice therefore captures no resource proofs in its copy-only preparation
stage and makes no epoch-based reuse decisions. Actual resource resolution is
still performed by the existing code in execution order. G4b/G5/G7 must expose
validated range/version snapshots before resource resolution moves to lanes.

## Submission and dependency rules

There is no arbitrary global PM4 order between independent guest queues.
Within a queue, packet and chunk order is strict. The scheduler assigns a
global runnable-chunk ordinal only after a queue's prior chunk has completed
and its wait/control conditions permit progress. Preparation may finish out of
order; the ordered worker consumes the original runnable order. A suspended
consumer has no submit ticket, allowing a later accepted producer on another
queue to run. Submissions queued behind that consumer on the same guest queue
cannot bypass it. A preparation/commit error poisons later commits and is
reported through the existing driver failure path.

Required dependency edges are:

* A label release follows the work it describes. Its consumer waits for the
  corresponding timeline value, or for actual guest-memory publication when
  CPU visibility is required. Parse-time publication is allowed only for labels
  whose existing semantics prove that no unfinished GPU result is exposed.
* A render target later sampled carries a producer-write to shader-read edge,
  the correct image layout transition, and a pinned image version. An ordered
  CPU submit alone is insufficient to establish Vulkan memory visibility.
* Indirect arguments written earlier carry a shader/transfer-write to indirect
  command-read edge. If the CPU must inspect those arguments, publication must
  complete before preparation reads them.
* Cross-family images and exclusive buffers need release/acquire ownership
  transfers. Alternatively, a resource intentionally created for concurrent
  sharing must list both families. A semaphore does not replace ownership
  transfer or layout tracking.

This slice retains the existing recorder's barriers and publication logic on
its existing graphics/compute-capable VkQueue. It does not remove per-dispatch
barriers or invent unproven resource hazard edges.

## Vulkan compute queues

With both `APS5_GPU_LANES=1` and `APS5_ASYNC_COMPUTE_QUEUES=1`, device creation
requests queues from the first nonempty compute-only family, if available.
Guest queues `0x20..0x57` map to distinct physical compute queues while capacity
allows and share them by `(guest - 0x20) % requestedQueueCount` otherwise. Queue
zero remains on graphics. Without a compute-only family, every guest maps to
the original graphics queue. No extra family is requested when either switch
is off.

`FindQueues(device)` exposes the mapping and family-specific worker pools
whenever lanes are enabled, including graphics-only mode with async compute
off. `Submit` accepts ordered primary command buffers and
explicit producer `Dependency` values. Each physical queue owns a timeline
semaphore; repeated waits on one semaphore merge to its highest value. All
wait stages currently use `ALL_COMMANDS` conservatively. Queue submissions use
the existing ordered Vulkan queue-call mechanism. The API refuses semaphore
submission when timeline support is unavailable; existing recording continues
on the original queue. Device teardown waits idle before destroying lane pools
and semaphores. API clients must release their queue handles before destroying
the owning device.

These are real VkQueues and the semaphore API is exercised by the Vulkan test.
Eligible imported-buffer dispatches now use those queues as described below.
Other dispatches continue through the graphics recorder.

## H2 imported-buffer routing

With both switches exactly `1`, a live async-queue dispatch can record its bind,
push constants and direct or indirect dispatch in a separate compute-family
command pool. Admission requires a reusable resource set containing only
nonempty guest storage-buffer descriptors, with every descriptor's entire
range present in `InPlaceReads()` and backed by a host import. This excludes
copied/staged buffers, device-local buffer-cache bindings, image mirrors, image
and sampler bindings, shader-data/SRT buffers, BDA, completion work, device-memory
LDS and checkpoint recording. Oversized direct dispatches retain their existing
split/error path. Resident heap mirrors always retain their existing path.
Missing dedicated compute families or timeline support also retain graphics.

Each admitted dispatch transfers the complete, deduplicated imported VkBuffers
from graphics to its compute family and back using matched release/acquire
barriers. Indirect arguments include their own import in those transfers.
The graphics recorder submits the release, its graphics timeline marker feeds
the compute submit, and an empty graphics submit waits for the compute timeline
before the recorder's acquire. The next graphics batch retains the compute
command pool, fence, pipeline and resources through completion. Pool destruction
waits for its compute fence, already signaled on normal retirement, including
when a handoff fails after compute submission. Guest writer declarations,
pending reads/writes, host visibility and resource retirement continue through
the existing recorder. Optional submit-worker calls retain their FIFO ordering.

This intentionally serializes the graphics/compute handoff. It establishes
live queue routing and ownership for this admitted subset; it does not establish
GPU overlap or four-worker recording. A pool is allocated per routed dispatch
until per-worker recorder retirement can safely recycle it.

GPU-recorded PM4 labels are submitted and associated with a graphics timeline
marker, storing their dword values in a bounded 4096-entry table. A satisfied
PM4 memory wait carries matching label dependencies to the next routed dispatch
on its guest queue; repeated semaphore dependencies merge to their highest
value. Changed label values do not contribute an edge. Clearing the bounded
table only loses redundant edges because waits still require actual guest
memory publication and the graphics handoff orders every admitted dispatch.
CPU/completion-action labels retain that existing publication requirement.
This does not yet suspend on an uncompleted timeline producer instead of guest
memory, or permit unrelated queues to overlap.

`agc_gpu_lanes_gpu_lane_queues_{off,on}` now also execute an atomic counter
through graphics, guest compute queues 0x20 and 0x21, and graphics again. They
assert two routed dispatches when host imports, dedicated compute and timelines
are available, verify that a subsequent GPU label contributes a timeline edge,
and verify the counter on fallback devices. The existing
`agc_gpu_lanes_queues_graphics` and new `agc_gpu_lanes_queues_submit_worker` cover
async-off and submit-worker configurations. CPU mock tests additionally check
both directions of ownership barriers, nested queue scopes, 64-bit label edge
merging and overwritten-label rejection.

`agc_gpu_lanes_gpu_lane_render_sample_{off,on}` submit actual PM4 containing
128 draws to an R32 float color target followed by a compute sample, then repeat
with a different color without a CPU wait between the two sequences. Every
sampled lane must see its producer's value. The fixture checks the first forced
chunk boundary explicitly. `agc_gpu_lanes_render_sample_submit_worker` adds the
submit-worker configuration. Image-bearing dispatches exercise the graphics
fallback; this regression does not claim cross-family image coverage.

## H2 remaining implementation plan

1. In `SubmissionCursor` and the lane preparation stage, decode immutable
   operation/register snapshots in PM4 order and declare earlier logical GPU
   writes before capturing each operation's shader/SRT, indirect, buffer,
   texture and target ranges with `RecordProof::Capture`. Carry writer-chunk
   identity and pinned cache generations beside those proofs. Validate with
   `RecordProof::Check` before publishing a prepared chunk. Changed, uncovered,
   pending-producer and aliased ranges must resolve serially or reprepare after
   the producer; a newer token cannot validate older prepared bytes.
2. Make resource/pipeline/texture lookup publication concurrent before removing
   `GpuMutex`: shared locks or shards around immutable entry lookup, per-key
   future/build ownership on misses, exclusive insertion/eviction, and fence
   pins on all returned versions. Move mutable resource validation and LRU
   touches out of shared-reader sections, preserve the existing descriptor
   allocator locks, and serialize access to the Vulkan pipeline-cache handle.
   Buffer/import and texture generation/layout APIs must expose pinned versions
   before their current mutable state can be used by preparation lanes.
3. Give each worker an independent lane recorder using its family-specific
   `CommandPools` slot, descriptor scratch and resource-state journal. Retire
   pools only after the owning timeline/fence. Replace legacy execution closures
   with prepared command buffers; the existing ordered worker must publish
   resource states and submit in each queue's PM4 order, inserting producer and
   label edges. Allow multiple in-flight chunks of a queue only after dependency
   extraction proves preparation independent. Validate forced preparation
   reordering, failure cancellation and four-worker image equivalence before
   removing the legacy mutex path.
4. Extend routing to shader-data, buffer-cache and image versions through an
   explicit Vulkan resource/ownership snapshot. Add cross-family image
   release/acquire and layout transitions, retain producer image versions, and
   permit pending-label timeline waits only when CPU visibility is unnecessary.
   Replace the conservative graphics handoff with proven resource dependencies
   to obtain actual compute/graphics overlap. Add a dedicated-family image
   regression and indirect GPU-writer chain before claiming full step 4.

Reconfigure CMake to discover `GpuLaneRenderSample.cpp`; add
`GpuLaneRenderSample` and `GpuLaneQueues` to `ANYPS5_AGC_EXECUTION_TESTS` when
filtered. H2 only ran syntax/conventions checks in its isolated worktree. The
integrator must run the registered Vulkan tests with validation enabled and
confirm routing on hardware exposing a dedicated compute family.

## W4g proof handoff slice

`RecordProof::Pin` is the first part of remaining step 1. It accepts the
original content proof of an immutable cache-object generation, its exact
ranges, the owning shared pointer, and a nonzero generation. At the parser
boundary it captures an additional token and validates both proofs. A newer
parse token cannot validate an older object's changed contents. The returned
`PinnedRecordProof` owns both proofs, the ranges and the object; workers check
those tokens without reading fields from the live cache object or guest bytes.
The tracker identity is checked as well as mapping, CPU/GPU epochs and the
mapping-forget serial.

Admission requires all three exact switches `APS5_GPU_LANES=1`,
`APS5_EPOCH_PROOFS=1` and `APS5_PAGE_TRACKER=1`. Empty dependencies, missing
content proofs, missing object pins, generation zero, uncovered/changed ranges
and pending writer-chunk identities are rejected. Private write-watch proofs
are also rejected because their observations cannot cross threads; this slice
does not weaken that tracker contract or turn a metadata epoch into a byte
snapshot.

`Lanes::WithProofs` attaches a pinned dependency list and an ordered serial
fallback to a `Chunk`. The pool validates before preparation, after preparation
and immediately before ordered publication. A failed check discards prepared
work and executes the fallback once in its original submit position. Exceptions
retain the existing cancellation of later publications. The fallback must not
publish the discarded work. The caller must keep the tracker alive and must
transfer object pins into recorder completion ownership before GPU submission.

This is an API and worker-handoff slice. Live `SubmissionCursor` still supplies
legacy resource-free preparation callbacks. It does not yet decode operation
dependencies or attach these pins. No draw/dispatch cache, resource cache,
pipeline cache, texture lookup, recorder or submission format is changed.
Remaining step 1 still needs real shader/SRT, buffer, indirect, texture and
target range extraction, logical GPU-write declarations and producer-version
edges. Steps 2 and 3 remain required before moving those live operations to
workers or removing their `GpuMutex` holds.

With lanes and `APS5_PROFILE_DRAW` enabled, a cumulative `[gpu-lanes]` line adds
`prepared`, `prepared-parallel`, `proof-fallbacks`, `worker-busy-ns` and
`worker-idle-ns`. Parallel preparations are jobs whose preparation intervals
overlap another worker's interval, including both participants. Busy time
covers proof validation and preparation; idle time covers waiting for work.
These counters currently describe the compatibility slice-copy jobs as well.
GPU-mutex acquisition counts and driver-cache contention counts still require
hooks in the future live preparation/cache paths; zero values are not invented
for those unavailable counters.

`agc_gpu_lanes_proof_{off,on,nonexact,unset,epoch_off,tracker_off}` reuse the
record-proof executable. They cover pin lifetime across replacement, original
content proof rejection, mapping identity, pending producers, 512 chunks with
serial-buffer equivalence, four simultaneous workers, invalidation during
preparation and before ordered publication, and four readers racing publication
and retirement of 512 immutable versions. A blocking mutex spy holds the actual
`GpuMutex` on another thread while every admitted chunk completes. This verifies
only the proof handoff and pure fixture preparation, not the existing draw or
dispatch paths. `agc_gpu_lanes_profile_{off,on}` cover profile gating and overlap
counters. The original lane tests additionally cover a failing proof validator
and a missing serial fallback. Existing H2 pixel regressions are unchanged and
must still run in the integrator's build. No new `.cpp` or production CMake
source registration is required; reconfigure for the added test registrations.

The mutex-removal handoff for this slice is unchanged at these live call sites:

| Site | Work retained inside `GpuMutex` |
| --- | --- |
| `Driver::draw`, `Driver::precheckDraw`, `Driver::commitDraw` | Lazy device creation, optional locked preparation, queued-label recording, guest writer declarations and `VulkanDevice::Draw`/`DrawFromRecipe`; the `lockForDraw` closure still covers the final recording path and indirect CPU replay. |
| `Driver::dispatch` | Lazy device creation and the final direct/indirect `Dispatch`/`DispatchRecipe` recording loop, including label and writer publication; its draw-pipeline commit closure retains the same hold when that separate mode is used. |
| `Driver::fillBuffer`, `Driver::copyBuffer` | Their existing HLE buffer recording, synchronization and publication paths. |
| `Driver::execute`, `Driver::flushBetweenPackets`, `Driver::preparePacketMemory`, `Driver::recordQueuedLabelsBeforeRead` | Suspension/chunk-end submits, packet flush boundaries, GPU stores/labels/copies, memory-read synchronization and deferred labels. These remain on the ordered worker. |
| `ShaderResources::Complete`/`buildComplete`, `Revalidate`, `RefreshData`, `MarkGpuWrites` | Binding completion, mutable buffer/image freshness checks, descriptor/data refresh and recorder write declarations reached from the final draw/dispatch holds. `cachedTexture`, `cachedStorageTexture`, `fastTexture` and `StorageImageCached` also retain their own cache locks and mutable image-generation/layout dependencies. |
| `Pipeline::AcquireFramebuffer`, `Recorder::ensureOpen`, `Recorder::Submit` | Shared framebuffer LRU, active batch/command buffer and command-pool state, resource hazards/layouts, fence retention and submission. `Recorder::EndComputeChain`, sample queries/dumps, `KeepThroughHold` and `NoteWrittenBack` explicitly retain the GPU-lock contract as well. |

No worker-mutex claim applies to any of those functions. Only
`PinnedRecordProof::Check`, the `WithProofs` guard and the pure CPU fixture's
preparation are covered by this slice's blocking mutex spy. Before moving a
live draw or dispatch, replace each inherited mutable resource/recorder
dependency above with pinned versions and worker-owned state, then add a real
per-worker mutex-acquisition hook and contention counters to the migrated
cache lookup/publication paths.

## Shared caches and migration

| Shared state | Requirement before parallel resource preparation/recording |
| --- | --- |
| Draw/dispatch records and shader/SRT proofs | Immutable versioned records, epoch proof publication, concurrent lookup and bounded insertion |
| Pipeline/module/render-pass/framebuffer caches | Per-key build ownership, read-mostly/sharded lookup, deferred destruction after all users/fences |
| Buffer cache, imports, staging/stream rings and heap mirrors | Range ownership and writer epochs, concurrent allocation, pinned generations and GPU retirement |
| Texture/image/depth/storage caches and descriptor resources | Alias/version tracking, per-image layouts, concurrent lookup/build, thread-local descriptor allocation and deferred eviction |
| Global LRU/budget accounting and guest mapping queries | Small independent locks or sharded counters; no cache-wide scan while holding the GPU mutex |
| Recorder batches, label tables and completion lists | Per-lane recording state, one ordered publication/submit authority, explicit dependency and lifetime edges |

Migration order:

1. Land and validate G3 coverage/safety, G2b lock diet, and G4b epoch proofs.
2. Land G5/G7 versioned buffer/texture caches, then make the listed cache and
   lifetime operations concurrent. Keep unknown coverage on the serial path.
3. Land G6b recording-worker/deferred-submit integration. Give each lane an
   independent recorder and pass its family-specific pool into it. Preserve
   ordered publication into the existing submit worker.
4. Move resource work into `Chunk::prepare`, carrying parse-time epoch/version
   proofs. Allow multiple in-flight chunks of one queue only after dependency
   extraction proves their preparation independent.
5. Route async dispatch command buffers to `VulkanQueues::Submit`, translating
   PM4 label edges into timeline dependencies and adding queue-family ownership
   transfers. Keep unavailable-family/timeline fallback on graphics.
6. Add an actual render-target-to-sampled-image PM4 regression across a forced
   chunk boundary, validate conditional/REWIND image cases, then measure the G8
   lock, parser-utilization and four-worker image-dump gates. No gate is claimed
   by the current infrastructure tests.

## Validation and integration

`agc_gpu_lanes_{off,on,nonexact,unset}` exercise parallel preparation, ordered
producer/consumer callbacks, failure cancellation, cooperative suspension,
per-worker pool ownership, queue selection/fallback and semaphore wait merging
with mock Vulkan calls. The producer/consumer callback test is a CPU ordering
model, not a rendered-image test.

`agc_gpu_lanes_gpu_lane_pm4_{off,on}` exercise actual driver PM4 with two queues,
a two-way label handshake, a 64-bit wait, a skipped conditional wait, a released
REWIND tail, 128-draw chunk boundaries, and a second submission that must
remain behind its queue's wait. `agc_gpu_lanes_pm4_submit_worker` enables both
lanes and the existing submit worker. `agc_gpu_lanes_shutdown` reuses the real
driver's suspended-queue shutdown regression.

`agc_gpu_lanes_gpu_lane_queues_{off,on}` record empty primaries on worker-owned
pools and run a graphics/compute semaphore chain on a Vulkan device, finishing
at a fence. They skip without Vulkan/timelines and report whether a dedicated
family was available. `agc_gpu_lanes_gpu_indirect_arguments_written_ahead_{off,on}`
reuse the GPU-written indirect-argument correctness regression.
`agc_gpu_lanes_queues_graphics` exercises the semaphore API with the async
switch off and all work mapped to graphics.

The new infrastructure is header-only except for `Submission.inc`, compiled
through the existing `Submission.cpp` hook, and queue-registry lookup in
`VulkanDevice.cpp`. CMake explicitly lists these sources. Reconfigure and build
the new test targets; include `GpuLanePm4`, `GpuLaneQueues`, and
`GpuIndirectArgumentsWrittenAhead` in `ANYPS5_AGC_EXECUTION_TESTS` if that filter
is set. Run the registered ctest variants in the integrator's build. This
worktree does not run CMake, the shared build, ctest, or the game.
