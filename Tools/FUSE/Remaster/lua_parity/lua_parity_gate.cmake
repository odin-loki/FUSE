# remaster_lua_parity (W4.3): the fixture Lua port must reproduce the expected event log of the original
# TorqueScript behaviour; a deliberately broken port (wrong multiplier) and a port that uses `io` must not.
# -DEXE=<fuse_remaster_lua_parity> -DFX=<Tools/FUSE/Remaster/tests/fixtures/port>
# EMU (optional): CMAKE_CROSSCOMPILING_EMULATOR, '|'-joined, that runs the cross-compiled executables
# (fuse-wine-run.sh for MinGW); empty for a native build.
string(REPLACE "|" ";" _emu "${EMU}")
execute_process(COMMAND ${_emu} "${EXE}" "${FX}/shapebase_impact.port.lua" "${FX}/shapebase_impact.trace.json"
                        --expect "${FX}/shapebase_impact.expected.log"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "fixture port does not match the expected log (rc ${rc}):\n${out}\n${err}")
endif()
execute_process(COMMAND ${_emu} "${EXE}" "${FX}/shapebase_impact.broken.lua" "${FX}/shapebase_impact.trace.json"
                        --expect "${FX}/shapebase_impact.expected.log"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0)
    message(FATAL_ERROR "the broken port passed the parity check:\n${out}")
endif()
execute_process(COMMAND ${_emu} "${EXE}" "${FX}/sandbox_io.lua" "${FX}/shapebase_impact.trace.json"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 3 OR NOT out MATCHES "error")
    message(FATAL_ERROR "a port using io was not stopped by the sandbox (rc ${rc}):\n${out}")
endif()
message(STATUS "lua parity: fixture port matches; broken port and sandbox escape rejected")
