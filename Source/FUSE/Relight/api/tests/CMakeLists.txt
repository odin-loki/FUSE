# FUSE Relight RL-6.2 tests: the C APIs. Added from ../CMakeLists.txt.
#
#   rl_api_unit         (relight;gate) CPU (native; PE through the Wine emulator in the MinGW tree): the Remix API
#                       0.6.5 ABI gate (every remixapi_* struct size / alignment / offset and enum value against
#                       remixapi_layout_ref.inc, generated from upstream remix_c.h by tools/gen_remixapi_layout.py;
#                       compile time in C++ and run time in C), the pinned FUSE-native ABI, version negotiation and
#                       struct-size / sType checks, and round trips through both front ends into the RL-1.7 scene model,
#                       the RL-4.4 light set, the RL-4.3 materials, the option layer and the RL-3.4 replacement engine.
#   rl_api_remixapi_c   (relight;wine) the remixapi_c app (Tests/relight/apps/remixapi_c, C99) loads Relight's
#                       d3d9.dll under Wine + Xvfb + Lavapipe, creates its D3D9 device through dxvk_CreateD3D9, drives
#                       the Remix API for 8 frames and checks every frame record. Skips (77) without Wine / Xvfb.

add_executable(fuse_relight_api_tests test_api.cpp test_api_layout_c.c)
target_link_libraries(fuse_relight_api_tests PRIVATE fuse_relight_api fuse_relight_replace)
target_include_directories(fuse_relight_api_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_source_files_properties(test_api_layout_c.c PROPERTIES LANGUAGE C)
set_target_properties(fuse_relight_api_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
_rl_api_cxx23(fuse_relight_api_tests)
add_test(NAME rl_api_unit COMMAND fuse_relight_api_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(rl_api_unit PROPERTIES LABELS "gate;relight" TIMEOUT 300)

if(NOT (WIN32 AND TARGET relight_d3d9))
    return()
endif()
get_filename_component(_rl_api_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
add_executable(rl_app_remixapi_c "${_rl_api_repo}/Tests/relight/apps/remixapi_c/remixapi_c.c")
target_include_directories(rl_app_remixapi_c PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(rl_app_remixapi_c PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(rl_app_remixapi_c PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
set_target_properties(rl_app_remixapi_c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON OUTPUT_NAME remixapi_c
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/relight/apps")
# d3d9.dll is loaded at run time; only user32 / gdi32 are linked.
target_link_libraries(rl_app_remixapi_c PRIVATE user32 gdi32)
if(MINGW)
    target_link_options(rl_app_remixapi_c PRIVATE -static -static-libgcc)
endif()
add_dependencies(rl_app_remixapi_c relight_d3d9)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT (Python3_Interpreter_FOUND AND CMAKE_CROSSCOMPILING))
    return()
endif()
if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_api_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_api_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
add_test(NAME rl_api_remixapi_c
         COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/rl_api_app_run.py"
                 --exe "$<TARGET_FILE:rl_app_remixapi_c>" --d3d9 "$<TARGET_FILE:relight_d3d9>"
                 --runner "${_rl_api_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh" --prefix-root "${_rl_api_prefix}"
                 --out "${CMAKE_BINARY_DIR}/relight/api/remixapi_c")
set_tests_properties(rl_api_remixapi_c PROPERTIES LABELS "relight;wine" SKIP_RETURN_CODE 77 TIMEOUT 600)
