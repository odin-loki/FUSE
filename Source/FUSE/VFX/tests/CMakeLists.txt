add_executable(fuse_vfx_tests
    test_vfx.cpp
)

target_link_libraries(fuse_vfx_tests PRIVATE fuse_vfx fuse_core)
add_test(NAME fuse_vfx_runtime COMMAND fuse_vfx_tests)
