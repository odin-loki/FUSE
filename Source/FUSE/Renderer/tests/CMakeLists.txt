set(FUSE_VULKAN_TEST_ENV "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json")
set(FUSE_VULKAN_ICD_LOCK_FILE "${CMAKE_BINARY_DIR}/fuse_vulkan_icd.lock")
set(FUSE_VULKAN_ICD_LOCK_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/run_vulkan_icd_locked.sh")
file(TOUCH "${FUSE_VULKAN_ICD_LOCK_FILE}")
# Serialise ICD-using tests on one lock and propagate the test's exit status unchanged.
# (The previous wrapper exited 0 on every failure via `if ! cmd; then status=$?`, and retried
# segfaults — both hid real failures.)
file(WRITE "${FUSE_VULKAN_ICD_LOCK_SCRIPT}"
"#!/usr/bin/env bash\nexec flock -x \"${FUSE_VULKAN_ICD_LOCK_FILE}\" \"$@\"\n")
file(CHMOD "${FUSE_VULKAN_ICD_LOCK_SCRIPT}"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

set(_fuse_vulkan_icd_tests
    fuse_vulkan_bootstrap
    fuse_vulkan_swapchain
    fuse_rhi_present_path_stub
    fuse_rhi_queue_submit
    fuse_pipeline_cache
    fuse_vulkan_phase2_integration
    fuse_vulkan_resources
    fuse_graphics_pipeline
    fuse_renderer_bootstrap
    fuse_render_graph
    fuse_composite_pass
)

add_executable(fuse_vulkan_bootstrap test_vulkan_bootstrap.cpp)
target_link_libraries(fuse_vulkan_bootstrap PRIVATE fuse_rhi)
add_test(NAME fuse_vulkan_bootstrap COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_vulkan_bootstrap>")

add_executable(fuse_vulkan_swapchain test_vulkan_swapchain.cpp)
target_link_libraries(fuse_vulkan_swapchain PRIVATE fuse_rhi)
add_test(NAME fuse_vulkan_swapchain COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_vulkan_swapchain>")

add_executable(fuse_win32_swapchain_wsi test_win32_swapchain_wsi.cpp)
target_link_libraries(fuse_win32_swapchain_wsi PRIVATE fuse_rhi)
add_test(NAME fuse_win32_swapchain_wsi COMMAND fuse_win32_swapchain_wsi)

add_executable(fuse_rhi_present_path_stub test_rhi_present_path_stub.cpp)
target_link_libraries(fuse_rhi_present_path_stub PRIVATE fuse_rhi)
add_test(NAME fuse_rhi_present_path_stub COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_rhi_present_path_stub>")

add_executable(fuse_rhi_queue_submit test_rhi_queue_submit.cpp)
target_link_libraries(fuse_rhi_queue_submit PRIVATE fuse_rhi)
add_test(NAME fuse_rhi_queue_submit COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_rhi_queue_submit>")

add_executable(fuse_rhi_resource_destroy_order test_rhi_resource_destroy_order.cpp)
target_link_libraries(fuse_rhi_resource_destroy_order PRIVATE fuse_rhi)
add_test(NAME fuse_rhi_resource_destroy_order COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_rhi_resource_destroy_order>")

add_executable(fuse_vulkan_resources test_vulkan_resources.cpp)
target_link_libraries(fuse_vulkan_resources PRIVATE fuse_rhi)
add_test(NAME fuse_vulkan_resources COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_vulkan_resources>")

add_executable(fuse_bindless_descriptors test_bindless_descriptors.cpp)
target_link_libraries(fuse_bindless_descriptors PRIVATE fuse_rhi)
add_test(NAME fuse_bindless_descriptors COMMAND fuse_bindless_descriptors)

add_executable(fuse_pipeline_cache test_pipeline_cache.cpp)
target_link_libraries(fuse_pipeline_cache PRIVATE fuse_rhi)
target_compile_definitions(fuse_pipeline_cache PRIVATE
    FUSE_SHADER_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer/shaders/fixtures"
)
add_test(NAME fuse_pipeline_cache COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_pipeline_cache>")

add_executable(fuse_shader_pipeline test_shader_pipeline.cpp)
target_link_libraries(fuse_shader_pipeline PRIVATE fuse_rhi)
target_compile_definitions(fuse_shader_pipeline PRIVATE
    FUSE_SHADER_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer/shaders/fixtures"
)
add_test(NAME fuse_shader_pipeline COMMAND fuse_shader_pipeline)

add_executable(fuse_shader_watch test_shader_watch.cpp)
target_link_libraries(fuse_shader_watch PRIVATE fuse_rhi)
add_test(NAME fuse_shader_watch COMMAND fuse_shader_watch)

add_executable(fuse_cuda_interop test_cuda_interop.cpp)
target_link_libraries(fuse_cuda_interop PRIVATE fuse_rhi)
add_test(NAME fuse_cuda_interop COMMAND fuse_cuda_interop)

add_executable(fuse_graphics_pipeline test_graphics_pipeline.cpp)
target_link_libraries(fuse_graphics_pipeline PRIVATE fuse_rhi)
target_compile_definitions(fuse_graphics_pipeline PRIVATE
    FUSE_SHADER_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer/shaders/fixtures"
)
add_test(NAME fuse_graphics_pipeline COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_graphics_pipeline>")

add_executable(fuse_b2_triangle_readback test_b2_triangle_readback.cpp)
target_link_libraries(fuse_b2_triangle_readback PRIVATE fuse_rhi)
add_test(NAME fuse_b2_triangle_readback COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_b2_triangle_readback>")

add_executable(fuse_render_graph test_render_graph.cpp)
target_link_libraries(fuse_render_graph PRIVATE fuse_rhi)
add_test(NAME fuse_render_graph COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_render_graph>")

add_executable(fuse_composite_pass test_composite_pass.cpp)
target_link_libraries(fuse_composite_pass PRIVATE fuse_rhi)
target_compile_definitions(fuse_composite_pass PRIVATE
    FUSE_SHADER_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/Source/FUSE/Renderer/shaders/fixtures"
)
add_test(NAME fuse_composite_pass COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_composite_pass>")

add_executable(fuse_renderer_bootstrap test_renderer_bootstrap.cpp)
target_link_libraries(fuse_renderer_bootstrap PRIVATE fuse_rhi)
add_test(NAME fuse_renderer_bootstrap COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_renderer_bootstrap>")

add_executable(fuse_vulkan_phase2_integration test_vulkan_phase2_integration.cpp)
target_link_libraries(fuse_vulkan_phase2_integration PRIVATE fuse_rhi)
add_test(NAME fuse_vulkan_phase2_integration COMMAND "${FUSE_VULKAN_ICD_LOCK_SCRIPT}" "$<TARGET_FILE:fuse_vulkan_phase2_integration>")

add_executable(fuse_frame_manager test_frame_manager.cpp)
target_link_libraries(fuse_frame_manager PRIVATE fuse_rhi)
add_test(NAME fuse_frame_manager COMMAND fuse_frame_manager)

add_executable(fuse_gbuffer test_gbuffer.cpp)
target_link_libraries(fuse_gbuffer PRIVATE fuse_rhi)
add_test(NAME fuse_gbuffer COMMAND fuse_gbuffer)

add_executable(fuse_draw_list test_draw_list.cpp)
target_link_libraries(fuse_draw_list PRIVATE fuse_rhi)
add_test(NAME fuse_draw_list COMMAND fuse_draw_list)

add_executable(fuse_material_system test_material_system.cpp)
target_link_libraries(fuse_material_system PRIVATE fuse_rhi)
add_test(NAME fuse_material_system COMMAND fuse_material_system)

add_executable(fuse_deferred_pipeline test_deferred_pipeline.cpp)
target_link_libraries(fuse_deferred_pipeline PRIVATE fuse_rhi)
add_test(NAME fuse_deferred_pipeline COMMAND fuse_deferred_pipeline)

add_executable(fuse_shadow_system test_shadow_system.cpp)
target_link_libraries(fuse_shadow_system PRIVATE fuse_rhi)
add_test(NAME fuse_shadow_system COMMAND fuse_shadow_system)

add_executable(fuse_clustered_light_culler test_clustered_light_culler.cpp)
target_link_libraries(fuse_clustered_light_culler PRIVATE fuse_rhi)
add_test(NAME fuse_clustered_light_culler COMMAND fuse_clustered_light_culler)

add_executable(fuse_taa_pass test_taa_pass.cpp)
target_link_libraries(fuse_taa_pass PRIVATE fuse_rhi)
add_test(NAME fuse_taa_pass COMMAND fuse_taa_pass)

add_executable(fuse_atmosphere_sky test_atmosphere_sky.cpp)
target_link_libraries(fuse_atmosphere_sky PRIVATE fuse_rhi)
add_test(NAME fuse_atmosphere_sky COMMAND fuse_atmosphere_sky)

add_executable(fuse_ddgi test_ddgi.cpp)
target_link_libraries(fuse_ddgi PRIVATE fuse_rhi)
add_test(NAME fuse_ddgi COMMAND fuse_ddgi)

add_executable(fuse_post_process_stack test_post_process_stack.cpp)
target_link_libraries(fuse_post_process_stack PRIVATE fuse_rhi)
add_test(NAME fuse_post_process_b510 COMMAND fuse_post_process_stack)

add_executable(fuse_volumetric_lighting test_volumetric_lighting.cpp)
target_link_libraries(fuse_volumetric_lighting PRIVATE fuse_rhi)
add_test(NAME fuse_volumetric_lighting_b511 COMMAND fuse_volumetric_lighting)

add_executable(fuse_phase5_deferred_integration test_phase5_deferred_integration.cpp)
target_link_libraries(fuse_phase5_deferred_integration PRIVATE fuse_rhi)
add_test(NAME fuse_phase5_deferred_integration COMMAND fuse_phase5_deferred_integration)

foreach(_fuse_vulkan_test IN LISTS _fuse_vulkan_icd_tests)
    if(TEST ${_fuse_vulkan_test})
        set_tests_properties(${_fuse_vulkan_test} PROPERTIES
            ENVIRONMENT "${FUSE_VULKAN_TEST_ENV}"
            RUN_SERIAL TRUE
        )
    endif()
endforeach()
