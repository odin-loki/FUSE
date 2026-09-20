# fuse_core — L0 FUSE core library (jobs + platform + shared services, U1/WP-03+)

if(EMSCRIPTEN)
    set(_fuse_jobs_single_thread_default ON)
else()
    set(_fuse_jobs_single_thread_default OFF)
endif()
option(FUSE_JOBS_SINGLE_THREAD "Force single-threaded job execution (tests/replay/Emscripten stub)" ${_fuse_jobs_single_thread_default})
option(FUSE_CORE_BAN_GLOBAL_NEW
    "Debug: intercept naked new/delete/malloc in fuse_core TUs (never replace process-wide ::operator new)"
    OFF)
option(FUSE_TRACK_B_UNLOCK "Unlock Track B production defaults on FUSE APIs (P7)" OFF)

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
    include/fuse/core/phase1_test_registry.hpp
    include/fuse/core/track_b.hpp
    include/fuse/log/logger.hpp
    include/fuse/assert.hpp
    include/fuse/profiler/profiler.hpp
    include/fuse/alloc/alloc_stats.hpp
    include/fuse/alloc/allocator.hpp
    include/fuse/alloc/new_ban.hpp
    include/fuse/alloc/frame_allocator.hpp
    include/fuse/alloc/pool_allocator.hpp
    include/fuse/alloc/stack_allocator.hpp
    include/fuse/alloc/ring_allocator.hpp
    include/fuse/alloc/freelist_allocator.hpp
    include/fuse/alloc/domain_budget.hpp
    include/fuse/io/asset.hpp
    include/fuse/io/vfs.hpp
    include/fuse/jobs/worker_count.hpp
    include/fuse/jobs/worker_context.hpp
    include/fuse/jobs/work_steal.hpp
    include/fuse/jobs/job_scheduler.hpp
    include/fuse/jobs/job_counter.hpp
    include/fuse/jobs/cuda_jobs.hpp
    include/fuse/jobs/parallel_for.hpp
    include/fuse/math/vec.hpp
    include/fuse/math/quat.hpp
    include/fuse/math/mat.hpp
    include/fuse/math/aabb.hpp
    include/fuse/math/frustum.hpp
    include/fuse/math/plane.hpp
    include/fuse/math/sdf.hpp
    include/fuse/math/simd.hpp
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
    include/fuse/platform/input.hpp
    include/fuse/platform/window_wsi.hpp
    include/fuse/platform/event_pump.hpp
)

