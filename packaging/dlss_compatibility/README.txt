SPDX-License-Identifier: Apache-2.0
Copyright (c) 2026 thomthehound.

APUSR DLSS compatibility package (D3D12)

Purpose:
  Broad D3D12 DLSS visibility and evaluation package for games that hide DLSS
  before loading NGX.

Files:
  dxgi.dll
    Early-load host compatibility gate. It allows APUSR to participate in DXGI
    adapter discovery and provides the cold-path seam for D3D12 queue capture.
    For stock Streamline hosts it preserves sl.dlss semantics and supplies an
    APUSR-backed NGXContext through Streamline's parameter bus when NVIDIA's
    signed NGX-core loader cannot load the unsigned APUSR implementation.
  nvapi64.dll
    NVAPI compatibility shim for early NVIDIA capability probes.
  nvngx_dlss.dll
    APUSR NGX/DLSS facade for the D3D12 FSR4 INT8 backend.
  nvngx.dll
    Tiny PE export-forwarder alias to nvngx_dlss.dll, so both NGX probe names
    resolve to one implementation/state owner.

Deliberately absent:
  sl.interposer.dll
  apusr_vulkan_host_capture.dll
  apusr_fsr4_int8_vulkan.shaderpack

  This package leaves stock Streamline DLLs untouched and does not stage Vulkan
  backend assets it cannot bootstrap by itself. Building this package therefore
  does not require generation of the Vulkan product shader pack.

Use instead:
  apusr_package_streamline_vulkan_compatibility
    for a Vulkan Streamline game. It preserves the game's stock interposer and
    attaches APUSR in-process by named exports.

  apusr_package_dlss_multibackend_compatibility
    when one install should support both D3D12 and Vulkan renderer modes while
    preserving the stock Streamline installation.

Diagnostics:
  Leave host discovery logging off for performance runs. Enable
  APUSR_HOST_DISCOVERY_LOG=1 only when DLSS visibility, NGX creation, or queue
  discovery is failing; APUSR_LOG_DIR selects the destination but does not
  enable logging.

Hot-path rule:
  The per-frame evaluate path must not do module discovery, JSON parsing,
  environment probing, optional logging, present-hook rendering, or fake
  passthrough output.

Feature policy:
  This package exposes APUSR's DLSS Super Resolution / DLAA-shaped path. DLSS
  sharpness is mapped into APUSR's guarded RCAS/FFX sharpening path when the
  host supplies a value. Unsupported DLSS/Streamline features remain unsupported.

Frame Generation:
  DLSS Frame Generation is not implemented here. Native AMD/FSR Frame Generation
  remains a separate compatibility path.
