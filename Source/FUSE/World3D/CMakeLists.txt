add_library(fuse_world3d STATIC
    src/scene_object_3d.cpp
    include/fuse/world3d/scene_object_3d.hpp
)

add_library(fuse::world3d ALIAS fuse_world3d)

target_include_directories(fuse_world3d
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_world3d PUBLIC fuse_world2d)
target_compile_features(fuse_world3d PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
