# Self-validating driver for the Relight licence gates (plan RL-0.1, §0.4.4). ctest:
#   rl_licence_text_scan  -> MODE=text
#   rl_binary_gate        -> MODE=binary
#
# 1. Generates seeded fixtures in SCRATCH at test time from the scanner's own marker / pattern
#    tables (nothing proprietary is committed: samples are licence titles, file names and API
#    prefixes, assembled from fragments inside relight_proprietary_scan.cmake).
# 2. Runs the scanner on a bad fixture tree: it must fail and report exactly the seeded findings.
# 3. Runs it on a good fixture tree of near misses: it must pass with no findings.
# 4. Runs it on the real repository: it must pass.
#
#   cmake -DMODE=text|binary -DREPO=<repo root> -DSCRATCH=<scratch dir> -P rl_licence_selftest.cmake

cmake_minimum_required(VERSION 3.21)

foreach(_v MODE REPO SCRATCH)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "rl_licence_selftest: pass -D${_v}=...")
    endif()
endforeach()
get_filename_component(_scan "${CMAKE_CURRENT_LIST_DIR}/../../cmake/relight_proprietary_scan.cmake" ABSOLUTE)
set(RL_SCAN_LIBRARY_ONLY ON)
include("${_scan}")

set(_errors 0)
macro(_fail msg)
    message(SEND_ERROR "${msg}")
    math(EXPR _errors "${_errors} + 1")
endmacro()

# Runs the scanner; sets _rc, _out and _findings (list of "<id> <path>" with any ":line" stripped).
function(_run_scan out_rc out_text out_findings)
    execute_process(COMMAND "${CMAKE_COMMAND}" ${ARGN} -P "${_scan}"
        RESULT_VARIABLE _r OUTPUT_VARIABLE _o ERROR_VARIABLE _o)
    string(REGEX MATCHALL "RL_FINDING [^\n]*" _lines "${_o}")
    set(_f "")
    foreach(_l IN LISTS _lines)
        string(REGEX REPLACE "^RL_FINDING " "" _l "${_l}")
        string(REGEX REPLACE ":[0-9]+$" "" _l "${_l}")
        list(APPEND _f "${_l}")
    endforeach()
    set(${out_rc} "${_r}" PARENT_SCOPE)
    set(${out_text} "${_o}" PARENT_SCOPE)
    set(${out_findings} "${_f}" PARENT_SCOPE)
endfunction()

# Compares expected vs actual findings (both lists of "<id> <path>").
macro(_expect_findings label expected actual)
    foreach(_e IN LISTS ${expected})
        if(NOT "${_e}" IN_LIST ${actual})
            _fail("${label}: seeded violation not reported: ${_e}")
        endif()
    endforeach()
    foreach(_a IN LISTS ${actual})
        if(NOT "${_a}" IN_LIST ${expected})
            _fail("${label}: unexpected finding: ${_a}")
        endif()
    endforeach()
endmacro()

file(REMOVE_RECURSE "${SCRATCH}")
set(_rl "Source/FUSE/Relight")

