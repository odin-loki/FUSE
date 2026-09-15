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
