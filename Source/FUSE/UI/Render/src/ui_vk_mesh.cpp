#include "fuse/ui/render/ui_vk_mesh.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>

#if __has_include("ui_mesh3d_vert_spv.h") && __has_include("ui_mesh3d_frag_spv.h")
#include "ui_mesh3d_frag_spv.h" // generated: kUiMesh3dFragSpv
#include "ui_mesh3d_vert_spv.h" // generated: kUiMesh3dVertSpv
#define FUSE_UI_VK_HAS_MESH3D 1
#endif
#if __has_include("ui_mesh3d_exact_frag_spv.h")
#include "ui_mesh3d_exact_frag_spv.h" // generated: kUiMesh3dExactFragSpv
#define FUSE_UI_VK_HAS_MESH3D_EXACT 1
#endif

namespace fuse::ui::render {

namespace {
struct MeshPush {
    f32 mvp[16];
    f32 light[4];
    f32 size[4];
};
static_assert(sizeof(MeshPush) == 96);
bool makeBuffer(VkDevice dev, VkPhysicalDevice phys, u64 size, VkBufferUsageFlags usage, void*& buf, void*& mem, void** mapped);

u32 memType(VkPhysicalDevice phys, u32 bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}
bool makeImage(VkDevice dev, VkPhysicalDevice phys, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect,
               u32 w, u32 h, void*& image, void*& mem, void*& view) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    VkImage im{};
    if (vkCreateImage(dev, &ii, nullptr, &im) != VK_SUCCESS) return false;
    image = im;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, im, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == ~0u) mai.memoryTypeIndex = memType(phys, req.memoryTypeBits, 0);
    VkDeviceMemory m{};
    if (vkAllocateMemory(dev, &mai, nullptr, &m) != VK_SUCCESS) return false;
    mem = m;
    vkBindImageMemory(dev, im, m, 0);
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = im;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView v{};
    if (vkCreateImageView(dev, &vi, nullptr, &v) != VK_SUCCESS) return false;
    view = v;
    return true;
}
VkShaderModule module(VkDevice dev, const u32* code, std::size_t size) {
    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = size;
    mi.pCode = code;
    VkShaderModule m{};
    return vkCreateShaderModule(dev, &mi, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}
} // namespace

namespace {
bool makeBuffer(VkDevice dev, VkPhysicalDevice phys, u64 size, VkBufferUsageFlags usage, void*& buf, void*& mem, void** mapped) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    VkBuffer b{};
    if (vkCreateBuffer(dev, &bi, nullptr, &b) != VK_SUCCESS) return false;
    buf = b;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory m{};
    if (mai.memoryTypeIndex == ~0u || vkAllocateMemory(dev, &mai, nullptr, &m) != VK_SUCCESS) return false;
    mem = m;
    vkBindBufferMemory(dev, b, m, 0);
    return vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, mapped) == VK_SUCCESS;
}
} // namespace

bool UiVkMesh3D::available() {
#if defined(FUSE_UI_VK_HAS_MESH3D)
    return true;
#else
    return false;
#endif
}

bool UiVkMesh3D::init(const UiVkMeshDesc& desc) {
    destroy();
#if defined(FUSE_UI_VK_HAS_MESH3D)
    if (!desc.device || !desc.physicalDevice || !desc.maxWidth || !desc.maxHeight) return false;
    m_desc = desc;
    m_device = desc.device;
    VkDevice dev = static_cast<VkDevice>(desc.device);
    VkPhysicalDevice phys = static_cast<VkPhysicalDevice>(desc.physicalDevice);
    VkAttachmentDescription att[3]{};
    att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    att[1] = att[0];
    att[1].format = VK_FORMAT_D32_SFLOAT;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[2] = att[0];
    att[2].format = VK_FORMAT_R32_UINT;
    att[2].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference cref[2] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 2;
    sub.pColorAttachments = cref;
    sub.pDepthStencilAttachment = &dref;
    VkSubpassDependency deps[2]{};
    // previous frame's sampling (compute / fragment) before this frame's writes
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = 0; // write-after-read: execution dependency only
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // this frame's colour visible to the draw-list kernel (compute) or a fragment compositor
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 3;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    VkRenderPass pass{};
    if (vkCreateRenderPass(dev, &rp, nullptr, &pass) != VK_SUCCESS) { destroy(); return false; }
    m_renderPass = pass;
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPush)};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
#if defined(FUSE_UI_VK_HAS_MESH3D_EXACT)
    m_exact = desc.exactDepth;
