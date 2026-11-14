# Renderer hardware exit criteria (docs/unification/RENDERER-HW-RUN.md): the rows RENDERER-EXECUTION.md tags
# "LVP-func / HW", measured on a discrete GPU. Owned by the HW-run work stream; other packages must not edit this file.
#
#   fuse_rp_hw_bench            plan §5.3 per-pass budget harness at 1440p (FrameComposer frame, GpuProfiler zones per
#                               render-graph pass, grouped into the §5.3 stages) + the P2 (4,096 lights), P3 (VSM
#                               cached frame) and P8 (volumetrics) budget scenarios on the procedural Sponza / Bistro
#                               stand-ins (test_rp_hw_bench_scene.hpp)
#   fuse_rp_hw_bench_dag        P5: > 100M source-triangle cluster-DAG scene (WP-5.2 DAG, WP-5.3 GPU cut) + the
#                               visibility-buffer cost of the selected cut
#   fuse_rp_hw_bench_restir     P7: ReSTIR DI with 10k emissive triangles, variance against a 4096-spp f64 reference
#   fuse_rp_hw_bench_p9         P9: neural texture compression and gaussian splats vs the conventional path (memory,
#                               quality, cost)
#   fuse_rp_hw_bench_refscenes  §8 Sponza / Bistro: pinned-sha256 asset check (fetched out of band, never by the test)
#
# Labels "perf;hw": RUN_SERIAL, exit 77 without a discrete GPU (Lavapipe CI, stub trees) or without the reference
# assets. No ENVIRONMENT: the gates must see the real ICDs, not the Lavapipe-pinned FUSE_VULKAN_LVP_ENV.
# Baselines: tests/perf/rp_hw_bench_baseline.json (per device name); FUSE_UPDATE_PERF_BASELINE=1 rewrites the entry
# of the device the run measured.

if(NOT FUSE_BUILD_CORE_TESTS)
    return()
endif()

set(_fuse_hw_dir "${CMAKE_CURRENT_LIST_DIR}/../tests")
set(_fuse_hw_baseline "${_fuse_hw_dir}/perf/rp_hw_bench_baseline.json")
set(_fuse_hw_assets "${CMAKE_SOURCE_DIR}/Samples/reference_scenes" CACHE PATH
    "Root of the fetched renderer reference scenes (Sponza / Bistro, docs/unification/RENDERER-HW-RUN.md)")

function(_fuse_hw_test name)
    cmake_parse_arguments(_a "" "TIMEOUT" "COMMAND" ${ARGN})
    if(NOT _a_TIMEOUT)
        set(_a_TIMEOUT 1800)
    endif()
    add_test(NAME ${name} COMMAND ${_a_COMMAND})
    set_tests_properties(${name} PROPERTIES
        LABELS "perf;hw;renderer"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        TIMEOUT ${_a_TIMEOUT})
endfunction()

# --- §5.3 budget harness + P2 / P3 / P8 ----------------------------------------------------------------------
add_executable(fuse_rp_hw_bench
    ${_fuse_hw_dir}/test_rp_hw_bench.cpp
    ${_fuse_hw_dir}/test_rp_hw_bench_common.hpp
    ${_fuse_hw_dir}/test_rp_hw_bench_scene.hpp)
target_link_libraries(fuse_rp_hw_bench PRIVATE fuse_frame fuse_geometry fuse_rhi_profiling)
target_compile_definitions(fuse_rp_hw_bench PRIVATE FUSE_RP_HW_BASELINE="${_fuse_hw_baseline}")
fuse_apply_cxx23(fuse_rp_hw_bench)
if(TARGET fuse_radix_sort_shaders)
    add_dependencies(fuse_rp_hw_bench fuse_radix_sort_shaders)
endif()
if(TARGET fuse_gsplat_header)
    add_dependencies(fuse_rp_hw_bench fuse_gsplat_header)
endif()
# The regression gate: every §5.3 stage and the frame total against the committed baseline (fail > 5 %).
_fuse_hw_test(fuse_rp_hw_budget_1440p_sponza COMMAND fuse_rp_hw_bench --mode gate --scene sponza)
_fuse_hw_test(fuse_rp_hw_budget_1440p_bistro COMMAND fuse_rp_hw_bench --mode gate --scene bistro)
# Plan budget numbers (asserted against the plan's figures, not a baseline).
_fuse_hw_test(fuse_rp_hw_p2_lights_4096 COMMAND fuse_rp_hw_bench --mode lights4096)
_fuse_hw_test(fuse_rp_hw_p3_vsm_cached COMMAND fuse_rp_hw_bench --mode vsm_cached)
_fuse_hw_test(fuse_rp_hw_p8_volumetrics COMMAND fuse_rp_hw_bench --mode volumetrics)

