# FUSE Relight RL-5.6: volumetrics (froxel media, RIS over the RL-4.4 light set + reservoir reuse, temporal filter, D3D
# fog mapping) and the particle composite (ray-traced RL-3.6 billboards with D3D blend modes)
# (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.1, §5.7, §5.8). Added by one line in Source/FUSE/Relight/CMakeLists.txt:
# add_subdirectory(render/volumetrics), after RL-5.1's render/pathtrace. Owns Relight/render/volumetrics/**.
#
# Targets
#   fuse_relight_volumetrics          the single-source core (shaders/rl_vol_types.h + rl_vol_core.h; C++ dialect
#                                     src/vol_cpp.hpp), the CPU reference (VolumetricsCpu, volumetrics.hpp) and the GPU
#                                     passes (VolumetricsGpu, volumetrics_gpu.hpp: "relight.vol.*"; "stub backend" in
#                                     trees without Vulkan).
#   fuse_relight_volumetrics_kernels  the SPIR-V: Slang (shaders/rl_vol.slang, -fp-mode precise) and the GLSL twin
#                                     (rl_vol.comp), spirv-val'd and embedded (rl_vol_spv.h).
# Tests: tests/ (rl_vol_*).

foreach(_dep fuse_relight_pathtrace fuse_relight_lights fuse_relight_particles fuse_rt_reference)
    if(NOT TARGET ${_dep})
        message(STATUS "FUSE Relight: RL-5.6 render/volumetrics skipped (no ${_dep})")
        return()
    endif()
endforeach()

macro(_rl_vol_cxx23 _t)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endmacro()

get_filename_component(RL_VOL_RELIGHT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(RL_VOL_RENDERER_DIR "${RL_VOL_RELIGHT_DIR}/../Renderer" ABSOLUTE)
set(RL_VOL_SHADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(RL_VOL_INCLUDE_DIRS "${RL_VOL_SHADERS_DIR}" "${RL_VOL_RELIGHT_DIR}/kernels" "${RL_VOL_RELIGHT_DIR}/render/lights/shaders"
                        "${RL_VOL_RENDERER_DIR}/shaders/light_tree")

file(GLOB _rl_vol_shader_files CONFIGURE_DEPENDS "${RL_VOL_SHADERS_DIR}/*")
add_library(fuse_relight_volumetrics STATIC
    src/vol_cpu.cpp
    src/vol_gpu.cpp
    src/vol_cpp.hpp
    ${_rl_vol_shader_files})
set_source_files_properties(${_rl_vol_shader_files} PROPERTIES HEADER_FILE_ONLY TRUE)
target_include_directories(fuse_relight_volumetrics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                           PRIVATE "${RL_VOL_SHADERS_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(fuse_relight_volumetrics PUBLIC fuse_relight_pathtrace fuse_relight_lights fuse_relight_particles
                      fuse_rt_reference)
_rl_vol_cxx23(fuse_relight_volumetrics)

set(_rl_vol_gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(_rl_vol_spv "${CMAKE_CURRENT_BINARY_DIR}/spv")
file(MAKE_DIRECTORY "${_rl_vol_gen}" "${_rl_vol_spv}")
set(_rl_vol_header "${_rl_vol_gen}/rl_vol_spv.h")
add_custom_target(fuse_relight_volumetrics_kernels)
set(_rl_vol_args "")
set(_rl_vol_deps "")
set(_rl_vol_core_deps "${RL_VOL_SHADERS_DIR}/rl_vol_types.h" "${RL_VOL_SHADERS_DIR}/rl_vol_core.h"
                      "${RL_VOL_RELIGHT_DIR}/kernels/light_core.h"
                      "${RL_VOL_RELIGHT_DIR}/render/lights/shaders/rl_lights.glsl"
                      "${RL_VOL_RELIGHT_DIR}/render/lights/shaders/rl_lights.slang"
                      "${RL_VOL_RENDERER_DIR}/shaders/light_tree/lt_common.glsl"
                      "${RL_VOL_RENDERER_DIR}/shaders/light_tree/lt_common.slang")
if(COMMAND fuse_add_slang_shaders)
    fuse_add_slang_shaders(fuse_relight_volumetrics_kernels
        SOURCES "${RL_VOL_SHADERS_DIR}/rl_vol.slang"
        STAGE compute
        INCLUDE_DIRS ${RL_VOL_INCLUDE_DIRS}
        FLAGS -fp-mode precise
        OUTPUT_DIR "${_rl_vol_spv}"
        OUTPUT_VAR _rl_vol_slang_out)
    list(LENGTH _rl_vol_slang_out _n)
    if(_n EQUAL 1)
        list(APPEND _rl_vol_args "-DSLANG_SPV=${_rl_vol_slang_out}")
        list(APPEND _rl_vol_deps "${_rl_vol_slang_out}")
    endif()
endif()
if(FUSE_GLSLANG_VALIDATOR)
    set(_out "${_rl_vol_spv}/rl_vol.glsl.spv")
    set(_incs "")
    foreach(_d IN LISTS RL_VOL_INCLUDE_DIRS)
        list(APPEND _incs "-I${_d}")
    endforeach()
    set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 ${_incs} "${RL_VOL_SHADERS_DIR}/rl_vol.comp"
                      -o "${_out}")
    if(FUSE_SPIRV_VAL)
        list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_out}")
    endif()
    add_custom_command(OUTPUT "${_out}" ${_cmds}
        DEPENDS "${RL_VOL_SHADERS_DIR}/rl_vol.comp" ${_rl_vol_core_deps}
        COMMENT "glslangValidator rl_vol.comp (GLSL twin of the Relight volumetrics stages)"
        VERBATIM)
    list(APPEND _rl_vol_args "-DGLSL_SPV=${_out}")
    list(APPEND _rl_vol_deps "${_out}")
endif()
add_custom_command(OUTPUT "${_rl_vol_header}"
    COMMAND "${CMAKE_COMMAND}" "-DOUT=${_rl_vol_header}" -DNAME=Vol -DMACRO=VOL ${_rl_vol_args}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
    DEPENDS ${_rl_vol_deps} "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
    COMMENT "Embedding the Relight volumetrics SPIR-V"
    VERBATIM)
add_custom_target(fuse_relight_volumetrics_header DEPENDS "${_rl_vol_header}")
add_dependencies(fuse_relight_volumetrics_header fuse_relight_volumetrics_kernels)
add_dependencies(fuse_relight_volumetrics fuse_relight_volumetrics_header)
target_include_directories(fuse_relight_volumetrics PRIVATE "${_rl_vol_gen}")

add_subdirectory(tests)
