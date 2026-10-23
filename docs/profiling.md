<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# APUSR profiler

APUSR has a backend-neutral flight-recorder profiler for two jobs that have to coexist: reconstructing failures and finding performance bottlenecks. It is designed for the drop-in D3D12/Vulkan architecture rather than as a backend-specific CSV logger, so traces from multiple APUSR DLLs can be merged and compared after a run.

The runtime writer is deliberately bounded. Each loaded APUSR module owns a fixed-size memory-mapped `.aprof` ring and writes fixed 256-byte records with ordinary memory stores plus a commit sequence. Normal profiling does not allocate per event, format text, flush a stream, or run a writer thread. The file mapping reserves the configured ring but pages are touched as records are written; the report states how much ring capacity existed, whether it wrapped, the observed record rate, and the approximate retention time at that rate.

`APUSR_ENABLE_PROFILER=OFF` compiles the profiler implementation out. With the profiler compiled in but `APUSR_PROFILE` unset/off, capture remains disabled and no trace mapping is created.

## Capture modes

`APUSR_PROFILE=1`, `on`, or `sampled` enables sampled capture. This is the normal tuning mode. The default is a three-frame burst once per second. A configuration change restarts a complete burst instead of consuming the tail of an older burst, and an observed graphics-backend transition forces a new burst. If APUSR records an error and the process survives, active samplers also start a fresh burst on their next decision so the trace contains post-failure timing context. Structured policy decisions can request the same next-burst behavior, so a rare drop/fallback/warm-up outcome automatically causes detailed timing on the frames immediately after the anomaly without turning every frame into a full capture.

`APUSR_PROFILE=full` instruments every eligible frame. Use it for short, deliberate captures rather than leaving it enabled indefinitely.

`APUSR_PROFILE=errors` is the low-overhead failure-recorder mode. CPU/GPU performance instrumentation is disabled, structured errors remain enabled, and the default ring is 4 MiB rather than 32 MiB. Long error text is continued across records instead of being silently truncated. Error records can carry both APUSR status and backend-native numeric context (for example a FrameGen result and failed job index). Error records request an asynchronous/rate-limited mapped-file flush so recent failures have a better chance of surviving an abnormal process exit without turning repeated errors into synchronous per-frame file I/O.

The accepted boolean values for profiler switches are `1/0`, `on/off`, `true/false`, and `yes/no` (case-insensitive).

Useful controls:

- `APUSR_PROFILE_DIR`: output directory. Otherwise APUSR uses `<APUSR_LOG_DIR>/profile`, then `C:/NGXLogs/APUSR/profile` on Windows.
- `APUSR_PROFILE_SESSION`: explicit experiment tag used for safe cross-launch comparison.
- `APUSR_PROFILE_INTERVAL_MS`: sampled-mode interval, default 1000 ms.
- `APUSR_PROFILE_BURST`: sampled-mode burst length, default 3 frames.
- `APUSR_PROFILE_RING_MIB`: ring size per loaded APUSR module; default 32 MiB, or 4 MiB in errors-only mode.
- `APUSR_PROFILE_CPU`, `APUSR_PROFILE_GPU`, `APUSR_PROFILE_ERRORS`: independently enable/disable record classes in sampled/full modes.

## Event model

Every event carries a source shard, backend, component, stream, process, thread where applicable, CPU timestamp, frame ID, and correlation/sample identity. Logical flow records can additionally carry an occurrence ID when one semantic host frame can produce repeated operations; this preserves the host frame ID exactly while still distinguishing separate Presents/work items. Streams also capture available native device/queue identity, API/driver versions, vendor/device IDs, queue family, and an optional semantic capability manifest. The manifest declares which record classes a stream knows how to emit; zero means “undeclared/legacy,” not “supports nothing.” This lets offline analysis distinguish *supported but not observed in this capture* from *instrumentation coverage unknown*. GPU frame/pass and calibration records can additionally carry a native-queue override when the work executes on a queue different from the stream's primary queue; this matters for FrameGen, where presentation and interpolation queues can differ. The common record stream currently represents:

