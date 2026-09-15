add_executable(fuse_ecs_registry_tests
    test_ecs_registry.cpp
)

target_link_libraries(fuse_ecs_registry_tests PRIVATE fuse_ecs)
add_test(NAME fuse_ecs_registry COMMAND fuse_ecs_registry_tests)

add_executable(fuse_ecs_components_tests
    test_ecs_components.cpp
)

target_link_libraries(fuse_ecs_components_tests PRIVATE fuse_ecs)
add_test(NAME fuse_ecs_components COMMAND fuse_ecs_components_tests)
