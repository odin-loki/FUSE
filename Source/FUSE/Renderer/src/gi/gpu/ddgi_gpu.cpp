// WP-6.1 DDGI on Vulkan (see include/fuse/renderer/gi/gpu/ddgi_gpu.hpp).
#include <fuse/renderer/gi/gpu/ddgi_gpu.hpp>

#include <fuse/compute_kernel/load_scale.hpp>

#include <fuse/renderer/gi/gpu/ddgi_gpu_reference.hpp>
#include <fuse/renderer/rt/rt_caps.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>

#include <algorithm>
#include <cstring>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#include "ddgi_gpu_spv.h"
#endif

namespace fuse::renderer::gi_gpu {

namespace {

constexpr u64 kSectionAlign = 256u;

u64 alignUp(u64 v, u64 a) {
    return (v + a - 1u) / a * a;
}

u32 groupsFor(u32 n, u32 size) {
    return (n + size - 1u) / size;
}

} // namespace

DdgiGpuCapabilities queryDdgiGpuCapabilities(const VulkanDevice* device) {
    DdgiGpuCapabilities caps{};
#if defined(FUSE_VULKAN_BACKEND)
    if (device == nullptr || !device->isValid()) {
        caps.reason = "no device";
        return caps;
    }
    // Buffer device address, int64 and sync2 are part of the T0 set every FUSE device has (WP-0.1).
    caps.compute = true;
    caps.rayQuery = rt::queryRtCapabilities(device).usable;
    caps.reason = "ok";
#else
    (void)device;
    caps.reason = "stub backend";
#endif
    return caps;
}

DdgiWorkLayout DdgiWorkLayout::compute(const DDGIDesc& volume, u32 probeCapacity) {
    DdgiWorkLayout l{};
    const u64 probes = ddgi_util::probeCount(volume);
    const u64 irrTile = static_cast<u64>(volume.irradiance_res + 2u) * (volume.irradiance_res + 2u);
    const u64 distTile = static_cast<u64>(volume.depth_res + 2u) * (volume.depth_res + 2u);
    l.irradianceBytes = probes * irrTile * 3u * sizeof(f32);
    l.distanceBytes = probes * distTile * 2u * sizeof(f32);
    u64 at = 0;
    l.irradiance = at;
    at = alignUp(at + l.irradianceBytes, kSectionAlign);
    l.distance = at;
    at = alignUp(at + l.distanceBytes, kSectionAlign);
    l.updateCounts = at;
    at = alignUp(at + probes * sizeof(u32), kSectionAlign);
    l.rayDirs = at;
    at = alignUp(at + static_cast<u64>(volume.rays_per_probe) * 16u, kSectionAlign);
    l.rays = at;
    at = alignUp(at + static_cast<u64>(probeCapacity) * volume.rays_per_probe * 16u, kSectionAlign);
    l.slotStats = at;
    at = alignUp(at + static_cast<u64>(probeCapacity) * sizeof(u32), kSectionAlign);
    l.probeData = at;
    at = alignUp(at + probes * 16u, kSectionAlign);
    l.bytes = at;
    return l;
}

DdgiGpu::~DdgiGpu() {
    destroy();
}

bool DdgiGpu::init(const DdgiGpuDesc& desc) {
    destroy();
    m_desc = desc;
    const DdgiGpuCapabilities caps = queryDdgiGpuCapabilities(desc.device);
    if (!caps.compute || desc.allocator == nullptr) {
        m_reason = desc.allocator == nullptr && caps.compute ? "no allocator" : caps.reason;
        return false;
    }
    const DDGIDesc& v = desc.volume;
    m_probeCount = ddgi_util::probeCount(v);
    if (m_probeCount == 0u || v.rays_per_probe == 0u || v.irradiance_res == 0u || v.depth_res == 0u ||
        v.irradiance_res > kMaxIrradianceRes || v.depth_res > kMaxDepthRes || v.max_ray_distance <= 0.f ||
        desc.framesInFlight == 0u) {
        m_reason = "unsupported volume (empty grid, no rays, or tile resolution above the blend's shared memory)";
        return false;
    }
    switch (desc.tracer) {
    case DdgiTracer::Auto:
        m_tracer = caps.rayQuery ? DdgiTracer::RayQuery : DdgiTracer::Sdf;
        break;
    case DdgiTracer::RayQuery:
        if (!caps.rayQuery) {
            m_reason = "ray-query tracer needs the T2 gate (rt::queryRtCapabilities)";
            return false;
        }
        m_tracer = DdgiTracer::RayQuery;
        break;
    case DdgiTracer::Sdf:
        m_tracer = DdgiTracer::Sdf;
        break;
    }
    m_probeCapacity = std::min(desc.probeCapacity != 0u ? desc.probeCapacity : std::max(v.probes_per_frame, 1u), m_probeCount);
    m_layout = DdgiWorkLayout::compute(v, m_probeCapacity);

    // Texel directions (the oracle's DdgiCpuVolume::init scratch tables).
    const u32 ir = v.irradiance_res;
    const u32 dr = v.depth_res;
    m_irrTexelDirs.resize(static_cast<usize>(ir) * ir);
    for (u32 y = 0; y < ir; ++y) {
        for (u32 x = 0; x < ir; ++x) {
            m_irrTexelDirs[y * ir + x] = ddgi_cpu::texelDirection(x, y, ir);
        }
    }
    m_distTexelDirs.resize(static_cast<usize>(dr) * dr);
    for (u32 y = 0; y < dr; ++y) {
        for (u32 x = 0; x < dr; ++x) {
            m_distTexelDirs[y * dr + x] = ddgi_cpu::texelDirection(x, y, dr);
        }
    }
    m_schedule.assign(m_probeCapacity, 0u);
    m_seen.assign(m_probeCount, 0u);
    m_sdfObjects.assign(std::max(desc.sdfCapacity, 1u), DdgiSdfObject{});
    m_surfaces.assign(std::max(desc.surfaceCapacity, 1u), DdgiSurface{});
    m_sdfCount = 0u;
    m_surfaceCount = 0u;

#if defined(FUSE_VULKAN_BACKEND)
    GpuAllocator& alloc = *desc.allocator;
    const u32 storage = static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress);
    BufferDesc work{};
    work.size = static_cast<usize>(m_layout.bytes);
    work.usage = static_cast<BufferUsage>(storage | static_cast<u32>(BufferUsage::TransferSrc) |
                                          static_cast<u32>(BufferUsage::TransferDst));
    work.memoryUsage = MemoryUsage::GpuOnly;
    work.name = "ddgi.work";
    BufferDesc dirs{};
    dirs.size = static_cast<usize>((m_irrTexelDirs.size() + m_distTexelDirs.size()) * 16u);
    dirs.usage = static_cast<BufferUsage>(storage);
    dirs.memoryUsage = MemoryUsage::CpuToGpu;
    dirs.name = "ddgi.texel_dirs";
    m_ringSchedule = alignUp(sizeof(DdgiFrameConstants), kSectionAlign);
    m_ringObjects = alignUp(m_ringSchedule + static_cast<u64>(m_probeCapacity) * sizeof(u32), kSectionAlign);
    m_ringSurfaces = alignUp(m_ringObjects + static_cast<u64>(m_sdfObjects.size()) * sizeof(DdgiSdfObject), kSectionAlign);
    m_ringSlotBytes = alignUp(m_ringSurfaces + static_cast<u64>(m_surfaces.size()) * sizeof(DdgiSurface), kSectionAlign);
    BufferDesc ring{};
    ring.size = static_cast<usize>(m_ringSlotBytes * desc.framesInFlight);
    ring.usage = static_cast<BufferUsage>(storage);
    ring.memoryUsage = MemoryUsage::CpuToGpu;
    ring.name = "ddgi.frame_ring";
    if (!alloc.createBuffer(work, m_work) || m_work.deviceAddress == 0u || !alloc.createBuffer(dirs, m_texelDirs) ||
        m_texelDirs.mapped == nullptr || m_texelDirs.deviceAddress == 0u || !alloc.createBuffer(ring, m_ring) ||
        m_ring.mapped == nullptr || m_ring.deviceAddress == 0u) {
        m_reason = "buffer creation failed";
        destroy();
        m_reason = "buffer creation failed";
        return false;
    }
    f32* d = static_cast<f32*>(m_texelDirs.mapped);
    usize k = 0;
    for (const math::Vec3& t : m_irrTexelDirs) {
        d[k++] = t.x;
        d[k++] = t.y;
        d[k++] = t.z;
        d[k++] = 0.f;
    }
    for (const math::Vec3& t : m_distTexelDirs) {
        d[k++] = t.x;
        d[k++] = t.y;
        d[k++] = t.z;
        d[k++] = 0.f;
    }
    if (!createPipelines()) {
        const char* reason = m_reason;
        destroy();
        m_reason = reason;
        return false;
    }
    m_needsReset = true;
    m_ready = true;
    m_reason = "ok";
    return true;
#else
    m_reason = "stub backend";
    return false;
#endif
}

