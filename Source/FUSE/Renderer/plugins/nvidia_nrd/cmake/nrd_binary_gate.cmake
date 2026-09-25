# ctest fuse_rp_nrd_no_committed_binaries (WP-6.4b): NVIDIA NRD binaries and headers are RTX-SDK-licensed and must
# never enter the (MIT) repository; FUSE loads a developer-built provider at run time.
#
# 1. self-test: the matcher flags every seeded bad path and none of the seeded good ones;
# 2. fails if `git ls-files` (committed or staged) contains a matching file.
# Prints NRD_BINARY_GATE_SKIP (ctest SKIP_REGULAR_EXPRESSION) when git or a work tree is missing.
#
#   cmake -DREPO=<source dir> -P <this file>

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED REPO)
    message(FATAL_ERROR "NRD_BINARY_GATE: pass -DREPO=<repository root>")
endif()

# Case-insensitive path regexes (applied to lower-cased repo-relative paths).
set(_patterns
    "(^|/)(lib)?nrd\\.(dll|so|lib|a|pdb|dylib)(\\.[0-9.]+)?$"   # NRD runtime / static library (NVIDIA RTX SDKs License)
    "(^|/)nrd(descs|settings|wrapper)?\\.h$"                  # NRD public headers (not vendored)
    "(^|/)nrd\\.hlsli$"
    "(^|/)(lib)?fuse_nrdplugin_nri[^/]*\\.(dll|so|dylib)$"    # FUSE provider binary links NRD: never committed
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

foreach(_bad IN ITEMS "bin/NRD.dll" "lib/libNRD.so" "lib/NRD.lib" "include/NRD.h" "include/NRDDescs.h" "include/NRDSettings.h" "shaders/NRD.hlsli" "out/libfuse_nrdplugin_nri.so")
    _gate_match("${_bad}" _hit)
    if(NOT _hit)
        message(FATAL_ERROR "NRD_BINARY_GATE self-test: '${_bad}' not flagged")
    endif()
endforeach()
foreach(_good IN ITEMS "plugins/nvidia_nrd/include/fuse/renderer/nrd/nrd_denoiser.hpp" "plugins/nvidia_nrd/include/fuse/renderer/nrd/fuse_nrd_plugin_abi.h" "plugins/nvidia_nrd/mock/fuse_nrdplugin_mock.cpp")
    _gate_match("${_good}" _hit)
    if(_hit)
        message(FATAL_ERROR "NRD_BINARY_GATE self-test: '${_good}' wrongly flagged")
    endif()
endforeach()

find_program(_git git)
if(NOT _git OR NOT EXISTS "${REPO}/.git")
    message("NRD_BINARY_GATE_SKIP: no git or no work tree at ${REPO}")
    return()
endif()
execute_process(COMMAND "${_git}" -C "${REPO}" ls-files RESULT_VARIABLE _rc OUTPUT_VARIABLE _files ERROR_QUIET)
if(NOT _rc EQUAL 0)
    message("NRD_BINARY_GATE_SKIP: git ls-files failed (${_rc})")
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
    message(FATAL_ERROR "NRD_BINARY_GATE: licensed vendor files are tracked by git:\n  ${_msg}")
endif()
message(STATUS "NRD_BINARY_GATE: ok (self-test + no tracked vendor files)")
