# FUSE Relight RL-3.4: the runtime replacement engine (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.2, §4.6, §4.7, Wave R3).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(replace), after RL-3.2 (mods/import).
#
# Targets
#   fuse_relight_replace       the engine, pure CPU (native and MinGW): mod discovery (rtx-remix/mods, fuse-relight/mods,
#                              explicit mods; Remix USD mods through the RL-3.2 importer in memory, FUSE-native imported
#                              stores), the RL-3.2 records + DB rows -> typed replacements (mod_content.hpp), stacking
#                              with priority (mod_stack.hpp), per-mod rtx.conf option layers, per-draw lookup (mesh by
#                              asset hash under each mod's rule, preserveOriginalDrawCall, material by texture hash),
#                              light replace / delete / attach, hot reload (file_watcher.hpp: inotify /
#                              ReadDirectoryChangesW / polling, polling under Wine), texture residency policy
#                              (texture_residency.hpp) and the capture-record JSON (replace_json.hpp).
#   fuse_relight_replace_live  the capture-tap frame processor (replace_live.hpp): CaptureTap (tap/capture) runs it on
#                              every flushed frame, so the replaced scene is written into the capture record
#                              ("replacement" per draw, "replace_frame" per frame). tap/capture links it when it exists
#                              ($<TARGET_NAME_IF_EXISTS>; the two static libraries reference each other).
# Tests: tests/ (rl_replace_unit; Wine: rl_replace_golden_<case>).

foreach(_dep fuse_relight_mods_import fuse_relight_tap_capture fuse_relight_scene_instances fuse_relight_options)
    if(NOT TARGET ${_dep})
        message(FATAL_ERROR "replace needs ${_dep} (add_subdirectory(replace) after RL-3.2 mods/import)")
    endif()
endforeach()

add_library(fuse_relight_replace STATIC
    src/file_watcher.cpp
    src/mod_content.cpp
    src/mod_stack.cpp
    src/replace_options.cpp
    src/replacement_engine.cpp
    src/replace_json.cpp
    src/texture_residency.cpp)
target_include_directories(fuse_relight_replace PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fuse_relight_replace
    PUBLIC fuse_relight_mods_import fuse_relight_capture_export fuse_relight_options fuse_relight_hash
           fuse_relight_classify fuse_relight_scene_translate
    PRIVATE fuse_relight_mods_usd fuse_relight_mods_assets)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_replace)
else()
    set_target_properties(fuse_relight_replace PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

add_library(fuse_relight_replace_live STATIC src/replace_live.cpp)
target_link_libraries(fuse_relight_replace_live
    PUBLIC fuse_relight_replace fuse_relight_tap_capture fuse_relight_scene_instances)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_replace_live)
else()
    set_target_properties(fuse_relight_replace_live PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

add_subdirectory(tests)
