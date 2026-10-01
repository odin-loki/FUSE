add_executable(fuse_scene_tests
    test_scene_hierarchy.cpp
)

target_link_libraries(fuse_scene_tests PRIVATE fuse_world2d fuse_world3d)
add_test(NAME fuse_scene_hierarchy COMMAND fuse_scene_tests)

# UNI-WP05-1: TRS world transforms, cache invalidation, reparent, HandleTable publication, snapshot matrices.
add_executable(fuse_scene_transform_gates test_scene_transforms.cpp)
target_link_libraries(fuse_scene_transform_gates PRIVATE fuse_world2d fuse_world3d)
add_test(NAME fuse_scene_transform_gates COMMAND fuse_scene_transform_gates)
set_tests_properties(fuse_scene_transform_gates PROPERTIES LABELS "gate;world")

# GAP-GAME-LOOP-ECS: runtime frame schedule (order, determinism, zero steady-state allocations).
add_executable(fuse_runtime_schedule_gates test_runtime_schedule.cpp)
target_link_libraries(fuse_runtime_schedule_gates PRIVATE fuse_world3d)
if(FUSE_BUILD_SCRIPT)
    target_link_libraries(fuse_runtime_schedule_gates PRIVATE fuse_script)
    target_compile_definitions(fuse_runtime_schedule_gates PRIVATE FUSE_TEST_HAS_SCRIPT=1)
endif()
add_test(NAME fuse_runtime_schedule_gates COMMAND fuse_runtime_schedule_gates)
set_tests_properties(fuse_runtime_schedule_gates PROPERTIES LABELS "gate;world" TIMEOUT 300)

# UNI-U7-WORLD-1 (E16): World3D / World2D `.fuselevel` load + save (v3 components, round trip, level physics) on the
# converted demo_3d_empty mission and Templates/BaseGame ExampleLevel.mis. Needs fuse_scene (FUSE_BUILD_PROJECT).
if(FUSE_BUILD_PROJECT)
    add_executable(fuse_world3d_level_gates test_world3d_level.cpp)
    target_link_libraries(fuse_world3d_level_gates PRIVATE fuse_world3d fuse_world2d fuse_world_converter fuse_project)
    file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" _fuse_world3d_source_dir)
    target_compile_definitions(fuse_world3d_level_gates PRIVATE FUSE_SOURCE_DIR="${_fuse_world3d_source_dir}")
    add_test(NAME fuse_world3d_level_gates COMMAND fuse_world3d_level_gates)
    set_tests_properties(fuse_world3d_level_gates PROPERTIES LABELS "gate;world" TIMEOUT 300)
endif()
