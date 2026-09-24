# FUSE Relight RL-1.1: the capture tap mode (relight.tap.mode = capture). Added by one line in
# Source/FUSE/Relight/CMakeLists.txt, after the packages it wires in (RL-1.2 to RL-1.5):
# add_subdirectory(tap/capture)
#
# Target
#   fuse_relight_tap_capture  CaptureTap (capture_tap.hpp): TextureTracker (RL-1.4), GeometryCapture
#                             (RL-1.3) and TranslateTap (the RL-1.2 classifier + RL-1.5 fixed-function
#                             translation, lights, camera) run live on the tap's events, with a
#                             per-frame capture record; createTapForDevice (device_tap.hpp), the factory
#                             the d3d9 dispatcher uses for every mode. CPU only, any platform. In PE
#                             builds it is linked into d3d9.dll (link-only, like fuse_relight_tap).
# Tests: ../../tests/tap_capture (rl_tap_capture_unit; Wine: rl_capture_live_<app>).

foreach(_dep fuse_relight_tap fuse_relight_classify fuse_relight_capture_geometry fuse_relight_capture_texture
             fuse_relight_scene_translate)
    if(NOT TARGET ${_dep})
        message(FATAL_ERROR "tap/capture needs ${_dep} (add_subdirectory(tap/capture) after RL-1.2/1.3/1.4/1.5)")
    endif()
endforeach()

add_library(fuse_relight_tap_capture STATIC src/capture_tap.cpp)
target_include_directories(fuse_relight_tap_capture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fuse_relight_tap_capture PUBLIC
    fuse_relight_tap fuse_relight_classify fuse_relight_capture_geometry fuse_relight_capture_texture
    fuse_relight_scene_translate)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_tap_capture)
else()
    set_target_properties(fuse_relight_tap_capture PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

if(TARGET relight_d3d9)
    # The dispatcher (fuse_relight_tap_d3d9, ../CMakeLists.txt) calls createTapForDevice. Link only:
    # FUSE's usage requirements (fuse_core include dirs and defines) stay out of the DXVK sources.
    target_link_libraries(relight_d3d9 PRIVATE $<LINK_ONLY:fuse_relight_tap_capture>)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tests/tap_capture ${CMAKE_CURRENT_BINARY_DIR}/tests)
