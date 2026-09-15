add_library(fuse_world2d STATIC
    src/scene_object_2d.cpp
    include/fuse/world2d/scene_object_2d.hpp
)

add_library(fuse::world2d ALIAS fuse_world2d)

target_include_directories(fuse_world2d
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_world2d PUBLIC fuse_core)
target_compile_features(fuse_world2d PUBLIC cxx_std_17)
