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
    src/svo.cpp
    src/svo_ray_cast.cpp
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
    include/fuse/scene/wire_stub.hpp
    include/fuse/scene/wire_runtime_bind.hpp
)

add_library(fuse::scene ALIAS fuse_scene)

target_include_directories(fuse_scene
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fuse_scene PUBLIC fuse_core fuse_ecs fuse_project)
target_compile_features(fuse_scene PUBLIC cxx_std_17)

# Single-source "svo_ray_cast" kernel (docs/compute-kernels.md): CUDA trampoline when the toolkit is found.
if(FUSE_CUDA_BACKEND)
    target_sources(fuse_scene PRIVATE kernels/svo_ray_cast.cu)
    set_target_properties(fuse_scene PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
