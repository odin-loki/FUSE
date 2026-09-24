# FUSE Relight RL-4.1 tests. Added from ../CMakeLists.txt.
#
#   rl_frame_<suite>               (gate;relight) CPU: options, bindless (registration / retire / reclaim semantics),
#                                  scene (SceneModel / replaced scene -> GPU scene adapter), swap (eligibility, inert
#                                  without a host). Native; PE through the Wine emulator in the MinGW tree.
#   rl_frame_vk_composite          (gate;vulkan;relight) the composite logic on Lavapipe against a simulated DXVK host
#                                  (test_frame_vk.cpp): passthrough composite + texture swap bit-identical to the tap
#                                  off over resize / texture update / texture destroy, solid composite below the HUD,
#                                  bindless registration on the renderer's GPU heap, validation + synchronization
#                                  validation clean. Native Vulkan trees only (skip 77 without layer / ICD).
#   rl_pe.frame_vk_composite       (relight;wine;renderer_pe) the same executable as PE under the Xvfb runner, without
#                                  the validation layer (Wine's vulkan-1.dll loads none).
#   rl_frame_passthrough_<app>     (relight;golden;wine) in-process, d3d9.dll: capture tap + relight.frame.mode =
#                                  passthrough + relight.frame.textureSwap vs FUSE_RELIGHT=0: every dump bit-identical,
#                                  and the frame record shows an injection every frame (and the swaps).
#   rl_frame_solid_hud             (relight;wine) sky_ui_hud with relight.frame.mode = solid: injected at the first UI
#                                  draw; FUSE's colour where the scene was, the HUD pixels as without FUSE.
#   rl_frame_validation            (relight;wine;vulkan;gate) apps with the host validation layer + synchronization
#                                  validation: the frame modes add no validation message id over the capture tap alone.

