# ctest fuse_rp_xess_no_committed_binaries (WP-4.3): Intel XeSS runtime binaries and SDK headers are under the
# Intel Simplified Software License and must never enter the (MIT) repository; FUSE loads the user's libxess at run time.
#
# 1. self-test: the matcher flags every seeded bad path and none of the seeded good ones;
# 2. fails if `git ls-files` (committed or staged) contains a matching file.
# Prints XESS_BINARY_GATE_SKIP (ctest SKIP_REGULAR_EXPRESSION) when git or a work tree is missing.
#
#   cmake -DREPO=<source dir> -P <this file>

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED REPO)
    message(FATAL_ERROR "XESS_BINARY_GATE: pass -DREPO=<repository root>")
endif()

# Case-insensitive path regexes (applied to lower-cased repo-relative paths).
set(_patterns
    "(^|/)libxess[^/]*\\.(dll|so|lib|pdb|dylib)(\\.[0-9.]+)?$"   # the XeSS runtime (Intel Simplified Software License)
    "(^|/)libxell[^/]*\\.(dll|so|lib)$"                        # XeLL (latency), same licence
    "(^|/)libxess_fg[^/]*\\.(dll|so|lib)$"                     # XeSS frame generation
    "(^|/)xess(_vk|_d3d12|_d3d11|_debug)?\\.h$"                # SDK headers (not redistributable here)
)

function(_gate_match path out)
    string(TOLOWER "${path}" _p)
    set(_hit FALSE)
    foreach(_re IN LISTS _patterns)
        if(_p MATCHES "${_re}")
            set(_hit TRUE)
        endif()
    endforeach()
    set(${out} ${_hit} PARENT_SCOPE)
endfunction()

foreach(_bad IN ITEMS "bin/libxess.dll" "x/libxess.so" "lib/libxess.lib" "inc/xess/xess.h" "inc/xess/xess_vk.h" "a/libxell.dll")
    _gate_match("${_bad}" _hit)
    if(NOT _hit)
        message(FATAL_ERROR "XESS_BINARY_GATE self-test: '${_bad}' not flagged")
    endif()
endforeach()
foreach(_good IN ITEMS "plugins/intel_xess/include/fuse/renderer/xess/xess_api.h" "plugins/intel_xess/mock/fuse_xess_mock.cpp" "docs/xess.md")
    _gate_match("${_good}" _hit)
    if(_hit)
        message(FATAL_ERROR "XESS_BINARY_GATE self-test: '${_good}' wrongly flagged")
    endif()
endforeach()

find_program(_git git)
if(NOT _git OR NOT EXISTS "${REPO}/.git")
    message("XESS_BINARY_GATE_SKIP: no git or no work tree at ${REPO}")
    return()
endif()
execute_process(COMMAND "${_git}" -C "${REPO}" ls-files RESULT_VARIABLE _rc OUTPUT_VARIABLE _files ERROR_QUIET)
if(NOT _rc EQUAL 0)
    message("XESS_BINARY_GATE_SKIP: git ls-files failed (${_rc})")
    return()
endif()
string(REPLACE "\n" ";" _files "${_files}")
set(_found "")
foreach(_f IN LISTS _files)
    if(_f STREQUAL "")
        continue()
    endif()
    _gate_match("${_f}" _hit)
    if(_hit)
        list(APPEND _found "${_f}")
    endif()
endforeach()
if(_found)
    list(JOIN _found "\n  " _msg)
    message(FATAL_ERROR "XESS_BINARY_GATE: licensed vendor files are tracked by git:\n  ${_msg}")
endif()
message(STATUS "XESS_BINARY_GATE: ok (self-test + no tracked vendor files)")