- stream/device identity;
- sampled CPU intervals and frame-level CPU totals;
- GPU frame and per-dispatch intervals;
- setup versus actual shader-execution time where the backend exposes the boundary;
- workgroups, barriers, descriptors, and APUSR logical resource read/write bytes where derivable;
- GPU/CPU clock calibration;
- static GPU workload identity recorded at pipeline creation/selection time: native pipeline/PSO identity, code hash and byte size, known shared-memory footprint, thread-group shape, required subgroup size, and backend-specific flags;
- structured counters and timeline instants;
- structured decisions with a backend-neutral outcome (`accepted`, `presented`, `source-only`, `deferred`, `warmup`, `skipped`, `dropped`, `fallback`, `retried`, or `failed`) plus a stable machine-readable reason token and four numeric context fields;
- durable state transitions with previous/next values, subject identity, and epoch so offline analysis can reconstruct observed state residency and catch state that was entered but never left;
- resource lifetime events (`create/destroy`, `allocate/free`, `import/export`, `acquire/release`, `bind/unbind`) that form an APUSR-owned-object ledger independent of OS memory-budget snapshots;
- logical flow begin/step/end/cancel markers for work that crosses modules or worker threads, while native GPU synchronization remains represented by queue events;
- thread identity/role records for APUSR-owned workers, including native thread ID and available priority/affinity context;
- explicit wait intervals that distinguish fence/timeline waits, CPU primitives, queue/device idle, WSI acquire/present waits, pacing timers, worker wakeups, and external waits, including result/timeout/native synchronization identity;
- generic metadata records for durable experiment/runtime facts that do not belong in counters or state transitions;
- queue submit/signal/wait/present/acquire ordering records carrying native queue and synchronization identity;
- configuration changes and backend switches;
- failure status/stage plus long-message continuation.

Queue records describe the CPU enqueue/observation point and native causal objects. Explicit wait records describe the wall-clock interval during which an APUSR thread actually waited. Offline analysis can connect a fence/timeline-semaphore signal to a later queue wait or explicit host wait and draw the dependency in `timeline.json`. The signal-to-wait enqueue gap is useful for explaining ordering, but it is **not** labelled as GPU stall time: measuring the duration for which hardware was actually blocked requires queue/device-specific GPU evidence that the current recorder does not always have.

Use `CpuInterval` for work whose duration matters and `WaitScope`/`WaitInterval` for blocking whose *cause and result* matter. Recording a sleep/fence wait as an ordinary CPU scope destroys exactly the information later performance work needs: whether the thread was computing, pacing, waiting for a producer, timing out, or being cancelled.

`read_bytes`/`write_bytes` are APUSR logical resource-traffic estimates from the execution graph. They are useful for normalizing or comparing workloads, but they are **not** hardware memory-controller traffic and `logical_io_gbps_mean` must not be interpreted as measured DRAM bandwidth.

`GpuWorkloadInfo` is deliberately static metadata rather than another per-dispatch record. A timed `GpuInterval` answers *how long did this named workload take?*; workload metadata answers *which code/pipeline did that name refer to?* This separation makes shader/compiler changes visible without adding hot-path reflection or hashing. Unknown fields stay zero rather than being filled from heuristics.

## Current backend coverage

### D3D12 FSR4

The D3D12 reference upscaler reuses its real timestamp-query path. It records frame and per-dispatch GPU time, setup/dispatch separation, workgroups, descriptor counts, logical resource traffic, and the shared FSR4 configuration signature. The same sampled-frame decision also records CPU time for frame planning, resource realization, pipeline preparation, tuned native-dispatch cache preparation, descriptor preparation, constant preparation, total command recording, and each enabled dispatch's command-recording work. Frame records therefore correlate CPU total/record cost with the GPU sample selected for that frame rather than sampling the two domains independently. APUSR-owned FSR4 resources emit allocation/free lifetime records when the realization changes or the runtime is destroyed. `ID3D12CommandQueue::GetTimestampFrequency` supplies the GPU clock and `GetClockCalibration` supplies a CPU/GPU calibration pair for absolute timeline placement. Unified GPU profiling resolves the captured D3D12 queue just like the legacy timestamp mode; `APUSR_DX12_GPU_TIMESTAMPS` is not required when `APUSR_PROFILE` has GPU profiling enabled. Compute-pipeline creation also records native PSO identity plus the DXIL hash and byte size. Thread-group dimensions remain unavailable unless a trustworthy reflection path provides them; they are not guessed from dispatch geometry.

