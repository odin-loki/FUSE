# FUSE Relight RL-3.5 tests: Logic graphs. Added from ../CMakeLists.txt.
#
#   rl_logic_unit             (relight;gate) CPU (native; PE through the Wine emulator in the MinGW tree): the component
#                             registry (68 components, every flexible variant), per-component semantics rewritten from
#                             dxvk-remix tests/rtx/unit/test_transform_components.cpp and graph/test_component.h (Sense,
#                             Transform, Act, Constants), token parsing and flexible type inference, the OmniGraph
#                             parser (test_graph_usd_parser.cpp semantics: DAG order, connections, relationships, old
#                             names, version checks, cycles, variants), the runtime (owners, reload, enable / pause),
#                             option-layer actions on the RL-0.6 option system, and the fixture graphs
#                             (Tests/relight/fixtures/logic/graphs/<case>.json: N frames of outputs compared with
#                             <case>.expected.txt; --update rewrites them).
#   rl_logic_golden_<case>    (relight;golden;wine) Tests/relight/fixtures/logic/golden/<case>.json: a fixture mod with
#                             a logic graph staged into an RL-0.4 app's run directory, the app run through the Relight
#                             d3d9.dll in capture mode, and the capture record's per-frame replacement stats and "logic"
#                             members compared with <case>.expected.json (the graph toggles an option layer that turns
#                             a replacement off at a known frame). Skips (77) without Wine / Xvfb.

add_executable(fuse_relight_logic_tests test_logic.cpp test_logic_components.cpp test_logic_parser.cpp)
target_link_libraries(fuse_relight_logic_tests PRIVATE fuse_relight_logic fuse_relight_capture_export)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_logic_tests)
else()
    set_target_properties(fuse_relight_logic_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
get_filename_component(_rl_lg_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(_rl_lg_fixtures "${_rl_lg_repo}/Tests/relight/fixtures/logic")
add_test(NAME rl_logic_unit COMMAND fuse_relight_logic_tests --fixtures "${_rl_lg_fixtures}"
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(rl_logic_unit PROPERTIES LABELS "gate;relight" TIMEOUT 300)

if(NOT (TARGET relight_d3d9 AND TARGET rl_apps AND CMAKE_CROSSCOMPILING AND TARGET fuse_relight_logic_live))
    return()
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    return()
endif()

if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_lg_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_lg_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
file(GLOB _rl_lg_cases CONFIGURE_DEPENDS "${_rl_lg_fixtures}/golden/*.json")
list(FILTER _rl_lg_cases EXCLUDE REGEX "\\.expected\\.json$")
list(SORT _rl_lg_cases)
foreach(_case_file IN LISTS _rl_lg_cases)
    get_filename_component(_case "${_case_file}" NAME_WE)
    file(READ "${_case_file}" _case_json)
    string(JSON _app GET "${_case_json}" app)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    add_test(NAME rl_logic_golden_${_case}
             COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/rl_logic_golden.py"
                     --case "${_case_file}" --fixtures "${_rl_lg_fixtures}"
                     --exe "$<TARGET_FILE:rl_app_${_app}>"
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_lg_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh"
                     --prefix-root "${_rl_lg_prefix}"
                     --out "${CMAKE_BINARY_DIR}/relight/logic/${_case}")
    set_tests_properties(rl_logic_golden_${_case} PROPERTIES
        LABELS "relight;golden;wine" SKIP_RETURN_CODE 77 TIMEOUT 900)
endforeach()
