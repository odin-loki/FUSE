#include "fuse/ui/render/ui_vk_mesh.hpp"

#include <vulkan/vulkan.h>

#include <cstring>

#if __has_include("ui_mesh3d_vert_spv.h") && __has_include("ui_mesh3d_frag_spv.h")
#include "ui_mesh3d_frag_spv.h" // generated: kUiMesh3dFragSpv
#include "ui_mesh3d_vert_spv.h" // generated: kUiMesh3dVertSpv
#define FUSE_UI_VK_HAS_MESH3D 1
#endif

namespace fuse::ui::render {

namespace {
struct MeshPush {
    f32 mvp[16];
    f32 light[4];
};
static_assert(sizeof(MeshPush) == 80);

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
    VkAttachmentDescription att[2]{};
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
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    VkSubpassDependency deps[2]{};
    // previous frame's sampling (compute / fragment) before this frame's writes
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // this frame's colour visible to the draw-list kernel (compute) or a fragment compositor
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 2;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    VkRenderPass pass{};
    if (vkCreateRenderPass(dev, &rp, nullptr, &pass) != VK_SUCCESS) { destroy(); return false; }
    m_renderPass = pass;
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(MeshPush)};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    VkPipelineLayout layout{};
    if (vkCreatePipelineLayout(dev, &li, nullptr, &layout) != VK_SUCCESS) { destroy(); return false; }
    m_layout = layout;
    VkShaderModule vs = module(dev, kUiMesh3dVertSpv, sizeof kUiMesh3dVertSpv);
    VkShaderModule fs = module(dev, kUiMesh3dFragSpv, sizeof kUiMesh3dFragSpv);
    VkPipelineShaderStageCreateInfo st[2]{};
    for (int i = 0; i < 2; ++i) st[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, st[i].pName = "main";
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT, st[0].module = vs;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, st[1].module = fs;
    VkVertexInputBindingDescription bind{0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                  {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                  {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 24}};
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 3;
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
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
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
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = m_indexOffset + u64(desc.maxIndices) * 4;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VkBuffer buf{};
    if (vkCreateBuffer(dev, &bi, nullptr, &buf) != VK_SUCCESS) { vkDestroyPipeline(dev, pipe, nullptr); destroy(); return false; }
    m_buffer = buf;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem{};
    void* mapped = nullptr;
    if (mai.memoryTypeIndex == ~0u || vkAllocateMemory(dev, &mai, nullptr, &mem) != VK_SUCCESS) { vkDestroyPipeline(dev, pipe, nullptr); destroy(); return false; }
    m_bufferMem = mem;
    vkBindBufferMemory(dev, buf, mem, 0);
    vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &mapped);
    m_mapped = static_cast<u8*>(mapped);
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
    m_pipeline = m_layout = m_renderPass = m_buffer = m_bufferMem = nullptr;
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
                   m_depth, m_depthMem, m_depthView)) {
        releaseTarget();
        return false;
    }
    VkImageView views[2] = {static_cast<VkImageView>(m_view), static_cast<VkImageView>(m_depthView)};
    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = static_cast<VkRenderPass>(m_renderPass);
    fb.attachmentCount = 2;
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
    m_framebuffer = m_view = m_image = m_imageMem = m_depthView = m_depth = m_depthMem = nullptr;
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
    VkClearValue clears[2]{};
    clears[0].color = {{f32(c & 0xFF) / 255.f, f32((c >> 8) & 0xFF) / 255.f, f32((c >> 16) & 0xFF) / 255.f, f32(c >> 24) / 255.f}};
    clears[1].depthStencil = {1.f, 0};
    VkRenderPassBeginInfo rb{};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = static_cast<VkRenderPass>(m_renderPass);
    rb.framebuffer = static_cast<VkFramebuffer>(m_framebuffer);
    rb.renderArea = {{0, 0}, {m_frame.width, m_frame.height}};
    rb.clearValueCount = 2;
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
        vkCmdPushConstants(cmd, static_cast<VkPipelineLayout>(m_layout), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof p, &p);
        const VkBuffer b = static_cast<VkBuffer>(m_buffer);
        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &b, &zero);
        vkCmdBindIndexBuffer(cmd, b, m_indexOffset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
}

} // namespace fuse::ui::render
