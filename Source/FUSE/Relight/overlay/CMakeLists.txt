# FUSE Relight RL-6.1: in-game developer overlay (docs/plans/FUSE_REMIX_PORT_PLAN.md §1.11, Wave R6). Added by one line in
# Source/FUSE/Relight/CMakeLists.txt, add_subdirectory(overlay). No Dear ImGui is vendored in Engine/lib, so the overlay
# is FUSE's own immediate-mode UI (ui.hpp, 5x7 font) drawn by one small compute pass.
#
# Owns Relight/overlay/**. Touches (minimal hooks): render/frame (render_tap.{hpp,cpp}: IFrameOverlay, 5 call sites;
# frame_orchestrator.{hpp,cpp}: postComposite; frame_renderer.hpp: DebugBuffer / DebugImage / IFrameRenderer::debugImage;
# CMakeLists.txt: links fuse_relight_overlay when it exists).
#
# Targets
#   fuse_relight_overlay_core   Vulkan-free: options (relight.overlay.*), the UI toolkit and its CPU rasteriser (the CPU
#                               reference of the compose pass), the window-message rules and queue, the WndProc hook
#                               (Windows), scripted input, the developer menu and its state machine. Builds everywhere.
#   fuse_relight_overlay        the GPU compose pass (overlay_gpu.hpp; Slang primary + GLSL twin, embedded) and the tap
#                               side (overlay_tap.hpp: RenderTap's IFrameOverlay). Only with RL-4.1 render/frame; linked
#                               into d3d9.dll through fuse_relight_render_frame.
# Tests: tests/ (rl_overlay_*).

if(NOT TARGET fuse_relight_options)
    message(STATUS "FUSE Relight: RL-6.1 overlay skipped (no fuse_relight_options)")
    return()
endif()

add_library(fuse_relight_overlay_core STATIC
    src/dev_menu.cpp
    src/font5x7.cpp
    src/input.cpp
    src/overlay_options.cpp
    src/raster.cpp
    src/script.cpp
    src/ui.cpp
    src/win32_hook.cpp)
target_include_directories(fuse_relight_overlay_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fuse_relight_overlay_core PUBLIC fuse_relight_options)
if(WIN32)
    target_link_libraries(fuse_relight_overlay_core PRIVATE user32)
endif()
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_overlay_core)
else()
    set_target_properties(fuse_relight_overlay_core PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

set(RL_OVERLAY_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")

# ---- GPU + tap (needs RL-4.1's render/frame, i.e. the renderer on a Vulkan fuse_rhi) --------------------------------
if(TARGET fuse_relight_render_frame AND TARGET fuse_relight_tap_capture AND FUSE_GLSLANG_VALIDATOR)
    set(_rl_ov_gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
    set(_rl_ov_spv "${CMAKE_CURRENT_BINARY_DIR}/spv")
    file(MAKE_DIRECTORY "${_rl_ov_gen}" "${_rl_ov_spv}")
    set(_rl_ov_header "${_rl_ov_gen}/overlay_spv.h")
    set(_rl_ov_embed_args "")
    set(_rl_ov_embed_deps "")
    add_custom_target(fuse_relight_overlay_kernels)
    if(COMMAND fuse_add_slang_shaders)
        fuse_add_slang_shaders(fuse_relight_overlay_kernels
            SOURCES "${RL_OVERLAY_SHADER_DIR}/overlay_compose.slang"
            STAGE compute
            OUTPUT_DIR "${_rl_ov_spv}"
            OUTPUT_VAR _rl_ov_slang_out)
        list(LENGTH _rl_ov_slang_out _n)
        if(_n EQUAL 1)
            list(APPEND _rl_ov_embed_args "-DSLANG=${_rl_ov_slang_out}")
            list(APPEND _rl_ov_embed_deps "${_rl_ov_slang_out}")
        endif()
    endif()
    set(_out "${_rl_ov_spv}/overlay_compose.glsl.spv")
    set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" --target-env vulkan1.3 "${RL_OVERLAY_SHADER_DIR}/overlay_compose.comp"
                      -o "${_out}")
    if(FUSE_SPIRV_VAL)
        list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.3 "${_out}")
    endif()
    add_custom_command(OUTPUT "${_out}" ${_cmds}
        DEPENDS "${RL_OVERLAY_SHADER_DIR}/overlay_compose.comp"
        COMMENT "glslangValidator overlay_compose.comp (GLSL twin of the RL-6.1 overlay compose pass)"
        VERBATIM)
    list(APPEND _rl_ov_embed_args "-DGLSL=${_out}")
    list(APPEND _rl_ov_embed_deps "${_out}")
    add_custom_command(OUTPUT "${_rl_ov_header}"
        COMMAND "${CMAKE_COMMAND}" "-DOUT=${_rl_ov_header}" ${_rl_ov_embed_args}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/overlay_embed.cmake"
        DEPENDS ${_rl_ov_embed_deps} "${CMAKE_CURRENT_SOURCE_DIR}/overlay_embed.cmake"
        COMMENT "Embedding the RL-6.1 overlay compose pass's SPIR-V"
        VERBATIM)
    add_custom_target(fuse_relight_overlay_header DEPENDS "${_rl_ov_header}")
    add_dependencies(fuse_relight_overlay_header fuse_relight_overlay_kernels)

    add_library(fuse_relight_overlay STATIC src/overlay_gpu.cpp src/overlay_tap.cpp)
    target_include_directories(fuse_relight_overlay PRIVATE "${_rl_ov_gen}")
    target_link_libraries(fuse_relight_overlay
        PUBLIC fuse_relight_overlay_core fuse_relight_render_frame fuse_relight_tap_capture
        PRIVATE fuse_rhi)
    add_dependencies(fuse_relight_overlay fuse_relight_overlay_header)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(fuse_relight_overlay)
    else()
        set_target_properties(fuse_relight_overlay PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
else()
    message(STATUS "FUSE Relight: RL-6.1 overlay GPU / tap side skipped (needs RL-4.1 render/frame and glslangValidator)")
endif()

add_subdirectory(tests)
