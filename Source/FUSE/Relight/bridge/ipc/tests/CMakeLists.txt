# FUSE Relight RL-2.1: bridge IPC core tests. Added from ../CMakeLists.txt.
#
#   rl_bridge_ipc_unit          (relight;bridge;gate) in-process codec, queues, ring, heap, sessions
#   rl_bridge_ipc_fuzz          (relight;bridge;gate) cross-process round trip of random commands
#                               (atomic queues) + a blocking-queue run
#   rl_bridge_ipc_heap          (relight;bridge;gate) cross-process shared heap with back-pressure
#   rl_bridge_ipc_peer_death    (relight;bridge;gate) peer killed while blocked: PeerDead < 1 s
# In the MinGW tree ctest runs the Windows binary through CMAKE_CROSSCOMPILING_EMULATOR (Wine) and
# the tests also carry the `wine` label; the binary spawns its peers as Windows processes.
#
# FUSE_RELIGHT_BRIDGE_FUZZ_COUNT sets the atomic-queue fuzz length (default 1,000,000 everywhere);
# the blocking-queue run uses a fifth of it.

set(FUSE_RELIGHT_BRIDGE_FUZZ_COUNT 1000000 CACHE STRING "Commands round-tripped by rl_bridge_ipc_fuzz")
math(EXPR _rl_bridge_blocking_count "${FUSE_RELIGHT_BRIDGE_FUZZ_COUNT} / 5")

add_executable(fuse_relight_bridge_ipc_tests test_bridge_ipc.cpp)
target_link_libraries(fuse_relight_bridge_ipc_tests PRIVATE fuse_relight_bridge_ipc)
add_dependencies(fuse_relight_bridge_ipc_tests fuse_relight_bridge_schema_gen)

set(_rl_bridge_labels "relight;bridge;gate")
if(CMAKE_CROSSCOMPILING AND WIN32)
    list(APPEND _rl_bridge_labels wine)
endif()

add_test(NAME rl_bridge_ipc_unit COMMAND fuse_relight_bridge_ipc_tests unit)
add_test(NAME rl_bridge_ipc_fuzz COMMAND fuse_relight_bridge_ipc_tests fuzz --count ${FUSE_RELIGHT_BRIDGE_FUZZ_COUNT})
add_test(NAME rl_bridge_ipc_fuzz_blocking
         COMMAND fuse_relight_bridge_ipc_tests fuzz --blocking --count ${_rl_bridge_blocking_count})
add_test(NAME rl_bridge_ipc_heap COMMAND fuse_relight_bridge_ipc_tests heap --count 2000)
add_test(NAME rl_bridge_ipc_peer_death COMMAND fuse_relight_bridge_ipc_tests peer-death)
set_tests_properties(rl_bridge_ipc_unit rl_bridge_ipc_fuzz rl_bridge_ipc_fuzz_blocking rl_bridge_ipc_heap
                     rl_bridge_ipc_peer_death
                     PROPERTIES LABELS "${_rl_bridge_labels}" TIMEOUT 600 SKIP_RETURN_CODE 77)
