# WP-0.5 (docs/unification/RENDERER-EXECUTION.md): Slang toolchain + pipeline cache v2.
# Owned by WP-0.5; other packages must not edit this file.
#
#   cmake/FuseSlang.cmake           slangc resolution (pinned download, FUSE_SLANG=AUTO|ON|OFF) and
#                                   fuse_add_slang_shaders(); reusable by other modules (Relight).
#   shaders/slang/*.slang           proof kernels: Slang twins of radix_histogram.comp and of
#                                   rp_twin_float.comp, compiled at build time (depfile, reflection
#                                   JSON, spirv-val)
#   fuse_rp_slang_twin              --mode twin | spirv-val | hot-reload (exit 77 without Slang)
#   fuse_rp_pipeline_cache_cold/warm  warm-start exit test (second run must hit 100%)

include("${CMAKE_SOURCE_DIR}/cmake/FuseSlang.cmake")

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/shader/shader_reflection.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/shader/shader_reflection.cpp
)
if(FUSE_SLANG_FOUND)
    # Runtime compiles (ShaderCompiler::compileWithSlang, hot reload) use the same slangc.
    target_compile_definitions(fuse_rhi PRIVATE FUSE_SLANGC_PATH="${FUSE_SLANGC_EXECUTABLE}")
endif()

set(_fuse_wp05_shader_dir "${CMAKE_CURRENT_LIST_DIR}/../shaders/slang")
get_filename_component(_fuse_wp05_shader_dir "${_fuse_wp05_shader_dir}" ABSOLUTE)
set(_fuse_wp05_out_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders/slang")

# Shader-only target: the proof kernels build even when tests are off (they are the Slang smoke).
add_custom_target(fuse_rp_slang_shaders ALL)
fuse_add_slang_shaders(fuse_rp_slang_shaders
    SOURCES "${_fuse_wp05_shader_dir}/rp_twin_histogram.slang" "${_fuse_wp05_shader_dir}/rp_twin_float.slang"
    STAGE compute
    OUTPUT_DIR "${_fuse_wp05_out_dir}"
    OUTPUT_VAR _fuse_wp05_slang_spv)

set(_fuse_wp05_twin_built FALSE)
if(_fuse_wp05_slang_spv AND FUSE_GLSLANG_VALIDATOR AND TARGET fuse_radix_sort_shaders)
    set(_fuse_wp05_glsl_float "${_fuse_wp05_out_dir}/rp_twin_float.comp.spv")
    set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" -V "${_fuse_wp05_shader_dir}/rp_twin_float.comp"
                      -o "${_fuse_wp05_glsl_float}")
    if(FUSE_SPIRV_VAL)
        list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" "${_fuse_wp05_glsl_float}")
    endif()
    add_custom_command(OUTPUT "${_fuse_wp05_glsl_float}" ${_cmds}
        DEPENDS "${_fuse_wp05_shader_dir}/rp_twin_float.comp"
        COMMENT "glslangValidator rp_twin_float.comp -> SPIR-V (Slang twin reference)" VERBATIM)
    add_custom_target(fuse_rp_slang_twin_glsl DEPENDS "${_fuse_wp05_glsl_float}")
    add_dependencies(fuse_rp_slang_shaders fuse_rp_slang_twin_glsl fuse_radix_sort_shaders)
    set(_fuse_wp05_twin_built TRUE)
endif()

