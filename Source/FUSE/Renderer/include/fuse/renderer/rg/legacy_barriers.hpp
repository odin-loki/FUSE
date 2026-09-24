#pragma once

// Barrier recording for the render graph v1 facade (CommandBufferRecorder encoding onto the fixed
// VkFrameEncodeContext images). Lives in src/rg/ so that no code outside the render graph records
// vkCmdPipelineBarrier* itself (fuse_rp_rg_no_manual_barrier_lint). Numeric Vulkan values
// (synchronization1 masks); `commandBuffer` / `image` / `buffer` are opaque Vk handles.

#include <fuse/types.hpp>

namespace fuse::renderer::rg {

void recordLegacyImageBarrier(void* commandBuffer, void* image, u32 oldLayout, u32 newLayout, u32 aspectMask,
                              u32 srcStages, u32 srcAccess, u32 dstStages, u32 dstAccess);

void recordLegacyBufferBarrier(void* commandBuffer, void* buffer, u32 srcStages, u32 srcAccess, u32 dstStages,
                               u32 dstAccess);

} // namespace fuse::renderer::rg
