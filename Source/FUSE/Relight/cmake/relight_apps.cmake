# FUSE Relight RL-0.4: D3D9/D3D8 fixed-function and shader test apps (plan §6.2).
#
# Included once from Source/FUSE/Relight/CMakeLists.txt:
#     include(cmake/relight_apps.cmake)                                   # RL-0.4
#
# Builds Tests/relight/apps/scenes/<name>.cpp into rl_app_<name>.exe (D3D9) and, for scenes with a
# D3D8 twin, rl_app_d3d8_<name>.exe. Each app renders a fixed number of 128x96 frames, dumps the
# last back buffer (PNG + raw RGBA8) and writes a JSON sidecar with the ground truth it fed D3D
# (schema: Tests/relight/apps/schema/rl_app_sidecar.schema.json).
#
# ctest (labels relight;wine), one per app:
#   rl_app_<name>       runs the app twice under Wine's builtin d3d9/d3d8 (wined3d on llvmpipe)
#                       through cmake/toolchains/fuse-wine-xvfb-run.sh (plain xvfb-run + wine when
#                       that runner is absent), validates the sidecar against the schema, checks
#                       the pixel probes and requires both runs to be byte-identical (image
#                       tolerances per app: Tests/relight/apps/tolerances.json). Skips (77)
#                       without Wine or Xvfb.
#   rl_app_kit_selftest (label relight) schema / checker self-test on generated good and bad
#                       sidecars; no Wine.
#
# The apps are plain Win32 PE programs: they need a Windows target (MinGW cross build, x64 or
# i686 via cmake/toolchains/mingw-w64-i686.cmake, or native Windows). Elsewhere this file only
# registers the self-test.

get_filename_component(_rl_apps_repo "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(_rl_apps_dir "${_rl_apps_repo}/Tests/relight/apps")

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME rl_app_kit_selftest
             COMMAND "${Python3_EXECUTABLE}" "${_rl_apps_dir}/tools/rl_app_check.py" selftest
                     --schema "${_rl_apps_dir}/schema/rl_app_sidecar.schema.json")
    set_tests_properties(rl_app_kit_selftest PROPERTIES LABELS "relight")
endif()

if(NOT WIN32)
    message(STATUS "Relight test apps (RL-0.4): not a Windows target; only rl_app_kit_selftest is registered")
    return()
endif()

enable_language(C)

set(FUSE_WINE_XVFB_RUN "${_rl_apps_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh"
    CACHE FILEPATH "Wine + Xvfb + Lavapipe runner for FUSE Relight tests")
set(FUSE_WINE_XVFB_PREFIX_ROOT "${CMAKE_BINARY_DIR}/wineprefix-xvfb"
    CACHE PATH "Prefix root (template + per-slot WINEPREFIXes) used by fuse-wine-xvfb-run.sh")

# name                  D3D8 twin
set(_rl_apps
    "ff_triangle         twin"
    "ff_textured         twin"
    "ff_lit              twin"
    "ff_alpha            twin"
    "ff_fog              twin"
    "ff_skinned          twin"
    "ff_multi_instance   twin"
    "sky_ui_hud          twin"
    "stencil_shadow      twin"
    "rt_target           twin"
    "dynamic_buffers     twin"
    "texture_formats     twin"
    "shader_sm1          twin"
    "vs_sm2              -"
    "vs_sm3              -"
    "raster_vs_rhw       -")                                   # RL-4.2: VS capture / XYZRHW in the raster remaster

set(_rl_apps_common_src
    "${_rl_apps_dir}/common/rl_app.cpp"
    "${_rl_apps_dir}/common/rl_dump.c")
set(_rl_apps_cxx_flags -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

foreach(_api 9 8)
    add_library(rl_app_kit_d3d${_api} STATIC ${_rl_apps_common_src} "${_rl_apps_dir}/common/rl_gfx_d3d${_api}.cpp")
    target_include_directories(rl_app_kit_d3d${_api} PUBLIC "${_rl_apps_dir}/common")
    target_compile_definitions(rl_app_kit_d3d${_api} PUBLIC RL_API=${_api} WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(rl_app_kit_d3d${_api} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${_rl_apps_cxx_flags}>)
    set_target_properties(rl_app_kit_d3d${_api} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
        C_STANDARD 99 C_STANDARD_REQUIRED ON
        # Same bits on every run and every x86 CPU: no x87 excess precision, no FMA contraction.
        POSITION_INDEPENDENT_CODE OFF)
    target_compile_options(rl_app_kit_d3d${_api} PUBLIC -ffp-contract=off $<$<BOOL:${MINGW}>:-msse2 -mfpmath=sse>)
    # d3d9.dll / d3d8.dll are loaded at run time; only user32/gdi32 are linked.
    target_link_libraries(rl_app_kit_d3d${_api} PUBLIC user32 gdi32)
    if(MINGW)
        target_link_options(rl_app_kit_d3d${_api} PUBLIC -static -static-libgcc -static-libstdc++ -s)
    endif()
endforeach()

set(_rl_apps_check "${_rl_apps_dir}/tools/rl_app_check.py")
set(_rl_apps_all "")
foreach(_entry IN LISTS _rl_apps)
    string(REGEX REPLACE " +" ";" _entry "${_entry}")
    list(GET _entry 0 _scene)
    list(GET _entry 1 _twin)
    set(_variants 9)
    if(_twin STREQUAL "twin")
        list(APPEND _variants 8)
    endif()
    foreach(_api IN LISTS _variants)
        if(_api EQUAL 8)
            set(_app "d3d8_${_scene}")
        else()
            set(_app "${_scene}")
        endif()
        set(_target "rl_app_${_app}")
        add_executable(${_target} "${_rl_apps_dir}/scenes/${_scene}.cpp")
        target_link_libraries(${_target} PRIVATE rl_app_kit_d3d${_api})
        target_compile_options(${_target} PRIVATE ${_rl_apps_cxx_flags})
        set_target_properties(${_target} PROPERTIES
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
            OUTPUT_NAME "${_app}"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/relight/apps")
        list(APPEND _rl_apps_all ${_target})
        if(Python3_Interpreter_FOUND AND CMAKE_CROSSCOMPILING)
            add_test(NAME ${_target}
                     COMMAND "${Python3_EXECUTABLE}" "${_rl_apps_check}" run
                             --exe "$<TARGET_FILE:${_target}>"
                             --app "${_app}"
                             --schema "${_rl_apps_dir}/schema/rl_app_sidecar.schema.json"
                             --tolerances "${_rl_apps_dir}/tolerances.json"
                             --out "${CMAKE_BINARY_DIR}/relight/apps/out/${_app}"
                             --runner "${FUSE_WINE_XVFB_RUN}"
                             --prefix-root "${FUSE_WINE_XVFB_PREFIX_ROOT}")
            set_tests_properties(${_target} PROPERTIES
                LABELS "relight;wine"
                SKIP_RETURN_CODE 77
                TIMEOUT 300)
        endif()
    endforeach()
endforeach()
add_custom_target(rl_apps DEPENDS ${_rl_apps_all})
