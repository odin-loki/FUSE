# WP-0.2 (docs/unification/RENDERER-EXECUTION.md): load Vulkan through the vendored volk meta-loader.
# Owned by WP-0.2; other packages must not edit this file. cmake/FuseVolk.cmake holds the wiring
# (VK_NO_PROTOTYPES, the <vulkan/vulkan.h> shim, dropping the link-time loader).

include(${CMAKE_SOURCE_DIR}/cmake/FuseVolk.cmake)

target_sources(fuse_rhi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/renderer/vk/loader.hpp)
if(FUSE_VULKAN_BACKEND)
    fuse_volk_attach(fuse_rhi IMPLEMENTATION ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/vk_loader.cpp)
else()
    # Stub build: the loader API compiles to no-ops.
    target_sources(fuse_rhi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/vk_loader.cpp)
endif()

if(FUSE_BUILD_CORE_TESTS)
    # tests/CMakeLists.txt writes the ICD lock wrapper; its variable is scoped to that directory.
    set(_fuse_rp_wp02_lock "${CMAKE_CURRENT_BINARY_DIR}/tests/run_vulkan_icd_locked.sh")

    # Gate: volk global/instance/device loading, device-level dispatch with one live device,
    # loader trampolines with two, reload hook, real work on each device, zero validation messages
    # (validation + sync validation). Exit 77 = skip (stub build, no ICD).
    add_executable(fuse_rp_volk_loader tests/test_rp_volk_loader.cpp)
    target_link_libraries(fuse_rp_volk_loader PRIVATE fuse_rhi)
    if(FUSE_VULKAN_BACKEND)
        add_test(NAME fuse_rp_volk_loader COMMAND "${_fuse_rp_wp02_lock}" "$<TARGET_FILE:fuse_rp_volk_loader>")
    else()
        add_test(NAME fuse_rp_volk_loader COMMAND fuse_rp_volk_loader)
    endif()
    set_tests_properties(fuse_rp_volk_loader PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
        LABELS "gate;vulkan")

    # Gate: nm/readelf over fuse_rhi and a test executable: no vk* import except vkGetInstanceProcAddr,
    # no DT_NEEDED libvulkan (ELF only; other platforms and the stub build skip).
    if(FUSE_VULKAN_BACKEND AND CMAKE_EXECUTABLE_FORMAT STREQUAL "ELF" AND CMAKE_NM AND CMAKE_READELF)
        add_test(NAME fuse_rp_volk_no_loader_imports
                 COMMAND "${CMAKE_COMMAND}" -DNM=${CMAKE_NM} -DREADELF=${CMAKE_READELF}
                         -DEXE=$<TARGET_FILE:fuse_rp_volk_loader> -DARCHIVE=$<TARGET_FILE:fuse_rhi>
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rp_wp02_imports.cmake")
    else()
        add_test(NAME fuse_rp_volk_no_loader_imports
                 COMMAND "${CMAKE_COMMAND}" -E echo "SKIP: needs the Vulkan backend on an ELF platform")
        set_tests_properties(fuse_rp_volk_no_loader_imports PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP:")
    endif()
    set_tests_properties(fuse_rp_volk_no_loader_imports PROPERTIES SKIP_RETURN_CODE 77 LABELS "gate;vulkan;lint")
endif()
