# FUSE lint gates (master plan: "No owning raw pointers in public FUSE APIs", Appendix A rename
# checklist, Appendix C carry-forward). Included once from Source/FUSE/CMakeLists.txt.
#
# * builds `fuse_lint` (Tools/FUSE/Lint/fuse_lint.cpp) — a dependency-free C++ scanner; every
#   check seeds a known-bad and a known-good sample into a scratch dir and proves it detects the
#   bad one before it scans the real tree (self-validating gates);
# * writes ${CMAKE_BINARY_DIR}/fuse_lint/targets.manifest at the end of the top-level configure
#   (cmake_language(DEFER)) with every fuse_* target's type, source dir, CXX_STANDARD and link
#   libraries, so target-level rows (C++23, no ImGui/Qt links) are checked against the final graph;
# * registers ctest entries labelled "gate;lint".

if(DEFINED _FUSE_LINT_GATES_INCLUDED)
    return()
endif()
set(_FUSE_LINT_GATES_INCLUDED TRUE)

set(FUSE_LINT_MANIFEST "${CMAKE_BINARY_DIR}/fuse_lint/targets.manifest")

function(_fuse_lint_collect_targets dir out_var)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        _fuse_lint_collect_targets("${_sub}" _sub_targets)
        list(APPEND _targets ${_sub_targets})
    endforeach()
    set(${out_var} ${_targets} PARENT_SCOPE)
endfunction()

# Runs deferred at the end of the top-level CMakeLists so later subdirs (Tools/FUSE) and late
# property edits are captured. One line per fuse_* target, '|' separated, lists joined by ','.
function(fuse_lint_write_target_manifest)
    # Deferred into the top-level directory scope: recompute the path rather than rely on a
    # Source/FUSE-scoped variable.
    set(FUSE_LINT_MANIFEST "${CMAKE_BINARY_DIR}/fuse_lint/targets.manifest")
    _fuse_lint_collect_targets("${CMAKE_SOURCE_DIR}" _all)
    set(_out "# name|type|source_dir|cxx_standard|has_cxx|has_cuda|link_libraries|interface_link_libraries\n")
    foreach(_t IN LISTS _all)
        if(NOT _t MATCHES "^fuse_")
            continue()
        endif()
        get_target_property(_type ${_t} TYPE)
        get_target_property(_dir ${_t} SOURCE_DIR)
        get_target_property(_std ${_t} CXX_STANDARD)
        set(_srcs "")
        set(_links "")
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(_srcs ${_t} SOURCES)
            get_target_property(_links ${_t} LINK_LIBRARIES)
        endif()
        get_target_property(_ilinks ${_t} INTERFACE_LINK_LIBRARIES)
        set(_has_cxx 0)
        set(_has_cuda 0)
        foreach(_s IN LISTS _srcs)
            if(_s MATCHES "\\.(cpp|cc|cxx|c\\+\\+|mm)$")
                set(_has_cxx 1)
            elseif(_s MATCHES "\\.cu$")
                set(_has_cuda 1)
            endif()
        endforeach()
        foreach(_v _std _links _ilinks)
            if(NOT ${_v} OR ${_v} MATCHES "-NOTFOUND$")
                set(${_v} "")
            endif()
            string(REPLACE ";" "," ${_v} "${${_v}}")
            string(REPLACE "|" "/" ${_v} "${${_v}}")
            string(REPLACE "\n" " " ${_v} "${${_v}}")
        endforeach()
        string(APPEND _out "${_t}|${_type}|${_dir}|${_std}|${_has_cxx}|${_has_cuda}|${_links}|${_ilinks}\n")
    endforeach()
    file(WRITE "${FUSE_LINT_MANIFEST}.tmp" "${_out}")
    file(COPY_FILE "${FUSE_LINT_MANIFEST}.tmp" "${FUSE_LINT_MANIFEST}" ONLY_IF_DIFFERENT)
    file(REMOVE "${FUSE_LINT_MANIFEST}.tmp")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL fuse_lint_write_target_manifest)

add_executable(fuse_lint "${CMAKE_SOURCE_DIR}/Tools/FUSE/Lint/fuse_lint.cpp"
    "${CMAKE_SOURCE_DIR}/Tools/FUSE/Lint/fuse_lint_asset_licences.cpp")  # asset-licences (FUSE_ASSET_PLAN W0.5)
