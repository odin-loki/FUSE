add_executable(fuse_editor_api_tests
    test_command_queue.cpp
)

target_link_libraries(fuse_editor_api_tests PRIVATE fuse_editor_api fuse_core)
add_test(NAME fuse_editor_command_queue COMMAND fuse_editor_api_tests)

add_executable(fuse_editor_host_tests
    test_editor_host.cpp
)

target_link_libraries(fuse_editor_host_tests PRIVATE fuse_editor_api fuse_core fuse_scene fuse_project)
if(FUSE_BUILD_VULKAN)
    set(FUSE_VULKAN_ICD_LOCK_SCRIPT "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
    set(FUSE_VULKAN_TEST_ENV "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json")
    add_test(NAME fuse_editor_host COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_editor_host_tests>")
    set_tests_properties(fuse_editor_host PROPERTIES
        ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}"
        RUN_SERIAL TRUE
    )
else()
    add_test(NAME fuse_editor_host COMMAND fuse_editor_host_tests)
endif()

add_executable(fuse_editor_viewport_present_gate_tests
    test_viewport_present_gate.cpp
)

target_link_libraries(fuse_editor_viewport_present_gate_tests PRIVATE fuse_editor_api fuse_core)
if(FUSE_BUILD_VULKAN)
    target_link_libraries(fuse_editor_viewport_present_gate_tests PRIVATE fuse_rhi)
    add_test(NAME fuse_editor_viewport_present_gate COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_editor_viewport_present_gate_tests>")
    set_tests_properties(fuse_editor_viewport_present_gate PROPERTIES
        ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}"
        RUN_SERIAL TRUE
    )
else()
    add_test(NAME fuse_editor_viewport_present_gate COMMAND fuse_editor_viewport_present_gate_tests)
endif()

add_executable(fuse_editor_runtime_embed_tests
    test_runtime_embed_session.cpp
)

target_link_libraries(fuse_editor_runtime_embed_tests PRIVATE fuse_editor_api fuse_core)
if(FUSE_BUILD_VULKAN)
    target_link_libraries(fuse_editor_runtime_embed_tests PRIVATE fuse_hybrid fuse_rhi)
    add_test(NAME fuse_editor_runtime_embed COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_editor_runtime_embed_tests>")
    set_tests_properties(fuse_editor_runtime_embed PROPERTIES
        ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}"
        RUN_SERIAL TRUE
    )
else()
    add_test(NAME fuse_editor_runtime_embed COMMAND fuse_editor_runtime_embed_tests)
endif()

add_executable(fuse_editor_panels_tests
    test_editor_panels.cpp
)

target_link_libraries(fuse_editor_panels_tests PRIVATE fuse_editor_api fuse_core)
add_test(NAME fuse_editor_panels COMMAND fuse_editor_panels_tests)

add_executable(fuse_editor_command_stack_tests
    test_command_stack.cpp
)

target_link_libraries(fuse_editor_command_stack_tests PRIVATE fuse_editor_api fuse_core)
add_test(NAME fuse_editor_command_stack COMMAND fuse_editor_command_stack_tests)

add_executable(fuse_editor_hierarchy_model_tests
    test_hierarchy_model.cpp
)

target_link_libraries(fuse_editor_hierarchy_model_tests
    PRIVATE
        fuse_editor_api
        fuse_core
        fuse_world2d
        fuse_world3d
)
add_test(NAME fuse_editor_hierarchy_model COMMAND fuse_editor_hierarchy_model_tests)

add_executable(fuse_editor_panels_b69_b612_tests
    test_editor_panels_b69_b612.cpp
)

target_link_libraries(fuse_editor_panels_b69_b612_tests PRIVATE fuse_editor_api fuse_core fuse_scene)
add_test(NAME fuse_editor_panels_b69_b612 COMMAND fuse_editor_panels_b69_b612_tests)

add_executable(fuse_editor_phase6_integration_tests
    test_editor_phase6_integration.cpp
)

target_link_libraries(fuse_editor_phase6_integration_tests
    PRIVATE
        fuse_editor_api
        fuse_core
)
add_test(NAME fuse_editor_phase6_integration COMMAND fuse_editor_phase6_integration_tests)

add_executable(fuse_editor_play_session_tests
    test_play_session.cpp
)

target_link_libraries(fuse_editor_play_session_tests PRIVATE fuse_editor_api fuse_core fuse_scene)
add_test(NAME fuse_editor_play_session COMMAND fuse_editor_play_session_tests)

add_executable(fuse_editor_gizmo_system_tests
    test_gizmo_system.cpp
)

target_link_libraries(fuse_editor_gizmo_system_tests PRIVATE fuse_editor_api fuse_core)
add_test(NAME fuse_editor_gizmo_system COMMAND fuse_editor_gizmo_system_tests)
