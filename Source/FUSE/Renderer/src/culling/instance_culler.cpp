// WP-1.3 GPU instance culling: see include/fuse/renderer/culling/instance_culler.hpp.
#include <fuse/renderer/culling/instance_culler.hpp>

#include <fuse/renderer/culling/hiz_build_kernel.hpp>
#include <fuse/renderer/geometry/meshlet_cull_kernel.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>

#include <cstring>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>

#include "culling_spv.h"
#endif

namespace fuse::renderer::culling {

namespace {
constexpr u32 kConstantsStride = 512u; // >= sizeof(CullConstants), keeps each ring slot 256-byte aligned
static_assert(sizeof(CullConstants) <= kConstantsStride, "constants ring stride");
constexpr u32 kVkLayoutShaderReadOnly = 5u; // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
constexpr u32 kVkFormatR32Sfloat = 100u;
constexpr u32 kCountResetData[kCountResetWords] = {0u, 0u, 0u, 0u, 0u, 1u, 1u, 0u};

u32 argsRegionBytes(u32 capacity) { return capacity * static_cast<u32>(sizeof(DrawIndexedIndirectCommand)); }
} // namespace

InstanceCuller::~InstanceCuller() { destroy(); }

bool InstanceCuller::init(const InstanceCullerDesc& desc) {
    destroy();
    m_desc = desc;
    if (m_desc.framesInFlight == 0u) {
        m_desc.framesInFlight = 1u;
    }
#if defined(FUSE_VULKAN_BACKEND)
    if (desc.device == nullptr || desc.allocator == nullptr || desc.bindless == nullptr || !desc.device->isValid()) {
        return false;
    }
    const RendererCaps& caps = desc.device->info().caps;
    if (!caps.bufferDeviceAddress || !caps.drawIndirectCount) {
        return false;
    }
    m_retired.reserve(8);
    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = 0u;
    samplerDesc.magFilter = 0u;
    samplerDesc.addressMode = 2u; // CLAMP_TO_EDGE
    samplerDesc.maxLod = 16.f;
    samplerDesc.name = "culling.point";
    m_sampler = m_desc.bindless->acquireSampler(samplerDesc);
    if (!m_sampler.isValid() || !createPipelines() ||
        !createBuffer(m_counts, kCountWords * sizeof(u32), true, false, "culling.counts") ||
        !createBuffer(m_constantsRing, static_cast<u64>(kConstantsStride) * m_desc.framesInFlight, false, true,
                      "culling.constants")) {
        m_initialized = true; // let destroy() release what was created
        destroy();
        return false;
    }
    m_initialized = true;
    if (!ensureCapacity(desc.instanceCapacity > 0u ? desc.instanceCapacity : 64u)) {
        destroy();
        return false;
    }
    m_stats.reallocations = 0;
    return true;
#else
    return false;
#endif
}

void InstanceCuller::destroy() {
    if (!m_initialized) {
        return;
    }
#if defined(FUSE_VULKAN_BACKEND)
    collectRetired(~0ull);
    destroyHiz(false);
    destroyBuffer(m_args);
    destroyBuffer(m_candidates);
    destroyBuffer(m_results);
    destroyBuffer(m_counts);
    destroyBuffer(m_constantsRing);
    const VkDevice device = static_cast<VkDevice>(m_desc.device->nativeHandle());
    if (m_cullPipeline != nullptr) {
        vkDestroyPipeline(device, static_cast<VkPipeline>(m_cullPipeline), nullptr);
    }
    if (m_hizPipeline != nullptr) {
        vkDestroyPipeline(device, static_cast<VkPipeline>(m_hizPipeline), nullptr);
    }
    if (m_layout != nullptr) {
        vkDestroyPipelineLayout(device, static_cast<VkPipelineLayout>(m_layout), nullptr);
    }
    if (m_sampler.isValid()) {
        m_desc.bindless->releaseSampler(m_sampler);
    }
#endif
    m_cullPipeline = nullptr;
    m_hizPipeline = nullptr;
    m_layout = nullptr;
    m_sampler = {};
    m_capacity = 0;
    m_initialized = false;
    m_retired.clear();
    m_constants = CullConstants{};
    m_lastFrameBuilt = false;
    m_builtThisFrame = false;
    m_stats = CullerStats{};
}

bool InstanceCuller::createPipelines() {
#if defined(FUSE_VULKAN_BACKEND)
    const u32* cullCode = nullptr;
    usize cullBytes = 0;
    const u32* hizCode = nullptr;
    usize hizBytes = 0;
#if defined(FUSE_CULLING_SLANG)
    if (m_desc.language == CullKernelLanguage::Auto || m_desc.language == CullKernelLanguage::Slang) {
        cullCode = kCullingCullSlangSpv;
        cullBytes = sizeof(kCullingCullSlangSpv);
        hizCode = kCullingHizSlangSpv;
        hizBytes = sizeof(kCullingHizSlangSpv);
        m_language = "slang";
    }
#endif
#if defined(FUSE_CULLING_GLSL)
    if (cullCode == nullptr &&
        (m_desc.language == CullKernelLanguage::Auto || m_desc.language == CullKernelLanguage::Glsl)) {
        cullCode = kCullingCullGlslSpv;
        cullBytes = sizeof(kCullingCullGlslSpv);
        hizCode = kCullingHizGlslSpv;
        hizBytes = sizeof(kCullingHizGlslSpv);
        m_language = "glsl";
    }
#endif
    if (cullCode == nullptr || hizCode == nullptr) {
        return false;
    }
    const VkDevice device = static_cast<VkDevice>(m_desc.device->nativeHandle());
    VkDescriptorSetLayout setLayout = static_cast<VkDescriptorSetLayout>(m_desc.bindless->layoutHandle());
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32u};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = setLayout != VK_NULL_HANDLE ? 1u : 0u;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
        return false;
    }
    m_layout = layout;
    auto build = [&](const u32* code, usize bytes, void*& out) {
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = bytes;
        moduleInfo.pCode = code;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
            return false;
        }
        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.flags = static_cast<VkPipelineCreateFlags>(m_desc.bindless->pipelineCreateFlags());
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.layout = layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        out = pipeline;
        return result == VK_SUCCESS;
    };
    return build(cullCode, cullBytes, m_cullPipeline) && build(hizCode, hizBytes, m_hizPipeline);
