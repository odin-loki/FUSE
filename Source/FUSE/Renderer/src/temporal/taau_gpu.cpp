// WP-4.1 TAAU Vulkan dispatch: see include/fuse/renderer/temporal/taau_gpu.hpp.
#include <fuse/renderer/temporal/taau_gpu.hpp>

#include "temporal_pipeline.hpp"

#include <fuse/math/mat.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>

#include <cmath>
#include <cstring>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer::temporal {

namespace {
constexpr u32 kFrameStride = 256u; ///< ring slot size (>= sizeof(TaauFrameConstants), 256-aligned)
static_assert(sizeof(TaauFrameConstants) <= kFrameStride, "frame ring slot");
constexpr u32 kOutputFormat = static_cast<u32>(GpuFormat::R16G16B16A16Sfloat);

u32 groups(u32 items) { return (items + kTileSize - 1u) / kTileSize; }

bool finiteIn(f32 v, f32 lo, f32 hi) { return std::isfinite(v) && v >= lo && v <= hi; }
} // namespace

TemporalCapabilities queryTemporalCapabilities(const VulkanDevice* device) {
    TemporalCapabilities c{};
#if defined(FUSE_VULKAN_BACKEND)
    if (device == nullptr || !device->isValid()) {
        return c;
    }
    const RendererCaps& caps = device->info().caps;
    if (!caps.bufferDeviceAddress || !caps.shaderInt64) {
        c.reason = "bufferDeviceAddress / shaderInt64 missing";
        return c;
    }
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(static_cast<VkPhysicalDevice>(device->nativePhysicalDevice()),
                                        static_cast<VkFormat>(kOutputFormat), &props);
    if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0u) {
        c.reason = "RGBA16F storage images unsupported";
        return c;
    }
    c.temporal = true;
    c.reason = "ok";
#else
    (void)device;
    c.reason = "stub backend";
#endif
    return c;
}

void taau_camera_terms(const UpscaleCamera& camera, const UpscaleCamera& previousCamera, taau_kernel::Params& p) {
    // taau.cpp (TaauUpscaler::upscale + currentToPreviousView), same expressions in the same order.
    if (!(camera.vertical_fov_rad > 0.f && camera.aspect > 0.f)) {
        return;
    }
    math::Mat4 inv{};
    if (!math::tryInverseAffine(camera.view, inv)) {
        return;
    }
    const math::Mat4 m = previousCamera.view * inv;
    for (u32 row = 0; row < 3u; ++row) {
        for (u32 col = 0; col < 4u; ++col) {
            p.cur_to_prev_view[row * 4u + col] = m.at(row, col);
        }
    }
    p.has_camera = 1u;
    p.tan_half_y = std::tan(0.5f * camera.vertical_fov_rad);
    p.tan_half_x = p.tan_half_y * camera.aspect;
}

void pack_taau_constants(const taau_kernel::Params& p, TaauFrameConstants& c) {
    c.renderW = p.render_w;
    c.renderH = p.render_h;
    c.displayW = p.display_w;
    c.displayH = p.display_h;
    c.jitterX = p.jitter_px.x;
    c.jitterY = p.jitter_px.y;
    c.exposure = p.exposure;
    c.historyValid = p.history_valid;
    c.hasCamera = p.has_camera;
    c.hasPrev = (!p.prev_depth.empty() || !p.prev_motion.empty()) ? 1u : 0u;
    c.tanHalfX = p.tan_half_x;
    c.tanHalfY = p.tan_half_y;
    std::memcpy(c.curToPrevView, p.cur_to_prev_view, sizeof(c.curToPrevView));
    const taau_kernel::Settings& s = p.settings;
    c.maxAccumulation = s.max_accumulation;
    c.accumulationMotionFalloff = s.accumulation_motion_falloff;
    c.clampGamma = s.clamp_gamma;
    c.depthRejection = s.depth_rejection;
    c.velocityRejectionPx = s.velocity_rejection_px;
    c.clipFullMotionPx = s.clip_full_motion_px;
    c.staticClipStrength = s.static_clip_strength;
    c.spatialWeight = s.spatial_weight;
    c.sampleKernelScale = s.sample_kernel_scale;
    c.reactiveStrength = s.reactive_strength;
    c.transparencyClip = s.transparency_clip;
    c.historyFilter = s.history_filter;
    c.dilateMotion = s.dilate_motion;
    c.dilateDepthThreshold = s.dilate_depth_threshold;
}

TaauGpu::~TaauGpu() { destroy(); }

