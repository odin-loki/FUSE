#include <fuse/renderer/gi/ddgi_cpu.hpp>

#include <fuse/renderer/deferred/gbuffer.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::renderer {
namespace {

using fuse::math::Vec2;
using fuse::math::Vec3;

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kInvPi = 1.f / kPi;
constexpr f32 kRayEpsilon = 1e-4f;

Vec3 mul(const Vec3& a, const Vec3& b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

f32 maxComponent(const Vec3& v) {
    return std::max(v.x, std::max(v.y, v.z));
}

Vec3 absVec(const Vec3& v) {
    return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)};
}

f32 axis(const Vec3& v, u32 i) {
    return i == 0u ? v.x : (i == 1u ? v.y : v.z);
}

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

/// Bordered-tile bilinear fetch at octahedral direction `direction`.
template <typename T>
T sampleTile(const T* tile, u32 res, const Vec3& direction) {
    const u32 stride = res + 2u;
    const Vec2 uv = DdgiIrradianceEncoding::encodeDirection(direction);
    // Interior texel k covers [k+1, k+2) of the bordered tile; centres sit at +0.5.
    const f32 px = 1.f + uv.x * static_cast<f32>(res) - 0.5f;
    const f32 py = 1.f + uv.y * static_cast<f32>(res) - 0.5f;
    const f32 fx = std::clamp(px, 0.f, static_cast<f32>(res));
    const f32 fy = std::clamp(py, 0.f, static_cast<f32>(res));
    const u32 x0 = std::min(static_cast<u32>(fx), res);
    const u32 y0 = std::min(static_cast<u32>(fy), res);
    const u32 x1 = x0 + 1u;
    const u32 y1 = y0 + 1u;
    const f32 tx = fx - static_cast<f32>(x0);
    const f32 ty = fy - static_cast<f32>(y0);
    const T a = tile[y0 * stride + x0] * (1.f - tx) + tile[y0 * stride + x1] * tx;
    const T b = tile[y1 * stride + x0] * (1.f - tx) + tile[y1 * stride + x1] * tx;
    return a * (1.f - ty) + b * ty;
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
    out_hit = {};
    f32 best = t_max;
    for (u32 b = 0; b < static_cast<u32>(boxes.size()); ++b) {
        const DdgiCpuBox& box = boxes[b];
        f32 t_near = -std::numeric_limits<f32>::infinity();
        f32 t_far = std::numeric_limits<f32>::infinity();
        u32 near_axis = 0u;
        u32 far_axis = 0u;
        bool miss = false;
        for (u32 a = 0; a < 3u; ++a) {
            const f32 o = axis(origin, a);
            const f32 d = axis(direction, a);
            const f32 lo = axis(box.min, a);
            const f32 hi = axis(box.max, a);
            if (std::fabs(d) < 1e-12f) {
                if (o < lo || o > hi) {
                    miss = true;
                    break;
                }
                continue;
            }
            const f32 inv = 1.f / d;
            f32 t0 = (lo - o) * inv;
            f32 t1 = (hi - o) * inv;
            if (t0 > t1) {
                std::swap(t0, t1);
            }
            if (t0 > t_near) {
                t_near = t0;
                near_axis = a;
            }
            if (t1 < t_far) {
                t_far = t1;
                far_axis = a;
            }
        }
        if (miss || t_near > t_far) {
            continue;
        }
        f32 t = 0.f;
        bool backface = false;
        u32 hit_axis = 0u;
        if (t_near > t_min) {
            t = t_near;
            hit_axis = near_axis;
        } else if (t_far > t_min) {
            t = t_far;
            hit_axis = far_axis;
            backface = true;
        } else {
            continue;
        }
        if (t >= best) {
            continue;
        }
        best = t;
        const f32 d = axis(direction, hit_axis);
        // Entry faces face against the ray; exit faces (backface) face along it.
        const f32 sign = backface ? (d >= 0.f ? 1.f : -1.f) : (d >= 0.f ? -1.f : 1.f);
        Vec3 n{};
        if (hit_axis == 0u) {
            n.x = sign;
        } else if (hit_axis == 1u) {
            n.y = sign;
        } else {
            n.z = sign;
        }
        out_hit.hit = true;
        out_hit.backface = backface;
        out_hit.t = t;
        out_hit.position = origin + direction * t;
        out_hit.normal = n;
        out_hit.box_index = b;
    }
    return out_hit.hit;
}

bool DdgiCpuScene::occluded(const Vec3& origin, const Vec3& direction, f32 t_min, f32 t_max) const {
    DdgiCpuHit hit{};
    return intersect(origin, direction, t_min, t_max, hit);
}

