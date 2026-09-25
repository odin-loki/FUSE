# FUSE asset plan W0.8 (docs/plans/FUSE_ASSET_PLAN.md §5.4, §6 Wave 0): content golden-render harness.
#
#   fuse_content_golden_lib      static library: LDR-FLIP over the vendored NVIDIA FLIP header
#                                (Engine/lib/nvidia-flip, BSD-3-Clause, fuse_lint_vendored_pins_nvidia_flip)
#                                + deterministic reference scenes (Samples/content_golden)
#   asset_golden_flip            CPU gate (every tree): FLIP identity / sanity ranges / FUSE-FLIP cross-check,
#                                committed goldens decode
#   asset_golden_material_balls  Lavapipe gate: re-render FLIP == 0, golden mean FLIP <= 0.05, sanity
#                                perturbations, zero validation + sync-validation messages
#
# ctest labels "asset;gate;golden" (+ "vulkan;tier_t0" for the render gate). Goldens live in
# Samples/content_golden/golden/; regenerate with
#   FUSE_UPDATE_GOLDENS=1 ctest --test-dir build/fuse-debug -R asset_golden_material_balls
# Depends on the WP-0.7 harness (rp_harness.cmake, included earlier in this directory).

if(NOT FUSE_BUILD_CORE_TESTS OR NOT TARGET fuse_rp_harness)
    return()
endif()

set(_fuse_cg_dir "${CMAKE_SOURCE_DIR}/Samples/content_golden")
set(_fuse_cg_flip "${FUSE_VENDOR_DIR}/nvidia-flip")

add_library(fuse_content_golden_lib STATIC
    ${_fuse_cg_dir}/flip_metric.hpp
    ${_fuse_cg_dir}/flip_metric.cpp
    ${_fuse_cg_dir}/reference_scenes.hpp
    ${_fuse_cg_dir}/reference_scenes.cpp
)
target_include_directories(fuse_content_golden_lib PUBLIC "${_fuse_cg_dir}")
# SYSTEM: FLIP.h is third-party code and is not held to FUSE's warning flags.
target_include_directories(fuse_content_golden_lib SYSTEM PRIVATE "${_fuse_cg_flip}")
target_link_libraries(fuse_content_golden_lib PUBLIC fuse_rp_harness)
fuse_apply_cxx23(fuse_content_golden_lib)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # FLIP's separable convolutions are ~20x slower unoptimised; keep Debug trees fast. No FMA
    # contraction, so the metric is identical across -march settings.
    set_source_files_properties(${_fuse_cg_dir}/flip_metric.cpp PROPERTIES COMPILE_OPTIONS "-O2;-ffp-contract=off")
    set_source_files_properties(${_fuse_cg_dir}/reference_scenes.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
elseif(MSVC)
    target_compile_options(fuse_content_golden_lib PRIVATE /fp:precise)
endif()

set(_fuse_cg_golden_dir "${_fuse_cg_dir}/golden")
set(_fuse_cg_artifacts "${CMAKE_BINARY_DIR}/content_golden_artifacts")

add_executable(fuse_content_golden_flip ${_fuse_cg_dir}/test_content_golden_flip.cpp)
target_link_libraries(fuse_content_golden_flip PRIVATE fuse_content_golden_lib)
target_compile_definitions(fuse_content_golden_flip PRIVATE FUSE_CONTENT_GOLDEN_DIR="${_fuse_cg_golden_dir}")
add_test(NAME asset_golden_flip COMMAND fuse_content_golden_flip --golden-dir "${_fuse_cg_golden_dir}")
set_tests_properties(asset_golden_flip PROPERTIES LABELS "asset;gate;golden" TIMEOUT 300)

add_executable(fuse_content_golden ${_fuse_cg_dir}/test_content_golden.cpp)
target_link_libraries(fuse_content_golden PRIVATE fuse_content_golden_lib fuse_core)
target_compile_definitions(fuse_content_golden PRIVATE
    FUSE_CONTENT_GOLDEN_DIR="${_fuse_cg_golden_dir}"
    FUSE_CONTENT_GOLDEN_ARTIFACT_DIR="${_fuse_cg_artifacts}"
    FUSE_RP_STOCK_SHADER_DIR="${FUSE_B5_RHI_SHADER_OUT_DIR}"
    FUSE_RP_HARNESS_SHADER_DIR="${FUSE_RP_HARNESS_SHADER_OUT_DIR}")
foreach(_dep fuse_rp_harness_shaders fuse_b5_rhi_shaders)
    if(TARGET ${_dep})
        add_dependencies(fuse_content_golden ${_dep})
    endif()
endforeach()
add_test(NAME asset_golden_material_balls
         COMMAND "${_fuse_rp_harness_lock}" "$<TARGET_FILE:fuse_content_golden>"
                 --golden-dir "${_fuse_cg_golden_dir}" --artifact-dir "${_fuse_cg_artifacts}")
set_tests_properties(asset_golden_material_balls PROPERTIES
    ENVIRONMENT "${_fuse_rp_harness_validation_env}"
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
    TIMEOUT 900
    LABELS "asset;gate;golden;vulkan;tier_t0")
