<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Microsoft Detours

APUSR's Windows Vulkan bootstrap uses Microsoft Detours 4.0.1 to install a process-lifetime interception boundary on the real system Vulkan loader. The headers and x64 static library in this directory correspond to Microsoft Detours 4.0.1 and retain the upstream MIT license.

Detours is used only by the Windows bootstrap in `src/frontends/vulkan_host_capture/apusr_vulkan_host_capture_win32.cpp`. The capture state/feature-negotiation core is separate and does not depend on Detours, so a future Windows/Linux Vulkan-layer bootstrap can reuse the same core without carrying this dependency.

Upstream: Microsoft Detours 4.0.1. License: MIT; see `LICENSE.md`.
