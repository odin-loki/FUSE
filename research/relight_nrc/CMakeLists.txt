# FUSE Relight RL-5.4 research track: an online-trained neural radiance cache on the WP-9.1 neural runtime
# (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.4 "Neural cache (research, T3 HW)"). Reports only: nothing in the runtime
# links it. Added from Source/FUSE/Relight/render/pathtrace/radiance_cache/CMakeLists.txt when fuse_neural exists.
#
# Targets
#   fuse_relight_nrc        NeuralRadianceCache (neural_radiance_cache.hpp): CPU online training (NeuralTrainer, Adam)
#                           on the hash grid's training records, CPU queries through RadianceCacheCpu::setLookup.
# Tests (labels relight;research):
#   rl_nrc_<suite>          train, report (bias / variance vs the hash grid -> nrc_report.json), zero_alloc.
#   rl_nrc_vk_<lang>        (vulkan) native Vulkan trees: GPU inference (NeuralGpu) == CPU inference, 0 validation messages.
#   rl_pe.nrc_vk[_validation]  PE under the Xvfb runner (MinGW tree), without / with the host's validation layer.

get_filename_component(_rl_nrc_pt_tests "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/FUSE/Relight/render/pathtrace/tests"
                       ABSOLUTE)

add_library(fuse_relight_nrc STATIC src/neural_radiance_cache.cpp include/relight_nrc/neural_radiance_cache.hpp)
target_include_directories(fuse_relight_nrc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fuse_relight_nrc PUBLIC fuse_relight_radiance_cache fuse_neural)
_rl_rc_cxx23(fuse_relight_nrc)

add_executable(fuse_relight_nrc_tests tests/test_nrc.cpp)
target_include_directories(fuse_relight_nrc_tests PRIVATE "${_rl_nrc_pt_tests}")
target_link_libraries(fuse_relight_nrc_tests PRIVATE fuse_relight_nrc fuse_relight_options)
_rl_rc_cxx23(fuse_relight_nrc_tests)
foreach(_suite train report zero_alloc)
    add_test(NAME rl_nrc_${_suite} COMMAND fuse_relight_nrc_tests ${_suite} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(rl_nrc_${_suite} PROPERTIES LABELS "relight;research" TIMEOUT 1800)
endforeach()

if(NOT TARGET fuse_rhi OR NOT (FUSE_VULKAN_BACKEND OR (CMAKE_CROSSCOMPILING AND FUSE_RELIGHT_RENDERER)))
    return()
endif()
add_executable(fuse_relight_nrc_vk_tests tests/test_nrc_vk.cpp)
target_include_directories(fuse_relight_nrc_vk_tests PRIVATE "${_rl_nrc_pt_tests}")
target_link_libraries(fuse_relight_nrc_vk_tests PRIVATE fuse_relight_nrc fuse_relight_options fuse_rhi)
_rl_rc_cxx23(fuse_relight_nrc_vk_tests)
if(NOT CMAKE_CROSSCOMPILING)
    set(_rl_nrc_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
    foreach(_lang slang glsl)
        if(EXISTS "${_rl_nrc_lock}")
            add_test(NAME rl_nrc_vk_${_lang}
                     COMMAND "${_rl_nrc_lock}" "$<TARGET_FILE:fuse_relight_nrc_vk_tests>" --language ${_lang})
        else()
            add_test(NAME rl_nrc_vk_${_lang} COMMAND fuse_relight_nrc_vk_tests --language ${_lang})
        endif()
        set_tests_properties(rl_nrc_vk_${_lang} PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
            RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 1800 LABELS "vulkan;relight;research")
    endforeach()
    return()
endif()
set(_rl_nrc_runner "${FUSE_WINE_XVFB_RUN}")
if(NOT _rl_nrc_runner)
    set(_rl_nrc_runner "${CMAKE_SOURCE_DIR}/cmake/toolchains/fuse-wine-xvfb-run.sh")
endif()
set(_rl_nrc_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
if(NOT _rl_nrc_prefix)
    set(_rl_nrc_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
add_test(NAME rl_pe.nrc_vk
         COMMAND "${_rl_nrc_runner}" "${_rl_nrc_prefix}" "$<TARGET_FILE:fuse_relight_nrc_vk_tests>" --no-validation)
set_tests_properties(rl_pe.nrc_vk PROPERTIES
    LABELS "relight;wine;renderer_pe;research" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe TIMEOUT 1800)
if(EXISTS "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" OR
   EXISTS "/usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json")
    add_test(NAME rl_pe.nrc_vk_validation
             COMMAND "${_rl_nrc_runner}" "${_rl_nrc_prefix}" "$<TARGET_FILE:fuse_relight_nrc_vk_tests>"
                     --external-validation)
    set_tests_properties(rl_pe.nrc_vk_validation PROPERTIES
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation;VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
        LABELS "relight;wine;renderer_pe;vulkan;research" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe
        TIMEOUT 1800)
endif()
