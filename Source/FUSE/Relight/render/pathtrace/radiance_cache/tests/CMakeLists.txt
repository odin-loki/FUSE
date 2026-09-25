# FUSE Relight RL-5.4 tests: the hash-grid radiance cache. Added from ../CMakeLists.txt.
#
#   rl_radiance_cache_<suite>  (gate;relight) CPU (native; PE through the Wine emulator in the MinGW tree), see
#                              test_radiance_cache_cpu.cpp: cells (cell ids: adaptive level by distance, bins,
#                              determinism), update (closed forms of update / resolve: mean, window, eviction, drops,
#                              order independence, CpuReference == CpuParallel per key), converge (cache-terminated
#                              paths within the documented bias bound of the RL-5.1 reference), variance (below plain
#                              path tracing at equal spp), hitrate (metrics), determinism, zero_alloc, options, layout.
#   rl_radiance_cache_vk_<lang> (gate;vulkan;relight) native Vulkan trees (Lavapipe, validation + sync validation), see
#                              test_radiance_cache_vk.cpp: GPU == CPU parity of update / resolve / query on read-back
#                              inputs, converge, determinism, zero_alloc, 0 validation messages.
#   rl_pe.radiance_cache_vk    (relight;wine;renderer_pe) PE under the Xvfb runner (MinGW tree), without / with the host's
#   rl_pe.radiance_cache_vk_validation  validation layer injected (--external-validation, negative control).

get_filename_component(_rl_rc_pt_tests "${CMAKE_CURRENT_SOURCE_DIR}/../../tests" ABSOLUTE)

add_executable(fuse_relight_radiance_cache_tests test_radiance_cache_cpu.cpp)
target_include_directories(fuse_relight_radiance_cache_tests PRIVATE "${_rl_rc_pt_tests}" "${RL_RC_KERNELS_DIR}")
target_link_libraries(fuse_relight_radiance_cache_tests PRIVATE fuse_relight_radiance_cache fuse_relight_options)
target_compile_definitions(fuse_relight_radiance_cache_tests PRIVATE
                           FUSE_RL_RC_SHADER_DIR="${RL_RC_SHADERS_DIR}" FUSE_RL_PT_SHADER_DIR="${RL_RC_PT_SHADERS_DIR}"
                           FUSE_RL_KERNELS_DIR="${RL_RC_KERNELS_DIR}")
_rl_rc_cxx23(fuse_relight_radiance_cache_tests)
foreach(_suite cells update converge variance hitrate determinism zero_alloc options layout)
    add_test(NAME rl_radiance_cache_${_suite} COMMAND fuse_relight_radiance_cache_tests ${_suite}
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(rl_radiance_cache_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 1800)
endforeach()

if(NOT TARGET fuse_rhi OR NOT (FUSE_VULKAN_BACKEND OR (CMAKE_CROSSCOMPILING AND FUSE_RELIGHT_RENDERER)))
    return()
endif()
add_executable(fuse_relight_radiance_cache_vk_tests test_radiance_cache_vk.cpp)
target_include_directories(fuse_relight_radiance_cache_vk_tests PRIVATE "${_rl_rc_pt_tests}" "${RL_RC_KERNELS_DIR}")
target_link_libraries(fuse_relight_radiance_cache_vk_tests PRIVATE fuse_relight_radiance_cache fuse_relight_options
                      fuse_rhi)
_rl_rc_cxx23(fuse_relight_radiance_cache_vk_tests)
if(NOT CMAKE_CROSSCOMPILING)
    set(_rl_rc_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
    foreach(_lang slang glsl)
        if(EXISTS "${_rl_rc_lock}")
            add_test(NAME rl_radiance_cache_vk_${_lang}
                     COMMAND "${_rl_rc_lock}" "$<TARGET_FILE:fuse_relight_radiance_cache_vk_tests>" --language ${_lang})
        else()
            add_test(NAME rl_radiance_cache_vk_${_lang}
                     COMMAND fuse_relight_radiance_cache_vk_tests --language ${_lang})
        endif()
        set_tests_properties(rl_radiance_cache_vk_${_lang} PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
            RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 1800 LABELS "gate;vulkan;relight")
    endforeach()
    return()
endif()
set(_rl_rc_runner "${FUSE_WINE_XVFB_RUN}")
if(NOT _rl_rc_runner)
    set(_rl_rc_runner "${CMAKE_SOURCE_DIR}/cmake/toolchains/fuse-wine-xvfb-run.sh")
endif()
set(_rl_rc_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
if(NOT _rl_rc_prefix)
    set(_rl_rc_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
add_test(NAME rl_pe.radiance_cache_vk
         COMMAND "${_rl_rc_runner}" "${_rl_rc_prefix}" "$<TARGET_FILE:fuse_relight_radiance_cache_vk_tests>"
                 --no-validation)
set_tests_properties(rl_pe.radiance_cache_vk PROPERTIES
    LABELS "relight;wine;renderer_pe" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe TIMEOUT 1800)
if(EXISTS "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" OR
   EXISTS "/usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json")
    add_test(NAME rl_pe.radiance_cache_vk_validation
             COMMAND "${_rl_rc_runner}" "${_rl_rc_prefix}" "$<TARGET_FILE:fuse_relight_radiance_cache_vk_tests>"
                     --external-validation)
    set_tests_properties(rl_pe.radiance_cache_vk_validation PROPERTIES
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation;VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
        LABELS "relight;wine;renderer_pe;vulkan;gate" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe
        TIMEOUT 1800)
endif()
