# WP-2.2 (docs/unification/RENDERER-EXECUTION.md): light types (LTC rectangle / disk area lights, sun
# disks) + BRDF multi-scatter energy compensation + the DFG / LTC look-up tables.
# Owned by WP-2.2; other packages must not edit this file. Included after rp_wp21.cmake.
#
#   fuse_ltc            STATIC  BrdfLut (LTC table from src/lighting/ltc/ltc_lut_data.inc + the DFG and
#                               horizon-sphere bakes, single-source kernels "brdf_lut.dfg" /
#                               "brdf_lut.sphere"), area-light rows (include/fuse/renderer/lighting/ltc/**,
#                               src/lighting/ltc/**); linked into fuse_lighting_gpu (WP-2.1), whose
#                               light.shade uses the compensated BRDF and area lights through
#                               shaders/lighting/lc_ltc.{glsl,slang} + shaders/common/brdf.{glsl,slang}
#   fuse_ltc_fit        tool    (EXCLUDE_FROM_ALL) refits ltc_lut_data.inc
#   ltc_probe kernels   SPIR-V  shaders/lighting/ltc/ltc_probe.{comp,slang}: the shade's per-light code on
#                               a list of cases (GPU gate)
#   fuse_rp_brdf_* / fuse_rp_ltc_*  ctest  CPU gates (stub-safe) and Lavapipe gates

get_filename_component(_fuse_wp22_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_fuse_wp22_inc "${_fuse_wp22_root}/include/fuse/renderer/lighting/ltc")
set(_fuse_wp22_src "${_fuse_wp22_root}/src/lighting/ltc")
set(_fuse_wp22_common "${_fuse_wp22_root}/shaders/common")
set(_fuse_wp22_lighting "${_fuse_wp22_root}/shaders/lighting")

add_library(fuse_ltc STATIC
    ${_fuse_wp22_src}/ltc_lut.cpp
    ${_fuse_wp22_src}/ltc_lut_data.inc
    ${_fuse_wp22_inc}/ltc_kernel.hpp
    ${_fuse_wp22_inc}/ltc_lut.hpp
    ${_fuse_wp22_common}/brdf.glsl
    ${_fuse_wp22_common}/brdf.slang
    ${_fuse_wp22_lighting}/lc_ltc.glsl
    ${_fuse_wp22_lighting}/lc_ltc.slang
)
set_source_files_properties(${_fuse_wp22_src}/ltc_lut_data.inc ${_fuse_wp22_common}/brdf.glsl
    ${_fuse_wp22_common}/brdf.slang ${_fuse_wp22_lighting}/lc_ltc.glsl ${_fuse_wp22_lighting}/lc_ltc.slang
    PROPERTIES HEADER_FILE_ONLY TRUE)
target_link_libraries(fuse_ltc PUBLIC fuse_rhi)
fuse_apply_cxx23(fuse_ltc)
target_link_libraries(fuse_lighting_gpu PUBLIC fuse_ltc)

add_executable(fuse_ltc_fit EXCLUDE_FROM_ALL ${_fuse_wp22_src}/ltc_fit.cpp)
fuse_apply_cxx23(fuse_ltc_fit)

# The WP-2.1 GLSL twins now include ../common/brdf.glsl (through lc_ltc.glsl): rebuild them when it
# changes (their commands live in this directory, so DEPENDS can be appended; Slang tracks includes
# through its depfile).
if(FUSE_GLSLANG_VALIDATOR)
    foreach(_kernel bounds bin cull scan compact shade)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/lighting/lc_${_kernel}.glsl.spv" APPEND
                           DEPENDS "${_fuse_wp22_common}/brdf.glsl" "${_fuse_wp22_lighting}/lc_ltc.glsl")
    endforeach()
endif()

