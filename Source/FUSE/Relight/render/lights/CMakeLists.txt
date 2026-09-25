# FUSE Relight RL-4.4: light model + light-tree integration (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.2, §5.3, §5.8,
# Wave R4). Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(render/lights), after RL-0.7's
# include(cmake/relight_renderer_mingw.cmake) (it needs the renderer's WP-7.1 light tree, fuse_light_tree, which
# RL-0.7 adds in the MinGW tree and the top level adds in native trees, stub backend included). Owns
# Relight/render/lights/** and Relight/kernels/light_*.
#
# Targets
#   fuse_relight_lights          the single-source light model (kernels/light_core.h, C++ dialect light_cpp.hpp, the
#                                compute_kernel bodies light_kernels.hpp: light_d3d_convert, light_set_sample), the
#                                per-frame RelightLightSet (game lights with the RL-1.5 conversion, UsdLux / Remix
#                                lights, emissive triangles, fallback light -> light table + WP-7.1 LightTree) and
#                                RelightLightsGpu (ring slots, "relight.lights.convert" = the D3D light conversion on
#                                the GPU, "relight.lights.sample", the tree through LightTreeGpu). The GPU half compiles
#                                to "unsupported" in the stub backend.
#   fuse_relight_lights_kernels  the SPIR-V: Slang (render/lights/shaders/rl_light_{convert,sample}.slang, -fp-mode
#                                precise) and the GLSL twins (.comp, glslangValidator), spirv-val'd and embedded into
#                                fuse_relight_lights (rl_lights_spv.h). Consumers include shaders/rl_lights.{slang,glsl}.
# Tests: tests/ (rl_lights_*).

foreach(_dep fuse_core fuse_light_tree fuse_relight_scene_lights fuse_relight_mods_import)
    if(NOT TARGET ${_dep})
        message(STATUS "FUSE Relight: RL-4.4 render/lights skipped (no ${_dep})")
        return()
    endif()
endforeach()

get_filename_component(RL_LIGHTS_KERNELS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../kernels" ABSOLUTE)
set(RL_LIGHTS_SHADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
get_filename_component(RL_LIGHTS_LT_SHADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../Renderer/shaders/light_tree" ABSOLUTE)

file(GLOB _rl_lights_shader_files CONFIGURE_DEPENDS "${RL_LIGHTS_SHADERS_DIR}/*")
add_library(fuse_relight_lights STATIC
    src/light_set.cpp
    src/light_set_gpu.cpp
    "${RL_LIGHTS_KERNELS_DIR}/light_core.h"
    "${RL_LIGHTS_KERNELS_DIR}/light_cpp.hpp"
    "${RL_LIGHTS_KERNELS_DIR}/light_kernels.hpp"
    ${_rl_lights_shader_files})
set_source_files_properties(${_rl_lights_shader_files} PROPERTIES HEADER_FILE_ONLY TRUE)
target_include_directories(fuse_relight_lights PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${RL_LIGHTS_KERNELS_DIR}")
target_link_libraries(fuse_relight_lights PUBLIC fuse_core fuse_light_tree fuse_relight_tap fuse_relight_hash
                      PRIVATE fuse_relight_scene_lights fuse_relight_mods_import fuse_relight_options)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_lights)
else()
    set_target_properties(fuse_relight_lights PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

# --- kernels (Slang primary, GLSL twin), embedded ------------------------------------------------------------------
set(_rl_lights_gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(_rl_lights_spv "${CMAKE_CURRENT_BINARY_DIR}/spv")
file(MAKE_DIRECTORY "${_rl_lights_gen}" "${_rl_lights_spv}")
set(_rl_lights_header "${_rl_lights_gen}/rl_lights_spv.h")
add_custom_target(fuse_relight_lights_kernels)
set(_rl_lights_embed_args "")
set(_rl_lights_embed_deps "")
set(_rl_lights_core_deps "${RL_LIGHTS_KERNELS_DIR}/light_core.h" "${RL_LIGHTS_LT_SHADERS_DIR}/lt_common.slang"
                         "${RL_LIGHTS_LT_SHADERS_DIR}/lt_common.glsl" "${RL_LIGHTS_SHADERS_DIR}/rl_lights.slang"
                         "${RL_LIGHTS_SHADERS_DIR}/rl_lights.glsl")
if(COMMAND fuse_add_slang_shaders)
    fuse_add_slang_shaders(fuse_relight_lights_kernels
        SOURCES "${RL_LIGHTS_SHADERS_DIR}/rl_light_convert.slang" "${RL_LIGHTS_SHADERS_DIR}/rl_light_sample.slang"
        STAGE compute
        INCLUDE_DIRS "${RL_LIGHTS_KERNELS_DIR}" "${RL_LIGHTS_SHADERS_DIR}" "${RL_LIGHTS_LT_SHADERS_DIR}"
        FLAGS -fp-mode precise
        OUTPUT_DIR "${_rl_lights_spv}"
        OUTPUT_VAR _rl_lights_slang_out)
    list(LENGTH _rl_lights_slang_out _n)
    if(_n EQUAL 2)
        list(GET _rl_lights_slang_out 0 _c)
        list(GET _rl_lights_slang_out 1 _s)
        list(APPEND _rl_lights_embed_args "-DSLANG_CONVERT=${_c}" "-DSLANG_SAMPLE=${_s}")
        list(APPEND _rl_lights_embed_deps "${_c}" "${_s}")
    endif()
endif()
if(FUSE_GLSLANG_VALIDATOR)
    foreach(_k convert sample)
        set(_out "${_rl_lights_spv}/rl_light_${_k}.glsl.spv")
        set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 "-I${RL_LIGHTS_KERNELS_DIR}"
                          "-I${RL_LIGHTS_SHADERS_DIR}" "-I${RL_LIGHTS_LT_SHADERS_DIR}"
                          "${RL_LIGHTS_SHADERS_DIR}/rl_light_${_k}.comp" -o "${_out}")
        if(FUSE_SPIRV_VAL)
            list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_out}")
        endif()
        add_custom_command(OUTPUT "${_out}" ${_cmds}
            DEPENDS "${RL_LIGHTS_SHADERS_DIR}/rl_light_${_k}.comp" ${_rl_lights_core_deps}
            COMMENT "glslangValidator rl_light_${_k}.comp (GLSL twin of the Relight light kernels)"
            VERBATIM)
        string(TOUPPER "${_k}" _K)
        list(APPEND _rl_lights_embed_args "-DGLSL_${_K}=${_out}")
        list(APPEND _rl_lights_embed_deps "${_out}")
    endforeach()
endif()
add_custom_command(OUTPUT "${_rl_lights_header}"
    COMMAND "${CMAKE_COMMAND}" "-DOUT=${_rl_lights_header}" ${_rl_lights_embed_args}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
    DEPENDS ${_rl_lights_embed_deps} "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
    COMMENT "Embedding the Relight light kernels' SPIR-V"
    VERBATIM)
add_custom_target(fuse_relight_lights_header DEPENDS "${_rl_lights_header}")
add_dependencies(fuse_relight_lights_header fuse_relight_lights_kernels)
add_dependencies(fuse_relight_lights fuse_relight_lights_header)
target_include_directories(fuse_relight_lights PRIVATE "${_rl_lights_gen}")

add_subdirectory(tests)
