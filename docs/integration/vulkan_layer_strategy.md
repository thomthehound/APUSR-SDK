<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Vulkan host-capture deployment strategy

Vulkan host capture has two independent requirements:

1. **Capture semantics** — where instance/device creation, physical-device ownership, native feature negotiation, and queue discovery should live.
2. **Bootstrap/discovery** — how an unmodified game causes that capture code to be present before `vkCreateInstance` / `vkCreateDevice`.

A loader-managed Vulkan layer is the preferred long-term answer to the first problem. It is **not**, by itself, a complete answer to the second problem for a drop-in game mod.


## Current implementation: Windows drop-in

The Windows drop-in path executes APUSR's FSR4 INT8 Vulkan backend with accelerated packed DOT4. Its device-capture and execution behavior is the compatibility baseline.

The loader-layer / launcher path remains **future work rather than part of the current package implementation**. When it is taken up, the most attractive use of the layer is specifically an **APUSR launcher**: the launcher can expose/enable the APUSR layer only for the child game process, without registering an always-active system-wide layer and without requiring the game itself to request APUSR. That gives the standards-native layer path a practical per-game bootstrap while preserving the ordinary copy-in drop-in route for games where that remains preferable.

The implementation sequence is:

```text
1. prove drop-in capture + native DOT4 + real upscaler execution in-game
2. retain the proven capture semantics as the reference behavior
3. then implement launcher-activated Vulkan-layer bootstrap over the same semantics
4. only after that consider installed/global deployment modes, if they are useful at all
```

## Why a Vulkan layer is desirable

A normal loader-managed layer participates in Vulkan's own dispatch chain instead of patching the loader's machine code. That gives APUSR a standards-defined place to intercept the cold calls it actually needs:

```text
vkCreateInstance
vkEnumeratePhysicalDevices / vkEnumeratePhysicalDeviceGroups
vkCreateDevice
vkDestroyDevice / vkDestroyInstance
vkGetInstanceProcAddr / vkGetDeviceProcAddr
```

The layer can use the same API-neutral capture core that exists today. Native feature negotiation remains at `vkCreateDevice`; complete queue capture remains immediately after successful device creation. No queue-submit, present, or shader-dispatch interception is required.

This is also the portable shape. Windows and Linux loaders use the same loader/layer chaining model even though discovery/install paths differ.

## Why the layer does not simply replace the Windows bootstrap

An unmodified game normally does not request `VK_LAYER_APUSR_host_capture` in `VkInstanceCreateInfo`.

An explicit layer therefore needs an external activation mechanism. An implicit layer can be loaded automatically, but the Vulkan loader still has to discover its manifest **before instance creation**. On Windows that normally means a registered manifest or an environment-controlled layer search path; on Linux it means the standard implicit-layer directories or a loader search-path environment variable.

Those mechanisms are appropriate for an installed APUSR runtime or an APUSR-aware launcher. They are not equivalent to today's "copy the compatibility DLLs beside the game and launch normally" deployment model. Registering an always-active global implicit layer would also broaden APUSR's scope to unrelated Vulkan applications, which is not an acceptable default.

Therefore the final architecture should support two bootstraps over one capture core:

```text
                         APUSR Vulkan capture core
                       /                           \
      loader-managed Vulkan layer          in-process drop-in bootstrap
      (preferred installed/launcher mode)   (Windows compatibility mode)
                       \                           /
                        instance/device/queue state
```

The Windows compatibility bootstrap may eventually shrink to the minimum needed to activate or hand off to the loader-managed layer, but it cannot be deleted until APUSR has a deployment mechanism that guarantees layer discovery before every target game's first Vulkan instance/device creation.

## Required refactor before the layer can be correct

The current capture core uses one process-wide downstream Vulkan dispatch table because the Detours bootstrap terminates directly at one system loader. A proper Vulkan layer must not assume that. Different `VkInstance` objects may have different layer chains, so the APUSR layer must retain the next-layer dispatch associated with each captured instance/device.

Before promoting the layer to production, the capture core should therefore move to:

```text
InstanceRecord
    requested API version
    next vkGetInstanceProcAddr
    instance-level downstream dispatch

DeviceRecord
    owning instance / physical device
    next vkGetDeviceProcAddr
    device-level downstream dispatch
    negotiated native feature flags
    complete created-queue set
```

That refactor is architectural, not diagnostic scaffolding. It is the prerequisite for a standards-correct layer and should be shared by both Windows and Linux implementations.

## Planned deployment modes

### Windows drop-in package

Keep a process-lifetime bootstrap loaded by the frontend that the game already discovers (Streamline/NGX/etc.). It must exist before device creation. Today this is the Detours host-capture DLL. After the layer path is implemented and proven, the bootstrap can be reduced to the narrowest mechanism that causes the APUSR layer to enter Vulkan's loader chain.

### Windows installed/launcher mode

Install or expose the APUSR layer manifest to the Vulkan loader and enable it only for processes launched with APUSR. This mode should not require loader detouring.

### Linux

Use the same layer implementation and capture core with the platform's standard Vulkan layer discovery mechanism. No Windows Detours dependency belongs in the Linux path.

## Non-goals

The layer is not a reason to add:

- per-game executable-name branches;
- global AMD-to-NVIDIA Vulkan identity spoofing;
- `vkQueueSubmit` / `vkQueuePresentKHR` interception just to find queues;
- a scalar/portable replacement for native DOT4;
- duplicated Streamline-owned device state.

Native accelerated packed INT8 dot-product remains a hard device-creation requirement for the current FSR4 INT8 Vulkan pipeline.

## Primary loader references

- Khronos Vulkan Loader: `https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md`
- Khronos Vulkan Loader: `https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderApplicationInterface.md`

These define loader/layer negotiation, `VK_LAYER_LINK_INFO`, explicit vs. implicit layer activation, manifest discovery, and the Windows/Linux layer search mechanisms.
