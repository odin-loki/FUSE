# FUSE Relight licence scanner (docs/plans/FUSE_REMIX_PORT_PLAN.md §0.3, §0.4.4, RL-0.1, AD-7, AD-9).
#
# Two modes, one script (run with `cmake -P`, or `include()` with RL_SCAN_LIBRARY_ONLY=ON to get the
# marker tables and matchers without scanning; tests/licence/rl_licence_selftest.cmake does that to
# generate its seeded fixtures from the very same tables):
#
#   MODE=text    Walks every file under ROOTS (default: the Relight trees and the vendored
#                Engine/lib/{dxvk,dxbc-spirv,xxhash}) and fails on
#                  - NVIDIA proprietary licence titles / SPDX ids               (category T)
#                  - NVIDIA proprietary header / EULA phrases                    (category H)
#                  - names and distinctive identifiers of the clean-room files   (category N)
#                    (the RTXDI bridge, RTXCR, NRC and NRD shader files and their public API prefixes)
#                  - MDL source syntax (Remix .mdl bodies are proprietary, AD-7)  (category M)
#                  - LGPL/GPL text and SPDX ids                                  (category L)
#                  - DXVK's LGPL mingw-directx-headers by path/name              (path check)
#   MODE=binary  Checks git-tracked plus untracked-not-ignored files of REPO (or the paths listed in
#                FILE_LIST) against NVIDIA/Intel runtime binary names (extends
#                Renderer/plugins/nvidia/cmake/nvidia_binary_gate.cmake with the §0.4.4 list) and
#                rejects any native binary inside the Relight / vendored-DXVK trees.
#
# Every marker is assembled from fragments so this file (and the self-test) never matches itself.
# No proprietary file content is reproduced here: only licence titles, SPDX ids, file names, public
# API prefixes and generic MDL language syntax.
#
#   cmake -DMODE=text   -DREPO=<repo root> [-DROOTS=<abs dir;...>] -P relight_proprietary_scan.cmake
#   cmake -DMODE=binary -DREPO=<repo root> [-DFILE_LIST=<file with one repo-relative path per line>] -P ...
#
# Output lines "RL_FINDING <id> <path>[:<line>]" are stable (the self-test greps them).
# MODE=binary prints RL_BINARY_GATE_SKIP when git or a work tree is unavailable (ctest skip regex).

cmake_minimum_required(VERSION 3.21)

# --------------------------------------------------------------------------------------------------
# Marker table. Entry fields (parallel lists): id, category, kind, pattern, sample.
#   kind ci = case-insensitive literal (pattern stored lower-case)
#        cs = case-sensitive literal
#        re = case-sensitive CMake regex (sample = a string the regex must match)
# --------------------------------------------------------------------------------------------------
set(RL_MARKER_IDS "")
# `${_}` is empty: it splits every marker literal in this file so the file never matches itself.
set(_ "")
function(_rl_marker id cat kind pattern)
    set(_sample "${pattern}")
    if(ARGC GREATER 4)
        set(_sample "${ARGV4}")
    endif()
    if(kind STREQUAL "ci")
        string(TOLOWER "${pattern}" pattern)
    endif()
    set(RL_MARKER_IDS ${RL_MARKER_IDS} ${id} PARENT_SCOPE)
    set(RL_MARKER_CAT_${id} "${cat}" PARENT_SCOPE)
    set(RL_MARKER_KIND_${id} "${kind}" PARENT_SCOPE)
    set(RL_MARKER_PAT_${id} "${pattern}" PARENT_SCOPE)
    set(RL_MARKER_SAMPLE_${id} "${_sample}" PARENT_SCOPE)
endfunction()

set(_nv "nvi${_}dia")
set(_wb "(^|[^A-Za-z0-9_])")                       # left word boundary (CMake regex has no \b)
set(_we "([^A-Za-z0-9_]|$)")                       # right word boundary