#endif
    VkDescriptorSetLayout dsl{};
    if (m_exact) {
        VkDescriptorSetLayoutBinding b[2]{};
        for (u32 i = 0; i < 2; ++i)
            b[i].binding = i, b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, b[i].descriptorCount = 1,
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dli{};
        dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dli.bindingCount = 2;
        dli.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &dli, nullptr, &dsl) != VK_SUCCESS) { destroy(); return false; }
        m_dsl = dsl;
        li.setLayoutCount = 1;
        li.pSetLayouts = &dsl;
    }
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    VkPipelineLayout layout{};
    if (vkCreatePipelineLayout(dev, &li, nullptr, &layout) != VK_SUCCESS) { destroy(); return false; }
    m_layout = layout;
    VkShaderModule vs = module(dev, kUiMesh3dVertSpv, sizeof kUiMesh3dVertSpv);
#if defined(FUSE_UI_VK_HAS_MESH3D_EXACT)
    VkShaderModule fs = m_exact ? module(dev, kUiMesh3dExactFragSpv, sizeof kUiMesh3dExactFragSpv)
                                : module(dev, kUiMesh3dFragSpv, sizeof kUiMesh3dFragSpv);
#else
    VkShaderModule fs = module(dev, kUiMesh3dFragSpv, sizeof kUiMesh3dFragSpv);
