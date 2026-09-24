#pragma once

// WP-8.1 froxel fog gates: the shared test scene (CPU gates test_rp_volumetric_gpu_cpu.cpp and Lavapipe gates
// test_rp_volumetric_gpu.cpp). A courtyard lit by a sun, spot and point lights, with exponential height fog,
// a spherical and a box fog volume, a camera path, and a synthetic depth / lit image pair for fog.apply.

#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog_reference.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog_types.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fog_test {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::renderer::ClusterCameraDesc;
using fuse::renderer::ClusterDesc;
using fuse::renderer::gpu_scene::GpuLight;
using fuse::renderer::gpu_scene::GpuLightType;
namespace vg = fuse::renderer::volumetric_gpu;

inline constexpr f32 kNear = 0.1f;
inline constexpr f32 kFar = 80.f;
inline constexpr f32 kFovY = 1.0471976f; // 60 degrees

/// Camera of frame `frame`: static (moving = 0), or translating sideways + yawing (moving = 1, a slow pan;
/// moving = 2, a faster pan used by the ghosting control).
inline ClusterCameraDesc camera(u32 frame, u32 moving, u32 width, u32 height) {
    const f32 t = static_cast<f32>(frame) * (moving == 2u ? 2.f : 1.f);
    const f32 m = moving != 0u ? 1.f : 0.f;
    ClusterCameraDesc c{};
    c.position = {-2.f + 0.12f * t * m, 1.6f + 0.01f * t * m, 6.f - 0.05f * t * m};
    const f32 yaw = -0.15f + 0.012f * t * m;
    c.forward = {std::sin(yaw), -0.08f, -std::cos(yaw)};
    c.up = {0.f, 1.f, 0.f};
    c.nearPlane = kNear;
    c.farPlane = kFar;
    c.screenWidth = width;
    c.screenHeight = height;
    c.fovYRadians = kFovY;
    c.reversedZ = false; // WP-1.5 RT4 = forward z / w
    return c;
}

inline GpuLight point(f32 x, f32 y, f32 z, f32 range, f32 r, f32 g, f32 b, f32 intensity) {
    GpuLight l{};
    l.type = static_cast<u32>(GpuLightType::Point);
    l.position[0] = x;
    l.position[1] = y;
    l.position[2] = z;
    l.range = range;
    l.color[0] = r;
    l.color[1] = g;
    l.color[2] = b;
    l.intensity = intensity;
    return l;
}

inline GpuLight spot(f32 x, f32 y, f32 z, f32 dx, f32 dy, f32 dz, f32 range, f32 inner, f32 outer, f32 intensity) {
    GpuLight l = point(x, y, z, range, 1.f, 0.9f, 0.7f, intensity);
    l.type = static_cast<u32>(GpuLightType::Spot);
    l.direction[0] = dx;
    l.direction[1] = dy;
    l.direction[2] = dz;
    l.cosInner = std::cos(inner);
    l.cosOuter = std::cos(outer);
    return l;
}

/// Light table (slot order: points, then spots, then a directional sun, a free slot, a no-range spot):
/// points below spots keep the WP-2.1 oracle order == slot order.
inline std::vector<GpuLight> lights(bool withSun = true) {
    std::vector<GpuLight> v;
    v.push_back(point(1.5f, 1.2f, -4.f, 6.f, 1.f, 0.5f, 0.2f, 8.f));
    v.push_back(point(-3.f, 0.8f, -9.f, 5.f, 0.3f, 0.6f, 1.f, 10.f));
    v.push_back(point(4.f, 2.5f, -14.f, 8.f, 0.9f, 0.9f, 1.f, 12.f));
    v.push_back(point(-6.f, 1.f, -22.f, 7.f, 1.f, 0.2f, 0.4f, 9.f));
    v.push_back(spot(0.f, 5.f, -8.f, 0.1f, -1.f, 0.05f, 12.f, 0.25f, 0.45f, 40.f));
    v.push_back(spot(-4.f, 3.f, -3.f, 0.6f, -0.5f, -0.6f, 14.f, 0.15f, 0.3f, 30.f));
    GpuLight sun{};
    sun.type = withSun ? static_cast<u32>(GpuLightType::Directional) : 0u;
    sun.direction[0] = 0.4f;
    sun.direction[1] = -0.7f;
    sun.direction[2] = -0.3f;
    sun.color[0] = 1.f;
    sun.color[1] = 0.95f;
    sun.color[2] = 0.85f;
    sun.intensity = 1.5f;
    v.push_back(sun);
    v.push_back(GpuLight{}); // free slot
    GpuLight none = spot(0.f, 1.f, -5.f, 0.f, -1.f, 0.f, 0.f, 0.2f, 0.4f, 5.f); // no range: no cluster, no light
    v.push_back(none);
    return v;
}

