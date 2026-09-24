# WP-0.8 — pure-logic cores for PRISM (Source/FUSE/Renderer/core_logic/**).
# Owned by one work stream; other packages must not edit this file.
#
#   fuse_core_logic                 STATIC: RG compiler model (+ header-only VSM page table and
#                                   cluster residency LRU). No Vulkan, no STL, no heap.
#   fuse_core_logic_tests           unit + property tests (ctest: fuse_core_logic_{rg,vsm,residency})
#   fuse_core_logic_no_vulkan       lint: core_logic sources include only <stdint.h>/<stddef.h>/own headers
#   fuse_core_logic_bmc_native_*    BMC harnesses run natively with a PRNG (smoke)
#   fuse_core_logic_cbmc_*          CBMC runs of the harnesses (only when `cbmc` is found; label bmc)
#   fuse_core_logic_tests_san       ASan+UBSan build of the tests (GNU/Clang, when FUSE_SANITIZE is
#                                   empty; with the fuse-asan preset every target is instrumented)
#   fuse_core_logic_coverage        custom target: gcov branch report (GNU only; see tools/)
#
# Consumers (WP-0.3 render graph v2, 3.1 VSM, 5.3 streaming): link fuse_core_logic.

set(_fuse_cl_dir "${CMAKE_CURRENT_LIST_DIR}/../core_logic")

add_library(fuse_core_logic STATIC
    ${_fuse_cl_dir}/src/rg_model.cpp
    ${_fuse_cl_dir}/include/fuse/core_logic/cl_common.hpp
    ${_fuse_cl_dir}/include/fuse/core_logic/rg_model.hpp
    ${_fuse_cl_dir}/include/fuse/core_logic/vsm_page_table.hpp
    ${_fuse_cl_dir}/include/fuse/core_logic/residency_lru.hpp
)
target_include_directories(fuse_core_logic PUBLIC ${_fuse_cl_dir}/include)
target_compile_features(fuse_core_logic PUBLIC cxx_std_11)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(fuse_core_logic PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion)
endif()