if(MODE STREQUAL "text")
    # ---- bad tree: one file per marker, plus LGPL DirectX header paths, plus allow-list abuse ----
    set(_bad "${SCRATCH}/bad")
    set(_expected "")
    foreach(_id IN LISTS RL_MARKER_IDS)
        set(_rel "${_rl}/fixture/m_${_id}.slang")
        file(WRITE "${_bad}/${_rel}" "// seeded fixture line 1\n${RL_MARKER_SAMPLE_${_id}}\n// end\n")
        list(APPEND _expected "${_id} ${_rel}")
    endforeach()
    # mingw-directx-headers (LGPL-2.1+) vendored by path, with and without the LGPL text.
    set(_rel "vendor/dxvk/include/native/directx/d3d9.h")
    file(WRITE "${_bad}/${_rel}" "/* fixture: placeholder header */\n")
    list(APPEND _expected "lgpl_directx_header ${_rel}")
    set(_rel "${_rl}/shell/d3d8types.h")
    file(WRITE "${_bad}/${_rel}" "/* fixture */\n${RL_MARKER_SAMPLE_lgpl_text}\n")
    list(APPEND _expected "lgpl_directx_header ${_rel}" "lgpl_text ${_rel}")
    set(_rel "vendor/dxvk/include/dxgi1_6.h")
    file(WRITE "${_bad}/${_rel}" "/* fixture */\n")
    list(APPEND _expected "lgpl_directx_header ${_rel}")
    # Markers in the other vendored roots and the other Relight trees are scanned too.
    foreach(_root "vendor/dxbc-spirv" "vendor/xxhash" "Tests/relight" "Tools/FUSE/Relight")
        set(_rel "${_root}/fixture.txt")
        file(WRITE "${_bad}/${_rel}" "${RL_MARKER_SAMPLE_hdr_licensors_retain}\n")
        list(APPEND _expected "hdr_licensors_retain ${_rel}")
    endforeach()
    # THIRD_PARTY.md may name licences (T) and clean-room files (N) but never carry H/M/L text.
    set(_rel "${_rl}/THIRD_PARTY.md")
    file(WRITE "${_bad}/${_rel}"
        "# Third party\n${RL_MARKER_SAMPLE_rtx_sdks_licence}\n${RL_MARKER_SAMPLE_name_rtxdi_bridge}\n"
        "${RL_MARKER_SAMPLE_hdr_express_agreement}\n${RL_MARKER_SAMPLE_mdl_version_stmt}\n${RL_MARKER_SAMPLE_gpl_text}\n")
    list(APPEND _expected "hdr_express_agreement ${_rel}" "mdl_version_stmt ${_rel}" "gpl_text ${_rel}")
    # A marker that is allowed in THIRD_PARTY.md is still flagged in any other file name.
    set(_rel "${_rl}/NOTES.md")
    file(WRITE "${_bad}/${_rel}" "${RL_MARKER_SAMPLE_rtx_sdks_licence}\n")
    list(APPEND _expected "rtx_sdks_licence ${_rel}")

    _run_scan(_rc _out _found -DMODE=text "-DREPO=${_bad}")
    if(_rc EQUAL 0)
        _fail("bad tree: scanner passed but must fail")
    endif()
    _expect_findings("bad tree" _expected _found)
    list(LENGTH _expected _nexp)
    list(LENGTH RL_MARKER_IDS _nmarkers)

    # ---- good tree: near misses that must not trip the scanner -------------------------------------
    set(_good "${SCRATCH}/good")
    file(WRITE "${_good}/${_rl}/options/near_miss.conf"
        "rtx.rtxdi.enableRayTracedLightSampling = True\nrtx.enableRaytracing = True\nrtx.nrd.enabled = False\n")
    file(WRITE "${_good}/${_rl}/mods/usd/near_miss.usda"
        "#usda 1.0\ndef Shader \"S\" {\n  uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@\n"
        "  uniform token info:mdl:sourceAsset:subIdentifier = \"AperturePBR_Opacity\"\n}\n")
    file(WRITE "${_good}/${_rl}/render/near_miss.cpp"
        "// SPDX-FileCopyrightText: Copyright (c) 2021-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.\n"
        "// SPDX-License-Identifier: MIT\n// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)\n"
        "#include \"NRC.hpp\"\n#include \"nrd_plugin_bridge.hpp\"\n// ReSTIR DI, RTXDI-style reservoirs (Bitterli 2020)\n"
        "int RTXDIish = 0; int kRAB = 1; int MyNRC_h = 2; // NRD denoiser via plugins/nvidia\n"
        "// model 1.6; import std::df;\n")
    file(WRITE "${_good}/vendor/dxvk/LICENSE"
        "This software is provided 'as-is', without any express or implied warranty.\n"
        "Altered source versions must be plainly marked as such.\n")
    file(WRITE "${_good}/vendor/dxvk/src/d3d9/d3d9_device.h" "#pragma once\n")
    file(WRITE "${_good}/vendor/dxvk/src/d3d9/d3d9_interfaces.h" "#pragma once\n")
    file(WRITE "${_good}/vendor/dxvk/src/dxgi/dxgi_format.h" "#pragma once\n")
    file(WRITE "${_good}/vendor/dxvk/src/d3d11/d3d11_include.h" "#pragma once\n")
    file(WRITE "${_good}/vendor/dxvk/include/native/windows/windows_base.h" "#pragma once\n")
    file(WRITE "${_good}/vendor/dxbc-spirv/LICENSE" "MIT License\nPermission is hereby granted, free of charge\n")
    file(WRITE "${_good}/vendor/xxhash/LICENSE" "BSD 2-Clause License\n")
    file(WRITE "${_good}/${_rl}/THIRD_PARTY.md"
        "# Third party\n${RL_MARKER_SAMPLE_rtx_sdks_licence}\n${RL_MARKER_SAMPLE_spdx_nv_proprietary}\n"
        "${RL_MARKER_SAMPLE_intel_simplified}\n${RL_MARKER_SAMPLE_name_rtxdi_bridge}\n"
        "${RL_MARKER_SAMPLE_name_nrc_h}\n${RL_MARKER_SAMPLE_name_core_definitions}\n")
    _run_scan(_rc _out _found -DMODE=text "-DREPO=${_good}")
    if(NOT _rc EQUAL 0 OR _found)
        _fail("good tree: scanner must pass with no findings (rc=${_rc}, findings: ${_found})\n${_out}")
    endif()

    message(STATUS "self-test: ${_nmarkers} markers + LGPL header paths + allow-list: ${_nexp} seeded findings reported, "
        "near-miss tree clean")

