# FUSE Relight RL-1.1 capture tap mode tests. Added from Source/FUSE/Relight/tap/capture/CMakeLists.txt.
#
#   rl_tap_capture_unit        (relight;gate) CPU: CaptureTap on a synthetic stream equals the packages
#                              fed separately (asset keys, geometry hashes, texture hashes, classification),
#                              with DXVK mappings and with buffer shadows; the capture record's lines;
#                              UpdateSurface extents; createTapForDevice for every mode. Any platform
#                              (Wine emulator in the MinGW tree).
#   rl_capture_live_<app>      (relight;wine;capture) every RL-0.4 app through the Relight d3d9.dll /
#                              d3d8.dll with relight.tap.mode = capture (+ captureRecord): the live,
#                              in-process capture record must equal, on every draw, the replay path run
#                              on the recorded event stream of the same run - geometry_replay (RL-1.3:
#                              every hash component, asset key, legacy keys, counts, AABB, skinning),
#                              the TextureTracker replay (RL-1.4: every texture's hashes per frame) and
#                              rl_classify_replay (RL-1.2: every classification field). The replay tests
#                              (rl_capture_geometry_*, rl_capture_texture_*, rl_classify_capture_*) stay
#                              the oracles' own checks. Skips (77) without Wine / Xvfb.

add_executable(fuse_relight_tap_capture_tests test_tap_capture.cpp)
target_link_libraries(fuse_relight_tap_capture_tests PRIVATE fuse_relight_tap_capture)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_tap_capture_tests)
else()
    set_target_properties(fuse_relight_tap_capture_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
add_test(NAME rl_tap_capture_unit COMMAND fuse_relight_tap_capture_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(rl_tap_capture_unit PROPERTIES LABELS "gate;relight" TIMEOUT 120)

if(NOT (TARGET relight_d3d9 AND TARGET rl_apps AND CMAKE_CROSSCOMPILING))
    return()
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    return()
endif()
foreach(_tool fuse_relight_geometry_replay fuse_relight_capture_texture_tests rl_classify_replay)
    if(NOT TARGET ${_tool})
        message(FATAL_ERROR "rl_capture_live needs the replay tool ${_tool}")
    endif()
endforeach()

get_filename_component(_rl_live_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_live_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_live_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
# The replay tools are PE executables here: the driver runs them through the emulator ('|'-joined).
string(REPLACE ";" "|" _rl_live_emulator "${CMAKE_CROSSCOMPILING_EMULATOR}")

set(_rl_live_apps
    ff_triangle ff_textured ff_lit ff_alpha ff_fog ff_skinned ff_multi_instance sky_ui_hud stencil_shadow
    rt_target dynamic_buffers texture_formats shader_sm1 vs_sm2 vs_sm3
    d3d8_ff_triangle d3d8_ff_textured d3d8_ff_lit d3d8_ff_alpha d3d8_ff_fog d3d8_ff_skinned
    d3d8_ff_multi_instance d3d8_sky_ui_hud d3d8_stencil_shadow d3d8_rt_target d3d8_dynamic_buffers
    d3d8_texture_formats d3d8_shader_sm1)
foreach(_app IN LISTS _rl_live_apps)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    add_test(NAME rl_capture_live_${_app}
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/rl_capture_live.py"
                     --exe "$<TARGET_FILE:rl_app_${_app}>" --app ${_app}
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_live_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh"
                     --prefix-root "${_rl_live_prefix}"
                     --geometry-replay "$<TARGET_FILE:fuse_relight_geometry_replay>"
                     --texture-replay "$<TARGET_FILE:fuse_relight_capture_texture_tests>"
                     --classify-replay "$<TARGET_FILE:rl_classify_replay>"
                     --emulator "${_rl_live_emulator}"
                     --out "${CMAKE_BINARY_DIR}/relight/capture_live/${_app}")
    set_tests_properties(rl_capture_live_${_app} PROPERTIES
        LABELS "relight;wine;capture" SKIP_RETURN_CODE 77 TIMEOUT 900)
endforeach()
