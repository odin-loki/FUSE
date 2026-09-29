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

# B7.2 "Audio engine initialises OpenAL device and context without error" — only where fuse_audio
# links OpenAL (system or vendored openal-soft). openal-soft's always-built null output driver makes
# it headless: the gate must come up on the real OpenAL backend, never the engine's Null fallback.
if(FUSE_AUDIO_HAS_OPENAL)
    add_test(NAME fuse_audio_b7_openal_init COMMAND fuse_audio_b7_gates_tests --require-openal)
    set_tests_properties(fuse_audio_b7_openal_init PROPERTIES
        LABELS "gate"
        ENVIRONMENT "ALSOFT_DRIVERS=null;ALSOFT_CONF=/dev/null")
endif()

# Single-source "reverb_fft" / "reverb_cmac" kernel gate (docs/compute-kernels.md): backend parity,
# accuracy vs direct convolution, delay-line reuse, zero steady-state allocations, stats, fallback, timing.
add_executable(fuse_reverb_fft_kernel_parity test_reverb_fft_kernel_parity.cpp)
target_link_libraries(fuse_reverb_fft_kernel_parity PRIVATE fuse_audio fuse_core)
add_test(NAME fuse_reverb_fft_kernel_parity COMMAND fuse_reverb_fft_kernel_parity)
set_tests_properties(fuse_reverb_fft_kernel_parity PROPERTIES LABELS "gate" TIMEOUT 300)

# B7 row "Convolution reverb CUDA FFT produces output within -60 dB noise floor of reference CPU FFT": CPU
# FFT reference vs direct convolution everywhere; ReverbCuda on the CUDA device vs the CPU FFT reverb (plus
# CUDA-event block timing) on the RTX 3090. Exit 77 after the CPU checks without a CUDA device.
add_executable(fuse_b7_reverb_cuda_device test_reverb_cuda_device.cpp)
target_link_libraries(fuse_b7_reverb_cuda_device PRIVATE fuse_audio fuse_core)
add_test(NAME fuse_b7_reverb_cuda_device COMMAND fuse_b7_reverb_cuda_device)
set_tests_properties(fuse_b7_reverb_cuda_device PROPERTIES
    LABELS "gate;cuda;cuda_device" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 600)
