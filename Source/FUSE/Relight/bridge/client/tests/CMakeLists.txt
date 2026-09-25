# FUSE Relight RL-2.2: bridge client tests. Added from ../CMakeLists.txt.
#
#   rl_bridge_client_unit     (relight;bridge;gate) format layout, DirectInput translation (every
#                             tree); on Windows/Wine also wire forms, state blocks, window
#                             subclassing and the input hooks
#   rl_bridge_x64_<app>       (relight;bridge;wine) the RL-0.4 app (x64) through the bridge client
#                             d3d9.dll / d3d8.dll + the test-local stub host (tests/stub_host.cpp,
#                             replaying on the vendored DXVK d3d9.dll) vs the same app on DXVK
#                             directly: every dump (.rgba) and the sidecar must be identical, and
#                             the stub host must have decoded and dispatched every command
#   rl_bridge_x86_<app>       (relight;bridge;wine32) the i686 build of the app and client (from the
#                             i686 tree, FUSE_RELIGHT_BRIDGE_X86_DIR) against the x64 stub host and
#                             DXVK: dumps and sidecar equal to the x64 app on DXVK directly, and the
#                             command stream equal to the x64 bridged run. Skips (77) without the
#                             i686 tree or wine32.
#   rl_bridge_host_{x64,x86}_<app>   the same against RL-2.3's fuse_relight_host.exe (no fallback allowed)
#   rl_bridge_fallback_x64_<app>     RL-2.3 host crashing at the 2nd Present: the client link falls
#                             back to in-process DXVK and the dumps are still identical
# The Wine tests skip (77) without Wine / Xvfb.

add_executable(fuse_relight_bridge_client_tests test_bridge_client.cpp)
target_link_libraries(fuse_relight_bridge_client_tests PRIVATE fuse_relight_bridge_client_core fuse_relight_bridge_schema)
add_dependencies(fuse_relight_bridge_client_tests fuse_relight_bridge_schema_gen)
set_target_properties(fuse_relight_bridge_client_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(TARGET fuse_relight_bridge_client_obj)
    target_link_libraries(fuse_relight_bridge_client_tests PRIVATE fuse_relight_bridge_client_obj dinput8 dxguid)
    target_link_options(fuse_relight_bridge_client_tests PRIVATE -static -static-libgcc -static-libstdc++)
endif()
set(_rl_bc_labels "relight;bridge;gate")
if(CMAKE_CROSSCOMPILING AND WIN32)
    list(APPEND _rl_bc_labels wine)
endif()
add_test(NAME rl_bridge_client_unit COMMAND fuse_relight_bridge_client_tests)
set_tests_properties(rl_bridge_client_unit PROPERTIES LABELS "${_rl_bc_labels}" TIMEOUT 300 SKIP_RETURN_CODE 77)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT (Python3_Interpreter_FOUND AND CMAKE_CROSSCOMPILING AND TARGET fuse_relight_bridge_stub_host AND TARGET rl_apps
        AND TARGET relight_d3d9 AND CMAKE_SIZEOF_VOID_P EQUAL 8))
    return()
endif()

get_filename_component(_rl_bc_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
set(_rl_bc_runner "${_rl_bc_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh")
if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_bc_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_bc_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
set(FUSE_RELIGHT_BRIDGE_X86_DIR "${CMAKE_BINARY_DIR}/../relight-mingw32" CACHE PATH
    "Build tree of the i686 (x86) MinGW configuration holding the x86 apps and bridge client (rl_bridge_x86_*)")
set(_rl_bc_out "${CMAKE_BINARY_DIR}/relight/bridge/tests")

set(_rl_bc_apps
    ff_triangle ff_textured ff_lit ff_alpha ff_fog ff_skinned ff_multi_instance sky_ui_hud stencil_shadow
    rt_target dynamic_buffers texture_formats shader_sm1 vs_sm2 vs_sm3
    d3d8_ff_triangle d3d8_ff_textured d3d8_ff_lit d3d8_ff_alpha d3d8_ff_fog d3d8_ff_skinned
    d3d8_ff_multi_instance d3d8_sky_ui_hud d3d8_stencil_shadow d3d8_rt_target d3d8_dynamic_buffers
    d3d8_texture_formats d3d8_shader_sm1)

foreach(_app IN LISTS _rl_bc_apps)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    set(_common
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/rl_bridge_run.py"
        --app ${_app}
        --exe "$<TARGET_FILE:rl_app_${_app}>"
        --dxvk-d3d9 "$<TARGET_FILE:relight_d3d9>" --dxvk-d3d8 "$<TARGET_FILE:relight_d3d8>"
        --client-d3d9 "$<TARGET_FILE:relight_bridge_d3d9>" --client-d3d8 "$<TARGET_FILE:relight_bridge_d3d8>"
        --runner "${_rl_bc_runner}" --prefix-root "${_rl_bc_prefix}")
    set(_stub ${_common} --host "$<TARGET_FILE:fuse_relight_bridge_stub_host>")
    add_test(NAME rl_bridge_x64_${_app} COMMAND ${_stub} --arch x64 --out "${_rl_bc_out}/x64/${_app}")
    set_tests_properties(rl_bridge_x64_${_app} PROPERTIES
        LABELS "relight;bridge;wine" SKIP_RETURN_CODE 77 TIMEOUT 900)
    add_test(NAME rl_bridge_x86_${_app} COMMAND ${_stub} --arch x86 --x86-dir "${FUSE_RELIGHT_BRIDGE_X86_DIR}"
             --out "${_rl_bc_out}/x86/${_app}")
    set_tests_properties(rl_bridge_x86_${_app} PROPERTIES
        LABELS "relight;bridge;wine;wine32" SKIP_RETURN_CODE 77 TIMEOUT 900)
    if(TARGET fuse_relight_host)
        # The same runs against RL-2.3's host (fuse_relight_host.exe, --relight 0), and a host crash
        # at the 2nd Present: the client's link falls back to in-process DXVK (x64 only: the x86
        # fallback needs an i686 build of vendored DXVK, see the report of RL-2.2).
        set(_real ${_common} --host "$<TARGET_FILE:fuse_relight_host>")
        add_test(NAME rl_bridge_host_x64_${_app} COMMAND ${_real} --host-kind real --arch x64
                 --out "${_rl_bc_out}/host_x64/${_app}")
        add_test(NAME rl_bridge_host_x86_${_app} COMMAND ${_real} --host-kind real --arch x86
                 --x86-dir "${FUSE_RELIGHT_BRIDGE_X86_DIR}" --out "${_rl_bc_out}/host_x86/${_app}")
        add_test(NAME rl_bridge_fallback_x64_${_app} COMMAND ${_real} --host-kind real --arch x64 --fault crash@2
                 --out "${_rl_bc_out}/fallback_x64/${_app}")
        set_tests_properties(rl_bridge_host_x64_${_app} rl_bridge_fallback_x64_${_app} PROPERTIES
            LABELS "relight;bridge;wine" SKIP_RETURN_CODE 77 TIMEOUT 900)
        set_tests_properties(rl_bridge_host_x86_${_app} PROPERTIES
            LABELS "relight;bridge;wine;wine32" SKIP_RETURN_CODE 77 TIMEOUT 900)
    endif()
endforeach()
