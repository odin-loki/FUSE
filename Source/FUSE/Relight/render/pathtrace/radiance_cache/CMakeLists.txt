# FUSE Relight RL-5.4: world-space hash-grid radiance cache for the path tracer (docs/plans/FUSE_REMIX_PORT_PLAN.md
# §5.4, §5.8 row `radiance_cache_*`; Wave R5). Added by one line in Source/FUSE/Relight/CMakeLists.txt, after RL-5.1's
# render/pathtrace. Owns Relight/render/pathtrace/radiance_cache*, Relight/kernels/radiance_cache*.
#
# Targets
#   fuse_relight_radiance_cache          RadianceCacheCpu (the CPU reference: kernels/radiance_cache_{types,core,path}.h
#                                        through kernels/radiance_cache_cpp.hpp) and RadianceCacheGpu (the
#                                        "relight.radiance_cache.*" passes; "unsupported" in the stub backend).
#   fuse_relight_radiance_cache_kernels  the SPIR-V: Slang (shaders/radiance_cache.slang, -fp-mode precise) and the GLSL
#                                        twin (radiance_cache.comp), spirv-val'd and embedded (rl_radiance_cache_spv.h).
# The path tracer's hook (kernels/radiance_cache_hook.{slang,glsl}) is compiled into RL-5.1's kernels (rl_pt.*).
# Tests: tests/ (rl_radiance_cache_*).

foreach(_dep fuse_relight_pathtrace fuse_relight_options fuse_relight_material)
    if(NOT TARGET ${_dep})
        message(STATUS "FUSE Relight: RL-5.4 radiance cache skipped (no ${_dep})")
        return()
    endif()
endforeach()

macro(_rl_rc_cxx23 _t)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endmacro()