set_target_properties(fuse_lint PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
# std::regex over ~2k files is ~10x slower unoptimised; keep the gates fast in Debug trees.
target_compile_options(fuse_lint PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O2>)

if(NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

set(_fuse_lint_src "${CMAKE_SOURCE_DIR}/Source/FUSE")
set(_fuse_lint_scratch "${CMAKE_BINARY_DIR}/fuse_lint/scratch")
set(_fuse_lint_tests)

function(_fuse_lint_add name)
    add_test(NAME ${name} COMMAND fuse_lint ${ARGN} --scratch "${_fuse_lint_scratch}/${name}")
    set_property(GLOBAL APPEND PROPERTY _FUSE_LINT_TESTS ${name})
endfunction()

# Master plan (every B* block): "No owning raw pointers in public FUSE APIs" — one test per
# module public include tree (Source/FUSE/<Mod>/include and Source/FUSE/<Group>/<Mod>/include).
file(GLOB _fuse_lint_inc1 LIST_DIRECTORIES true "${_fuse_lint_src}/*/include")
file(GLOB _fuse_lint_inc2 LIST_DIRECTORIES true "${_fuse_lint_src}/*/*/include")
foreach(_inc IN LISTS _fuse_lint_inc1 _fuse_lint_inc2)
    if(NOT IS_DIRECTORY "${_inc}")
        continue()
    endif()
    file(RELATIVE_PATH _rel "${_fuse_lint_src}" "${_inc}")
    string(REGEX REPLACE "/include$" "" _mod "${_rel}")
    string(TOLOWER "${_mod}" _mod)
    string(REPLACE "/" "_" _mod "${_mod}")
    _fuse_lint_add(fuse_lint_ownership_${_mod} ownership --dir "${_inc}")
endforeach()

# Appendix A — rename checklist.
_fuse_lint_add(fuse_lint_a_namespace_fuse      namespace      --root "${_fuse_lint_src}")
_fuse_lint_add(fuse_lint_a_fuse_macros         macros         --root "${_fuse_lint_src}")
_fuse_lint_add(fuse_lint_a_torque_macros_compat torque-macros --root "${_fuse_lint_src}")
_fuse_lint_add(fuse_lint_a_log_mem_domains     torque-names   --root "${_fuse_lint_src}")
_fuse_lint_add(fuse_lint_a_no_meridian_imgui   banned-deps    --root "${_fuse_lint_src}" --manifest "${FUSE_LINT_MANIFEST}")
# "Icons, installer, docs, CI badge names": workflow names, README title + CI badges, docs titles
# (icons / installer half: fuse_package_gate + fuse_branding_icons_regen in cmake/FusePackaging.cmake).
_fuse_lint_add(fuse_lint_a_branding_ci_docs    branding       --repo "${CMAKE_SOURCE_DIR}")
# Appendix C — Phase 1 carry-forward.
_fuse_lint_add(fuse_lint_c_cxx23_targets       cxx-standard   --manifest "${FUSE_LINT_MANIFEST}" --repo "${CMAKE_SOURCE_DIR}")
_fuse_lint_add(fuse_lint_c_no_qt_in_core       qt-includes    --root "${_fuse_lint_src}" --manifest "${FUSE_LINT_MANIFEST}")
# B6.1-B6.12 "Qt 6 only for editor chrome — no Dear ImGui": fuse_editor link closure + editor includes.
# Skips (77) when fuse_editor is not configured (Qt6 not found / FUSE_BUILD_EDITOR=OFF).
_fuse_lint_add(fuse_lint_b6_editor_qt6_only    editor-qt6     --root "${_fuse_lint_src}" --manifest "${FUSE_LINT_MANIFEST}")
_fuse_lint_add(fuse_lint_c_doc_headings        doc-headings
    --plan "${CMAKE_SOURCE_DIR}/docs/plans/FUSE_MASTER_PLAN.md" --sources "${CMAKE_SOURCE_DIR}/docs/sources")
# B1 gate: third-party dependencies build from vendored source with pinned commits.
_fuse_lint_add(fuse_lint_vendored_pins_vma     vendored-pins  --dir "${FUSE_VENDOR_DIR}/vma")
# Upscalers (Renderer/upscale): FidelityFX SDK FSR1 + CAS subset and NVIDIA Image Scaling, both MIT.
_fuse_lint_add(fuse_lint_vendored_pins_fidelityfx vendored-pins --dir "${FUSE_VENDOR_DIR}/fidelityfx")
_fuse_lint_add(fuse_lint_vendored_pins_nvidia_nis vendored-pins --dir "${FUSE_VENDOR_DIR}/nvidia-nis")
# Optional NVIDIA plugin (docs/nvidia-plugin.md): Streamline public headers subset, MIT (binaries never vendored).
_fuse_lint_add(fuse_lint_vendored_pins_streamline vendored-pins --dir "${FUSE_VENDOR_DIR}/streamline")
_fuse_lint_add(fuse_lint_vendored_pins_volk vendored-pins --dir "${FUSE_VENDOR_DIR}/volk")  # WP-0.2 volk meta-loader (MIT)
# FUSE Relight (RL-0.2): vendored DXVK 3.1.1 subset (zlib) and dxbc-spirv (MIT).
_fuse_lint_add(fuse_lint_vendored_pins_dxvk vendored-pins --dir "${FUSE_VENDOR_DIR}/dxvk")
_fuse_lint_add(fuse_lint_vendored_pins_dxbc_spirv vendored-pins --dir "${FUSE_VENDOR_DIR}/dxbc-spirv")
# FUSE Relight RL-0.5: xxHash 0.8.x (BSD-2) for the Remix-compatible asset hashes.
_fuse_lint_add(fuse_lint_vendored_pins_xxhash vendored-pins --dir "${FUSE_VENDOR_DIR}/xxhash")
_fuse_lint_add(fuse_lint_vendored_pins_tracy vendored-pins --dir "${FUSE_VENDOR_DIR}/tracy")  # WP-0.6 Tracy client (BSD-3), optional FUSE_TRACY
_fuse_lint_add(fuse_lint_vendored_pins_meshoptimizer vendored-pins --dir "${FUSE_VENDOR_DIR}/meshoptimizer")  # WP-1.2 meshlet cook (MIT)
_fuse_lint_add(fuse_lint_vendored_pins_gdeflate vendored-pins --dir "${FUSE_VENDOR_DIR}/gdeflate")  # RL-3.3 GDeflate CPU codec (MIT + Apache-2.0)
_fuse_lint_add(fuse_lint_vendored_pins_tinyusdz vendored-pins --dir "${FUSE_VENDOR_DIR}/tinyusdz")  # RL-3.1 / Remaster W2.2 TinyUSDZ USDA + USDC reader (Apache-2.0)
_fuse_lint_add(fuse_lint_vendored_pins_nvidia_flip vendored-pins --dir "${FUSE_VENDOR_DIR}/nvidia-flip")  # Asset plan W0.8 NVIDIA FLIP golden metric (BSD-3)

get_property(_fuse_lint_all GLOBAL PROPERTY _FUSE_LINT_TESTS)
set_tests_properties(${_fuse_lint_all} PROPERTIES LABELS "gate;lint" TIMEOUT 300)
set_tests_properties(fuse_lint_b6_editor_qt6_only PROPERTIES LABELS "gate;lint;qt" SKIP_RETURN_CODE 77)
# FUSE_ASSET_PLAN §2.4 / W0.5: Content/licences.lock.json covers every file under Content/ (labels lint;asset).
add_test(NAME fuse_lint_asset_licences COMMAND fuse_lint asset-licences --root "${CMAKE_SOURCE_DIR}/Content"
         --scratch "${_fuse_lint_scratch}/fuse_lint_asset_licences")
set_tests_properties(fuse_lint_asset_licences PROPERTIES LABELS "gate;lint;asset" TIMEOUT 300)
# FUSE_ASSET_PLAN §5.3 / W0.6: fuse_assetcheck validation gates (labels asset;gate).
include(${CMAKE_SOURCE_DIR}/cmake/FuseAssetCheck.cmake)
include(${CMAKE_SOURCE_DIR}/Source/FUSE/Relight/cmake/relight_licence_gates.cmake)  # RL-0.1 licence gates (rl_licence_text_scan, rl_binary_gate)
