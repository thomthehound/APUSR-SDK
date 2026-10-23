SPDX-License-Identifier: Apache-2.0
Copyright (c) 2026 thomthehound.

APUSR Streamline Vulkan compatibility package

Purpose:
  Native Vulkan DLSS/Streamline compatibility while preserving the game's exact
  stock Streamline implementation and stock sl.dlss semantics. The default path
  leaves stock sl.interposer.dll at the loader boundary. An optional loader-only
  bootstrap is staged for hosts that enter slInit before APUSR can otherwise
  become visible.

Installation:
  1. If an older APUSR package replaced sl.interposer.dll, restore the game's
     original file first. A sidecar named sl_interposer_apusr_original.dll is a
     strong sign that this restoration is required.
  2. Leave all stock Streamline DLLs in place.
  3. Copy this package's files into the game's binary directory.

  Default install: do not rename or replace sl.interposer.dll.

  If a host reaches stock slInit before APUSR's normal bootstrap can run:
    .\install_apusr_streamline_bootstrap.bat "."

  Restore the original loader boundary with:
    .\install_apusr_streamline_bootstrap.bat "." /restore

Files:
  nvapi64.dll
    NVAPI compatibility surface for stock Streamline capability discovery. It
    deliberately does not install Streamline hooks.

  nvngx_dlss.dll
    APUSR's ordinary NGX/DLSS implementation boundary. The game's own sl.dlss
    plugin translates Streamline SR requests into this API.

  nvngx.dll
    PE export-forwarder to nvngx_dlss.dll.


  apusr_sl_interposer_bootstrap.dll
    Inactive loader-only proxy. The installer preserves the exact game
    sl.interposer.dll as sl_interposer_apusr_stock.dll, then exposes this proxy
    under the original loader name. 261/262 exports are direct PE forwarders to
    the stock sidecar with identical names and ordinals; slInit performs only
    the early private attach and then enters the same stock implementation.

  install_apusr_streamline_bootstrap.bat
    Fail-closed activation/restoration script for that optional path.

  apusr_streamline_attach.dll
    Private Detours-based attachment to the already-loaded stock Streamline
    interposer. Entry points are resolved by name; optional functions that the
    loaded revision does not export are simply left unattached.

    For DLSS-SR, stock sl.dlss remains the owner of tags, options and external
    image-state preparation. APUSR changes only Vulkan requirements that would
    otherwise demand NVIDIA-only device extensions, then executes through NGX.
    Vulkan Reflex and DLSS-G compatibility functions are APUSR-owned attach
    translations. APUSR therefore excludes Reflex, DLSS-G, and unsupported
    DLSS-RR from the feature list passed into stock Vulkan slInit while retaining
    stock DLSS-SR and optional PCL ownership.

  apusr_vulkan_host_capture.dll
    Process-lifetime Vulkan loader/device/queue capture. APUSR installs it before
    host VkDevice creation, negotiates the native DOT4/compute requirements, and
    records the real queue topology.

  apusr_fsr4_int8_vulkan.shaderpack
    Native Vulkan FSR4 INT8 product shader pack.

Diagnostics:
  APUSR_HOST_DISCOVERY_LOG=1 enables the cold-path JSONL log.
  APUSR_LOG_DIR selects its output directory.

  Expected attach signature:
    event=attach.installed
    stock_pe_preserved=1

  Optional-bootstrap signature:
    event=bootstrap.slInit
    loader_shim_only=1
    event=attach.installed ... loader_bootstrap_alias=1

  A stale APUSR replacement interposer is rejected explicitly:
    reason=legacy_apusr_sl_interposer_detected

  APUSR never modifies the stock Streamline PE. On the default path it remains
  sl.interposer.dll; on the optional bootstrap path the same bytes are preserved
  as sl_interposer_apusr_stock.dll and remain the attached semantic backing PE.
