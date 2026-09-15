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
    include/fuse/assert.hpp
    include/fuse/profiler/profiler.hpp
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
    include/fuse/math/quat.hpp
    include/fuse/math/mat.hpp
    include/fuse/math/aabb.hpp
    include/fuse/math/frustum.hpp
    include/fuse/math/sdf.hpp
    include/fuse/math/math.hpp
    include/fuse/platform/power.hpp
    include/fuse/platform/lifecycle.hpp
    include/fuse/platform/surface_loss.hpp
    include/fuse/platform/crash_report.hpp
    include/fuse/platform/profile.hpp
    include/fuse/platform/thread.hpp
    include/fuse/platform/fiber.hpp
    include/fuse/platform/gl_context.hpp
    include/fuse/platform/window.hpp
)

set(FUSE_CORE_SOURCES
    src/fuse_core_stub.cpp
    src/core/init.cpp
    src/frame/frame_barrier.cpp
    src/object.cpp
    src/log/logger.cpp
    src/assert/fatal_handler.cpp
    src/profiler/profiler.cpp
    src/alloc/frame_allocator.cpp
    src/io/vfs.cpp
    src/jobs/worker_count.cpp
    src/jobs/job_scheduler.cpp
    src/jobs/job_counter.cpp
    src/jobs/cuda_jobs.cpp
    src/platform/platform_stub.cpp
    src/platform/platform_hardening.cpp
    src/platform/window_stub.cpp
)

option(FUSE_PLATFORM_WINDOW_GLFW "Enable GLFW desktop window backend in fuse_core (off in CI)" OFF)
set(FUSE_HAS_GLFW_WINDOW FALSE)
if(FUSE_PLATFORM_WINDOW_GLFW)
    find_package(glfw3 QUIET)
    if(glfw3_FOUND)
        list(APPEND FUSE_CORE_SOURCES src/platform/window_glfw.cpp)
        set(FUSE_HAS_GLFW_WINDOW TRUE)
        message(STATUS "FUSE: GLFW window backend enabled for fuse_core")
    else()
        message(STATUS "FUSE: FUSE_PLATFORM_WINDOW_GLFW=ON but glfw3 not found — null window only")
    endif()
endif()

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

if(FUSE_CORE_ENABLE_TSAN)
    if(FUSE_SMOKE_ENABLE_ASAN)
        message(FATAL_ERROR "FUSE_CORE_ENABLE_TSAN and FUSE_SMOKE_ENABLE_ASAN are mutually exclusive")
    endif()
    function(fuse_core_apply_tsan target)
        target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endfunction()
else()
    function(fuse_core_apply_tsan target)
    endfunction()
endif()

target_compile_features(fuse_core PUBLIC cxx_std_17)
target_link_libraries(fuse_core PUBLIC Threads::Threads)

if(FUSE_HAS_GLFW_WINDOW)
    target_compile_definitions(fuse_core PRIVATE FUSE_HAS_GLFW_WINDOW=1)
    target_link_libraries(fuse_core PRIVATE glfw)
endif()

target_compile_definitions(fuse_core
    PUBLIC
        $<$<CONFIG:Debug>:FUSE_DEBUG=1>
        $<$<CONFIG:RelWithDebInfo>:FUSE_PROFILE=1>
)

if(FUSE_CUDA_BACKEND)
    target_compile_definitions(fuse_core PUBLIC FUSE_HAS_CUDA=1)
    target_link_libraries(fuse_core PUBLIC CUDA::cudart)
endif()

fuse_core_apply_tsan(fuse_core)

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