inline ClusterDesc clusters() {
    ClusterDesc d{};
    d.tilesX = 16;
    d.tilesY = 9;
    d.slicesZ = 24;
    d.maxLightsPerCluster = 64;
    return d;
}

/// Fog settings: height fog + two local volumes; a grid of 32 x 18 x 48 (2 x 2 x 2 froxels per 16 x 9 x 24
/// cluster when the ranges agree).
inline vg::FroxelFogSettings settings() {
    vg::FroxelFogSettings s{};
    s.gridX = 32;
    s.gridY = 18;
    s.gridZ = 48;
    s.farPlane = 48.f;
    s.medium.density = 0.04f;
    s.medium.height_falloff = 0.15f;
    s.medium.base_height = 0.5f;
    s.medium.anisotropy = 0.4f;
    s.medium.fog_color = {0.9f, 0.92f, 0.95f};
    s.ambient[0] = 0.05f;
    s.ambient[1] = 0.06f;
    s.ambient[2] = 0.08f;
    s.volumeCount = 2;
    vg::FogVolume& sphere = s.volumes[0];
    sphere.center[0] = 1.f;
    sphere.center[1] = 1.5f;
    sphere.center[2] = -7.f;
    sphere.halfExtent[0] = 2.5f;
    sphere.density = 0.3f;
    sphere.albedo[0] = 0.8f;
    sphere.albedo[1] = 0.85f;
    sphere.albedo[2] = 0.9f;
    sphere.edge = 0.4f;
    vg::FogVolume& box = s.volumes[1];
    box.shape = vg::kFogVolumeBox;
    box.center[0] = -4.f;
    box.center[1] = 1.f;
    box.center[2] = -15.f;
    box.halfExtent[0] = 3.f;
    box.halfExtent[1] = 1.5f;
    box.halfExtent[2] = 2.f;
    box.density = 0.2f;
    box.albedo[0] = 0.6f;
    box.albedo[1] = 0.7f;
    box.albedo[2] = 0.65f;
    box.edge = 0.3f;
    return s;
}

/// Forward z / w device depth of a view depth (inverse of view_depth_from_device_depth, reversedZ = false).
inline f32 deviceDepth(f32 viewDepth) {
    return (kFar * (viewDepth - kNear)) / (viewDepth * (kFar - kNear));
}

/// Synthetic depth / lit pair: a ground-like depth ramp with a few boxes in front, sky (1.0) at the top.
inline void images(u32 w, u32 h, std::vector<f32>& depth, std::vector<fuse::math::Vec4>& lit) {
    depth.assign(static_cast<size_t>(w) * h, 1.f);
    lit.assign(static_cast<size_t>(w) * h, fuse::math::Vec4{});
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const f32 sy = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(h);
            const f32 sx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(w);
            f32 z = 0.f;
            if (sy > 0.35f) {
                z = 1.5f / (sy - 0.33f); // ground: nearer towards the bottom
            }
            if (sx > 0.2f && sx < 0.35f && sy > 0.3f && sy < 0.7f) {
                z = 6.f;
            }
            if (sx > 0.6f && sx < 0.8f && sy > 0.25f && sy < 0.6f) {
                z = 18.f + 4.f * sx;
            }
            const f32 checker = ((x / 4u + y / 4u) % 2u) == 0u ? 1.f : 0.6f;
            if (z > 0.f && z < kFar) {
                depth[i] = deviceDepth(z);
                lit[i] = fuse::math::Vec4{0.4f * checker, 0.35f * checker + 0.1f * sx, 0.3f + 0.2f * sy, 1.f};
            } else {
                lit[i] = fuse::math::Vec4{0.5f, 0.7f, 1.2f, 1.f}; // sky radiance
            }
        }
    }
}

} // namespace fog_test

