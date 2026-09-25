# FuseSlang.cmake — Slang shader toolchain (WP-0.5, docs/unification/RENDERER-EXECUTION.md §5/§8).
#
# Resolves a `slangc` for the build host and offers fuse_add_slang_shaders() to compile .slang
# sources to SPIR-V at build time (depfile tracking of #include/import, reflection JSON, spirv-val).
# Reusable by any module (Renderer, Relight, ...): include() it from any directory; results are
# published as INTERNAL cache variables so every directory scope sees them.
#
# Options
#   FUSE_SLANG=AUTO|ON|OFF    AUTO (default): use Slang when a slangc resolves, else disable Slang
#                             shaders (tests skip with 77). ON: configure fails without slangc.
#                             OFF: never look for or download slangc.
#   FUSE_SLANGC=<path>        Use this slangc (any version; a mismatch with the pin only warns).
#   FUSE_SLANG_DOWNLOAD=ON    Allow the pinned release download (Engine/lib/slang/VERSION).
#   FUSE_SLANG_CACHE_DIR=...  Unpack root (default ~/.cache/fuse/slang, %LOCALAPPDATA%/fuse/slang).
#                             The release lands in <root>/<version>; nothing enters the source tree.
#   FUSE_SLANG_KEEP_LLVM=OFF  Keep libslang-llvm (CPU/host code generation, ~150 MB) after unpacking.
#
# Results (INTERNAL cache variables)
#   FUSE_SLANG_FOUND          TRUE when a working slangc was resolved.
#   FUSE_SLANGC_EXECUTABLE    Absolute path of slangc.
#   FUSE_SLANGC_VERSION       `slangc -version` output (e.g. 2026.18.2).
#   FUSE_SLANG_PIN_VERSION    Version pinned in Engine/lib/slang/VERSION.
#   FUSE_SLANG_SPIRV_FLAGS    Code-generation flags shared by build-time and runtime compiles.
#
# fuse_add_slang_shaders(<target>
#     SOURCES <a.slang> ...          .slang files (relative to the current source dir or absolute)
#     [OUTPUT_DIR <dir>]             default ${CMAKE_CURRENT_BINARY_DIR}/slang/<target>
#     [ENTRY <name>]                 entry point (default: main)
#     [STAGE <compute|vertex|...>]   omit when the entry carries [shader("...")]
#     [SUFFIX <text>]                output stem suffix, for permutations of one source
#     [DEFINES <NAME[=VALUE]> ...]   -D defines
#     [INCLUDE_DIRS <dir> ...]       -I search paths for #include and import
#     [FLAGS <flag> ...]             extra slangc flags
#     [OUTPUT_VAR <var>]             receives the list of generated .spv files
#     [NO_SPIRV_VAL])                skip spirv-val (it runs by default when found)
# Produces <OUTPUT_DIR>/<stem><SUFFIX>.spv, .reflection.json and .d (depfile), a custom target
# <target>_slang_<n> that <target> depends on, and nothing at all when FUSE_SLANG_FOUND is false
# (OUTPUT_VAR is then empty) so callers can compile-define "shaders built" and skip otherwise.

include_guard(GLOBAL)

set(FUSE_SLANG "AUTO" CACHE STRING "Slang shader compiler: AUTO, ON (required) or OFF")
set_property(CACHE FUSE_SLANG PROPERTY STRINGS AUTO ON OFF)
set(FUSE_SLANGC "" CACHE FILEPATH "Explicit slangc executable (skips the pinned download)")
option(FUSE_SLANG_DOWNLOAD "Allow downloading the pinned slangc release (Engine/lib/slang/VERSION)" ON)
set(FUSE_SLANG_CACHE_DIR "" CACHE PATH "Root for downloaded slangc releases (outside the source tree)")
option(FUSE_SLANG_KEEP_LLVM "Keep libslang-llvm (host/CPU targets) when unpacking slangc" OFF)

find_program(FUSE_SPIRV_VAL NAMES spirv-val)

set(FUSE_SLANG_PIN_FILE "${FUSE_VENDOR_DIR}/slang/VERSION")
get_filename_component(FUSE_SLANG_PIN_FILE "${FUSE_SLANG_PIN_FILE}" ABSOLUTE)

# Code generation shared with the runtime compiler (ShaderCompiler::compileWithSlang mirrors it).
set(FUSE_SLANG_SPIRV_FLAGS -target spirv -profile spirv_1_5 CACHE INTERNAL "slangc SPIR-V flags")

