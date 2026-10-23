# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Keep opt-in compatibility diagnostics useful without turning them into per-frame logs."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NGX = (ROOT / "src/frontends/ngx_dlss_dx12/apusr_ngx_dlss_dx12_frontend.cpp").read_text(encoding="utf-8")
IDENTITY = (ROOT / "src/frontends/identity_spoof/apusr_identity_spoof_hooks.cpp").read_text(encoding="utf-8")
README = (ROOT / "README.md").read_text(encoding="utf-8")


def function_body(source: str, signature: str, next_signature: str) -> str:
    end = source.find(next_signature)
    assert end >= 0, f"missing {next_signature}"
    start = source.rfind(signature, 0, end)
    assert start >= 0, f"missing {signature}"
    assert end > start, f"missing boundary after {signature}"
    return source[start:end]


def main() -> int:
    d3d = function_body(
        NGX,
        "NVSDK_NGX_D3D12_EvaluateFeature(",
        "NVSDK_NGX_D3D12_EvaluateFeature_C(",
    )
    vk = function_body(
        NGX,
        "NVSDK_NGX_VULKAN_EvaluateFeature(",
        "NVSDK_NGX_VULKAN_EvaluateFeature_C(",
    )
    assert 'ngxTraceHot("NVSDK_NGX_D3D12_EvaluateFeature"' in d3d
    assert 'ngxTrace("NVSDK_NGX_D3D12_EvaluateFeature")' not in d3d
    assert 'ngxTraceHot("NVSDK_NGX_VULKAN_EvaluateFeature"' in vk
    assert 'ngxTrace("NVSDK_NGX_VULKAN_EvaluateFeature")' not in vk
    assert 'std::strcmp(value, "full") == 0' in NGX
    assert "APUSR_NGX_TRACE=full" in README

    # Streamline per-frame capture remains diagnostic, but at a sparse cadence.
    for needle in (
        "apusrShouldLogPeriodic(g_apusrConstantsLogCounter, 4, 1200)",
        "apusrShouldLogPeriodic(g_apusrSetOptionsLogCounter, 4, 1200)",
        "apusrShouldLogPeriodic(g_apusrTagLogCounter, 4, 1200)",
        "apusrShouldLogPeriodic(g_apusrEvaluateResultLogCounter, 8, 1200)",
    ):
        assert needle in IDENTITY, f"diagnostic sampling contract drift: {needle}"

    eval_start = IDENTITY.rfind("int __cdecl apusrCoreSlEvaluateFeature(")
    eval_end = IDENTITY.find("int __cdecl apusrCoreSlAllocateResources(", eval_start)
    assert eval_start >= 0 and eval_end > eval_start
    eval_body = IDENTITY[eval_start:eval_end]
    assert "if (feature != kFeatureDLSS)" in eval_body
    assert '"core.evaluate.called"' in eval_body
    assert "g_apusrLoggedPluginFunctions.emplace(functionName).second" in IDENTITY

    print("APUSR logging-volume contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
