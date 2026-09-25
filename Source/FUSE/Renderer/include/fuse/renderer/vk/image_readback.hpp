#pragma once

#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// Copy a single-mip RGBA8 colour image into `outRgba` (tightly packed rows) with a one-shot
/// graphics-queue submit. Waits for the device to idle first. `trackedLayout` (VkImageLayout as
/// u32) is the image's current layout and is restored afterwards (an UNDEFINED image is left in
/// TRANSFER_SRC_OPTIMAL). The image needs TRANSFER_SRC usage. Returns false in the stub backend.
bool readbackColorImage(VulkanDevice& device, void* image, u32 width, u32 height, u32& trackedLayout,
                        std::vector<u8>& outRgba);

} // namespace fuse::renderer
