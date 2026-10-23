<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# RGP/RMV RDF helpers

AMD RGP (`.rgp`) and RMV (`.rmv`) captures are RDF (`AMD_RDF`) containers.
This directory contains small diagnostic helpers for offline inspection of those
captures. They are not part of the APUSR runtime or distributable DLL path.

## One-command APUSR profile report

Use this first when a zip contains an RGP capture and/or APUSR timestamp CSV:

```powershell
python tools/analysis/apusr_profile_bundle_report.py path\to\rgp_profiles_v526.zip --top 20
```

The report prints:

- timestamp dispatch medians and p95s, when `apusr_dx12_gpu_timestamps*.csv` is present;
- RDF chunk counts;
- compute-shader CodeObject candidates sorted by LDS/VGPR/SGPR shape;
- RGP ISA instruction-export hotspots, when a text instruction dump is included.

By default the CodeObject section is not filtered to any pass or workgroup shape.
Use repeatable `--workgroup` filters only when you intentionally want to narrow it:

```powershell
python tools/analysis/apusr_profile_bundle_report.py rgp_profiles.zip --workgroup 16,16,1 --workgroup 8,8,1
```


## Generic profile series report

Use this when comparing many versions. It is dispatch-agnostic: it writes tables
for every dispatch present in the timestamp captures and only creates a narrow
focus table when explicitly requested.

```powershell
python tools/analysis/apusr_profile_series_report.py `
  rgp_profiles_v526.zip rgp_profiles_v529.zip apusr_dx12_gpu_timestamps_v526.zip `
  --out-dir profile_report --baseline-run baseline --top-dispatches 32

python tools/analysis/apusr_profile_series_report.py captures\*.zip `
  --out-dir profile_report --focus-regex "fsr4_pass_.*" --focus-dispatch fsr4_post
```

Primary outputs:

- `dispatch_timing_stats.csv`: all dispatches, all runs, p50/p90/p99/max/mean.
- `dispatch_median_matrix.csv`: every dispatch as a column, every run as a row.
- `top_dispatch_medians.csv`: compact matrix for the top ranked dispatches.
- `dispatch_delta_vs_baseline.csv`: all-dispatch deltas against the baseline.
- `focus_dispatch_medians.csv`: optional exact/regex focus table only when requested.
- `instruction_dump_summary.csv` and `instruction_top_lines.csv`: generic ISA dump summaries.

## RDF / code-object summary

For lower-level capture inspection:

```powershell
python tools/analysis/rgp_rdf_summary.py path\to\capture.rgp --chunks
python tools/analysis/rgp_rdf_summary.py path\to\capture.rgp --code-objects --stage CS --workgroup 8,8,1 --min-lds 6000
```

The script can also read directories or zip bundles directly:

```powershell
python tools/analysis/rgp_rdf_summary.py rgp_profiles_v526.zip --code-objects --stage CS --workgroup 8,8,1 --sort-by lds_size --descending
```

The code-object table includes:

- requested workgroup size;
- wavefront size;
- SGPR/VGPR counts;
- LDS bytes per threadgroup;
- scratch bytes;
- API PSO hash and internal pipeline hash when RGP correlation chunks are present;
- loaded code-object event count and size when `COLoadEvent` chunks are present.

CSV output is useful for side-by-side comparisons:

```powershell
python tools/analysis/rgp_rdf_summary.py rgp_profiles_v526.zip rgp_profiles_v529.zip `
  --code-objects --stage CS --workgroup 8,8,1 --csv rgp_cs_8x8.csv
```


## Code-object side-by-side comparison

To see how SGPR/VGPR/LDS counts changed between captures, compare by API PSO
hash or internal pipeline hash:

```powershell
python tools/analysis/rgp_code_object_compare.py rgp_profiles_v526.zip rgp_profiles_v529.zip `
  --stage CS --workgroup 8,8,1 --min-lds 6000
