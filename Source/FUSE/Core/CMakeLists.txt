# fuse_core — L0 FUSE core library (jobs + platform stubs, U1/WP-03)

option(FUSE_JOBS_SINGLE_THREAD "Force single-threaded job execution (tests/replay)" OFF)

set(FUSE_CORE_PUBLIC_HEADERS
    include/fuse/types.hpp
    include/fuse/config.hpp
    include/fuse/jobs/worker_count.hpp
    include/fuse/jobs/job_scheduler.hpp
    include/fuse/jobs/job_counter.hpp
    include/fuse/jobs/parallel_for.hpp
    include/fuse/platform/power.hpp
    include/fuse/platform/thread.hpp
)

set(FUSE_CORE_SOURCES
    src/fuse_core_stub.cpp
    src/jobs/worker_count.cpp
    src/jobs/job_scheduler_stub.cpp
    src/jobs/job_counter_stub.cpp
    src/platform/platform_stub.cpp
)

add_library(fuse_core STATIC
    ${FUSE_CORE_SOURCES}
    ${FUSE_CORE_PUBLIC_HEADERS}
)

add_library(fuse::core ALIAS fuse_core)

target_include_directories(fuse_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(fuse_core
    PRIVATE
        $<$<BOOL:${FUSE_PLATFORM_MOBILE}>:FUSE_PLATFORM_MOBILE=1>
        $<$<BOOL:${FUSE_PLATFORM_DESKTOP}>:FUSE_PLATFORM_DESKTOP=1>
)

if(FUSE_JOBS_SINGLE_THREAD)
    target_compile_definitions(fuse_core PUBLIC FUSE_JOBS_SINGLE_THREAD=1)
endif()

target_compile_features(fuse_core PUBLIC cxx_std_17)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
