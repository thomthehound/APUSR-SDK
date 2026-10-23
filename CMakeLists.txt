# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

cmake_minimum_required(VERSION 3.24)

project(apusr LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

# APUSR host code is C++23. Keep the requirement target-scoped so embedding
# projects do not inherit a directory-global language standard.
add_library(apusr_host_cxx_contract INTERFACE)
add_library(apusr::host_cxx_contract ALIAS apusr_host_cxx_contract)
target_compile_features(apusr_host_cxx_contract INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(apusr_host_cxx_contract INTERFACE /Zc:__cplusplus)
endif()

function(apusr_apply_host_cxx_contract target_name)
    target_link_libraries(${target_name} PUBLIC apusr::host_cxx_contract)
    if(WIN32)
        # Keep the Windows SDK min/max macros out of APUSR host C++ without
        # imposing those definitions on projects that consume APUSR targets.
        target_compile_definitions(${target_name} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
endfunction()

option(APUSR_BUILD_TESTS "Build APUSR behavior/regression tests" OFF)
option(APUSR_BUILD_FRONTENDS "Build deployable APUSR frontend shared libraries" ON)
set(APUSR_PRODUCT_TEST_TIMEOUT_SECONDS 15)
set(APUSR_SHADER_TEST_TIMEOUT_SECONDS 60)
set(APUSR_SHADER_COMPILER_CHILD_TIMEOUT_SECONDS 120)
set(APUSR_SHADER_COMPILE_JOBS 0 CACHE STRING "Concurrent shader compiler jobs; 0 selects automatically")
set(APUSR_VULKAN_DOT_ACCUMULATE_LOWERING "bounded-fused" CACHE STRING "Vulkan signed DOT4 accumulate lowering: bounded-fused product path or split A/B baseline")
set_property(CACHE APUSR_VULKAN_DOT_ACCUMULATE_LOWERING PROPERTY STRINGS bounded-fused split)
if(NOT (APUSR_VULKAN_DOT_ACCUMULATE_LOWERING STREQUAL "bounded-fused" OR APUSR_VULKAN_DOT_ACCUMULATE_LOWERING STREQUAL "split"))
    message(FATAL_ERROR "APUSR_VULKAN_DOT_ACCUMULATE_LOWERING must be bounded-fused or split")
endif()
if(APUSR_VULKAN_DOT_ACCUMULATE_LOWERING STREQUAL "bounded-fused")
    set(APUSR_VULKAN_FSR4_REQUIRES_PACKED_SIGNED_ACC_SAT 1)
else()
    set(APUSR_VULKAN_FSR4_REQUIRES_PACKED_SIGNED_ACC_SAT 0)
endif()
option(APUSR_ENABLE_DX12_BACKEND "Build the Direct3D 12 graphics backend seam" ON)
option(APUSR_ENABLE_VULKAN_BACKEND "Build the Vulkan graphics backend seam" ON)
option(APUSR_ENABLE_XRT_BACKEND "Build the AMD XRT NPU backend seam" OFF)
set(XRT_ROOT "" CACHE PATH "Path to an XRT install tree with include/ and xrt_coreutil import library")
set(XRT_DLL_DIR "" CACHE PATH "Directory containing signed XRT runtime DLLs; defaults to XRT_ROOT/bin, XRT_ROOT, or System32 on Windows")
option(APUSR_ENABLE_HOT_PATH_GUARDS "Enable extra runtime invariant checks in per-token hot paths" OFF)
option(APUSR_ENABLE_PERF_TELEMETRY "Collect developer hot-path submission timing counters" OFF)
option(APUSR_ENABLE_PROFILER "Compile the bounded APUSR cross-backend flight-recorder profiler" ON)
option(APUSR_ENABLE_DX12_TRACE "Write APUSR-owned DX12 export/import trace logs for game bring-up" OFF)
option(APUSR_ENABLE_GPU_DEBUG_MARKERS "Compile opt-in DX12 GPU debug markers for PIX/RGP profiling builds. OFF compiles marker code out of the product hot path." OFF)
set(APUSR_WINPIX_EVENT_RUNTIME_ROOT "" CACHE PATH "Optional Microsoft.WinPixEventRuntime package root override. Usually not needed if the package is restored to the normal NuGet global packages cache or WINPIX_EVENT_RUNTIME_ROOT/NUGET_PACKAGES is set.")
set(APUSR_WINPIX_EVENT_RUNTIME_LICENSE_FILE "" CACHE FILEPATH "Optional exact WinPixEventRuntime package license-file override. Profiling builds that redistribute WinPixEventRuntime.dll require the MIT license belonging to the selected package.")
set(APUSR_D3D12_AGILITY_SDK_ROOT "" CACHE PATH "Optional Microsoft.Direct3D.D3D12 / DirectX 12 Agility SDK package root override. Usually not needed if the package is restored to the normal NuGet global packages cache or D3D12_AGILITY_SDK_ROOT/NUGET_PACKAGES is set.")
option(APUSR_ENABLE_DEBUG_HALF4_SIDECARS "Use R16G16B16A16_FLOAT async bookend sidecars for debug/parity testing; packed R32_UINT remains the product default" OFF)
option(APUSR_BUILD_AIE_KERNELS "Build repo-owned mlir-aie kernels and xclbins" OFF)

option(APUSR_BUILD_FRAME_CONVEYOR_BENCHMARK "Build the standalone XRT-owned BO frame-conveyor bandwidth benchmark" OFF)
set(APUSR_FRAME_CONVEYOR_BENCHMARK_FRAME_BYTES "19906560" CACHE STRING "Semantic payload bytes for the standalone frame-conveyor bandwidth benchmark")
set(APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMNS "4" CACHE STRING "Logical shim-DMA column count for the standalone frame-conveyor bandwidth benchmark")
set(APUSR_FRAME_CONVEYOR_BENCHMARK_CHANNELS "2" CACHE STRING "DMA stream lanes per selected benchmark column")
set(APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMN_MAP "" CACHE STRING "Optional comma-separated physical AIE column map for the standalone frame-conveyor benchmark")
option(APUSR_REQUIRE_PACKAGED_NATIVE "Require the FSR4-compatible DX12 frontend to carry a packaged native NPU artifact and forbid fake output paths" OFF)
set(APUSR_AIE_TARGET "npu2" CACHE STRING "APUSR AIE kernel build target name")
set(APUSR_AIE_KERNEL_PROFILE "MiddleCoreStage1To12Spatial4Col" CACHE STRING "Product AIE kernel profile. Spatial4Col is canonical for producer-ring placement; Spatial2Col remains diagnostic. The old full-arena two-column graph is quarantined.")
set_property(CACHE APUSR_AIE_KERNEL_PROFILE PROPERTY STRINGS MiddleCoreStage1To12Spatial2Col MiddleCoreStage1To12Spatial4Col)
set(APUSR_AIE_GRAPH_PHASE "spatial-core" CACHE STRING "AIE graph selection. spatial-core is the current product-shaped spine; stage1-resident, stage2-resident, stage2-plane-window-resident, stage1_2-linked-resident, stage1_2-fused-overlap-resident, stage1_2-worker-ring-resident, stage1_4-worker-ring-resident, stage1_2-to-stage3_4-worker-ring-resident, stage3-down-resident, stage3_4-fused-resident, and stage3_4-product-resident are resident primitive proofs; front-c16-row, cell-conveyor, and c16-halo are diagnostics.")
set_property(CACHE APUSR_AIE_GRAPH_PHASE PROPERTY STRINGS spatial-core stage1-resident stage2-resident stage2-plane-window-resident stage1_2-linked-resident stage1_2-fused-overlap-resident stage1_2-worker-ring-resident stage1_4-worker-ring-resident stage1_2-to-stage3_4-worker-ring-resident stage1_2-to-stage3-paired-macroblock-resident stage3-down-resident stage3_4-fused-resident stage3_4-product-resident front-c16-row cell-conveyor c16-halo "")
if(APUSR_AIE_KERNEL_PROFILE STREQUAL "MiddleCoreStage1To12Spatial2Col")
    set(_apusr_middle_core_macro_lane_columns 2)
elseif(APUSR_AIE_KERNEL_PROFILE STREQUAL "MiddleCoreStage1To12Spatial4Col")
    set(_apusr_middle_core_macro_lane_columns 4)
elseif(APUSR_AIE_KERNEL_PROFILE STREQUAL "MiddleCoreStage1To12FullArena")
    message(FATAL_ERROR "MiddleCoreStage1To12FullArena is quarantined as a failed package-FIFO experiment. Use MiddleCoreStage1To12Spatial4Col for product work or MiddleCoreStage1To12Spatial2Col for diagnostics.")
else()
    message(FATAL_ERROR "Unsupported APUSR_AIE_KERNEL_PROFILE='${APUSR_AIE_KERNEL_PROFILE}'.")
endif()
set(APUSR_MIDDLE_CORE_FRAME_CAPACITY "2160" CACHE STRING "FSR4 model-family frame-capacity class compiled into the Stage1->12 AIE graph; this is not the user's active resolution or quality preset")
set_property(CACHE APUSR_MIDDLE_CORE_FRAME_CAPACITY PROPERTY STRINGS 1080 2160 4320)
if(NOT (APUSR_MIDDLE_CORE_FRAME_CAPACITY STREQUAL "1080" OR APUSR_MIDDLE_CORE_FRAME_CAPACITY STREQUAL "2160" OR APUSR_MIDDLE_CORE_FRAME_CAPACITY STREQUAL "4320"))
    message(FATAL_ERROR "APUSR_MIDDLE_CORE_FRAME_CAPACITY must be one of 1080, 2160, or 4320")
endif()
set(APUSR_FRAME_CONVEYOR_FRAME_BYTES "32768" CACHE STRING "Semantic C16 seam byte count for the frame-conveyor transport slot; transport capacity is aligned up to 4096-byte chunks")
set(APUSR_FRAME_CONVEYOR_COLUMNS "4" CACHE STRING "Frame-conveyor shim-DMA column count for packaged AIE frame transport")
set(APUSR_FRAME_CONVEYOR_CHANNELS "2" CACHE STRING "Frame-conveyor DMA stream lanes per selected column")
set(APUSR_MIDDLE_CORE_WORKER_GROUPS "auto" CACHE STRING "Independent middle-core worker-group count, or auto to fill the selected target with the selected worker width")
set_property(CACHE APUSR_MIDDLE_CORE_WORKER_GROUPS PROPERTY STRINGS auto 1 2 4)
if(NOT (APUSR_MIDDLE_CORE_WORKER_GROUPS STREQUAL "auto" OR APUSR_MIDDLE_CORE_WORKER_GROUPS STREQUAL "1" OR APUSR_MIDDLE_CORE_WORKER_GROUPS STREQUAL "2" OR APUSR_MIDDLE_CORE_WORKER_GROUPS STREQUAL "4"))
    message(FATAL_ERROR "APUSR_MIDDLE_CORE_WORKER_GROUPS must be auto, 1, 2, or 4.")
endif()
set(_apusr_middle_core_target_columns 8)
if(APUSR_AIE_TARGET STREQUAL "npu" OR APUSR_AIE_TARGET STREQUAL "npu1")
    set(_apusr_middle_core_target_columns 4)
endif()
if(APUSR_MIDDLE_CORE_WORKER_GROUPS STREQUAL "auto")
    math(EXPR APUSR_EFFECTIVE_MIDDLE_CORE_WORKER_GROUPS "${_apusr_middle_core_target_columns} / ${_apusr_middle_core_macro_lane_columns}")
else()
    set(APUSR_EFFECTIVE_MIDDLE_CORE_WORKER_GROUPS "${APUSR_MIDDLE_CORE_WORKER_GROUPS}")
endif()
math(EXPR _apusr_middle_core_required_columns "${APUSR_EFFECTIVE_MIDDLE_CORE_WORKER_GROUPS} * ${_apusr_middle_core_macro_lane_columns}")
if(_apusr_middle_core_required_columns GREATER _apusr_middle_core_target_columns)
    message(FATAL_ERROR "${APUSR_AIE_KERNEL_PROFILE} with ${APUSR_EFFECTIVE_MIDDLE_CORE_WORKER_GROUPS} worker group(s) requires ${_apusr_middle_core_required_columns} columns, but ${APUSR_AIE_TARGET} exposes ${_apusr_middle_core_target_columns}.")
endif()
set(APUSR_AIECC "" CACHE FILEPATH "Optional explicit path to the mlir-aie aiecc driver; active ironenv PATH is used when empty")
set(APUSR_PEANO_INSTALL_DIR "" CACHE PATH "Optional explicit Peano/llvm-aie install prefix; active mlir-aie/IRON environment is used when empty")
set(APUSR_AIE_BUILD_COMMAND "" CACHE STRING "Developer override command for AIE kernel builds; normal builds use the repo-owned mlir-aie driver")
# Product frontends always use the distribution DP4a path. Strict-reference and
# tuned-candidate graphs remain available to API-neutral tests/planning code,
# but they are not separate product build configurations.
set(APUSR_FSR4_REFERENCE_DX12_SHADER_VARIANT_ARGS --dp4a-optimized-front-c16-tiled-exact)
set(APUSR_FSR4_DX12_EMBED_XCLBIN "" CACHE FILEPATH "XCLBIN file to embed in the FSR4-compatible DX12 frontend DLL")
set(APUSR_FSR4_DX12_EMBED_INSTRUCTIONS "" CACHE FILEPATH "Hot-path MLIR-AIE insts.bin file to embed in the FSR4-compatible DX12 frontend DLL")
set(APUSR_FSR4_DX12_EMBED_PARAMETER_LOAD_INSTRUCTIONS "" CACHE FILEPATH "Model-epoch parameter-load MLIR-AIE insts.bin file to embed in the FSR4-compatible DX12 frontend DLL")
set(APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETERS "" CACHE FILEPATH "Single resident parameter arena binary to embed for explicit diagnostics")
set(APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETER_SET "" CACHE FILEPATH "Resident parameter arena set manifest to embed in the FSR4-compatible DX12 frontend DLL")
set(APUSR_FSR4_DX12_KERNEL_NAME "MLIR_AIE" CACHE STRING "Compiled-in XRT kernel name for mlir-aie packaged DPU artifacts")
set(APUSR_FSR4_DX12_MODEL_PERMUTATION "auto" CACHE STRING "Diagnostic single-arena FSR4 INT8 override. Product paths select model quality/resolution permutations from the universal set at runtime.")
set(APUSR_FSR4_DX12_PARAMETER_PROFILE "am020_4x4_base" CACHE STRING "NPU parameter package profile used for generated resident middle-core parameters")
set(APUSR_FSR4_REFERENCE_DX12_PACKAGED_SHADER_CACHE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/fsr4_reference_dx12_shader_cache")
find_program(Python3_EXECUTABLE NAMES python3 python REQUIRED)

if(WIN32)
    set(APUSR_DX12_SHADER_COMPILER_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_compiler/windows/bin/dxc.exe"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_compiler/windows/bin/dxcompiler.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_compiler/windows/bin/dxil.dll")
else()
    set(APUSR_DX12_SHADER_COMPILER_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_compiler/linux/bin/dxc"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_compiler/linux/bin/libdxcompiler.so"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_compiler/linux/bin/libdxil.so")
endif()

set(APUSR_FSR4_DX12_OPTIMIZED_SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dx12/optimized/apusr_fsr4_i8_pass1_tiled_exact.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dx12/optimized/apusr_fsr4_i8_pass2_tiled_exact.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dx12/optimized/apusr_fsr4_rcas_exposure_hoist.hlsl")
set(APUSR_FSR4_DX12_REFERENCE_SHADER_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/models/fsr4_int8/reference/fsr4_4_0_2_int8/include/fsr4/pre_common.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/models/fsr4_int8/reference/fsr4_4_0_2_int8/include/fsr4/post_common.hlsli")

set(APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/include/apusr/generated/apusr_fsr4_reference_dx12_shader_blobs.h")
set(APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/apusr_fsr4_reference_dx12_shader_blobs.cpp")
set(APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_HEADER "${APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_HEADER}")
set(APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_SOURCE "${APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_SOURCE}")
set(APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_INCLUDE_DIR "")
if(WIN32)
    set(APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_INCLUDE_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/fsr4_reference_dx12_product/include")
    set(APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_HEADER
        "${APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_INCLUDE_DIR}/apusr/generated/apusr_fsr4_reference_dx12_shader_blobs.h")
    set(APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_SOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/fsr4_reference_dx12_product/src/apusr_fsr4_reference_dx12_shader_blobs.cpp")
    add_custom_command(
        OUTPUT
            "${APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_HEADER}"
            "${APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_SOURCE}"
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/compile_fsr4_reference_dx12_shaders.py"
            --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --preset all
            --capacity all
            --product-matrix
            --header "${APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_HEADER}"
            --source "${APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_SOURCE}"
            --shader-timeout-seconds "${APUSR_SHADER_COMPILER_CHILD_TIMEOUT_SECONDS}"
            --jobs "${APUSR_SHADER_COMPILE_JOBS}"
            ${APUSR_FSR4_REFERENCE_DX12_SHADER_VARIANT_ARGS}
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/compile_fsr4_reference_dx12_shaders.py"
            ${APUSR_FSR4_DX12_OPTIMIZED_SHADER_SOURCES}
            ${APUSR_FSR4_DX12_REFERENCE_SHADER_DEPENDS}
            ${APUSR_DX12_SHADER_COMPILER_FILES}
        USES_TERMINAL
        VERBATIM
    )
    set_source_files_properties("${APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_SOURCE}" PROPERTIES GENERATED TRUE)
endif()

set(APUSR_FSR4_VULKAN_SHADER_PACK "")
set(APUSR_FSR4_VULKAN_SHADER_PACK_MANIFEST "")
if(WIN32 AND APUSR_ENABLE_VULKAN_BACKEND)
    set(APUSR_FSR4_VULKAN_SHADER_PACK_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/fsr4_vulkan_product")
    file(MAKE_DIRECTORY "${APUSR_FSR4_VULKAN_SHADER_PACK_DIR}")
    set(APUSR_FSR4_VULKAN_SHADER_PACK "${APUSR_FSR4_VULKAN_SHADER_PACK_DIR}/fsr4_int8_vulkan.shaderpack")
    set(APUSR_FSR4_VULKAN_SHADER_PACK_MANIFEST "${APUSR_FSR4_VULKAN_SHADER_PACK_DIR}/fsr4_int8_vulkan.shaderpack.json")

    file(GLOB_RECURSE APUSR_FSR4_VULKAN_SHADER_INPUTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/models/fsr4_int8/reference/fsr4_4_0_2_int8/*.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/models/fsr4_int8/reference/fsr4_4_0_2_int8/*.hlsli")
    list(APPEND APUSR_FSR4_VULKAN_SHADER_INPUTS
        ${APUSR_FSR4_DX12_OPTIMIZED_SHADER_SOURCES})

    add_custom_command(
        OUTPUT
            "${APUSR_FSR4_VULKAN_SHADER_PACK}"
            "${APUSR_FSR4_VULKAN_SHADER_PACK_MANIFEST}"
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/compile_fsr4_reference_vulkan_shaders.py"
            --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --preset all
            --capacity all
            --product-matrix
            --pipeline-variant dp4a-optimized
            --dot-accumulate-lowering "${APUSR_VULKAN_DOT_ACCUMULATE_LOWERING}"
            --output-pack "${APUSR_FSR4_VULKAN_SHADER_PACK}"
            --manifest "${APUSR_FSR4_VULKAN_SHADER_PACK_MANIFEST}"
            --shader-timeout-seconds "${APUSR_SHADER_COMPILER_CHILD_TIMEOUT_SECONDS}"
            --jobs "${APUSR_SHADER_COMPILE_JOBS}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/compile_fsr4_reference_vulkan_shaders.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/spirv_dot_accumulate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/compile_fsr4_reference_dx12_shaders.py"
            ${APUSR_FSR4_VULKAN_SHADER_INPUTS}
            ${APUSR_DX12_SHADER_COMPILER_FILES}
        USES_TERMINAL
        VERBATIM
    )
    add_custom_target(apusr_generate_fsr4_vulkan_shader_pack
        DEPENDS "${APUSR_FSR4_VULKAN_SHADER_PACK}" "${APUSR_FSR4_VULKAN_SHADER_PACK_MANIFEST}")

endif()

if(WIN32 AND APUSR_AIE_BUILD_COMMAND)
    string(TOLOWER "${APUSR_AIE_BUILD_COMMAND}" _apusr_aie_build_command_lower)
    if(_apusr_aie_build_command_lower MATCHES "xchess|xbridge")
        message(FATAL_ERROR "APUSR_AIE_BUILD_COMMAND mentions Chess/xbridge on Windows. Windows AIE builds must use Peano only.")
    endif()
endif()

add_library(apusr_profiler_runtime STATIC
    src/core/apusr_profiler.cpp
)
apusr_apply_host_cxx_contract(apusr_profiler_runtime)
set_target_properties(apusr_profiler_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(apusr_profiler_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(APUSR_ENABLE_PROFILER)
    target_compile_definitions(apusr_profiler_runtime PUBLIC APUSR_ENABLE_PROFILER=1)
else()
    target_compile_definitions(apusr_profiler_runtime PUBLIC APUSR_ENABLE_PROFILER=0)
endif()

add_library(apusr_core STATIC
    src/core/apusr_backend_names.cpp
    src/core/apusr_context.cpp
    src/core/apusr_frame_scheduler.cpp
    src/core/apusr_frontend.cpp
    src/core/apusr_frame_desc.cpp
    src/core/apusr_diagnostics.cpp
    src/core/apusr_execution_plan.cpp
    src/core/apusr_manifest.cpp
    src/core/apusr_protocol.cpp
    src/core/apusr_pipeline.cpp
    src/core/apusr_token_layout.cpp
    src/core/apusr_token_planner.cpp
    src/core/apusr_upscaler_common.cpp
    src/models/fsr4_int8/apusr_fsr4_int8_graph.cpp
    src/models/fsr4_int8/apusr_fsr4_int8_tiled_fusion.cpp
    src/models/fsr4_int8/apusr_fsr4_int8_scratch_layout.cpp
    src/models/fsr4_int8/apusr_fsr4_int8_resources.cpp
    src/models/fsr4_int8/apusr_fsr4_int8_reference_runner.cpp
    src/backends/gfx_dx12/apusr_fsr4_reference_dx12.cpp
    "${APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_SOURCE}"
    src/generated/apusr_fsr4_reference_initializers.cpp
    src/generated/apusr_fsr4_reference_pass0_weights.cpp
    src/frontends/common/apusr_host_capture.cpp
    src/frontends/common_dx12/apusr_dx12_queue_bridge.cpp
    src/adapters/ffx_fsr4/apusr_ffx_fsr4_adapter.cpp
    src/adapters/ffx_fsr4/apusr_fsr4_boundary.cpp
)

add_library(apusr::core ALIAS apusr_core)
apusr_apply_host_cxx_contract(apusr_core)
target_link_libraries(apusr_core PUBLIC apusr_profiler_runtime)
set_target_properties(apusr_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(APUSR_ENABLE_VULKAN_BACKEND)
    target_sources(apusr_core PRIVATE src/backends/gfx_vulkan/apusr_fsr4_reference_vulkan.cpp)

    # Frame Generation is backend-abstracted separately from the FSR4 SR core.
    # Keep this SDK-free Vulkan compute backend linkable by both compatibility
    # frontends without pulling DX12 presentation code into the target.
    add_library(apusr_framegen_vk_backend STATIC
        src/framegen/ffx_vk/apusr_framegen_backend_vk.cpp
        src/framegen/ffx_vk/generated/apusr_framegen_vk_spirv.cpp
    )
    apusr_apply_host_cxx_contract(apusr_framegen_vk_backend)
    target_link_libraries(apusr_framegen_vk_backend PUBLIC apusr_profiler_runtime)
    set_target_properties(apusr_framegen_vk_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(apusr_framegen_vk_backend
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/ffx_vk"
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/ffx_vk/generated"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm/internal"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm/generated"
    )

    # The FI/OF scheduler is backend-neutral. Build one native algorithm library
    # for the Vulkan runtime rather than embedding a second algorithm fork.
    add_library(apusr_framegen_algorithm_native STATIC
        src/framegen/algorithm/apusr_framegen_assert.cpp
        src/framegen/algorithm/apusr_framegen_message.cpp
        src/framegen/algorithm/apusr_frameinterpolation.cpp
        src/framegen/algorithm/apusr_frameinterpolation_shaderblobs.cpp
        src/framegen/algorithm/apusr_opticalflow.cpp
        src/framegen/algorithm/apusr_opticalflow_shaderblobs.cpp
        src/framegen/algorithm/generated/apusr_framegen_dxil.cpp
        src/framegen/ffx_vk/apusr_framegen_resource_size_vk.cpp
    )
    apusr_apply_host_cxx_contract(apusr_framegen_algorithm_native)
    set_target_properties(apusr_framegen_algorithm_native PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(apusr_framegen_algorithm_native
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm/include"
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm/internal"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm/generated"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/ffx_compat/abi"
    )
    target_compile_definitions(apusr_framegen_algorithm_native PRIVATE FFX_DX12_ENABLE_GPU_MARKERS=0)

    add_library(apusr_framegen_vk_runtime STATIC
        src/framegen/ffx_vk/apusr_framegen_session_vk.cpp
        src/framegen/ffx_vk/apusr_framegen_streamline_vk.cpp
        src/framegen/ffx_vk/apusr_framegen_present_resources_vk.cpp
        src/framegen/ffx_vk/apusr_framegen_present_images_vk.cpp
        src/framegen/ffx_vk/apusr_framegen_present_sync_vk.cpp
        src/framegen/ffx_vk/apusr_framegen_direct_wsi_vk.cpp
        src/framegen/ffx_vk/apusr_framegen_source_replay_vk.cpp
        src/framegen/ffx_vk/apusr_framegen_presentation_vk.cpp
    )
    apusr_apply_host_cxx_contract(apusr_framegen_vk_runtime)
    set_target_properties(apusr_framegen_vk_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(apusr_framegen_vk_runtime
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/ffx_vk"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm/include"
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm/internal"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm/generated"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/ffx_compat/abi"
    )
    target_link_libraries(apusr_framegen_vk_runtime PUBLIC
        apusr_framegen_vk_backend
        apusr_framegen_algorithm_native)
endif()

target_include_directories(apusr_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
if(APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_INCLUDE_DIR)
    target_include_directories(apusr_core BEFORE PUBLIC
        "$<BUILD_INTERFACE:${APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_TARGET_INCLUDE_DIR}>"
    )
endif()
if(WIN32)
    target_link_libraries(apusr_core PUBLIC d3d12 dxgi)
elseif(APUSR_ENABLE_VULKAN_BACKEND)
    target_link_libraries(apusr_core PRIVATE dl)
endif()
if(APUSR_ENABLE_VULKAN_BACKEND)
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_VULKAN_BACKEND=1)
else()
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_VULKAN_BACKEND=0)
endif()

target_compile_definitions(apusr_core PRIVATE APUSR_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")



if(APUSR_ENABLE_HOT_PATH_GUARDS)
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_HOT_PATH_GUARDS=1)
else()
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_HOT_PATH_GUARDS=0)
endif()

if(APUSR_ENABLE_DX12_TRACE)
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_DX12_TRACE=1)
else()
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_DX12_TRACE=0)
endif()

if(APUSR_ENABLE_GPU_DEBUG_MARKERS)
    if(NOT WIN32)
        message(FATAL_ERROR "APUSR_ENABLE_GPU_DEBUG_MARKERS=ON requires Windows/DX12 because the RGP marker path uses Microsoft WinPixEventRuntime / pix3.h.")
    endif()

    set(_apusr_winpix_roots "")
    set(_apusr_winpix_package_globs "")

    if(APUSR_WINPIX_EVENT_RUNTIME_ROOT)
        list(APPEND _apusr_winpix_roots "${APUSR_WINPIX_EVENT_RUNTIME_ROOT}")
    endif()
    if(DEFINED ENV{WINPIX_EVENT_RUNTIME_ROOT})
        list(APPEND _apusr_winpix_roots "$ENV{WINPIX_EVENT_RUNTIME_ROOT}")
    endif()

    # Normal NuGet global package cache. This lets a user install/restore the package
    # once and configure APUSR with only -DAPUSR_ENABLE_GPU_DEBUG_MARKERS=ON.
    if(DEFINED ENV{NUGET_PACKAGES})
        list(APPEND _apusr_winpix_package_globs
            "$ENV{NUGET_PACKAGES}/winpixeventruntime/*"
            "$ENV{NUGET_PACKAGES}/WinPixEventRuntime/*"
            "$ENV{NUGET_PACKAGES}/microsoft.winpixeventruntime/*"
            "$ENV{NUGET_PACKAGES}/Microsoft.WinPixEventRuntime/*")
    endif()
    if(DEFINED ENV{USERPROFILE})
        list(APPEND _apusr_winpix_package_globs
            "$ENV{USERPROFILE}/.nuget/packages/winpixeventruntime/*"
            "$ENV{USERPROFILE}/.nuget/packages/WinPixEventRuntime/*"
            "$ENV{USERPROFILE}/.nuget/packages/microsoft.winpixeventruntime/*"
            "$ENV{USERPROFILE}/.nuget/packages/Microsoft.WinPixEventRuntime/*")
    endif()

    # Also support repo-local or adjacent dependency folders for developers who keep
    # unpacked NuGet packages outside the global cache.
    list(APPEND _apusr_winpix_package_globs
        "${CMAKE_SOURCE_DIR}/deps/winpixeventruntime*"
        "${CMAKE_SOURCE_DIR}/deps/WinPixEventRuntime*"
        "${CMAKE_SOURCE_DIR}/external/winpixeventruntime*"
        "${CMAKE_SOURCE_DIR}/external/WinPixEventRuntime*"
        "${CMAKE_SOURCE_DIR}/third_party/winpixeventruntime*"
        "${CMAKE_SOURCE_DIR}/third_party/WinPixEventRuntime*"
        "${CMAKE_SOURCE_DIR}/packages/winpixeventruntime*"
        "${CMAKE_SOURCE_DIR}/packages/WinPixEventRuntime*"
        "${CMAKE_SOURCE_DIR}/../apusr_deps/winpixeventruntime*"
        "${CMAKE_SOURCE_DIR}/../apusr_deps/WinPixEventRuntime*")

    foreach(_apusr_winpix_glob IN LISTS _apusr_winpix_package_globs)
        file(GLOB _apusr_winpix_glob_roots LIST_DIRECTORIES TRUE "${_apusr_winpix_glob}")
        list(APPEND _apusr_winpix_roots ${_apusr_winpix_glob_roots})
    endforeach()
    list(REMOVE_DUPLICATES _apusr_winpix_roots)

    find_path(APUSR_WINPIX_EVENT_RUNTIME_INCLUDE_DIR
        NAMES pix3.h WinPixEventRuntime/pix3.h
        PATHS ${_apusr_winpix_roots}
        PATH_SUFFIXES Include Include/WinPixEventRuntime include include/WinPixEventRuntime
        DOC "Directory containing WinPixEventRuntime pix3.h")
    find_library(APUSR_WINPIX_EVENT_RUNTIME_LIBRARY
        NAMES WinPixEventRuntime
        PATHS ${_apusr_winpix_roots}
        PATH_SUFFIXES bin/x64 lib/x64 Lib/x64
        DOC "WinPixEventRuntime import library")
    find_file(APUSR_WINPIX_EVENT_RUNTIME_DLL
        NAMES WinPixEventRuntime.dll
        PATHS ${_apusr_winpix_roots}
        PATH_SUFFIXES bin/x64
        DOC "WinPixEventRuntime runtime DLL")
    if(NOT APUSR_WINPIX_EVENT_RUNTIME_LICENSE_FILE)
        find_file(APUSR_WINPIX_EVENT_RUNTIME_LICENSE_FILE
            NAMES LICENSE.txt LICENSE LICENSE.md
            PATHS ${_apusr_winpix_roots}
            NO_DEFAULT_PATH
            DOC "License file belonging to the selected WinPixEventRuntime package")
    endif()

    if(NOT APUSR_WINPIX_EVENT_RUNTIME_INCLUDE_DIR OR NOT APUSR_WINPIX_EVENT_RUNTIME_LIBRARY)
        message(FATAL_ERROR
            "APUSR_ENABLE_GPU_DEBUG_MARKERS=ON requires Microsoft WinPixEventRuntime, but CMake could not find pix3.h and/or WinPixEventRuntime.lib. "
            "Install/restore the WinPixEventRuntime NuGet package into the normal NuGet global cache and reconfigure, or set one of: "
            "APUSR_WINPIX_EVENT_RUNTIME_ROOT, WINPIX_EVENT_RUNTIME_ROOT, or NUGET_PACKAGES. "
            "CMake searched common NuGet package locations plus repo-local deps/external/third_party/packages folders. "
            "For normal distribution builds, leave APUSR_ENABLE_GPU_DEBUG_MARKERS=OFF so no PIX/RGP marker dependency is required.")
    endif()

    if(APUSR_WINPIX_EVENT_RUNTIME_DLL)
        if(NOT APUSR_WINPIX_EVENT_RUNTIME_LICENSE_FILE OR NOT EXISTS "${APUSR_WINPIX_EVENT_RUNTIME_LICENSE_FILE}")
            message(FATAL_ERROR
                "APUSR_ENABLE_GPU_DEBUG_MARKERS=ON found WinPixEventRuntime.dll but not the license file belonging to that package. "
                "Use a current MIT-licensed WinPixEventRuntime package or set APUSR_WINPIX_EVENT_RUNTIME_LICENSE_FILE to its exact LICENSE file. "
                "APUSR will not redistribute an externally supplied runtime without its provenance notice.")
        endif()
        file(READ "${APUSR_WINPIX_EVENT_RUNTIME_LICENSE_FILE}" _apusr_winpix_license_text)
        if(NOT _apusr_winpix_license_text MATCHES "Copyright \(c\) Microsoft Corporation\."
           OR NOT _apusr_winpix_license_text MATCHES "MIT License")
            message(FATAL_ERROR
                "The selected WinPixEventRuntime license does not match the audited Microsoft MIT license. "
                "Use a current MIT-licensed package or audit the selected package before redistribution.")
        endif()
    endif()

    set(_apusr_d3d12_agility_roots "")
    set(_apusr_d3d12_agility_package_globs "")

    if(APUSR_D3D12_AGILITY_SDK_ROOT)
        list(APPEND _apusr_d3d12_agility_roots "${APUSR_D3D12_AGILITY_SDK_ROOT}")
    endif()
    if(DEFINED ENV{D3D12_AGILITY_SDK_ROOT})
        list(APPEND _apusr_d3d12_agility_roots "$ENV{D3D12_AGILITY_SDK_ROOT}")
    endif()

    # Microsoft.Direct3D.D3D12 is the NuGet package that carries newer
    # d3d12.h Agility SDK headers. WinPixEventRuntime supplies pix3.h, but
    # the RGP annotation-mode API is declared by the newer D3D12 headers.
    if(DEFINED ENV{NUGET_PACKAGES})
        list(APPEND _apusr_d3d12_agility_package_globs
            "$ENV{NUGET_PACKAGES}/microsoft.direct3d.d3d12/*"
            "$ENV{NUGET_PACKAGES}/Microsoft.Direct3D.D3D12/*")
    endif()
    if(DEFINED ENV{USERPROFILE})
        list(APPEND _apusr_d3d12_agility_package_globs
            "$ENV{USERPROFILE}/.nuget/packages/microsoft.direct3d.d3d12/*"
            "$ENV{USERPROFILE}/.nuget/packages/Microsoft.Direct3D.D3D12/*")
    endif()
    list(APPEND _apusr_d3d12_agility_package_globs
        "${CMAKE_SOURCE_DIR}/deps/microsoft.direct3d.d3d12*"
        "${CMAKE_SOURCE_DIR}/deps/Microsoft.Direct3D.D3D12*"
        "${CMAKE_SOURCE_DIR}/external/microsoft.direct3d.d3d12*"
        "${CMAKE_SOURCE_DIR}/external/Microsoft.Direct3D.D3D12*"
        "${CMAKE_SOURCE_DIR}/third_party/microsoft.direct3d.d3d12*"
        "${CMAKE_SOURCE_DIR}/third_party/Microsoft.Direct3D.D3D12*"
        "${CMAKE_SOURCE_DIR}/packages/microsoft.direct3d.d3d12*"
        "${CMAKE_SOURCE_DIR}/packages/Microsoft.Direct3D.D3D12*"
        "${CMAKE_SOURCE_DIR}/../apusr_deps/microsoft.direct3d.d3d12*"
        "${CMAKE_SOURCE_DIR}/../apusr_deps/Microsoft.Direct3D.D3D12*")

    foreach(_apusr_d3d12_agility_glob IN LISTS _apusr_d3d12_agility_package_globs)
        file(GLOB _apusr_d3d12_agility_glob_roots LIST_DIRECTORIES TRUE "${_apusr_d3d12_agility_glob}")
        list(APPEND _apusr_d3d12_agility_roots ${_apusr_d3d12_agility_glob_roots})
    endforeach()
    list(REMOVE_DUPLICATES _apusr_d3d12_agility_roots)

    # Do not fall back to the Windows SDK here.  We specifically need the
    # Microsoft.Direct3D.D3D12/Agility package headers when probing for the
    # RGP driver-retail annotation API.  A normal find_path() would happily
    # return C:/Program Files (x86)/Windows Kits/.../d3d12.h and make the
    # status output look as if Agility was active when it was not.
    list(SORT _apusr_d3d12_agility_roots COMPARE NATURAL ORDER DESCENDING)
    set(_apusr_d3d12_agility_include_candidates "")
    foreach(_apusr_d3d12_agility_root IN LISTS _apusr_d3d12_agility_roots)
        foreach(_apusr_d3d12_agility_suffix IN ITEMS
                "build/native/include"
                "build/native/include/directx"
                "include"
                "Include"
                "")
            if(_apusr_d3d12_agility_suffix STREQUAL "")
                set(_apusr_d3d12_agility_candidate "${_apusr_d3d12_agility_root}")
            else()
                set(_apusr_d3d12_agility_candidate "${_apusr_d3d12_agility_root}/${_apusr_d3d12_agility_suffix}")
            endif()
            if(EXISTS "${_apusr_d3d12_agility_candidate}/d3d12.h")
                list(APPEND _apusr_d3d12_agility_include_candidates "${_apusr_d3d12_agility_candidate}")
            endif()
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES _apusr_d3d12_agility_include_candidates)
    list(SORT _apusr_d3d12_agility_include_candidates COMPARE NATURAL ORDER DESCENDING)

    set(APUSR_D3D12_AGILITY_SDK_INCLUDE_DIR "" CACHE PATH "Directory containing DirectX 12 Agility SDK d3d12.h" FORCE)
    set(APUSR_D3D12_DRIVER_RETAIL_ANNOTATION_API_AVAILABLE FALSE)

    if(_apusr_d3d12_agility_include_candidates)
        include(CheckCXXSourceCompiles)
        set(_apusr_saved_required_includes "${CMAKE_REQUIRED_INCLUDES}")
        set(_apusr_d3d12_annotation_probe_source [=[
#include <windows.h>
#include <d3d12.h>
int main() {
    D3D12_FEATURE feature = D3D12_FEATURE_USER_DEFINED_ANNOTATION;
    D3D12_FEATURE_DATA_USER_DEFINED_ANNOTATION data{};
    ID3D12DeviceTools2* tools = nullptr;
    D3D12_USER_DEFINED_ANNOTATION_MODE mode = D3D12_USER_DEFINED_ANNOTATION_MODE_DRIVER_RETAIL;
    return static_cast<int>(feature) + static_cast<int>(mode) + (data.Supported ? 1 : 0) + (tools != nullptr ? 1 : 0);
}
]=])

        set(_apusr_d3d12_first_candidate "")
        set(_apusr_d3d12_probe_index 0)
        foreach(_apusr_d3d12_candidate IN LISTS _apusr_d3d12_agility_include_candidates)
            if(_apusr_d3d12_first_candidate STREQUAL "")
                set(_apusr_d3d12_first_candidate "${_apusr_d3d12_candidate}")
            endif()

            math(EXPR _apusr_d3d12_probe_index "${_apusr_d3d12_probe_index} + 1")
            set(_apusr_d3d12_probe_var "APUSR_D3D12_DRIVER_RETAIL_ANNOTATION_API_AVAILABLE_${_apusr_d3d12_probe_index}")
            set(CMAKE_REQUIRED_INCLUDES "${_apusr_d3d12_candidate}")
            check_cxx_source_compiles("${_apusr_d3d12_annotation_probe_source}" ${_apusr_d3d12_probe_var})
            if(${_apusr_d3d12_probe_var})
                set(APUSR_D3D12_AGILITY_SDK_INCLUDE_DIR "${_apusr_d3d12_candidate}" CACHE PATH "Directory containing DirectX 12 Agility SDK d3d12.h" FORCE)
                set(APUSR_D3D12_DRIVER_RETAIL_ANNOTATION_API_AVAILABLE TRUE)
                break()
            endif()
        endforeach()
        set(CMAKE_REQUIRED_INCLUDES "${_apusr_saved_required_includes}")

        if(NOT APUSR_D3D12_AGILITY_SDK_INCLUDE_DIR)
            # Keep the newest package header in the include path so status and
            # diagnostics are deterministic, but report that it lacks the API.
            set(APUSR_D3D12_AGILITY_SDK_INCLUDE_DIR "${_apusr_d3d12_first_candidate}" CACHE PATH "Directory containing DirectX 12 Agility SDK d3d12.h" FORCE)
        endif()
    endif()

    if(APUSR_D3D12_AGILITY_SDK_INCLUDE_DIR)
        target_compile_definitions(apusr_core PUBLIC APUSR_D3D12_AGILITY_SDK_HEADERS=1)
        # Put the selected package headers before the Windows SDK headers.  This
        # must be a real Microsoft.Direct3D.D3D12 package include directory, not
        # the Windows Kits d3d12.h fallback.
        target_include_directories(apusr_core BEFORE PRIVATE "${APUSR_D3D12_AGILITY_SDK_INCLUDE_DIR}")
        message(STATUS "APUSR GPU debug markers: using D3D12 package headers from ${APUSR_D3D12_AGILITY_SDK_INCLUDE_DIR}")

        if(APUSR_D3D12_DRIVER_RETAIL_ANNOTATION_API_AVAILABLE)
            target_compile_definitions(apusr_core PUBLIC APUSR_D3D12_DRIVER_RETAIL_ANNOTATION_API=1)
            message(STATUS "APUSR GPU debug markers: D3D12 driver-retail annotation API is available")
        else()
            target_compile_definitions(apusr_core PUBLIC APUSR_D3D12_DRIVER_RETAIL_ANNOTATION_API=0)
            message(WARNING
                "APUSR_ENABLE_GPU_DEBUG_MARKERS=ON found Microsoft.Direct3D.D3D12 headers at ${APUSR_D3D12_AGILITY_SDK_INCLUDE_DIR}, but they do not expose the D3D12 driver-retail annotation API "
                "(ID3D12DeviceTools2 / D3D12_FEATURE_USER_DEFINED_ANNOTATION / D3D12_USER_DEFINED_ANNOTATION_MODE_DRIVER_RETAIL). "
                "RGP annotation mode will report annotation_mode=header_unavailable. Install/restore a newer Microsoft.Direct3D.D3D12 NuGet package, expected to be Agility SDK 1.721+ for RGP driver-visible PIX markers, or leave markers OFF for distribution builds.")
        endif()
    else()
        target_compile_definitions(apusr_core PUBLIC APUSR_D3D12_AGILITY_SDK_HEADERS=0 APUSR_D3D12_DRIVER_RETAIL_ANNOTATION_API=0)
        message(WARNING
            "APUSR_ENABLE_GPU_DEBUG_MARKERS=ON found WinPixEventRuntime but not Microsoft.Direct3D.D3D12 / DirectX 12 Agility SDK package headers. "
            "PIX markers can still compile, but RGP annotation mode will report annotation_mode=header_unavailable and user markers may not appear in RGP. "
            "Install/restore the Microsoft.Direct3D.D3D12 NuGet package into the normal NuGet global cache or repo-local deps folder, or set APUSR_D3D12_AGILITY_SDK_ROOT/D3D12_AGILITY_SDK_ROOT. "
            "The Windows SDK d3d12.h fallback is intentionally ignored for this probe because it may be too old for RGP driver-visible PIX markers.")
    endif()

    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_GPU_DEBUG_MARKERS=1 APUSR_ENABLE_WINPIX_EVENT_RUNTIME=1)
    target_include_directories(apusr_core PRIVATE "${APUSR_WINPIX_EVENT_RUNTIME_INCLUDE_DIR}")
    target_link_libraries(apusr_core PUBLIC "${APUSR_WINPIX_EVENT_RUNTIME_LIBRARY}")
    if(APUSR_WINPIX_EVENT_RUNTIME_DLL)
        target_compile_definitions(apusr_core PUBLIC APUSR_WINPIX_EVENT_RUNTIME_DLL_AVAILABLE=1)
    endif()
else()
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_GPU_DEBUG_MARKERS=0 APUSR_ENABLE_WINPIX_EVENT_RUNTIME=0)
endif()


function(apusr_maybe_copy_winpix_event_runtime target_name)
    if(WIN32 AND APUSR_ENABLE_GPU_DEBUG_MARKERS AND APUSR_WINPIX_EVENT_RUNTIME_DLL)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${APUSR_WINPIX_EVENT_RUNTIME_DLL}"
                "$<TARGET_FILE_DIR:${target_name}>/WinPixEventRuntime.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${APUSR_WINPIX_EVENT_RUNTIME_LICENSE_FILE}"
                "$<TARGET_FILE_DIR:${target_name}>/LICENSE-WinPixEventRuntime-MIT.txt"
            VERBATIM
        )
    endif()
endfunction()


if(APUSR_ENABLE_DEBUG_HALF4_SIDECARS)
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_DEBUG_HALF4_SIDECARS=1)
else()
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_DEBUG_HALF4_SIDECARS=0)
endif()

if(APUSR_ENABLE_PERF_TELEMETRY)
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_PERF_TELEMETRY=1)
else()
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_PERF_TELEMETRY=0)
endif()

target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_PERSISTENT_CONTROL_RING_EXECUTION=0)

target_compile_definitions(apusr_core PUBLIC APUSR_FRAME_CONVEYOR_FRAME_BYTES=${APUSR_FRAME_CONVEYOR_FRAME_BYTES})
target_compile_definitions(apusr_core PUBLIC APUSR_FRAME_CONVEYOR_COLUMNS=${APUSR_FRAME_CONVEYOR_COLUMNS})
target_compile_definitions(apusr_core PUBLIC APUSR_FRAME_CONVEYOR_CHANNELS=${APUSR_FRAME_CONVEYOR_CHANNELS})
target_compile_definitions(apusr_core PUBLIC APUSR_MIDDLE_CORE_WORKER_GROUPS=${APUSR_EFFECTIVE_MIDDLE_CORE_WORKER_GROUPS})

if(MSVC)
    target_compile_options(apusr_core
        PRIVATE
            /W4
            /permissive-
    )
else()
    target_compile_options(apusr_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(APUSR_ENABLE_DX12_BACKEND)
    target_sources(apusr_core PRIVATE
        src/backends/gfx_dx12/apusr_gfx_dx12.cpp
    )
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_DX12_BACKEND=1)
else()
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_DX12_BACKEND=0)
endif()

if(APUSR_ENABLE_XRT_BACKEND)
    target_sources(apusr_core PRIVATE
        src/backends/npu_xrt/apusr_npu_xrt.cpp
    )
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_XRT_BACKEND=1)

    set(_apusr_xrt_root "${XRT_ROOT}")
    if(NOT _apusr_xrt_root AND DEFINED ENV{XRT_ROOT})
        set(_apusr_xrt_root "$ENV{XRT_ROOT}")
    endif()
    if(NOT _apusr_xrt_root AND WIN32 AND EXISTS "C:/Xilinx/XRT/include")
        set(_apusr_xrt_root "C:/Xilinx/XRT")
    endif()

    set(_apusr_has_native_xrt OFF)
    if(WIN32 AND _apusr_xrt_root)
        set(_apusr_xrt_include_dir "${_apusr_xrt_root}/include")
        if(EXISTS "${_apusr_xrt_root}/build/Release/opt/xilinx/xrt/include")
            set(_apusr_xrt_include_dir "${_apusr_xrt_root}/build/Release/opt/xilinx/xrt/include")
        endif()

        set(_apusr_xrt_lib_dir "${_apusr_xrt_root}/lib")
        if(EXISTS "${_apusr_xrt_root}/xrt_coreutil.lib")
            set(_apusr_xrt_lib_dir "${_apusr_xrt_root}")
        endif()
        set(_apusr_xrt_coreutil_lib "${_apusr_xrt_lib_dir}/xrt_coreutil.lib")

        if(NOT XRT_DLL_DIR)
            set(XRT_DLL_DIR "${_apusr_xrt_root}")
            if(EXISTS "${_apusr_xrt_root}/bin/xrt_coreutil.dll")
                set(XRT_DLL_DIR "${_apusr_xrt_root}/bin")
            elseif(DEFINED ENV{WINDIR} AND EXISTS "$ENV{WINDIR}/System32/xrt_coreutil.dll")
                set(XRT_DLL_DIR "$ENV{WINDIR}/System32")
            endif()
        endif()

        if(EXISTS "${_apusr_xrt_include_dir}" AND EXISTS "${_apusr_xrt_coreutil_lib}")
            set(_apusr_has_native_xrt ON)
            target_include_directories(apusr_core PRIVATE "${_apusr_xrt_include_dir}")
            target_link_libraries(apusr_core PUBLIC "${_apusr_xrt_coreutil_lib}")
            target_compile_definitions(apusr_core PUBLIC APUSR_HAS_NATIVE_XRT=1)
        endif()
    endif()

    if(NOT _apusr_has_native_xrt)
        target_compile_definitions(apusr_core PUBLIC APUSR_HAS_NATIVE_XRT=0)
        if(WIN32)
            message(FATAL_ERROR "APUSR_ENABLE_XRT_BACKEND=ON requires native Windows XRT headers/libs. Set -DXRT_ROOT=C:/Xilinx/XRT or disable the paused NPU backend.")
        endif()
    endif()
else()
    target_compile_definitions(apusr_core PUBLIC APUSR_ENABLE_XRT_BACKEND=0)
endif()




set(APUSR_DX12_SHADER_BLOBS_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/src/frontends/ffx_fsr4_dx12/apusr_fsr4_dx12_shader_blobs.h")
set(APUSR_DX12_SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dx12/apusr_fsr4_auto_exposure_reduce.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dx12/apusr_fsr4_auto_exposure_resolve.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dx12/apusr_fsr4_capture_model_input.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dx12/apusr_fsr4_export_tokens.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dx12/apusr_fsr4_import_tokens.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dx12/apusr_roundtrip_copy_verify.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dx12/apusr_fsr4_rcas.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common/apusr_token_layout.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common/apusr_fsr4_color.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common/apusr_model_sidecar.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common/apusr_auto_exposure.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/generated/apusr_fsr4_bookend_weights.hlsli")
if(WIN32)
    set(APUSR_DX12_SHADER_COMPILER_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_compiler/windows/bin/dxc.exe"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_compiler/windows/bin/dxcompiler.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_compiler/windows/bin/dxil.dll")
else()
    set(APUSR_DX12_SHADER_COMPILER_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_compiler/linux/bin/dxc"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_compiler/linux/bin/libdxcompiler.so"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_compiler/linux/bin/libdxil.so")
endif()
add_custom_target(apusr_compile_dx12_shaders
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_dx12_shaders.py"
        --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --output "${APUSR_DX12_SHADER_BLOBS_HEADER}"
        --shader-timeout-seconds "${APUSR_SHADER_COMPILER_CHILD_TIMEOUT_SECONDS}"
        --jobs "${APUSR_SHADER_COMPILE_JOBS}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_dx12_shaders.py"
        ${APUSR_DX12_SHADER_SOURCES}
        ${APUSR_DX12_SHADER_COMPILER_FILES}
    VERBATIM
)

# Frame Generation owns its permutation generator. Regeneration is explicit so
# a normal build never rewrites checked-in shader payloads.
add_custom_target(apusr_regenerate_framegen_dx12_shaders
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_framegen_dx12_shaders.py"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_framegen_dx12_shaders.py"
        ${APUSR_DX12_SHADER_COMPILER_FILES}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM
)

set(APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/include/apusr/generated/apusr_fsr4_reference_dx12_shader_blobs.h")
set(APUSR_FSR4_REFERENCE_DX12_SHADER_BLOBS_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/apusr_fsr4_reference_dx12_shader_blobs.cpp")
set(APUSR_FSR4_REFERENCE_INITIALIZERS_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/include/apusr/generated/apusr_fsr4_reference_initializers.h")
set(APUSR_FSR4_REFERENCE_INITIALIZERS_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/apusr_fsr4_reference_initializers.cpp")
set(APUSR_FSR4_REFERENCE_PASS0_WEIGHTS_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/include/apusr/generated/apusr_fsr4_reference_pass0_weights.h")
set(APUSR_FSR4_REFERENCE_PASS0_WEIGHTS_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/apusr_fsr4_reference_pass0_weights.cpp")
add_custom_target(apusr_embed_fsr4_reference_initializers
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/embed_fsr4_reference_initializers.py"
        --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --header "${APUSR_FSR4_REFERENCE_INITIALIZERS_HEADER}"
        --source "${APUSR_FSR4_REFERENCE_INITIALIZERS_SOURCE}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/embed_fsr4_reference_initializers.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/models/fsr4_int8/reference/fsr4_4_0_2_int8/quality/initializers.bin"
        "${CMAKE_CURRENT_SOURCE_DIR}/models/fsr4_int8/reference/fsr4_4_0_2_int8/balanced/initializers.bin"
        "${CMAKE_CURRENT_SOURCE_DIR}/models/fsr4_int8/reference/fsr4_4_0_2_int8/performance/initializers.bin"
        "${CMAKE_CURRENT_SOURCE_DIR}/models/fsr4_int8/reference/fsr4_4_0_2_int8/ultraperf/initializers.bin"
        "${CMAKE_CURRENT_SOURCE_DIR}/models/fsr4_int8/reference/fsr4_4_0_2_int8/native/initializers.bin"
        "${CMAKE_CURRENT_SOURCE_DIR}/models/fsr4_int8/reference/fsr4_4_0_2_int8/drs/initializers.bin"
    USES_TERMINAL
    VERBATIM
)
add_custom_target(apusr_verify_fsr4_reference_initializers_embedded
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/embed_fsr4_reference_initializers.py"
        --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --header "${APUSR_FSR4_REFERENCE_INITIALIZERS_HEADER}"
        --source "${APUSR_FSR4_REFERENCE_INITIALIZERS_SOURCE}"
        --verify
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/embed_fsr4_reference_initializers.py"
        "${APUSR_FSR4_REFERENCE_INITIALIZERS_HEADER}"
        "${APUSR_FSR4_REFERENCE_INITIALIZERS_SOURCE}"
    USES_TERMINAL
    VERBATIM
)

add_custom_target(apusr_compile_fsr4_reference_dx12_packaged_shader_cache
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/compile_fsr4_reference_dx12_shaders.py"
        --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --preset all
        --capacity all
        --product-matrix
        --header "${CMAKE_CURRENT_BINARY_DIR}/generated/apusr_fsr4_reference_dx12_shader_blobs.cache_unused.h"
        --source "${CMAKE_CURRENT_BINARY_DIR}/generated/apusr_fsr4_reference_dx12_shader_blobs.cache_unused.cpp"
        --cache-root "${APUSR_FSR4_REFERENCE_DX12_PACKAGED_SHADER_CACHE_ROOT}"
        --cache-only
        --shader-timeout-seconds "${APUSR_SHADER_COMPILER_CHILD_TIMEOUT_SECONDS}"
        --jobs "${APUSR_SHADER_COMPILE_JOBS}"
        ${APUSR_FSR4_REFERENCE_DX12_SHADER_VARIANT_ARGS}
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/compile_fsr4_reference_dx12_shaders.py"
        ${APUSR_FSR4_DX12_OPTIMIZED_SHADER_SOURCES}
        ${APUSR_FSR4_DX12_REFERENCE_SHADER_DEPENDS}
        ${APUSR_DX12_SHADER_COMPILER_FILES}
    USES_TERMINAL
    VERBATIM
)

add_custom_target(apusr_verify_fsr4_reference_dx12_packaged_shader_cache
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/verify_fsr4_reference_dx12_shader_cache.py"
        --cache-root "${APUSR_FSR4_REFERENCE_DX12_PACKAGED_SHADER_CACHE_ROOT}"
        --write-manifest "${CMAKE_CURRENT_BINARY_DIR}/fsr4_reference_dx12_shader_cache_manifest.json"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/verify_fsr4_reference_dx12_shader_cache.py"
    USES_TERMINAL
    VERBATIM
)

add_custom_target(apusr_verify_fsr4_reference_shaders
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/verify_fsr4_reference_shaders.py"
        --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --preset all
        --capacity all
        --include-utility
        --timeout-seconds "${APUSR_SHADER_COMPILER_CHILD_TIMEOUT_SECONDS}"
        --jobs "${APUSR_SHADER_COMPILE_JOBS}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/verify_fsr4_reference_shaders.py"
        ${APUSR_DX12_SHADER_COMPILER_FILES}
    USES_TERMINAL
    VERBATIM
)

add_custom_target(apusr_verify_fsr4_reference_shaders_quick
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/verify_fsr4_reference_shaders.py"
        --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --preset all
        --capacity all
        --quick
        --include-utility
        --timeout-seconds "${APUSR_SHADER_COMPILER_CHILD_TIMEOUT_SECONDS}"
        --jobs "${APUSR_SHADER_COMPILE_JOBS}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/verify_fsr4_reference_shaders.py"
        ${APUSR_DX12_SHADER_COMPILER_FILES}
    USES_TERMINAL
    VERBATIM
)

add_custom_target(apusr_verify_fsr4_dp4a_optimized_front_c16_shaders
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/compile_fsr4_tiled_experiment_shaders.py"
        --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --verify
        --timeout-seconds "${APUSR_SHADER_COMPILER_CHILD_TIMEOUT_SECONDS}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/compile_fsr4_tiled_experiment_shaders.py"
        ${APUSR_FSR4_DX12_OPTIMIZED_SHADER_SOURCES}
        ${APUSR_FSR4_DX12_REFERENCE_SHADER_DEPENDS}
        ${APUSR_DX12_SHADER_COMPILER_FILES}
    USES_TERMINAL
    VERBATIM
)

add_custom_target(apusr_verify_fsr4_dp4a_optimized_pass1_shader
    DEPENDS apusr_verify_fsr4_dp4a_optimized_front_c16_shaders
)

add_custom_target(apusr_generate_fsr4_reference_dx12_reflection_manifest
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/generate_fsr4_reference_shader_resource_manifest.py"
        --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --target-api dx12
        --preset all
        --capacity all
        --product-matrix
        --include-utility
        --timeout-seconds "${APUSR_SHADER_COMPILER_CHILD_TIMEOUT_SECONDS}"
        --jobs "${APUSR_SHADER_COMPILE_JOBS}"
        --output "${CMAKE_CURRENT_BINARY_DIR}/fsr4_reference_dx12_shader_resource_manifest.json"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/generate_fsr4_reference_shader_resource_manifest.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/generate_fsr4_reference_dx12_reflection_manifest.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/compile_fsr4_reference_dx12_shaders.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/verify_fsr4_reference_shaders.py"
        ${APUSR_DX12_SHADER_COMPILER_FILES}
    USES_TERMINAL
    VERBATIM
)

add_custom_target(apusr_generate_fsr4_reference_dx12_reflection_manifest_quick
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/generate_fsr4_reference_shader_resource_manifest.py"
        --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --target-api dx12
        --preset balanced
        --capacity 2160
        --quick
        --include-utility
        --timeout-seconds "${APUSR_SHADER_COMPILER_CHILD_TIMEOUT_SECONDS}"
        --jobs "${APUSR_SHADER_COMPILE_JOBS}"
        --output "${CMAKE_CURRENT_BINARY_DIR}/fsr4_reference_dx12_shader_resource_manifest.quick.json"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/generate_fsr4_reference_shader_resource_manifest.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/compile_fsr4_reference_dx12_shaders.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/verify_fsr4_reference_shaders.py"
        ${APUSR_DX12_SHADER_COMPILER_FILES}
    USES_TERMINAL
    VERBATIM
)

add_subdirectory(kernels)
if(APUSR_ENABLE_PERSISTENT_CONTROL_RING_EXECUTION AND NOT (APUSR_AIE_KERNEL_PROFILE STREQUAL "MiddleCoreStage1To12Spatial2Col" OR APUSR_AIE_KERNEL_PROFILE STREQUAL "MiddleCoreStage1To12Spatial4Col"))
    message(FATAL_ERROR "persistent frame-conveyor execution belongs to the selected product middle-core graph, not a standalone control-ring profile")
endif()
if(APUSR_REQUIRE_PACKAGED_NATIVE AND NOT (APUSR_AIE_KERNEL_PROFILE STREQUAL "MiddleCoreStage1To12Spatial2Col" OR APUSR_AIE_KERNEL_PROFILE STREQUAL "MiddleCoreStage1To12Spatial4Col"))
    message(FATAL_ERROR "APUSR_REQUIRE_PACKAGED_NATIVE requires a spatial Stage1->12 product middle-core profile")
endif()

set(_apusr_selected_aie_xclbin "${APUSR_MIDDLE_CORE_STAGE1_12_XCLBIN}")
set(_apusr_selected_aie_insts "${APUSR_MIDDLE_CORE_STAGE1_12_INSTS}")
set(_apusr_selected_aie_parameter_load_insts "${APUSR_MIDDLE_CORE_STAGE1_12_PARAMETER_LOAD_INSTS}")
set(_apusr_packaged_kernel_profile_value 17)
target_compile_definitions(apusr_core PUBLIC APUSR_PACKAGED_KERNEL_PROFILE=${_apusr_packaged_kernel_profile_value})
target_compile_definitions(apusr_core PUBLIC APUSR_PACKAGED_MIDDLE_CORE_FRAME_CAPACITY=${APUSR_MIDDLE_CORE_FRAME_CAPACITY})
if(APUSR_BUILD_AIE_KERNELS AND NOT APUSR_FSR4_DX12_EMBED_XCLBIN)
    set(APUSR_FSR4_DX12_EMBED_XCLBIN "${_apusr_selected_aie_xclbin}")
endif()
if(APUSR_BUILD_AIE_KERNELS AND NOT APUSR_FSR4_DX12_EMBED_INSTRUCTIONS)
    set(APUSR_FSR4_DX12_EMBED_INSTRUCTIONS "${_apusr_selected_aie_insts}")
endif()
if(APUSR_BUILD_AIE_KERNELS AND _apusr_selected_aie_parameter_load_insts AND NOT APUSR_FSR4_DX12_EMBED_PARAMETER_LOAD_INSTRUCTIONS)
    set(APUSR_FSR4_DX12_EMBED_PARAMETER_LOAD_INSTRUCTIONS "${_apusr_selected_aie_parameter_load_insts}")
endif()

if((APUSR_AIE_KERNEL_PROFILE STREQUAL "MiddleCoreStage1To12Spatial2Col" OR APUSR_AIE_KERNEL_PROFILE STREQUAL "MiddleCoreStage1To12Spatial4Col") AND NOT APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETERS AND NOT APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETER_SET)
    set(_apusr_product_resident_parameter_set
        "${CMAKE_CURRENT_SOURCE_DIR}/models/fsr4_int8/resident_params/stage1_12_full_arena__${APUSR_FSR4_DX12_PARAMETER_PROFILE}.json")
    if(NOT EXISTS "${_apusr_product_resident_parameter_set}")
        message(FATAL_ERROR "The selected spatial Stage1->12 profile requires the universal product resident parameter set: ${_apusr_product_resident_parameter_set}. Regenerate it from the external parameter workspace or set APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETER_SET explicitly.")
    endif()
    set(APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETER_SET "${_apusr_product_resident_parameter_set}")
endif()

if(APUSR_REQUIRE_PACKAGED_NATIVE)
    if(NOT APUSR_ENABLE_XRT_BACKEND)
        message(FATAL_ERROR "APUSR_REQUIRE_PACKAGED_NATIVE requires APUSR_ENABLE_XRT_BACKEND=ON.")
    endif()
    if(NOT APUSR_FSR4_DX12_EMBED_XCLBIN)
        message(FATAL_ERROR "APUSR_REQUIRE_PACKAGED_NATIVE requires an embedded xclbin. Set APUSR_FSR4_DX12_EMBED_XCLBIN or enable APUSR_BUILD_AIE_KERNELS.")
    endif()
    if(NOT APUSR_FSR4_DX12_EMBED_INSTRUCTIONS)
        message(FATAL_ERROR "APUSR_REQUIRE_PACKAGED_NATIVE requires embedded MLIR-AIE instructions. Set APUSR_FSR4_DX12_EMBED_INSTRUCTIONS or enable APUSR_BUILD_AIE_KERNELS.")
    endif()
    if(_apusr_packaged_kernel_profile_value EQUAL 17)
        if(NOT APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETERS AND NOT APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETER_SET)
            message(FATAL_ERROR "APUSR_REQUIRE_PACKAGED_NATIVE resident middle-core profiles require embedded resident parameter arenas. Set APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETER_SET or APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETERS.")
        endif()
        if(NOT APUSR_FSR4_DX12_EMBED_PARAMETER_LOAD_INSTRUCTIONS)
            message(FATAL_ERROR "APUSR_REQUIRE_PACKAGED_NATIVE resident middle-core profiles require embedded model-epoch parameter-load instructions. Set APUSR_FSR4_DX12_EMBED_PARAMETER_LOAD_INSTRUCTIONS.")
        endif()
    endif()
endif()
set(APUSR_FSR4_DX12_EMBEDDED_XCLBIN_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/apusr_fsr4_dx12_embedded_xclbin.cpp")
set(_apusr_embed_xclbin_arg "")
set(_apusr_embed_xclbin_depends "")
if(APUSR_FSR4_DX12_EMBED_XCLBIN)
    set(_apusr_embed_xclbin_arg --input "${APUSR_FSR4_DX12_EMBED_XCLBIN}")
    set(_apusr_embed_xclbin_depends "${APUSR_FSR4_DX12_EMBED_XCLBIN}")
endif()
set(_apusr_embed_insts_arg "")
set(_apusr_embed_insts_depends "")
if(APUSR_FSR4_DX12_EMBED_INSTRUCTIONS)
    set(_apusr_embed_insts_arg --instructions "${APUSR_FSR4_DX12_EMBED_INSTRUCTIONS}")
    set(_apusr_embed_insts_depends "${APUSR_FSR4_DX12_EMBED_INSTRUCTIONS}")
endif()
set(_apusr_embed_param_load_insts_arg "")
set(_apusr_embed_param_load_insts_depends "")
if(APUSR_FSR4_DX12_EMBED_PARAMETER_LOAD_INSTRUCTIONS)
    set(_apusr_embed_param_load_insts_arg --parameter-load-instructions "${APUSR_FSR4_DX12_EMBED_PARAMETER_LOAD_INSTRUCTIONS}")
    set(_apusr_embed_param_load_insts_depends "${APUSR_FSR4_DX12_EMBED_PARAMETER_LOAD_INSTRUCTIONS}")
endif()
set(_apusr_embed_resident_params_arg "")
set(_apusr_embed_resident_params_depends "")
if(APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETER_SET)
    set(_apusr_embed_resident_params_arg --resident-parameter-set "${APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETER_SET}")
    get_filename_component(_apusr_embed_resident_parameter_set_dir "${APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETER_SET}" DIRECTORY)
    file(GLOB_RECURSE _apusr_embed_resident_parameter_set_bins CONFIGURE_DEPENDS
        "${_apusr_embed_resident_parameter_set_dir}/*/*/stage1_12_full_arena.bin")
    set(_apusr_embed_resident_params_depends "${APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETER_SET}" ${_apusr_embed_resident_parameter_set_bins})
elseif(APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETERS)
    set(_apusr_embed_resident_params_arg --resident-parameters "${APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETERS}" --resident-parameter-model "${APUSR_FSR4_DX12_MODEL_PERMUTATION}" --resident-parameter-profile "${APUSR_FSR4_DX12_PARAMETER_PROFILE}")
    set(_apusr_embed_resident_params_depends "${APUSR_FSR4_DX12_EMBED_RESIDENT_PARAMETERS}")
endif()
add_custom_command(
    OUTPUT "${APUSR_FSR4_DX12_EMBEDDED_XCLBIN_CPP}"
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py"
        --output "${APUSR_FSR4_DX12_EMBEDDED_XCLBIN_CPP}"
        ${_apusr_embed_xclbin_arg}
        ${_apusr_embed_insts_arg}
        ${_apusr_embed_param_load_insts_arg}
        ${_apusr_embed_resident_params_arg}
        --kernel-name "${APUSR_FSR4_DX12_KERNEL_NAME}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py" ${_apusr_embed_xclbin_depends} ${_apusr_embed_insts_depends} ${_apusr_embed_param_load_insts_depends} ${_apusr_embed_resident_params_depends}
    VERBATIM
)


if(APUSR_BUILD_FRAME_CONVEYOR_BENCHMARK)
    if(NOT APUSR_BUILD_AIE_KERNELS)
        message(FATAL_ERROR "APUSR_BUILD_FRAME_CONVEYOR_BENCHMARK requires APUSR_BUILD_AIE_KERNELS=ON so the benchmark xclbin/insts are built with the matching byte and stream geometry.")
    endif()
    if(NOT APUSR_ENABLE_XRT_BACKEND OR NOT _apusr_has_native_xrt)
        message(FATAL_ERROR "APUSR_BUILD_FRAME_CONVEYOR_BENCHMARK requires the native XRT backend. Set -DAPUSR_ENABLE_XRT_BACKEND=ON -DXRT_ROOT=C:/Xilinx/XRT.")
    endif()

    add_executable(apusr_frame_conveyor_xrt_benchmark
        tools/apusr_frame_conveyor_xrt_benchmark.cpp
    )
    apusr_apply_host_cxx_contract(apusr_frame_conveyor_xrt_benchmark)
    target_include_directories(apusr_frame_conveyor_xrt_benchmark
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${_apusr_xrt_include_dir}"
    )
    target_link_libraries(apusr_frame_conveyor_xrt_benchmark PRIVATE "${_apusr_xrt_coreutil_lib}")
    target_compile_definitions(apusr_frame_conveyor_xrt_benchmark
        PRIVATE
            APUSR_FRAME_CONVEYOR_BENCHMARK_FRAME_BYTES=${APUSR_FRAME_CONVEYOR_BENCHMARK_FRAME_BYTES}
            APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMNS=${APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMNS}
            APUSR_FRAME_CONVEYOR_BENCHMARK_CHANNELS=${APUSR_FRAME_CONVEYOR_BENCHMARK_CHANNELS}
            APUSR_FRAME_CONVEYOR_BENCHMARK_DEFAULT_XCLBIN="apusr_frame_conveyor_benchmark.xclbin"
            APUSR_FRAME_CONVEYOR_BENCHMARK_DEFAULT_INSTS="apusr_frame_conveyor_benchmark.insts.bin"
    )
    add_dependencies(apusr_frame_conveyor_xrt_benchmark apusr_frame_conveyor_benchmark_xclbin)
    add_custom_command(TARGET apusr_frame_conveyor_xrt_benchmark POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${APUSR_FRAME_CONVEYOR_BENCHMARK_XCLBIN}"
            "$<TARGET_FILE_DIR:apusr_frame_conveyor_xrt_benchmark>/apusr_frame_conveyor_benchmark.xclbin"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${APUSR_FRAME_CONVEYOR_BENCHMARK_INSTS}"
            "$<TARGET_FILE_DIR:apusr_frame_conveyor_xrt_benchmark>/apusr_frame_conveyor_benchmark.insts.bin"
        VERBATIM
    )
    if(WIN32 AND XRT_DLL_DIR AND EXISTS "${XRT_DLL_DIR}/xrt_coreutil.dll")
        add_custom_command(TARGET apusr_frame_conveyor_xrt_benchmark POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${XRT_DLL_DIR}/xrt_coreutil.dll"
                "$<TARGET_FILE_DIR:apusr_frame_conveyor_xrt_benchmark>/xrt_coreutil.dll"
            VERBATIM
        )
    endif()
endif()

if(APUSR_BUILD_FRONTENDS)
    if(WIN32)
        enable_language(RC)
        set(_apusr_framegen_algorithm_root "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm")
        set(_apusr_framegen_generated_dir "${_apusr_framegen_algorithm_root}/generated")
        add_library(apusr_ffx_framegen_dx12 SHARED
            src/framegen/apusr_framegen_allocator.h
            src/framegen/apusr_framegen_backend.h
            src/framegen/apusr_framegen_backend_interface.h
            src/framegen/apusr_framegen_backend_types.h
            src/framegen/apusr_framegen_core_types.h
            src/framegen/apusr_framegen_resource_types.h
            src/framegen/algorithm/apusr_framegen_algorithm_types.h
            src/framegen/algorithm/internal/apusr_framegen_algorithm_storage.h
            src/framegen/ffx_compat/apusr_framegen_algorithm_compat.h
            src/framegen/ffx_compat/apusr_framegen_config_compat.h
            src/framegen/ffx_compat/apusr_framegen_backend_compat.h
            src/framegen/ffx_dx12/apusr_framegen_frame_latency_dx12.cpp
            src/framegen/ffx_dx12/apusr_framegen_frame_latency_dx12.h
            src/framegen/ffx_dx12/apusr_framegen_pacing_dx12.cpp
            src/framegen/ffx_dx12/apusr_framegen_pacing_dx12.h
            src/framegen/ffx_dx12/apusr_framegen_present_data_dx12.h
            src/framegen/ffx_dx12/apusr_framegen_present_path_dx12.cpp
            src/framegen/ffx_dx12/apusr_framegen_present_path_dx12.h
            src/framegen/ffx_dx12/apusr_framegen_provider_identity.cpp
            src/framegen/ffx_dx12/apusr_framegen_provider_registry_dx12.cpp
            src/framegen/ffx_dx12/apusr_framegen_present_dx12.cpp
            src/framegen/ffx_dx12/apusr_framegen_present_dx12.h
            src/framegen/ffx_dx12/apusr_framegen_swapchain_provider_dx12.cpp
            src/framegen/ffx_dx12/apusr_framegen_swapchain_provider_dx12.h
            src/framegen/ffx_dx12/apusr_framegen_resource_size_dx12.cpp
            src/framegen/ffx_compat/apusr_framegen_ffx_api.cpp
            src/framegen/ffx_compat/apusr_framegen_resource_compat.h
            src/framegen/ffx_compat/apusr_framegen_provider.h
            src/framegen/ffx_compat/apusr_framegen_provider_registry.h
            src/framegen/algorithm/apusr_framegen_assert.cpp
            src/framegen/algorithm/apusr_framegen_message.cpp
            src/framegen/ffx_dx12/apusr_framegen_backend_dx12.cpp
            src/framegen/ffx_dx12/apusr_framegen_backend_compat_dx12.cpp
            src/framegen/ffx_dx12/apusr_framegen_backend_dx12.h
            src/framegen/ffx_dx12/apusr_framegen_ffx_dx12.cpp
            src/framegen/ffx_dx12/apusr_framegen_ffx_dx12.h
            src/framegen/ffx_dx12/apusr_framegen_dx12_resource_compat.h
            src/framegen/ffx_dx12/apusr_framegen_swapchain_dx12.cpp
            src/framegen/ffx_dx12/apusr_framegen_swapchain_dx12.h
            src/framegen/ffx_dx12/apusr_framegen_debug_pacing_dx12.cpp
            src/framegen/ffx_dx12/apusr_framegen_debug_pacing_dx12.h
            src/framegen/ffx_dx12/apusr_framegen_dx12_helpers.cpp
            src/framegen/ffx_dx12/apusr_framegen_dx12_helpers.h
            src/framegen/ffx_dx12/apusr_framegen_ui_composition_dx12.cpp
            src/framegen/ffx_dx12/apusr_framegen_ui_composition_dx12.h
            src/framegen/algorithm/apusr_frameinterpolation.cpp
            src/framegen/algorithm/apusr_frameinterpolation_shaderblobs.cpp
            src/framegen/algorithm/apusr_opticalflow.cpp
            src/framegen/algorithm/apusr_opticalflow_shaderblobs.cpp
            src/framegen/algorithm/generated/apusr_framegen_dxil.cpp
            src/framegen/ffx_compat/apusr_frameinterpolation_compat.cpp
            src/framegen/ffx_compat/apusr_opticalflow_compat.cpp
            src/framegen/ffx_compat/apusr_framegen_algorithm_provider.cpp
            src/framegen/ffx_dx12/apusr_framegen_dll.rc
        )
        apusr_apply_host_cxx_contract(apusr_ffx_framegen_dx12)
        target_link_libraries(apusr_ffx_framegen_dx12 PRIVATE apusr_profiler_runtime)
        target_include_directories(apusr_ffx_framegen_dx12 PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/ffx_compat"
            "${_apusr_framegen_algorithm_root}/include"
            "${_apusr_framegen_algorithm_root}/internal"
            "${_apusr_framegen_generated_dir}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/ffx_compat/abi"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/ffx_dx12"
        )
        target_compile_definitions(apusr_ffx_framegen_dx12 PRIVATE
            AMDFIDELITYFXFRAMEGENERATIONDX12_EXPORTS=1
            NOMINMAX=1
            _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR=1
            FFX_FRAMEGENERATION=1
            FFX_BACKEND_DX12=1
            FFX_DX12_ENABLE_GPU_MARKERS=0
            CI_PIPELINE_ID=0
        )
        if(MSVC)
            target_compile_options(apusr_ffx_framegen_dx12 PRIVATE
                /wd4068 /wd4100 /wd4189 /wd4201 /wd4324 /wd4505 /wd4996)
        endif()
        target_link_libraries(apusr_ffx_framegen_dx12 PRIVATE d3d12 dxgi dwmapi winmm)
        set_target_properties(apusr_ffx_framegen_dx12 PROPERTIES
            OUTPUT_NAME "amd_fidelityfx_framegeneration_dx12")
        add_custom_target(apusr_framegen_dx12 DEPENDS apusr_ffx_framegen_dx12)
    endif()

    add_library(apusr_fsr4_dx12 SHARED
        "${APUSR_FSR4_DX12_EMBEDDED_XCLBIN_CPP}"
        src/frontends/ffx_fsr4_dx12/apusr_fsr4_dx12_frontend.cpp
        src/frontends/ffx_fsr4_dx12/apusr_fsr4_dx12_runtime.cpp
        src/frontends/ffx_fsr4_dx12/apusr_fsr4_dx12_shaders.cpp
    )
    apusr_apply_host_cxx_contract(apusr_fsr4_dx12)
    target_link_libraries(apusr_fsr4_dx12 PRIVATE apusr::core)
    target_compile_definitions(apusr_fsr4_dx12 PRIVATE
        APUSR_FSR4_DX12_FRONTEND_EXPORTS=1
        APUSR_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    if(APUSR_REQUIRE_PACKAGED_NATIVE)
        target_compile_definitions(apusr_fsr4_dx12 PRIVATE APUSR_REQUIRE_PACKAGED_NATIVE=1)
    else()
        target_compile_definitions(apusr_fsr4_dx12 PRIVATE APUSR_REQUIRE_PACKAGED_NATIVE=0)
    endif()
    target_include_directories(apusr_fsr4_dx12
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    )
    set_target_properties(apusr_fsr4_dx12 PROPERTIES
        OUTPUT_NAME "amd_fidelityfx_dx12"
    )
    if(WIN32)
        target_link_libraries(apusr_fsr4_dx12 PRIVATE d3d12 dxgi)
    endif()

    apusr_maybe_copy_winpix_event_runtime(apusr_fsr4_dx12)

    function(apusr_attach_package_legal_files target_name package_dir)
        set(options WITH_DETOURS)
        cmake_parse_arguments(APUSR_PACKAGE "${options}" "" "" ${ARGN})
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                "${package_dir}/LICENSE"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE"
                "${package_dir}/NOTICE"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
                "${package_dir}/THIRD_PARTY_NOTICES.md"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/FidelityFX-MIT.txt"
                "${package_dir}/LICENSE-FidelityFX-MIT.txt"
            VERBATIM
        )
        if(APUSR_PACKAGE_WITH_DETOURS)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/Detours-MIT.txt"
                    "${package_dir}/LICENSE-Detours-MIT.txt"
                VERBATIM
            )
        endif()
        if(APUSR_ENABLE_GPU_DEBUG_MARKERS AND APUSR_WINPIX_EVENT_RUNTIME_DLL)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${APUSR_WINPIX_EVENT_RUNTIME_LICENSE_FILE}"
                    "${package_dir}/LICENSE-WinPixEventRuntime-MIT.txt"
                VERBATIM
            )
        endif()
    endfunction()

    if(WIN32 AND APUSR_ENABLE_XRT_BACKEND AND XRT_DLL_DIR AND EXISTS "${XRT_DLL_DIR}/xrt_coreutil.dll")
        add_custom_command(TARGET apusr_fsr4_dx12 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${XRT_DLL_DIR}/xrt_coreutil.dll"
                "$<TARGET_FILE_DIR:apusr_fsr4_dx12>/xrt_coreutil.dll"
            VERBATIM
        )
    endif()

    if(WIN32)
        set(_apusr_ffx_dx12_package_depends apusr_fsr4_dx12)
        set(_apusr_ffx_dx12_package_commands
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_BINARY_DIR}/package_ffx_dx12_replacement"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/packaging/ffx_dx12_replacement/README.txt"
                "${CMAKE_BINARY_DIR}/package_ffx_dx12_replacement/README.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:apusr_fsr4_dx12>"
                "${CMAKE_BINARY_DIR}/package_ffx_dx12_replacement/amd_fidelityfx_dx12.dll")
        list(APPEND _apusr_ffx_dx12_package_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:apusr_ffx_framegen_dx12>"
                "${CMAKE_BINARY_DIR}/package_ffx_dx12_replacement/amd_fidelityfx_framegeneration_dx12.dll")
        list(APPEND _apusr_ffx_dx12_package_depends apusr_ffx_framegen_dx12)
        if(APUSR_ENABLE_GPU_DEBUG_MARKERS AND APUSR_WINPIX_EVENT_RUNTIME_DLL)
            list(APPEND _apusr_ffx_dx12_package_commands
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${APUSR_WINPIX_EVENT_RUNTIME_DLL}"
                    "${CMAKE_BINARY_DIR}/package_ffx_dx12_replacement/WinPixEventRuntime.dll")
        endif()
        add_custom_target(apusr_package_ffx_dx12_replacement
            ${_apusr_ffx_dx12_package_commands}
            DEPENDS ${_apusr_ffx_dx12_package_depends}
            VERBATIM
        )
        apusr_attach_package_legal_files(
            apusr_package_ffx_dx12_replacement
            "${CMAKE_BINARY_DIR}/package_ffx_dx12_replacement")

        add_custom_target(apusr_package_ffx_dx12_compat_aliases
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:apusr_fsr4_dx12>"
                "${CMAKE_BINARY_DIR}/package_ffx_dx12_replacement/amd_fidelityfx_upscaler_dx12.dll"
            DEPENDS apusr_package_ffx_dx12_replacement
            VERBATIM
        )
    endif()

    add_library(apusr_ngx_dlss_dx12 SHARED
        src/frontends/ngx_dlss_dx12/apusr_ngx_dlss_dx12_frontend.cpp
    )
    if(WIN32)
        enable_language(RC)
        set(APUSR_NGX_DLSS_EXPORTS_DEF
            "${CMAKE_CURRENT_SOURCE_DIR}/src/frontends/ngx_dlss_dx12/apusr_ngx_dlss_exports.def")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${APUSR_NGX_DLSS_EXPORTS_DEF}")
        set(APUSR_NVNGX_FORWARDER_DEF
            "${CMAKE_CURRENT_BINARY_DIR}/generated/ngx_dlss/apusr_nvngx_alias_forwarder.def")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/ngx_dlss")
        file(STRINGS "${APUSR_NGX_DLSS_EXPORTS_DEF}" _apusr_ngx_export_lines)
        file(WRITE "${APUSR_NVNGX_FORWARDER_DEF}" "LIBRARY nvngx\nEXPORTS\n")
        foreach(_apusr_ngx_export_line IN LISTS _apusr_ngx_export_lines)
            string(STRIP "${_apusr_ngx_export_line}" _apusr_ngx_export_line)
            if(_apusr_ngx_export_line STREQUAL "" OR
               _apusr_ngx_export_line MATCHES "^LIBRARY[ \t]" OR
               _apusr_ngx_export_line STREQUAL "EXPORTS" OR
               _apusr_ngx_export_line MATCHES "^[;#]")
                continue()
            endif()
            string(REGEX MATCH "^[^ \t=]+" _apusr_ngx_export_name "${_apusr_ngx_export_line}")
            file(APPEND "${APUSR_NVNGX_FORWARDER_DEF}"
                "    ${_apusr_ngx_export_name}=nvngx_dlss.${_apusr_ngx_export_name}\n")
        endforeach()

        target_sources(apusr_ngx_dlss_dx12 PRIVATE
            src/frontends/ngx_dlss_dx12/apusr_ngx_dlss_dx12_version.rc
            "${APUSR_NGX_DLSS_EXPORTS_DEF}"
        )
        add_library(apusr_nvngx_alias_forwarder SHARED
            src/frontends/ngx_dlss_dx12/apusr_nvngx_alias_forwarder.cpp
            src/frontends/ngx_dlss_dx12/apusr_nvngx_alias_forwarder_version.rc
            "${APUSR_NVNGX_FORWARDER_DEF}"
        )
        apusr_apply_host_cxx_contract(apusr_nvngx_alias_forwarder)
        # MSVC LINK validates named PE forwarders through the target DLL/import library.
        # Linking the real frontend also gives Ninja the required clean-build ordering.
        target_link_libraries(apusr_nvngx_alias_forwarder PRIVATE apusr_ngx_dlss_dx12)
        set_target_properties(apusr_nvngx_alias_forwarder PROPERTIES OUTPUT_NAME "nvngx")
    endif()
    apusr_apply_host_cxx_contract(apusr_ngx_dlss_dx12)
    target_link_libraries(apusr_ngx_dlss_dx12 PUBLIC apusr::core)
    target_compile_definitions(apusr_ngx_dlss_dx12 PRIVATE
        APUSR_NGX_DLSS_DX12_FRONTEND_EXPORTS=1
        APUSR_HOST_DISCOVERY_LOG_COMPILED=1
        APUSR_NGX_TRACE_COMPILED=1)
    target_include_directories(apusr_ngx_dlss_dx12
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    )
    set_target_properties(apusr_ngx_dlss_dx12 PROPERTIES
        OUTPUT_NAME "nvngx_dlss"
    )
    if(WIN32)
        target_link_libraries(apusr_ngx_dlss_dx12 PRIVATE d3d12 dxgi)
    endif()
    apusr_maybe_copy_winpix_event_runtime(apusr_ngx_dlss_dx12)

    set(_apusr_package_ngx_dlss_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_BINARY_DIR}/package_ngx_dlss"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/ngx_dlss/README.txt"
            "${CMAKE_BINARY_DIR}/package_ngx_dlss/README.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:apusr_ngx_dlss_dx12>"
            "${CMAKE_BINARY_DIR}/package_ngx_dlss/$<TARGET_FILE_NAME:apusr_ngx_dlss_dx12>")
    if(WIN32)
        list(APPEND _apusr_package_ngx_dlss_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:apusr_nvngx_alias_forwarder>"
                "${CMAKE_BINARY_DIR}/package_ngx_dlss/nvngx.dll")
    else()
        list(APPEND _apusr_package_ngx_dlss_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:apusr_ngx_dlss_dx12>"
                "${CMAKE_BINARY_DIR}/package_ngx_dlss/nvngx.dll")
    endif()
    if(APUSR_ENABLE_GPU_DEBUG_MARKERS AND APUSR_WINPIX_EVENT_RUNTIME_DLL)
        list(APPEND _apusr_package_ngx_dlss_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${APUSR_WINPIX_EVENT_RUNTIME_DLL}"
                "${CMAKE_BINARY_DIR}/package_ngx_dlss/WinPixEventRuntime.dll")
    endif()
    set(_apusr_package_ngx_dlss_depends apusr_ngx_dlss_dx12)
    if(WIN32)
        list(APPEND _apusr_package_ngx_dlss_depends apusr_nvngx_alias_forwarder)
    endif()
    add_custom_target(apusr_package_ngx_dlss
        ${_apusr_package_ngx_dlss_commands}
        DEPENDS ${_apusr_package_ngx_dlss_depends}
        VERBATIM
    )
    apusr_attach_package_legal_files(
        apusr_package_ngx_dlss
        "${CMAKE_BINARY_DIR}/package_ngx_dlss")
    add_custom_target(apusr_package_dlss_ngx_dropin
        DEPENDS apusr_package_ngx_dlss
    )

    if(WIN32)
        add_library(apusr_nvapi_host_proxy SHARED
            src/frontends/nvapi_host_probe/apusr_nvapi_host_proxy.cpp
            src/frontends/nvapi_host_probe/apusr_nvapi_host_proxy.def
        )
        apusr_apply_host_cxx_contract(apusr_nvapi_host_proxy)
        target_compile_definitions(apusr_nvapi_host_proxy PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _CRT_SECURE_NO_WARNINGS
        )
        set_target_properties(apusr_nvapi_host_proxy PROPERTIES
            OUTPUT_NAME "nvapi64"
        )
        apusr_maybe_copy_winpix_event_runtime(apusr_nvapi_host_proxy)

        add_custom_target(apusr_package_dlss_ngx_with_nvapi
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_BINARY_DIR}/package_dlss_ngx_with_nvapi"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:apusr_nvapi_host_proxy>"
                "${CMAKE_BINARY_DIR}/package_dlss_ngx_with_nvapi/$<TARGET_FILE_NAME:apusr_nvapi_host_proxy>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:apusr_ngx_dlss_dx12>"
                "${CMAKE_BINARY_DIR}/package_dlss_ngx_with_nvapi/$<TARGET_FILE_NAME:apusr_ngx_dlss_dx12>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:apusr_nvngx_alias_forwarder>"
                "${CMAKE_BINARY_DIR}/package_dlss_ngx_with_nvapi/nvngx.dll"
            DEPENDS apusr_nvapi_host_proxy apusr_ngx_dlss_dx12 apusr_nvngx_alias_forwarder
            VERBATIM
        )
        apusr_attach_package_legal_files(
            apusr_package_dlss_ngx_with_nvapi
            "${CMAKE_BINARY_DIR}/package_dlss_ngx_with_nvapi")

        if(APUSR_ENABLE_VULKAN_BACKEND)
        add_library(apusr_detours STATIC IMPORTED GLOBAL)
        set_target_properties(apusr_detours PROPERTIES
            IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/third_party/detours/lib/x64/detours.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/detours/include"
        )

        add_library(apusr_vulkan_host_capture SHARED
            src/frontends/vulkan_host_capture/apusr_vulkan_host_capture_core.cpp
            src/frontends/vulkan_host_capture/apusr_vulkan_host_capture_win32.cpp
            src/frontends/vulkan_host_capture/apusr_vulkan_host_capture.def
        )
        apusr_apply_host_cxx_contract(apusr_vulkan_host_capture)
        target_link_libraries(apusr_vulkan_host_capture PRIVATE apusr_detours apusr_profiler_runtime)
        target_compile_definitions(apusr_vulkan_host_capture PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _CRT_SECURE_NO_WARNINGS
            APUSR_HOST_DISCOVERY_LOG_COMPILED=1
            APUSR_VK_HOST_CAPTURE_BUILD=1
            APUSR_VULKAN_FSR4_REQUIRES_PACKED_SIGNED_ACC_SAT=${APUSR_VULKAN_FSR4_REQUIRES_PACKED_SIGNED_ACC_SAT}
        )
        set_target_properties(apusr_vulkan_host_capture PROPERTIES
            OUTPUT_NAME "apusr_vulkan_host_capture"
        )

        # Vulkan Streamline integration: attach privately to the exact stock
        # interposer by named exports. Normally it is the host-loaded canonical
        # module; the optional loader bootstrap can supply the preserved stock
        # sidecar explicitly. Stock sl.dlss remains the Vulkan DLSS-SR translator
        # and calls APUSR through nvngx_dlss.dll.
        add_library(apusr_streamline_attach SHARED
            src/frontends/streamline_attach/apusr_streamline_attach.cpp
            src/frontends/identity_spoof/apusr_identity_spoof_hooks.cpp
            src/frontends/streamline_attach/apusr_streamline_attach.def
        )
        apusr_apply_host_cxx_contract(apusr_streamline_attach)
        target_link_libraries(apusr_streamline_attach PRIVATE
            apusr_detours
            apusr_vulkan_host_capture
            apusr_framegen_vk_runtime
            version)
        target_compile_definitions(apusr_streamline_attach PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _CRT_SECURE_NO_WARNINGS
            APUSR_HOST_DISCOVERY_LOG_COMPILED=1
            APUSR_STREAMLINE_ATTACH_MODE=1
        )
        set_target_properties(apusr_streamline_attach PROPERTIES
            OUTPUT_NAME "apusr_streamline_attach"
        )
        add_custom_command(TARGET apusr_streamline_attach POST_BUILD
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_streamline_stock_attach_contract.py"
                --dll "$<TARGET_FILE:apusr_streamline_attach>"
            VERBATIM)

        # Optional loader-only Streamline bootstrap for hosts that enter stock
        # slInit before DXGI/NVAPI/NGX can expose APUSR. Keep one canonical
        # name+ordinal surface, then generate two DEFs from it: one for APUSR's
        # public sl.interposer forwarder surface and one for a synthetic import
        # library matching the preserved stock sidecar. The import library exists
        # only because MSVC LINK validates named forwarders against linked imports.
        set(_apusr_streamline_bootstrap_exports
            "${CMAKE_CURRENT_SOURCE_DIR}/src/frontends/streamline_bootstrap_proxy/apusr_streamline_bootstrap_exports.def.inc")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${_apusr_streamline_bootstrap_exports}")
        # DEF comments use semicolons, which are list separators in CMake.
        # Select export records before converting the file into a CMake list.
        file(STRINGS "${_apusr_streamline_bootstrap_exports}" _apusr_streamline_bootstrap_export_lines
            REGEX "^[ \\t]*[A-Za-z_]")
        list(LENGTH _apusr_streamline_bootstrap_export_lines _apusr_streamline_bootstrap_export_count)
        if(NOT _apusr_streamline_bootstrap_export_count EQUAL 262)
            message(FATAL_ERROR
                "Expected 262 Streamline bootstrap exports, found ${_apusr_streamline_bootstrap_export_count}")
        endif()
        set(_apusr_streamline_bootstrap_generated_dir
            "${CMAKE_CURRENT_BINARY_DIR}/generated/streamline_bootstrap_proxy")
        file(MAKE_DIRECTORY "${_apusr_streamline_bootstrap_generated_dir}")
        set(_apusr_streamline_bootstrap_def
            "${_apusr_streamline_bootstrap_generated_dir}/apusr_streamline_bootstrap_proxy.def")
        set(_apusr_streamline_stock_import_def
            "${_apusr_streamline_bootstrap_generated_dir}/apusr_streamline_stock_import.def")
        file(WRITE "${_apusr_streamline_bootstrap_def}" "LIBRARY sl.interposer.dll\nEXPORTS\n")
        file(WRITE "${_apusr_streamline_stock_import_def}" "LIBRARY sl_interposer_apusr_stock.dll\nEXPORTS\n")
        foreach(_apusr_streamline_export_line IN LISTS _apusr_streamline_bootstrap_export_lines)
            string(STRIP "${_apusr_streamline_export_line}" _apusr_streamline_export_line)
            if(_apusr_streamline_export_line STREQUAL "")
                continue()
            endif()
            string(REGEX MATCH "^([^ \t]+)[ \t]+(@[0-9]+)$" _apusr_streamline_export_match
                "${_apusr_streamline_export_line}")
            if(NOT _apusr_streamline_export_match)
                message(FATAL_ERROR
                    "Invalid Streamline bootstrap export contract line: ${_apusr_streamline_export_line}")
            endif()
            set(_apusr_streamline_export_name "${CMAKE_MATCH_1}")
            set(_apusr_streamline_export_ordinal "${CMAKE_MATCH_2}")
            file(APPEND "${_apusr_streamline_stock_import_def}"
                "    ${_apusr_streamline_export_name} ${_apusr_streamline_export_ordinal}\n")
            if(_apusr_streamline_export_name STREQUAL "slInit")
                file(APPEND "${_apusr_streamline_bootstrap_def}"
                    "    slInit ${_apusr_streamline_export_ordinal}\n")
            else()
                file(APPEND "${_apusr_streamline_bootstrap_def}"
                    "    ${_apusr_streamline_export_name}=sl_interposer_apusr_stock.${_apusr_streamline_export_name} ${_apusr_streamline_export_ordinal}\n")
            endif()
        endforeach()

        # MSVC LINK validates named PE forwarders through the target DLL/import
        # library. The stock game DLL does not exist at build time, so generate
        # its import contract from the same canonical export list and link that
        # library only to satisfy validation. The emitted 261 passthrough exports
        # remain true PE forwarders; only slInit has local code.
        set(_apusr_streamline_stock_import_lib
            "${_apusr_streamline_bootstrap_generated_dir}/sl_interposer_apusr_stock.lib")
        add_custom_command(
            OUTPUT "${_apusr_streamline_stock_import_lib}"
            COMMAND "${CMAKE_AR}" /nologo /machine:x64
                "/def:${_apusr_streamline_stock_import_def}"
                "/out:${_apusr_streamline_stock_import_lib}"
            DEPENDS "${_apusr_streamline_stock_import_def}"
            COMMENT "Generating preserved-stock Streamline import library"
            VERBATIM)
        add_custom_target(apusr_streamline_stock_import_library
            DEPENDS "${_apusr_streamline_stock_import_lib}")

        add_library(apusr_streamline_bootstrap_proxy SHARED
            src/frontends/streamline_bootstrap_proxy/apusr_streamline_bootstrap_proxy.cpp
            "${_apusr_streamline_bootstrap_def}"
        )
        add_dependencies(apusr_streamline_bootstrap_proxy
            apusr_streamline_stock_import_library)
        apusr_apply_host_cxx_contract(apusr_streamline_bootstrap_proxy)
        target_link_libraries(apusr_streamline_bootstrap_proxy PRIVATE
            "${_apusr_streamline_stock_import_lib}")
        target_compile_definitions(apusr_streamline_bootstrap_proxy PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _CRT_SECURE_NO_WARNINGS
            APUSR_HOST_DISCOVERY_LOG_COMPILED=1
        )
        set_target_properties(apusr_streamline_bootstrap_proxy PROPERTIES
            OUTPUT_NAME "apusr_sl_interposer_bootstrap"
        )
        add_custom_command(TARGET apusr_streamline_bootstrap_proxy POST_BUILD
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_streamline_bootstrap_proxy_contract.py"
                --dll "$<TARGET_FILE:apusr_streamline_bootstrap_proxy>"
            VERBATIM)

        endif() # APUSR_ENABLE_VULKAN_BACKEND

        add_library(apusr_dxgi_spoof_proxy SHARED
            src/frontends/dxgi_spoof_proxy/apusr_dxgi_spoof_proxy.cpp
            src/frontends/identity_spoof/apusr_identity_spoof_hooks.cpp
            src/frontends/dxgi_spoof_proxy/apusr_dxgi_spoof_proxy.def
        )
        apusr_apply_host_cxx_contract(apusr_dxgi_spoof_proxy)
        target_compile_definitions(apusr_dxgi_spoof_proxy PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _CRT_SECURE_NO_WARNINGS
        )
        set_target_properties(apusr_dxgi_spoof_proxy PROPERTIES
            OUTPUT_NAME "dxgi"
        )

        # D3D12 DLSS compatibility package. Load early through DXGI, preserve
        # the game's stock Streamline evaluator, provide coherent NVAPI identity,
        # and bridge stock sl.dlss to APUSR at the NGXContext boundary when the
        # NVIDIA signed-core loader cannot load an unsigned replacement. Vulkan
        # backend assets belong only to Vulkan-capable packages.
        function(apusr_add_dlss_compatibility_package target_name package_dir)
            set(_apusr_dlss_compatibility_commands
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${package_dir}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/dlss_compatibility/README.txt"
                    "${package_dir}/README.txt"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_dxgi_spoof_proxy>"
                    "${package_dir}/dxgi.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_nvapi_host_proxy>"
                    "${package_dir}/$<TARGET_FILE_NAME:apusr_nvapi_host_proxy>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_ngx_dlss_dx12>"
                    "${package_dir}/$<TARGET_FILE_NAME:apusr_ngx_dlss_dx12>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_nvngx_alias_forwarder>"
                    "${package_dir}/nvngx.dll")
            set(_apusr_dlss_compatibility_dependencies
                apusr_dxgi_spoof_proxy apusr_nvapi_host_proxy apusr_ngx_dlss_dx12 apusr_nvngx_alias_forwarder)
            add_custom_target(${target_name}
                ${_apusr_dlss_compatibility_commands}
                DEPENDS ${_apusr_dlss_compatibility_dependencies}
                VERBATIM
            )
            apusr_attach_package_legal_files(${target_name} "${package_dir}")
        endfunction()

        apusr_add_dlss_compatibility_package(
            apusr_package_dlss_compatibility
            "${CMAKE_BINARY_DIR}/package_dlss_compatibility")

        if(APUSR_ENABLE_VULKAN_BACKEND)
            # APUSR captures the real system Vulkan loader at process scope, independently
            # of Streamline's own Vulkan topology. The stable host-capture module owns the
            # cold instance/device/queue boundary; Streamline remains a frontend that
            # consumes that captured context and supplies frame/tag metadata.
            add_custom_target(apusr_package_streamline_vulkan_compatibility
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_BINARY_DIR}/package_streamline_vulkan_compatibility"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/streamline_vulkan_compatibility/README.txt"
                    "${CMAKE_BINARY_DIR}/package_streamline_vulkan_compatibility/README.txt"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_streamline_bootstrap_proxy>"
                    "${CMAKE_BINARY_DIR}/package_streamline_vulkan_compatibility/apusr_sl_interposer_bootstrap.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/streamline_bootstrap/install_apusr_streamline_bootstrap.bat"
                    "${CMAKE_BINARY_DIR}/package_streamline_vulkan_compatibility/install_apusr_streamline_bootstrap.bat"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/streamline_bootstrap/README.txt"
                    "${CMAKE_BINARY_DIR}/package_streamline_vulkan_compatibility/README_streamline_bootstrap.txt"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_nvapi_host_proxy>"
                    "${CMAKE_BINARY_DIR}/package_streamline_vulkan_compatibility/$<TARGET_FILE_NAME:apusr_nvapi_host_proxy>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_ngx_dlss_dx12>"
                    "${CMAKE_BINARY_DIR}/package_streamline_vulkan_compatibility/$<TARGET_FILE_NAME:apusr_ngx_dlss_dx12>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_nvngx_alias_forwarder>"
                    "${CMAKE_BINARY_DIR}/package_streamline_vulkan_compatibility/nvngx.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_streamline_attach>"
                    "${CMAKE_BINARY_DIR}/package_streamline_vulkan_compatibility/$<TARGET_FILE_NAME:apusr_streamline_attach>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_vulkan_host_capture>"
                    "${CMAKE_BINARY_DIR}/package_streamline_vulkan_compatibility/$<TARGET_FILE_NAME:apusr_vulkan_host_capture>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${APUSR_FSR4_VULKAN_SHADER_PACK}"
                    "${CMAKE_BINARY_DIR}/package_streamline_vulkan_compatibility/apusr_fsr4_int8_vulkan.shaderpack"
                DEPENDS apusr_nvapi_host_proxy apusr_ngx_dlss_dx12 apusr_nvngx_alias_forwarder
                    apusr_streamline_attach apusr_streamline_bootstrap_proxy apusr_vulkan_host_capture
                    apusr_generate_fsr4_vulkan_shader_pack
                VERBATIM
            )
            apusr_attach_package_legal_files(
                apusr_package_streamline_vulkan_compatibility
                "${CMAKE_BINARY_DIR}/package_streamline_vulkan_compatibility"
                WITH_DETOURS)

            # Combined D3D12 + Vulkan package for Streamline games that can switch
            # render backends. The default install leaves the game's stock
            # sl.interposer.dll at the loader boundary. An inactive loader-only
            # bootstrap + installer are staged for hosts that enter slInit before
            # APUSR can otherwise become visible; the exact stock PE remains the
            # semantic backing module. Stock sl.dlss continues to own
            # Streamline-to-NGX translation for DLSS-SR on every backend.
            add_custom_target(apusr_package_dlss_multibackend_compatibility
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_BINARY_DIR}/package_dlss_multibackend_compatibility"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/dlss_multibackend_compatibility/README.txt"
                    "${CMAKE_BINARY_DIR}/package_dlss_multibackend_compatibility/README.txt"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_streamline_bootstrap_proxy>"
                    "${CMAKE_BINARY_DIR}/package_dlss_multibackend_compatibility/apusr_sl_interposer_bootstrap.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/streamline_bootstrap/install_apusr_streamline_bootstrap.bat"
                    "${CMAKE_BINARY_DIR}/package_dlss_multibackend_compatibility/install_apusr_streamline_bootstrap.bat"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/streamline_bootstrap/README.txt"
                    "${CMAKE_BINARY_DIR}/package_dlss_multibackend_compatibility/README_streamline_bootstrap.txt"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_dxgi_spoof_proxy>"
                    "${CMAKE_BINARY_DIR}/package_dlss_multibackend_compatibility/dxgi.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_nvapi_host_proxy>"
                    "${CMAKE_BINARY_DIR}/package_dlss_multibackend_compatibility/$<TARGET_FILE_NAME:apusr_nvapi_host_proxy>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_ngx_dlss_dx12>"
                    "${CMAKE_BINARY_DIR}/package_dlss_multibackend_compatibility/$<TARGET_FILE_NAME:apusr_ngx_dlss_dx12>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_nvngx_alias_forwarder>"
                    "${CMAKE_BINARY_DIR}/package_dlss_multibackend_compatibility/nvngx.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_streamline_attach>"
                    "${CMAKE_BINARY_DIR}/package_dlss_multibackend_compatibility/$<TARGET_FILE_NAME:apusr_streamline_attach>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_vulkan_host_capture>"
                    "${CMAKE_BINARY_DIR}/package_dlss_multibackend_compatibility/$<TARGET_FILE_NAME:apusr_vulkan_host_capture>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${APUSR_FSR4_VULKAN_SHADER_PACK}"
                    "${CMAKE_BINARY_DIR}/package_dlss_multibackend_compatibility/apusr_fsr4_int8_vulkan.shaderpack"
                DEPENDS apusr_dxgi_spoof_proxy apusr_nvapi_host_proxy apusr_ngx_dlss_dx12
                    apusr_nvngx_alias_forwarder apusr_streamline_attach apusr_streamline_bootstrap_proxy
                    apusr_vulkan_host_capture apusr_generate_fsr4_vulkan_shader_pack
                VERBATIM
            )
            apusr_attach_package_legal_files(
                apusr_package_dlss_multibackend_compatibility
                "${CMAKE_BINARY_DIR}/package_dlss_multibackend_compatibility"
                WITH_DETOURS)
        endif()
    endif()

endif()



if(APUSR_BUILD_TESTS)
    enable_testing()

    add_test(NAME apusr_repository_licensing_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_repository_licensing_contract.py")
    set_tests_properties(apusr_repository_licensing_contract PROPERTIES
        LABELS "product;quality;repository"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME apusr_streamline_stock_attach_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_streamline_stock_attach_contract.py")
    set_tests_properties(apusr_streamline_stock_attach_contract PROPERTIES
        LABELS "product;quality;frontend"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME apusr_streamline_bootstrap_proxy_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_streamline_bootstrap_proxy_contract.py")
    set_tests_properties(apusr_streamline_bootstrap_proxy_contract PROPERTIES
        LABELS "product;quality;frontend"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME apusr_logging_volume_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_logging_volume_contract.py")
    set_tests_properties(apusr_logging_volume_contract PROPERTIES
        LABELS "product;quality;frontend;diagnostics"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME apusr_profiler_fail_open_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_profiler_fail_open_contract.py")
    set_tests_properties(apusr_profiler_fail_open_contract PROPERTIES
        LABELS "product;quality;profiling"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME apusr_vulkan_optimization_profiler_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_vulkan_optimization_profiler_contract.py")
    set_tests_properties(apusr_vulkan_optimization_profiler_contract PROPERTIES
        LABELS "product;quality;profiling;vulkan"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME apusr_fsr4_shader_compile_dedup_smoke
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_fsr4_shader_compile_dedup_smoke.py")
    set_tests_properties(apusr_fsr4_shader_compile_dedup_smoke PROPERTIES
        LABELS "product;quality"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME apusr_post_gaussian_centering_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_post_gaussian_centering_contract.py")
    set_tests_properties(apusr_post_gaussian_centering_contract PROPERTIES
        LABELS "product;quality;shader;fsr4"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME apusr_vulkan_bookend_input_cache_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_vulkan_bookend_input_cache_contract.py")
    set_tests_properties(apusr_vulkan_bookend_input_cache_contract PROPERTIES
        LABELS "product;quality;shader;vulkan;fsr4"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME apusr_spirv_dot_accumulate_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_spirv_dot_accumulate_contract.py")
    set_tests_properties(apusr_spirv_dot_accumulate_contract PROPERTIES
        LABELS "product;quality;shader;vulkan"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")


    add_test(NAME apusr_framegen_shader_ownership_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_framegen_shader_ownership_contract.py")
    set_tests_properties(apusr_framegen_shader_ownership_contract PROPERTIES
        LABELS "product;quality;shader"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME apusr_dx12_shader_fingerprint_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_dx12_shader_fingerprint_contract.py")
    set_tests_properties(apusr_dx12_shader_fingerprint_contract PROPERTIES
        LABELS "product;quality;shader"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    if(WIN32 OR (UNIX AND NOT APPLE))
        add_test(NAME apusr_dx12_shader_compile_check
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_dx12_shaders.py"
                --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
                --output "${APUSR_DX12_SHADER_BLOBS_HEADER}"
                --verify
                --shader-timeout-seconds "${APUSR_SHADER_COMPILER_CHILD_TIMEOUT_SECONDS}"
        )
        set_tests_properties(apusr_dx12_shader_compile_check PROPERTIES
            LABELS "product;quality;shader"
            TIMEOUT "${APUSR_SHADER_TEST_TIMEOUT_SECONDS}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        )

        add_test(NAME apusr_fsr4_reference_shader_quick_preflight
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/model/verify_fsr4_reference_shaders.py"
                --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
                --preset all
                --capacity all
                --quick
                --include-utility
                --timeout-seconds "${APUSR_SHADER_COMPILER_CHILD_TIMEOUT_SECONDS}"
        )
        set_tests_properties(apusr_fsr4_reference_shader_quick_preflight PROPERTIES
            LABELS "developer;quality;shader"
            TIMEOUT "${APUSR_SHADER_TEST_TIMEOUT_SECONDS}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        )
    endif()

    function(apusr_register_product_test target_name)
        apusr_apply_host_cxx_contract(${target_name})
        if(MSVC)
            target_compile_options(${target_name} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${target_name} PRIVATE -UNDEBUG)
        endif()
        add_test(NAME "${target_name}" COMMAND "$<TARGET_FILE:${target_name}>")
        set_property(GLOBAL APPEND PROPERTY APUSR_QUALITY_TARGETS "${target_name}")
        set_tests_properties("${target_name}" PROPERTIES
            LABELS "product;quality"
            TIMEOUT "${APUSR_PRODUCT_TEST_TIMEOUT_SECONDS}"
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target_name}>"
        )
    endfunction()

    add_executable(apusr_framegen_dxil_pack_contract
        tests/apusr_framegen_dxil_pack_contract.cpp
        src/framegen/algorithm/generated/apusr_framegen_dxil.cpp)
    target_include_directories(apusr_framegen_dxil_pack_contract PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm/generated")
    apusr_register_product_test(apusr_framegen_dxil_pack_contract)

    add_executable(apusr_smoke tests/apusr_smoke.cpp)
    target_link_libraries(apusr_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_smoke)

    if(APUSR_ENABLE_PROFILER)
        add_executable(apusr_profiler_sample_gate_contract tests/apusr_profiler_sample_gate_contract.cpp)
        target_link_libraries(apusr_profiler_sample_gate_contract PRIVATE apusr_profiler_runtime)
        apusr_register_product_test(apusr_profiler_sample_gate_contract)

        add_executable(apusr_profiler_structured_contract tests/apusr_profiler_structured_contract.cpp)
        target_link_libraries(apusr_profiler_structured_contract PRIVATE apusr_profiler_runtime)
        apusr_register_product_test(apusr_profiler_structured_contract)
        add_test(NAME apusr_profiler_report_structured_contract
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_profiler_report_structured_contract.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/profiler/apusr_profile_report.py"
                "$<TARGET_FILE_DIR:apusr_profiler_structured_contract>/profiler-structured-contract-output"
                "$<TARGET_FILE_DIR:apusr_profiler_structured_contract>/profiler-structured-contract-report")
        set_tests_properties(apusr_profiler_report_structured_contract PROPERTIES
            DEPENDS apusr_profiler_structured_contract
            LABELS "product;quality"
            TIMEOUT "${APUSR_PRODUCT_TEST_TIMEOUT_SECONDS}"
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:apusr_profiler_structured_contract>")
        add_test(NAME apusr_profiler_compare_contract
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/apusr_profiler_compare_contract.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/profiler/apusr_profile_compare.py"
                "$<TARGET_FILE_DIR:apusr_profiler_structured_contract>/profiler-structured-contract-report"
                "$<TARGET_FILE_DIR:apusr_profiler_structured_contract>/profiler-structured-contract-compare")
        set_tests_properties(apusr_profiler_compare_contract PROPERTIES
            DEPENDS apusr_profiler_report_structured_contract
            LABELS "product;quality"
            TIMEOUT "${APUSR_PRODUCT_TEST_TIMEOUT_SECONDS}"
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:apusr_profiler_structured_contract>")
    endif()

    add_executable(apusr_schedule_smoke tests/apusr_schedule_smoke.cpp)
    target_link_libraries(apusr_schedule_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_schedule_smoke)

    add_executable(apusr_pipeline_smoke tests/apusr_pipeline_smoke.cpp)
    target_link_libraries(apusr_pipeline_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_pipeline_smoke)

    add_executable(apusr_fsr4_real_boundary_smoke tests/apusr_fsr4_real_boundary_smoke.cpp)
    target_link_libraries(apusr_fsr4_real_boundary_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_fsr4_real_boundary_smoke)

    add_executable(apusr_upscaler_common_smoke tests/apusr_upscaler_common_smoke.cpp)
    target_link_libraries(apusr_upscaler_common_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_upscaler_common_smoke)

    add_executable(apusr_middle_core_resident_abi_smoke tests/apusr_middle_core_resident_abi_smoke.cpp)
    target_link_libraries(apusr_middle_core_resident_abi_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_middle_core_resident_abi_smoke)

    add_executable(apusr_middle_core_spatial_layout_smoke tests/apusr_middle_core_spatial_layout_smoke.cpp)
    target_link_libraries(apusr_middle_core_spatial_layout_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_middle_core_spatial_layout_smoke)

    add_executable(apusr_middle_core_spatial_row_ring_smoke tests/apusr_middle_core_spatial_row_ring_smoke.cpp)
    target_link_libraries(apusr_middle_core_spatial_row_ring_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_middle_core_spatial_row_ring_smoke)

    add_executable(apusr_middle_core_front_c16_row_kernel_smoke tests/apusr_middle_core_front_c16_row_kernel_smoke.cpp)
    target_link_libraries(apusr_middle_core_front_c16_row_kernel_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_middle_core_front_c16_row_kernel_smoke)

    add_executable(apusr_middle_core_front_c16_row_halo_kernel_smoke tests/apusr_middle_core_front_c16_row_halo_kernel_smoke.cpp)
    target_link_libraries(apusr_middle_core_front_c16_row_halo_kernel_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_middle_core_front_c16_row_halo_kernel_smoke)

    add_executable(apusr_middle_core_aie_vector_contract_smoke tests/apusr_middle_core_aie_vector_contract_smoke.cpp)
    target_link_libraries(apusr_middle_core_aie_vector_contract_smoke PRIVATE apusr::core)
    target_compile_definitions(apusr_middle_core_aie_vector_contract_smoke
        PRIVATE APUSR_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    apusr_register_product_test(apusr_middle_core_aie_vector_contract_smoke)

    add_executable(apusr_token_planner_smoke tests/apusr_token_planner_smoke.cpp)
    target_link_libraries(apusr_token_planner_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_token_planner_smoke)

    add_executable(apusr_host_capture_smoke tests/apusr_host_capture_smoke.cpp)
    target_link_libraries(apusr_host_capture_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_host_capture_smoke)

    add_executable(apusr_vulkan_antilag_present_contract
        tests/apusr_vulkan_antilag_present_contract.cpp
        src/frontends/vulkan_host_capture/apusr_vulkan_host_capture_core.cpp)
    target_link_libraries(apusr_vulkan_antilag_present_contract PRIVATE apusr_profiler_runtime)
    target_include_directories(apusr_vulkan_antilag_present_contract PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/frontends/vulkan_host_capture"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/backends/gfx_vulkan")
    target_compile_definitions(apusr_vulkan_antilag_present_contract PRIVATE
        APUSR_VULKAN_FSR4_REQUIRES_PACKED_SIGNED_ACC_SAT=${APUSR_VULKAN_FSR4_REQUIRES_PACKED_SIGNED_ACC_SAT})
    apusr_register_product_test(apusr_vulkan_antilag_present_contract)

    add_executable(apusr_streamline_dlssg_abi_contract
        tests/apusr_streamline_dlssg_abi_contract.cpp)
    target_include_directories(apusr_streamline_dlssg_abi_contract PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/frontends/streamline_attach")
    apusr_register_product_test(apusr_streamline_dlssg_abi_contract)

    add_executable(apusr_streamline_feature_ownership_contract
        tests/apusr_streamline_feature_ownership_contract.cpp)
    target_include_directories(apusr_streamline_feature_ownership_contract PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/frontends/streamline_attach")
    apusr_register_product_test(apusr_streamline_feature_ownership_contract)

    if(APUSR_ENABLE_VULKAN_BACKEND)
        add_executable(apusr_framegen_vulkan_backend_contract
            tests/apusr_framegen_vulkan_backend_contract.cpp)
        target_link_libraries(apusr_framegen_vulkan_backend_contract PRIVATE apusr_framegen_vk_runtime)
        target_include_directories(apusr_framegen_vulkan_backend_contract PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/backends/gfx_vulkan"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm/internal"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm/generated"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/ffx_compat/abi"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/ffx_vk/generated")
        target_compile_definitions(apusr_framegen_vulkan_backend_contract PRIVATE
            FFX_DX12_ENABLE_GPU_MARKERS=0)
        apusr_register_product_test(apusr_framegen_vulkan_backend_contract)
    endif()

    add_executable(apusr_framegen_job_contract tests/apusr_framegen_job_contract.cpp)
    target_include_directories(apusr_framegen_job_contract PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/algorithm/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen/ffx_compat/abi")
    target_compile_definitions(apusr_framegen_job_contract PRIVATE
        FFX_DX12_ENABLE_GPU_MARKERS=0)
    apusr_register_product_test(apusr_framegen_job_contract)

    if(WIN32)
        add_executable(apusr_framegen_waitable_contract
            tests/apusr_framegen_waitable_contract.cpp
            src/framegen/ffx_dx12/apusr_framegen_frame_latency_dx12.cpp)
        target_include_directories(apusr_framegen_waitable_contract PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/framegen")
        target_link_libraries(apusr_framegen_waitable_contract PRIVATE d3d12)
        apusr_register_product_test(apusr_framegen_waitable_contract)
    endif()

    add_executable(apusr_execution_plan_smoke tests/apusr_execution_plan_smoke.cpp)
    target_link_libraries(apusr_execution_plan_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_execution_plan_smoke)


    add_executable(apusr_fsr4_int8_graph_smoke tests/apusr_fsr4_int8_graph_smoke.cpp)
    target_link_libraries(apusr_fsr4_int8_graph_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_fsr4_int8_graph_smoke)

    add_executable(apusr_fsr4_int8_tiled_fusion_plan_smoke tests/apusr_fsr4_int8_tiled_fusion_plan_smoke.cpp)
    target_link_libraries(apusr_fsr4_int8_tiled_fusion_plan_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_fsr4_int8_tiled_fusion_plan_smoke)

    add_executable(apusr_fsr4_int8_tiled_fusion_shader_contract_smoke tests/apusr_fsr4_int8_tiled_fusion_shader_contract_smoke.cpp)
    target_link_libraries(apusr_fsr4_int8_tiled_fusion_shader_contract_smoke PRIVATE apusr::core)
    target_compile_definitions(apusr_fsr4_int8_tiled_fusion_shader_contract_smoke PRIVATE APUSR_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    apusr_register_product_test(apusr_fsr4_int8_tiled_fusion_shader_contract_smoke)

    add_executable(apusr_fsr4_int8_scratch_layout_smoke tests/apusr_fsr4_int8_scratch_layout_smoke.cpp)
    target_link_libraries(apusr_fsr4_int8_scratch_layout_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_fsr4_int8_scratch_layout_smoke)

    add_executable(apusr_fsr4_int8_artifact_manifest_smoke tests/apusr_fsr4_int8_artifact_manifest_smoke.cpp)
    target_link_libraries(apusr_fsr4_int8_artifact_manifest_smoke PRIVATE apusr::core)
    target_compile_definitions(apusr_fsr4_int8_artifact_manifest_smoke PRIVATE APUSR_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    apusr_register_product_test(apusr_fsr4_int8_artifact_manifest_smoke)

    add_executable(apusr_fsr4_int8_resource_plan_smoke tests/apusr_fsr4_int8_resource_plan_smoke.cpp)
    target_link_libraries(apusr_fsr4_int8_resource_plan_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_fsr4_int8_resource_plan_smoke)

    add_executable(apusr_fsr4_int8_include_closure_smoke tests/apusr_fsr4_int8_include_closure_smoke.cpp)
    target_link_libraries(apusr_fsr4_int8_include_closure_smoke PRIVATE apusr::core)
    target_compile_definitions(apusr_fsr4_int8_include_closure_smoke PRIVATE APUSR_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    apusr_register_product_test(apusr_fsr4_int8_include_closure_smoke)

    add_executable(apusr_fsr4_int8_reference_runner_smoke tests/apusr_fsr4_int8_reference_runner_smoke.cpp)
    target_link_libraries(apusr_fsr4_int8_reference_runner_smoke PRIVATE apusr::core)
    target_compile_definitions(apusr_fsr4_int8_reference_runner_smoke PRIVATE APUSR_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    apusr_register_product_test(apusr_fsr4_int8_reference_runner_smoke)

    add_executable(apusr_fsr4_reference_dx12_plan_smoke tests/apusr_fsr4_reference_dx12_plan_smoke.cpp)
    target_link_libraries(apusr_fsr4_reference_dx12_plan_smoke PRIVATE apusr::core)
    target_compile_definitions(apusr_fsr4_reference_dx12_plan_smoke PRIVATE
        APUSR_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
        APUSR_REFLECTION_MANIFEST_PATH="${CMAKE_CURRENT_BINARY_DIR}/fsr4_reference_dx12_shader_resource_manifest.quick.json")
    apusr_register_product_test(apusr_fsr4_reference_dx12_plan_smoke)

    add_executable(apusr_fsr4_reference_dx12_record_smoke tests/apusr_fsr4_reference_dx12_record_smoke.cpp)
    target_link_libraries(apusr_fsr4_reference_dx12_record_smoke PRIVATE apusr::core)
    target_compile_definitions(apusr_fsr4_reference_dx12_record_smoke PRIVATE
        APUSR_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    if(WIN32)
        target_link_libraries(apusr_fsr4_reference_dx12_record_smoke PRIVATE d3d12 dxgi)
    endif()
    apusr_register_product_test(apusr_fsr4_reference_dx12_record_smoke)
    set_tests_properties(apusr_fsr4_reference_dx12_record_smoke PROPERTIES SKIP_RETURN_CODE 77)

    if(APUSR_ENABLE_VULKAN_BACKEND AND UNIX)
        add_library(apusr_fake_vulkan SHARED tests/apusr_fake_vulkan.cpp)
        set_target_properties(apusr_fake_vulkan PROPERTIES OUTPUT_NAME vulkan SOVERSION 1)
        apusr_apply_host_cxx_contract(apusr_fake_vulkan)

        add_executable(apusr_fsr4_reference_vulkan_record_smoke tests/apusr_fsr4_reference_vulkan_record_smoke.cpp)
        target_link_libraries(apusr_fsr4_reference_vulkan_record_smoke PRIVATE apusr::core apusr_fake_vulkan)
        target_compile_definitions(apusr_fsr4_reference_vulkan_record_smoke PRIVATE
            APUSR_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
        apusr_register_product_test(apusr_fsr4_reference_vulkan_record_smoke)
        set_tests_properties(apusr_fsr4_reference_vulkan_record_smoke PROPERTIES
            ENVIRONMENT "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:apusr_fake_vulkan>")

        add_executable(apusr_fsr4_reference_vulkan_spirv_contract tests/apusr_fsr4_reference_vulkan_spirv_contract.cpp)
        target_link_libraries(apusr_fsr4_reference_vulkan_spirv_contract PRIVATE apusr::core)
        target_compile_definitions(apusr_fsr4_reference_vulkan_spirv_contract PRIVATE
            APUSR_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
        apusr_register_product_test(apusr_fsr4_reference_vulkan_spirv_contract)
    endif()

    add_executable(apusr_fsr4_reference_dx12_output_smoke tests/apusr_fsr4_reference_dx12_output_smoke.cpp)
    target_link_libraries(apusr_fsr4_reference_dx12_output_smoke PRIVATE apusr::core)
    target_compile_definitions(apusr_fsr4_reference_dx12_output_smoke PRIVATE
        APUSR_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    if(WIN32)
        target_link_libraries(apusr_fsr4_reference_dx12_output_smoke PRIVATE d3d12 dxgi)
    endif()
    apusr_register_product_test(apusr_fsr4_reference_dx12_output_smoke)
    set_tests_properties(apusr_fsr4_reference_dx12_output_smoke PROPERTIES SKIP_RETURN_CODE 77)

    add_custom_target(apusr_fsr4_reference_planning_smokes
        DEPENDS
            apusr_fsr4_int8_graph_smoke
            apusr_fsr4_int8_tiled_fusion_plan_smoke
            apusr_fsr4_int8_tiled_fusion_shader_contract_smoke
            apusr_fsr4_int8_scratch_layout_smoke
            apusr_fsr4_int8_resource_plan_smoke
            apusr_fsr4_int8_reference_runner_smoke
            apusr_fsr4_reference_dx12_plan_smoke
    )

    add_custom_target(apusr_fsr4_reference_dx12_runtime_smokes
        DEPENDS
            apusr_fsr4_reference_dx12_plan_smoke
            apusr_fsr4_reference_dx12_record_smoke
            apusr_fsr4_reference_dx12_output_smoke
    )

    add_executable(apusr_frame_input_contract_smoke tests/apusr_frame_input_contract_smoke.cpp)
    target_link_libraries(apusr_frame_input_contract_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_frame_input_contract_smoke)

    add_executable(apusr_frontend_smoke tests/apusr_frontend_smoke.cpp)
    target_link_libraries(apusr_frontend_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_frontend_smoke)

    add_executable(apusr_protocol_smoke tests/apusr_protocol_smoke.cpp)
    target_link_libraries(apusr_protocol_smoke PRIVATE apusr::core)
    apusr_register_product_test(apusr_protocol_smoke)

    add_executable(apusr_ngx_dlss_frontend_smoke
        tests/apusr_ngx_dlss_frontend_smoke.cpp
        src/frontends/ngx_dlss_dx12/apusr_ngx_dlss_dx12_frontend.cpp
    )
    target_link_libraries(apusr_ngx_dlss_frontend_smoke PRIVATE apusr::core)
    if(WIN32)
        target_link_libraries(apusr_ngx_dlss_frontend_smoke PRIVATE d3d12 dxgi)
    endif()
    target_compile_definitions(apusr_ngx_dlss_frontend_smoke PRIVATE
        APUSR_NGX_DLSS_DX12_FRONTEND_EXPORTS=1
        APUSR_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    if(APUSR_ENABLE_VULKAN_BACKEND AND UNIX)
        target_link_libraries(apusr_ngx_dlss_frontend_smoke PRIVATE apusr_fake_vulkan)
    endif()
    apusr_register_product_test(apusr_ngx_dlss_frontend_smoke)
    if(APUSR_ENABLE_VULKAN_BACKEND AND UNIX)
        set_tests_properties(apusr_ngx_dlss_frontend_smoke PROPERTIES
            ENVIRONMENT "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:apusr_fake_vulkan>")
    endif()


    if(APUSR_BUILD_FRONTENDS)
        add_executable(apusr_fsr4_abi_smoke tests/apusr_fsr4_abi_smoke.cpp)
        target_link_libraries(apusr_fsr4_abi_smoke PRIVATE apusr_fsr4_dx12)
        if(WIN32)
            set(_apusr_fsr4_abi_isolated_dir "${CMAKE_BINARY_DIR}/tests/ffx_abi_isolated")
            set_target_properties(apusr_fsr4_abi_smoke PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${_apusr_fsr4_abi_isolated_dir}")
            add_custom_command(TARGET apusr_fsr4_abi_smoke POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_fsr4_dx12>"
                    "$<TARGET_FILE_DIR:apusr_fsr4_abi_smoke>"
                VERBATIM)
        endif()
        apusr_register_product_test(apusr_fsr4_abi_smoke)
        if(WIN32)
            set_tests_properties(apusr_fsr4_abi_smoke PROPERTIES
                ENVIRONMENT "APUSR_FFX_DX12_PASSTHROUGH_DLL="
                WORKING_DIRECTORY "$<TARGET_FILE_DIR:apusr_fsr4_dx12>")

            add_library(apusr_fake_ffx_passthrough SHARED tests/apusr_fake_ffx_passthrough.cpp)
            apusr_apply_host_cxx_contract(apusr_fake_ffx_passthrough)
            target_compile_definitions(apusr_fake_ffx_passthrough PRIVATE
                APUSR_FSR4_DX12_FRONTEND_EXPORTS=1)
            target_include_directories(apusr_fake_ffx_passthrough PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/include")
            set_target_properties(apusr_fake_ffx_passthrough PROPERTIES
                OUTPUT_NAME "apusr_fake_ffx_passthrough")

            add_executable(apusr_fsr4_passthrough_smoke tests/apusr_fsr4_passthrough_smoke.cpp)
            target_link_libraries(apusr_fsr4_passthrough_smoke PRIVATE apusr_fsr4_dx12)
            add_dependencies(apusr_fsr4_passthrough_smoke apusr_fake_ffx_passthrough)
            set(_apusr_fsr4_passthrough_isolated_dir "${CMAKE_BINARY_DIR}/tests/ffx_passthrough_isolated")
            set_target_properties(apusr_fsr4_passthrough_smoke PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${_apusr_fsr4_passthrough_isolated_dir}")
            add_custom_command(TARGET apusr_fsr4_passthrough_smoke POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:apusr_fsr4_dx12>"
                    "$<TARGET_FILE_DIR:apusr_fsr4_passthrough_smoke>"
                VERBATIM)
            apusr_register_product_test(apusr_fsr4_passthrough_smoke)
            set_tests_properties(apusr_fsr4_passthrough_smoke PROPERTIES
                ENVIRONMENT "APUSR_FFX_DX12_PASSTHROUGH_DLL=$<TARGET_FILE:apusr_fake_ffx_passthrough>"
                WORKING_DIRECTORY "$<TARGET_FILE_DIR:apusr_fsr4_dx12>")
        endif()

        add_executable(apusr_fsr4_normalization_smoke
            tests/apusr_fsr4_normalization_smoke.cpp
            "${APUSR_FSR4_DX12_EMBEDDED_XCLBIN_CPP}"
            src/frontends/ffx_fsr4_dx12/apusr_fsr4_dx12_runtime.cpp
        )
        target_link_libraries(apusr_fsr4_normalization_smoke PRIVATE apusr::core)
        target_compile_definitions(apusr_fsr4_normalization_smoke PRIVATE APUSR_REQUIRE_PACKAGED_NATIVE=$<BOOL:${APUSR_REQUIRE_PACKAGED_NATIVE}>)
        apusr_register_product_test(apusr_fsr4_normalization_smoke)

        add_executable(apusr_fsr4_dropin_config_smoke
            tests/apusr_fsr4_dropin_config_smoke.cpp
            "${APUSR_FSR4_DX12_EMBEDDED_XCLBIN_CPP}"
            src/frontends/ffx_fsr4_dx12/apusr_fsr4_dx12_runtime.cpp
        )
        target_link_libraries(apusr_fsr4_dropin_config_smoke PRIVATE apusr::core)
        target_compile_definitions(apusr_fsr4_dropin_config_smoke PRIVATE APUSR_REQUIRE_PACKAGED_NATIVE=$<BOOL:${APUSR_REQUIRE_PACKAGED_NATIVE}>)
        apusr_register_product_test(apusr_fsr4_dropin_config_smoke)

        set(_apusr_fsr4_dx12_frontend_smoke_targets
            apusr_fsr4_abi_smoke
            apusr_fsr4_normalization_smoke)
        if(WIN32)
            list(APPEND _apusr_fsr4_dx12_frontend_smoke_targets apusr_fsr4_passthrough_smoke)
        endif()
        add_custom_target(apusr_fsr4_dx12_frontend_smokes
            DEPENDS ${_apusr_fsr4_dx12_frontend_smoke_targets})

        add_custom_target(apusr_fsr4_dx12_reference_runtime_ready_smokes
            DEPENDS
                apusr_fsr4_reference_dx12_runtime_smokes
                apusr_fsr4_dx12_frontend_smokes
        )
    endif()

    get_property(_apusr_quality_targets GLOBAL PROPERTY APUSR_QUALITY_TARGETS)
    if(_apusr_quality_targets)
        add_custom_target(apusr_quality_tests DEPENDS ${_apusr_quality_targets})
    endif()

endif()
