add_executable(fuse_core_tests
    test_worker_count.cpp
)

target_link_libraries(fuse_core_tests PRIVATE fuse_core)
add_test(NAME fuse_core_worker_count COMMAND fuse_core_tests)

add_executable(fuse_core_job_tests
    test_jobs.cpp
)

target_link_libraries(fuse_core_job_tests PRIVATE fuse_core)
add_test(NAME fuse_core_jobs COMMAND fuse_core_job_tests)

add_executable(fuse_cuda_job_tests
    test_cuda_jobs.cpp
)

target_link_libraries(fuse_cuda_job_tests PRIVATE fuse_core)
add_test(NAME fuse_cuda_jobs COMMAND fuse_cuda_job_tests)

add_executable(fuse_core_services_tests
    test_services.cpp
)

target_link_libraries(fuse_core_services_tests PRIVATE fuse_core)
add_test(NAME fuse_core_services COMMAND fuse_core_services_tests)
