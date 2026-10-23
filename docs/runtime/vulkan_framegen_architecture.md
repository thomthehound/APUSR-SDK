<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Vulkan Frame Generation architecture

This document is the authoritative architecture contract for APUSR Vulkan Frame Generation. The implementation is defined by the ownership, lifetime, synchronization, and presentation rules below.

## Design objective

APUSR treats the application's source frame as mandatory and every generated frame as opportunistic. Heavy optical-flow/frame-interpolation work must never turn the game render queue, virtual acquire path, or physical WSI path into a FIFO behind obsolete generated work.

The final-form hot path is built around these invariants:

1. the application-visible virtual backbuffer is the retained source frame; there is no second full-resolution source snapshot;
2. the application's binary render-finished signal is consumed once and fanned out to independent APUSR consumers;
3. transient game-owned inputs are consumed in an early Phase A on an APUSR-owned queue and are released at an explicit input-completion boundary;
4. heavy Phase B prefers a genuine non-graphics compute queue;
5. mandatory source replay runs independently, preferably on a transfer-only queue;
6. generated output goes directly into a privately acquired real WSI STORAGE image whenever the swapchain supports it;
7. WSI acquisition for optional generation is always zero-timeout and occurs only after the mandatory source target is secured;
8. a generated frame that exceeds its real-time **execution** budget after the application's producer-ready dependency completes is dropped; time spent waiting for the game to finish the source frame is not charged to Frame Generation;
9. resource reuse is governed by the timelines of the real consumers, not by synthetic release submissions or CPU waits.

## Logical and physical swapchains

While Vulkan Frame Generation is active, the application's logical swapchain is an APUSR-owned pool of ordinary images. `vkGetSwapchainImagesKHR` exposes those virtual images to the game while the loader's real WSI swapchain and its images remain private to APUSR.

When the game presents virtual image `N`, that image becomes an immutable retained source. APUSR does not copy it into a second source texture. The same image can be read concurrently by:

- source replay, which copies the source into a private real WSI image; and
- Phase B, which samples the source as the presented current backbuffer.

Virtual images therefore carry at least `TRANSFER_SRC` and `SAMPLED` usage. Their sharing contract includes every APUSR family that actually reads them: the graphics family, the generation family when different, and the transfer-replay family when different. This removes per-frame ownership-transfer ping-pong.

The real WSI swapchain remains private. For Frame Generation APUSR requests the real-image capabilities it can exploit without changing the application's public image contract:

- `TRANSFER_DST` for mandatory source replay and generated fallback replay;
- `TRANSFER_SRC` where supported;
- `STORAGE` when supported, enabling direct generated output.

These physical-WSI mutations are applied only when Frame Generation is **active**. Merely advertising/requesting FG capability does not alter the game's ordinary startup swapchain: APUSR preserves its minimum image count, usage, sharing mode, and timing flags until the application recreates the swapchain for active FG. This keeps normal WSI behavior untouched while still negotiating the device-level queues/features that cannot be added later.

When Frame Generation is active and the surface permits it, APUSR raises the physical WSI minimum to at least three images. A three-image physical swapchain gives the low-latency direct path room for a presentation-owned image, the mandatory source target, and an optional generated target without blindly allocating extra full-resolution images on an APU.

## Queue topology

APUSR discovers and reserves queue roles at logical-device creation rather than borrowing arbitrary application queues later.

Preferred topology:

- **application graphics queue:** owned by the game; APUSR does not submit Frame Generation work to it;
- **input queue:** private queue in the application's graphics family for Phase A, so transient game resources can be consumed without cross-family ownership assumptions;
- **generation queue:** private non-graphics compute queue for Phase B when the physical device exposes spare capacity;
- **replay queue:** private transfer-only queue when available, otherwise the private presentation queue;
- **presentation queue:** private present-capable graphics queue;
- **virtual-acquire signaling queue:** separate private graphics queue used only to signal application-visible virtual acquire primitives after a slot is already known reusable.

If no spare non-graphics compute family exists, Phase A and Phase B share APUSR's private generation graphics queue. They are then submitted in queue order with no artificial A-to-B synchronization round trip. APUSR still does not inject Phase A onto the game's queue.

If a non-graphics generation family is used, Phase A remains in the graphics family because game-owned depth, motion vectors, HUD-less color, and distortion resources cannot be assumed to have been created for another family. Only provider-owned resources cross the Phase-A/Phase-B boundary.

