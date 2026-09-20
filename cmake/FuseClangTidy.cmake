# Optional Track A P7 clang-tidy gate for FUSE-owned seed files.
# Does not set CMAKE_CXX_CLANG_TIDY (too slow / breaks MSVC).
#
# fuse_clang_tidy runs: clang-tidy -p ${CMAKE_BINARY_DIR} <seed files>
# compile_commands.json is required for -p. Ninja/Makefile write it at
# configure when CMAKE_EXPORT_COMPILE_COMMANDS is ON (FUSE_ENABLE_CLANG_TIDY
# or the fuse-debug / fuse-asan presets). cmake --build is recommended
# before the target / ctest so the compilation database matches a built tree.

if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT EXISTS "${FUSE_COMPILE_COMMANDS}")
        message(STATUS "fuse_p7_clang_tidy: compile_commands.json missing — skip (enable CMAKE_EXPORT_COMPILE_COMMANDS, reconfigure, then cmake --build)")
        return()
    endif()
    if(NOT FUSE_CLANG_TIDY_EXE)
        message(STATUS "fuse_p7_clang_tidy: clang-tidy not found — skip")
        return()
    endif()

    set(_fuse_tidy_seed
        "${FUSE_SOURCE_DIR}/Source/FUSE/Core/include/fuse/handle.hpp"
        "${FUSE_SOURCE_DIR}/Source/FUSE/Core/include/fuse/types.hpp"
        "${FUSE_SOURCE_DIR}/Source/FUSE/Core/src/core/init.cpp"
        "${FUSE_SOURCE_DIR}/Source/FUSE/Core/src/alloc/allocator_detail.cpp"
        "${FUSE_SOURCE_DIR}/Source/FUSE/Scene/src/serialiser.cpp")

    execute_process(
        COMMAND "${FUSE_CLANG_TIDY_EXE}"
                -p "${FUSE_BINARY_DIR}"
                --extra-arg=-Wno-unknown-warning-option
                --extra-arg=-Wno-unused-command-line-argument
                ${_fuse_tidy_seed}
        WORKING_DIRECTORY "${FUSE_SOURCE_DIR}"
        RESULT_VARIABLE _fuse_tidy_result)
    if(NOT _fuse_tidy_result EQUAL 0)
        message(FATAL_ERROR "clang-tidy exited with ${_fuse_tidy_result}")
    endif()
    return()
endif()

find_program(CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-18 clang-tidy-17)

function(fuse_add_clang_tidy_target)
    if(TARGET fuse_clang_tidy)
        return()
    endif()

    set(_fuse_clang_tidy_files
        "${CMAKE_SOURCE_DIR}/Source/FUSE/Core/include/fuse/handle.hpp"
        "${CMAKE_SOURCE_DIR}/Source/FUSE/Core/include/fuse/types.hpp"
        "${CMAKE_SOURCE_DIR}/Source/FUSE/Core/src/core/init.cpp"
        "${CMAKE_SOURCE_DIR}/Source/FUSE/Core/src/alloc/allocator_detail.cpp"
        "${CMAKE_SOURCE_DIR}/Source/FUSE/Scene/src/serialiser.cpp")

    if(NOT CLANG_TIDY_EXE)
        add_custom_target(fuse_clang_tidy
            COMMAND ${CMAKE_COMMAND} -E echo "clang-tidy not found; skipping fuse_clang_tidy"
            COMMENT "clang-tidy not available — fuse_clang_tidy skipped")
        message(STATUS "FUSE: clang-tidy not found — fuse_clang_tidy will skip")
        return()
    endif()

    message(STATUS "FUSE: clang-tidy found (${CLANG_TIDY_EXE}) — fuse_clang_tidy target enabled")

    add_custom_target(fuse_clang_tidy
        COMMAND "${CLANG_TIDY_EXE}"
                -p "${CMAKE_BINARY_DIR}"
                --extra-arg=-Wno-unknown-warning-option
                --extra-arg=-Wno-unused-command-line-argument
                ${_fuse_clang_tidy_files}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "clang-tidy FUSE seed files (-p compile_commands.json; cmake --build first)"
        VERBATIM)

    if(FUSE_BUILD_CORE_TESTS)
        add_test(
            NAME fuse_p7_clang_tidy
            COMMAND ${CMAKE_COMMAND} -E env
                    ${CMAKE_COMMAND}
                    "-DFUSE_CLANG_TIDY_EXE=${CLANG_TIDY_EXE}"
                    "-DFUSE_COMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json"
                    "-DFUSE_BINARY_DIR=${CMAKE_BINARY_DIR}"
                    "-DFUSE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                    -P "${CMAKE_SOURCE_DIR}/cmake/FuseClangTidy.cmake"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()
endfunction()

fuse_add_clang_tidy_target()
