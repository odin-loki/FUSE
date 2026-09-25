# FUSE Relight RL-6.3 tests. Added from ../CMakeLists.txt.
#
#   rl_setup_unit          (gate;relight) profile JSON and rtx.conf round trips, the profile as the app-config
#                          option layer (dxvk.conf < profile < rtx.conf / environment / dynamic layers; `-hash`
#                          removal), discovery by exe name / hash / explicit path, the assistant's rules on
#                          synthetic capture records. Any platform (Wine emulator in the MinGW tree).
#   rl_setup_content       (relight) every Content/relight/profiles/*.json parses and is in canonical form.
#   rl_setup_wine_<app>    (relight;wine;capture) the app under Wine in capture mode without a profile; the
#                          assistant (fuse_relight_setup analyze) on that capture must propose the categories of
#                          expectations/<scene>.json (by the app's sidecar tags); runs with the generated profile
#                          (discovered by exe name in <exe dir>/relight/profiles), with accepted suggestions and
#                          a hash-only match, and with a profile for another exe must change (or keep) the capture
#                          record's classification as expected. Skips (77) without Wine / Xvfb.

add_executable(fuse_relight_setup_tests test_setup.cpp)
# fuse_relight_classify: the real rtx.* options the profiles feed (and the category table is checked against).
target_link_libraries(fuse_relight_setup_tests PRIVATE fuse_relight_setup fuse_relight_classify fuse_relight_hash)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_setup_tests)
else()
    set_target_properties(fuse_relight_setup_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
add_test(NAME rl_setup_unit COMMAND fuse_relight_setup_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(rl_setup_unit PROPERTIES LABELS "gate;relight" TIMEOUT 120)

get_filename_component(_rl_setup_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
file(GLOB _rl_setup_content "${_rl_setup_repo}/Content/relight/profiles/*.json")
if(_rl_setup_content)
    foreach(_f IN LISTS _rl_setup_content)
        get_filename_component(_n "${_f}" NAME_WE)
        add_test(NAME rl_setup_content_${_n} COMMAND fuse_relight_setup_tool check "${_f}")
        set_tests_properties(rl_setup_content_${_n} PROPERTIES LABELS "relight" TIMEOUT 60)
    endforeach()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND OR NOT (TARGET relight_d3d9 AND TARGET rl_apps AND CMAKE_CROSSCOMPILING))
    return()
endif()
if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_setup_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_setup_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
string(REPLACE ";" "|" _rl_setup_emulator "${CMAKE_CROSSCOMPILING_EMULATOR}")
foreach(_app sky_ui_hud d3d8_sky_ui_hud rt_target stencil_shadow)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    add_test(NAME rl_setup_wine_${_app}
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/rl_setup_wine.py"
                     --exe "$<TARGET_FILE:rl_app_${_app}>" --app ${_app}
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_setup_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh"
                     --prefix-root "${_rl_setup_prefix}"
                     --tool "$<TARGET_FILE:fuse_relight_setup_tool>" --emulator "${_rl_setup_emulator}"
                     --expect "${CMAKE_CURRENT_SOURCE_DIR}/expectations"
                     --content "${_rl_setup_repo}/Content/relight/profiles"
                     --out "${CMAKE_BINARY_DIR}/relight/setup/${_app}")
    set_tests_properties(rl_setup_wine_${_app} PROPERTIES
        LABELS "relight;wine;capture" SKIP_RETURN_CODE 77 TIMEOUT 900)
endforeach()