Several logical queues in one universal family are not treated as independent hardware engines. Real overlap comes from the non-graphics compute and transfer families where the device exposes them.

## Producer-ready fan-out

The application's present normally arrives with one or more binary render-finished semaphores. Those semaphores may only be consumed once.

APUSR consumes them in one producer fan-out submission and signals a `producerReady` timeline. Independent APUSR work can then wait on the same timeline value:

- mandatory source replay; and
- Phase A.

This is what allows source replay to overlap Phase A without consuming the game's binary semaphore twice.

On the same input/generation queue, producer fan-out, Phase A, and Phase B are three ordered `VkSubmitInfo`s issued in one `vkQueueSubmit` host call. On the true async-compute topology, fan-out + Phase A are one input-queue call and Phase B is one generation-queue call.

## Phase A: consume transient client inputs once

Phase A is the last GPU user of transient Streamline/game-owned resources. It consumes depth and motion vectors, retains any HUD-less interpolation source, retains distortion when present, and produces the FI history/intermediate state required by Phase B.

The HUD-less path has one logically required full-resolution retention write because Phase B needs both current and previous interpolation colors after the game is allowed to reuse its tagged image. Phase A writes the transient HUD-less image directly into provider-owned ping-pong history; no independent full-resolution staging copy is permitted.

Likewise, the source backbuffer itself is never staged: the virtual source image remains retained until its consumers finish.

At the end of Phase A, APUSR signals the Streamline input-completion timeline. From that point onward, Phase B does not touch game-owned depth, motion-vector, HUD-less, or distortion resources.

When Phase A and Phase B use different families, the FI/OF intermediate images and buffers that cross this boundary are created with concurrent sharing over exactly those APUSR families. There is no hot-path queue-family ownership ping-pong.

## Phase B: serial history, asynchronous execution

FSR-derived FI/OF history is inherently ordered, but ordering does **not** require dropping the next source while the preceding Phase B is still finishing. On a split input/generation topology, source replay may proceed from `producerReady` immediately while Phase A for the next source waits on the preceding generation-completion value before mutating the two-frame FI history. Phase B submissions remain serialized by the APUSR generation queue itself. Per-source command-slot and output-ring lifetime checks remain nonblocking hard guards; genuinely saturated work is allowed to fail open to source without turning ordinary overlap into a mandatory warm-up cascade.

Phase B waits only on dependencies that are actually required:

- Phase-A completion when Phase A and Phase B are on different queues;
- a direct WSI target's acquire semaphore when using direct output; and
- fallback-output reuse only when the selected fallback slot still has a consumer.

On the common same-queue path, Phase B follows Phase A by queue order and no A-to-B timeline wait is emitted.

## Direct generated output to WSI

The preferred generated path contains **no generated full-resolution replay copy**.

After the mandatory source WSI target has been secured, APUSR performs a zero-timeout acquire for a second private real WSI image. If the physical swapchain supports STORAGE and swapchain-maintenance1, Phase B transitions that acquired image from discarded contents into `GENERAL`, writes FI output directly into it, and transitions it to `PRESENT_SRC_KHR` before signaling the generation-completion timeline.

Conceptually:

```text
application renders retained virtual source
                 |
        render-finished binary
                 |
          producerReady timeline
           /               \
          /                 \
 source replay              Phase A
 transfer queue      transient input retention
      |                       |
 real WSI source       input-completion timeline
                              |
                       async Phase B
                              |
                  acquired real WSI STORAGE target
                              |
                     FI writes output directly
                              |
                    PRESENT_SRC + generation timeline
```

Direct target acquisition is deliberately **opportunistic**. APUSR never waits for a generated target on the application thread and never takes it before source replay has secured the mandatory source target.

### Why swapchain-maintenance1 is part of the direct path

A generated frame can become useless after its WSI image has already been acquired. Presenting it late would damage cadence; waiting for it would damage source latency.

With swapchain-maintenance1, APUSR can keep a missed direct target private until the generation timeline proves FI has stopped touching it, then release the image back to the swapchain without presenting it. This gives the direct path a clean fail-open lifetime model.

The device extension is negotiated only with its matching instance dependency: `VK_KHR_swapchain_maintenance1` requires `VK_KHR_surface_maintenance1`, while the EXT variant requires `VK_EXT_surface_maintenance1`. APUSR enables the supported instance-side variants before physical-device selection and later selects only a legal matching device-side pair.

