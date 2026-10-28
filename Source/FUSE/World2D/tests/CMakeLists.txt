add_executable(fuse_world2d_tests
    test_world2d_frame.cpp
)

target_link_libraries(fuse_world2d_tests PRIVATE fuse_world2d fuse_core)

add_test(NAME fuse_world2d_tests COMMAND fuse_world2d_tests)

# Steady-state World2D::tick heap gate (physics, parallel cull, animated image-map tint).
add_executable(fuse_world2d_tick_alloc_gates test_world2d_tick_alloc.cpp)
target_link_libraries(fuse_world2d_tick_alloc_gates PRIVATE fuse_world2d fuse_core)
add_test(NAME fuse_world2d_tick_alloc_gates COMMAND fuse_world2d_tick_alloc_gates)
set_tests_properties(fuse_world2d_tick_alloc_gates PROPERTIES LABELS "gate;world")

add_executable(fuse_world2d_sprite_image_map_tests test_sprite_image_map.cpp)
target_link_libraries(fuse_world2d_sprite_image_map_tests PRIVATE fuse_world2d fuse_core)
target_compile_definitions(fuse_world2d_sprite_image_map_tests PRIVATE FUSE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME fuse_world2d_sprite_image_map_tests COMMAND fuse_world2d_sprite_image_map_tests)
set_tests_properties(fuse_world2d_sprite_image_map_tests PROPERTIES LABELS "u8")

if(FUSE_BUILD_PROJECT)
    add_executable(fuse_world2d_fuselevel_bridge_tests
        test_fuselevel_bridge.cpp
    )

    target_link_libraries(fuse_world2d_fuselevel_bridge_tests PRIVATE fuse_world2d fuse_world_converter fuse_core fuse_project)
    add_test(NAME fuse_world2d_fuselevel_bridge COMMAND fuse_world2d_fuselevel_bridge_tests)
endif()

add_executable(fuse_world2d_tile_edges test_tile_edges.cpp)
target_link_libraries(fuse_world2d_tile_edges PRIVATE fuse_world2d fuse_core)
add_test(NAME fuse_world2d_tile_edges COMMAND fuse_world2d_tile_edges)