bool DdgiGpu::createPipelines() {
#if defined(FUSE_VULKAN_BACKEND)
    struct Code {
        const u32* words = nullptr;
        usize bytes = 0;
    };
    Code code[kKernelCount] = {};
    const char* name = "none";
    const bool rq = m_tracer == DdgiTracer::RayQuery;
#if defined(FUSE_DDGI_GPU_SLANG)
    if (m_desc.language != DdgiKernelLanguage::Glsl) {
        code[kReset] = {kDdgiResetSlangSpv, sizeof(kDdgiResetSlangSpv)};
        code[kRaygen] = {kDdgiRaygenSlangSpv, sizeof(kDdgiRaygenSlangSpv)};
        code[kTrace] = rq ? Code{kDdgiTraceRqSlangSpv, sizeof(kDdgiTraceRqSlangSpv)}
                          : Code{kDdgiTraceSdfSlangSpv, sizeof(kDdgiTraceSdfSlangSpv)};
        code[kBlend] = {kDdgiBlendSlangSpv, sizeof(kDdgiBlendSlangSpv)};
        code[kProbe] = {kDdgiProbeSlangSpv, sizeof(kDdgiProbeSlangSpv)};
        code[kState] = {kDdgiStateSlangSpv, sizeof(kDdgiStateSlangSpv)};
        name = "slang";
    }
#endif
#if defined(FUSE_DDGI_GPU_GLSL)
    if (code[kReset].words == nullptr && m_desc.language != DdgiKernelLanguage::Slang) {
        code[kReset] = {kDdgiResetGlslSpv, sizeof(kDdgiResetGlslSpv)};
        code[kRaygen] = {kDdgiRaygenGlslSpv, sizeof(kDdgiRaygenGlslSpv)};
        code[kTrace] = rq ? Code{kDdgiTraceRqGlslSpv, sizeof(kDdgiTraceRqGlslSpv)}
                          : Code{kDdgiTraceSdfGlslSpv, sizeof(kDdgiTraceSdfGlslSpv)};
        code[kBlend] = {kDdgiBlendGlslSpv, sizeof(kDdgiBlendGlslSpv)};
        code[kProbe] = {kDdgiProbeGlslSpv, sizeof(kDdgiProbeGlslSpv)};
        code[kState] = {kDdgiStateGlslSpv, sizeof(kDdgiStateGlslSpv)};
        name = "glsl";
    }
#endif
    (void)rq;
    if (code[kReset].words == nullptr) {
        m_reason = "ddgi kernels not built for the requested language";
        return false;
    }
    const VkDevice vk = static_cast<VkDevice>(m_desc.device->nativeHandle());
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DdgiPush)};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(vk, &li, nullptr, &layout) != VK_SUCCESS) {
        m_reason = "vkCreatePipelineLayout failed";
        return false;
    }
    m_layoutHandle = layout;
    for (u32 k = 0; k < kKernelCount; ++k) {
        VkShaderModuleCreateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = code[k].bytes;
        mi.pCode = code[k].words;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(vk, &mi, nullptr, &module) != VK_SUCCESS) {
            m_reason = "vkCreateShaderModule failed";
            return false;
        }
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.flags = m_desc.bindless != nullptr ? static_cast<VkPipelineCreateFlags>(m_desc.bindless->pipelineCreateFlags()) : 0u;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result = vkCreateComputePipelines(vk, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
        vkDestroyShaderModule(vk, module, nullptr);
        if (result != VK_SUCCESS) {
            m_reason = "vkCreateComputePipelines failed";
            return false;
        }
        m_pipelines[k] = pipeline;
    }
    m_language = name;
    return true;
