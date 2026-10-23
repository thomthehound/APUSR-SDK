// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

// The nvngx.dll compatibility name is a PE export-forwarder to nvngx_dlss.dll.
// Keep implementation/state in one module; this translation unit only gives
// the forwarding DLL a native link unit.
extern "C" void apusrNvngxAliasForwarderAnchor() noexcept {}
