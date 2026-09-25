# FUSE Relight RL-1.3 tests: geometry capture. Added from Source/FUSE/Relight/capture/geometry/CMakeLists.txt.
#
#   rl_capture_geometry_unit      (relight;gate;capture) CPU unit tests on synthetic buffers: index
#                                 rebasing + memoization, GeometryCapture vs hash::computeDrawGeometryHashes
#                                 on random draws (inline and on JobScheduler workers, with and without
#                                 CPU mappings), UP draws, texcoordIndex selection, skinning, bounding box.
#   rl_capture_geometry_selftest  (relight;capture) rl_capture_geometry.py on a synthetic sidecar + tap
#                                 stream pair (every draw kind), and seeded differences must be caught.
#   rl_capture_geometry_<app>     (relight;wine;capture) the RL-0.4 app under Wine through the Relight
#                                 d3d9.dll / d3d8.dll with the recording tap; its event stream replayed
#                                 into GeometryCapture (geometry_replay) must match
#                                 Tools/FUSE/Relight/remix_hash_ref.py on every draw, and on the sidecar's
#                                 recorded frames (ground truth). Skips (77) without Wine / Xvfb.
# In a MinGW tree the C++ executables are PE and run through CMAKE_CROSSCOMPILING_EMULATOR (Wine).

add_executable(fuse_relight_capture_geometry_tests test_capture_geometry.cpp)
target_link_libraries(fuse_relight_capture_geometry_tests PRIVATE fuse_relight_capture_geometry)
add_executable(fuse_relight_geometry_replay geometry_replay.cpp)
target_link_libraries(fuse_relight_geometry_replay PRIVATE fuse_relight_capture_geometry)
foreach(_t fuse_relight_capture_geometry_tests fuse_relight_geometry_replay)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endforeach()

add_test(NAME rl_capture_geometry_unit COMMAND fuse_relight_capture_geometry_tests)
set_tests_properties(rl_capture_geometry_unit PROPERTIES LABELS "gate;relight;capture" TIMEOUT 300)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    return()
endif()

get_filename_component(_rl_geo_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(_rl_geo_script "${CMAKE_CURRENT_SOURCE_DIR}/rl_capture_geometry.py")
set(_rl_geo_common
    "${Python3_EXECUTABLE}" "${_rl_geo_script}"
    --ref "${_rl_geo_repo}/Tools/FUSE/Relight/remix_hash_ref.py"
    --replay "$<TARGET_FILE:fuse_relight_geometry_replay>")
if(CMAKE_CROSSCOMPILING AND CMAKE_CROSSCOMPILING_EMULATOR)
    foreach(_arg IN LISTS CMAKE_CROSSCOMPILING_EMULATOR)
        list(APPEND _rl_geo_common "--emulator=${_arg}")
    endforeach()
endif()
set(_rl_geo_out "${CMAKE_BINARY_DIR}/relight/capture_geometry")

add_test(NAME rl_capture_geometry_selftest COMMAND ${_rl_geo_common} selftest --out "${_rl_geo_out}/selftest")
set_tests_properties(rl_capture_geometry_selftest PROPERTIES LABELS "relight;capture" TIMEOUT 600 SKIP_RETURN_CODE 77)

if(NOT (TARGET relight_d3d9 AND TARGET rl_apps AND CMAKE_CROSSCOMPILING))
    return()
endif()

set(_rl_geo_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_geo_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
endif()
set(_rl_geo_apps
    ff_triangle ff_textured ff_lit ff_alpha ff_fog ff_skinned ff_multi_instance sky_ui_hud stencil_shadow
    rt_target dynamic_buffers texture_formats shader_sm1 vs_sm2 vs_sm3
    d3d8_ff_triangle d3d8_ff_textured d3d8_ff_lit d3d8_ff_alpha d3d8_ff_fog d3d8_ff_skinned
    d3d8_ff_multi_instance d3d8_sky_ui_hud d3d8_stencil_shadow d3d8_rt_target d3d8_dynamic_buffers
    d3d8_texture_formats d3d8_shader_sm1)
foreach(_app IN LISTS _rl_geo_apps)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    add_test(NAME rl_capture_geometry_${_app}
             COMMAND ${_rl_geo_common} run
                     --exe "$<TARGET_FILE:rl_app_${_app}>" --app ${_app}
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_geo_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh"
                     --prefix-root "${_rl_geo_prefix}"
                     --tap-tools "${CMAKE_CURRENT_SOURCE_DIR}/../tap"
                     --out "${_rl_geo_out}/${_app}")
    set_tests_properties(rl_capture_geometry_${_app} PROPERTIES
        LABELS "relight;wine;capture" SKIP_RETURN_CODE 77 TIMEOUT 900)
endforeach()
