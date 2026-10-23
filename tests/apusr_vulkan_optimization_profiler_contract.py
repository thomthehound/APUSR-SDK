# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = (ROOT / "src/frontends/vulkan_host_capture/apusr_vulkan_host_capture_win32.cpp").read_text(encoding="utf-8")
ATTACH = (ROOT / "src/frontends/streamline_attach/apusr_streamline_attach.cpp").read_text(encoding="utf-8")
REPORT = (ROOT / "tools/profiler/apusr_profile_report.py").read_text(encoding="utf-8")


def require(text: str, needle: str, message: str) -> None:
    if needle not in text:
        raise SystemExit(message)


for api in (
    "vkCreateShaderModule",
    "vkDestroyShaderModule",
    "vkCreateComputePipelines",
    "vkCreateGraphicsPipelines",
    "vkDestroyPipeline",
    "vkCreatePipelineCache",
    "vkDestroyPipelineCache",
    "vkGetPipelineCacheData",
    "vkMergePipelineCaches",
):
    require(HOST, f'"{api}"', f"missing Vulkan optimization hook for {api}")

require(HOST, '.name = "vulkan.pipeline_activity"', "missing Vulkan pipeline activity profiler stream")
require(HOST, '"vulkan.pipeline.callsite_module"', "missing Vulkan pipeline caller-module attribution")
require(HOST, "hashPipelineBytes", "missing Vulkan shader-module code hashing")
require(HOST, '"vulkan.pipeline.compute.create"', "missing Vulkan compute-pipeline cache attribution")
require(HOST, '"vulkan.pipeline.graphics.create"', "missing Vulkan graphics-pipeline cache attribution")
require(HOST, '"vulkan.pipeline_cache.get_data"', "missing Vulkan pipeline-cache extraction attribution")
require(HOST, '"vulkan.pipeline_cache.merge"', "missing Vulkan pipeline-cache merge attribution")

require(ATTACH, "reflexProfileStream", "missing backend-specific Reflex profiler stream")
require(ATTACH, '.name = "streamline.reflex"', "Reflex state is not on its own semantic stream")
for scope in (
    "reflex.sleep",
    "reflex.pcl.simulation_start",
    "reflex.pcl.render_submit_end",
    "reflex.pcl.present_start",
    "reflex.d3d12.update",
    "reflex.d3d12.mark_render_end",
):
    require(ATTACH, f'"{scope}"', f"missing Reflex/Anti-Lag CPU scope {scope}")
if 'if (const auto stream = dlssgProfileStream()) {\n        if (previousAntiLagMode != antiLagMode)' in ATTACH:
    raise SystemExit("Reflex state is still being recorded on the Vulkan DLSS-G stream")

require(REPORT, 'save("vulkan_pipeline_activity.csv"', "reporter does not emit Vulkan pipeline activity table")
require(REPORT, "Vulkan shader/pipeline/cache activity", "reporter lacks Vulkan optimization summary")
require(REPORT, "Reflex / Anti-Lag conditioned CPU wall time", "reporter lacks Reflex conditioned summary")
