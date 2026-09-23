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
