cmake_minimum_required(VERSION 3.21)

project(FidelityFX_SC_Linux LANGUAGES CXX)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "Install root for FidelityFX Shader Compiler Linux port" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FFX_SC_DXC_DEPENDENCY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/dxc")
set(FFX_SC_DXC_LIBRARY_DIR "${FFX_SC_DXC_DEPENDENCY_ROOT}/lib")

add_library(ffx_sc_linux_core
    src/DXBCChecksum.cpp
    src/ffx_sc.cpp
    src/hlsl_compiler.cpp
    src/linux_host_app.cpp
    src/utils.cpp
)

target_include_directories(ffx_sc_linux_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${FFX_SC_DXC_DEPENDENCY_ROOT}/include
)

target_link_libraries(ffx_sc_linux_core
    PUBLIC
        ${FFX_SC_DXC_LIBRARY_DIR}/libdxcompiler.so
        ${FFX_SC_DXC_LIBRARY_DIR}/libdxil.so
)

if(MSVC)
    target_compile_options(ffx_sc_linux_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(ffx_sc_linux_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(ffx_sc_linux src/main.cpp)
target_link_libraries(ffx_sc_linux PRIVATE ffx_sc_linux_core pthread)

set_property(TARGET ffx_sc_linux PROPERTY OUTPUT_NAME "ffx_sc_cli")
set_target_properties(ffx_sc_linux PROPERTIES
    BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    INSTALL_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)

include(GNUInstallDirs)
install(TARGETS ffx_sc_linux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
