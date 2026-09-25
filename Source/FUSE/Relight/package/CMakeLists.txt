# FUSE Relight RL-6.4: plugin discovery module, the fuse_relight_plugins tool and the package gates that need
# no build products. Added from ../cmake/relight_package.cmake.
#
# Targets
#   fuse_relight_package          plugin discovery + PE version reader (portable C++, every tree)
#   fuse_relight_plugins          plugin report tool (shipped in the package on Windows)
#   fuse_relight_plugin_discovery_tests
# Tests
#   rl_package_plugin_discovery   (relight;package;gate) mock plugin dirs: present / absent / incomplete /
#                                 wrong and unknown versions / search order; PE parser truncation + corruption
#   rl_package_selftest           (relight;package;gate;licence) the layout / manifest / licence gates on
#                                 placeholder packages built by relight_package_build.cmake, with seeded
#                                 violations

add_library(fuse_relight_package STATIC src/plugin_discovery.cpp src/pe_version.cpp)
target_include_directories(fuse_relight_package PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(fuse_relight_package PUBLIC cxx_std_20)
set_target_properties(fuse_relight_package PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_executable(fuse_relight_plugins tools/fuse_relight_plugins.cpp)
target_link_libraries(fuse_relight_plugins PRIVATE fuse_relight_package)
set_target_properties(fuse_relight_plugins PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MINGW)
    # Shipped next to the Relight DLLs: no MinGW runtime DLL dependencies.
    target_link_options(fuse_relight_plugins PRIVATE -static -static-libgcc -static-libstdc++)
endif()
if(DEFINED FUSE_RELIGHT_BIN_DIR)
    set_target_properties(fuse_relight_plugins PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FUSE_RELIGHT_BIN_DIR}")
endif()

if(DEFINED FUSE_BUILD_CORE_TESTS AND NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

add_executable(fuse_relight_plugin_discovery_tests tests/test_plugin_discovery.cpp)
target_link_libraries(fuse_relight_plugin_discovery_tests PRIVATE fuse_relight_package)
set_target_properties(fuse_relight_plugin_discovery_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MINGW)
    target_link_options(fuse_relight_plugin_discovery_tests PRIVATE -static -static-libgcc -static-libstdc++)
endif()
set(_rl_pkg_labels "relight;package;gate")
set(_rl_pkg_real_pe "")
if(CMAKE_CROSSCOMPILING AND WIN32)
    list(APPEND _rl_pkg_labels wine)
    if(TARGET relight_d3d8)
        # DXVK's d3d8.dll carries a VERSIONINFO resource (dxvk-gen/d3d8_version.o): a real PE for the parser.
        set(_rl_pkg_real_pe "$<TARGET_FILE:relight_d3d8>")
    endif()
endif()
add_test(NAME rl_package_plugin_discovery
    COMMAND fuse_relight_plugin_discovery_tests "${CMAKE_CURRENT_BINARY_DIR}/plugin_discovery_scratch" ${_rl_pkg_real_pe})
set_tests_properties(rl_package_plugin_discovery PROPERTIES LABELS "${_rl_pkg_labels}" TIMEOUT 300)

get_filename_component(_rl_pkg_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
add_test(NAME rl_package_selftest
    COMMAND "${CMAKE_COMMAND}" -DMODE=selftest "-DREPO=${_rl_pkg_repo}"
            "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/package_selftest" -P "${CMAKE_CURRENT_SOURCE_DIR}/relight_package_check.cmake")
set_tests_properties(rl_package_selftest PROPERTIES LABELS "relight;package;gate;licence" TIMEOUT 300)
