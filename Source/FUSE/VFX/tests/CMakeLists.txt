add_executable(fuse_vfx_tests
    test_vfx.cpp
)

target_link_libraries(fuse_vfx_tests PRIVATE fuse_vfx fuse_core)
add_test(NAME fuse_vfx_runtime COMMAND fuse_vfx_tests)

add_executable(fuse_b7_vfx_gates_tests
    test_b7_vfx_gates.cpp
)

target_link_libraries(fuse_b7_vfx_gates_tests PRIVATE fuse_vfx fuse_core)
add_test(NAME fuse_b7_vfx_gates COMMAND fuse_b7_vfx_gates_tests)
set_tests_properties(fuse_b7_vfx_gates PROPERTIES RUN_SERIAL TRUE LABELS "perf;gate")
