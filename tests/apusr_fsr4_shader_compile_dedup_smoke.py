# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Regression test for product-shader compile canonicalization."""

from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(repo / "tools/model"))
import compile_fsr4_reference_dx12_shaders as shaders  # noqa: E402
import compile_fsr4_reference_vulkan_shaders as vulkan_shaders  # noqa: E402


def main() -> int:
    options = shaders.product_matrix_options(False)
    tasks, entries = shaders.build_embedded_product_tasks(
        list(shaders.PRESET_ENUM), list(shaders.CAPACITY_ENUM), options
    )
    if len(entries) != 18432:
        raise SystemExit(f"unexpected logical shader-entry count: {len(entries)}")
    if len(tasks) != 852:
        raise SystemExit(f"compile canonicalization regressed: expected 852 unique jobs, got {len(tasks)}")

    vulkan_tasks, vulkan_entries = vulkan_shaders.build_pack_plan(
        "dp4a-optimized", list(shaders.PRESET_ENUM), list(shaders.CAPACITY_ENUM), options
    )
    if len(vulkan_entries) != 18432 or len(vulkan_tasks) != 852:
        raise SystemExit(
            "Vulkan product pack must cover the same 18,432 logical shaders with 852 canonical compile jobs; "
            f"got logical={len(vulkan_entries)} compile_jobs={len(vulkan_tasks)}"
        )
    if vulkan_shaders.VECTOR_LOAD_STAGES != (3, 6, 7, 8, 9):
        raise SystemExit("Vulkan aligned vector-load lowering must remain scoped to audited stages 3, 6, 7, 8, and 9")
    if vulkan_shaders.VECTOR_STORE_STAGES != (3, 6, 9):
        raise SystemExit("Vulkan aligned vector-store lowering must remain scoped to audited stages 3, 6, and 9")
    if (vulkan_shaders.VECTOR_INITIALIZER_SRV_REGISTER, vulkan_shaders.VECTOR_SCRATCH_UAV_REGISTER) != (31, 31):
        raise SystemExit("Vulkan aligned vector-load descriptor aliases changed without a runtime contract update")

    perf_jobs = {j.name: j for j in shaders.shader_jobs("performance", "2160")}
    optimized_jobs = {
        j.name: j
        for j in shaders.shader_jobs(
            "performance",
            "2160",
            dp4a_optimized_front_c16_tiled_exact=True,
        )
    }
    reference_rcas = Path("models/fsr4_int8/reference/fsr4_4_0_2_int8/common/rcas.hlsl")
    optimized_rcas = Path("shaders/dx12/optimized/apusr_fsr4_rcas_exposure_hoist.hlsl")
    if perf_jobs["rcas"].source != reference_rcas:
        raise SystemExit("strict/reference shader jobs must retain the imported RCAS source")
    if optimized_jobs["rcas"].source != optimized_rcas:
        raise SystemExit("optimized product shader jobs must select APUSR's exposure-hoisted RCAS source")

    reference_rcas_text = (repo / reference_rcas).read_text(encoding="utf-8")
    optimized_rcas_text = (repo / optimized_rcas).read_text(encoding="utf-8")
    if "RemoveMuLaw(c.rgb) / LoadExposureValue()" not in reference_rcas_text:
        raise SystemExit("imported reference RCAS must retain its original exposure division")
    if "const FfxFloat32 exposureValue = LoadExposureValue();" not in optimized_rcas_text:
        raise SystemExit("optimized RCAS must hoist one exposure load per shader thread")
    if "RemoveMuLaw(c.rgb) / exposureValue" not in optimized_rcas_text:
        raise SystemExit("optimized RCAS must retain divide-by-exposure filter arithmetic")
    if "/ LoadExposureValue()" in optimized_rcas_text or "LoadInverseExposureValue" in optimized_rcas_text:
        raise SystemExit("optimized RCAS must not reintroduce per-pixel exposure loads or explicit reciprocal rewriting")

    debug_jobs = {j.name: j for j in shaders.shader_jobs("performance", "2160", debug_visualize=True)}
    if "debug_view" in perf_jobs or "debug_view" not in debug_jobs:
        raise SystemExit("debug-view shader packaging must follow the debug-visualize dispatch permutation")
    base = shaders.PermutationOptions(False, False, False, False, False, "linear", False)
    depth_only = shaders.PermutationOptions(False, True, False, False, False, "linear", False)
    if shaders.effective_options_for_job(perf_jobs["fsr4_pre"], base) != shaders.effective_options_for_job(perf_jobs["fsr4_pre"], depth_only):
        raise SystemExit("display-resolution MV pre shader must not specialize depth inversion")

    requested_p138 = shaders.PermutationOptions(
        False, False, True, False, False, "linear", False
    )
    if shaders.generation_options(requested_p138, False, False) != [requested_p138]:
        raise SystemExit(
            "single-slice packaged generation must preserve CLI permutation options"
        )

    srgb_no_ae = shaders.PermutationOptions(False, False, True, False, False, "srgb", False)
    srgb_ae = shaders.PermutationOptions(False, False, True, True, False, "srgb", False)
    for name in ("fsr4_pre", "fsr4_post", "rcas"):
        if shaders.effective_options_for_job(perf_jobs[name], srgb_no_ae) != shaders.effective_options_for_job(perf_jobs[name], srgb_ae):
            raise SystemExit(f"{name} must not specialize auto exposure in sRGB mode")

    print("APUSR shader compile dedup: DX12/Vulkan product matrices both map 18,432 logical entries -> 852 unique DXC jobs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
