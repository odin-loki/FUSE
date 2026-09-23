add_executable(fuse_audio_tests
    test_audio_engine.cpp
)

target_link_libraries(fuse_audio_tests PRIVATE fuse_audio fuse_core)
add_test(NAME fuse_audio_b72 COMMAND fuse_audio_tests)

add_executable(fuse_audio_bus_tests
    test_audio_bus.cpp
)

target_link_libraries(fuse_audio_bus_tests PRIVATE fuse_audio fuse_core)
add_test(NAME fuse_audio_bus COMMAND fuse_audio_bus_tests)

add_executable(fuse_audio_binaural_hrtf_tests
    test_binaural_hrtf.cpp
)

target_link_libraries(fuse_audio_binaural_hrtf_tests PRIVATE fuse_audio fuse_core)
add_test(NAME fuse_audio_binaural_hrtf COMMAND fuse_audio_binaural_hrtf_tests)

add_executable(fuse_audio_occlusion_reverb_tests
    test_occlusion_reverb.cpp
)

target_link_libraries(fuse_audio_occlusion_reverb_tests PRIVATE fuse_audio fuse_core)
add_test(NAME fuse_audio_occlusion_reverb COMMAND fuse_audio_occlusion_reverb_tests)

# B7.2 Phase 7 deliverable gates (headless: null/OpenAL backend + offline mix buffers).
add_executable(fuse_audio_b7_gates_tests
    test_b7_audio_gates.cpp
)

target_link_libraries(fuse_audio_b7_gates_tests PRIVATE fuse_audio fuse_core)
add_test(NAME fuse_audio_b7_gates COMMAND fuse_audio_b7_gates_tests)
set_tests_properties(fuse_audio_b7_gates PROPERTIES LABELS "gate")

# CPU budget: 32 spatial voices x 512 frames within 10% of the buffer period (NDEBUG only).
add_test(NAME fuse_audio_b7_perf COMMAND fuse_audio_b7_gates_tests --perf)
set_tests_properties(fuse_audio_b7_perf PROPERTIES RUN_SERIAL TRUE LABELS "perf;gate")
