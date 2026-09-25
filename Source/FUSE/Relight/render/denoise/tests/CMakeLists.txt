# FUSE Relight RL-5.5 tests: the path tracer's denoiser. Added from ../CMakeLists.txt.
#
#   rl_denoise_<suite>          (gate;relight) CPU (native; PE through the Wine emulator in the MinGW tree), see
#                               test_denoise_cpu.cpp: options, select (plugin switch / mocks), quality (error reduction
#                               >= 4x and bias vs the converged reference, diffuse / specular / radiance, Cornell box),
#                               gradient (A-SVGF producer semantics), asvgf (light-change responsiveness), determinism,
#                               zero_alloc. The renderer-side gates (per-pass GPU == CPU parity, ghosting on the
#                               moving-camera mirror scene, disocclusion, firefly) are fuse_rp_rdn_* (rp_rl55.cmake).
#   rl_denoise_vk_<lang>        (gate;vulkan;relight) native Vulkan trees (Lavapipe, validation + sync validation), see
#   rl_pe.denoise_vk            test_denoise_vk.cpp; PE under the Xvfb runner in the MinGW tree without / with the
#   rl_pe.denoise_vk_validation host's validation layer injected.
#   rl_denoise_frame_ff_lit     (relight;wine;gate) relight.frame.mode = pathtrace + relight.post.enable +
#   rl_denoise_frame_determinism relight.denoise.enable on ff_lit under Wine (rl_denoise_frame_run.py): every frame
#   rl_denoise_frame_validation denoised (A-SVGF), finite, smoother than raw 1 spp; reruns bit-identical; the denoiser
#                               adds no validation / sync-validation message over the path tracer + post.

add_executable(fuse_relight_denoise_tests test_denoise_cpu.cpp)
target_link_libraries(fuse_relight_denoise_tests PRIVATE fuse_relight_denoise fuse_relight_pathtrace fuse_relight_options)
target_include_directories(fuse_relight_denoise_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../pathtrace/tests")
_rl_pt_cxx23(fuse_relight_denoise_tests)
foreach(_suite options select quality gradient asvgf determinism zero_alloc)
    add_test(NAME rl_denoise_${_suite} COMMAND fuse_relight_denoise_tests ${_suite}
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(rl_denoise_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 1800)
endforeach()

# relight.frame.mode = pathtrace + relight.post.enable + relight.denoise.enable on ff_lit under Wine (MinGW tree).
find_package(Python3 COMPONENTS Interpreter QUIET)
# (render/post is added after this directory: its hook target does not exist yet; the path tracer frame target does)
if(Python3_Interpreter_FOUND AND TARGET relight_d3d9 AND TARGET rl_app_ff_lit AND TARGET fuse_relight_pathtrace_frame
   AND CMAKE_CROSSCOMPILING)
    get_filename_component(_rl_dn_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
    get_filename_component(_rl_dn_taptools "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/tap" ABSOLUTE)
    get_filename_component(_rl_dn_rastertools "${CMAKE_CURRENT_SOURCE_DIR}/../../raster/tests" ABSOLUTE)
    if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
        set(_rl_dn_frame_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
    else()
        set(_rl_dn_frame_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
    endif()
    set(_rl_dn_common "${CMAKE_CURRENT_SOURCE_DIR}/rl_denoise_frame_run.py" --tap-tools "${_rl_dn_taptools}"
        --raster-tools "${_rl_dn_rastertools}" --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
        --runner "${_rl_dn_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh" --prefix-root "${_rl_dn_frame_prefix}"
        --exe "$<TARGET_FILE:rl_app_ff_lit>" --app ff_lit)
    foreach(_cmd frame determinism validation)
        set(_name rl_denoise_frame_${_cmd})
        if(_cmd STREQUAL "frame")
            set(_name rl_denoise_frame_ff_lit)
        endif()
        add_test(NAME ${_name} COMMAND "${Python3_EXECUTABLE}" ${_rl_dn_common} ${_cmd}
                 --out "${CMAKE_BINARY_DIR}/relight/denoise/${_cmd}")
        set_tests_properties(${_name} PROPERTIES LABELS "relight;wine;gate" SKIP_RETURN_CODE 77 TIMEOUT 1800
                                                 RESOURCE_LOCK fuse_relight_renderer_pe)
    endforeach()
endif()

if(NOT (FUSE_VULKAN_BACKEND OR (CMAKE_CROSSCOMPILING AND FUSE_RELIGHT_RENDERER)) OR NOT TARGET fuse_rhi)
    return()
endif()
add_executable(fuse_relight_denoise_vk_tests test_denoise_vk.cpp)
target_link_libraries(fuse_relight_denoise_vk_tests PRIVATE fuse_relight_denoise fuse_relight_pathtrace
                      fuse_relight_options fuse_rhi)
target_include_directories(fuse_relight_denoise_vk_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../pathtrace/tests")
_rl_pt_cxx23(fuse_relight_denoise_vk_tests)
if(NOT CMAKE_CROSSCOMPILING)
    set(_rl_dn_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
    foreach(_lang slang glsl)
        if(EXISTS "${_rl_dn_lock}")
            add_test(NAME rl_denoise_vk_${_lang}
                     COMMAND "${_rl_dn_lock}" "$<TARGET_FILE:fuse_relight_denoise_vk_tests>" --language ${_lang})
        else()
            add_test(NAME rl_denoise_vk_${_lang} COMMAND fuse_relight_denoise_vk_tests --language ${_lang})
        endif()
        set_tests_properties(rl_denoise_vk_${_lang} PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
            RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 1800 LABELS "gate;vulkan;relight")
    endforeach()
else()
    set(_rl_dn_runner "${FUSE_WINE_XVFB_RUN}")
    if(NOT _rl_dn_runner)
        set(_rl_dn_runner "${CMAKE_SOURCE_DIR}/cmake/toolchains/fuse-wine-xvfb-run.sh")
    endif()
    set(_rl_dn_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
    if(NOT _rl_dn_prefix)
        set(_rl_dn_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
    endif()
    add_test(NAME rl_pe.denoise_vk
             COMMAND "${_rl_dn_runner}" "${_rl_dn_prefix}" "$<TARGET_FILE:fuse_relight_denoise_vk_tests>" --no-validation)
    set_tests_properties(rl_pe.denoise_vk PROPERTIES
        LABELS "relight;wine;renderer_pe" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe TIMEOUT 1800)
    if(EXISTS "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" OR
       EXISTS "/usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json")
        add_test(NAME rl_pe.denoise_vk_validation
                 COMMAND "${_rl_dn_runner}" "${_rl_dn_prefix}" "$<TARGET_FILE:fuse_relight_denoise_vk_tests>"
                         --external-validation)
        set_tests_properties(rl_pe.denoise_vk_validation PROPERTIES
            ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation;VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
            LABELS "relight;wine;renderer_pe;vulkan;gate" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe
            TIMEOUT 1800)
    endif()
endif()
