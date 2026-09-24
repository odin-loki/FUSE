# FUSE Relight RL-2.3: bridge host tests. Added from ../CMakeLists.txt.
#
#   rl_bridge_host_unit              (relight;bridge;gate) protocol rules, journal compaction (bounded;
#                                    replaying it reproduces a model device), in-process host loop
#                                    with shared-heap payloads
#   rl_bridge_host_crash_<scenario>  (relight;bridge;gate) test-local client + this executable as the
#                                    host process on a model backend: none, kill (mid-run, between
#                                    frames), kill-async (random moment), crash, hang, exit, nohost,
#                                    mismatch. Every frame is verified; the fallback engages once.
#   rl_bridge_host_d3d9_<scenario>   (relight;bridge;wine;gate) MinGW tree: the real
#                                    fuse_relight_host.exe on vendored DXVK under Wine + Xvfb +
#                                    Lavapipe, driven by a test-local client that renders and reads
#                                    back pixels; the host is killed / crashes / hangs mid-run and the
#                                    client falls back to in-process DXVK passthrough and completes.
#                                    In a 32-bit tree the client is x86 and the host is the x64
#                                    tree's (FUSE_RELIGHT_BRIDGE_HOST64); the x86 passthrough needs a
#                                    32-bit Vulkan driver, else the tests skip (77).

add_executable(fuse_relight_bridge_host_tests test_bridge_host.cpp)
target_link_libraries(fuse_relight_bridge_host_tests PRIVATE fuse_relight_bridge_host_core)

set(_rl_host_labels "relight;bridge;gate")
if(CMAKE_CROSSCOMPILING AND WIN32)
    list(APPEND _rl_host_labels wine)
endif()
add_test(NAME rl_bridge_host_unit COMMAND fuse_relight_bridge_host_tests unit)
set(_rl_host_tests rl_bridge_host_unit)
foreach(_s none kill kill-async crash hang exit nohost mismatch)
    add_test(NAME rl_bridge_host_crash_${_s} COMMAND fuse_relight_bridge_host_tests crash ${_s})
    list(APPEND _rl_host_tests rl_bridge_host_crash_${_s})
endforeach()
set_tests_properties(${_rl_host_tests} PROPERTIES LABELS "${_rl_host_labels}" TIMEOUT 300 SKIP_RETURN_CODE 77)

if(WIN32 AND CMAKE_CROSSCOMPILING AND TARGET relight_d3d9)
    add_executable(fuse_relight_bridge_host_d3d9_client test_host_d3d9_client.cpp)
    target_link_libraries(fuse_relight_bridge_host_d3d9_client PRIVATE fuse_relight_bridge_d3d9_executor)
    # Next to this architecture's vendored d3d9.dll (the passthrough), found relative to the client.
    set_target_properties(fuse_relight_bridge_host_d3d9_client PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FUSE_RELIGHT_BRIDGE_BIN_DIR}")
    add_dependencies(fuse_relight_bridge_host_d3d9_client relight_d3d9)
    set(_rl_host_args "")
    if(TARGET fuse_relight_host)
        add_dependencies(fuse_relight_bridge_host_d3d9_client fuse_relight_host)
    else()
        # 32-bit tree: the client is x86, the host is the x64 tree's (the real bridge topology).
        list(APPEND _rl_host_args --host "${FUSE_RELIGHT_BRIDGE_HOST64}"
                                  --host-d3d9 "${FUSE_RELIGHT_BRIDGE_HOST64_DIR}/d3d9.dll")
    endif()
    get_filename_component(_rl_host_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
    set(_rl_host_runner "${_rl_host_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh")
    if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
        set(_rl_host_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
    else()
        set(_rl_host_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
    endif()
    set(_rl_d3d9_tests "")
    foreach(_s none kill kill-async crash hang nohost)
        add_test(NAME rl_bridge_host_d3d9_${_s}
                 COMMAND bash "${_rl_host_runner}" --native-d3d "${_rl_host_prefix}"
                         "$<TARGET_FILE:fuse_relight_bridge_host_d3d9_client>" ${_s} ${_rl_host_args})
        list(APPEND _rl_d3d9_tests rl_bridge_host_d3d9_${_s})
    endforeach()
    set_tests_properties(${_rl_d3d9_tests} PROPERTIES
        LABELS "relight;bridge;wine;gate" TIMEOUT 600 SKIP_RETURN_CODE 77)
endif()
