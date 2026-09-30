add_executable(fuse_hybrid_tests
    test_hybrid_composer.cpp
    test_frame_pipeline_mt.cpp
    test_frame_barrier_integration.cpp
    test_parallel_cull_edge.cpp
)

add_executable(fuse_hybrid_cooked_bindings test_cooked_asset_bindings.cpp)
target_link_libraries(fuse_hybrid_cooked_bindings PRIVATE fuse_hybrid fuse_core)
add_test(NAME fuse_hybrid_cooked_bindings COMMAND fuse_hybrid_cooked_bindings)

target_link_libraries(fuse_hybrid_tests PRIVATE fuse_hybrid fuse_core)

if(FUSE_BUILD_VULKAN)
    add_test(NAME fuse_hybrid_tests COMMAND "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh" "$<TARGET_FILE:fuse_hybrid_tests>")
    set_tests_properties(fuse_hybrid_tests PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE
    )
else()
    add_test(NAME fuse_hybrid_tests COMMAND fuse_hybrid_tests)
endif()

if(FUSE_BUILD_VULKAN)
    set(FUSE_VULKAN_TEST_ENV "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json")

    set(_fuse_hybrid_vulkan_icd_tests
        fuse_hybrid_renderer_bootstrap
        fuse_hybrid_vulkan_presentable
        fuse_placeholder_renderer_retirement
    )

    add_executable(fuse_render_command_list test_render_command_list.cpp)
    target_link_libraries(fuse_render_command_list PRIVATE fuse_hybrid fuse_core)
    add_test(NAME fuse_render_command_list COMMAND "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh" "$<TARGET_FILE:fuse_render_command_list>")
    set_tests_properties(fuse_render_command_list PROPERTIES
        ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}"
        RUN_SERIAL TRUE
    )

    add_executable(fuse_hybrid_renderer_bootstrap test_hybrid_renderer_bootstrap.cpp)
    target_link_libraries(fuse_hybrid_renderer_bootstrap PRIVATE fuse_hybrid fuse_core)
    add_test(NAME fuse_hybrid_renderer_bootstrap COMMAND "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh" "$<TARGET_FILE:fuse_hybrid_renderer_bootstrap>")

    add_executable(fuse_hybrid_vulkan_presentable test_vulkan_presentable_stub.cpp)
    target_link_libraries(fuse_hybrid_vulkan_presentable PRIVATE fuse_hybrid fuse_core)
    add_test(NAME fuse_hybrid_vulkan_presentable COMMAND "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh" "$<TARGET_FILE:fuse_hybrid_vulkan_presentable>")

    # B2.11 3.5 follow-up: HybridComposer::render steady-state heap allocations.
    add_executable(fuse_b2_hybrid_alloc_budget test_b2_hybrid_alloc_budget.cpp)
    target_link_libraries(fuse_b2_hybrid_alloc_budget PRIVATE fuse_hybrid fuse_core)
    add_test(NAME fuse_b2_hybrid_alloc_budget COMMAND "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh" "$<TARGET_FILE:fuse_b2_hybrid_alloc_budget>")
    list(APPEND _fuse_hybrid_vulkan_icd_tests fuse_b2_hybrid_alloc_budget)

    add_executable(fuse_placeholder_renderer_retirement test_placeholder_renderer_retirement.cpp)
    target_link_libraries(fuse_placeholder_renderer_retirement PRIVATE fuse_hybrid fuse_core)
    add_test(NAME fuse_placeholder_renderer_retirement COMMAND "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh" "$<TARGET_FILE:fuse_placeholder_renderer_retirement>")

    foreach(_fuse_hybrid_vulkan_test IN LISTS _fuse_hybrid_vulkan_icd_tests)
        if(TEST ${_fuse_hybrid_vulkan_test})
            set_tests_properties(${_fuse_hybrid_vulkan_test} PROPERTIES
                ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}"
                RUN_SERIAL TRUE
            )
        endif()
    endforeach()

    if(MINGW)
        foreach(_fuse_hybrid_link_test IN ITEMS
            fuse_hybrid_tests
            fuse_render_command_list
            fuse_hybrid_renderer_bootstrap
            fuse_hybrid_vulkan_presentable
            fuse_placeholder_renderer_retirement)
            if(TARGET ${_fuse_hybrid_link_test})
                target_link_options(${_fuse_hybrid_link_test} PRIVATE -Wl,--allow-multiple-definition)
            endif()
        endforeach()
    endif()
