add_executable(fuse_scene_serial_tests
    test_main.cpp
    test_camera.cpp
    test_scene_serialiser.cpp
)

target_link_libraries(fuse_scene_serial_tests PRIVATE fuse_scene fuse_project)
add_test(NAME fuse_scene_b37_b39 COMMAND fuse_scene_serial_tests)

add_executable(fuse_scene_svo_tests
    test_svo.cpp
)

target_link_libraries(fuse_scene_svo_tests PRIVATE fuse_scene fuse_core fuse_ecs)
add_test(NAME fuse_scene_svo COMMAND fuse_scene_svo_tests)

add_executable(fuse_scene_manager_tests
    test_scene_manager.cpp
)

target_link_libraries(fuse_scene_manager_tests PRIVATE fuse_scene fuse_core fuse_ecs)
add_test(NAME fuse_scene_manager COMMAND fuse_scene_manager_tests)

add_executable(fuse_ecs_phase3_integration_tests
    test_phase3_integration.cpp
)

target_link_libraries(fuse_ecs_phase3_integration_tests PRIVATE fuse_scene fuse_core fuse_ecs)
add_test(NAME fuse_ecs_phase3_integration COMMAND fuse_ecs_phase3_integration_tests)

add_executable(fuse_scene_wire_stub_tests
    test_wire_stub.cpp
)

target_link_libraries(fuse_scene_wire_stub_tests PRIVATE fuse_scene)
add_test(NAME fuse_scene_wire_stub COMMAND fuse_scene_wire_stub_tests)

add_executable(fuse_scene_wire_runtime_bind_tests
    test_wire_runtime_bind.cpp
)

target_link_libraries(fuse_scene_wire_runtime_bind_tests PRIVATE fuse_scene fuse_ecs fuse_project fuse_world_converter)
add_test(NAME fuse_scene_wire_runtime_bind COMMAND fuse_scene_wire_runtime_bind_tests)

add_executable(fuse_p4_golden_mis
    test_p4_golden_mis.cpp
)

file(TO_CMAKE_PATH
    "${CMAKE_SOURCE_DIR}/Samples/unification/demo_3d_empty/worlds/example.mis"
    FUSE_GOLDEN_MIS_PATH_CMAKE
)

target_link_libraries(fuse_p4_golden_mis PRIVATE fuse_scene fuse_project fuse_core)
target_compile_definitions(fuse_p4_golden_mis PRIVATE
    FUSE_GOLDEN_MIS_PATH="${FUSE_GOLDEN_MIS_PATH_CMAKE}"
)
add_test(NAME fuse_p4_golden_mis COMMAND fuse_p4_golden_mis)
set_tests_properties(fuse_p4_golden_mis PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(fuse_p7_parity_scenes
    test_p7_parity_scenes.cpp
)

file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" FUSE_SOURCE_DIR_CMAKE)
target_link_libraries(fuse_p7_parity_scenes PRIVATE fuse_scene fuse_project fuse_world_converter fuse_core)
target_compile_definitions(fuse_p7_parity_scenes PRIVATE
    FUSE_SOURCE_DIR="${FUSE_SOURCE_DIR_CMAKE}"
)
add_test(NAME fuse_p7_parity_scenes COMMAND fuse_p7_parity_scenes)
set_tests_properties(fuse_p7_parity_scenes PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
