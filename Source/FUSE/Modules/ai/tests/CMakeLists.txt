add_executable(fuse_ai_tests
    test_behavior_tree.cpp
    test_spatial_query.cpp
    test_parallel_policy.cpp
)

target_link_libraries(fuse_ai_tests PRIVATE fuse_ai fuse_core)
if(FUSE_BUILD_SCRIPT)
    target_link_libraries(fuse_ai_tests PRIVATE fuse_script)
endif()
add_test(NAME fuse_ai_behavior_tree COMMAND fuse_ai_tests)

add_executable(fuse_ai_nav_query_tests test_nav_query.cpp)
target_link_libraries(fuse_ai_nav_query_tests PRIVATE fuse_ai fuse_navigation fuse_core)
add_test(NAME fuse_ai_nav_query COMMAND fuse_ai_nav_query_tests)
