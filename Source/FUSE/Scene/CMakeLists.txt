add_library(fuse_scene STATIC
    src/bvh_stub.cpp
    src/camera.cpp
    src/project_io.cpp
    src/scene.cpp
    src/scene_manager.cpp
    src/scene_snapshot.cpp
    src/serialiser.cpp
    src/svo.cpp
    include/fuse/scene/bvh_stub.hpp
    include/fuse/scene/camera.hpp
    include/fuse/scene/math.hpp
    include/fuse/scene/project_io.hpp
    include/fuse/scene/scene.hpp
    include/fuse/scene/scene_manager.hpp
    include/fuse/scene/scene_snapshot.hpp
    include/fuse/scene/serialiser.hpp
    include/fuse/scene/svo.hpp
)

add_library(fuse::scene ALIAS fuse_scene)

target_include_directories(fuse_scene
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_scene PUBLIC fuse_core fuse_ecs fuse_project)
target_compile_features(fuse_scene PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
