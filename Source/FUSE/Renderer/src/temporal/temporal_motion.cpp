// WP-4.1 GPU motion vectors: see include/fuse/renderer/temporal/temporal_motion.hpp.
#include <fuse/renderer/temporal/temporal_motion.hpp>

#include "temporal_pipeline.hpp"

#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>

#include <cstring>

namespace fuse::renderer::temporal {

namespace {
constexpr u32 kFrameStride = 512u; ///< ring slot size (>= sizeof(MotionFrameConstants), 256-aligned)
static_assert(sizeof(MotionFrameConstants) <= kFrameStride, "frame ring slot");
} // namespace

TemporalMotion::~TemporalMotion() { destroy(); }

bool TemporalMotion::init(const TemporalMotionDesc& desc) {
    destroy();
    m_desc = desc;
#if defined(FUSE_VULKAN_BACKEND)
    if (desc.allocator == nullptr || desc.bindless == nullptr || desc.framesInFlight == 0u || desc.width == 0u ||
        desc.height == 0u || !queryTemporalCapabilities(desc.device).temporal) {
        return false;
    }
    const detail::KernelCode code = detail::kernelCode(detail::TemporalKernel::Motion, desc.language);
    if (code.words == nullptr) {
        return false;
    }
    m_initialized = true; // destroy() releases partial state from here on
    m_language = code.language;
    m_width = desc.width;
    m_height = desc.height;
    const u64 pixels = static_cast<u64>(desc.width) * desc.height;
    BufferDesc ring{};
    ring.size = static_cast<usize>(desc.framesInFlight) * kFrameStride;
    ring.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                          static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    ring.memoryUsage = MemoryUsage::CpuToGpu;
    ring.name = "temporal_motion.frame";
    BufferDesc out{};
    out.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress) |
                                         static_cast<u32>(BufferUsage::TransferSrc));
    out.memoryUsage = MemoryUsage::GpuOnly;
    out.size = static_cast<usize>(pixels * 8u);
    out.name = "temporal_motion.motion";
    BufferDesc depth = out;
    depth.size = static_cast<usize>(pixels * 4u);
    depth.name = "temporal_motion.depth";
    if (!m_desc.allocator->createBuffer(ring, m_frameRing) || m_frameRing.mapped == nullptr || m_frameRing.deviceAddress == 0u ||
        !m_desc.allocator->createBuffer(out, m_motion) || m_motion.deviceAddress == 0u ||
        !m_desc.allocator->createBuffer(depth, m_depth) || m_depth.deviceAddress == 0u ||
        !detail::createComputePipeline(*desc.device, *desc.bindless, code, m_layoutHandle, m_pipeline)) {
        destroy();
        return false;
    }
    return true;
#else
    return false;
#endif
}

void TemporalMotion::destroy() {
    if (!m_initialized) {
        return;
    }
    Buffer* buffers[3] = {&m_frameRing, &m_motion, &m_depth};
    for (Buffer* b : buffers) {
        if (b->handle != nullptr) {
            m_desc.allocator->destroyBuffer(*b);
        }
        *b = Buffer{};
    }
    detail::destroyComputePipeline(*m_desc.device, m_layoutHandle, m_pipeline);
    m_queues[0] = m_queues[1] = rg::kNoQueue;
    m_frameAddress = 0;
    m_constants = MotionFrameConstants{};
    m_width = m_height = 0;
    m_language = "none";
    m_stats = MotionStats{};
    m_initialized = false;
}

bool TemporalMotion::beginFrame(u64 frameSerial, const MotionFrameDesc& frame) {
    m_stats.motionPasses = 0;
    if (!m_initialized || frame.scene == 0u || frame.vis == 0u) {
        return false;
    }
    MotionFrameConstants& c = m_constants;
    c = MotionFrameConstants{};
    jitter_view_proj(frame.viewProj, frame.jitter_px.x, frame.jitter_px.y, m_width, m_height, c.drawViewProj);
    std::memcpy(c.viewProj, frame.viewProj, sizeof(c.viewProj));
    std::memcpy(c.prevViewProj, frame.prevViewProj, sizeof(c.prevViewProj));
    const bool sky = sky_reprojection(frame.viewProj, frame.prevViewProj, c.skyReproj);
    c.motion = m_motion.deviceAddress;
    c.depth = m_depth.deviceAddress;
    c.width = m_width;
    c.height = m_height;
    c.scene = frame.scene;
    c.vis = frame.vis;
    c.jitterX = frame.jitter_px.x;
    c.jitterY = frame.jitter_px.y;
    c.flags = sky ? kMotionSkyValid : 0u;
    m_stats.skyValid = sky;
    const u64 offset = (frameSerial % m_desc.framesInFlight) * kFrameStride;
    std::memcpy(static_cast<u8*>(m_frameRing.mapped) + offset, &c, sizeof(c));
    m_frameAddress = m_frameRing.deviceAddress + offset;
    return true;
}

MotionGraphRefs TemporalMotion::importInto(rg::Graph& graph) {
    MotionGraphRefs refs{};
    if (!m_initialized) {
        return refs;
    }
    refs.motion = graph.importBuffer(
        rg::ImportedBuffer{m_motion.handle, m_motion.desc.size, m_queues[0], &m_queues[0], "temporal_motion.motion"});
    refs.depth = graph.importBuffer(
        rg::ImportedBuffer{m_depth.handle, m_depth.desc.size, m_queues[1], &m_queues[1], "temporal_motion.depth"});
    return refs;
}

void TemporalMotion::addMotion(rg::Graph& graph, const MotionGraphRefs& refs, rg::TextureRef vis,
                               const gpu_scene::GpuSceneGraphRefs& scene) {
    if (!m_initialized || !refs.motion.valid() || !refs.depth.valid() || !vis.valid() || m_frameAddress == 0u) {
        return;
    }
    m_record = PassRecord{};
    m_record.self = this;
    m_record.push.frame = m_frameAddress;
    rg::PassBuilder pass = graph.addPass("temporal.motion", &TemporalMotion::recordMotion, &m_record);
    pass.use(vis, rg::Access::StorageRead, {}, rg::kStageCompute);
    gpu_scene::GpuScene::useAll(pass, scene, rg::Access::StorageRead, rg::kStageCompute);
    if (scene.indices.valid()) {
        pass.use(scene.indices, rg::Access::StorageRead, {}, rg::kStageCompute);
    }
    pass.use(refs.motion, rg::Access::StorageWrite, {}, rg::kStageCompute)
        .use(refs.depth, rg::Access::StorageWrite, {}, rg::kStageCompute);
    ++m_stats.motionPasses;
}

void TemporalMotion::recordMotion(const rg::PassContext& context, void* user) {
    const PassRecord& r = *static_cast<const PassRecord*>(user);
    const TemporalMotion& self = *r.self;
    detail::dispatch(context.commandBuffer, *self.m_desc.bindless, self.m_layoutHandle, self.m_pipeline, &r.push,
                     (self.m_width + kTileSize - 1u) / kTileSize, (self.m_height + kTileSize - 1u) / kTileSize);
}

} // namespace fuse::renderer::temporal