#else
    m_reason = "stub backend";
    return false;
#endif
}

void DdgiGpu::destroy() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_desc.device != nullptr) {
        const VkDevice vk = static_cast<VkDevice>(m_desc.device->nativeHandle());
        for (void*& p : m_pipelines) {
            if (p != nullptr) {
                vkDestroyPipeline(vk, static_cast<VkPipeline>(p), nullptr);
            }
            p = nullptr;
        }
        if (m_layoutHandle != nullptr) {
            vkDestroyPipelineLayout(vk, static_cast<VkPipelineLayout>(m_layoutHandle), nullptr);
        }
    }
    if (m_desc.allocator != nullptr) {
        if (m_work.handle != nullptr) {
            m_desc.allocator->destroyBuffer(m_work);
        }
        if (m_texelDirs.handle != nullptr) {
            m_desc.allocator->destroyBuffer(m_texelDirs);
        }
        if (m_ring.handle != nullptr) {
            m_desc.allocator->destroyBuffer(m_ring);
        }
    }
#endif
    for (void*& p : m_pipelines) {
        p = nullptr;
    }
    m_layoutHandle = nullptr;
    m_work = Buffer{};
    m_texelDirs = Buffer{};
    m_ring = Buffer{};
    m_ready = false;
    m_frameBegun = false;
    m_language = "none";
    m_reason = "not initialised";
    m_recordCount = 0;
    m_frameAddress = 0;
}