Vec3 DdgiCpuScene::directRadiance(const DdgiCpuHit& hit) const {
    if (!hit.hit || hit.backface || hit.box_index >= boxes.size()) {
        return {};
    }
    const DdgiCpuSurface& surface = boxes[hit.box_index].surface;
    Vec3 radiance = surface.emissive;
    const f32 cos_sun = hit.normal.dot(sun_direction);
    if (cos_sun > 0.f && maxComponent(sun_irradiance) > 0.f) {
        const Vec3 shadow_origin = hit.position + hit.normal * kRayEpsilon;
        if (!occluded(shadow_origin, sun_direction, 0.f, std::numeric_limits<f32>::infinity())) {
            radiance = radiance + mul(surface.albedo, sun_irradiance) * (cos_sun * kInvPi);
        }
    }
    return radiance;
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
    m_scratch_dirs.clear();
    m_scratch_radiance.clear();
    m_scratch_distance.clear();
    m_scratch_texel_dirs.clear();
    m_scratch_distance_dirs.clear();
    m_scratch_incoming.clear();
    m_scratch_valid.clear();
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
    std::vector<u32> indices(std::max(m_desc.probes_per_frame, 1u));
    u32 scheduled = 0u;
    ddgi_util::scheduleProbeUpdates(frame_index,
                                    m_probe_count,
                                    m_desc.probes_per_frame,
                                    indices.data(),
                                    static_cast<u32>(indices.size()),
                                    &scheduled);
    return updateProbes(scene, indices.data(), scheduled, frame_index);
}

Vec3 DdgiCpuVolume::traceRadiance(const DdgiCpuScene& scene,
                                  const Vec3& origin,
                                  const Vec3& direction,
                                  f32& out_distance) const {
    DdgiCpuHit hit{};
    if (!scene.intersect(origin, direction, 0.f, m_desc.max_ray_distance, hit)) {
        out_distance = m_desc.max_ray_distance;
        return scene.sky_radiance;
    }
    if (hit.backface) {
        // Probe sits inside geometry: no light, and a short distance so visibility rejects it.
        out_distance = hit.t * m_config.backface_distance_scale;
        return {};
    }
    out_distance = hit.t;
    Vec3 radiance = scene.directRadiance(hit);
    if (m_config.multi_bounce) {
        const Vec3& albedo = scene.boxes[hit.box_index].surface.albedo;
        radiance = radiance + mul(albedo, sampleIrradiance(hit.position, hit.normal)) * kInvPi;
    }
    return radiance;
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

    // Trace pass: every scheduled probe reads the pre-update volume (multi-bounce feedback),
    // mirroring the separate trace and blend kernels on the device.
    for (u32 p = 0; p < probe_count; ++p) {
        const u32 probe = probe_indices[p];
        if (probe >= m_probe_count) {
            continue;
        }
        const Vec3 origin = ddgi_util::probeWorldPosition(m_desc, probe);
        for (u32 r = 0; r < rays; ++r) {
            const usize slot = static_cast<usize>(p) * rays + r;
            m_scratch_radiance[slot] = traceRadiance(scene, origin, m_scratch_dirs[r], m_scratch_distance[slot]);
        }
        stats.rays_traced += rays;
    }

    // Blend pass.
    for (u32 p = 0; p < probe_count; ++p) {
        const u32 probe = probe_indices[p];
        if (probe >= m_probe_count) {
            continue;
        }
        const usize base = static_cast<usize>(p) * rays;
        blendProbe(probe, m_scratch_dirs.data(), &m_scratch_radiance[base], &m_scratch_distance[base], rays, stats);
        ++stats.probes_updated;
    }
    return stats;
}

