// WP-8.1 froxel fog: host settings, frame constants and the CPU reference (froxel_fog_reference.hpp).
#include <fuse/renderer/volumetric/gpu/froxel_fog_reference.hpp>

#include <fuse/renderer/lighting/clustered_kernel.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_reference.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::volumetric_gpu {

using math::Vec3;
using math::Vec4;

f32 halton(u32 index, u32 base) {
    f32 f = 1.f;
    f32 r = 0.f;
    const f32 inv = 1.f / static_cast<f32>(base);
    while (index > 0u) {
        f = f * inv;
        r = r + f * static_cast<f32>(index % base);
        index /= base;
    }
    return r;
}

void fogJitter(u32 frameIndex, bool jitter, f32 (&out)[3]) {
    if (!jitter) {
        out[0] = out[1] = out[2] = 0.5f;
        return;
    }
    const u32 i = (frameIndex % kFogJitterPeriod) + 1u;
    out[0] = halton(i, 2u);
    out[1] = halton(i, 3u);
    out[2] = halton(i, 5u);
}

void clampFogGrid(u32& x, u32& y, u32& z) {
    x = std::min(x, kFogMaxGridX);
    y = std::min(y, kFogMaxGridY);
    z = std::min(z, kFogMaxSlices);
}

namespace {
bool cameraValid(const ClusterCameraDesc& c) {
    return c.nearPlane > 0.f && c.farPlane > c.nearPlane && std::isfinite(c.farPlane) && c.fovYRadians > 0.f &&
           c.screenWidth > 0u && c.screenHeight > 0u;
}
} // namespace

bool resolveFogConstants(const FroxelFogSettings& s, const ClusterCameraDesc& camera, const ClusterCameraDesc* prev,
                         bool historyValid, u32 frameIndex, u32 width, u32 height, FogFrameConstants& out) {
    FogFrameConstants c{};
    u32 gx = s.gridX;
    u32 gy = s.gridY;
    u32 gz = s.gridZ;
    clampFogGrid(gx, gy, gz);
    if (gx == 0u || gy == 0u || gz == 0u || !cameraValid(camera)) {
        return false;
    }
    c.gridX = gx;
    c.gridY = gy;
    c.gridZ = gz;
    c.froxelCount = gx * gy * gz;
    c.invGridX = 1.f / static_cast<f32>(gx);
    c.invGridY = 1.f / static_cast<f32>(gy);
    c.width = width;
    c.height = height;
    c.invWidth = width > 0u ? 1.f / static_cast<f32>(width) : 0.f;
    c.invHeight = height > 0u ? 1.f / static_cast<f32>(height) : 0.f;
    c.frameIndex = frameIndex;

    const clustered_kernel::CameraView view = clustered_kernel::make_camera(camera);
    const Vec3* src[4] = {&view.position, &view.right, &view.up, &view.back};
    f32* dst[4] = {c.position, c.right, c.up, c.back};
    for (u32 i = 0; i < 4u; ++i) {
        dst[i][0] = src[i]->x;
        dst[i][1] = src[i]->y;
        dst[i][2] = src[i]->z;
    }
    c.tanX = view.tan_x;
    c.tanY = view.tan_y;
    c.nearPlane = camera.nearPlane;
    c.farPlane = std::max(std::min(s.farPlane, camera.farPlane), camera.nearPlane * 1.0001f);
    c.depthNear = camera.nearPlane;
    c.depthFar = camera.farPlane;
    if (camera.reversedZ) {
        c.flags |= kFogFlagReversedZ;
    }
    // The B5 FroxelSliceLayout / clustered slice_near_z distribution (same expression).
    const f32 ratio = c.farPlane / c.nearPlane;
    for (u32 z = 0; z < gz; ++z) {
        c.sliceDepth[z] = c.nearPlane * std::pow(ratio, static_cast<f32>(z) / static_cast<f32>(gz));
    }
    c.sliceDepth[gz] = c.farPlane;

    const bool history = s.temporal && historyValid && prev != nullptr && cameraValid(*prev);
    if (history) {
        c.flags |= kFogFlagHistory;
        if (s.reproject) {
            c.flags |= kFogFlagReproject;
        }
        const clustered_kernel::CameraView pv = clustered_kernel::make_camera(*prev);
        const Vec3* psrc[4] = {&pv.position, &pv.right, &pv.up, &pv.back};
        f32* pdst[4] = {c.prevPosition, c.prevRight, c.prevUp, c.prevBack};
        for (u32 i = 0; i < 4u; ++i) {
            pdst[i][0] = psrc[i]->x;
            pdst[i][1] = psrc[i]->y;
            pdst[i][2] = psrc[i]->z;
        }
        c.prevTanX = pv.tan_x;
        c.prevTanY = pv.tan_y;
    }
    fogJitter(frameIndex, s.jitter && s.temporal, c.jitter);
    c.temporalAlpha = std::clamp(s.temporalAlpha, 0.f, 1.f);

    const VolumetricFogParams& m = s.medium;
    c.density = std::max(m.density, 0.f);
    c.heightFalloff = std::max(m.height_falloff, 0.f);
    c.baseHeight = m.base_height;
    c.anisotropy = std::clamp(m.anisotropy, -fog_kernel::kMaxAnisotropy, fog_kernel::kMaxAnisotropy);
    c.albedo[0] = std::max(m.fog_color.x, 0.f);
    c.albedo[1] = std::max(m.fog_color.y, 0.f);
    c.albedo[2] = std::max(m.fog_color.z, 0.f);
    for (u32 i = 0; i < 3u; ++i) {
        c.ambient[i] = std::max(s.ambient[i], 0.f);
    }
    c.volumeCount = std::min(s.volumeCount, kFogMaxVolumes);
    for (u32 i = 0; i < c.volumeCount; ++i) {
        FogVolume v = s.volumes[i];
        v.density = std::max(v.density, 0.f);
        for (f32& e : v.halfExtent) {
            e = std::max(e, 1e-4f);
        }
        for (f32& a : v.albedo) {
            a = std::max(a, 0.f);
        }
        v.shape = v.shape == kFogVolumeBox ? kFogVolumeBox : kFogVolumeSphere;
        c.volumes[i] = v;
    }
    if (!m.receive_shadows) {
        c.flags &= ~static_cast<u32>(kFogFlagShadows);
    }
    out = c;
    return true;
}

