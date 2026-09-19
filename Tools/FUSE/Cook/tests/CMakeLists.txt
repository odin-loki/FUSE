add_executable(fuse_bc7_encoder_tests
    test_bc7_encoder.cpp
)

target_link_libraries(fuse_bc7_encoder_tests PRIVATE fuse_cook_stubs fuse_core)
add_test(NAME fuse_bc7_encoder COMMAND fuse_bc7_encoder_tests)
