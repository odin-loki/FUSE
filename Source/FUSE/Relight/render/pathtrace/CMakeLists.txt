# FUSE Relight RL-5.1: path-tracing core, primary / PSR and the CPU reference path tracer
# (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.1, §5.2, §5.3, §5.8, §6.5; Wave R5). Added by one line in
# Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(render/pathtrace), after RL-4.4's render/lights. Owns
# Relight/render/pathtrace/** and Relight/kernels/pt_reference*.
#
# Targets
#   fuse_relight_pathtrace          the single-source path-tracing core (kernels/pt_reference_{types,core}.h; C++ dialect
#                                   pt_reference_cpp.hpp, kernel body pt_reference_kernels.hpp), the scene compiler
#                                   (pt_scene.hpp: WP-1.2 meshlet cooking -> the WP-6.0 BLAS input, packed tables,
#                                   RL-4.4 light set with emissive triangles, the WP-6.0 CPU reference BVH), the CPU
#                                   reference path tracer (pt_reference.hpp), the RL-1.8 capture loader
#                                   (pt_capture_scene.hpp) and PathTracerGpu (pt_gpu.hpp: ray-query pass
#                                   "relight.pt.trace" on the WP-6.0 TLAS; "unsupported" in the stub backend).
#   fuse_relight_pathtrace_kernels  the SPIR-V: Slang (shaders/rl_pt_trace.slang, -fp-mode precise) and the GLSL twin
#                                   (rl_pt_trace.comp, glslangValidator), spirv-val'd and embedded (rl_pt_spv.h).
#   fuse_relight_pathtrace_frame    relight.frame.mode = pathtrace (pt_frame_renderer.hpp): RL-4.2's CPU frame -> PtScene,
#                                   PathTracerGpu with accumulation, output through RL-4.1's composite.
# Tests: tests/ (rl_pt_*).

foreach(_dep fuse_core fuse_relight_bsdf fuse_relight_material fuse_relight_lights fuse_relight_mods_import
             fuse_rt fuse_rt_reference fuse_geometry fuse_gpu_scene)
    if(NOT TARGET ${_dep})
        message(STATUS "FUSE Relight: RL-5.1 render/pathtrace skipped (no ${_dep})")
        return()
    endif()
endforeach()

macro(_rl_pt_cxx23 _t)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endmacro()

