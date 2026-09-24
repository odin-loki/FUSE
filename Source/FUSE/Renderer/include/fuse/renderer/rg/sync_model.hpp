#pragma once

// Render graph v2 synchronization model: maps a declared rg::Access to synchronization2 stage and
// access masks, an image layout and the usage bits the resource needs. Pure CPU, no vulkan.h: the
// values are the numeric Vulkan constants (checked against vulkan.h in src/rg/sync_model.cpp).
//
// Only the low 32 bits of VkPipelineStageFlags2 / VkAccessFlags2 are ever produced, which are
// bit-identical to the synchronization1 flags, so the sync1 fallback (devices without
// synchronization2 enabled) is a truncation plus the NONE -> TOP/BOTTOM_OF_PIPE rule.

#include <fuse/renderer/rg/rg_types.hpp>

namespace fuse::renderer::rg {

namespace vkc {
// VkPipelineStageFlagBits2
constexpr u64 kStageNone = 0;
constexpr u64 kStageTopOfPipe = 0x1;
constexpr u64 kStageDrawIndirect = 0x2;
constexpr u64 kStageVertexInput = 0x4;
constexpr u64 kStageVertexShader = 0x8;
constexpr u64 kStageFragmentShader = 0x80;
constexpr u64 kStageEarlyFragmentTests = 0x100;
constexpr u64 kStageLateFragmentTests = 0x200;
constexpr u64 kStageColorAttachmentOutput = 0x400;
constexpr u64 kStageComputeShader = 0x800;
constexpr u64 kStageTransfer = 0x1000;
constexpr u64 kStageBottomOfPipe = 0x2000;
constexpr u64 kStageHost = 0x4000;
constexpr u64 kStageAllGraphics = 0x8000;
constexpr u64 kStageAllCommands = 0x10000;
constexpr u64 kStageTaskShader = 0x80000;  ///< VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT (WP-5.1)
constexpr u64 kStageMeshShader = 0x100000; ///< VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT (WP-5.1)
// VkAccessFlagBits2
constexpr u64 kAccessIndirectCommandRead = 0x1;
constexpr u64 kAccessIndexRead = 0x2;
constexpr u64 kAccessVertexAttributeRead = 0x4;
constexpr u64 kAccessUniformRead = 0x8;
constexpr u64 kAccessShaderRead = 0x20;
constexpr u64 kAccessShaderWrite = 0x40;
constexpr u64 kAccessColorAttachmentRead = 0x80;
constexpr u64 kAccessColorAttachmentWrite = 0x100;
constexpr u64 kAccessDepthStencilRead = 0x200;
constexpr u64 kAccessDepthStencilWrite = 0x400;
constexpr u64 kAccessTransferRead = 0x800;
constexpr u64 kAccessTransferWrite = 0x1000;
constexpr u64 kAccessHostRead = 0x2000;
constexpr u64 kAccessHostWrite = 0x4000;
constexpr u64 kAccessMemoryRead = 0x8000;
constexpr u64 kAccessMemoryWrite = 0x10000;
constexpr u64 kAccessWriteMask = kAccessShaderWrite | kAccessColorAttachmentWrite | kAccessDepthStencilWrite |
                                 kAccessTransferWrite | kAccessHostWrite | kAccessMemoryWrite;
// VkImageLayout
constexpr u32 kLayoutUndefined = 0;
constexpr u32 kLayoutGeneral = 1;
constexpr u32 kLayoutColorAttachment = 2;
constexpr u32 kLayoutDepthStencilAttachment = 3;
constexpr u32 kLayoutDepthStencilReadOnly = 4;
constexpr u32 kLayoutShaderReadOnly = 5;
constexpr u32 kLayoutTransferSrc = 6;
constexpr u32 kLayoutTransferDst = 7;
constexpr u32 kLayoutPresentSrc = 1000001002;
// VkImageAspectFlagBits
constexpr u32 kAspectColor = 0x1;
constexpr u32 kAspectDepth = 0x2;
constexpr u32 kAspectStencil = 0x4;
// VkImageUsageFlagBits
constexpr u32 kImageUsageTransferSrc = 0x1;
constexpr u32 kImageUsageTransferDst = 0x2;
constexpr u32 kImageUsageSampled = 0x4;
constexpr u32 kImageUsageStorage = 0x8;
constexpr u32 kImageUsageColorAttachment = 0x10;
constexpr u32 kImageUsageDepthStencilAttachment = 0x20;
// VkBufferUsageFlagBits
constexpr u32 kBufferUsageTransferSrc = 0x1;
constexpr u32 kBufferUsageTransferDst = 0x2;
constexpr u32 kBufferUsageUniform = 0x10;
constexpr u32 kBufferUsageStorage = 0x20;
constexpr u32 kBufferUsageIndex = 0x40;
constexpr u32 kBufferUsageVertex = 0x80;
constexpr u32 kBufferUsageIndirect = 0x100;
} // namespace vkc

struct AccessInfo {
    u64 stages = 0;
    u64 access = 0;
    u32 layout = vkc::kLayoutUndefined;
    u32 imageUsage = 0;
    u32 bufferUsage = 0;
    bool write = false;
    /// Needs a graphics-capable queue (attachments, vertex input, indirect draw, present).
    bool graphicsOnly = false;
    /// Legal on a transfer-only queue (transfer ops and host readback declarations).
    bool transferOk = false;
};

/// Stage/access/layout/usage for `access`. `shaderStages` (ShaderStage bits) only matters for
/// shader accesses; 0 derives it from `queue`.
AccessInfo describeAccess(Access access, u8 shaderStages, QueueClass queue);

bool isDepthFormat(u32 vkFormat);
bool hasStencil(u32 vkFormat);
/// VkImageAspectFlags for a full-image barrier on `vkFormat`.
u32 aspectForFormat(u32 vkFormat);

/// synchronization2 -> synchronization1 translation (flags produced by describeAccess only use
/// the shared low bits). NONE becomes TOP_OF_PIPE in a source scope, BOTTOM_OF_PIPE in a dest.
u32 toSync1Stages(u64 stages2, bool srcScope);
u32 toSync1Access(u64 access2);

const char* accessName(Access access);
const char* queueClassName(QueueClass queue);

} // namespace fuse::renderer::rg
