# FUSE Relight RL-1.2: draw classifier tests. Added from Source/FUSE/Relight/scene/classify/CMakeLists.txt.
#
#   rl_classify_unit               (relight;gate) CPU: every makeDrawCallType / isRenderingUI /
#                                  processTextures / sky / terrain / raytraced-render-target rule on
#                                  synthetic D3DStateModels, the texture-list categories, option-driven
#                                  behaviour and the tap -> model tracker. Any platform (Wine emulator
#                                  in the MinGW tree).
#   rl_classify_expect_selftest    (relight) the expectation files parse; the tag matcher catches
#                                  seeded mismatches.
#   rl_classify_capture_<app>      (relight;wine;capture) the app through our d3d9.dll / d3d8.dll with
#                                  the recording tap; the stream replayed through ClassifyTap
#                                  (rl_classify_replay) under each configuration of
#                                  expectations/<scene>.json; every recorded draw's classification
#                                  matches the expectation for its sidecar tag. Apps: sky_ui_hud,
#                                  stencil_shadow, rt_target and their D3D8 twins. (The plan also lists
#                                  a shadow_mask app; RL-0.4 does not build one, so there is no
#                                  capture for that rule: rl_classify_unit covers it.)
# The Wine tests skip (77) without Wine / Xvfb.

add_executable(fuse_relight_classify_tests test_classify_rules.cpp)
target_link_libraries(fuse_relight_classify_tests PRIVATE fuse_relight_classify fuse_relight_hash)
add_executable(rl_classify_replay rl_classify_replay.cpp)
target_link_libraries(rl_classify_replay PRIVATE fuse_relight_classify)
foreach(_t fuse_relight_classify_tests rl_classify_replay)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endforeach()
add_test(NAME rl_classify_unit COMMAND fuse_relight_classify_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(rl_classify_unit PROPERTIES LABELS "gate;relight" TIMEOUT 120)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    return()
endif()

set(_rl_cls_expect "${CMAKE_CURRENT_SOURCE_DIR}/expectations")
add_test(NAME rl_classify_expect_selftest
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/rl_classify_capture.py" selftest
                 --expect "${_rl_cls_expect}")
set_tests_properties(rl_classify_expect_selftest PROPERTIES LABELS "relight" TIMEOUT 60)

if(NOT (TARGET relight_d3d9 AND TARGET rl_apps AND CMAKE_CROSSCOMPILING))
    return()
endif()

get_filename_component(_rl_cls_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_cls_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_cls_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
# The replay tool is a PE executable here: rl_classify_capture.py runs it through the emulator.
string(REPLACE ";" "|" _rl_cls_emulator "${CMAKE_CROSSCOMPILING_EMULATOR}")

foreach(_app sky_ui_hud stencil_shadow rt_target d3d8_sky_ui_hud d3d8_stencil_shadow d3d8_rt_target)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    add_test(NAME rl_classify_capture_${_app}
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/rl_classify_capture.py"
                     --exe "$<TARGET_FILE:rl_app_${_app}>" --app ${_app}
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_cls_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh" --prefix-root "${_rl_cls_prefix}"
                     --replay "$<TARGET_FILE:rl_classify_replay>" --emulator "${_rl_cls_emulator}"
                     --expect "${_rl_cls_expect}" --hash-ref "${_rl_cls_repo}/Tools/FUSE/Relight"
                     --out "${CMAKE_BINARY_DIR}/relight/classify/${_app}")
    set_tests_properties(rl_classify_capture_${_app} PROPERTIES
        LABELS "relight;wine;capture" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()