set(FUSE_CORE_SOURCES
    src/fuse_core_stub.cpp
    src/core/init.cpp
    src/core/phase1_test_registry.cpp
    src/core/track_b.cpp
    src/frame/frame_barrier.cpp
    src/object.cpp
    src/log/logger.cpp
    src/assert/fatal_handler.cpp
    src/profiler/profiler.cpp
    src/alloc/alloc_stats.cpp
    src/alloc/allocator_detail.cpp
    src/alloc/new_ban.cpp
    src/alloc/frame_allocator.cpp
    src/alloc/pool_allocator.cpp
    src/alloc/stack_allocator.cpp
    src/alloc/ring_allocator.cpp
    src/alloc/freelist_allocator.cpp
    src/alloc/domain_budget.cpp
    src/io/vfs.cpp
    src/jobs/worker_count.cpp
    src/jobs/work_steal.cpp
    src/jobs/job_scheduler.cpp
    src/jobs/job_counter.cpp
    src/jobs/cuda_jobs.cpp
    src/platform/platform_stub.cpp
    src/platform/platform_hardening.cpp
    src/platform/platform_window.cpp
    src/platform/input.cpp
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

if(FUSE_TRACK_B_UNLOCK)
    target_compile_definitions(fuse_core PUBLIC FUSE_TRACK_B_UNLOCK=1)
else()
    target_compile_definitions(fuse_core PUBLIC FUSE_TRACK_B_UNLOCK=0)
endif()

if((FUSE_CORE_ENABLE_ASAN OR FUSE_CORE_ENABLE_UBSAN) AND FUSE_CORE_ENABLE_TSAN)
    message(FATAL_ERROR "FUSE_CORE_ENABLE_ASAN/FUSE_CORE_ENABLE_UBSAN and FUSE_CORE_ENABLE_TSAN are mutually exclusive")
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

function(fuse_core_apply_asan_ubsan target)
    if(NOT FUSE_CORE_ENABLE_ASAN AND NOT FUSE_CORE_ENABLE_UBSAN)
        return()
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        if(FUSE_CORE_ENABLE_ASAN AND FUSE_ASAN_SUPPORTED)
            target_compile_options(${target} PRIVATE /fsanitize=address)
            target_link_options(${target} PRIVATE /fsanitize=address)
        endif()
        return()
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        return()
    endif()
    set(_fuse_core_sanitizers)
    if(FUSE_CORE_ENABLE_ASAN AND FUSE_ASAN_SUPPORTED)
        list(APPEND _fuse_core_sanitizers address)
    endif()
    if(FUSE_CORE_ENABLE_UBSAN)
        list(APPEND _fuse_core_sanitizers undefined)
    endif()
    if(NOT _fuse_core_sanitizers)
        return()
    endif()
    list(JOIN _fuse_core_sanitizers "," _fuse_core_sanitizer_csv)
    target_compile_options(${target} PRIVATE
        -fsanitize=${_fuse_core_sanitizer_csv}
        -fno-omit-frame-pointer
    )
    target_link_options(${target} PRIVATE
        -fsanitize=${_fuse_core_sanitizer_csv}
    )
endfunction()

if(FUSE_CORE_ENABLE_UBSAN AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(STATUS "FUSE: FUSE_CORE_ENABLE_UBSAN ignored on MSVC (no /fsanitize=undefined)")
endif()

fuse_apply_cxx23(fuse_core)
target_link_libraries(fuse_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(fuse_core PUBLIC user32)
endif()

target_compile_definitions(fuse_core
    PUBLIC
        $<$<CONFIG:Debug>:FUSE_DEBUG=1>
        $<$<CONFIG:RelWithDebInfo>:FUSE_PROFILE=1>
)

if(FUSE_PROFILE)
    target_compile_definitions(fuse_core PUBLIC FUSE_PROFILE=1)
endif()

if(FUSE_SHIPPING)
    target_compile_definitions(fuse_core PUBLIC FUSE_NO_ASSERT=1 FUSE_SHIPPING=1)
endif()

if(FUSE_CORE_BAN_GLOBAL_NEW)
    # PRIVATE + Debug only. Engine TUs that include fuse/alloc/new_ban.hpp route
    # keyword new/malloc through HeapGuard. Do not replace global ::operator new —
    # Qt/STL in the same process would abort.
    target_compile_definitions(fuse_core
        PRIVATE
            $<$<CONFIG:Debug>:FUSE_ENGINE_HEAP_GUARD=1>
    )
endif()

if(FUSE_CUDA_BACKEND)
    target_compile_definitions(fuse_core PUBLIC FUSE_HAS_CUDA=1)
    target_link_libraries(fuse_core PUBLIC CUDA::cudart)
endif()

fuse_core_apply_tsan(fuse_core)
fuse_core_apply_asan_ubsan(fuse_core)

option(FUSE_PLATFORM_WINDOW_GLFW "Enable optional GLFW hidden-window WSI scaffold (desktop present path)" OFF)
set(_fuse_glfw_wsi_active FALSE)
if(FUSE_PLATFORM_WINDOW_GLFW)
    find_package(glfw3 QUIET)
    if(NOT TARGET Vulkan::Vulkan)
        include(${CMAKE_SOURCE_DIR}/cmake/FuseFindVulkan.cmake)
    endif()
    if(glfw3_FOUND AND TARGET Vulkan::Vulkan)
        target_sources(fuse_core PRIVATE src/platform/window_wsi_glfw.cpp)
        target_link_libraries(fuse_core PRIVATE glfw Vulkan::Vulkan)
        target_compile_definitions(fuse_core PUBLIC FUSE_PLATFORM_WINDOW_GLFW=1)
        message(STATUS "FUSE: fuse_core GLFW null-WSI scaffold enabled")
        set(_fuse_glfw_wsi_active TRUE)
    else()
        message(STATUS "FUSE: FUSE_PLATFORM_WINDOW_GLFW=ON but glfw3/Vulkan missing — null WSI only")
    endif()
endif()
if(NOT _fuse_glfw_wsi_active)
    target_sources(fuse_core PRIVATE src/platform/window_wsi.cpp)
    if(WIN32)
    if(NOT TARGET Vulkan::Vulkan)
        include(${CMAKE_SOURCE_DIR}/cmake/FuseFindVulkan.cmake)
    endif()
    if(TARGET Vulkan::Vulkan)
            set(FUSE_PLATFORM_WINDOW_WIN32 ON CACHE INTERNAL "Win32 WSI compiled into fuse_core")
            target_compile_definitions(fuse_core PUBLIC
                FUSE_PLATFORM_WINDOW_WIN32=1
                VK_USE_PLATFORM_WIN32_KHR=1
            )
            target_link_libraries(fuse_core PUBLIC Vulkan::Vulkan)
            message(STATUS "FUSE: fuse_core Win32 WSI enabled")
        endif()
    endif()
endif()

if(FUSE_BUILD_CORE_TESTS)
    add_subdirectory(tests)
endif()
