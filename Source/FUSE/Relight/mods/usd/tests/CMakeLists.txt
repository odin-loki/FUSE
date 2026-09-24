# FUSE Relight RL-3.1 tests: the USD reader with the Remix profile. Added from ../CMakeLists.txt.
#
#   rl_mod_usd_unit                (relight;gate) values, paths, classification, composition corner cases
#                                  (list-op order, variant fallbacks, payloads off, arc limits, cycles, bad and
#                                  missing layers, namespace mapping, sublayer offsets), determinism, and 300
#                                  mutated layers that must not crash the loader.
#   rl_mod_capture_roundtrip       (relight;gate) RL-1.8's USDA capture (writeRemixUsda) read back through the
#                                  composed stage: every written fact comes back, with no diagnostics.
#   rl_mod_fixture_<name>_usda     (relight;gate) one per Tests/relight/fixtures/mods/<name> (handcrafted mods with
#   rl_mod_fixture_<name>_usdc      an expect.txt): expectations on the USDA files, then on a USDC twin generated
#                                  at test time with TinyUSDZ's crate writer, whose canonical dump must equal the
#                                  USDA one (rl_mod_fixture.cmake).
#   rl_mod_fixture_count           (relight;gate) at least 20 fixtures are registered.
#   rl_mod_flatten_check_selftest  (relight) Tools/FUSE/Relight/usd_flatten_check.py compares synthetic dumps
#                                  (equal, tolerances, seeded differences).
#   rl_mod_flatten_crosscheck      (relight) every fixture composed by usd-core (pxr) vs our canonical dump;
#                                  skips (77) when the usd-core wheel is not installed.
# In a MinGW tree the executables are PE and run through CMAKE_CROSSCOMPILING_EMULATOR (Wine).

set(_rl_mu_targets fuse_relight_mods_usd_tests)
add_executable(fuse_relight_mods_usd_tests test_mods_usd.cpp)
target_link_libraries(fuse_relight_mods_usd_tests PRIVATE fuse_relight_mods_usd)
add_test(NAME rl_mod_usd_unit COMMAND fuse_relight_mods_usd_tests)
set_tests_properties(rl_mod_usd_unit PROPERTIES LABELS "gate;relight" TIMEOUT 300)

if(TARGET fuse_relight_capture_export)
    add_executable(fuse_relight_mods_usd_roundtrip test_capture_roundtrip.cpp)
    target_link_libraries(fuse_relight_mods_usd_roundtrip PRIVATE fuse_relight_mods_usd fuse_relight_capture_export)
    list(APPEND _rl_mu_targets fuse_relight_mods_usd_roundtrip)
    add_test(NAME rl_mod_capture_roundtrip COMMAND fuse_relight_mods_usd_roundtrip)
    set_tests_properties(rl_mod_capture_roundtrip PROPERTIES LABELS "gate;relight" TIMEOUT 300)
endif()

foreach(_t IN LISTS _rl_mu_targets)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endforeach()

# ---- fixtures -----------------------------------------------------------------------------------------------
get_filename_component(_rl_mu_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
set(_rl_mu_fixtures_dir "${_rl_mu_repo}/Tests/relight/fixtures/mods")
file(GLOB _rl_mu_expects LIST_DIRECTORIES false CONFIGURE_DEPENDS "${_rl_mu_fixtures_dir}/*/expect.txt")
list(SORT _rl_mu_expects)
set(_rl_mu_emulator "")
if(CMAKE_CROSSCOMPILING AND CMAKE_CROSSCOMPILING_EMULATOR)
    string(REPLACE ";" "|" _rl_mu_emulator "${CMAKE_CROSSCOMPILING_EMULATOR}")
endif()
set(_rl_mu_count 0)
foreach(_e IN LISTS _rl_mu_expects)
    get_filename_component(_dir "${_e}" DIRECTORY)
    get_filename_component(_name "${_dir}" NAME)
    if(NOT EXISTS "${_dir}/mod.usda")
        continue()
    endif()
    math(EXPR _rl_mu_count "${_rl_mu_count} + 1")
    foreach(_fmt usda usdc)
        add_test(NAME rl_mod_fixture_${_name}_${_fmt}
                 COMMAND "${CMAKE_COMMAND}" "-DTOOL=$<TARGET_FILE:fuse_relight_usd_tool>" "-DEMULATOR=${_rl_mu_emulator}"
                         "-DFIXTURE=${_dir}" -DFORMAT=${_fmt} "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/usdc/${_name}"
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/rl_mod_fixture.cmake")
        set_tests_properties(rl_mod_fixture_${_name}_${_fmt} PROPERTIES LABELS "gate;relight" TIMEOUT 120)
    endforeach()
endforeach()
add_test(NAME rl_mod_fixture_count COMMAND "${CMAKE_COMMAND}" -E echo "rl_mod fixtures: ${_rl_mu_count}")
set_tests_properties(rl_mod_fixture_count PROPERTIES LABELS "gate;relight" TIMEOUT 30)
if(_rl_mu_count LESS 20)
    set_tests_properties(rl_mod_fixture_count PROPERTIES WILL_FAIL TRUE) # plan RL-3.1: 20+ handcrafted fixtures
endif()

# ---- usd-core cross-check -----------------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    return()
endif()
set(_rl_mu_check "${_rl_mu_repo}/Tools/FUSE/Relight/usd_flatten_check.py")
add_test(NAME rl_mod_flatten_check_selftest COMMAND "${Python3_EXECUTABLE}" -B "${_rl_mu_check}" selftest)
set_tests_properties(rl_mod_flatten_check_selftest PROPERTIES LABELS "relight" TIMEOUT 120
                     ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
add_test(NAME rl_mod_flatten_crosscheck
         COMMAND "${Python3_EXECUTABLE}" -B "${_rl_mu_check}" fixtures --fixtures "${_rl_mu_fixtures_dir}"
                 --tool "$<TARGET_FILE:fuse_relight_usd_tool>" --emulator "${_rl_mu_emulator}")
set_tests_properties(rl_mod_flatten_crosscheck PROPERTIES LABELS "relight" TIMEOUT 600 SKIP_RETURN_CODE 77
                     ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
