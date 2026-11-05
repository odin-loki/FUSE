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