bool DdgiGpu::setSdfScene(const compute::SdfObject* objects, u32 objectCount, const DdgiSurface* surfaces, u32 surfaceCount) {
    static_assert(sizeof(compute::SdfObject) == sizeof(DdgiSdfObject), "DdgiSdfObject mirrors compute::SdfObject");
    if (objectCount > m_sdfObjects.size() || surfaceCount > m_surfaces.size() || (objectCount != 0u && objects == nullptr) ||
        (surfaceCount != 0u && surfaces == nullptr)) {
        return false;
    }
    for (u32 i = 0; i < objectCount; ++i) {
        const compute::SdfObject& o = objects[i];
        DdgiSdfObject& g = m_sdfObjects[i];
        g.position[0] = o.position.x;
        g.position[1] = o.position.y;
        g.position[2] = o.position.z;
        g.params[0] = o.params.x;
        g.params[1] = o.params.y;
        g.params[2] = o.params.z;
        g.type = o.type;
        g.alpha = o.alpha;
        g.materialId = o.material_id;
        g.rounding = o.rounding;
    }
    for (u32 i = 0; i < surfaceCount; ++i) {
        m_surfaces[i] = surfaces[i];
    }
    m_sdfCount = objectCount;
    m_surfaceCount = surfaceCount;
    return true;
}

bool DdgiGpu::beginFrame(u64 frameSerial, const DdgiFrameDesc& frame) {
    m_frameBegun = false;
    m_recordCount = 0;
    m_stats = DdgiFrameStats{};
    if (!m_ready) {
        return false;
    }
    if (frame.update && m_tracer == DdgiTracer::RayQuery && (frame.tlasAddress == 0u || frame.sceneAddress == 0u)) {
        m_reason = "ray-query tracer: the frame has no TLAS / scene address";
        return false;
    }
    m_frameSerial = frameSerial;
    // Schedule: explicit (deduplicated, clamped) or the oracle's rolling window.
    u32 scheduled = 0;
    if (frame.update) {
        if (frame.probes != nullptr && frame.probeCount != 0u) {
            for (u32 i = 0; i < frame.probeCount && scheduled < m_probeCapacity; ++i) {
                const u32 p = frame.probes[i];
                if (p >= m_probeCount || m_seen[p] != 0u) {
                    ++m_stats.duplicatesDropped;
                    continue;
                }
                m_seen[p] = 1u;
                m_schedule[scheduled++] = p;
            }
            for (u32 i = 0; i < scheduled; ++i) {
                m_seen[m_schedule[i]] = 0u;
            }
        } else {
            // The oracle's rolling budget (DdgiCpuVolume::update): probes_per_frame x LoadScale::probes.
            const u32 budget = kernel::scaled_count(m_desc.volume.probes_per_frame, kernel::load_scale().probes);
            // Same window start as the oracle (frameIndex x budget); the count is clamped to the capacity.
            ddgi_util::scheduleProbeUpdates(frame.frameIndex, m_probeCount, budget, m_schedule.data(), m_probeCapacity, &scheduled);
        }
    }
    m_constants = makeFrameConstants(m_desc.volume, m_desc.config, m_desc.tuning, frame, scheduled);
    const u64 slot = frameSerial % m_desc.framesInFlight;
    const u64 base = slot * m_ringSlotBytes;
    const u64 ring = m_ring.deviceAddress + base;
    const u64 work = m_work.deviceAddress;
    m_constants.volume.irradiance = work + m_layout.irradiance;
    m_constants.volume.distance = work + m_layout.distance;
    m_constants.volume.probeData = probeStatesEnabled() ? work + m_layout.probeData : 0u;
    m_constants.schedule = ring + m_ringSchedule;
    m_constants.rayDirs = work + m_layout.rayDirs;
    m_constants.rays = work + m_layout.rays;
    m_constants.updateCounts = work + m_layout.updateCounts;
    m_constants.irradianceTexelDirs = m_texelDirs.deviceAddress;
    m_constants.distanceTexelDirs = m_texelDirs.deviceAddress + static_cast<u64>(m_irrTexelDirs.size()) * 16u;
    m_constants.tlas = m_tracer == DdgiTracer::RayQuery ? frame.tlasAddress : 0u;
    m_constants.scene = m_tracer == DdgiTracer::RayQuery ? frame.sceneAddress : 0u;
    m_constants.sdfObjects = ring + m_ringObjects;
    m_constants.sdfSurfaces = ring + m_ringSurfaces;
    m_constants.sdfCount = m_sdfCount;
    m_constants.sdfSurfaceCount = m_surfaceCount;
    m_constants.slotStats = work + m_layout.slotStats;
    u8* dst = static_cast<u8*>(m_ring.mapped) + base;
    std::memcpy(dst, &m_constants, sizeof(m_constants));
    if (scheduled != 0u) {
        std::memcpy(dst + m_ringSchedule, m_schedule.data(), static_cast<usize>(scheduled) * sizeof(u32));
    }
    if (m_sdfCount != 0u) {
        std::memcpy(dst + m_ringObjects, m_sdfObjects.data(), static_cast<usize>(m_sdfCount) * sizeof(DdgiSdfObject));
    }
    if (m_surfaceCount != 0u) {
        std::memcpy(dst + m_ringSurfaces, m_surfaces.data(), static_cast<usize>(m_surfaceCount) * sizeof(DdgiSurface));
    }
    m_frameAddress = ring;
    m_stats.scheduled = scheduled;
    m_frameBegun = true;
    return true;
}

