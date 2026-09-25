# FUSE Relight RL-6.4: stages the drop-in package (cmake -P; run by the relight_package target).
#
#   cmake -DOUT=<package dir> -DREPO=<repo root> -DVERSION=<x.y.z> [-DBUILD_ID=<text>]
#         -DX64_D3D9=<d3d9.dll> -DX64_D3D8=<d3d8.dll> -DHOST=<fuse_relight_host.exe>
#         -DLAUNCHER=<fuse_relight_launcher.exe> -DPLUGINS_TOOL=<fuse_relight_plugins.exe>
#         [-DX86_DIR=<i686 build tree>] [-DREQUIRE_X86=ON] -P relight_package_build.cmake
#
# Layout and notices come from relight_package_spec.cmake. The x86 bridge files come from the i686 tree
# when it exists; without it the package is x64 only plus the x86/fuse_relight host payload, and
# manifest.json says so ("x86_bridge": false). Writes manifest.json (paths, sizes, sha256) and
# MANIFEST.sha256 (sha256sum -c format, covering manifest.json too). The package gate
# (relight_package_check.cmake) verifies the result.

cmake_minimum_required(VERSION 3.21)
foreach(_v OUT REPO VERSION X64_D3D9 X64_D3D8 HOST LAUNCHER PLUGINS_TOOL)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "relight_package_build: pass -D${_v}=...")
    endif()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/relight_package_spec.cmake")
set(_dist "${CMAKE_CURRENT_LIST_DIR}/dist")
if(NOT DEFINED BUILD_ID)
    set(BUILD_ID "")
endif()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

function(_rl_put src dst)
    if(NOT EXISTS "${src}")
        message(FATAL_ERROR "relight_package_build: missing input ${src} (for ${dst})")
    endif()
    get_filename_component(_dir "${OUT}/${dst}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dir}")
    file(COPY_FILE "${src}" "${OUT}/${dst}")
endfunction()

# ---- binaries ------------------------------------------------------------------------------------------
foreach(_e IN LISTS RL_PKG_X64_BINARIES)
    string(REPLACE "|" ";" _e "${_e}")
    list(GET _e 0 _dst)
    list(GET _e 1 _var)
    _rl_put("${${_var}}" "${_dst}")
endforeach()
set(_x86 FALSE)
if(DEFINED X86_DIR AND NOT X86_DIR STREQUAL "" AND EXISTS "${X86_DIR}/relight/bridge/client/d3d9.dll")
    set(_x86 TRUE)
    foreach(_e IN LISTS RL_PKG_X86_BINARIES)
        string(REPLACE "|" ";" _e "${_e}")
        list(GET _e 0 _dst)
        list(GET _e 1 _src)
        _rl_put("${X86_DIR}/${_src}" "${_dst}")
    endforeach()
elseif(REQUIRE_X86)
    message(FATAL_ERROR "relight_package_build: REQUIRE_X86 is set but the i686 tree '${X86_DIR}' has no "
        "relight/bridge/client/d3d9.dll (configure and build the fuse-mingw i686 tree first)")
else()
    message(STATUS "relight_package_build: no i686 tree at '${X86_DIR}': x86 bridge client not packaged "
        "(x86/ holds only the x64 host payload)")
endif()

# ---- text files -----------------------------------------------------------------------------------------
set(RL_PKG_VERSION "${VERSION}")
set(RL_PKG_BUILD "${BUILD_ID}")
configure_file("${_dist}/README.txt" "${OUT}/README.txt" @ONLY NEWLINE_STYLE CRLF)
foreach(_d x64 x86)
    configure_file("${_dist}/rtx.conf" "${OUT}/${_d}/rtx.conf" COPYONLY)
endforeach()
configure_file("${_dist}/plugins_README.txt" "${OUT}/x64/fuse_relight_plugins/README.txt" COPYONLY)
configure_file("${_dist}/plugins_README.txt" "${OUT}/x86/fuse_relight/fuse_relight_plugins/README.txt" COPYONLY)
_rl_put("${REPO}/LICENSE.md" "LICENSE.txt")
_rl_put("${REPO}/docs/relight.md" "docs/relight.md")

# ---- licences + THIRD_PARTY_NOTICES.txt -------------------------------------------------------------------
set(_n "FUSE Relight ${VERSION}: third-party notices\n")
string(APPEND _n "==========================================================================\n\n")
string(APPEND _n "FUSE Relight is AGPL-3.0-licensed. It contains the components listed below; each one's licence\n")
string(APPEND _n "text follows and is also in licenses/. No NVIDIA, Intel or other proprietary runtime is\n")
string(APPEND _n "included: DLSS, Reflex, XeSS and NRD are optional plugins the user installs separately.\n\n")
foreach(_id IN LISTS RL_NOTICE_IDS)
    string(APPEND _n "  - ${RL_NOTICE_TITLE_${_id}} [${RL_NOTICE_LICENCE_${_id}}]\n")
endforeach()
foreach(_id IN LISTS RL_NOTICE_IDS)
    rl_notice_package_names(${_id} _names)
    string(APPEND _n "\n\n==========================================================================\n")
    string(APPEND _n "${RL_NOTICE_TITLE_${_id}}\nLicence: ${RL_NOTICE_LICENCE_${_id}}\n")
    string(APPEND _n "==========================================================================\n")
    set(_i 0)
    foreach(_src IN LISTS RL_NOTICE_FILES_${_id})
        list(GET _names ${_i} _dst)
        math(EXPR _i "${_i} + 1")
        _rl_put("${REPO}/${_src}" "${_dst}")
        file(READ "${REPO}/${_src}" _text)
        string(APPEND _n "\n--- ${_dst} (from ${_src}) ---\n\n${_text}\n")
    endforeach()
endforeach()
file(WRITE "${OUT}/THIRD_PARTY_NOTICES.txt" "${_n}")

# ---- manifests --------------------------------------------------------------------------------------------
file(GLOB_RECURSE _files LIST_DIRECTORIES false RELATIVE "${OUT}" "${OUT}/*")
list(SORT _files)
set(_json_files "")
foreach(_f IN LISTS _files)
    file(SHA256 "${OUT}/${_f}" _h)
    file(SIZE "${OUT}/${_f}" _sz)
    if(NOT _json_files STREQUAL "")
        string(APPEND _json_files ",\n")
    endif()
    string(APPEND _json_files "    {\"path\": \"${_f}\", \"size\": ${_sz}, \"sha256\": \"${_h}\"}")
endforeach()
if(_x86)
    set(_x86_json true)
else()
    set(_x86_json false)
endif()
file(WRITE "${OUT}/manifest.json"
    "{\n  \"name\": \"FUSE Relight\",\n  \"version\": \"${VERSION}\",\n  \"build\": \"${BUILD_ID}\",\n"
    "  \"x86_bridge\": ${_x86_json},\n  \"files\": [\n${_json_files}\n  ]\n}\n")
list(APPEND _files manifest.json)
list(SORT _files)
set(_sums "")
foreach(_f IN LISTS _files)
    file(SHA256 "${OUT}/${_f}" _h)
    string(APPEND _sums "${_h}  ${_f}\n")
endforeach()
file(WRITE "${OUT}/MANIFEST.sha256" "${_sums}")
list(LENGTH _files _nf)
message(STATUS "relight_package_build: ${OUT}: ${_nf} files (x86 bridge: ${_x86_json})")
