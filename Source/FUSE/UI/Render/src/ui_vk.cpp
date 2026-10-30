#include "fuse/ui/render/ui_vk.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>

#if __has_include("ui_draw_list_spv.h")
#include "ui_draw_list_spv.h" // generated: kUiDrawListSpv
#define FUSE_UI_VK_HAS_KERNEL 1
#endif
#if __has_include("ui_backdrop_spv.h")
#include "ui_backdrop_spv.h" // generated: kUiBackdropSpv
#define FUSE_UI_VK_HAS_BACKDROP 1
#endif

#if __has_include("ui_encode_pq_spv.h")
#include "ui_encode_pq_spv.h" // generated: kUiEncodePqSpv
#define FUSE_UI_VK_HAS_ENCODE_PQ 1
#endif
#if __has_include("ui_encode_scrgb_spv.h")
#include "ui_encode_scrgb_spv.h" // generated: kUiEncodeScrgbSpv
#define FUSE_UI_VK_HAS_ENCODE_SCRGB 1
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
/// ui.backdrop push block (must match BlurPush in ui_backdrop.comp).
struct BlurPush {
    i32 area[4];
    i32 srcOrigin[2], srcSize[2];
    u32 mode, cmd;
    u32 radius;
    f32 inv;
};
static_assert(sizeof(BlurPush) == 48);
void computeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}
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

    constexpr u32 kBindings = 5 + kMaxExternal; // target, 4 buffers, external textures
    VkDescriptorSetLayoutBinding b[kBindings]{};
    for (u32 i = 0; i < kBindings; ++i) {
        b[i].binding = i;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                              : i < 5 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                      : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = kBindings;
    sli.pBindings = b;
    VkDescriptorSetLayout setLayout{};
    if (vkCreateDescriptorSetLayout(device, &sli, nullptr, &setLayout) != VK_SUCCESS) return destroy(), false;
    m_setLayout = setLayout;
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuPush)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    VkPipelineLayout layout{};
    if (vkCreatePipelineLayout(device, &pli, nullptr, &layout) != VK_SUCCESS) return destroy(), false;
    m_layout = layout;
    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = sizeof(kUiDrawListSpv);
    mi.pCode = kUiDrawListSpv;
    VkShaderModule module{};
    if (vkCreateShaderModule(device, &mi, nullptr, &module) != VK_SUCCESS) return destroy(), false;
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
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
    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * n},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 * n},
                                           {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxExternal * n}};
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 2 * n;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    VkDescriptorPool pool{};
    if (vkCreateDescriptorPool(device, &dpi, nullptr, &pool) != VK_SUCCESS) return destroy(), false;
    m_pool = pool;

    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);

