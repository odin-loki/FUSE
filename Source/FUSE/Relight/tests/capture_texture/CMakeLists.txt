# FUSE Relight RL-1.4 tests: texture tracking + hash on upload. Added from
# Source/FUSE/Relight/capture/texture/CMakeLists.txt.
#
#   rl_capture_texture_unit        (relight;gate) CPU: canonical repack, first-upload hashing (DEFAULT /
#                                  MANAGED), obsolete hash, Update* inheritance, recompute-on-write,
#                                  render-target hashes, external image registry, options. Any
#                                  platform (Wine emulator in the MinGW tree).
#   rl_capture_texture_<app>       (relight;wine;capture) texture_formats / d3d8_texture_formats through
#                                  the Relight d3d9.dll with the recording tap under Wine + Xvfb +
#                                  Lavapipe; the recorded texture events (bytes from the app sidecar,
#                                  matched by the tap's SHA-256) are replayed into TextureTracker, and
#                                  every format's hash must equal remix_hash_ref.py on the sidecar
#                                  (inheritance, render-target and not-hashed cases included). Skips
#                                  (77) without Wine / Xvfb.

add_executable(fuse_relight_capture_texture_tests test_capture_texture.cpp)
target_link_libraries(fuse_relight_capture_texture_tests PRIVATE fuse_relight_capture_texture)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_capture_texture_tests)
else()
    set_target_properties(fuse_relight_capture_texture_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
add_test(NAME rl_capture_texture_unit COMMAND fuse_relight_capture_texture_tests
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(rl_capture_texture_unit PROPERTIES LABELS "gate;relight" TIMEOUT 120)

if(NOT (TARGET relight_d3d9 AND TARGET rl_apps AND CMAKE_CROSSCOMPILING))
    return()
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    return()
endif()

get_filename_component(_rl_ctex_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(_rl_ctex_runner "${_rl_ctex_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh")
if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_ctex_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_ctex_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
set(_rl_ctex_emulator "")
if(CMAKE_CROSSCOMPILING_EMULATOR)
    set(_rl_ctex_emulator --emulator ${CMAKE_CROSSCOMPILING_EMULATOR})
endif()

foreach(_app IN ITEMS texture_formats d3d8_texture_formats)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    add_test(NAME rl_capture_texture_${_app}
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/rl_capture_texture_run.py"
                     --exe "$<TARGET_FILE:rl_app_${_app}>" --app ${_app}
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_ctex_runner}" --prefix-root "${_rl_ctex_prefix}"
                     --tool "$<TARGET_FILE:fuse_relight_capture_texture_tests>" ${_rl_ctex_emulator}
                     --ref "${_rl_ctex_repo}/Tools/FUSE/Relight/remix_hash_ref.py"
                     --out "${CMAKE_BINARY_DIR}/relight/capture_texture/${_app}")
    set_tests_properties(rl_capture_texture_${_app} PROPERTIES
        LABELS "relight;wine;capture" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()