get_filename_component(RL_RC_RELIGHT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
get_filename_component(RL_RC_RENDERER_DIR "${RL_RC_RELIGHT_DIR}/../Renderer" ABSOLUTE)
set(RL_RC_KERNELS_DIR "${RL_RC_RELIGHT_DIR}/kernels")
set(RL_RC_PT_SHADERS_DIR "${RL_RC_RELIGHT_DIR}/render/pathtrace/shaders")
set(RL_RC_SHADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(RL_RC_INCLUDE_DIRS "${RL_RC_KERNELS_DIR}" "${RL_RC_PT_SHADERS_DIR}" "${RL_RC_RELIGHT_DIR}/shaders/material"
                       "${RL_RC_RELIGHT_DIR}/render/lights/shaders" "${RL_RC_RENDERER_DIR}/shaders/light_tree"
                       "${RL_RC_RENDERER_DIR}/shaders/common")
set(RL_RC_KERNEL_FILES "${RL_RC_KERNELS_DIR}/radiance_cache_types.h" "${RL_RC_KERNELS_DIR}/radiance_cache_core.h"
                       "${RL_RC_KERNELS_DIR}/radiance_cache_path.h" "${RL_RC_KERNELS_DIR}/radiance_cache_cpp.hpp"
                       "${RL_RC_KERNELS_DIR}/radiance_cache_hook.slang" "${RL_RC_KERNELS_DIR}/radiance_cache_hook.glsl")
file(GLOB _rl_rc_deps CONFIGURE_DEPENDS "${RL_RC_PT_SHADERS_DIR}/*" "${RL_RC_KERNELS_DIR}/*.h"
     "${RL_RC_RELIGHT_DIR}/shaders/material/*" "${RL_RC_RELIGHT_DIR}/render/lights/shaders/*"
     "${RL_RC_RENDERER_DIR}/shaders/light_tree/*" "${RL_RC_RENDERER_DIR}/shaders/common/bindless.glsl")
list(APPEND _rl_rc_deps ${RL_RC_KERNEL_FILES})

set(_rl_rc_gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(_rl_rc_spv "${CMAKE_CURRENT_BINARY_DIR}/spv")
file(MAKE_DIRECTORY "${_rl_rc_gen}" "${_rl_rc_spv}")
set(_rl_rc_header "${_rl_rc_gen}/rl_radiance_cache_spv.h")
set(_rl_rc_args "")
set(_rl_rc_embed_deps "")
add_custom_target(fuse_relight_radiance_cache_kernels)
if(COMMAND fuse_add_slang_shaders)
    fuse_add_slang_shaders(fuse_relight_radiance_cache_kernels
        SOURCES "${RL_RC_SHADERS_DIR}/radiance_cache.slang"
        STAGE compute
        INCLUDE_DIRS ${RL_RC_INCLUDE_DIRS}
        FLAGS -fp-mode precise
        OUTPUT_DIR "${_rl_rc_spv}"
        OUTPUT_VAR _rl_rc_slang_out)
    list(LENGTH _rl_rc_slang_out _n)
    if(_n EQUAL 1)
        list(APPEND _rl_rc_args "-DSLANG_SPV=${_rl_rc_slang_out}")
        list(APPEND _rl_rc_embed_deps "${_rl_rc_slang_out}")
    endif()
endif()
if(FUSE_GLSLANG_VALIDATOR)
    set(_out "${_rl_rc_spv}/radiance_cache.glsl.spv")
    set(_incs "")
    foreach(_d IN LISTS RL_RC_INCLUDE_DIRS)
        list(APPEND _incs "-I${_d}")
    endforeach()
    set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 ${_incs}
                      "${RL_RC_SHADERS_DIR}/radiance_cache.comp" -o "${_out}")
    if(FUSE_SPIRV_VAL)
        list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_out}")
    endif()
    add_custom_command(OUTPUT "${_out}" ${_cmds}
        DEPENDS "${RL_RC_SHADERS_DIR}/radiance_cache.comp" ${_rl_rc_deps}
        COMMENT "glslangValidator radiance_cache.comp (GLSL twin of the radiance cache stages)"
        VERBATIM)
    list(APPEND _rl_rc_args "-DGLSL_SPV=${_out}")
    list(APPEND _rl_rc_embed_deps "${_out}")
endif()
add_custom_command(OUTPUT "${_rl_rc_header}"
    COMMAND "${CMAKE_COMMAND}" "-DOUT=${_rl_rc_header}" ${_rl_rc_args}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/radiance_cache_embed.cmake"
    DEPENDS ${_rl_rc_embed_deps} "${CMAKE_CURRENT_SOURCE_DIR}/radiance_cache_embed.cmake"
    COMMENT "Embedding the radiance cache SPIR-V"
    VERBATIM)
add_custom_target(fuse_relight_radiance_cache_header DEPENDS "${_rl_rc_header}")
add_dependencies(fuse_relight_radiance_cache_header fuse_relight_radiance_cache_kernels)

file(GLOB _rl_rc_shader_files CONFIGURE_DEPENDS "${RL_RC_SHADERS_DIR}/*")
add_library(fuse_relight_radiance_cache STATIC src/radiance_cache.cpp src/radiance_cache_gpu.cpp
            ${RL_RC_KERNEL_FILES} ${_rl_rc_shader_files})
set_source_files_properties(${_rl_rc_shader_files} "${RL_RC_KERNELS_DIR}/radiance_cache_hook.slang"
                            "${RL_RC_KERNELS_DIR}/radiance_cache_hook.glsl" PROPERTIES HEADER_FILE_ONLY TRUE)
target_include_directories(fuse_relight_radiance_cache PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                           PRIVATE "${RL_RC_KERNELS_DIR}" "${_rl_rc_gen}")
target_link_libraries(fuse_relight_radiance_cache PUBLIC fuse_relight_pathtrace fuse_relight_options
                      PRIVATE fuse_relight_material)
add_dependencies(fuse_relight_radiance_cache fuse_relight_radiance_cache_header)
_rl_rc_cxx23(fuse_relight_radiance_cache)

add_subdirectory(tests)
