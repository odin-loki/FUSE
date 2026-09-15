add_executable(fuse_animation_tests
    test_animation.cpp
)

target_link_libraries(fuse_animation_tests PRIVATE fuse_animation fuse_core fuse_ecs)
add_test(NAME fuse_animation_runtime COMMAND fuse_animation_tests)