# --- P5: 100M-triangle DAG scene --------------------------------------------------------------------------
add_executable(fuse_rp_hw_bench_dag
    ${_fuse_hw_dir}/test_rp_hw_bench_dag.cpp
    ${_fuse_hw_dir}/test_rp_hw_bench_common.hpp)
target_link_libraries(fuse_rp_hw_bench_dag PRIVATE fuse_geometry_streaming fuse_frame fuse_geometry fuse_rhi_profiling)
fuse_apply_cxx23(fuse_rp_hw_bench_dag)
_fuse_hw_test(fuse_rp_hw_p5_dag_100m COMMAND fuse_rp_hw_bench_dag TIMEOUT 3600)

# --- P7: ReSTIR DI, 10k emissive lights, variance vs reference ----------------------------------------------
add_executable(fuse_rp_hw_bench_restir
    ${_fuse_hw_dir}/test_rp_hw_bench_restir.cpp
    ${_fuse_hw_dir}/test_rp_hw_bench_common.hpp)
target_link_libraries(fuse_rp_hw_bench_restir PRIVATE fuse_restir fuse_rt_reference fuse_denoise fuse_geometry fuse_rhi_profiling)
fuse_apply_cxx23(fuse_rp_hw_bench_restir)
_fuse_hw_test(fuse_rp_hw_p7_restir_10k COMMAND fuse_rp_hw_bench_restir TIMEOUT 3600)

# --- P9: neural texture compression + gaussian splats vs the conventional path ------------------------------
add_executable(fuse_rp_hw_bench_p9
    ${_fuse_hw_dir}/test_rp_hw_bench_p9.cpp
    ${_fuse_hw_dir}/test_rp_hw_bench_common.hpp)
target_link_libraries(fuse_rp_hw_bench_p9 PRIVATE fuse_neural fuse_gsplat fuse_frame fuse_geometry fuse_rhi_profiling)
# BCn rows: the cook encoders (Tools/FUSE/Cook) compiled into the test (unity include, no assimp / physics deps).
target_include_directories(fuse_rp_hw_bench_p9 PRIVATE "${CMAKE_SOURCE_DIR}/Tools/FUSE/Cook/include")
target_compile_definitions(fuse_rp_hw_bench_p9 PRIVATE FUSE_P9_BC_UNITY=1)
fuse_apply_cxx23(fuse_rp_hw_bench_p9)
if(TARGET fuse_radix_sort_shaders)
    add_dependencies(fuse_rp_hw_bench_p9 fuse_radix_sort_shaders)
endif()
if(TARGET fuse_gsplat_header)
    add_dependencies(fuse_rp_hw_bench_p9 fuse_gsplat_header)
endif()
_fuse_hw_test(fuse_rp_hw_p9_conventional COMMAND fuse_rp_hw_bench_p9)

# --- §8 reference scenes (Sponza, Bistro): asset presence + pinned sha256 -------------------------------------
# The test never downloads; it skips (77) while an archive is absent or unpinned. Fetch (out of band, with
# permission): cmake -DDEST=<asset root> -P Source/FUSE/Renderer/cmake/rp_hw_fetch_reference_scenes.cmake
add_executable(fuse_rp_hw_bench_refscenes ${_fuse_hw_dir}/test_rp_hw_bench_refscenes.cpp)
target_compile_definitions(fuse_rp_hw_bench_refscenes PRIVATE FUSE_RP_HW_ASSET_ROOT="${_fuse_hw_assets}"
    FUSE_RP_HW_MANIFEST="${_fuse_hw_dir}/perf/reference_scenes.manifest")
fuse_apply_cxx23(fuse_rp_hw_bench_refscenes)
_fuse_hw_test(fuse_rp_hw_refscene_sponza COMMAND fuse_rp_hw_bench_refscenes --scene sponza TIMEOUT 600)
_fuse_hw_test(fuse_rp_hw_refscene_bistro COMMAND fuse_rp_hw_bench_refscenes --scene bistro TIMEOUT 600)
