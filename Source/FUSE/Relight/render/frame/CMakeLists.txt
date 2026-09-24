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
#                                  bindless registration of DXVK images (BindlessImageRegistry), the scene feed
#                                  (RL-1.7 SceneModel + RL-3.4 replaced scene -> AdapterDraw / AdapterLight), the
#                                  GpuSceneAdapter (feed -> gpu_scene::GpuScene) and RendererContext: the renderer on
#                                  the host's device (volk from the host's loader, VulkanDevice::adopt, the WP-0.4
#                                  bindless heap with GPU descriptors, the WP-1.1 GPU scene). Linked into d3d9.dll
#                                  through fuse_relight_tap_capture; d3d9.dll / d3d8.dll set FUSE_RHI_VOLK_NO_AUTO_INIT,
#                                  so fuse_rhi opens no Vulkan loader from DllMain (volk is initialised from DXVK's
#                                  vkGetInstanceProcAddr at the first device attach).
#   fuse_relight_render_frame_gpu  alias of the above (RL-4.1 kept the renderer objects out of d3d9.dll until
#                                  VulkanDevice::adopt and the volk opt-out existed).
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
    src/bindless_renderer_heap.cpp
    src/frame_gpu.cpp
    src/frame_options.cpp
    src/frame_orchestrator.cpp
    src/render_tap.cpp
    src/renderer_context.cpp
    src/scene_adapter.cpp
    src/scene_feed.cpp
    src/vk_dispatch.cpp)
target_include_directories(fuse_relight_render_frame PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fuse_relight_render_frame
    PUBLIC fuse_relight_tap_capture fuse_relight_scene_instances fuse_relight_replace fuse_relight_options fuse_rhi
           fuse_gpu_scene
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
# RL-4.2 raster remaster (render/raster, added after this directory; it links this library): relight.frame.mode =
# raster creates its frame renderer. Resolved when the build is generated, like the RL-3.4 / RL-3.5 hooks.
target_link_libraries(fuse_relight_render_frame PRIVATE $<TARGET_NAME_IF_EXISTS:fuse_relight_render_raster>)
target_compile_definitions(fuse_relight_render_frame PRIVATE
    $<$<TARGET_EXISTS:fuse_relight_render_raster>:FUSE_RELIGHT_HAVE_RASTER=1>)
add_library(fuse_relight_render_frame_gpu ALIAS fuse_relight_render_frame)

if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_render_frame)
else()
    set_target_properties(fuse_relight_render_frame PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

# The DXVK DLLs link fuse_rhi (through the tap): no volk auto-initialisation (a LoadLibrary of vulkan-1.dll from a
# static initialiser would run inside DllMain). cmake/FuseVolk.cmake reads this on the image being linked.
foreach(_dll relight_d3d9 relight_d3d8)
    if(TARGET ${_dll})
        set_target_properties(${_dll} PROPERTIES FUSE_RHI_VOLK_NO_AUTO_INIT ON)
    endif()
endforeach()

add_subdirectory(tests)
