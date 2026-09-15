add_executable(fuse_script_tests
    test_script_console.cpp
    test_script_host.cpp
)

target_link_libraries(fuse_script_tests PRIVATE fuse_script fuse_core)
add_test(NAME fuse_script_b73 COMMAND fuse_script_tests)
