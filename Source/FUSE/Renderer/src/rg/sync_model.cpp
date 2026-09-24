#include <fuse/renderer/rg/sync_model.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer::rg {

#if defined(FUSE_VULKAN_BACKEND)
// The Vulkan-free constants must be the real values (and the sync1 low bits must match sync2).
static_assert(vkc::kStageTopOfPipe == VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
static_assert(vkc::kStageDrawIndirect == VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
static_assert(vkc::kStageVertexInput == VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT);
static_assert(vkc::kStageVertexShader == VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
static_assert(vkc::kStageFragmentShader == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
static_assert(vkc::kStageEarlyFragmentTests == VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT);
static_assert(vkc::kStageLateFragmentTests == VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
static_assert(vkc::kStageColorAttachmentOutput == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
static_assert(vkc::kStageComputeShader == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
static_assert(vkc::kStageTransfer == VK_PIPELINE_STAGE_2_TRANSFER_BIT);
static_assert(vkc::kStageBottomOfPipe == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
static_assert(vkc::kStageHost == VK_PIPELINE_STAGE_2_HOST_BIT);
static_assert(vkc::kStageAllGraphics == VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);
static_assert(vkc::kStageAllCommands == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
#if defined(VK_EXT_mesh_shader)
static_assert(vkc::kStageTaskShader == VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT);
static_assert(vkc::kStageMeshShader == VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT);
static_assert(vkc::kStageTaskShader == VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT);
static_assert(vkc::kStageMeshShader == VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT);
#endif
static_assert(vkc::kStageTransfer == VK_PIPELINE_STAGE_TRANSFER_BIT);
static_assert(vkc::kStageAllCommands == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
#if defined(VK_KHR_acceleration_structure)
static_assert(vkc::kStageAccelerationStructureBuild == VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
static_assert(vkc::kStageAccelerationStructureBuild == VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
static_assert(vkc::kAccessAccelerationStructureRead == VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
static_assert(vkc::kAccessAccelerationStructureWrite == VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
static_assert(vkc::kAccessAccelerationStructureRead == VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
static_assert(vkc::kBufferUsageAccelerationStructureBuildInput ==
              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
static_assert(vkc::kBufferUsageAccelerationStructureStorage == VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
#endif
static_assert(vkc::kBufferUsageShaderDeviceAddress == VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
static_assert(vkc::kAccessIndirectCommandRead == VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
static_assert(vkc::kAccessIndexRead == VK_ACCESS_2_INDEX_READ_BIT);
static_assert(vkc::kAccessVertexAttributeRead == VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
static_assert(vkc::kAccessUniformRead == VK_ACCESS_2_UNIFORM_READ_BIT);
static_assert(vkc::kAccessShaderRead == VK_ACCESS_2_SHADER_READ_BIT);
static_assert(vkc::kAccessShaderWrite == VK_ACCESS_2_SHADER_WRITE_BIT);
static_assert(vkc::kAccessColorAttachmentRead == VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
static_assert(vkc::kAccessColorAttachmentWrite == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
static_assert(vkc::kAccessDepthStencilRead == VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
static_assert(vkc::kAccessDepthStencilWrite == VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
static_assert(vkc::kAccessTransferRead == VK_ACCESS_2_TRANSFER_READ_BIT);
static_assert(vkc::kAccessTransferWrite == VK_ACCESS_2_TRANSFER_WRITE_BIT);
static_assert(vkc::kAccessHostRead == VK_ACCESS_2_HOST_READ_BIT);
static_assert(vkc::kAccessHostWrite == VK_ACCESS_2_HOST_WRITE_BIT);
static_assert(vkc::kAccessMemoryRead == VK_ACCESS_2_MEMORY_READ_BIT);
static_assert(vkc::kAccessMemoryWrite == VK_ACCESS_2_MEMORY_WRITE_BIT);
static_assert(vkc::kAccessMemoryWrite == VK_ACCESS_MEMORY_WRITE_BIT);
static_assert(vkc::kLayoutGeneral == VK_IMAGE_LAYOUT_GENERAL);
static_assert(vkc::kLayoutColorAttachment == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
static_assert(vkc::kLayoutDepthStencilAttachment == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
static_assert(vkc::kLayoutDepthStencilReadOnly == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
static_assert(vkc::kLayoutShaderReadOnly == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
static_assert(vkc::kLayoutTransferSrc == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
static_assert(vkc::kLayoutTransferDst == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
static_assert(vkc::kLayoutPresentSrc == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
static_assert(vkc::kAspectColor == VK_IMAGE_ASPECT_COLOR_BIT);
static_assert(vkc::kAspectDepth == VK_IMAGE_ASPECT_DEPTH_BIT);
static_assert(vkc::kAspectStencil == VK_IMAGE_ASPECT_STENCIL_BIT);
static_assert(vkc::kImageUsageTransferSrc == VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
static_assert(vkc::kImageUsageTransferDst == VK_IMAGE_USAGE_TRANSFER_DST_BIT);
static_assert(vkc::kImageUsageSampled == VK_IMAGE_USAGE_SAMPLED_BIT);
static_assert(vkc::kImageUsageStorage == VK_IMAGE_USAGE_STORAGE_BIT);
static_assert(vkc::kImageUsageColorAttachment == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
static_assert(vkc::kImageUsageDepthStencilAttachment == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
static_assert(vkc::kBufferUsageTransferSrc == VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
static_assert(vkc::kBufferUsageTransferDst == VK_BUFFER_USAGE_TRANSFER_DST_BIT);
static_assert(vkc::kBufferUsageUniform == VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
static_assert(vkc::kBufferUsageStorage == VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
static_assert(vkc::kBufferUsageIndex == VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
static_assert(vkc::kBufferUsageVertex == VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
static_assert(vkc::kBufferUsageIndirect == VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
#endif

namespace {

u64 shaderStageMask(u8 stages, QueueClass queue) {
    if (stages == 0u) {
        stages = queue == QueueClass::Graphics ? static_cast<u8>(kStageVertex | kStageFragment | kStageCompute)
                                               : static_cast<u8>(kStageCompute);
    }
    u64 mask = 0;
    if ((stages & kStageVertex) != 0u) {
        mask |= vkc::kStageVertexShader;
    }
    if ((stages & kStageFragment) != 0u) {
        mask |= vkc::kStageFragmentShader;
    }
    if ((stages & kStageCompute) != 0u) {
        mask |= vkc::kStageComputeShader;
    }
    if ((stages & kStageTask) != 0u) {
        mask |= vkc::kStageTaskShader;
    }
    if ((stages & kStageMesh) != 0u) {
        mask |= vkc::kStageMeshShader;
    }
    return mask;
}

} // namespace

AccessInfo describeAccess(Access access, u8 shaderStages, QueueClass queue) {
    AccessInfo info;
    switch (access) {
    case Access::None:
        break;
    case Access::ColorAttachmentWrite:
        info.stages = vkc::kStageColorAttachmentOutput;
        info.access = vkc::kAccessColorAttachmentRead | vkc::kAccessColorAttachmentWrite;
        info.layout = vkc::kLayoutColorAttachment;
        info.imageUsage = vkc::kImageUsageColorAttachment;
        info.write = true;
        info.graphicsOnly = true;
        break;
    case Access::DepthAttachmentWrite:
        info.stages = vkc::kStageEarlyFragmentTests | vkc::kStageLateFragmentTests;
        info.access = vkc::kAccessDepthStencilRead | vkc::kAccessDepthStencilWrite;
        info.layout = vkc::kLayoutDepthStencilAttachment;
        info.imageUsage = vkc::kImageUsageDepthStencilAttachment;
        info.write = true;
        info.graphicsOnly = true;
        break;
    case Access::DepthAttachmentRead:
        info.stages = vkc::kStageEarlyFragmentTests | vkc::kStageLateFragmentTests;
        info.access = vkc::kAccessDepthStencilRead;
        info.layout = vkc::kLayoutDepthStencilReadOnly;
        info.imageUsage = vkc::kImageUsageDepthStencilAttachment;
        info.graphicsOnly = true;
        break;
    case Access::SampledRead:
        info.stages = shaderStageMask(shaderStages, queue);
        info.access = vkc::kAccessShaderRead;
        info.layout = vkc::kLayoutShaderReadOnly;
        info.imageUsage = vkc::kImageUsageSampled;
        info.bufferUsage = vkc::kBufferUsageStorage; // texel/storage fetch through a buffer view
        break;
    case Access::StorageRead:
        info.stages = shaderStageMask(shaderStages, queue);
        info.access = vkc::kAccessShaderRead;
        info.layout = vkc::kLayoutGeneral;
        info.imageUsage = vkc::kImageUsageStorage;
        info.bufferUsage = vkc::kBufferUsageStorage;
        break;
    case Access::StorageWrite:
        info.stages = shaderStageMask(shaderStages, queue);
        info.access = vkc::kAccessShaderWrite;
        info.layout = vkc::kLayoutGeneral;
        info.imageUsage = vkc::kImageUsageStorage;
        info.bufferUsage = vkc::kBufferUsageStorage;
        info.write = true;
        break;
    case Access::StorageReadWrite:
        info.stages = shaderStageMask(shaderStages, queue);
        info.access = vkc::kAccessShaderRead | vkc::kAccessShaderWrite;
        info.layout = vkc::kLayoutGeneral;
        info.imageUsage = vkc::kImageUsageStorage;
        info.bufferUsage = vkc::kBufferUsageStorage;
        info.write = true;
        break;
    case Access::UniformRead:
        info.stages = shaderStageMask(shaderStages, queue);
        info.access = vkc::kAccessUniformRead;
        info.bufferUsage = vkc::kBufferUsageUniform;
        break;
    case Access::VertexRead:
        info.stages = vkc::kStageVertexInput;
        info.access = vkc::kAccessVertexAttributeRead;
        info.bufferUsage = vkc::kBufferUsageVertex;
        info.graphicsOnly = true;
        break;
    case Access::IndexRead:
        info.stages = vkc::kStageVertexInput;
        info.access = vkc::kAccessIndexRead;
        info.bufferUsage = vkc::kBufferUsageIndex;
        info.graphicsOnly = true;
        break;
    case Access::IndirectRead:
        info.stages = vkc::kStageDrawIndirect;
        info.access = vkc::kAccessIndirectCommandRead;
        info.bufferUsage = vkc::kBufferUsageIndirect;
        break;
    case Access::TransferSrc:
        info.stages = vkc::kStageTransfer;
        info.access = vkc::kAccessTransferRead;
        info.layout = vkc::kLayoutTransferSrc;
        info.imageUsage = vkc::kImageUsageTransferSrc;
        info.bufferUsage = vkc::kBufferUsageTransferSrc;
        info.transferOk = true;
        break;
    case Access::TransferDst:
        info.stages = vkc::kStageTransfer;
        info.access = vkc::kAccessTransferWrite;
        info.layout = vkc::kLayoutTransferDst;
        info.imageUsage = vkc::kImageUsageTransferDst;
        info.bufferUsage = vkc::kBufferUsageTransferDst;
        info.write = true;
        info.transferOk = true;
        break;
    case Access::HostRead:
        info.stages = vkc::kStageHost;
        info.access = vkc::kAccessHostRead;
        info.layout = vkc::kLayoutGeneral;
        info.transferOk = true;
        break;
    case Access::Present:
        info.stages = vkc::kStageNone;
        info.access = 0;
        info.layout = vkc::kLayoutPresentSrc;
        info.graphicsOnly = true;
        break;
    case Access::ExternalRead:
        info.stages = vkc::kStageAllCommands;
        info.access = vkc::kAccessMemoryRead;
        info.layout = vkc::kLayoutGeneral;
        break;
    case Access::ExternalWrite:
        info.stages = vkc::kStageAllCommands;
        info.access = vkc::kAccessMemoryRead | vkc::kAccessMemoryWrite;
        info.layout = vkc::kLayoutGeneral;
        info.write = true;
        break;
    case Access::AccelerationStructureBuildInput:
        info.stages = vkc::kStageAccelerationStructureBuild;
        info.access = vkc::kAccessShaderRead;
        info.bufferUsage = vkc::kBufferUsageAccelerationStructureBuildInput | vkc::kBufferUsageShaderDeviceAddress;
        break;
    case Access::AccelerationStructureBuildRead:
        info.stages = vkc::kStageAccelerationStructureBuild;
        info.access = vkc::kAccessAccelerationStructureRead;
        info.bufferUsage = vkc::kBufferUsageAccelerationStructureStorage | vkc::kBufferUsageShaderDeviceAddress;
        break;
    case Access::AccelerationStructureBuildWrite:
        info.stages = vkc::kStageAccelerationStructureBuild;
        info.access = vkc::kAccessAccelerationStructureRead | vkc::kAccessAccelerationStructureWrite;
        info.bufferUsage = vkc::kBufferUsageAccelerationStructureStorage | vkc::kBufferUsageShaderDeviceAddress;
        info.write = true;
        break;
    case Access::AccelerationStructureRead:
        info.stages = shaderStageMask(shaderStages, queue);
        info.access = vkc::kAccessAccelerationStructureRead;
        info.bufferUsage = vkc::kBufferUsageAccelerationStructureStorage | vkc::kBufferUsageShaderDeviceAddress;
        break;
    }
    return info;
}

bool isDepthFormat(u32 vkFormat) {
    return vkFormat >= 124u && vkFormat <= 130u && vkFormat != 127u; // D16 .. D32_S8, not S8
}

bool hasStencil(u32 vkFormat) {
    return vkFormat >= 127u && vkFormat <= 130u;
}

u32 aspectForFormat(u32 vkFormat) {
    if (vkFormat == 127u) {
        return vkc::kAspectStencil;
    }
    if (isDepthFormat(vkFormat)) {
        return hasStencil(vkFormat) ? (vkc::kAspectDepth | vkc::kAspectStencil) : vkc::kAspectDepth;
    }
    return vkc::kAspectColor;
}

u32 toSync1Stages(u64 stages2, bool srcScope) {
    const u32 low = static_cast<u32>(stages2 & 0xFFFFFFFFull);
    if (low == 0u) {
        return static_cast<u32>(srcScope ? vkc::kStageTopOfPipe : vkc::kStageBottomOfPipe);
    }
    return low;
}

u32 toSync1Access(u64 access2) {
    return static_cast<u32>(access2 & 0xFFFFFFFFull);
}

const char* accessName(Access access) {
    switch (access) {
    case Access::None: return "None";
    case Access::ColorAttachmentWrite: return "ColorAttachmentWrite";
    case Access::DepthAttachmentWrite: return "DepthAttachmentWrite";
    case Access::DepthAttachmentRead: return "DepthAttachmentRead";
    case Access::SampledRead: return "SampledRead";
    case Access::StorageRead: return "StorageRead";
    case Access::StorageWrite: return "StorageWrite";
    case Access::StorageReadWrite: return "StorageReadWrite";
    case Access::UniformRead: return "UniformRead";
    case Access::VertexRead: return "VertexRead";
    case Access::IndexRead: return "IndexRead";
    case Access::IndirectRead: return "IndirectRead";
    case Access::TransferSrc: return "TransferSrc";
    case Access::TransferDst: return "TransferDst";
    case Access::HostRead: return "HostRead";
    case Access::Present: return "Present";
    case Access::ExternalRead: return "ExternalRead";
    case Access::ExternalWrite: return "ExternalWrite";
    case Access::AccelerationStructureBuildInput: return "AccelerationStructureBuildInput";
    case Access::AccelerationStructureBuildRead: return "AccelerationStructureBuildRead";
    case Access::AccelerationStructureBuildWrite: return "AccelerationStructureBuildWrite";
    case Access::AccelerationStructureRead: return "AccelerationStructureRead";
    }
    return "?";
}

const char* queueClassName(QueueClass queue) {
    switch (queue) {
    case QueueClass::Graphics: return "graphics";
    case QueueClass::AsyncCompute: return "async-compute";
    case QueueClass::Transfer: return "transfer";
    }
    return "?";
}

} // namespace fuse::renderer::rg
