# UNI-U7-ASSET-1: runtime asset core (fuse_asset). Cooked readers (FMSH, .fusetex, .fusemat), the
# AssetRegistry (AssetId <-> VFS path <-> Handle<Asset>, refcount, async load, unload), job decode and
# the MPSC render-upload queue. Links fuse_core only (plus meshoptimizer's decoder when the renderer
# provides it), never assimp / stb / the BCn encoders: Tools/FUSE/Cook links this library for reading.

add_library(fuse_asset STATIC
    src/cooked_mesh_reader.cpp
    src/cooked_texture_reader.cpp
    src/cooked_material_reader.cpp
    src/render_upload.cpp
    src/asset_registry.cpp
    include/fuse/asset/cooked_mesh.hpp
    include/fuse/asset/cooked_texture.hpp
    include/fuse/asset/cooked_material.hpp
    include/fuse/asset/render_upload.hpp
    include/fuse/asset/asset_registry.hpp
    include/fuse/asset/detail/fmsh_layout.hpp
)
add_library(fuse::asset ALIAS fuse_asset)
target_include_directories(fuse_asset PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(fuse_asset PUBLIC fuse_core)
# meshoptimizer (FMSH meshopt codec, W0.2) is vendored by the renderer (Renderer/cmake/rp_wp12.cmake),
# which is configured after this directory: resolve it at generate time.
target_link_libraries(fuse_asset PRIVATE $<$<TARGET_EXISTS:fuse_meshoptimizer>:fuse_meshoptimizer>)
target_compile_definitions(fuse_asset PRIVATE $<$<TARGET_EXISTS:fuse_meshoptimizer>:FUSE_ASSET_HAS_MESHOPTIMIZER=1>)
fuse_apply_cxx23(fuse_asset)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
