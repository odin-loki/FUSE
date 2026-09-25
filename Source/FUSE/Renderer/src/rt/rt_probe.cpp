// WP-6.0 ray-query compute probe (see include/fuse/renderer/rt/rt_probe.hpp).
#include <fuse/renderer/rt/rt_probe.hpp>

#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#include "rt_spv.h"
#endif

namespace fuse::renderer::rt {

RtProbe::~RtProbe() {
    destroy();
}

bool RtProbe::init(VulkanDevice* device, RtKernelLanguage language, BindlessDescriptors* bindless) {
    destroy();
    const RtCapabilities caps = queryRtCapabilities(device);
    if (!caps.usable) {
        m_reason = caps.reason;
        return false;
    }
#if defined(FUSE_VULKAN_BACKEND)
    const u32* code = nullptr;
    usize bytes = 0;
    const char* name = "none";
#if defined(FUSE_RT_SLANG)
    if (language != RtKernelLanguage::Glsl) {
        code = kRtProbeSlangSpv;
        bytes = sizeof(kRtProbeSlangSpv);
        name = "slang";
    }
#endif
#if defined(FUSE_RT_GLSL)
    if (code == nullptr && language != RtKernelLanguage::Slang) {
        code = kRtProbeGlslSpv;
        bytes = sizeof(kRtProbeGlslSpv);
        name = "glsl";
    }
#endif
    (void)language;
    if (code == nullptr) {
        m_reason = "rt_probe kernel not built for the requested language";
        return false;
    }
    const VkDevice vk = static_cast<VkDevice>(device->nativeHandle());
    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = bytes;
    mi.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(vk, &mi, nullptr, &module) != VK_SUCCESS) {
        m_reason = "vkCreateShaderModule failed";
        return false;
    }
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RtProbePush)};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool ok = vkCreatePipelineLayout(vk, &li, nullptr, &layout) == VK_SUCCESS;
    if (ok) {
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.flags = bindless != nullptr ? static_cast<VkPipelineCreateFlags>(bindless->pipelineCreateFlags()) : 0u;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = layout;
        ok = vkCreateComputePipelines(vk, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) == VK_SUCCESS;
    }
    vkDestroyShaderModule(vk, module, nullptr);
    if (!ok) {
        if (layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vk, layout, nullptr);
        }
        m_reason = "rt_probe pipeline creation failed";
        return false;
    }
    m_device = device;
    m_layout = layout;
    m_pipeline = pipeline;
    m_language = name;
    m_reason = "ok";
    return true;
#else
    (void)language;
    (void)bindless;
    m_reason = "stub backend";
    return false;
#endif
}

void RtProbe::destroy() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device != nullptr) {
        const VkDevice vk = static_cast<VkDevice>(m_device->nativeHandle());
        if (m_pipeline != nullptr) {
            vkDestroyPipeline(vk, static_cast<VkPipeline>(m_pipeline), nullptr);
        }
        if (m_layout != nullptr) {
            vkDestroyPipelineLayout(vk, static_cast<VkPipelineLayout>(m_layout), nullptr);
        }
    }
#endif
    m_device = nullptr;
    m_pipeline = nullptr;
    m_layout = nullptr;
    m_language = "none";
    m_count = 0;
}

bool RtProbe::addPass(rg::Graph& graph, const RtProbeDispatch& dispatch) {
    if (!ready() || dispatch.count == 0u || m_count >= kMaxDispatches || dispatch.tlasAddress == 0u ||
        dispatch.raysAddress == 0u || dispatch.hitsAddress == 0u) {
        return false;
    }
    Slot& slot = m_slots[m_count++];
    slot.owner = this;
    slot.push = RtProbePush{};
    RtProbePush& push = slot.push;
    push.tlas = dispatch.tlasAddress;
    push.rays = dispatch.raysAddress;
    push.hits = dispatch.hitsAddress;
    push.count = dispatch.count;
    push.cullMask = dispatch.cullMask;
    push.rayFlags = dispatch.rayFlags;
    rg::PassBuilder pass = graph.addPass("rt.probe", &RtProbe::record, &slot);
    if (dispatch.tlas.valid()) {
        pass.use(dispatch.tlas, rg::Access::AccelerationStructureRead, {}, rg::kStageCompute);
    }
    // Whole buffers: the addresses may point anywhere inside them (several dispatches per buffer).
    if (dispatch.rays.valid()) {
        pass.use(dispatch.rays, rg::Access::StorageRead, {}, rg::kStageCompute);
    }
    if (dispatch.hits.valid()) {
        pass.use(dispatch.hits, rg::Access::StorageWrite, {}, rg::kStageCompute);
    }
    return true;
}

void RtProbe::record(const rg::PassContext& context, void* user) {
#if defined(FUSE_VULKAN_BACKEND)
    const Slot& slot = *static_cast<const Slot*>(user);
    const RtProbe& self = *slot.owner;
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, static_cast<VkPipeline>(self.m_pipeline));
    vkCmdPushConstants(cmd, static_cast<VkPipelineLayout>(self.m_layout), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RtProbePush),
                       &slot.push);
    vkCmdDispatch(cmd, (slot.push.count + kRtWorkgroupSize - 1u) / kRtWorkgroupSize, 1, 1);
#else
    (void)context;
    (void)user;
#endif
}

} // namespace fuse::renderer::rt
