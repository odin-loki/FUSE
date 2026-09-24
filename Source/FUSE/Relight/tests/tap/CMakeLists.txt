# FUSE Relight RL-1.1: tap seam + device import tests. Added from Source/FUSE/Relight/tap/CMakeLists.txt.
#
#   rl_tap_unit                   (relight;gate) CPU: SHA-256, mode parsing, option resolution,
#                                 null tap, recording tap JSON Lines. Any platform (Wine emulator
#                                 in the MinGW tree).
#   rl_tap_expect_selftest        (relight) the sidecar -> expected-stream mapping on a synthetic
#                                 sidecar/stream pair, and that seeded differences are caught.
#   rl_passthrough_golden_<app>   (relight;golden;wine) the RL-0.4 app through our d3d9.dll / d3d8.dll:
#                                 FUSE_RELIGHT=0 (the RL-0.2 behaviour: DXVK's own device, no tap)
#                                 vs device import + tap off, import + null tap and import +
#                                 recording tap: every dump (.rgba) bit-identical.
#   rl_tap_record_<app>           (relight;wine;capture) the recording tap's event stream equals the
#                                 stream derived from the app's sidecar (rl_tap_expect.py).
#   rl_tap_validation             (relight;wine;vulkan;gate) apps on the imported device with the
#                                 Khronos validation layer (host loader, see rl_tap_run.py): 0 messages.
# The Wine tests skip (77) without Wine / Xvfb.

add_executable(fuse_relight_tap_tests test_tap_unit.cpp)
target_link_libraries(fuse_relight_tap_tests PRIVATE fuse_relight_tap)
target_include_directories(fuse_relight_tap_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../tap/src)
add_test(NAME rl_tap_unit COMMAND fuse_relight_tap_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(rl_tap_unit PROPERTIES LABELS "gate;relight" TIMEOUT 120)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    return()
endif()

set(_rl_tap_tools "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME rl_tap_expect_selftest
         COMMAND "${Python3_EXECUTABLE}" "${_rl_tap_tools}/rl_tap_expect.py" selftest)
set_tests_properties(rl_tap_expect_selftest PROPERTIES LABELS "relight" TIMEOUT 60)

if(NOT (TARGET relight_d3d9 AND TARGET rl_apps AND CMAKE_CROSSCOMPILING))
    return()
endif()

get_filename_component(_rl_tap_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(_rl_tap_runner "${_rl_tap_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh")
if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_tap_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_tap_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
set(_rl_tap_out "${CMAKE_BINARY_DIR}/relight/tap")

# The RL-0.4 apps (relight_apps.cmake builds them as rl_app_<app>).
set(_rl_tap_apps
    ff_triangle ff_textured ff_lit ff_alpha ff_fog ff_skinned ff_multi_instance sky_ui_hud stencil_shadow
    rt_target dynamic_buffers texture_formats shader_sm1 vs_sm2 vs_sm3
    d3d8_ff_triangle d3d8_ff_textured d3d8_ff_lit d3d8_ff_alpha d3d8_ff_fog d3d8_ff_skinned
    d3d8_ff_multi_instance d3d8_sky_ui_hud d3d8_stencil_shadow d3d8_rt_target d3d8_dynamic_buffers
    d3d8_texture_formats d3d8_shader_sm1)

foreach(_app IN LISTS _rl_tap_apps)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    set(_common
        "${Python3_EXECUTABLE}" "${_rl_tap_tools}/rl_tap_run.py"
        --exe "$<TARGET_FILE:rl_app_${_app}>" --app ${_app}
        --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
        --runner "${_rl_tap_runner}" --prefix-root "${_rl_tap_prefix}")
    add_test(NAME rl_passthrough_golden_${_app}
             COMMAND ${_common} golden --out "${_rl_tap_out}/golden/${_app}")
    set_tests_properties(rl_passthrough_golden_${_app} PROPERTIES
        LABELS "relight;golden;wine" SKIP_RETURN_CODE 77 TIMEOUT 600)
    add_test(NAME rl_tap_record_${_app}
             COMMAND ${_common} record --out "${_rl_tap_out}/record/${_app}")
    set_tests_properties(rl_tap_record_${_app} PROPERTIES
        LABELS "relight;wine;capture" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()

set(_rl_tap_validation_apps ff_triangle ff_textured ff_lit rt_target dynamic_buffers shader_sm1 d3d8_ff_textured)
set(_rl_tap_validation_exes "")
foreach(_app IN LISTS _rl_tap_validation_apps)
    if(TARGET rl_app_${_app})
        list(APPEND _rl_tap_validation_exes --exe "$<TARGET_FILE:rl_app_${_app}>")
    endif()
endforeach()
add_test(NAME rl_tap_validation
         COMMAND "${Python3_EXECUTABLE}" "${_rl_tap_tools}/rl_tap_run.py"
                 ${_rl_tap_validation_exes} --app validation
                 --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                 --runner "${_rl_tap_runner}" --prefix-root "${_rl_tap_prefix}"
                 validation --out "${_rl_tap_out}/validation")
set_tests_properties(rl_tap_validation PROPERTIES
    LABELS "relight;wine;vulkan;gate" SKIP_RETURN_CODE 77 TIMEOUT 900)
