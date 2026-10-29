#include "fuse/ui/render/ui_vk.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>

#if __has_include("ui_draw_list_spv.h")
#include "ui_draw_list_spv.h" // generated: kUiDrawListSpv
#define FUSE_UI_VK_HAS_KERNEL 1
#endif

namespace fuse::ui::render {

bool UiVkRenderer::kernelAvailable() {
#if defined(FUSE_UI_VK_HAS_KERNEL)
    return true;
#else
    return false;
#endif
}

namespace {
u64 alignUp(u64 v, u64 a) { return (v + a - 1) / a * a; }
} // namespace

bool UiVkRenderer::init(const UiVkDesc& desc) {
    destroy();
#if defined(FUSE_UI_VK_HAS_KERNEL)
    if (!desc.device || !desc.physicalDevice || desc.maxCmds == 0) return false;
    m_desc = desc;
    if (m_desc.framesInFlight == 0) m_desc.framesInFlight = 1;
    if (m_desc.framesInFlight > kMaxSlots) m_desc.framesInFlight = kMaxSlots;
    const VkDevice device = static_cast<VkDevice>(desc.device);
    const VkPhysicalDevice phys = static_cast<VkPhysicalDevice>(desc.physicalDevice);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    const u64 al = std::max<u64>(props.limits.minStorageBufferOffsetAlignment, 16);
    m_offsets[0] = 0;
    m_offsets[1] = alignUp(m_offsets[0] + u64(desc.maxCmds) * sizeof(GpuCmd), al);
    m_offsets[2] = alignUp(m_offsets[1] + u64(desc.maxTileWords) * 4, al);
    m_offsets[3] = alignUp(m_offsets[2] + alignUp(u64(desc.maxAtlasSize) * desc.maxAtlasSize, 4), al);
    m_offsets[4] = alignUp(m_offsets[3] + u64(desc.maxImageWords) * 4, al);

    VkDescriptorSetLayoutBinding b[5]{};
    for (u32 i = 0; i < 5; ++i) {
        b[i].binding = i;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 5;
    sli.pBindings = b;
    VkDescriptorSetLayout setLayout{};
    if (vkCreateDescriptorSetLayout(device, &sli, nullptr, &setLayout) != VK_SUCCESS) return destroy(), false;
    m_setLayout = setLayout;
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    VkPipelineLayout layout{};
    if (vkCreatePipelineLayout(device, &pli, nullptr, &layout) != VK_SUCCESS) return destroy(), false;
    m_layout = layout;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(kUiDrawListSpv);
    mi.pCode = kUiDrawListSpv;
    VkShaderModule module{};
    if (vkCreateShaderModule(device, &mi, nullptr, &module) != VK_SUCCESS) return destroy(), false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline pipeline{};
    const VkResult pr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (pr != VK_SUCCESS) return destroy(), false;
    m_pipeline = pipeline;

    const u32 n = m_desc.framesInFlight;
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, n}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * n}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = n;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    VkDescriptorPool pool{};
    if (vkCreateDescriptorPool(device, &dpi, nullptr, &pool) != VK_SUCCESS) return destroy(), false;
    m_pool = pool;

    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    for (u32 s = 0; s < n; ++s) {
        Slot& slot = m_slots[s];
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = m_offsets[4];
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VkBuffer buf{};
        if (vkCreateBuffer(device, &bi, nullptr, &buf) != VK_SUCCESS) return destroy(), false;
        slot.buffer = buf;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, buf, &req);
        u32 type = ~0u;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (u32 t = 0; t < mem.memoryTypeCount && type == ~0u; ++t)
            if ((req.memoryTypeBits & (1u << t)) && (mem.memoryTypes[t].propertyFlags & want) == want) type = t;
        if (type == ~0u) return destroy(), false;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VkDeviceMemory dm{};
        if (vkAllocateMemory(device, &ai, nullptr, &dm) != VK_SUCCESS) return destroy(), false;
        slot.memory = dm;
        if (vkBindBufferMemory(device, buf, dm, 0) != VK_SUCCESS) return destroy(), false;
        void* p = nullptr;
        if (vkMapMemory(device, dm, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return destroy(), false;
        slot.mapped = static_cast<u8*>(p);
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = pool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &setLayout;
        VkDescriptorSet set{};
        if (vkAllocateDescriptorSets(device, &dai, &set) != VK_SUCCESS) return destroy(), false;
        slot.set = set;
        // buffer bindings never change: write them once
        VkDescriptorBufferInfo infos[4];
        VkWriteDescriptorSet w[4]{};
        const u32 order[4] = {0, 1, 2, 3}; // binding 1 cmds, 2 tiles, 3 atlas, 4 images
        for (u32 i = 0; i < 4; ++i) {
            infos[i] = {buf, m_offsets[order[i]], m_offsets[order[i] + 1] - m_offsets[order[i]]};
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = i + 1;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, 4, w, 0, nullptr);
    }
    return true;
#else
    (void)desc;
    return false;
#endif
}

void UiVkRenderer::destroy() {
    if (!m_desc.device) return;
    const VkDevice device = static_cast<VkDevice>(m_desc.device);
    for (Slot& s : m_slots) {
        if (s.mapped) vkUnmapMemory(device, static_cast<VkDeviceMemory>(s.memory));
        if (s.buffer) vkDestroyBuffer(device, static_cast<VkBuffer>(s.buffer), nullptr);
        if (s.memory) vkFreeMemory(device, static_cast<VkDeviceMemory>(s.memory), nullptr);
        s = Slot{};
    }
    if (m_pool) vkDestroyDescriptorPool(device, static_cast<VkDescriptorPool>(m_pool), nullptr);
    if (m_pipeline) vkDestroyPipeline(device, static_cast<VkPipeline>(m_pipeline), nullptr);
    if (m_layout) vkDestroyPipelineLayout(device, static_cast<VkPipelineLayout>(m_layout), nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(device, static_cast<VkDescriptorSetLayout>(m_setLayout), nullptr);
    m_pool = m_pipeline = m_layout = m_setLayout = nullptr;
    m_desc = UiVkDesc{};
}

bool UiVkRenderer::upload(u32 slotIndex, const GpuDrawData& d) {
    if (!valid() || slotIndex >= m_desc.framesInFlight) return false;
    if (d.cmds.size() > m_desc.maxCmds || d.tiles.size() > m_desc.maxTileWords ||
        d.images.size() > m_desc.maxImageWords || d.atlasSize > m_desc.maxAtlasSize)
        return false;
    Slot& s = m_slots[slotIndex];
    std::memcpy(s.mapped + m_offsets[0], d.cmds.data(), d.cmdBytes());
    std::memcpy(s.mapped + m_offsets[1], d.tiles.data(), d.tileBytes());
    if (d.atlas && d.atlasSize) std::memcpy(s.mapped + m_offsets[2], d.atlas, d.atlasBytes());
    std::memcpy(s.mapped + m_offsets[3], d.images.data(), d.imageBytes());
    s.push = d.push;
    return true;
}

void UiVkRenderer::record(void* commandBuffer, void* targetView, u32 slotIndex) {
    if (!valid() || slotIndex >= m_desc.framesInFlight) return;
    const Slot& s = m_slots[slotIndex];
    if (s.push.extent[0] == 0 || s.push.extent[1] == 0) return;
    const VkDevice device = static_cast<VkDevice>(m_desc.device);
    VkDescriptorImageInfo ii{};
    ii.imageView = static_cast<VkImageView>(targetView);
    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = static_cast<VkDescriptorSet>(s.set);
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(commandBuffer);
    const VkDescriptorSet set = static_cast<VkDescriptorSet>(s.set);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, static_cast<VkPipeline>(m_pipeline));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, static_cast<VkPipelineLayout>(m_layout), 0, 1, &set, 0,
                            nullptr);
    vkCmdPushConstants(cmd, static_cast<VkPipelineLayout>(m_layout), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuPush),
                       &s.push);
    vkCmdDispatch(cmd, (s.push.extent[0] + kUiTile - 1) / kUiTile, (s.push.extent[1] + kUiTile - 1) / kUiTile, 1);
}

} // namespace fuse::ui::render
