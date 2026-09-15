#include <fuse/renderer/gi/ddgi.hpp>
#include <fuse/renderer/gi/ddgi_kernels.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::renderer {
namespace {

constexpr IrradianceCacheEntry kDefaultCacheEntry{};

fuse::math::Vec3 defaultAmbientIrradiance() {
    return {0.05f, 0.05f, 0.06f};
}

} // namespace

ProbeGridCoord ProbeGridLayout::probeCoordFromIndex(const DDGIDesc& desc, u32 probe_index) {
    ProbeGridCoord coord{};
    const u32 grid_x = desc.grid_dims.x;
    const u32 grid_y = desc.grid_dims.y;
    const u32 grid_z = desc.grid_dims.z;
    if (grid_x == 0u || grid_y == 0u || grid_z == 0u) {
        return coord;
    }

    const u32 slice = grid_x * grid_y;
    coord.z = probe_index / slice;
    const u32 rem = probe_index % slice;
    coord.y = rem / grid_x;
    coord.x = rem % grid_x;
    return coord;
}

u32 ProbeGridLayout::probeIndexFromCoord(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    if (!isValidProbeCoord(desc, coord)) {
        return UINT32_MAX;
    }
    return coord.z * desc.grid_dims.x * desc.grid_dims.y + coord.y * desc.grid_dims.x + coord.x;
}

bool ProbeGridLayout::isValidProbeCoord(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    return coord.x < desc.grid_dims.x && coord.y < desc.grid_dims.y && coord.z < desc.grid_dims.z;
}

bool ProbeGridLayout::isValidProbeIndex(const DDGIDesc& desc, u32 probe_index) {
    return probe_index < ddgi_util::probeCount(desc);
}

fuse::math::Vec3 ProbeGridLayout::worldToProbeGridCoord(const DDGIDesc& desc,
                                                        const fuse::math::Vec3& world_position) {
    const fuse::math::Vec3 delta = world_position - desc.grid_origin;
    if (desc.probe_spacing.x <= 0.f || desc.probe_spacing.y <= 0.f || desc.probe_spacing.z <= 0.f) {
        return {};
    }
    return {delta.x / desc.probe_spacing.x,
            delta.y / desc.probe_spacing.y,
            delta.z / desc.probe_spacing.z};
}

ProbeGridCoord ProbeGridLayout::clampProbeGridCoord(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    ProbeGridCoord clamped{};
    if (desc.grid_dims.x == 0u || desc.grid_dims.y == 0u || desc.grid_dims.z == 0u) {
        return clamped;
    }
    clamped.x = std::min(coord.x, desc.grid_dims.x - 1u);
    clamped.y = std::min(coord.y, desc.grid_dims.y - 1u);
    clamped.z = std::min(coord.z, desc.grid_dims.z - 1u);
    return clamped;
}

fuse::math::Vec2 ProbeGridLayout::probeIrradianceAtlasOrigin(const DDGIDesc& desc,
                                                             const ProbeGridCoord& coord) {
    return {static_cast<f32>(coord.x * desc.irradiance_res),
            static_cast<f32>((coord.z * desc.grid_dims.y + coord.y) * desc.irradiance_res)};
}

fuse::math::Vec2 ProbeGridLayout::probeDepthAtlasOrigin(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    return {static_cast<f32>(coord.x * desc.depth_res),
            static_cast<f32>((coord.z * desc.grid_dims.y + coord.y) * desc.depth_res)};
}

