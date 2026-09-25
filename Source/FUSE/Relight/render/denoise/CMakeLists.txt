# FUSE Relight RL-5.5: the path tracer's denoiser (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.5, RL-5.5 row; Wave R5).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(render/denoise), after render/pathtrace and
# before render/post (which links it into the RL-5.7 post hook). Owns Relight/render/denoise/**; the denoiser itself is
# the renderer's fuse_radiance_denoise (Source/FUSE/Renderer/cmake/rp_rl55.cmake).
#
# Targets
#   fuse_relight_denoise          PtDenoiser (pt_denoise.hpp: "relight.denoise.gradient" - the A-SVGF producer on the
#                                 path tracer - + the rdn.* passes on RL-5.1's output sections), PtDenoiseCpu (the CPU
#                                 oracle: CPU reference path tracer + gradient producer + RdnReference), the options
#                                 (rtx.useDenoiser, relight.denoise.*) and the backend selection (plugins / dlss_rr).
#   fuse_relight_denoise_kernels  the gradient producer's SPIR-V: Slang (shaders/rl_dn_gradient.slang, -fp-mode
#                                 precise) + GLSL twin (rl_dn_gradient.comp), spirv-val'd, embedded.
# Tests: tests/ (rl_denoise_*).

foreach(_dep fuse_relight_pathtrace fuse_relight_options fuse_radiance_denoise fuse_denoiser fuse_relight_material)
    if(NOT TARGET ${_dep})
        message(STATUS "FUSE Relight: RL-5.5 render/denoise skipped (no ${_dep})")
        return()
    endif()
endforeach()

get_filename_component(RL_DN_RELIGHT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(RL_DN_RENDERER_DIR "${RL_DN_RELIGHT_DIR}/../Renderer" ABSOLUTE)
set(RL_DN_SHADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(RL_DN_INCLUDE_DIRS "${RL_DN_SHADERS_DIR}" "${RL_DN_RELIGHT_DIR}/kernels" "${RL_DN_RELIGHT_DIR}/render/pathtrace/shaders"
                       "${RL_DN_RELIGHT_DIR}/shaders/material" "${RL_DN_RELIGHT_DIR}/render/lights/shaders"
                       "${RL_DN_RENDERER_DIR}/shaders/light_tree" "${RL_DN_RENDERER_DIR}/shaders/common")

file(GLOB _rl_dn_shader_files CONFIGURE_DEPENDS "${RL_DN_SHADERS_DIR}/*")
file(GLOB _rl_dn_core_deps CONFIGURE_DEPENDS "${RL_DN_RELIGHT_DIR}/kernels/*.h" "${RL_DN_RELIGHT_DIR}/kernels/*.glsl"
                                             "${RL_DN_RELIGHT_DIR}/kernels/*.slang"
                                             "${RL_DN_RELIGHT_DIR}/render/pathtrace/shaders/*"
                                             "${RL_DN_RELIGHT_DIR}/shaders/material/*"
                                             "${RL_DN_RELIGHT_DIR}/render/lights/shaders/*"
                                             "${RL_DN_RENDERER_DIR}/shaders/light_tree/*")
add_library(fuse_relight_denoise STATIC
    src/pt_denoise.cpp
    src/pt_denoise_cpu.cpp
    src/rl_dn_gradient_cpp.hpp
    include/fuse/relight/render/denoise/pt_denoise.hpp
    ${_rl_dn_shader_files})
set_source_files_properties(${_rl_dn_shader_files} PROPERTIES HEADER_FILE_ONLY TRUE)
target_include_directories(fuse_relight_denoise PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                           PRIVATE "${RL_DN_SHADERS_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(fuse_relight_denoise PUBLIC fuse_relight_pathtrace fuse_relight_options fuse_radiance_denoise
                      fuse_denoiser PRIVATE fuse_relight_material)
_rl_pt_cxx23(fuse_relight_denoise)

# --- the gradient producer's kernels (Slang primary, GLSL twin), embedded -----------------------------------------------
set(_rl_dn_gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(_rl_dn_spv "${CMAKE_CURRENT_BINARY_DIR}/spv")
file(MAKE_DIRECTORY "${_rl_dn_gen}" "${_rl_dn_spv}")
set(_rl_dn_header "${_rl_dn_gen}/rl_dn_gradient_spv.h")
add_custom_target(fuse_relight_denoise_kernels)
set(_rl_dn_args "")
set(_rl_dn_deps "")
if(COMMAND fuse_add_slang_shaders)
    fuse_add_slang_shaders(fuse_relight_denoise_kernels
        SOURCES "${RL_DN_SHADERS_DIR}/rl_dn_gradient.slang"
        STAGE compute
        INCLUDE_DIRS ${RL_DN_INCLUDE_DIRS}
        FLAGS -fp-mode precise
        OUTPUT_DIR "${_rl_dn_spv}"
        OUTPUT_VAR _rl_dn_slang_out)
    list(LENGTH _rl_dn_slang_out _n)
    if(_n EQUAL 1)
        list(APPEND _rl_dn_args "-DSLANG_SPV=${_rl_dn_slang_out}")
        list(APPEND _rl_dn_deps "${_rl_dn_slang_out}")
    endif()
endif()
if(FUSE_GLSLANG_VALIDATOR)
    set(_out "${_rl_dn_spv}/rl_dn_gradient.glsl.spv")
    set(_incs "")
    foreach(_d IN LISTS RL_DN_INCLUDE_DIRS)
        list(APPEND _incs "-I${_d}")
    endforeach()
    set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 ${_incs}
                      "${RL_DN_SHADERS_DIR}/rl_dn_gradient.comp" -o "${_out}")
    if(FUSE_SPIRV_VAL)
        list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_out}")
    endif()
    add_custom_command(OUTPUT "${_out}" ${_cmds}
        DEPENDS "${RL_DN_SHADERS_DIR}/rl_dn_gradient.comp" "${RL_DN_SHADERS_DIR}/rl_dn_gradient_core.h"
                ${_rl_dn_core_deps} "${RL_DN_RENDERER_DIR}/shaders/common/bindless.glsl"
        COMMENT "glslangValidator rl_dn_gradient.comp (GLSL twin of the RL-5.5 gradient producer)"
        VERBATIM)
    list(APPEND _rl_dn_args "-DGLSL_SPV=${_out}")
    list(APPEND _rl_dn_deps "${_out}")
endif()
add_custom_command(OUTPUT "${_rl_dn_header}"
    COMMAND "${CMAKE_COMMAND}" "-DOUT=${_rl_dn_header}" ${_rl_dn_args}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
    DEPENDS ${_rl_dn_deps} "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
    COMMENT "Embedding the RL-5.5 gradient producer's SPIR-V"
    VERBATIM)
add_custom_target(fuse_relight_denoise_header DEPENDS "${_rl_dn_header}")
add_dependencies(fuse_relight_denoise_header fuse_relight_denoise_kernels)
add_dependencies(fuse_relight_denoise fuse_relight_denoise_header)
target_include_directories(fuse_relight_denoise PRIVATE "${_rl_dn_gen}")

add_subdirectory(tests)
