# fuse_core — L0 FUSE core library (jobs + platform + shared services, U1/WP-03+)

option(FUSE_JOBS_SINGLE_THREAD "Force single-threaded job execution (tests/replay)" OFF)

set(FUSE_CORE_PUBLIC_HEADERS
    include/fuse/types.hpp
    include/fuse/config.hpp
    include/fuse/handle.hpp
    include/fuse/handle_map.hpp
    include/fuse/handle_table.hpp
    include/fuse/frame/frame_ctx.hpp
    include/fuse/frame/frame_barrier.hpp
    include/fuse/dimension/world_handle.hpp
    include/fuse/dimension/idimension.hpp
    include/fuse/object.hpp
    include/fuse/core/init.hpp
    include/fuse/log/logger.hpp
    include/fuse/alloc/frame_allocator.hpp
    include/fuse/io/asset.hpp
    include/fuse/io/vfs.hpp
    include/fuse/jobs/worker_count.hpp
    include/fuse/jobs/worker_context.hpp
    include/fuse/jobs/job_scheduler.hpp
    include/fuse/jobs/job_counter.hpp
    include/fuse/jobs/cuda_jobs.hpp
    include/fuse/jobs/parallel_for.hpp
    include/fuse/math/vec.hpp
    include/fuse/math/sdf.hpp
    include/fuse/platform/power.hpp
    include/fuse/platform/lifecycle.hpp
    include/fuse/platform/surface_loss.hpp
    include/fuse/platform/crash_report.hpp
    include/fuse/platform/profile.hpp
    include/fuse/platform/thread.hpp
    include/fuse/platform/fiber.hpp
    include/fuse/platform/gl_context.hpp
)

set(FUSE_CORE_SOURCES
    src/fuse_core_stub.cpp
    src/core/init.cpp
    src/frame/frame_barrier.cpp
    src/object.cpp
    src/log/logger.cpp
    src/alloc/frame_allocator.cpp
    src/io/vfs.cpp
    src/jobs/worker_count.cpp
    src/jobs/job_scheduler.cpp
    src/jobs/job_counter.cpp
    src/jobs/cuda_jobs.cpp
    src/platform/platform_stub.cpp
    src/platform/platform_hardening.cpp
)

if(WIN32)
    list(APPEND FUSE_CORE_SOURCES src/platform/win/fiber_win32.cpp)
else()
    list(APPEND FUSE_CORE_SOURCES src/platform/fiber_posix.cpp)
endif()

find_package(Threads REQUIRED)

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
target_link_libraries(fuse_core PUBLIC Threads::Threads)

if(FUSE_CUDA_BACKEND)
    target_compile_definitions(fuse_core PUBLIC FUSE_HAS_CUDA=1)
    target_link_libraries(fuse_core PUBLIC CUDA::cudart)
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