#else
    return false;
#endif
}

bool InstanceCuller::createBuffer(OwnedBuffer& out, u64 bytes, bool indirect, bool hostVisible, const char* name) {
    out = OwnedBuffer{};
    BufferDesc desc{};
    desc.size = static_cast<usize>(bytes);
    desc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::TransferDst) |
                                          static_cast<u32>(BufferUsage::TransferSrc) |
                                          static_cast<u32>(BufferUsage::ShaderDeviceAddress) |
                                          (indirect ? static_cast<u32>(BufferUsage::Indirect) : 0u));
    // Host-visible (the constants ring): persistently mapped, HOST_COHERENT (GpuAllocator contract).
    desc.memoryUsage = hostVisible ? MemoryUsage::CpuToGpu : MemoryUsage::GpuOnly;
    desc.name = name;
    if (!m_desc.allocator->createBuffer(desc, out.buffer) || out.buffer.deviceAddress == 0u ||
        (hostVisible && out.buffer.mapped == nullptr)) {
        if (out.buffer.handle != nullptr) {
            m_desc.allocator->destroyBuffer(out.buffer);
        }
        out = OwnedBuffer{};
        return false;
    }
    out.slot = m_desc.bindless->registerBufferSlot(out.buffer, false);
    out.handle = m_desc.bindless->shaderHandle(out.slot);
    return out.slot.isValid();
}

