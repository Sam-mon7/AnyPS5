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
Existing dispatches continue through their current recorder. PM4 label waits
do not yet generate `Dependency` objects for that API. Switching live dispatch
onto these queues requires the recorder and resource-ownership work below.

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
