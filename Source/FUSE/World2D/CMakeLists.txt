add_library(fuse_world2d STATIC
    src/scene_object_2d.cpp
    src/scene_snapshot.cpp
    src/transform_stubs.cpp
    src/world_2d.cpp
    include/fuse/world2d/scene_object_2d.hpp
    include/fuse/world2d/scene_snapshot.hpp
    include/fuse/world2d/transform_stubs.hpp
    include/fuse/world2d/world_2d.hpp
)

add_library(fuse::world2d ALIAS fuse_world2d)

target_include_directories(fuse_world2d
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_world2d PUBLIC fuse_core fuse_physics)
target_compile_features(fuse_world2d PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