add_executable(fuse_relight_frame_tests test_frame_cpu.cpp)
target_link_libraries(fuse_relight_frame_tests PRIVATE fuse_relight_render_frame_gpu)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_frame_tests)
endif()
foreach(_suite options bindless scene swap)
    add_test(NAME rl_frame_${_suite} COMMAND fuse_relight_frame_tests ${_suite} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(rl_frame_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 300)
endforeach()

if(FUSE_VULKAN_BACKEND OR (CMAKE_CROSSCOMPILING AND FUSE_RELIGHT_RENDERER))
    add_executable(fuse_relight_frame_vk_tests test_frame_vk.cpp)
    target_link_libraries(fuse_relight_frame_vk_tests PRIVATE fuse_relight_render_frame_gpu fuse_rhi)
    if(COMMAND fuse_apply_cxx23)
        fuse_apply_cxx23(fuse_relight_frame_vk_tests)
    endif()
    if(NOT CMAKE_CROSSCOMPILING)
        set(_rl_frame_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
        if(EXISTS "${_rl_frame_lock}")
            add_test(NAME rl_frame_vk_composite COMMAND "${_rl_frame_lock}" "$<TARGET_FILE:fuse_relight_frame_vk_tests>")
        else()
            add_test(NAME rl_frame_vk_composite COMMAND fuse_relight_frame_vk_tests)
        endif()
        set_tests_properties(rl_frame_vk_composite PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
            RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS "gate;vulkan;relight")
    else()
        set(_rl_frame_runner "${FUSE_WINE_XVFB_RUN}")
        if(NOT _rl_frame_runner)
            set(_rl_frame_runner "${CMAKE_SOURCE_DIR}/cmake/toolchains/fuse-wine-xvfb-run.sh")
        endif()
        set(_rl_frame_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
        if(NOT _rl_frame_prefix)
            set(_rl_frame_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
        endif()
        add_test(NAME rl_pe.frame_vk_composite
                 COMMAND "${_rl_frame_runner}" "${_rl_frame_prefix}" "$<TARGET_FILE:fuse_relight_frame_vk_tests>"
                         --no-validation)
        set_tests_properties(rl_pe.frame_vk_composite PROPERTIES
            LABELS "relight;wine;renderer_pe" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe TIMEOUT 900)
        # The same harness with the host's Khronos validation layer + synchronization validation injected through the
        # host loader (Wine's vulkan-1.dll enumerates no layer): a negative control proves the layer is live, then
        # every message fails the test. Skips (77) when the host has no layer.
        if(EXISTS "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" OR
           EXISTS "/usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json")
            add_test(NAME rl_pe.frame_vk_validation
                     COMMAND "${_rl_frame_runner}" "${_rl_frame_prefix}" "$<TARGET_FILE:fuse_relight_frame_vk_tests>"
                             --external-validation)
            set_tests_properties(rl_pe.frame_vk_validation PROPERTIES
                ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation;VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
                LABELS "relight;wine;renderer_pe;vulkan;gate" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe
                TIMEOUT 900)
        endif()
    endif()
endif()

# ---- in-process (d3d9.dll under Wine) --------------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT (Python3_Interpreter_FOUND AND TARGET relight_d3d9 AND TARGET rl_apps AND CMAKE_CROSSCOMPILING))
    return()
endif()
get_filename_component(_rl_frame_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
set(_rl_frame_runner "${_rl_frame_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh")
if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
    set(_rl_frame_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
else()
    set(_rl_frame_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
set(_rl_frame_out "${CMAKE_BINARY_DIR}/relight/frame")
set(_rl_frame_driver "${CMAKE_CURRENT_SOURCE_DIR}/rl_frame_run.py")
get_filename_component(_rl_frame_taptools "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/tap" ABSOLUTE)

set(_rl_frame_apps
    ff_triangle ff_textured ff_lit ff_alpha ff_fog ff_skinned ff_multi_instance sky_ui_hud stencil_shadow
    rt_target dynamic_buffers texture_formats shader_sm1 vs_sm2 vs_sm3
    d3d8_ff_triangle d3d8_ff_textured d3d8_ff_lit d3d8_ff_alpha d3d8_ff_fog d3d8_ff_skinned
    d3d8_ff_multi_instance d3d8_sky_ui_hud d3d8_stencil_shadow d3d8_rt_target d3d8_dynamic_buffers
    d3d8_texture_formats d3d8_shader_sm1)
foreach(_app IN LISTS _rl_frame_apps)
    if(NOT TARGET rl_app_${_app})
        continue()
    endif()
    add_test(NAME rl_frame_passthrough_${_app}
             COMMAND "${Python3_EXECUTABLE}" "${_rl_frame_driver}" --tap-tools "${_rl_frame_taptools}"
                     --exe "$<TARGET_FILE:rl_app_${_app}>" --app ${_app}
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_frame_runner}" --prefix-root "${_rl_frame_prefix}"
                     passthrough --out "${_rl_frame_out}/passthrough/${_app}")
    set_tests_properties(rl_frame_passthrough_${_app} PROPERTIES
        LABELS "relight;golden;wine" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()
foreach(_app sky_ui_hud d3d8_sky_ui_hud)
    if(TARGET rl_app_${_app})
        add_test(NAME rl_frame_solid_hud_${_app}
                 COMMAND "${Python3_EXECUTABLE}" "${_rl_frame_driver}" --tap-tools "${_rl_frame_taptools}"
                         --exe "$<TARGET_FILE:rl_app_${_app}>" --app ${_app}
                         --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                         --runner "${_rl_frame_runner}" --prefix-root "${_rl_frame_prefix}"
                         solid --out "${_rl_frame_out}/solid/${_app}")
        set_tests_properties(rl_frame_solid_hud_${_app} PROPERTIES LABELS "relight;wine" SKIP_RETURN_CODE 77 TIMEOUT 600)
    endif()
endforeach()
# RL-4.1: the renderer adopted inside d3d9.dll; no Vulkan loader from DllMain (loader_probe.c).
add_executable(rl_frame_loader_probe loader_probe.c)
set_target_properties(rl_frame_loader_probe PROPERTIES OUTPUT_NAME loader_probe)
if(TARGET rl_app_sky_ui_hud)
    add_test(NAME rl_frame_adopt
             COMMAND "${Python3_EXECUTABLE}" "${_rl_frame_driver}" --tap-tools "${_rl_frame_taptools}"
                     --exe "$<TARGET_FILE:rl_app_sky_ui_hud>" --app sky_ui_hud
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_frame_runner}" --prefix-root "${_rl_frame_prefix}"
                     adopt --out "${_rl_frame_out}/adopt" --probe "$<TARGET_FILE:rl_frame_loader_probe>"
                     --nm "${CMAKE_NM}")
    set_tests_properties(rl_frame_adopt PROPERTIES LABELS "relight;wine;gate" SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

set(_rl_frame_validation_exes "")
foreach(_app sky_ui_hud ff_textured texture_formats)
    if(TARGET rl_app_${_app})
        list(APPEND _rl_frame_validation_exes --exe "$<TARGET_FILE:rl_app_${_app}>")
    endif()
endforeach()
if(_rl_frame_validation_exes)
    add_test(NAME rl_frame_validation
             COMMAND "${Python3_EXECUTABLE}" "${_rl_frame_driver}" --tap-tools "${_rl_frame_taptools}"
                     ${_rl_frame_validation_exes} --app validation
                     --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
                     --runner "${_rl_frame_runner}" --prefix-root "${_rl_frame_prefix}"
                     validation --out "${_rl_frame_out}/validation")
    set_tests_properties(rl_frame_validation PROPERTIES
        LABELS "relight;wine;vulkan;gate" SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()