void DdgiCpuVolume::blendProbe(u32 probe_index,
                               const Vec3* ray_dirs,
                               const Vec3* radiance,
                               const f32* distances,
                               u32 ray_count,
                               DdgiCpuUpdateStats& stats) {
    const bool first = m_update_counts[probe_index] == 0u;
    const f32 hysteresis = first ? 0.f : std::clamp(m_desc.hysteresis, 0.f, 1.f);

    const u32 ir = m_desc.irradiance_res;
    const u32 irr_stride = ir + 2u;
    Vec3* irr_tile = &m_irradiance[irradianceOffset(probe_index)];
    m_scratch_incoming.resize(static_cast<usize>(ir) * ir);
    m_scratch_valid.resize(static_cast<usize>(ir) * ir);
    Vec3 incoming_mean{};
    Vec3 history_mean{};
    u32 valid_count = 0u;
    for (u32 y = 0; y < ir; ++y) {
        for (u32 x = 0; x < ir; ++x) {
            const u32 t = y * ir + x;
            const Vec3& texel_dir = m_scratch_texel_dirs[t];
            const f32 tx = texel_dir.x;
            const f32 ty = texel_dir.y;
            const f32 tz = texel_dir.z;
            f32 sum_r = 0.f;
            f32 sum_g = 0.f;
            f32 sum_b = 0.f;
            f32 weight_sum = 0.f;
            for (u32 r = 0; r < ray_count; ++r) {
                const Vec3& d = ray_dirs[r];
                const f32 w = tx * d.x + ty * d.y + tz * d.z;
                if (w > 0.f) {
                    const Vec3& l = radiance[r];
                    sum_r += l.x * w;
                    sum_g += l.y * w;
                    sum_b += l.z * w;
                    weight_sum += w;
                }
            }
            m_scratch_valid[t] = weight_sum > 0.f ? 1u : 0u;
            if (weight_sum <= 0.f) {
                continue;
            }
            const f32 inv_weight = 1.f / weight_sum;
            m_scratch_incoming[t] = {sum_r * inv_weight, sum_g * inv_weight, sum_b * inv_weight};
            incoming_mean = incoming_mean + m_scratch_incoming[t];
            history_mean = history_mean + irr_tile[(y + 1u) * irr_stride + (x + 1u)];
            ++valid_count;
        }
    }

    // Lighting discontinuity detection. Probe level: the mean over all texels averages out most
    // per-texel ray noise, so a modest threshold catches global changes (sun, sky) with few
    // false triggers, and the stale history is dropped. Texel level: a larger threshold catches
    // local changes (one light in one direction) and lowers the hysteresis by
    // `change_hysteresis_drop`. Without either, a probe updated every N frames at hysteresis h
    // needs log(0.1)/log(h) updates (76 at h = 0.97) to reach 90% of a step.
    bool probe_changed = false;
    if (!first && valid_count > 0u) {
        const f32 inv = 1.f / static_cast<f32>(valid_count);
        const Vec3 in_mean = incoming_mean * inv;
        const Vec3 hist_mean = history_mean * inv;
        const f32 scale = std::max(std::max(maxComponent(in_mean), maxComponent(hist_mean)), m_config.change_floor);
        probe_changed = maxComponent(absVec(in_mean - hist_mean)) > m_config.probe_change_threshold * scale;
    }
    const f32 fast_hysteresis = std::max(0.f, hysteresis - m_config.change_hysteresis_drop);
    for (u32 y = 0; y < ir; ++y) {
        for (u32 x = 0; x < ir; ++x) {
            const u32 t = y * ir + x;
            if (m_scratch_valid[t] == 0u) {
                continue;
            }
            const Vec3& incoming = m_scratch_incoming[t];
            Vec3& texel = irr_tile[(y + 1u) * irr_stride + (x + 1u)];
            f32 h = hysteresis;
            if (!first) {
                const f32 scale = std::max(std::max(maxComponent(texel), maxComponent(incoming)),
                                           m_config.change_floor);
                if (probe_changed) {
                    h = std::min(hysteresis, std::clamp(m_config.probe_change_hysteresis, 0.f, 1.f));
                    ++stats.fast_response_texels;
                } else if (maxComponent(absVec(incoming - texel)) > m_config.change_threshold * scale) {
                    h = fast_hysteresis;
                    ++stats.fast_response_texels;
                }
            }
            texel = incoming * (1.f - h) + texel * h;
        }
    }
    ddgi_cpu::copyOctahedralBorder(irr_tile, ir, irr_stride);

    const u32 dr = m_desc.depth_res;
    const u32 dist_stride = dr + 2u;
    Vec2* dist_tile = &m_distance[distanceOffset(probe_index)];
    // Rays whose cos^power weight is below 1e-6 cannot move the weighted mean; skip them.
    const f32 power = std::max(m_config.distance_power, 1e-3f);
    const f32 min_cos = std::pow(1e-6f, 1.f / power);
    const f32 max_distance = m_desc.max_ray_distance;
    for (u32 y = 0; y < dr; ++y) {
        for (u32 x = 0; x < dr; ++x) {
            const Vec3& texel_dir = m_scratch_distance_dirs[y * dr + x];
            const f32 tx = texel_dir.x;
            const f32 ty = texel_dir.y;
            const f32 tz = texel_dir.z;
            f32 sum_d = 0.f;
            f32 sum_d2 = 0.f;
            f32 weight_sum = 0.f;
            for (u32 r = 0; r < ray_count; ++r) {
                const Vec3& dir = ray_dirs[r];
                const f32 c = tx * dir.x + ty * dir.y + tz * dir.z;
                if (c > min_cos) {
                    const f32 w = std::pow(c, power);
                    const f32 d = std::min(distances[r], max_distance);
                    sum_d += d * w;
                    sum_d2 += d * d * w;
                    weight_sum += w;
                }
            }
            if (weight_sum <= 1e-12f) {
                continue;
            }
            const Vec2 incoming{sum_d / weight_sum, sum_d2 / weight_sum};
            Vec2& texel = dist_tile[(y + 1u) * dist_stride + (x + 1u)];
            texel = incoming * (1.f - hysteresis) + texel * hysteresis;
        }
    }
    ddgi_cpu::copyOctahedralBorder(dist_tile, dr, dist_stride);
    ++m_update_counts[probe_index];
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
    if (!m_ready || probe_index >= m_probe_count) {
        return {};
    }
    const Vec3 dir = DdgiIrradianceEncoding::resolveSampleDirection(direction);
    return sampleTile(&m_irradiance[irradianceOffset(probe_index)], m_desc.irradiance_res, dir) * kPi;
}

