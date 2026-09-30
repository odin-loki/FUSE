add_executable(fuse_animation_tests
    test_animation.cpp
)

target_link_libraries(fuse_animation_tests PRIVATE fuse_animation fuse_core fuse_ecs)
add_test(NAME fuse_animation_runtime COMMAND fuse_animation_tests)

# B7.1 / B7.10 animation gates: skeleton binary load, clip keyframe sampling, BlendNode2,
# state-machine crossfade timing, FABRIK / TwoBoneIK accuracy, Animator bone buffer (no leak).
add_executable(fuse_b7_animation_gates
    test_b7_animation_gates.cpp
)

target_link_libraries(fuse_b7_animation_gates PRIVATE fuse_animation fuse_core fuse_ecs)
add_test(NAME fuse_b7_animation_gates COMMAND fuse_b7_animation_gates)
set_tests_properties(fuse_b7_animation_gates PROPERTIES LABELS "gate")

# Single-source skinning kernel gate (docs/compute-kernels.md): CpuReference vs CpuParallel bit-exact,
# "skinning_lbs" stats, GPU-without-device fallback, validation.
add_executable(fuse_skinning_kernel_parity test_skinning_kernel_parity.cpp)
target_link_libraries(fuse_skinning_kernel_parity PRIVATE fuse_animation fuse_core)
target_compile_features(fuse_skinning_kernel_parity PRIVATE cxx_std_20)
add_test(NAME fuse_skinning_kernel_parity COMMAND fuse_skinning_kernel_parity)
set_tests_properties(fuse_skinning_kernel_parity PROPERTIES LABELS "gate" TIMEOUT 300)

# B7 row "GPU skinning transforms 10k vertices in < 0.5 ms — verified with CUDA events": CPU parity of the
# 10k-vertex gate mesh everywhere; CUDA-event timing on resident buffers + device parity on the RTX 3090
# (time_skinning_cuda, kernels/skinning.cu). Exit 77 after the CPU checks when there is no CUDA device.
add_executable(fuse_b7_skinning_cuda_events test_skinning_cuda_events.cpp)
target_link_libraries(fuse_b7_skinning_cuda_events PRIVATE fuse_animation fuse_core)
target_compile_features(fuse_b7_skinning_cuda_events PRIVATE cxx_std_20)
add_test(NAME fuse_b7_skinning_cuda_events COMMAND fuse_b7_skinning_cuda_events)
set_tests_properties(fuse_b7_skinning_cuda_events PROPERTIES
    LABELS "gate;cuda;cuda_device" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 300)

# MP-B7.1-PARALLEL-EVAL: AnimationSystem parallel == serial (bit-identical) over 1000 Animators,
# worker-count independent, zero steady-state allocations.
add_executable(fuse_animation_system_gates test_animation_system.cpp)
target_link_libraries(fuse_animation_system_gates PRIVATE fuse_animation fuse_core fuse_ecs)
add_test(NAME fuse_animation_system_gates COMMAND fuse_animation_system_gates)
set_tests_properties(fuse_animation_system_gates PROPERTIES LABELS "gate" TIMEOUT 300)
