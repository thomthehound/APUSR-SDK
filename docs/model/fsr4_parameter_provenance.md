<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 parameter provenance

APUSR uses the FSR4 INT8 model shape, but it does not own the FSR4 source model.
The source tree for extraction is the modified FidelityFX SDK workspace
(`ffxint8`). The exploration and extraction lab is `ffxnpu`. APUSR keeps only the
product artifacts and the knowledge needed to regenerate them.

## Source layers

```text
ffxint8 / FidelityFX SDK INT8 source
  pre.hlsl
  passes_*.hlsl
  post.hlsl
  initializers.bin
        │
        ▼
ffxnpu raw export
  manifest.json
  audit.json
  model_graph.json
  raw tensor payloads in ML2Code storage layout
        │
        ▼
ffxnpu logical normalization
  logical_model.json
  dense logical tensor payloads
  tensor sidecars with shape, layout, dtype, qscale, and source offsets
        │
        ▼
APUSR product artifacts
  boundary manifest
  generated bookend shader constants
  resident NPU parameter arenas
```

The raw export layer is lossless. It records how HLSL descriptors and
`initializers.bin` byte ranges point at the original model data. The logical
normalization layer removes descriptor padding and gives downstream tools dense
logical tensors. APUSR does not carry either layer.

## FSR4 source files that matter

The INT8 extraction path is built around four source classes:

| Source class | Role |
|---|---|
| `pre.hlsl` / `pre_common.hlsli` | Provider-dispatched preprocessing plus logical stage 0. |
| `passes_*.hlsl` | Generated logical stages 1 through 12. |
| `post.hlsl` / `post_common.hlsli` | Logical stage 13 plus model-guided post filtering and output writes. |
| `initializers.bin` | Mode-specific model initializer blob referenced by generated HLSL descriptors. |

The shader sources in APUSR remain useful reference material. They explain the
provider schedule, parameter use, scale-domain names, and bookend math. They are
not the runtime parameter database.

## Product artifacts kept by APUSR

APUSR keeps three FSR4-derived artifact classes:

| Artifact | Path | Why it stays |
|---|---|---|
| Boundary manifest | `models/fsr4_int8/real_boundary_manifest_v0.json` | Records the real C16 GPU/NPU seam for all mode/capacity permutations. |
| Bookend shader constants | `shaders/generated/apusr_fsr4_bookend_weights.hlsli` | Feeds the current GPU ingress/egress bookends without a raw model sidecar. |
| Resident arenas | `models/fsr4_int8/resident_params/` | Parameter payloads consumed by the NPU middle-core load sequence. |

Anything below these layers is source extraction or schema exploration. That data
belongs in `ffxnpu` and should be copied back into APUSR only after it has been
reduced to a product-facing artifact.

## Regeneration rule

Regeneration is a two-repo operation:

```text
1. Use ffxnpu against ffxint8 to produce audited raw/logical model data.
2. Validate raw/logical data in ffxnpu.
3. Generate APUSR-facing boundary/bookend/resident artifacts.
4. Copy only those generated artifacts into APUSR.
5. Run APUSR product artifact checks.
```

Do not copy `raw/`, `logical/`, `DATASET_MANIFEST.json`, or exploratory export
scripts back into APUSR. If APUSR needs a new fact from extraction, add that fact
to a product manifest or to these docs.
