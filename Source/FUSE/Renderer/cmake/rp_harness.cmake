# WP-0.7 renderer test harness and goldens (docs/unification/RENDERER-EXECUTION.md, plan §8).
# Owned by the WP-0.7 work stream; other packages must not edit this file.
#
#   fuse_rp_harness            static library: headless frame runner on the G-buffer raster path,
#                              PNG/EXR encoders, deterministic mini scenes, golden comparison
#   fuse_rp_harness_image_io   CPU-only gate (also runs in the stub backend)
#   fuse_rp_harness_golden     Vulkan golden gates (one ctest per scene + the round-trip exit test)
#
# ctest labels: every test carries "renderer;golden" and "tier_t<N>" (the minimum renderer tier it
# needs; the binary exits 77 when the device is below it). Vulkan tests also carry "vulkan", run
# with VK_LAYER_KHRONOS_validation + synchronization validation through the layer-settings
# environment (see _fuse_rp_harness_validation_env), and serialise on the ICD lock.
#
# Goldens live in Tests/golden/renderer/ (small PNGs, policy in its README.md). Regenerate with
#   FUSE_UPDATE_GOLDENS=1 ctest --test-dir build/fuse-debug -L golden
# which prints a summary of every rewritten file.

if(NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

set(_fuse_rp_harness_dir "${CMAKE_CURRENT_LIST_DIR}/../tests/harness")

add_library(fuse_rp_harness STATIC
    ${_fuse_rp_harness_dir}/image_io.hpp
    ${_fuse_rp_harness_dir}/image_io.cpp
    ${_fuse_rp_harness_dir}/scene.hpp
    ${_fuse_rp_harness_dir}/scene.cpp
    ${_fuse_rp_harness_dir}/golden.hpp
    ${_fuse_rp_harness_dir}/golden.cpp
    ${_fuse_rp_harness_dir}/frame_runner.hpp
    ${_fuse_rp_harness_dir}/frame_runner.cpp
)
target_include_directories(fuse_rp_harness PUBLIC "${_fuse_rp_harness_dir}")
target_link_libraries(fuse_rp_harness PUBLIC fuse_rhi)
fuse_apply_cxx23(fuse_rp_harness)
# Scene geometry and the resolve must be bit-reproducible across compilers: no FMA contraction.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(fuse_rp_harness PRIVATE -ffp-contract=off)
elseif(MSVC)
    target_compile_options(fuse_rp_harness PRIVATE /fp:precise)
endif()

# ---- harness vertex stage (CPU-projected NDC with per-vertex depth) -----------------------------
set(FUSE_RP_HARNESS_SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders/rp_harness")
if(FUSE_GLSLANG_VALIDATOR)
    file(MAKE_DIRECTORY "${FUSE_RP_HARNESS_SHADER_OUT_DIR}")
    set(_src "${_fuse_rp_harness_dir}/shaders/harness_gbuffer.vert")
    set(_out "${FUSE_RP_HARNESS_SHADER_OUT_DIR}/harness_gbuffer.vert.spv")
    set(_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" -V "${_src}" -o "${_out}")
    if(FUSE_SPIRV_VAL)
        list(APPEND _cmds COMMAND "${FUSE_SPIRV_VAL}" "${_out}")
    endif()
    add_custom_command(OUTPUT "${_out}" ${_cmds} DEPENDS "${_src}"
        COMMENT "glslangValidator harness_gbuffer.vert -> SPIR-V" VERBATIM)
    add_custom_target(fuse_rp_harness_shaders ALL DEPENDS "${_out}")
endif()

# ---- tests --------------------------------------------------------------------------------------
set(_fuse_rp_harness_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")
set(FUSE_RP_GOLDEN_DIR "${CMAKE_SOURCE_DIR}/Tests/golden/renderer")
set(FUSE_RP_HARNESS_ARTIFACT_DIR "${CMAKE_BINARY_DIR}/rp_harness_artifacts")

# Synchronization validation for every renderer/golden Vulkan test (Khronos layer settings env).
set(_fuse_rp_harness_validation_env
    "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
    "VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
    "VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT"
    "VK_KHRONOS_VALIDATION_DEBUG_ACTION=VK_DBG_LAYER_ACTION_CALLBACK")

add_executable(fuse_rp_harness_image_io ${_fuse_rp_harness_dir}/test_rp_harness_image_io.cpp)
target_link_libraries(fuse_rp_harness_image_io PRIVATE fuse_rp_harness)
add_test(NAME rp_harness_image_io
         COMMAND fuse_rp_harness_image_io "${CMAKE_CURRENT_BINARY_DIR}/rp_harness_image_io")
set_tests_properties(rp_harness_image_io PROPERTIES LABELS "renderer;golden;tier_t0")
# Cross-check the PNG encoder with an independent decoder when the vendored stb_image is present.
if(EXISTS "${FUSE_STB_DIR}/stb_image.h")
    target_include_directories(fuse_rp_harness_image_io SYSTEM PRIVATE "${FUSE_STB_DIR}")
    target_compile_definitions(fuse_rp_harness_image_io PRIVATE FUSE_RP_HARNESS_HAS_STB=1)
endif()

add_executable(fuse_rp_harness_golden ${_fuse_rp_harness_dir}/test_rp_harness_golden.cpp)
target_link_libraries(fuse_rp_harness_golden PRIVATE fuse_rp_harness fuse_core)
target_compile_definitions(fuse_rp_harness_golden PRIVATE
    FUSE_RP_GOLDEN_DIR="${FUSE_RP_GOLDEN_DIR}"
    FUSE_RP_HARNESS_ARTIFACT_DIR="${FUSE_RP_HARNESS_ARTIFACT_DIR}"
    FUSE_RP_STOCK_SHADER_DIR="${FUSE_B5_RHI_SHADER_OUT_DIR}"
    FUSE_RP_HARNESS_SHADER_DIR="${FUSE_RP_HARNESS_SHADER_OUT_DIR}")
if(TARGET fuse_rp_harness_shaders)
    add_dependencies(fuse_rp_harness_golden fuse_rp_harness_shaders)
endif()
if(TARGET fuse_b5_rhi_shaders)
    add_dependencies(fuse_rp_harness_golden fuse_b5_rhi_shaders)
endif()

# fuse_rp_add_golden_test(<name> TIER <t0|t1|t2|t3> [TIMEOUT s] ARGS ...)
function(fuse_rp_add_golden_test name)
    cmake_parse_arguments(ARG "" "TIER;TIMEOUT" "ARGS" ${ARGN})
    if(NOT ARG_TIER)
        set(ARG_TIER t0)
    endif()
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 600)
    endif()
    add_test(NAME ${name} COMMAND "${_fuse_rp_harness_lock}" "$<TARGET_FILE:fuse_rp_harness_golden>" ${ARG_ARGS})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "${_fuse_rp_harness_validation_env}"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        TIMEOUT ${ARG_TIMEOUT}
        LABELS "renderer;golden;vulkan;tier_${ARG_TIER}")
endfunction()

# Exit test: stock G-buffer raster path round trip + deliberate one-pixel failure + sync control.
fuse_rp_add_golden_test(rp_golden_gbuffer_roundtrip TIER t0 ARGS --roundtrip --scene gbuffer_quads,cornell_box)
foreach(_scene cornell_box sphere_field thin_wall hud_overlay instance_grid_1k instance_grid_100k)
    fuse_rp_add_golden_test(rp_golden_${_scene} TIER t0 ARGS --scene ${_scene})
endforeach()
# Tier matrix: the legacy raster path must render the same golden on a device capped at each tier.
foreach(_tier t0 t1 t2)
    fuse_rp_add_golden_test(rp_golden_cornell_box_${_tier} TIER ${_tier}
        ARGS --scene cornell_box --tier-cap ${_tier} --require-tier ${_tier})
endforeach()

# ---- late detection (after every fuse_rhi fragment is included) ---------------------------------
# WP-0.1 RendererCaps (render_tier.cpp in fuse_rhi) and the image-metrics library (FLIP / SSIM)
# are used when present; otherwise the harness infers the tier and falls back to PSNR.
function(_fuse_rp_harness_finalize)
    get_target_property(_srcs fuse_rhi SOURCES)
    set(_caps OFF)
    set(_metrics OFF)
    foreach(_s IN LISTS _srcs)
        if(_s MATCHES "src/vk/render_tier\\.cpp$")
            set(_caps ON)
        endif()
        if(_s MATCHES "quality/image_metrics\\.cpp$")
            set(_metrics ON)
        endif()
    endforeach()
    if(_caps)
        target_compile_definitions(fuse_rp_harness PRIVATE FUSE_RP_HARNESS_RENDERER_CAPS=1)
    endif()
    if(NOT _metrics)
        foreach(_t fuse_renderer_quality fuse_quality_metrics fuse_image_metrics)
            if(TARGET ${_t})
                target_link_libraries(fuse_rp_harness PUBLIC ${_t})
                set(_metrics ON)
                break()
            endif()
        endforeach()
    endif()
    if(_metrics)
        target_compile_definitions(fuse_rp_harness PRIVATE FUSE_RP_HARNESS_QUALITY_METRICS=1)
    endif()
    message(STATUS "FUSE: WP-0.7 harness — RendererCaps ${_caps}, FLIP/SSIM metrics ${_metrics} (else PSNR)")
endfunction()
cmake_language(DEFER CALL _fuse_rp_harness_finalize)
