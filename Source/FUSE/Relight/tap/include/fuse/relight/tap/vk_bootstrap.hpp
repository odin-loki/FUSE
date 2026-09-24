// FUSE Relight RL-1.1: queries on the FUSE-created Vulkan instance / device that DXVK imports
// (src/vk_bootstrap.cpp; Windows / MinGW builds only). The creation itself is driven by DXVK
// through the FUSE-DXVK patches RL-1.1-01/02 and the options in tap_config.hpp.
#pragma once

#include <fuse/relight/tap/frame_host.hpp>

#include <cstdint>

namespace fuse::relight::tap::vkboot {

struct Stats {
    std::uint32_t validationErrors = 0;   ///< debug-utils ERROR messages (validation / performance)
    std::uint32_t validationWarnings = 0; ///< debug-utils WARNING messages
    bool validationLayer = false;         ///< VK_LAYER_KHRONOS_validation enabled on the PE side
    bool debugMessenger = false;          ///< a messenger is installed (relight.vk.validation)
};

/// True when `device` (a VkDevice value) was created by the bootstrap and imported by DXVK.
bool isImportedDevice(std::uint64_t device);

Stats stats();

/// The creation parameters of a bootstrap device (`device` a VkDevice value): the loader, instance, enabled
/// instance / device extensions, the feature chain passed to vkCreateDevice and the queue (storage owned by the
/// bootstrap, alive for the process). False for a device the bootstrap did not create.
bool deviceCreateInfo(std::uint64_t device, HostDeviceInfo& out);

} // namespace fuse::relight::tap::vkboot
