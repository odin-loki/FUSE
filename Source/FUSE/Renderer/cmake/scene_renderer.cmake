# E02 SceneRenderer (docs/unification/RENDERER-EXECUTION.md RE-FI-1, TRACK-B-VULKAN.md RE-RUNTIME-3D-RENDER,
# U4-HYBRID-FRAME.md U4-1): an ECS 3D scene through the frame::FrameComposer (mesh registry, material feed, UI stage,
# present blit). Owned by the E02 package.
#
#   fuse_scene_renderer          STATIC  include/fuse/renderer/scene_renderer/**, src/scene_renderer/**
#   fuse_scene_renderer_cpu      ctest   CPU gates (mesh registry + material feed + ECS remap; stub-safe)
#   fuse_scene_renderer_vk_*     ctest   Lavapipe gates (validation + sync validation; exit 77 without Vulkan)

get_filename_component(_fuse_sr_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_fuse_sr_inc "${_fuse_sr_root}/include/fuse/renderer/scene_renderer")
set(_fuse_sr_src "${_fuse_sr_root}/src/scene_renderer")

add_library(fuse_scene_renderer STATIC
    ${_fuse_sr_src}/mesh_registry.cpp
    ${_fuse_sr_src}/procedural_meshes.cpp
    ${_fuse_sr_src}/scene_renderer.cpp
    ${_fuse_sr_inc}/mesh_registry.hpp
    ${_fuse_sr_inc}/procedural_meshes.hpp
    ${_fuse_sr_inc}/scene_renderer.hpp
)
target_link_libraries(fuse_scene_renderer PUBLIC fuse_frame fuse_gpu_scene_ecs fuse_geometry)
if(TARGET fuse_geometry_cook)
    # .fusemesh (FMSH v1) -> meshlet sidecar rebuild (MeshRegistry::registerFuseMesh).
    target_link_libraries(fuse_scene_renderer PRIVATE fuse_geometry_cook)
    target_compile_definitions(fuse_scene_renderer PRIVATE FUSE_SCENE_RENDERER_HAS_COOK=1)
endif()
fuse_apply_cxx23(fuse_scene_renderer)

if(NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

set(_fuse_sr_tests_dir "${_fuse_sr_root}/tests")

# --- CPU gates (CPU-only GpuScene; also in the stub tree) ------------------------------------------------------
add_executable(fuse_scene_renderer_cpu ${_fuse_sr_tests_dir}/test_scene_renderer_cpu.cpp)
target_link_libraries(fuse_scene_renderer_cpu PRIVATE fuse_scene_renderer)
target_compile_definitions(fuse_scene_renderer_cpu PRIVATE
    FUSE_SR_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/scene_renderer_tmp")
fuse_apply_cxx23(fuse_scene_renderer_cpu)
foreach(_suite registry material_feed extract procedural)
    add_test(NAME fuse_scene_renderer_${_suite} COMMAND fuse_scene_renderer_cpu ${_suite})
    set_tests_properties(fuse_scene_renderer_${_suite} PROPERTIES LABELS "gate;renderer;scene_renderer" TIMEOUT 300)
endforeach()

# --- Lavapipe gates ----------------------------------------------------------------------------------------------
#   --mode golden      SceneRenderer image == FrameComposer image of the same scene built directly (bit for bit)
#   --mode zero_alloc  30 steady-state frames: 0 operator-new calls in renderScene + graph build + pass callbacks
#   --mode blit        present blit into a headless RGBA8 target == CPU conversion of the output (+ FG hand-off)
#   --mode ui          UI stage: frame.ui_composite == frame_ui_composite_texel (<= 1 half ulp), HUD-less untouched
add_executable(fuse_scene_renderer_vk ${_fuse_sr_tests_dir}/test_scene_renderer_vk.cpp)
target_link_libraries(fuse_scene_renderer_vk PRIVATE fuse_scene_renderer)
fuse_apply_cxx23(fuse_scene_renderer_vk)

set(_fuse_sr_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")
set(_fuse_sr_vk_tests "")
foreach(_case "golden;t0" "golden;t2" "zero_alloc;t0" "blit;t0" "ui;t0")
    list(GET _case 0 _mode)
    list(GET _case 1 _tier)
    set(_name "fuse_scene_renderer_vk_${_mode}_${_tier}")
    if(FUSE_VULKAN_BACKEND)
        add_test(NAME ${_name} COMMAND "${_fuse_sr_lock}" "$<TARGET_FILE:fuse_scene_renderer_vk>" --mode ${_mode} --tier ${_tier})
    else()
        add_test(NAME ${_name} COMMAND fuse_scene_renderer_vk --mode ${_mode} --tier ${_tier})
    endif()
    list(APPEND _fuse_sr_vk_tests ${_name})
endforeach()
set_tests_properties(${_fuse_sr_vk_tests} PROPERTIES
    ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
    TIMEOUT 1800
    LABELS "gate;vulkan;renderer;scene_renderer")
