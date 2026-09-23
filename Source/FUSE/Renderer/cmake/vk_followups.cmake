# Vulkan core follow-ups (async upload queue, bindless sizing): sources and tests.
# Owned by one work stream; other topics must not edit this file.

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/renderer/vk/upload_queue.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/upload_queue.cpp
)

if(FUSE_BUILD_CORE_TESTS)
    # tests/CMakeLists.txt writes the ICD lock wrapper; its variable is scoped to that directory.
    set(_fuse_vk_followups_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")

    add_executable(fuse_b2_async_upload tests/test_b2_async_upload.cpp)
    target_link_libraries(fuse_b2_async_upload PRIVATE fuse_rhi)
    add_test(NAME fuse_b2_async_upload COMMAND "${_fuse_vk_followups_lock}" "$<TARGET_FILE:fuse_b2_async_upload>")
    set_tests_properties(fuse_b2_async_upload PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE
        LABELS "gate"
    )

    # Layout tracking (generateMips / readback preserve uploaded data) + deferred destroy.
    add_executable(fuse_resource_layout_tracking tests/test_resource_layout_tracking.cpp)
    target_link_libraries(fuse_resource_layout_tracking PRIVATE fuse_rhi)
    add_test(NAME fuse_resource_layout_tracking
             COMMAND "${_fuse_vk_followups_lock}" "$<TARGET_FILE:fuse_resource_layout_tracking>")

    # TaaResolve runs the CPU reference resolver when host surfaces are bound.
    add_executable(fuse_taa_cpu_resolve_wiring tests/test_taa_cpu_resolve_wiring.cpp)
    target_link_libraries(fuse_taa_cpu_resolve_wiring PRIVATE fuse_rhi)
    add_test(NAME fuse_taa_cpu_resolve_wiring
             COMMAND "${_fuse_vk_followups_lock}" "$<TARGET_FILE:fuse_taa_cpu_resolve_wiring>")

    set_tests_properties(fuse_resource_layout_tracking fuse_taa_cpu_resolve_wiring PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE
    )
endif()