void InstanceCuller::destroyBuffer(OwnedBuffer& buffer) {
    if (buffer.slot.isValid()) {
        m_desc.bindless->unregisterSlot(buffer.slot);
    }
    if (buffer.buffer.handle != nullptr) {
        m_desc.allocator->destroyBuffer(buffer.buffer);
    }
    buffer = OwnedBuffer{};
}

void InstanceCuller::retireBuffer(OwnedBuffer& buffer) {
    if (buffer.buffer.handle == nullptr) {
        return;
    }
    if (m_retired.empty() || m_retired.back().serial != m_frameSerial || m_retired.back().bufferCount == 4u ||
        m_retired.back().image.image != nullptr) {
        m_retired.push_back(Retired{});
        m_retired.back().serial = m_frameSerial;
    }
    Retired& r = m_retired.back();
    r.buffers[r.bufferCount++] = buffer;
    buffer = OwnedBuffer{};
    ++m_stats.retired;
}

bool InstanceCuller::ensureCapacity(u32 instances) {
    if (instances <= m_capacity && m_args.buffer.handle != nullptr) {
        return true;
    }
    u32 capacity = m_capacity > 0u ? m_capacity : 64u;
    while (capacity < instances) {
        capacity *= 2u;
    }
    retireBuffer(m_args);
    retireBuffer(m_candidates);
    retireBuffer(m_results);
    const u64 slotBytes = static_cast<u64>(capacity) * sizeof(u32);
    if (!createBuffer(m_args, 2ull * argsRegionBytes(capacity), true, false, "culling.args") ||
        !createBuffer(m_candidates, slotBytes, false, false, "culling.candidates") ||
        !createBuffer(m_results, slotBytes, false, false, "culling.results")) {
        m_capacity = 0;
        return false;
    }
    m_capacity = capacity;
    m_stats.capacity = capacity;
    ++m_stats.reallocations;
    return true;
}

void InstanceCuller::destroyHiz(bool retire) {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_hiz.image == nullptr) {
        return;
    }
    if (retire) {
        m_retired.push_back(Retired{});
        Retired& r = m_retired.back();
        r.serial = m_frameSerial;
        r.image = m_hiz;
        for (u32 i = 0; i < kMaxHizMips; ++i) {
            r.views[i] = m_hizMipViews[i];
            r.slots[i] = m_hizMipSlots[i];
        }
        r.slots[kMaxHizMips] = m_hizSampledSlot;
        ++m_stats.retired;
    } else {
        const VkDevice device = static_cast<VkDevice>(m_desc.device->nativeHandle());
        for (u32 i = 0; i < kMaxHizMips; ++i) {
            if (m_hizMipSlots[i].isValid()) {
                m_desc.bindless->unregisterSlot(m_hizMipSlots[i]);
            }
            if (m_hizMipViews[i] != nullptr) {
                vkDestroyImageView(device, static_cast<VkImageView>(m_hizMipViews[i]), nullptr);
            }
        }
        if (m_hizSampledSlot.isValid()) {
            m_desc.bindless->unregisterSlot(m_hizSampledSlot);
        }
        m_desc.allocator->destroyImage(m_hiz);
    }
#else
    (void)retire;
#endif
    m_hiz = Texture{};
    for (u32 i = 0; i < kMaxHizMips; ++i) {
        m_hizMipViews[i] = nullptr;
        m_hizMipSlots[i] = {};
    }
    m_hizSampledSlot = {};
    m_hizDim = 0;
    m_hizMipCount = 0;
}

