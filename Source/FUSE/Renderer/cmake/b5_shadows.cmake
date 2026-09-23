# B5 shadows: sources (target_sources(fuse_rhi PRIVATE ...)) and gate tests for this topic.
# Owned by one work stream; other topics must not edit this file.
#
# SDF soft shadows are header-only (include/fuse/renderer/shadow/sdf_soft_shadow.hpp); CSM lives in
# src/shadow/csm.cpp which is already part of fuse_rhi.

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/shadow/sdf_soft_shadow.hpp
)

if(FUSE_BUILD_CORE_TESTS)
    add_executable(fuse_b5_shadows_gates ${CMAKE_CURRENT_LIST_DIR}/../tests/test_b5_shadows_gates.cpp)
    target_link_libraries(fuse_b5_shadows_gates PRIVATE fuse_rhi)
    add_test(NAME fuse_b5_shadows_gates COMMAND fuse_b5_shadows_gates)

    # Pre-existing CSM guard test that was never registered.
    add_executable(fuse_csm_guards ${CMAKE_CURRENT_LIST_DIR}/../tests/test_csm_guards.cpp)
    target_link_libraries(fuse_csm_guards PRIVATE fuse_rhi)
    add_test(NAME fuse_csm_guards COMMAND fuse_csm_guards)
endif()

# Single-source `sdf_shadows` pass (docs/compute-kernels.md). The occluder SDF is the ray march's
# compute::ray_march_kernel::scene_eval: fuse_rhi uses fuse_compute's device-safe headers only (header-only,
# no link dependency, so fuse_rhi still does not depend on the fuse_compute library).
target_include_directories(fuse_rhi PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../../Compute/include>)
target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/shadow/sdf_shadows.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/shadow/sdf_shadow_kernel.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/shadow/sdf_shadows.cpp
)
if(FUSE_CUDA_BACKEND)
    target_sources(fuse_rhi PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../kernels/sdf_shadows.cu)
endif()

if(FUSE_BUILD_CORE_TESTS)
    # CpuReference == CpuParallel bit-exact (0/2/4 workers), "sdf_shadows" stats, GPU-request fallback.
    add_executable(fuse_sdf_shadows_kernel_parity ${CMAKE_CURRENT_LIST_DIR}/../tests/test_sdf_shadows_kernel_parity.cpp)
    target_link_libraries(fuse_sdf_shadows_kernel_parity PRIVATE fuse_rhi)
    add_test(NAME fuse_sdf_shadows_kernel_parity COMMAND fuse_sdf_shadows_kernel_parity)
    set_tests_properties(fuse_sdf_shadows_kernel_parity PROPERTIES LABELS "gate" TIMEOUT 300)
endif()
