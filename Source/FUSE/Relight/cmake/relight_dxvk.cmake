# RL-0.2: CMake port of upstream DXVK 3.1.1's meson build, restricted to the D3D9 and D3D8 front ends
# (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.2, §7 RL-0.2). Produces 64-bit PE d3d9.dll and d3d8.dll
# from Engine/lib/dxvk + Engine/lib/dxbc-spirv. d3d10/d3d11/dxgi are not vendored and never built.
#
# What is translated from meson (Engine/lib/dxvk/meson.build, src/*/meson.build, dxbc-spirv's
# meson.build, subprojects/libdisplay-info/meson.build):
#   * source lists: read straight from the vendored meson.build files at configure time, so an
#     upstream rebase that adds or removes a file needs no edit here (a missing list is a hard error);
#   * compiler/linker arguments of the Windows (MinGW) branch, incl. the 32-bit x86 extras;
#   * generated headers: version.h (vcs_tag) and buildenv.h (configure_file) from the upstream
#     templates, and one SPIR-V header per GLSL shader (glslangValidator --vn <basename>, as meson's
#     glsl_generator does), with depfiles so #included .glsl changes rebuild;
#   * libdisplay-info's pnp-id-table.c (tool/gen-search-table.py, Python 3);
#   * version.rc through windres; d3d9.def / d3d8.def exports.
# DirectX headers come from the MinGW-w64 toolchain (d3d9.h, d3d8.h, ...). DXVK's
# include/native/directx (LGPL-2.1+) is never vendored and is not needed for PE builds (§0.3).
#
# Linux-native DXVK ("dxvk-native") is not built: it needs SDL2/SDL3/GLFW WSI and the LGPL
# mingw-directx-headers fetched into build/ at configure time. Nothing in Relight needs it yet;
# the PE DLLs run on Linux under Wine/Proton (G7). See Engine/lib/dxvk/PATCHES.md "Build notes".
#
# Vendored sources compile with upstream's own warning suppressions below; fuse_warnings_finalize
# additionally marks every vendored source file -w (they are not FUSE code), while FUSE-owned sources
# in these targets (Tests/relight/smoke, copied into the FUSE binary dir) keep -Wall -Wextra -Werror.
#
# Tests (ctest -L relight):
#   rl_dxvk_exports  PE32+ x86-64 and the D3D9/D3D8 entry points are exported (objdump -p).
#   rl_dxvk_pins     fuse_lint vendored-pins over Engine/lib/dxvk and Engine/lib/dxbc-spirv.
#   rl_dxvk_smoke    Tests/relight/smoke/create_device.c through our d3d9.dll under Xvfb + Wine +
#                    Lavapipe: device, clear, present, readback (skips 77 without Wine or Xvfb).

set(FUSE_DXVK_DIR "${FUSE_VENDOR_DIR}/dxvk")
set(FUSE_DXBC_SPIRV_DIR "${FUSE_VENDOR_DIR}/dxbc-spirv")
set(FUSE_RELIGHT_SMOKE_DIR "${CMAKE_SOURCE_DIR}/Tests/relight/smoke")

# The pin lint runs in every Relight tree (it is pure CPU); the DLL build is PE-only.
if(FUSE_BUILD_CORE_TESTS)
    # The emulator is a list (runner;prefix): '|'-joined so add_test keeps it one argument.
    string(REPLACE ";" "|" _rl_emulator "${CMAKE_CROSSCOMPILING_EMULATOR}")
    add_test(NAME rl_dxvk_pins
        COMMAND "${CMAKE_COMMAND}"
            "-DFUSE_LINT=$<TARGET_FILE:fuse_lint>"
            "-DFUSE_EMULATOR=${_rl_emulator}"
            "-DFUSE_DIRS=${FUSE_DXVK_DIR}|${FUSE_DXBC_SPIRV_DIR}"
            "-DFUSE_SCRATCH=${CMAKE_BINARY_DIR}/relight/pins_scratch"
            -P "${FUSE_RELIGHT_SMOKE_DIR}/check_pins.cmake")
    set_tests_properties(rl_dxvk_pins PROPERTIES LABELS "relight;gate" TIMEOUT 300)
endif()

if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW))
    message(STATUS "FUSE Relight: vendored DXVK d3d9/d3d8 build skipped (PE/MinGW-w64 only; dxvk-native not ported)")
    return()
endif()