bool InstanceCuller::setResolution(u32 depthWidth, u32 depthHeight) {
    if (!m_initialized) {
        return false;
    }
    const hiz_kernel::HizDims dims = hiz_kernel::hiz_dims(depthWidth, depthHeight);
    if (dims.dim0 == 0u) {
        return false;
    }
    if (depthWidth == m_depthWidth && depthHeight == m_depthHeight && m_hiz.image != nullptr) {
        return true;
    }
#if defined(FUSE_VULKAN_BACKEND)
    destroyHiz(true);
    TextureDesc desc{};
    desc.width = dims.dim0;
    desc.height = dims.dim0;
    desc.mipLevels = dims.mipCount;
    desc.format = GpuFormat::R32Sfloat;
    desc.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::Storage) |
                                         static_cast<u32>(ImageUsage::TransferSrc));
    desc.name = "culling.hiz";
    if (!m_desc.allocator->createImage(desc, m_hiz)) {
        m_hiz = Texture{};
        return false;
    }
    const VkDevice device = static_cast<VkDevice>(m_desc.device->nativeHandle());
    for (u32 mip = 0; mip < dims.mipCount; ++mip) {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = static_cast<VkImage>(m_hiz.image);
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R32_SFLOAT;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.baseMipLevel = mip;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        VkImageView handle = VK_NULL_HANDLE;
        if (vkCreateImageView(device, &view, nullptr, &handle) != VK_SUCCESS) {
            destroyHiz(false);
            return false;
        }
        m_hizMipViews[mip] = handle;
        Texture mipTexture = m_hiz;
        mipTexture.view = handle;
        m_hizMipSlots[mip] = m_desc.bindless->registerTextureSlot(mipTexture, true);
    }
    m_hizSampledSlot = m_desc.bindless->registerTextureSlot(m_hiz, false);
    m_hizDim = dims.dim0;
    m_hizMipCount = dims.mipCount;
    m_depthWidth = depthWidth;
    m_depthHeight = depthHeight;
    m_hizLayout = 0;
    m_hizQueue = rg::kNoQueue;
    m_lastFrameBuilt = false;
    m_builtThisFrame = false;
    ++m_stats.hizRebuilds;
    return m_hizSampledSlot.isValid();
#else
    return false;
#endif
}

bool InstanceCuller::beginFrame(u64 frameSerial, const CullFrameDesc& frame) {
    if (!m_initialized) {
        return false;
    }
    m_frameSerial = frameSerial;
    if (!ensureCapacity(frame.instanceCount)) {
        return false;
    }
    const bool history = m_builtThisFrame && !frame.cameraCut && m_hizDim != 0u;
    m_builtThisFrame = false;
    m_lastFrameBuilt = history;
    m_hizBuilds = 0;

    CullConstants& c = m_constants;
    std::memcpy(c.viewProj, frame.viewProj, sizeof(c.viewProj));
    std::memcpy(c.prevViewProj, m_lastViewProj, sizeof(c.prevViewProj));
    const f32 origin[3] = {0.f, 0.f, 0.f};
    const geometry::cull_kernel::CullView view = geometry::cull_kernel::make_cull_view(frame.viewProj, origin);
    std::memcpy(c.planes, view.planes, sizeof(c.planes));
    for (u32 i = 0; i < 16u; ++i) {
        c.hizMips[i] = i < m_hizMipCount ? m_desc.bindless->shaderHandle(m_hizMipSlots[i]) : 0u;
    }
    c.instanceCount = frame.instanceCount;
    c.flags = (frame.frustum ? static_cast<u32>(kCullFrustum) : 0u) |
              (frame.occlusion && m_hizDim != 0u ? static_cast<u32>(kCullOcclusion) : 0u) |
              (history ? static_cast<u32>(kCullHistoryValid) : 0u);
    c.hizDim = m_hizDim;
    c.hizMipCount = m_hizMipCount;
    const hiz_kernel::HizDims dims = hiz_kernel::hiz_dims(m_depthWidth, m_depthHeight);
    c.hizScale[0] = dims.scaleX;
    c.hizScale[1] = dims.scaleY;
    c.maxDraws = m_capacity;
    c.phase2DrawBase = m_capacity;
    c.argsBuffer = m_args.handle;
    c.countsBuffer = m_counts.handle;
    c.candidatesBuffer = m_candidates.handle;
    c.resultsBuffer = m_results.handle;
    c.hizTexture = m_hizSampledSlot.isValid() ? m_desc.bindless->shaderHandle(m_hizSampledSlot) : 0u;
    c.pointSampler = m_desc.bindless->shaderHandle(m_sampler);

    m_ringSlot = (m_ringSlot + 1u) % m_desc.framesInFlight;
    const u64 offset = static_cast<u64>(m_ringSlot) * kConstantsStride;
    std::memcpy(static_cast<u8*>(m_constantsRing.buffer.mapped) + offset, &c, sizeof(CullConstants));
    m_constantsAddress = m_constantsRing.buffer.deviceAddress + offset;
    std::memcpy(m_lastViewProj, frame.viewProj, sizeof(m_lastViewProj));
    return true;
}

