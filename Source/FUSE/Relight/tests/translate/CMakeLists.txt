# FUSE Relight RL-1.5 tests: fixed-function translation, game lights, cameras. Added from
# Source/FUSE/Relight/scene/translate/CMakeLists.txt.
#
#   rl_translate_unit             (relight;gate) CPU: setLegacyMaterialState, setTextureStageState, texture
#                                 factor blending, transforms / clip plane, setFogState + fog discovery, and
#                                 TranslateTap on synthetic tap events.
#   rl_lights_unit                (relight;gate) CPU: D3DLIGHT9 -> light records, the stable light hash,
#                                 calculateIntensity and the light options, LightTranslator.
#   rl_camera_kat                 (relight;gate) CPU: camera decomposition known-answer tests within 1e-5
#                                 (FOV, aspect, near / far, handedness, reversed Z, infinite far, ortho,
#                                 shear), jitter detection, camera classification and camera cuts.
#   rl_translate_expect_selftest  (relight) the expectation files parse; the reference and the matcher catch
#                                 seeded mismatches.
#   rl_translate_capture_<app>    (relight;wine;capture) the RL-0.4 app through our d3d9.dll / d3d8.dll with
#                                 the recording tap; the stream replayed through TranslateTap
#                                 (rl_translate_replay) must match, draw by draw and frame by frame, an
#                                 independent Python reference computed from the app's sidecar (material,
#                                 fog, lights + light hashes, camera within 1e-5 of the app's own camera) and
#                                 the hand-written expectations/<scene>.json. Apps: ff_lit, ff_alpha, ff_fog
#                                 and their D3D8 twins. Skips (77) without Wine / Xvfb.
# In a MinGW tree the C++ executables are PE and run through CMAKE_CROSSCOMPILING_EMULATOR (Wine).

add_executable(fuse_relight_translate_tests test_translate.cpp)
target_link_libraries(fuse_relight_translate_tests PRIVATE fuse_relight_scene_translate)
add_executable(rl_translate_replay rl_translate_replay.cpp)
target_link_libraries(rl_translate_replay PRIVATE fuse_relight_scene_translate)
foreach(_t fuse_relight_translate_tests rl_translate_replay)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endforeach()

foreach(_group translate lights camera)
    if(_group STREQUAL "camera")
        set(_name rl_camera_kat)
    else()
        set(_name rl_${_group}_unit)
    endif()
    add_test(NAME ${_name} COMMAND fuse_relight_translate_tests ${_group})
    set_tests_properties(${_name} PROPERTIES LABELS "gate;relight" TIMEOUT 120)
endforeach()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    return()
endif()

get_filename_component(_rl_tr_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(_rl_tr_script "${CMAKE_CURRENT_SOURCE_DIR}/rl_translate_capture.py")
set(_rl_tr_expect "${CMAKE_CURRENT_SOURCE_DIR}/expectations")
add_test(NAME rl_translate_expect_selftest
         COMMAND "${Python3_EXECUTABLE}" "${_rl_tr_script}" selftest --expect "${_rl_tr_expect}"
                 --hash-ref "${_rl_tr_repo}/Tools/FUSE/Relight")
set_tests_properties(rl_translate_expect_selftest PROPERTIES LABELS "relight" TIMEOUT 120)

if(NOT (TARGET relight_d3d9 AND TARGET rl_apps AND CMAKE_CROSSCOMPILING))
    return()
endif()

if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_tr_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_tr_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
# The replay tool is a PE executable here: rl_translate_capture.py runs it through the emulator.
string(REPLACE ";" "|" _rl_tr_emulator "${CMAKE_CROSSCOMPILING_EMULATOR}")

foreach(_app ff_lit ff_alpha ff_fog d3d8_ff_lit d3d8_ff_alpha d3d8_ff_fog)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    add_test(NAME rl_translate_capture_${_app}
             COMMAND "${Python3_EXECUTABLE}" "${_rl_tr_script}" capture
                     --exe "$<TARGET_FILE:rl_app_${_app}>" --app ${_app}
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_tr_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh" --prefix-root "${_rl_tr_prefix}"
                     --replay "$<TARGET_FILE:rl_translate_replay>" --emulator "${_rl_tr_emulator}"
                     --expect "${_rl_tr_expect}" --hash-ref "${_rl_tr_repo}/Tools/FUSE/Relight"
                     --out "${CMAKE_BINARY_DIR}/relight/translate/${_app}")
    set_tests_properties(rl_translate_capture_${_app} PROPERTIES
        LABELS "relight;wine;capture" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()
