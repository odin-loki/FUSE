# FUSE Relight RL-1.4: texture tracking + Remix-compatible hash on upload
# (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.1.3, Wave R1).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(capture/texture)
#
# Target
#   fuse_relight_capture_texture   CanonicalMip0 (canonical repack), TextureTracker (first-upload
#                                  hashing, Update* inheritance, recompute-on-write, render-target
#                                  descriptor hash; an IRelightTap), ExternalImageRegistry
#                                  (pre-bindless), rtx.* options. CPU only, any platform.
# Tests: ../../tests/capture_texture (rl_capture_texture_unit; Wine: rl_capture_texture_<app>).

add_library(fuse_relight_capture_texture STATIC
    src/canonical_mip0.cpp
    src/external_image_registry.cpp
    src/texture_options.cpp
    src/texture_tracker.cpp
)
target_include_directories(fuse_relight_capture_texture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fuse_relight_capture_texture PUBLIC fuse_relight_hash fuse_relight_tap fuse_relight_options)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_capture_texture)
else()
    set_target_properties(fuse_relight_capture_texture PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tests/capture_texture ${CMAKE_CURRENT_BINARY_DIR}/tests)
