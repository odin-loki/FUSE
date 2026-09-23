# FUSE Relight: FUSE-native D3D8/D3D9 remastering runtime (docs/plans/FUSE_REMIX_PORT_PLAN.md).
#
# Added once from Source/FUSE/CMakeLists.txt (RL-0.2). Each work package owns one
# cmake/relight_<wp>.cmake and adds exactly one include() line below (plan §7.0). Packages that can
# land before this file existed use the `if(EXISTS)` form; the guards stay harmless afterwards.
#
# FUSE_BUILD_RELIGHT defaults to ON only for MinGW-w64 cross builds (the fuse-mingw-* presets), where
# the vendored DXVK d3d9.dll / d3d8.dll are produced and the Wine + Xvfb + Lavapipe tests run.
# Linux-native trees can opt in with -DFUSE_BUILD_RELIGHT=ON: the CPU-only Relight modules and gates
# build there, and relight_dxvk.cmake reports that the DXVK DLLs are PE-only (no dxvk-native build).

# The DXVK build compiles GLSL with glslangValidator, so the MinGW default only turns on when it
# is available (the dedicated fuse-relight-wine workflow installs it and passes ON explicitly).
find_program(FUSE_GLSLANG_VALIDATOR NAMES glslang glslangValidator)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW AND FUSE_GLSLANG_VALIDATOR)
    set(_fuse_relight_default ON)
else()
    set(_fuse_relight_default OFF)
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW)
        message(STATUS "FUSE: Relight off by default (glslangValidator not found; pass -DFUSE_BUILD_RELIGHT=ON after installing it)")
    endif()
endif()
option(FUSE_BUILD_RELIGHT
    "Build FUSE Relight (vendored DXVK d3d9.dll/d3d8.dll + Relight modules); default ON for MinGW cross builds"
    ${_fuse_relight_default})
if(NOT FUSE_BUILD_RELIGHT)
    return()
endif()

set(FUSE_RELIGHT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
# The top-level project is CXX-only; Relight's test apps (C99) and parts of vendored DXVK
# (util/sha1, libdisplay-info) are C. Every C target lives in this directory tree.
enable_language(C)
# Local bisecting aid: names of optional package includes to skip (e.g. -DFUSE_RELIGHT_SKIP=relight_apps
# while that package is mid-landing). Empty by default; CI never sets it.
set(FUSE_RELIGHT_SKIP "" CACHE STRING "Optional Relight package cmake files to skip (list of names without .cmake)")

include(cmake/relight_dxvk.cmake)                                                        # RL-0.2
include(cmake/relight_licence_gates.cmake)                                               # RL-0.1
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/relight_apps.cmake" AND NOT "relight_apps" IN_LIST FUSE_RELIGHT_SKIP)  # RL-0.4
    include(cmake/relight_apps.cmake)
endif()
add_subdirectory(tests/env)                                                              # RL-0.3
add_subdirectory(options)                                                                # RL-0.6
add_subdirectory(tests/options)                                                          # RL-0.6
add_subdirectory(hash)                                                                   # RL-0.5
add_subdirectory(tests/hash)                                                             # RL-0.5