# Reads key=value lines of the pin file into _fuse_slang_pin_<key> (':' in keys becomes '_').
function(_fuse_slang_read_pin)
    if(NOT EXISTS "${FUSE_SLANG_PIN_FILE}")
        return()
    endif()
    file(STRINGS "${FUSE_SLANG_PIN_FILE}" _lines REGEX "^[a-z_0-9:-]+=")
    foreach(_line IN LISTS _lines)
        string(FIND "${_line}" "=" _eq)
        string(SUBSTRING "${_line}" 0 ${_eq} _key)
        math(EXPR _vstart "${_eq} + 1")
        string(SUBSTRING "${_line}" ${_vstart} -1 _value)
        string(REGEX REPLACE "[:-]" "_" _key "${_key}")
        set(_fuse_slang_pin_${_key} "${_value}" PARENT_SCOPE)
    endforeach()
endfunction()

function(_fuse_slang_host_platform out_var)
    set(_platform "")
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$")
        if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
            set(_platform "linux-x86_64")
        elseif(CMAKE_HOST_WIN32)
            set(_platform "windows-x86_64")
        endif()
    endif()
    set(${out_var} "${_platform}" PARENT_SCOPE)
endfunction()

# Runs `slangc -version`; sets <out_var> to the version string or "" when it does not run.
function(_fuse_slang_probe exe out_var)
    set(_version "")
    if(exe AND EXISTS "${exe}")
        execute_process(COMMAND "${exe}" -version
            OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE TIMEOUT 60)
        if(_rc EQUAL 0)
            # Older releases print the version on stderr.
            string(STRIP "${_out}${_err}" _version)
            string(REGEX MATCH "[0-9]+\\.[0-9]+(\\.[0-9]+)?" _version "${_version}")
        endif()
    endif()
    set(${out_var} "${_version}" PARENT_SCOPE)
endfunction()

function(_fuse_slang_default_cache_root out_var)
    if(FUSE_SLANG_CACHE_DIR)
        set(_root "${FUSE_SLANG_CACHE_DIR}")
    elseif(CMAKE_HOST_WIN32 AND DEFINED ENV{LOCALAPPDATA})
        file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/fuse/slang" _root)
    elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
        set(_root "$ENV{XDG_CACHE_HOME}/fuse/slang")
    elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
        set(_root "$ENV{HOME}/.cache/fuse/slang")
    else()
        # Still outside the source tree; build trees are git-ignored.
        set(_root "${CMAKE_BINARY_DIR}/_fuse_slang")
    endif()
    set(${out_var} "${_root}" PARENT_SCOPE)
endfunction()

# Downloads + verifies + unpacks the pinned archive into <root>/<version>. Sets <out_var> to the
# slangc path on success, "" otherwise (reason in a STATUS message).
function(_fuse_slang_fetch root platform out_var)
    set(${out_var} "" PARENT_SCOPE)
    string(REPLACE "-" "_" _key "${platform}")
    set(_file "${_fuse_slang_pin_download_${_key}}")
    set(_sha "${_fuse_slang_pin_download_sha256_${_key}}")
    set(_ver "${_fuse_slang_pin_version}")
    if(NOT _file OR NOT _sha OR NOT _ver)
        message(STATUS "FUSE: Slang pin has no download for ${platform}")
        return()
    endif()
    set(_exe_name "slangc")
    if(platform MATCHES "^windows")
        set(_exe_name "slangc.exe")
    endif()
    set(_dest "${root}/${_ver}")
    file(MAKE_DIRECTORY "${root}")
    # Serialise concurrent configures (several build trees share one cache root).
    file(LOCK "${root}/.fuse_slang.lock" GUARD FUNCTION TIMEOUT 900 RESULT_VARIABLE _lock_rc)
    if(NOT _lock_rc EQUAL 0)
        message(STATUS "FUSE: could not lock ${root} (${_lock_rc}); skipping Slang download")
        return()
    endif()
    if(EXISTS "${_dest}/bin/${_exe_name}")
        set(${out_var} "${_dest}/bin/${_exe_name}" PARENT_SCOPE)
        return()
    endif()

    set(_url "${_fuse_slang_pin_upstream}/releases/download/${_fuse_slang_pin_tag}/${_file}")
    set(_archive "${root}/${_file}")
    message(STATUS "FUSE: downloading ${_url}")
    file(DOWNLOAD "${_url}" "${_archive}" EXPECTED_HASH SHA256=${_sha} TLS_VERIFY ON
         INACTIVITY_TIMEOUT 120 STATUS _status)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
        list(GET _status 1 _msg)
        file(REMOVE "${_archive}")
        message(STATUS "FUSE: Slang download failed (${_code}: ${_msg})")
        return()
    endif()

    # Unpack beside the destination, then rename: an interrupted unpack never looks installed.
    set(_staging "${root}/.staging-${_ver}")
    file(REMOVE_RECURSE "${_staging}")
    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_staging}")
    file(REMOVE "${_archive}")
    if(NOT EXISTS "${_staging}/bin/${_exe_name}")
        file(REMOVE_RECURSE "${_staging}")
        message(STATUS "FUSE: Slang archive ${_file} has no bin/${_exe_name}")
        return()
    endif()
    if(NOT FUSE_SLANG_KEEP_LLVM)
        file(GLOB _llvm "${_staging}/lib/libslang-llvm*" "${_staging}/bin/slang-llvm*")
        if(_llvm)
            file(REMOVE ${_llvm})
        endif()
    endif()
    file(RENAME "${_staging}" "${_dest}")
    set(${out_var} "${_dest}/bin/${_exe_name}" PARENT_SCOPE)
