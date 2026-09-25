add_executable(fuse_runtime_smoke
    main.cpp
)

target_link_libraries(fuse_runtime_smoke
    PRIVATE
        fuse_core
        fuse_world2d
        fuse_world3d
)

if(TARGET fuse_b7)
    target_link_libraries(fuse_runtime_smoke PRIVATE fuse_b7)
    target_compile_definitions(fuse_runtime_smoke PRIVATE FUSE_HAS_B7_INTEGRATION=1)
endif()

if(FUSE_BUILD_VULKAN)
    target_link_libraries(fuse_runtime_smoke PRIVATE fuse_rhi)
    target_compile_definitions(fuse_runtime_smoke PRIVATE FUSE_HAS_VULKAN_RHI=1)
endif()

fuse_apply_cxx23(fuse_runtime_smoke)

if(MINGW)
    target_link_options(fuse_runtime_smoke PRIVATE -Wl,--allow-multiple-definition)
endif()

if(COMMAND fuse_apply_asan)
    fuse_apply_asan(fuse_runtime_smoke)
endif()

add_test(NAME fuse_runtime_smoke COMMAND fuse_runtime_smoke)

if(COMMAND fuse_shipping_assert_runtime_targets)
    fuse_shipping_assert_runtime_targets()
endif()

# B1.8 "Zero heap allocations anywhere in engine code": engine-wide steady-state frame audit.
if(TARGET fuse_hybrid AND TARGET fuse_scene AND TARGET fuse_animation AND TARGET fuse_audio AND TARGET fuse_vfx)
    add_executable(fuse_runtime_steady_state_alloc steady_state_alloc.cpp)
    target_link_libraries(fuse_runtime_steady_state_alloc PRIVATE
        fuse_hybrid fuse_scene fuse_animation fuse_audio fuse_vfx fuse_world3d fuse_world2d fuse_ecs fuse_core
        ${CMAKE_DL_LIBS})
    target_compile_features(fuse_runtime_steady_state_alloc PRIVATE cxx_std_20)
    if(TARGET fuse_script AND TARGET fuse_script_physics)
        target_link_libraries(fuse_runtime_steady_state_alloc PRIVATE fuse_script fuse_script_physics)
        target_compile_definitions(fuse_runtime_steady_state_alloc PRIVATE FUSE_STEADY_HAS_SCRIPT=1)
    endif()
    if(TARGET fuse_net)
        target_link_libraries(fuse_runtime_steady_state_alloc PRIVATE fuse_net)
        target_compile_definitions(fuse_runtime_steady_state_alloc PRIVATE FUSE_STEADY_HAS_NET=1)
    endif()
    if(TARGET fuse_editor_api)
        target_link_libraries(fuse_runtime_steady_state_alloc PRIVATE fuse_editor_api)
        target_compile_definitions(fuse_runtime_steady_state_alloc PRIVATE FUSE_STEADY_HAS_EDITOR=1)
        if(COMMAND fuse_apply_cxx23)
            fuse_apply_cxx23(fuse_runtime_steady_state_alloc)
        endif()
    endif()
    set(_fuse_steady_cmd fuse_runtime_steady_state_alloc)
    if(FUSE_BUILD_VULKAN AND EXISTS "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
        set(_fuse_steady_cmd "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh"
            $<TARGET_FILE:fuse_runtime_steady_state_alloc>)
    endif()
    add_test(NAME fuse_runtime_steady_state_alloc COMMAND ${_fuse_steady_cmd})
    set_tests_properties(fuse_runtime_steady_state_alloc PROPERTIES
        LABELS "gate" RUN_SERIAL TRUE TIMEOUT 600 ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}")
endif()