Vec2 DdgiCpuVolume::probeDistance(u32 probe_index, const Vec3& direction) const {
    if (!m_ready || probe_index >= m_probe_count) {
        return {};
    }
    const Vec3 dir = DdgiIrradianceEncoding::resolveSampleDirection(direction);
    return sampleTile(&m_distance[distanceOffset(probe_index)], m_desc.depth_res, dir);
}

Vec3 DdgiCpuVolume::sampleIrradiance(const Vec3& position, const Vec3& normal) const {
    if (!m_ready) {
        return {};
    }
    const Vec3 n = DdgiIrradianceEncoding::resolveSampleDirection(normal);
    const Vec3 biased = position + n * m_config.normal_bias;
    const Vec3 grid = ProbeGridLayout::worldToProbeGridCoord(m_desc, biased);

    const u32 dims[3] = {m_desc.grid_dims.x, m_desc.grid_dims.y, m_desc.grid_dims.z};
    u32 base[3]{};
    f32 alpha[3]{};
    for (u32 a = 0; a < 3u; ++a) {
        const f32 g = axis(grid, a);
        const f32 max_base = static_cast<f32>(dims[a] - 1u);
        const f32 fb = std::clamp(std::floor(g), 0.f, max_base);
        base[a] = static_cast<u32>(fb);
        alpha[a] = std::clamp(g - fb, 0.f, 1.f);
    }

    Vec3 sum{};
    f32 weight_sum = 0.f;
    for (u32 corner = 0; corner < 8u; ++corner) {
        ProbeGridCoord coord{};
        f32 trilinear = 1.f;
        u32 c[3]{};
        for (u32 a = 0; a < 3u; ++a) {
            const u32 bit = (corner >> a) & 1u;
            c[a] = std::min(base[a] + bit, dims[a] - 1u);
            trilinear *= bit ? alpha[a] : (1.f - alpha[a]);
        }
        coord.x = c[0];
        coord.y = c[1];
        coord.z = c[2];
        if (trilinear <= 0.f) {
            continue;
        }
        const u32 probe = ProbeGridLayout::probeIndexFromCoord(m_desc, coord);
        const Vec3 probe_pos = ddgi_util::probeWorldPosition(m_desc, probe);

        // Smooth backface term: probes behind the surface fade out without a hard cut.
        f32 weight = 1.f;
        const Vec3 to_probe = probe_pos - position;
        const f32 to_probe_len = to_probe.length();
        if (to_probe_len > 1e-6f) {
            const f32 wrap = (to_probe.dot(n) / to_probe_len + 1.f) * 0.5f;
            weight *= wrap * wrap + 0.2f;
        }

        // Chebyshev visibility from the probe's distance moments toward the biased point.
        const Vec3 probe_to_point = biased - probe_pos;
        const f32 dist = probe_to_point.length();
        if (dist > 1e-6f) {
            const Vec2 moments = probeDistance(probe, probe_to_point * (1.f / dist));
            const f32 mean = moments.x;
            if (dist > mean) {
                const f32 variance = std::fabs(moments.y - mean * mean);
                const f32 delta = dist - mean;
                f32 chebyshev = variance / std::max(variance + delta * delta, 1e-12f);
                chebyshev = std::max(chebyshev * chebyshev * chebyshev, 0.f);
                weight *= std::max(chebyshev, 0.05f);
            }
        }
        weight = std::max(weight, 1e-6f);
        const f32 crush = m_config.weight_crush_threshold;
        if (crush > 0.f && weight < crush) {
            weight *= (weight * weight) / (crush * crush);
        }
        weight *= trilinear;

        sum = sum + probeIrradiance(probe, n) * weight;
        weight_sum += weight;
    }
    if (weight_sum <= 0.f) {
        return {};
    }
    return sum * (1.f / weight_sum);
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