The same release primitive makes application-thread source prearm safe: if an error occurs after a zero-timeout source acquire, APUSR can consume the acquire semaphore and return the image without displaying undefined contents.

## Binary present bridge

`vkQueuePresentKHR` consumes binary semaphores, while generation completion is tracked by a timeline semaphore. APUSR does **not** signal a reusable presentation binary in the heavy generation submission: if the generated frame is later dropped, that binary would remain signaled and would need artificial retirement.

Instead, the presentation worker first verifies that generation met its deadline. Only then does it submit a zero-command timeline-to-binary bridge and present the direct image. The timeline is already complete at that point, so this bridge cannot queue a slow FI/OF dependency in front of the mandatory source present.

## Generated-output fallback

Direct WSI is the preferred path, but WSI availability and format/storage support are not universal. APUSR therefore retains an independent two-slot provider output ring as the compatibility/resilience path.

If a direct target is unavailable immediately, Phase B may write to one of these provider-owned outputs. Once generation is ready, the presentation worker attempts a zero-timeout real-WSI acquire and copies the generated output on the replay queue. If no physical image is immediately available, the generated frame is dropped rather than waiting.

The two-slot ring is independent of the virtual source ring. Heavy FI/OF remains serial, but output `N` may still be awaiting its presentation decision/replay while generation `N+1` advances into the other slot. The ring is deliberately preallocated so the first direct-WSI miss does not create a resource-allocation stutter.

## Mandatory source replay

Source presentation is independent from generation. The source replay path copies the retained virtual source directly into a real WSI image; there is no source snapshot/staging image.

When swapchain-maintenance1 is available, APUSR first attempts to **prearm** source replay on the application-present thread using a zero-timeout WSI acquire. On generated frames the replay submission waits the future `producerReady` timeline value, not the game's binary semaphore, so source copy can begin as soon as the producer fan-out lands and can overlap Phase A. Source-only/recovery frames use the same nonblocking prearm but wait the game's render-finished binaries directly, avoiding an unnecessary trip through the presenter before their mandatory copy can begin.

A prearm miss is not an error. The presentation worker later performs the mandatory acquire/replay. The optional generated path never takes a real WSI image before source prearm succeeds.

Without maintenance1, APUSR skips application-thread prearm because a post-acquire failure could otherwise strand an image. The worker path remains the correctness fallback.

## Virtual-slot lifetime and acquire

A virtual source slot is reusable only after its actual consumers are complete. Host capture records up to two timeline dependencies for each slot:

- source-replay consumption of the retained source; and
- generation consumption of the retained source when that source launched Phase B.

There is no GPU-side release bridge. Virtual acquire queries each unique timeline once, coalesces dependencies, and returns only an already-reusable slot. If no slot is ready, the blocking acquire waits on the earliest values that can make *any* slot reusable and rechecks. A nonblocking acquire returns `VK_NOT_READY`.

Only after the slot is proven reusable does the dedicated acquire-signaling queue signal the application's binary acquire semaphore/fence. No unfinished APUSR work is ever placed in front of the game's acquire primitive.

## Real-time presentation policy

The presentation worker is deliberately fail-open:

- the source frame is mandatory;
- the generated frame is opportunistic;
- an unsatisfied generation timeline is never enqueued as a wait on the physical WSI queue;
- the generation-admission window remains separate from display pacing: once `producerReady` proves Phase A is physically runnable, FI/OF receives almost the full measured **source-production interval** to become ready, with the small direct/replay bridge margin preserved; CPU-side Present interception and upstream game render-finished latency do not consume this budget;
- if generation misses that admission window or cannot obtain an optional real-WSI target, the source is presented and the generated frame is retired/released;
- generation completion decides whether G is usable, but it does not define the long-term display phase.

The application-facing virtual swapchain must also preserve a bounded relationship to
physical WSI. Returning every virtual `vkQueuePresentKHR` immediately can let the game
produce several future source frames while their real source PRESENT updates are still
queued on APUSR's worker. Besides increasing retained-image pressure, that breaks the
normal pacing environment expected by latency controllers such as `VK_AMD_anti_lag`.
Frontends may therefore configure a small `maxApplicationPresentQueueDepth`. When the
bound is reached, the *next application Present* waits on a condition variable until one
physical-source presentation task retires. This is the only steady-state application-thread
back-pressure in the presenter contract. It is not a frame-generation deadline or display
target, does not wait for an optional generated frame specifically, and does not move any
G/S midpoint timer onto the game thread. The readiness and presentation workers continue
to overlap normally inside that bounded lead.

