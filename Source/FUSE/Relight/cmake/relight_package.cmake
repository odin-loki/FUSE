# FUSE Relight RL-6.4: packaging, user docs and plugin discovery (docs/plans/FUSE_REMIX_PORT_PLAN.md RL-6.4,
# §0.4.1; user guide docs/relight.md). Included last from ../CMakeLists.txt (needs the runtime targets).
#
# Every tree: package/ (fuse_relight_package discovery library, fuse_relight_plugins tool,
#   rl_package_plugin_discovery, rl_package_selftest).
# MinGW-w64 x64 trees with the runtime (relight_d3d9, relight_d3d8, fuse_relight_host, fuse_relight_launcher):
#   relight_package (ALL)       stages <build>/relight/package/FUSE-Relight with
#                               package/relight_package_build.cmake: x64/ drop-in DLLs + launcher + plugin
#                               tool, x86/ bridge client (from the i686 tree FUSE_RELIGHT_BRIDGE_X86_DIR, when
#                               built) + x86/fuse_relight/ host payload, rtx.conf defaults, README, LICENSE,
#                               THIRD_PARTY_NOTICES.txt + licenses/, docs/relight.md, manifest.json +
#                               MANIFEST.sha256
#   rl_package_layout           (relight;package;gate) layout, forbidden files, PE machines, licences, hashes
#   rl_package_licence_gates    (relight;package;gate;licence) RL-0.1 text + binary gates over the package
#   rl_package_wine_smoke       (relight;package;wine) apps run from the packaged layout under Wine + Xvfb +
#                               Lavapipe: x64 direct, the bridge layout (x86/fuse_relight host folder), and the
#                               i686 app on the packaged x86/ folder when the i686 tree and wine32 exist

add_subdirectory(package)

if(NOT (CMAKE_CROSSCOMPILING AND WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND TARGET relight_d3d9 AND TARGET relight_d3d8
        AND TARGET fuse_relight_host AND TARGET fuse_relight_launcher))
    return()
endif()

option(FUSE_RELIGHT_PACKAGE_REQUIRE_X86 "relight_package: fail when the i686 tree has no x86 bridge client" OFF)
if(NOT DEFINED FUSE_RELIGHT_BRIDGE_X86_DIR)
    set(FUSE_RELIGHT_BRIDGE_X86_DIR "${CMAKE_BINARY_DIR}/../relight-mingw32" CACHE PATH
        "Build tree of the i686 (x86) MinGW configuration holding the x86 apps and bridge client")
endif()

get_filename_component(_rl_pkg_repo "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(_rl_pkg_src "${CMAKE_CURRENT_LIST_DIR}/../package")
set(FUSE_RELIGHT_PACKAGE_DIR "${CMAKE_BINARY_DIR}/relight/package/FUSE-Relight")
set(_rl_pkg_version "${PROJECT_VERSION}")
if(_rl_pkg_version STREQUAL "")
    set(_rl_pkg_version "0.0.0")
endif()
find_package(Git QUIET)
set(_rl_pkg_build "")
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_rl_pkg_repo}" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE _rl_pkg_build OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
set(_rl_pkg_build "git ${_rl_pkg_build}, ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, ${CMAKE_BUILD_TYPE}")

# Inputs that re-stage the package when they change (the i686 files are picked up when present at build time;
# a rebuilt i686 tree needs `cmake --build <x64 tree> --target relight_package` again).
file(GLOB _rl_pkg_dist_files CONFIGURE_DEPENDS "${_rl_pkg_src}/dist/*" "${_rl_pkg_src}/dist/licenses/*")
set(_rl_pkg_x86_inputs "")
foreach(_f relight/bridge/client/d3d9.dll relight/bridge/client/d3d8.dll relight/bin/fuse_relight_launcher.exe relight/bin/d3d9.dll)
    if(EXISTS "${FUSE_RELIGHT_BRIDGE_X86_DIR}/${_f}")
        list(APPEND _rl_pkg_x86_inputs "${FUSE_RELIGHT_BRIDGE_X86_DIR}/${_f}")
    endif()
