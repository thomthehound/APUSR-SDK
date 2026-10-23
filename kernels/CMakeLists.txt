# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

if(APUSR_AIE_KERNEL_PROFILE STREQUAL "MiddleCoreStage1To12Spatial2Col")
    set(_apusr_middle_core_dir "apusr_middle_core_stage1_12_spatial2col")
elseif(APUSR_AIE_KERNEL_PROFILE STREQUAL "MiddleCoreStage1To12Spatial4Col")
    set(_apusr_middle_core_dir "apusr_middle_core_stage1_12_spatial4col")
else()
    message(FATAL_ERROR "Unsupported APUSR_AIE_KERNEL_PROFILE='${APUSR_AIE_KERNEL_PROFILE}'. The old full-arena two-column graph is quarantined; use MiddleCoreStage1To12Spatial2Col or MiddleCoreStage1To12Spatial4Col.")
endif()

set(APUSR_MIDDLE_CORE_STAGE1_12_KERNEL_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/${_apusr_middle_core_dir}")
set(APUSR_MIDDLE_CORE_STAGE1_12_MANIFEST
    "${APUSR_MIDDLE_CORE_STAGE1_12_KERNEL_ROOT}/manifest.json")
set(APUSR_MIDDLE_CORE_STAGE1_12_XCLBIN
    "${CMAKE_CURRENT_BINARY_DIR}/${_apusr_middle_core_dir}/${_apusr_middle_core_dir}.xclbin")
set(APUSR_MIDDLE_CORE_STAGE1_12_INSTS
    "${CMAKE_CURRENT_BINARY_DIR}/${_apusr_middle_core_dir}/${_apusr_middle_core_dir}.insts.bin")
set(APUSR_MIDDLE_CORE_STAGE1_12_PARAMETER_LOAD_INSTS
    "${CMAKE_CURRENT_BINARY_DIR}/${_apusr_middle_core_dir}/${_apusr_middle_core_dir}.load_params.insts.bin")

set(APUSR_MIDDLE_CORE_STAGE1_12_XCLBIN "${APUSR_MIDDLE_CORE_STAGE1_12_XCLBIN}" PARENT_SCOPE)
set(APUSR_MIDDLE_CORE_STAGE1_12_INSTS "${APUSR_MIDDLE_CORE_STAGE1_12_INSTS}" PARENT_SCOPE)
set(APUSR_MIDDLE_CORE_STAGE1_12_PARAMETER_LOAD_INSTS "${APUSR_MIDDLE_CORE_STAGE1_12_PARAMETER_LOAD_INSTS}" PARENT_SCOPE)

set(APUSR_FRAME_CONVEYOR_BENCHMARK_KERNEL_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/apusr_frame_conveyor_benchmark")
set(APUSR_FRAME_CONVEYOR_BENCHMARK_MANIFEST
    "${APUSR_FRAME_CONVEYOR_BENCHMARK_KERNEL_ROOT}/manifest.json")
set(APUSR_FRAME_CONVEYOR_BENCHMARK_XCLBIN
    "${CMAKE_CURRENT_BINARY_DIR}/apusr_frame_conveyor_benchmark/apusr_frame_conveyor_benchmark.xclbin")
set(APUSR_FRAME_CONVEYOR_BENCHMARK_INSTS
    "${CMAKE_CURRENT_BINARY_DIR}/apusr_frame_conveyor_benchmark/apusr_frame_conveyor_benchmark.insts.bin")

set(APUSR_FRAME_CONVEYOR_BENCHMARK_XCLBIN "${APUSR_FRAME_CONVEYOR_BENCHMARK_XCLBIN}" PARENT_SCOPE)
set(APUSR_FRAME_CONVEYOR_BENCHMARK_INSTS "${APUSR_FRAME_CONVEYOR_BENCHMARK_INSTS}" PARENT_SCOPE)

set(APUSR_FRAME_CONVEYOR_BENCHMARK_MANIFEST_STAMP
    "${CMAKE_CURRENT_BINARY_DIR}/apusr_frame_conveyor_benchmark_manifest.stamp")
add_custom_command(
    OUTPUT "${APUSR_FRAME_CONVEYOR_BENCHMARK_MANIFEST_STAMP}"
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/validate_kernel_manifest.py"
        "${APUSR_FRAME_CONVEYOR_BENCHMARK_MANIFEST}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${APUSR_FRAME_CONVEYOR_BENCHMARK_MANIFEST_STAMP}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/validate_kernel_manifest.py"
        "${APUSR_FRAME_CONVEYOR_BENCHMARK_MANIFEST}"
        "${APUSR_FRAME_CONVEYOR_BENCHMARK_KERNEL_ROOT}/src/apusr_frame_conveyor_benchmark.py"
    VERBATIM
)

file(GLOB_RECURSE APUSR_MIDDLE_CORE_STAGE1_12_SOURCES CONFIGURE_DEPENDS
    "${APUSR_MIDDLE_CORE_STAGE1_12_KERNEL_ROOT}/src/*.py"
    "${APUSR_MIDDLE_CORE_STAGE1_12_KERNEL_ROOT}/src/*.cc"
    "${APUSR_MIDDLE_CORE_STAGE1_12_KERNEL_ROOT}/*.json"
    "${APUSR_MIDDLE_CORE_STAGE1_12_KERNEL_ROOT}/*.md")
set(APUSR_MIDDLE_CORE_STAGE1_12_SHARED_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/analysis/middle_core_spatial_plan.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/analysis/middle_core_spatial_graph.py")

set(APUSR_MIDDLE_CORE_STAGE1_12_MANIFEST_STAMP
    "${CMAKE_CURRENT_BINARY_DIR}/apusr_middle_core_stage1_12_manifest.stamp")
add_custom_command(
    OUTPUT "${APUSR_MIDDLE_CORE_STAGE1_12_MANIFEST_STAMP}"
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/validate_kernel_manifest.py"
        "${APUSR_MIDDLE_CORE_STAGE1_12_MANIFEST}"
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/analysis/middle_core_resource_budget.py"
        "${APUSR_MIDDLE_CORE_STAGE1_12_MANIFEST}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${APUSR_MIDDLE_CORE_STAGE1_12_MANIFEST_STAMP}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/validate_kernel_manifest.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/analysis/middle_core_resource_budget.py"
        "${APUSR_MIDDLE_CORE_STAGE1_12_MANIFEST}"
        ${APUSR_MIDDLE_CORE_STAGE1_12_SOURCES}
        ${APUSR_MIDDLE_CORE_STAGE1_12_SHARED_SOURCES}
    VERBATIM
)
add_custom_target(apusr_kernel_manifests
    DEPENDS
        "${APUSR_MIDDLE_CORE_STAGE1_12_MANIFEST_STAMP}"
        "${APUSR_FRAME_CONVEYOR_BENCHMARK_MANIFEST_STAMP}"
)

if(APUSR_BUILD_AIE_KERNELS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tools/build_aie_kernel.py")
        message(FATAL_ERROR
            "APUSR_BUILD_AIE_KERNELS is preserved for the paused NPU lane, but the repo-owned MLIR-AIE build wrapper is not present in this checkpoint. "
            "Keep APUSR_BUILD_AIE_KERNELS=OFF until NPU development resumes and the wrapper is restored/rebased.")
    endif()
    set(_apusr_peano_arg "")
    if(APUSR_PEANO_INSTALL_DIR)
        set(_apusr_peano_arg --peano-install-dir "${APUSR_PEANO_INSTALL_DIR}")
    endif()

    set(_apusr_aiecc_arg "")
    if(APUSR_AIECC)
        set(_apusr_aiecc_arg --aiecc "${APUSR_AIECC}")
    endif()

    set(_apusr_aie_build_command_arg "")
    if(APUSR_AIE_BUILD_COMMAND)
        set(_apusr_aie_build_command_arg --build-command "${APUSR_AIE_BUILD_COMMAND}")
    endif()

    set(_apusr_aie_graph_phase_arg "")
    if(APUSR_AIE_GRAPH_PHASE)
        set(_apusr_aie_graph_phase_arg --graph-phase "${APUSR_AIE_GRAPH_PHASE}")
    endif()

    add_custom_command(
        OUTPUT
            "${APUSR_MIDDLE_CORE_STAGE1_12_XCLBIN}"
            "${APUSR_MIDDLE_CORE_STAGE1_12_INSTS}"
            "${APUSR_MIDDLE_CORE_STAGE1_12_PARAMETER_LOAD_INSTS}"
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/build_aie_kernel.py"
            --manifest "${APUSR_MIDDLE_CORE_STAGE1_12_MANIFEST}"
            --kernel-root "${APUSR_MIDDLE_CORE_STAGE1_12_KERNEL_ROOT}"
            --output "${APUSR_MIDDLE_CORE_STAGE1_12_XCLBIN}"
            --parameter-load-output "${APUSR_MIDDLE_CORE_STAGE1_12_PARAMETER_LOAD_INSTS}"
            --target "${APUSR_AIE_TARGET}"
            --model-resolution "${APUSR_MIDDLE_CORE_FRAME_CAPACITY}"
            --worker-groups "${APUSR_EFFECTIVE_MIDDLE_CORE_WORKER_GROUPS}"
            ${_apusr_aie_graph_phase_arg}
            ${_apusr_peano_arg}
            ${_apusr_aiecc_arg}
            ${_apusr_aie_build_command_arg}
        DEPENDS
            apusr_kernel_manifests
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/build_aie_kernel.py"
            "${APUSR_MIDDLE_CORE_STAGE1_12_MANIFEST}"
            ${APUSR_MIDDLE_CORE_STAGE1_12_SOURCES}
            ${APUSR_MIDDLE_CORE_STAGE1_12_SHARED_SOURCES}
        VERBATIM
    )

    add_custom_target(apusr_middle_core_stage1_12_xclbin
        DEPENDS "${APUSR_MIDDLE_CORE_STAGE1_12_XCLBIN}"
    )

    add_custom_command(
        OUTPUT
            "${APUSR_FRAME_CONVEYOR_BENCHMARK_XCLBIN}"
            "${APUSR_FRAME_CONVEYOR_BENCHMARK_INSTS}"
        COMMAND "${CMAKE_COMMAND}" -E env
            "APUSR_FRAME_CONVEYOR_BENCHMARK_FRAME_BYTES=${APUSR_FRAME_CONVEYOR_BENCHMARK_FRAME_BYTES}"
            "APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMNS=${APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMNS}"
            "APUSR_FRAME_CONVEYOR_BENCHMARK_CHANNELS=${APUSR_FRAME_CONVEYOR_BENCHMARK_CHANNELS}"
            "APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMN_MAP=${APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMN_MAP}"
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/build_aie_kernel.py"
            --manifest "${APUSR_FRAME_CONVEYOR_BENCHMARK_MANIFEST}"
            --kernel-root "${APUSR_FRAME_CONVEYOR_BENCHMARK_KERNEL_ROOT}"
            --output "${APUSR_FRAME_CONVEYOR_BENCHMARK_XCLBIN}"
            --target "${APUSR_AIE_TARGET}"
            ${_apusr_peano_arg}
            ${_apusr_aiecc_arg}
            ${_apusr_aie_build_command_arg}
        DEPENDS
            apusr_kernel_manifests
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/build_aie_kernel.py"
            "${APUSR_FRAME_CONVEYOR_BENCHMARK_MANIFEST}"
            "${APUSR_FRAME_CONVEYOR_BENCHMARK_KERNEL_ROOT}/src/apusr_frame_conveyor_benchmark.py"
        VERBATIM
    )

    add_custom_target(apusr_frame_conveyor_benchmark_xclbin
        DEPENDS "${APUSR_FRAME_CONVEYOR_BENCHMARK_XCLBIN}"
    )
endif()
