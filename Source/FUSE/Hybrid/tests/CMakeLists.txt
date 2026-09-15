add_executable(fuse_hybrid_tests
    test_hybrid_composer.cpp
    test_frame_pipeline_mt.cpp
)

target_link_libraries(fuse_hybrid_tests PRIVATE fuse_hybrid fuse_core)
target_compile_features(fuse_hybrid_tests PRIVATE cxx_std_17)

add_test(NAME fuse_hybrid_tests COMMAND fuse_hybrid_tests)
