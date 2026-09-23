# B5 taa_ssfx: sources (target_sources(fuse_rhi PRIVATE ...)) and gate tests for this topic.
# Owned by one work stream; other topics must not edit this file.

# B5.9 CPU reference TAA resolve + B5.7 screen-space effects (HBAO, SSR) CPU references.
target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/taa/taa_cpu_resolve.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/ssfx/ssfx_view.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/ssfx/hbao.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/ssfx/ssr.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/taa/taa_cpu_resolve.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/ssfx/ssfx_view.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/ssfx/hbao.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/ssfx/ssr.cpp
)

if(FUSE_BUILD_CORE_TESTS)
    add_executable(fuse_b5_taa_ssfx_gates ${CMAKE_CURRENT_LIST_DIR}/../tests/test_b5_taa_ssfx_gates.cpp)
    target_link_libraries(fuse_b5_taa_ssfx_gates PRIVATE fuse_rhi)
    add_test(NAME fuse_b5_taa_ssfx_gates COMMAND fuse_b5_taa_ssfx_gates)
endif()
