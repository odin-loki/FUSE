# FUSE Relight RL-2.1: bridge IPC core (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.6, Wave R2).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(bridge/ipc)
# Ported from dxvk-remix bridge/src/util (MIT); see the per-file notices for what changed.
#
# Targets (all C++17, no FUSE engine dependency, so the RL-2.2 x86 client can link them)
#   fuse_relight_bridge_ipc_core    shared memory, semaphores, peer supervision (Win32 + POSIX
#                                   backends), atomic and blocking queues, data ring, shared heap,
#                                   channel, wire format
#   fuse_relight_bridge_schema      generated from ../schema/commands.table: command ids, structs,
#                                   encoders, decoders, dispatch, kSchemaHash (../schema)
#   fuse_relight_bridge_ipc         session: control block, versioned handshake, typed send/receive
# Tests: tests/ (rl_bridge_ipc_unit, rl_bridge_ipc_fuzz, rl_bridge_ipc_heap, rl_bridge_ipc_peer_death;
# the MinGW tree runs the same executable under Wine).

find_package(Threads REQUIRED)

add_library(fuse_relight_bridge_ipc_core STATIC
    src/channel.cpp
    src/platform_posix.cpp
    src/platform_win32.cpp
    src/shared_heap.cpp
)
target_include_directories(fuse_relight_bridge_ipc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fuse_relight_bridge_ipc_core PUBLIC Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(fuse_relight_bridge_ipc_core PUBLIC rt)  # shm_open on glibc < 2.34
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../schema ${CMAKE_CURRENT_BINARY_DIR}/schema)

add_library(fuse_relight_bridge_ipc STATIC src/session.cpp)
target_link_libraries(fuse_relight_bridge_ipc PUBLIC fuse_relight_bridge_schema fuse_relight_bridge_ipc_core)
add_dependencies(fuse_relight_bridge_ipc fuse_relight_bridge_schema_gen)

add_subdirectory(tests)