DdgiGraphRefs DdgiGpu::importInto(rg::Graph& graph) {
    DdgiGraphRefs refs{};
    if (!m_ready) {
        return refs;
    }
    refs.work = graph.importBuffer(rg::ImportedBuffer{m_work.handle, m_work.desc.size, rg::kNoQueue, nullptr, "ddgi.work"});
    return refs;
}

DdgiGpu::PassRecord* DdgiGpu::nextRecord() {
    if (m_recordCount >= kMaxPasses) {
        return nullptr;
    }
    PassRecord* r = &m_records[m_recordCount++];
    *r = PassRecord{};
    r->self = this;
    r->push.frame = m_frameAddress;
    return r;
}

bool DdgiGpu::addUpdate(rg::Graph& graph, const DdgiGraphRefs& refs, const rt::RtGraphRefs* rtRefs,
                        const gpu_scene::GpuSceneGraphRefs* sceneRefs) {
    if (!m_ready || !m_frameBegun || !refs.work.valid()) {
        return false;
    }
    if (m_needsReset) {
        PassRecord* r = nextRecord();
        if (r == nullptr) {
            return false;
        }
        const u32 irr = m_probeCount * (m_desc.volume.irradiance_res + 2u) * (m_desc.volume.irradiance_res + 2u);
        const u32 dist = m_probeCount * (m_desc.volume.depth_res + 2u) * (m_desc.volume.depth_res + 2u);
        r->kernel = kReset;
        r->push.count = std::max(std::max(irr, dist), m_probeCount);
        r->groups[0] = groupsFor(r->push.count, kWorkgroup);
        graph.addPass("ddgi.reset", &DdgiGpu::recordDispatch, r).use(refs.work, rg::Access::StorageWrite, {}, rg::kStageCompute);
        m_needsReset = false;
        m_stats.reset = true;
        ++m_stats.passes;
    }
    const u32 scheduled = m_constants.scheduled;
    if (scheduled == 0u) {
        return true;
    }
    if (m_tracer == DdgiTracer::RayQuery && (rtRefs == nullptr || !rtRefs->tlas.valid() || sceneRefs == nullptr)) {
        m_reason = "ray-query tracer: addUpdate needs the frame's rt and scene refs";
        return false;
    }
    if (m_recordCount + 3u > kMaxPasses) {
        return false;
    }
    PassRecord* raygen = nextRecord();
    raygen->kernel = kRaygen;
    raygen->groups[0] = groupsFor(m_desc.volume.rays_per_probe, kWorkgroup);
    graph.addPass("ddgi.raygen", &DdgiGpu::recordDispatch, raygen).use(refs.work, rg::Access::StorageReadWrite, {}, rg::kStageCompute);

    PassRecord* trace = nextRecord();
    trace->kernel = kTrace;
    trace->groups[0] = groupsFor(m_desc.volume.rays_per_probe, kWorkgroup);
    trace->groups[1] = scheduled;
    rg::PassBuilder tracePass = graph.addPass("ddgi.trace", &DdgiGpu::recordDispatch, trace);
    tracePass.use(refs.work, rg::Access::StorageReadWrite, {}, rg::kStageCompute);
    if (m_tracer == DdgiTracer::RayQuery) {
        tracePass.use(rtRefs->tlas, rg::Access::AccelerationStructureRead, {}, rg::kStageCompute);
        gpu_scene::GpuScene::useAll(tracePass, *sceneRefs, rg::Access::StorageRead, rg::kStageCompute);
        if (sceneRefs->indices.valid()) {
            tracePass.use(sceneRefs->indices, rg::Access::StorageRead, {}, rg::kStageCompute);
        }
    }

    PassRecord* blend = nextRecord();
    blend->kernel = kBlend;
    blend->groups[0] = scheduled;
    graph.addPass("ddgi.blend", &DdgiGpu::recordDispatch, blend).use(refs.work, rg::Access::StorageReadWrite, {}, rg::kStageCompute);
    m_stats.passes += 3u;

    if (probeStatesEnabled()) {
        // After the blend (which reads the states from before this update), from this update's rays.
        PassRecord* state = nextRecord();
        if (state == nullptr) {
            return false;
        }
        state->kernel = kState;
        state->groups[0] = groupsFor(scheduled, kWorkgroup);
        graph.addPass("ddgi.state", &DdgiGpu::recordDispatch, state).use(refs.work, rg::Access::StorageReadWrite, {}, rg::kStageCompute);
        ++m_stats.passes;
        m_stats.probeStates = true;
    }
    return true;
}

