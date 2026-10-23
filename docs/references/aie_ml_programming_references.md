<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# AIE-ML programming reference index

This file is a repo-local index of upstream programming references used by APUSR. It is not a copy of AMD documentation.

## Architecture baseline

APUSR uses AMD AIE-ML architecture/programming documentation as the baseline for tile memory, memory tiles, streams, locks, cascade/SRS datapaths, DMA, sparse support, and tensor-addressed transfers. Vendor manuals are referenced rather than redistributed in this repository.

## AMD online references

- AI Engine-ML Kernel and Graph Programming Guide, UG1603:
  - `https://docs.amd.com/r/en-US/ug1603-ai-engine-ml-kernel-graph/AI-Engine-API-Overview`
  - `https://docs.amd.com/r/en-US/ug1603-ai-engine-ml-kernel-graph/Matrix-Multiplication`
  - `https://docs.amd.com/r/en-US/ug1603-ai-engine-ml-kernel-graph/Multi-Channel-Sliding-Multiplication`
  - `https://docs.amd.com/r/en-US/ug1603-ai-engine-ml-kernel-graph/Memory-and-DMA-Programming`
  - `https://docs.amd.com/r/en-US/ug1603-ai-engine-ml-kernel-graph/Tiling-Parameters-and-Buffer-Descriptors`
  - `https://docs.amd.com/r/en-US/ug1603-ai-engine-ml-kernel-graph/Sparse-Matrix-Multiplication`

- AI Engine-ML Intrinsics User Guide:
  - `https://download.amd.com/docnav/aiengine/xilinx2023_2/aiengine_ml_intrinsics/intrinsics/modules.html`
  - `https://download.amd.com/docnav/aiengine/xilinx2023_2/aiengine_ml_intrinsics/intrinsics/group__intr__gpvectorconv__srs.html`
  - `https://download.amd.com/docnav/aiengine/xilinx2023_2/aiengine_ml_intrinsics/intrinsics/group__intr__loadstore.html`

- AI Engine-ML v2 Intrinsics User Guide, feature survey only:
  - `https://download.amd.com/docnav/aiengine/xilinx2025_2/aiengine_ml_v2_intrinsics/intrinsics/topics.html`
  - `https://download.amd.com/docnav/aiengine/xilinx2025_2/aiengine_ml_v2_intrinsics/intrinsics/group__intr__gpvectorop__mul__8bx8b.html`

Use v2 pages to find modern names and related features. Do not treat a v2-only feature as a Phoenix/Hawk base requirement.

## APUSR halo notes

For APUSR front halo handling, use the GPU-written zero guard plus tensor DMA gather as the active frame-edge mechanism. Interior front halos come from the same row-major guarded frame view. Later same-scale stages can use local, neighbor, or memory-tile line views when that is the right physical shape.
