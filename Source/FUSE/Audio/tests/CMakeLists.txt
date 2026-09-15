add_executable(fuse_audio_tests
    test_audio_engine.cpp
)

target_link_libraries(fuse_audio_tests PRIVATE fuse_audio fuse_core)
add_test(NAME fuse_audio_b72 COMMAND fuse_audio_tests)

add_executable(fuse_audio_binaural_hrtf_tests
    test_binaural_hrtf.cpp
)

target_link_libraries(fuse_audio_binaural_hrtf_tests PRIVATE fuse_audio fuse_core)
add_test(NAME fuse_audio_binaural_hrtf COMMAND fuse_audio_binaural_hrtf_tests)
