# FUSE Relight RL-3.2: runs one fixture of Tests/relight/fixtures/mods_import (ctest rl_mods_import_fixture_<name>).
#
#   cmake -DIMPORT=<fuse_relight_import> -DSTAGER=<fuse_relight_mods_import_tests> [-DEMULATOR=<a|b|c>]
#         -DFIXTURE=<fixture dir> -DOUT=<scratch dir> [-DUPDATE=ON] -P rl_mods_import_fixture.cmake
#
#   1. `STAGER stage FIXTURE OUT/mod`: copies the fixture (without expected/, args.txt, gen_textures.txt) and
#      generates the DDS textures and .pkg packages gen_textures.txt lists (nothing binary is committed);
#   2. `IMPORT OUT/mod --game <game> --out OUT/a --no-merge --verify [args.txt]`, then the same into OUT/b;
#   3. OUT/a and OUT/b must be byte-identical (records, blobs, DB): the import is deterministic;
#   4. the records and the DB (OUT/a/poco/**, OUT/a/db/**) must equal FIXTURE/expected/** file for file.
#      -DUPDATE=ON rewrites FIXTURE/expected instead (review the diff before committing).
# OUT is removed when the test passes. args.txt: one CLI argument per line (default game: fixture_game).
# EMULATOR ('|'-separated) runs the PE tools under Wine in MinGW trees.

foreach(_v IMPORT STAGER FIXTURE OUT)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "rl_mods_import_fixture: pass -D${_v}=")
    endif()
endforeach()
set(_emu "")
if(DEFINED EMULATOR AND NOT EMULATOR STREQUAL "")
    string(REPLACE "|" ";" _emu "${EMULATOR}")
endif()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
execute_process(COMMAND ${_emu} "${STAGER}" stage "${FIXTURE}" "${OUT}/mod" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "rl_mods_import_fixture: staging failed (${_rc})")
endif()

set(_args "")
set(_game "fixture_game")
if(EXISTS "${FIXTURE}/args.txt")
    file(STRINGS "${FIXTURE}/args.txt" _lines)
    set(_take_game FALSE)
    foreach(_l IN LISTS _lines)
        if(_l STREQUAL "")
            continue()
        endif()
        if(_take_game)
            set(_game "${_l}")
            set(_take_game FALSE)
        elseif(_l STREQUAL "--game")
            set(_take_game TRUE)
        else()
            list(APPEND _args "${_l}")
        endif()
    endforeach()
endif()

foreach(_run a b)
    execute_process(COMMAND ${_emu} "${IMPORT}" "${OUT}/mod" --game "${_game}" --out "${OUT}/${_run}" --no-merge --verify ${_args}
                    RESULT_VARIABLE _rc OUTPUT_VARIABLE _log ERROR_VARIABLE _err)
    file(WRITE "${OUT}/${_run}.log" "${_log}${_err}")
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "rl_mods_import_fixture: import ${_run} failed (${_rc}):\n${_log}${_err}")
    endif()
endforeach()
string(REGEX MATCH "^[^\n]*" _summary "${_log}")
message(STATUS "${_summary}")

# 3. Determinism.
file(GLOB_RECURSE _fa RELATIVE "${OUT}/a" "${OUT}/a/*")
file(GLOB_RECURSE _fb RELATIVE "${OUT}/b" "${OUT}/b/*")
list(SORT _fa)
list(SORT _fb)
if(NOT _fa STREQUAL _fb)
    message(FATAL_ERROR "rl_mods_import_fixture: the two imports wrote different file sets")
endif()
foreach(_f IN LISTS _fa)
    file(SHA256 "${OUT}/a/${_f}" _ha)
    file(SHA256 "${OUT}/b/${_f}" _hb)
    if(NOT _ha STREQUAL _hb)
        message(FATAL_ERROR "rl_mods_import_fixture: ${_f} differs between two imports")
    endif()
endforeach()

# 4. Expected records + DB.
set(_actual "")
foreach(_f IN LISTS _fa)
    if(_f MATCHES "^(poco|db)/")
        list(APPEND _actual "${_f}")
    endif()
endforeach()
if(UPDATE)
    file(REMOVE_RECURSE "${FIXTURE}/expected")
    foreach(_f IN LISTS _actual)
        get_filename_component(_d "${FIXTURE}/expected/${_f}" DIRECTORY)
        file(MAKE_DIRECTORY "${_d}")
        file(COPY_FILE "${OUT}/a/${_f}" "${FIXTURE}/expected/${_f}")
    endforeach()
    message(STATUS "rl_mods_import_fixture: rewrote ${FIXTURE}/expected")
else()
    file(GLOB_RECURSE _expected RELATIVE "${FIXTURE}/expected" "${FIXTURE}/expected/*")
    list(SORT _expected)
    if(NOT _expected STREQUAL _actual)
        message(FATAL_ERROR "rl_mods_import_fixture: record set differs from expected/\n  expected: ${_expected}\n  actual:   ${_actual}")
    endif()
    foreach(_f IN LISTS _actual)
        file(SHA256 "${OUT}/a/${_f}" _ha)
        file(SHA256 "${FIXTURE}/expected/${_f}" _he)
        if(NOT _ha STREQUAL _he)
            message(FATAL_ERROR "rl_mods_import_fixture: ${_f} differs from expected/ (diff ${OUT}/a/${_f} ${FIXTURE}/expected/${_f})")
        endif()
    endforeach()
endif()
file(REMOVE_RECURSE "${OUT}")