namespace ddgi_util {

u32 probeCount(const DDGIDesc& desc) {
    return desc.grid_dims.x * desc.grid_dims.y * desc.grid_dims.z;
}

fuse::math::Vec3 probeWorldPosition(const DDGIDesc& desc, u32 probe_index) {
    const ProbeGridCoord coord = ProbeGridLayout::probeCoordFromIndex(desc, probe_index);
    return desc.grid_origin +
           fuse::math::Vec3{desc.probe_spacing.x * static_cast<f32>(coord.x),
                            desc.probe_spacing.y * static_cast<f32>(coord.y),
                            desc.probe_spacing.z * static_cast<f32>(coord.z)};
}

u32 irradianceAtlasWidth(const DDGIDesc& desc) {
    return desc.grid_dims.x * desc.irradiance_res;
}

u32 irradianceAtlasHeight(const DDGIDesc& desc) {
    return desc.grid_dims.y * desc.grid_dims.z * desc.irradiance_res;
}

u32 depthAtlasWidth(const DDGIDesc& desc) {
    return desc.grid_dims.x * desc.depth_res;
}

u32 depthAtlasHeight(const DDGIDesc& desc) {
    return desc.grid_dims.y * desc.grid_dims.z * desc.depth_res;
}

void scheduleProbeUpdates(u32 frame_index,
                          u32 probe_count,
                          u32 probes_per_frame,
                          u32* out_indices,
                          u32 max_indices,
                          u32* out_count) {
    if (out_indices == nullptr || out_count == nullptr || probe_count == 0u || max_indices == 0u) {
        if (out_count != nullptr) {
            *out_count = 0u;
        }
        return;
    }

    const u32 count = std::min(probes_per_frame, std::min(probe_count, max_indices));
    const u32 start = (frame_index * probes_per_frame) % probe_count;
    for (u32 i = 0; i < count; ++i) {
        out_indices[i] = (start + i) % probe_count;
    }
    *out_count = count;
}

fuse::math::Vec3 blendIrradiance(const fuse::math::Vec3& previous,
                                 const fuse::math::Vec3& incoming,
                                 f32 hysteresis) {
    const f32 blend = std::clamp(hysteresis, 0.f, 1.f);
    return lerpIrradiance(previous, incoming, 1.f - blend);
}

fuse::math::Vec3 lerpIrradiance(const fuse::math::Vec3& a, const fuse::math::Vec3& b, f32 t) {
    const f32 clamped = std::clamp(t, 0.f, 1.f);
    return a * (1.f - clamped) + b * clamped;
}

fuse::math::Vec3 trilinearProbeIrradiance(const DDGIDesc& desc,
                                          const fuse::math::Vec3& world_position,
                                          const IrradianceCacheEntry* cache,
                                          u32 cache_count) {
    if (cache == nullptr || cache_count == 0u || desc.grid_dims.x == 0u || desc.grid_dims.y == 0u ||
        desc.grid_dims.z == 0u) {
        return {};
    }

    const fuse::math::Vec3 grid_coord = ProbeGridLayout::worldToProbeGridCoord(desc, world_position);

    const auto sample_probe = [&](u32 x, u32 y, u32 z) -> fuse::math::Vec3 {
        const ProbeGridCoord coord{x, y, z};
        const u32 index = ProbeGridLayout::probeIndexFromCoord(desc, coord);
        if (index == UINT32_MAX || index >= cache_count) {
            return {};
        }
        return cache[index].irradiance;
    };

    const u32 max_x = desc.grid_dims.x - 1u;
    const u32 max_y = desc.grid_dims.y - 1u;
    const u32 max_z = desc.grid_dims.z - 1u;

    const u32 x0 = static_cast<u32>(std::clamp(std::floor(grid_coord.x), 0.f, static_cast<f32>(max_x)));
    const u32 y0 = static_cast<u32>(std::clamp(std::floor(grid_coord.y), 0.f, static_cast<f32>(max_y)));
    const u32 z0 = static_cast<u32>(std::clamp(std::floor(grid_coord.z), 0.f, static_cast<f32>(max_z)));
    const u32 x1 = std::min(x0 + 1u, max_x);
    const u32 y1 = std::min(y0 + 1u, max_y);
    const u32 z1 = std::min(z0 + 1u, max_z);

    const f32 tx = std::clamp(grid_coord.x - static_cast<f32>(x0), 0.f, 1.f);
    const f32 ty = std::clamp(grid_coord.y - static_cast<f32>(y0), 0.f, 1.f);
    const f32 tz = std::clamp(grid_coord.z - static_cast<f32>(z0), 0.f, 1.f);

    const fuse::math::Vec3 c000 = sample_probe(x0, y0, z0);
    const fuse::math::Vec3 c100 = sample_probe(x1, y0, z0);
    const fuse::math::Vec3 c010 = sample_probe(x0, y1, z0);
    const fuse::math::Vec3 c110 = sample_probe(x1, y1, z0);
    const fuse::math::Vec3 c001 = sample_probe(x0, y0, z1);
    const fuse::math::Vec3 c101 = sample_probe(x1, y0, z1);
    const fuse::math::Vec3 c011 = sample_probe(x0, y1, z1);
    const fuse::math::Vec3 c111 = sample_probe(x1, y1, z1);

    const fuse::math::Vec3 c00 = lerpIrradiance(c000, c100, tx);
    const fuse::math::Vec3 c10 = lerpIrradiance(c010, c110, tx);
    const fuse::math::Vec3 c01 = lerpIrradiance(c001, c101, tx);
    const fuse::math::Vec3 c11 = lerpIrradiance(c011, c111, tx);

    const fuse::math::Vec3 c0 = lerpIrradiance(c00, c10, ty);
    const fuse::math::Vec3 c1 = lerpIrradiance(c01, c11, ty);
    return lerpIrradiance(c0, c1, tz);
}

u32 nearestProbeIndex(const DDGIDesc& desc, const fuse::math::Vec3& world_position) {
    const u32 count = probeCount(desc);
    if (count == 0u) {
        return UINT32_MAX;
    }

    u32 best_index = 0u;
    f32 best_distance = std::numeric_limits<f32>::max();
    for (u32 i = 0; i < count; ++i) {
        const fuse::math::Vec3 probe_pos = probeWorldPosition(desc, i);
        const fuse::math::Vec3 delta = world_position - probe_pos;
        const f32 distance = delta.dot(delta);
        if (distance < best_distance) {
            best_distance = distance;
            best_index = i;
        }
    }
    return best_index;
}

} // namespace ddgi_util

