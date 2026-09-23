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