lighting_gpu::LightingFrameConstants makeLightingView(const ClusterDesc& rawDesc, const ClusterCameraDesc& camera,
                                                      u32 lightCount) {
    const ClusterDesc desc = ClusterDesc::clampCounts(rawDesc);
    lighting_gpu::LightingFrameConstants f{};
    const clustered_kernel::CameraView view = clustered_kernel::make_camera(camera);
    const Vec3* src[4] = {&view.position, &view.right, &view.up, &view.back};
    f32* dst[4] = {f.cameraPosition, f.right, f.up, f.back};
    for (u32 i = 0; i < 4u; ++i) {
        dst[i][0] = src[i]->x;
        dst[i][1] = src[i]->y;
        dst[i][2] = src[i]->z;
    }
    f.nearPlane = view.near_plane;
    f.farPlane = view.far_plane;
    f.tanX = view.tan_x;
    f.tanY = view.tan_y;
    f.tilesX = desc.tilesX;
    f.tilesY = desc.tilesY;
    f.slicesZ = desc.slicesZ;
    f.clusterCount = desc.clusterCount();
    f.capacity = desc.maxLightsPerCluster == 0u ? lighting_gpu::kMaxLightsPerCluster
                                                : std::min(desc.maxLightsPerCluster, lighting_gpu::kMaxLightsPerCluster);
    f.lightCount = lightCount;
    return f;
}

void oracleFogLights(const ClusterDesc& desc, const ClusterCameraDesc& camera, const gpu_scene::GpuLight* lights,
                     u32 lightCount, FogLightLists& out) {
    lighting_gpu::OracleLights oracle{};
    lighting_gpu::makeOracleLights(lights, lightCount, oracle);
    ClusterGridSoA grid{};
    lighting_gpu::oracleLightGrid(desc, camera, oracle, grid);
    ClusterGridSoA slots{};
    lighting_gpu::translateToSlots(grid, oracle, slots);
    out.grid.resize(slots.grid.size() * 2u);
    for (usize i = 0; i < slots.grid.size(); ++i) {
        out.grid[i * 2u] = slots.grid[i].offset;
        out.grid[i * 2u + 1u] = slots.grid[i].count;
    }
    out.lightList = slots.lightList;
    out.directional = oracle.directional;
}

fog_kernel::LightView makeLightView(const lighting_gpu::LightingFrameConstants* frame, const gpu_scene::GpuLight* lights,
                                    const FogLightLists& lists) {
    fog_kernel::LightView v{};
    v.frame = frame;
    v.lights = lights;
    v.directional = lists.directional.data();
    v.directionalCount = static_cast<u32>(lists.directional.size());
    v.grid = lists.grid.empty() ? nullptr : lists.grid.data();
    v.lightList = lists.lightList.data();
    return v;
}

void injectReference(const FogFrameConstants& c, const fog_kernel::LightView& lights, std::vector<Vec4>& out) {
    out.assign(c.froxelCount, Vec4{});
    for (u32 y = 0; y < c.gridY; ++y) {
        for (u32 x = 0; x < c.gridX; ++x) {
            for (u32 z = 0; z < c.gridZ; ++z) {
                out[fog_kernel::froxel_index(c, x, y, z)] = fog_kernel::inject_froxel(c, lights, x, y, z);
            }
        }
    }
}