# T: proprietary licence titles and SPDX ids (THIRD_PARTY.md may name them).
_rl_marker(rtx_sdks_licence        T ci "${_nv} rtx sd${_}ks licen" "NVI${_}DIA RTX SD${_}Ks LICENSE")
_rl_marker(spdx_nv_proprietary     T ci "licenseref-${_nv}proprie${_}tary" "SPDX-License-Identifier: LicenseRef-Nvi${_}diaProprie${_}tary")
_rl_marker(nv_sw_licence_agreement T ci "${_nv} software licen${_}se agreement")
_rl_marker(nv_customer_use         T ci "licen${_}se for customer use of ${_nv} software")
_rl_marker(nv_eula                 T ci "${_nv} end user licen${_}se agreement")
_rl_marker(omniverse_licence       T ci "omniverse licen${_}se agreement")
_rl_marker(intel_simplified        T ci "intel simplified softw${_}are licen" "Intel Simplified Softw${_}are License (Version X)")

# H: proprietary header / EULA phrases (never allowed anywhere in the scanned trees).
_rl_marker(hdr_licensors_retain    H ci "and its licensors re${_}tain all intellectual property")
_rl_marker(hdr_express_agreement   H ci "without an express licen${_}se agreement from ${_nv}")
_rl_marker(hdr_proprietary_rights  H ci "proprietary rights in and to this soft${_}ware, related documentation")
_rl_marker(hdr_rtx_sdk_governs     H ci "governs the use of the ${_nv} rtx soft${_}ware development kits")

# N: clean-room file names and distinctive identifiers of the RTXDI / RTXCR / NRC / NRD sources.
_rl_marker(name_rtxdi_bridge       N cs "Rtxdi${_}ApplicationBridge")
_rl_marker(name_rtxcr_material     N cs "rtxcr_mat${_}erial.slangh")
_rl_marker(name_rtxcr              N re "${_wb}rtx${_}cr\\.slangh"   "#include \"rtx${_}cr.slangh\"")
_rl_marker(name_nrc_h              N re "${_wb}NR${_}C\\.h${_we}"    "#include \"NR${_}C.h\"")
_rl_marker(name_nrd_slangh         N re "${_wb}NR${_}D\\.slangh"     "#include \"NR${_}D.slangh\"")
_rl_marker(name_core_definitions   N cs "core_defi${_}nitions.mdl")
_rl_marker(id_rtxdi_api            N re "${_wb}RTX${_}DI_[A-Z][A-Za-z0-9_]*" "int x = RTX${_}DI_ReservoirSample;")
_rl_marker(id_rtxcr_api            N re "${_wb}RTX${_}CR_[A-Z][A-Za-z0-9_]*" "float3 r = RTX${_}CR_SubsurfaceEval();")
_rl_marker(id_rab_bridge           N re "${_wb}RA${_}B_(Surface|LightSample|LightInfo|RandomSamplerState|Material|GetGBuffer)"
    "RA${_}B_Surface s;")
_rl_marker(id_nrd_frontend         N re "${_wb}(NRD|REBLUR|RELAX|SIGMA)_Front${_}End_[A-Za-z]"
    "REBLUR_Front${_}End_PackRadiance(x);")

# M: MDL source syntax (Remix's .mdl bodies are proprietary; FUSE never compiles MDL, AD-7).
_rl_marker(mdl_version_stmt        M re "(^|\n)[ \t]*md${_}l [0-9]+\\.[0-9]+[ \t]*;" "// fixture\nmd${_}l 1.6;\n")
_rl_marker(mdl_nvidia_import       M cs "::${_nv}::core_defi${_}nitions")
_rl_marker(mdl_df_import           M re "import[ \t]+::d${_}f::"      "imp${_}ort ::d${_}f::*;")

# L: LGPL / GPL text (mingw-directx-headers and Wine are LGPL-2.1+; G8: no (L)GPL file in the tree).
_rl_marker(lgpl_text               L ci "gnu lesser gen${_}eral public")
_rl_marker(lgpl_library_text       L ci "gnu library gen${_}eral public")
_rl_marker(gpl_text                L ci "gnu gen${_}eral public licen")
_rl_marker(spdx_gpl                L re "SPDX-License-Iden${_}tifier:[ \t]*L?GPL-" "// SPDX-License-Iden${_}tifier: LGPL-2.1-or-later")

