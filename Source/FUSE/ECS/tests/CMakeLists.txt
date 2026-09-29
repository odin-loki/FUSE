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
fuse_ecs_add_test(fuse_b3_ecs_gates test_b3_ecs_gates.cpp)
fuse_ecs_add_test(fuse_b3_bvh_gates test_b3_bvh_gates.cpp)
fuse_ecs_add_test(fuse_b3_serialiser test_b3_serialiser.cpp)
# Timing budgets are meaningless while other tests compete for cores.
set_tests_properties(fuse_b3_ecs_gates fuse_b3_bvh_gates PROPERTIES RUN_SERIAL TRUE LABELS "perf;gate")
fuse_ecs_add_test(fuse_ecs_sdf_csg test_sdf_csg.cpp)

# B3 row "CUDA kernel reads Transform positions from managed-memory ECS column — verified with device-side
# assert". CPU gate (everywhere): Registry::set_column_memory_resource keeps every Transform column in a
# pmr resource through migrations / copies / serialisation, and the single-source row check
# (ecs_managed_column_check.hpp) finds all 10k gate positions. Device gate (CUDA builds; exit 77 without a
# device): the same check in a kernel over the column in CUDA managed memory with device-side assert();
# the _assert_fires variant corrupts one row and requires cudaErrorAssert.
fuse_ecs_add_test(fuse_b3_ecs_managed_column test_b3_ecs_managed_column.cpp)
set_tests_properties(fuse_b3_ecs_managed_column PROPERTIES LABELS "gate;cuda")
add_executable(fuse_b3_ecs_managed_column_device test_ecs_managed_column_device.cpp)
target_link_libraries(fuse_b3_ecs_managed_column_device PRIVATE fuse_ecs fuse_core)
if(FUSE_CUDA_BACKEND)
    target_sources(fuse_b3_ecs_managed_column_device PRIVATE cuda/ecs_managed_column_kernel.cu)
    target_link_libraries(fuse_b3_ecs_managed_column_device PRIVATE CUDA::cudart)
    set_target_properties(fuse_b3_ecs_managed_column_device PROPERTIES
        CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_EXTENSIONS OFF)
endif()
add_test(NAME fuse_b3_ecs_managed_column_device COMMAND fuse_b3_ecs_managed_column_device)
add_test(NAME fuse_b3_ecs_managed_column_device_assert_fires COMMAND fuse_b3_ecs_managed_column_device --corrupt)
set_tests_properties(fuse_b3_ecs_managed_column_device fuse_b3_ecs_managed_column_device_assert_fires PROPERTIES
    LABELS "gate;cuda;cuda_device" SKIP_RETURN_CODE 77 TIMEOUT 300)
