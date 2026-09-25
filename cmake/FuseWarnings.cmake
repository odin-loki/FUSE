# -----------------------------------------------------------------------------
# FUSE compiler warnings — master plan rows "CMake builds cleanly in Debug,
# Release, Profile, Shipping — zero warnings with -Wall -Wextra" and "Shipping
# build compiles with zero warnings".
#
#   FUSE_WARNINGS=ON (default)          -Wall -Wextra (GNU/Clang), /W3 (MSVC), /W4 (clang-cl)
#   FUSE_WARNINGS_AS_ERRORS=OFF (default) adds -Werror (/WX); ON in the CI
#                                        fuse-warnings-as-errors job/presets.
#
# MSVC-style front ends (cl.exe and clang-cl; CMake's MSVC variable):
#   * clang-cl gets /W4 — clang-cl maps /W4 to -Wall -Wextra, while a literal -Wall (or /Wall)
#     there means -Weverything, so the GNU branch must never apply to it.
#   * cl.exe gets /W3 minus the level-2/3 conversion warnings GCC/Clang only emit under
#     -Wconversion (not part of -Wall -Wextra): C4244/C4267 (integer/float narrowing), C4305
#     (double -> float truncation), C4146 (unary minus on unsigned, well-defined modular math).
#   * Every FUSE target (whatever FUSE_WARNINGS says) gets NOMINMAX (windows.h min/max macros
#     break std::min/std::max/numeric_limits::max) and _CRT_SECURE_NO_WARNINGS /
#     _CRT_NONSTDC_NO_WARNINGS (C4996 on fopen/getenv/strncpy/POSIX names — portable ISO C
#     calls, not deprecated anywhere else), plus the conformance options GCC/Clang behave like by
#     default: /utf-8 (sources and literals are UTF-8; the runner code page would otherwise
#     re-encode them and raise C4819), /permissive- (two-phase lookup, and/or/not tokens even in
#     C++17 targets), /Zc:__cplusplus (honest __cplusplus) and /bigobj (large test TUs).
#
# Applied once, at the end of the top-level CMakeLists (fuse_warnings_finalize),
# to every buildsystem target whose SOURCE_DIR lives under Source/FUSE or
# Tools/FUSE. Vendored third-party subprojects (Engine/lib/*) are not touched,
# and third-party headers must be reached through SYSTEM include directories so
# their diagnostics stay out of FUSE builds. Vendored C/C++ sources compiled
# directly into a FUSE target (outside Source/FUSE and Tools/FUSE) get the
# warning flags removed per source file (-w) — they are not FUSE code.
# -----------------------------------------------------------------------------
option(FUSE_WARNINGS "Compile Source/FUSE + Tools/FUSE targets with -Wall -Wextra (MSVC /W3)" ON)
option(FUSE_WARNINGS_AS_ERRORS "Treat warnings in Source/FUSE + Tools/FUSE targets as errors (-Werror / /WX)" OFF)

function(_fuse_warnings_collect dir out_targets)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    set(_all ${_targets})
    foreach(_sd IN LISTS _subdirs)
        _fuse_warnings_collect("${_sd}" _t)
        list(APPEND _all ${_t})
    endforeach()
    set(${out_targets} ${_all} PARENT_SCOPE)
endfunction()

function(_fuse_warnings_is_fuse_path path out)
    set(_ok OFF)
    foreach(_root "${CMAKE_SOURCE_DIR}/Source/FUSE" "${CMAKE_SOURCE_DIR}/Tools/FUSE"
                  "${CMAKE_BINARY_DIR}/Source/FUSE" "${CMAKE_BINARY_DIR}/Tools/FUSE")
        string(FIND "${path}/" "${_root}/" _pos)
        if(_pos EQUAL 0)
            set(_ok ON)
        endif()
    endforeach()
    set(${out} ${_ok} PARENT_SCOPE)
endfunction()

# Call once at the end of the top-level CMakeLists.txt.
function(fuse_warnings_finalize)
    set(_flags)
    set(_defs)
    set(_conformance)
    set(_vendored_flags)
    if(MSVC)
        # cl.exe or clang-cl (both take MSVC-style options; see the header comment).
        set(_defs NOMINMAX _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
        set(_conformance
            $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8> $<$<COMPILE_LANGUAGE:C,CXX>:/bigobj>
            $<$<COMPILE_LANGUAGE:CXX>:/permissive-> $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)
        if(FUSE_WARNINGS)
            if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
                list(APPEND _flags /W4)
            else()
                list(APPEND _flags /W3 /wd4244 /wd4267 /wd4305 /wd4146)
            endif()
        endif()
        if(FUSE_WARNINGS_AS_ERRORS)
            list(APPEND _flags /WX)
        endif()
        set(_vendored_flags /W0)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        if(FUSE_WARNINGS)
            list(APPEND _flags -Wall -Wextra)
        endif()
        if(FUSE_WARNINGS_AS_ERRORS)
            list(APPEND _flags -Werror)
        endif()
        set(_vendored_flags -w)
    elseif(FUSE_WARNINGS OR FUSE_WARNINGS_AS_ERRORS)
        message(STATUS "FUSE: FUSE_WARNINGS ignored on ${CMAKE_CXX_COMPILER_ID}")
    endif()
    if(NOT _flags AND NOT _defs)
        return()
    endif()

    _fuse_warnings_collect("${CMAKE_SOURCE_DIR}" _targets)
    set(_count 0)
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(NOT _type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
            continue()
        endif()
        get_target_property(_imported ${_t} IMPORTED)
        if(_imported)
            continue()
        endif()
        get_target_property(_sdir ${_t} SOURCE_DIR)
        _fuse_warnings_is_fuse_path("${_sdir}" _is_fuse)
        if(NOT _is_fuse)
            continue()
        endif()
        if(_defs)
            target_compile_definitions(${_t} PRIVATE ${_defs})
            target_compile_options(${_t} PRIVATE ${_conformance})
        endif()
        if(NOT _flags)
            continue()
        endif()
        target_compile_options(${_t} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:${_flags}>)
        math(EXPR _count "${_count} + 1")

        # Vendored sources compiled straight into a FUSE target (e.g. bundled Lua).
        get_target_property(_sources ${_t} SOURCES)
        foreach(_src IN LISTS _sources)
            if(_src MATCHES "^\\$<")
                continue()
            endif()
            if(NOT _src MATCHES "\\.(c|cc|cpp|cxx)$")
                continue()
            endif()
            if(IS_ABSOLUTE "${_src}")
                set(_abs "${_src}")
            else()
                set(_abs "${_sdir}/${_src}")
            endif()
            cmake_path(NORMAL_PATH _abs)
            _fuse_warnings_is_fuse_path("${_abs}" _src_is_fuse)
            if(NOT _src_is_fuse)
                set_property(SOURCE "${_abs}" TARGET_DIRECTORY ${_t}
                    APPEND PROPERTY COMPILE_OPTIONS ${_vendored_flags})
            endif()
        endforeach()
    endforeach()
    if(NOT _flags)
        return()
    endif()
    set(_mode "${_flags}")
    message(STATUS "FUSE: warnings (${_mode}) applied to ${_count} Source/FUSE + Tools/FUSE targets")
endfunction()