# ---- tools -------------------------------------------------------------------------------------
find_program(FUSE_GLSLANG_VALIDATOR NAMES glslang glslangValidator REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(NOT CMAKE_RC_COMPILER)
    message(FATAL_ERROR "FUSE Relight: windres (CMAKE_RC_COMPILER) is required for DXVK's version.rc")
endif()

set(_rl_gen "${CMAKE_BINARY_DIR}/relight/dxvk-gen")   # outside build/Source/FUSE: not FUSE code
file(MAKE_DIRECTORY "${_rl_gen}")

# ---- meson source-list reader ------------------------------------------------------------------
# Collects every quoted entry of `<var> = [ ... ]`, `<var> = files([ ... ])` and `<var> += [ ... ]` in
# a meson.build, prefixed with the file's directory. Upstream lists hold one 'path' per entry.
function(_rl_meson_sources meson_file var out)
    file(READ "${meson_file}" _txt)
    string(REGEX MATCHALL "(^|\n) *${var} \\+?= (files\\()?\\[[^]]*\\]" _blocks "${_txt}")
    if(NOT _blocks)
        message(FATAL_ERROR "FUSE Relight: no '${var}' list in ${meson_file} (upstream layout changed?)")
    endif()
    get_filename_component(_dir "${meson_file}" DIRECTORY)
    set(_files)
    foreach(_b IN LISTS _blocks)
        string(REGEX MATCHALL "'[^']+'" _items "${_b}")
        foreach(_i IN LISTS _items)
            string(REGEX REPLACE "^'(.*)'$" "\\1" _i "${_i}")
            if(NOT EXISTS "${_dir}/${_i}")
                message(FATAL_ERROR "FUSE Relight: ${meson_file} lists missing file ${_i}")
            endif()
            list(APPEND _files "${_dir}/${_i}")
        endforeach()
    endforeach()
    set(${out} ${_files} PARENT_SCOPE)
endfunction()

# ---- GLSL -> SPIR-V header rule (meson glsl_generator) ------------------------------------------
function(_rl_glsl_headers out_dir out_var)
    file(MAKE_DIRECTORY "${out_dir}")
    set(_headers)
    foreach(_src IN LISTS ARGN)
        get_filename_component(_base "${_src}" NAME_WLE)
        set(_h "${out_dir}/${_base}.h")
        add_custom_command(
            OUTPUT "${_h}"
            COMMAND "${FUSE_GLSLANG_VALIDATOR}" --quiet --target-env vulkan1.3 --vn "${_base}"
                    --depfile "${_h}.d" "${_src}" -o "${_h}"
            DEPENDS "${_src}"
            DEPFILE "${_h}.d"
            COMMENT "glslang ${_base}"
            VERBATIM)
        list(APPEND _headers "${_h}")
    endforeach()
    set(${out_var} ${_headers} PARENT_SCOPE)
endfunction()

# ---- version.rc -> COFF object through windres (meson wrc_generator) ----------------------------
function(_rl_version_res rc_file out_obj)
    add_custom_command(
        OUTPUT "${out_obj}"
        COMMAND "${CMAKE_RC_COMPILER}" -i "${rc_file}" -o "${out_obj}"
        DEPENDS "${rc_file}"
        COMMENT "windres ${rc_file}"
        VERBATIM)
    set_source_files_properties("${out_obj}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
endfunction()

# ---- generated headers: version.h, buildenv.h, fuse_dxvk_version.h cross-check -------------------
file(READ "${FUSE_DXVK_DIR}/RELEASE" _rl_release)
string(STRIP "${_rl_release}" _rl_release)
file(STRINGS "${FUSE_DXVK_DIR}/fuse_dxvk_version.h" _rl_pin_lines REGEX "#define FUSE_DXVK_VERSION_(MAJOR|MINOR|PATCH)")
set(_rl_pin_version "")
foreach(_part MAJOR MINOR PATCH)
    foreach(_l IN LISTS _rl_pin_lines)
        if(_l MATCHES "_${_part} +([0-9]+)")
            list(APPEND _rl_pin_version "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endforeach()
list(JOIN _rl_pin_version "." _rl_pin_version)
if(NOT _rl_pin_version STREQUAL _rl_release)
    message(FATAL_ERROR "FUSE Relight: third_party/vendor/dxvk/RELEASE (${_rl_release}) != fuse_dxvk_version.h (${_rl_pin_version})")
endif()
# meson's vcs_tag runs `git describe --dirty=+` in the DXVK checkout; the vendored tree is not one, so
# the tag is the pinned release plus a FUSE marker (zlib clause 2: altered builds are marked).
set(VCS_TAG "v${_rl_release}-fuse")
configure_file("${FUSE_DXVK_DIR}/version.h.in" "${_rl_gen}/version.h" @ONLY)
set(BUILD_COMPILER "${CMAKE_CXX_COMPILER_ID}")
string(TOLOWER "${BUILD_COMPILER}" BUILD_COMPILER)
if(BUILD_COMPILER STREQUAL "gnu")
    set(BUILD_COMPILER "gcc")
endif()
set(BUILD_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(BUILD_TARGET "x86_64")
    set(_rl_x86 OFF)
else()
    set(BUILD_TARGET "x86")
    set(_rl_x86 ON)
endif()
configure_file("${FUSE_DXVK_DIR}/buildenv.h.in" "${_rl_gen}/buildenv.h" @ONLY)

# ---- common flags (meson.build, Windows / non-MSVC branch) --------------------------------------
add_library(relight_dxvk_flags INTERFACE)
target_compile_options(relight_dxvk_flags INTERFACE
    -msse -msse2 -msse3 -mfpmath=sse
    -Wimplicit-fallthrough
    -Wno-missing-field-initializers -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-cast-function-type
    $<$<BOOL:${_rl_x86}>:-mpreferred-stack-boundary=2>)
target_compile_definitions(relight_dxvk_flags INTERFACE NOMINMAX _WIN32_WINNT=0xa00 DXVK_WSI_WIN32)
target_include_directories(relight_dxvk_flags SYSTEM INTERFACE
    "${FUSE_DXVK_DIR}/include"
    "${FUSE_DXVK_DIR}/include/vulkan/include"
    "${FUSE_DXVK_DIR}/include/spirv/include"
    "${_rl_gen}")
target_link_options(relight_dxvk_flags INTERFACE
    -static -static-libgcc -static-libstdc++ -Wl,--file-alignment=4096
    $<$<BOOL:${_rl_x86}>:-Wl,--enable-stdcall-fixup>
    $<$<BOOL:${_rl_x86}>:-Wl,--kill-at>)

function(_rl_cxx17 target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endfunction()

# ---- dxbc-spirv (meson: cpp_std=c++17, cpp_eh=none, sm3 + sm5 + spirv enabled) -------------------
set(_rl_dxbc_meson "${FUSE_DXBC_SPIRV_DIR}/meson.build")
_rl_meson_sources("${_rl_dxbc_meson}" dxbc_spv_files _rl_dxbc_core)
_rl_meson_sources("${_rl_dxbc_meson}" sm3_files _rl_dxbc_sm3)
_rl_meson_sources("${_rl_dxbc_meson}" sm5_files _rl_dxbc_sm5)
_rl_meson_sources("${_rl_dxbc_meson}" spv_files _rl_dxbc_spv)
add_library(relight_dxbc_spirv STATIC ${_rl_dxbc_core} ${_rl_dxbc_sm3} ${_rl_dxbc_sm5} ${_rl_dxbc_spv})
_rl_cxx17(relight_dxbc_spirv)
target_compile_definitions(relight_dxbc_spirv PRIVATE
    DXBC_SPV_ENABLE_SM3 DXBC_SPV_ENABLE_SM5 DXBC_SPV_ENABLE_SPIRV)
target_compile_options(relight_dxbc_spirv PRIVATE -fno-exceptions -Wno-unused-parameter
    -Wno-missing-field-initializers)
target_include_directories(relight_dxbc_spirv SYSTEM PUBLIC "${FUSE_DXBC_SPIRV_DIR}")

# ---- libdisplay-info (DXVK subproject, 'windows' branch; C11) ------------------------------------
set(_rl_di "${FUSE_DXVK_DIR}/subprojects/libdisplay-info")
add_custom_command(
    OUTPUT "${_rl_gen}/pnp-id-table.c"
    COMMAND "${Python3_EXECUTABLE}" "${_rl_di}/tool/gen-search-table.py"
            "${_rl_di}/pnp.ids" "${_rl_gen}/pnp-id-table.c" pnp_id_table
    DEPENDS "${_rl_di}/tool/gen-search-table.py" "${_rl_di}/pnp.ids"
    COMMENT "libdisplay-info pnp-id-table.c"
    VERBATIM)
add_library(relight_displayinfo STATIC
    "${_rl_di}/cta.c" "${_rl_di}/displayid.c" "${_rl_di}/dmt-table.c" "${_rl_di}/edid.c"
    "${_rl_di}/gtf.c" "${_rl_di}/info.c" "${_rl_di}/log.c" "${_rl_di}/memory-stream.c"
    "${_rl_gen}/pnp-id-table.c")
set_target_properties(relight_displayinfo PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(relight_displayinfo PRIVATE _POSIX_C_SOURCE=200809L static_array=static)
target_compile_options(relight_displayinfo PRIVATE -Wno-unused-parameter -Wno-missing-field-initializers)
target_include_directories(relight_displayinfo SYSTEM PUBLIC "${_rl_di}/include")

# ---- DXVK static libraries (src/{util,spirv,vulkan,wsi,dxvk}) -----------------------------------
set(_rl_src "${FUSE_DXVK_DIR}/src")

_rl_meson_sources("${_rl_src}/util/meson.build" util_src _rl_util_src)
add_library(relight_dxvk_util STATIC ${_rl_util_src})
target_link_libraries(relight_dxvk_util PUBLIC relight_dxvk_flags)

_rl_meson_sources("${_rl_src}/spirv/meson.build" spirv_src _rl_spirv_src)
add_library(relight_dxvk_spirv STATIC ${_rl_spirv_src})
target_link_libraries(relight_dxvk_spirv PUBLIC relight_dxvk_flags)

_rl_meson_sources("${_rl_src}/vulkan/meson.build" vkcommon_src _rl_vk_src)
add_library(relight_dxvk_vkcommon STATIC ${_rl_vk_src})
find_package(Threads REQUIRED)
target_link_libraries(relight_dxvk_vkcommon PUBLIC relight_dxvk_flags Threads::Threads)

_rl_meson_sources("${_rl_src}/wsi/meson.build" wsi_src _rl_wsi_src)
add_library(relight_dxvk_wsi STATIC ${_rl_wsi_src})
target_link_libraries(relight_dxvk_wsi PUBLIC relight_dxvk_flags relight_displayinfo setupapi)

_rl_meson_sources("${_rl_src}/dxvk/meson.build" dxvk_shaders _rl_dxvk_glsl)
_rl_meson_sources("${_rl_src}/dxvk/meson.build" dxvk_src _rl_dxvk_src)   # '+=' adds openvr/openxr
_rl_glsl_headers("${_rl_gen}/dxvk" _rl_dxvk_spv ${_rl_dxvk_glsl})
add_library(relight_dxvk STATIC ${_rl_dxvk_src} ${_rl_dxvk_spv})
target_include_directories(relight_dxvk PRIVATE "${_rl_gen}/dxvk")
target_link_libraries(relight_dxvk PUBLIC
    relight_dxvk_util relight_dxvk_spirv relight_dxvk_wsi relight_dxvk_vkcommon relight_dxbc_spirv
    relight_dxvk_flags Threads::Threads)

# meson adds each target's own source directory to its include path (implicit_include_directories),
# and upstream relies on it (e.g. src/util/com/*.h includes "../util/thread.h").
foreach(_t relight_dxvk_util relight_dxvk_spirv relight_dxvk_vkcommon relight_dxvk_wsi relight_dxvk)
    _rl_cxx17(${_t})
    string(REGEX REPLACE "^relight_dxvk_?" "" _dir "${_t}")
    if(_dir STREQUAL "")
        set(_dir dxvk)
    elseif(_dir STREQUAL "vkcommon")
        set(_dir vulkan)
    endif()
    target_include_directories(${_t} PRIVATE "${_rl_src}/${_dir}")
endforeach()

# ---- d3d9.dll / d3d8.dll ------------------------------------------------------------------------
set(FUSE_RELIGHT_BIN_DIR "${CMAKE_BINARY_DIR}/relight/bin")

_rl_meson_sources("${_rl_src}/d3d9/meson.build" d3d9_shaders _rl_d3d9_glsl)
_rl_meson_sources("${_rl_src}/d3d9/meson.build" d3d9_src _rl_d3d9_src)
_rl_glsl_headers("${_rl_gen}/d3d9" _rl_d3d9_spv ${_rl_d3d9_glsl})
_rl_version_res("${_rl_src}/d3d9/version.rc" "${_rl_gen}/d3d9_version.o")
add_library(relight_d3d9 SHARED ${_rl_d3d9_src} ${_rl_d3d9_spv}
    "${_rl_gen}/d3d9_version.o" "${_rl_src}/d3d9/d3d9.def")
target_include_directories(relight_d3d9 PRIVATE "${_rl_gen}/d3d9")
target_link_libraries(relight_d3d9 PRIVATE relight_dxvk)

_rl_meson_sources("${_rl_src}/d3d8/meson.build" d3d8_src _rl_d3d8_src)
_rl_version_res("${_rl_src}/d3d8/version.rc" "${_rl_gen}/d3d8_version.o")
add_library(relight_d3d8 SHARED ${_rl_d3d8_src} "${_rl_gen}/d3d8_version.o" "${_rl_src}/d3d8/d3d8.def")
# meson: d3d8 links the toolchain's d3d9 import library, so d3d8.dll imports D3D9.DLL by name
# (ours when it sits next to it).
target_link_libraries(relight_d3d8 PRIVATE relight_dxvk d3d9)

foreach(_t relight_d3d9 relight_d3d8)
    _rl_cxx17(${_t})
    string(REPLACE "relight_" "" _name "${_t}")
    target_include_directories(${_t} PRIVATE "${_rl_src}/${_name}")
    set_target_properties(${_t} PROPERTIES
        PREFIX "" OUTPUT_NAME "${_name}" IMPORT_PREFIX "lib" IMPORT_SUFFIX ".dll.a"
        RUNTIME_OUTPUT_DIRECTORY "${FUSE_RELIGHT_BIN_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/relight/lib")
endforeach()
add_custom_target(relight_dxvk_dlls ALL DEPENDS relight_d3d9 relight_d3d8)

# ---- smoke app (FUSE code: copied into the FUSE binary dir so fuse_warnings_finalize treats it as
# FUSE code and applies -Wall -Wextra [-Werror] instead of the vendored -w) ------------------------
configure_file("${FUSE_RELIGHT_SMOKE_DIR}/create_device.c" "${CMAKE_CURRENT_BINARY_DIR}/smoke/create_device.c" COPYONLY)
add_executable(rl_smoke_create_device "${CMAKE_CURRENT_BINARY_DIR}/smoke/create_device.c")
set_target_properties(rl_smoke_create_device PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
    OUTPUT_NAME create_device
    RUNTIME_OUTPUT_DIRECTORY "${FUSE_RELIGHT_BIN_DIR}")
target_compile_options(rl_smoke_create_device PRIVATE -Wall -Wextra -Werror)
# Imports d3d9.dll through the toolchain import library: at run time Wine resolves it to the
# d3d9.dll next to the exe (WINEDLLOVERRIDES=d3d9=n), which the app itself verifies.
target_link_libraries(rl_smoke_create_device PRIVATE d3d9)
add_dependencies(rl_smoke_create_device relight_d3d9 relight_d3d8)

if(FUSE_BUILD_CORE_TESTS)
    add_test(NAME rl_dxvk_exports
        COMMAND "${CMAKE_COMMAND}"
            "-DFUSE_OBJDUMP=${CMAKE_OBJDUMP}"
            "-DFUSE_D3D9=$<TARGET_FILE:relight_d3d9>"
            "-DFUSE_D3D8=$<TARGET_FILE:relight_d3d8>"
            -P "${FUSE_RELIGHT_SMOKE_DIR}/check_exports.cmake")
    set_tests_properties(rl_dxvk_exports PROPERTIES LABELS "relight;gate" TIMEOUT 60)

    # RL-0.3's Xvfb runner, sharing its prefix root (template + per-slot prefixes) with the other
    # Relight Wine tests (same default as Source/FUSE/Relight/tests/env).
    if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
        set(_rl_prefix_root "${FUSE_WINE_XVFB_PREFIX_ROOT}")
    else()
        set(_rl_prefix_root "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
    endif()
    add_test(NAME rl_dxvk_smoke
        COMMAND "${FUSE_RELIGHT_SMOKE_DIR}/run_smoke.sh"
            "${CMAKE_SOURCE_DIR}/cmake/toolchains/fuse-wine-xvfb-run.sh"
            "${_rl_prefix_root}"
            "$<TARGET_FILE:rl_smoke_create_device>")
    set_tests_properties(rl_dxvk_smoke PROPERTIES
        LABELS "relight;wine;vulkan"
        SKIP_RETURN_CODE 77
        TIMEOUT 600)
endif()
