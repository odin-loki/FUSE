# FUSE Relight RL-3.1: runs one fixture of Tests/relight/fixtures/mods (ctest rl_mod_fixture_<name>_{usda,usdc}).
#
#   cmake -DTOOL=<fuse_relight_usd_tool> [-DEMULATOR=<a|b|c>] -DFIXTURE=<fixture dir> -DFORMAT=usda|usdc
#         [-DOUT=<scratch dir>] -P rl_mod_fixture.cmake
#
# usda: `check <fixture>/mod.usda <fixture>/expect.txt`.
# usdc: generates the fixture's USDC twin with TinyUSDZ's crate writer (`usdc-fixture`: every .usd layer re-encoded,
#       mod.usda -> mod.usdc) into OUT, then `check OUT/mod.usdc <fixture>/expect.txt --same-as <fixture>/mod.usda`:
#       the same expectations, and a canonical dump equal to the USDA original's. Nothing binary is committed.
# EMULATOR ('|'-separated) runs a PE tool under Wine in MinGW trees.

foreach(_v TOOL FIXTURE FORMAT)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "rl_mod_fixture: pass -D${_v}=")
    endif()
endforeach()
set(_run "")
if(DEFINED EMULATOR AND NOT EMULATOR STREQUAL "")
    string(REPLACE "|" ";" _run "${EMULATOR}")
endif()
list(APPEND _run "${TOOL}")

if(FORMAT STREQUAL "usda")
    execute_process(COMMAND ${_run} check "${FIXTURE}/mod.usda" "${FIXTURE}/expect.txt" RESULT_VARIABLE _rc)
elseif(FORMAT STREQUAL "usdc")
    if(NOT DEFINED OUT)
        message(FATAL_ERROR "rl_mod_fixture: FORMAT=usdc needs -DOUT=")
    endif()
    file(REMOVE_RECURSE "${OUT}")
    execute_process(COMMAND ${_run} usdc-fixture "${FIXTURE}" "${OUT}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "rl_mod_fixture: usdc-fixture failed (${_rc})")
    endif()
    file(READ "${OUT}/mod.usdc" _magic LIMIT 8 HEX)
    if(NOT _magic STREQUAL "5058522d55534443") # "PXR-USDC"
        message(FATAL_ERROR "rl_mod_fixture: ${OUT}/mod.usdc is not a crate file")
    endif()
    execute_process(COMMAND ${_run} check "${OUT}/mod.usdc" "${FIXTURE}/expect.txt" --same-as "${FIXTURE}/mod.usda"
                    RESULT_VARIABLE _rc)
else()
    message(FATAL_ERROR "rl_mod_fixture: FORMAT must be usda or usdc")
endif()
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "rl_mod_fixture: ${FORMAT} check failed (${_rc})")
endif()