For the Streamline Vulkan path the current bound is two outstanding source tasks. The
ownership-derived hard task capacity remains larger and exists for correctness/resource
accounting; it is not the desired steady-state application lead.

Generation readiness uses two timeline boundaries. The readiness worker first waits
for `producerReady`, the fan-out value that represents the application's actual
render-finished dependency. Only then is the source-period generation budget armed.
The worker waits the final generation **timeline semaphore** inside that execution
window. This prevents a frame from "missing" an FG deadline while Phase A was still
physically blocked behind the game, while preserving the same bounded fail-open rule
once APUSR work can run. A generation timeout means "drop generated and continue
with source," not "enqueue the unfinished timeline on WSI."

### Pair-local AMD-style pacing

The current presenter deliberately avoids a source-anchored global phase lattice. The pacing
state is only the most recent physical **source** presentation plus one conservative
half-frame interval `delta`. A healthy 2x pair is scheduled as:

```text
previous physical source -- delta --> generated -- delta --> current source
```

Both halves use the same `delta`, matching AMD's current FrameInterpolationSwapchain
presenter. The next pair starts from the source that actually ended the previous
pair; there is no absolute midpoint phase to recover, catch up to, or drag
forward after a late interpolation.

`delta` is derived from a ten-observation moving window over source-production
cadence. AMD's DX12 swapchain samples its ten-entry window after interpolation-fence
completion; APUSR deliberately uses the directly observed application source cadence
instead, keeping FI/OF completion as an admission signal rather than allowing its
jitter to redefine real-frame cadence. After warm-up the estimator uses AMD's same
conservative policy: half the mean, minus `0.1 * standard deviation`, minus a
0.1 ms safety margin. A source-cadence discontinuity at 100 ms resets the short
history while retaining the last useful display delta through the new warm-up.
Before ten valid samples exist, APUSR uses half of the current valid source interval
rather than deliberately running nine unpaced startup pairs.

A generated frame that becomes ready **before** its pair-local target waits only
until that target. A generated frame that becomes ready **after** the target is
not granted a new phase or a catch-up correction: if it still satisfies the current
generation-admission contract, it is presented immediately and the mandatory source
follows one `delta` later. If it fails admission, G is discarded and the source is
presented immediately. This keeps AMD's pair-local timing model and NVIDIA's
source-always-wins asymmetry without reintroducing a global deadline lattice.

Generation ownership is settled immediately after G is queued or dropped, before
the worker waits the second half interval for the source. Provider output is
therefore not held idle through presentation pacing; replay completion timelines
still protect any copy that actually references the generated slot.

### Presentation-engine timing versus host timing

The preferred pacing path is presentation-engine scheduling through
`VK_EXT_present_timing` with `presentAtRelativeTime`, **when that capability was
already legitimately enabled by the host**. APUSR applies the same relative duration
to both halves of the pair:

- an early generated frame can be queued immediately while WSI keeps the preceding
  real source visible for `delta`; and
- once G is queued, the mandatory source can also be queued immediately while WSI
  keeps G visible for the same `delta`.

Relative timing is a minimum **previous-image visible duration**, not a sleep
measured from the `vkQueuePresentKHR` call. APUSR therefore uses it for G whenever a
previous physical source exists, even when a host-clock estimate says the ideal target
has already passed. If the minimum duration has already elapsed, WSI can proceed as
soon as G is ready; if it has not, WSI preserves the remaining source->G spacing on
its actual display timeline. GPU timelines answer *whether the image is ready*; WSI
timing answers *when a ready image should become visible*. This is the same
distinction NVIDIA documents for DLSS-G when it warns that Present-call cadence is
not equivalent to display-change cadence.

Present timing remains optional and APUSR does not inject missing instance-side WSI
dependencies to obtain it. Some Vulkan implementations do not expose usable swapchain
time-domain IDs until presentation has begun, so capability discovery is retried
lazily instead of being permanently downgraded by an early empty query.