CullGraphRefs InstanceCuller::importInto(rg::Graph& graph) {
    CullGraphRefs refs{};
    if (!m_initialized) {
        return refs;
    }
    const u8 gfx = static_cast<u8>(rg::QueueClass::Graphics);
    auto import = [&](const OwnedBuffer& b, const char* name) {
        return graph.importBuffer(rg::ImportedBuffer{b.buffer.handle, b.buffer.desc.size, gfx, nullptr, name});
    };
    refs.args = import(m_args, "culling.args");
    refs.counts = import(m_counts, "culling.counts");
    refs.candidates = import(m_candidates, "culling.candidates");
    refs.results = import(m_results, "culling.results");
    refs.constants = import(m_constantsRing, "culling.constants");
    if (m_hiz.image != nullptr) {
        rg::ImportedImage image{};
        image.image = m_hiz.image;
        image.view = m_hiz.view;
        image.format = kVkFormatR32Sfloat;
        image.width = m_hizDim;
        image.height = m_hizDim;
        image.mipLevels = m_hizMipCount;
        image.initialLayout = m_hizLayout;
        image.initialQueue = m_hizQueue;
        image.finalLayout = kVkLayoutShaderReadOnly;
        image.layoutTracker = &m_hizLayout;
        image.queueTracker = &m_hizQueue;
        image.name = "culling.hiz";
        refs.hiz = graph.importImage(image);
    }
    return refs;
}

void InstanceCuller::addPhase1(rg::Graph& graph, const CullGraphRefs& refs, const gpu_scene::GpuSceneGraphRefs& scene,
                               u32 sceneHandle) {
    if (!m_initialized) {
        return;
    }
    graph.addPass("cull.reset", &InstanceCuller::recordReset, this)
        .use(refs.counts, rg::Access::TransferDst, rg::BufferRange{0, kCountResetWords * sizeof(u32)});
    CullRecord& record = m_cullRecords[0];
    record.self = this;
    record.push = CullPush{};
    record.push.constants = m_constantsAddress;
    record.push.scene = sceneHandle;
    record.push.phase = 1u;
    record.groups = (m_constants.instanceCount + kCullWorkgroup - 1u) / kCullWorkgroup;
    rg::PassBuilder pass = graph.addPass("cull.phase1", &InstanceCuller::recordPhase1, &record);
    gpu_scene::GpuScene::useAll(pass, scene, rg::Access::StorageRead, rg::kStageCompute);
    pass.use(refs.constants, rg::Access::StorageRead, {}, rg::kStageCompute)
        .use(refs.counts, rg::Access::StorageReadWrite, {}, rg::kStageCompute)
        .use(refs.args, rg::Access::StorageWrite, rg::BufferRange{0, argsRegionBytes(m_capacity)}, rg::kStageCompute)
        .use(refs.candidates, rg::Access::StorageWrite, {}, rg::kStageCompute)
        .use(refs.results, rg::Access::StorageWrite, {}, rg::kStageCompute);
    const u32 needHistory = kCullOcclusion | kCullHistoryValid;
    if ((m_constants.flags & needHistory) == needHistory && refs.hiz.valid()) {
        pass.use(refs.hiz, rg::Access::SampledRead, {}, rg::kStageCompute);
    }
}