if(NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

# tests/CMakeLists.txt writes the ICD lock wrapper; its variable is scoped to that directory.
set(_fuse_wp05_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")
set(_fuse_wp05_env
    "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
    "FUSE_SHADER_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/rp_wp05_shader_cache")

set(_fuse_wp05_twin_defs "")
if(_fuse_wp05_twin_built)
    set(_fuse_wp05_twin_defs
        FUSE_RP_SLANG_TWIN_BUILT=1
        FUSE_RP_SLANG_HISTOGRAM_SPV="${_fuse_wp05_out_dir}/rp_twin_histogram.spv"
        FUSE_RP_SLANG_HISTOGRAM_JSON="${_fuse_wp05_out_dir}/rp_twin_histogram.reflection.json"
        FUSE_RP_SLANG_HISTOGRAM_SRC="${_fuse_wp05_shader_dir}/rp_twin_histogram.slang"
        FUSE_RP_SLANG_FLOAT_SPV="${_fuse_wp05_out_dir}/rp_twin_float.spv"
        FUSE_RP_SLANG_FLOAT_SRC="${_fuse_wp05_shader_dir}/rp_twin_float.slang"
        FUSE_RP_GLSL_FLOAT_SPV="${_fuse_wp05_glsl_float}"
        FUSE_RP_GLSL_HISTOGRAM_SPV="${FUSE_RADIX_SORT_SHADER_OUT_DIR}/radix_histogram.comp.spv")
endif()

add_executable(fuse_rp_slang_twin ${CMAKE_CURRENT_LIST_DIR}/../tests/test_rp_slang_twin.cpp)
target_link_libraries(fuse_rp_slang_twin PRIVATE fuse_rhi)
target_compile_definitions(fuse_rp_slang_twin PRIVATE ${_fuse_wp05_twin_defs})
if(FUSE_SPIRV_VAL)
    target_compile_definitions(fuse_rp_slang_twin PRIVATE FUSE_RP_SPIRV_VAL="${FUSE_SPIRV_VAL}")
endif()
add_dependencies(fuse_rp_slang_twin fuse_rp_slang_shaders)
foreach(_mode twin spirv-val hot-reload)
    string(REPLACE "-" "_" _name "${_mode}")
    if(_mode STREQUAL "twin")
        set(_test fuse_rp_slang_twin)
    else()
        set(_test fuse_rp_slang_${_name})
    endif()
    if(FUSE_VULKAN_BACKEND)
        add_test(NAME ${_test} COMMAND "${_fuse_wp05_lock}" "$<TARGET_FILE:fuse_rp_slang_twin>" --mode ${_mode})
    else()
        add_test(NAME ${_test} COMMAND fuse_rp_slang_twin --mode ${_mode})
    endif()
    set_tests_properties(${_test} PROPERTIES
        ENVIRONMENT "${_fuse_wp05_env}"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        TIMEOUT 600
        LABELS "gate;vulkan;renderer;slang")
endforeach()

# Pipeline cache v2: the existing fuse_pipeline_cache binary (unit + v2 gates by default) also runs
# the two halves of the warm-start exit test. It builds the Slang twins into its pipeline set when
# they exist.
if(TARGET fuse_pipeline_cache)
    target_compile_definitions(fuse_pipeline_cache PRIVATE ${_fuse_wp05_twin_defs})
    if(_fuse_wp05_twin_built)
        add_dependencies(fuse_pipeline_cache fuse_rp_slang_shaders)
    endif()
    set(_fuse_wp05_warm_dir "${CMAKE_CURRENT_BINARY_DIR}/rp_wp05_pipeline_cache")
    foreach(_mode cold warm)
        if(FUSE_VULKAN_BACKEND)
            add_test(NAME fuse_rp_pipeline_cache_${_mode}
                     COMMAND "${_fuse_wp05_lock}" "$<TARGET_FILE:fuse_pipeline_cache>" --mode ${_mode}
                             --dir "${_fuse_wp05_warm_dir}")
        else()
            add_test(NAME fuse_rp_pipeline_cache_${_mode}
                     COMMAND fuse_pipeline_cache --mode ${_mode} --dir "${_fuse_wp05_warm_dir}")
        endif()
        set_tests_properties(fuse_rp_pipeline_cache_${_mode} PROPERTIES
            ENVIRONMENT "${_fuse_wp05_env}"
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77
            TIMEOUT 600
            LABELS "gate;vulkan;renderer")
    endforeach()
    set_tests_properties(fuse_rp_pipeline_cache_cold PROPERTIES FIXTURES_SETUP fuse_rp_pipeline_cache_dir)
    set_tests_properties(fuse_rp_pipeline_cache_warm PROPERTIES FIXTURES_REQUIRED fuse_rp_pipeline_cache_dir)
endif()
