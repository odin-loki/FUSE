add_executable(fuse_scene_tests
    test_scene_hierarchy.cpp
)

target_link_libraries(fuse_scene_tests PRIVATE fuse_world3d)
add_test(NAME fuse_scene_hierarchy COMMAND fuse_scene_tests)