void temporalReference(const FogFrameConstants& c, const std::vector<Vec4>& current, const std::vector<Vec4>& history,
                       std::vector<Vec4>& out) {
    out.assign(c.froxelCount, Vec4{});
    for (u32 y = 0; y < c.gridY; ++y) {
        for (u32 x = 0; x < c.gridX; ++x) {
            for (u32 z = 0; z < c.gridZ; ++z) {
                out[fog_kernel::froxel_index(c, x, y, z)] =
                    fog_kernel::temporal_froxel(c, current.data(), history.data(), x, y, z);
            }
        }
    }
}

void integrateReference(const FogFrameConstants& c, const std::vector<Vec4>& froxels, std::vector<Vec4>& out) {
    out.assign(c.froxelCount, Vec4{});
    for (u32 y = 0; y < c.gridY; ++y) {
        for (u32 x = 0; x < c.gridX; ++x) {
            fog_kernel::integrate_column(c, froxels.data(), out.data(), x, y);
        }
    }
}

void applyReference(const FogFrameConstants& c, const std::vector<Vec4>& integrated, const std::vector<f32>& depth,
                    const std::vector<Vec4>& lit, std::vector<Vec4>& out) {
    out.assign(static_cast<usize>(c.width) * c.height, Vec4{});
    for (u32 y = 0; y < c.height; ++y) {
        for (u32 x = 0; x < c.width; ++x) {
            const usize i = static_cast<usize>(y) * c.width + x;
            out[i] = fog_kernel::apply_pixel(c, integrated.data(), x, y, depth[i], lit[i]);
        }
    }
}

void froxelAverageReference(const FogFrameConstants& c, const fog_kernel::LightView& lights, u32 n,
                            std::vector<Vec4>& out) {
    out.assign(c.froxelCount, Vec4{});
    n = std::max(n, 1u);
    const f64 inv = 1.0 / (static_cast<f64>(n) * n * n);
    for (u32 y = 0; y < c.gridY; ++y) {
        for (u32 x = 0; x < c.gridX; ++x) {
            for (u32 z = 0; z < c.gridZ; ++z) {
                f64 acc[4] = {0.0, 0.0, 0.0, 0.0};
                for (u32 k = 0; k < n; ++k) {
                    for (u32 j = 0; j < n; ++j) {
                        for (u32 i = 0; i < n; ++i) {
                            const f32 jx = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(n);
                            const f32 jy = (static_cast<f32>(j) + 0.5f) / static_cast<f32>(n);
                            const f32 jz = (static_cast<f32>(k) + 0.5f) / static_cast<f32>(n);
                            const Vec4 v = fog_kernel::inject_point(c, lights, fog_kernel::froxel_point(c, x, y, z, jx, jy, jz));
                            acc[0] += v.x;
                            acc[1] += v.y;
                            acc[2] += v.z;
                            acc[3] += v.w;
                        }
                    }
                }
                out[fog_kernel::froxel_index(c, x, y, z)] =
                    Vec4{static_cast<f32>(acc[0] * inv), static_cast<f32>(acc[1] * inv), static_cast<f32>(acc[2] * inv),
                         static_cast<f32>(acc[3] * inv)};
            }
        }
    }
}

void jitterPeriodMeanReference(const FogFrameConstants& c, const fog_kernel::LightView& lights, std::vector<Vec4>& out) {
    out.assign(c.froxelCount, Vec4{});
    std::vector<f64> acc(static_cast<usize>(c.froxelCount) * 4u, 0.0);
    for (u32 f = 0; f < kFogJitterPeriod; ++f) {
        f32 j[3];
        fogJitter(f, true, j);
        for (u32 y = 0; y < c.gridY; ++y) {
            for (u32 x = 0; x < c.gridX; ++x) {
                for (u32 z = 0; z < c.gridZ; ++z) {
                    const u32 i = fog_kernel::froxel_index(c, x, y, z);
                    const Vec4 v = fog_kernel::inject_point(c, lights, fog_kernel::froxel_point(c, x, y, z, j[0], j[1], j[2]));
                    acc[i * 4u] += v.x;
                    acc[i * 4u + 1u] += v.y;
                    acc[i * 4u + 2u] += v.z;
                    acc[i * 4u + 3u] += v.w;
                }
            }
        }
    }
    const f64 inv = 1.0 / static_cast<f64>(kFogJitterPeriod);
    for (u32 i = 0; i < c.froxelCount; ++i) {
        out[i] = Vec4{static_cast<f32>(acc[i * 4u] * inv), static_cast<f32>(acc[i * 4u + 1u] * inv),
                      static_cast<f32>(acc[i * 4u + 2u] * inv), static_cast<f32>(acc[i * 4u + 3u] * inv)};
    }
}

} // namespace fuse::renderer::volumetric_gpu