endfunction()

function(_fuse_slang_resolve)
    set(FUSE_SLANG_FOUND FALSE CACHE INTERNAL "")
    set(FUSE_SLANGC_EXECUTABLE "" CACHE INTERNAL "")
    set(FUSE_SLANGC_VERSION "" CACHE INTERNAL "")
    _fuse_slang_read_pin()
    set(FUSE_SLANG_PIN_VERSION "${_fuse_slang_pin_version}" CACHE INTERNAL "")

    string(TOUPPER "${FUSE_SLANG}" _mode)
    if(_mode STREQUAL "OFF" OR _mode STREQUAL "FALSE" OR _mode STREQUAL "0")
        message(STATUS "FUSE: Slang disabled (FUSE_SLANG=OFF)")
        return()
    endif()

    set(_exe "")
    set(_how "")
    # 1. Explicit override.
    if(FUSE_SLANGC)
        set(_exe "${FUSE_SLANGC}")
        set(_how "FUSE_SLANGC")
    endif()
    # 2. Pinned release, already unpacked or downloaded now.
    _fuse_slang_host_platform(_platform)
    if(NOT _exe AND _platform AND _fuse_slang_pin_version)
        _fuse_slang_default_cache_root(_root)
        set(_exe_name "slangc")
        if(CMAKE_HOST_WIN32)
            set(_exe_name "slangc.exe")
        endif()
        if(EXISTS "${_root}/${_fuse_slang_pin_version}/bin/${_exe_name}")
            set(_exe "${_root}/${_fuse_slang_pin_version}/bin/${_exe_name}")
            set(_how "pinned ${_fuse_slang_pin_version} (cached)")
        elseif(FUSE_SLANG_DOWNLOAD)
            _fuse_slang_fetch("${_root}" "${_platform}" _exe)
            if(_exe)
                set(_how "pinned ${_fuse_slang_pin_version} (downloaded)")
            endif()
        endif()
    endif()
    # 3. PATH, accepted only when it is exactly the pinned version.
    if(NOT _exe)
        find_program(_fuse_slangc_on_path NAMES slangc NO_CACHE)
        if(_fuse_slangc_on_path)
            _fuse_slang_probe("${_fuse_slangc_on_path}" _path_version)
            if(_path_version STREQUAL _fuse_slang_pin_version)
                set(_exe "${_fuse_slangc_on_path}")
                set(_how "PATH")
            else()
                message(STATUS "FUSE: ignoring ${_fuse_slangc_on_path} (version '${_path_version}', "
                               "pin ${_fuse_slang_pin_version}); set FUSE_SLANGC to force it")
            endif()
        endif()
    endif()

    _fuse_slang_probe("${_exe}" _version)
    if(NOT _version)
        set(_reason "no slangc")
        if(_exe)
            set(_reason "${_exe} does not run")
        elseif(NOT _platform)
            set(_reason "no pinned slangc for ${CMAKE_HOST_SYSTEM_NAME}/${CMAKE_HOST_SYSTEM_PROCESSOR}")
        endif()
        if(_mode STREQUAL "ON")
            message(FATAL_ERROR "FUSE: FUSE_SLANG=ON but ${_reason}. Set FUSE_SLANGC or allow "
                                "FUSE_SLANG_DOWNLOAD.")
        endif()
        message(STATUS "FUSE: Slang unavailable (${_reason}); Slang shaders disabled, their tests skip")
        return()
    endif()
    if(NOT _version STREQUAL _fuse_slang_pin_version)
        message(WARNING "FUSE: slangc ${_version} differs from the pin ${_fuse_slang_pin_version}")
    endif()
    get_filename_component(_exe "${_exe}" ABSOLUTE)
    set(FUSE_SLANG_FOUND TRUE CACHE INTERNAL "")
    set(FUSE_SLANGC_EXECUTABLE "${_exe}" CACHE INTERNAL "")
    set(FUSE_SLANGC_VERSION "${_version}" CACHE INTERNAL "")
    message(STATUS "FUSE: slangc ${_version} via ${_how}: ${_exe}")
