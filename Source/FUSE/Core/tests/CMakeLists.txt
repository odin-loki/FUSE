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

# Always compile the single-thread job tests in default (multi-thread) CI.
# fuse_core stays multi-thread; this executable alone sees FUSE_JOBS_SINGLE_THREAD=1
# because test_jobs_single_thread.cpp requires the macro at compile time.
add_executable(fuse_core_job_single_thread_tests
    test_jobs_single_thread.cpp
)

target_compile_definitions(fuse_core_job_single_thread_tests PRIVATE FUSE_JOBS_SINGLE_THREAD=1)
target_link_libraries(fuse_core_job_single_thread_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_job_single_thread_tests)
add_test(NAME fuse_core_jobs_single_thread COMMAND fuse_core_job_single_thread_tests)

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

add_executable(fuse_core_logger_tests
    test_logger.cpp
)

target_link_libraries(fuse_core_logger_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_logger_tests)
add_test(NAME fuse_core_logger COMMAND fuse_core_logger_tests)

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

add_executable(fuse_core_u3_gate_tests
    test_u3_gate.cpp
    ${CMAKE_SOURCE_DIR}/Source/FUSE/Legacy/T3D/src/console_shim.cpp
    ${CMAKE_SOURCE_DIR}/Source/FUSE/Legacy/T2D/src/console_shim.cpp
)

target_include_directories(fuse_core_u3_gate_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Source/FUSE/Legacy/T3D/include
    ${CMAKE_SOURCE_DIR}/Source/FUSE/Legacy/T2D/include
)
target_link_libraries(fuse_core_u3_gate_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_u3_gate_tests)
add_test(NAME fuse_core_u3_gate COMMAND fuse_core_u3_gate_tests)

add_executable(fuse_core_profiler_assert_tests
    test_profiler_assert.cpp
)

target_link_libraries(fuse_core_profiler_assert_tests PRIVATE fuse_core)
add_test(NAME fuse_core_profiler_assert COMMAND fuse_core_profiler_assert_tests)

add_executable(fuse_core_chrome_trace_tests
    test_chrome_trace.cpp
)

target_link_libraries(fuse_core_chrome_trace_tests PRIVATE fuse_core)
add_test(NAME fuse_core_chrome_trace COMMAND fuse_core_chrome_trace_tests)

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

add_executable(fuse_core_new_ban
    test_new_ban.cpp
)

target_link_libraries(fuse_core_new_ban PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_new_ban)
add_test(NAME fuse_core_new_ban COMMAND fuse_core_new_ban)

add_executable(fuse_core_platform_window_tests
    test_platform_window.cpp
)

target_link_libraries(fuse_core_platform_window_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_platform_window_tests)
add_test(NAME fuse_core_platform_window COMMAND fuse_core_platform_window_tests)

add_executable(fuse_core_p7_input_smoke
    test_p7_input_smoke.cpp
)

target_link_libraries(fuse_core_p7_input_smoke PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_p7_input_smoke)
add_test(NAME fuse_core_p7_input_smoke COMMAND fuse_core_p7_input_smoke)

add_executable(fuse_core_input_state_tests
    test_input_state.cpp
)

target_link_libraries(fuse_core_input_state_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_input_state_tests)
add_test(NAME fuse_core_input_state_tests COMMAND fuse_core_input_state_tests)

add_executable(fuse_core_phase1_integration
    test_phase1_integration.cpp
)

target_link_libraries(fuse_core_phase1_integration PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_phase1_integration)
add_test(NAME fuse_core_phase1_integration COMMAND fuse_core_phase1_integration)

add_executable(fuse_core_frame_barrier_tests
    test_frame_barrier.cpp
)

target_link_libraries(fuse_core_frame_barrier_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_frame_barrier_tests)
add_test(NAME fuse_core_frame_barrier COMMAND fuse_core_frame_barrier_tests)

add_executable(fuse_core_p7_track_b_unlock
    test_p7_track_b_unlock.cpp
)

target_link_libraries(fuse_core_p7_track_b_unlock PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_p7_track_b_unlock)
add_test(NAME fuse_core_p7_track_b_unlock COMMAND fuse_core_p7_track_b_unlock)

add_executable(fuse_mobile_vulkan_stub_tests
    test_mobile_vulkan_stub.cpp
)

target_link_libraries(fuse_mobile_vulkan_stub_tests PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_mobile_vulkan_stub_tests)
add_test(NAME fuse_mobile_vulkan_stub COMMAND fuse_mobile_vulkan_stub_tests)

