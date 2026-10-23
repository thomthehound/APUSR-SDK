<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Streamline in-process attach

`apusr_streamline_attach.dll` is the canonical multi-backend Streamline integration boundary.
The attach DLL itself does **not** replace, rename, forward, or impersonate the game's
`sl.interposer.dll`.

Normally APUSR resolves the named exports that exist in the already-loaded canonical stock
interposer and uses Detours to attach only the behavior it owns. A separate optional loader-only
bootstrap may instead pass an explicit handle to the exact stock PE preserved as
`sl_interposer_apusr_stock.dll`; this exists only for hosts whose `slInit` call occurs before any
normal APUSR loader boundary can become visible. Attachment semantics are identical in both
cases. Optional exports such as `slSetTagForFrame` are discovered by name, so no Streamline PE
ordinal/version profile is required by the attach layer.

DLSS Super Resolution remains owned by the stock `sl.dlss` plugin. APUSR enters at the NGX
boundary (`nvngx_dlss.dll`). On Vulkan, the attach layer sanitizes stock DLSS requirements that
otherwise add NVIDIA-only extensions on an AMD device; Vulkan Reflex/DLSS-G remain APUSR-owned
compatibility features and reuse the existing tag/constants/presentation implementation. On D3D12,
the module is loaded only when Reflex is requested and translates Reflex/PCL to AMD Anti-Lag 2 on
the native D3D12 device; it does not take ownership of D3D12 DLSS-SR.

That split ownership begins at Vulkan `slInit`. The attachment forwards DLSS-SR and unrelated
features to stock Streamline, retains stock PCL as an optional backing marker/statistics path, and
removes Reflex, DLSS-G, and unsupported DLSS-RR from the feature list passed to the stock plugin
manager. The host-facing feature APIs remain attached, so APUSR supplies the requested Reflex and
DLSS-G functions without initializing NVIDIA's competing Vulkan plugins. D3D12 `slInit` is not
filtered.

A legacy APUSR replacement `sl.interposer.dll` is not a supported backing module. The attachment
recognizes its private bridge exports and refuses to attach, with an explicit
`legacy_apusr_sl_interposer_detected` diagnostic. Restore the game's stock interposer before using
the current package; APUSR deliberately does not try to synchronize two generations of frontend
state.

The attach module also exposes a private thread-local evaluation-provenance query. The NGX Vulkan frontend uses it only to prove that an evaluation is currently nested inside stock `sl.dlss`; direct/native NGX Vulkan callers are not allowed to claim Streamline-prepared image states. While hooks are installed, APUSR holds one ordinary loader reference to the stock interposer so it cannot be physically unloaded underneath Detours. Hooks are detached after stock `slShutdown` returns and that reference is then released, allowing normal unload/reload without stale patched entry points.

The early renderer bootstrap resolves the attach/capture private ABI during cold initialization. If an APUSR-visible host module is already present before the host resolves `slInit`, the normal named-lookup hook can arm Vulkan attachment. If the first APUSR-visible module load happens only from inside stock `slInit`, that route is causally too late; the optional loader-only proxy establishes the same attachment immediately before entering the preserved stock `slInit`. The NGX Vulkan path remains late recovery, and D3D12 arms the attach layer only for requested Reflex unless the optional loader alias is active for a multi-backend host. Per-frame evaluation uses cached function pointers and does not repeat module lookup, `GetProcAddress`, or attachment locking.
