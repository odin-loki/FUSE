# WP-0.3 (docs/unification/RENDERER-EXECUTION.md): render graph v2 — sync2 barrier batches from
# declared accesses, resource table, aliased transient heap (VMA), queue classes with timeline
# semaphores + queue-family ownership transfers, per-pass debug labels. v1 RenderGraph stays as
# the facade. Owned by WP-0.3; other packages must not edit this file.

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/renderer/rg/rg_types.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/renderer/rg/sync_model.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/renderer/rg/graph.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/renderer/rg/executor.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/renderer/rg/legacy_barriers.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rg/sync_model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rg/graph.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rg/executor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rg/legacy_barriers.cpp
)

if(FUSE_BUILD_CORE_TESTS)
    # tests/CMakeLists.txt writes the ICD lock wrapper; its variable is scoped to that directory.
    set(_fuse_rp_wp03_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")

    # CPU gate (stub-safe): barrier derivation (RAW/WAR/WAW on images + buffers, subresource and
    # byte ranges), culling, queue batches + ownership transfers, >32 passes, zero steady-state heap
    # allocations for compile + plan.
    add_executable(fuse_rp_rg_compile tests/test_rp_rg_compile.cpp)
    target_link_libraries(fuse_rp_rg_compile PRIVATE fuse_rhi)
    add_test(NAME fuse_rp_rg_compile COMMAND fuse_rp_rg_compile)
    set_tests_properties(fuse_rp_rg_compile PROPERTIES LABELS "gate;renderer;rg")

    # Vulkan gates (exit 77 = skip: stub build, no ICD, no validation layer, no shaders).
    add_executable(fuse_rp_rg_vulkan tests/test_rp_rg_vulkan.cpp)
    target_link_libraries(fuse_rp_rg_vulkan PRIVATE fuse_rhi)
    set(_fuse_rg_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders/rg")
    if(FUSE_GLSLANG_VALIDATOR)
        file(MAKE_DIRECTORY "${_fuse_rg_shader_dir}")
        set(_fuse_rg_src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/rg/rg_test_add.comp")
        set(_fuse_rg_spv "${_fuse_rg_shader_dir}/rg_test_add.comp.spv")
        set(_fuse_rg_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" -V "${_fuse_rg_src}" -o "${_fuse_rg_spv}")
        if(FUSE_SPIRV_VAL)
            list(APPEND _fuse_rg_cmds COMMAND "${FUSE_SPIRV_VAL}" "${_fuse_rg_spv}")
        endif()
        add_custom_command(OUTPUT "${_fuse_rg_spv}" ${_fuse_rg_cmds}
            DEPENDS "${_fuse_rg_src}"
            COMMENT "glslangValidator rg_test_add.comp -> rg_test_add.comp.spv"
            VERBATIM)
        # WP-7.3 follow-up: raygen kernel for the kStageRayTracing gate (Vulkan 1.2 SPIR-V for GL_EXT_ray_tracing).
        set(_fuse_rg_rt_src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/rg/rg_test_rt.rgen")
        set(_fuse_rg_rt_spv "${_fuse_rg_shader_dir}/rg_test_rt.rgen.spv")
        set(_fuse_rg_rt_cmds COMMAND "${FUSE_GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 "${_fuse_rg_rt_src}" -o "${_fuse_rg_rt_spv}")
        if(FUSE_SPIRV_VAL)
            list(APPEND _fuse_rg_rt_cmds COMMAND "${FUSE_SPIRV_VAL}" --target-env vulkan1.2 "${_fuse_rg_rt_spv}")
        endif()
        add_custom_command(OUTPUT "${_fuse_rg_rt_spv}" ${_fuse_rg_rt_cmds}
            DEPENDS "${_fuse_rg_rt_src}"
            COMMENT "glslangValidator rg_test_rt.rgen -> rg_test_rt.rgen.spv"
            VERBATIM)
        add_custom_target(fuse_rp_rg_shaders DEPENDS "${_fuse_rg_spv}" "${_fuse_rg_rt_spv}")
        add_dependencies(fuse_rp_rg_vulkan fuse_rp_rg_shaders)
        target_compile_definitions(fuse_rp_rg_vulkan PRIVATE FUSE_RG_TEST_SHADER="${_fuse_rg_spv}"
                                                             FUSE_RG_TEST_RT_SHADER="${_fuse_rg_rt_spv}")
    endif()
    set(_fuse_rg_env "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json")
    # (a) 12-pass synthetic graph, RAW/WAR/WAW on images + buffers, sync validation clean,
    #     readback equals the CPU expectation; negative control: barriers suppressed -> hazards.
    add_test(NAME fuse_rp_rg_hazard_sync
             COMMAND "${_fuse_rp_wp03_lock}" "$<TARGET_FILE:fuse_rp_rg_vulkan>" --mode hazards)
    # (c) two non-overlapping transients share one VMA block; readbacks match aliasing off.
    add_test(NAME fuse_rp_rg_transient_alias
             COMMAND "${_fuse_rp_wp03_lock}" "$<TARGET_FILE:fuse_rp_rg_vulkan>" --mode alias)
    # gpu_radix_sort migrated onto RG passes: sort through the graph under sync validation.
    add_test(NAME fuse_rp_rg_radix_sort
             COMMAND "${_fuse_rp_wp03_lock}" "$<TARGET_FILE:fuse_rp_rg_vulkan>" --mode radix)
    # WP-7.3 follow-up: rg::kStageRayTracing. Compute write -> raygen storage read, transfer clear -> raygen sampled
    # read, raygen read -> compute write (WAR); barriers at RAY_TRACING_SHADER, SBT from the allocator
    # (BufferUsage::ShaderBindingTable), sync validation clean; negative control: the same reads declared at the
    # compute stage trip sync validation. Skips (77) without VK_KHR_ray_tracing_pipeline enabled.
    add_test(NAME fuse_rp_rg_rt_stage
             COMMAND "${_fuse_rp_wp03_lock}" "$<TARGET_FILE:fuse_rp_rg_vulkan>" --mode rtstage)
    set_tests_properties(fuse_rp_rg_hazard_sync fuse_rp_rg_transient_alias fuse_rp_rg_radix_sort fuse_rp_rg_rt_stage PROPERTIES
        ENVIRONMENT "${_fuse_rg_env}"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        TIMEOUT 600
        LABELS "gate;vulkan;renderer;rg")
    # (d) async compute + transfer queues through VK_LAYER_FUSE_split_transfer_family (built by
    #     vk_followups.cmake) with its compute family enabled: ownership transfers + timeline waits
    #     clean under sync validation, results equal the single-queue run.
    if(FUSE_VULKAN_BACKEND AND TARGET VkLayer_fuse_split_transfer_family)
        add_dependencies(fuse_rp_rg_vulkan VkLayer_fuse_split_transfer_family)
        add_test(NAME fuse_rp_rg_async_spoof
                 COMMAND "${_fuse_rp_wp03_lock}" "$<TARGET_FILE:fuse_rp_rg_vulkan>" --mode async
                         --layer-dir "${CMAKE_CURRENT_BINARY_DIR}/split_transfer_family_layer")
        set_tests_properties(fuse_rp_rg_async_spoof PROPERTIES
            ENVIRONMENT "${_fuse_rg_env}"
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77
            TIMEOUT 600
            LABELS "gate;vulkan;renderer;rg")
    endif()

    # (b) "No manual barrier" lint: vkCmdPipelineBarrier[2] only inside src/rg/ and the allow-list.
    set(_fuse_rg_lint "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp03_barrier_lint.cmake")
    add_test(NAME fuse_rp_rg_no_manual_barrier_lint
             COMMAND "${CMAKE_COMMAND}" -DFUSE_LINT_ROOT=${CMAKE_SOURCE_DIR}/Source/FUSE -P "${_fuse_rg_lint}")
    # Negative control: a fixture tree with a manual barrier outside src/rg/ must fail the lint.
    set(_fuse_rg_lint_fixture "${CMAKE_CURRENT_BINARY_DIR}/rg_lint_fixture")
    file(WRITE "${_fuse_rg_lint_fixture}/Renderer/src/rg/ok.cpp" "void f(){ vkCmdPipelineBarrier2(cmd, &dep); }\n")
    file(WRITE "${_fuse_rg_lint_fixture}/Renderer/src/feature/bad.cpp"
         "void g(){ vkCmdPipelineBarrier(cmd, 0, 0, 0, 0, nullptr, 0, nullptr, 0, nullptr); }\n")
    add_test(NAME fuse_rp_rg_no_manual_barrier_lint_negative
             COMMAND "${CMAKE_COMMAND}" -DFUSE_LINT_ROOT=${_fuse_rg_lint_fixture} -P "${_fuse_rg_lint}")
    set_tests_properties(fuse_rp_rg_no_manual_barrier_lint fuse_rp_rg_no_manual_barrier_lint_negative PROPERTIES
        LABELS "gate;lint;renderer;rg")
    set_tests_properties(fuse_rp_rg_no_manual_barrier_lint_negative PROPERTIES WILL_FAIL TRUE)
endif()
