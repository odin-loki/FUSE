# FUSE Relight RL-4.3: the Relight material and BSDF model (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.2, §5.8, Wave R4).
# Added by one line in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(render/material), after RL-3.2
# (mods/import). Owns Relight/render/material/**, Relight/shaders/material/** and Relight/kernels/bsdf_*.
#
# Targets
#   fuse_relight_bsdf          the BSDF: the single-source core (kernels/bsdf_core.h, C++ dialect bsdf_cpp.hpp), the
#                              compute_kernel bodies (kernels/bsdf_kernels.hpp: bsdf_eval, bsdf_pdf, bsdf_sample,
#                              bsdf_albedo_lut, bsdf_albedo_avg), the albedo-table bake and the CPU launches. Pure CPU,
#                              depends on fuse_core only (native and MinGW).
#   fuse_relight_material      RL-3.2 MaterialParams -> BsdfMaterial (links fuse_relight_mods_import).
#   fuse_relight_bsdf_shaders  compile-only check of the shader twins when the compilers exist: the Slang
#                              implementation (shaders/material/bsdf.slang, via fuse_add_slang_shaders) and the GLSL
#                              fallback (bsdf.glsl, glslangValidator), both through the probe kernel bsdf_probe.*,
#                              spirv-val'd. The SPIR-V feeds the Lavapipe parity gate.
# Tests: tests/ (rl_bsdf_*).

foreach(_dep fuse_core fuse_relight_mods_import)
    if(NOT TARGET ${_dep})
        message(FATAL_ERROR "render/material needs ${_dep} (add_subdirectory(render/material) after RL-3.2 mods/import)")
    endif()
endforeach()

macro(_rl_bsdf_cxx23 _t)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endmacro()

get_filename_component(RL_BSDF_KERNELS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../kernels" ABSOLUTE)
get_filename_component(RL_BSDF_SHADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/material" ABSOLUTE)

add_library(fuse_relight_bsdf STATIC
    src/bsdf_host.cpp
    "${RL_BSDF_KERNELS_DIR}/bsdf_core.h"
    "${RL_BSDF_KERNELS_DIR}/bsdf_cpp.hpp"
    "${RL_BSDF_KERNELS_DIR}/bsdf_kernels.hpp")
target_include_directories(fuse_relight_bsdf PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${RL_BSDF_KERNELS_DIR}")
target_link_libraries(fuse_relight_bsdf PUBLIC fuse_core)
_rl_bsdf_cxx23(fuse_relight_bsdf)

add_library(fuse_relight_material STATIC src/material_bsdf.cpp)
target_link_libraries(fuse_relight_material PUBLIC fuse_relight_bsdf fuse_relight_mods_import)
_rl_bsdf_cxx23(fuse_relight_material)

# --- shader twins (compile + spirv-val; the parity gate loads the probe SPIR-V) ----------------------------------
add_custom_target(fuse_relight_bsdf_shaders ALL)
set(RL_BSDF_SLANG_SPV "")
set(RL_BSDF_GLSL_SPV "")
set(_rl_bsdf_spv_dir "${CMAKE_CURRENT_BINARY_DIR}/spv")
file(MAKE_DIRECTORY "${_rl_bsdf_spv_dir}")
set(_rl_bsdf_shader_deps
    "${RL_BSDF_KERNELS_DIR}/bsdf_core.h" "${RL_BSDF_SHADERS_DIR}/bsdf.slang" "${RL_BSDF_SHADERS_DIR}/bsdf.glsl")
if(COMMAND fuse_add_slang_shaders)
    fuse_add_slang_shaders(fuse_relight_bsdf_shaders
        SOURCES "${RL_BSDF_SHADERS_DIR}/bsdf_probe.slang"
        STAGE compute
        INCLUDE_DIRS "${RL_BSDF_KERNELS_DIR}" "${RL_BSDF_SHADERS_DIR}"
        FLAGS -fp-mode precise
        OUTPUT_DIR "${_rl_bsdf_spv_dir}"
        OUTPUT_VAR RL_BSDF_SLANG_SPV)
endif()
if(FUSE_GLSLANG_VALIDATOR)
    set(RL_BSDF_GLSL_SPV "${_rl_bsdf_spv_dir}/bsdf_probe.glsl.spv")
    set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 "-I${RL_BSDF_KERNELS_DIR}" "-I${RL_BSDF_SHADERS_DIR}"
                      "${RL_BSDF_SHADERS_DIR}/bsdf_probe.comp" -o "${RL_BSDF_GLSL_SPV}")
    if(FUSE_SPIRV_VAL)
        list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${RL_BSDF_GLSL_SPV}")
    endif()
    add_custom_command(OUTPUT "${RL_BSDF_GLSL_SPV}" ${_cmds}
        DEPENDS "${RL_BSDF_SHADERS_DIR}/bsdf_probe.comp" ${_rl_bsdf_shader_deps}
        COMMENT "glslangValidator bsdf_probe.comp (GLSL fallback of the Relight BSDF)"
        VERBATIM)
    add_custom_target(fuse_relight_bsdf_glsl DEPENDS "${RL_BSDF_GLSL_SPV}")
    add_dependencies(fuse_relight_bsdf_shaders fuse_relight_bsdf_glsl)
endif()

add_subdirectory(tests)