# Path check: DXVK's include/native/directx (mingw-directx-headers, LGPL-2.1+) must never be vendored.
# Relight PE builds use the MinGW toolchain's own headers; native unit builds fetch them into build/.
set(RL_LGPL_HEADER_PATH_RE
    "(^|/)include/native/directx/"
    # Exact mingw-directx-headers file names; DXVK's own zlib files (dxgi_format.h, d3d11_include.h,
    # d3d9_interfaces.h, ...) use a <api>_<topic> naming scheme and must not match.
    "(^|/)(d3d8|d3d8caps|d3d8types|d3d9|d3d9caps|d3d9types|d3dcommon|d3dcompiler|dxgi|dxgi1_[0-9]|dxgicommon|dxgiformat|dxgitype|dxgidebug|d3d10|d3d10_1|d3d10misc|d3d10shader|d3d10effect|d3d11|d3d11_[0-9]|d3d11shader|d3d11sdklayers|d3d12|d3d12sdklayers|d3d12shader)\\.(h|idl)$")

# Per-file allow-list: repo-relative path -> categories allowed there (nominative references only).
# THIRD_PARTY.md must name the licences and the clean-room files it forbids; it may never carry
# header phrases (H), MDL bodies (M) or LGPL text (L).
set(RL_ALLOW_Source/FUSE/Relight/THIRD_PARTY.md "T;N")

# --------------------------------------------------------------------------------------------------
# Binary gate patterns (lower-cased repo-relative paths).
# --------------------------------------------------------------------------------------------------
set(RL_BINARY_PATTERNS
    # Carried over from Renderer/plugins/nvidia/cmake/nvidia_binary_gate.cmake (DLSS/NGX/Streamline).
    "(^|/)nvngx[^/]*\\.(dll|bin)$"
    "(^|/)libnvidia-ngx[^/]*\\.so(\\.[0-9.]+)?$"
    "(^|/)libnvsdk_ngx[^/]*\\.a$"
    "(^|/)nvsdk_ngx[^/]*\\.lib$"
    "(^|/)sl\\.[^/]*\\.(dll|pdb|so)$"
    "(^|/)nvperf_[^/]*\\.(dll|so)$"
    # §0.4.4 additions: NRC, CUDA runtime, NVRTC, RTX IO, NRD/NRI, MDL, Remix runtime/toolkit.
    "(^|/)nrc_[^/]*\\.(dll|so|lib)$"
    "(^|/)(lib)?cudart(64)?[^/]*\\.(dll|so(\\.[0-9.]+)?)$"
    "(^|/)(lib)?nvrtc[^/]*\\.(dll|so(\\.[0-9.]+)?)$"
    "(^|/)(lib)?rtxio[^/]*\\.(dll|so|lib)$"
    "(^|/)(lib)?nr[di]\\.(dll|so|lib)$"
    # *.mdl: Remix's own MDL module names anywhere (assimp's Quake/HL test models share the
    # extension, so the generic *.mdl rule is scoped to the Relight trees below).
    "(^|/)(aperturepbr_[^/]*|core_definitions|opaquepbr[^/]*|translucentpbr[^/]*)\\.mdl$"
    "(^|/)nvremix[^/]*\\.exe$"
    "(^|/)\\.trex/"
    "(^|/)remixapi\\.dll$"
    # Other NVIDIA GameWorks / driver-side runtimes Remix packages (Reflex, Aftermath).
    "(^|/)nvlowlatencyvk[^/]*\\.dll$"
    "(^|/)gfsdk_[^/]*\\.(dll|so|lib)$"
    # Intel XeSS / XeLL (binary-only Intel licence).
    "(^|/)libxe(ss|ll)[^/]*\\.(dll|so|lib)$")
# Any native binary or MDL module inside the Relight / vendored trees (vendored code is source-only).
set(RL_BINARY_SCOPED_ROOTS "source/fuse/relight/" "tests/relight/" "tools/fuse/relight/"
    "engine/lib/dxvk/" "engine/lib/dxbc-spirv/" "engine/lib/xxhash/")
set(RL_BINARY_SCOPED_RE "\\.(dll|so|so\\.[0-9.]+|exe|lib|a|pdb|dylib|obj|o|mdl)$")

