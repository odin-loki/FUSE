# FUSE Relight RL-5.1 tests: path-tracing core, primary / PSR and the CPU reference path tracer. Added from
# ../CMakeLists.txt.
#
#   rl_pt_<suite>              (gate;relight) CPU (native; PE through the Wine emulator in the MinGW tree), see
#                              test_pt_cpu.cpp: furnace (white furnace: energy conservation of the RL-4.3 BSDFs under a
#                              uniform sky), lambert_sky (albedo x sky radiance), direct (closed forms: sphere, rect and
#                              emissive-triangle area lights, a delta distant light), mis (NEE-only / BSDF-only / MIS
#                              estimators agree), rr (Russian roulette unbiased), psr (mirror: G-buffer moved to the
#                              reflected surface, radiance unchanged, demodulation exact), alpha (legacy alpha test and
#                              blend), portal (a ray portal shows what its partner sees), capture (the RL-1.8 capture
#                              loader on a written capture), determinism (repeatable, CpuReference == CpuParallel),
#                              zero_alloc (steady-state scene update + reference frames).

add_executable(fuse_relight_pt_tests test_pt_cpu.cpp)
target_link_libraries(fuse_relight_pt_tests PRIVATE fuse_relight_pathtrace fuse_relight_options fuse_relight_mods_usd)
_rl_pt_cxx23(fuse_relight_pt_tests)
foreach(_suite furnace lambert_sky direct mis rr psr alpha portal capture determinism zero_alloc)
    add_test(NAME rl_pt_${_suite} COMMAND fuse_relight_pt_tests ${_suite} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(rl_pt_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 900)
endforeach()

# RL-5.2 ReSTIR DI (test_restir_di_cpu.cpp): rl_restir_di_<suite> (gate;relight): presample, unbiased (mean within 3
# sigma of the RL-5.1 reference), variance (below plain light-tree NEE and RIS-only), fast, determinism, zero_alloc,
# options, layout (push blocks vs C++).
if(TARGET fuse_relight_restir_di)
    add_executable(fuse_relight_restir_di_tests test_restir_di_cpu.cpp)
    target_link_libraries(fuse_relight_restir_di_tests PRIVATE fuse_relight_restir_di fuse_relight_options)
    target_compile_definitions(fuse_relight_restir_di_tests PRIVATE
                               FUSE_RL_PT_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../shaders")
    _rl_pt_cxx23(fuse_relight_restir_di_tests)
    foreach(_suite presample unbiased variance fast determinism zero_alloc options layout)
        add_test(NAME rl_restir_di_${_suite} COMMAND fuse_relight_restir_di_tests ${_suite}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(rl_restir_di_${_suite} PROPERTIES LABELS "gate;relight" TIMEOUT 1800)
    endforeach()
endif()

# Vulkan gates of PathTracerGpu (test_pt_vk.cpp): converge (GPU accumulation vs the CPU reference per pixel within
# 5 sigma on the Cornell scenes and the PSR mirror scene), psr (G-buffer), determinism, zero_alloc, validation.
#   rl_pt_vk_<lang>           (gate;vulkan;relight) native Vulkan trees (Lavapipe, validation + sync validation).
#   rl_pe.pt_vk               (relight;wine;renderer_pe) PE under the Xvfb runner (MinGW tree), without / with the
#   rl_pe.pt_vk_validation    host's validation layer injected (--external-validation, negative control).
# relight.frame.mode = pathtrace on a test app under Wine (MinGW tree; rl_pt_frame_run.py, the RL-4.2 raster driver's
# run machinery):
#   rl_pt_frame_ff_lit             (relight;wine;gate) ff_lit path traced at 4 and 64 spp: rendered every frame,
#                                  transform-only updates, within 4 sigma of the in-process CPU reference of the same
#                                  captured scene, RMSE falling with the sample count.
#   rl_pt_frame_determinism        (relight;wine) two runs bit-identical (dumps and frame records).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND TARGET relight_d3d9 AND TARGET rl_app_ff_lit AND TARGET fuse_relight_pathtrace_frame
   AND CMAKE_CROSSCOMPILING)
    get_filename_component(_rl_pt_repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
    get_filename_component(_rl_pt_taptools "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/tap" ABSOLUTE)
    get_filename_component(_rl_pt_rastertools "${CMAKE_CURRENT_SOURCE_DIR}/../../raster/tests" ABSOLUTE)
    if(DEFINED FUSE_WINE_XVFB_PREFIX_ROOT)
        set(_rl_pt_frame_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
    else()
        set(_rl_pt_frame_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
    endif()
    set(_rl_pt_frame_common "${CMAKE_CURRENT_SOURCE_DIR}/rl_pt_frame_run.py" --tap-tools "${_rl_pt_taptools}"
        --raster-tools "${_rl_pt_rastertools}" --d3d9 "$<TARGET_FILE:relight_d3d9>" --d3d8 "$<TARGET_FILE:relight_d3d8>"
        --runner "${_rl_pt_repo}/cmake/toolchains/fuse-wine-xvfb-run.sh" --prefix-root "${_rl_pt_frame_prefix}"
        --exe "$<TARGET_FILE:rl_app_ff_lit>" --app ff_lit)
    add_test(NAME rl_pt_frame_ff_lit COMMAND "${Python3_EXECUTABLE}" ${_rl_pt_frame_common}
             frame --out "${CMAKE_BINARY_DIR}/relight/pathtrace/frame")
    add_test(NAME rl_pt_frame_determinism COMMAND "${Python3_EXECUTABLE}" ${_rl_pt_frame_common}
             determinism --out "${CMAKE_BINARY_DIR}/relight/pathtrace/determinism")
    set_tests_properties(rl_pt_frame_ff_lit PROPERTIES LABELS "relight;wine;gate" SKIP_RETURN_CODE 77 TIMEOUT 900
                                                       RESOURCE_LOCK fuse_relight_renderer_pe)
    set_tests_properties(rl_pt_frame_determinism PROPERTIES LABELS "relight;wine" SKIP_RETURN_CODE 77 TIMEOUT 900
                                                            RESOURCE_LOCK fuse_relight_renderer_pe)
    # RL-5.2: the same app with rtx.useRTXDI = True (restir_di_frame_run.py wraps rl_pt_frame_run.py): ReSTIR DI
    # passes in the frame renderer, frames within 4 sigma of the CPU reference, reruns bit-identical.
    if(TARGET fuse_relight_restir_di)
        set(_rl_rdi_frame_common "${CMAKE_CURRENT_SOURCE_DIR}/restir_di_frame_run.py"
            ${_rl_pt_frame_common})
        list(REMOVE_AT _rl_rdi_frame_common 1) # rl_pt_frame_run.py (the wrapper imports it)
        add_test(NAME rl_restir_di_frame_ff_lit COMMAND "${Python3_EXECUTABLE}" ${_rl_rdi_frame_common}
                 frame --out "${CMAKE_BINARY_DIR}/relight/restir_di/frame")
        add_test(NAME rl_restir_di_frame_determinism COMMAND "${Python3_EXECUTABLE}" ${_rl_rdi_frame_common}
                 determinism --out "${CMAKE_BINARY_DIR}/relight/restir_di/determinism")
        set_tests_properties(rl_restir_di_frame_ff_lit PROPERTIES LABELS "relight;wine;gate" SKIP_RETURN_CODE 77
                             TIMEOUT 900 RESOURCE_LOCK fuse_relight_renderer_pe)
        set_tests_properties(rl_restir_di_frame_determinism PROPERTIES LABELS "relight;wine" SKIP_RETURN_CODE 77
                             TIMEOUT 900 RESOURCE_LOCK fuse_relight_renderer_pe)
    endif()
endif()

# RL-5.2 Vulkan gates of RestirDiGpu (test_restir_di_vk.cpp): parity (presample bit-exact, surfaces, each reuse pass
# replayed by the CPU oracle on the GPU's inputs), converge, determinism, zero_alloc, 0 validation messages.
#   rl_restir_di_vk_<lang>        (gate;vulkan;relight) native Vulkan trees (Lavapipe, validation + sync validation).
#   rl_pe.restir_di_vk            (relight;wine;renderer_pe) PE under the Xvfb runner (MinGW tree), without / with the
#   rl_pe.restir_di_vk_validation host's validation layer injected (--external-validation, negative control).
if(TARGET fuse_relight_restir_di AND TARGET fuse_rhi AND (FUSE_VULKAN_BACKEND OR (CMAKE_CROSSCOMPILING AND
                                                                                  FUSE_RELIGHT_RENDERER)))
    add_executable(fuse_relight_restir_di_vk_tests test_restir_di_vk.cpp)
    target_link_libraries(fuse_relight_restir_di_vk_tests PRIVATE fuse_relight_restir_di fuse_relight_options fuse_rhi)
    _rl_pt_cxx23(fuse_relight_restir_di_vk_tests)
    if(NOT CMAKE_CROSSCOMPILING)
        set(_rl_rdi_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
        foreach(_lang slang glsl)
            if(EXISTS "${_rl_rdi_lock}")
                add_test(NAME rl_restir_di_vk_${_lang}
                         COMMAND "${_rl_rdi_lock}" "$<TARGET_FILE:fuse_relight_restir_di_vk_tests>" --language ${_lang})
            else()
                add_test(NAME rl_restir_di_vk_${_lang} COMMAND fuse_relight_restir_di_vk_tests --language ${_lang})
            endif()
            set_tests_properties(rl_restir_di_vk_${_lang} PROPERTIES
                ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
                RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 1800 LABELS "gate;vulkan;relight")
        endforeach()
    else()
        set(_rl_rdi_runner "${FUSE_WINE_XVFB_RUN}")
        if(NOT _rl_rdi_runner)
            set(_rl_rdi_runner "${CMAKE_SOURCE_DIR}/cmake/toolchains/fuse-wine-xvfb-run.sh")
        endif()
        set(_rl_rdi_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
        if(NOT _rl_rdi_prefix)
            set(_rl_rdi_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
        endif()
        add_test(NAME rl_pe.restir_di_vk
                 COMMAND "${_rl_rdi_runner}" "${_rl_rdi_prefix}" "$<TARGET_FILE:fuse_relight_restir_di_vk_tests>"
                         --no-validation)
        set_tests_properties(rl_pe.restir_di_vk PROPERTIES
            LABELS "relight;wine;renderer_pe" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe TIMEOUT 1800)
        if(EXISTS "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" OR
           EXISTS "/usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json")
            add_test(NAME rl_pe.restir_di_vk_validation
                     COMMAND "${_rl_rdi_runner}" "${_rl_rdi_prefix}" "$<TARGET_FILE:fuse_relight_restir_di_vk_tests>"
                             --external-validation)
            set_tests_properties(rl_pe.restir_di_vk_validation PROPERTIES
                ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation;VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
                LABELS "relight;wine;renderer_pe;vulkan;gate" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe
                TIMEOUT 1800)
        endif()
    endif()
endif()

if(NOT (FUSE_VULKAN_BACKEND OR (CMAKE_CROSSCOMPILING AND FUSE_RELIGHT_RENDERER)) OR NOT TARGET fuse_rhi)
    return()
endif()
add_executable(fuse_relight_pt_vk_tests test_pt_vk.cpp)
target_link_libraries(fuse_relight_pt_vk_tests PRIVATE fuse_relight_pathtrace fuse_relight_options fuse_rhi)
_rl_pt_cxx23(fuse_relight_pt_vk_tests)
if(NOT CMAKE_CROSSCOMPILING)
    set(_rl_pt_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
    set(_rl_pt_vk_tests "")
    foreach(_lang slang glsl)
        if(EXISTS "${_rl_pt_lock}")
            add_test(NAME rl_pt_vk_${_lang}
                     COMMAND "${_rl_pt_lock}" "$<TARGET_FILE:fuse_relight_pt_vk_tests>" --language ${_lang})
        else()
            add_test(NAME rl_pt_vk_${_lang} COMMAND fuse_relight_pt_vk_tests --language ${_lang})
        endif()
        list(APPEND _rl_pt_vk_tests rl_pt_vk_${_lang})
    endforeach()
    set_tests_properties(${_rl_pt_vk_tests} PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 1800 LABELS "gate;vulkan;relight")
    return()
endif()
set(_rl_pt_runner "${FUSE_WINE_XVFB_RUN}")
if(NOT _rl_pt_runner)
    set(_rl_pt_runner "${CMAKE_SOURCE_DIR}/cmake/toolchains/fuse-wine-xvfb-run.sh")
endif()
set(_rl_pt_prefix "${FUSE_WINE_XVFB_PREFIX_ROOT}")
if(NOT _rl_pt_prefix)
    set(_rl_pt_prefix "${CMAKE_BINARY_DIR}/wineprefix-xvfb")
endif()
add_test(NAME rl_pe.pt_vk
         COMMAND "${_rl_pt_runner}" "${_rl_pt_prefix}" "$<TARGET_FILE:fuse_relight_pt_vk_tests>" --no-validation)
set_tests_properties(rl_pe.pt_vk PROPERTIES
    LABELS "relight;wine;renderer_pe" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe TIMEOUT 1800)
if(EXISTS "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" OR
   EXISTS "/usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json")
    add_test(NAME rl_pe.pt_vk_validation
             COMMAND "${_rl_pt_runner}" "${_rl_pt_prefix}" "$<TARGET_FILE:fuse_relight_pt_vk_tests>"
                     --external-validation)
    set_tests_properties(rl_pe.pt_vk_validation PROPERTIES
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation;VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
        LABELS "relight;wine;renderer_pe;vulkan;gate" SKIP_RETURN_CODE 77 RESOURCE_LOCK fuse_relight_renderer_pe
        TIMEOUT 1800)
endif()