### Vulkan FSR4

The Vulkan reference upscaler likewise reuses real query pools and obeys the selected queue family's `timestampValidBits`; wrap-around is handled modulo the valid timestamp width. It records frame/pass GPU time, setup versus dispatch time, CPU planning/binding/command-recording time, workgroups, barriers, descriptors, and logical traffic. Pipeline creation records native `VkPipeline`, SPIR-V hash/byte size, and literal `LocalSize` when present in the module. The lightweight parser intentionally does not pretend to resolve `LocalSizeId`; an unresolved shape remains unavailable.

When the host enabled and exposes `VK_KHR_calibrated_timestamps` or its EXT alias, APUSR discovers a compatible host time domain and records calibrated device/host timestamp pairs including the driver's maximum deviation. If calibrated timestamps are unavailable, GPU durations remain correct but absolute GPU placement in `timeline.json` is explicitly marked as resolution-time anchored rather than fabricated.

### Frame generation

Vulkan FrameGen has queue/timestamp machinery at the layer APUSR owns, so sampled per-dispatch GPU intervals and calibrated timestamps are emitted when available. Its compute pipelines also publish the same SPIR-V code identity/thread-shape metadata used by the Vulkan upscaler, including the required subgroup size when APUSR deliberately forces one. Runtime GPU-job recording failures are emitted immediately as structured errors, and an active sampled GPU frame is marked failed so the timing report does not quietly treat a broken frame as a normal sample.

D3D12 FrameGen now has real GPU timing at both the total interpolation-command-list and per-job levels for command lists APUSR records itself. Its compute backend records native PSO plus DXIL hash/byte-size metadata when a pipeline is created. The presentation seam owns the interpolation queue and command-wrapper/fence lifetime, so a sampled wrapper lazily creates its timestamp-query/readback resources, records a queue clock calibration, brackets the total command list, and exposes the active wrapper to the synchronous `executeJobs` callback while that same list is being recorded. `executeJobs` then brackets each recorded FrameGen job with begin/end timestamp queries and copies the small amount of job metadata needed for deferred reporting. The backend-local sampled gate derives configuration identity from each effect context’s realized static-resource shape and tracks change state per logical effect context while retaining one global sampling cadence. The several normal recurring FrameGen contexts/batches are recorded separately as `jobs.workload_hash`; ordinary A/B/B-style context cycling therefore does not masquerade as a fresh configuration change and continuously restart sampled bursts. One `ResolveQueryData` covers the total interval and all job pairs; readback waits only for the wrapper's existing availability fence, so profiling adds no extra synchronization fence or CPU wait. Normal/non-sampled wrappers allocate none of the per-job metadata/query resources. GPU records carry the actual interpolation-queue identity even when the surrounding `framegen.present` stream belongs to another queue. Pre-recorded/registered interpolation command lists still cannot be retroactively bracketed without changing their recording contract, and the profiler leaves those per-job measurements unavailable rather than substituting CPU time.

The D3D12 presentation path additionally records pacing/synchronization intervals such as composition waits, interpolation waits, replacement-buffer waits, UI preparation, real/interpolated submit, configuration application, and total present-path CPU time. The presenter and interpolation worker threads expose their own wait, scheduling, composition, present, and pacing-estimation scopes, along with native queue/fence ordering events. These intervals are important because a frame-generation performance problem can be a queue/pacing problem even when the shader itself is fast.

The Vulkan presentation worker records source/generated replay preparation, worker-queue delay, generation-readiness checks, target/deadline waits, generated/source presents, source cadence, the AMD-style pair-local paced interval, generated/source target lateness, queue depth, and whether a generated presentation opportunity was missed because generation was not ready in time. When a frontend configures bounded application-present lead, `presentation.wait_application_lead` records the condition-variable back-pressure separately from presenter target waits so normal WSI-style throttling cannot be mistaken for FI/OF execution or midpoint pacing. Its internal submits/semaphore dependencies share the same queue-event schema as D3D12.

Both FrameGen backends also emit allocation/free and current-effect-usage counters for APUSR-owned resources. They now additionally emit `framegen.effect_resource` lifetime records keyed by a stable effect/resource identity, allowing the reporter to reconstruct peak live allocation count/bytes and unmatched or outstanding resources instead of inferring lifetime from aggregate counters. Those sizes are real APUSR allocation sizes, but they are not physical DRAM consumption, cache traffic, or memory-controller bandwidth.


