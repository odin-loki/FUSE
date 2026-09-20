# Mobile Vulkan WSI stubs — honest OFF defaults until surface bootstrap lands.
# Android CI keeps FUSE_BUILD_VULKAN=OFF; macOS MoltenVK module deferred.

option(FUSE_VULKAN_ANDROID_WSI_STUB
    "Document Android Vulkan surface bootstrap stub (no VkSurfaceKHR yet)" ON)
option(FUSE_VULKAN_MOLTENVK_STUB
    "Document MoltenVK macOS backend stub (no FUSE_PLATFORM_MACOS RHI yet)" ON)

function(fuse_report_mobile_vulkan_status)
    if(FUSE_PLATFORM_ANDROID)
        if(FUSE_BUILD_VULKAN)
            message(WARNING
                "FUSE: Android Vulkan WSI not wired — prefer FUSE_BUILD_VULKAN=OFF in mobile CI")
        elseif(FUSE_VULKAN_ANDROID_WSI_STUB)
            message(STATUS
                "FUSE: Android Vulkan WSI stub — VkSurfaceKHR via ANativeWindow deferred")
        endif()
    endif()

    if(FUSE_PLATFORM_MACOS AND FUSE_VULKAN_MOLTENVK_STUB)
        message(STATUS
            "FUSE: MoltenVK macOS module stub — real VkSurfaceKHR via Cocoa deferred")
    endif()
endfunction()

fuse_report_mobile_vulkan_status()

# Configure-time summary for docs/tests (see mobile_vulkan_stub.hpp).
set(FUSE_MOBILE_VULKAN_STUB_DOCUMENTED
    "${FUSE_VULKAN_ANDROID_WSI_STUB}|${FUSE_VULKAN_MOLTENVK_STUB}"
    CACHE INTERNAL "Mobile Vulkan stub options enabled at configure time")
