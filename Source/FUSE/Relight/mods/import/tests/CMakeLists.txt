# FUSE Relight RL-3.2 tests: the mod importer. Added from ../CMakeLists.txt.
#
#   rl_mods_import_unit              (relight;gate) material table facts and the every-parameter round trip, lights,
#                                    meshes and transforms, an in-memory mod (keys, rules, licence, determinism, DB
#                                    merge, verifier faults), and an RL-1.8 capture imported as a mod (hash_key and
#                                    original_asset rows equal RL-1.8's store; mesh streams byte-identical; RL-1.8's
#                                    ingestPocoStore accepts the import).
#   rl_mods_import_fixture_<name>    (relight;gate) one per Tests/relight/fixtures/mods_import/<name>: the fixture is
#                                    staged (textures / packages generated from gen_textures.txt), imported twice with
#                                    fuse_relight_import (--verify), the two stores must be byte-identical, and the
#                                    records + DB must equal <fixture>/expected/ (rl_mods_import_fixture.cmake).
#   rl_mods_import_fixture_count     (relight;gate) at least 4 fixtures are registered.
# In a MinGW tree the executables are PE and run through CMAKE_CROSSCOMPILING_EMULATOR (Wine).

add_executable(fuse_relight_mods_import_tests test_mods_import.cpp)
target_link_libraries(fuse_relight_mods_import_tests PRIVATE fuse_relight_mods_import fuse_relight_mods_assets)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_mods_import_tests)
else()
    set_target_properties(fuse_relight_mods_import_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
add_test(NAME rl_mods_import_unit COMMAND fuse_relight_mods_import_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(rl_mods_import_unit PROPERTIES LABELS "gate;relight" TIMEOUT 300)

get_filename_component(_rl_mi_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
set(_rl_mi_fixtures "${_rl_mi_repo}/Tests/relight/fixtures/mods_import")
file(GLOB _rl_mi_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS "${_rl_mi_fixtures}/*")
list(SORT _rl_mi_dirs)
set(_rl_mi_emulator "")
if(CMAKE_CROSSCOMPILING AND CMAKE_CROSSCOMPILING_EMULATOR)
    string(REPLACE ";" "|" _rl_mi_emulator "${CMAKE_CROSSCOMPILING_EMULATOR}")
endif()
set(_rl_mi_count 0)
foreach(_dir IN LISTS _rl_mi_dirs)
    if(NOT IS_DIRECTORY "${_dir}" OR NOT IS_DIRECTORY "${_dir}/expected")
        continue()
    endif()
    get_filename_component(_name "${_dir}" NAME)
    math(EXPR _rl_mi_count "${_rl_mi_count} + 1")
    add_test(NAME rl_mods_import_fixture_${_name}
             COMMAND "${CMAKE_COMMAND}" "-DIMPORT=$<TARGET_FILE:fuse_relight_import>"
                     "-DSTAGER=$<TARGET_FILE:fuse_relight_mods_import_tests>" "-DEMULATOR=${_rl_mi_emulator}"
                     "-DFIXTURE=${_dir}" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/fixtures/${_name}"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/rl_mods_import_fixture.cmake")
    set_tests_properties(rl_mods_import_fixture_${_name} PROPERTIES LABELS "gate;relight" TIMEOUT 300)
endforeach()
add_test(NAME rl_mods_import_fixture_count COMMAND "${CMAKE_COMMAND}" -E echo "rl_mods_import fixtures: ${_rl_mi_count}")
set_tests_properties(rl_mods_import_fixture_count PROPERTIES LABELS "gate;relight" TIMEOUT 30)
if(_rl_mi_count LESS 4)
    set_tests_properties(rl_mods_import_fixture_count PROPERTIES WILL_FAIL TRUE)
endif()
