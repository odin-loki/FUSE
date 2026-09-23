add_executable(fuse_ray_march_stub test_ray_march_stub.cpp)
target_link_libraries(fuse_ray_march_stub PRIVATE fuse_compute)
add_test(NAME fuse_ray_march_stub COMMAND fuse_ray_march_stub)

add_executable(fuse_screen_space_effects_stub test_screen_space_effects_stub.cpp)
target_link_libraries(fuse_screen_space_effects_stub PRIVATE fuse_compute fuse_ssfx)
add_test(NAME fuse_screen_space_effects_stub COMMAND fuse_screen_space_effects_stub)

# Gate: "SDF normals are smooth at surface — no faceting visible at any zoom level" (CPU reference normal).
add_executable(fuse_b2_sdf_normal_smoothness test_sdf_normal_smoothness_gates.cpp)
target_link_libraries(fuse_b2_sdf_normal_smoothness PRIVATE fuse_compute)
add_test(NAME fuse_b2_sdf_normal_smoothness COMMAND fuse_b2_sdf_normal_smoothness)
set_tests_properties(fuse_b2_sdf_normal_smoothness PROPERTIES LABELS "gate")

# Single-source kernel port gate: the SDF ray march body (ray_march_kernel.hpp) produces bit-identical
# depth + normal images on CpuReference and CpuParallel, records "sdf_ray_march" stats, and GPU
# requests without a device fall back cleanly.
add_executable(fuse_ray_march_kernel_parity test_ray_march_kernel_parity.cpp)
target_link_libraries(fuse_ray_march_kernel_parity PRIVATE fuse_compute)
add_test(NAME fuse_ray_march_kernel_parity COMMAND fuse_ray_march_kernel_parity)
set_tests_properties(fuse_ray_march_kernel_parity PROPERTIES LABELS "gate" TIMEOUT 300)

# Single-source SSAO / SSR / SSGI port gate (fuse/ssfx/*_kernel.hpp + fuse/compute/screen_space_kernels.hpp):
# CpuReference == CpuParallel bit-exact at 0/2/4 workers, "screen_space_*" stats, GPU-request fallback.
add_executable(fuse_screen_space_kernel_parity test_screen_space_kernel_parity.cpp)
target_link_libraries(fuse_screen_space_kernel_parity PRIVATE fuse_compute fuse_ssfx)
add_test(NAME fuse_screen_space_kernel_parity COMMAND fuse_screen_space_kernel_parity)
set_tests_properties(fuse_screen_space_kernel_parity PROPERTIES LABELS "gate" TIMEOUT 300)
