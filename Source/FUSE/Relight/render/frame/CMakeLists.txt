# FUSE Relight RL-4.1: frame orchestration, GPU-scene adapter and composite (docs/plans/FUSE_REMIX_PORT_PLAN.md
# §2.3, Wave R4). Added by one line in Source/FUSE/Relight/CMakeLists.txt, add_subdirectory(render/frame), right after
# RL-0.7's include(cmake/relight_renderer_mingw.cmake): it needs the renderer (fuse_rhi: bindless v2 WP-0.4, render
# graph v2 WP-0.3; fuse_gpu_scene WP-1.1), which RL-0.7 adds in the MinGW tree (FUSE_RELIGHT_RENDERER) and the top
# level adds in Vulkan trees. Without the renderer nothing here is configured, and createTapForDevice keeps the plain
# capture tap (tap/capture links this library only when the target exists).
#
# Owns Relight/render/frame/**. Touches (minimal hooks, see Engine/lib/dxvk/PATCHES.md "Frame orchestration"):
# tap/include/fuse/relight/tap/frame_host.hpp (the host interface, tap interface 4), tap/dxvk/fuse_tap_dxvk.{h,cpp}
# (the DXVK frame host), tap/capture (the factory hook) and FUSE-DXVK patch RL-4.1-01.
#
# Targets
#   fuse_relight_render_frame      RenderTap (the capture tap + frame orchestration), FrameOrchestrator (injection,
#                                  composite, timeline values, passthrough texture swap), FrameGpu (FUSE's images
#                                  and RG v2-planned submissions on the host's device, own Vulkan dispatch), the
#                                  bindless registration of DXVK images (BindlessImageRegistry) and the scene feed
#                                  (RL-1.7 SceneModel + RL-3.4 replaced scene -> AdapterDraw / AdapterLight). Linked
#                                  into d3d9.dll through fuse_relight_tap_capture.
#   fuse_relight_render_frame_gpu  GpuSceneAdapter (the feed -> gpu_scene::GpuScene) and RendererBindlessHeap (the
#                                  registry on the renderer's BindlessDescriptors); not in d3d9.dll (see above).
# Tests: tests/ (rl_frame_*).

foreach(_dep fuse_rhi fuse_gpu_scene fuse_relight_tap_capture fuse_relight_scene_instances fuse_relight_replace)
    if(NOT TARGET ${_dep})
        message(STATUS "FUSE Relight: RL-4.1 render/frame skipped (no ${_dep}; the renderer is built with "
                       "FUSE_BUILD_VULKAN or, in MinGW trees, FUSE_RELIGHT_RENDERER)")
        return()
    endif()
endforeach()
# FUSE's frame work records Vulkan commands: a stub fuse_rhi (no Vulkan headers / backend) is not enough.
get_target_property(_rl_frame_rhi_defs fuse_rhi INTERFACE_COMPILE_DEFINITIONS)
if(NOT "FUSE_VULKAN_BACKEND=1" IN_LIST _rl_frame_rhi_defs)
    message(STATUS "FUSE Relight: RL-4.1 render/frame skipped (fuse_rhi is the stub backend)")
    return()
endif()

add_library(fuse_relight_render_frame STATIC
    src/bindless_images.cpp
    src/frame_gpu.cpp
    src/frame_options.cpp
    src/frame_orchestrator.cpp
    src/render_tap.cpp
    src/scene_feed.cpp
    src/vk_dispatch.cpp)
target_include_directories(fuse_relight_render_frame PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# fuse_rhi: headers (bindless handle layout, gpu_scene row types) and render graph v2 (rg/graph.cpp, Vulkan-free).
# Nothing here may reference fuse_rhi's volk globals: their translation unit loads vulkan-1.dll from a static
# initialiser, which inside d3d9.dll would run in DllMain (scene_feed.hpp).
target_link_libraries(fuse_relight_render_frame
    PUBLIC fuse_relight_tap_capture fuse_relight_scene_instances fuse_relight_replace fuse_relight_options fuse_rhi
    PRIVATE fuse_relight_capture_geometry)
foreach(_opt replace_live:REPLACE logic_live:LOGIC)
    string(REPLACE ":" ";" _opt "${_opt}")
    list(GET _opt 0 _lib)
    list(GET _opt 1 _def)
    if(TARGET fuse_relight_${_lib})
        target_link_libraries(fuse_relight_render_frame PRIVATE fuse_relight_${_lib})
        target_compile_definitions(fuse_relight_render_frame PRIVATE FUSE_RELIGHT_HAVE_${_def}=1)
    endif()
endforeach()

# The renderer-object side: GpuSceneAdapter (gpu_scene::GpuScene) and the WP-0.4 bindless heap behind the registry.
add_library(fuse_relight_render_frame_gpu STATIC
    src/bindless_renderer_heap.cpp
    src/scene_adapter.cpp)
target_link_libraries(fuse_relight_render_frame_gpu PUBLIC fuse_relight_render_frame fuse_gpu_scene fuse_rhi)

foreach(_t fuse_relight_render_frame fuse_relight_render_frame_gpu)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endforeach()

add_subdirectory(tests)
