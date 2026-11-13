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

# UNI-U3-SCRIPT-1: chunks load through ScriptHostService; a Script component runs via ScriptSystem.
if(TARGET fuse_script)
    target_link_libraries(fuse_runtime_smoke PRIVATE fuse_script)
    target_compile_definitions(fuse_runtime_smoke PRIVATE FUSE_SMOKE_HAS_SCRIPT=1)
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

# E03: `--gpu-frame` renders a World3D cube through the hybrid SceneRenderer path and checks it by GPU readback (the
# valgrind twin runs without it: Lavapipe's JIT under memcheck is too slow for the frame composer).
if(TARGET fuse_hybrid AND FUSE_BUILD_VULKAN)
    target_link_libraries(fuse_runtime_smoke PRIVATE fuse_hybrid)
    target_compile_definitions(fuse_runtime_smoke PRIVATE FUSE_SMOKE_HAS_HYBRID=1)
endif()
if(TARGET fuse_hybrid AND FUSE_VULKAN_BACKEND AND NOT CMAKE_CROSSCOMPILING
   AND EXISTS "${FUSE_VULKAN_ICD_LOCK_PATH}")
    add_test(NAME fuse_runtime_smoke
             COMMAND ${FUSE_VULKAN_TEST_LAUNCHER}
                     $<TARGET_FILE:fuse_runtime_smoke> --gpu-frame)
    set_tests_properties(fuse_runtime_smoke PROPERTIES
        ENVIRONMENT "${FUSE_VULKAN_LVP_ENV}" RUN_SERIAL TRUE)
else()
    add_test(NAME fuse_runtime_smoke COMMAND fuse_runtime_smoke)
endif()

# B7.10 "Engine boots ... scene loads, physics runs, audio plays in < 3 seconds": `--boot-budget-ms` times
# process start -> engine boot (core, jobs, VFS, scripts, renderer, GPU frame) -> .fuselevel load -> 30
# physics steps -> a tone through the audio mixer, and fails over the budget. The editor is not in this
# process. Label "perf": the 3 s budget is for the reference workstation (ThinkStation P920 + RTX 3090).
# The Lavapipe variant below runs the GPU frame on a software rasteriser, where "hybrid renderer + 4 GPU frames" is
# the driver compiling the frame composer's kernels on the CPU (LLVM): 4.7 s before the kernels took the correctly
# rounded division / square root (common/exact_math) and the tiled light loop, 12.3 s with them on the machine that
# measured both (a GitHub ubuntu-24.04 runner ran the 18.6 s of the tree before the SSRC change in 11.9 s). That
# phase gets its own budget there (--boot-budget-software-gpu-ms, applied only when the device is a
# CPU one); everything else of the boot (about 60 ms) keeps the 3 s. On a hardware device the 3 s cover it all.
if(TARGET fuse_audio AND FUSE_BUILD_PROJECT)
    target_link_libraries(fuse_runtime_smoke PRIVATE fuse_audio)
    target_compile_definitions(fuse_runtime_smoke PRIVATE FUSE_SMOKE_HAS_BOOT_SCENE=1)
    if(TARGET fuse_hybrid AND FUSE_VULKAN_BACKEND AND NOT CMAKE_CROSSCOMPILING
       AND EXISTS "${FUSE_VULKAN_ICD_LOCK_PATH}")
        add_test(NAME fuse_runtime_boot_time
                 COMMAND ${FUSE_VULKAN_TEST_LAUNCHER}
                         $<TARGET_FILE:fuse_runtime_smoke> --gpu-frame --boot-budget-ms 3000
                         --boot-budget-software-gpu-ms 20000)
        set_tests_properties(fuse_runtime_boot_time PROPERTIES
            ENVIRONMENT "${FUSE_VULKAN_LVP_ENV}" RUN_SERIAL TRUE LABELS "perf")
    else()
        add_test(NAME fuse_runtime_boot_time COMMAND fuse_runtime_smoke --boot-budget-ms 3000)
        set_tests_properties(fuse_runtime_boot_time PROPERTIES LABELS "perf")
    endif()
endif()

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
    if(FUSE_BUILD_VULKAN AND EXISTS "${FUSE_VULKAN_ICD_LOCK_PATH}")
        set(_fuse_steady_cmd ${FUSE_VULKAN_TEST_LAUNCHER}
            $<TARGET_FILE:fuse_runtime_steady_state_alloc>)
    endif()
    add_test(NAME fuse_runtime_steady_state_alloc COMMAND ${_fuse_steady_cmd})
    set_tests_properties(fuse_runtime_steady_state_alloc PROPERTIES
        LABELS "gate" RUN_SERIAL TRUE TIMEOUT 600 ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}")
endif()
