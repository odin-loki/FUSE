# FUSE Relight RL-1.5: fixed-function state translation - material, lights, fog, camera
# (docs/plans/FUSE_REMIX_PORT_PLAN.md §1.5, Wave R1). Added by one line in Source/FUSE/Relight/CMakeLists.txt:
# add_subdirectory(scene/translate). It also adds ../lights and ../camera (same package) and the tests.
#
# Targets
#   fuse_relight_scene_lights     (../lights) D3DLIGHT9 -> light records + light hash
#   fuse_relight_scene_camera     (../camera) camera decomposition, jitter detection, classification
#   fuse_relight_scene_translate  setLegacyMaterialState / setTextureStageState / setFogState /
#                                 processRenderState transforms, the rtx.legacyMaterial.* and related
#                                 options, and TranslateTap (an IRelightTap running RL-1.2's classifier and
#                                 all of the above per draw). Pure CPU logic: native and MinGW.
# Tests: ../../tests/translate (rl_translate_unit, rl_lights_unit, rl_camera_kat, rl_translate_expect_selftest;
# Wine: rl_translate_capture_<app>).

if(NOT TARGET fuse_relight_classify)
    message(FATAL_ERROR "scene/translate needs fuse_relight_classify (RL-1.2)")
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lights ${CMAKE_CURRENT_BINARY_DIR}/lights)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../camera ${CMAKE_CURRENT_BINARY_DIR}/camera)

add_library(fuse_relight_scene_translate STATIC
    src/ff_translate.cpp
    src/translate_tap.cpp
)
target_include_directories(fuse_relight_scene_translate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fuse_relight_scene_translate
    PUBLIC fuse_relight_classify fuse_relight_scene_lights fuse_relight_scene_camera fuse_relight_hash
           fuse_relight_tap fuse_relight_options)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_scene_translate)
else()
    set_target_properties(fuse_relight_scene_translate PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tests/translate ${CMAKE_CURRENT_BINARY_DIR}/tests)
