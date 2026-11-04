# Custom shader effects for fuse_ui_vk (Core/include/fuse/ui/effects.hpp).
#
#   fuse_ui_add_effect(<target> NAME <identifier> SOURCE <snippet.glsl>)
#
# Splices the GLSL snippet (it defines `vec4 fx_main(ivec2 p)`) into Render/src/shaders/ui_effect.comp.in, compiles it
# with glslangValidator (Vulkan 1.1) and embeds the SPIR-V as `static const std::uint32_t <identifier>[]` in the
# generated header "<identifier>_spv.h" on <target>'s include path. Register it with
#   registry.add({.name = "...", .cpu = &cpuTwin, .spirv = <identifier>});   renderer.setEffects(registry);
# Without glslangValidator nothing is generated (test for the header with __has_include).
set(FUSE_UI_EFFECT_SHADER_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/shaders" CACHE INTERNAL "")
set(FUSE_UI_EFFECT_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")

function(fuse_ui_add_effect target)
    cmake_parse_arguments(FX "" "NAME;SOURCE" "" ${ARGN})
    if(NOT FX_NAME OR NOT FX_SOURCE)
        message(FATAL_ERROR "fuse_ui_add_effect: NAME and SOURCE are required")
    endif()
    if(NOT FUSE_UI_GLSLANG)
        find_program(FUSE_UI_GLSLANG NAMES glslangValidator HINTS ENV VULKAN_SDK PATH_SUFFIXES bin Bin)
    endif()
    if(NOT FUSE_UI_GLSLANG)
        message(STATUS "fuse_ui_add_effect(${FX_NAME}): no glslangValidator, effect not compiled")
        return()
    endif()
    get_filename_component(_src "${FX_SOURCE}" ABSOLUTE)
    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/fuse_ui_effects")
    file(MAKE_DIRECTORY "${_gen}")
    set(_tpl "${FUSE_UI_EFFECT_SHADER_DIR}/ui_effect.comp.in")
    set(_comp "${_gen}/${FX_NAME}.comp")
    set(_spv "${_gen}/${FX_NAME}.comp.spv")
    set(_hdr "${_gen}/${FX_NAME}_spv.h")
    add_custom_command(OUTPUT "${_comp}"
        COMMAND "${CMAKE_COMMAND}" -DTEMPLATE=${_tpl} -DSNIPPET=${_src} -DOUT=${_comp}
                -P "${FUSE_UI_EFFECT_CMAKE_DIR}/make_effect.cmake"
        DEPENDS "${_tpl}" "${_src}" "${FUSE_UI_EFFECT_CMAKE_DIR}/make_effect.cmake" VERBATIM)
    add_custom_command(OUTPUT "${_spv}"
        COMMAND "${FUSE_UI_GLSLANG}" --target-env vulkan1.1 -S comp "-I${FUSE_UI_EFFECT_SHADER_DIR}" "${_comp}" -o "${_spv}"
        DEPENDS "${_comp}" "${FUSE_UI_EFFECT_SHADER_DIR}/ui_math.glsl" COMMENT "glslangValidator effect ${FX_NAME}" VERBATIM)
    add_custom_command(OUTPUT "${_hdr}"
        COMMAND "${CMAKE_COMMAND}" -DSPV=${_spv} -DOUT=${_hdr} -DNAME=${FX_NAME}
                -P "${FUSE_UI_EFFECT_CMAKE_DIR}/embed_spv.cmake"
        DEPENDS "${_spv}" "${FUSE_UI_EFFECT_CMAKE_DIR}/embed_spv.cmake" VERBATIM)
    target_sources(${target} PRIVATE "${_hdr}")
    target_include_directories(${target} PRIVATE "${_gen}")
endfunction()

# A plain shader (vertex / fragment / compute) embedded the same way: fuse_ui_embed_shader(<target> NAME <id> SOURCE <file>)
function(fuse_ui_embed_shader target)
    cmake_parse_arguments(SH "" "NAME;SOURCE" "" ${ARGN})
    if(NOT FUSE_UI_GLSLANG)
        return()
    endif()
    get_filename_component(_src "${SH_SOURCE}" ABSOLUTE)
    get_filename_component(_dir "${_src}" DIRECTORY)
    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/fuse_ui_effects")
    file(MAKE_DIRECTORY "${_gen}")
    set(_spv "${_gen}/${SH_NAME}.spv")
    set(_hdr "${_gen}/${SH_NAME}_spv.h")
    add_custom_command(OUTPUT "${_spv}"
        COMMAND "${FUSE_UI_GLSLANG}" --target-env vulkan1.1 "-I${_dir}" "${_src}" -o "${_spv}"
        DEPENDS "${_src}" "${_dir}/ui_math.glsl" COMMENT "glslangValidator ${SH_NAME}" VERBATIM)
    add_custom_command(OUTPUT "${_hdr}"
        COMMAND "${CMAKE_COMMAND}" -DSPV=${_spv} -DOUT=${_hdr} -DNAME=${SH_NAME}
                -P "${FUSE_UI_EFFECT_CMAKE_DIR}/embed_spv.cmake"
        DEPENDS "${_spv}" "${FUSE_UI_EFFECT_CMAKE_DIR}/embed_spv.cmake" VERBATIM)
    target_sources(${target} PRIVATE "${_hdr}")
    target_include_directories(${target} PRIVATE "${_gen}")
endfunction()