```

This is useful for checking whether an apparent occupancy/register change is a
real shader-resource change, a different loaded variant, or simply a different
internal pipeline hash for the same API PSO.

## RGP instruction-export summary

When RGP's pipeline ISA view is exported as text, summarize it with:

```powershell
python tools/analysis/rgp_instruction_summary.py post_instructions_v526.zip --top 30
python tools/analysis/rgp_instruction_summary.py pass11_instructions.zip --only waitcnt
```

The instruction helper extracts header metadata such as API shader hash, internal
pipeline hash, and register allocation when present. It aggregates percent/cycle
cost by opcode and broad class (`waitcnt`, `dot4`, `vmem_load`, `lds_store`,
`valu`, etc.) and prints the largest individual hotspots.

## Vendored libamdrdf

`libamdrdf/` is a trimmed copy of AMD's MIT-licensed Radeon Data File library and
`rdfi` chunk-dump tool. Build it only when the Python summary is not enough:

```powershell
cmake -S tools/rgp/libamdrdf -B build_libamdrdf -G Ninja -DRDF_STATIC=ON
cmake --build build_libamdrdf --target rdfi
build_libamdrdf\rdfi\rdfi.exe print-chunk-info path\to\capture.rgp
```

On Linux/macOS the executable path is typically `build_libamdrdf/rdfi/rdfi`.

## Extracting code-object ELFs from RGP

RGP stores loaded shader code objects as AMDGPU ELF blobs in `CodeObject` RDF
chunks. Extract them directly from a capture or profile zip with:

```powershell
python tools/analysis/rgp_code_object_extract.py rgp_profiles_v526.zip `
  --stage CS --workgroup 8,8,1 --min-lds 6000 --out-dir rgp_code_objects
```

The output directory contains one `.elf` per matched shader code object and a
`manifest.csv` with the chunk ordinal, CodeObject index, workgroup size,
SGPR/VGPR/LDS/scratch counts, API PSO hash, and internal pipeline hash.

The tool can also write raw `.text` bytes:

```powershell
python tools/analysis/rgp_code_object_extract.py rgp_profiles_v526.zip `
  --stage CS --workgroup 8,8,1 --min-lds 6000 --extract-text
```

If an AMDGPU-capable `llvm-objdump` is available, it can produce static ISA text:

```powershell
python tools/analysis/rgp_code_object_extract.py rgp_profiles_v526.zip `
  --stage CS --workgroup 8,8,1 --min-lds 6000 `
  --disassemble --llvm-objdump C:\path\to\llvm-objdump.exe --mcpu gfx1151
```

This avoids manual code-object export from RGP. It does **not** yet reproduce
RGP's per-line Instruction Timing table. That table is derived from SQTT timing
correlation in the GUI; until the project decodes QueueEvent/SQTT correlation,
manual text export from RGP is still needed for percent/cycle hotspots.

## Static RGA analysis from RGP code objects

The Radeon Developer Tool Suite includes command-line `rga` binary-analysis mode,
plus AMD's `amdgpu-dis` and `shae` liveness analyzer.  APUSR does not vendor the
full suite, but the tools can consume a local extracted suite or the Linux `.tgz`
directly.

Use this when you want static ISA/resource/liveness information from code objects
embedded in an RGP capture, without manually exporting pipeline binaries in the
RGP GUI:

```powershell
python tools/analysis/rgp_code_object_rga_report.py rgp_profiles_v526.zip `
  --rdts-archive RadeonDeveloperToolSuite-2026-05-28-1806.tgz `
  --stage CS --workgroup 8,8,1 --min-lds 6000 --limit 8 `
  --out-dir rgp_rga_report
```

If the suite is already extracted:

```powershell
python tools/analysis/rgp_code_object_rga_report.py rgp_profiles_v526.zip `
  --rdts-root path\to\RadeonDeveloperToolSuite-2026-05-28-1806 `
  --stage CS --workgroup 64,1,1 --limit 16
```

Primary output:

- `rga_code_object_report.csv`: one row per analyzed code object, combining RGP
  CodeObject metadata with RGA resource usage and max-live VGPR/SGPR liveness.
- `code_objects/`: extracted `.elf` blobs.
- `rga/<code-object>/`: RGA-generated ISA, analysis CSV, VGPR liveness, SGPR
  liveness, logs, and optional CFG files.

This is static analysis.  It gives exact code-object ISA and live-register views,
but it still does **not** reproduce RGP's per-line Instruction Timing percentages
or cycles.  Those require SQTT timing correlation for the selected event.

## Experimental SQTT extraction/decode

RGP Instruction Timing is derived from SQTT/thread-trace data and is not stored
as a ready-made CSV table in the `.rgp` file.  The project now includes two
experimental helpers:

