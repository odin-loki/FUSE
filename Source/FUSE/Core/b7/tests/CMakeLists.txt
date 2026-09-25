add_executable(fuse_b7_integration
    test_b7_integration.cpp
)

target_link_libraries(fuse_b7_integration PRIVATE fuse_b7)
add_test(NAME fuse_b7_integration COMMAND fuse_b7_integration)