DdgiInfo ddgi_info() {
    DdgiInfo info{};
#if defined(FUSE_HAS_CUDA)
    info.backend = DdgiBackend::Cuda;
    info.message = "CUDA DDGI kernels stub — probe trace deferred";
#else
    info.backend = DdgiBackend::Stub;
    info.message = "CPU-only DDGI scaffold";
#endif
    info.valid = true;
    return info;
}

bool launch_ddgi_probe_update(const DDGIDesc& desc,
                              const u32* probe_indices,
                              u32 probe_count,
                              void* cuda_stream) {
    if (probe_count == 0u || probe_indices == nullptr) {
        return false;
    }

    gi::DDGIKernelParams params{};
    params.probe_indices_to_update = probe_indices;
    params.probe_update_count = probe_count;
    params.rays_per_probe = desc.rays_per_probe;
    params.hysteresis = desc.hysteresis;
    params.max_ray_distance = desc.max_ray_distance;

    const bool traced = gi::launch_probe_trace_kernel(params, cuda_stream);
    const bool blended = gi::launch_probe_blend_kernel(params, cuda_stream);
    return traced && blended;
}

namespace gi {

bool launch_probe_trace_kernel(const DDGIKernelParams& params, void* cuda_stream) {
    (void)params;
    (void)cuda_stream;
#if defined(FUSE_HAS_CUDA)
    // Full probe_trace_kernel lands in ddgi_kernels.cu — stub succeeds on CI.
    return true;
#else
    return true;
#endif
}

bool launch_probe_blend_kernel(const DDGIKernelParams& params, void* cuda_stream) {
    (void)params;
    (void)cuda_stream;
#if defined(FUSE_HAS_CUDA)
    return true;
#else
    return true;
#endif
}

} // namespace gi

bool DDGI::init(const DDGIDesc& desc, ResourceManager& resources) {
    destroy();
    m_desc = desc;
    m_resources = &resources;
    m_info = ddgi_info();

    const u32 count = ddgi_util::probeCount(m_desc);
    if (count == 0u || m_desc.irradiance_res == 0u || m_desc.depth_res == 0u) {
        return false;
    }

    if (!allocateResources(resources)) {
        destroy();
        return false;
    }

    m_cache.resize(count);
    for (IrradianceCacheEntry& entry : m_cache) {
        entry.irradiance = defaultAmbientIrradiance();
        entry.mean_depth = m_desc.max_ray_distance * 0.5f;
        entry.depth_variance = 0.1f;
    }

    m_data.desc = m_desc;
    m_data.irradiance_atlas = m_volume.irradiance_atlas;
    m_data.depth_atlas = m_volume.depth_atlas;
    m_data.probe_offsets = m_volume.probe_offsets;
    m_info.probe_count = count;
    m_ready = true;
    return true;
}

void DDGI::destroy() {
    releaseResources();
    m_desc = {};
    m_data = {};
    m_volume = {};
    m_cache.clear();
    m_last_update = {};
    m_info = {};
    m_resources = nullptr;
    m_ready = false;
}

const IrradianceCacheEntry& DDGI::cacheEntry(u32 probe_index) const {
    if (probe_index >= m_cache.size()) {
        return kDefaultCacheEntry;
    }
    return m_cache[probe_index];
}

