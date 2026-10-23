<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Engineering backlog

This is the project-wide index for unresolved correctness, performance, compatibility, release, and research work. Detailed source TODOs remain authoritative where they describe a local invariant.

The backlog is organized by durable issue and required engineering outcome.

## P0 — Public-repository hardening

**State:** active.

- Keep Apache-2.0 source headers and DCO contribution policy consistent as the tree evolves.
- Re-audit third-party redistribution whenever a dependency, SDK snapshot, generated vendor payload, or model source changes.
- Add continuous integration for the supported Windows build/test configurations.
- Document reproducible toolchain requirements, package outputs, and known host limitations.
- Remove remaining generated/debug artifacts from the source tree and enforce this through `.gitignore`/quality checks.
- Establish a stable release/versioning policy for public tags and releases.

## P0 — Host compatibility without semantic duplication

**State:** active.

APUSR supports multiple host entry points, but stock integration runtimes should remain authoritative where they already own private semantics. Continue broadening compatibility without creating parallel Streamline/NGX state machines or introducing per-host hacks into the model/backend core.

Priorities:

- validate stock-Streamline attachment across additional loader/order topologies;
- keep renderer-state ownership process-consistent across late-loaded compatibility modules;
- retain fail-closed capability negotiation for unsupported Vulkan device features;
- improve installation/deployment ergonomics without requiring scripting environments that are unnecessary for the runtime itself.

## P1 — Vulkan compatibility and product polish

**State:** active.

The native Vulkan super-resolution backend is functional and near the D3D12 compute baseline on matched APUSR work. Remaining work is primarily host breadth, packaging, and targeted backend refinement rather than a generic rewrite.

- broaden queue/device/topology qualification;
- close borrowed-command-buffer lifetime edge cases with real submission/retirement information;
- preserve exact resource-layout provenance at host ingress;
- continue SPIR-V/native-ISA investigation only when profiling identifies a backend-specific bottleneck;
- prepare the Vulkan path for Linux-oriented distribution/loader integration.

## P1 — Frame-generation performance and pacing

**State:** active.

- reduce FI/OF GPU cost toward a consistently low single-digit millisecond budget;
- keep virtual-swapchain lead bounded and source presentation responsive;
- eliminate remaining avoidable full-resolution copies;
- preserve source-frame delivery when generation misses a real-time deadline;
- continue validating teardown/re-enable behavior and low-latency integration across supported queue topologies.

## P1 — CPU/runtime optimization pass

**State:** ready.

The GPU DP4A path has received substantially more attention than the general CPU/front-end runtime. Audit the production frame path for:

- repeated parameter conversion or lookup;
- unnecessary allocation/lifetime churn;
- redundant descriptor/resource bookkeeping;
- avoidable locking/atomics on the hot path;
- repeated frontend discovery or feature negotiation;
- logging/profiling work that leaks into normal disabled operation.

Measure before and after at the APUSR boundary rather than relying on source-level intuition.

## P1 — Profiler maturity

**State:** active.

The structured profiler is useful for CPU/GPU pass attribution and frame-generation flow. Continue improving it where new optimization work exposes missing observability, but avoid injecting heavy queue instrumentation into normal profiling unless a specific question requires it.

Priorities:

- maintain accurate separation of logical frame IDs and presentation serials;
- keep sampled mode bounded and representative;
- improve cross-backend workload identity reporting;
- keep RGP/RGA/native-ISA tooling as focused escalation for questions the built-in profiler cannot answer.

## P2 — D3D11On12 integration

**State:** planned.

Implement D3D11 support as a host bridge into the existing D3D12/API-neutral runtime rather than as a separate model backend. The bridge must establish resource state, queue ownership, descriptor/view lifetime, and frame metadata before entering APUSR.

## P2 — WMMA / cooperative-matrix experiments

**State:** planned, not started.

Do not replace the production DP4A path wholesale. Begin with one measured dense pass whose shape maps naturally to hardware matrix operations. Preserve a reference/DP4A lane and require correctness plus hardware performance evidence before promoting any matrix implementation.

Open questions include:

- best INT8 cooperative-matrix spelling/lowering for AMD D3D12 and Vulkan;
- register/LDS pressure versus DP4A kernels;
- opportunities for fusion that remove memory traffic rather than merely dispatch count;
- whether the current compiler stack exposes the required AMD-native operations consistently across DXIL and SPIR-V.

## P2 — NPU/XRT/MLIR-AIE execution

**State:** paused experimental research.

Resume only after the GPU/product path and host synchronization contracts justify the complexity of a second accelerator.

When resumed:

- finish real vector/SRS implementations for the staged middle-core kernels;
- validate memory-tile budgets across the complete stage lifetime schedule;
- measure transfer/synchronization cost at full-frame cadence;
- prefer unified-memory/data-placement designs that avoid host-visible frame-delay tricks;
- qualify capacity classes and device families explicitly.

## P2 — RDNA2 compatibility research

**State:** quarantined experiment.

Mainline APUSR targets modern AMD APUs with RDNA3-class capabilities. The existing RDNA2 DP4A scheduling experiment remains isolated under `experiments/rdna2_dp4a_compat/` because preserving older scheduling behavior has a measurable cost on current targets and has not been justified by representative hardware results.

Reopen only with real compatibility demand and representative hardware/driver access.

## P2 — Optical-flow native masked-SAD lowering

**State:** needs hardware/compiler evidence.

Investigate whether Vulkan/SPIR-V can reliably reach AMD's specialized masked-SAD hardware operation for the optical-flow search pattern. Promote a compiler/runtime path only when the source contract, native ISA, and measured performance all agree. Otherwise retain the current exact packed-byte difference plus accelerated unsigned DOT formulation.
