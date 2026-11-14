# FUSE Remaster W0.7 gate remaster_cli (plan §7 Wave 0, W0.7 exit criteria):
#   * `init` writes a project with cloud_upload "none", an empty canonical lock, ai-budget.json and the store dirs,
#     and refuses to overwrite it without --force;
#   * `doctor` reports "gpu: none" without CUDA (forced with FUSE_REMASTER_DOCTOR_NO_GPU=1) and, off Windows,
#     "fuse-remix: unavailable (Windows only)"; with --project it checks the lock and every blob;
#   * `report` counts the project; a corrupted blob or a non-canonical lock makes doctor fail.
# Usage: cmake -DEXE=<fuse_remaster> -DWORK=<scratch dir> [-DTARGET_WIN32=ON] [-DEMU=<emulator>] -P remaster_cli_gate.cmake
if(NOT EXE OR NOT WORK)
    message(FATAL_ERROR "remaster_cli_gate: pass -DEXE=... -DWORK=...")
endif()
# EMU (optional): CMAKE_CROSSCOMPILING_EMULATOR, '|'-joined, that runs the cross-compiled executables
# (fuse-wine-run.sh for MinGW); empty for a native build.
string(REPLACE "|" ";" _emu "${EMU}")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(_proj "${WORK}/proj")

function(_run expect_code out_var)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT "${_rc}" STREQUAL "${expect_code}")
        message(FATAL_ERROR "remaster_cli_gate: `${ARGN}` exited ${_rc}, expected ${expect_code}\n${_out}\n${_err}")
    endif()
    set(${out_var} "${_out}${_err}" PARENT_SCOPE)
endfunction()

function(_expect text needle)
    string(FIND "${text}" "${needle}" _at)
    if(_at LESS 0)
        message(FATAL_ERROR "remaster_cli_gate: expected '${needle}' in:\n${text}")
    endif()
endfunction()

# init
_run(0 _o ${_emu} "${EXE}" init "${_proj}" --game "Test Game" --name "Test")
foreach(_f remaster.project.json remaster.lock.json ai-budget.json .gitignore)
    if(NOT EXISTS "${_proj}/${_f}")
        message(FATAL_ERROR "remaster_cli_gate: init did not write ${_f}")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${_proj}/store/poco" OR NOT IS_DIRECTORY "${_proj}/store/blobs/sha256")
    message(FATAL_ERROR "remaster_cli_gate: init did not create the store")
endif()
file(READ "${_proj}/remaster.project.json" _project)
string(JSON _cloud GET "${_project}" cloud_upload)
string(JSON _game GET "${_project}" game_id)
if(NOT _cloud STREQUAL "none" OR NOT _game STREQUAL "test_game")
    message(FATAL_ERROR "remaster_cli_gate: project has cloud_upload '${_cloud}', game_id '${_game}'")
endif()
file(READ "${_proj}/remaster.lock.json" _lock)
string(JSON _schema GET "${_lock}" schema)
string(JSON _n LENGTH "${_lock}" original_asset)
if(NOT _schema STREQUAL "fuse.remaster.lock/1" OR NOT _n EQUAL 0)
    message(FATAL_ERROR "remaster_cli_gate: bad initial lock")
endif()
_run(2 _o ${_emu} "${EXE}" init "${_proj}" --game other)            # no overwrite
_run(0 _o ${_emu} "${EXE}" init "${_proj}" --game "Test Game" --name "Test" --force)
_run(2 _o ${_emu} "${EXE}" init)                                      # usage

# doctor (host)
_run(0 _o "${CMAKE_COMMAND}" -E env FUSE_REMASTER_DOCTOR_NO_GPU=1 ${_emu} "${EXE}" doctor)
_expect("${_o}" "gpu: none")
_expect("${_o}" "doctor: ok")
# TARGET_WIN32: the platform fuse_remaster was built for (a MinGW cross build is Windows on a Linux host).
if(TARGET_WIN32)
    _expect("${_o}" "fuse-remix: host supported (Windows)")
else()
    _expect("${_o}" "fuse-remix: unavailable (Windows only)")
endif()
_run(0 _o "${CMAKE_COMMAND}" -E env FUSE_REMASTER_DOCTOR_NO_GPU=1 ${_emu} "${EXE}" doctor --json)
string(JSON _gpu GET "${_o}" gpu)
if(NOT _gpu STREQUAL "none")
    message(FATAL_ERROR "remaster_cli_gate: doctor --json gpu '${_gpu}'")
endif()

# doctor / report with the project, then with a blob added and corrupted
_run(0 _o ${_emu} "${EXE}" doctor --project "${_proj}")
_expect("${_o}" "cloud_upload: none")
_run(0 _o ${_emu} "${EXE}" report --project "${_proj}" --json)
string(JSON _blobs GET "${_o}" blobs count)
if(NOT _blobs EQUAL 0)
    message(FATAL_ERROR "remaster_cli_gate: report counts ${_blobs} blobs in an empty store")
endif()
set(_sha "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824") # sha256("hello")
file(WRITE "${_proj}/store/blobs/sha256/2c/${_sha}" "hello")
_run(0 _o ${_emu} "${EXE}" doctor --project "${_proj}")
_expect("${_o}" "blobs: 1 (0 bad)")
file(WRITE "${_proj}/store/blobs/sha256/2c/${_sha}" "hellO")
_run(1 _o ${_emu} "${EXE}" doctor --project "${_proj}")
_expect("${_o}" "does not hash to its address")
file(WRITE "${_proj}/store/blobs/sha256/2c/${_sha}" "hello")
file(WRITE "${_proj}/remaster.lock.json" "{\"schema\": \"fuse.remaster.lock/1\"}")
_run(1 _o ${_emu} "${EXE}" doctor --project "${_proj}")
_expect("${_o}" "not in canonical form")

file(REMOVE_RECURSE "${WORK}")
message(STATUS "remaster_cli_gate: PASS")
