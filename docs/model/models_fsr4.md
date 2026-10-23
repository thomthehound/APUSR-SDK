<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 INT8 model notes

APUSR currently executes the FSR 4.0.2-derived INT8 graph on the GPU. The
resident-parameter and logical boundary descriptions also support experimental
GPU/NPU partitioning; NPU inference is not a production path.

Important axes:

```text
model family      FSR4 INT8
quality preset    native / quality / balanced / performance / DRS / ultraperf
capacity class    1080 / 2160 / 4320
NPU target        npu1 / npu2 (experimental artifacts only)
```

Capacity class controls the compiled graph/frame-slot envelope. Quality preset
controls model parameters and bookend shader permutation. Active render/display
size is per-frame metadata and must not be baked into the product. No product
path may single out the local test setup as the runtime contract; Balanced at
2160p is only one useful regression case among the full preset/resolution matrix.

For compatibility frontends, `UpscalerFrameOptions::qualityMode` is selected from the caller-provided quality/preset mode or from the active render/display ratio when the protocol does not supply an explicit mode. Ratio inference uses the active integer extents and only a one-pixel quantization allowance; it does not reclassify an explicit caller preset from rounded or model-aligned dimensions. The selected output class comes from the active display size: up to 1080p, up to 2160p, or 4320-class. Those choices become part of the reference runtime key and select the imported reference preset/capacity/permutation.

The GPU runtime prepares the selected shader/model resources outside the frame hot path; frame execution uses the active preset, capacity, and permutation. The resident NPU parameter arena and its explicit model-epoch load protocol are separate experimental work. They do not define the current GPU frame-submission ABI.

The raw/logical FSR4 INT8 extraction corpus is not part of APUSR anymore. It
belongs in the extraction/tooling workspace (`ffxnpu`, using `ffxint8` as the FSR4
source tree). APUSR keeps generated GPU runtime artifacts and the metadata needed to validate experimental NPU seams:

- model, input/output, and parameter metadata used by current GPU execution;
- C16 GPU/NPU boundary metadata for experimental placement;
- bookend shader constants and reference fixtures;
- experimental resident parameter arena manifests and binaries;
- validation tools for retained artifacts.

Future NPU parameter-pack work should produce an APUSR-native package layout rather
than preserve the extraction-tool workspace shape. Generated planning files are
allowed only when they describe current product artifacts or are needed to
regenerate them.

## Parameter knowledge bridge

APUSR keeps enough FSR4 INT8 model knowledge to regenerate and validate its
product artifacts without carrying the `ffxnpu` raw/logical extraction corpus.
The bridge docs live under `docs/model/`:

- `fsr4_parameter_provenance.md` records the extraction layers and repo boundary.
- `fsr4_parameter_usage_map.md` maps logical stages to GPU bookends and NPU resident arena slices.
- `apusr_parameter_packing_plan.md` records the current resident arena contract and future packing rules.

Use `tools/model/inspect_fsr4_params.py` and
`tools/model/validate_resident_params.py` for product-artifact checks. The raw
extraction and logical normalization tools remain in `ffxnpu`.

## INT8 implementation evidence from 4.0.2b and 4.1.1

Binary analysis of later FSR4 INT8 implementations provides useful independent
evidence for two APUSR design/performance directions: parameter-buffer-driven
kernels and bounded DP4A accumulator dependency depth. Treat these binaries as
comparative evidence only. APUSR owns its source and should implement the
semantics and scheduling that are best for its target hardware rather than copy
binary-patch decisions.

### 4.1.1: shader and parameter-layout differences

The inspected AMD-signed `4.1.1` / `FSR4-i8` upscaler has different shader
packaging and parameter access from the verified `4.0.2c` INT8 lineage.
Aggregate binary figures do not, by themselves, establish a change in neural
network topology:

| Property | 4.0.2c | 4.1.1 INT8 |
| --- | ---: | ---: |
| upscaler DLL size | 41,036,800 bytes | 28,761,864 bytes |
| embedded DXIL containers | 1,325 | 1,028 |
| DP4A-using shader variants | 396 | 168 |

The newer kernels move substantial packed parameter traffic out of literal
shader constants and into raw-buffer loads consumed by reusable DP4A kernels.
That is directionally consistent with APUSR's resident-parameter architecture:
large fleets of weight-specialized shaders are not a requirement for an FSR4
INT8 execution path. This observation does not establish AMD's parameter
lifetime or residency policy, and APUSR should retain its own model-epoch and
resident-arena contracts.

The stronger performance finding is DP4A scheduling. For 10 of the 12 inspected
neural passes, the maximum serial `dot4AddPacked` accumulator dependency depth
in the 4.1.1 INT8 shaders is four. Passes 9 and 11 are the exceptions, retaining
chains as deep as 36. The observed maxima are:

