<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Project status

This document describes the current APUSR capability surface.

## Super resolution

### D3D12

The D3D12 backend is the most mature APUSR path. It executes the optimized FSR4-derived INT8 graph using offline-generated DXIL and accelerated packed signed integer dot products. FidelityFX-compatible and NGX/DLSS-compatible frontends are available.

### Vulkan

The Vulkan backend executes the same API-neutral graph as native Vulkan compute using offline-generated SPIR-V. It negotiates the integer-dot, subgroup, and compute-derivative capabilities required by the shader products and uses captured host device/queue topology rather than a D3D12 translation layer.

Backend-specific lowering is used when equivalent shared HLSL produces materially inferior SPIR-V. Matched APUSR workloads are near D3D12 compute parity, with some individual Vulkan passes meeting or outperforming their D3D12 counterparts.

## Frame generation

APUSR carries source-built frame-interpolation and optical-flow infrastructure rather than depending on a renamed binary provider.

D3D12 frame generation is functional. Vulkan frame generation is also functional through the Streamline compatibility path, including virtualized presentation resources, native Vulkan FI/OF dispatch, and latency-control integration. Broad compatibility, pacing consistency, and GPU cost remain active work.

## Compatibility frontends

| Frontend/integration | D3D12 | Vulkan |
| --- | --- | --- |
| FidelityFX-compatible FSR4 | Yes | Not currently a product frontend |
| NGX/DLSS-compatible SR | Yes | Yes |
| Stock Streamline cooperation | Yes | Yes |
| Reflex-style latency translation | Yes | Yes |
| DLSS-G-shaped frame-generation compatibility | Partial | Yes |
| DLSS Ray Reconstruction | No | No |
| Multi Frame Generation | No | No |

Unsupported feature selections should fail cleanly rather than silently executing an unrelated fallback.

## Platform direction

Windows is the current product environment. Linux is a planned distribution target centered on the native Vulkan backend.

D3D11 is not implemented as a native backend. The intended direction is a D3D11On12 bridge that establishes host resource/queue ownership and then enters the same API-neutral runtime rather than creating a separate model implementation.

## Experimental acceleration

### WMMA / cooperative matrices

Not part of the production path. Matrix-kernel work will begin from measured pass-level bottlenecks and preserve the existing DP4A implementation as the correctness/performance reference.

### NPU / XRT / MLIR-AIE

Experimental and currently paused. The repository contains model decomposition, memory-layout, tile, and kernel research intended to support selective NPU offload. No public release should imply that the NPU path is production-ready.
