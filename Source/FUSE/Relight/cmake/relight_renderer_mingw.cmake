# RL-0.7 (docs/plans/FUSE_REMIX_PORT_PLAN.md §7): the FUSE renderer (fuse_rhi and the Wave 0-3
# renderer libraries) built as Windows PE with MinGW-w64, loading Vulkan through volk (WP-0.2), and
# its gates run under Wine + Xvfb + Lavapipe (cmake/toolchains/fuse-wine-xvfb-run.sh).
# Owned by RL-0.7; included once from Source/FUSE/Relight/CMakeLists.txt.
#
#   FUSE_RELIGHT_RENDERER=ON   (the fuse-mingw-relight-* presets turn it on)
#
# How the renderer gets into a MinGW tree
#   * fuse_rhi already exists (FUSE_BUILD_VULKAN=ON): only the PE gates below are registered. On a
#     Linux-hosted cross build that path finds no loader, so fuse_rhi is the stub; use the default.
#   * FUSE_BUILD_VULKAN=OFF (the fuse-mingw-* default, and build/relight-mingw): Source/FUSE/Renderer
#     is added from here, EXCLUDE_FROM_ALL, with a headers-only Vulkan::Vulkan (the vendored Khronos
#     headers, cmake/FuseFindVulkan.cmake's Vulkan::Headers). fuse_volk_attach drops that target from
#     fuse_rhi's link interface, so nothing links a loader or an import library: volk opens
#     vulkan-1.dll at run time. The rest of the tree is unchanged (fuse_core keeps its null WSI, no
#     other directory sees FUSE_VULKAN_BACKEND).
#   * Shaders: every glslang / slangc / spirv-val step is a custom command on the *build host*
#     (find_program ignores the MinGW root; FuseSlang resolves the host slangc), so SPIR-V is compiled
#     on Linux and read by the PE tests from the build tree (Wine maps /home/... to Z:).
#
# Tests
#   The renderer's own ctest entries run their binaries through the Linux ICD-lock script, which
#   cannot start a PE, so in this tree they are DISABLED. The gates in the table below are
#   re-registered as `rl_pe.<gate>`: the same executable and arguments, run by the Xvfb runner
#   (Lavapipe through winevulkan, one Wine prefix slot per job). Their executables join the default
#   build through the rl_renderer_pe_gates target (the rest of the renderer stays out of `all`).
#     rl_pe.fuse_rp_device_tiers         WP-0.1 tier gate: Lavapipe reports T2 (PE path of the test:
#                                        tier caps + feature exercise; the mask layer and the second
#                                        ICD are Linux-only)
#     rl_pe.rp_golden_gbuffer_roundtrip  WP-0.7 golden round trip on the G-buffer raster path
#     rl_pe.<others>                     the remaining goldens, the WP-1.x..3.1 CPU suites and the
#                                        Vulkan gates that do not require the validation layer
#   Wine's builtin vulkan-1.dll loads no layers, so these run without VK_LAYER_KHRONOS_validation
#   unless FUSE_VK_LAYER_DIR provides a Windows build of it (see the runner).
#   Labels: relight;wine;renderer_pe.

if(NOT WIN32 OR NOT MINGW)
    return()
endif()

option(FUSE_RELIGHT_RENDERER "RL-0.7: build the FUSE renderer as PE (volk, host-compiled shaders) and run its gates under Wine" OFF)
if(NOT FUSE_RELIGHT_RENDERER)
    return()
endif()

set(_rl_rmw_renderer_src "${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer")

