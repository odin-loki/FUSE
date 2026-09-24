# FUSE Relight RL-1.7: instance tracking and the scene model (docs/plans/FUSE_REMIX_PORT_PLAN.md §1.5, Wave R1).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(scene/instances). It also adds
# the package's tests (../../tests/instances).
#
# Target
#   fuse_relight_scene_instances  The CPU scene of Relight: the BLAS cache (DrawCallCache), frame-to-frame
#                                 draw identity (DrawCallTracker: identity hash, same-BLAS nearest within
#                                 rtx.uniqueObjectDistance), RtInstance ids / transform history
#                                 (InstanceManager::processSceneObject), anti-culling (frustum SAT, GC),
#                                 foliage / SSS bookkeeping, the point instancer (CPU reference of the culling
#                                 pass), SceneModel tying them together, and the rtx.sceneScale /
#                                 rtx.uniqueObjectDistance / rtx.antiCulling.* / rtx.pointInstancer.* options.
#                                 Pure CPU logic: native and MinGW.
# Tests: ../../tests/instances (rl_instances_unit; Wine: rl_instances_capture_<app>).

foreach(_dep fuse_relight_scene_translate fuse_relight_capture_geometry)
    if(NOT TARGET ${_dep})
        message(FATAL_ERROR "scene/instances needs ${_dep} (add_subdirectory(scene/instances) after RL-1.3 / RL-1.5)")
    endif()
endforeach()

add_library(fuse_relight_scene_instances STATIC
    src/anti_culling.cpp
    src/draw_call_cache.cpp
    src/draw_call_tracker.cpp
    src/instance_manager.cpp
    src/instance_options.cpp
    src/point_instancer.cpp
    src/scene_input.cpp
    src/scene_model.cpp
)
target_include_directories(fuse_relight_scene_instances PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fuse_relight_scene_instances
    PUBLIC fuse_relight_scene_translate fuse_relight_capture_geometry fuse_relight_hash fuse_relight_options)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_scene_instances)
else()
    set_target_properties(fuse_relight_scene_instances PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tests/instances ${CMAKE_CURRENT_BINARY_DIR}/tests)
