# FUSE Relight RL-2.3: launcher tests. Added from ../CMakeLists.txt.
#
#   rl_bridge_launcher_unit      (relight;bridge;gate) anti-cheat table/scans, glob, PE, quoting.
#                                Any platform (Wine emulator in the MinGW tree).
#   rl_bridge_launcher_selftest  (relight;bridge;wine;gate) fuse_relight_launcher --self-test under
#                                Wine: suspended creation + LoadLibraryW injection before main,
#                                exit-code passthrough, refusals (install layout, running process,
#                                architecture), failed injection terminates the game.

add_executable(fuse_relight_launcher_unit_tests test_launcher_unit.cpp)
target_link_libraries(fuse_relight_launcher_unit_tests PRIVATE fuse_relight_launcher_core)

set(_rl_launcher_labels "relight;bridge;gate")
if(CMAKE_CROSSCOMPILING AND WIN32)
    list(APPEND _rl_launcher_labels wine)
endif()
add_test(NAME rl_bridge_launcher_unit COMMAND fuse_relight_launcher_unit_tests)
set_tests_properties(rl_bridge_launcher_unit PROPERTIES LABELS "${_rl_launcher_labels}" TIMEOUT 120)

if(WIN32)
    add_test(NAME rl_bridge_launcher_selftest COMMAND fuse_relight_launcher --self-test --verbose)
    set_tests_properties(rl_bridge_launcher_selftest PROPERTIES
        LABELS "${_rl_launcher_labels}" TIMEOUT 300 SKIP_RETURN_CODE 77)
endif()