bool DdgiGpu::addProbe(rg::Graph& graph, const DdgiGraphRefs& refs, rg::BufferRef points, u64 pointsAddress, rg::BufferRef out,
                       u64 outAddress, u32 count) {
    if (!m_ready || !m_frameBegun || !refs.work.valid() || count == 0u || pointsAddress == 0u || outAddress == 0u) {
        return false;
    }
    PassRecord* r = nextRecord();
    if (r == nullptr) {
        return false;
    }
    r->kernel = kProbe;
    r->push.aux = pointsAddress;
    r->push.out = outAddress;
    r->push.count = count;
    r->groups[0] = groupsFor(count, kWorkgroup);
    rg::PassBuilder pass = graph.addPass("ddgi.probe", &DdgiGpu::recordDispatch, r);
    pass.use(refs.work, rg::Access::StorageRead, {}, rg::kStageCompute);
    if (points.valid()) {
        pass.use(points, rg::Access::StorageRead, {}, rg::kStageCompute);
    }
    if (out.valid()) {
        pass.use(out, rg::Access::StorageWrite, {}, rg::kStageCompute);
    }
    ++m_stats.passes;
    return true;
}

void DdgiGpu::addSamplingUse(rg::Graph& graph, const DdgiGraphRefs& refs, u8 stages) {
    if (!m_ready || !refs.work.valid()) {
        return;
    }
    graph.addPass("ddgi.sample_use", nullptr, nullptr).use(refs.work, rg::Access::StorageRead, {}, stages).neverCull();
}

void DdgiGpu::recordDispatch(const rg::PassContext& context, void* user) {
#if defined(FUSE_VULKAN_BACKEND)
    const PassRecord& r = *static_cast<const PassRecord*>(user);
    const DdgiGpu& self = *r.self;
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(context.commandBuffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, static_cast<VkPipeline>(self.m_pipelines[r.kernel]));
    vkCmdPushConstants(cmd, static_cast<VkPipelineLayout>(self.m_layoutHandle), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DdgiPush), &r.push);
    vkCmdDispatch(cmd, r.groups[0], r.groups[1], r.groups[2]);
#else
    (void)context;
    (void)user;
#endif
}

} // namespace fuse::renderer::gi_gpu