#endif
    VkPipelineShaderStageCreateInfo st[2]{};
    for (int i = 0; i < 2; ++i) st[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, st[i].pName = "main";
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT, st[0].module = vs;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, st[1].module = fs;
    VkVertexInputBindingDescription bind{0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[4] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                  {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                  {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 24},
                                                  {3, 0, VK_FORMAT_R32_UINT, 28}};
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 4;
    vin.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; // two-sided, as rasterMesh
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cb[2]{};
    cb[0].colorWriteMask = 0xF;
    cb[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 2;
    cbs.pAttachments = cb;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &dy;
    gp.layout = layout;
    gp.renderPass = pass;
    VkPipeline pipe{};
    const bool ok = vs && fs && vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe) == VK_SUCCESS;
    if (vs) vkDestroyShaderModule(dev, vs, nullptr);
    if (fs) vkDestroyShaderModule(dev, fs, nullptr);
    if (!ok) { destroy(); return false; }

    m_indexOffset = (u64(desc.maxVertices) * sizeof(MeshVertex) + 255) & ~u64(255);
    const u64 total = m_indexOffset + u64(desc.maxIndices) * 4;
    void* mapped = nullptr;
    if (!makeBuffer(dev, phys, total,
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    m_buffer, m_bufferMem, &mapped)) {
        vkDestroyPipeline(dev, pipe, nullptr);
        destroy();
        return false;
    }
    m_mapped = static_cast<u8*>(mapped);
    if (m_exact) {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        VkDescriptorPool dp{};
        VkDescriptorSet set{};
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &dsl;
        if (vkCreateDescriptorPool(dev, &pi, nullptr, &dp) != VK_SUCCESS) { vkDestroyPipeline(dev, pipe, nullptr); destroy(); return false; }
        m_dpool = dp;
        ai.descriptorPool = dp;
        if (vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS) { vkDestroyPipeline(dev, pipe, nullptr); destroy(); return false; }
        m_dset = set;
        const VkBuffer b = static_cast<VkBuffer>(m_buffer);
        VkDescriptorBufferInfo bi[2] = {{b, 0, m_indexOffset}, {b, m_indexOffset, u64(desc.maxIndices) * 4}};
        VkWriteDescriptorSet w[2]{};
        for (u32 i = 0; i < 2; ++i)
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w[i].dstSet = set, w[i].dstBinding = i, w[i].descriptorCount = 1,
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[i].pBufferInfo = &bi[i];
        vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
    }
    {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(phys, &pp);
        VkQueryPoolCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2;
        VkQueryPool q{};
        if (pp.limits.timestampPeriod > 0.f && vkCreateQueryPool(dev, &qi, nullptr, &q) == VK_SUCCESS)
            m_query = q, m_tsPeriod = pp.limits.timestampPeriod;
    }
    m_pipeline = pipe;
    return true;
#else
    (void)desc;
    return false;
#endif
}

void UiVkMesh3D::destroy() {
    if (!m_device) return;
    VkDevice dev = static_cast<VkDevice>(m_device);
    releaseTarget();
    if (m_pipeline) vkDestroyPipeline(dev, static_cast<VkPipeline>(m_pipeline), nullptr);
    if (m_layout) vkDestroyPipelineLayout(dev, static_cast<VkPipelineLayout>(m_layout), nullptr);
    if (m_renderPass) vkDestroyRenderPass(dev, static_cast<VkRenderPass>(m_renderPass), nullptr);
    if (m_buffer) vkDestroyBuffer(dev, static_cast<VkBuffer>(m_buffer), nullptr);
    if (m_bufferMem) vkFreeMemory(dev, static_cast<VkDeviceMemory>(m_bufferMem), nullptr);
    if (m_dpool) vkDestroyDescriptorPool(dev, static_cast<VkDescriptorPool>(m_dpool), nullptr);
    if (m_dsl) vkDestroyDescriptorSetLayout(dev, static_cast<VkDescriptorSetLayout>(m_dsl), nullptr);
    if (m_query) vkDestroyQueryPool(dev, static_cast<VkQueryPool>(m_query), nullptr);
    m_pipeline = m_layout = m_renderPass = m_buffer = m_bufferMem = m_dpool = m_dsl = m_dset = m_query = nullptr;
    m_tsPeriod = 0, m_recorded = m_exact = false;
    m_mapped = nullptr;
    m_device = nullptr;
    m_frame = {};
    m_indexCount = 0;
}

bool UiVkMesh3D::ensureTarget(u32 w, u32 h) {
    if (m_image && m_targetW == w && m_targetH == h) return true;
    VkDevice dev = static_cast<VkDevice>(m_device);
    VkPhysicalDevice phys = static_cast<VkPhysicalDevice>(m_desc.physicalDevice);
    vkDeviceWaitIdle(dev); // resize only: the previous target may still be sampled
    releaseTarget();
    if (!makeImage(dev, phys, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, w, h, m_image, m_imageMem, m_view) ||
        !makeImage(dev, phys, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, w, h,
                   m_depth, m_depthMem, m_depthView) ||
        !makeImage(dev, phys, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, w, h, m_idImage, m_idMem, m_idView)) {
        releaseTarget();
        return false;
    }
    void* rm = nullptr;
    if (!makeBuffer(dev, phys, u64(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, m_readBuf, m_readMem, &rm)) {
        releaseTarget();
        return false;
    }
    m_readMapped = static_cast<u32*>(rm);
    VkImageView views[3] = {static_cast<VkImageView>(m_view), static_cast<VkImageView>(m_depthView), static_cast<VkImageView>(m_idView)};
    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = static_cast<VkRenderPass>(m_renderPass);
    fb.attachmentCount = 3;
    fb.pAttachments = views;
    fb.width = w, fb.height = h, fb.layers = 1;
    VkFramebuffer f{};
    if (vkCreateFramebuffer(dev, &fb, nullptr, &f) != VK_SUCCESS) { releaseTarget(); return false; }
    m_framebuffer = f;
    m_targetW = w, m_targetH = h;
    return true;
}

void UiVkMesh3D::releaseTarget() {
    if (!m_device) return;
    VkDevice dev = static_cast<VkDevice>(m_device);
    if (m_framebuffer) vkDestroyFramebuffer(dev, static_cast<VkFramebuffer>(m_framebuffer), nullptr);
    if (m_view) vkDestroyImageView(dev, static_cast<VkImageView>(m_view), nullptr);
    if (m_image) vkDestroyImage(dev, static_cast<VkImage>(m_image), nullptr);
    if (m_imageMem) vkFreeMemory(dev, static_cast<VkDeviceMemory>(m_imageMem), nullptr);
    if (m_depthView) vkDestroyImageView(dev, static_cast<VkImageView>(m_depthView), nullptr);
    if (m_depth) vkDestroyImage(dev, static_cast<VkImage>(m_depth), nullptr);
    if (m_depthMem) vkFreeMemory(dev, static_cast<VkDeviceMemory>(m_depthMem), nullptr);
    if (m_idView) vkDestroyImageView(dev, static_cast<VkImageView>(m_idView), nullptr);
    if (m_idImage) vkDestroyImage(dev, static_cast<VkImage>(m_idImage), nullptr);
    if (m_idMem) vkFreeMemory(dev, static_cast<VkDeviceMemory>(m_idMem), nullptr);
    if (m_readBuf) vkDestroyBuffer(dev, static_cast<VkBuffer>(m_readBuf), nullptr);
    if (m_readMem) vkFreeMemory(dev, static_cast<VkDeviceMemory>(m_readMem), nullptr);
    m_framebuffer = m_view = m_image = m_imageMem = m_depthView = m_depth = m_depthMem = nullptr;
    m_idView = m_idImage = m_idMem = m_readBuf = m_readMem = nullptr;
    m_readMapped = nullptr;
    m_pickW = m_pickH = m_reqW = m_reqH = 0;
    m_recorded = false;
    m_targetW = m_targetH = 0;
}

bool UiVkMesh3D::upload(const Mesh3D& mesh) {
    if (!valid() || mesh.vertices.size() > m_desc.maxVertices || mesh.indices.size() > m_desc.maxIndices ||
        mesh.frame.width == 0 || mesh.frame.height == 0 || mesh.frame.width > m_desc.maxWidth ||
        mesh.frame.height > m_desc.maxHeight || !ensureTarget(mesh.frame.width, mesh.frame.height))
        return false;
    const std::size_t vb = mesh.vertices.size() * sizeof(MeshVertex), ib = mesh.indices.size() * 4;
    if (vb) std::memcpy(m_mapped, mesh.vertices.data(), vb);
    if (ib) std::memcpy(m_mapped + m_indexOffset, mesh.indices.data(), ib);
    m_uploaded += vb + ib;
    m_frame = mesh.frame;
    m_indexCount = u32(mesh.indices.size() / 3 * 3);
    return true;
}

void UiVkMesh3D::record(void* commandBuffer) {
    if (!valid() || !m_frame.width) return;
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(commandBuffer);
    const u32 c = m_frame.clear;
    VkQueryPool q = static_cast<VkQueryPool>(m_query);
    if (q) {
        vkCmdResetQueryPool(cmd, q, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, q, 0);
    }
    VkClearValue clears[3]{};
    clears[0].color = {{f32(c & 0xFF) / 255.f, f32((c >> 8) & 0xFF) / 255.f, f32((c >> 16) & 0xFF) / 255.f, f32(c >> 24) / 255.f}};
    clears[1].depthStencil = {1.f, 0};
    VkRenderPassBeginInfo rb{};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = static_cast<VkRenderPass>(m_renderPass);
    rb.framebuffer = static_cast<VkFramebuffer>(m_framebuffer);
    rb.renderArea = {{0, 0}, {m_frame.width, m_frame.height}};
    clears[2].color.uint32[0] = 0;
    rb.clearValueCount = 3;
    rb.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport v{0.f, 0.f, f32(m_frame.width), f32(m_frame.height), 0.f, 1.f};
    const VkRect2D sc{{0, 0}, {m_frame.width, m_frame.height}};
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    if (m_indexCount) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, static_cast<VkPipeline>(m_pipeline));
        MeshPush p;
        std::memcpy(p.mvp, m_frame.mvp, sizeof p.mvp);
        std::memcpy(p.light, m_frame.light, sizeof p.light);
        p.size[0] = f32(m_frame.width), p.size[1] = f32(m_frame.height), p.size[2] = p.size[3] = 0.f;
        vkCmdPushConstants(cmd, static_cast<VkPipelineLayout>(m_layout), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof p, &p);
        if (m_dset) {
            const VkDescriptorSet set = static_cast<VkDescriptorSet>(m_dset);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, static_cast<VkPipelineLayout>(m_layout), 0, 1, &set, 0, nullptr);
        }
        const VkBuffer b = static_cast<VkBuffer>(m_buffer);
        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &b, &zero);
        vkCmdBindIndexBuffer(cmd, b, m_indexOffset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
    if (m_reqW && m_reqH) { // id rect -> host (image already in TRANSFER_SRC via the render pass)
        VkBufferImageCopy r{};
        r.bufferRowLength = m_reqW;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageOffset = {int32_t(m_reqX), int32_t(m_reqY), 0};
        r.imageExtent = {m_reqW, m_reqH, 1};
        vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(m_idImage), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               static_cast<VkBuffer>(m_readBuf), 1, &r);
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        m_pickW = m_reqW, m_pickH = m_reqH;
        m_reqW = m_reqH = 0;
    }
    if (q) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, q, 1);
    m_recorded = true;
}

void UiVkMesh3D::requestPick(u32 x, u32 y, u32 w, u32 h) {
    const u32 W = m_frame.width, H = m_frame.height;
    if (x >= W || y >= H || !w || !h) { m_reqW = m_reqH = 0; return; }
    m_reqX = x, m_reqY = y, m_reqW = std::min(w, W - x), m_reqH = std::min(h, H - y);
}

double UiVkMesh3D::gpuMs() const {
    if (!m_query || !m_recorded) return -1.0;
    u64 ts[2] = {};
    const VkResult r = vkGetQueryPoolResults(static_cast<VkDevice>(m_device), static_cast<VkQueryPool>(m_query), 0, 2, sizeof ts, ts,
                                             sizeof(u64), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS || ts[1] < ts[0]) return -1.0;
    return double(ts[1] - ts[0]) * double(m_tsPeriod) * 1e-6;
}

} // namespace fuse::ui::render
