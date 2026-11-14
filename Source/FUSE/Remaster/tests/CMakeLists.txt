# FUSE Remaster W0.1-W0.3 / W0.7 gates (docs/plans/FUSE_REMASTER_PLAN.md §7 Wave 0). ctest -L remaster.

add_executable(remaster_poco_tests test_poco.cpp)
target_link_libraries(remaster_poco_tests PRIVATE fuse_poco)
_fuse_remaster_cxx(remaster_poco_tests)

add_executable(remaster_store_tests test_store.cpp)
target_link_libraries(remaster_store_tests PRIVATE fuse_remaster_store)
target_include_directories(remaster_store_tests PRIVATE "${FUSE_REMASTER_REPO_ROOT}/vendor/stb")
_fuse_remaster_cxx(remaster_store_tests)

add_executable(remaster_db_tests test_db.cpp)
target_link_libraries(remaster_db_tests PRIVATE fuse_remaster_db fuse_remaster_store)
_fuse_remaster_cxx(remaster_db_tests)

add_test(NAME remaster_poco_roundtrip COMMAND remaster_poco_tests roundtrip)
add_test(NAME remaster_poco_schema_parity COMMAND remaster_poco_tests parity "${FUSE_REMASTER_REPO_ROOT}/Content/schemas/poco")
add_test(NAME remaster_poco_fuzz COMMAND remaster_poco_tests fuzz 1000)
add_test(NAME remaster_store_canonical COMMAND remaster_store_tests "${CMAKE_CURRENT_BINARY_DIR}/store_tmp")
add_test(NAME remaster_db COMMAND remaster_db_tests "${CMAKE_CURRENT_BINARY_DIR}/db_tmp")
# The gate runs fuse_remaster itself: in a MinGW tree that is a PE run through the emulator ('|'-joined), and the
# doctor output is the Windows one.
string(REPLACE ";" "|" _rm_cli_emu "${CMAKE_CROSSCOMPILING_EMULATOR}")
add_test(NAME remaster_cli
    COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:fuse_remaster> -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli_tmp
            "-DEMU=${_rm_cli_emu}" -DTARGET_WIN32=$<BOOL:${WIN32}>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/remaster_cli_gate.cmake)
set_tests_properties(remaster_poco_roundtrip remaster_poco_schema_parity remaster_poco_fuzz PROPERTIES LABELS "remaster;poco")
set_tests_properties(remaster_store_canonical PROPERTIES LABELS "remaster;store")
set_tests_properties(remaster_db PROPERTIES LABELS "remaster;db")
set_tests_properties(remaster_cli PROPERTIES LABELS "remaster;cli")
set_tests_properties(remaster_poco_roundtrip remaster_poco_schema_parity remaster_poco_fuzz remaster_store_canonical remaster_db
                     remaster_cli PROPERTIES TIMEOUT 120)
