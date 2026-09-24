# FUSE Relight RL-1.7 tests: instance tracking and the scene model. Added from
# Source/FUSE/Relight/scene/instances/CMakeLists.txt.
#
#   rl_instances_unit              (relight;gate) CPU: the options (and RL-1.5 / RL-1.3 reading them), the spatial
#                                  map, transform history, the BLAS cache, the draw-call tracker (identity, same-BLAS
#                                  nearest within rtx.uniqueObjectDistance), SceneModel on the ff_multi_instance scene
#                                  (ids stable over 60 frames, teleport -> new id, previous transforms = the analytic
#                                  previous frame), anti-culling, foliage / SSS bookkeeping, the point instancer.
#   rl_instances_expect_selftest   (relight) rl_instances_capture.py's checker on a synthetic replay of the
#                                  analytic scene passes, and seeded id / teleport / motion faults are caught.
#   rl_instances_capture_<app>     (relight;wine;capture) the RL-0.4 app through our d3d9.dll / d3d8.dll with
#                                  relight.tap.mode = capture + record; the run replayed through TranslateTap +
#                                  SceneModel (rl_instances_replay) must give every logical instance one id over the
#                                  60 frames, a new id to the teleported one at frame 30, objectToWorld equal to the
#                                  app's recorded WORLD, previous transforms equal to the previous frame's, and
#                                  per-instance motion equal to the scene's analytic motion. Apps: ff_multi_instance
#                                  and its D3D8 twin. Skips (77) without Wine / Xvfb.
# In a MinGW tree the C++ executables are PE and run through CMAKE_CROSSCOMPILING_EMULATOR (Wine).

add_executable(fuse_relight_instances_tests test_instances.cpp)
target_link_libraries(fuse_relight_instances_tests PRIVATE fuse_relight_scene_instances)
add_executable(rl_instances_replay rl_instances_replay.cpp)
target_link_libraries(rl_instances_replay PRIVATE fuse_relight_scene_instances)
foreach(_t fuse_relight_instances_tests rl_instances_replay)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endforeach()

add_test(NAME rl_instances_unit COMMAND fuse_relight_instances_tests)
set_tests_properties(rl_instances_unit PROPERTIES LABELS "gate;relight" TIMEOUT 120)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    return()
endif()

set(_rl_inst_script "${CMAKE_CURRENT_SOURCE_DIR}/rl_instances_capture.py")
add_test(NAME rl_instances_expect_selftest COMMAND "${Python3_EXECUTABLE}" "${_rl_inst_script}" selftest)
set_tests_properties(rl_instances_expect_selftest PROPERTIES LABELS "relight" TIMEOUT 120)

if(NOT (TARGET relight_d3d9 AND TARGET rl_apps AND CMAKE_CROSSCOMPILING))
    return()
endif()

get_filename_component(_rl_inst_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_inst_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_inst_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
# The replay tool is a PE executable here: the driver runs it through the emulator ('|'-joined).
string(REPLACE ";" "|" _rl_inst_emulator "${CMAKE_CROSSCOMPILING_EMULATOR}")

foreach(_app ff_multi_instance d3d8_ff_multi_instance)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    add_test(NAME rl_instances_capture_${_app}
             COMMAND "${Python3_EXECUTABLE}" "${_rl_inst_script}" capture
                     --exe "$<TARGET_FILE:rl_app_${_app}>" --app ${_app}
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_inst_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh" --prefix-root "${_rl_inst_prefix}"
                     --replay "$<TARGET_FILE:rl_instances_replay>" --emulator "${_rl_inst_emulator}"
                     --out "${CMAKE_BINARY_DIR}/relight/instances/${_app}")
    set_tests_properties(rl_instances_capture_${_app} PROPERTIES
        LABELS "relight;wine;capture" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()