# Adds Source/FUSE/Renderer from here (function scope: FUSE_VULKAN_BACKEND stays local).
function(_rl_rmw_add_renderer)
    if(NOT TARGET Vulkan::Headers)
        include("${CMAKE_SOURCE_DIR}/cmake/FuseFindVulkan.cmake")
    endif()
    if(NOT TARGET Vulkan::Headers)
        message(FATAL_ERROR "FUSE_RELIGHT_RENDERER: no Vulkan headers (expected the vendored Khronos headers under "
                            "third_party/vendor/sdl/src/video/khronos)")
    endif()
    if(NOT TARGET Vulkan::Vulkan)
        # Headers only: volk (fuse_volk_attach) replaces the loader, so there is nothing to link.
        add_library(Vulkan::Vulkan INTERFACE IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES INTERFACE_LINK_LIBRARIES Vulkan::Headers)
    endif()
    set(FUSE_VULKAN_BACKEND TRUE)
    add_subdirectory("${_rl_rmw_renderer_src}" "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer" EXCLUDE_FROM_ALL)
    # A Windows Vulkan build gets VK_USE_PLATFORM_WIN32_KHR from fuse_core's Win32 WSI (PUBLIC); the
    # renderer's Win32 external-memory / semaphore code (vulkan_win32.h) relies on it. fuse_core has
    # no Vulkan WSI here, so fuse_rhi carries the define itself. vk_loader.cpp #undefs it before
    # volk's implementation, so volk still defines no platform entry points.
    target_compile_definitions(fuse_rhi PUBLIC VK_USE_PLATFORM_WIN32_KHR=1)
    # MinGW GCC 13 at -O3 reports -Wmaybe-uninitialized inside the vendored stb_image.h (included
    # SYSTEM, but the diagnostic lands on inlined code); third-party, so silence it for that test only.
    if(TARGET fuse_rp_harness_image_io)
        target_compile_options(fuse_rp_harness_image_io PRIVATE -Wno-maybe-uninitialized)
    endif()
    message(STATUS "FUSE Relight: renderer added as PE (RL-0.7; volk, headers-only Vulkan, host shaders)")
endfunction()

if(NOT TARGET fuse_rhi)
    if(NOT EXISTS "${_rl_rmw_renderer_src}/CMakeLists.txt")
        message(FATAL_ERROR "FUSE_RELIGHT_RENDERER: ${_rl_rmw_renderer_src} is missing")
    endif()
    _rl_rmw_add_renderer()
endif()

# Every test registered under a directory and its subdirectories, as "<dir>|<name>".
function(_rl_rmw_collect_tests dir out_var)
    get_property(_tests DIRECTORY "${dir}" PROPERTY TESTS)
    set(_all "")
    foreach(_t IN LISTS _tests)
        list(APPEND _all "${dir}|${_t}")
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_d IN LISTS _subdirs)
        _rl_rmw_collect_tests("${_d}" _sub)
        list(APPEND _all ${_sub})
    endforeach()
    set(${out_var} "${_all}" PARENT_SCOPE)
endfunction()

