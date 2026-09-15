add_executable(fuse_ai_tests
    test_behavior_tree.cpp
)

target_link_libraries(fuse_ai_tests PRIVATE fuse_ai fuse_core)
add_test(NAME fuse_ai_behavior_tree COMMAND fuse_ai_tests)
