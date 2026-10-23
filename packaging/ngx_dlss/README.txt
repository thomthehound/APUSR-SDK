SPDX-License-Identifier: Apache-2.0
Copyright (c) 2026 thomthehound.

APUSR direct NGX/DLSS package (D3D12)

Purpose:
  Direct D3D12 NGX/DLSS facade for games that already load nvngx.dll or
  nvngx_dlss.dll.

Files:
  nvngx_dlss.dll
    APUSR NGX/DLSS implementation for the established D3D12 FSR4 INT8 backend.
  nvngx.dll
    Tiny PE export-forwarder alias to nvngx_dlss.dll; it does not duplicate the
    implementation or state.

Deliberately absent:
  apusr_fsr4_int8_vulkan.shaderpack
  apusr_vulkan_host_capture.dll
  sl.interposer.dll

  Direct NGX packaging is D3D12-oriented and no longer triggers or carries a
  Vulkan product shader pack that it cannot reliably bootstrap on its own.

Notes:
  This package does not forward into NVIDIA DLSS. It translates the host's
  NGX/DLSS parameter bag into APUSR's internal frame contract and records
  APUSR-owned D3D12 backend work. Use apusr_package_dlss_compatibility when the
  host hides DLSS before NGX is loaded, or
  apusr_package_dlss_multibackend_compatibility for a Streamline game that can
  switch between D3D12 and Vulkan. The multi-backend package preserves the
  game's stock Streamline DLLs.
