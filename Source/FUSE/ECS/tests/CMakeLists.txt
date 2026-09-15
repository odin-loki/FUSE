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

function(fuse_ecs_add_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE fuse_ecs fuse_core)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

fuse_ecs_add_test(fuse_ecs_system_scheduler test_system_scheduler.cpp)
fuse_ecs_add_test(fuse_ecs_bvh test_bvh.cpp)
fuse_ecs_add_test(fuse_ecs_systems test_systems.cpp)
fuse_ecs_add_test(fuse_ecs_each_parallel test_each_parallel.cpp)
fuse_ecs_add_test(fuse_ecs_transform_system test_transform_system.cpp)
fuse_ecs_add_test(fuse_ecs_query_filter test_ecs_query_filter.cpp)