bool TaauGpu::init(const TaauGpuDesc& desc) {
    destroy();
    m_desc = desc;
#if defined(FUSE_VULKAN_BACKEND)
    if (desc.allocator == nullptr || desc.bindless == nullptr || desc.framesInFlight == 0u || !desc.resolution.valid() ||
        !queryTemporalCapabilities(desc.device).temporal) {
        return false;
    }
    const detail::KernelCode code = detail::kernelCode(detail::TemporalKernel::Taau, desc.language);
    if (code.words == nullptr) {
        return false;
    }
    m_initialized = true; // destroy() releases partial state from here on
    m_language = code.language;
    m_settings = desc.settings;
    m_retired.reserve(4);
    BufferDesc ring{};
    ring.size = static_cast<usize>(desc.framesInFlight) * kFrameStride;
    ring.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                          static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    ring.memoryUsage = MemoryUsage::CpuToGpu;
    ring.name = "taau_gpu.frame";
    if (!m_desc.allocator->createBuffer(ring, m_frameRing) || m_frameRing.mapped == nullptr || m_frameRing.deviceAddress == 0u ||
        !detail::createComputePipeline(*desc.device, *desc.bindless, code, m_layoutHandle, m_pipeline) ||
        !createResources(m_res, desc.resolution)) {
        destroy();
        return false;
    }
    m_resolution = desc.resolution;
    m_stats.rebuilds = 1u;
    return true;
#else
    return false;
#endif
}

void TaauGpu::destroy() {
    if (!m_initialized) {
        return;
    }
    collectRetired(~0ull);
    destroyResources(m_res);
    if (m_frameRing.handle != nullptr) {
        m_desc.allocator->destroyBuffer(m_frameRing);
    }
    detail::destroyComputePipeline(*m_desc.device, m_layoutHandle, m_pipeline);
    m_frameRing = Buffer{};
    m_frameAddress = 0;
    m_retired.clear();
    m_resolution = UpscaleResolution{};
    m_current = 0;
    m_valid = false;
    m_frameHistory = false;
    m_begun = false;
    m_language = "none";
    m_stats = TaauGpuStats{};
    m_initialized = false;
}

bool TaauGpu::createResources(Resources& r, const UpscaleResolution& res) {
    r = Resources{};
    const u64 rn = static_cast<u64>(res.render_width) * res.render_height;
    const u64 dn = static_cast<u64>(res.display_width) * res.display_height;
    BufferDesc b{};
    b.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress) |
                                       static_cast<u32>(BufferUsage::TransferSrc) | static_cast<u32>(BufferUsage::TransferDst));
    b.memoryUsage = MemoryUsage::GpuOnly;
    bool ok = true;
    for (u32 i = 0; i < 2u && ok; ++i) {
        b.size = static_cast<usize>(dn * 16u);
        b.name = i == 0u ? "taau_gpu.history0" : "taau_gpu.history1";
        ok = m_desc.allocator->createBuffer(b, r.history[i]) && r.history[i].deviceAddress != 0u;
    }
    if (ok) {
        b.size = static_cast<usize>(rn * 4u);
        b.name = "taau_gpu.prev_depth";
        ok = m_desc.allocator->createBuffer(b, r.prevDepth) && r.prevDepth.deviceAddress != 0u;
    }
    if (ok) {
        b.size = static_cast<usize>(rn * 8u);
        b.name = "taau_gpu.prev_motion";
        ok = m_desc.allocator->createBuffer(b, r.prevMotion) && r.prevMotion.deviceAddress != 0u;
    }
    if (ok) {
        TextureDesc d{};
        d.width = res.display_width;
        d.height = res.display_height;
        d.format = static_cast<GpuFormat>(kOutputFormat);
        d.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Storage) | static_cast<u32>(ImageUsage::Sampled) |
                                          static_cast<u32>(ImageUsage::TransferSrc));
        d.name = "taau_gpu.output";
        ok = m_desc.allocator->createImage(d, r.output);
        if (ok) {
            r.outputSlot = m_desc.bindless->registerTextureSlot(r.output, true);
            ok = r.outputSlot.isValid();
        }
    }
    if (!ok) {
        destroyResources(r);
    }
    return ok;
}

void TaauGpu::destroyResources(Resources& r) {
    if (r.outputSlot.isValid()) {
        m_desc.bindless->unregisterSlot(r.outputSlot);
    }
    for (Buffer& b : r.history) {
        if (b.handle != nullptr) {
            m_desc.allocator->destroyBuffer(b);
        }
    }
    if (r.prevDepth.handle != nullptr) {
        m_desc.allocator->destroyBuffer(r.prevDepth);
    }
    if (r.prevMotion.handle != nullptr) {
        m_desc.allocator->destroyBuffer(r.prevMotion);
    }
    if (r.output.image != nullptr) {
        m_desc.allocator->destroyImage(r.output);
    }
    r = Resources{};
}

