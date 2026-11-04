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
