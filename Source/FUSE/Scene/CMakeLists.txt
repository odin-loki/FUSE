add_library(fuse_scene STATIC
    src/bvh_stub.cpp
    src/camera.cpp
    src/free_camera.cpp
    src/mission_load.cpp
    src/project_io.cpp
    src/scene.cpp
    src/scene_manager.cpp
    src/scene_snapshot.cpp
    src/serialiser.cpp
    src/wire_stub.cpp
    src/wire_runtime_bind.cpp
    include/fuse/scene/bvh_stub.hpp
    include/fuse/scene/camera.hpp
    include/fuse/scene/free_camera.hpp
    include/fuse/scene/math.hpp
    include/fuse/scene/mission_load.hpp
    include/fuse/scene/project_io.hpp
    include/fuse/scene/scene.hpp
    include/fuse/scene/scene_manager.hpp
    include/fuse/scene/scene_snapshot.hpp
    include/fuse/scene/serialiser.hpp
    include/fuse/scene/svo.hpp
    include/fuse/scene/svo_ray_kernel.hpp
    include/fuse/scene/svo_ray_device.hpp
    include/fuse/scene/wire_stub.hpp
    include/fuse/scene/wire_runtime_bind.hpp
)

add_library(fuse::scene ALIAS fuse_scene)

target_include_directories(fuse_scene
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_scene PUBLIC fuse_core fuse_ecs fuse_project)

# The SVO (src/svo.cpp, src/svo_ray_cast.cpp and the CUDA "svo_ray_cast" kernel) is the fuse_svo library
# (cmake/fuse_svo.cmake), shared with fuse_physics (UNI-B4-VOX-1).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/fuse_svo.cmake)
target_link_libraries(fuse_scene PUBLIC fuse_svo)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
