add_executable(fuse_cinematics_tests
    test_cinematics.cpp
)

target_link_libraries(fuse_cinematics_tests PRIVATE fuse_cinematics fuse_core)
target_compile_features(fuse_cinematics_tests PRIVATE cxx_std_17)

add_test(NAME fuse_cinematics_tests COMMAND fuse_cinematics_tests)
