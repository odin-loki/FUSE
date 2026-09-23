add_executable(fuse_runtime_smoke
    main.cpp
)

target_link_libraries(fuse_runtime_smoke
    PRIVATE
        fuse_core
        fuse_t3d_legacy
        fuse_t2d_legacy
)

if(FUSE_BUILD_VULKAN)
    target_link_libraries(fuse_runtime_smoke PRIVATE fuse_rhi)
    target_compile_definitions(fuse_runtime_smoke PRIVATE FUSE_HAS_VULKAN_RHI=1)
endif()

target_compile_features(fuse_runtime_smoke PRIVATE cxx_std_17)

if(MINGW)
    target_link_options(fuse_runtime_smoke PRIVATE -Wl,--allow-multiple-definition)
endif()

if(COMMAND fuse_apply_asan)
    fuse_apply_asan(fuse_runtime_smoke)
endif()

if(FUSE_T3D_LEGACY_ENGINE_PROBE)
    target_compile_definitions(fuse_runtime_smoke PRIVATE FUSE_T3D_LEGACY_ENGINE_PROBE=1)
endif()
if(FUSE_T3D_LEGACY_ENGINE_PROBE_PNG)
    target_compile_definitions(fuse_runtime_smoke PRIVATE FUSE_T3D_LEGACY_ENGINE_PROBE_PNG=1)
endif()

add_test(NAME fuse_runtime_smoke COMMAND fuse_runtime_smoke)

if(COMMAND fuse_shipping_assert_runtime_targets)
    fuse_shipping_assert_runtime_targets()
endif()

# B1.8 "Zero heap allocations anywhere in engine code": engine-wide steady-state frame audit.
# Replaces global operator new (and the glibc malloc family) with backtrace-capturing counters and
# drives input/profiler/jobs/frame memory/scene/hybrid/physics/animation/audio/vfx/log per frame.
if(TARGET fuse_hybrid AND TARGET fuse_scene AND TARGET fuse_animation AND TARGET fuse_audio AND TARGET fuse_vfx)
    add_executable(fuse_runtime_steady_state_alloc steady_state_alloc.cpp)
    target_link_libraries(fuse_runtime_steady_state_alloc PRIVATE
        fuse_hybrid fuse_scene fuse_animation fuse_audio fuse_vfx fuse_world3d fuse_world2d fuse_ecs fuse_core
        ${CMAKE_DL_LIBS})
    target_compile_features(fuse_runtime_steady_state_alloc PRIVATE cxx_std_20)
    set(_fuse_steady_cmd $<TARGET_FILE:fuse_runtime_steady_state_alloc>)
    if(FUSE_BUILD_VULKAN AND EXISTS "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
        set(_fuse_steady_cmd "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh" ${_fuse_steady_cmd})
    endif()
    add_test(NAME fuse_runtime_steady_state_alloc COMMAND ${_fuse_steady_cmd})
    set_tests_properties(fuse_runtime_steady_state_alloc PROPERTIES
        LABELS "gate" RUN_SERIAL TRUE TIMEOUT 600 ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}")
endif()
