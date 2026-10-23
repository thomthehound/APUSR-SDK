<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# RDNA2 DP4A compatibility quarantine

This directory preserves the DX12 packed-DOT4/DP4A scheduling experiments from
APUSR DX12 compatibility experiments without making them part of the product build, product
test graph, or normal shader compiler interface.

The mainline policy is deliberate: APUSR's current AMD APU targets are RDNA3 /
RDNA3.5-class devices, so the shipping DX12 path keeps the faster measured
reference-style accumulation for passes 3, 6, 9, and 11. RDNA2 compatibility
may be revived as a separate compatibility effort when representative hardware
and driver validation are available.

## Evidence and conclusion

The community FSR4 INT8 `4.0.2b` shader corpus did **not** replace packed DOT4
with scalar arithmetic. It retained signed `dx.op.dot4AddPacked.i32` operations
and changed the dependency shape of selected accumulations. Relative to the
pre-`b` build, exactly 252 shader containers changed: all 18 variants of passes
3, 6, 9, and 11 plus 180 additional `main` variants. Passes 1, 2, 4, 5, 7, 8,
10, and 12 were unchanged. The changed form starts packed dots from zero and
performs the reduction with ordinary `i32` additions. `4.0.2c` made no further
shader-corpus changes.

The later AMD-signed FSR 4.1.1 INT8 implementation supplies a related but not
identical clue: ten of twelve inspected neural passes limit maximum serial
packed-dot dependency depth to four; passes 9 and 11 retain deeper chains. This
supports the idea that dependency depth can matter, but it does **not** prove
that the `4.0.2b` transformation is the reason for RDNA2 compatibility or that
full de-chaining is universally optimal.

## APUSR reproduction

The quarantined include shadows preserve two controlled transformations while
leaving the imported FFX/ML2Code source untouched:

- passes 3/6: `depth4` caps a direct packed-DOT4 chain at four; `dechained`
  starts every packed DOT4 from zero and reduces separately;
- passes 9/11: `dechained` reproduces the compiler-visible `4.0.2b` form in
  `FNB_CT2D_ADD`, transforming 53 source-level self-accumulating DP4A statements.

Representative balanced/2160 DXIL measurements were:

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

The full six-preset x three-capacity pass-9/pass-11 matrix was also compiled and
audited in both schedules; all 72 shaders retained the expected
signed-DP4A counts and dependency depths.

## Hardware cost measured on a current RDNA3-class target

The isolated passes-3/6 hardware comparison produced:

| pass3/pass6 schedule | whole-upscaler GPU median | pass 3 median | pass 6 median |
| --- | ---: | ---: | ---: |
| serial | 4.7617 ms | 0.0781 ms | 0.0701 ms |
| depth4 | 4.7495 ms | 0.0790 ms | 0.0807 ms |
| dechained | 4.8202 ms | 0.0880 ms | 0.1123 ms |

Those three runs landed in the same unusually fast overall regime, so the
experiment must not be credited for the unrelated frame-level speedup. The local
pass behavior still showed that de-chaining was not an optimization on the
newer target.

The next capture kept passes 3/6 de-chained and added the `4.0.2b`-style
pass-9/pass-11 de-chain. Comparing the two existing captures produced 286
samples per side:

| Metric | pass3/6 de-chained baseline | + pass9/11 de-chained | Delta |
| --- | ---: | ---: | ---: |
| pass 9 execution median | 0.41316 ms | 0.43942 ms | +0.02626 ms / **+6.36%** |
| pass 11 execution median | 0.46082 ms | 0.49020 ms | +0.02938 ms / **+6.38%** |
| whole-upscaler GPU median | 4.81970 ms | 4.86766 ms | +0.04796 ms / **+1.00%** |
| unchanged `fsr4_post` median | 1.16760 ms | 1.16980 ms | +0.00220 ms / +0.19% |

Bootstrap 95% intervals were +0.02484..+0.02780 ms for pass 9,
+0.02662..+0.03152 ms for pass 11, and +0.03368..+0.06258 ms for the whole
upscaler. The profiler's workload-identity comparison showed changed DXIL hashes
for passes 9 and 11 while `fsr4_post` retained the same hash.

This is enough to reject blanket compatibility de-chaining as a mainline
RDNA3/RDNA3.5 policy. It is **not** enough to reject a future RDNA2-specific
branch, because no representative RDNA2 device was available to measure the
benefit side of the tradeoff.

## Mainline boundary

The product build has no `APUSR_FSR4_DP4A_PASS36_SCHEDULE` or
`APUSR_FSR4_DP4A_PASS911_SCHEDULE` cache variables. The normal shader compiler
has no corresponding command-line switches. These sources are not in the
shipping shader dependency list and this verifier is not registered as a
product/quality test.

`tools/analysis/dxil_dp4a_schedule.py` remains in the normal toolset because it
is a generally useful DXIL analysis utility and has no runtime or shader-path
cost.

## Reproducing the quarantined compiler experiment

From the repository root:

```text
python experiments/rdna2_dp4a_compat/verify_dp4a_schedule.py
```

That audits the balanced/2160 representative slice. For the full six-preset x
three-capacity matrix:

```text
python experiments/rdna2_dp4a_compat/verify_dp4a_schedule.py --full-matrix
```

The verifier injects the quarantined include directory directly when compiling
its temporary DXIL. It does not alter the product configuration or generated
shader cache.

## Conditions for revival

Do not promote this work back into mainline merely because it compiles or
appears compatible in another binary. A future RDNA2 compatibility effort must:

1. validate on representative RDNA2 hardware and drivers;
2. identify the minimum compiler-visible transformation required for correct
   execution rather than assuming full de-chaining is necessary;
3. retain native packed DP4A unless evidence proves a different path is better;
4. measure the cost on current RDNA3/RDNA3.5 targets before sharing any shader
   family with mainline; and
5. prefer a separate compatibility branch/path when the compatibility tax is
   material.
