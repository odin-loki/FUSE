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
add_subdirectory(tap)                                                                    # RL-1.1
add_subdirectory(scene/classify)                                                         # RL-1.2
add_subdirectory(capture/texture)                                                        # RL-1.4
add_subdirectory(capture/geometry)                                                       # RL-1.3
add_subdirectory(bridge/ipc)                                                             # RL-2.1
add_subdirectory(scene/translate)                                                        # RL-1.5
add_subdirectory(tap/capture)                                                            # RL-1.1 capture mode (after 1.2-1.5)
add_subdirectory(scene/instances)                                                        # RL-1.7
add_subdirectory(bridge/host)                                                            # RL-2.3
add_subdirectory(bridge/client)                                                          # RL-2.2
add_subdirectory(capture/export)                                                         # RL-1.8
add_subdirectory(mods/assets)                                                            # RL-3.3
add_subdirectory(mods/usd)                                                               # RL-3.1
add_subdirectory(mods/import)                                                            # RL-3.2
add_subdirectory(replace)                                                                # RL-3.4
add_subdirectory(logic)                                                                  # RL-3.5
add_subdirectory(particles)                                                              # RL-3.6
add_subdirectory(render/material)                                                        # RL-4.3
include(cmake/relight_renderer_mingw.cmake)                                              # RL-0.7 (MinGW PE renderer; last: defers its gate registration)
include(cmake/relight_vertex_capture.cmake)                                              # RL-1.6 (after RL-0.7: hooks the DXVK and renderer targets)
add_subdirectory(render/frame)                                                           # RL-4.1 (after RL-0.7: needs fuse_rhi / the renderer)
add_subdirectory(render/raster)                                                          # RL-4.2 (after RL-4.1: implements its frame renderer)
add_subdirectory(render/lights)                                                          # RL-4.4 (after RL-0.7: needs the WP-7.1 light tree)
add_subdirectory(render/pathtrace)                                                       # RL-5.1 (after RL-4.4: light set, WP-6.0 AS)
add_subdirectory(api)                                                                    # RL-6.2 (after RL-1.7 / 3.4 / 4.3 / 4.4: drives them)
add_subdirectory(render/pathtrace/omm)                                                   # RL-5.6 opacity micromaps (after RL-5.1)
add_subdirectory(render/volumetrics)                                                     # RL-5.6 (after RL-5.1: PT frame, lights, RL-3.6 particles)
add_subdirectory(terrain)                                                                # RL-5.6 terrain baking
add_subdirectory(render/post)                                                            # RL-5.7 (after RL-5.1: hooks its frame renderer)
add_subdirectory(setup)                                                                  # RL-6.3 (after RL-1.1 / 1.2: hooks the tap, reads the classifier options)
add_subdirectory(overlay)                                                                # RL-6.1 (after RL-4.1: draws through the frame orchestration)
include(cmake/relight_package.cmake)                                                     # RL-6.4 (last: packages the runtime targets)
