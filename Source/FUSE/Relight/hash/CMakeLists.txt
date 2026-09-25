# FUSE Relight RL-0.5: Remix-compatible asset hashes (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.1).
# Pure logic: no Vulkan, D3D or DXVK headers; builds natively and for MinGW.
#
# Added from Source/FUSE/Relight/CMakeLists.txt with `add_subdirectory(hash)` (tests:
# `add_subdirectory(tests/hash)`).

get_filename_component(FUSE_RELIGHT_XXHASH_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../Engine/lib/xxhash" ABSOLUTE)
set(FUSE_RELIGHT_XXHASH_DIR "${FUSE_RELIGHT_XXHASH_DIR}" CACHE INTERNAL "Vendored xxHash 0.8.x (Engine/lib/xxhash)")

add_library(fuse_relight_hash STATIC
    src/d3d_types.cpp
    src/geometry_hash.cpp
    src/hash_string.cpp
    src/light_hash.cpp
    src/texture_hash.cpp
    src/xxh.cpp
)
target_include_directories(fuse_relight_hash PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# xxhash.h stays private (compiled inline into src/xxh.cpp); SYSTEM keeps its diagnostics out.
target_include_directories(fuse_relight_hash SYSTEM PRIVATE "${FUSE_RELIGHT_XXHASH_DIR}")
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_hash)
else()
    set_target_properties(fuse_relight_hash PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
