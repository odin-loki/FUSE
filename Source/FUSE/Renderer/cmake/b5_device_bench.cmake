# B5 renderer device gates + frame benchmark (docs/plans/FUSE_MASTER_PLAN.md B5 "Full Frame Performance",
# docs/compute-kernels.md): the device-resident DDGI and clustered paths (headers here, device code in
# kernels/ddgi_probe_update.cu / kernels/clustered_lighting.cu, host routing in src/gi/ddgi_cpu.cpp /
# src/lighting/clustered_shading.cpp), their CUDA-event gates, and one per-pass benchmark harness.
# Owned by the B5 device-timing work stream; other topics must not edit this file.
#
#   fuse_b5_ddgi_device_cpu        --cpu: CpuReference == CpuParallel on the timed DDGI workloads (every build)
#   fuse_b5_ddgi_device            RTX 3090: init / first update, device == CPU parity, CUDA-event timing (77 = no GPU)
#   fuse_b5_clustered_device_cpu   --cpu: fixed-capacity light grid == compacted grid shading (every build)
#   fuse_b5_clustered_device       RTX 3090: 1080p / 1000 lights parity + CUDA-event timing (77 = no GPU)
#   fuse_b5_frame_bench_smoke      every pass the tree runs on a device, small resolution: Vulkan passes on
#                                  Lavapipe (timestamp queries), CUDA passes skip without a GPU (77 = no device)
#   fuse_b5_frame_bench            RTX 3090, 1920x1080: per-pass CUDA events / Vulkan timestamps, total, and the
#                                  profiler-vs-CUDA-event cross-check (77 without a CUDA device). Not asserted.

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/gi/ddgi_device.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/fuse/renderer/lighting/clustered_device.hpp
)

if(NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

set(_fuse_b5_bench_tests "${CMAKE_CURRENT_LIST_DIR}/../tests")

# --- DDGI -------------------------------------------------------------------------------------------------
add_executable(fuse_b5_ddgi_device ${_fuse_b5_bench_tests}/test_b5_ddgi_device.cpp)
target_link_libraries(fuse_b5_ddgi_device PRIVATE fuse_rhi)
add_test(NAME fuse_b5_ddgi_device_cpu COMMAND fuse_b5_ddgi_device --cpu)
set_tests_properties(fuse_b5_ddgi_device_cpu PROPERTIES LABELS "gate;renderer" TIMEOUT 600)
add_test(NAME fuse_b5_ddgi_device COMMAND fuse_b5_ddgi_device)
set_tests_properties(fuse_b5_ddgi_device PROPERTIES
    LABELS "gate;cuda;device" RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 900)

# --- Clustered cull + deferred shade ----------------------------------------------------------------------
add_executable(fuse_b5_clustered_device ${_fuse_b5_bench_tests}/test_b5_clustered_device.cpp)
target_link_libraries(fuse_b5_clustered_device PRIVATE fuse_rhi)
add_test(NAME fuse_b5_clustered_device_cpu COMMAND fuse_b5_clustered_device --cpu)
set_tests_properties(fuse_b5_clustered_device_cpu PROPERTIES LABELS "gate;renderer" TIMEOUT 600)
add_test(NAME fuse_b5_clustered_device COMMAND fuse_b5_clustered_device)
set_tests_properties(fuse_b5_clustered_device PROPERTIES
    LABELS "gate;cuda;device" RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 900)

# --- Per-pass frame benchmark -----------------------------------------------------------------------------
# CUDA passes: resident buffers + CUDA events (tests/cuda/b5_frame_bench_cuda.cu, the sdf_ray_march /
# sdf_shadows / screen_space_ao / screen_space_reflections kernel bodies from their device-safe headers) and the
# DdgiDeviceVolume / ClusteredDeviceFrame paths. Vulkan passes: GpuProfiler timestamp zones (G-buffer raster,
# TAAU, GPU post stack). The screen-space kernel headers live in fuse_ssfx's include tree (header-only use).
add_executable(fuse_b5_frame_bench
    ${_fuse_b5_bench_tests}/test_b5_frame_bench.cpp
    ${_fuse_b5_bench_tests}/b5_frame_bench_vk.cpp)
target_link_libraries(fuse_b5_frame_bench PRIVATE fuse_rhi fuse_rhi_profiling fuse_temporal fuse_post_gpu)
target_include_directories(fuse_b5_frame_bench PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../ScreenSpace/include")
target_compile_definitions(fuse_b5_frame_bench PRIVATE FUSE_B5_RHI_SHADER_DIR="${FUSE_B5_RHI_SHADER_OUT_DIR}")
if(TARGET fuse_b5_rhi_shaders)
    add_dependencies(fuse_b5_frame_bench fuse_b5_rhi_shaders)
    target_compile_definitions(fuse_b5_frame_bench PRIVATE FUSE_B5_RHI_SHADERS_BUILT=1)
endif()
if(FUSE_CUDA_BACKEND)
    target_sources(fuse_b5_frame_bench PRIVATE ${_fuse_b5_bench_tests}/cuda/b5_frame_bench_cuda.cu)
    target_compile_definitions(fuse_b5_frame_bench PRIVATE FUSE_B5_FRAME_BENCH_CUDA=1)
endif()

# Smoke (every build): small frame, Vulkan passes on Lavapipe, CUDA passes skip without a GPU. Timings printed,
# never asserted.
if(FUSE_VULKAN_BACKEND AND NOT WIN32)
    # Linux CI: Lavapipe, serialised on the shared ICD lock (tests/CMakeLists.txt writes the wrapper).
    add_test(NAME fuse_b5_frame_bench_smoke
             COMMAND "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh" "$<TARGET_FILE:fuse_b5_frame_bench>"
                     --width 320 --height 180 --iterations 3 --warmup 1 --draws 100)
    set_tests_properties(fuse_b5_frame_bench_smoke PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json")
else()
    add_test(NAME fuse_b5_frame_bench_smoke COMMAND fuse_b5_frame_bench --width 320 --height 180 --iterations 3
                                                    --warmup 1 --draws 100)
endif()
set_tests_properties(fuse_b5_frame_bench_smoke PROPERTIES
    LABELS "gate;vulkan;renderer" RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 900)

# The RTX 3090 run: 1920x1080, real Vulkan driver (no ICD override), exit 77 without a CUDA device.
add_test(NAME fuse_b5_frame_bench COMMAND fuse_b5_frame_bench --require-cuda)
set_tests_properties(fuse_b5_frame_bench PROPERTIES
    LABELS "cuda;device;bench" RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 1800)