void InstanceCuller::addHizBuild(rg::Graph& graph, const CullGraphRefs& refs, rg::TextureRef depth, u32 depthHandle) {
    if (!m_initialized || !refs.hiz.valid() || m_hizBuilds >= kMaxHizBuilds) {
        return;
    }
    HizRecord& record = m_hizRecords[m_hizBuilds++];
    record.self = this;
    record.push = HizPush{};
    record.push.constants = m_constantsAddress;
    record.push.depthTexture = depthHandle;
    record.push.depthSampler = m_desc.bindless->shaderHandle(m_sampler);
    record.push.depthWidth = m_depthWidth;
    record.push.depthHeight = m_depthHeight;
    record.push.groupsX = (m_hizDim + kHizTile - 1u) / kHizTile;
    graph.addPass("cull.hiz", &InstanceCuller::recordHiz, &record)
        .use(depth, rg::Access::SampledRead, {}, rg::kStageCompute)
        .use(refs.hiz, rg::Access::StorageReadWrite, {}, rg::kStageCompute)
        .use(refs.counts, rg::Access::StorageReadWrite, {}, rg::kStageCompute)
        .use(refs.constants, rg::Access::StorageRead, {}, rg::kStageCompute);
    m_builtThisFrame = true;
}

void InstanceCuller::addPhase2(rg::Graph& graph, const CullGraphRefs& refs, const gpu_scene::GpuSceneGraphRefs& scene,
                               u32 sceneHandle) {
    if (!m_initialized || (m_constants.flags & kCullOcclusion) == 0u || !refs.hiz.valid()) {
        return; // no occlusion: phase 1 drew everything, the phase-2 count stays 0
    }
    CullRecord& record = m_cullRecords[1];
    record.self = this;
    record.push = CullPush{};
    record.push.constants = m_constantsAddress;
    record.push.scene = sceneHandle;
    record.push.phase = 2u;
    record.groups = 0;
    rg::PassBuilder pass = graph.addPass("cull.phase2", &InstanceCuller::recordPhase2, &record);
    gpu_scene::GpuScene::useAll(pass, scene, rg::Access::StorageRead, rg::kStageCompute);
    const u64 region = argsRegionBytes(m_capacity);
    pass.use(refs.constants, rg::Access::StorageRead, {}, rg::kStageCompute)
        .use(refs.counts, rg::Access::IndirectRead)
        .use(refs.counts, rg::Access::StorageReadWrite, {}, rg::kStageCompute)
        .use(refs.candidates, rg::Access::StorageRead, {}, rg::kStageCompute)
        .use(refs.results, rg::Access::StorageReadWrite, {}, rg::kStageCompute)
        .use(refs.args, rg::Access::StorageWrite, rg::BufferRange{region, region}, rg::kStageCompute)
        .use(refs.hiz, rg::Access::SampledRead, {}, rg::kStageCompute);
}

void InstanceCuller::useDraws(rg::PassBuilder& pass, const CullGraphRefs& refs, CullPhase phase) const {
    const u64 region = argsRegionBytes(m_capacity);
    pass.use(refs.args, rg::Access::IndirectRead, rg::BufferRange{phase == CullPhase::Phase1 ? 0u : region, region})
        .use(refs.counts, rg::Access::IndirectRead);
}

void InstanceCuller::recordDraws(void* commandBuffer, CullPhase phase) const {
#if defined(FUSE_VULKAN_BACKEND)
    const u64 region = argsRegionBytes(m_capacity);
    const bool second = phase == CullPhase::Phase2;
    vkCmdDrawIndexedIndirectCount(static_cast<VkCommandBuffer>(commandBuffer), static_cast<VkBuffer>(m_args.buffer.handle),
                                  second ? region : 0u, static_cast<VkBuffer>(m_counts.buffer.handle),
                                  (second ? kCountPhase2Draws : kCountPhase1Draws) * sizeof(u32), m_capacity,
                                  sizeof(DrawIndexedIndirectCommand));
#else
    (void)commandBuffer;
    (void)phase;
#endif
}