if(NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

set(_fuse_wp22_tests_dir "${_fuse_wp22_root}/tests")

# --- CPU gates (also run in the stub tree) --------------------------------------------------------
add_executable(fuse_rp_brdf_cpu ${_fuse_wp22_tests_dir}/test_rp_brdf.cpp)
target_link_libraries(fuse_rp_brdf_cpu PRIVATE fuse_ltc)
fuse_apply_cxx23(fuse_rp_brdf_cpu)
foreach(_suite dfg sphere furnace reference)
    add_test(NAME fuse_rp_brdf_${_suite} COMMAND fuse_rp_brdf_cpu ${_suite})
    set_tests_properties(fuse_rp_brdf_${_suite} PROPERTIES LABELS "gate;renderer;lighting" TIMEOUT 600)
endforeach()

add_executable(fuse_rp_ltc_cpu ${_fuse_wp22_tests_dir}/test_rp_ltc_cpu.cpp)
target_link_libraries(fuse_rp_ltc_cpu PRIVATE fuse_lighting_gpu)
fuse_apply_cxx23(fuse_rp_ltc_cpu)
foreach(_suite integral encode rect disk sun furnace shade)
    add_test(NAME fuse_rp_ltc_${_suite} COMMAND fuse_rp_ltc_cpu ${_suite})
    set_tests_properties(fuse_rp_ltc_${_suite} PROPERTIES LABELS "gate;renderer;lighting" TIMEOUT 600)
endforeach()

# --- Lavapipe gate: the shade's per-light code (Slang and GLSL) on the GPU == the CPU reference, and
# both against the Monte Carlo references of the CPU gates ------------------------------------------
set(_fuse_wp22_probe_dir "${_fuse_wp22_lighting}/ltc")
set(_fuse_wp22_spv "${CMAKE_CURRENT_BINARY_DIR}/shaders/lighting/ltc")
file(MAKE_DIRECTORY "${_fuse_wp22_spv}")
add_custom_target(fuse_ltc_probe_kernels)
set(_fuse_wp22_probe_defs "")
set(_fuse_wp22_scene_inc "${_fuse_wp22_root}/include/fuse/renderer/gpu_scene")
if(COMMAND fuse_add_slang_shaders)
    fuse_add_slang_shaders(fuse_ltc_probe_kernels
        SOURCES "${_fuse_wp22_probe_dir}/ltc_probe.slang"
        STAGE compute
        INCLUDE_DIRS "${_fuse_wp22_scene_inc}" "${_fuse_wp22_lighting}"
        FLAGS -warnings-disable 39001 -fp-mode precise
        OUTPUT_DIR "${_fuse_wp22_spv}"
        OUTPUT_VAR _fuse_wp22_probe_slang)
    if(_fuse_wp22_probe_slang)
        list(APPEND _fuse_wp22_probe_defs "FUSE_LTC_PROBE_SLANG_SPV=\"${_fuse_wp22_probe_slang}\"")
    endif()
endif()
if(FUSE_GLSLANG_VALIDATOR)
    set(_out "${_fuse_wp22_spv}/ltc_probe.glsl.spv")
    set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 "-I${_fuse_wp22_common}"
                      "-I${_fuse_wp22_scene_inc}" "-I${_fuse_wp22_lighting}" "${_fuse_wp22_probe_dir}/ltc_probe.comp"
                      -o "${_out}")
    if(FUSE_SPIRV_VAL)
        list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_out}")
    endif()
    file(GLOB _fuse_wp22_glsl_deps "${_fuse_wp22_lighting}/*.glsl")
    add_custom_command(OUTPUT "${_out}" ${_cmds}
        DEPENDS "${_fuse_wp22_probe_dir}/ltc_probe.comp" ${_fuse_wp22_glsl_deps} "${_fuse_wp22_common}/brdf.glsl"
                "${_fuse_wp22_common}/bindless.glsl" "${_fuse_wp22_scene_inc}/gpu_scene.glsl"
        COMMENT "glslangValidator ltc_probe.comp (GLSL twin)"
        VERBATIM)
    add_custom_target(fuse_ltc_probe_glsl DEPENDS "${_out}")
    add_dependencies(fuse_ltc_probe_kernels fuse_ltc_probe_glsl)
    list(APPEND _fuse_wp22_probe_defs "FUSE_LTC_PROBE_GLSL_SPV=\"${_out}\"")
endif()

add_executable(fuse_rp_ltc ${_fuse_wp22_tests_dir}/test_rp_ltc.cpp)
target_link_libraries(fuse_rp_ltc PRIVATE fuse_lighting_gpu)
target_compile_definitions(fuse_rp_ltc PRIVATE ${_fuse_wp22_probe_defs})
add_dependencies(fuse_rp_ltc fuse_ltc_probe_kernels)
fuse_apply_cxx23(fuse_rp_ltc)

set(_fuse_wp22_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")
set(_fuse_wp22_vk_tests "")
foreach(_lang slang glsl)
    set(_name "fuse_rp_ltc_vk_probe_${_lang}")
    if(FUSE_VULKAN_BACKEND)
        add_test(NAME ${_name} COMMAND "${_fuse_wp22_lock}" "$<TARGET_FILE:fuse_rp_ltc>" --language ${_lang})
    else()
        add_test(NAME ${_name} COMMAND fuse_rp_ltc --language ${_lang})
    endif()
    list(APPEND _fuse_wp22_vk_tests ${_name})
endforeach()
set_tests_properties(${_fuse_wp22_vk_tests} PROPERTIES
    ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
    TIMEOUT 900
    LABELS "gate;vulkan;renderer;lighting")
