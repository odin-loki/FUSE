add_executable(fuse_animation_tests
    test_animation.cpp
)

target_link_libraries(fuse_animation_tests PRIVATE fuse_animation fuse_core fuse_ecs)
add_test(NAME fuse_animation_runtime COMMAND fuse_animation_tests)

# B7.1 / B7.10 animation gates: skeleton binary load, clip keyframe sampling, BlendNode2,
# state-machine crossfade timing, FABRIK / TwoBoneIK accuracy, Animator bone buffer (no leak).
add_executable(fuse_b7_animation_gates
    test_b7_animation_gates.cpp
)

target_link_libraries(fuse_b7_animation_gates PRIVATE fuse_animation fuse_core fuse_ecs)
add_test(NAME fuse_b7_animation_gates COMMAND fuse_b7_animation_gates)
set_tests_properties(fuse_b7_animation_gates PROPERTIES LABELS "gate")
