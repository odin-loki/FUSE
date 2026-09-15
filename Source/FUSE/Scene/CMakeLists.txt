add_library(fuse_scene STATIC
    src/camera.cpp
    src/scene.cpp
    src/serialiser.cpp
    src/project_io.cpp
    include/fuse/scene/camera.hpp
    include/fuse/scene/scene.hpp
    include/fuse/scene/serialiser.hpp
    include/fuse/scene/project_io.hpp
)

add_library(fuse::scene ALIAS fuse_scene)

target_include_directories(fuse_scene
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_scene PUBLIC fuse_core fuse_project)
target_compile_features(fuse_scene PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
