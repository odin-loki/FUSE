add_executable(fuse_scene_serial_tests
    test_main.cpp
    test_camera.cpp
    test_scene_serialiser.cpp
)

target_link_libraries(fuse_scene_serial_tests PRIVATE fuse_scene fuse_project)
add_test(NAME fuse_scene_b37_b39 COMMAND fuse_scene_serial_tests)
