# B2/B3/B7 RHI master-plan rows proven on Lavapipe: frame-ring timelines, debug names, memory
# usage types, SPIR-V validity, shader hot reload, push constants, G-buffer MRT pass, draw-list
# state changes and the Animator GPU bone buffer. Sources + gate tests for this work stream.
# Owned by one work stream; other topics must not edit this file.
#
# Every test here is labelled "gate;vulkan" and exits 77 (SKIP_RETURN_CODE) when there is no
# Vulkan backend / device / required tool, so the stub build reports them as skipped.

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/deferred/gbuffer_raster_pass.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/deferred/gbuffer_raster_pass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/skinning/bone_buffer.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/skinning/bone_buffer.cpp
)

# Runtime GLSL -> SPIR-V for ShaderCompiler hot reload (opt-in per compiler instance).
if(FUSE_GLSLANG_VALIDATOR)
    target_compile_definitions(fuse_rhi PRIVATE FUSE_GLSLANG_VALIDATOR_PATH="${FUSE_GLSLANG_VALIDATOR}")
endif()

# ---- Build-time SPIR-V for the raster test shaders (shaders/raster/*.{vert,frag}) -------------
# Same convention as FuseShaderSpirvRegen.cmake: glslangValidator -V, then spirv-val when present.
set(FUSE_B5_RHI_SHADER_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../shaders/raster")
set(FUSE_B5_RHI_SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders/raster")
set(_fuse_b5_rhi_spv_outputs "")
if(FUSE_GLSLANG_VALIDATOR)
    file(MAKE_DIRECTORY "${FUSE_B5_RHI_SHADER_OUT_DIR}")
    foreach(_shader gbuffer.vert gbuffer.frag push_constants.vert push_constants.frag)
        set(_src "${FUSE_B5_RHI_SHADER_SRC_DIR}/${_shader}")
        set(_out "${FUSE_B5_RHI_SHADER_OUT_DIR}/${_shader}.spv")
        set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" -V
                          -I${CMAKE_CURRENT_LIST_DIR}/../shaders/common "${_src}" -o "${_out}")
        if(FUSE_SPIRV_VAL)
            list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" "${_out}")
        endif()
        add_custom_command(OUTPUT "${_out}"
            ${_cmds}
            DEPENDS "${_src}" "${CMAKE_CURRENT_LIST_DIR}/../shaders/common/gbuffer.glsl"
            COMMENT "glslangValidator ${_shader} -> SPIR-V"
            VERBATIM)
        list(APPEND _fuse_b5_rhi_spv_outputs "${_out}")
    endforeach()
    add_custom_target(fuse_b5_rhi_shaders ALL DEPENDS ${_fuse_b5_rhi_spv_outputs})
    message(STATUS "FUSE: B5 RHI raster shaders compiled at build time (${FUSE_B5_RHI_SHADER_OUT_DIR})")
else()
    message(STATUS "FUSE: glslangValidator not found — B5 RHI raster shader tests will skip")
endif()

if(FUSE_BUILD_CORE_TESTS)
    # tests/CMakeLists.txt writes the ICD lock wrapper; its variable is scoped to that directory.
    set(_fuse_b5_rhi_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")
    set(_fuse_b5_rhi_tests "")

    # fuse_b5_rhi_add_test(<name> <source> [HOOKS] [LIBS ...])
    function(fuse_b5_rhi_add_test name source)
        cmake_parse_arguments(ARG "HOOKS" "" "LIBS" ${ARGN})
        set(_sources "${CMAKE_CURRENT_LIST_DIR}/../tests/${source}")
        if(ARG_HOOKS)
            list(APPEND _sources "${CMAKE_CURRENT_LIST_DIR}/../tests/b5_vk_call_hooks.cpp")
        endif()
        add_executable(${name} ${_sources})
        target_link_libraries(${name} PRIVATE fuse_rhi ${ARG_LIBS} ${CMAKE_DL_LIBS})
        target_compile_definitions(${name} PRIVATE
            FUSE_SHADER_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer/shaders/fixtures"
            FUSE_B5_RHI_SHADER_DIR="${FUSE_B5_RHI_SHADER_OUT_DIR}")
        if(TARGET fuse_b5_rhi_shaders)
            add_dependencies(${name} fuse_b5_rhi_shaders)
            target_compile_definitions(${name} PRIVATE FUSE_B5_RHI_SHADERS_BUILT=1)
        endif()
        add_test(NAME ${name} COMMAND "${_fuse_b5_rhi_lock}" "$<TARGET_FILE:${name}>")
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77
            LABELS "gate;vulkan")
    endfunction()

    # Row: frame-in-flight management holds three independent frame data sets (timeline values).
    fuse_b5_rhi_add_test(fuse_b5_rhi_frame_timelines test_b5_rhi_frame_timelines.cpp)
    # Row: all Vulkan objects named via vkSetDebugUtilsObjectNameEXT.
    fuse_b5_rhi_add_test(fuse_b5_rhi_object_names test_b5_rhi_object_names.cpp HOOKS)
    # Row: texture and buffer creation with all VMA memory usage types.
    fuse_b5_rhi_add_test(fuse_b5_rhi_memory_types test_b5_rhi_memory_types.cpp)
    # Row: hot reload rebuilds the pipeline in < 200 ms (GLSL write -> first redrawn frame).
    fuse_b5_rhi_add_test(fuse_b5_rhi_hot_reload test_b5_rhi_hot_reload.cpp)
    target_compile_definitions(fuse_b5_rhi_hot_reload PRIVATE
        FUSE_B5_RHI_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tests/b5_rhi_hot_reload")
    # Row: push constants pass per-draw data to shaders (pixel readback).
    fuse_b5_rhi_add_test(fuse_b5_rhi_push_constants test_b5_rhi_push_constants.cpp)
    # Row: G-buffer pass populates normal, albedo, depth attachments.
    fuse_b5_rhi_add_test(fuse_b5_rhi_gbuffer_pass test_b5_rhi_gbuffer_pass.cpp)
    # Row: draw list sorted by material, no redundant state changes.
    fuse_b5_rhi_add_test(fuse_b5_rhi_draw_state test_b5_rhi_draw_state.cpp HOOKS)
    # Row: Animator uploads the bone buffer every frame without leaks.
    fuse_b5_rhi_add_test(fuse_b5_rhi_bone_buffer test_b5_rhi_bone_buffer.cpp LIBS fuse_animation)

    # Row: shader compiler produces valid SPIR-V for all test shaders (spirv-val over every .spv).
    add_test(NAME fuse_b5_rhi_spirv_val
        COMMAND "${CMAKE_COMMAND}"
                -DFUSE_SPIRV_VAL=${FUSE_SPIRV_VAL}
                -DFUSE_GLSLANG_VALIDATOR=${FUSE_GLSLANG_VALIDATOR}
                -DFUSE_SHADER_DIRS=${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer/shaders/fixtures|${FUSE_B5_RHI_SHADER_OUT_DIR}
                -DFUSE_GLSL_SOURCE_DIRS=${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer/shaders/fixtures|${FUSE_B5_RHI_SHADER_SRC_DIR}
                -DFUSE_GLSL_INCLUDE_DIR=${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer/shaders/common
                -DFUSE_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/tests/b5_rhi_spirv_val
                -P "${CMAKE_CURRENT_LIST_DIR}/b5_rhi_spirv_val.cmake")
    set_tests_properties(fuse_b5_rhi_spirv_val PROPERTIES SKIP_RETURN_CODE 77 LABELS "gate;vulkan")
endif()
