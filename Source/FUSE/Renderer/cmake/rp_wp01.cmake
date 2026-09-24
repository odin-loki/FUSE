# WP-0.1 (docs/unification/RENDERER-EXECUTION.md): Vulkan 1.3 device + renderer tier detection.
# Owned by WP-0.1; other packages must not edit this file.

target_sources(fuse_rhi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/renderer/vk/render_tier.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/render_tier.cpp
)

if(FUSE_BUILD_CORE_TESTS)
    # tests/CMakeLists.txt writes the ICD lock wrapper; its variable is scoped to that directory.
    set(_fuse_rp_wp01_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")

    # Gate: tier reporting (T2 on Lavapipe; T0/T1 via FUSE_RENDER_TIER_MAX and the masking layer),
    # T0 hard-fail in device selection, feature structs that reach the driver (recorded by the
    # layer), sync2 + dynamic rendering + BDA exercised, zero validation messages.
    # Exit 77 = skip (stub build, non-Linux, ICD not resolvable).
    add_executable(fuse_rp_device_tiers tests/test_rp_device_tiers.cpp)
    target_link_libraries(fuse_rp_device_tiers PRIVATE fuse_rhi ${CMAKE_DL_LIBS})
    if(FUSE_VULKAN_BACKEND)
        set(_fuse_mask_layer_dir "${CMAKE_CURRENT_BINARY_DIR}/mask_features_layer")
        add_library(VkLayer_fuse_mask_features MODULE tests/vk_layer_mask_features.cpp)
        # Headers only: a layer must not link the loader.
        if(TARGET Vulkan::Headers)
            target_link_libraries(VkLayer_fuse_mask_features PRIVATE Vulkan::Headers)
        else()
            target_include_directories(VkLayer_fuse_mask_features PRIVATE ${Vulkan_INCLUDE_DIRS})
        endif()
        if(MSVC)
            target_link_options(VkLayer_fuse_mask_features PRIVATE /EXPORT:vkGetInstanceProcAddr
                /EXPORT:vkGetDeviceProcAddr /EXPORT:vkNegotiateLoaderLayerInterfaceVersion
                /EXPORT:fuseMaskFeaturesLastDeviceCreate)
        endif()
        set_target_properties(VkLayer_fuse_mask_features PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${_fuse_mask_layer_dir}"
            CXX_VISIBILITY_PRESET hidden)
        file(GENERATE OUTPUT "${_fuse_mask_layer_dir}/VkLayer_fuse_mask_features.json" CONTENT
"{
    \"file_format_version\": \"1.1.2\",
    \"layer\": {
        \"name\": \"VK_LAYER_FUSE_mask_features\",
        \"type\": \"GLOBAL\",
        \"library_path\": \"./$<TARGET_FILE_NAME:VkLayer_fuse_mask_features>\",
        \"api_version\": \"1.3.0\",
        \"implementation_version\": \"1\",
        \"description\": \"FUSE test layer: hides device extensions / features and records vkCreateDevice\"
    }
}
")
        add_dependencies(fuse_rp_device_tiers VkLayer_fuse_mask_features)
        add_test(NAME fuse_rp_device_tiers
                 COMMAND "${_fuse_rp_wp01_lock}" "$<TARGET_FILE:fuse_rp_device_tiers>"
                         --layer-dir "${_fuse_mask_layer_dir}")
    else()
        add_test(NAME fuse_rp_device_tiers COMMAND fuse_rp_device_tiers)
    endif()
    set_tests_properties(fuse_rp_device_tiers PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        LABELS "gate;vulkan")
endif()
