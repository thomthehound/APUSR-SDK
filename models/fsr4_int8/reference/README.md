<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 INT8 reference artifacts

This subtree contains the FidelityFX FSR4 INT8 shader and parameter reference
artifacts used by APUSR's reference runner and shader-generation pipeline.

The imported shader sources and initializer payloads originate from the AMD
FidelityFX SDK 2.0.0 / FSR 4.0.2 reference snapshot retained under
`third_party/ffx_sdk/`. That snapshot is distributed under the MIT License; a
copy of that license is retained here as `LICENSE.md` because this subtree is a
redistributable subset of the reference material.

Some files contain APUSR-maintained backend/lowering modifications. Those
modifications do not erase the AMD provenance or change the MIT license of the
underlying reference files.
