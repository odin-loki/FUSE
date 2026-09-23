# B5 ddgi: sources (target_sources(fuse_rhi PRIVATE ...)) and gate tests for this topic.
# Owned by one work stream; other topics must not edit this file.

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/renderer/gi/ddgi_cpu.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gi/ddgi_cpu.cpp
)

if(FUSE_BUILD_CORE_TESTS)
    add_executable(fuse_b5_ddgi_gates ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_b5_ddgi_gates.cpp)
    target_link_libraries(fuse_b5_ddgi_gates PRIVATE fuse_rhi)
    add_test(NAME fuse_b5_ddgi_gates COMMAND fuse_b5_ddgi_gates)
    # CPU timing of one 64-probe x 256-ray update; serial so parallel tests do not skew it.
    add_test(NAME fuse_b5_ddgi_timing COMMAND fuse_b5_ddgi_gates --timing)
    set_tests_properties(fuse_b5_ddgi_timing PROPERTIES RUN_SERIAL TRUE)
endif()