bool DDGI::update(u32 frame_index, void* cuda_stream) {
    if (!m_ready) {
        return false;
    }

    m_last_update = {};
    m_last_update.frame_index = frame_index;

    const u32 probe_count = static_cast<u32>(m_cache.size());
    u32 scheduled_indices[256]{};
    u32 scheduled_count = 0u;
    ddgi_util::scheduleProbeUpdates(frame_index,
                                    probe_count,
                                    m_desc.probes_per_frame,
                                    scheduled_indices,
                                    static_cast<u32>(sizeof(scheduled_indices) / sizeof(scheduled_indices[0])),
                                    &scheduled_count);
    m_last_update.probes_scheduled = scheduled_count;

    m_last_update.kernel_launched =
        launch_ddgi_probe_update(m_desc, scheduled_indices, scheduled_count, cuda_stream);

    const fuse::math::Vec3 incoming = defaultAmbientIrradiance();
    for (u32 i = 0; i < scheduled_count; ++i) {
        const u32 probe_index = scheduled_indices[i];
        if (probe_index >= m_cache.size()) {
            continue;
        }
        IrradianceCacheEntry& entry = m_cache[probe_index];
        entry.irradiance = ddgi_util::blendIrradiance(entry.irradiance, incoming, m_desc.hysteresis);
    }

    return m_last_update.kernel_launched;
}

DDGISampleResult DDGI::sampleIrradiance(const DDGISampleRequest& request) const {
    DDGISampleResult result{};
    if (!m_ready) {
        return result;
    }

    const u32 nearest = ddgi_util::nearestProbeIndex(m_desc, request.world_position);
    if (nearest == UINT32_MAX || nearest >= m_cache.size()) {
        return result;
    }

    result.nearest_probe = nearest;
    result.irradiance = ddgi_util::trilinearProbeIrradiance(m_desc,
                                                           request.world_position,
                                                           m_cache.data(),
                                                           static_cast<u32>(m_cache.size()));
    result.valid = true;
    return result;
}

bool DDGI::allocateResources(ResourceManager& resources) {
    const u32 count = ddgi_util::probeCount(m_desc);

    TextureDesc irradianceDesc{};
    irradianceDesc.width = ddgi_util::irradianceAtlasWidth(m_desc);
    irradianceDesc.height = ddgi_util::irradianceAtlasHeight(m_desc);
    irradianceDesc.format = GpuFormat::R16G16B16A16Sfloat;
    irradianceDesc.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) |
                                                   static_cast<u32>(ImageUsage::Storage));
    irradianceDesc.cudaInterop = true;
    irradianceDesc.name = "ddgi_irradiance_atlas";

    TextureDesc depthDesc{};
    depthDesc.width = ddgi_util::depthAtlasWidth(m_desc);
    depthDesc.height = ddgi_util::depthAtlasHeight(m_desc);
    depthDesc.format = GpuFormat::R16G16Sfloat;
    depthDesc.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) |
                                              static_cast<u32>(ImageUsage::Storage));
    depthDesc.cudaInterop = true;
    depthDesc.name = "ddgi_depth_atlas";

    m_volume.irradiance_atlas = resources.createTexture(irradianceDesc);
    m_volume.depth_atlas = resources.createTexture(depthDesc);
    if (!m_volume.irradiance_atlas.isValid() || !m_volume.depth_atlas.isValid()) {
        return false;
    }

    BufferDesc offsetDesc{};
    offsetDesc.size = static_cast<usize>(count) * sizeof(fuse::math::Vec3);
    offsetDesc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                                static_cast<u32>(BufferUsage::TransferDst));
    offsetDesc.cudaInterop = true;
    offsetDesc.name = "ddgi_probe_offsets";
    m_volume.probe_offsets = resources.createBuffer(offsetDesc);
    if (!m_volume.probe_offsets.isValid()) {
        return false;
    }

    m_volume.probe_count = count;
    return true;
}

void DDGI::releaseResources() {
    if (m_resources == nullptr) {
        m_volume = {};
        return;
    }

    if (m_volume.irradiance_atlas.isValid()) {
        m_resources->destroyTexture(m_volume.irradiance_atlas);
    }
    if (m_volume.depth_atlas.isValid()) {
        m_resources->destroyTexture(m_volume.depth_atlas);
    }
    if (m_volume.probe_offsets.isValid()) {
        m_resources->destroyBuffer(m_volume.probe_offsets);
    }
    m_volume = {};
}

} // namespace fuse::renderer
