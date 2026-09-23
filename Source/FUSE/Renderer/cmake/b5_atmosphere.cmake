# B5 atmosphere: sources (target_sources(fuse_rhi PRIVATE ...)) and gate tests for this topic.
# Owned by one work stream; other topics must not edit this file.

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/atmosphere/sky_output.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/atmosphere/sky_output.cpp
)

if(FUSE_BUILD_CORE_TESTS)
    add_executable(fuse_b5_atmosphere_gates ${CMAKE_CURRENT_LIST_DIR}/../tests/test_b5_atmosphere_gates.cpp)
    target_link_libraries(fuse_b5_atmosphere_gates PRIVATE fuse_rhi)
    add_test(NAME fuse_b5_atmosphere_gates COMMAND fuse_b5_atmosphere_gates)
endif()
