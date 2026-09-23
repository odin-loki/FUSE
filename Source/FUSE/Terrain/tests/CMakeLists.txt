add_executable(fuse_terrain_tests
    test_terrain.cpp
)

target_link_libraries(fuse_terrain_tests PRIVATE fuse_terrain fuse_core)
add_test(NAME fuse_terrain_b75 COMMAND fuse_terrain_tests)

add_executable(fuse_b7_terrain_gates_tests
    test_b7_terrain_gates.cpp
)

target_link_libraries(fuse_b7_terrain_gates_tests PRIVATE fuse_terrain fuse_core)
add_test(NAME fuse_b7_terrain_gates COMMAND fuse_b7_terrain_gates_tests)
set_tests_properties(fuse_b7_terrain_gates PROPERTIES RUN_SERIAL TRUE LABELS "perf;gate")
