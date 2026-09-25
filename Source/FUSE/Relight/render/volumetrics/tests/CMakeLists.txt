# FUSE Relight RL-5.6 tests: volumetrics and the particle composite. Added from ../CMakeLists.txt.
#
#   rl_vol_<suite>          (gate;relight) CPU (native; PE through the Wine emulator in the MinGW tree), see
#                           test_vol_cpu.cpp: fog_d3d, convergence, reuse, ghosting, shadows, particles, determinism,
#                           zero_alloc, layout.
#   rl_vol_vk_<lang>        (gate;vulkan;relight) native Vulkan trees (Lavapipe, validation + sync validation), see
#                           test_vol_vk.cpp: GPU stages vs the CPU reference replayed on the GPU's inputs, after the
#                           RL-5.1 path tracer's trace pass; determinism; zero_alloc.
#   rl_pe.vol_vk            (relight;wine;renderer_pe) the same as PE under the Xvfb runner (MinGW tree), without / with
#   rl_pe.vol_vk_validation the host's validation layer injected (--external-validation, negative control).

add_executable(fuse_relight_vol_tests test_vol_cpu.cpp)
target_link_libraries(fuse_relight_vol_tests PRIVATE fuse_relight_volumetrics fuse_relight_options)
target_include_directories(fuse_relight_vol_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src"
                           "${CMAKE_CURRENT_SOURCE_DIR}/../shaders")
target_compile_definitions(fuse_relight_vol_tests PRIVATE FUSE_RL_VOL_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../shaders")
_rl_vol_cxx23(fuse_relight_vol_tests)
foreach(_suite fog_d3d convergence reuse ghosting shadows particles determinism zero_alloc layout)
    add_test(NAME rl_vol_${_suite} COMMAND fuse_relight_vol_tests ${_suite} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(rl_vol_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 900)
endforeach()

if(NOT (FUSE_VULKAN_BACKEND OR (CMAKE_CROSSCOMPILING AND FUSE_RELIGHT_RENDERER)) OR NOT TARGET fuse_rhi)
    return()
endif()
add_executable(fuse_relight_vol_vk_tests test_vol_vk.cpp)
# fuse_relight_omm: the RL-5.6 OMM device capability gate runs here (this test already holds a device).
target_link_libraries(fuse_relight_vol_vk_tests PRIVATE fuse_relight_volumetrics fuse_relight_omm fuse_relight_options
                      fuse_rhi)
target_include_directories(fuse_relight_vol_vk_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src"
                           "${CMAKE_CURRENT_SOURCE_DIR}/../shaders"
                           "${CMAKE_CURRENT_SOURCE_DIR}/../../pathtrace/tests") # pt_test_scenes.hpp (RL-5.1 Cornell)
_rl_vol_cxx23(fuse_relight_vol_vk_tests)
if(NOT CMAKE_CROSSCOMPILING)
    set(_rl_vol_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
    set(_rl_vol_vk_tests "")
    foreach(_lang slang glsl)
        if(EXISTS "${_rl_vol_lock}")
            add_test(NAME rl_vol_vk_${_lang}
                     COMMAND "${_rl_vol_lock}" "$<TARGET_FILE:fuse_relight_vol_vk_tests>" --language ${_lang})
        else()
            add_test(NAME rl_vol_vk_${_lang} COMMAND fuse_relight_vol_vk_tests --language ${_lang})
        endif()
        list(APPEND _rl_vol_vk_tests rl_vol_vk_${_lang})
    endforeach()
    set_tests_properties(${_rl_vol_vk_tests} PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 1800 LABELS "gate;vulkan;relight")
    return()
endif()
set(_rl_vol_runner "${FUSE_WINE_XVFB_RUN}")
if(NOT _rl_vol_runner)
    set(_rl_vol_runner "${CMAKE_SOURCE_DIR}/cmake/toolchains/fuse-wine-xvfb-run.sh")
endif()
set(_rl_vol_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
if(NOT _rl_vol_prefix)
    set(_rl_vol_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
add_test(NAME rl_pe.vol_vk
         COMMAND "${_rl_vol_runner}" "${_rl_vol_prefix}" "$<TARGET_FILE:fuse_relight_vol_vk_tests>" --no-validation)
set_tests_properties(rl_pe.vol_vk PROPERTIES
    LABELS "relight;wine;renderer_pe" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe TIMEOUT 1800)
if(EXISTS "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" OR
   EXISTS "/usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json")
    add_test(NAME rl_pe.vol_vk_validation
             COMMAND "${_rl_vol_runner}" "${_rl_vol_prefix}" "$<TARGET_FILE:fuse_relight_vol_vk_tests>"
                     --external-validation)
    set_tests_properties(rl_pe.vol_vk_validation PROPERTIES
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation;VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
        LABELS "relight;wine;renderer_pe;vulkan;gate" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe
        TIMEOUT 1800)
endif()
