add_executable(fuse_core_tests
    test_worker_count.cpp
)

target_link_libraries(fuse_core_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_tests)
add_test(NAME fuse_core_worker_count COMMAND fuse_core_tests)

add_executable(fuse_core_job_tests
    test_jobs.cpp
)

target_link_libraries(fuse_core_job_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_job_tests)
add_test(NAME fuse_core_jobs COMMAND fuse_core_job_tests)

add_executable(fuse_core_fiber_tests
    test_fiber.cpp
)

target_link_libraries(fuse_core_fiber_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_fiber_tests)
add_test(NAME fuse_core_fiber COMMAND fuse_core_fiber_tests)

add_executable(fuse_cuda_job_tests
    test_cuda_jobs.cpp
)

target_link_libraries(fuse_cuda_job_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_cuda_job_tests)
add_test(NAME fuse_cuda_jobs COMMAND fuse_cuda_job_tests)

add_executable(fuse_core_services_tests
    test_services.cpp
)

target_link_libraries(fuse_core_services_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_services_tests)
add_test(NAME fuse_core_services COMMAND fuse_core_services_tests)

add_executable(fuse_core_platform_hardening_tests
    test_platform_hardening.cpp
)

target_link_libraries(fuse_core_platform_hardening_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_platform_hardening_tests)
add_test(NAME fuse_core_platform_hardening COMMAND fuse_core_platform_hardening_tests)

add_executable(fuse_core_io_handle_tests
    test_io_handle.cpp
)

target_link_libraries(fuse_core_io_handle_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_io_handle_tests)
add_test(NAME fuse_core_io_handle COMMAND fuse_core_io_handle_tests)

add_executable(fuse_core_profiler_assert_tests
    test_profiler_assert.cpp
)

target_link_libraries(fuse_core_profiler_assert_tests PRIVATE fuse_core)
add_test(NAME fuse_core_profiler_assert COMMAND fuse_core_profiler_assert_tests)

add_executable(fuse_core_math_tests
    test_math.cpp
)

target_link_libraries(fuse_core_math_tests PRIVATE fuse_core)
add_test(NAME fuse_core_math COMMAND fuse_core_math_tests)

add_executable(fuse_core_allocator_tests
    test_allocator.cpp
)

target_link_libraries(fuse_core_allocator_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_allocator_tests)
add_test(NAME fuse_core_allocator COMMAND fuse_core_allocator_tests)

add_executable(fuse_core_platform_window_tests
    test_platform_window.cpp
)

target_link_libraries(fuse_core_platform_window_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_platform_window_tests)
add_test(NAME fuse_core_platform_window COMMAND fuse_core_platform_window_tests)
