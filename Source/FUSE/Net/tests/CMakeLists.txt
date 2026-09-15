add_executable(fuse_net_tests
    test_net_transport.cpp
    test_net_serializer.cpp
    test_net_rollback.cpp
    test_net_rollback_buffer.cpp
    test_net_checksum.cpp
    test_net_input_history.cpp
    test_net_reconcile.cpp
    test_net_snapshot_delta.cpp
    test_net_state_sync.cpp
    test_main.cpp
)

target_link_libraries(fuse_net_tests PRIVATE fuse_net fuse_ecs fuse_core)
add_test(NAME fuse_net_b74 COMMAND fuse_net_tests)
