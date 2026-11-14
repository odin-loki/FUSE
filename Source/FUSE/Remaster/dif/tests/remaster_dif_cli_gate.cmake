# FUSE Remaster W1.5 gate remaster_dif_cli: `fuse_remaster dif <file>` on the synthetic interior written by
# fuse_legacy_dif (remaster_dif_tests --write-fixture, with 3 dummy lightmaps): 3 brushes, 36 triangles, area 307,
# lightmaps skipped; an empty file is refused with exit 1.
# Usage: cmake -DFIXTURE_EXE=<remaster_dif_tests> -DEXE=<fuse_remaster> -DWORK=<dir> -P remaster_dif_cli_gate.cmake
if(NOT FIXTURE_EXE OR NOT EXE OR NOT WORK)
    message(FATAL_ERROR "remaster_dif_cli_gate: pass -DFIXTURE_EXE=... -DEXE=... -DWORK=...")
endif()
# EMU (optional): CMAKE_CROSSCOMPILING_EMULATOR, '|'-joined, that runs the cross-compiled executables
# (fuse-wine-run.sh for MinGW); empty for a native build.
string(REPLACE "|" ";" _emu "${EMU}")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(_dif "${WORK}/fixture.dif")
execute_process(COMMAND ${_emu} "${FIXTURE_EXE}" --write-fixture "${_dif}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0 OR NOT EXISTS "${_dif}")
    message(FATAL_ERROR "remaster_dif_cli_gate: fixture generation failed (${_rc})")
endif()
execute_process(COMMAND ${_emu} "${EXE}" dif "${_dif}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "remaster_dif_cli_gate: dif exited ${_rc}\n${_out}\n${_err}")
endif()
string(JSON _schema GET "${_out}" schema)
string(JSON _brushes GET "${_out}" detail_levels 0 brushes)
string(JSON _tris GET "${_out}" detail_levels 0 triangles)
string(FIND "${_out}" "\"area\": 307.000000," _area)
string(JSON _lm GET "${_out}" detail_levels 0 lightmaps_skipped)
if(NOT _schema STREQUAL "fuse.remaster.dif-summary/1" OR NOT _brushes EQUAL 3 OR NOT _tris EQUAL 36 OR
   _area LESS 0 OR NOT _lm EQUAL 3)
    message(FATAL_ERROR "remaster_dif_cli_gate: unexpected summary\n${_out}")
endif()
# An empty file is refused.
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo_append "" OUTPUT_FILE "${WORK}/empty.dif")
execute_process(COMMAND ${_emu} "${EXE}" dif "${WORK}/empty.dif" RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
if(NOT _rc EQUAL 1)
    message(FATAL_ERROR "remaster_dif_cli_gate: an empty .dif should exit 1 (got ${_rc}: ${_err})")
endif()
message(STATUS "remaster_dif_cli: 3 brushes, 36 triangles, area 307, 3 lightmaps skipped; empty file refused")
