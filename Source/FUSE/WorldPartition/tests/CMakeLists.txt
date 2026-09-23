add_executable(fuse_world_partition_tests
    test_world_partition.cpp
)

target_link_libraries(fuse_world_partition_tests PRIVATE fuse_world_partition fuse_ecs fuse_core)
add_test(NAME fuse_world_partition_b76 COMMAND fuse_world_partition_tests)

add_executable(fuse_b7_streaming_gates_tests
    test_b7_streaming_gates.cpp
)

target_link_libraries(fuse_b7_streaming_gates_tests PRIVATE fuse_world_partition fuse_ecs fuse_core)
add_test(NAME fuse_b7_streaming_gates COMMAND fuse_b7_streaming_gates_tests)
set_tests_properties(fuse_b7_streaming_gates PROPERTIES RUN_SERIAL TRUE LABELS "perf;gate")
