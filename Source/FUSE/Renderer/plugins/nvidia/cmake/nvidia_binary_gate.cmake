# ctest fuse_nvidia_no_committed_binaries (docs/nvidia-plugin.md "Licensing").
#
# NVIDIA's DLSS / NGX / Streamline-plugin runtime binaries are licensed under the NVIDIA RTX SDKs
# License: redistributable only in object form inside an application for NVIDIA GPUs, and never under
# an open-source licence. FUSE is MIT, so none of them may enter the repository. This gate:
#   1. self-test: the matcher flags every seeded bad path and none of the seeded good ones;
#   2. fails if `git ls-files` (committed or staged) contains a matching file;
#   3. fails if .gitignore does not ignore the canonical file names (git check-ignore --no-index).
# Prints NVIDIA_BINARY_GATE_SKIP (ctest SKIP_REGULAR_EXPRESSION) when git or a work tree is missing.
#
#   cmake -DREPO=<source dir> -P nvidia_binary_gate.cmake

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED REPO)
    message(FATAL_ERROR "nvidia_binary_gate: pass -DREPO=<repository root>")
endif()

# Case-insensitive path regexes (applied to lower-cased repo-relative paths).
set(_patterns
    "(^|/)nvngx[^/]*\\.(dll|bin)$"                 # nvngx_dlss.dll, nvngx_dlssd.dll, nvngx_dlssg.dll
    "(^|/)libnvidia-ngx[^/]*\\.so(\\.[0-9.]+)?$"   # libnvidia-ngx-dlss.so.310.9.1 (DLSS SDK Linux)
    "(^|/)libnvsdk_ngx[^/]*\\.a$"                  # NGX static loader (Linux)
    "(^|/)nvsdk_ngx[^/]*\\.lib$"                   # NGX import/static libs (Windows)
    "(^|/)sl\\.[^/]*\\.(dll|pdb|so)$"              # sl.interposer.dll, sl.dlss.dll, sl.dlss_g.dll, sl.common.dll ...
    "(^|/)nvperf_grfx_target[^/]*\\.(dll|so)$"     # Nsight Perf (sl.nvperf dependency)
    "(^|/)libfuse_nvplugin_ngx[^/]*\\.so$"         # FUSE NGX bridge: statically links NVIDIA's NGX loader
    "(^|/)fuse_nvplugin_ngx[^/]*\\.dll$"
)

function(_nv_match path out)
    string(TOLOWER "${path}" _p)
    set(_hit FALSE)
    foreach(_re IN LISTS _patterns)
        if(_p MATCHES "${_re}")
            set(_hit TRUE)
        endif()
    endforeach()
    set(${out} ${_hit} PARENT_SCOPE)
endfunction()

# 1. Self-test.
set(_bad
    "bin/nvngx_dlss.dll" "heritage/torque3d/bin/NVNGX_DLSSD.DLL" "nvngx_dlssg.dll" "x/nvngx_dlss_310.bin"
    "lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.1" "libnvidia-ngx-dlssg.so" "lib/libnvsdk_ngx.a"
    "lib/Windows_x86_64/nvsdk_ngx_d.lib" "sdk/sl.interposer.dll" "plugins/sl.dlss_g.dll" "sl.common.pdb"
    "tools/nvperf_grfx_target.dll" "out/libfuse_nvplugin_ngx.so")
set(_good
    "third_party/vendor/streamline/include/sl.h" "third_party/vendor/streamline/include/sl_dlss.h" "third_party/vendor/streamline/VERSION"
    "Source/FUSE/Renderer/plugins/nvidia/mock/fuse_nvplugin_mock.cpp" "docs/nvidia-plugin.md"
    "third_party/vendor/nvidia-nis/NIS/NIS_Scaler.h" "slides/sl.dlss.txt" "libfuse_nvplugin_mock.so" "d3dcompiler_47.dll"
    "Source/FUSE/Renderer/plugins/nvidia/providers/ngx/fuse_nvplugin_ngx.cpp")
set(_selftest_errors 0)
foreach(_p IN LISTS _bad)
    _nv_match("${_p}" _hit)
    if(NOT _hit)
        message(SEND_ERROR "self-test: seeded NVIDIA binary not flagged: ${_p}")
        math(EXPR _selftest_errors "${_selftest_errors} + 1")
    endif()
endforeach()
foreach(_p IN LISTS _good)
    _nv_match("${_p}" _hit)
    if(_hit)
        message(SEND_ERROR "self-test: allowed path flagged: ${_p}")
        math(EXPR _selftest_errors "${_selftest_errors} + 1")
    endif()
endforeach()
if(_selftest_errors GREATER 0)
    message(FATAL_ERROR "nvidia_binary_gate: matcher self-test failed (${_selftest_errors})")
endif()
list(LENGTH _bad _nbad)
list(LENGTH _good _ngood)
message(STATUS "self-test: ${_nbad} seeded binaries flagged, ${_ngood} allowed paths pass")

# 2. Tracked files.
find_program(_git git)
if(NOT _git)
    message(STATUS "NVIDIA_BINARY_GATE_SKIP: git not found")
    return()
endif()
execute_process(COMMAND "${_git}" -C "${REPO}" rev-parse --is-inside-work-tree
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _rc EQUAL 0 OR NOT _out STREQUAL "true")
    message(STATUS "NVIDIA_BINARY_GATE_SKIP: ${REPO} is not a git work tree")
    return()
endif()
execute_process(COMMAND "${_git}" -C "${REPO}" -c core.quotepath=off ls-files --cached
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _files ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "git ls-files failed: ${_err}")
endif()
string(REPLACE ";" "\\;" _files "${_files}")
string(REPLACE "\n" ";" _files "${_files}")
set(_violations "")
set(_count 0)
foreach(_f IN LISTS _files)
    if(_f STREQUAL "")
        continue()
    endif()
    math(EXPR _count "${_count} + 1")
    _nv_match("${_f}" _hit)
    if(_hit)
        list(APPEND _violations "${_f}")
    endif()
endforeach()
if(_count LESS 100)
    message(FATAL_ERROR "git ls-files listed only ${_count} files (wrong REPO?)")
endif()

# 3. .gitignore keeps the canonical names out.
set(_must_ignore
    "nvngx_dlss.dll" "bin/nvngx_dlssg.dll" "libnvidia-ngx-dlss.so.310.9.1" "lib/libnvsdk_ngx.a"
    "sl.interposer.dll" "plugins/sl.dlss.dll" "nvsdk_ngx_d.lib" "libfuse_nvplugin_ngx.so")
set(_not_ignored "")
foreach(_n IN LISTS _must_ignore)
    execute_process(COMMAND "${_git}" -C "${REPO}" check-ignore -q --no-index "${_n}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        list(APPEND _not_ignored "${_n}")
    endif()
endforeach()

if(_violations OR _not_ignored)
    foreach(_v IN LISTS _violations)
        message(SEND_ERROR "NVIDIA RTX SDK binary is tracked by git (remove it; RTX SDKs License): ${_v}")
    endforeach()
    foreach(_v IN LISTS _not_ignored)
        message(SEND_ERROR ".gitignore does not ignore NVIDIA runtime file name: ${_v}")
    endforeach()
    message(FATAL_ERROR "nvidia_binary_gate: FAILED")
endif()
message(STATUS "nvidia_binary_gate: ${_count} tracked files scanned, no NVIDIA runtime binaries; .gitignore covers ${_must_ignore}")
