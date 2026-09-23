# B5 clustered: sources (target_sources(fuse_rhi PRIVATE ...)) and gate tests for this topic.
# Owned by one work stream; other topics must not edit this file.

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/renderer/lighting/clustered_shading.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lighting/clustered_shading.cpp
)

if(FUSE_BUILD_CORE_TESTS)
    add_executable(fuse_b5_clustered_gates ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_b5_clustered_gates.cpp)
    target_link_libraries(fuse_b5_clustered_gates PRIVATE fuse_rhi)
    add_test(NAME fuse_b5_clustered_gates COMMAND fuse_b5_clustered_gates)
endif()

# Single-source clustered cull + deferred shade (docs/compute-kernels.md): cluster build / light bounds /
# cull / compact / shade kernel bodies shared by the CPU backends and the CUDA wrapper.
target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/renderer/lighting/clustered_kernel.hpp
)
if(FUSE_CUDA_BACKEND)
    target_sources(fuse_rhi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/kernels/clustered_lighting.cu)
endif()

if(FUSE_BUILD_CORE_TESTS)
    # CpuReference == CpuParallel bit-exact (0/2/4 workers), clustered_* / deferred_shading stats, fallback.
    add_executable(fuse_clustered_kernel_parity ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_clustered_kernel_parity.cpp)
    target_link_libraries(fuse_clustered_kernel_parity PRIVATE fuse_rhi)
    add_test(NAME fuse_clustered_kernel_parity COMMAND fuse_clustered_kernel_parity)
    set_tests_properties(fuse_clustered_kernel_parity PROPERTIES LABELS "gate" TIMEOUT 300)
endif()
