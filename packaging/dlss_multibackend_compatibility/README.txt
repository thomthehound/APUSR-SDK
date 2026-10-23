SPDX-License-Identifier: Apache-2.0
Copyright (c) 2026 thomthehound.

APUSR DLSS multi-backend compatibility package

Purpose:
  One install for Streamline/DLSS games that can switch between D3D12 and
  Vulkan. By default the game's own sl.interposer.dll remains at the loader
  boundary. For hosts that enter stock slInit before any normal APUSR DLL can
  become visible, the package also stages an optional loader-only bootstrap.
  That bootstrap preserves the exact stock PE as a sidecar and does not replace
  Streamline semantics.

Installation:
  1. If an older APUSR package replaced sl.interposer.dll, restore the game's
     original file first. A sidecar named sl_interposer_apusr_original.dll is a
     strong sign that this restoration is required.
  2. Leave the stock Streamline DLLs exactly where the game installed them.
  3. Copy this package's files into the game's binary directory.

  Default install: do not replace or rename sl.interposer.dll.

  Early-bootstrap install, only when the host is known to enter slInit too early:
    .\install_apusr_streamline_bootstrap.bat "."

  The installer renames the exact game file to sl_interposer_apusr_stock.dll and
  installs the staged loader-only proxy as sl.interposer.dll. Restore with:
    .\install_apusr_streamline_bootstrap.bat "." /restore

Files:
  dxgi.dll
    Early D3D12/DXGI visibility and adapter-discovery compatibility surface.
    Its renderer-neutral LoadLibrary/GetProcAddress interception remains an
    opportunistic bootstrap when DXGI becomes visible before the host resolves
    slInit. It cannot solve a host whose first APUSR-visible DXGI load happens
    from inside an already-entered stock slInit. It does not select a renderer
    or start identity spoofing merely because dxgi.dll was loaded.
    It preserves stock D3D12 Streamline evaluation and, at plugin startup only,
    supplies APUSR's NGXContext through the Streamline parameter bus when the
    NVIDIA signed-core loader cannot load an unsigned replacement core.

  nvapi64.dll
    NVAPI compatibility shim used by D3D12 host and Streamline capability
    discovery. It supplies the NVIDIA-shaped capability answers expected by
    those probes and does not itself load the private Streamline attachment.

  nvngx_dlss.dll
    APUSR NGX/DLSS implementation boundary. Direct NGX hosts enter here; D3D12
    stock sl.dlss reaches its exported implementation through the injected NGX
    context callbacks; and stock sl.dlss reaches the ordinary NGX boundary on
    Vulkan.

  nvngx.dll
    PE export-forwarder to nvngx_dlss.dll; it does not duplicate frontend state.


  apusr_sl_interposer_bootstrap.dll
    Inactive loader-only Streamline bootstrap. When explicitly installed with
    install_apusr_streamline_bootstrap.bat it becomes sl.interposer.dll, while
    the exact game PE is retained as sl_interposer_apusr_stock.dll. All exports
    except slInit are direct PE forwarders to that sidecar with the stock names
    and ordinals. slInit only establishes the private APUSR attachment, then
    enters the same stock slInit implementation.

  install_apusr_streamline_bootstrap.bat
    Fail-closed installer/restorer for the optional bootstrap. It refuses an
    ambiguous sidecar or an unrelated sl.interposer.dll rather than guessing.

  apusr_streamline_attach.dll
    Private in-process Streamline compatibility module. It finds the already-loaded
    stock sl.interposer.dll, resolves supported API entry points by NAME, and
    attaches only renderer-specific behavior APUSR owns. It is not a host-facing
    Streamline facade.

    DLSS Super Resolution remains owned semantically by the game's stock
    Streamline sl.dlss plugin. On Vulkan, APUSR sanitizes only the device
    requirements that stock sl.dlss would otherwise populate with NVIDIA-only
    extensions; evaluation then enters APUSR through the ordinary NGX boundary.
    APUSR-owned Vulkan Reflex and DLSS Frame Generation translation remain on
    the attach path. D3D12 Reflex also uses the attach path and maps to AMD
    Anti-Lag 2 after stock Streamline unwraps the native D3D12 device.

    On Vulkan, APUSR passes DLSS-SR and unrelated features into stock slInit but
    excludes Reflex, DLSS-G, and unsupported DLSS-RR. This prevents NVIDIA's
    optional Vulkan plugins from competing with APUSR's Reflex/presentation
    implementations. Stock PCL remains an optional backing marker path.

  apusr_vulkan_host_capture.dll
    Process-lifetime Vulkan loader/device/queue capture used by APUSR's native
    Vulkan backend. It is installed before the real VkDevice is created.

  apusr_fsr4_int8_vulkan.shaderpack
    Backend-owned Vulkan product shader asset using accelerated packed INT8 DOT4.

Backend behavior:
  D3D12:
    The stock sl.interposer.dll, sl.common.dll and sl.dlss.dll retain loader and
    SR semantic ownership. DXGI/NVAPI make normal DLSS discovery succeed. If
    stock common cannot load an unsigned replacement through NVIDIA's signed
    NGX-core loader, APUSR injects only the common NGXContext at startup.
    Stock sl.dlss still owns tags/options/transitions/evaluation and calls APUSR
    through that context. If Reflex is requested, the early slInit bootstrap loads
    apusr_streamline_attach.dll only for the Reflex/PCL -> AMD Anti-Lag 2
    translation; DLSS-SR remains stock-owned.

  Vulkan:
    APUSR must attach before stock slInit can reject the Vulkan feature graph.
    If an already-visible DXGI/loader boundary can intercept the host's named
    slInit lookup, the normal cold bootstrap is sufficient. If no APUSR module
    can be loaded before that call, activate the staged loader-only bootstrap.
    In both cases the backing implementation is the game's exact stock
    interposer: stock Streamline owns tags/options/SR translation, APUSR
    sanitizes DLSS Vulkan device requirements, captures the real Vulkan
    device/queues, and receives SR evaluation at NGX. Reflex and the constrained
    APUSR-owned DLSS-G path are attached separately.

Diagnostics:
  Logging is OFF by default.

    APUSR_HOST_DISCOVERY_LOG=1
    APUSR_NGX_TRACE=1       (sampled hot calls)
    APUSR_NGX_TRACE=full    (every NGX call; short debugging runs only)
    APUSR_LOG_DIR=<writable directory>

  A successful attach emits a Streamline attach.installed event containing:

    stock_pe_preserved=1

  When the optional loader bootstrap is active, it additionally reports:

    loader_bootstrap_alias=1

  A stale APUSR replacement interposer is rejected explicitly:

    reason=legacy_apusr_sl_interposer_detected

  The loaded stock Streamline revision is logged only when it is useful for
  diagnosis.
