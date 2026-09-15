cmake_minimum_required(VERSION 3.21.0)

# -----------------------------------------------------------------------------
# FUSE umbrella entry (U1) — wraps Torque3D, optional Torque2D, and fuse_core.
#
# Legacy T3D-only path (unchanged workflow):
#   cmake -B build -DTORQUE_APP_NAME=Torque3D
#
# Umbrella path (default when TORQUE_APP_NAME is unset):
#   cmake -B build -DFUSE_UMBRELLA=ON
# -----------------------------------------------------------------------------
set(TORQUE_APP_NAME "" CACHE STRING "Torque3D application name (required for FUSE_BUILD_T3D)")

set(_fuse_umbrella_default ON)
if(NOT "${TORQUE_APP_NAME}" STREQUAL "")
    set(_fuse_umbrella_default OFF)
endif()
option(FUSE_UMBRELLA "Enable the FUSE umbrella build (fuse_core + optional legacy engines)" ${_fuse_umbrella_default})

if(NOT FUSE_UMBRELLA)
    include("${CMAKE_SOURCE_DIR}/cmake/LegacyTorque3D.cmake")
    return()
endif()

project(FUSE VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("${CMAKE_SOURCE_DIR}/cmake/FusePlatforms.cmake")
fuse_log_platform_profile()

option(FUSE_BUILD_CORE "Build fuse_core library and tests" ON)
option(FUSE_BUILD_CORE_TESTS "Build fuse_core unit tests (fuse_core_tests)" ON)
option(FUSE_BUILD_LEGACY "Build fuse_t3d_legacy / fuse_t2d_legacy quarantine libs" ON)
option(FUSE_BUILD_SMOKE "Build fuse_runtime_smoke one-process test binary" ON)
option(FUSE_BUILD_HYBRID_DEMO "Build demo_hybrid_hud U4 hybrid frame demo" ON)
option(FUSE_BUILD_MODULES "Build fuse_* feature modules (U5)" ON)
option(FUSE_BUILD_EDITOR_API "Build fuse_editor_api command queue stub (U6/WP-08)" ON)
option(FUSE_SMOKE_ENABLE_ASAN "Enable AddressSanitizer for fuse_runtime_smoke" OFF)
option(FUSE_BUILD_T3D "Build Torque3D application target(s)" ON)
option(FUSE_BUILD_T2D "Build Torque2D engine target (requires submodule)" ON)

if(FUSE_BUILD_T3D AND "${TORQUE_APP_NAME}" STREQUAL "")
    set(TORQUE_APP_NAME "Torque3D" CACHE STRING "Torque3D application name" FORCE)
    message(STATUS "FUSE: defaulting TORQUE_APP_NAME to Torque3D for FUSE_BUILD_T3D")
endif()

if(FUSE_BUILD_CORE)
    if(FUSE_BUILD_CORE_TESTS)
        enable_testing()
    endif()
    add_subdirectory(Source/FUSE)
endif()

if(FUSE_BUILD_T3D)
    if("${TORQUE_APP_NAME}" STREQUAL "")
        message(FATAL_ERROR "FUSE_BUILD_T3D requires TORQUE_APP_NAME")
    endif()
    include("${CMAKE_SOURCE_DIR}/cmake/LegacyTorque3D.cmake")
endif()

if(FUSE_BUILD_T2D)
    include("${CMAKE_SOURCE_DIR}/cmake/FuseTorque2D.cmake")
    fuse_configure_torque2d()
    if(NOT FUSE_T2D_AVAILABLE)
        message(STATUS "FUSE_BUILD_T2D=ON but Torque2D unavailable — continuing without T2D target")
    endif()
endif()
