# FUSE_MASTER_PLAN B1.8: "`valgrind --leak-check=full` reports zero leaks on the test suite binary".
#
# fuse_add_valgrind_tests() registers a `valgrind.<test>` ctest twin for a representative set of
# existing test executables (fuse_core unit tests, ECS + scene tests, fuse_runtime_smoke). Each twin
# runs the binary under memcheck with --leak-check=full and fails (exit 1) on any memory error or
# any definitely/indirectly lost block. Third-party noise is silenced only through the checked-in,
# commented suppression file cmake/valgrind/fuse.supp. Twins run with FUSE_INSTRUMENTED_RUN=valgrind so
# wall-clock budget asserts are skipped (fuse::core::timingBudgetsEnforced() in fuse/core/sanitizer.hpp).
#
#   ctest --test-dir build/fuse-debug -L valgrind            # run the leak gate
#   ctest --test-dir build/fuse-debug -LE valgrind           # everything else, without valgrind
#
# Off automatically when valgrind is missing, on non-Linux hosts, or when a sanitizer build is
# active (ASan/TSan runtimes and memcheck cannot share a process).

option(FUSE_VALGRIND_TESTS "Register valgrind memcheck twins (label: valgrind) of representative tests" ON)
find_program(FUSE_VALGRIND_EXECUTABLE valgrind)

set(FUSE_VALGRIND_SUPPRESSIONS "${CMAKE_SOURCE_DIR}/cmake/valgrind/fuse.supp")
set(FUSE_VALGRIND_ARGS
    --tool=memcheck
    --leak-check=full
    --show-leak-kinds=definite,indirect
    --errors-for-leak-kinds=definite,indirect
    --error-exitcode=1
    --num-callers=40
    --keep-debuginfo=yes
    --track-fds=no
    "--suppressions=${FUSE_VALGRIND_SUPPRESSIONS}"
    --gen-suppressions=all)

function(fuse_valgrind_enabled out_var)
    set(_enabled OFF)
    if(FUSE_VALGRIND_TESTS AND FUSE_VALGRIND_EXECUTABLE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_enabled ON)
        if(DEFINED FUSE_SANITIZE AND NOT "${FUSE_SANITIZE}" STREQUAL "")
            set(_enabled OFF)
        endif()
        foreach(_opt FUSE_CORE_ENABLE_ASAN FUSE_CORE_ENABLE_TSAN FUSE_SMOKE_ENABLE_ASAN)
            if(${_opt})
                set(_enabled OFF)
            endif()
        endforeach()
    endif()
    set(${out_var} ${_enabled} PARENT_SCOPE)
endfunction()

# fuse_add_valgrind_test(<ctest name> <target> [WORKING_DIRECTORY dir] [TIMEOUT s] [ARGS ...])
function(fuse_add_valgrind_test name target)
    cmake_parse_arguments(_vg "" "WORKING_DIRECTORY;TIMEOUT" "ARGS" ${ARGN})
    if(NOT TARGET ${target})
        return()
    endif()
    if(NOT _vg_TIMEOUT)
        set(_vg_TIMEOUT 900)
    endif()
    if(NOT _vg_WORKING_DIRECTORY)
        set(_vg_WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>")
    endif()
    add_test(NAME valgrind.${name}
             COMMAND ${FUSE_VALGRIND_EXECUTABLE} ${FUSE_VALGRIND_ARGS} $<TARGET_FILE:${target}> ${_vg_ARGS}
             WORKING_DIRECTORY "${_vg_WORKING_DIRECTORY}")
    # FUSE_INSTRUMENTED_RUN tells tests (fuse::core::timingBudgetsEnforced()) to skip wall-clock
    # budgets: memcheck slows code ~50x, so only correctness + leak checks are meaningful here.
    set_tests_properties(valgrind.${name} PROPERTIES LABELS "valgrind" TIMEOUT ${_vg_TIMEOUT}
                         ENVIRONMENT "FUSE_INSTRUMENTED_RUN=valgrind")
endfunction()

function(fuse_add_valgrind_tests)
    fuse_valgrind_enabled(_on)
    if(NOT _on)
        message(STATUS "FUSE: valgrind leak tests disabled (valgrind='${FUSE_VALGRIND_EXECUTABLE}', FUSE_SANITIZE='${FUSE_SANITIZE}')")
        return()
    endif()
    # fuse_core: allocator, jobs/fibers, logging, profiler, platform input, phase-1 integration.
    foreach(_pair
            "core_worker_count:fuse_core_tests"
            "core_jobs:fuse_core_job_tests"
            "core_jobs_single_thread:fuse_core_job_single_thread_tests"
            "core_fiber:fuse_core_fiber_tests"
            "core_services:fuse_core_services_tests"
            "core_logger:fuse_core_logger_tests"
            "core_io_handle:fuse_core_io_handle_tests"
            "core_profiler_assert:fuse_core_profiler_assert_tests"
            "core_chrome_trace:fuse_core_chrome_trace_tests"
            "core_math:fuse_core_math_tests"
            "core_allocator:fuse_core_allocator_tests"
            "core_new_ban:fuse_core_new_ban"
            "core_input_state:fuse_core_input_state_tests"
            "core_phase1_integration:fuse_core_phase1_integration"
            "core_frame_barrier:fuse_core_frame_barrier_tests"
            # ECS / scene
            "ecs_registry:fuse_ecs_registry_tests"
            "ecs_systems:fuse_ecs_systems"
            "ecs_transform_system:fuse_ecs_transform_system"
            "scene_hierarchy:fuse_scene_tests"
            "scene_manager:fuse_scene_manager_tests"
            "scene_b3_gates:fuse_b3_scene_gates"
            "ecs_phase3_integration:fuse_ecs_phase3_integration_tests")
        string(REPLACE ":" ";" _parts "${_pair}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _target)
        fuse_add_valgrind_test(${_name} ${_target})
    endforeach()
    # Whole-runtime smoke: core + both legacy dimensions (+ RendererBootstrap init/shutdown).
    fuse_add_valgrind_test(runtime_smoke fuse_runtime_smoke TIMEOUT 1200)
    if(TEST valgrind.runtime_smoke AND DEFINED FUSE_VULKAN_TEST_ENV)
        set_tests_properties(valgrind.runtime_smoke PROPERTIES RUN_SERIAL TRUE)
    endif()
endfunction()