function(rl_binary_match path out)
    string(TOLOWER "${path}" _p)
    set(_hit "")
    foreach(_re IN LISTS RL_BINARY_PATTERNS)
        if(_p MATCHES "${_re}")
            set(_hit "${_re}")
            break()
        endif()
    endforeach()
    if(NOT _hit AND _p MATCHES "${RL_BINARY_SCOPED_RE}")
        foreach(_r IN LISTS RL_BINARY_SCOPED_ROOTS)
            string(FIND "${_p}" "${_r}" _pos)
            if(_pos EQUAL 0)
                set(_hit "native binary under ${_r}")
                break()
            endif()
        endforeach()
    endif()
    set(${out} "${_hit}" PARENT_SCOPE)
endfunction()

# Scans one file's text. Appends "<id>|<line>" entries to out (empty when clean).
function(rl_text_match_file file rel out)
    set(_found "")
    set(_allowed "${RL_ALLOW_${rel}}")
    file(READ "${file}" _raw)
    string(TOLOWER "${_raw}" _lc)
    foreach(_id IN LISTS RL_MARKER_IDS)
        if("${RL_MARKER_CAT_${_id}}" IN_LIST _allowed)
            continue()
        endif()
        set(_kind "${RL_MARKER_KIND_${_id}}")
        set(_pos -1)
        if(_kind STREQUAL "ci")
            string(FIND "${_lc}" "${RL_MARKER_PAT_${_id}}" _pos)
        elseif(_kind STREQUAL "cs")
            string(FIND "${_raw}" "${RL_MARKER_PAT_${_id}}" _pos)
        else()
            string(REGEX MATCH "${RL_MARKER_PAT_${_id}}" _m "${_raw}")
            if(NOT _m STREQUAL "")
                string(FIND "${_raw}" "${_m}" _pos)
            endif()
        endif()
        if(_pos GREATER -1)
            string(SUBSTRING "${_raw}" 0 ${_pos} _pre)
            string(REGEX MATCHALL "\n" _nls "${_pre}")
            list(LENGTH _nls _line)
            math(EXPR _line "${_line} + 1")
            list(APPEND _found "${_id}|${_line}")
        endif()
    endforeach()
    set(${out} "${_found}" PARENT_SCOPE)
endfunction()

function(rl_lgpl_header_path_match rel out)
    string(TOLOWER "${rel}" _p)
    set(_hit FALSE)
    foreach(_re IN LISTS RL_LGPL_HEADER_PATH_RE)
        if(_p MATCHES "${_re}")
            set(_hit TRUE)
        endif()
    endforeach()
    set(${out} ${_hit} PARENT_SCOPE)
endfunction()

if(RL_SCAN_LIBRARY_ONLY)
    return()
endif()

# --------------------------------------------------------------------------------------------------
# Main.
# --------------------------------------------------------------------------------------------------
if(NOT DEFINED REPO)
    message(FATAL_ERROR "relight_proprietary_scan: pass -DREPO=<repository root>")
endif()
get_filename_component(REPO "${REPO}" ABSOLUTE)
if(NOT DEFINED MODE)
    set(MODE text)
endif()