u32 TaauGpu::collectRetired(u64 completedSerial) {
    u32 collected = 0;
    usize keep = 0;
    for (usize i = 0; i < m_retired.size(); ++i) {
        if (m_retired[i].serial > completedSerial) {
            m_retired[keep++] = m_retired[i];
            continue;
        }
        destroyResources(m_retired[i].res);
        ++collected;
    }
    m_retired.resize(keep);
    m_stats.retired = static_cast<u32>(m_retired.size());
    return collected;
}

u32 TaauGpu::outputStorageHandle() const {
    return m_res.outputSlot.isValid() ? m_desc.bindless->shaderHandle(m_res.outputSlot) : 0u;
}

bool TaauGpu::beginFrame(u64 frameSerial, const TaauGpuFrameDesc& frame) {
    m_begun = false;
    m_stats.resolvePasses = 0;
    m_stats.historyUsed = false;
    if (!m_initialized || !frame.resolution.valid() || frame.color == 0u || frame.depth == 0u || frame.motion == 0u ||
        !std::isfinite(frame.exposure) || !(frame.exposure > 0.f) || !finiteIn(frame.jitter_px.x, -0.5f, 0.5f) ||
        !finiteIn(frame.jitter_px.y, -0.5f, 0.5f)) {
        return false;
    }
    m_frameSerial = frameSerial;
    const UpscaleResolution& r = frame.resolution;
    if (r.render_width != m_resolution.render_width || r.render_height != m_resolution.render_height ||
        r.display_width != m_resolution.display_width || r.display_height != m_resolution.display_height) {
        Resources fresh{};
        if (!createResources(fresh, r)) {
            return false;
        }
        Retired old{};
        old.res = m_res;
        old.serial = frameSerial;
        m_retired.push_back(old);
        m_res = fresh;
        m_resolution = r;
        m_current = 0;
        m_valid = false;
        ++m_stats.rebuilds;
        m_stats.retired = static_cast<u32>(m_retired.size());
    }
    if (frame.reset_history) {
        m_valid = false;
    }

    // The CPU driver's Params (taau.cpp), minus the spans.
    taau_kernel::Params p{};
    p.render_w = r.render_width;
    p.render_h = r.render_height;
    p.display_w = r.display_width;
    p.display_h = r.display_height;
    p.jitter_px = frame.jitter_px;
    p.exposure = frame.exposure;
    p.history_valid = m_valid ? 1u : 0u;
    p.settings = m_settings;
    taau_camera_terms(frame.camera, frame.previous_camera, p);

    TaauFrameConstants c{};
    pack_taau_constants(p, c);
    c.hasPrev = m_valid ? 1u : 0u; // the CPU driver hands prev_depth / prev_motion only with a valid history
    c.depth = frame.depth;
    c.motion = frame.motion;
    c.prevDepth = m_res.prevDepth.deviceAddress;
    c.prevMotion = m_res.prevMotion.deviceAddress;
    c.historyIn = m_res.history[m_current].deviceAddress;
    c.historyOut = m_res.history[m_current ^ 1u].deviceAddress;
    c.color = frame.color;
    c.reactive = frame.reactive;
    c.transparency = frame.transparency;
    c.output = outputStorageHandle();
    const u64 offset = (frameSerial % m_desc.framesInFlight) * kFrameStride;
    std::memcpy(static_cast<u8*>(m_frameRing.mapped) + offset, &c, sizeof(c));
    m_frameAddress = m_frameRing.deviceAddress + offset;
    m_frameHistory = m_valid;
    m_begun = true;
    return true;
}

TaauGraphRefs TaauGpu::importInto(rg::Graph& graph) {
    TaauGraphRefs refs{};
    if (!m_initialized || m_res.output.image == nullptr) {
        return refs;
    }
    Buffer* buffers[4] = {&m_res.history[0], &m_res.history[1], &m_res.prevDepth, &m_res.prevMotion};
    const char* names[4] = {"taau_gpu.history0", "taau_gpu.history1", "taau_gpu.prev_depth", "taau_gpu.prev_motion"};
    rg::BufferRef out[4];
    for (u32 i = 0; i < 4u; ++i) {
        out[i] = graph.importBuffer(
            rg::ImportedBuffer{buffers[i]->handle, buffers[i]->desc.size, m_res.queues[i], &m_res.queues[i], names[i]});
    }
    refs.history[0] = out[0];
    refs.history[1] = out[1];
    refs.prevDepth = out[2];
    refs.prevMotion = out[3];
    rg::ImportedImage i{};
    i.image = m_res.output.image;
    i.view = m_res.output.view;
    i.format = kOutputFormat;
    i.width = m_resolution.display_width;
    i.height = m_resolution.display_height;
    i.initialLayout = m_res.outputLayout;
    i.initialQueue = m_res.outputQueue;
    i.layoutTracker = &m_res.outputLayout;
    i.queueTracker = &m_res.outputQueue;
    i.name = "taau_gpu.output";
    refs.output = graph.importImage(i);
    return refs;
}

