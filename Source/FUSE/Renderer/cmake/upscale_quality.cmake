# Upscaler evaluation stream (docs/research/upscaling-framegen-and-post-injectors.md, recommendations 1-3):
# image-quality / temporal metrics (quality/), the render/display split + UpscaleInputs contract (upscale/) and the
# native single-source TAAU kernel "taau" (taa/taau*) in fuse_rhi; the deterministic reference scenes in
# fuse_upscale_refscenes (needs fuse_compute's SDF ray-march kernel). Owned by one work stream; other topics must
# not edit this file.

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/quality/image_metrics.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/quality/quality_kernels.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/taa/taa_kernel_common.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/taa/taau.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/taa/taau_kernel.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/upscale/upscale_inputs.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/quality/image_metrics.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/taa/taau.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/upscale/upscale_inputs.cpp
)

if(FUSE_BUILD_CORE_TESTS)
    # Metric self-tests: known values (identical images, analytic noise PSNR, constant-offset SSIM), official
    # flip_evaluator 1.7 / scikit-image reference values on deterministic image pairs, kernel parity.
    add_executable(fuse_render_quality_metrics ${CMAKE_CURRENT_LIST_DIR}/../tests/test_render_quality_metrics.cpp)
    target_link_libraries(fuse_render_quality_metrics PRIVATE fuse_rhi)
    add_test(NAME fuse_render_quality_metrics COMMAND fuse_render_quality_metrics)
    set_tests_properties(fuse_render_quality_metrics PROPERTIES LABELS "gate" TIMEOUT 300)
    # Optional published FLIP pair check (FUSE_FLIP_REFERENCE_DIR) decodes PNGs with the vendored stb_image.
    set(_fuse_quality_stb_dir "${CMAKE_SOURCE_DIR}/Engine/source/gfx/bitmap/loaders/stb")
    if(EXISTS "${_fuse_quality_stb_dir}/stb_image.h")
        target_include_directories(fuse_render_quality_metrics SYSTEM PRIVATE "${_fuse_quality_stb_dir}")
        target_compile_definitions(fuse_render_quality_metrics PRIVATE FUSE_QUALITY_TEST_HAS_STB=1)
    endif()
endif()

# fuse_compute is added after Renderer (Source/FUSE/CMakeLists.txt) under this same condition.
if(FUSE_BUILD_COMPUTE OR FUSE_BUILD_CUDA)
    add_library(fuse_upscale_refscenes STATIC
        ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/upscale/reference_scene.hpp
        ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/upscale/reference_scene_kernel.hpp
        ${CMAKE_CURRENT_LIST_DIR}/../src/upscale/reference_scene.cpp
    )
    target_link_libraries(fuse_upscale_refscenes PUBLIC fuse_rhi fuse_compute)
    target_compile_features(fuse_upscale_refscenes PUBLIC cxx_std_17)

    if(FUSE_BUILD_CORE_TESTS)
        # TAAU gates: 1.5x / 2x on the reference scenes vs native ground truth (PSNR / SSIM / FLIP, flicker,
        # ghosting, disocclusion, NaN), spatial baselines, CpuReference == CpuParallel parity, kernel stats.
        add_executable(fuse_taau_gates ${CMAKE_CURRENT_LIST_DIR}/../tests/test_taau_gates.cpp)
        target_link_libraries(fuse_taau_gates PRIVATE fuse_upscale_refscenes)
        add_test(NAME fuse_taau_gates COMMAND fuse_taau_gates)
        set_tests_properties(fuse_taau_gates PROPERTIES LABELS "gate" TIMEOUT 900)
    endif()
endif()
