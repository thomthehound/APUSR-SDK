<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Roadmap

APUSR already has working D3D12 and Vulkan super-resolution backends plus multiple compatibility frontends. The roadmap therefore emphasizes turning that engineering base into a maintainable public project rather than adding unrelated features as quickly as possible.

## Near term

1. **Public-repository hardening**
   - keep licensing/provenance metadata current as dependencies change;
   - complete documentation cleanup;
   - establish CI and reproducible Release builds;
   - standardize package naming and release artifacts.

2. **Compatibility hardening**
   - broaden D3D12/Vulkan host coverage;
   - keep stock Streamline semantics where possible;
   - improve failure diagnostics and installation/restore behavior;
   - close remaining Vulkan resource-lifetime edge cases.

3. **Frame-generation refinement**
   - improve GPU cost and pacing consistency;
   - remove remaining unnecessary copies;
   - retain clean source-frame behavior under missed generation deadlines.

4. **CPU/runtime optimization**
   - profile and reduce frontend/setup overhead now that the GPU DP4A path is mature.

## Medium term

5. **Linux/Vulkan distribution path**
   - package the native Vulkan backend for Linux-oriented hosts;
   - prefer standard loader/layer mechanisms where they improve deployment without taking over application semantics.

6. **D3D11On12 bridge**
   - translate D3D11 host resources/state into the existing D3D12/API-neutral runtime.

7. **WMMA/cooperative-matrix research**
   - prototype one measured dense pass at a time;
   - keep DP4A as the production/reference lane until a matrix implementation wins on real hardware.

## Longer term

8. **NPU offload**
   - resume XRT/MLIR-AIE work when the data-placement and synchronization model is strong enough to justify a second accelerator;
   - target meaningful contiguous portions of the model rather than isolated kernels whose transfer cost erases the compute win.

9. **Broader hardware/model flexibility**
   - generalize model/parameter manifests and backend capability selection without sacrificing the optimized AMD APU path.
