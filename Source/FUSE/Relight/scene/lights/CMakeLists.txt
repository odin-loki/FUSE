# FUSE Relight RL-1.5: D3DLIGHT9 -> light records + the stable Remix light hash
# (docs/plans/FUSE_REMIX_PORT_PLAN.md §1.5, §4.1.4). Added from ../translate/CMakeLists.txt (the
# package's one registration line in Source/FUSE/Relight/CMakeLists.txt is add_subdirectory(scene/translate)).
#
# Target
#   fuse_relight_scene_lights   LightData::createFromPointSpot / createFromDirectional / toRtLight,
#                               LightUtils::calculateIntensity, the stable D3D9 light hash, the game-light
#                               step of processRenderState / LightManager::addGameLight, and the
#                               rtx.lightConversion* / rtx.ignoreGame*Lights options. Pure CPU logic.

add_library(fuse_relight_scene_lights STATIC
    src/legacy_light.cpp
    src/light_options.cpp
    src/light_translator.cpp
)
target_include_directories(fuse_relight_scene_lights PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fuse_relight_scene_lights PUBLIC fuse_relight_tap fuse_relight_hash fuse_relight_options)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_scene_lights)
else()
    set_target_properties(fuse_relight_scene_lights PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
