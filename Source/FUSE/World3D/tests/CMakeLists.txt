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
