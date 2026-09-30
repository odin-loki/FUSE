add_library(fuse_world3d STATIC
    src/scene_object_3d.cpp
    src/scene_snapshot.cpp
    src/runtime_schedule.cpp
    src/world_3d.cpp
    include/fuse/world3d/scene_object_3d.hpp
    include/fuse/world3d/scene_snapshot.hpp
    include/fuse/world3d/runtime_schedule.hpp
    include/fuse/world3d/runtime_schedule_script.hpp
    include/fuse/world3d/world_3d.hpp
    include/fuse/world3d/render_scene.hpp
)

add_library(fuse::world3d ALIAS fuse_world3d)

target_include_directories(fuse_world3d
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# GAP-GAME-LOOP-ECS runtime schedule: animation, PhysicsManager, VFX (and audio when built).
target_link_libraries(fuse_world3d PUBLIC fuse_world2d fuse_physics fuse_ecs fuse_animation fuse_vfx)

# fuse_audio is added after World3D (Source/FUSE/CMakeLists.txt order), so key off the option, not TARGET.
if(FUSE_BUILD_AUDIO)
    target_link_libraries(fuse_world3d PUBLIC fuse_audio)
    target_compile_definitions(fuse_world3d PUBLIC FUSE_WORLD3D_HAS_AUDIO=1)
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