| Pass | 4.0.2c max DP4A chain | 4.1.1 max DP4A chain |
| --- | ---: | ---: |
| 1 | 8 | 4 |
| 2 | 8 | 4 |
| 3 | 1 | 4 |
| 4 | 16 | 4 |
| 5 | 16 | 4 |
| 6 | 1 | 4 |
| 7 | 32 | 4 |
| 8 | 32 | 4 |
| 9 | 32 | 36 |
| 10 | 16 | 4 |
| 11 | 16 | 36 |
| 12 | 8 | 4 |

The 4.1.1 corpus provides independent evidence for bounded accumulator
dependency depth. Ten passes have four-deep maximum chains; passes 9 and 11
retain longer chains. APUSR measurements show that aggressive de-chaining can
regress newer hardware, so scheduling changes require target-hardware evidence.

The internal shader names still use the `v07_fp8_no_scale` model-family label,
but the inspected INT8 variants execute signed packed `dot4AddPacked` DP4A.
Treat the internal name as model lineage, not as evidence that the `FSR4-i8`
path is executing FP8 arithmetic.

### 4.0.2b: scheduling evidence

The community FSR4 INT8 `4.0.2b` build provides independent scheduling evidence.
Binary comparison found that the change **keeps native signed packed
DOT4/DP4A** and instead rewrites selected long accumulator dependency chains
into independent `dot4AddPacked` operations followed by ordinary `i32`
reduction. The verified `4.0.2b` and `4.0.2c` binaries contain the same 1,325
shader containers; the later `c` changes are therefore outside the shader
corpus.

The inspected pre-`b` INT8 build and `4.0.2b` differ in exactly 252 shader
containers: all 18 variants of passes 3, 6, 9, and 11 plus 180 additional
`main` variants. Passes 1, 2, 4, 5, 7, 8, 10, and 12 are unchanged. This is
not a blanket removal of DP4A: unchanged passes also contain chained DP4A. The
changed kernels instead start packed dots from zero and reduce them with
ordinary `i32` adds, pointing to accumulator scheduling/compiler behavior
rather than a general incompatibility with the packed-dot instruction.

### APUSR compatibility experiment

APUSR reproduced the compatibility pattern incrementally without modifying the
imported FFX/ML2Code source. The experiment first isolated passes 3/6 and then
passes 9/11. The source shadows are now quarantined under
`experiments/rdna2_dp4a_compat/` rather than exposed through the product shader
compiler or CMake configuration.

`tools/analysis/dxil_dp4a_schedule.py` established that the experiment preserved
native signed packed DP4A while changing direct dependency depth. On the
balanced/2160 representative slice:

| Pass | Schedule | signed DP4A calls | max direct DP4A chain | DXIL bytes |
| --- | --- | ---: | ---: | ---: |
| 3 | serial | 512 | 16 | 16,064 |
| 3 | depth4 | 512 | 4 | 16,576 |
| 3 | dechained | 512 | 1 | 18,052 |
| 6 | serial | 2,048 | 32 | 59,208 |
| 6 | depth4 | 2,048 | 4 | 61,400 |
| 6 | dechained | 2,048 | 1 | 69,352 |
| 9 | serial | 5,760 | 32 | 174,884 |
| 9 | dechained | 5,760 | 1 | 201,992 |
| 11 | serial | 2,112 | 16 | 65,976 |
| 11 | dechained | 2,112 | 1 | 75,752 |

The isolated passes-3/6 comparison produced whole-upscaler GPU
medians were 4.7617 ms (`serial`), 4.7495 ms (`depth4`), and 4.8202 ms
(`dechained`). Those runs also shared an unusually fast game-level regime, so
that external speedup is not attributed to the scheduling change. Locally,
full de-chaining already made the affected kernels slower.

A second comparison held passes 3/6 de-chained and added the `4.0.2b`-style pass-9/11
de-chain. Pass 9 moved from 0.41316 to 0.43942 ms median execution
(**+6.36%**), pass 11 from 0.46082 to 0.49020 ms (**+6.38%**), and whole-upscaler
GPU median from 4.81970 to 4.86766 ms (**+1.00%**). Unchanged `fsr4_post` stayed
effectively flat at 1.16760 versus 1.16980 ms and retained the same recorded
DXIL hash. The changed passes had different DXIL hashes, matching the intended
experiment.

That cost is material enough to settle the mainline policy. RDNA2 compatibility
will not constrain the RDNA3/RDNA3.5 product shaders. The normal product path
keeps the faster reference-style accumulation for passes 3, 6, 9, and 11; the
compatibility source transforms are not part of product configuration, the
shipping shader dependency graph, or quality tests. The 180 additional
`main` variants changed by `4.0.2b` were never extended in APUSR and remain
unmodified.

This is not evidence that RDNA2 support is impossible or worthless. APUSR does
not have representative RDNA2 hardware with which to measure the compatibility
benefit. If a future RDNA2-specific branch is justified, the quarantine retains
the exact transforms, source contract, DXIL audit, and measured newer-hardware
cost so the work can resume without rediscovery.

See `../../experiments/rdna2_dp4a_compat/README.md` for the reproducible experiment setup and detailed measurements.
