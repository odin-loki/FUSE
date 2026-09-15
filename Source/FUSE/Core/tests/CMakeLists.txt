add_executable(fuse_core_tests
    test_worker_count.cpp
)

target_link_libraries(fuse_core_tests PRIVATE fuse_core)

add_test(NAME fuse_core_worker_count COMMAND fuse_core_tests)
