# FUSE Relight RL-1.8: capture writers (docs/plans/FUSE_REMIX_PORT_PLAN.md §1.9, §4.1.5, §4.8, Wave R1).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(capture/export). It also adds the
# package's tests (../../tests/capture_export).
#
# Target
#   fuse_relight_capture_export  GameCapturer (port of dxvk-remix's rtx_game_capturer, fed by the RL-1.3 /
#                                RL-1.5 / RL-1.7 per-frame output), the Remix-compatible USDA writer (port of
#                                lssusd::GameExporter, §1.9 layout, no OpenUSD), the DDS writer / reader (BC
#                                passthrough), and the POCO + hash_key store shim (Remaster W0.1-W0.3 are not in
#                                the tree yet) with its re-ingest. Pure CPU logic: native and MinGW.
# Tests: ../../tests/capture_export (rl_capture_export_unit, rl_capture_diff_selftest; Wine:
#        rl_capture_export_<app>). Tool: Tools/FUSE/Relight/capture_diff.py.

foreach(_dep fuse_relight_scene_instances fuse_relight_tap fuse_relight_hash)
    if(NOT TARGET ${_dep})
        message(FATAL_ERROR "capture/export needs ${_dep} (add_subdirectory(capture/export) after RL-1.7)")
    endif()
endforeach()

add_library(fuse_relight_capture_export STATIC
    src/capture_builder.cpp
    src/capture_model.cpp
    src/capture_writer.cpp
    src/dds.cpp
    src/digest.cpp
    src/json.cpp
    src/poco_store.cpp
    src/usda_writer.cpp
)
target_include_directories(fuse_relight_capture_export PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fuse_relight_capture_export
    PUBLIC fuse_relight_scene_instances fuse_relight_capture_geometry fuse_relight_scene_translate fuse_relight_tap
           fuse_relight_hash)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_capture_export)
else()
    set_target_properties(fuse_relight_capture_export PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tests/capture_export ${CMAKE_CURRENT_BINARY_DIR}/tests)
