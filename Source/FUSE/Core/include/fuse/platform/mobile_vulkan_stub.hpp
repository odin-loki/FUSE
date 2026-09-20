#pragma once

#include <fuse/types.hpp>

namespace fuse::platform {

/// Mobile Vulkan WSI bootstrap status (honest stubs until surface wiring lands).
enum class MobileVulkanStubKind : u8 {
    None = 0,
    AndroidWsi = 1,
    MoltenVkMacos = 2,
};

struct MobileVulkanStubStatus {
    MobileVulkanStubKind kind = MobileVulkanStubKind::None;
    bool documented = false;
    bool buildVulkanEnabled = false;
    const char* message = nullptr;
};

/// Android ANativeWindow / VkSurfaceKHR bootstrap — deferred; CI keeps FUSE_BUILD_VULKAN=OFF.
inline MobileVulkanStubStatus androidVulkanWsiStubStatus(bool buildVulkanEnabled) {
    MobileVulkanStubStatus status{};
    status.kind = MobileVulkanStubKind::AndroidWsi;
    status.documented = true;
    status.buildVulkanEnabled = buildVulkanEnabled;
    status.message = buildVulkanEnabled
                         ? "Android Vulkan WSI not wired — prefer FUSE_BUILD_VULKAN=OFF in mobile CI"
                         : "Android Vulkan WSI stub — VkSurfaceKHR via ANativeWindow deferred";
    return status;
}

/// MoltenVK macOS module — deferred until FUSE_PLATFORM_MACOS RHI backend lands.
inline MobileVulkanStubStatus moltenVkMacosStubStatus() {
    MobileVulkanStubStatus status{};
    status.kind = MobileVulkanStubKind::MoltenVkMacos;
    status.documented = true;
    status.message = "MoltenVK macOS module stub — real VkSurfaceKHR via Cocoa deferred";
    return status;
}

} // namespace fuse::platform
