# FUSE Relight RL-4.4 tests: the Relight light model + light-tree integration. Added from ../CMakeLists.txt.
#
#   rl_lights_<suite>              (gate;relight) CPU (native; PE through the Wine emulator in the MinGW tree), see
#                                  test_lights_cpu.cpp: convert (CPU kernel == RL-1.5 convertLegacyLight bit for bit,
#                                  ff_lit expectations of the RL-1.5 capture reference), pdf (every light kind's pdf
#                                  integrates to 1 ± 1e-3; the light set's to 1 - P(delta)), sample (sample / pdf /
#                                  eval consistency, light and light-set estimators vs brute-force quadrature, tree
#                                  selection frequencies vs pmf), usd (UsdLux / Remix light mapping and shaping),
#                                  emissive (captured emissive triangles), set (table / tree assembly, refit vs build,
#                                  fallback light, rejection rules), parity (CpuReference == CpuParallel), zero_alloc.
#   rl_lights_vk_<lang>            (gate;vulkan;relight) native Vulkan trees: Lavapipe with validation + synchronization
#                                  validation, "relight.lights.convert" (GPU D3D conversion) == the CPU conversion,
#                                  "relight.lights.sample" == the CPU light-set sampler, steady-state frames with no heap
#                                  allocation. Skip (77) without a Vulkan backend / ICD / layer / kernel.
#   rl_pe.lights_vk                (relight;wine;renderer_pe) the same executable as PE under the Xvfb runner (MinGW
#   rl_pe.lights_vk_validation     tree), without / with the host's validation layer injected (--external-validation).

add_executable(fuse_relight_lights_tests test_lights_cpu.cpp)
target_link_libraries(fuse_relight_lights_tests PRIVATE fuse_relight_lights fuse_relight_scene_lights
                                                        fuse_relight_mods_import fuse_relight_options)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_lights_tests)
else()
    set_target_properties(fuse_relight_lights_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
foreach(_suite convert pdf sample usd emissive set parity zero_alloc)
    add_test(NAME rl_lights_${_suite} COMMAND fuse_relight_lights_tests ${_suite}
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(rl_lights_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 900)
endforeach()

if(NOT (FUSE_VULKAN_BACKEND OR (CMAKE_CROSSCOMPILING AND FUSE_RELIGHT_RENDERER)) OR NOT TARGET fuse_rhi)
    return()
endif()
add_executable(fuse_relight_lights_vk_tests test_lights_vk.cpp)
target_link_libraries(fuse_relight_lights_vk_tests PRIVATE fuse_relight_lights fuse_relight_scene_lights fuse_rhi)
if(COMMAND fuse_apply_cxx23)
    fuse_apply_cxx23(fuse_relight_lights_vk_tests)
else()
    set_target_properties(fuse_relight_lights_vk_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
if(NOT CMAKE_CROSSCOMPILING)
    set(_rl_lights_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
    set(_rl_lights_vk_tests "")
    foreach(_lang slang glsl)
        if(EXISTS "${_rl_lights_lock}")
            add_test(NAME rl_lights_vk_${_lang}
                     COMMAND "${_rl_lights_lock}" "$<TARGET_FILE:fuse_relight_lights_vk_tests>" --language ${_lang})
        else()
            add_test(NAME rl_lights_vk_${_lang} COMMAND fuse_relight_lights_vk_tests --language ${_lang})
        endif()
        list(APPEND _rl_lights_vk_tests rl_lights_vk_${_lang})
    endforeach()
    set_tests_properties(${_rl_lights_vk_tests} PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 900 LABELS "gate;vulkan;relight")
    return()
endif()
set(_rl_lights_runner "${FUSE_WINE_XVFB_RUN}")
if(NOT _rl_lights_runner)
    set(_rl_lights_runner "${CMAKE_SOURCE_DIR}/cmake/toolchains/fuse-wine-xvfb-run.sh")
endif()
set(_rl_lights_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
if(NOT _rl_lights_prefix)
    set(_rl_lights_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
add_test(NAME rl_pe.lights_vk
         COMMAND "${_rl_lights_runner}" "${_rl_lights_prefix}" "$<TARGET_FILE:fuse_relight_lights_vk_tests>"
                 --no-validation)
set_tests_properties(rl_pe.lights_vk PROPERTIES
    LABELS "relight;wine;renderer_pe" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe TIMEOUT 900)
# The host's Khronos validation layer + synchronization validation injected through the host loader (Wine's
# vulkan-1.dll enumerates no layer): a negative control proves the layer is live, then every message fails the test.
if(EXISTS "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" OR
   EXISTS "/usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json")
    add_test(NAME rl_pe.lights_vk_validation
             COMMAND "${_rl_lights_runner}" "${_rl_lights_prefix}" "$<TARGET_FILE:fuse_relight_lights_vk_tests>"
                     --external-validation)
    set_tests_properties(rl_pe.lights_vk_validation PROPERTIES
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation;VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
        LABELS "relight;wine;renderer_pe;vulkan;gate" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe
        TIMEOUT 900)
endif()
