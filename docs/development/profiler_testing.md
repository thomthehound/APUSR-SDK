<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# APUSR profiler validation plan

This plan validates the unified APUSR profiler without tying the procedure to a repository checkpoint or a particular application. The goal is to prove two things independently: profiler-disabled builds preserve runtime behavior and performance, and profiler-enabled captures provide trustworthy, non-perturbative evidence.

## 1. Build contracts

Validate both configurations from the same source tree:

```powershell
cmake -S . -B build-profile-off -G Ninja -DCMAKE_BUILD_TYPE=Release -DAPUSR_ENABLE_PROFILER=OFF
cmake --build build-profile-off
ctest --test-dir build-profile-off --output-on-failure

cmake -S . -B build-profile-on -G Ninja -DCMAKE_BUILD_TYPE=Release -DAPUSR_ENABLE_PROFILER=ON
cmake --build build-profile-on
ctest --test-dir build-profile-on --output-on-failure
```

The profiler-enabled suite must include the structured-record and report-generator contracts. The profiler-disabled suite proves that public instrumentation calls compile to the intended no-op behavior.

Windows-only frontends/backends must also be built with the supported MSVC/Windows SDK environment before a Windows package is treated as qualified.

## 2. Profiler-disabled runtime preservation

Before trusting measurements, run a stable workload with `APUSR_PROFILE` unset. Compare it against a profiler-disabled reference built from the same functional source state.

Check:

- no `.aprof` mapping is created;
- no profiler-owned GPU submission, fence, semaphore, command list, or host wait appears;
- queue topology and presentation behavior are unchanged;
- frame time, source cadence, and pacing remain within normal run-to-run variance;
- enabling profiler support at compile time alone does not create persistent worker threads or background I/O.

This test is more important than any particular FPS number. A profiler that materially changes the path it measures is not trustworthy.

## 3. Sampled capture

Use sampled mode for ordinary tuning:

```powershell
$env:APUSR_PROFILE = "sampled"
$env:APUSR_PROFILE_SESSION = "experiment-a"
$env:APUSR_PROFILE_DIR = "C:\NGXLogs\APUSR\profile"
```

Exercise a representative workload long enough to include stable operation, at least one configuration/state transition, and any feature enable/disable sequence under investigation.

Generate the report:

```powershell
python .\tools\profiler\apusr_profile_report.py `
  C:\NGXLogs\APUSR\profile `
  -o .\profile-report `
  --timeline
```

Open `report_index.md` first, then verify `capture_quality.csv` and `trace_health.csv`. A capture that overwrote the relevant window should not be used for negative conclusions. Calibration or static-workload metadata that a platform cannot expose should remain informational rather than turning into a synthetic failure.

Then verify the expected subsystem in both `stream_coverage.csv` and `capability_coverage.csv`. A declared capability with zero retained observations means “supported but not observed in this capture.” A zero/absent capability manifest means coverage is unknown/legacy, not that the subsystem cannot emit anything. Any `observed_undeclared` row means the instrumentation manifest is stale and must be corrected before relying on negative coverage claims.

For frame/path disposition questions, use `frame_outcomes.csv`, `decisions.csv`, and `decision_stats.csv` before inferring anything from missing timing records. For synchronization questions, use explicit `wait_intervals.csv`/`wait_reason_stats.csv` together with `queue_dependencies.csv` and `wait_dependencies.csv`. For teardown/resource questions, use `state_residency.csv`, `resource_lifetimes.csv`, `resource_balance.csv`, and `anomalies.csv`. For feature-mode questions, check `state_conditioned_stats.csv` before comparing unrelated periods. For shader/compiler work, confirm `gpu_workloads.csv`/`gpu_workload_stats.csv` identifies the actual code variant before attributing a timing change to code generation.

## 4. Full short capture

Use `APUSR_PROFILE=full` only for a short controlled interval when sampled bursts do not provide enough contiguous detail. Increase `APUSR_PROFILE_RING_MIB` rather than converting the recorder into an unbounded logger.

Confirm after the run:

- `trace_health.csv` reports no unexpected overwrite;
- CPU scopes and explicit waits have sane nesting/durations;
- GPU timestamp calibration is present where the backend supports it;
- `gpu_queue_gaps.csv` is interpreted only as uncovered *profiled-interval* space, not automatically as hardware idle;
- the timeline does not show profiler-created synchronization that was absent from the runtime path.

## 5. Error-recorder mode

Use `APUSR_PROFILE=errors` for long-running failure reproduction. CPU/GPU performance instrumentation remains disabled while structured errors and semantic records are retained in the bounded ring.

A surviving error should request a fresh sampled burst only when sampled profiling is active; errors-only mode must not unexpectedly turn into continuous timing capture.

## 6. Cross-backend and cross-run comparison

Use the same non-empty `APUSR_PROFILE_SESSION` only for runs that are intentionally comparable. Configuration hashes and semantic stream identities must match before treating timing differences as equivalent work.

For two report directories, use:

```powershell
python .\tools\profiler\apusr_profile_compare.py `
  .\baseline-report `
  .\candidate-report `
  -o .\comparison `
  --baseline-label baseline `
  --candidate-label candidate
```

The comparator deliberately does not infer repository versions or application names from paths. Experiment labels are supplied by the caller. Inspect both `metric_deltas.csv` and `distribution_deltas.csv`: the latter uses retained raw samples and reports bootstrap intervals/common-language direction evidence. The generated summary also separates same-state timing and static GPU code-size changes from ordinary timing deltas.

Bootstrap intervals only describe sampling uncertainty in the retained samples. They do not compensate for different scenes, input, clocks, thermals, power state, or warm-up behavior. Controlled experiments remain mandatory.

## 7. Frame-generation-specific sanity checks

When profiling Frame Generation, verify the actual architecture rather than assuming one queue model:

- source presentation must remain fail-open;
- generated-frame eligibility/disposition is visible as structured decisions;
- temporal reset/history-gap warmup reasons are distinguishable;
- previous-generation history dependencies are represented by native synchronization records rather than CPU serialization;
- producer-ready latency is distinguishable from actual generation execution latency;
- presentation pacing waits are explicit timer waits rather than opaque CPU scopes;
- GPU intervals identify the actual interpolation/generation queue when it differs from the presentation stream queue;
- source and generated physical presentations remain distinguishable in display-timing data.

Do not add a profiler-only submit or wait merely to make one of these quantities easier to measure.

## 8. Qualification standard

A profiler change is ready to land when:

1. profiler-disabled and profiler-enabled test suites pass;
2. profiler-disabled runtime behavior is unchanged;
3. the structured-report contract parses a real emitted trace;
4. an older compatible `.aprof` trace still parses when the trace format was not intentionally revised;
5. `python -m py_compile` succeeds for the reporter/comparator/tests;
6. a sampled capture answers the intended question without relying on undocumented assumptions;
7. stream capability manifests agree with actually emitted semantic record classes;
8. static GPU workload identity is present where the backend declares support and remains explicitly unavailable where it does not;
9. any platform path not compiled in the available environment is called out explicitly rather than implied to be validated.
