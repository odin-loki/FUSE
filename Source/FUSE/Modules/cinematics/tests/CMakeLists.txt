add_executable(fuse_cinematics_tests
    test_cinematics.cpp
)

target_link_libraries(fuse_cinematics_tests PRIVATE fuse_cinematics fuse_core)

add_test(NAME fuse_cinematics_tests COMMAND fuse_cinematics_tests)