D3D12 presentation also emits a generic `presentation.lifecycle` logical flow for sampled source Presents. The flow crosses the game-facing Present call, interpolation submission/completion, pacing publication, and generated/real physical Presents. It uses the game-provided frame ID unchanged and a separate occurrence ID for repeated Presents of the same semantic frame (for example a static menu). This is built on the same backend-neutral flow record used by runtime/upscaler work rather than a FrameGen-only trace format.

Frame-generation eligibility is recorded separately from timing. D3D12 and Vulkan use the same `framegen.source_disposition` decision subject so cross-backend reports can answer whether each source frame was accepted for interpolation or went source-only and why. Vulkan records stable reasons at the actual frontend branch (`pcl_present_marker_missing`, `output_ring_full`, resource-contract failures, session/submit failures, and so on), and successful history-only generations distinguish `explicit_reset`, `history_gap`, and the combined condition rather than making the reporter reverse-engineer them from missing presentation counters. The Vulkan presenter separately records `framegen.generated_present`, so a generation that was scheduled can be distinguished from one later dropped for a generation deadline or failed presentation. Rare non-success decisions trigger the next sampled burst.

The profiler separately samples OS/driver memory-budget state without turning it into a per-frame polling cost. D3D12 uses the adapter's local/non-local video-memory information to record current usage, budget, current reservation, and available reservation. Vulkan uses `VK_EXT_memory_budget` when the physical device exposes it and records each heap plus local/non-local aggregates. These queries run once at the start of a sampled burst rather than on every profiled event. On unified-memory APUs this is particularly useful for correlating pressure/headroom with slow frames, but it remains a budget/residency-pressure signal rather than a measured DRAM-bandwidth counter.

### Application-facing Vulkan WSI

The Vulkan host-capture frontend profiles the application's outer `vkAcquireNextImageKHR`/`vkAcquireNextImage2KHR` and `vkQueuePresentKHR` calls, including the virtualized swapchain route used by Vulkan FrameGen. It records sampled API-call CPU duration, acquire/present cadence, result/image context, present wait-semaphore identity, swapchain contract metadata, and negative `VkResult` failures. The outer present is profiled once; APUSR's nested/private real-WSI replays deliberately do not masquerade as additional application presents.

This gives the timeline a useful boundary between "the game called present" and APUSR's internal frame-generation/presentation work. The profiler now also uses trustworthy display-timing information where the platform exposes it. On D3D12 FrameGen, DXGI frame statistics are correlated with APUSR's bounded PresentCount ring to report presentation-call-begin/return to the corresponding display synchronization QPC observation, refresh counts, and sync GPU time. On Vulkan, APUSR can enable `VK_GOOGLE_display_timing` for profiling and attach sampled present IDs to real physical presents; returned past-presentation timings provide actual display cadence, earliest-present timing, and present margin. Vulkan FrameGen routes sampled generated and source physical presents through the same host-capture timing path, preserving which kind of frame was shown.

The Vulkan display clock is kept in its native presentation-engine monotonic-nanosecond domain because APUSR does not yet have a proven calibration from that clock to its CPU/QPC clock. The reporter therefore trusts cadence/margin and D3D12's QPC-domain synchronization correlation, but it does **not** fabricate a Vulkan CPU-enqueue-to-display duration or call either platform's result literal photon/scanline time.

### NPU/XRT

The XRT backend records sampled host-side model-epoch load/DMA/wait, quiescent drain, frame/band enqueue, persistent-controller startup, ingress/egress synchronization, run acquisition, argument binding/submission, diagnostic waits, payload sizes, worklist sizes, model-parameter bytes, and frame-level host enqueue latency.

This is host/XRT submission telemetry, not yet AI Engine hardware-counter telemetry. AIE/AIE-ML hardware exposes event-driven trace and configurable performance counters, but importing tile/core/memory counter and trace data requires a runtime path whose availability and ABI we can rely on. APUSR therefore does not claim hardware NPU utilization or stall counts until that path is actually wired and validated. The event model keeps room for those records, but profiler work can proceed independently while NPU development is paused.