# _rl_rmw_pe_gate(<ctest name> <executable target> [args...])
#   Mirrors one renderer gate (same arguments as its Linux registration) as rl_pe.<ctest name>.
function(_rl_rmw_pe_gate name target)
    if(NOT TARGET ${target})
        message(STATUS "FUSE Relight: RL-0.7 gate ${name} skipped (no target ${target})")
        return()
    endif()
    set(_runner "${FUSE_WINE_XVFB_RUN}")
    if(NOT _runner)
        set(_runner "${CMAKE_SOURCE_DIR}/cmake/toolchains/fuse-wine-xvfb-run.sh")
    endif()
    set(_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
    if(NOT _prefix)
        set(_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
    endif()
    add_test(NAME "rl_pe.${name}" COMMAND "${_runner}" "${_prefix}" "$<TARGET_FILE:${target}>" ${ARGN})
    set_tests_properties("rl_pe.${name}" PROPERTIES
        LABELS "relight;wine;renderer_pe"
        SKIP_RETURN_CODE 77
        # Lavapipe is CPU-bound: one renderer gate at a time (the runner's slots would allow more).
        RESOURCE_LOCK fuse_relight_renderer_pe
        # Covers the one-off Wine prefix boot and Lavapipe under Wine.
        TIMEOUT 900)
    set_property(GLOBAL APPEND PROPERTY FUSE_RELIGHT_RENDERER_PE_TARGETS ${target})
endfunction()

# Runs at the end of the top-level configure (in its scope: only cache variables and globals are
# visible), after deferred renderer fragments added their tests.
function(_rl_rmw_register_pe_gates)
    _rl_rmw_collect_tests("${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer" _entries)
    foreach(_entry IN LISTS _entries)
        string(FIND "${_entry}" "|" _bar)
        string(SUBSTRING "${_entry}" 0 ${_bar} _dir)
        math(EXPR _bar "${_bar} + 1")
        string(SUBSTRING "${_entry}" ${_bar} -1 _name)
        set_tests_properties("${_name}" DIRECTORY "${_dir}" PROPERTIES DISABLED TRUE)
    endforeach()

    # Exit criterion (plan RL-0.7). Arguments as in rp_wp01.cmake / rp_harness.cmake, minus the
    # Linux-only --layer-dir.
    _rl_rmw_pe_gate(fuse_rp_device_tiers fuse_rp_device_tiers)
    _rl_rmw_pe_gate(rp_golden_gbuffer_roundtrip fuse_rp_harness_golden --roundtrip --scene gbuffer_quads,cornell_box)

    # Further Wave 0-3 gates that pass as PE (plan RL-0.7 "as many as practical"). Not mirrored:
    #   * Vulkan gates that require VK_LAYER_KHRONOS_validation (bindless, rg hazard/alias/radix,
    #     gpu_scene/culling/visbuffer/material_resolve/clustered/forward/vsm *_vk_*, ltc probes,
    #     pipeline cache, slang twin, gpu profiler): Wine's builtin vulkan-1.dll enumerates no
    #     layers, so they exit 77;
    #   * fuse_rp_volk_loader: winevulkan hands out the same thunks from vkGetDeviceProcAddr and
    #     vkGetInstanceProcAddr, so "device dispatch bypasses the trampolines" cannot hold;
    #   * fuse_rp_rg_async_spoof (Linux split-family layer), fuse_rp_slang_spirv_val (spawns the
    #     host spirv-val), fuse_rp_volk_no_loader_imports (ELF), the cmake-script lints (host);
    #   * WP-3.2 VSM raster gates while that package is in flight.
    _rl_rmw_pe_gate(rp_harness_image_io fuse_rp_harness_image_io
                    "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/rp_harness_image_io")
    foreach(_scene cornell_box sphere_field thin_wall hud_overlay instance_grid_1k instance_grid_100k)
        _rl_rmw_pe_gate(rp_golden_${_scene} fuse_rp_harness_golden --scene ${_scene})
    endforeach()
    foreach(_tier t0 t1 t2)
        _rl_rmw_pe_gate(rp_golden_cornell_box_${_tier} fuse_rp_harness_golden
                        --scene cornell_box --tier-cap ${_tier} --require-tier ${_tier})
    endforeach()
    _rl_rmw_pe_gate(fuse_rp_rg_compile fuse_rp_rg_compile)
    _rl_rmw_pe_gate(fuse_rp_renderdoc_capture fuse_rp_renderdoc_capture)
    _rl_rmw_pe_gate(fuse_rp_culling_vk_submit_flat_set fuse_rp_culling --mode submit_flat --backend set)
    # CPU suites (<executable>|<suite>...), as registered by rp_wp1x / rp_wp2x / rp_wp31.
    foreach(_spec
            "fuse_rp_meshlet_cook_tests|meshlet|codec;build;format;cook"
            "fuse_rp_gpu_scene_cpu|gpu_scene|table;delta_scaling;extract;churn;meshlets;zero_alloc"
            "fuse_rp_culling_cpu|culling|hiz;frustum;occlusion;phases;parity;api"
            "fuse_rp_visbuffer_cpu|visbuffer|format;indices;decode;reference;api"
            "fuse_rp_material_resolve_cpu|material_resolve|layout;bary;attributes;classify;api"
            "fuse_rp_clustered_gpu_cpu|clustered_gpu|layout;oracle;brdf;shade;api"
            "fuse_rp_brdf_cpu|brdf|dfg;sphere;furnace;reference"
            "fuse_rp_ltc_cpu|ltc|integral;encode;rect;disk;sun;furnace;shade"
            "fuse_rp_forward_cpu|forward|layout;collect;sort;blend;api"
            "fuse_rp_vsm_cpu|vsm|layout;clipmap;mark;invalidate;frame;api")
        string(REPLACE "|" ";" _parts "${_spec}")
        list(GET _parts 0 _exe)
        list(GET _parts 1 _stem)
        list(SUBLIST _parts 2 -1 _suites)
        foreach(_suite IN LISTS _suites)
            _rl_rmw_pe_gate(fuse_rp_${_stem}_${_suite} ${_exe} ${_suite})
        endforeach()
    endforeach()

    get_property(_targets GLOBAL PROPERTY FUSE_RELIGHT_RENDERER_PE_TARGETS)
    if(_targets)
        list(REMOVE_DUPLICATES _targets)
        add_custom_target(rl_renderer_pe_gates ALL DEPENDS ${_targets})
    endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _rl_rmw_register_pe_gates)