void TaauGpu::addResolve(rg::Graph& graph, const TaauGraphRefs& refs, const TaauGpuInputs& inputs, rg::BufferRef dump,
                         u64 dumpAddress, u64 dumpOffset) {
    if (!m_initialized || !m_begun || !refs.output.valid() || !refs.prevDepth.valid() || !inputs.color.valid() ||
        !inputs.depth.valid() || !inputs.motion.valid()) {
        return;
    }
    m_begun = false;
    const u64 rn = static_cast<u64>(m_resolution.render_width) * m_resolution.render_height;
    const u64 dn = static_cast<u64>(m_resolution.display_width) * m_resolution.display_height;
    const u32 next = m_current ^ 1u;
    m_record = PassRecord{};
    m_record.self = this;
    m_record.push.frame = m_frameAddress;
    m_record.push.out = dump.valid() ? dumpAddress : 0u;
    m_record.groups[0] = groups(m_resolution.display_width);
    m_record.groups[1] = groups(m_resolution.display_height);
    m_record.depth = inputs.depth;
    m_record.motion = inputs.motion;
    m_record.prevDepth = refs.prevDepth;
    m_record.prevMotion = refs.prevMotion;
    m_record.depthBytes = rn * 4u;
    m_record.motionBytes = rn * 8u;
    {
        rg::PassBuilder pass = graph.addPass("taau.resolve", &TaauGpu::recordResolve, &m_record);
        pass.use(inputs.color, rg::Access::SampledRead, {}, rg::kStageCompute);
        if (inputs.reactive.valid()) {
            pass.use(inputs.reactive, rg::Access::SampledRead, {}, rg::kStageCompute);
        }
        if (inputs.transparency.valid()) {
            pass.use(inputs.transparency, rg::Access::SampledRead, {}, rg::kStageCompute);
        }
        pass.use(inputs.depth, rg::Access::StorageRead, rg::BufferRange{0, rn * 4u}, rg::kStageCompute)
            .use(inputs.motion, rg::Access::StorageRead, rg::BufferRange{0, rn * 8u}, rg::kStageCompute)
            .use(refs.history[m_current], rg::Access::StorageRead, {}, rg::kStageCompute)
            .use(refs.history[next], rg::Access::StorageWrite, {}, rg::kStageCompute)
            .use(refs.output, rg::Access::StorageWrite, {}, rg::kStageCompute);
        if (m_frameHistory) {
            pass.use(refs.prevDepth, rg::Access::StorageRead, {}, rg::kStageCompute)
                .use(refs.prevMotion, rg::Access::StorageRead, {}, rg::kStageCompute);
        }
        if (dump.valid()) {
            pass.use(dump, rg::Access::StorageWrite, rg::BufferRange{dumpOffset, dn * 16u}, rg::kStageCompute);
        }
    }
    graph.addPass("taau.keep", &TaauGpu::recordKeep, &m_record)
        .use(inputs.depth, rg::Access::TransferSrc, rg::BufferRange{0, rn * 4u})
        .use(inputs.motion, rg::Access::TransferSrc, rg::BufferRange{0, rn * 8u})
        .use(refs.prevDepth, rg::Access::TransferDst)
        .use(refs.prevMotion, rg::Access::TransferDst);
    m_stats.historyUsed = m_frameHistory;
    ++m_stats.resolvePasses;
    // TaauUpscaler::upscale: the written history becomes current and valid.
    m_current = next;
    m_valid = true;
}

void TaauGpu::recordResolve(const rg::PassContext& context, void* user) {
    const PassRecord& r = *static_cast<const PassRecord*>(user);
    const TaauGpu& self = *r.self;
    detail::dispatch(context.commandBuffer, *self.m_desc.bindless, self.m_layoutHandle, self.m_pipeline, &r.push, r.groups[0],
                     r.groups[1]);
}

void TaauGpu::recordKeep(const rg::PassContext& context, void* user) {
#if defined(FUSE_VULKAN_BACKEND)
    const PassRecord& r = *static_cast<const PassRecord*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    const VkBufferCopy depth{0, 0, r.depthBytes};
    const VkBufferCopy motion{0, 0, r.motionBytes};
    vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(context.buffer(r.depth)), static_cast<VkBuffer>(context.buffer(r.prevDepth)), 1,
                    &depth);
    vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(context.buffer(r.motion)), static_cast<VkBuffer>(context.buffer(r.prevMotion)), 1,
                    &motion);
#else
    (void)context;
    (void)user;
#endif
}

} // namespace fuse::renderer::temporal