## Backend comparison

The D3D12 and Vulkan FSR4 paths intentionally use the same logical configuration-hash schema: pipeline variant, preset, frame capacity, permutation, render/display size, and RCAS state. Backend identity is a stream dimension, not part of that hash.

At the runtime layer, `runtime.frame` also uses a backend-neutral render/display configuration signature. A transition such as D3D12 -> Vulkan is emitted as `backend.switch` and as a structured `graphics.backend` state transition, and the new backend gets a fresh sampled burst. Sampled `runtime.execute_frame` work is also represented as a logical flow whose steps are the real execution stages and whose terminal event records success/cancellation. D3D12 and Vulkan FSR4 record `upscaler.configuration_hash` state transitions when their common logical configuration changes. This allows a game that can switch while remaining alive to be compared without pretending that unrelated configurations are equivalent, and makes long-lived state/configuration changes visible without reconstructing them from timing rows.

There are two comparison policies:

- `backend_compare.csv` and `backend_frame_compare.csv` are strict live comparisons. Samples must come from the same process/session and have the same component, stream, configuration, and (for pass comparison) pass name.
- `backend_compare_session.csv` and `backend_frame_compare_session.csv` are cross-launch comparisons. They aggregate only traces carrying the exact same **non-empty** `APUSR_PROFILE_SESSION`, making separate D3D12/Vulkan launches opt-in rather than silently mixing arbitrary runs.

The pass reports include mean/median/p95/p99 timing, setup and execution ratios, and logical IO. The frame reports compare available GPU frame time, CPU total/record time, workgroups, and failed-sample counts. Raw GPU begin/end ticks without a non-zero timestamp frequency are retained in the raw CSVs as forensic evidence but marked `gpu_duration_valid=False` and excluded from timing statistics; the reporter does not reinterpret an unconvertible tick delta as zero milliseconds. A missing real measurement stays missing rather than being substituted with a different clock domain.

## Reporting

Run:

```powershell
python .\tools\profiler\apusr_profile_report.py C:\NGXLogs\APUSR\profile -o .\profile-report --timeline
```

The reporter merges independent `.aprof` shards using source-scoped stream identities. Its main outputs are:

