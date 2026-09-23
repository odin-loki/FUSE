# B5 taa_ssfx: sources (target_sources(fuse_rhi PRIVATE ...)) and gate tests for this topic.
# Owned by one work stream; other topics must not edit this file.

# B5.7 screen-space effects (HBAO, SSR, SSGI) CPU references live in the shared Qt-/Vulkan-free fuse_ssfx
# library (Source/FUSE/ScreenSpace) so fuse_compute can reuse them without depending on fuse_rhi. The headers
# under fuse/renderer/ssfx/ forward into namespace fuse::renderer.
include(${CMAKE_CURRENT_LIST_DIR}/../../ScreenSpace/fuse_ssfx.cmake)
target_link_libraries(fuse_rhi PUBLIC fuse_ssfx)

# B5.9 CPU reference TAA resolve + forwarding headers for the screen-space effect references.
target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/taa/taa_cpu_resolve.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/ssfx/ssfx_view.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/ssfx/hbao.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/ssfx/ssr.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/ssfx/ssgi.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/taa/taa_cpu_resolve.cpp
)

if(FUSE_BUILD_CORE_TESTS)
    add_executable(fuse_b5_taa_ssfx_gates ${CMAKE_CURRENT_LIST_DIR}/../tests/test_b5_taa_ssfx_gates.cpp)
    target_link_libraries(fuse_b5_taa_ssfx_gates PRIVATE fuse_rhi)
    add_test(NAME fuse_b5_taa_ssfx_gates COMMAND fuse_b5_taa_ssfx_gates)
endif()
