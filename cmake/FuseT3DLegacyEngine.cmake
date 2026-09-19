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

set(_fuse_t3d_legacy_engine_probe_include
    "${CMAKE_CURRENT_SOURCE_DIR}/engine_probe/include"
)

set(_fuse_t3d_legacy_engine_sources
    src/engine_probe/platform_stub.cpp
    src/engine_probe/string_stub.cpp
    src/engine_probe/frame_allocator_stub.cpp
    src/engine_probe/string_table_stub.cpp
    src/engine_probe/platform_net_stub.cpp
    src/engine_probe/thread_pool_stub.cpp
    src/engine_probe/fs_volume_stub.cpp
    src/engine_probe/console_stub.cpp
    src/engine_probe/platform_assert_stub.cpp
    src/engine_probe/gbitmap_probe_stub.cpp
    src/engine_probe/bitmap_probe_smoke.cpp
    src/engine_probe/engine_probe_batch_smoke.cpp
    src/engine_probe/color_probe_smoke.cpp
    "${CMAKE_SOURCE_DIR}/Engine/source/gfx/bitmap/bitmapUtils.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/gfx/bitmap/loaders/ies/ies_loader.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/util/md5.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/util/hashFunction.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/util/commonSwizzles.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/stream/stream.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/stream/memStream.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/stream/fileStream.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/util/path.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/strings/stringFunctions.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/util/byteBuffer.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/util/refBase.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/util/tVector.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/util/timeClass.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/util/tSignal.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/gfx/bitmap/loaders/bitmapSTB.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/crc.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/bitVector.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/idGenerator.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/util/tDictionary.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/color.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/source/core/dataChunker.cpp"
)

# bitmapPng.cpp uses bundled lpng headers (Engine/lib/lpng) + system libpng/zlib.
find_package(PNG QUIET)
find_package(ZLIB QUIET)
set(FUSE_T3D_LEGACY_ENGINE_PROBE_PNG OFF)
if(PNG_FOUND AND ZLIB_FOUND)
    list(APPEND _fuse_t3d_legacy_engine_sources
        "${CMAKE_SOURCE_DIR}/Engine/source/gfx/bitmap/loaders/bitmapPng.cpp"
    )
    target_link_libraries(fuse_t3d_legacy PRIVATE PNG::PNG ZLIB::ZLIB)
    target_include_directories(fuse_t3d_legacy PRIVATE
        "${CMAKE_SOURCE_DIR}/Engine/lib"
        "${CMAKE_SOURCE_DIR}/Engine/lib/zlib"
    )
    target_compile_definitions(fuse_t3d_legacy PRIVATE FUSE_T3D_LEGACY_ENGINE_PROBE_PNG=1)
    set(FUSE_T3D_LEGACY_ENGINE_PROBE_PNG ON CACHE BOOL "Engine probe linked bitmapPng.cpp" FORCE)
    message(STATUS "FUSE: engine probe bitmapPng enabled (libpng+zlib found)")
else()
    set(FUSE_T3D_LEGACY_ENGINE_PROBE_PNG OFF CACHE BOOL "Engine probe linked bitmapPng.cpp" FORCE)
    message(STATUS "FUSE: engine probe bitmapPng skipped (libpng or zlib not found)")
endif()

target_sources(fuse_t3d_legacy PRIVATE ${_fuse_t3d_legacy_engine_sources})

target_include_directories(fuse_t3d_legacy PRIVATE
    "${_fuse_t3d_legacy_engine_probe_include}"
    "${CMAKE_SOURCE_DIR}/Engine/source"
    "${_fuse_t3d_legacy_engine_dir}"
)

# Per-TU include roots — do NOT add Engine/core/util globally (shadows system <endian.h>).
set(_fuse_t3d_legacy_md5_cpp "${CMAKE_SOURCE_DIR}/Engine/source/core/util/md5.cpp")
set(_fuse_t3d_legacy_ies_cpp "${CMAKE_SOURCE_DIR}/Engine/source/gfx/bitmap/loaders/ies/ies_loader.cpp")
set_source_files_properties(
    ${_fuse_t3d_legacy_md5_cpp}
    PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/Engine/source/core/util"
)
set_source_files_properties(
    ${_fuse_t3d_legacy_ies_cpp}
    PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/Engine/source/gfx/bitmap/loaders/ies"
)

# C++17 drops the legacy `linux` macro; types.gcc.h needs LINUX for types.posix.h (dsize_t, FileTime).
target_compile_definitions(fuse_t3d_legacy PRIVATE
    LINUX=1
    TORQUE_LITTLE_ENDIAN=1
    TORQUE_DISABLE_MEMORY_MANAGER=1
    FUSE_T3D_LEGACY_ENGINE_PROBE=1
)

set(_fuse_t3d_legacy_color_cpp "${CMAKE_SOURCE_DIR}/Engine/source/core/color.cpp")
set(_fuse_t3d_legacy_color_probe_tus
    ${_fuse_t3d_legacy_color_cpp}
    src/engine_probe/color_probe_smoke.cpp
)
set_source_files_properties(
    ${_fuse_t3d_legacy_color_probe_tus}
    PROPERTIES
        COMPILE_OPTIONS "-include${_fuse_t3d_legacy_engine_probe_include}/color_cpp_prelude.h"
)

message(STATUS "FUSE: fuse_t3d_legacy Engine probe enabled (batch 1-8: bitmapUtils/ies/md5/hash/swizzles/stream + bitmapSTB/PNG + read/writeBitmap stub + crc/bitVector/idGenerator/tDictionary + timeClass/tSignal + color/dataChunker + console shadows + stubs)")
