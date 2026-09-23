# B5 post: sources (target_sources(fuse_rhi PRIVATE ...)) and gate tests for this topic.
# Owned by one work stream; other topics must not edit this file.

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/postprocess/dof.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/postprocess/motion_blur.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/postprocess/dof.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/postprocess/motion_blur.cpp
)

if(FUSE_BUILD_CORE_TESTS)
    add_executable(fuse_b5_post_gates ${CMAKE_CURRENT_LIST_DIR}/../tests/test_b5_post_gates.cpp)
    target_link_libraries(fuse_b5_post_gates PRIVATE fuse_rhi)
    add_test(NAME fuse_b5_post_gates COMMAND fuse_b5_post_gates)
endif()
