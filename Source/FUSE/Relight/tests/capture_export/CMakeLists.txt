# FUSE Relight RL-1.8 tests: the capture writers. Added from Source/FUSE/Relight/capture/export/CMakeLists.txt.
#
#   rl_capture_export_unit       (relight;gate) CPU: SHA-256 / SHA-1 / UUIDv5 vectors, JSON, DDS round trips for
#                                every texture_formats D3DFORMAT (BC passthrough, pitch rule, mips, bad files, RGBA8
#                                decode), GameCapturer on synthetic records (strip / fan -> list, reduction, instance
#                                naming, transform samples, LHS correction, lights, camera, skinning), the capture
#                                written to disk (USDA parsed by the minimal parser, re-ingest = identical key set,
#                                byte-identical twice) and seeded faults caught by the re-ingest.
#   rl_capture_diff_selftest     (relight) Tools/FUSE/Relight/capture_diff.py selftest: identical synthetic captures
#                                compare equal, seeded differences are each reported in their category.
#   rl_capture_export_expect_selftest (relight) rl_capture_export.py's key-set checker passes a synthetic live record
#                                + replay summary and catches seeded faults.
#   rl_capture_export_<app>      (relight;wine;capture) every RL-0.4 app through our d3d9.dll / d3d8.dll with
#                                relight.tap.mode = capture + record; rl_capture_export_replay replays the recorded
#                                stream (bytes restored from the sidecar) through CaptureTap + SceneModel +
#                                GameCapturer and writes the capture; the driver checks: re-ingest key set ==
#                                written key set == the key set implied by the live, in-process capture record;
#                                USDA parses and resolves; DDS files round-trip to their texture hashes; a second
#                                replay is identical (capture_diff); a seeded difference is reported. Skips (77)
#                                without Wine / Xvfb.
# In a MinGW tree the C++ executables are PE and run through CMAKE_CROSSCOMPILING_EMULATOR (Wine).

add_executable(fuse_relight_capture_export_tests test_capture_export.cpp)
target_link_libraries(fuse_relight_capture_export_tests PRIVATE fuse_relight_capture_export)
set(_rl_cexp_targets fuse_relight_capture_export_tests)
if(TARGET fuse_relight_tap_capture)
    add_executable(rl_capture_export_replay rl_capture_export_replay.cpp)
    target_link_libraries(rl_capture_export_replay PRIVATE fuse_relight_capture_export fuse_relight_tap_capture)
    list(APPEND _rl_cexp_targets rl_capture_export_replay)
endif()
foreach(_t IN LISTS _rl_cexp_targets)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endforeach()

add_test(NAME rl_capture_export_unit COMMAND fuse_relight_capture_export_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(rl_capture_export_unit PROPERTIES LABELS "gate;relight" TIMEOUT 120)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    return()
endif()

get_filename_component(_rl_cexp_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(_rl_cexp_diff "${_rl_cexp_repo}/Tools/FUSE/Relight/capture_diff.py")
set(_rl_cexp_script "${CMAKE_CURRENT_SOURCE_DIR}/rl_capture_export.py")
add_test(NAME rl_capture_diff_selftest COMMAND "${Python3_EXECUTABLE}" "${_rl_cexp_diff}" selftest)
set_tests_properties(rl_capture_diff_selftest PROPERTIES LABELS "relight" TIMEOUT 120)
add_test(NAME rl_capture_export_expect_selftest COMMAND "${Python3_EXECUTABLE}" "${_rl_cexp_script}" selftest)
set_tests_properties(rl_capture_export_expect_selftest PROPERTIES LABELS "relight" TIMEOUT 120)

if(NOT (TARGET relight_d3d9 AND TARGET rl_apps AND TARGET rl_capture_export_replay AND CMAKE_CROSSCOMPILING))
    return()
endif()

if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_cexp_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_cexp_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
# The replay tool is a PE executable here: the driver runs it through the emulator ('|'-joined).
string(REPLACE ";" "|" _rl_cexp_emulator "${CMAKE_CROSSCOMPILING_EMULATOR}")

# Every RL-0.4 app (the rl_capture_live list).
set(_rl_cexp_apps
    ff_triangle ff_textured ff_lit ff_alpha ff_fog ff_skinned ff_multi_instance sky_ui_hud stencil_shadow
    rt_target dynamic_buffers texture_formats shader_sm1 vs_sm2 vs_sm3
    d3d8_ff_triangle d3d8_ff_textured d3d8_ff_lit d3d8_ff_alpha d3d8_ff_fog d3d8_ff_skinned
    d3d8_ff_multi_instance d3d8_sky_ui_hud d3d8_stencil_shadow d3d8_rt_target d3d8_dynamic_buffers
    d3d8_texture_formats d3d8_shader_sm1)
foreach(_app IN LISTS _rl_cexp_apps)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    add_test(NAME rl_capture_export_${_app}
             COMMAND "${Python3_EXECUTABLE}" "${_rl_cexp_script}" capture
                     --exe "$<TARGET_FILE:rl_app_${_app}>" --app ${_app}
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_cexp_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh" --prefix-root "${_rl_cexp_prefix}"
                     --replay "$<TARGET_FILE:rl_capture_export_replay>" --diff "${_rl_cexp_diff}"
                     --emulator "${_rl_cexp_emulator}"
                     --out "${CMAKE_BINARY_DIR}/relight/capture_export/${_app}")
    set_tests_properties(rl_capture_export_${_app} PROPERTIES
        LABELS "relight;wine;capture" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()