# P6 shipping Qt ban — linked only to fuse_core (never fuse_editor). Always
# built with core tests so default product graphs catch FUSE_HAS_QT leakage.
add_executable(fuse_p6_shipping_qt_ban
    ${CMAKE_SOURCE_DIR}/Source/FUSE/Apps/RuntimeSmoke/test_p6_shipping_qt_ban.cpp
)
target_link_libraries(fuse_p6_shipping_qt_ban PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_p6_shipping_qt_ban)
add_test(NAME fuse_p6_shipping_qt_ban COMMAND fuse_p6_shipping_qt_ban)

add_executable(fuse_core_b17_win32_window_input
    test_b17_win32_window_input.cpp
)

target_link_libraries(fuse_core_b17_win32_window_input PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_b17_win32_window_input)
add_test(NAME fuse_core_b17_win32_window_input COMMAND fuse_core_b17_win32_window_input)

# B1 gate suites (FUSE_MASTER_PLAN B1.8 carry-forward checklist).
add_executable(fuse_core_b1_jobs_gates
    test_b1_jobs_gates.cpp
)
target_link_libraries(fuse_core_b1_jobs_gates PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_b1_jobs_gates)
add_test(NAME fuse_core_b1_jobs_gates COMMAND fuse_core_b1_jobs_gates)
set_tests_properties(fuse_core_b1_jobs_gates PROPERTIES RUN_SERIAL TRUE LABELS "gate" TIMEOUT 900)

add_executable(fuse_core_b1_memory_gates
    test_b1_memory_gates.cpp
)
target_link_libraries(fuse_core_b1_memory_gates PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_b1_memory_gates)
add_test(NAME fuse_core_b1_memory_gates COMMAND fuse_core_b1_memory_gates)
set_tests_properties(fuse_core_b1_memory_gates PROPERTIES RUN_SERIAL TRUE LABELS "gate" TIMEOUT 600)

add_executable(fuse_core_b1_math_gates
    test_b1_math_gates.cpp
)
target_link_libraries(fuse_core_b1_math_gates PRIVATE fuse_core)
add_test(NAME fuse_core_b1_math_gates COMMAND fuse_core_b1_math_gates)
set_tests_properties(fuse_core_b1_math_gates PROPERTIES LABELS "gate" TIMEOUT 600)

# Logging/assert/profiler gates include the profiler scope-overhead timing budget.
add_executable(fuse_core_b1_logging_gates
    test_b1_logging_gates.cpp
)
target_link_libraries(fuse_core_b1_logging_gates PRIVATE fuse_core)
fuse_core_apply_tsan(fuse_core_b1_logging_gates)
add_test(NAME fuse_core_b1_logging_gates COMMAND fuse_core_b1_logging_gates)
set_tests_properties(fuse_core_b1_logging_gates PROPERTIES RUN_SERIAL TRUE LABELS "perf;gate" TIMEOUT 600)

add_executable(fuse_core_b1_platform_gates
    test_b1_platform_gates.cpp
)
target_link_libraries(fuse_core_b1_platform_gates PRIVATE fuse_core)
add_test(NAME fuse_core_b1_platform_gates COMMAND fuse_core_b1_platform_gates)
set_tests_properties(fuse_core_b1_platform_gates PROPERTIES LABELS "gate")

# FUSE_ASSERT codegen: Debug objects must call fuse::assertion::fatal, other configs must not.
add_library(fuse_core_b1_assert_probe OBJECT b1_assert_probe.cpp)
target_link_libraries(fuse_core_b1_assert_probe PRIVATE fuse_core)
if(CMAKE_NM AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_test(NAME fuse_core_b1_assert_codegen
        COMMAND ${CMAKE_COMMAND}
            -DNM=${CMAKE_NM}
            "-DOBJECTS=$<TARGET_OBJECTS:fuse_core_b1_assert_probe>"
            -DEXPECT=$<IF:$<CONFIG:Debug>,present,absent>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_b1_assert_codegen.cmake)
    set_tests_properties(fuse_core_b1_assert_codegen PROPERTIES LABELS "gate")
endif()

get_property(_fuse_core_dir_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_fuse_core_test IN LISTS _fuse_core_dir_targets)
    if(_fuse_core_test MATCHES "^fuse_core_" OR _fuse_core_test STREQUAL "fuse_p6_shipping_qt_ban")
        fuse_core_apply_asan_ubsan(${_fuse_core_test})
    endif()
endforeach()

add_executable(fuse_core_window_wsi_win32_tests
    test_window_wsi_win32.cpp
)
target_link_libraries(fuse_core_window_wsi_win32_tests PRIVATE fuse_core)
if(TARGET Vulkan::Vulkan)
    target_link_libraries(fuse_core_window_wsi_win32_tests PRIVATE Vulkan::Vulkan)
endif()
fuse_core_apply_tsan(fuse_core_window_wsi_win32_tests)
fuse_core_apply_asan_ubsan(fuse_core_window_wsi_win32_tests)
add_test(NAME fuse_core_window_wsi_win32 COMMAND fuse_core_window_wsi_win32_tests)