When presentation-engine scheduling is unavailable, the same pair-local targets are
implemented by the dedicated high-priority presenter thread. On Windows, a
high-resolution waitable timer handles the coarse wait and only the final ~50
microseconds use an active yield tail. This is a real compatibility path for hosts
without present-timing support; it does not move pacing back onto the application's
render/present thread.

All APUSR-owned real-WSI host operations are externally synchronized per swapchain.
This is required because source prearm can acquire a real image on the application
thread while the presentation worker acquires, releases, presents, or performs a
timing-domain query on another thread. The lock covers only Vulkan host calls; it
does not serialize GPU execution or defeat graphics/transfer/compute overlap.

Presenter admission is bounded by ownership rather than by an arbitrary steady-state
queue limit. At runtime creation APUSR preallocates enough task storage for `virtual
source image count + private real WSI image count`. Every pending task necessarily
owns at least one of those finite resources: either its virtual source has not yet
been replay-consumed, or source prearm has already privately acquired a real WSI
image. Saturation is therefore treated as a broken lifetime/admission invariant, not
as load to be "fixed" by draining presentation work on the application's present
thread.

### FG-off recovery debt

Disabling interpolation is **not yet equivalent to restoring a native Vulkan swapchain** in hosts that keep an APUSR-virtualized swapchain alive. Their source-only path still pays the proxy replay copy and synchronization, so a general drop-in-safe lifecycle must eventually cause or coordinate native swapchain recreation before zero-overhead post-FG recovery can be claimed.

Some measured hosts do recreate a new non-virtual native swapchain after FG teardown and can still retain a smaller post-FG throughput loss even after FFX allocations are fully released. Treat that as a separate persistent-state/host issue rather than evidence that the proxy swapchain itself survived.

A particularly important negative result is already established: APUSR must **not issue a native AMD Anti-Lag OFF transition on an existing logical device merely because logical Reflex/FG was disabled**. Both standalone mode-only OFF and formally paired OFF INPUT/PRESENT variants reproduced severe post-FG WSI blocking on a tested host. Logical Reflex Off therefore updates APUSR bookkeeping only until there is a genuinely different, proven driver/device-lifecycle mechanism. Do not retry cosmetically different OFF sequencing under the assumption that teardown ordering alone was the problem.

## Synchronization policy

Synchronization is scoped to the real consumer wherever practical:

- source/generated replay waits at `TRANSFER`;
- generation dependencies wait at `COMPUTE`;
- retained source images remain in `GENERAL` for their shared read-only lifetime;
- direct WSI output transitions from discarded/`UNDEFINED` contents to `GENERAL`, then to `PRESENT_SRC_KHR`;
- final present transitions use bottom-of-pipe presentation scope rather than broad `ALL_COMMANDS` barriers.

Broad synchronization and queue-idle waits are reserved for cold teardown/recovery paths where correctness is more important than hot-path latency.

## Performance model

The ideal direct path intentionally removes the large structural traffic that can be removed without changing FI itself:

- **0** full-resolution source snapshot copies;
- **0** independent HUD-less staging copies before history retention;
- **0** generated-output-to-WSI copies on the direct path;
- source replay is the one unavoidable physical copy required by the virtual-swapchain proxy;
- HUD-less history performs one required retention write when a transient HUD-less image is supplied;
- fallback generated replay occurs only when direct WSI is unavailable.

The remaining heavy work is the FI/OF algorithm plus the information that must genuinely be retained across the client-input lifetime boundary. Kernel fusion, barrier tightening, DOT/WMMA reformulation, compact history representations, and other shader-level tuning must preserve this architecture and be justified by hardware measurements.

## Compatibility ladder

The intended capability ladder is:

1. **preferred:** retained virtual source + private source replay + true async compute + direct generated WSI STORAGE + maintenance1 release;
2. **fallback:** same retained/async architecture, provider-owned double-buffered generated output + zero-timeout transfer replay;
3. **degraded source-only:** when generation, output capacity, or a real-time deadline is unavailable, present the source and skip heavy generation rather than stalling;
4. **unsupported contract:** if the Vulkan/Streamline contract cannot safely virtualize the swapchain or provide required input lifetime semantics, do not fake correctness by borrowing queues or forwarding virtual image indices to real WSI.

That last rule is intentional. APUSR should broaden compatibility by implementing the missing contract correctly, not by inserting a slow or semantically ambiguous compatibility path into the performance-critical implementation.
