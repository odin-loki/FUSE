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
endif()
