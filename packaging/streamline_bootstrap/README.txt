SPDX-License-Identifier: Apache-2.0
Copyright (c) 2026 thomthehound.

APUSR optional early Streamline bootstrap

Use this only when the host enters stock slInit before any normal APUSR loader
boundary becomes visible. The shim does not implement Streamline semantics.
It preserves the game's exact stock interposer bytes as:

  sl_interposer_apusr_stock.dll

and installs APUSR's loader-only proxy as sl.interposer.dll. Every export except
slInit is a PE forwarder directly to the preserved stock module, with the same
name and ordinal. slInit only establishes APUSR's private attachment early enough
for Vulkan device/feature ownership setup, then enters the same stock slInit via
the attachment trampoline.

Install from either PowerShell or cmd.exe:

  .\install_apusr_streamline_bootstrap.bat "<game directory>"

Restore the original loader boundary:

  .\install_apusr_streamline_bootstrap.bat "<game directory>" /restore

The script is deliberately fail-closed: it will not overwrite an unrelated
sl.interposer.dll or an ambiguous pre-existing sidecar.
