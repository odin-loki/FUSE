# Upscaler abstraction + spatial upscalers (fuse/renderer/upscale/, docs/upscalers.md): fuse_rhi sources,
# vendored third-party includes, build-time SPIR-V of the vendored GLSL passes and the gate tests.
# Owned by the upscaler work stream; other topics must not edit this file.
#
# Vendored (MIT, pinned in each VERSION file, checked by fuse_lint_vendored_pins_*):
#   Engine/lib/fidelityfx  — FidelityFX SDK v1.1.4 FSR1 (EASU + RCAS) + CAS headers and Vulkan GLSL passes
#   Engine/lib/nvidia-nis  — NVIDIA Image Scaling SDK v1.0.3 (NIS_Scaler.h, NIS_Config.h, NIS_Main.glsl)

set(FUSE_FIDELITYFX_DIR "${CMAKE_SOURCE_DIR}/Engine/lib/fidelityfx")
set(FUSE_NVIDIA_NIS_DIR "${CMAKE_SOURCE_DIR}/Engine/lib/nvidia-nis")
foreach(_fuse_upscale_pin "${FUSE_FIDELITYFX_DIR}/VERSION" "${FUSE_NVIDIA_NIS_DIR}/VERSION")
    if(NOT EXISTS "${_fuse_upscale_pin}")
        message(FATAL_ERROR "FUSE upscalers: vendored pin ${_fuse_upscale_pin} is missing")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fuse_upscale_pin}")
endforeach()

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/upscale/upscaler.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/upscale/upscale_passes.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/upscale/upscale_kernel_common.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/upscale/fsr1_kernel.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/upscale/cas_kernel.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/upscale/nis_kernel.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/upscale/upscale_passes.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/upscale/upscaler_registry.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/upscale/upscaler_backends.cpp
)
# NIS_Config.h (constants + coefficient banks) is included by upscale_passes.cpp only. SYSTEM: third-party
# header, keep FUSE warning levels off it.
target_include_directories(fuse_rhi SYSTEM PRIVATE "${FUSE_NVIDIA_NIS_DIR}/NIS")

# FSR 3.1 temporal backend (WP-4.2, cmake/rp_wp42.cmake: lib fuse_fsr3 over the vendored FidelityFX SDK v1.1.4,
# Engine/lib/fidelityfx). FUSE_UPSCALER_FSR3 controls whether fsr3::register_fsr3_backend() registers "fsr3"
# (compile definition FUSE_UPSCALER_FSR3=1 on fuse_fsr3); fuse_fsr3 and its CPU gates build either way. Default
# ON with the Vulkan backend; without it registration is impossible (no device) and the option has no effect.
if(FUSE_VULKAN_BACKEND)
    set(_fuse_upscaler_fsr3_default ON)
else()
    set(_fuse_upscaler_fsr3_default OFF)
endif()
# Trees configured before FSR 3 was vendored cached a forced OFF (ON was a FATAL_ERROR): drop that entry so the
# new default applies; a value set with the current description is a user choice and is kept.
get_property(_fuse_upscaler_fsr3_help CACHE FUSE_UPSCALER_FSR3 PROPERTY HELPSTRING)
if(_fuse_upscaler_fsr3_help MATCHES "not vendored yet")
    unset(FUSE_UPSCALER_FSR3 CACHE)
endif()
option(FUSE_UPSCALER_FSR3 "Register the FidelityFX FSR 3.1 (Vulkan) temporal upscaler backend \"fsr3\" (WP-4.2)"
       ${_fuse_upscaler_fsr3_default})
if(FUSE_UPSCALER_FSR3 AND NOT FUSE_VULKAN_BACKEND)
    message(STATUS "FUSE: FUSE_UPSCALER_FSR3=ON has no effect without the Vulkan backend (fsr3 not registered)")
endif()

