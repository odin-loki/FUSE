# FUSE Relight RL-2.3: bridge host + launcher (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.6, Wave R2).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(bridge/host)
# Ported from dxvk-remix bridge/src/{server,launcher} and the client's server supervision (MIT); see
# the per-file notices for what changed.
#
# Targets
#   fuse_relight_bridge_host_core   (all platforms) protocol rules, command journal, client link with
#                                   the crash/hang fallback, host command loop. The RL-2.2 x86 client
#                                   links it (with fuse_relight_bridge_d3d9_executor for the fallback).
#   fuse_relight_bridge_d3d9_executor  (Windows) executes the command stream on a d3d9.dll; the host
#                                   uses it on x64 DXVK + Relight, the client fallback on the
#                                   passthrough DXVK (FUSE_RELIGHT=0)
#   fuse_relight_host               (Windows x64) fuse_relight_host.exe, next to the vendored d3d9.dll
#   ../launcher                     fuse_relight_launcher.exe + its self-test helpers
# Tests: tests/ and ../launcher/tests/.

add_library(fuse_relight_bridge_host_core STATIC
    src/protocol.cpp
    src/journal.cpp
    src/link.cpp
    src/host_loop.cpp
)
target_include_directories(fuse_relight_bridge_host_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fuse_relight_bridge_host_core PUBLIC fuse_relight_bridge_ipc)
add_dependencies(fuse_relight_bridge_host_core fuse_relight_bridge_schema_gen)

if(DEFINED FUSE_RELIGHT_BIN_DIR)
    set(FUSE_RELIGHT_BRIDGE_BIN_DIR "${FUSE_RELIGHT_BIN_DIR}")
else()
    set(FUSE_RELIGHT_BRIDGE_BIN_DIR "${CMAKE_BINARY_DIR}/relight/bin")
endif()

if(WIN32)
    add_library(fuse_relight_bridge_d3d9_executor STATIC src/d3d9_executor.cpp)
    target_link_libraries(fuse_relight_bridge_d3d9_executor PUBLIC fuse_relight_bridge_host_core)
endif()

# The host is x64 only (plan §2.6: x86 games talk to an x64 host). A 32-bit tree builds the client
# side (executor for the fallback) and points its tests at the x64 tree's host.
set(FUSE_RELIGHT_BRIDGE_HOST64 "${CMAKE_BINARY_DIR}/../relight-mingw/relight/bin/fuse_relight_host.exe"
    CACHE FILEPATH "x64 fuse_relight_host.exe used by the tests of a 32-bit tree")
get_filename_component(FUSE_RELIGHT_BRIDGE_HOST64_DIR "${FUSE_RELIGHT_BRIDGE_HOST64}" DIRECTORY)

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(fuse_relight_host src/host_main.cpp)
    target_link_libraries(fuse_relight_host PRIVATE fuse_relight_bridge_d3d9_executor)
    # Next to the vendored d3d9.dll, which the host loads by default.
    set_target_properties(fuse_relight_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FUSE_RELIGHT_BRIDGE_BIN_DIR}")
    if(TARGET relight_d3d9)
        add_dependencies(fuse_relight_host relight_d3d9)
    endif()
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../launcher ${CMAKE_CURRENT_BINARY_DIR}/launcher)
add_subdirectory(tests)
