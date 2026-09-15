add_executable(fuse_mechanics_tests
    test_register_interact.cpp
)

target_link_libraries(fuse_mechanics_tests PRIVATE fuse_mechanics fuse_core)
target_compile_features(fuse_mechanics_tests PRIVATE cxx_std_17)

add_test(NAME fuse_mechanics_tests COMMAND fuse_mechanics_tests)
