# Look system (data-driven effect graph, .fuselook blend space, 3D LUT grading, lens / HDR-output
# kernels): fuse_rhi sources and the gate test. Owned by one work stream; other topics must not edit
# this file. Kernels are single-source (docs/compute-kernels.md): include/fuse/renderer/look/look_kernels.hpp.

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/look/effect_graph.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/look/look_cas_bridge.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/look/look_kernels.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/look/look_params.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/look/look_post_chain.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/look/look_schema.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/look/look_system.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/look/lut3d.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/look/look_cas_bridge.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/look/look_params.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/look/look_post_chain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/look/look_schema.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/look/look_system.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/look/lut3d.cpp
)

if(FUSE_BUILD_CORE_TESTS)
    add_executable(fuse_look_gates ${CMAKE_CURRENT_LIST_DIR}/../tests/test_look_gates.cpp)
    target_link_libraries(fuse_look_gates PRIVATE fuse_rhi)
    target_compile_definitions(fuse_look_gates PRIVATE
        FUSE_LOOK_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/Samples/Looks")
    add_test(NAME fuse_look_gates COMMAND fuse_look_gates)
    set_tests_properties(fuse_look_gates PROPERTIES LABELS "gate;look" TIMEOUT 600)
endif()