- `report_index.md`: human landing page for capture quality, largest measured GPU hotspots, largest explicit waits, semantic/lifetime entry points, timeline use, and controlled A/B comparison. It ranks observations but deliberately does not declare root cause from correlation.
- `summary.txt`: quick capture-health and bottleneck verdict: frame distributions, hotspots, concrete tail-frame forensics, tail-latency signals, setup-heavy passes, synchronization waits, backend switches, failures, and clock-calibration coverage.
- `trace_health.csv` and `capture_quality.csv`: mode/ring settings, retained/overwritten records, observed record rate/retention window, calibration availability, semantic-manifest health, sample volume, and explicit anomalies. Missing optional hardware capability is informational rather than mislabeled as a failure.
- `streams.csv`, `stream_coverage.csv`, and `capability_coverage.csv`: backend/device/queue identity, retained semantic record counts, declared stream capabilities, and a normalized declared-versus-observed matrix. `observed_undeclared` is a profiler-contract warning; `declared_not_observed` only says that this capture did not exercise/retain that class.
- `frames.csv` and `frame_stats.csv`: raw frame samples plus mean/median/p90/p95/p99/min/max/stddev distributions.
- `gpu_intervals.csv` and `hotspot_stats.csv`: raw per-pass GPU timings and aggregated setup/execution/workgroup/logical-IO statistics. When known, raw GPU rows also expose the actual native execution queue rather than assuming the stream's queue. Hotspot rows join static code-size/thread-shape/subgroup metadata by semantic workload identity.
- `gpu_workloads.csv` and `gpu_workload_stats.csv`: pipeline-creation/selection identity, native PSO/pipeline, code hashes and byte sizes, known shared-memory/subgroup/thread-group shape, and grouped code variants. These tables are intended to make shader/compiler changes auditable even before runtime timing moves.
- `cpu_intervals.csv`, `cpu_interval_stats.csv`, `cpu_exclusive.csv`, `cpu_exclusive_stats.csv`, and `wait_stats.csv`: raw/aggregated CPU intervals, offline-derived exclusive/self time for nested scopes, and a focused synchronization/wait view. Exclusive time is computed by subtracting the union of strictly-contained child scopes, so nested instrumentation does not make parent CPU cost look larger than it really is.
- `cpu_thread_frames.csv`, `cpu_thread_stats.csv`, `cpu_concurrency.csv`, and `cpu_concurrency_stats.csv`: offline union of captured scopes per thread plus per-frame profiled thread overlap/parallelism. Nested scopes are deduplicated before busy time is calculated. These files describe APUSR-instrumented scope occupancy, **not** whole-process CPU utilization.
- `queue_events.csv`, `queue_event_stats.csv`, and `queue_dependencies.csv`: native queue ordering events and reconstructed signal-to-wait causal edges. Reported enqueue gaps are CPU-side spacing, not claimed GPU stall duration.
- `gpu_queue_gaps.csv` and `gpu_queue_gap_stats.csv`: uncovered spans between consecutive calibrated APUSR GPU intervals on the same native queue, with matching wait/signal/cross-queue dependency context when available. These are deliberately labelled *observed profiled-interval gaps* rather than hardware-idle time because uninstrumented game/driver work can exist inside them. This is the strongest generic non-perturbative queue-bubble signal the embedded profiler can derive without injecting marker submissions.
- `wsi_stats.csv`: per-swapchain Vulkan acquire/present call distributions, host cadence, swapchain contract, virtualization state, and sampled API error counts.
- `pacing_stats.csv`: normalized frame-generation presenter/interpolation metrics, including worker/present/wait CPU scopes, Vulkan pair-local paced interval, generated/source target lateness, generation-deadline outcomes/queue depth, and D3D12 QPC pacing deltas/blocking-fence choices.
- `frame_breakdown.csv`: frame GPU time versus measured pass/setup/execution sums.
- `tail_frames.csv`: concrete sampled frames that form each configuration's p95 tail, so an aggregate regression can be traced back to frame IDs and pass mix.
- `stutter_frames.csv`: per-tail-frame forensic join of the heaviest GPU pass, inclusive and exclusive CPU hotspots, named waits, queue-dependency context, display-cadence/synchronization observations, the latest local/non-local memory-budget pressure snapshot, and errors. It is deliberately presented as context/correlation rather than automatic causal attribution.
- `tail_attribution.csv`: GPU passes and CPU scopes whose mean duration grows on those concrete p95-tail frames versus non-tail samples; this is correlation for diagnosis, not proof of causation.
- `calibrations.csv`: CPU/GPU calibration pairs, max deviation, GPU frequency, valid timestamp width, and native queue when known. Timeline calibration is queue-aware so a calibration from one queue is not silently borrowed for another.
- `counters.csv`, `counter_stats.csv`, and `instants.csv`: structured telemetry, counter distributions, and timeline markers.
- `decisions.csv` and `decision_stats.csv`: raw policy/eligibility outcomes plus grouped outcome/reason percentages. These are the primary answer to questions such as “why was this source frame not generated?” rather than an offline guess based on absent timing.
- `frame_outcomes.csv`: compact per-stream/per-frame semantic index joining the ordered decision path, terminal outcome/reason, structured errors, explicit waits, and flow completion. Start here when the question is simply “what happened to frame N?”; the raw tables remain authoritative for detail.
- `state_transitions.csv` and `state_residency.csv`: durable logical/device state changes and the observed time spent in each state until the next transition (the last retained state is explicitly open-ended).
- `state_conditioned_stats.csv`: frame/GPU/CPU/wait timing distributions conditioned on the latest known durable state value. State assignment uses the host observation timestamp; asynchronous GPU work that straddles a transition must still be checked in `timeline.json` before making a boundary-sensitive claim.
- `resource_events.csv`, `resource_lifetimes.csv`, and `resource_balance.csv`: APUSR-owned resource events, individual object lifetimes/reuse problems, and peak/outstanding/unmatched ledger results. If a shard wrapped, a retained close whose open predates the retained window is marked indeterminate rather than reported as a lifetime violation. This is deliberately separate from OS/driver memory-budget telemetry.
- `flows.csv`, `flow_stats.csv`, and `flow_summary.csv`: cross-thread/module logical work lifecycles and grouped duration/completion statistics.
- `threads.csv` and `thread_activity.csv`: APUSR worker identity/role plus observed CPU-scope and explicit-wait occupancy using interval unions so nested scopes are not double-counted. These are capture-window observations, not OS scheduler utilization counters.
- `wait_intervals.csv`, `wait_reason_stats.csv`, and `wait_dependencies.csv`: explicit blocking intervals, grouped wait causes/results, and native signal correlation when the same fence/timeline object/value is visible in queue records.
- `metadata.csv`: generic runtime/experiment metadata emitted by instrumented subsystems.
- `vulkan_pipeline_activity.csv`: joined Vulkan shader-module, compute/graphics-pipeline, and pipeline-cache calls from the host-capture layer. Each cold-path event carries elapsed CPU wall time plus available shader code size/hash, cache/object handle, result, and caller-module attribution. Use it to distinguish APUSR pipeline creation from host/middleware/driver-facing compilation activity before changing shader source.
- `record_counts.csv`: retained record counts by shard. Use it together with the stream/capability coverage tables before treating absence as evidence.
- `anomalies.csv`: automatic triage index for ring overwrite, structured runtime errors, malformed/outstanding resource lifetimes, incomplete flows, unsuccessful explicit waits, and dropped/failed/fallback decisions. It is an index into evidence, not an autonomous root-cause oracle.
- `memory_budget.csv` and `memory_reservation.csv`: sampled OS/driver memory usage/budget/headroom/reservation telemetry, including Vulkan heap identity/device-local classification where available.
- `display_timing.csv`: D3D12 presentation-to-display-sync observations and Vulkan actual-presentation cadence/margin records, preserving generated-versus-source FrameGen identity.
- `errors.csv` and `error_context.csv`: failures (including backend-native numeric context when supplied) plus the nearest sampled frame before/after each error in the same process.
- `backend_switches.csv`: explicit runtime backend transitions.
- `backend_compare.csv`, `backend_compare_session.csv`, `backend_frame_compare.csv`, `backend_frame_compare_session.csv`: strict live and explicitly tagged cross-launch D3D12/Vulkan A/B reports.
- `timeline.json`: Chrome/Perfetto-compatible CPU/GPU timeline. Calibrated GPU timestamps are mapped to the CPU clock; uncalibrated events remain explicitly anchored at resolution time.

