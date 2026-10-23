<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# APUSR parameter packing plan

APUSR needs an NPU-native parameter format, not a preserved copy of the ffxnpu
logical tree. The current resident arena is a seed format for the stage 1..12
middle core. It is intentionally narrow and can change as the kernel contract
becomes more concrete.

## Current contract

```text
schema:              com.apusr.middle_core_resident_parameter_arena.v1
profile:             am020_4x4_base
payload bytes:        122,160
arena bytes:          122,880
alignment:            4 KiB arena alignment, 32-byte internal packing floor
load timing:          model epoch / preset change
hot-path DMA bytes:   0
```

The runtime-visible rule is simple: load the matching resident arena before a
model epoch becomes active, then run frame slots without touching parameter
memory on the hot path.

## Stage-ledger dependency

Parameter packing follows `models/fsr4_middle_core_stage_ledger_v0.json`. The
ledger is the source of truth for which logical stage owns a primitive, which
physical row should consume the record, and which current resident slice is only
a provisional grouping. Do not derive packed order from source tensor names alone.

## Packing principles

1. Pack for the NPU kernel, not for the source shader layout.
2. Keep the public GPU/NPU seam independent from internal tile layout.
3. Keep scale-domain and epilogue records explicit.
4. Treat stage grouping as a product-kernel decision.
5. Keep debug/parity paths honest about bandwidth cost.
6. Validate every checked-in binary by size, SHA256, slice offsets, and slice
   hashes.

## Near-term native packing work

The next real packer should consume the ffxnpu logical bundle externally and emit
a fresh APUSR product artifact set. Expected output:

```text
models/fsr4_int8/resident_params/<permutation>/<profile>/stage1_12_full_arena.bin
models/fsr4_int8/resident_params/<permutation>/<profile>/stage1_12_full_arena.json
models/fsr4_int8/resident_params/stage1_12_full_arena__<profile>.json
```

The emitted metadata should record:

- source logical bundle hash;
- source tensor IDs for every packed record;
- tensor dtype/layout before packing;
- physical lane/tile order after packing;
- epilogue scale/multiplier/shift records;
- activation clamp domain (`s8`, `u8_relu`, or explicit fallback);
- per-slice offsets, sizes, padding, and SHA256.

## Repo-local tools

The scripts under `tools/model/` are product-facing helpers:

| Tool | Purpose |
|---|---|
| `inspect_fsr4_params.py` | Print boundary and resident-arena summaries from checked-in artifacts. |
| `validate_resident_params.py` | Validate checked-in resident arena binaries and manifests. |
| `pack_apusr_params.py` | Stage the current resident parameter set into an output directory for runtime/package experiments. |

These tools do not reintroduce raw/logical extraction into APUSR. External
extraction remains an ffxnpu job until a new product artifact is ready to land.
