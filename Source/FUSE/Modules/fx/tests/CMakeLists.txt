add_executable(fuse_fx_tests
    test_fx_runtime.cpp
)

target_link_libraries(fuse_fx_tests PRIVATE fuse_fx fuse_core)
add_test(NAME fuse_fx_runtime COMMAND fuse_fx_tests)
