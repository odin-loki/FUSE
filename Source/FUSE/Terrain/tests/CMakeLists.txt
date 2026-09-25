add_executable(fuse_terrain_tests
    test_terrain.cpp
)

target_link_libraries(fuse_terrain_tests PRIVATE fuse_terrain fuse_core)
add_test(NAME fuse_terrain_b75 COMMAND fuse_terrain_tests)

add_executable(fuse_b7_terrain_gates_tests
    test_b7_terrain_gates.cpp
)

target_link_libraries(fuse_b7_terrain_gates_tests PRIVATE fuse_terrain fuse_core)
add_test(NAME fuse_b7_terrain_gates COMMAND fuse_b7_terrain_gates_tests)
set_tests_properties(fuse_b7_terrain_gates PROPERTIES RUN_SERIAL TRUE LABELS "perf;gate")

# B7.5 "Terrain-SVO cave renders below the surface": CPU render of the hybrid heightfield + SVO ray
# march vs an exact voxel-DDA reference; writes terrain_svo_cave_render.ppm into this build dir.
add_executable(fuse_b7_terrain_cave_render_tests
    test_b7_terrain_cave_render.cpp
)

target_link_libraries(fuse_b7_terrain_cave_render_tests PRIVATE fuse_terrain fuse_core)
target_compile_definitions(fuse_b7_terrain_cave_render_tests PRIVATE
    FUSE_TERRAIN_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME fuse_b7_terrain_cave_render COMMAND fuse_b7_terrain_cave_render_tests)
set_tests_properties(fuse_b7_terrain_cave_render PROPERTIES LABELS "gate")
