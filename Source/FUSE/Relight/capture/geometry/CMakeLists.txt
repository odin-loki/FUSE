# FUSE Relight RL-1.3: geometry extraction + Remix-compatible geometry hashing + FF skinning data
# (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.1.1, Wave R1). Added by one line in
# Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(capture/geometry)
#
# Target
#   fuse_relight_capture_geometry  GeometryCapture (an IRelightTap consumer): memoized index
#                                  rebasing, stream slicing by usage, texcoordIndex selection,
#                                  hash / bounding-box / skinning jobs on fuse::jobs::JobScheduler.
#                                  CPU only (no Vulkan, D3D or DXVK headers); any platform.
# Tests: ../../tests/capture_geometry (rl_capture_geometry_unit, rl_capture_geometry_selftest and,
# in the MinGW tree with Wine, rl_capture_geometry_<app>).

if(NOT TARGET fuse_relight_hash OR NOT TARGET fuse_relight_tap)
    message(FATAL_ERROR "capture/geometry needs fuse_relight_hash (RL-0.5) and fuse_relight_tap (RL-1.1)")
endif()

add_library(fuse_relight_capture_geometry STATIC
    src/geometry_capture.cpp
    src/geometry_options.cpp
    src/index_rebase.cpp
    src/skinning.cpp
    src/texcoord_select.cpp
    src/vertex_streams.cpp
)
target_include_directories(fuse_relight_capture_geometry PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fuse_relight_capture_geometry
    PUBLIC fuse_relight_hash fuse_relight_tap fuse_relight_options fuse_core)
# RL-1.6: GeometryCaptureConfig::fromOptions wires the vertexshader component (vs_hash.hpp); the
# target is added later (capture/vertex_capture), which CMake resolves at generation time.
target_link_libraries(fuse_relight_capture_geometry PRIVATE fuse_relight_vertex_capture)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_capture_geometry)
else()
    set_target_properties(fuse_relight_capture_geometry PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tests/capture_geometry ${CMAKE_CURRENT_BINARY_DIR}/tests)