#if defined(FUSE_UI_VK_HAS_BACKDROP)
    // ui.backdrop: optional (a failure here keeps the draw kernel; backdrops are then drawn unblurred)
    [&] {
        VkDescriptorSetLayoutBinding bb[3]{};
        for (u32 i = 0; i < 3; ++i) {
            bb[i].binding = i;
            bb[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bb[i].descriptorCount = 1;
            bb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo bli{};
        bli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        bli.bindingCount = 3;
        bli.pBindings = bb;
        VkDescriptorSetLayout bl{};
        if (vkCreateDescriptorSetLayout(device, &bli, nullptr, &bl) != VK_SUCCESS) return;
        m_blurSetLayout = bl;
        const VkPushConstantRange br{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BlurPush)};
        VkPipelineLayoutCreateInfo bpl{};
        bpl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        bpl.setLayoutCount = 1;
        bpl.pSetLayouts = &bl;
        bpl.pushConstantRangeCount = 1;
        bpl.pPushConstantRanges = &br;
        VkPipelineLayout blay{};
        if (vkCreatePipelineLayout(device, &bpl, nullptr, &blay) != VK_SUCCESS) return;
        m_blurLayout = blay;
        VkBufferCreateInfo sbi{};
        sbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        sbi.size = VkDeviceSize(std::max(1u, m_desc.maxBlurPixels)) * 32;
        sbi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VkBuffer sb{};
        if (vkCreateBuffer(device, &sbi, nullptr, &sb) != VK_SUCCESS) return;
        m_scratch = sb;
        VkMemoryRequirements sreq{};
        vkGetBufferMemoryRequirements(device, sb, &sreq);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        u32 st = ~0u;
        for (u32 t = 0; t < mp.memoryTypeCount && st == ~0u; ++t)
            if ((sreq.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) st = t;
        for (u32 t = 0; t < mp.memoryTypeCount && st == ~0u; ++t)
            if (sreq.memoryTypeBits & (1u << t)) st = t;
        VkMemoryAllocateInfo sai{};
        sai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        sai.allocationSize = sreq.size;
        sai.memoryTypeIndex = st;
        VkDeviceMemory sm{};
        if (st == ~0u || vkAllocateMemory(device, &sai, nullptr, &sm) != VK_SUCCESS) return;
        m_scratchMem = sm;
        if (vkBindBufferMemory(device, sb, sm, 0) != VK_SUCCESS) return;
        VkShaderModuleCreateInfo bmi{};
        bmi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        bmi.codeSize = sizeof(kUiBackdropSpv);
        bmi.pCode = kUiBackdropSpv;
        VkShaderModule bm{};
        if (vkCreateShaderModule(device, &bmi, nullptr, &bm) != VK_SUCCESS) return;
        VkComputePipelineCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        bci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        bci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        bci.stage.module = bm;
        bci.stage.pName = "main";
        bci.layout = blay;
        VkPipeline bp{};
        const VkResult bpr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &bci, nullptr, &bp);
        vkDestroyShaderModule(device, bm, nullptr);
        if (bpr == VK_SUCCESS) m_blurPipeline = bp;
    }();
#endif

    // external texture slots: nearest sampler + a 1x1 dummy image for unbound slots
    VkSamplerCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    smi.magFilter = smi.minFilter = VK_FILTER_NEAREST;
    smi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    smi.addressModeU = smi.addressModeV = smi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smi.maxLod = 0.f;
    VkSampler sampler{};
    if (vkCreateSampler(device, &smi, nullptr, &sampler) != VK_SUCCESS) return destroy(), false;
    m_sampler = sampler;
    {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {1, 1, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage img{};
        if (vkCreateImage(device, &ii, nullptr, &img) != VK_SUCCESS) return destroy(), false;
        m_dummy = img;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, img, &req);
        u32 type = ~0u;
        for (u32 t = 0; t < mem.memoryTypeCount && type == ~0u; ++t)
            if (req.memoryTypeBits & (1u << t)) type = t;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VkDeviceMemory dm{};
        if (type == ~0u || vkAllocateMemory(device, &ai, nullptr, &dm) != VK_SUCCESS) return destroy(), false;
        m_dummyMem = dm;
        if (vkBindImageMemory(device, img, dm, 0) != VK_SUCCESS) return destroy(), false;
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view{};
        if (vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS) return destroy(), false;
        m_dummyView = view;
        m_dummyReady = false;
    }
    for (u32 s = 0; s < n; ++s) {
        Slot& slot = m_slots[s];
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
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
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VkDeviceMemory dm{};
        if (vkAllocateMemory(device, &ai, nullptr, &dm) != VK_SUCCESS) return destroy(), false;
        slot.memory = dm;
        if (vkBindBufferMemory(device, buf, dm, 0) != VK_SUCCESS) return destroy(), false;
        void* p = nullptr;
        if (vkMapMemory(device, dm, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return destroy(), false;
        slot.mapped = static_cast<u8*>(p);
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
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
        if (m_blurPipeline) {
            VkDescriptorSetAllocateInfo bai{};
            bai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            bai.descriptorPool = pool;
            bai.descriptorSetCount = 1;
            const VkDescriptorSetLayout bl = static_cast<VkDescriptorSetLayout>(m_blurSetLayout);
            bai.pSetLayouts = &bl;
            VkDescriptorSet bs{};
            if (vkAllocateDescriptorSets(device, &bai, &bs) != VK_SUCCESS) return destroy(), false;
            slot.blurSet = bs;
            VkDescriptorBufferInfo bi2[2] = {{buf, m_offsets[0], m_offsets[1] - m_offsets[0]},
                                             {static_cast<VkBuffer>(m_scratch), 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet bw[2]{};
            for (u32 i = 0; i < 2; ++i) {
                bw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                bw[i].dstSet = bs;
                bw[i].dstBinding = i + 1;
                bw[i].descriptorCount = 1;
                bw[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bw[i].pBufferInfo = &bi2[i];
            }
            vkUpdateDescriptorSets(device, 2, bw, 0, nullptr);
        }
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
    if (m_dummyView) vkDestroyImageView(device, static_cast<VkImageView>(m_dummyView), nullptr);
    if (m_dummy) vkDestroyImage(device, static_cast<VkImage>(m_dummy), nullptr);
    if (m_dummyMem) vkFreeMemory(device, static_cast<VkDeviceMemory>(m_dummyMem), nullptr);
    if (m_sampler) vkDestroySampler(device, static_cast<VkSampler>(m_sampler), nullptr);
    m_dummyView = m_dummy = m_dummyMem = m_sampler = nullptr;
    if (m_blurPipeline) vkDestroyPipeline(device, static_cast<VkPipeline>(m_blurPipeline), nullptr);
    if (m_blurLayout) vkDestroyPipelineLayout(device, static_cast<VkPipelineLayout>(m_blurLayout), nullptr);
    if (m_blurSetLayout) vkDestroyDescriptorSetLayout(device, static_cast<VkDescriptorSetLayout>(m_blurSetLayout), nullptr);
    if (m_scratch) vkDestroyBuffer(device, static_cast<VkBuffer>(m_scratch), nullptr);
    if (m_scratchMem) vkFreeMemory(device, static_cast<VkDeviceMemory>(m_scratchMem), nullptr);
    m_blurPipeline = m_blurLayout = m_blurSetLayout = m_scratch = m_scratchMem = nullptr;
    m_atlasUploads = 0;
    m_dummyReady = false;
    for (void*& e : m_external) e = nullptr;
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
    // glyph atlas: only when it changed since this slot last held it (atlasGeneration ~0u = unknown: always)
    const bool atlasSame = d.atlasGeneration != ~0u && s.atlasGeneration == d.atlasGeneration && s.atlasPtr == d.atlas &&
                           s.atlasSize == d.atlasSize;
    if (d.atlas && d.atlasSize && !atlasSame) {
        std::memcpy(s.mapped + m_offsets[2], d.atlas, d.atlasBytes());
        ++m_atlasUploads;
    }
    s.atlasPtr = d.atlas;
    s.atlasSize = d.atlasSize;
    s.atlasGeneration = d.atlasGeneration;
    std::memcpy(s.mapped + m_offsets[3], d.images.data(), d.imageBytes());
    s.push = d.push;
    s.passes.assign(d.passes.begin(), d.passes.end());
    if (s.passes.empty()) s.passes.push_back(GpuPass{}); // pre-pass data: one draw over tileBase 0
    return true;
}

void UiVkRenderer::setExternalImage(u32 ext, void* view) {
    if (ext < kMaxExternal) m_external[ext] = view;
}

void UiVkRenderer::record(void* commandBuffer, void* targetView, u32 slotIndex) {
    if (!valid() || slotIndex >= m_desc.framesInFlight) return;
    const Slot& s = m_slots[slotIndex];
    if (s.push.extent[0] == 0 || s.push.extent[1] == 0) return;
    const VkDevice device = static_cast<VkDevice>(m_desc.device);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(commandBuffer);
    if (!m_dummyReady) {
        // the dummy is never read (no command references an unbound slot) but must be in the declared layout
        VkImageMemoryBarrier db{};
        db.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        db.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        db.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.image = static_cast<VkImage>(m_dummy);
        db.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &db);
        m_dummyReady = true;
    }
    VkDescriptorImageInfo ii[1 + kMaxExternal]{};
    VkWriteDescriptorSet w[1 + kMaxExternal]{};
    ii[0].imageView = static_cast<VkImageView>(targetView);
    ii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    for (u32 e = 0; e < kMaxExternal; ++e) {
        ii[1 + e].sampler = static_cast<VkSampler>(m_sampler);
        ii[1 + e].imageView = static_cast<VkImageView>(m_external[e] ? m_external[e] : m_dummyView);
        ii[1 + e].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    for (u32 i = 0; i < 1 + kMaxExternal; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = static_cast<VkDescriptorSet>(s.set);
        w[i].dstBinding = i == 0 ? 0 : 4 + i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(device, 1 + kMaxExternal, w, 0, nullptr);
    if (s.blurSet) {
        VkWriteDescriptorSet bw{};
        bw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        bw.dstSet = static_cast<VkDescriptorSet>(s.blurSet);
        bw.dstBinding = 0;
        bw.descriptorCount = 1;
        bw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bw.pImageInfo = &ii[0];
        vkUpdateDescriptorSets(device, 1, &bw, 0, nullptr);
    }
    const VkDescriptorSet set = static_cast<VkDescriptorSet>(s.set);
    const VkPipelineLayout layout = static_cast<VkPipelineLayout>(m_layout);
    if (s.push.size[0] == 0 || s.push.size[1] == 0) return; // empty damage box
    bool first = true;
    for (const GpuPass& pass : s.passes) {
        if (!first) computeBarrier(cmd);
        first = false;
        if (pass.kind == kPassDraw) {
            GpuPush push = s.push;
            push.tileBase = pass.tileBase;
            push.flags = pass.flags;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, static_cast<VkPipeline>(m_pipeline));
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuPush), &push);
            vkCmdDispatch(cmd, (push.size[0] + kUiTile - 1) / kUiTile, (push.size[1] + kUiTile - 1) / kUiTile, 1);
            continue;
        }
        // backdrop blur (skipped without the kernel or beyond the scratch: the panel stays unblurred, as before)
        const i64 w = pass.src[2], h = pass.src[3];
        const i64 aw = pass.area[2] - pass.area[0], ah = pass.area[3] - pass.area[1];
        if (!m_blurPipeline || w <= 0 || h <= 0 || aw <= 0 || ah <= 0 || w * h > i64(m_desc.maxBlurPixels)) continue;
        const VkPipelineLayout bl = static_cast<VkPipelineLayout>(m_blurLayout);
        const VkDescriptorSet bs = static_cast<VkDescriptorSet>(s.blurSet);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, static_cast<VkPipeline>(m_blurPipeline));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bl, 0, 1, &bs, 0, nullptr);
        BlurPush bp{};
        bp.cmd = pass.cmd;
        bp.srcOrigin[0] = pass.src[0];
        bp.srcOrigin[1] = pass.src[1];
        bp.srcSize[0] = pass.src[2];
        bp.srcSize[1] = pass.src[3];
        for (int k = 0; k < 4; ++k) bp.area[k] = pass.area[k];
        bp.radius = pass.radius;
        bp.inv = pass.inv;
        auto run = [&](u32 mode, i64 threads) {
            bp.mode = mode;
            vkCmdPushConstants(cmd, bl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BlurPush), &bp);
            vkCmdDispatch(cmd, u32((threads + 63) / 64), 1, 1);
        };
        run(0, w * h);
        for (int it = 0; it < 3; ++it) {
            computeBarrier(cmd);
            run(1, h * 4);
            computeBarrier(cmd);
            run(2, w * 4);
        }
        computeBarrier(cmd);
        run(3, aw * ah);
    }
}

// ---- UiVkEncoder -------------------------------------------------------------------------------------------------
namespace {
struct EncodePush {
    i32 area[4];
    f32 sdrWhite;
};
bool encodeCode(UiVkEncoder::Mode m, const u32*& code, std::size_t& size) {
    code = nullptr;
    size = 0;
#if defined(FUSE_UI_VK_HAS_ENCODE_PQ)
    if (m == UiVkEncoder::Mode::Hdr10) code = kUiEncodePqSpv, size = sizeof(kUiEncodePqSpv);
#endif
#if defined(FUSE_UI_VK_HAS_ENCODE_SCRGB)
    if (m == UiVkEncoder::Mode::ScRgb) code = kUiEncodeScrgbSpv, size = sizeof(kUiEncodeScrgbSpv);
#endif
    return code != nullptr;
}
} // namespace

bool UiVkEncoder::kernelAvailable(Mode mode) {
    const u32* c;
    std::size_t n;
    return encodeCode(mode, c, n);
}

u32 UiVkEncoder::outputFormat() const {
    return m_mode == Mode::Hdr10 ? u32(VK_FORMAT_A2B10G10R10_UNORM_PACK32) : u32(VK_FORMAT_R16G16B16A16_SFLOAT);
}

bool UiVkEncoder::init(void* devicePtr, Mode mode, u32 slots) {
    destroy();
    const u32* code;
    std::size_t size;
    if (!devicePtr || !encodeCode(mode, code, size)) return false;
    VkDevice device = static_cast<VkDevice>(devicePtr);
    m_device = devicePtr;
    m_mode = mode;
    m_slots = std::clamp(slots, 1u, UiVkRenderer::kMaxSlots);
    VkDescriptorSetLayoutBinding b[2]{};
    for (u32 i = 0; i < 2; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = b;
    VkDescriptorSetLayout sl{};
    if (vkCreateDescriptorSetLayout(device, &li, nullptr, &sl) != VK_SUCCESS) return destroy(), false;
    m_setLayout = sl;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(EncodePush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &sl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    VkPipelineLayout pl{};
    if (vkCreatePipelineLayout(device, &pli, nullptr, &pl) != VK_SUCCESS) return destroy(), false;
    m_layout = pl;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = size;
    mi.pCode = code;
    VkShaderModule sm{};
    if (vkCreateShaderModule(device, &mi, nullptr, &sm) != VK_SUCCESS) return destroy(), false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = sm;
    ci.stage.pName = "main";
    ci.layout = pl;
    VkPipeline pp{};
    const VkResult pr2 = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pp);
    vkDestroyShaderModule(device, sm, nullptr);
    if (pr2 != VK_SUCCESS) return destroy(), false;
    const VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * m_slots};
    VkDescriptorPoolCreateInfo poi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poi.maxSets = m_slots;
    poi.poolSizeCount = 1;
    poi.pPoolSizes = &ps;
    VkDescriptorPool pool{};
    if (vkCreateDescriptorPool(device, &poi, nullptr, &pool) != VK_SUCCESS) {
        vkDestroyPipeline(device, pp, nullptr);
        return destroy(), false;
    }
    m_pool = pool;
    VkDescriptorSetLayout layouts[UiVkRenderer::kMaxSlots];
    std::fill(layouts, layouts + m_slots, sl);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = m_slots;
    ai.pSetLayouts = layouts;
    VkDescriptorSet sets[UiVkRenderer::kMaxSlots]{};
    if (vkAllocateDescriptorSets(device, &ai, sets) != VK_SUCCESS) {
        vkDestroyPipeline(device, pp, nullptr);
        return destroy(), false;
    }
    for (u32 i = 0; i < m_slots; ++i) m_sets[i] = sets[i];
    m_pipeline = pp;
    return true;
}

void UiVkEncoder::destroy() {
    if (!m_device) return;
    VkDevice device = static_cast<VkDevice>(m_device);
    if (m_pipeline) vkDestroyPipeline(device, static_cast<VkPipeline>(m_pipeline), nullptr);
    if (m_pool) vkDestroyDescriptorPool(device, static_cast<VkDescriptorPool>(m_pool), nullptr);
    if (m_layout) vkDestroyPipelineLayout(device, static_cast<VkPipelineLayout>(m_layout), nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(device, static_cast<VkDescriptorSetLayout>(m_setLayout), nullptr);
    m_pipeline = m_pool = m_layout = m_setLayout = nullptr;
    for (void*& s : m_sets) s = nullptr;
    m_device = nullptr;
}

void UiVkEncoder::record(void* commandBuffer, u32 slot, void* srcView, void* dstView, int x0, int y0, int x1, int y1,
                         f32 sdrWhiteNits) {
    if (!valid() || slot >= m_slots || x1 <= x0 || y1 <= y0) return;
    VkDevice device = static_cast<VkDevice>(m_device);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(commandBuffer);
    VkDescriptorImageInfo ii[2]{};
    ii[0].imageView = static_cast<VkImageView>(srcView);
    ii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ii[1].imageView = static_cast<VkImageView>(dstView);
    ii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet wr[2]{};
    for (u32 i = 0; i < 2; ++i) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = static_cast<VkDescriptorSet>(m_sets[slot]);
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(device, 2, wr, 0, nullptr);
    const VkDescriptorSet set = static_cast<VkDescriptorSet>(m_sets[slot]);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, static_cast<VkPipeline>(m_pipeline));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, static_cast<VkPipelineLayout>(m_layout), 0, 1, &set, 0,
                            nullptr);
    const EncodePush push{{x0, y0, x1, y1}, sdrWhiteNits};
    vkCmdPushConstants(cmd, static_cast<VkPipelineLayout>(m_layout), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(cmd, u32(x1 - x0 + 15) / 16, u32(y1 - y0 + 15) / 16, 1);
}

} // namespace fuse::ui::render
