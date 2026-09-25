# FUSE Relight RL-1.2: draw classifier and heuristics (docs/plans/FUSE_REMIX_PORT_PLAN.md §1.5).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(scene/classify)
#
# Target
#   fuse_relight_classify   D3DStateModel (tap events -> classifier input), the port of Remix's
#                           makeDrawCallType / isRenderingUI / processTextures categories / sky, terrain
#                           and raytraced-render-target heuristics, the rtx.* options they read, and
#                           ClassifyTap (an IRelightTap running them). Pure CPU logic: builds natively
#                           and for MinGW.
# Tests: ../../tests/classify (rl_classify_unit; Wine: rl_classify_capture_<app>).

add_library(fuse_relight_classify STATIC
    src/classify_options.cpp
    src/classify_tap.cpp
    src/d3d_state_model.cpp
    src/draw_classifier.cpp
)
target_include_directories(fuse_relight_classify PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# fuse_relight_tap: IRelightTap and its event structs; fuse_relight_hash: D3D format table and the
# render-target descriptor hash; fuse_relight_options (through the tap): the rtx.* options.
target_link_libraries(fuse_relight_classify PUBLIC fuse_relight_tap fuse_relight_options PRIVATE fuse_relight_hash)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_classify)
else()
    set_target_properties(fuse_relight_classify PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tests/classify ${CMAKE_CURRENT_BINARY_DIR}/tests)
