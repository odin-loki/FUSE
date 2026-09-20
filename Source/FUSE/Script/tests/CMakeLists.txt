add_executable(fuse_script_tests
    test_script_console.cpp
    test_script_host.cpp
)

target_link_libraries(fuse_script_tests PRIVATE fuse_script fuse_core)
add_test(NAME fuse_script_b73 COMMAND fuse_script_tests)

add_executable(fuse_script_compat_route_tests
    test_script_compat_route.cpp
)

target_link_libraries(fuse_script_compat_route_tests PRIVATE fuse_script fuse_core)
add_test(NAME fuse_script_compat_route COMMAND fuse_script_compat_route_tests)

if(MINGW)
    target_link_options(fuse_script_tests PRIVATE -Wl,--allow-multiple-definition)
    target_link_options(fuse_script_compat_route_tests PRIVATE -Wl,--allow-multiple-definition)
endif()
