# Runs a CUDA device gate under compute-sanitizer (memcheck + leak check) and fails on any reported
# error. Skips (prints "FUSE_SANITIZER_SKIP:", matched by SKIP_REGULAR_EXPRESSION) when there is no CUDA
# device: the gate is first run with --probe, which exits 77 without a device.
# Inputs: FUSE_SANITIZER (compute-sanitizer path), FUSE_GATE_EXE (gate executable),
#         FUSE_SANITIZER_TOOL (default memcheck).
if(NOT FUSE_GATE_EXE)
    message(FATAL_ERROR "FUSE_GATE_EXE not set")
endif()
if(NOT FUSE_SANITIZER_TOOL)
    set(FUSE_SANITIZER_TOOL memcheck)
endif()

execute_process(COMMAND "${FUSE_GATE_EXE}" --probe RESULT_VARIABLE _probe OUTPUT_VARIABLE _probe_out
                ERROR_VARIABLE _probe_out)
if(_probe EQUAL 77)
    message(STATUS "FUSE_SANITIZER_SKIP: no CUDA device (${_probe_out})")
    return()
endif()
if(NOT _probe EQUAL 0)
    message(FATAL_ERROR "device probe failed (${_probe}): ${_probe_out}")
endif()
if(NOT FUSE_SANITIZER OR NOT EXISTS "${FUSE_SANITIZER}")
    message(FATAL_ERROR "compute-sanitizer not found (FUSE_COMPUTE_SANITIZER='${FUSE_SANITIZER}') but a CUDA device is "
                        "present: install it with the CUDA toolkit or pass -DFUSE_COMPUTE_SANITIZER=<path>")
endif()

set(_args --tool ${FUSE_SANITIZER_TOOL} --error-exitcode 99)
if(FUSE_SANITIZER_TOOL STREQUAL "memcheck")
    list(APPEND _args --leak-check full)
endif()
# Wall-clock budgets are meaningless under instrumentation (fuse/core/sanitizer.hpp).
set(ENV{FUSE_INSTRUMENTED_RUN} compute-sanitizer)
execute_process(COMMAND "${FUSE_SANITIZER}" ${_args} "${FUSE_GATE_EXE}"
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
message("${_out}${_err}")
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "compute-sanitizer ${FUSE_SANITIZER_TOOL} failed (exit ${_rc}) for ${FUSE_GATE_EXE}")
endif()
if(NOT "${_out}${_err}" MATCHES "ERROR SUMMARY: 0 errors")
    message(FATAL_ERROR "compute-sanitizer did not report 'ERROR SUMMARY: 0 errors'")
endif()
message(STATUS "compute-sanitizer ${FUSE_SANITIZER_TOOL}: 0 errors")