namespace fog_test {

using fuse::math::Vec4;

/// Sum |a.rgb - ref.rgb| / sum |ref.rgb| (the scattering channels; extinction is not radiance).
inline f64 relL1(const std::vector<Vec4>& a, const std::vector<Vec4>& ref) {
    f64 num = 0.0;
    f64 den = 0.0;
    for (size_t i = 0; i < ref.size(); ++i) {
        num += std::fabs(static_cast<f64>(a[i].x) - ref[i].x) + std::fabs(static_cast<f64>(a[i].y) - ref[i].y) +
               std::fabs(static_cast<f64>(a[i].z) - ref[i].z);
        den += std::fabs(static_cast<f64>(ref[i].x)) + std::fabs(static_cast<f64>(ref[i].y)) + std::fabs(static_cast<f64>(ref[i].z));
    }
    return den > 0.0 ? num / den : num;
}

/// Trail energy: scattering the history holds above the reference (light left behind by the history),
/// sum max(0, a - ref) / sum ref over the rgb channels.
inline f64 trailEnergy(const std::vector<Vec4>& a, const std::vector<Vec4>& ref) {
    f64 num = 0.0;
    f64 den = 0.0;
    for (size_t i = 0; i < ref.size(); ++i) {
        const f64 d[3] = {static_cast<f64>(a[i].x) - ref[i].x, static_cast<f64>(a[i].y) - ref[i].y,
                          static_cast<f64>(a[i].z) - ref[i].z};
        for (f64 v : d) {
            num += v > 0.0 ? v : 0.0;
        }
        den += static_cast<f64>(ref[i].x) + ref[i].y + ref[i].z;
    }
    return den > 0.0 ? num / den : num;
}

/// Temporal variance of a window of frames: sqrt(sum var / sum mean^2) over froxels and rgb channels
/// (the relative RMS frame-to-frame fluctuation).
struct VarianceWindow {
    std::vector<f64> sum;
    std::vector<f64> sum2;
    u32 frames = 0;
    void add(const std::vector<Vec4>& v) {
        if (sum.empty()) {
            sum.assign(v.size() * 3u, 0.0);
            sum2.assign(v.size() * 3u, 0.0);
        }
        for (size_t i = 0; i < v.size(); ++i) {
            const f64 c[3] = {v[i].x, v[i].y, v[i].z};
            for (u32 k = 0; k < 3u; ++k) {
                sum[i * 3u + k] += c[k];
                sum2[i * 3u + k] += c[k] * c[k];
            }
        }
        ++frames;
    }
    f64 relativeStd() const {
        f64 var = 0.0;
        f64 mean2 = 0.0;
        for (size_t i = 0; i < sum.size(); ++i) {
            const f64 m = sum[i] / frames;
            var += std::max(sum2[i] / frames - m * m, 0.0);
            mean2 += m * m;
        }
        return mean2 > 0.0 ? std::sqrt(var / mean2) : 0.0;
    }
    std::vector<Vec4> mean() const {
        std::vector<Vec4> out(sum.size() / 3u);
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = Vec4{static_cast<f32>(sum[i * 3u] / frames), static_cast<f32>(sum[i * 3u + 1u] / frames),
                          static_cast<f32>(sum[i * 3u + 2u] / frames), 0.f};
        }
        return out;
    }
};

} // namespace fog_test