elseif(MODE STREQUAL "binary")
    set(_bad_paths
        # carried over from nvidia_binary_gate.cmake
        "bin/nvngx_dlss.dll" "lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.1" "sdk/sl.interposer.dll"
        # §0.4.4
        "bin/NRC_Vulkan.dll" "bin/cudart64_13.dll" "lib/libcudart.so.13" "bin/nvrtc64_120_0.dll" "bin/nvrtc-builtins64_120.dll"
        "bin/rtxio.dll" "bin/NRD.dll" "bin/NRI.dll" "lib/libNRD.so"
        "rtx-remix/mods/x/AperturePBR_Opacity.mdl" "mdl/nvidia/core_defi${_}nitions.mdl"
        "NvRemixBridge.exe" "NvRemixLauncher.exe" "rtx-remix/.trex/bridge.conf" "remixapi.dll"
        "bin/NvLowLatencyVk.dll" "bin/GFSDK_Aftermath_Lib.x64.dll"
        # Intel XeSS / XeLL
        "bin/libxess.dll" "bin/libxess_dx11.dll" "bin/libxell.dll"
        # any native binary or MDL module inside the Relight / vendored trees
        "Source/FUSE/Relight/shell/d3d9.dll" "Tests/relight/apps/ff_triangle.exe" "vendor/dxvk/lib/libdxvk_d3d9.a"
        "vendor/xxhash/xxhash.o" "Tools/FUSE/Relight/tool.pdb" "Tests/relight/fixtures/mods/m/custom.mdl")
    set(_good_paths
        "vendor/streamline/include/sl.h" "Source/FUSE/Relight/mods/usd/aperture_mdl_map.cpp"
        "Tests/relight/fixtures/mods/m/mod.usda" "Source/FUSE/Relight/shell/d3d9.def"
        "Source/FUSE/Relight/render/pathtrace/nrd_plugin_bridge.cpp" "Source/FUSE/Relight/render/pathtrace/radiance_cache.cpp"
        "vendor/xxhash/xxhash.h" "vendor/dxvk/src/d3d9/d3d9_main.cpp" "d3dcompiler_47.dll"
        "vendor/assimp/test/models/MDL/MDL (HL1)/man.mdl" "docs/nvidia-plugin.md" "trex/readme.txt"
        "Source/FUSE/Relight/api/remixapi_compat.cpp" "libfuse_nvplugin_mock.so" "Source/FUSE/Relight/cmake/relight_dxvk.cmake")
    set(_expected "")
    foreach(_p IN LISTS _bad_paths)
        list(APPEND _expected "binary ${_p}")
    endforeach()
    string(REPLACE ";" "\n" _txt "${_bad_paths}")
    file(WRITE "${SCRATCH}/bad.list" "${_txt}\n")
    string(REPLACE ";" "\n" _txt "${_good_paths}")
    file(WRITE "${SCRATCH}/good.list" "${_txt}\n")

    _run_scan(_rc _out _found -DMODE=binary "-DREPO=${REPO}" "-DFILE_LIST=${SCRATCH}/bad.list")
    if(_rc EQUAL 0)
        _fail("bad list: binary gate passed but must fail")
    endif()
    _expect_findings("bad list" _expected _found)
    _run_scan(_rc _out _found -DMODE=binary "-DREPO=${REPO}" "-DFILE_LIST=${SCRATCH}/good.list")
    if(NOT _rc EQUAL 0 OR _found)
        _fail("good list: binary gate must pass with no findings (rc=${_rc}, findings: ${_found})\n${_out}")
    endif()
    list(LENGTH _bad_paths _nb)
    list(LENGTH _good_paths _ng)
    message(STATUS "self-test: ${_nb} seeded binaries flagged, ${_ng} allowed paths pass")
else()
    message(FATAL_ERROR "rl_licence_selftest: MODE must be text or binary")
endif()

if(_errors GREATER 0)
    message(FATAL_ERROR "rl_licence_selftest(${MODE}): self-test FAILED (${_errors})")
endif()

# ---- real repository -------------------------------------------------------------------------------
_run_scan(_rc _out _found -DMODE=${MODE} "-DREPO=${REPO}")
message("${_out}")
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "rl_licence_selftest(${MODE}): the repository fails the licence gate (see findings above)")
endif()
file(REMOVE_RECURSE "${SCRATCH}")
message(STATUS "rl_licence_selftest(${MODE}): PASS")
