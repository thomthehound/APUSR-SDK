<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Models

This directory is for APUSR model manifests, generated product metadata, and
small resident artifacts that are part of the current build contract.

Do not commit third-party raw model dumps, vendor blobs, xclbins, tuning
databases, or SDK-bundled model artifacts unless their license explicitly
permits redistribution in this repository.

When a required artifact cannot be redistributed, commit a manifest that records:

- artifact name
- upstream source
- expected hash, when available
- local destination path
- redistribution status
- instructions for supplying it locally

The runtime can still use precompiled and device-native artifacts. The repository
boundary is about ownership and redistribution, not about forcing a slow runtime
path.

## FSR4 boundary metadata

The FSR4-compatible product path needs generated boundary metadata, not
hand-maintained qscale guesses. The checked-in manifest records the real C16
seam: mode, model-resolution family, active output size, model output size,
logical middle-core size, padded width, row pitch, tensor IDs, scale domains,
layout, dtype, token size, and NPU target family.

APUSR does not carry a general-purpose raw model extraction workspace. It keeps
only the model-facing artifacts that are part of the build, reference-validation,
or reproducibility contract. The checked-in FSR4 INT8 reference payloads and
resident parameter arenas are derived from the audited FidelityFX SDK 2.0.0 /
FSR 4.0.2 MIT-licensed snapshot; see the local `README.md` and `LICENSE.md` files
in those directories.

The real-boundary manifest lives at `models/fsr4_int8/real_boundary_manifest_v0.json`.
The generated resident arena set lives under `models/fsr4_int8/resident_params/`.
If the FSR4 extraction changes, regenerate these artifacts from the external
extraction workspace and copy only the product-facing outputs back into APUSR.

See `docs/model/` for the parameter provenance, usage map, stage ledger,
stage placement plan, and APUSR-native packing plan. Use
`tools/model/inspect_fsr4_params.py` for a quick artifact summary and
`tools/model/validate_resident_params.py` for resident arena validation.

- `fsr4_middle_core_resident_records_v0.json` is the canonical resident primitive record catalog. Add future stage records there instead of creating one-off per-stage manifests or docs.

## FSR4 INT8 reference artifacts

`models/fsr4_int8/reference/fsr4_4_0_2_int8/` carries the imported FidelityFX SDK 2.0.0 / FSR 4.0.2 INT8 reference artifacts used by the APUSR FSR4 reference runner. These files are not frontend-specific: FFX replacement, NGX/DLSS facade, and future Vulkan/D3D11 frontends should all normalize into the same APUSR graph/backend contract.

The C++ pass graph boundary for those artifacts lives in `include/apusr/models/apusr_fsr4_int8_graph.h`. It deliberately describes logical model passes, dispatch grids, and backend candidates without exposing DX12, Vulkan, NGX, or FFX ABI structs.
