<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 parameter usage map

The FSR4 INT8 model is a fixed 14-stage U-Net-like core. The imported DP4A reference provider does
not dispatch all 14 logical stages as separate generated pass kernels. The real
provider layout is:

```text
GPU pre shader:  preprocessing + logical stage 0
NPU target:      logical stages 1 through 12
GPU post shader: logical stage 13 + model-guided output/post work
```

The APUSR product split follows the compact C16 seam:

```text
GPU bookend ingress -> int8 NHWC C16 frame slot
NPU middle core     -> stages 1..12
GPU bookend egress  -> int8 NHWC C16 frame slot
```

## Stage ownership

| Logical stage | Operator family | Owner in APUSR | Parameter handling |
|---:|---|---|---|
| 0 | `Conv2D_k2s2b` | GPU ingress bookend | Generated HLSL constants. |
| 1 | `ConvNextBlock` | NPU middle core | Resident arena slice. |
| 2 | `ConvNextBlock` | NPU middle core | Resident arena slice; C16 skip source. |
| 3 | `FusedConv2D_k2s2b_QuantizedOutput` | NPU middle core | Packed with adjacent C32 work in the current arena. |
| 4 | `FasterNetBlock` | NPU middle core | Packed with stage 3 in the current arena. |
| 5 | `FasterNetBlock` | NPU middle core | Resident arena slice; C32 skip source. |
| 6 | `FusedConv2D_k2s2b_QuantizedOutput` | NPU middle core | Packed with adjacent C64 work in the current arena. |
| 7 | `FasterNetBlock` | NPU middle core | Packed with stage 6 in the current arena. |
| 8 | `FasterNetBlock` | NPU middle core | Resident arena slice. |
| 9 | `FNB_CT2D_ADD` | NPU middle core | Resident arena slice; consumes C32 skip. |
| 10 | `FasterNetBlock` | NPU middle core | Resident arena slice. |
| 11 | `FNB_CT2D_ADD` | NPU middle core | Resident arena slice; consumes C16 skip. |
| 12 | `ConvNextBlock` | NPU middle core | Resident arena slice. |
| 13 | `CNB_CT2D` plus post filter | GPU egress/post bookend | Generated HLSL constants and post shader logic. |

The current resident arena groups some adjacent logical stages because the
product graph is a spatial microtile pipeline, not a literal provider-dispatch
clone. The canonical planning contract for those choices is
`models/fsr4_middle_core_stage_ledger_v0.json`; this page is only the compact
human map.

## Current resident arena slices

Each mode/capacity permutation uses the same resident arena shape for
`am020_4x4_base`:

| Slice | Logical role | Payload bytes |
|---|---|---:|
| `stage1_convnext16` | stage 1 C16 ConvNext block | 4,048 |
| `stage2_convnext16` | stage 2 C16 ConvNext block | 4,048 |
| `stage3_4_down_fnb32` | stage 3 downsample plus stage 4 C32 block | 10,000 |
| `stage5_fnb32_c8group` | stage 5 C32 block | 7,696 |
| `stage6_7_down_fnb64` | stage 6 downsample plus stage 7 C64 block | 36,880 |
| `stage8_fnb64` | stage 8 C64 block | 28,176 |
| `stage9_ct2d32_chunks` | stage 9 C32 upsample/skip join | 8,896 |
| `stage10_fnb32_c8group` | stage 10 C32 block | 7,696 |
| `stage11_fnb32_ct2d16` | stage 11 C16 upsample/skip join | 10,064 |
| `stage12_convnext16` | stage 12 C16 ConvNext block | 4,048 |

The arena payload is 122,160 bytes. The checked-in binary is 122,880 bytes after
page alignment. The model-epoch load path moves the resident arena once; hot
frame runs move no parameter bytes.

## Scale and activation conventions

The logical INT8 bundle records both tensor quantization scales and scalar
arguments from the generated shader call sites. APUSR carries the resulting
product-scale facts in boundary manifests, generated shader constants, and
resident arena metadata.

Implementation rules:

- Public stage boundaries use the manifest-declared INT8 scale domains.
- Fused internal ReLU edges may use an unsigned `u8 × s8` NPU path when the
  packer records that domain explicitly.
- Do not infer a stage scale from a filename or from a shader symbol name alone.
- The NPU packing manifest owns physical layout and epilogue records; the FSR4
  logical model owns tensor meaning.
