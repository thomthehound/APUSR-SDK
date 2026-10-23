# Vendored AMD Radeon Data File helper used for offline RGP/RMV inspection.
# This is intentionally not wired into the APUSR product build.
cmake_minimum_required(VERSION 3.20)
project(apusr_vendored_libamdrdf LANGUAGES C CXX)

option(RDF_STATIC "Build libamdrdf as a static library" ON)
option(RDF_ENABLE_CXX_BINDINGS "Enable libamdrdf C++ bindings" ON)
option(RDF_BUILD_TESTS "Build libamdrdf tests" OFF)
option(RDF_BUILD_TOOLS "Build libamdrdf tools" ON)
option(RDF_BUILD_INSTALL "Install libamdrdf" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(imported/zstd)
add_subdirectory(imported/cli11)
add_subdirectory(imported/json)
add_subdirectory(rdf)

if(RDF_BUILD_TOOLS)
  add_subdirectory(rdfi)
endif()