```powershell
python tools/analysis/rgp_sqtt_extract.py rgp_profiles_v526_2.zip --out-dir rgp_sqtt --write-code-objects
```

This extracts raw `SqttData` chunks, raw `QueueEvent` blobs, and optional
CodeObject ELFs.  It is useful even before we can fully decode instruction
timing because it preserves the raw inputs needed for offline analysis.

```powershell
python tools/analysis/rgp_sqtt_decode_experimental.py rgp_profiles_v526_2.zip \
  --decoder-lib /path/to/librocprof-trace-decoder.so \
  --out-dir rgp_sqtt_decode
```

This tries to feed RGP `SqttData` chunks into AMD's ROCprof Trace Decoder
library.  It writes raw wave/instruction records and an aggregated
`decoded_instruction_latency_by_pc.csv` if the library accepts the stream.
The path is deliberately experimental because the public decoder API is aimed
at ROCprof `.att` data, while RGP stores its SQTT payloads in RDF chunks with
RGP-specific event/pipeline correlation metadata.

If `--decoder-lib` is omitted, the tool only extracts the raw SQTT chunks and
writes a decode report with `status=extracted_only`.

### Building ROCprof Trace Decoder from rocm-systems

AMD is consolidating trace/profiling components under `rocm-systems`.  The
thread-trace decoder currently lives at:

```text
rocm-systems/projects/rocprof-trace-decoder
```

Build it from an extracted checkout or an archive:

```powershell
python tools/analysis/rocprof_trace_decoder_build.py `
  --rocm-systems-root path\to\rocm-systems-develop `
  --out-dir build\rocprof_trace_decoder `
  --build-aggregate-tool
```

or:

```powershell
python tools/analysis/rocprof_trace_decoder_build.py `
  --rocm-systems-archive rocm-systems-develop.zip `
  --out-dir build\rocprof_trace_decoder `
  --build-aggregate-tool
```

The build report prints the decoder library path.  Pass that to the SQTT probe:

```powershell
python tools/analysis/rgp_sqtt_decode_experimental.py rgp_profiles_v526_2.zip `
  --decoder-lib build\rocprof_trace_decoder\decoder_build\lib\librocprof-trace-decoder.so `
  --chunk-index 0 --max-bytes 4096 --decode-timeout-sec 10 `
  --out-dir rgp_sqtt_decode_probe
```

Use `--decode-timeout-sec` while experimenting.  Current observations on RGP
SQTT chunks:

- a very small prefix decodes as header/event/realtime records;
- larger raw RGP SQTT chunks can stall the public decoder path;
- this means the remaining gap is likely an RGP-specific slicing/wrapping or
  dispatch-range problem, not a missing decoder library.

The optional C++ aggregate tool built by `--build-aggregate-tool` is useful for
known-good ROCprof `.att` files and for future RGP slices:

```powershell
build\rocprof_trace_decoder\bin\rocprof_trace_aggregate input.att out_latency_by_pc.csv
```

It aggregates decoded timing by `(code_object_id, pc_address)`.  Joining that
with RGA disassembly is the intended path toward an RGP-like instruction timing
CSV once the RGP SQTT slicing/correlation problem is solved.

## QueueEvent and SQTT range probing

The RGP instruction-timing pane is derived from SQTT/thread-trace data plus the
selected queue event.  The timing table is not stored as a plain RDF CSV.  The
current tooling therefore exposes the intermediate pieces instead of pretending
that the full RGP GUI table is already solved.

Decode QueueEvent records into a generic CSV:

```powershell
python tools/analysis/rgp_queue_event_summary.py rgp_profiles_v526_2.zip --out-dir rgp_queue_events
```

Important columns:

- `queue_event_index`: RGP QueueEvent chunk index.
- `event_type_name`: `timed_event`, marker-like records, or observed SQTT region records.
- `maybe_command_buffer_id` and `event_id_in_command_buffer`: useful for grouping contiguous work.
- `gpu_begin_timestamp`, `gpu_end_timestamp`, `gpu_duration_ticks`: queue-event timing in RGP ticks.
- raw `u32_0..u32_17`: retained because the schema is still partially reverse-engineered.

After building `rocprof_trace_aggregate`, probe SQTT prefixes/windows:

```powershell
python tools/analysis/rocprof_trace_decoder_build.py `
  --rocm-systems-root path\to\rocm-systems-develop `
  --out-dir build\rocprof_trace_decoder `
  --build-aggregate-tool

python tools/analysis/rgp_sqtt_window_probe.py rgp_profiles_v526_2.zip `
  --aggregate-tool build\rocprof_trace_decoder\bin\rocprof_trace_aggregate `
  --chunk-index 0 --prefix 64K --prefix 256K --prefix 1M `
  --timeout-sec 20 --out-dir rgp_sqtt_probe