endfunction()

_fuse_slang_resolve()

function(fuse_add_slang_shaders target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "NO_SPIRV_VAL" "OUTPUT_DIR;ENTRY;STAGE;SUFFIX;OUTPUT_VAR"
                          "SOURCES;DEFINES;INCLUDE_DIRS;FLAGS")
    if(ARG_OUTPUT_VAR)
        set(${ARG_OUTPUT_VAR} "" PARENT_SCOPE)
    endif()
    if(NOT FUSE_SLANG_FOUND)
        return()
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR "fuse_add_slang_shaders: '${target}' is not a target")
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "fuse_add_slang_shaders(${target}): SOURCES is required")
    endif()
    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/slang/${target}")
    endif()
    if(NOT ARG_ENTRY)
        set(ARG_ENTRY "main")
    endif()
    file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")

    set(_common ${FUSE_SLANG_SPIRV_FLAGS} -entry ${ARG_ENTRY})
    if(ARG_STAGE)
        list(APPEND _common -stage ${ARG_STAGE})
    endif()
    foreach(_define IN LISTS ARG_DEFINES)
        list(APPEND _common "-D${_define}")
    endforeach()
    foreach(_dir IN LISTS ARG_INCLUDE_DIRS)
        get_filename_component(_dir "${_dir}" ABSOLUTE)
        list(APPEND _common -I "${_dir}")
    endforeach()
    list(APPEND _common ${ARG_FLAGS})

    # Ninja, Makefile and Visual Studio generators honour DEPFILE (CMake >= 3.21); others fall
    # back to depending on every .slang file next to the source.
    set(_use_depfile FALSE)
    if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles|Visual Studio")
        set(_use_depfile TRUE)
    endif()

    set(_outputs "")
    foreach(_src IN LISTS ARG_SOURCES)
        get_filename_component(_src "${_src}" ABSOLUTE)
        get_filename_component(_stem "${_src}" NAME_WE)
        set(_base "${ARG_OUTPUT_DIR}/${_stem}${ARG_SUFFIX}")
        set(_spv "${_base}.spv")
        set(_json "${_base}.reflection.json")
        set(_dep "${_base}.d")
        set(_cmds COMMAND "${FUSE_SLANGC_EXECUTABLE}" "${_src}" ${_common}
                  -o "${_spv}" -reflection-json "${_json}" -depfile "${_dep}")
        if(FUSE_SPIRV_VAL AND NOT ARG_NO_SPIRV_VAL)
            list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_spv}")
        endif()
        set(_extra "")
        if(_use_depfile)
            set(_extra DEPFILE "${_dep}")
            set(_deps "${_src}")
        else()
            get_filename_component(_src_dir "${_src}" DIRECTORY)
            file(GLOB _deps "${_src_dir}/*.slang")
        endif()
        add_custom_command(OUTPUT "${_spv}" "${_json}"
            ${_cmds}
            DEPENDS ${_deps} "${FUSE_SLANGC_EXECUTABLE}"
            ${_extra}
            COMMENT "slangc ${_stem}.slang${ARG_SUFFIX} -> SPIR-V"
            VERBATIM)
        list(APPEND _outputs "${_spv}")
    endforeach()

    get_property(_counter TARGET ${target} PROPERTY FUSE_SLANG_SHADER_SETS)
    if(NOT _counter)
        set(_counter 0)
    endif()
    math(EXPR _counter "${_counter} + 1")
    set_property(TARGET ${target} PROPERTY FUSE_SLANG_SHADER_SETS ${_counter})
    add_custom_target(${target}_slang_${_counter} DEPENDS ${_outputs})
    add_dependencies(${target} ${target}_slang_${_counter})
    if(ARG_OUTPUT_VAR)
        set(${ARG_OUTPUT_VAR} "${_outputs}" PARENT_SCOPE)
    endif()
endfunction()