The ring is a flight recorder, not an ever-growing log. After it wraps, the oldest records are overwritten. `trace_health.csv` makes that visible and estimates how many seconds the configured ring would retain at the observed record rate. Offline resource analysis treats closes whose opens may have been overwritten as indeterminate retained-window history rather than inventing a lifetime error. Increase `APUSR_PROFILE_RING_MIB` for intentionally long full captures; sampled mode should stay sparse enough that an ordinary capture does not need a larger ring merely to compensate for a sampling bug.

### Comparing experiments

For two already-generated report directories, use the semantic comparator rather than hand-diffing CSVs:

```powershell
python .\tools\profiler\apusr_profile_compare.py `
  .\baseline-report `
  .\candidate-report `
  -o .\comparison `
  --baseline-label baseline `
  --candidate-label candidate
```

The comparator keys measurements by backend/component/stream/configuration/event identity. It does not infer repository checkpoints or application identity from path names. `metric_deltas.csv` contains normalized aggregate deltas for timings, decisions, resources, state-conditioned observations, and static GPU-workload metadata. `distribution_deltas.csv` goes back to retained raw GPU/CPU/frame/wait/flow samples, compares medians, reports a common-language probability that the candidate is slower, and computes deterministic non-parametric bootstrap 95% intervals by default. `gpu_workload_identity_deltas.csv` separately compares stable recorded shader metadata: exact code hashes, thread-group shapes, subgroup sizes, and workload details. A matching code hash proves that APUSR supplied the same recorded shader bytecode in both captures; it does **not** claim visibility into driver-native ISA, VGPR/SGPR allocation, occupancy, or hardware cache counters. `summary.md` surfaces ordinary timing changes, distribution-backed changes, same-state timing changes, shader/pipeline code-size changes, and workload-identity changes separately so static compiler output is not confused with runtime performance.

Use `--bootstrap 0` only when the resampling calculation is intentionally unwanted. The bootstrap interval describes sampling uncertainty in the two retained sample sets; it does **not** control differences in workload, clocks, thermals, power policy, or any other run-to-run variable.

For a trustworthy A/B, keep workload, settings, scene/input, warm-up policy, power state, and capture mode controlled. Prefer multiple runs when ordinary run-to-run variance is large. Statistical output makes the evidence more explicit; it does not rescue an uncontrolled experiment.

## Vulkan SR optimization and Reflex attribution

Vulkan optimization captures now include cold-path interception of `vkCreateShaderModule`, compute/graphics pipeline creation, pipeline destruction, pipeline-cache create/destroy/get-data/merge, and caller-module attribution. This instrumentation is intentionally event-driven: it does not add per-dispatch logging to the model hot path. A continuing shader-compilation indicator should first be checked against `vulkan_pipeline_activity.csv` to answer who is creating what and whether a real `VkPipelineCache` participates. APUSR's own stable `gpu_workloads.csv` identities remain the complementary answer to “did the runtime select/recreate different FSR4 shader code?”

Reflex state no longer shares the Vulkan DLSS-G profiler stream. `streamline.reflex` is backend-correct and records durable mode/load state so ordinary CPU/wait observations can be split by `state_conditioned_stats.csv`. The D3D12 translation also records direct wall-time scopes around Anti-Lag 2 update/state/render-end calls and the relevant `slReflexSleep`/PCL marker paths. Vulkan host capture already records direct `VK_AMD_anti_lag` input/present call wall time. Treat a long driver/API call as elapsed wall time: it may represent intentional pacing/blocking and must not be relabeled as pure CPU computation without stronger evidence.

For Vulkan SR optimization, keep the workload and frame contract stable, collect a warm baseline and candidate, inspect pipeline/cache attribution before interpreting a host shader-compilation indicator, compare per-pass GPU distributions, then use `apusr_profile_compare.py`. Reuse an existing valid baseline instead of repeating hardware runs without a concrete need. Backend-specific source/lowering changes should follow the evidence rules in `performance.md`.

## Legacy timestamp CSVs

`APUSR_DX12_GPU_TIMESTAMPS`, `APUSR_VULKAN_GPU_TIMESTAMPS`, and `APUSR_FRAMEGEN_VULKAN_GPU_TIMESTAMPS` remain usable for existing workflows. The unified profiler reuses those timestamp-query implementations instead of creating a second parallel GPU-timing mechanism. `.aprof` is the preferred format for new tuning work.

## Planned instrumentation

The broad GPU/CPU feature set is now close to the point where further work becomes platform-specific rather than missing core profiler architecture. The remaining measurements are ones APUSR should only add when there is a trustworthy, non-perturbative source:

- exact hardware queue-idle/blocked duration when a backend/driver exposes evidence stronger than the current calibrated profiled-interval-gap plus native dependency analysis. The generic implementation intentionally does not inject extra marker submissions merely to make a wait measurable;
- fuller end-to-end latency correlation from an application/input event through generated/source presentation to scanline/photon. Current D3D12 display-sync and Vulkan actual-presentation timing provide a strong presentation endpoint but not a generic input timestamp or literal photon sensor;
- optional hardware memory-controller/cache/clock/power telemetry through a stable OS/vendor interface that is available on the target machine and whose counter semantics can be reported accurately. The current logical-resource traffic and OS/driver budget data are not substitutes for those counters;
- true XRT/AIE tile/core/memory performance counters and execution trace once NPU work resumes and a dependable runtime interface is validated. This is intentionally deprioritized for now, while the common event model remains able to accept it later.

Those should extend the same stream/correlation model. Until a measurement is real, the profiler should leave the field unavailable rather than infer a hardware fact from an unrelated proxy. With the generic D3D12/Vulkan timing, queue-dependency, memory-pressure, display-cadence, CPU-scope, stutter-forensics, and backend-comparison features in place, the next major phase can reasonably concentrate on polish and usability rather than adding proxies for hardware facts we cannot actually observe.

### Instrumentation must fail open

Profiling is observational and must not become a rendering dependency. If DX12 GPU timestamp query/readback setup fails, APUSR records the failure once, disables GPU timestamp instrumentation for that feature runtime, and continues recording the real upscaler dispatches. Creating a fresh feature runtime resets that diagnostic-disable state and permits another timestamp setup attempt.
