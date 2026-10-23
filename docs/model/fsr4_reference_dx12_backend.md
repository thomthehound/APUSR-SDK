<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 INT8 DX12 reference backend

This backend is the first implementation-side consumer of the API-neutral FSR4
INT8 graph and resource plan.  Its current job is to turn the reference plan into
native DX12 runtime work while keeping DX12 details below the model layer:

- turn `ReferenceFramePlan` into a DX12-oriented shader build plan;
- turn the FFX-compatible resource binding inventory into scoped SRV/UAV/CBV register
  ranges;
- turn those ranges into an FFX-compatible root-signature and descriptor-heap plan;
- realize persistent/internal resources, upload resources, and borrowed external
  resources;
- patch scoped SRV/UAV descriptors and root CBV bindings;
- record the native DX12 compute dispatch sequence when supplied a real command
  list;
- prove that the imported FFX INT8 HLSL artifacts compile with APUSR's selected
  defines and include roots;
- keep this backend behind an APUSR-owned seam so the future Vulkan backend can
  consume the same graph instead of copying frontend logic.

The backend can now record the native DX12 command sequence in a controlled path.
It is still not wired as the default in-game replacement path; the next product
step is a controlled frame validation harness and then frontend integration once
resource contents and image parity are understood.

## Shader compile contract

The imported FFX INT8 shaders require the following baseline DX12 compile flags:

```text
-T cs_6_6
-HV 2021
-enable-16bit-types
-D FFX_HLSL=1
-D FFX_HALF=1
-D FFX_HLSL_SM=66
-D DOT4_ENABLED=1
-D WMMA_ENABLED=0
```

Generated middle stages are additionally selected with one `MLSR_PASS_N=1`
define.  Without that define, `passes_*.hlsl` contains the generated function but
DXC sees no active entry point.

## Developer preflight

A quick compile check over the universal preset/capacity source matrix is available:

```powershell
python tools/model/verify_fsr4_reference_shaders.py `
  --repo-root . `
  --preset all `
  --capacity all `
  --quick
```

A full middle-chain preflight omits `--quick` and should be run on selected slices before packaging a new cache matrix.

CMake also exposes:

```powershell
cmake --build build --target apusr_verify_fsr4_reference_shaders
```

## Runtime resource lifecycle

The backend now converts the API-neutral `ResourcePlan` into a DX12 resource
realization plan.  External input/output resources are borrowed per frame and are
never owned by APUSR.  Persistent internal textures/buffers and upload resources
are keyed by max render size, aligned max upscale size, capacity, scratch size,
initializer size, and debug-resource presence, so ordinary frames do not rebuild
allocations.  The realization plan also records alias targets such as
`MlsrOutput -> RcasTemp` when RCAS is enabled.

With a real `ID3D12Device`, `recordFrame` creates persistent/internal and upload
`ID3D12Resource` objects, records one-time initializer/default-exposure copies,
patches SRV/UAV descriptors, uploads per-frame root-CBV constants, emits resource
barriers, and records the planned dispatches. Borrowed external resources are
restored to the host-supplied states after dispatch, matching the FidelityFX
provider unregister-resource contract. This is now a fixed correctness contract,
not a CMake or environment-variable mode.

## Release hot-path policy

The reference route is not allowed to spend release-frame work on diagnostics that the FFX INT8 provider does not perform.  Product builds must keep these out of `recordFrame`/dispatch recording unless an explicit diagnostic build option enables them:

- route/diagnostic trace string construction and file I/O;
- shader reflection or reflection-manifest validation;
- runtime HLSL compilation or filesystem source probing;
- descriptor view format validation through `CheckFeatureSupport`;
- readback, checksum, progress probes, or host-specific safety clears;
- external-resource state restoration unless the diagnostic restore option is explicitly enabled.

The remaining per-frame work should correspond to the FFX provider's real work: resolve the frame plan, bind/register the borrowed resources, update constants, patch descriptors as needed, issue required resource barriers/clears, and record the selected dispatches.  Validation scaffolding belongs in tests, packaging tools, or `APUSR_ENABLE_HOT_PATH_GUARDS`, not the release hot path.

Model-state changes follow FFX `CreateModel()` behavior.  First use, preset/capacity changes, and shader/model permutation changes force history reset.  Per-frame debug-view drawing and RCAS do not re-key the model; debug drawing only controls the optional debug clear/dispatch path.

Stem-to-stern parity now includes backend execution semantics, not only shader math.  The DX12 recorder must match the FFX provider contract for descriptor lifetime, constant upload lifetime, resource state progression, clear jobs, UAV ordering barriers, model-state resets, and frame-to-frame constants.  Any deviation must either remove work from the hot path without changing observable behavior or be documented as an intentional backend improvement.  Fake output routes are not valid reference behavior and are not part of the DLL.

Reference constants must follow the FFX dispatch code exactly.  High-resolution motion-vector scaling uses the raw requested upscale size, not the aligned internal output extent.  Preset selection uses the aligned active output/render ratio unless the context is explicitly dynamic-resolution.  Explicit frame/model resets must reach `MLSR_Optimized_Constants.reset`; tests that run in Release must keep assertions enabled for these contracts.

## Controlled native smoke

`apusr_fsr4_reference_dx12_record_smoke` is a Windows-only controlled recorder
smoke.  It creates a D3D12 device, queue, allocator, command list, borrowed input
textures, and borrowed output texture, then records and executes one Balanced
R2160 reference frame through the backend using the configured precompiled shader provider.  It is a
structural command-recording test, not a visual parity test.

Build it with:

```powershell
cmake --build build --target apusr_fsr4_reference_dx12_runtime_smokes
```

Run it with:

```powershell
ctest --test-dir build -R "apusr_fsr4_reference_dx12_record_smoke|apusr_fsr4_reference_dx12_plan_smoke" --output-on-failure
```

## Next step

The next implementation step is a controlled output-validation path: initialize
known input textures, read back a small output slice or checksum, and then start
comparing against the FFX reference behavior before wiring this path into any
game/frontend default.

## Descriptor/register plan

The DX12 reference backend now carries a scoped descriptor/register plan.  See `fsr4_reference_dx12_descriptor_plan.md` for the details and the reflected register facts that drive it.  The key rule is that descriptor patching must be dispatch-scope aware; a single sequential descriptor list is not correct for these shaders.

## Root-signature plan

The backend now also carries a root-signature plan.  It deliberately follows the FFX DX12 backend policy: UAV table, SRV table, root CBVs, and static samplers.  See `fsr4_reference_dx12_root_signature_plan.md`.

## Shader and PSO state

The backend consumes precompiled DXIL from either a packaged cache or the generated embedded product shader table.  See `fsr4_reference_dx12_shader_pso_plan.md` for the current shader matrix, compile defines, and PSO-preparation boundary.
