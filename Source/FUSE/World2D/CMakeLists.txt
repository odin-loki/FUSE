add_library(fuse_world2d STATIC
    src/scene_object_2d.cpp
    src/scene_snapshot.cpp
    src/transform_stubs.cpp
    src/world_2d.cpp
    include/fuse/world2d/cull_fork_join.hpp
    include/fuse/world2d/scene_object_2d.hpp
    include/fuse/world2d/scene_snapshot.hpp
    include/fuse/world2d/transform_stubs.hpp
    include/fuse/world2d/fuselevel_bridge.hpp
    include/fuse/world2d/world_2d.hpp
)

add_library(fuse::world2d ALIAS fuse_world2d)

target_include_directories(fuse_world2d
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_world2d PUBLIC fuse_core fuse_physics)

# `.fuselevel` loading (fuselevel_bridge) reads converted worlds through fuse_scene, which is only
# built with FUSE_BUILD_PROJECT (fuse_scene links fuse_project). Without it World2D still builds and
# ticks; loadWorldFromFuselevel() returns ok=false and the wire-binding fields are compiled out.
if(FUSE_BUILD_PROJECT)
    target_sources(fuse_world2d PRIVATE src/fuselevel_bridge.cpp)
    target_link_libraries(fuse_world2d PUBLIC fuse_scene)
    target_compile_definitions(fuse_world2d PUBLIC FUSE_WORLD2D_HAS_FUSELEVEL=1)
else()
    message(STATUS "FUSE: FUSE_BUILD_PROJECT=OFF — fuse_world2d built without .fuselevel loading (no fuse_scene)")
endif()
target_compile_features(fuse_world2d PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
