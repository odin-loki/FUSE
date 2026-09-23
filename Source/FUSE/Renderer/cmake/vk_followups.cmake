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

    # Cross-queue-family upload path (ownership release/acquire). Lavapipe has one queue family, so
    # the test-only layer VK_LAYER_FUSE_split_transfer_family (loaded below the validation layer)
    # exposes a transfer-only family 1 folded onto the real queue. Exit 77 = skip (stub build, no
    # ICD, no validation layer).
    add_executable(fuse_b2_upload_queue_family tests/test_b2_upload_queue_family.cpp)
    target_link_libraries(fuse_b2_upload_queue_family PRIVATE fuse_rhi)
    if(FUSE_VULKAN_BACKEND)
        set(_fuse_split_layer_dir "${CMAKE_CURRENT_BINARY_DIR}/split_transfer_family_layer")
        add_library(VkLayer_fuse_split_transfer_family MODULE tests/vk_layer_split_transfer_family.cpp)
        # Headers only: a layer must not link the loader.
        if(TARGET Vulkan::Headers)
            target_link_libraries(VkLayer_fuse_split_transfer_family PRIVATE Vulkan::Headers)
        else()
            target_include_directories(VkLayer_fuse_split_transfer_family PRIVATE ${Vulkan_INCLUDE_DIRS})
        endif()
        set_target_properties(VkLayer_fuse_split_transfer_family PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${_fuse_split_layer_dir}"
            CXX_VISIBILITY_PRESET hidden)
        file(GENERATE OUTPUT "${_fuse_split_layer_dir}/VkLayer_fuse_split_transfer_family.json" CONTENT
"{
    \"file_format_version\": \"1.1.2\",
    \"layer\": {
        \"name\": \"VK_LAYER_FUSE_split_transfer_family\",
        \"type\": \"GLOBAL\",
        \"library_path\": \"./$<TARGET_FILE_NAME:VkLayer_fuse_split_transfer_family>\",
        \"api_version\": \"1.3.0\",
        \"implementation_version\": \"1\",
        \"description\": \"FUSE test layer: splits a transfer-only queue family off a single-family ICD\"
    }
}
")
        add_dependencies(fuse_b2_upload_queue_family VkLayer_fuse_split_transfer_family)

        # Row "Logical device created with graphics, compute, and transfer queues on separate families
        # where available": device creation against three topologies (split layer with an extra
        # compute-only family, split layer transfer-only, plain single-family Lavapipe).
        add_executable(fuse_b2_device_queue_families tests/test_b2_device_queue_families.cpp)
        target_link_libraries(fuse_b2_device_queue_families PRIVATE fuse_rhi)
        add_dependencies(fuse_b2_device_queue_families VkLayer_fuse_split_transfer_family)
        add_test(NAME fuse_b2_device_queue_families_split
                 COMMAND "${_fuse_vk_followups_lock}" "$<TARGET_FILE:fuse_b2_device_queue_families>"
                         --layer-dir "${_fuse_split_layer_dir}" --mode transfer-compute)
        add_test(NAME fuse_b2_device_queue_families_transfer
                 COMMAND "${_fuse_vk_followups_lock}" "$<TARGET_FILE:fuse_b2_device_queue_families>"
                         --layer-dir "${_fuse_split_layer_dir}" --mode transfer)
        add_test(NAME fuse_b2_device_queue_families_shared
                 COMMAND "${_fuse_vk_followups_lock}" "$<TARGET_FILE:fuse_b2_device_queue_families>" --mode shared)
        set_tests_properties(fuse_b2_device_queue_families_split fuse_b2_device_queue_families_transfer
                             fuse_b2_device_queue_families_shared PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77
            LABELS "gate;vulkan")
        add_test(NAME fuse_b2_upload_queue_family
                 COMMAND "${_fuse_vk_followups_lock}" "$<TARGET_FILE:fuse_b2_upload_queue_family>"
                         --layer-dir "${_fuse_split_layer_dir}" --expect-split)
        # Same scenarios on the single-family path (plain Lavapipe).
        add_test(NAME fuse_b2_upload_queue_same_family
                 COMMAND "${_fuse_vk_followups_lock}" "$<TARGET_FILE:fuse_b2_upload_queue_family>")
        set_tests_properties(fuse_b2_upload_queue_same_family PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77
            LABELS "vulkan")
    else()
        add_test(NAME fuse_b2_upload_queue_family COMMAND fuse_b2_upload_queue_family)
    endif()
    set_tests_properties(fuse_b2_upload_queue_family PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        LABELS "gate;vulkan")
endif()
