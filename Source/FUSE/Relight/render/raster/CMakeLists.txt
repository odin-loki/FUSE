# FUSE Relight RL-4.2: raster remaster, tiers T0-T2 (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.8, §5.9, Wave R4). Added by
# one line in Source/FUSE/Relight/CMakeLists.txt, add_subdirectory(render/raster), right after RL-4.1's render/frame
# (it implements that package's IFrameRenderer; render/frame links this library back when it exists, like the RL-3.4
# / RL-3.5 hooks of the capture tap).
#
# Owns Relight/render/raster/** and Relight/shaders/raster/**.
#
# Targets
#   fuse_relight_render_raster      RasterRenderer (relight.frame.mode = raster): the CPU frame (raster_scene.hpp:
#                                   legacy geometry / materials / lights / fog / tiers), the GPU passes on the renderer
#                                   RL-4.1 adopted from DXVK's device (raster_renderer.hpp), the options
#                                   (raster_options.hpp). Linked into d3d9.dll through fuse_relight_render_frame.
#   fuse_relight_raster_shaders     glslangValidator (host) -> SPIR-V (spirv-val'd) -> raster_spv.h (embedded).
# Tests: tests/ (rl_raster_*).

if(NOT TARGET fuse_relight_render_frame)
    message(STATUS "FUSE Relight: RL-4.2 render/raster skipped (no RL-4.1 render/frame)")
    return()
endif()
foreach(_dep fuse_relight_bsdf fuse_relight_replace)
    if(NOT TARGET ${_dep})
        message(STATUS "FUSE Relight: RL-4.2 render/raster skipped (no ${_dep})")
        return()
    endif()
endforeach()
if(NOT FUSE_GLSLANG_VALIDATOR)
    message(STATUS "FUSE Relight: RL-4.2 render/raster skipped (glslangValidator not found: the shaders are GLSL)")
    return()
endif()

get_filename_component(_rl_raster_relight "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(_rl_raster_renderer "${_rl_raster_relight}/../Renderer" ABSOLUTE)
set(_rl_raster_shd "${_rl_raster_relight}/shaders/raster")
set(_rl_raster_gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(_rl_raster_spv "${CMAKE_CURRENT_BINARY_DIR}/spv")
file(MAKE_DIRECTORY "${_rl_raster_gen}" "${_rl_raster_spv}")

# Shaders: KEY | source | defines. The embed step (cmake/relight_raster_embed.cmake) names the arrays
# kRaster<Key>Spv with Key = the KEY in CamelCase (see raster_renderer.cpp).
set(_rl_raster_shaders
    "SurfaceVs|raster_surface.vert|"
    "GbufferFs|raster_surface.frag|-DRASTER_GBUFFER"
    "DecalFs|raster_surface.frag|-DRASTER_DECAL"
    "ForwardFs|raster_surface.frag|-DRASTER_FORWARD"
    "ShadowFs|raster_surface.frag|-DRASTER_SHADOW"
    "LightVs|raster_light.vert|"
    "LightFs|raster_light.frag|")
file(GLOB _rl_raster_includes "${_rl_raster_shd}/*.glsl")
list(APPEND _rl_raster_includes
    "${_rl_raster_renderer}/shaders/common/bindless.glsl"
    "${_rl_raster_renderer}/include/fuse/renderer/gpu_scene/gpu_scene.glsl"
    "${_rl_raster_relight}/kernels/bsdf_core.h"
    "${_rl_raster_relight}/shaders/material/bsdf.glsl")
set(_rl_raster_embed_args "")
set(_rl_raster_outputs "")
foreach(_row IN LISTS _rl_raster_shaders)
    string(REPLACE "|" ";" _entry "${_row}")
    list(GET _entry 0 _key)
    list(GET _entry 1 _src)
    list(LENGTH _entry _n)
    set(_defs "")
    if(_n GREATER 2)
        list(GET _entry 2 _defs)
    endif()
    set(_out "${_rl_raster_spv}/${_key}.spv")
    set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 ${_defs}
                      "-I${_rl_raster_shd}" "-I${_rl_raster_renderer}/shaders/common"
                      "-I${_rl_raster_renderer}/include/fuse/renderer/gpu_scene" "-I${_rl_raster_relight}/kernels"
                      "-I${_rl_raster_relight}/shaders/material" "${_rl_raster_shd}/${_src}" -o "${_out}")
    if(FUSE_SPIRV_VAL)
        list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_out}")
    endif()
    add_custom_command(OUTPUT "${_out}" ${_cmds}
        DEPENDS "${_rl_raster_shd}/${_src}" ${_rl_raster_includes}
        COMMENT "glslangValidator ${_src} ${_defs} (RL-4.2 raster ${_key})"
        VERBATIM)
    list(APPEND _rl_raster_embed_args "-D${_key}=${_out}")
    list(APPEND _rl_raster_outputs "${_out}")
endforeach()
set(_rl_raster_header "${_rl_raster_gen}/raster_spv.h")
set(_rl_raster_embed "${_rl_raster_relight}/cmake/relight_raster_embed.cmake")
string(REPLACE ";" "," _rl_raster_keys "SurfaceVs;GbufferFs;DecalFs;ForwardFs;ShadowFs;LightVs;LightFs")
add_custom_command(OUTPUT "${_rl_raster_header}"
    COMMAND "${CMAKE_COMMAND}" "-DOUT=${_rl_raster_header}" "-DKEYS=${_rl_raster_keys}" ${_rl_raster_embed_args}
            -P "${_rl_raster_embed}"
    DEPENDS ${_rl_raster_outputs} "${_rl_raster_embed}"
    COMMENT "Embedding the RL-4.2 raster shaders' SPIR-V"
    VERBATIM)
add_custom_target(fuse_relight_raster_shaders DEPENDS "${_rl_raster_header}")

add_library(fuse_relight_render_raster STATIC
    src/raster_options.cpp
    src/raster_renderer.cpp
    src/raster_scene.cpp)
target_include_directories(fuse_relight_render_raster PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                           PRIVATE "${_rl_raster_gen}")
target_link_libraries(fuse_relight_render_raster
    PUBLIC fuse_relight_render_frame fuse_relight_options
    PRIVATE fuse_relight_bsdf fuse_relight_replace fuse_rhi)
add_dependencies(fuse_relight_render_raster fuse_relight_raster_shaders)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_render_raster)
else()
    set_target_properties(fuse_relight_render_raster PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON
                                                                CXX_EXTENSIONS OFF)
endif()

add_subdirectory(tests)