endforeach()
set(_rl_pkg_stamp "${CMAKE_BINARY_DIR}/relight/package/FUSE-Relight.stamp")
add_custom_command(OUTPUT "${_rl_pkg_stamp}"
    COMMAND "${CMAKE_COMMAND}" "-DOUT=${FUSE_RELIGHT_PACKAGE_DIR}" "-DREPO=${_rl_pkg_repo}"
            "-DVERSION=${_rl_pkg_version}" "-DBUILD_ID=${_rl_pkg_build}"
            "-DX64_D3D9=$<TARGET_FILE:relight_d3d9>" "-DX64_D3D8=$<TARGET_FILE:relight_d3d8>"
            "-DHOST=$<TARGET_FILE:fuse_relight_host>" "-DLAUNCHER=$<TARGET_FILE:fuse_relight_launcher>"
            "-DPLUGINS_TOOL=$<TARGET_FILE:fuse_relight_plugins>"
            "-DX86_DIR=${FUSE_RELIGHT_BRIDGE_X86_DIR}" "-DREQUIRE_X86=${FUSE_RELIGHT_PACKAGE_REQUIRE_X86}"
            -P "${_rl_pkg_src}/relight_package_build.cmake"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_rl_pkg_stamp}"
    DEPENDS relight_d3d9 relight_d3d8 fuse_relight_host fuse_relight_launcher fuse_relight_plugins
            "$<TARGET_FILE:relight_d3d9>" "$<TARGET_FILE:relight_d3d8>" "$<TARGET_FILE:fuse_relight_host>"
            "$<TARGET_FILE:fuse_relight_launcher>" "$<TARGET_FILE:fuse_relight_plugins>"
            "${_rl_pkg_src}/relight_package_build.cmake" "${_rl_pkg_src}/relight_package_spec.cmake"
            "${_rl_pkg_repo}/docs/relight.md" "${_rl_pkg_repo}/LICENSE.md" ${_rl_pkg_dist_files} ${_rl_pkg_x86_inputs}
    COMMENT "FUSE Relight: staging the package in ${FUSE_RELIGHT_PACKAGE_DIR}"
    VERBATIM)
add_custom_target(relight_package ALL DEPENDS "${_rl_pkg_stamp}")

if(DEFINED FUSE_BUILD_CORE_TESTS AND NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

set(_rl_pkg_check "${_rl_pkg_src}/relight_package_check.cmake")
add_test(NAME rl_package_layout
    COMMAND "${CMAKE_COMMAND}" -DMODE=layout "-DPKG=${FUSE_RELIGHT_PACKAGE_DIR}" "-DREPO=${_rl_pkg_repo}" -DCHECK_PE=ON
            "-DREQUIRE_X86=${FUSE_RELIGHT_PACKAGE_REQUIRE_X86}" -P "${_rl_pkg_check}")
add_test(NAME rl_package_licence_gates
    COMMAND "${CMAKE_COMMAND}" -DMODE=licence "-DPKG=${FUSE_RELIGHT_PACKAGE_DIR}" "-DREPO=${_rl_pkg_repo}" -P "${_rl_pkg_check}")
set_tests_properties(rl_package_layout PROPERTIES LABELS "relight;package;gate" TIMEOUT 300)
set_tests_properties(rl_package_licence_gates PROPERTIES LABELS "relight;package;gate;licence" TIMEOUT 600)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND TARGET rl_app_ff_triangle AND TARGET relight_bridge_d3d9 AND TARGET relight_bridge_d3d8)
    if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
        set(_rl_pkg_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
    else()
        set(_rl_pkg_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
    endif()
    add_test(NAME rl_package_wine_smoke
        COMMAND "${Python3_EXECUTABLE}" "${_rl_pkg_src}/tests/rl_package_smoke.py"
                --pkg "${FUSE_RELIGHT_PACKAGE_DIR}"
                --app "$<TARGET_FILE:rl_app_ff_triangle>"
                --x64-client-d3d9 "$<TARGET_FILE:relight_bridge_d3d9>" --x64-client-d3d8 "$<TARGET_FILE:relight_bridge_d3d8>"
                --x86-app "${FUSE_RELIGHT_BRIDGE_X86_DIR}/relight/apps/ff_triangle.exe"
                --runner "${_rl_pkg_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh" --prefix-root "${_rl_pkg_prefix}"
                --out "${CMAKE_BINARY_DIR}/relight/package/smoke"
                # Trees without the RL-0.7 renderer (FUSE_RELIGHT_RENDERER=OFF, e.g. the CI fuse-mingw-release
                # preset) build no frame orchestration, so the packaged rtx.conf's raster mode cannot attach there.
                --frame-renderer "$<IF:$<TARGET_EXISTS:fuse_relight_render_frame>,1,0>")
    set_tests_properties(rl_package_wine_smoke PROPERTIES LABELS "relight;package;wine" SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()
