# FUSE Relight RL-2.3: fuse_relight_launcher (suspended process + LoadLibraryW injection; refuses
# known anti-cheat, see anticheat_list.inc). Added from ../host/CMakeLists.txt.
#
# Targets
#   fuse_relight_launcher_core    (all platforms) anti-cheat table and scans, PE machine, quoting;
#                                 Windows adds process enumeration, launch + injection, self-test
#   fuse_relight_launcher         (Windows) the launcher executable
#   fuse_relight_launcher_selftest_target / _probe   (Windows) self-test helpers, next to the launcher
# Tests: tests/ (rl_bridge_launcher_unit everywhere; rl_bridge_launcher_selftest under Wine).

set(_rl_launcher_src src/anticheat.cpp)
if(WIN32)
    list(APPEND _rl_launcher_src src/launch_win32.cpp src/launcher_selftest.cpp)
endif()
add_library(fuse_relight_launcher_core STATIC ${_rl_launcher_src})
target_include_directories(fuse_relight_launcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(fuse_relight_launcher_core PUBLIC cxx_std_17)
set_source_files_properties(src/anticheat.cpp PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/anticheat_list.inc")

if(WIN32)
    add_executable(fuse_relight_launcher src/launcher_main.cpp)
    target_link_libraries(fuse_relight_launcher PRIVATE fuse_relight_launcher_core)

    add_executable(fuse_relight_launcher_selftest_target selftest/selftest_target.cpp)
    add_library(fuse_relight_launcher_selftest_probe SHARED selftest/selftest_probe.cpp)
    set_target_properties(fuse_relight_launcher_selftest_probe PROPERTIES PREFIX "")
    set_target_properties(fuse_relight_launcher fuse_relight_launcher_selftest_target fuse_relight_launcher_selftest_probe
                          PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FUSE_RELIGHT_BRIDGE_BIN_DIR}")
    add_dependencies(fuse_relight_launcher fuse_relight_launcher_selftest_target fuse_relight_launcher_selftest_probe)
endif()

add_subdirectory(tests)
