# fuse_remaster_tscript: TorqueScript parser -> AST + call graph + datablock tree
# (docs/plans/FUSE_REMASTER_PLAN.md §1.2 Track B, W1.6). std-only static library.
# Added from Source/FUSE/Remaster/CMakeLists.txt, which provides _fuse_remaster_cxx() and FUSE_REMASTER_REPO_ROOT.
#
# Gates (ctest -L tscript):
#   remaster_tscript_unit       snippet tests: statements / expressions, error recovery, call graph, datablock tree,
#                               JSON determinism
#   remaster_tscript_templates  parses every Templates/**/*.{cs,tscript} (>= 500 files) with zero diagnostics

add_library(fuse_remaster_tscript STATIC
    src/lexer.cpp
    src/parser.cpp
    src/analysis.cpp
    src/json.cpp)
target_include_directories(fuse_remaster_tscript PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
_fuse_remaster_cxx(fuse_remaster_tscript)

add_executable(fuse_remaster_tscript_dump tools/tscript_dump.cpp)
target_link_libraries(fuse_remaster_tscript_dump PRIVATE fuse_remaster_tscript)
_fuse_remaster_cxx(fuse_remaster_tscript_dump)

if(FUSE_BUILD_CORE_TESTS OR BUILD_TESTING)
    add_executable(fuse_remaster_tscript_unit tests/tscript_unit.cpp)
    target_link_libraries(fuse_remaster_tscript_unit PRIVATE fuse_remaster_tscript)
    _fuse_remaster_cxx(fuse_remaster_tscript_unit)
    add_test(NAME remaster_tscript_unit COMMAND fuse_remaster_tscript_unit)
    set_tests_properties(remaster_tscript_unit PROPERTIES LABELS "remaster;tscript")

    add_executable(fuse_remaster_tscript_templates tests/tscript_templates.cpp)
    target_link_libraries(fuse_remaster_tscript_templates PRIVATE fuse_remaster_tscript)
    _fuse_remaster_cxx(fuse_remaster_tscript_templates)
    add_test(NAME remaster_tscript_templates
             COMMAND fuse_remaster_tscript_templates "${FUSE_REMASTER_REPO_ROOT}/Templates" 500)
    set_tests_properties(remaster_tscript_templates PROPERTIES LABELS "remaster;tscript" SKIP_RETURN_CODE 77)
endif()
