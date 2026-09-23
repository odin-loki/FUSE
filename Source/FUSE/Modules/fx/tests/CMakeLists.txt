add_executable(fuse_fx_tests
    test_fx_runtime.cpp
)

target_link_libraries(fuse_fx_tests PRIVATE fuse_fx fuse_core)
add_test(NAME fuse_fx_runtime COMMAND fuse_fx_tests)

# Single-source particle pool integrate kernel gate (docs/compute-kernels.md): CPU pool tick ==
# packed-SSBO kernel (the CUDA body) bit-exact on CpuReference / CpuParallel, stats, GPU fallback.
add_executable(fuse_fx_particle_pool_kernel_parity test_particle_pool_kernel_parity.cpp)
target_link_libraries(fuse_fx_particle_pool_kernel_parity PRIVATE fuse_fx fuse_core)
target_compile_features(fuse_fx_particle_pool_kernel_parity PRIVATE cxx_std_20)
add_test(NAME fuse_fx_particle_pool_kernel_parity COMMAND fuse_fx_particle_pool_kernel_parity)
set_tests_properties(fuse_fx_particle_pool_kernel_parity PROPERTIES LABELS "gate" TIMEOUT 300)