void InstanceCuller::recordReset(const rg::PassContext& context, void* user) {
#if defined(FUSE_VULKAN_BACKEND)
    const InstanceCuller& self = *static_cast<const InstanceCuller*>(user);
    vkCmdUpdateBuffer(static_cast<VkCommandBuffer>(context.commandBuffer), static_cast<VkBuffer>(self.m_counts.buffer.handle),
                      0, sizeof(kCountResetData), kCountResetData);
#else
    (void)context;
    (void)user;
#endif
}

void InstanceCuller::recordPhase1(const rg::PassContext& context, void* user) {
#if defined(FUSE_VULKAN_BACKEND)
    const CullRecord& record = *static_cast<const CullRecord*>(user);
    const InstanceCuller& self = *record.self;
    if (record.groups == 0u) {
        return;
    }
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, static_cast<VkPipeline>(self.m_cullPipeline));
    self.m_desc.bindless->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, self.m_layout, 0);
    vkCmdPushConstants(cmd, static_cast<VkPipelineLayout>(self.m_layout), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPush),
                       &record.push);
    vkCmdDispatch(cmd, record.groups, 1, 1);
#else
    (void)context;
    (void)user;
#endif
}

void InstanceCuller::recordPhase2(const rg::PassContext& context, void* user) {
#if defined(FUSE_VULKAN_BACKEND)
    const CullRecord& record = *static_cast<const CullRecord*>(user);
    const InstanceCuller& self = *record.self;
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, static_cast<VkPipeline>(self.m_cullPipeline));
    self.m_desc.bindless->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, self.m_layout, 0);
    vkCmdPushConstants(cmd, static_cast<VkPipelineLayout>(self.m_layout), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPush),
                       &record.push);
    vkCmdDispatchIndirect(cmd, static_cast<VkBuffer>(self.m_counts.buffer.handle), kCountDispatchX * sizeof(u32));
#else
    (void)context;
    (void)user;
#endif
}

void InstanceCuller::recordHiz(const rg::PassContext& context, void* user) {
#if defined(FUSE_VULKAN_BACKEND)
    const HizRecord& record = *static_cast<const HizRecord*>(user);
    const InstanceCuller& self = *record.self;
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, static_cast<VkPipeline>(self.m_hizPipeline));
    self.m_desc.bindless->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, self.m_layout, 0);
    vkCmdPushConstants(cmd, static_cast<VkPipelineLayout>(self.m_layout), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(HizPush),
                       &record.push);
    vkCmdDispatch(cmd, record.push.groupsX, record.push.groupsX, 1);
#else
    (void)context;
    (void)user;
#endif
}

u32 InstanceCuller::collectRetired(u64 completedSerial) {
    u32 collected = 0;
#if defined(FUSE_VULKAN_BACKEND)
    usize keep = 0;
    for (usize i = 0; i < m_retired.size(); ++i) {
        Retired& r = m_retired[i];
        if (r.serial > completedSerial) {
            m_retired[keep++] = r;
            continue;
        }
        for (u32 b = 0; b < r.bufferCount; ++b) {
            destroyBuffer(r.buffers[b]);
            ++collected;
        }
        if (r.image.image != nullptr) {
            const VkDevice device = static_cast<VkDevice>(m_desc.device->nativeHandle());
            for (u32 s = 0; s <= kMaxHizMips; ++s) {
                if (r.slots[s].isValid()) {
                    m_desc.bindless->unregisterSlot(r.slots[s]);
                }
            }
            for (u32 v = 0; v < kMaxHizMips; ++v) {
                if (r.views[v] != nullptr) {
                    vkDestroyImageView(device, static_cast<VkImageView>(r.views[v]), nullptr);
                }
            }
            m_desc.allocator->destroyImage(r.image);
            ++collected;
        }
    }
    m_retired.resize(keep);
    m_stats.retired = m_stats.retired >= collected ? m_stats.retired - collected : 0u;
#else
    (void)completedSerial;
#endif
    return collected;
}

} // namespace fuse::renderer::culling