get_filename_component(RL_PT_KERNELS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../kernels" ABSOLUTE)
get_filename_component(RL_PT_RELIGHT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(RL_PT_RENDERER_DIR "${RL_PT_RELIGHT_DIR}/../Renderer" ABSOLUTE)
set(RL_PT_SHADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(RL_PT_INCLUDE_DIRS "${RL_PT_KERNELS_DIR}" "${RL_PT_SHADERS_DIR}" "${RL_PT_RELIGHT_DIR}/shaders/material"
                       "${RL_PT_RELIGHT_DIR}/render/lights/shaders" "${RL_PT_RENDERER_DIR}/shaders/light_tree"
                       "${RL_PT_RENDERER_DIR}/shaders/common")

file(GLOB _rl_pt_shader_files CONFIGURE_DEPENDS "${RL_PT_SHADERS_DIR}/*")
add_library(fuse_relight_pathtrace STATIC
    src/pt_scene.cpp
    src/pt_reference.cpp
    src/pt_capture_scene.cpp
    src/pt_gpu.cpp
    "${RL_PT_KERNELS_DIR}/pt_reference_types.h"
    "${RL_PT_KERNELS_DIR}/pt_reference_core.h"
    "${RL_PT_KERNELS_DIR}/pt_reference_cpp.hpp"
    "${RL_PT_KERNELS_DIR}/pt_reference_kernels.hpp"
    ${_rl_pt_shader_files})
set_source_files_properties(${_rl_pt_shader_files} PROPERTIES HEADER_FILE_ONLY TRUE)
target_include_directories(fuse_relight_pathtrace PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${RL_PT_KERNELS_DIR}")
target_link_libraries(fuse_relight_pathtrace
    PUBLIC fuse_core fuse_relight_bsdf fuse_relight_lights fuse_rt fuse_rt_reference fuse_geometry fuse_gpu_scene
    PRIVATE fuse_relight_material fuse_relight_mods_import fuse_relight_mods_usd)
_rl_pt_cxx23(fuse_relight_pathtrace)

# --- kernels (Slang primary, GLSL twin), embedded ------------------------------------------------------------------
set(_rl_pt_gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(_rl_pt_spv "${CMAKE_CURRENT_BINARY_DIR}/spv")
file(MAKE_DIRECTORY "${_rl_pt_gen}" "${_rl_pt_spv}")
set(_rl_pt_header "${_rl_pt_gen}/rl_pt_spv.h")
add_custom_target(fuse_relight_pathtrace_kernels)
set(_rl_pt_embed_args "")
set(_rl_pt_embed_deps "")
set(_rl_pt_core_deps "${RL_PT_KERNELS_DIR}/pt_reference_types.h" "${RL_PT_KERNELS_DIR}/pt_reference_core.h"
                     "${RL_PT_KERNELS_DIR}/bsdf_core.h" "${RL_PT_KERNELS_DIR}/light_core.h"
                     "${RL_PT_RENDERER_DIR}/shaders/light_tree/lt_common.slang"
                     "${RL_PT_RENDERER_DIR}/shaders/light_tree/lt_common.glsl"
                     "${RL_PT_RELIGHT_DIR}/render/lights/shaders/rl_lights.slang"
                     "${RL_PT_RELIGHT_DIR}/render/lights/shaders/rl_lights.glsl"
                     "${RL_PT_RELIGHT_DIR}/shaders/material/bsdf.slang"
                     "${RL_PT_RELIGHT_DIR}/shaders/material/bsdf.glsl"
                     "${RL_PT_SHADERS_DIR}/rl_pt.slang" "${RL_PT_SHADERS_DIR}/rl_pt.glsl"
                     "${RL_PT_SHADERS_DIR}/restir_di_hook.slang" "${RL_PT_SHADERS_DIR}/restir_di_hook.glsl"
                     "${RL_PT_SHADERS_DIR}/restir_gi_hook.slang" "${RL_PT_SHADERS_DIR}/restir_gi_hook.glsl"
                     "${RL_PT_KERNELS_DIR}/radiance_cache_types.h" "${RL_PT_KERNELS_DIR}/radiance_cache_core.h"
                     "${RL_PT_KERNELS_DIR}/radiance_cache_path.h" "${RL_PT_KERNELS_DIR}/radiance_cache_hook.slang"
                     "${RL_PT_KERNELS_DIR}/radiance_cache_hook.glsl")
if(COMMAND fuse_add_slang_shaders)
    fuse_add_slang_shaders(fuse_relight_pathtrace_kernels
        SOURCES "${RL_PT_SHADERS_DIR}/rl_pt_trace.slang"
        STAGE compute
        INCLUDE_DIRS ${RL_PT_INCLUDE_DIRS}
        FLAGS -fp-mode precise
        OUTPUT_DIR "${_rl_pt_spv}"
        OUTPUT_VAR _rl_pt_slang_out)
    list(LENGTH _rl_pt_slang_out _n)
    if(_n EQUAL 1)
        list(APPEND _rl_pt_embed_args "-DSLANG_TRACE=${_rl_pt_slang_out}")
        list(APPEND _rl_pt_embed_deps "${_rl_pt_slang_out}")
    endif()
endif()
if(FUSE_GLSLANG_VALIDATOR)
    set(_out "${_rl_pt_spv}/rl_pt_trace.glsl.spv")
    set(_incs "")
    foreach(_d IN LISTS RL_PT_INCLUDE_DIRS)
        list(APPEND _incs "-I${_d}")
    endforeach()
    set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 ${_incs}
                      "${RL_PT_SHADERS_DIR}/rl_pt_trace.comp" -o "${_out}")
    if(FUSE_SPIRV_VAL)
        list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_out}")
    endif()
    add_custom_command(OUTPUT "${_out}" ${_cmds}
        DEPENDS "${RL_PT_SHADERS_DIR}/rl_pt_trace.comp" ${_rl_pt_core_deps}
                "${RL_PT_RENDERER_DIR}/shaders/common/bindless.glsl"
        COMMENT "glslangValidator rl_pt_trace.comp (GLSL twin of the Relight path tracer)"
        VERBATIM)
    list(APPEND _rl_pt_embed_args "-DGLSL_TRACE=${_out}")
    list(APPEND _rl_pt_embed_deps "${_out}")
endif()
add_custom_command(OUTPUT "${_rl_pt_header}"
    COMMAND "${CMAKE_COMMAND}" "-DOUT=${_rl_pt_header}" ${_rl_pt_embed_args}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
    DEPENDS ${_rl_pt_embed_deps} "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
    COMMENT "Embedding the Relight path tracer's SPIR-V"
    VERBATIM)
add_custom_target(fuse_relight_pathtrace_header DEPENDS "${_rl_pt_header}")
add_dependencies(fuse_relight_pathtrace_header fuse_relight_pathtrace_kernels)
add_dependencies(fuse_relight_pathtrace fuse_relight_pathtrace_header)
target_include_directories(fuse_relight_pathtrace PRIVATE "${_rl_pt_gen}")

# relight.frame.mode = pathtrace (pt_frame_renderer.hpp): the IFrameRenderer RL-4.1's render_tap creates; needs RL-4.2's
# CPU frame (render/raster).
if(TARGET fuse_relight_render_raster AND TARGET fuse_relight_render_frame)
    add_library(fuse_relight_pathtrace_frame STATIC src/pt_frame_renderer.cpp)
    target_link_libraries(fuse_relight_pathtrace_frame
        PUBLIC fuse_relight_pathtrace fuse_relight_render_raster fuse_relight_render_frame fuse_relight_options
        PRIVATE fuse_rhi)
    _rl_pt_cxx23(fuse_relight_pathtrace_frame)
endif()

# RL-5.2 ReSTIR DI (restir_di*): the CPU runner and the GPU passes ("relight.restir_di.*"), the options rtx.useRTXDI /
# rtx.di.*. The shared core is shaders/restir_di_core.h (C++ dialect src/restir_di_cpp.hpp).
if(TARGET fuse_relight_options)
    set(_rl_rdi_header "${_rl_pt_gen}/rl_restir_di_spv.h")
    set(_rl_rdi_args "")
    set(_rl_rdi_deps ${_rl_pt_core_deps} "${RL_PT_SHADERS_DIR}/restir_di_core.h")
    add_custom_target(fuse_relight_restir_di_kernels)
    if(COMMAND fuse_add_slang_shaders)
        fuse_add_slang_shaders(fuse_relight_restir_di_kernels
            SOURCES "${RL_PT_SHADERS_DIR}/restir_di.slang"
            STAGE compute
            INCLUDE_DIRS ${RL_PT_INCLUDE_DIRS}
            FLAGS -fp-mode precise
            OUTPUT_DIR "${_rl_pt_spv}"
            OUTPUT_VAR _rl_rdi_slang_out)
        list(LENGTH _rl_rdi_slang_out _n)
        if(_n EQUAL 1)
            list(APPEND _rl_rdi_args "-DSLANG_SPV=${_rl_rdi_slang_out}")
            list(APPEND _rl_rdi_deps "${_rl_rdi_slang_out}")
        endif()
    endif()
    if(FUSE_GLSLANG_VALIDATOR)
        set(_out "${_rl_pt_spv}/restir_di.glsl.spv")
        set(_incs "")
        foreach(_d IN LISTS RL_PT_INCLUDE_DIRS)
            list(APPEND _incs "-I${_d}")
        endforeach()
        set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 ${_incs}
                          "${RL_PT_SHADERS_DIR}/restir_di.comp" -o "${_out}")
        if(FUSE_SPIRV_VAL)
            list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_out}")
        endif()
        add_custom_command(OUTPUT "${_out}" ${_cmds}
            DEPENDS "${RL_PT_SHADERS_DIR}/restir_di.comp" ${_rl_pt_core_deps} "${RL_PT_SHADERS_DIR}/restir_di_core.h"
                    "${RL_PT_RENDERER_DIR}/shaders/common/bindless.glsl"
            COMMENT "glslangValidator restir_di.comp (GLSL twin of the ReSTIR DI stages)"
            VERBATIM)
        list(APPEND _rl_rdi_args "-DGLSL_SPV=${_out}")
        list(APPEND _rl_rdi_deps "${_out}")
    endif()
    add_custom_command(OUTPUT "${_rl_rdi_header}"
        COMMAND "${CMAKE_COMMAND}" "-DOUT=${_rl_rdi_header}" ${_rl_rdi_args}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/restir_di_embed.cmake"
        DEPENDS ${_rl_rdi_deps} "${CMAKE_CURRENT_SOURCE_DIR}/restir_di_embed.cmake"
        COMMENT "Embedding the ReSTIR DI SPIR-V"
        VERBATIM)
    add_custom_target(fuse_relight_restir_di_header DEPENDS "${_rl_rdi_header}")
    add_dependencies(fuse_relight_restir_di_header fuse_relight_restir_di_kernels)

    add_library(fuse_relight_restir_di STATIC src/restir_di.cpp src/restir_di_gpu.cpp src/restir_di_cpp.hpp
                "${RL_PT_SHADERS_DIR}/restir_di_core.h")
    target_include_directories(fuse_relight_restir_di PRIVATE "${RL_PT_SHADERS_DIR}" "${_rl_pt_gen}")
    target_link_libraries(fuse_relight_restir_di PUBLIC fuse_relight_pathtrace fuse_relight_options
                          PRIVATE fuse_relight_material)
    add_dependencies(fuse_relight_restir_di fuse_relight_restir_di_header)
    _rl_pt_cxx23(fuse_relight_restir_di)
    if(TARGET fuse_relight_pathtrace_frame)
        target_link_libraries(fuse_relight_pathtrace_frame PRIVATE fuse_relight_restir_di) # rtx.useRTXDI
    endif()
endif()

# RL-5.3 ReSTIR GI (restir_gi*): the CPU runner and the GPU passes ("relight.restir_gi.*"), the options
# rtx.useReSTIRGI / rtx.restirGI.*. The shared core is shaders/restir_gi_core.h (C++ dialect src/restir_gi_cpp.hpp).
if(TARGET fuse_relight_options)
    set(_rl_rgi_header "${_rl_pt_gen}/rl_restir_gi_spv.h")
    set(_rl_rgi_args "")
    set(_rl_rgi_deps ${_rl_pt_core_deps} "${RL_PT_SHADERS_DIR}/restir_gi_core.h")
    add_custom_target(fuse_relight_restir_gi_kernels)
    if(COMMAND fuse_add_slang_shaders)
        fuse_add_slang_shaders(fuse_relight_restir_gi_kernels
            SOURCES "${RL_PT_SHADERS_DIR}/restir_gi.slang"
            STAGE compute
            INCLUDE_DIRS ${RL_PT_INCLUDE_DIRS}
            FLAGS -fp-mode precise
            OUTPUT_DIR "${_rl_pt_spv}"
            OUTPUT_VAR _rl_rgi_slang_out)
        list(LENGTH _rl_rgi_slang_out _n)
        if(_n EQUAL 1)
            list(APPEND _rl_rgi_args "-DSLANG_SPV=${_rl_rgi_slang_out}")
            list(APPEND _rl_rgi_deps "${_rl_rgi_slang_out}")
        endif()
    endif()
    if(FUSE_GLSLANG_VALIDATOR)
        set(_out "${_rl_pt_spv}/restir_gi.glsl.spv")
        set(_incs "")
        foreach(_d IN LISTS RL_PT_INCLUDE_DIRS)
            list(APPEND _incs "-I${_d}")
        endforeach()
        set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 ${_incs}
                          "${RL_PT_SHADERS_DIR}/restir_gi.comp" -o "${_out}")
        if(FUSE_SPIRV_VAL)
            list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_out}")
        endif()
        add_custom_command(OUTPUT "${_out}" ${_cmds}
            DEPENDS "${RL_PT_SHADERS_DIR}/restir_gi.comp" ${_rl_pt_core_deps} "${RL_PT_SHADERS_DIR}/restir_gi_core.h"
                    "${RL_PT_RENDERER_DIR}/shaders/common/bindless.glsl"
            COMMENT "glslangValidator restir_gi.comp (GLSL twin of the ReSTIR GI stages)"
            VERBATIM)
        list(APPEND _rl_rgi_args "-DGLSL_SPV=${_out}")
        list(APPEND _rl_rgi_deps "${_out}")
    endif()
    add_custom_command(OUTPUT "${_rl_rgi_header}"
        COMMAND "${CMAKE_COMMAND}" "-DOUT=${_rl_rgi_header}" ${_rl_rgi_args}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/restir_gi_embed.cmake"
        DEPENDS ${_rl_rgi_deps} "${CMAKE_CURRENT_SOURCE_DIR}/restir_gi_embed.cmake"
        COMMENT "Embedding the ReSTIR GI SPIR-V"
        VERBATIM)
    add_custom_target(fuse_relight_restir_gi_header DEPENDS "${_rl_rgi_header}")
    add_dependencies(fuse_relight_restir_gi_header fuse_relight_restir_gi_kernels)

    add_library(fuse_relight_restir_gi STATIC src/restir_gi.cpp src/restir_gi_gpu.cpp src/restir_gi_cpp.hpp
                "${RL_PT_SHADERS_DIR}/restir_gi_core.h")
    target_include_directories(fuse_relight_restir_gi PRIVATE "${RL_PT_SHADERS_DIR}" "${_rl_pt_gen}")
    target_link_libraries(fuse_relight_restir_gi PUBLIC fuse_relight_pathtrace fuse_relight_options
                          PRIVATE fuse_relight_material)
    add_dependencies(fuse_relight_restir_gi fuse_relight_restir_gi_header)
    _rl_pt_cxx23(fuse_relight_restir_gi)
    if(TARGET fuse_relight_pathtrace_frame)
        target_link_libraries(fuse_relight_pathtrace_frame PRIVATE fuse_relight_restir_gi) # rtx.useReSTIRGI
    endif()
endif()

add_subdirectory(tests)
