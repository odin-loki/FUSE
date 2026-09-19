# Minimal Engine/source probe for fuse_t3d_legacy quarantine (U2 incremental).
# Compiles gfx/bitmap/bitmapUtils.cpp with a stripped torqueConfig — no SimObject/Con:: closure.

if(NOT TARGET fuse_t3d_legacy)
    message(FATAL_ERROR "FuseT3DLegacyEngine.cmake requires fuse_t3d_legacy target")
endif()

# SimObject remains blocked — 122-header closure, IMPLEMENT_CONOBJECT, T2D ODR. See U2-SMOKE §3.1.
set(FUSE_T3D_LEGACY_ENGINE_PROBE OFF CACHE BOOL
    "Compile Engine/source gfx probe into fuse_t3d_legacy (bitmapUtils extrude)")

if(NOT FUSE_T3D_LEGACY_ENGINE_PROBE)
    return()
endif()

set(_fuse_t3d_legacy_engine_dir "${CMAKE_CURRENT_BINARY_DIR}/fuse_t3d_legacy_engine")
file(MAKE_DIRECTORY "${_fuse_t3d_legacy_engine_dir}")

set(TORQUE_APP_NAME "fuse_t3d_legacy_probe")
set(TORQUE_APP_VERSION 1000)
set(TORQUE_APP_VERSION_STRING "1.0.0")
set(TORQUE_APP_PASSWORD "")
set(TORQUE_DTS_VERSION 124)
set(TORQUE_SCRIPT_EXTENSION "tscript")
set(TORQUE_ENTRY_FUNCTION "main")

configure_file(
    "${CMAKE_SOURCE_DIR}/Tools/CMake/torqueConfig.h.in"
    "${_fuse_t3d_legacy_engine_dir}/torqueConfig.h"
    @ONLY
)

target_sources(fuse_t3d_legacy PRIVATE
    src/engine_probe/platform_stub.cpp
    src/engine_probe/bitmap_probe_smoke.cpp
    "${CMAKE_SOURCE_DIR}/Engine/source/gfx/bitmap/bitmapUtils.cpp"
)

target_include_directories(fuse_t3d_legacy PRIVATE
    "${CMAKE_SOURCE_DIR}/Engine/source"
    "${_fuse_t3d_legacy_engine_dir}"
)

# C++17 drops the legacy `linux` macro; types.gcc.h needs LINUX for types.posix.h (dsize_t, FileTime).
target_compile_definitions(fuse_t3d_legacy PRIVATE
    LINUX=1
    TORQUE_LITTLE_ENDIAN=1
    TORQUE_DISABLE_MEMORY_MANAGER=1
    FUSE_T3D_LEGACY_ENGINE_PROBE=1
)

message(STATUS "FUSE: fuse_t3d_legacy Engine probe enabled (bitmapUtils extrude/convert + platform_stub)")