if(MODE STREQUAL "text")
    if(NOT DEFINED ROOTS)
        set(ROOTS
            "${REPO}/Source/FUSE/Relight" "${REPO}/Tests/relight" "${REPO}/Tools/FUSE/Relight"
            "${REPO}/Engine/lib/dxvk" "${REPO}/Engine/lib/dxbc-spirv" "${REPO}/Engine/lib/xxhash")
    endif()
    set(_violations 0)
    set(_nfiles 0)
    set(_nroots 0)
    foreach(_root IN LISTS ROOTS)
        if(NOT IS_DIRECTORY "${_root}")
            continue()
        endif()
        math(EXPR _nroots "${_nroots} + 1")
        file(GLOB_RECURSE _files LIST_DIRECTORIES false "${_root}/*")
        foreach(_f IN LISTS _files)
            if(_f MATCHES "/\\.git/")
                continue()
            endif()
            file(RELATIVE_PATH _rel "${REPO}" "${_f}")
            math(EXPR _nfiles "${_nfiles} + 1")
            rl_lgpl_header_path_match("${_rel}" _lgpl_path)
            if(_lgpl_path)
                message("RL_FINDING lgpl_directx_header ${_rel}")
                message(SEND_ERROR "LGPL DirectX header (mingw-directx-headers) must not be vendored: ${_rel}")
                math(EXPR _violations "${_violations} + 1")
            endif()
            file(SIZE "${_f}" _sz)
            if(_sz GREATER 33554432)
                message(WARNING "skipping ${_rel}: ${_sz} bytes (text scan limit 32 MiB)")
                continue()
            endif()
            rl_text_match_file("${_f}" "${_rel}" _hits)
            foreach(_h IN LISTS _hits)
                string(REPLACE "|" ";" _hl "${_h}")
                list(GET _hl 0 _id)
                list(GET _hl 1 _line)
                message("RL_FINDING ${_id} ${_rel}:${_line}")
                message(SEND_ERROR "licence marker '${_id}' (category ${RL_MARKER_CAT_${_id}}) in ${_rel}:${_line}")
                math(EXPR _violations "${_violations} + 1")
            endforeach()
        endforeach()
    endforeach()
    list(LENGTH RL_MARKER_IDS _nm)
    if(_violations GREATER 0)
        message(FATAL_ERROR "relight_proprietary_scan(text): ${_violations} finding(s) in ${_nfiles} files. "
            "NVIDIA proprietary sources are clean-room only (plan §0.3, AD-9); LGPL headers are never vendored.")
    endif()
    message(STATUS "relight_proprietary_scan(text): ${_nfiles} files in ${_nroots} root(s), ${_nm} markers, clean")

elseif(MODE STREQUAL "binary")
    if(DEFINED FILE_LIST)
        file(STRINGS "${FILE_LIST}" _files)
        set(_source "${FILE_LIST}")
    else()
        find_program(_git git)
        if(NOT _git)
            message(STATUS "RL_BINARY_GATE_SKIP: git not found")
            return()
        endif()
        execute_process(COMMAND "${_git}" -C "${REPO}" rev-parse --is-inside-work-tree
            RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _rc EQUAL 0 OR NOT _out STREQUAL "true")
            message(STATUS "RL_BINARY_GATE_SKIP: ${REPO} is not a git work tree")
            return()
        endif()
        # Tracked (committed or staged) plus untracked files git would pick up (not ignored).
        execute_process(COMMAND "${_git}" -C "${REPO}" -c core.quotepath=off ls-files --cached --others --exclude-standard
            RESULT_VARIABLE _rc OUTPUT_VARIABLE _files ERROR_VARIABLE _err)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "git ls-files failed: ${_err}")
        endif()
        string(REPLACE ";" "\\;" _files "${_files}")
        string(REPLACE "\n" ";" _files "${_files}")
        set(_source "git ls-files")
    endif()
    set(_count 0)
    set(_violations 0)
    foreach(_f IN LISTS _files)
        if(_f STREQUAL "")
            continue()
        endif()
        math(EXPR _count "${_count} + 1")
        rl_binary_match("${_f}" _hit)
        if(_hit)
            message("RL_FINDING binary ${_f}")
            message(SEND_ERROR "proprietary runtime binary / native binary in vendored tree (${_hit}): ${_f}")
            math(EXPR _violations "${_violations} + 1")
        endif()
    endforeach()
    if(NOT DEFINED FILE_LIST AND _count LESS 100)
        message(FATAL_ERROR "git ls-files listed only ${_count} files (wrong REPO?)")
    endif()
    if(_violations GREATER 0)
        message(FATAL_ERROR "relight_proprietary_scan(binary): ${_violations} forbidden file(s) of ${_count}. "
            "NVIDIA RTX SDK / Intel XeSS runtimes are user-installed plugins only (plan §0.3, §0.4.4).")
    endif()
    message(STATUS "relight_proprietary_scan(binary): ${_count} paths from ${_source}, no proprietary runtime binaries")
else()
    message(FATAL_ERROR "relight_proprietary_scan: MODE must be text or binary (got '${MODE}')")
endif()
