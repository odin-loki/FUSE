# FUSE Relight RL-3.3: DDS reader, Remix `.pkg` package reader and GDeflate CPU decode
# (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.6, Wave R3). Added by one line in Source/FUSE/Relight/CMakeLists.txt:
# add_subdirectory(mods/assets). It also adds the package's tests (../../tests/mods_assets).
#
# Targets
#   fuse_relight_gdeflate      the vendored GDeflate reference (Engine/lib/gdeflate: NVIDIA's libdeflate GDeflate
#                              fork, MIT + Apache-2.0, and Microsoft DirectStorage's tile-stream wrapper,
#                              Apache-2.0). CPU only. Skipped with -DFUSE_RELIGHT_GDEFLATE=OFF.
#   fuse_relight_mods_assets   texture formats + CPU decode (BC1-7 incl. BC6H), the DDS reader (DX9 + DX10),
#                              the asset package reader / writer and the hardened GDeflate tile-stream decoder.
#                              Pure CPU logic: native and MinGW.
# Tests: ../../tests/mods_assets (rl_mods_assets_unit, rl_mods_assets_fuzz, rl_mods_assets_fuzz_asan).

option(FUSE_RELIGHT_GDEFLATE "Build the vendored GDeflate CPU codec for Remix packages (Engine/lib/gdeflate)" ON)

get_filename_component(FUSE_RELIGHT_GDEFLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../Engine/lib/gdeflate" ABSOLUTE)

# Vendored sources, compiled verbatim (fuse_warnings_finalize gives files outside Source/FUSE `-w`).
set(FUSE_RELIGHT_GDEFLATE_SOURCES
    "${FUSE_RELIGHT_GDEFLATE_DIR}/libdeflate/lib/crc32.c"
    "${FUSE_RELIGHT_GDEFLATE_DIR}/libdeflate/lib/deflate_compress.c"
    "${FUSE_RELIGHT_GDEFLATE_DIR}/libdeflate/lib/deflate_decompress.c"
    "${FUSE_RELIGHT_GDEFLATE_DIR}/libdeflate/lib/gdeflate_compress.c"
    "${FUSE_RELIGHT_GDEFLATE_DIR}/libdeflate/lib/gdeflate_decompress.c"
    "${FUSE_RELIGHT_GDEFLATE_DIR}/libdeflate/lib/utils.c"
    "${FUSE_RELIGHT_GDEFLATE_DIR}/libdeflate/lib/x86/cpu_features.c"
    "${FUSE_RELIGHT_GDEFLATE_DIR}/libdeflate/lib/arm/cpu_features.c"
    "${FUSE_RELIGHT_GDEFLATE_DIR}/GDeflate/GDeflateCompress.cpp"
    "${FUSE_RELIGHT_GDEFLATE_DIR}/GDeflate/GDeflateDecompress.cpp")
set(FUSE_RELIGHT_GDEFLATE_INCLUDES "${FUSE_RELIGHT_GDEFLATE_DIR}" "${FUSE_RELIGHT_GDEFLATE_DIR}/libdeflate"
                                   "${FUSE_RELIGHT_GDEFLATE_DIR}/GDeflate")

if(FUSE_RELIGHT_GDEFLATE)
    add_library(fuse_relight_gdeflate STATIC ${FUSE_RELIGHT_GDEFLATE_SOURCES})
    target_include_directories(fuse_relight_gdeflate SYSTEM PUBLIC ${FUSE_RELIGHT_GDEFLATE_INCLUDES})
    find_package(Threads REQUIRED)
    target_link_libraries(fuse_relight_gdeflate PUBLIC Threads::Threads)
    if(MINGW)
        # libdeflate's compiler_gcc.h defines _rotr/_rotr64 macros; MinGW's stdlib.h declares functions of
        # those names, so it must be seen first (upstream only builds this code with MSVC on Windows).
        target_compile_options(fuse_relight_gdeflate PRIVATE "$<$<COMPILE_LANGUAGE:C>:SHELL:-include stdlib.h>")
    endif()
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(fuse_relight_gdeflate)
    else()
        set_target_properties(fuse_relight_gdeflate PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endif()

set(FUSE_RELIGHT_MODS_ASSETS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/asset_package.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dds.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gdeflate.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/texture_decode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/texture_format.cpp")

add_library(fuse_relight_mods_assets STATIC ${FUSE_RELIGHT_MODS_ASSETS_SOURCES})
target_include_directories(fuse_relight_mods_assets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fuse_relight_mods_assets PUBLIC fuse_relight_hash)
if(FUSE_RELIGHT_GDEFLATE)
    target_link_libraries(fuse_relight_mods_assets PRIVATE fuse_relight_gdeflate)
    target_compile_definitions(fuse_relight_mods_assets PRIVATE FUSE_RELIGHT_HAS_GDEFLATE=1)
else()
    target_compile_definitions(fuse_relight_mods_assets PRIVATE FUSE_RELIGHT_HAS_GDEFLATE=0)
endif()
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_mods_assets)
else()
    set_target_properties(fuse_relight_mods_assets PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tests/mods_assets ${CMAKE_CURRENT_BINARY_DIR}/tests)
