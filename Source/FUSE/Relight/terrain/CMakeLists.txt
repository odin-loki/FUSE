# FUSE Relight RL-5.6: terrain baking (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.7, port table row 29). Added by one line
# in Source/FUSE/Relight/CMakeLists.txt: add_subdirectory(terrain). Owns Relight/terrain/**.
#
# Targets
#   fuse_relight_terrain       TerrainBaker (terrain_baker.hpp): terrain draws + decals rasterised top-down into one
#                              cached RGBA texture with the draws' blend states (CPU reference), content-hash cache,
#                              the path tracer's world-position lookup.
#   fuse_relight_terrain_gpu   TerrainBakerGpu (terrain_baker_gpu.hpp): the same bake as RG compute passes
#                              ("relight.terrain.*"; Slang shaders/rl_terrain_bake.slang, GLSL twin .comp, embedded);
#                              "stub backend" without Vulkan.
# Tests: tests/ (rl_terrain_*).
if(NOT TARGET fuse_core)
    return()
endif()
macro(_rl_terrain_cxx23 _t)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(${_t})
    else()
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endmacro()
add_library(fuse_relight_terrain STATIC src/terrain_baker.cpp)
target_include_directories(fuse_relight_terrain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fuse_relight_terrain PUBLIC fuse_core)
# The GPU twin reproduces the reference's float expressions: no contraction on either side.
if(NOT MSVC)
    target_compile_options(fuse_relight_terrain PRIVATE -ffp-contract=off)
endif()
_rl_terrain_cxx23(fuse_relight_terrain)

if(TARGET fuse_rhi)
    set(_rl_tb_shaders "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    set(_rl_tb_gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
    set(_rl_tb_spv "${CMAKE_CURRENT_BINARY_DIR}/spv")
    file(MAKE_DIRECTORY "${_rl_tb_gen}" "${_rl_tb_spv}")
    set(_rl_tb_header "${_rl_tb_gen}/rl_terrain_spv.h")
    add_custom_target(fuse_relight_terrain_kernels)
    set(_rl_tb_args "")
    set(_rl_tb_deps "")
    if(COMMAND fuse_add_slang_shaders)
        fuse_add_slang_shaders(fuse_relight_terrain_kernels
            SOURCES "${_rl_tb_shaders}/rl_terrain_bake.slang"
            STAGE compute
            FLAGS -fp-mode precise
            OUTPUT_DIR "${_rl_tb_spv}"
            OUTPUT_VAR _rl_tb_slang_out)
        list(LENGTH _rl_tb_slang_out _n)
        if(_n EQUAL 1)
            list(APPEND _rl_tb_args "-DSLANG_SPV=${_rl_tb_slang_out}")
            list(APPEND _rl_tb_deps "${_rl_tb_slang_out}")
        endif()
    endif()
    if(FUSE_GLSLANG_VALIDATOR)
        set(_out "${_rl_tb_spv}/rl_terrain_bake.glsl.spv")
        set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 "${_rl_tb_shaders}/rl_terrain_bake.comp"
                          -o "${_out}")
        if(FUSE_SPIRV_VAL)
            list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_out}")
        endif()
        add_custom_command(OUTPUT "${_out}" ${_cmds}
            DEPENDS "${_rl_tb_shaders}/rl_terrain_bake.comp"
            COMMENT "glslangValidator rl_terrain_bake.comp (GLSL twin of the terrain bake)"
            VERBATIM)
        list(APPEND _rl_tb_args "-DGLSL_SPV=${_out}")
        list(APPEND _rl_tb_deps "${_out}")
    endif()
    add_custom_command(OUTPUT "${_rl_tb_header}"
        COMMAND "${CMAKE_COMMAND}" "-DOUT=${_rl_tb_header}" -DNAME=Terrain -DMACRO=TERRAIN ${_rl_tb_args}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        DEPENDS ${_rl_tb_deps} "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        COMMENT "Embedding the Relight terrain bake SPIR-V"
        VERBATIM)
    add_custom_target(fuse_relight_terrain_header DEPENDS "${_rl_tb_header}")
    add_dependencies(fuse_relight_terrain_header fuse_relight_terrain_kernels)
    file(GLOB _rl_tb_shader_files CONFIGURE_DEPENDS "${_rl_tb_shaders}/*")
    add_library(fuse_relight_terrain_gpu STATIC src/terrain_baker_gpu.cpp ${_rl_tb_shader_files})
    set_source_files_properties(${_rl_tb_shader_files} PROPERTIES HEADER_FILE_ONLY TRUE)
    target_include_directories(fuse_relight_terrain_gpu PRIVATE "${_rl_tb_gen}")
    target_link_libraries(fuse_relight_terrain_gpu PUBLIC fuse_relight_terrain fuse_rhi)
    add_dependencies(fuse_relight_terrain_gpu fuse_relight_terrain_header)
    _rl_terrain_cxx23(fuse_relight_terrain_gpu)
endif()
add_subdirectory(tests)
