# Lavapipe test concurrency.
#
# Every Vulkan gate that runs on Lavapipe (ENVIRONMENT pins lvp_icd.json) is RUN_SERIAL and its launcher takes one
# global flock, so ctest runs them one at a time: in the Linux ASan job that is ~355 tests and ~72% of the wall time,
# with 3 of the runner's 4 cores idle while the (single-threaded) host side of each gate runs.
#
#   -DFUSE_VULKAN_TEST_SLOTS=N   (N > 0) turns those tests from RUN_SERIAL into RESOURCE_GROUPS "lavapipe:1" and writes
#                                ${CMAKE_BINARY_DIR}/fuse_ctest_resources.json with N lavapipe slots. Each such test gets
#                                LP_NUM_THREADS = max(1, nproc / N) so N concurrent llvmpipe instances do not
#                                oversubscribe the machine.
#   ctest ... --resource-spec-file <build>/fuse_ctest_resources.json
#                                is what bounds the concurrency: ctest then sets CTEST_RESOURCE_GROUP_COUNT and the
#                                launcher (Renderer/tests run_vulkan_icd_locked.sh) skips its flock. Without the spec
#                                file the tests fall back to the flock (one at a time), but no longer RUN_SERIAL.
#   0 (default)                  unchanged: RUN_SERIAL + flock.
#
# Tests labelled perf / hw / bench keep RUN_SERIAL (wall-clock budgets).
set(FUSE_VULKAN_TEST_SLOTS "0" CACHE STRING
    "Lavapipe gates that may run at once under ctest --resource-spec-file <build>/fuse_ctest_resources.json (0 = RUN_SERIAL)")

function(_fuse_vk_slots_collect dir out_dirs)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    set(_all "${dir}")
    foreach(_sd IN LISTS _subdirs)
        _fuse_vk_slots_collect("${_sd}" _d)
        list(APPEND _all ${_d})
    endforeach()
    set(${out_dirs} ${_all} PARENT_SCOPE)
endfunction()

# Call once at the end of the top-level CMakeLists.txt, before fuse_sanitize_finalize (which appends its own
# VK_ICD_FILENAMES to every test and would make every test look like a Lavapipe one).
function(fuse_vulkan_slots_finalize)
    if(NOT FUSE_VULKAN_TEST_SLOTS GREATER 0 OR WIN32)
        return()
    endif()
    cmake_host_system_information(RESULT _cores QUERY NUMBER_OF_LOGICAL_CORES)
    math(EXPR _lp "${_cores} / ${FUSE_VULKAN_TEST_SLOTS}")
    if(_lp LESS 1)
        set(_lp 1)
    endif()
    file(WRITE "${CMAKE_BINARY_DIR}/fuse_ctest_resources.json"
        "{\"version\": {\"major\": 1, \"minor\": 0}, \"local\": [{\"lavapipe\": [{\"id\": \"0\", \"slots\": ${FUSE_VULKAN_TEST_SLOTS}}]}]}\n")
    _fuse_vk_slots_collect("${CMAKE_SOURCE_DIR}" _dirs)
    set(_count 0)
    foreach(_d IN LISTS _dirs)
        get_property(_tests DIRECTORY "${_d}" PROPERTY TESTS)
        foreach(_t IN LISTS _tests)
            get_property(_serial TEST "${_t}" DIRECTORY "${_d}" PROPERTY RUN_SERIAL)
            if(NOT _serial)
                continue()
            endif()
            get_property(_env TEST "${_t}" DIRECTORY "${_d}" PROPERTY ENVIRONMENT)
            if(NOT "${_env}" MATCHES "lvp_icd\\.json")
                continue()
            endif()
            get_property(_labels TEST "${_t}" DIRECTORY "${_d}" PROPERTY LABELS)
            if("${_labels}" MATCHES "(^|;)(perf|hw|bench)(;|$)")
                continue()
            endif()
            set_property(TEST "${_t}" DIRECTORY "${_d}" PROPERTY RUN_SERIAL FALSE)
            set_property(TEST "${_t}" DIRECTORY "${_d}" PROPERTY RESOURCE_GROUPS "lavapipe:1")
            set_property(TEST "${_t}" DIRECTORY "${_d}" APPEND PROPERTY ENVIRONMENT "LP_NUM_THREADS=${_lp}")
            math(EXPR _count "${_count} + 1")
        endforeach()
    endforeach()
    message(STATUS "FUSE: ${_count} Lavapipe tests share ${FUSE_VULKAN_TEST_SLOTS} lavapipe slots "
                   "(LP_NUM_THREADS=${_lp}; ctest --resource-spec-file ${CMAKE_BINARY_DIR}/fuse_ctest_resources.json)")
endfunction()
