# Vulkan gate launcher + environment shared by every directory under Source/FUSE.
#   FUSE_VULKAN_ICD_LOCK_PATH   tests/run_vulkan_icd_locked.sh (written by Renderer/tests/CMakeLists.txt)
#   FUSE_VULKAN_TEST_LAUNCHER   the lock script, empty for Windows targets (no bash / flock). Use it unquoted so it
#                               drops out of `COMMAND ${FUSE_VULKAN_TEST_LAUNCHER} exe ...`.
#   FUSE_VULKAN_LVP_ENV         pins Lavapipe; for Windows targets only where its ICD exists, else the system driver.
set(FUSE_VULKAN_ICD_LOCK_PATH "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
if(WIN32)
    set(FUSE_VULKAN_TEST_LAUNCHER "")
    if(EXISTS "/usr/share/vulkan/icd.d/lvp_icd.json")
        set(FUSE_VULKAN_LVP_ENV "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json")
    else()
        set(FUSE_VULKAN_LVP_ENV "FUSE_VULKAN_SYSTEM_ICD=1")
    endif()
else()
    set(FUSE_VULKAN_TEST_LAUNCHER "${FUSE_VULKAN_ICD_LOCK_PATH}")
    set(FUSE_VULKAN_LVP_ENV "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json")
endif()
