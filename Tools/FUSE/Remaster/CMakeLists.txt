# Tools/FUSE/Remaster: Python tooling of FUSE Remaster (docs/plans/FUSE_REMASTER_PLAN.md §3, §7 Wave 0).
# Added from Source/FUSE/Remaster/CMakeLists.txt when tests are enabled; FUSE_REMASTER_REPO_ROOT is the repo root.
#
# Gates (ctest -L remaster)
#   remaster_fuse_ai   W0.5 + W0.8: the fuse_ai package (stdlib unittest, pytest-compatible): replay miss fails,
#                      budget refusal, mock determinism (pinned hashes), record->replay, recipe hash, cloud opt-in
#                      guard + upload audit. CPU-only; the tests block the network (tests/__init__.py).
# Nothing is written into the source tree: bytecode is off (-B, PYTHONDONTWRITEBYTECODE) and the AI cache
# (FUSE_AI_CACHE) points into the binary dir; the tests themselves use temp dirs.

set(FUSE_REMASTER_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(_fuse_ai_work "${CMAKE_CURRENT_BINARY_DIR}/fuse_ai")
    file(MAKE_DIRECTORY "${_fuse_ai_work}")
    add_test(NAME remaster_fuse_ai
             COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
                     -s "${FUSE_REMASTER_TOOLS_DIR}/tests" -p "test_fuse_ai*.py" -t "${FUSE_REMASTER_TOOLS_DIR}" -v
             WORKING_DIRECTORY "${_fuse_ai_work}")
    set_tests_properties(remaster_fuse_ai PROPERTIES
        LABELS "remaster;fuse_ai;python"
        TIMEOUT 300
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;FUSE_AI_CACHE=${_fuse_ai_work}/ai-cache")
else()
    message(STATUS "FUSE Remaster: Python 3 not found; remaster_fuse_ai gate skipped")
endif()

# ---- other Tools/FUSE/Remaster gates (added by the parent) ----
#   remaster_poco_python   W0.1: fuse_ai/poco_types.py is up to date with Content/schemas/poco (poco_codegen.py
#                          --check) and round-trips records of every kind.
#   remaster_no_originals  W0.6: no original asset is tracked by git (paths, POCO records, the local original-hash
#                          index in build/remaster-cache/), .gitignore covers the caches; seeded self-test first.
#                          Exits 77 (skip) without git.
if(Python3_Interpreter_FOUND)
    add_test(NAME remaster_poco_python
             COMMAND "${Python3_EXECUTABLE}" -B "${FUSE_REMASTER_TOOLS_DIR}/poco_codegen.py" --check
                     --schemas "${FUSE_REMASTER_REPO_ROOT}/Content/schemas/poco")
    add_test(NAME remaster_no_originals
             COMMAND "${Python3_EXECUTABLE}" -B "${FUSE_REMASTER_TOOLS_DIR}/remaster_no_originals.py"
                     --repo "${FUSE_REMASTER_REPO_ROOT}" --self-test)
    set_tests_properties(remaster_poco_python PROPERTIES LABELS "remaster;poco;python" TIMEOUT 60
                         ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    set_tests_properties(remaster_no_originals PROPERTIES LABELS "remaster;gate;python" TIMEOUT 600
                         SKIP_RETURN_CODE 77 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()

# ---- Waves 3-5: CPU tools, the gemini / local-llm providers and the port flow -------------------------------
#   fuse_remaster_lua_parity   W4.3 harness: runs a ported Lua behaviour in the engine's Lua (vendor/lua 5.2.3)
#                              under ScriptVM's sandbox, replays an event trace, compares the event log.
#   remaster_lua_parity        the harness on the fixture port (must match) and on a broken port (must differ).
#   remaster_ai_textures       W3.1 classifiers (>= 90 % on the labelled synthetic set), W3.4 library matcher
#                              (top-3 >= 80 %), W3.5 rebuild within tolerance + fingerprint attack PSNR < 15 dB.
#   remaster_ai_gemini         W4.1 gemini REST contract tests (replayed hand-written HTTP fixtures, no network),
#                              W4.2 prompts + JSON schemas, W4.4 SynthID lock records, W4.5 cost report + budget
#                              preflight, W4.6 local-llm + routing of originals without the cloud opt-in.
#   remaster_ai_port           W4.3 TorqueScript (MIT template) -> tscript dump -> spec -> Lua in replay mode; the
#                              Lua runs through fuse_remaster_lua_parity when it is built (structural checks always).
#   remaster_ai_world          W5.2 collision vs render, W5.3 light calibration, W5.4 look LUT (.fuselook + .cube),
#                              W5.6 emissive -> proxy light.
if(NOT TARGET fuse_lua AND EXISTS "${FUSE_REMASTER_REPO_ROOT}/vendor/lua/CMakeLists.txt")
    # Same binary dir and guard as Source/FUSE/Script, which then reuses this target.
    add_subdirectory("${FUSE_REMASTER_REPO_ROOT}/vendor/lua" "${CMAKE_BINARY_DIR}/fuse_vendor/lua" EXCLUDE_FROM_ALL)
endif()
if(TARGET fuse_lua)
    add_executable(fuse_remaster_lua_parity "${FUSE_REMASTER_TOOLS_DIR}/lua_parity/lua_parity.cpp")
    target_link_libraries(fuse_remaster_lua_parity PRIVATE fuse_lua fuse_poco)
    _fuse_remaster_cxx(fuse_remaster_lua_parity)
    set(_fx "${FUSE_REMASTER_TOOLS_DIR}/tests/fixtures/port")
    add_test(NAME remaster_lua_parity
             COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:fuse_remaster_lua_parity> -DFX=${_fx}
                     -P "${FUSE_REMASTER_TOOLS_DIR}/lua_parity/lua_parity_gate.cmake")
    set_tests_properties(remaster_lua_parity PROPERTIES LABELS "remaster;port" TIMEOUT 60)
endif()

if(Python3_Interpreter_FOUND)
    set(_ai_env "PYTHONDONTWRITEBYTECODE=1;FUSE_AI_OFFLINE=1;FUSE_AI_CACHE=${_fuse_ai_work}/ai-cache")
    if(TARGET fuse_remaster_tscript_dump)
        list(APPEND _ai_env "FUSE_REMASTER_TSCRIPT_DUMP=$<TARGET_FILE:fuse_remaster_tscript_dump>")
    endif()
    if(TARGET fuse_remaster_lua_parity)
        list(APPEND _ai_env "FUSE_REMASTER_LUA_PARITY=$<TARGET_FILE:fuse_remaster_lua_parity>")
    endif()
    foreach(_area textures gemini port world)
        add_test(NAME remaster_ai_${_area}
                 COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
                         -s "${FUSE_REMASTER_TOOLS_DIR}/tests" -p "test_ai_${_area}.py" -t "${FUSE_REMASTER_TOOLS_DIR}" -v
                 WORKING_DIRECTORY "${_fuse_ai_work}")
        set_tests_properties(remaster_ai_${_area} PROPERTIES LABELS "remaster;fuse_ai;python;ai_${_area}"
                             TIMEOUT 600 ENVIRONMENT "${_ai_env}")
    endforeach()
endif()
