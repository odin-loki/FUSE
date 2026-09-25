# FUSE Relight RL-6.4: package layout and the third-party notices shipped in it (plan §0.4.1, RL-6.4).
#
# One entry per component compiled into the shipped binaries (d3d9.dll, d3d8.dll, the bridge client,
# fuse_relight_host.exe, fuse_relight_launcher.exe, fuse_relight_plugins.exe). Section 1 of
# Source/FUSE/Relight/THIRD_PARTY.md lists the Relight-specific upstreams; the FUSE renderer the runtime
# links adds its own MIT dependencies (meshoptimizer, VMA, volk, FidelityFX, NIS). Plugins (DLSS, NRD,
# XeSS, Streamline runtimes) are never shipped, so they carry no notice here.
#
# Fields (parallel variables per id in RL_NOTICE_IDS):
#   RL_NOTICE_TITLE_<id>   heading in THIRD_PARTY_NOTICES.txt
#   RL_NOTICE_LICENCE_<id> licence name
#   RL_NOTICE_FILES_<id>   licence texts, repo-relative (copied verbatim into licenses/<id>[-n].txt)
#   RL_NOTICE_PHRASE_<id>  a phrase the shipped text must contain (checked by the package gate)
#   RL_NOTICE_TP_KEY_<id>  text that must appear in THIRD_PARTY.md (empty: a FUSE renderer dependency)

set(RL_NOTICE_IDS "")
macro(_rl_notice id title licence phrase tpkey)
    list(APPEND RL_NOTICE_IDS ${id})
    set(RL_NOTICE_TITLE_${id} "${title}")
    set(RL_NOTICE_LICENCE_${id} "${licence}")
    set(RL_NOTICE_PHRASE_${id} "${phrase}")
    set(RL_NOTICE_TP_KEY_${id} "${tpkey}")
    set(RL_NOTICE_FILES_${id} ${ARGN})
endmacro()

set(_rl_pkg "Source/FUSE/Relight/package/dist/licenses")

_rl_notice(fuse "FUSE and FUSE Relight" "AGPL-3.0" "GNU Affero General Public License" ""
    "LICENSE.md")
_rl_notice(dxvk "DXVK 3.1.1 (doitsujin/dxvk), with FUSE-DXVK marked modifications" "zlib"
    "Altered source versions must be plainly marked" "doitsujin/dxvk"
    "vendor/dxvk/LICENSE")
_rl_notice(dxbc_spirv "dxbc-spirv (doitsujin/dxbc-spirv)" "MIT" "Philip Rebohle" "doitsujin/dxbc-spirv"
    "vendor/dxbc-spirv/LICENSE")
_rl_notice(libdisplay_info "libdisplay-info (bundled with DXVK)" "MIT" "libdisplay-info Contributors" "libdisplay-info"
    "vendor/dxvk/subprojects/libdisplay-info/LICENSE")
_rl_notice(openvr "OpenVR headers (bundled with DXVK)" "BSD-3-Clause" "Valve Corporation" "OpenVR"
    "vendor/dxvk/include/openvr/LICENSE")
_rl_notice(vulkan_headers "Vulkan-Headers (bundled with DXVK)" "Apache-2.0 OR MIT" "The Khronos Group" "Vulkan-Headers"
    "vendor/dxvk/include/vulkan/LICENSE.md" "vendor/dxvk/include/vulkan/LICENSES/MIT.txt"
    "vendor/dxvk/include/vulkan/LICENSES/Apache-2.0.txt")
_rl_notice(spirv_headers "SPIRV-Headers (bundled with DXVK and dxbc-spirv)" "MIT" "The Khronos Group" "SPIRV-Headers"
    "vendor/dxvk/include/spirv/LICENSE" "vendor/dxbc-spirv/submodules/spirv_headers/LICENSE")
_rl_notice(dxvk_remix "dxvk-remix runtime and bridge code, ported (NVIDIAGameWorks/dxvk-remix, MIT parts only)" "MIT"
    "Permission is hereby granted, free of charge" "NVIDIAGameWorks/dxvk-remix"
    "${_rl_pkg}/dxvk-remix-MIT.txt")
_rl_notice(xxhash "xxHash (Yann Collet)" "BSD-2-Clause" "Yann Collet" "xxHash"
    "vendor/xxhash/LICENSE")
_rl_notice(gdeflate "GDeflate CPU codec: libdeflate (gdeflate branch) and Microsoft DirectStorage GDeflate" "MIT; Apache-2.0"
    "Apache License" "GDeflate"
    "vendor/gdeflate/libdeflate/COPYING" "vendor/gdeflate/GDeflate/LICENSE")
