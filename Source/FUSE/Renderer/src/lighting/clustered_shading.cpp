#include <fuse/renderer/lighting/clustered_shading.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

f32 clustered_shading::pointLightFalloff(f32 distance, f32 radius) {
    if (!(radius > 0.f) || !(distance < radius)) {
        return 0.f;
    }
    const f32 ratio = distance / radius;
    const f32 ratio2 = ratio * ratio;
    const f32 window = std::clamp(1.f - ratio2 * ratio2, 0.f, 1.f);
    return (window * window) / std::max(distance * distance, 1e-4f);
}

fuse::math::Vec3 clustered_shading::pointLightContribution(const PointLightInput& light,
                                                           const DeferredSurfaceSample& surface) {
    const fuse::math::Vec3 toLight = light.position - surface.worldPos;
    const f32 distance = toLight.length();
    const f32 falloff = pointLightFalloff(distance, light.radius);
    if (falloff == 0.f) {
        return {};
    }
    const f32 nDotL = std::max(surface.normal.dot(toLight * (1.f / std::max(distance, 1e-6f))), 0.f);
    const f32 scale = light.intensity * falloff * nDotL;
    return {light.color.x * surface.albedo.x * scale,
            light.color.y * surface.albedo.y * scale,
            light.color.z * surface.albedo.z * scale};
}

fuse::math::Vec3 clustered_shading::shadeAllLights(const std::vector<PointLightInput>& lights,
                                                   const DeferredSurfaceSample& surface,
                                                   u64* outEvaluations) {
    fuse::math::Vec3 radiance{};
    for (const PointLightInput& light : lights) {
        radiance = radiance + pointLightContribution(light, surface);
    }
    if (outEvaluations != nullptr) {
        *outEvaluations += lights.size();
    }
    return radiance;
}

fuse::math::Vec3 clustered_shading::shadeClusterLights(const ClusterGridSoA& grid,
                                                       u32 clusterIdx,
                                                       const std::vector<PointLightInput>& lights,
                                                       const DeferredSurfaceSample& surface,
                                                       u64* outEvaluations) {
    fuse::math::Vec3 radiance{};
    if (clusterIdx >= grid.grid.size()) {
        return radiance;
    }
    const ClusterGridEntry& entry = grid.grid[clusterIdx];
    const u32 end = std::min(entry.offset + entry.count, static_cast<u32>(grid.lightList.size()));
    u64 evaluations = 0u;
    for (u32 i = entry.offset; i < end; ++i) {
        const u32 lightIdx = grid.lightList[i];
        if (lightIdx >= lights.size()) {
            continue; // Spot light (encoded after point lights) — not handled by this reference.
        }
        radiance = radiance + pointLightContribution(lights[lightIdx], surface);
        ++evaluations;
    }
    if (outEvaluations != nullptr) {
        *outEvaluations += evaluations;
    }
    return radiance;
}

bool clustered_shading::reconstructWorldPosition(const ClusterCameraDesc& camera,
                                                 f32 screenX,
                                                 f32 screenY,
                                                 f32 deviceDepth,
                                                 fuse::math::Vec3& outWorld,
                                                 f32& outViewDepth) {
    const f32 viewDepth = cluster_math::viewDepthFromDeviceDepth(deviceDepth, camera);
    outViewDepth = viewDepth;
    if (!(viewDepth > 0.f) || !std::isfinite(viewDepth)) {
        return false;
    }
    outWorld = cluster_math::viewToWorld(camera,
                                         cluster_math::viewPositionFromScreen(screenX, screenY, viewDepth, camera));
    return true;
}

DeferredShadeStats clustered_shading::shadeDeferredFrame(const DeferredGBufferView& gbuffer,
                                                         const ClusterDesc& desc,
                                                         const ClusterCameraDesc& camera,
                                                         const ClusterGridSoA& grid,
                                                         const std::vector<PointLightInput>& lights,
                                                         bool useClusters,
                                                         std::vector<fuse::math::Vec3>& outRadiance) {
    DeferredShadeStats stats{};
    const usize pixelCount = static_cast<usize>(gbuffer.width) * gbuffer.height;
    outRadiance.assign(pixelCount, fuse::math::Vec3{});
    if (pixelCount == 0u || gbuffer.deviceDepth == nullptr || gbuffer.normals == nullptr ||
        gbuffer.albedo == nullptr) {
        return stats;
    }

    const f32 invWidth = 1.f / static_cast<f32>(gbuffer.width);
    const f32 invHeight = 1.f / static_cast<f32>(gbuffer.height);
    for (u32 py = 0; py < gbuffer.height; ++py) {
        for (u32 px = 0; px < gbuffer.width; ++px) {
            const usize pixel = static_cast<usize>(py) * gbuffer.width + px;
            const f32 screenX = (static_cast<f32>(px) + 0.5f) * invWidth;
            const f32 screenY = (static_cast<f32>(py) + 0.5f) * invHeight;

            DeferredSurfaceSample surface{};
            f32 viewDepth = 0.f;
            if (!reconstructWorldPosition(camera, screenX, screenY, gbuffer.deviceDepth[pixel], surface.worldPos,
                                          viewDepth)) {
                ++stats.skippedPixels;
                continue;
            }
            u32 clusterIdx = 0u;
            if (!ClusterGridLayout::mapScreenDepthToClusterIndex(screenX, screenY, viewDepth, desc, camera,
                                                                 clusterIdx)) {
                ++stats.skippedPixels;
                continue;
            }
            surface.normal = gbuffer.normals[pixel];
            surface.albedo = gbuffer.albedo[pixel];

            outRadiance[pixel] = useClusters
                                     ? shadeClusterLights(grid, clusterIdx, lights, surface, &stats.lightEvaluations)
                                     : shadeAllLights(lights, surface, &stats.lightEvaluations);
            ++stats.shadedPixels;
        }
    }
    return stats;
}

} // namespace fuse::renderer
