# FUSE product source umbrella — L0 fuse_core and future layers.
# C++23 for product subdirs. Legacy/ is pinned back to C++17 after it is added.

include(${CMAKE_SOURCE_DIR}/cmake/FuseCxx23.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/FuseVulkanMobile.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/FuseFindVulkan.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# FUSE_CUDA_BACKEND / enable_language(CUDA) live in the top-level CMakeLists.txt so every directory
# (incl. Tools/FUSE executables that link fuse_rhi/fuse_fx) gets the CUDA device-link phase.

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
    if(NOT TARGET Vulkan::Vulkan)
        include(${CMAKE_SOURCE_DIR}/cmake/FuseFindVulkan.cmake)
    endif()
    if(TARGET Vulkan::Vulkan)
        set(FUSE_VULKAN_BACKEND TRUE)
    else()
        set(FUSE_VULKAN_BACKEND FALSE)
        message(STATUS "FUSE: Vulkan loader not found — fuse_rhi stub backend")
    endif()
    add_subdirectory(Renderer)
endif()

# fuse_compute hosts single-source compute kernels (docs/compute-kernels.md): they run on the CPU
# backends in every build and additionally through CUDA when FUSE_BUILD_CUDA finds the toolkit.
option(FUSE_BUILD_COMPUTE "Build fuse_compute single-source kernels (CPU backends; CUDA with FUSE_BUILD_CUDA)" ON)
if(FUSE_BUILD_COMPUTE OR FUSE_BUILD_CUDA)
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
add_subdirectory(Relight)  # FUSE Relight (docs/plans/FUSE_REMIX_PORT_PLAN.md); FUSE_BUILD_RELIGHT inside, default ON for MinGW

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

# CUDA: static libraries that merely link a separable-compilation lib (fuse_rhi/fuse_fx/fuse_compute)
# must not run their own nvlink device-link step — CMake otherwise emits one per static lib, which
# races the cyclic static-lib graph (fuse_scene <-> fuse_project) and would duplicate device code.
# Executables and shared libraries still device-link once at the final link.
if(FUSE_CUDA_BACKEND)
    function(_fuse_cuda_static_no_device_link dir)
        get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_t IN LISTS _targets)
            get_target_property(_type ${_t} TYPE)
            if(_type STREQUAL "STATIC_LIBRARY")
                set_target_properties(${_t} PROPERTIES CUDA_RESOLVE_DEVICE_SYMBOLS OFF)
            endif()
        endforeach()
        get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
        foreach(_d IN LISTS _subdirs)
            _fuse_cuda_static_no_device_link("${_d}")
        endforeach()
    endfunction()
    _fuse_cuda_static_no_device_link("${CMAKE_CURRENT_SOURCE_DIR}")
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

# B2.11: whole-suite rerun under VK_LAYER_KHRONOS_validation (skips when the layer is absent).
# RUN_SERIAL: the nested run shares sample-tree outputs with the outer suite.
if(FUSE_VULKAN_BACKEND AND FUSE_BUILD_CORE_TESTS AND TARGET fuse_rhi)
  add_test(NAME fuse_vulkan_validation_gate
    COMMAND "${CMAKE_COMMAND}"
            -DFUSE_GATE_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DFUSE_GATE_SELF=fuse_vulkan_validation_gate
            "-DFUSE_GATE_SANITIZE=${FUSE_SANITIZE}"
            -P "${CMAKE_CURRENT_LIST_DIR}/../../cmake/FuseVulkanValidationGate.cmake")
  set_tests_properties(fuse_vulkan_validation_gate PROPERTIES
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
    SKIP_REGULAR_EXPRESSION "FUSE_GATE_SKIP: "
    TIMEOUT 1200
    LABELS "vulkan;gate")
endif()

# B1 CUDA compile gate (ctest -L cuda): every FUSE .cu TU must be built and carry an sm_XX cubin.
# Compile-only — kernel launches/results are NOT verified here (no GPU needed or assumed).
if(FUSE_CUDA_BACKEND AND FUSE_BUILD_CORE_TESTS)
  set(_fuse_cuda_gate_targets)
  set(_fuse_cuda_gate_expected)
  foreach(_pair IN ITEMS
      "fuse_compute:ray_march.cu|ssao.cu|ssr.cu|ssgi.cu"
      "fuse_rhi:interop_fill.cu|ddgi_probe_update.cu|clustered_lighting.cu|sdf_shadows.cu"
      "fuse_fx:particle_pool_cuda.cu"
      "fuse_cuda_shared_headers_gate:test_cuda_shared_headers.cu"
      "fuse_compute_kernel_cuda_gate:test_compute_kernel_device.cu"
      "fuse_animation:skinning.cu"
      "fuse_vfx:particle_sim.cu"
      "fuse_scene:svo_ray_cast.cu"
      "fuse_audio:reverb_fft.cu"
      "fuse_physics:physics_broadphase.cu")
    string(REPLACE ":" ";" _pair "${_pair}")
    list(GET _pair 0 _tgt)
    list(GET _pair 1 _cus)
    if(TARGET ${_tgt})
      list(APPEND _fuse_cuda_gate_targets ${_tgt})
      list(APPEND _fuse_cuda_gate_expected ${_cus})
    endif()
  endforeach()
  set(_fuse_cuda_gate_objects)
  foreach(_tgt IN LISTS _fuse_cuda_gate_targets)
    list(APPEND _fuse_cuda_gate_objects "$<JOIN:$<TARGET_OBJECTS:${_tgt}>,|>")
  endforeach()
  list(JOIN _fuse_cuda_gate_objects "|" _fuse_cuda_gate_objects)
  list(JOIN _fuse_cuda_gate_expected "|" _fuse_cuda_gate_expected)
  find_program(FUSE_CUOBJDUMP cuobjdump HINTS "${CUDAToolkit_BIN_DIR}")
  add_test(NAME fuse_cuda_compile_gate
    COMMAND "${CMAKE_COMMAND}"
            "-DFUSE_CUDA_OBJECTS=${_fuse_cuda_gate_objects}"
            "-DFUSE_CUDA_EXPECTED=${_fuse_cuda_gate_expected}"
            "-DFUSE_CUOBJDUMP=${FUSE_CUOBJDUMP}"
            "-DFUSE_CUDA_SOURCE_ROOT=${CMAKE_CURRENT_LIST_DIR}"
            -P "${CMAKE_CURRENT_LIST_DIR}/Core/tests/cuda/check_cuda_objects.cmake")
  set_tests_properties(fuse_cuda_compile_gate PROPERTIES LABELS "gate;cuda" TIMEOUT 120)
endif()

# Lint gates (ctest -L lint): ownership per module include tree, Appendix A rename checklist,
# Appendix C carry-forward (C++23 targets, no Qt in core libs, plan/source heading parity).
include(${CMAKE_SOURCE_DIR}/cmake/FuseLintGates.cmake)
