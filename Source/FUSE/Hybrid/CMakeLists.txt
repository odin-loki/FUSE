add_library(fuse_hybrid STATIC
    src/cooked_asset_bindings.cpp
    src/hybrid_composer.cpp
    src/mesh_sdf_preview_stub.cpp
    src/placeholder_renderer.cpp
    src/sprite_layer.cpp
    include/fuse/hybrid/cooked_asset_bindings.hpp
    include/fuse/hybrid/hybrid_composer.hpp
    include/fuse/hybrid/mesh_sdf_preview_stub.hpp
    include/fuse/hybrid/placeholder_renderer.hpp
    include/fuse/hybrid/project_flags.hpp
    include/fuse/hybrid/sprite_layer.hpp
)
# E03: the sprite-layer CPU reference must match the `precise` GLSL kernel (no FMA contraction).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/sprite_layer.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
elseif(MSVC)
    set_source_files_properties(src/sprite_layer.cpp PROPERTIES COMPILE_OPTIONS "/fp:precise")
endif()

if(FUSE_BUILD_VULKAN)
    target_sources(fuse_hybrid PRIVATE
        src/hybrid_renderer_bootstrap.cpp
        src/hybrid_scene_renderer.cpp
        src/sprite_layer_gpu.cpp
        src/vulkan_presentable.cpp
        include/fuse/hybrid/hybrid_renderer_bootstrap.hpp
        include/fuse/hybrid/hybrid_scene_renderer.hpp
        include/fuse/hybrid/sprite_layer_gpu.hpp
        include/fuse/hybrid/vulkan_presentable.hpp
    )
endif()

add_library(fuse::hybrid ALIAS fuse_hybrid)

target_include_directories(fuse_hybrid
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_hybrid PUBLIC fuse_world3d fuse_world2d fuse_core)
# E06: cooked_asset_bindings reads cooked files with the runtime readers and uploads them through the renderer's
# CookedAssetRegistry when the renderer is in the build.
target_link_libraries(fuse_hybrid PRIVATE fuse_asset)
if(TARGET fuse_cooked_assets)
    target_link_libraries(fuse_hybrid PUBLIC fuse_cooked_assets)
    target_compile_definitions(fuse_hybrid PRIVATE FUSE_HYBRID_HAS_COOKED_REGISTRY=1)
endif()

if(FUSE_BUILD_VULKAN)
    # E03: the GPU scene path renders through the E02 SceneRenderer (fuse_scene_renderer -> fuse_frame -> fuse_rhi).
    target_link_libraries(fuse_hybrid PUBLIC fuse_rhi fuse_scene_renderer)
    target_compile_definitions(fuse_hybrid PUBLIC FUSE_HAS_VULKAN_RHI=1)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/hybrid_gpu.cmake")
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
