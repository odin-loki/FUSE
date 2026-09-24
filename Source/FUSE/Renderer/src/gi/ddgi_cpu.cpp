// CPU entry points of the DDGI probe update. All trace / sample / blend math lives once in
// fuse/renderer/gi/ddgi_probe_kernel.hpp (FUSE_HOST_DEVICE, shared with kernels/ddgi_probe_update.cu);
// this TU keeps the host-side state (atlases, ray set, schedule) and launches the kernel bodies.

#include <fuse/renderer/gi/ddgi_cpu.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/load_scale.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/gi/ddgi_probe_kernel.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

#if defined(FUSE_HAS_CUDA)
/// kernels/ddgi_probe_update.cu: stages the scene, ray set and atlases on the device, runs the same
/// trace + blend kernel bodies and reads the atlases / update counts / stats back. Only called for
/// duplicate-free probe lists (device workgroups of one launch run concurrently).
bool launchDdgiProbeUpdateCuda(const ddgi_kernel::TraceParams& trace,
                               const ddgi_kernel::BlendParams& blend,
                               u32 slots,
                               void* stream);
#endif

namespace {

using fuse::math::Vec2;
using fuse::math::Vec3;
using fuse::math::Vec4;

constexpr f32 kPi = ddgi_kernel::kPi;

u64 splitMix64(u64& state) {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

f32 uniform01(u64& state) {
    return static_cast<f32>(splitMix64(state) >> 40) * (1.f / 16777216.f);
}

ddgi_kernel::SceneView sceneView(const DdgiCpuScene& scene) {
    ddgi_kernel::SceneView view{};
    view.boxes = scene.boxes.data();
    view.box_count = static_cast<u32>(scene.boxes.size());
    view.sun_direction = scene.sun_direction;
    view.sun_irradiance = scene.sun_irradiance;
    view.sky_radiance = scene.sky_radiance;
    return view;
}

ddgi_kernel::VolumeView volumeView(const DdgiCpuVolume& volume) {
    ddgi_kernel::VolumeView view{};
    view.desc = volume.desc();
    view.probe_count = volume.probeCount();
    view.irradiance = volume.irradianceAtlas().data();
    view.distance = volume.distanceAtlas().data();
    view.normal_bias = volume.config().normal_bias;
    view.weight_crush_threshold = volume.config().weight_crush_threshold;
    view.probe_data = volume.probeStatesEnabled() ? volume.probeData().data() : nullptr;
    view.view_bias = volume.config().view_bias;
    return view;
}

} // namespace

DdgiCpuSurface ddgiSurfaceFromMaterial(const Material& material) {
    DdgiCpuSurface surface{};
    const f32 dielectric = 1.f - std::clamp(material.metallic, 0.f, 1.f);
    surface.albedo = material.baseColor * dielectric;
    surface.emissive = material.emissiveColor * std::max(material.emissiveIntensity, 0.f);
    return surface;
}

u32 DdgiCpuScene::addBox(const Vec3& min, const Vec3& max, const DdgiCpuSurface& surface) {
    DdgiCpuBox box{};
    box.min = {std::min(min.x, max.x), std::min(min.y, max.y), std::min(min.z, max.z)};
    box.max = {std::max(min.x, max.x), std::max(min.y, max.y), std::max(min.z, max.z)};
    box.surface = surface;
    boxes.push_back(box);
    return static_cast<u32>(boxes.size() - 1u);
}

bool DdgiCpuScene::intersect(const Vec3& origin,
                             const Vec3& direction,
                             f32 t_min,
                             f32 t_max,
                             DdgiCpuHit& out_hit) const {
    return ddgi_kernel::intersect_boxes(boxes.data(), static_cast<u32>(boxes.size()), origin, direction, t_min, t_max,
                                        out_hit);
}

bool DdgiCpuScene::occluded(const Vec3& origin, const Vec3& direction, f32 t_min, f32 t_max) const {
    DdgiCpuHit hit{};
    return intersect(origin, direction, t_min, t_max, hit);
}

Vec3 DdgiCpuScene::directRadiance(const DdgiCpuHit& hit) const {
    return ddgi_kernel::direct_radiance(sceneView(*this), hit);
}

namespace ddgi_cpu {

Vec3 sphericalFibonacci(u32 index, u32 count) {
    if (count == 0u) {
        return {0.f, 1.f, 0.f};
    }
    // Golden-angle spiral with equal-area z bands.
    constexpr f64 kGoldenAngle = 2.39996322972865332;
    const f64 phi = kGoldenAngle * static_cast<f64>(index);
    const f64 z = 1.0 - (2.0 * static_cast<f64>(index) + 1.0) / static_cast<f64>(count);
    const f64 r = std::sqrt(std::max(0.0, 1.0 - z * z));
    return {static_cast<f32>(r * std::cos(phi)), static_cast<f32>(r * std::sin(phi)), static_cast<f32>(z)};
}

DdgiRayRotation randomRotation(u64 seed) {
    u64 state = seed;
    const f32 u1 = uniform01(state);
    const f32 u2 = uniform01(state);
    const f32 u3 = uniform01(state);
    // Shoemake uniform random unit quaternion.
    const f32 a = std::sqrt(1.f - u1);
    const f32 b = std::sqrt(u1);
    const f32 qx = a * std::sin(2.f * kPi * u2);
    const f32 qy = a * std::cos(2.f * kPi * u2);
    const f32 qz = b * std::sin(2.f * kPi * u3);
    const f32 qw = b * std::cos(2.f * kPi * u3);

    DdgiRayRotation r{};
    r.row0 = {1.f - 2.f * (qy * qy + qz * qz), 2.f * (qx * qy - qz * qw), 2.f * (qx * qz + qy * qw)};
    r.row1 = {2.f * (qx * qy + qz * qw), 1.f - 2.f * (qx * qx + qz * qz), 2.f * (qy * qz - qx * qw)};
    r.row2 = {2.f * (qx * qz - qy * qw), 2.f * (qy * qz + qx * qw), 1.f - 2.f * (qx * qx + qy * qy)};
    return r;
}

DdgiRayRotation updateRotation(u64 seed, u32 frame_index) {
    u64 state = seed ^ (static_cast<u64>(frame_index) * 0xD1B54A32D192ED03ull);
    return randomRotation(splitMix64(state));
}

Vec3 texelDirection(u32 x, u32 y, u32 res) {
    if (res == 0u) {
        return {0.f, 1.f, 0.f};
    }
    const f32 inv = 1.f / static_cast<f32>(res);
    return DdgiIrradianceEncoding::decodeDirection(
        {(static_cast<f32>(x) + 0.5f) * inv, (static_cast<f32>(y) + 0.5f) * inv});
}

} // namespace ddgi_cpu

bool DdgiCpuVolume::init(const DDGIDesc& desc, const DdgiCpuConfig& config) {
    reset();
    const u32 count = ddgi_util::probeCount(desc);
    if (count == 0u || desc.irradiance_res == 0u || desc.depth_res == 0u || desc.rays_per_probe == 0u) {
        return false;
    }
    m_desc = desc;
    m_config = config;
    m_probe_count = count;

    const usize irr_tile = static_cast<usize>(irradianceTileSize()) * irradianceTileSize();
    const usize dist_tile = static_cast<usize>(distanceTileSize()) * distanceTileSize();
    m_irradiance.assign(irr_tile * count, config.initial_irradiance);
    const f32 initial_distance = desc.max_ray_distance;
    m_distance.assign(dist_tile * count, Vec2{initial_distance, initial_distance * initial_distance});
    m_update_counts.assign(count, 0u);
    m_probe_data.assign(count, Vec4{0.f, 0.f, 0.f, ddgi_kernel::kProbeActive});

    const u32 ir = desc.irradiance_res;
    m_scratch_texel_dirs.resize(static_cast<usize>(ir) * ir);
    for (u32 y = 0; y < ir; ++y) {
        for (u32 x = 0; x < ir; ++x) {
            m_scratch_texel_dirs[y * ir + x] = ddgi_cpu::texelDirection(x, y, ir);
        }
    }
    const u32 dr = desc.depth_res;
    m_scratch_distance_dirs.resize(static_cast<usize>(dr) * dr);
    for (u32 y = 0; y < dr; ++y) {
        for (u32 x = 0; x < dr; ++x) {
            m_scratch_distance_dirs[y * dr + x] = ddgi_cpu::texelDirection(x, y, dr);
        }
    }
    m_ready = true;
    return true;
}

void DdgiCpuVolume::reset() {
    m_desc = {};
    m_probe_count = 0u;
    m_irradiance.clear();
    m_distance.clear();
    m_update_counts.clear();
    m_probe_data.clear();
    m_scratch_dirs.clear();
    m_scratch_radiance.clear();
    m_scratch_distance.clear();
    m_scratch_texel_dirs.clear();
    m_scratch_distance_dirs.clear();
    m_scratch_incoming.clear();
    m_scratch_seen.clear();
    m_ready = false;
}

usize DdgiCpuVolume::irradianceOffset(u32 probe_index) const {
    return static_cast<usize>(probe_index) * irradianceTileSize() * irradianceTileSize();
}

usize DdgiCpuVolume::distanceOffset(u32 probe_index) const {
    return static_cast<usize>(probe_index) * distanceTileSize() * distanceTileSize();
}

DdgiCpuUpdateStats DdgiCpuVolume::update(const DdgiCpuScene& scene, u32 frame_index) {
    if (!m_ready) {
        return {};
    }
    // LoadScale::probes scales the rolling per-frame budget (1 = the authored probes_per_frame).
    const u32 budget = kernel::scaled_count(m_desc.probes_per_frame, kernel::load_scale().probes);
    std::vector<u32> indices(std::max(budget, 1u));
    u32 scheduled = 0u;
    ddgi_util::scheduleProbeUpdates(frame_index,
                                    m_probe_count,
                                    budget,
                                    indices.data(),
                                    static_cast<u32>(indices.size()),
                                    &scheduled);
    return updateProbes(scene, indices.data(), scheduled, frame_index);
}

DdgiCpuUpdateStats DdgiCpuVolume::updateProbes(const DdgiCpuScene& scene,
                                               const u32* probe_indices,
                                               u32 probe_count,
                                               u32 frame_index) {
    DdgiCpuUpdateStats stats{};
    if (!m_ready || probe_indices == nullptr || probe_count == 0u) {
        return stats;
    }
    const u32 rays = m_desc.rays_per_probe;
    const DdgiRayRotation rotation = ddgi_cpu::updateRotation(m_config.rotation_seed, frame_index);
    m_scratch_dirs.resize(rays);
    for (u32 r = 0; r < rays; ++r) {
        m_scratch_dirs[r] = rotation.apply(ddgi_cpu::sphericalFibonacci(r, rays)).normalized();
    }
    const usize total = static_cast<usize>(probe_count) * rays;
    m_scratch_radiance.resize(total);
    m_scratch_distance.resize(total);
    const u32 ir = m_desc.irradiance_res;
    m_scratch_incoming.resize(static_cast<usize>(probe_count) * ir * ir);

    // Valid slots count toward the stats; a probe listed twice makes the blend order-dependent, so
    // such a list blends serially (CpuReference = list order, the pre-kernel behaviour).
    m_scratch_seen.assign(m_probe_count, 0u);
    bool duplicates = false;
    for (u32 p = 0; p < probe_count; ++p) {
        const u32 probe = probe_indices[p];
        if (probe >= m_probe_count) {
            continue;
        }
        duplicates = duplicates || m_scratch_seen[probe] != 0u;
        m_scratch_seen[probe] = 1u;
        stats.rays_traced += rays;
        ++stats.probes_updated;
        if (probeStatesEnabled() && m_probe_data[probe].w != ddgi_kernel::kProbeActive) {
            ++stats.probes_inactive;
        }
    }
    const bool states = probeStatesEnabled();

    ddgi_kernel::TraceParams trace{};
    trace.probe_indices = {probe_indices, probe_count};
    trace.ray_dirs = {m_scratch_dirs.data(), rays};
    trace.scene = sceneView(scene);
    trace.volume = volumeView(*this);
    trace.backface_distance_scale = m_config.backface_distance_scale;
    trace.multi_bounce = m_config.multi_bounce;
    trace.out_radiance = {m_scratch_radiance.data(), static_cast<u32>(total)};
    trace.out_distance = {m_scratch_distance.data(), static_cast<u32>(total)};

    u32 fast_response = 0u;
    ddgi_kernel::BlendParams blend{};
    blend.probe_indices = {probe_indices, probe_count};
    blend.probe_count = m_probe_count;
    blend.ray_dirs = {m_scratch_dirs.data(), rays};
    blend.radiance = {m_scratch_radiance.data(), static_cast<u32>(total)};
    blend.distance = {m_scratch_distance.data(), static_cast<u32>(total)};
    blend.irradiance_texel_dirs = {m_scratch_texel_dirs.data(), static_cast<u32>(m_scratch_texel_dirs.size())};
    blend.distance_texel_dirs = {m_scratch_distance_dirs.data(), static_cast<u32>(m_scratch_distance_dirs.size())};
    blend.irradiance = m_irradiance.data();
    blend.distance_moments = m_distance.data();
    blend.update_counts = m_update_counts.data();
    blend.incoming = {m_scratch_incoming.data(), static_cast<u32>(m_scratch_incoming.size())};
    blend.fast_response_texels = &fast_response;
    blend.irradiance_res = ir;
    blend.depth_res = m_desc.depth_res;
    blend.hysteresis = std::clamp(m_desc.hysteresis, 0.f, 1.f);
    blend.probe_change_hysteresis = std::clamp(m_config.probe_change_hysteresis, 0.f, 1.f);
    blend.probe_change_threshold = m_config.probe_change_threshold;
    blend.change_threshold = m_config.change_threshold;
    blend.change_hysteresis_drop = m_config.change_hysteresis_drop;
    blend.change_floor = m_config.change_floor;
    // Rays whose cos^power weight is below 1e-6 cannot move the weighted mean; the kernel skips them.
    blend.distance_power = std::max(m_config.distance_power, 1e-3f);
    blend.distance_min_cos = std::pow(1e-6f, 1.f / blend.distance_power);
    blend.max_distance = m_config.distance_clamp > 0.f ? std::min(m_config.distance_clamp, m_desc.max_ray_distance)
                                                       : m_desc.max_ray_distance;
    blend.probe_data = states ? m_probe_data.data() : nullptr;

    ddgi_kernel::ProbeStateParams state{};
    state.probe_indices = {probe_indices, probe_count};
    state.probe_count = m_probe_count;
    state.ray_dirs = {m_scratch_dirs.data(), rays};
    state.distance = {m_scratch_distance.data(), static_cast<u32>(total)};
    state.probe_data = m_probe_data.data();
    state.spacing = m_desc.probe_spacing;
    state.max_distance = m_desc.max_ray_distance;
    state.backface_distance_scale = m_config.backface_distance_scale;
    state.min_frontface_distance = m_config.probe_min_frontface_distance;
    state.backface_threshold = m_config.probe_backface_threshold;
    state.max_offset = m_config.probe_max_offset;
    state.relocation_step = m_config.probe_relocation_step;
    state.relocation = m_config.probe_relocation;
    state.classification = m_config.probe_classification;

#if defined(FUSE_HAS_CUDA)
    // The CUDA wrapper stages no probe data: relocation / classification run on the CPU backends.
    if (!duplicates && !states && (m_backend == kernel::Backend::Cuda || m_backend == kernel::Backend::Auto) &&
        kernel::backend_available(kernel::Backend::Cuda) &&
        launchDdgiProbeUpdateCuda(trace, blend, probe_count, nullptr)) {
        stats.fast_response_texels = fast_response;
        return stats;
    }
#endif
    // CPU backends, or a GPU backend that cannot run here: kernel::launch resolves the fallback
    // (CpuParallel) and records the requested vs executed backend. The trace reads the pre-update
    // volume for every probe (multi-bounce feedback), exactly like the separate device kernels.
    kernel::launch(m_backend, ddgi_kernel::make_trace_launch(rays, probe_count), ddgi_kernel::TraceKernel{}, trace);
    kernel::launch(duplicates ? kernel::Backend::CpuReference : m_backend, ddgi_kernel::make_blend_launch(probe_count),
                   ddgi_kernel::BlendKernel{}, blend);
    if (states) {
        // After the blend (which used the states from before this update), from this update's rays.
        kernel::launch(duplicates ? kernel::Backend::CpuReference : m_backend, ddgi_kernel::make_state_launch(probe_count),
                       ddgi_kernel::ProbeStateKernel{}, state);
    }
    stats.fast_response_texels = fast_response;
    return stats;
}

Vec3 DdgiCpuVolume::probePosition(u32 probe_index) const {
    if (!m_ready) {
        return {};
    }
    return ddgi_kernel::probe_position(volumeView(*this), probe_index);
}

bool DdgiCpuVolume::probeActive(u32 probe_index) const {
    return m_ready && probe_index < m_probe_count && (!probeStatesEnabled() || m_probe_data[probe_index].w == ddgi_kernel::kProbeActive);
}

u32 DdgiCpuVolume::probeUpdateCount(u32 probe_index) const {
    return probe_index < m_update_counts.size() ? m_update_counts[probe_index] : 0u;
}

Vec3 DdgiCpuVolume::irradianceTexel(u32 probe_index, u32 x, u32 y) const {
    const u32 tile = irradianceTileSize();
    if (!m_ready || probe_index >= m_probe_count || x >= tile || y >= tile) {
        return {};
    }
    return m_irradiance[irradianceOffset(probe_index) + static_cast<usize>(y) * tile + x];
}

Vec2 DdgiCpuVolume::distanceTexel(u32 probe_index, u32 x, u32 y) const {
    const u32 tile = distanceTileSize();
    if (!m_ready || probe_index >= m_probe_count || x >= tile || y >= tile) {
        return {};
    }
    return m_distance[distanceOffset(probe_index) + static_cast<usize>(y) * tile + x];
}

Vec3 DdgiCpuVolume::probeMeanTexel(u32 probe_index) const {
    if (!m_ready || probe_index >= m_probe_count) {
        return {};
    }
    const u32 ir = m_desc.irradiance_res;
    Vec3 sum{};
    for (u32 y = 1; y <= ir; ++y) {
        for (u32 x = 1; x <= ir; ++x) {
            sum = sum + irradianceTexel(probe_index, x, y);
        }
    }
    return sum * (1.f / static_cast<f32>(ir * ir));
}

Vec2 DdgiCpuVolume::probeMeanDistance(u32 probe_index) const {
    if (!m_ready || probe_index >= m_probe_count) {
        return {};
    }
    const u32 dr = m_desc.depth_res;
    Vec2 sum{};
    for (u32 y = 1; y <= dr; ++y) {
        for (u32 x = 1; x <= dr; ++x) {
            sum = sum + distanceTexel(probe_index, x, y);
        }
    }
    return sum * (1.f / static_cast<f32>(dr * dr));
}

Vec3 DdgiCpuVolume::probeIrradiance(u32 probe_index, const Vec3& direction) const {
    if (!m_ready) {
        return {};
    }
    return ddgi_kernel::probe_irradiance(volumeView(*this), probe_index, direction);
}

Vec2 DdgiCpuVolume::probeDistance(u32 probe_index, const Vec3& direction) const {
    if (!m_ready) {
        return {};
    }
    return ddgi_kernel::probe_distance(volumeView(*this), probe_index, direction);
}

Vec3 DdgiCpuVolume::sampleIrradiance(const Vec3& position, const Vec3& normal) const {
    if (!m_ready) {
        return {};
    }
    return ddgi_kernel::sample_irradiance(volumeView(*this), position, normal);
}

Vec3 DdgiCpuVolume::sampleIrradiance(const Vec3& position, const Vec3& normal, const Vec3& view) const {
    if (!m_ready) {
        return {};
    }
    return ddgi_kernel::sample_irradiance(volumeView(*this), position, normal, view);
}

namespace ddgi_cpu {

namespace {

/// Visit every bordered tile texel with its atlas position (column x, row z * dims.y + y).
template <typename Fn>
bool forEachAtlasTexel(const DdgiCpuVolume& volume, u32 tile, u32 atlasWidth, Fn&& fn) {
    const DDGIDesc& desc = volume.desc();
    for (u32 probe = 0; probe < volume.probeCount(); ++probe) {
        const ProbeGridCoord coord = ProbeGridLayout::probeCoordFromIndex(desc, probe);
        const usize originX = static_cast<usize>(coord.x) * tile;
        const usize originY = static_cast<usize>(coord.z * desc.grid_dims.y + coord.y) * tile;
        for (u32 y = 0; y < tile; ++y) {
            for (u32 x = 0; x < tile; ++x) {
                fn(probe, x, y, (originY + y) * atlasWidth + originX + x);
            }
        }
    }
    return true;
}

} // namespace

bool packIrradianceAtlasRgba16f(const DdgiCpuVolume& volume, std::vector<u16>& out) {
    if (!volume.isReady()) {
        return false;
    }
    const DDGIDesc& desc = volume.desc();
    const u32 width = ddgi_util::irradianceAtlasWidth(desc);
    const u32 height = ddgi_util::irradianceAtlasHeight(desc);
    const u32 tile = volume.irradianceTileSize();
    if (width == 0u || height == 0u || tile != ProbeGridLayout::irradianceTileSize(desc)) {
        return false;
    }
    out.assign(static_cast<usize>(width) * height * 4u, 0u);
    const u16 one = GBufferQuantize::floatToHalf(1.f);
    return forEachAtlasTexel(volume, tile, width, [&](u32 probe, u32 x, u32 y, usize texel) {
        const Vec3 e = volume.irradianceTexel(probe, x, y);
        u16* dst = &out[texel * 4u];
        dst[0] = GBufferQuantize::floatToHalf(e.x);
        dst[1] = GBufferQuantize::floatToHalf(e.y);
        dst[2] = GBufferQuantize::floatToHalf(e.z);
        dst[3] = one;
    });
}

bool packDistanceAtlasRg16f(const DdgiCpuVolume& volume, std::vector<u16>& out) {
    if (!volume.isReady()) {
        return false;
    }
    const DDGIDesc& desc = volume.desc();
    const u32 width = ddgi_util::depthAtlasWidth(desc);
    const u32 height = ddgi_util::depthAtlasHeight(desc);
    const u32 tile = volume.distanceTileSize();
    if (width == 0u || height == 0u || tile != ProbeGridLayout::depthTileSize(desc)) {
        return false;
    }
    out.assign(static_cast<usize>(width) * height * 2u, 0u);
    return forEachAtlasTexel(volume, tile, width, [&](u32 probe, u32 x, u32 y, usize texel) {
        const Vec2 moments = volume.distanceTexel(probe, x, y);
        out[texel * 2u + 0u] = GBufferQuantize::floatToHalf(moments.x);
        out[texel * 2u + 1u] = GBufferQuantize::floatToHalf(moments.y);
    });
}

} // namespace ddgi_cpu

} // namespace fuse::renderer
