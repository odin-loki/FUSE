# FUSE Relight RL-1.5: camera decomposition and classification (docs/plans/FUSE_REMIX_PORT_PLAN.md §1.5
# "Camera"). Added from ../translate/CMakeLists.txt (the package's one registration line in
# Source/FUSE/Relight/CMakeLists.txt is add_subdirectory(scene/translate)).
#
# Target
#   fuse_relight_scene_camera   decomposeProjection (MathLib DecomposeProjection, NDC_D3D), game TAA jitter
#                               detection, CameraManager::processCameraData (Main / Sky / ViewModel /
#                               RenderToTexture / Unknown), per-camera state and camera cuts, and the
#                               rtx.fusedWorldViewMode / rtx.viewModel.{enable,maxZThreshold} options.

add_library(fuse_relight_scene_camera STATIC
    src/camera_manager.cpp
    src/projection.cpp
)
target_include_directories(fuse_relight_scene_camera PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fuse_relight_scene_camera PUBLIC fuse_relight_options)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_scene_camera)
else()
    set_target_properties(fuse_relight_scene_camera PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