_rl_notice(tinyusdz "TinyUSDZ 0.9.4 (lighttransport/tinyusdz) with its bundled components (lz4, xxHash, ghc::filesystem, glob, fast_float, dragonbox, jsteemann atoi, linalg)"
    "Apache-2.0; BSD-2-Clause; MIT; BSL-1.0; Unlicense" "Syoyo Fujita" "TinyUSDZ"
    "vendor/tinyusdz/LICENSE"
    "vendor/tinyusdz/src/lz4/LICENSE"
    "vendor/tinyusdz/src/external/xxhash.LICENSE"
    "vendor/tinyusdz/src/external/filesystem/LICENSE"
    "vendor/tinyusdz/src/external/glob/LICENSE"
    "vendor/tinyusdz/src/external/fast_float/LICENSE-MIT"
    "vendor/tinyusdz/src/external/fast_float/LICENSE-APACHE"
    "vendor/tinyusdz/src/external/fast_float/LICENSE-BOOST"
    "vendor/tinyusdz/src/external/dragonbox/LICENSE-Boost"
    "vendor/tinyusdz/src/external/dragonbox/LICENSE-Apache2-LLVM"
    "vendor/tinyusdz/src/external/jsteemann/LICENSE"
    "vendor/tinyusdz/src/external/linalg.UNLICENSE")
_rl_notice(meshoptimizer "meshoptimizer (FUSE renderer)" "MIT" "Arseny Kapoulkine" ""
    "vendor/meshoptimizer/LICENSE.md")
_rl_notice(vma "Vulkan Memory Allocator (FUSE renderer)" "MIT" "Advanced Micro Devices" ""
    "vendor/vma/LICENSE.txt")
_rl_notice(volk "volk (FUSE renderer)" "MIT" "Arseny Kapoulkine" ""
    "vendor/volk/LICENSE.md")
_rl_notice(fidelityfx "AMD FidelityFX SDK shaders (FUSE renderer)" "MIT" "Advanced Micro Devices" ""
    "vendor/fidelityfx/LICENSE.txt")
_rl_notice(nis "NVIDIA Image Scaling shaders (FUSE renderer)" "MIT" "The MIT License" ""
    "vendor/nvidia-nis/licence.txt")

# Licence file name inside the package: licenses/<id>.txt, or licenses/<id>-<n>.txt for the n-th (n >= 2).
function(rl_notice_package_names id out)
    set(_names "")
    set(_n 0)
    foreach(_f IN LISTS RL_NOTICE_FILES_${id})
        math(EXPR _n "${_n} + 1")
        if(_n EQUAL 1)
            list(APPEND _names "licenses/${id}.txt")
        else()
            list(APPEND _names "licenses/${id}-${_n}.txt")
        endif()
    endforeach()
    set(${out} "${_names}" PARENT_SCOPE)
endfunction()

# --------------------------------------------------------------------------------------------------
# Layout (package-relative paths). Binaries: <package path>|<source variable of the build script>.
# --------------------------------------------------------------------------------------------------
set(RL_PKG_X64_BINARIES
    "x64/d3d9.dll|X64_D3D9"
    "x64/d3d8.dll|X64_D3D8"
    "x64/fuse_relight_launcher.exe|LAUNCHER"
    "x64/fuse_relight_plugins.exe|PLUGINS_TOOL"
    "x86/fuse_relight/fuse_relight_host.exe|HOST"
    "x86/fuse_relight/d3d9.dll|X64_D3D9"
    "x86/fuse_relight/fuse_relight_plugins.exe|PLUGINS_TOOL")
# From the i686 tree (FUSE_RELIGHT_BRIDGE_X86_DIR), relative to it. Optional unless REQUIRE_X86.
set(RL_PKG_X86_BINARIES
    "x86/d3d9.dll|relight/bridge/client/d3d9.dll"
    "x86/d3d8.dll|relight/bridge/client/d3d8.dll"
    "x86/fuse_relight_launcher.exe|relight/bin/fuse_relight_launcher.exe"
    "x86/fuse_relight_passthrough/d3d9.dll|relight/bin/d3d9.dll")
set(RL_PKG_TEXT_FILES
    "README.txt" "LICENSE.txt" "THIRD_PARTY_NOTICES.txt" "docs/relight.md"
    "x64/rtx.conf" "x86/rtx.conf"
    "x64/fuse_relight_plugins/README.txt" "x86/fuse_relight/fuse_relight_plugins/README.txt")
set(RL_PKG_MANIFESTS "manifest.json" "MANIFEST.sha256")
# Extra forbidden names in a package on top of rl_binary_match (relight_proprietary_scan.cmake): debug
# symbols, import libraries, test helpers, and any binary inside a fuse_relight_plugins folder.
set(RL_PKG_FORBIDDEN_RE
    "\\.(pdb|lib|a|exp|ilk|o|obj)$"
    "(^|/)(create_device|fuse_relight_bridge_stub_host|fuse_relight_launcher_selftest[^/]*|fuse_relight_bridge_host_d3d9_client)\\.(exe|dll)$"
    "(^|/)fuse_relight_plugins/.+\\.(dll|so|exe)$")
