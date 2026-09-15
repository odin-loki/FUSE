add_executable(fuse_terrain_tests
    test_terrain.cpp
)

target_link_libraries(fuse_terrain_tests PRIVATE fuse_terrain fuse_core)
add_test(NAME fuse_terrain_b75 COMMAND fuse_terrain_tests)