```

This writes `sqtt_window_probe.csv` with decode status, wave/instruction counts,
record counts, decoded time ranges, and paths to any latency/wave/raw-instruction
CSVs emitted by the aggregate tool.  Arbitrary window slicing is also supported
with `--window-size`, `--stride`, `--start`, and `--stop`, but arbitrary slices
are expected to fail unless they begin at a decoder-valid SQTT boundary.  The
main purpose is to discover usable byte ranges before attempting a full decode.

`rocprof_trace_aggregate` now accepts:

```text
--summary-json <path>
--wave-csv <path>
--raw-instruction-csv <path>
--min-time <ticks>
--max-time <ticks>
```

The raw timing values coming out of the ROCprof decoder must still be validated
against RGP's GUI.  In current experiments, small RGP SQTT prefixes decode, but
large raw chunks may stall or produce time domains that need more RGP-specific
correlation work.

### QueueEvent/SQTT realtime correlation probe

`rocprof_trace_aggregate` also emits the auxiliary records that AMD's
`rocprof-trace-decoder` exposes: realtime clock anchors, SQTT events,
occupancy, shaderdata, perf counters, GFX11+ other-SIMD activity, wave ranges,
and raw instruction rows.

Use this when a capture includes SQTT and you want to try producing a
QueueEvent-specific latency table without manually exporting RGP's Instruction
Timing text:

```powershell
python tools/analysis/rgp_sqtt_queue_correlate.py rgp_profiles_v526_2.zip `
  --aggregate-tool build\rocprof_trace_decoder\bin\rocprof_trace_aggregate `
  --sqtt-index 0 --prefix 1M --top-events 20 --out-dir rgp_sqtt_qe_corr
```

Primary outputs per capture:

- `decoded/slice_records.realtime.csv`: shader-clock/realtime-clock anchors.
- `decoded/slice_records.instructions.csv`: decoded raw instruction rows.
- `decoded/slice_records.events.csv`: decoded SQTT events, including cache
  flushes, dispatch-end events, code-object load/unload events, and cluster
  markers when present.
- `decoded/slice_records.occupancy.csv`: wave start/end occupancy records.
- `queue_event_correlation.csv`: every timed QueueEvent mapped, when possible,
  into decoder shader-clock begin/end windows.
- `selected_events.csv`: top correlated events selected for aggregation.
- `event_<queue_event_index>_latency_by_pc.csv`: per-PC latency/stall/active/idle
  rows for decoded instructions falling inside that QueueEvent window.

The current correlation assumes RGP QueueEvent GPU timestamps share the decoded
realtime-clock domain and handles the observed 36-bit timestamp wrap.  This is
still experimental.  A successful correlation tells us that a QueueEvent falls
within the decoded SQTT slice and that decoded instruction rows were observed in
that mapped time window; it is not yet guaranteed to match RGP's GUI timing
percentages exactly.

Useful narrowing examples:

```powershell
# Aggregate one specific QueueEvent index, e.g. after selecting it in RGP.
python tools/analysis/rgp_sqtt_queue_correlate.py rgp_profiles_v526_2.zip `
  --aggregate-tool build\rocprof_trace_decoder\bin\rocprof_trace_aggregate `
  --sqtt-index 0 --prefix 1M --queue-event-index 589

# Only consider long timed events.
python tools/analysis/rgp_sqtt_queue_correlate.py rgp_profiles_v526_2.zip `
  --aggregate-tool build\rocprof_trace_decoder\bin\rocprof_trace_aggregate `
  --sqtt-index 0 --prefix 1M --min-duration-ticks 10000 --top-events 30
```

If the selected event maps to a shader-clock range but has zero decoded
instructions, the decoded SQTT slice probably does not include the waves for
that event, or the RGP trace target CU/SIMD did not sample that dispatch.  Try a
larger prefix, a different `--sqtt-index`, or a specific byte window once valid
SQTT boundaries are known.
