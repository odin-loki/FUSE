# FUSE Relight RL-3.4 tests: the runtime replacement engine. Added from ../CMakeLists.txt.
#
#   rl_replace_unit            (relight;gate) CPU (native; PE through the Wine emulator in the MinGW tree): stacking,
#                              index, mods on disk (Remix USD imported in memory, FUSE-native store) through the
#                              engine with synthetic draws (mesh / preserveOriginalDrawCall / removal, materials by
#                              texture hash, light replace / delete / attach, per-mod rtx.conf layers, the
#                              rtx.enableReplacement* switches), hot reload with every available notification
#                              backend (latency <= 1 frame), texture residency, and CaptureTap + the processor on a
#                              synthetic stream (the record's "replacement" / "replace_frame").
#   rl_replace_golden_<case>   (relight;golden;wine) Tests/relight/fixtures/replace/golden/<case>.json: fixture mods
#                              staged into an RL-0.4 app's run directory (rtx-remix/mods, fuse-relight/mods), the app
#                              run through the Relight d3d9.dll in capture mode, and the replaced scene in the capture
#                              record summarised and compared with <case>.expected.json (stacking order, hidden /
#                              preserved draws, materials, lights replaced / deleted / attached, per-mod rtx.conf
#                              layers, residency; hot reload: a mod edited at a known frame is applied within one
#                              frame of its notification). Skips (77) without Wine / Xvfb.

add_executable(fuse_relight_replace_tests test_replace.cpp)
target_link_libraries(fuse_relight_replace_tests PRIVATE fuse_relight_replace_live fuse_relight_mods_assets)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_replace_tests)
else()
    set_target_properties(fuse_relight_replace_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
get_filename_component(_rl_rp_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(_rl_rp_fixtures "${_rl_rp_repo}/Tests/relight/fixtures/replace")
add_test(NAME rl_replace_unit COMMAND fuse_relight_replace_tests --fixtures "${_rl_rp_fixtures}"
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(rl_replace_unit PROPERTIES LABELS "gate;relight" TIMEOUT 300)

if(NOT (TARGET relight_d3d9 AND TARGET rl_apps AND CMAKE_CROSSCOMPILING))
    return()
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    return()
endif()

if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_rp_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_rp_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
string(REPLACE ";" "|" _rl_rp_emulator "${CMAKE_CROSSCOMPILING_EMULATOR}")
file(GLOB _rl_rp_cases CONFIGURE_DEPENDS "${_rl_rp_fixtures}/golden/*.json")
list(FILTER _rl_rp_cases EXCLUDE REGEX "\\.expected\\.json$")
list(SORT _rl_rp_cases)
foreach(_case_file IN LISTS _rl_rp_cases)
    get_filename_component(_case "${_case_file}" NAME_WE)
    file(READ "${_case_file}" _case_json)
    string(JSON _app GET "${_case_json}" app)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    add_test(NAME rl_replace_golden_${_case}
             COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/rl_replace_golden.py"
                     --case "${_case_file}" --fixtures "${_rl_rp_fixtures}"
                     --exe "$<TARGET_FILE:rl_app_${_app}>"
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_rp_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh"
                     --prefix-root "${_rl_rp_prefix}"
                     --stager "$<TARGET_FILE:fuse_relight_replace_tests>" --emulator "${_rl_rp_emulator}"
                     --out "${CMAKE_BINARY_DIR}/relight/replace/${_case}")
    set_tests_properties(rl_replace_golden_${_case} PROPERTIES
        LABELS "relight;golden;wine" SKIP_RETURN_CODE 77 TIMEOUT 900)
endforeach()