endif()

# --- E03 hybrid GPU frame (RE-FI-1 / RE-RUNTIME-3D-RENDER / UNI-U4-1): test_hybrid_frame_vk.cpp -------------------
#   fuse_hybrid_sprite_layer_cpu   CPU (every tree): sprite-layer mapping + "over" reference
#   fuse_hybrid_frame_vk_golden    Lavapipe: headless hybrid frame (lit cube + sprite + UI) == golden (FLIP), overlay ==
#                                  CPU sprite layer, 0 validation / sync-validation messages
#   fuse_hybrid_frame_vk_sprite    Lavapipe: "hybrid.sprite_layer" == CPU reference (<= 1 half ulp)
#   fuse_hybrid_frame_vk_zero_alloc  Lavapipe: 30 steady-state frames, 0 render-thread heap allocations
#   fuse_hybrid_frame_vk_swapchain   Xvfb + Lavapipe: 60 frames blitted into X11 swapchain images and presented
# Golden: Hybrid/tests/golden/hybrid_frame_t0.png (FUSE_UPDATE_GOLDENS=1 rewrites it).
add_executable(fuse_hybrid_frame_vk test_hybrid_frame_vk.cpp)
target_link_libraries(fuse_hybrid_frame_vk PRIVATE fuse_hybrid fuse_core)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_hybrid_frame_vk)
endif()
if(TARGET fuse_rp_harness)
    target_link_libraries(fuse_hybrid_frame_vk PRIVATE fuse_rp_harness)
    target_compile_definitions(fuse_hybrid_frame_vk PRIVATE
        FUSE_HYBRID_TEST_HAS_HARNESS=1
        FUSE_HYBRID_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
        FUSE_HYBRID_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/hybrid_frame_artifacts")
endif()
add_test(NAME fuse_hybrid_sprite_layer_cpu COMMAND fuse_hybrid_frame_vk --mode cpu)
set_tests_properties(fuse_hybrid_sprite_layer_cpu PROPERTIES LABELS "gate;hybrid" TIMEOUT 120)
if(FUSE_BUILD_VULKAN)
    set(_fuse_hybrid_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
    set(_fuse_hybrid_frame_tests "")
    foreach(_mode golden sprite zero_alloc)
        if(FUSE_VULKAN_BACKEND)
            add_test(NAME fuse_hybrid_frame_vk_${_mode}
                     COMMAND "${_fuse_hybrid_lock}" "$<TARGET_FILE:fuse_hybrid_frame_vk>" --mode ${_mode})
        else()
            add_test(NAME fuse_hybrid_frame_vk_${_mode} COMMAND fuse_hybrid_frame_vk --mode ${_mode})
        endif()
        list(APPEND _fuse_hybrid_frame_tests fuse_hybrid_frame_vk_${_mode})
    endforeach()
    if(FUSE_VULKAN_BACKEND AND FUSE_PLATFORM_WINDOW_X11_ACTIVE AND FUSE_DISPLAY_TEST_WRAPPER)
        add_test(NAME fuse_hybrid_frame_vk_swapchain
                 COMMAND "${FUSE_DISPLAY_TEST_WRAPPER}" "${_fuse_hybrid_lock}" "$<TARGET_FILE:fuse_hybrid_frame_vk>"
                         --mode swapchain)
    else()
        add_test(NAME fuse_hybrid_frame_vk_swapchain COMMAND fuse_hybrid_frame_vk --mode swapchain)
    endif()
    list(APPEND _fuse_hybrid_frame_tests fuse_hybrid_frame_vk_swapchain)
    set_tests_properties(${_fuse_hybrid_frame_tests} PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        TIMEOUT 1800
        LABELS "gate;vulkan;hybrid")
endif()
