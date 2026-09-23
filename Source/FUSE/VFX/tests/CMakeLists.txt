add_executable(fuse_vfx_tests
    test_vfx.cpp
)

target_link_libraries(fuse_vfx_tests PRIVATE fuse_vfx fuse_core)
add_test(NAME fuse_vfx_runtime COMMAND fuse_vfx_tests)

add_executable(fuse_b7_vfx_gates_tests
    test_b7_vfx_gates.cpp
)

target_link_libraries(fuse_b7_vfx_gates_tests PRIVATE fuse_vfx fuse_core)
add_test(NAME fuse_b7_vfx_gates COMMAND fuse_b7_vfx_gates_tests)
set_tests_properties(fuse_b7_vfx_gates PROPERTIES RUN_SERIAL TRUE LABELS "perf;gate")

# Single-source particle kernel gate (docs/compute-kernels.md): update + count/scan/write compaction,
# CpuReference vs CpuParallel bit-exact, "particle_update"/"particle_compact" stats, GPU fallback.
add_executable(fuse_particle_kernel_parity test_particle_kernel_parity.cpp)
target_link_libraries(fuse_particle_kernel_parity PRIVATE fuse_vfx fuse_core)
target_compile_features(fuse_particle_kernel_parity PRIVATE cxx_std_20)
add_test(NAME fuse_particle_kernel_parity COMMAND fuse_particle_kernel_parity)
set_tests_properties(fuse_particle_kernel_parity PROPERTIES LABELS "gate" TIMEOUT 300)
