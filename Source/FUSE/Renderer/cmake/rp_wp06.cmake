# WP-0.6 (docs/unification/RENDERER-EXECUTION.md): profiling and capture.
#   fuse_rhi_profiling        GpuProfiler (timestamp-query GPU zones per render-graph pass through
#                             rg::PassHooks; TracyVk zones when FUSE_TRACY=ON) + RenderDocCapture
#                             (in-app API loaded at run time, vendored Engine/lib/renderdoc header)
#   fuse_rhi_profiling_tracy  the same sources built with the Tracy backend (FUSE_TRACY=OFF builds
#                             only: tests the enabled variant without a second build tree)
# Owned by WP-0.6; other packages must not edit this file.

set(_fuse_wp06_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/renderer/vk/gpu_profiler.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/renderer/vk/renderdoc_capture.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/gpu_profiler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/renderdoc_capture.cpp)
set(_fuse_wp06_renderdoc_dir "${FUSE_VENDOR_DIR}/renderdoc")

function(_fuse_wp06_profiling_library name)
    add_library(${name} STATIC ${_fuse_wp06_sources})
    target_link_libraries(${name} PUBLIC fuse_rhi PRIVATE ${CMAKE_DL_LIBS})
    # SYSTEM: vendored MIT header (renderdoc_app.h), verbatim.
    target_include_directories(${name} SYSTEM PRIVATE "${_fuse_wp06_renderdoc_dir}/include")
endfunction()

_fuse_wp06_profiling_library(fuse_rhi_profiling)
add_library(fuse::rhi_profiling ALIAS fuse_rhi_profiling)
if(NOT FUSE_TRACY AND TARGET fuse_profiler_tracy)
    _fuse_wp06_profiling_library(fuse_rhi_profiling_tracy)
    target_link_libraries(fuse_rhi_profiling_tracy PUBLIC fuse_profiler_tracy)
endif()

if(FUSE_BUILD_CORE_TESTS)
    # tests/CMakeLists.txt writes the ICD lock wrapper; its variable is scoped to that directory.
    set(_fuse_rp_wp06_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")
    set(_fuse_rp_wp06_env "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json")

    # Vendored renderdoc_app.h matches its VERSION pin (sha256 + declared API version).
    add_test(NAME fuse_rp_renderdoc_header_pin
             COMMAND "${CMAKE_COMMAND}" -DPIN_DIR=${_fuse_wp06_renderdoc_dir}
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp06_pin_check.cmake")
    set_tests_properties(fuse_rp_renderdoc_header_pin PROPERTIES LABELS "gate;lint;renderer")

    # RenderDoc hook (CPU, stub-safe): "unavailable" without RenderDoc, triggers, and the capture
    # path against a counting RENDERDOC_GetAPI double.
    add_library(fuse_rp_fake_renderdoc MODULE tests/rp_wp06_fake_renderdoc.cpp)
    target_include_directories(fuse_rp_fake_renderdoc SYSTEM PRIVATE "${_fuse_wp06_renderdoc_dir}/include")
    add_executable(fuse_rp_renderdoc_capture tests/test_rp_renderdoc_capture.cpp)
    target_link_libraries(fuse_rp_renderdoc_capture PRIVATE fuse_rhi_profiling ${CMAKE_DL_LIBS})
    target_compile_definitions(fuse_rp_renderdoc_capture PRIVATE
        FUSE_RP_FAKE_RENDERDOC="$<TARGET_FILE:fuse_rp_fake_renderdoc>")
    add_dependencies(fuse_rp_renderdoc_capture fuse_rp_fake_renderdoc)
    add_test(NAME fuse_rp_renderdoc_capture COMMAND fuse_rp_renderdoc_capture)
    set_tests_properties(fuse_rp_renderdoc_capture PROPERTIES LABELS "gate;renderer;profiling")

    # GPU zones on Lavapipe (validation + sync validation, 0 messages; 77 = skip): the configured
    # FUSE_TRACY mode, and the Tracy-on variant (TracyVk contexts on the device).
    set(_fuse_wp06_gpu_tests fuse_rp_gpu_profiler)
    add_executable(fuse_rp_gpu_profiler tests/test_rp_gpu_profiler.cpp)
    target_link_libraries(fuse_rp_gpu_profiler PRIVATE fuse_rhi_profiling)
    if(TARGET fuse_rhi_profiling_tracy)
        add_executable(fuse_rp_gpu_profiler_tracy tests/test_rp_gpu_profiler.cpp)
        target_link_libraries(fuse_rp_gpu_profiler_tracy PRIVATE fuse_rhi_profiling_tracy)
        target_compile_definitions(fuse_rp_gpu_profiler_tracy PRIVATE FUSE_RP_GPU_PROFILER_EXPECT_TRACY=1)
        list(APPEND _fuse_wp06_gpu_tests fuse_rp_gpu_profiler_tracy)
    endif()
    foreach(_t IN LISTS _fuse_wp06_gpu_tests)
        if(FUSE_VULKAN_BACKEND)
            add_test(NAME ${_t} COMMAND "${_fuse_rp_wp06_lock}" "$<TARGET_FILE:${_t}>")
        else()
            add_test(NAME ${_t} COMMAND ${_t})
        endif()
        set_tests_properties(${_t} PROPERTIES
            ENVIRONMENT "${_fuse_rp_wp06_env}"
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77
            TIMEOUT 600
            LABELS "gate;vulkan;renderer;profiling")
    endforeach()
endif()
