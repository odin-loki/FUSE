add_executable(fuse_net_tests
    test_net_transport.cpp
    test_net_serializer.cpp
    test_net_rollback.cpp
    test_net_rollback_window.cpp
    test_net_rollback_buffer.cpp
    test_net_checksum.cpp
    test_net_input_history.cpp
    test_net_reconcile.cpp
    test_net_snapshot_delta.cpp
    test_net_state_sync.cpp
    test_net_interest_management.cpp
    test_main.cpp
)

target_link_libraries(fuse_net_tests PRIVATE fuse_net fuse_ecs fuse_core)
add_test(NAME fuse_net_b74 COMMAND fuse_net_tests)

add_executable(fuse_p7_net_smoke
    test_p7_net_smoke.cpp
)

target_link_libraries(fuse_p7_net_smoke PRIVATE fuse_net fuse_core)
add_test(NAME fuse_p7_net_smoke COMMAND fuse_p7_net_smoke)

# B7.4 / B7.10 networking gate proofs (ENet two-process, rollback, interpolation, deltas, relevance).
add_executable(fuse_b7_net_gates
    test_b7_net_gates.cpp
)

target_link_libraries(fuse_b7_net_gates PRIVATE fuse_net fuse_ecs fuse_core)
add_test(NAME fuse_b7_net_gates COMMAND fuse_b7_net_gates)
set_tests_properties(fuse_b7_net_gates PROPERTIES TIMEOUT 180)

# Opt-in sanitizers follow the repo-wide switches (fuse-asan preset / FUSE_CORE_ENABLE_UBSAN).
foreach(_fuse_net_target fuse_net fuse_net_tests fuse_p7_net_smoke fuse_b7_net_gates)
    if(COMMAND fuse_apply_asan)
        fuse_apply_asan(${_fuse_net_target})
    endif()
    if(FUSE_CORE_ENABLE_UBSAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${_fuse_net_target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
        target_link_options(${_fuse_net_target} PRIVATE -fsanitize=undefined)
    endif()
endforeach()
