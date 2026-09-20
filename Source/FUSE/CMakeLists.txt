# FUSE product source umbrella — L0 fuse_core and future layers.
# C++23 for product subdirs. Legacy/ is pinned back to C++17 after it is added.

include(${CMAKE_SOURCE_DIR}/cmake/FuseCxx23.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/FuseVulkanMobile.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FUSE_CUDA_BACKEND FALSE)
if(FUSE_BUILD_CUDA)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        enable_language(CUDA)
        set(FUSE_CUDA_BACKEND TRUE)
        message(STATUS "FUSE: CUDA toolkit found — FUSE_HAS_CUDA enabled")
    else()
        message(STATUS "FUSE: FUSE_BUILD_CUDA=ON but toolkit not found — CUDA stub path only")
    endif()
endif()

add_subdirectory(Core)
add_subdirectory(ECS)
add_subdirectory(Animation)
add_subdirectory(Physics)
add_subdirectory(Net)
add_subdirectory(VFX)
add_subdirectory(Terrain)
add_subdirectory(WorldPartition)
add_subdirectory(Phase7)
add_subdirectory(World2D)
add_subdirectory(World3D)

if(FUSE_BUILD_AUDIO)
    add_subdirectory(Audio)
endif()

if(FUSE_BUILD_SCRIPT)
    add_subdirectory(Script)
endif()

option(FUSE_BUILD_COMPAT_TS "Build quarantined dialect script VMs under Compat/" ON)
# Re-apply shipping forces now that FUSE_BUILD_COMPAT_TS exists.
if(COMMAND fuse_shipping_force_options)
    fuse_shipping_force_options()
endif()
if(FUSE_BUILD_COMPAT_TS)
    add_subdirectory(Compat)
endif()

if(FUSE_BUILD_VULKAN)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(FUSE_VULKAN_BACKEND TRUE)
    else()
        set(FUSE_VULKAN_BACKEND FALSE)
        message(STATUS "FUSE: Vulkan loader not found — fuse_rhi stub backend")
    endif()
    add_subdirectory(Renderer)
endif()

if(FUSE_BUILD_CUDA)
    add_subdirectory(Compute)
endif()

add_subdirectory(Hybrid)

if(FUSE_BUILD_PROJECT)
    add_subdirectory(${CMAKE_SOURCE_DIR}/Tools/FUSE/Cook ${CMAKE_BINARY_DIR}/Tools/FUSE/Cook)
    add_subdirectory(Project)
    add_subdirectory(Scene)
endif()

if(FUSE_BUILD_MODULES)
    add_subdirectory(Modules)
endif()

if(FUSE_BUILD_EDITOR_API OR FUSE_BUILD_EDITOR)
    add_subdirectory(Editor)
endif()

if(FUSE_BUILD_LEGACY)
    add_subdirectory(Legacy)
endif()

if(TARGET fuse_script AND TARGET fuse_compat_ts)
    target_link_libraries(fuse_script PRIVATE fuse_compat_ts fuse_compat_ts_t3d fuse_compat_ts_t2d)
    target_compile_definitions(fuse_script PRIVATE FUSE_HAS_COMPAT_TS=1)
    if(TARGET fuse_script_tests)
        target_compile_definitions(fuse_script_tests PRIVATE FUSE_HAS_COMPAT_TS=1)
    endif()
    if(TARGET fuse_script_compat_route_tests)
        target_compile_definitions(fuse_script_compat_route_tests PRIVATE FUSE_HAS_COMPAT_TS=1)
    endif()
endif()

if(TARGET fuse_world_converter AND TARGET fuse_t2d_legacy)
    target_link_libraries(fuse_world_converter PUBLIC fuse_t2d_legacy)
endif()

if(FUSE_BUILD_LEGACY AND FUSE_BUILD_SMOKE OR FUSE_BUILD_HYBRID_DEMO OR FUSE_BUILD_PARITY_DEMOS)
    add_subdirectory(Apps)
endif()

if(COMMAND fuse_shipping_assert_runtime_targets)
    fuse_shipping_assert_runtime_targets()
endif()

# Product libs compile as C++23. cxx_std_23 stays PRIVATE so Legacy C++17
# quarantine targets that PUBLIC-link fuse_* do not inherit cxx_std_23.
set(_fuse_product_cxx23_targets
  fuse_core fuse_ecs fuse_net fuse_scene fuse_project fuse_world_converter
  fuse_hybrid fuse_world2d fuse_world3d fuse_script fuse_audio
  fuse_animation fuse_physics fuse_vfx fuse_terrain fuse_world_partition
  fuse_ai fuse_cinematics fuse_fx fuse_mechanics fuse_adventure
  fuse_rhi fuse_compute fuse_editor_api
)
set(_fuse_demo_cxx23_targets
  demo_3d_empty demo_2d_sprites demo_ai_bt demo_timeline demo_fx
  demo_adventure_stub demo_hybrid_hud fuse_runtime_smoke
)
set(_fuse_cxx23_pinned)
foreach(_t IN LISTS _fuse_product_cxx23_targets _fuse_demo_cxx23_targets)
  if(TARGET ${_t})
    fuse_apply_cxx23(${_t})
    list(APPEND _fuse_cxx23_pinned ${_t})
  endif()
endforeach()
if(_fuse_cxx23_pinned)
  message(STATUS "FUSE: C++23 pinned: ${_fuse_cxx23_pinned}")
endif()
