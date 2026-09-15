add_executable(fuse_editor_api_tests
    test_command_queue.cpp
)

target_link_libraries(fuse_editor_api_tests PRIVATE fuse_editor_api fuse_core)
add_test(NAME fuse_editor_command_queue COMMAND fuse_editor_api_tests)

add_executable(fuse_editor_host_tests
    test_editor_host.cpp
)

target_link_libraries(fuse_editor_host_tests PRIVATE fuse_editor_api fuse_core)
add_test(NAME fuse_editor_host COMMAND fuse_editor_host_tests)

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