# Vendored GLSL passes -> SPIR-V (glslangValidator -V, + spirv-val), same convention as gpu_radix_sort.cmake.
# These are the GPU-shader references the CPU ports are checked against (fuse_upscale_gpu_reference).
set(FUSE_UPSCALE_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders/upscale")
if(FUSE_GLSLANG_VALIDATOR)
    file(MAKE_DIRECTORY "${FUSE_UPSCALE_SPV_DIR}")
    set(_ffx_gpu "${FUSE_FIDELITYFX_DIR}/include/FidelityFX/gpu")
    set(_ffx_vk "${FUSE_FIDELITYFX_DIR}/shaders/vk")
    set(_fuse_upscale_spv_outputs "")
    # <output>|<source>|<defines (comma separated)>|<include dirs (comma separated)>
    foreach(_entry
            "fsr1_easu|${_ffx_vk}/fsr1/ffx_fsr1_easu_pass.glsl|-DFFX_GPU=1,-DFFX_GLSL=1,-DFFX_HALF=0,-DFFX_FSR1_OPTION_APPLY_RCAS=0,-DFFX_FSR1_OPTION_RCAS_PASSTHROUGH_ALPHA=0,-DFFX_FSR1_OPTION_SRGB_CONVERSIONS=0|${_ffx_gpu},${_ffx_gpu}/fsr1"
            "fsr1_rcas|${_ffx_vk}/fsr1/ffx_fsr1_rcas_pass.glsl|-DFFX_GPU=1,-DFFX_GLSL=1,-DFFX_HALF=0,-DFFX_FSR1_OPTION_APPLY_RCAS=1,-DFFX_FSR1_OPTION_RCAS_PASSTHROUGH_ALPHA=0,-DFFX_FSR1_OPTION_SRGB_CONVERSIONS=0|${_ffx_gpu},${_ffx_gpu}/fsr1"
            "cas_sharpen|${_ffx_vk}/cas/ffx_cas_sharpen_pass.glsl|-DFFX_GPU=1,-DFFX_GLSL=1,-DFFX_HALF=0,-DFFX_CAS_OPTION_SHARPEN_ONLY=1,-DFFX_CAS_COLOR_SPACE_CONVERSION=0|${_ffx_gpu},${_ffx_gpu}/cas"
            "nis_scaler|${FUSE_NVIDIA_NIS_DIR}/NIS/NIS_Main.glsl|-DNIS_SCALER=1,-DNIS_USE_HALF_PRECISION=0,-DNIS_HDR_MODE=0|${FUSE_NVIDIA_NIS_DIR}/NIS")
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _src)
        list(GET _parts 2 _defines)
        list(GET _parts 3 _includes)
        string(REPLACE "," ";" _defines "${_defines}")
        string(REPLACE "," ";" _includes "${_includes}")
        set(_inc_args "")
        foreach(_inc IN LISTS _includes)
            list(APPEND _inc_args "-I${_inc}")
        endforeach()
        set(_out "${FUSE_UPSCALE_SPV_DIR}/${_name}.comp.spv")
        set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" -V -S comp ${_defines} ${_inc_args} "${_src}" -o "${_out}")
        if(FUSE_SPIRV_VAL)
            list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" "${_out}")
        endif()
        file(GLOB _deps "${_ffx_gpu}/*.h" "${_ffx_gpu}/fsr1/*.h" "${_ffx_gpu}/cas/*.h" "${FUSE_NVIDIA_NIS_DIR}/NIS/*.h")
        add_custom_command(OUTPUT "${_out}"
            ${_cmds}
            DEPENDS "${_src}" ${_deps}
            COMMENT "glslangValidator ${_name} (vendored upscaler pass) -> ${_name}.comp.spv"
            VERBATIM)
        list(APPEND _fuse_upscale_spv_outputs "${_out}")
    endforeach()
    add_custom_target(fuse_upscale_reference_shaders ALL DEPENDS ${_fuse_upscale_spv_outputs})
    message(STATUS "FUSE: vendored FSR1 / CAS / NIS GLSL passes compiled at build time (${FUSE_UPSCALE_SPV_DIR})")
else()
    message(STATUS "FUSE: glslangValidator not found — upscaler GPU-shader reference gate skips")
endif()

if(FUSE_BUILD_CORE_TESTS)
    # Interface / registry, CpuReference vs CpuParallel parity, SDK constant setup vs the vendored host
    # helpers, quality (PSNR / SSIM) vs bilinear with a recorded report.
    add_executable(fuse_upscale_gates ${CMAKE_CURRENT_LIST_DIR}/../tests/test_upscale_gates.cpp)
    target_link_libraries(fuse_upscale_gates PRIVATE fuse_rhi)
    target_include_directories(fuse_upscale_gates SYSTEM PRIVATE "${FUSE_FIDELITYFX_DIR}/include")
    target_compile_definitions(fuse_upscale_gates PRIVATE
        FUSE_UPSCALE_REPORT_PATH="${CMAKE_CURRENT_BINARY_DIR}/upscale_quality_report.json")
    add_test(NAME fuse_upscale_gates COMMAND fuse_upscale_gates)
    set_tests_properties(fuse_upscale_gates PROPERTIES LABELS "gate" TIMEOUT 600)

    # CPU ports vs the vendored GLSL passes executed on a Vulkan device (Lavapipe in CI). The Vulkan loader
    # is opened at run time (no link dependency), so the gate also runs in stub (FUSE_VULKAN_BACKEND=OFF)
    # builds; SKIP (77) without shaders, a loader or a device.
    set(_fuse_upscale_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")
    add_executable(fuse_upscale_gpu_reference ${CMAKE_CURRENT_LIST_DIR}/../tests/test_upscale_gpu_reference.cpp)
    target_link_libraries(fuse_upscale_gpu_reference PRIVATE fuse_rhi ${CMAKE_DL_LIBS})
    target_include_directories(fuse_upscale_gpu_reference SYSTEM PRIVATE "${FUSE_NVIDIA_NIS_DIR}/NIS")
    target_compile_definitions(fuse_upscale_gpu_reference PRIVATE
        FUSE_UPSCALE_SPV_DIR="${FUSE_UPSCALE_SPV_DIR}"
        FUSE_UPSCALE_GPU_REPORT_PATH="${CMAKE_CURRENT_BINARY_DIR}/upscale_gpu_reference_report.json")
    if(TARGET fuse_upscale_reference_shaders)
        add_dependencies(fuse_upscale_gpu_reference fuse_upscale_reference_shaders)
        target_compile_definitions(fuse_upscale_gpu_reference PRIVATE FUSE_UPSCALE_SHADERS_BUILT=1)
    endif()
    add_test(NAME fuse_upscale_gpu_reference
             COMMAND "${_fuse_upscale_lock}" "$<TARGET_FILE:fuse_upscale_gpu_reference>")
    set_tests_properties(fuse_upscale_gpu_reference PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        TIMEOUT 900
        LABELS "gate;vulkan")
endif()
