add_executable(demo_hybrid_hud
    main.cpp
    hybrid_module_gates.cpp
)

target_include_directories(demo_hybrid_hud PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Common)

target_link_libraries(demo_hybrid_hud PRIVATE
    fuse_demo_wiring
    fuse_hybrid
    fuse_ai
    fuse_cinematics
    fuse_fx
    fuse_mechanics
    fuse_adventure
    fuse_core
)
if(FUSE_BUILD_SCRIPT)
    target_link_libraries(demo_hybrid_hud PRIVATE fuse_script)
endif()

# UI4 game path: the HUD on a FUSE UI context (hud_ui.cpp), drawn by ui.draw_list in the hybrid UI stage.
if(TARGET fuse_ui_game AND TARGET fuse_ui_vk AND TARGET fuse_rhi AND TARGET fuse_frame AND FUSE_BUILD_VULKAN)
    target_sources(demo_hybrid_hud PRIVATE hud_ui.cpp)
    target_link_libraries(demo_hybrid_hud PRIVATE fuse_ui_game fuse_ui_vk)
    target_compile_definitions(demo_hybrid_hud PRIVATE FUSE_HYBRID_HUD_HAS_UI=1)
    if(FUSE_BUILD_CORE_TESTS AND FUSE_VULKAN_BACKEND AND NOT CMAKE_CROSSCOMPILING
       AND EXISTS "${FUSE_VULKAN_ICD_LOCK_PATH}")
        # Headless: the outpost slice on the GPU with the FUSE UI HUD; checks real text / widgets in the readback.
        add_test(NAME fuse_demo_hybrid_hud_ui COMMAND ${FUSE_VULKAN_TEST_LAUNCHER} "$<TARGET_FILE:demo_hybrid_hud>")
        set_tests_properties(fuse_demo_hybrid_hud_ui PROPERTIES
            ENVIRONMENT "${FUSE_VULKAN_LVP_ENV}" RUN_SERIAL TRUE TIMEOUT 600
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" LABELS "gate;vulkan;hybrid;ui")
    endif()
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_executable(fuse_hybrid_module_gates_tests
        test_hybrid_module_gates.cpp
        hybrid_module_gates.cpp
    )
    target_link_libraries(fuse_hybrid_module_gates_tests PRIVATE
        fuse_hybrid
        fuse_ai
        fuse_cinematics
        fuse_fx
        fuse_mechanics
        fuse_adventure
        fuse_core
    )
    if(FUSE_BUILD_SCRIPT)
        target_link_libraries(fuse_hybrid_module_gates_tests PRIVATE fuse_script)
    endif()
    if(FUSE_BUILD_VULKAN)
        set(FUSE_HYBRID_GATES_VULKAN_TEST_ENV "${FUSE_VULKAN_LVP_ENV}")
        add_test(NAME fuse_hybrid_module_gates_tests
            COMMAND ${FUSE_VULKAN_TEST_LAUNCHER}
                    "$<TARGET_FILE:fuse_hybrid_module_gates_tests>")
        set_tests_properties(fuse_hybrid_module_gates_tests PROPERTIES
            ENVIRONMENT "${FUSE_HYBRID_GATES_VULKAN_TEST_ENV}"
            RUN_SERIAL TRUE
        )
    else()
        add_test(NAME fuse_hybrid_module_gates_tests COMMAND fuse_hybrid_module_gates_tests)
    endif()
endif()

if(COMMAND fuse_apply_asan)
    fuse_apply_asan(demo_hybrid_hud)
endif()
