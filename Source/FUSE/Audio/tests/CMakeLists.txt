add_executable(fuse_audio_tests
    test_audio_engine.cpp
)

target_link_libraries(fuse_audio_tests PRIVATE fuse_audio fuse_core)
add_test(NAME fuse_audio_b72 COMMAND fuse_audio_tests)