if(FUSE_BUILD_CORE_TESTS)
    set(_fuse_cl_test_sources
        ${_fuse_cl_dir}/tests/test_core_logic_main.cpp
        ${_fuse_cl_dir}/tests/test_core_logic_rg.cpp
        ${_fuse_cl_dir}/tests/test_core_logic_vsm.cpp
        ${_fuse_cl_dir}/tests/test_core_logic_residency.cpp)

    add_executable(fuse_core_logic_tests ${_fuse_cl_test_sources})
    target_include_directories(fuse_core_logic_tests PRIVATE ${_fuse_cl_dir}/tests)
    target_link_libraries(fuse_core_logic_tests PRIVATE fuse_core_logic)
    foreach(_suite rg vsm residency)
        add_test(NAME fuse_core_logic_${_suite} COMMAND fuse_core_logic_tests ${_suite})
        set_tests_properties(fuse_core_logic_${_suite} PROPERTIES LABELS "gate;core_logic;prism" TIMEOUT 300)
    endforeach()

    # Lint: the cores must stay free of Vulkan / STL / platform headers so BMC tools take them.
    add_test(NAME fuse_core_logic_no_vulkan
        COMMAND ${CMAKE_COMMAND} -DCL_DIR=${_fuse_cl_dir} -P ${_fuse_cl_dir}/tools/check_includes.cmake)
    set_tests_properties(fuse_core_logic_no_vulkan PROPERTIES LABELS "gate;core_logic;prism;lint")

    # BMC harnesses, native smoke (PRNG-driven nondet, same assertions).
    foreach(_h vsm_page_table residency_lru rg_compiler)
        set(_srcs ${_fuse_cl_dir}/bmc/bmc_${_h}.cpp)
        if(_h STREQUAL "rg_compiler")
            # Small capacities; the harness TU and rg_model.cpp must agree (target-wide defines).
            list(APPEND _srcs ${_fuse_cl_dir}/src/rg_model.cpp)
        endif()
        add_executable(fuse_core_logic_bmc_${_h} ${_srcs})
        target_include_directories(fuse_core_logic_bmc_${_h} PRIVATE ${_fuse_cl_dir}/include ${_fuse_cl_dir}/tests)
        if(_h STREQUAL "rg_compiler")
            target_compile_definitions(fuse_core_logic_bmc_${_h} PRIVATE
                FUSE_CL_RG_MAX_PASSES=3u FUSE_CL_RG_MAX_RESOURCES=2u FUSE_CL_RG_MAX_ACCESSES_PER_PASS=2u)
        endif()
        add_test(NAME fuse_core_logic_bmc_native_${_h} COMMAND fuse_core_logic_bmc_${_h} 20000)
        set_tests_properties(fuse_core_logic_bmc_native_${_h} PROPERTIES LABELS "gate;core_logic;prism;bmc" TIMEOUT 300)
    endforeach()

    # CBMC (apt: cbmc 5.95). Bounds are small on purpose (measured here: VSM 3 steps ~1 min,
    # residency 2 steps ~1.3 min — 3 steps take ~18 min; RG 3 passes x 2 resources x 1 access
    # ~7.5 min — 2 accesses per pass did not finish in 50 min); the property tests cover long runs.
    # Registered DISABLED unless -DFUSE_CORE_LOGIC_CBMC=ON so a plain `ctest` stays fast; run with
    # `ctest -L bmc` after configuring with the option (or copy the command lines below).
    option(FUSE_CORE_LOGIC_CBMC "Enable the (slow) CBMC ctest runs of the core_logic harnesses" OFF)
    find_program(FUSE_CBMC_EXECUTABLE cbmc)
    if(FUSE_CBMC_EXECUTABLE)
        set(_cbmc_common --cpp11 -I${_fuse_cl_dir}/include -I${_fuse_cl_dir}/tests
            --unwinding-assertions --bounds-check --pointer-check --div-by-zero-check
            --signed-overflow-check --undefined-shift-check)
        add_test(NAME fuse_core_logic_cbmc_vsm_page_table
            COMMAND ${FUSE_CBMC_EXECUTABLE} ${_cbmc_common} -DBMC_STEPS=3 --unwind 10
                    ${_fuse_cl_dir}/bmc/bmc_vsm_page_table.cpp)
        add_test(NAME fuse_core_logic_cbmc_residency_lru
            COMMAND ${FUSE_CBMC_EXECUTABLE} ${_cbmc_common} -DBMC_STEPS=2 --unwind 7
                    ${_fuse_cl_dir}/bmc/bmc_residency_lru.cpp)
        add_test(NAME fuse_core_logic_cbmc_rg_compiler
            COMMAND ${FUSE_CBMC_EXECUTABLE} ${_cbmc_common} --unwind 8
                    -DFUSE_CL_RG_MAX_PASSES=3u -DFUSE_CL_RG_MAX_RESOURCES=2u -DFUSE_CL_RG_MAX_ACCESSES_PER_PASS=1u
                    ${_fuse_cl_dir}/bmc/bmc_rg_compiler.cpp ${_fuse_cl_dir}/src/rg_model.cpp)
        set_tests_properties(fuse_core_logic_cbmc_vsm_page_table fuse_core_logic_cbmc_residency_lru
            fuse_core_logic_cbmc_rg_compiler PROPERTIES LABELS "core_logic;prism;bmc;slow" TIMEOUT 1800)
        if(NOT FUSE_CORE_LOGIC_CBMC)
            set_tests_properties(fuse_core_logic_cbmc_vsm_page_table fuse_core_logic_cbmc_residency_lru
                fuse_core_logic_cbmc_rg_compiler PROPERTIES DISABLED TRUE)
        endif()
        message(STATUS "FUSE: core_logic CBMC harnesses registered (${FUSE_CBMC_EXECUTABLE}; FUSE_CORE_LOGIC_CBMC=${FUSE_CORE_LOGIC_CBMC})")
    else()
        message(STATUS "FUSE: cbmc not found — core_logic BMC harnesses run natively only")
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND "${FUSE_SANITIZE}" STREQUAL "")
        include(CheckCXXSourceCompiles)
        set(_cl_saved_flags "${CMAKE_REQUIRED_FLAGS}")
        set(_cl_saved_link "${CMAKE_REQUIRED_LINK_OPTIONS}")
        set(_cl_saved_quiet "${CMAKE_REQUIRED_QUIET}")
        set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
        set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address,undefined)
        set(CMAKE_REQUIRED_QUIET TRUE)
        check_cxx_source_compiles("int main() { return 0; }" FUSE_CORE_LOGIC_SAN_OK)
        set(CMAKE_REQUIRED_FLAGS "${_cl_saved_flags}")
        set(CMAKE_REQUIRED_LINK_OPTIONS "${_cl_saved_link}")
        set(CMAKE_REQUIRED_QUIET "${_cl_saved_quiet}")
        if(FUSE_CORE_LOGIC_SAN_OK)
            set(_san -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
            add_executable(fuse_core_logic_tests_san ${_fuse_cl_test_sources} ${_fuse_cl_dir}/src/rg_model.cpp)
            target_include_directories(fuse_core_logic_tests_san PRIVATE ${_fuse_cl_dir}/include ${_fuse_cl_dir}/tests)
            target_compile_options(fuse_core_logic_tests_san PRIVATE ${_san} -O1 -g)
            target_link_options(fuse_core_logic_tests_san PRIVATE ${_san})
            add_test(NAME fuse_core_logic_sanitize COMMAND fuse_core_logic_tests_san all)
            set_tests_properties(fuse_core_logic_sanitize PROPERTIES LABELS "gate;core_logic;prism;sanitize"
                TIMEOUT 600 ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1;UBSAN_OPTIONS=print_stacktrace=1")
        endif()
    endif()

    # Coverage (GNU gcov): `cmake --build <dir> --target fuse_core_logic_coverage` rebuilds the
    # tests with --coverage and prints merged line/branch coverage of include/ + src/.
    find_program(FUSE_GCOV_EXECUTABLE gcov)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND FUSE_GCOV_EXECUTABLE AND Python3_Interpreter_FOUND)
        add_executable(fuse_core_logic_tests_cov EXCLUDE_FROM_ALL ${_fuse_cl_test_sources} ${_fuse_cl_dir}/src/rg_model.cpp)
        target_include_directories(fuse_core_logic_tests_cov PRIVATE ${_fuse_cl_dir}/include ${_fuse_cl_dir}/tests)
        target_compile_options(fuse_core_logic_tests_cov PRIVATE --coverage -O0 -fno-exceptions -fno-inline)
        target_link_options(fuse_core_logic_tests_cov PRIVATE --coverage)
        add_custom_target(fuse_core_logic_coverage
            COMMAND ${Python3_EXECUTABLE} ${_fuse_cl_dir}/tools/coverage_report.py
                    --gcov ${FUSE_GCOV_EXECUTABLE}
                    --objdir ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/fuse_core_logic_tests_cov.dir
                    --root ${_fuse_cl_dir} --min-branch 100
                    --run $<TARGET_FILE:fuse_core_logic_tests_cov> all
            DEPENDS fuse_core_logic_tests_cov
            USES_TERMINAL
            COMMENT "core_logic gcov line/branch coverage (WP-0.8)")
    endif()
endif()
